// File/layer: core/src/recorder.cpp | C++17, Qt-free; the recorder
//             (ADR-0049 sections 1 to 4), armed too (ADR-0063)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// The worker copies each frame and its values, its link transitions and
// its reply timing into a byte ring (single producer, single consumer);
// a writer thread of the recorder's own drains it into frames.csv,
// signals.csv, events.log's link and cond lines, markers.csv and
// latency.csv in ADR-0017's rows, and the caller's notes and marks, which
// come through a queue of their own, into events.log and markers.csv. A
// frame's condition events ride in its record; a frame that doesn't fit
// leaves them in a carry of 256 that goes in ahead of the next record, and
// past it they are counted and said in a drop line (ADR-0063 section 8).
// The writer never holds the session: what it writes reached it through
// the ring or the queue, or was copied at attach.
//
// This file holds the worker's puts, the ABI and the test seams; the
// class is in recorder_impl.h, its writer in recorder_writer.cpp and
// recorder_armed.cpp, the text helpers in recorder_text.cpp.

#include "recorder.h"

#include "condition_eval.h"
#include "error_detail.h"
#include "recorder_impl.h"
#include "recorder_text.h"
#include "recording_lines.h" /* signal_line */
#include "recording_rows.h" /* kFormat3, the first line */
#include "session.h"
#include "sha256.h"
#include "signal_file_build.h" /* a loaded file's bytes */

#include "ecuio/internal.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ecuio_rec {

constexpr uint32_t kDefaultRingBytes = 4u * 1024u * 1024u;
/* "conditions  conditions.json,<64 hex>\n" and its NUL */
constexpr size_t kConditionsLineMax = 96;
constexpr auto kFinalTimingWait = std::chrono::seconds(1);
constexpr auto kFinalTimingRetry = std::chrono::milliseconds(10);

/* ecuio_recorder_test_set_clock's (internal.h) */
std::atomic<bool> g_test_clock_on{false};
std::atomic<uint64_t> g_test_clock_us{0};

/* The session's focus; the caller holds focus_mutex */
focus_named focus_now(const ecuio_session_t* session)
{
    focus_named f;
    f.ids = session->focus_ids;
    f.pick = (session->focus_flags & ECUIO_FOCUS_DEFAULT_PICK) != 0;
    /* Only a session that asks has one: the others take it and change
     * nothing */
    f.min_sweep_ms = session->focus_requests ? session->focus_min_sweep_ms : 0;
    return f;
}

} // namespace ecuio_rec

void ecuio_recorder::copy_in(uint64_t pos, const void* src, size_t n)
{
    const size_t at = static_cast<size_t>(pos % cap);
    const size_t first = (std::min)(n, cap - at);
    std::memcpy(ring.get() + at, src, first);
    if (n > first) {
        std::memcpy(ring.get(), static_cast<const uint8_t*>(src) + first,
                    n - first);
    }
}

uint64_t ecuio_record_clock_us()
{
    return g_test_clock_on.load(std::memory_order_acquire)
               ? g_test_clock_us.load(std::memory_order_acquire)
               : ecuio_clock_us();
}

