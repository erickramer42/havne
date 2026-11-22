// File/layer: core/src/recorder_armed.cpp | C++17, Qt-free; an armed
//             recorder's windows (ADR-0063 section 7)
// Testing: UNIT: ArmedRecorderTest, RecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge)
//
// The armed writer's members: the scan ahead of the tail, the windows it
// keeps, and the tail that writes a record inside one or lets it go.

#include "recorder_impl.h"
#include "recorder_text.h"
#include "recording_rows.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ecuio_rec {

/* The pre-roll a full ring cuts: past this many of its bytes queued, an
 * armed writer lets its oldest record go */
constexpr uint64_t kPressedQuarters = 3;

} // namespace ecuio_rec

/* The caller's items, held with their times; the begin item is the
 * attach's header, which a window builds itself */
void ecuio_recorder::take_items()
{
    std::deque<item> taken;
    {
        std::lock_guard<std::mutex> g(note_m);
        taken.swap(items);
    }
    for (item& it : taken) {
        if (it.kind == item_kind::begin) {
            continue;
        }
        if (pending.size() >= note_cap) {
            notes_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        pending.push_back(std::move(it));
    }
}

/* Each condition event in a record's body */
template <typename F>
void ecuio_recorder::each_event(uint32_t type, const uint8_t* body, size_t size,
                                F f)
{
    size_t at = 0;
    uint32_t count = 0;
    if (type == kRecordFrame) {
        if (size < sizeof(frame_body)) {
            return;
        }
        frame_body fb;
        std::memcpy(&fb, body, sizeof fb);
        at = sizeof fb + fb.length;
        for (uint16_t v = 0; v < fb.values; ++v) {
            if (at + sizeof(value_head) > size) {
                return;
            }
            value_head vh;
            std::memcpy(&vh, body + at, sizeof vh);
            at += sizeof vh +
                  (is_text_kind(vh.kind) ? vh.text_len : sizeof(double));
        }
        count = fb.events;
    } else if (type == kRecordConditions) {
        if (size < sizeof(conditions_body)) {
            return;
        }
        conditions_body cb;
        std::memcpy(&cb, body, sizeof cb);
        at = sizeof cb;
        count = cb.events;
    }
    for (uint32_t k = 0; k < count && at + sizeof(event_body) <= size;
         ++k) {
        event_body e;
        std::memcpy(&e, body + at, sizeof e);
        at += sizeof e;
        f(e);
    }
}

/* A record condition's change at the scan, at its session time: a rise
 * opens a window or carries the last one on; the last fall gives the
 * window its end, after past it or the record that settled it,
 * whichever is later */
void ecuio_recorder::scan_event(const event_body& e, uint64_t record_us)
{
    const ecuio_condition_record* set = conditions.get();
    if (set == nullptr || e.condition >= scan_on.size() ||
        e.condition >= set->conditions.size() ||
        !set->conditions[e.condition].record) {
        return;
    }
    const ecuio_condition_record::entry& c = set->conditions[e.condition];
    const bool on = e.state == ECUIO_CONDITION_ON &&
                    (e.flags & ECUIO_CONDITION_STALE) == 0;
    if (on == (scan_on[e.condition] != 0)) {
        return;
    }
    scan_on[e.condition] = on ? 1 : 0;
    if (on) {
        ++scan_active;
        const uint64_t start =
            e.t_us > c.before_us ? e.t_us - c.before_us : 0;
        if (!windows_q.empty()) {
            window& w = windows_q.back();
            if (w.open_end || start <= w.end_us) {
                w.open_end = true;
                if (!w.opened && start < w.start_us) {
                    w.start_us = start;
                }
                return;
            }
        }
        window w;
        w.start_us = start;
        w.trigger = e.condition;
        windows_q.push_back(w);
        return;
    }
    --scan_active;
    if (windows_q.empty()) {
        return;
    }
    window& w = windows_q.back();
    w.end_us =
        (std::max)(w.end_us, (std::max)(e.t_us + c.after_us, record_us));
    if (scan_active == 0) {
        w.open_end = false;
    }
}

/* What the tail passes, written or not: the conditions' states and the
 * link's, for the next window's header and held lines */
void ecuio_recorder::follow(const record_head& rh, const uint8_t* body,
                            size_t size)
{
    if (rh.type == kRecordLink && size >= sizeof(link_body)) {
        link_body lb;
        std::memcpy(&lb, body, sizeof lb);
        win_link_state = lb.state;
        win_link_episodes = lb.episodes;
        return;
    }
    each_event(rh.type, body, size, [this](const event_body& e) {
        if (e.condition < tail_cond.size()) {
            tail_state& s = tail_cond[e.condition];
            s.on = e.state == ECUIO_CONDITION_ON;
            s.stale = (e.flags & ECUIO_CONDITION_STALE) != 0;
            s.value = e.value;
            s.term = e.term;
        }
    });
}

/* A record into the open window */
void ecuio_recorder::write_record(const record_head& rh, const uint8_t* body,
                                  size_t size)
{
    if (rh.dropped_before > 0 && size >= sizeof(uint64_t)) {
        uint64_t host_us;
        std::memcpy(&host_us, body, sizeof host_us);
        drop_line(host_us, rh.dropped_before);
        win_dropped += rh.dropped_before;
    }
    switch (rh.type) {
    case kRecordFrame:
        take_frame(body, size);
        break;
    case kRecordLink:
        take_link(body, size);
        break;
    case kRecordTiming:
        take_timing(body, size);
        break;
    case kRecordConditions:
        take_conditions(body, size);
        break;
    default:
        break;
    }
}

/* The session time of the first frame from ring position at on */
uint64_t ecuio_recorder::first_frame_t_us(uint64_t at) const
{
    while (scan - at >= sizeof(record_head)) {
        record_head rh;
        copy_out(at, &rh, sizeof rh);
        if (rh.size < sizeof rh || rh.size > scan - at) {
            break;
        }
        if (rh.type == kRecordFrame &&
            rh.size >= sizeof rh + sizeof(frame_body)) {
            frame_body fb;
            copy_out(at + sizeof rh, &fb, sizeof fb);
            return fb.timestamp_us;
        }
        at += rh.size;
    }
    return 0;
}

/* A window's header: a recording begun mid-session's, at its start */
std::string ecuio_recorder::window_header(const window& w)
{
    std::string h = std::string(ecuio_recording_rows::kFormat4) + "\n";
    h += started_lines(wall_base);
    h += armed_core;
    const ecuio_condition_record::entry& c =
        conditions->conditions[w.trigger];
    h += "trigger  " + c.key + "," +
         std::to_string((uint64_t{c.before_us} + 500) / 1000) + " ms\n";
    size_t named = 0;
    const std::vector<std::string> parts =
        focus_parts(defs, win_focus.ids, &named);
    h += focus_header(parts, named, win_focus);
    h += header_link(win_link_state, win_link_episodes);
    h += armed_signals;
    h += armed_caller;
    h += "columns  local time, seconds since the recording started "
         "(host_us / 1e6 in the CSV files), kind, text\n\n";
    return h;
}

/* The front window's folder, made at its first record (at, host_us):
 * the copies and the header written and flushed before the folder is
 * given to get_folder, so a front end never reads a half-made one */
void ecuio_recorder::open_window(window& w, uint64_t at, uint64_t host_us)
{
    w.opened = true;
    base_us = host_us;
    wall_base = start_wall + std::chrono::microseconds(host_us);
    std::string leaf;
    std::string why;
    const std::time_t second =
        std::chrono::system_clock::to_time_t(wall_base);
    if (second != name_second) {
        name_second = second;
        name_next = 1;
    }
    if (failed() || !make_recording_folder(root, second, label_tail, &leaf,
                                           &why, &name_next)) {
        if (!why.empty()) {
            fail_text(why);
        }
        return;
    }
    folder = absolute(leaf);
    errno = 0;
    if ((!window_json.empty() &&
         !write_bytes(folder + "/" + ecuio_recording_rows::kSignalFileName,
                      window_json)) ||
        !write_bytes(folder + "/" + ecuio_recording_rows::kConditionsName,
                     conditions->bytes)) {
        fail(errno != 0 ? errno : EIO);
        return;
    }
    frames = open_file(folder + "/frames.csv");
    signals = open_file(folder + "/signals.csv");
    events = open_file(folder + "/events.log");
    if (frames == nullptr || signals == nullptr || events == nullptr) {
        fail(errno != 0 ? errno : EIO);
        return;
    }
    std::memset(timing_totals, 0, sizeof timing_totals);
    win_frames0 = frame_rows.load(std::memory_order_relaxed);
    win_values0 = value_rows.load(std::memory_order_relaxed);
    win_notes0 = notes_dropped.load(std::memory_order_relaxed);
    win_dropped = 0;
    win_last_host = host_us;
    event_out += window_header(w);
    begin_rows();
    /* Each condition standing ON at the window's start */
    const uint64_t t_us = first_frame_t_us(at);
    for (uint32_t i = 0; i < tail_cond.size(); ++i) {
        const tail_state& s = tail_cond[i];
        if (!s.on) {
            continue;
        }
        event_body e{};
        e.host_us = host_us;
        e.t_us = t_us;
        e.value = s.value;
        e.condition = i;
        e.term = s.term;
        e.state = ECUIO_CONDITION_ON;
        e.flags = static_cast<uint8_t>(
            ECUIO_CONDITION_HELD | (s.stale ? ECUIO_CONDITION_STALE : 0u));
        take_event(e);
    }
    write_outs();
    flush_all();
    if (early_any && w.start_us <= early_upto_us) {
        prerolls_short.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> g(folder_m);
        published = folder;
    }
    windows_opened.fetch_add(1, std::memory_order_release);
    in_window = true;
    window_open.store(true, std::memory_order_release);
}

/* The open window's end line at end_us and its files closed */
void ecuio_recorder::close_window(uint64_t end_us, const std::string& why)
{
    if (!in_window) {
        return;
    }
    event_out += '\n';
    event_line(
        end_us, "end",
        why + ": " + std::to_string(frame_rows.load() - win_frames0) +
            " frames, " + std::to_string(value_rows.load() - win_values0) +
            " values, " + std::to_string(win_dropped) +
            " records dropped, " +
            std::to_string(notes_dropped.load() - win_notes0) +
            " notes dropped");
    write_outs();
    sync_all();
    close_files();
    in_window = false;
    window_open.store(false, std::memory_order_release);
}

/* The held items at or before limit, the recording's time of the record
 * the tail is at: into the window when that record is written in it,
 * else gone; a focus moves the next header's either way */
void ecuio_recorder::flush_items(uint64_t limit)
{
    while (!pending.empty() && pending.front().host_us <= limit) {
        const item it = std::move(pending.front());
        pending.pop_front();
        if (it.kind == item_kind::focus) {
            win_focus = it.focus;
        }
        if (in_window) {
            write_item(it);
        }
    }
}

/* The held focus items at or before limit, taken into win_focus: a
 * window opening there names them in its header, once */
void ecuio_recorder::take_focus_items(uint64_t limit)
{
    for (auto it = pending.begin();
         it != pending.end() && it->host_us <= limit;) {
        if (it->kind == item_kind::focus) {
            win_focus = it->focus;
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
}

/* A record's place in session time: a frame's own, a link change's
 * own, or the newest before it, from pos */
uint64_t ecuio_recorder::place_of(const record_head& rh, const uint8_t* body,
                                  size_t size, uint64_t pos)
{
    if (rh.type == kRecordFrame && size >= sizeof(frame_body)) {
        frame_body fb;
        std::memcpy(&fb, body, sizeof fb);
        return (std::max)(pos, fb.timestamp_us);
    }
    if (rh.type == kRecordLink && size >= sizeof(link_body)) {
        link_body lb;
        std::memcpy(&lb, body, sizeof lb);
        return (std::max)(pos, lb.t_us);
    }
    return pos;
}

bool ecuio_recorder::pressed() const
{
    const uint64_t queued = head.load(std::memory_order_acquire) -
                            tail.load(std::memory_order_relaxed);
    return queued > cap / 4 * kPressedQuarters;
}

/* The tail as far as the windows allow; final (the recorder closing,
 * or its session gone) holds nothing back */
bool ecuio_recorder::advance_tail(bool final)
{
    bool any = false;
    for (;;) {
        const uint64_t t = tail.load(std::memory_order_relaxed);
        if (scan - t < sizeof(record_head)) {
            break;
        }
        record_head rh;
        copy_out(t, &rh, sizeof rh);
        const size_t body = rh.size - sizeof rh;
        if (scratch.size() < body) {
            scratch.resize(body);
        }
        copy_out(t + sizeof rh, scratch.data(), body);
        uint64_t host_us = 0;
        if (body >= sizeof host_us) {
            std::memcpy(&host_us, scratch.data(), sizeof host_us);
        }
        const uint64_t pos = place_of(rh, scratch.data(), body, tail_pos);
        window* w = windows_q.empty() ? nullptr : &windows_q.front();
        if (w != nullptr && pos >= w->start_us &&
            (w->open_end || pos <= w->end_us)) {
            if (!w->opened) {
                take_focus_items(host_us);
                open_window(*w, t, host_us);
            }
            flush_items(host_us);
            if (in_window) { /* a failed open writes nothing */
                write_record(rh, scratch.data(), body);
                win_last_host = (std::max)(win_last_host, host_us);
            }
        } else if (w != nullptr && !w->open_end && pos > w->end_us) {
            /* Past the window's end: a firing whose before reaches
             * back into it would carry it on, until the scan is that
             * far past its end */
            if (!final && !pressed() &&
                !(scanned_frame &&
                  newest_frame_us > w->end_us + longest_before)) {
                break;
            }
            if (!w->opened) { /* a full ring let all of it go */
                prerolls_short.fetch_add(1, std::memory_order_relaxed);
            }
            close_window(win_last_host, kWindowEnded);
            windows_q.pop_front();
            continue;
        } else {
            const bool old =
                scanned_frame && pos + longest_before < newest_frame_us;
            if (!final && !old) {
                if (!pressed()) {
                    break;
                }
                early_any = true;
                early_upto_us = (std::max)(early_upto_us, pos);
            }
            flush_items(host_us);
            if (rh.type == kRecordFrame) {
                frames_passed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        follow(rh, scratch.data(), body);
        tail_pos = pos;
        tail.store(t + rh.size, std::memory_order_release);
        any = true;
    }
    return any;
}

/* One wake of an armed writer: the scan to the head, the tail after
 * it; final leaves the open window for close to end */
bool ecuio_recorder::armed_pass(bool final)
{
    bool any = false;
    const uint64_t h = head.load(std::memory_order_acquire);
    while (h - scan >= sizeof(record_head)) {
        record_head rh;
        copy_out(scan, &rh, sizeof rh);
        if (rh.size < sizeof rh || rh.size > h - scan) {
            break;
        }
        const size_t body = rh.size - sizeof rh;
        if (scratch.size() < body) {
            scratch.resize(body);
        }
        copy_out(scan + sizeof rh, scratch.data(), body);
        const uint64_t pos =
            place_of(rh, scratch.data(), body, newest_frame_us);
        if (rh.type == kRecordFrame || rh.type == kRecordLink) {
            newest_frame_us = pos;
            scanned_frame = true;
        }
        each_event(
            rh.type, scratch.data(), body,
            [this, pos](const event_body& e) { scan_event(e, pos); });
        scan += rh.size;
        any = true;
    }
    any = advance_tail(final) || any;
    if (final) {
        /* The tail is at the scan: a window still unopened had all of
         * its records let go by a full ring */
        while (!windows_q.empty() && !windows_q.front().opened) {
            prerolls_short.fetch_add(1, std::memory_order_relaxed);
            windows_q.pop_front();
        }
        /* What came after the last record: the open window's while a
         * record condition is ON, else past its end and in no window */
        const bool ended =
            in_window && !windows_q.empty() && !windows_q.front().open_end;
        flush_items(ended ? win_last_host : UINT64_MAX);
    }
    write_outs();
    return any;
}
