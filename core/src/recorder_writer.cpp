// File/layer: core/src/recorder_writer.cpp | C++17, Qt-free; the
//             recorder's writer thread (ADR-0049 sections 1 to 4)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// The writer's members: the ring drained into the files' rows, the
// caller's items, the flushes and syncs, the sleep check and its loop;
// and the row helpers it alone calls, kept in its unit so a frame's
// row inlines them.

#include "access_cut.h"
#include "recorder_impl.h"
#include "recorder_text.h"
#include "recording_rows.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace ecuio_rec {

constexpr auto kBusyWake = std::chrono::milliseconds(10);
constexpr auto kIdleWake = std::chrono::milliseconds(100);
constexpr auto kFlushEvery = std::chrono::milliseconds(100);
constexpr auto kSyncEvery = std::chrono::seconds(2);
constexpr uint64_t kSleepLineUs = 1000000;

void append_u64(std::string& out, uint64_t v)
{
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%llu",
                                static_cast<unsigned long long>(v));
    out.append(buf, static_cast<size_t>(n));
}

/* A frame's bytes as frames.csv holds them, "67 01 00 00": zeroes past
 * its first kept where the frame is cut (kept 0: whole) */
void append_bytes(std::string& out, const uint8_t* p, uint16_t n, size_t kept)
{
    static const char kHex[] = "0123456789ABCDEF";
    for (uint16_t i = 0; i < n; ++i) {
        if (i > 0) {
            out += ' ';
        }
        const uint8_t b = kept != 0 && i >= kept ? 0 : p[i];
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    }
}

std::string cut_text(uint32_t id, const uint8_t* p, uint16_t n, size_t kept)
{
    char text[ECUIO_ACCESS_CUT_TEXT + 32];
    int at = std::snprintf(text, sizeof text, "0x%X", id);
    for (size_t i = 0; i < kept; ++i) {
        at += std::snprintf(text + at, sizeof text - static_cast<size_t>(at),
                            " %02X", p[i]);
    }
    at += std::snprintf(text + at, sizeof text - static_cast<size_t>(at), ": ");
    ecuio_capture_cut_text(p, n, kept, text + at,
                           sizeof text - static_cast<size_t>(at));
    return text;
}

void append_hex(std::string& out, uint32_t v)
{
    char buf[16];
    const int n = std::snprintf(buf, sizeof buf, "0x%X", v);
    out.append(buf, static_cast<size_t>(n));
}

/* Seconds since the start to the millisecond, rounded, as "+1.234" */
void append_seconds(std::string& out, uint64_t host_us)
{
    const uint64_t ms = (host_us + 500) / 1000;
    char buf[32];
    const int n = std::snprintf(buf, sizeof buf, "+%llu.%03llu",
                                static_cast<unsigned long long>(ms / 1000),
                                static_cast<unsigned long long>(ms % 1000));
    out.append(buf, static_cast<size_t>(n));
}

/* The interval's clock as havne-cli's timing lines name it */
const char* clock_name(uint32_t clock)
{
    switch (clock) {
    case ECUIO_TIMING_CLOCK_HOST:
        return "host";
    case ECUIO_TIMING_CLOCK_ADAPTER:
        return "adapter";
    case ECUIO_TIMING_CLOCK_KERNEL:
        return "kernel";
    default:
        return "none";
    }
}

/* A quantile, or nothing when it has too few samples */
void append_quantile(std::string& out, uint32_t us, bool has)
{
    if (has) {
        append_u64(out, us);
    }
}

} // namespace ecuio_rec

uint64_t ecuio_recorder::rel(uint64_t host_us) const
{
    return host_us > base_us ? host_us - base_us : 0;
}

void ecuio_recorder::fail_text(const std::string& why)
{
    {
        std::lock_guard<std::mutex> g(reason_m);
        if (failed()) {
            return;
        }
        reason = why;
    }
    state.store(ECUIO_RECORDING_FAILED, std::memory_order_release);
}

bool ecuio_recorder::failed() const
{
    return state.load(std::memory_order_acquire) == ECUIO_RECORDING_FAILED;
}

void ecuio_recorder::fail(int err)
{
    {
        std::lock_guard<std::mutex> g(reason_m);
        if (failed()) {
            return;
        }
        reason = write_reason(err);
    }
    state.store(ECUIO_RECORDING_FAILED, std::memory_order_release);
}