void ecuio_record_detach(ecuio_record_link& link, ecuio_recorder_t* only)
{
    std::lock_guard<std::mutex> g(link.m);
    if (only != nullptr) {
        ecuio_recorder_t* now = link.active.load(std::memory_order_seq_cst);
        if (now != nullptr && now != only) {
            return;
        }
    }
    ecuio_recorder_t* r =
        link.active.exchange(nullptr, std::memory_order_seq_cst);
    while (link.pins.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    if (r != nullptr) {
        r->detached.store(true, std::memory_order_release);
    }
}

namespace {

/* The worker's: whether size bytes fit in the ring */
bool ring_has_room(const ecuio_recorder_t* r, uint64_t size)
{
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    const uint64_t t = r->tail.load(std::memory_order_acquire);
    return size <= r->cap - (h - t);
}

/* The worker's: a carried link transition into the ring ahead of
 * anything else; false while it still doesn't fit. It carries no drop
 * count, so the drop line keeps the time of the record after the drops. */
bool put_pending_link(ecuio_recorder_t* r)
{
    if (!r->link_pending) {
        return true;
    }
    const uint64_t size = sizeof(record_head) + sizeof(link_body);
    if (!ring_has_room(r, size)) {
        return false;
    }
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    const record_head rh{static_cast<uint32_t>(size), kRecordLink, 0};
    r->copy_in(h, &rh, sizeof rh);
    r->copy_in(h + sizeof rh, &r->pending_link, sizeof r->pending_link);
    r->head.store(h + size, std::memory_order_release);
    r->link_pending = false;
    /* No event put after the link line is placed before it */
    r->put_host_us = (std::max)(r->put_host_us, r->pending_link.host_us);
    return true;
}

/* The worker's: carried events into the ring ahead of anything else, as
 * many as fit; false while some still wait. A run's lost count goes once. */
bool put_pending_conditions(ecuio_recorder_t* r)
{
    constexpr uint64_t kHead = sizeof(record_head) + sizeof(conditions_body);
    while (r->carry_count > 0) {
        const uint64_t h = r->head.load(std::memory_order_relaxed);
        const uint64_t t = r->tail.load(std::memory_order_acquire);
        const uint64_t room = r->cap - (h - t);
        if (room < kHead + sizeof(event_body)) {
            return false;
        }
        const uint32_t n = static_cast<uint32_t>(
            (std::min)(uint64_t{r->carry_count},
                       (room - kHead) / sizeof(event_body)));
        const uint64_t size = kHead + uint64_t{n} * sizeof(event_body);
        const event_body* first = r->carry + r->carry_start;
        const record_head rh{static_cast<uint32_t>(size), kRecordConditions, 0};
        const conditions_body cb{first->host_us, n, 0, r->carry_lost,
                                 r->carry_lost_us};
        r->copy_in(h, &rh, sizeof rh);
        r->copy_in(h + sizeof rh, &cb, sizeof cb);
        r->copy_in(h + kHead, first, n * sizeof(event_body));
        r->head.store(h + size, std::memory_order_release);
        r->carry_lost = 0; /* a loss after this is a run of its own */
        r->carry_start += n;
        r->carry_count -= n;
        if (r->carry_count == 0) {
            r->carry_start = 0;
        }
    }
    return true;
}

/* The worker's: an event whose frame found no room waits in the carry;
 * past it, the event is counted lost */
void carry_event(ecuio_recorder_t* r, const event_body& e)
{
    if (r->carry_count == kCarryEvents) {
        if (r->carry_lost == 0) {
            r->carry_lost_us = e.host_us;
        }
        ++r->carry_lost;
        r->conditions_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (r->carry_start + r->carry_count == kCarryEvents) {
        std::memmove(r->carry, r->carry + r->carry_start,
                     r->carry_count * sizeof(event_body));
        r->carry_start = 0;
    }
    r->carry[r->carry_start + r->carry_count++] = e;
}

/* ecuio_session_test_before_record_attach's (internal.h) */
std::atomic<void (*)(void*)> g_before_record_attach{nullptr};
std::atomic<void*> g_before_record_attach_user{nullptr};

uint64_t host_us_now(const ecuio_record_link& link)
{
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t zero = link.zero_us.load(std::memory_order_relaxed);
    return now > zero ? now - zero : 0;
}

} // namespace

void ecuio_record_link_state(ecuio_record_link* link, int32_t state,
                             uint32_t episodes, uint64_t t_us)
{
    if (link == nullptr) {
        return;
    }
    ecuio_recorder_t* r = ecuio_record_pin(*link);
    if (r == nullptr) {
        return;
    }
    /* A transition still carried gives way to this one: the latest state
     * and count are what a reader needs */
    r->pending_link = link_body{host_us_now(*link), state, episodes, t_us};
    r->link_pending = true;
    if (put_pending_conditions(r)) {
        put_pending_link(r);
    }
    ecuio_record_unpin(*link);
}

void ecuio_record_timing(ecuio_record_link* link,
                         const ecuio_session_timing_t& timing,
                         const ecuio_request_timing_t* rows, uint32_t count,
                         bool final)
{
    if (link == nullptr) {
        return;
    }
    const uint32_t n = (std::min)(count, uint32_t{ECUIO_PROFILE_MAX_REQUESTS});
    const uint64_t size = sizeof(record_head) + sizeof(timing_body) +
                          uint64_t{n} * sizeof(ecuio_request_timing_t);
    const auto give_up = std::chrono::steady_clock::now() + kFinalTimingWait;
    /* The record's time is the call's, read once a recorder is found: a
     * final snapshot that waits for room keeps the time of its figures */
    uint64_t at = 0;
    bool stamped = false;
    for (;;) {
        ecuio_recorder_t* r = ecuio_record_pin(*link);
        if (r == nullptr) {
            return;
        }
        if (!final && timing.snapshot_us < r->timing_next_us) {
            ecuio_record_unpin(*link);
            return;
        }
        if (!stamped) {
            at = host_us_now(*link);
            stamped = true;
        }
        if (put_pending_conditions(r) && put_pending_link(r) &&
            ring_has_room(r, size)) {
            const uint64_t h = r->head.load(std::memory_order_relaxed);
            const record_head rh{static_cast<uint32_t>(size), kRecordTiming,
                                 r->pending_dropped};
            r->pending_dropped = 0;
            const timing_body tb{at, timing, n, 0};
            r->copy_in(h, &rh, sizeof rh);
            r->copy_in(h + sizeof rh, &tb, sizeof tb);
            r->copy_in(h + sizeof rh + sizeof tb, rows,
                       n * sizeof(ecuio_request_timing_t));
            r->head.store(h + size, std::memory_order_release);
            r->timing_next_us = timing.snapshot_us + kTimingEveryUs;
            ecuio_record_unpin(*link);
            return;
        }
        /* No room: a periodic snapshot is dropped and counted (the next's
         * totals cover it); the final one waits unpinned, so a detach never
         * waits on it */
        if (!final || size > r->cap ||
            std::chrono::steady_clock::now() >= give_up) {
            r->records_dropped.fetch_add(1, std::memory_order_relaxed);
            r->timing_next_us = timing.snapshot_us + kTimingEveryUs;
            ecuio_record_unpin(*link);
            return;
        }
        ecuio_record_unpin(*link);
        std::this_thread::sleep_for(kFinalTimingRetry);
    }
}

void ecuio_recorder_hold_states(ecuio_recorder_t* r,
                                const ecuio_condition_eval& conditions)
{
    if (r->held_taken || r->conditions == nullptr ||
        conditions.generation != r->conditions_generation) {
        return;
    }
    r->held_taken = true;
    uint32_t n = 0;
    for (uint32_t i = 0;
         i < conditions.conditions.size() && n < ECUIO_CONDITION_MAX_CONDITIONS;
         ++i) {
        const ecuio_condition_eval::condition& c = conditions.conditions[i];
        if (c.state != ECUIO_CONDITION_ON) {
            continue;
        }
        event_body& e = r->held[n++];
        e = event_body{};
        e.value = c.value;
        e.condition = i;
        e.term = c.changed_term;
        e.state = ECUIO_CONDITION_ON;
        e.flags = static_cast<uint8_t>(
            ECUIO_CONDITION_HELD | (c.stale != 0 ? ECUIO_CONDITION_STALE : 0u));
    }
    r->held_count = n;
}

void ecuio_recorder_put_frame(ecuio_recorder_t* r, uint64_t host_us,
                              const ecuio_frame_t* frame,
                              const ecuio_value_t* values, uint32_t count,
                              const ecuio_condition_event_t* events,
                              uint32_t event_count, uint32_t generation)
{
    const uint16_t length = frame->data_length <= ECUIO_FRAME_DATA_MAX
                                ? frame->data_length
                                : static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX);
    const uint16_t nvalues =
        count > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(count);
    /* The recording's own set's events alone, its held states first (an
     * older set's frame under way at the attach gives none) */
    const bool own = r->conditions != nullptr &&
                     generation == r->conditions_generation &&
                     (events != nullptr || event_count == 0);
    const uint32_t held = own ? r->held_count : 0;
    const uint32_t settled = own ? event_count : 0;
    r->held_count = 0;
    const uint32_t nevents = held + settled;
    /* A held state stands before anything this frame settles */
    const uint64_t held_us =
        settled > 0 && events[0].timestamp_us < frame->timestamp_us
            ? events[0].timestamp_us
            : frame->timestamp_us;
    /* An event's recording time: the frame's, less how far its own time
     * lies before the frame's, never before the frame put before it */
    const uint64_t floor_us = (std::min)(r->put_host_us, host_us);
    const auto host_of = [&](uint64_t t_us) {
        if (t_us >= frame->timestamp_us) {
            return host_us;
        }
        const uint64_t back = frame->timestamp_us - t_us;
        return back < host_us - floor_us ? host_us - back : floor_us;
    };
    const auto event_at = [&](uint32_t k) {
        event_body e{};
        if (k < held) {
            e = r->held[k];
            e.t_us = held_us;
        } else {
            const ecuio_condition_event_t& c = events[k - held];
            e.t_us = c.timestamp_us;
            e.value = c.value;
            e.condition = c.condition;
            e.term = c.term;
            e.state = static_cast<uint8_t>(c.state);
            e.flags = static_cast<uint8_t>(c.flags);
        }
        e.host_us = host_of(e.t_us);
        return e;
    };

    uint64_t size = sizeof(record_head) + sizeof(frame_body) + length +
                    uint64_t{nevents} * sizeof(event_body);
    for (uint16_t i = 0; i < nvalues; ++i) {
        size += sizeof(value_head) + (is_text_kind(values[i].kind)
                                          ? values[i].text_len
                                          : sizeof(double));
    }
    /* What was carried goes first, or the frame waits behind it, dropped;
     * its events are carried, never dropped with it */
    if (!put_pending_conditions(r) || !put_pending_link(r) ||
        !ring_has_room(r, size)) {
        ++r->pending_dropped;
        r->records_dropped.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t k = 0; k < nevents; ++k) {
            carry_event(r, event_at(k));
        }
        return;
    }
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    record_head rh{static_cast<uint32_t>(size), kRecordFrame,
                   r->pending_dropped};
    r->pending_dropped = 0;
    frame_body fb{host_us,
                  frame->timestamp_us,
                  frame->arbitration_id,
                  frame->flags,
                  length,
                  nvalues,
                  nevents};
    uint64_t at = h;
    r->copy_in(at, &rh, sizeof rh);
    at += sizeof rh;
    r->copy_in(at, &fb, sizeof fb);
    at += sizeof fb;
    r->copy_in(at, frame->data, length);
    at += length;
    for (uint16_t i = 0; i < nvalues; ++i) {
        const ecuio_value_t& v = values[i];
        const bool text = is_text_kind(v.kind);
        value_head vh{v.signal_id, v.kind, 0, text ? v.text_len : 0};
        r->copy_in(at, &vh, sizeof vh);
        at += sizeof vh;
        if (text) {
            r->copy_in(at, v.text, v.text_len);
            at += v.text_len;
        } else {
            r->copy_in(at, &v.value, sizeof v.value);
            at += sizeof v.value;
        }
    }
    for (uint32_t k = 0; k < nevents; ++k) {
        const event_body e = event_at(k);
        r->copy_in(at, &e, sizeof e);
        at += sizeof e;
    }
    r->head.store(h + size, std::memory_order_release);
    r->put_host_us = host_us;
}

