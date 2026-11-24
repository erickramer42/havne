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
// This file holds the worker's puts, the test clock and the ring; the class
// is in recorder_impl.h, the attach (open, signal_file, session_record) in
// recorder_attach.cpp, the calls on a recorder in flight (focus, note, mark,
// state, folder, close) in recorder_control.cpp, its writer in
// recorder_writer.cpp and recorder_armed.cpp, the text helpers in
// recorder_text.cpp.

#include "recorder.h"

#include "condition_eval.h"
#include "error_detail.h"
#include "recorder_impl.h"
#include "recorder_text.h"
#include "recording_lines.h" /* signal_line */
#include "recording_keys.h"  /* a set's keys */
#include "recording_rows.h"  /* kFormat4, the first line */
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

constexpr auto kFinalTimingWait = std::chrono::seconds(1);
constexpr auto kFinalTimingRetry = std::chrono::milliseconds(10);

/* ecuio_recorder_test_set_clock's (internal.h) */
std::atomic<bool> g_test_clock_on{false};
std::atomic<uint64_t> g_test_clock_us{0};

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

uint64_t host_us_now(const ecuio_record_link& link)
{
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t zero = link.zero_us.load(std::memory_order_relaxed);
    return now > zero ? now - zero : 0;
}

/* The worker's: carried module states into the ring after a carried link
 * transition; false while they still don't fit */
bool put_pending_modules(ecuio_recorder_t* r)
{
    if (!r->modules_pending) {
        return true;
    }
    const uint64_t size = sizeof(record_head) + sizeof(modules_body);
    if (!ring_has_room(r, size)) {
        return false;
    }
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    const record_head rh{static_cast<uint32_t>(size), kRecordModules, 0};
    r->copy_in(h, &rh, sizeof rh);
    r->copy_in(h + sizeof rh, &r->pending_modules, sizeof r->pending_modules);
    r->head.store(h + size, std::memory_order_release);
    r->modules_pending = false;
    r->put_host_us = (std::max)(r->put_host_us, r->pending_modules.host_us);
    return true;
}

} // namespace

void ecuio_record_modules(ecuio_record_link* link, uint32_t closed,
                          uint32_t no_room, const uint8_t* reasons,
                          uint32_t count)
{
    if (link == nullptr) {
        return;
    }
    ecuio_recorder_t* r = ecuio_record_pin(*link);
    if (r == nullptr) {
        return;
    }
    /* States still carried give way to these: the latest are a reader's */
    modules_body& mb = r->pending_modules;
    mb = modules_body{host_us_now(*link), closed, no_room, {}};
    std::memcpy(mb.reasons, reasons,
                (std::min)(count, uint32_t{ECUIO_MAX_CHANNELS}));
    r->modules_pending = true;
    if (put_pending_conditions(r) && put_pending_link(r)) {
        put_pending_modules(r);
    }
    ecuio_record_unpin(*link);
}

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
            put_pending_modules(r) && ring_has_room(r, size)) {
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
                              uint32_t event_count, uint32_t generation,
                              bool access_cut)
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
        !put_pending_modules(r) || !ring_has_room(r, size)) {
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
                  frame->flags | (access_cut ? ECUIO_FRAME_FLAG_ACCESS_CUT : 0u),
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
    const bool behind = !put_pending_conditions(r) || !put_pending_link(r) ||
                        !put_pending_modules(r);
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
        /* One header line for each line of plugin */
        std::string lines;
        for (const char* at = plugin; *at != '\0';) {
            const char* end = strchr(at, '\n');
            const std::string one(at, end != nullptr ? end : at + strlen(at));
            if (!one.empty()) {
                lines += ecuio_recording_rows::kPluginKey +
                         one_line(one.c_str()) + "\n";
            }
            at += one.size() + (end != nullptr ? 1 : 0);
        }
        recorder->signals_plugin = std::move(lines);
        recorder->signals_json = std::move(json);
    } catch (...) {
        recorder->signals_sha.clear();
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    recorder->signals_set = set;
    return ECUIO_OK;
}

extern "C" {

void ecuio_recorder_test_set_clock(int on, uint64_t now_us)
{
    g_test_clock_us.store(now_us, std::memory_order_release);
    g_test_clock_on.store(on != 0, std::memory_order_release);
}

} // extern "C"