/* Writes and clears out; on a failed write the recording stops
 * writing, and what follows is dropped */
void ecuio_recorder::write(FILE* f, std::string& out)
{
    if (out.empty() || f == nullptr) {
        out.clear();
        return;
    }
    if (!failed()) {
        if (fail_next.exchange(false)) {
            fail(ENOSPC);
        } else if (std::fwrite(out.data(), 1, out.size(), f) !=
                   out.size()) {
            fail(errno != 0 ? errno : EIO);
        }
    }
    out.clear();
}

void ecuio_recorder::copy_out(uint64_t pos, void* dst, size_t n) const
{
    const size_t at = static_cast<size_t>(pos % cap);
    const size_t first = (std::min)(n, cap - at);
    std::memcpy(dst, ring.get() + at, first);
    if (n > first) {
        std::memcpy(static_cast<uint8_t*>(dst) + first, ring.get(),
                    n - first);
    }
}

const def_entry* ecuio_recorder::def(uint16_t id) const
{
    auto it = std::lower_bound(
        defs.begin(), defs.end(), id,
        [](const def_entry& d, uint16_t v) { return d.id < v; });
    return (it != defs.end() && it->id == id) ? &*it : nullptr;
}

/* "HH:mm:ss.zzz  +S.sss  <kind>  <text>" at host_us */
void ecuio_recorder::event_line(uint64_t host_us, const char* kind,
                                const std::string& text)
{
    host_us = rel(host_us);
    const auto at = wall_base + std::chrono::microseconds(host_us);
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            at.time_since_epoch())
            .count() %
        1000;
    const std::tm tm = local_tm(t);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03lld", tm.tm_hour,
                  tm.tm_min, tm.tm_sec, ms < 0 ? ms + 1000 : ms);
    event_out += stamp;
    event_out += "  ";
    append_seconds(event_out, host_us);
    event_out += "  ";
    event_out += kind;
    event_out += "  ";
    event_out += text;
    event_out += '\n';
}

void ecuio_recorder::drop_line(uint64_t host_us, uint64_t dropped)
{
    event_line(host_us, "drop",
               "the recording dropped " + std::to_string(dropped) +
                   (dropped == 1 ? " frame" : " frames") +
                   ": the disk fell behind");
}

void ecuio_recorder::add_marker(uint64_t host_us, const std::string& label)
{
    if (markers == nullptr && !failed()) {
        markers = open_file(folder + "/markers.csv");
        if (markers == nullptr) {
            fail(errno != 0 ? errno : EIO);
            return;
        }
        marker_out += "host_us,label\n";
    }
    append_u64(marker_out, rel(host_us));
    marker_out += ',';
    marker_out += csv_quoted(label.data(), label.size());
    marker_out += '\n';
}

/* "cond  <key>,<on|off>,<flags>,<value>,<term>,<host_us>,<t_us>", and
 * a marker named for the condition at a firing of one with mark */
void ecuio_recorder::take_event(const event_body& e)
{
    const ecuio_condition_record* set = conditions.get();
    if (set == nullptr || e.condition >= set->conditions.size()) {
        return;
    }
    const ecuio_condition_record::entry& c = set->conditions[e.condition];
    ecuio_recording_rows::cond_line line;
    const size_t key_len =
        c.key.size() < sizeof line.key ? c.key.size() : sizeof line.key - 1;
    std::memcpy(line.key, c.key.data(), key_len);
    line.on = e.state == ECUIO_CONDITION_ON;
    line.flags = e.flags;
    line.value = e.value;
    line.term = e.term;
    line.host_us = rel(e.host_us);
    line.t_us = e.t_us;
    cond_text.clear();
    ecuio_recording_rows::append_cond_text(cond_text, line);
    event_line(e.host_us, "cond", cond_text);
    if ((e.flags & ECUIO_CONDITION_FIRED) != 0 && c.mark) {
        add_marker(e.host_us, c.name);
    }
}

void ecuio_recorder::lost_line(uint64_t host_us, uint64_t lost)
{
    std::string text;
    ecuio_recording_rows::append_lost_text(text, lost);
    event_line(host_us, "drop", text);
}