void ecuio_recorder_put_events(ecuio_recorder_t* r, uint64_t host_us,
                               uint64_t time_us,
                               const ecuio_condition_event_t* events,
                               uint32_t event_count, uint32_t generation)
{
    const bool own = r->conditions != nullptr &&
                     generation == r->conditions_generation &&
                     (events != nullptr || event_count == 0);
    const uint32_t held = own ? r->held_count : 0;
    const uint32_t settled = own ? event_count : 0;
    r->held_count = 0;
    if (held + settled == 0) {
        return;
    }
    const uint64_t held_us = settled > 0 && events[0].timestamp_us < time_us
                                 ? events[0].timestamp_us
                                 : time_us;
    /* As a frame's: a deadline's event earlier by its distance before
     * time_us, never before the record put before it */
    const uint64_t floor_us = (std::min)(r->put_host_us, host_us);
    /* What was carried, and the link line before these, go in first */
    const bool behind = !put_pending_conditions(r) || !put_pending_link(r);
    for (uint32_t k = 0; k < held + settled; ++k) {
        event_body e{};
        if (k < held) {
            e = r->held[k];
            e.t_us = held_us;
        } else {
            const ecuio_condition_event_t& c = events[k - held];
            e.t_us = c.timestamp_us;
            e.value = c.value;
            e.condition = c.condition;
            e.term = c.term;
            e.state = static_cast<uint8_t>(c.state);
            e.flags = static_cast<uint8_t>(c.flags);
        }
        const uint64_t back = time_us - (std::min)(e.t_us, time_us);
        e.host_us = back < host_us - floor_us ? host_us - back : floor_us;
        carry_event(r, e);
    }
    /* In a full ring they wait in the carry, which goes in ahead of a link
     * state still carried */
    if (!behind && put_pending_conditions(r)) {
        r->put_host_us = host_us;
    }
}