void ecuio_recorder::take_conditions(const uint8_t* body, size_t size)
{
    if (size < sizeof(conditions_body)) {
        return;
    }
    conditions_body cb;
    std::memcpy(&cb, body, sizeof cb);
    size_t at = sizeof cb;
    for (uint32_t k = 0; k < cb.events && at + sizeof(event_body) <= size;
         ++k) {
        event_body e;
        std::memcpy(&e, body + at, sizeof e);
        at += sizeof e;
        take_event(e);
    }
    if (cb.lost > 0) {
        lost_line(cb.lost_us, cb.lost);
    }
}

void ecuio_recorder::take_link(const uint8_t* body, size_t size)
{
    if (size < sizeof(link_body)) {
        return;
    }
    link_body lb;
    std::memcpy(&lb, body, sizeof lb);
    event_line(lb.host_us, "link", link_text(lb.state, lb.episodes));
}

/* A line for each module whose state moved since the last written, in
 * channel order */
void ecuio_recorder::take_modules(const uint8_t* body, size_t size)
{
    if (size < sizeof(modules_body)) {
        return;
    }
    modules_body mb;
    std::memcpy(&mb, body, sizeof mb);
    const uint32_t moved =
        (mb.closed ^ written_closed) | (mb.no_room ^ written_no_room);
    for (uint32_t c = 0; c < channels && c < ECUIO_MAX_CHANNELS; ++c) {
        const uint32_t bit = 1u << c;
        if ((moved & bit) != 0) {
            event_line(mb.host_us, "module",
                       module_text(rx_ids[c], (mb.closed & bit) != 0,
                                   (mb.no_room & bit) != 0, mb.reasons[c]));
        }
    }
    written_closed = mb.closed;
    written_no_room = mb.no_room;
}

void ecuio_recorder::take_operation(const uint8_t* body, size_t size)
{
    if (size < sizeof(operation_body)) {
        return;
    }
    operation_body ob;
    std::memcpy(&ob, body, sizeof ob);
    ob.id[sizeof ob.id - 1] = '\0';
    if (ob.envelope != 0) {
        event_line(
            ob.host_us, "envelope",
            envelope_text(ob.rx, ob.volts_mv, ob.rpm, ob.kmh, ob.checks));
        return;
    }
    event_line(ob.host_us, "operation",
               operation_text(ob.id, ob.rx, ob.bytes, ob.end));
}

void ecuio_recorder::take_carried()
{
    if (link_pending) {
        event_line(pending_link.host_us, "link",
                   link_text(pending_link.state, pending_link.episodes));
    }
    if (modules_pending) {
        take_modules(reinterpret_cast<const uint8_t*>(&pending_modules),
                     sizeof pending_modules);
    }
    for (uint32_t i = 0; i < operations_pending; ++i) {
        take_operation(reinterpret_cast<const uint8_t*>(&pending_operations[i]),
                       sizeof pending_operations[i]);
    }
}

/* latency.csv's rows whose counts changed since their last row
 * (ADR-0017's appendix of 2026-10-31), the file made with the first */
void ecuio_recorder::take_timing(const uint8_t* body, size_t size)
{
    if (size < sizeof(timing_body)) {
        return;
    }
    timing_body tb;
    std::memcpy(&tb, body, sizeof tb);
    /* t_s on the recording's clock, as every other file's time: the
     * snapshot's session time is the session's, older than the
     * recording when the recorder attached after start */
    const uint64_t ms = (rel(tb.host_us) + 500) / 1000;
    char at[32];
    std::snprintf(at, sizeof at, "%llu.%03llu",
                  static_cast<unsigned long long>(ms / 1000),
                  static_cast<unsigned long long>(ms % 1000));
    static const char kHex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < tb.rows && i < ECUIO_PROFILE_MAX_REQUESTS;
         ++i) {
        const size_t off = sizeof tb + i * sizeof(ecuio_request_timing_t);
        if (off + sizeof(ecuio_request_timing_t) > size) {
            break;
        }
        ecuio_request_timing_t r;
        std::memcpy(&r, body + off, sizeof r);
        const uint64_t total =
            uint64_t{r.answered} + r.refused + r.timeouts;
        if (total == timing_totals[i]) {
            continue;
        }
        timing_totals[i] = total;
        const bool latency_has = r.answered + r.refused > 0;
        const bool interval_has = r.intervals > 0;
        latency_out += at;
        latency_out += ',';
        append_hex(latency_out,
                   r.channel < channels ? rx_ids[r.channel] : 0);
        latency_out += ',';
        for (uint8_t k = 0; k < r.length && k < 3; ++k) {
            if (k > 0) {
                latency_out += ' ';
            }
            latency_out += kHex[r.request[k] >> 4];
            latency_out += kHex[r.request[k] & 0x0F];
        }
        for (uint32_t n : {r.answered, r.refused, r.timeouts, r.packed}) {
            latency_out += ',';
            append_u64(latency_out, n);
        }
        latency_out += ',';
        append_quantile(latency_out, r.latency_p50_us, latency_has);
        latency_out += ',';
        append_quantile(latency_out, r.latency_p99_us,
                        (r.flags & ECUIO_TIMING_LATENCY_P99) != 0);
        latency_out += ',';
        append_quantile(latency_out, r.latency_p999_us,
                        (r.flags & ECUIO_TIMING_LATENCY_P999) != 0);
        latency_out += ',';
        append_quantile(latency_out, r.latency_max_us, latency_has);
        latency_out += ',';
        append_u64(latency_out, r.intervals);
        latency_out += ',';
        append_u64(latency_out, r.intervals_mixed);
        latency_out += ',';
        append_quantile(latency_out, r.interval_p50_us, interval_has);
        latency_out += ',';
        append_quantile(latency_out, r.interval_p99_us,
                        (r.flags & ECUIO_TIMING_INTERVAL_P99) != 0);
        latency_out += ',';
        append_quantile(latency_out, r.interval_p999_us,
                        (r.flags & ECUIO_TIMING_INTERVAL_P999) != 0);
        latency_out += ',';
        append_quantile(latency_out, r.interval_max_us, interval_has);
        latency_out += ',';
        latency_out += clock_name(tb.timing.interval_clock);
        latency_out += '\n';
    }
    if (!latency_out.empty() && latency == nullptr && !failed()) {
        latency = open_file(folder + "/latency.csv");
        if (latency == nullptr) {
            fail(errno != 0 ? errno : EIO);
            return;
        }
        latency_out.insert(0, ECUIO_TIMING_COLUMNS "\n");
    }
}

void ecuio_recorder::take_frame(const uint8_t* body, size_t size)
{
    if (size < sizeof(frame_body)) {
        return;
    }
    frame_body fb;
    std::memcpy(&fb, body, sizeof fb);
    size_t at = sizeof fb;
    if (at + fb.length > size) {
        return;
    }
    const uint64_t host_us = rel(fb.host_us);
    append_u64(frame_out, host_us);
    frame_out += ',';
    append_u64(frame_out, fb.timestamp_us);
    frame_out += ',';
    append_hex(frame_out, fb.id);
    frame_out += ',';
    append_hex(frame_out, fb.flags);
    frame_out += ',';
    const size_t kept = (fb.flags & ECUIO_FRAME_FLAG_ACCESS_CUT) != 0
                            ? ecuio_capture_kept(body + at, fb.length)
                            : 0;
    append_bytes(frame_out, body + at, fb.length, kept);
    frame_out += '\n';
    if (kept != 0) {
        event_line(fb.host_us, "cut",
                   cut_text(fb.id, body + at, fb.length, kept));
    }
    at += fb.length;
    frame_rows.fetch_add(1, std::memory_order_relaxed);

    for (uint16_t v = 0; v < fb.values; ++v) {
        if (at + sizeof(value_head) > size) {
            return;
        }
        value_head vh;
        std::memcpy(&vh, body + at, sizeof vh);
        at += sizeof vh;
        const def_entry* d = def(vh.signal_id);
        const bool text = is_text_kind(vh.kind);
        const size_t len = text ? vh.text_len : sizeof(double);
        if (at + len > size) {
            return;
        }
        if (d != nullptr) {
            append_u64(value_out, host_us);
            value_out += ',';
            value_out += d->key;
            value_out += ',';
            if (text) {
                value_out += csv_quoted(
                    reinterpret_cast<const char*>(body + at), len);
            } else {
                double x;
                std::memcpy(&x, body + at, sizeof x);
                ecuio_recording_rows::append_value(value_out, x);
            }
            value_out += ',';
            value_out += d->unit;
            value_out += '\n';
            value_rows.fetch_add(1, std::memory_order_relaxed);
        }
        at += len;
    }
    for (uint32_t k = 0; k < fb.events; ++k) {
        if (at + sizeof(event_body) > size) {
            return;
        }
        event_body e;
        std::memcpy(&e, body + at, sizeof e);
        at += sizeof e;
        take_event(e);
    }
}