ecuio_status_t ecuio_recorder_take_signals(ecuio_recorder_t* recorder,
                                           const ecuio_signal_set_t* set,
                                           const char* bytes, size_t length,
                                           const char* plugin)
{
    if (recorder == nullptr || set == nullptr || plugin == nullptr ||
        (bytes == nullptr && length > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (recorder->attached.load(std::memory_order_acquire) ||
        recorder->signals_set != nullptr ||
        length > ECUIO_SIGNAL_SET_MAX_BYTES) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::string json(bytes != nullptr ? bytes : "", length);
        recorder->signals_sha = ecuio_sha256::hex_of(json.data(), json.size());
        recorder->signals_plugin = one_line(plugin);
        recorder->signals_json = std::move(json);
    } catch (...) {
        recorder->signals_sha.clear();
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    recorder->signals_set = set;
    return ECUIO_OK;
}

extern "C" {

ecuio_status_t ecuio_recorder_open(const ecuio_record_options_t* options,
                                   ecuio_recorder_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (options == nullptr || options->root_utf8 == nullptr ||
        (options->header_line_count > 0 && options->header_lines == nullptr)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if ((options->flags & ~ECUIO_RECORD_ARMED) != 0 ||
        options->reserved[0] != 0 || options->reserved[1] != 0 ||
        options->reserved[2] != 0 || options->reserved[3] != 0 ||
        options->root_utf8[0] == '\0') {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    std::unique_ptr<ecuio_recorder> r;
    try {
        r = std::make_unique<ecuio_recorder>();
        r->cap =
            options->ring_bytes != 0 ? options->ring_bytes : kDefaultRingBytes;
        r->ring.reset(new uint8_t[r->cap]);
        r->note_cap =
            options->note_lines != 0 ? options->note_lines : kDefaultNoteLines;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    std::string root = options->root_utf8;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) {
        root.pop_back();
    }
    std::string why;
    if (!make_root(root, &why)) {
        ecuio_detail_set("can't create the recordings' folder: %s",
                         why.c_str());
        return ECUIO_ERR_RECORDING_FAILED;
    }

    const std::string label =
        options->label_utf8 != nullptr ? reduce_label(options->label_utf8) : "";
    const std::string tail = label.empty() ? "" : " " + label;
    if ((options->flags & ECUIO_RECORD_ARMED) != 0) {
        /* Each window makes its own folder, under the same name's rule */
        r->armed = true;
        r->root = root;
        r->label_tail = tail;
    } else {
        std::string leaf;
        if (!make_recording_folder(root, std::time(nullptr), tail, &leaf,
                                   &why)) {
            ecuio_detail_set("can't create the recording's folder: %s",
                             why.c_str());
            return ECUIO_ERR_RECORDING_FAILED;
        }
        r->folder = absolute(leaf);
        r->published = r->folder;

        r->frames = open_file(leaf + "/frames.csv");
        r->signals = open_file(leaf + "/signals.csv");
        r->events = open_file(leaf + "/events.log");
        if (r->frames == nullptr || r->signals == nullptr ||
            r->events == nullptr) {
            const int e = errno;
            ecuio_detail_set("can't create the recording's files: %s",
                             e != 0 ? errno_text(e).c_str()
                                    : "the folder refused them");
            return ECUIO_ERR_RECORDING_FAILED;
        }
    }

    /* The caller's header lines, under one key, written at attach */
    std::string caller;
    for (uint32_t i = 0; i < options->header_line_count; ++i) {
        if (options->header_lines[i] != nullptr) {
            caller += "caller  " + one_line(options->header_lines[i]) + "\n";
        }
    }
    r->items.push_back(item{item_kind::begin, 0, caller, {}, {}});
    if (r->armed) {
        r->armed_caller = caller;
    }

    try {
        r->writer = std::thread([rec = r.get()] { rec->run(); });
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = r.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_signal_file(ecuio_recorder_t* recorder,
                                          const ecuio_signal_file_t* file)
{
    if (recorder == nullptr || file == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const std::string& bytes = ecuio_sf::bytes_of(*file);
    return ecuio_recorder_take_signals(recorder, &file->set, bytes.data(),
                                       bytes.size(), file->plugin.c_str());
}

ecuio_status_t ecuio_session_record(ecuio_session_t* session,
                                    ecuio_recorder_t* recorder)
{
    if (session == nullptr || recorder == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    /* A recorder has one session ever; a session one recorder at a time
     * (ADR-0056 section 3), judged under the link's mutex below */
    if (recorder->attached.load()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->signals_set != nullptr &&
        recorder->signals_set != session->active_signal_set) {
        ecuio_detail_set("the signal file isn't what decodes the session "
                         "(its profile is %s)",
                         session->info.profile);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    /* Built in locals first: the recorder changes only once the link is
     * taken, so a refused attach leaves it ready for the next try */
    const std::shared_ptr<ecuio_record_link> link = session->record_link;
    std::vector<def_entry> defs;
    std::vector<uint32_t> rx_ids; /* each module's reply ID */
    std::string header;
    size_t conditions_at = 0;
    int32_t link_state = ECUIO_OK;
    uint32_t link_episodes = 0;
    /* An armed recorder's: a window's header lines from core to frame
     * flags (the conditions line's place in them) and the signal lines,
     * and room for a state per condition */
    std::string armed_core;
    size_t armed_conditions_at = 0;
    std::string armed_signals;
    std::vector<uint8_t> armed_cond;
    std::vector<ecuio_recorder::tail_state> armed_tail;
    focus_named armed_focus;
    uint64_t zero_us = 0;
    uint64_t slept_at_attach = 0;
    focus_named named_focus;
    std::chrono::system_clock::time_point start_wall{};
    try {
        const ecuio_signal_set_t* set = session->active_signal_set;
        rx_ids = session->module_rx; /* fixed since create */
        header = std::string(ecuio_recording_rows::kFormat3) + "\n";
        for (uint32_t i = 0;
             set != nullptr && set->defs != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const size_t nlen = strnlen(d.name, sizeof d.name);
            const size_t ulen = strnlen(d.unit, sizeof d.unit);
            const bool text = is_text_kind(d.kind);
            defs.push_back(def_entry{d.id, text, csv_quoted(d.name, nlen),
                                     csv_quoted(d.unit, ulen)});
        }
        std::sort(
            defs.begin(), defs.end(),
            [](const def_entry& a, const def_entry& b) { return a.id < b.id; });

        /* The zero and the wall clock read back to back */
        zero_us = ecuio_record_clock_us();
        slept_at_attach = ecuio_clock_slept_us() + recorder->slept_added.load();
        start_wall = std::chrono::system_clock::now();

        header += started_lines(start_wall);
        const size_t core_at = header.size();
        header += std::string("core  ") + ecuio_version_string() + "\n";
        header += "uri  " + one_line(session->uri.c_str()) + "\n";
        header += std::string("profile  ") + session->info.profile + "\n";
        if (!recorder->signals_json.empty()) {
            header += std::string(ecuio_recording_rows::kSignalFileKey) +
                      ecuio_recording_rows::kSignalFileName + "," +
                      recorder->signals_sha + "\n";
        }
        if (!recorder->signals_plugin.empty()) {
            header += std::string(ecuio_recording_rows::kPluginKey) +
                      recorder->signals_plugin + "\n";
        }
        /* The conditions line goes here once the set is read under the
         * locks, into room kept now */
        conditions_at = header.size();
        /* A replay's frames carry its source's flags, so it writes its
         * source's line, which a recording from before ADR-0049 lacks */
        header += "frame flags  ";
        header += session->info.backend != ECUIO_BACKEND_REPLAY ? "complete"
                  : session->source_frame_flags.empty()
                      ? "from an older recording"
                      : session->source_frame_flags.c_str();
        header += "\n";
        /* A window's header has these lines from the copy, the conditions
         * line put in where the full header takes it */
        if (recorder->armed) {
            armed_core = header.substr(core_at);
            armed_core.reserve(armed_core.size() + kConditionsLineMax);
            armed_conditions_at = conditions_at - core_at;
        }
        /* A session attached to after start (ADR-0056 section 3) says what
         * it had come to: its focus in pick order (none: no line) and, once
         * started, the link's state and episodes in the link lines' words,
         * so a reader needn't wait for a transition */
        {
            std::lock_guard<std::mutex> f(session->focus_mutex);
            named_focus = focus_now(session);
        }
        size_t named = 0;
        /* As many lines as the names take; a reader appends them */
        const std::vector<std::string> parts =
            focus_parts(set, named_focus.ids, &named);
        header += focus_header(parts, named, named_focus);
        if (recorder->armed) {
            armed_focus = named_focus;
        }
        if (session->started) {
            ecuio_session_get_error_state(session, &link_state, &link_episodes);
            header += header_link(link_state, link_episodes);
        }
        const size_t signals_at = header.size();
        for (uint32_t i = 0;
             set != nullptr && set->defs != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const bool enumeration = d.kind == ECUIO_VALUE_ENUM;
            const std::string name =
                csv_quoted(d.name, strnlen(d.name, sizeof d.name));
            header += ecuio_recording_lines::signal_line(
                name,
                is_text_kind(d.kind) ? "text"
                : enumeration        ? "enum"
                                     : "number",
                csv_quoted(d.unit, strnlen(d.unit, sizeof d.unit)),
                d.decimal_places, d.plot_rank, d.group);
            header += label_lines(set, d, name);
        }
        header.reserve(header.size() + kConditionsLineMax);
        if (recorder->armed) {
            armed_signals = header.substr(signals_at);
            armed_cond.reserve(ECUIO_CONDITION_MAX_CONDITIONS);
            armed_tail.reserve(ECUIO_CONDITION_MAX_CONDITIONS);
        }
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    /* The set the session judges by, read under the link's mutex and
     * callback_mutex, so it is the one the recording keeps */
    std::shared_ptr<const ecuio_condition_record> conditions;
    uint32_t generation = 0;

    /* The recorder takes what was made, under the link's mutex. Its one
     * allocation, the header's text joined, comes before any change, so a
     * throw leaves the recorder ready for the next try. */
    const auto commit = [&] {
        if (conditions != nullptr) {
            char line[kConditionsLineMax];
            const int n = std::snprintf(line, sizeof line, "%s%s,%s\n",
                                        ecuio_recording_rows::kConditionsKey,
                                        ecuio_recording_rows::kConditionsName,
                                        conditions->sha256.c_str());
            const size_t len = n < 0 ? 0
                               : static_cast<size_t>(n) < sizeof line
                                   ? static_cast<size_t>(n)
                                   : sizeof line - 1;
            header.insert(conditions_at, line, len);
            if (recorder->armed) {
                armed_core.insert(armed_conditions_at, line, len);
            }
        }
        if (!recorder->armed) {
            static const char kColumns[] =
                "columns  local time, seconds since the recording started "
                "(host_us / 1e6 in the CSV files), kind, text\n\n";
            std::lock_guard<std::mutex> n(recorder->note_m);
            /* The caller's lines open() queued, then the columns */
            item& begin = recorder->items.front();
            header.reserve(header.size() + begin.text.size() + sizeof kColumns);
            header += begin.text;
            header += kColumns;
            begin.text.swap(header);
            begin.bytes = std::move(recorder->signals_json);
        }
        if (recorder->armed) {
            /* Room for each state was kept before the lock, so these sizes
             * take no allocation; conditions is not NULL (the check) */
            const size_t n = conditions->conditions.size();
            armed_cond.resize(n);
            armed_tail.resize(n);
            for (const ecuio_condition_record::entry& c :
                 conditions->conditions) {
                if (c.record) {
                    recorder->longest_before =
                        (std::max)(recorder->longest_before,
                                   uint64_t{c.before_us});
                }
            }
            recorder->scan_on.swap(armed_cond);
            recorder->tail_cond.swap(armed_tail);
            recorder->armed_core.swap(armed_core);
            recorder->armed_signals.swap(armed_signals);
            recorder->window_json.swap(recorder->signals_json);
            recorder->win_link_state = link_state;
            recorder->win_link_episodes = link_episodes;
            recorder->win_focus.ids.swap(armed_focus.ids);
            recorder->win_focus.pick = armed_focus.pick;
            recorder->win_focus.min_sweep_ms = armed_focus.min_sweep_ms;
        }
        recorder->conditions = std::move(conditions);
        recorder->conditions_generation = generation;
        recorder->defs = std::move(defs);
        recorder->rx_ids.swap(rx_ids);
        recorder->channels = static_cast<uint32_t>(recorder->rx_ids.size());
        recorder->link = link;
        recorder->zero_us = zero_us;
        recorder->slept_at_attach = slept_at_attach;
        recorder->start_wall = start_wall;
        recorder->wall_base = start_wall;
        recorder->named_focus = std::move(named_focus);
    };
    {
        /* Under the mutex that orders detaches, so a closing recorder or a
         * second attach sees one state of the link, and the refusal is judged
         * here alone. The zero goes in before active, whose release publishes
         * it to the worker (dispatch pins the recorder, then reads the
         * zero). */
        std::lock_guard<std::mutex> g(link->m);
        if (link->active.load(std::memory_order_seq_cst) != nullptr) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> c(session->callback_mutex);
            conditions = session->conditions_record;
            generation = session->conditions_generation;
        }
        /* An armed recorder that nothing could open a window for is
         * refused, so the caller learns it now (ADR-0063 section 7) */
        if (recorder->armed) {
            bool records = false;
            for (size_t i = 0; conditions != nullptr &&
                               i < conditions->conditions.size() && !records;
                 ++i) {
                records = conditions->conditions[i].record;
            }
            if (!records) {
                ecuio_detail_set(
                    conditions == nullptr
                        ? "no condition records: the session has no set of "
                          "conditions attached"
                        : "no condition records: no enabled condition in the "
                          "set has record");
                return ECUIO_ERR_INVALID_ARGUMENT;
            }
        }
        if (const auto hook = g_before_record_attach.load()) {
            hook(g_before_record_attach_user.load());
        }
        try {
            commit();
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        link->zero_us.store(zero_us, std::memory_order_relaxed);
        link->active.store(recorder, std::memory_order_release);
    }
    recorder->attached.store(true, std::memory_order_release);
    /* A focus set since the header's read may have found no recorder to
     * pin; the compare keeps it from being written twice */
    ecuio_record_focus(session);
    return ECUIO_OK;
}

} // extern "C"

namespace {

/* The focus's activity lines into the pinned recorder's note queue: the
 * count, the minimum when there is one, then the names */
void push_focus(ecuio_recorder_t* r, const ecuio_signal_set_t* set,
                const focus_named& f)
{
    size_t named = 0;
    const std::vector<std::string> parts = focus_parts(set, f.ids, &named);
    std::string lines;
    if (parts.empty()) {
        lines = "none: every signal asked";
    }
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k == 0) {
            lines +=
                std::to_string(named) + (named == 1 ? " signal" : " signals");
            if (f.min_sweep_ms != 0) {
                lines += ", at least " + std::to_string(f.min_sweep_ms) +
                         " ms between sweeps";
            }
            lines += ": ";
        } else {
            lines += std::string(1, '\0') + "more: ";
        }
        lines += parts[k];
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
    std::lock_guard<std::mutex> g(r->note_m);
    if (r->items.size() >= r->note_cap) {
        r->notes_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        r->items.push_back(item{item_kind::focus, host_us,
                                f.pick ? "pick" : "focus", std::move(lines),
                                f});
    }
}

} // namespace

void ecuio_record_focus(ecuio_session_t* session)
{
    ecuio_record_link& link = *session->record_link;
    ecuio_recorder_t* r = nullptr;
    {
        std::lock_guard<std::mutex> g(link.m);
        r = ecuio_record_pin(link);
    }
    if (r == nullptr) {
        return;
    }
    try {
        focus_named f;
        bool changed = false;
        {
            std::lock_guard<std::mutex> g(session->focus_mutex);
            f = focus_now(session);
            changed = !(f == r->named_focus);
            if (changed) {
                r->named_focus = f;
            }
        }
        if (changed) {
            push_focus(r, session->active_signal_set, f);
        }
    } catch (...) {
        /* The focus stands; its line is lost, counted as a note */
        r->notes_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ecuio_record_unpin(link);
}

extern "C" {

ecuio_status_t ecuio_recorder_note(ecuio_recorder_t* recorder,
                                   const char* utf8_line)
{
    if (recorder == nullptr || utf8_line == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!recorder->attached.load(std::memory_order_acquire)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->failed()) {
        return ECUIO_ERR_RECORDING_FAILED;
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us =
        now > recorder->zero_us ? now - recorder->zero_us : 0;
    try {
        std::string line = one_line(utf8_line);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::note, host_us, std::move(line), {}, {}});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_mark(ecuio_recorder_t* recorder,
                                   const char* utf8_label)
{
    if (recorder == nullptr || utf8_label == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!recorder->attached.load(std::memory_order_acquire)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->failed()) {
        return ECUIO_ERR_RECORDING_FAILED;
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us =
        now > recorder->zero_us ? now - recorder->zero_us : 0;
    try {
        std::string label = one_line(utf8_label);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::mark, host_us, std::move(label), {}, {}});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_get_state(const ecuio_recorder_t* recorder,
                                        ecuio_recorder_state_t* out)
{
    if (recorder == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::memset(out, 0, sizeof *out);
    if (recorder->drained.load(std::memory_order_acquire)) {
        out->flags |= ECUIO_RECORDER_DRAINED;
    }
    const uint32_t state = recorder->state.load(std::memory_order_acquire);
    out->state = state == ECUIO_RECORDING_FAILED ? ECUIO_RECORDING_FAILED
                 : recorder->detached.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_DETACHED
                 : recorder->armed &&
                         !recorder->window_open.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_ARMED
                     : ECUIO_RECORDING_ACTIVE;
    out->frame_rows = recorder->frame_rows.load(std::memory_order_relaxed);
    out->value_rows = recorder->value_rows.load(std::memory_order_relaxed);
    out->records_dropped =
        recorder->records_dropped.load(std::memory_order_relaxed);
    out->notes_dropped =
        recorder->notes_dropped.load(std::memory_order_relaxed);
    out->conditions_lost =
        recorder->conditions_lost.load(std::memory_order_relaxed);
    out->windows = recorder->windows_opened.load(std::memory_order_acquire);
    out->preroll_short =
        recorder->prerolls_short.load(std::memory_order_relaxed);
    out->frames_passed =
        recorder->frames_passed.load(std::memory_order_relaxed);
    if (state == ECUIO_RECORDING_FAILED) {
        std::lock_guard<std::mutex> g(recorder->reason_m);
        const size_t n =
            (std::min)(recorder->reason.size(), sizeof out->reason - 1);
        std::memcpy(out->reason, recorder->reason.data(), n);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_get_folder(const ecuio_recorder_t* recorder,
                                         char* utf8, uint32_t capacity,
                                         uint32_t* out_len)
{
    if (recorder == nullptr || out_len == nullptr ||
        (utf8 == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> g(recorder->folder_m);
    const std::string& f = recorder->published;
    *out_len = static_cast<uint32_t>(f.size());
    if (capacity > 0) {
        const size_t n =
            (std::min)(f.size(), static_cast<size_t>(capacity) - 1);
        std::memcpy(utf8, f.data(), n);
        utf8[n] = '\0';
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_close(ecuio_recorder_t* recorder,
                                    const char* utf8_why)
{
    if (recorder == nullptr) {
        return ECUIO_OK;
    }
    std::unique_ptr<ecuio_recorder> r(recorder);
    if (r->link != nullptr) {
        ecuio_record_detach(*r->link, r.get());
    }
    {
        std::lock_guard<std::mutex> g(r->wake_m);
        r->stop = true;
    }
    r->wake_cv.notify_all();
    if (r->writer.joinable()) {
        r->writer.join();
    }
    /* The writer has drained everything; the summary is the last line */
    if (r->attached.load() && r->armed) {
        /* The window still open, if one is: what the worker carried, then
         * its end, at the close while a record condition is ON */
        if (r->in_window && !r->windows_q.empty()) {
            const ecuio_recorder::window w = r->windows_q.front();
            const uint64_t now = ecuio_record_clock_us();
            const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
            /* A window whose conditions cleared ends at its last record,
             * one still ON at the close; the carry goes in up to its end */
            uint64_t end_host = w.open_end ? host_us : r->win_last_host;
            for (uint32_t k = 0; k < r->carry_count; ++k) {
                const event_body& e = r->carry[r->carry_start + k];
                if (w.open_end || e.t_us <= w.end_us) {
                    r->take_event(e);
                    end_host = (std::max)(end_host, e.host_us);
                }
            }
            if (w.open_end) {
                if (r->carry_lost > 0) {
                    r->lost_line(r->carry_lost_us, r->carry_lost);
                }
                if (r->link_pending) {
                    r->event_line(r->pending_link.host_us, "link",
                                  link_text(r->pending_link.state,
                                            r->pending_link.episodes));
                }
                if (r->pending_dropped > 0) {
                    r->drop_line(end_host, r->pending_dropped);
                    r->win_dropped += r->pending_dropped;
                }
            }
            r->close_window(
                end_host,
                w.open_end ? one_line(utf8_why != nullptr ? utf8_why : "closed")
                           : std::string(kWindowEnded));
        }
    } else if (r->attached.load()) {
        std::string why = one_line(utf8_why != nullptr ? utf8_why : "closed");
        const uint64_t now = ecuio_record_clock_us();
        const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
        /* What the worker couldn't queue before it let go: the detach
         * waited for it, so its fields are this thread's now */
        for (uint32_t k = 0; k < r->carry_count; ++k) {
            r->take_event(r->carry[r->carry_start + k]);
        }
        if (r->carry_lost > 0) {
            r->lost_line(r->carry_lost_us, r->carry_lost);
        }
        if (r->link_pending) {
            r->event_line(
                r->pending_link.host_us, "link",
                link_text(r->pending_link.state, r->pending_link.episodes));
        }
        if (r->pending_dropped > 0) {
            r->drop_line(host_us, r->pending_dropped);
        }
        r->event_out += '\n';
        r->event_line(
            host_us, "end",
            why + ": " + std::to_string(r->frame_rows.load()) + " frames, " +
                std::to_string(r->value_rows.load()) + " values, " +
                std::to_string(r->records_dropped.load()) +
                " records dropped, " + std::to_string(r->notes_dropped.load()) +
                " notes dropped");
        r->write(r->events, r->event_out);
        r->write(r->markers, r->marker_out);
    }
    r->sync_all();
    r->close_files();
    return r->failed() ? ECUIO_ERR_RECORDING_FAILED : ECUIO_OK;
}

void ecuio_session_test_before_record_attach(void (*hook)(void* user),
                                             void* user)
{
    /* The user before the hook, and a clear leaves the user: a reader
     * that took the old hook still finds its own */
    if (hook != nullptr) {
        g_before_record_attach_user.store(user);
    }
    g_before_record_attach.store(hook);
}

void ecuio_recorder_test_hold(ecuio_recorder_t* recorder, int hold)
{
    if (recorder != nullptr) {
        recorder->hold.store(hold != 0, std::memory_order_release);
    }
}

void ecuio_recorder_test_fail_writes(ecuio_recorder_t* recorder)
{
    if (recorder != nullptr) {
        recorder->fail_next.store(true, std::memory_order_release);
    }
}

void ecuio_recorder_test_add_slept(ecuio_recorder_t* recorder, uint64_t us)
{
    if (recorder != nullptr) {
        recorder->slept_added.fetch_add(us, std::memory_order_relaxed);
    }
}

void ecuio_recorder_test_set_clock(int on, uint64_t now_us)
{
    g_test_clock_us.store(now_us, std::memory_order_release);
    g_test_clock_on.store(on != 0, std::memory_order_release);
}

uint64_t ecuio_recorder_test_wakes(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->wakes.load(std::memory_order_relaxed)
                               : 0;
}

uint64_t ecuio_recorder_test_syncs(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->syncs.load(std::memory_order_relaxed)
                               : 0;
}

} // extern "C"