/* Everything the worker has queued; true when there was any */
bool ecuio_recorder::drain_ring()
{
    bool any = false;
    uint64_t t = tail.load(std::memory_order_relaxed);
    const uint64_t h = head.load(std::memory_order_acquire);
    while (h - t >= sizeof(record_head)) {
        record_head rh;
        copy_out(t, &rh, sizeof rh);
        if (rh.size < sizeof rh || rh.size > h - t) {
            break;
        }
        const size_t body = rh.size - sizeof rh;
        if (scratch.size() < body) {
            scratch.resize(body);
        }
        copy_out(t + sizeof rh, scratch.data(), body);
        if (rh.dropped_before > 0 && body >= sizeof(uint64_t)) {
            uint64_t host_us;
            std::memcpy(&host_us, scratch.data(), sizeof host_us);
            drop_line(host_us, rh.dropped_before);
        }
        switch (rh.type) {
        case kRecordFrame:
            take_frame(scratch.data(), body);
            break;
        case kRecordLink:
            take_link(scratch.data(), body);
            break;
        case kRecordTiming:
            take_timing(scratch.data(), body);
            break;
        case kRecordConditions:
            take_conditions(scratch.data(), body);
            break;
        case kRecordModules:
            take_modules(scratch.data(), body);
            break;
        case kRecordOperation:
            take_operation(scratch.data(), body);
            break;
        default:
            break;
        }
        t += rh.size;
        any = true;
    }
    tail.store(t, std::memory_order_release);
    write(frames, frame_out);
    write(signals, value_out);
    write(events, event_out);
    write(markers, marker_out);
    write(latency, latency_out);
    return any;
}

bool ecuio_recorder::drain_items()
{
    std::deque<item> taken;
    {
        std::lock_guard<std::mutex> g(note_m);
        taken.swap(items);
    }
    for (const item& it : taken) {
        switch (it.kind) {
        case item_kind::begin:
            /* The file before the header line naming its hash */
            if (!it.bytes.empty() && !failed()) {
                errno = 0;
                if (fail_next.exchange(false)) {
                    fail(ENOSPC);
                } else if (!write_bytes(
                               folder + "/" +
                                   ecuio_recording_rows::kSignalFileName,
                               it.bytes)) {
                    fail(errno != 0 ? errno : EIO);
                }
            }
            if (conditions != nullptr && !failed()) {
                errno = 0;
                if (!write_bytes(folder + "/" +
                                     ecuio_recording_rows::kConditionsName,
                                 conditions->bytes)) {
                    fail(errno != 0 ? errno : EIO);
                }
            }
            event_out += it.text;
            begin_rows();
            break;
        default:
            write_item(it);
            break;
        }
    }
    write(frames, frame_out);
    write(signals, value_out);
    write(events, event_out);
    write(markers, marker_out);
    return !taken.empty();
}

/* frames.csv's and signals.csv's header rows */
void ecuio_recorder::begin_rows()
{
    frame_out += "host_us,timestamp_us,id,flags,data\n";
    /* A byte-order mark, so a spreadsheet reads the units' UTF-8 as
     * UTF-8 (ADR-0017) */
    value_out += "\xEF\xBB\xBFhost_us,signal,value,unit\n";
}

/* A note, a mark, a focus's lines or an asleep line */
void ecuio_recorder::write_item(const item& it)
{
    switch (it.kind) {
    case item_kind::focus: {
        size_t at = 0;
        while (at <= it.bytes.size()) {
            const size_t end =
                (std::min)(it.bytes.find('\0', at), it.bytes.size());
            event_line(it.host_us, it.text.c_str(),
                       it.bytes.substr(at, end - at));
            at = end + 1;
        }
        break;
    }
    case item_kind::note:
        event_line(it.host_us, "note", it.text);
        break;
    case item_kind::asleep:
        event_line(it.host_us, "asleep", it.text);
        break;
    case item_kind::mark:
        add_marker(it.host_us, it.text);
        break;
    case item_kind::begin:
        break;
    }
}

void ecuio_recorder::write_outs()
{
    write(frames, frame_out);
    write(signals, value_out);
    write(events, event_out);
    write(markers, marker_out);
    write(latency, latency_out);
}

void ecuio_recorder::flush_all()
{
    for (FILE* f : {frames, signals, events, markers, latency}) {
        if (f != nullptr && !failed() && std::fflush(f) != 0) {
            fail(errno != 0 ? errno : EIO);
        }
    }
}

void ecuio_recorder::sync_all()
{
    for (FILE* f : {frames, signals, events, markers, latency}) {
        if (f != nullptr && !failed() && !sync_file(f)) {
            fail(errno != 0 ? errno : EIO);
        }
    }
    syncs.fetch_add(1, std::memory_order_relaxed);
}

/* An asleep line when the machine's slept count rose a second or more
 * since the last wake (ADR-0049 section 2): stamped as the writer
 * woke, its N s the N s before. Smaller rises are taken in without a
 * line, as the app's tick takes them. */
void ecuio_recorder::check_sleep()
{
    if (!attached.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t now = ecuio_clock_slept_us() +
                         slept_added.load(std::memory_order_relaxed);
    if (!sleep_armed) {
        sleep_armed = true;
        slept_seen = slept_at_attach;
    }
    const uint64_t rise = now > slept_seen ? now - slept_seen : 0;
    slept_seen = (std::max)(now, slept_seen);
    if (rise < kSleepLineUs) {
        return;
    }
    const uint64_t tenths = (rise + 50000) / 100000;
    const uint64_t host_us = host_us_of(ecuio_record_clock_us());
    std::string text = "Asleep for " + std::to_string(tenths / 10) + "." +
                       std::to_string(tenths % 10) +
                       " s: nothing was read meanwhile";
    if (armed) {
        pending.push_back(
            item{item_kind::asleep, host_us, std::move(text), {}, {}});
    } else {
        event_line(host_us, "asleep", text);
    }
}

uint64_t ecuio_recorder::host_us_of(uint64_t clock_us) const
{
    return clock_us > zero_us ? clock_us - zero_us : 0;
}

void ecuio_recorder::run()
{
    auto last_flush = std::chrono::steady_clock::now();
    auto last_sync = last_flush;
    bool busy = false;
    for (;;) {
        bool stopping;
        {
            std::unique_lock<std::mutex> lk(wake_m);
            wake_cv.wait_for(lk, busy ? kBusyWake : kIdleWake,
                             [this] { return stop; });
            stopping = stop;
        }
        wakes.fetch_add(1, std::memory_order_relaxed);
        if (hold.load(std::memory_order_acquire) && !stopping) {
            continue;
        }
        /* Nothing until a session attaches: it completes the header
         * item open queued; one closed unattached leaves its files empty */
        if (!attached.load(std::memory_order_acquire)) {
            if (stopping) {
                return;
            }
            continue;
        }
        /* Read before the pass: a detach after it can still put */
        const bool gone = detached.load(std::memory_order_acquire);
        if (armed) {
            /* Its session gone, no record follows: nothing held back */
            const bool final = stopping || gone;
            take_items();
            check_sleep();
            busy = armed_pass(final);
        } else {
            /* The items first: the header comes ahead of any row */
            const bool notes = drain_items();
            check_sleep();
            const bool frames_taken = drain_ring();
            busy = notes || frames_taken;
        }
        if (gone && tail.load(std::memory_order_relaxed) ==
                        head.load(std::memory_order_acquire)) {
            drained.store(true, std::memory_order_release);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= kFlushEvery) {
            flush_all();
            last_flush = now;
        }
        if (now - last_sync >= kSyncEvery) {
            sync_all();
            last_sync = now;
        }
        if (stopping) {
            return;
        }
    }
}

void ecuio_recorder::close_files()
{
    for (FILE** f : {&frames, &signals, &events, &markers, &latency}) {
        if (*f != nullptr) {
            if (std::fclose(*f) != 0 && !failed()) {
                fail(errno != 0 ? errno : EIO);
            }
            *f = nullptr;
        }
    }
}

/* Files still open only when open failed part way, which reported
 * its own failure: closed, nothing more recorded */
ecuio_recorder::~ecuio_recorder()
{
    for (FILE* f : {frames, signals, events, markers, latency}) {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
}
