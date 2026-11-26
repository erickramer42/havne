// File/layer: core/src/j2534_worker.cpp | C++17, Qt-free, Windows only
// Testing: HARNESS: test_j2534_stream, test_j2534_health; Cross-arch
//          Reply timing on the adapter's clock (ADR-0042):
//          J2534HealthTest.VxTimingReportsEachRequestOnTheAdaptersClock.
//          A recording's link lines (ADR-0049) come through link_health.h,
//          tested there (RecorderTest.ALinkTransition*) and through the
//          fake ELM327; this worker's argument by code reading.
//          ECUIO_FRAME_FLAG_REASSEMBLED on every message:
//          J2534StreamTest.DirectLoadUriDeliversInjectedFramesExactly,
//          J2534HealthTest.LongRepliesArriveWhole
//          The floor and the timer's wait: J2534HealthTest.TheFloor*,
//          .VxTheFloorsWaitReadsNothing
//          The hold (Known Issue #42): UNIT: J2534HoldTest; on the fake,
//          tests/test_j2534_hold_bus.cpp (J2534HealthTest)
//          The filters following the modules: J2534ModulesTest
//          Read before a wait ends: J2534HealthTest.AReplyInTheAdapter
//          WhenItsWaitRunsOutAnswersIt
//
// The J2534 session's worker: the read pump, a request's write, the
// link-health feed and the close's 10 01. j2534_transport.cpp starts it
// (attach, start, detach); j2534_transport.h is what they share.

#ifdef _WIN32

#include "j2534_transport.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534.h"
#include "ecuio/j2534_ptypes.h"    /* FLOW_CONTROL_FILTER, CAN_29BIT_ID,
                                      TX_ISO15765_FRAME_PAD */
#include "session.h"
#include "request_engine.h"
#include "j2534_hold.h"
#include "request_setup.h"
#include "request_conversation.h"
#include "link_health.h"
#include "j2534_clock.h"
#include "host_clock.h"
#include "error_detail.h"
#include "isotp.h"
#include "ecuio/profile.h"
#include "precise_wait.h"          /* the wait after an empty read */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace ecuio_j2534;

namespace {

/* A read's outcome. "matched" (stripped and dispatched to one of our
 * channels) is kept apart from "delivered but unmatched" (the adapter
 * is reading the bus, but the frame isn't from an ECU we talk to),
 * because only a matched frame proves the link works end to end. A
 * matched frame also carries its channel and the start of its payload,
 * for the engine to correlate. */
struct read_outcome {
    ecuio_status_t status = ECUIO_OK;
    bool got = false;
    bool matched = false;
    uint32_t chan = 0;      /* the channel it matched */
    uint16_t len = 0;       /* its payload's length, the CAN ID stripped */
    const uint8_t* payload = nullptr;   /* the payload, in the transport's
                                         * rx_frame until the next read */
};

/* A received message as the frame the session delivers: skip bytes
 * dropped from the front (the CAN ID, once it matched a channel), the
 * rest cut at ECUIO_FRAME_DATA_MAX. EXTENDED comes from RxStatus, and
 * TRUNCATED says the frame holds less than the message. Every channel
 * is ISO 15765, so the adapter reassembled each message: REASSEMBLED. */
void frame_from_msg(const ecuio_pt_rx_t& m, uint32_t skip,
                    uint32_t arbitration_id, uint64_t timestamp_us,
                    ecuio_frame_t* f)
{
    *f = ecuio_frame_t{};
    f->timestamp_us = timestamp_us;
    f->arbitration_id = arbitration_id;
    const uint32_t have = m.data_size - skip;
    const uint32_t n = (have < ECUIO_FRAME_DATA_MAX) ? have
                                                     : ECUIO_FRAME_DATA_MAX;
    memcpy(f->data, m.data + skip, n);
    f->data_length = static_cast<uint16_t>(n);
    f->flags =
        ((m.rx_status & ECUIO_PT_CAN_29BIT_ID) ? ECUIO_FRAME_FLAG_EXTENDED
                                               : 0u) |
        ECUIO_FRAME_FLAG_REASSEMBLED;
    if (n < have) {
        f->flags |= ECUIO_FRAME_FLAG_TRUNCATED;
    }
}

/* ECUIO_FRAME_FLAG_OVERFLOW on the frame about to be dispatched, when a
 * message since the last one carried BUFFER_OVERFLOW (J2534-1 v05.00's
 * RxStatus bit; the surrogate child sets it for a v04.04 vendor's
 * ERR_BUFFER_OVERFLOW). */
void mark_overflow(ecuio_j2534_transport* t, ecuio_frame_t* f)
{
    if (t->overflow_pending) {
        f->flags |= ECUIO_FRAME_FLAG_OVERFLOW;
        t->overflow_pending = false;
        ecuio_engine_overflowed(&t->engine);
    }
}

/* Read one message and dispatch it as a frame if it matches a channel.
 * Returns the outcome: status, whether a message arrived, and whether
 * it matched. An unmatched delivery is kept distinct from silence (see
 * read_outcome). Every delivered message, dispatched or dropped,
 * advances the vendor clock, so the next delta is measured from the
 * latest stamp. */
read_outcome pump_one(ecuio_j2534_transport* t, uint32_t timeout_ms)
{
    read_outcome o;
    bool got = false;
    ecuio_pt_rx_t msg;
    o.status = ecuio_j2534_read_msg(t->handle, t->channel, &msg,
                                    timeout_ms, &got);
    if (o.status != ECUIO_OK || !got) {
        return o;   /* no match if no frame */
    }
    o.got = true;
    /* Both backends deliver the vendor's 32-bit stamp as it came. */
    const uint64_t host_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            host_clock::now() - t->epoch).count());
    const uint64_t at_us = j2534_clock_step(t->clock, msg.timestamp,
                                            host_us);
    ecuio_frame_t& frame = t->rx_frame;
    /* Frames were lost before this message: the next frame dispatched
     * says so, this one or a later one if this isn't ours */
    t->overflow_pending =
        t->overflow_pending || (msg.rx_status & ECUIO_PT_BUFFER_OVERFLOW) != 0;

    if (t->chan_count == 0) {
        /* no channels: verbatim, id 0 (raw passive) counts as matched
         * since we explicitly requested verbatim delivery. */
        frame_from_msg(msg, 0, 0, at_us, &frame);
        mark_overflow(t, &frame);
        ecuio_session_dispatch_callbacks(t->session, &frame);
        o.matched = true;
        return o;
    }

    if (msg.data_size >= J2534_ADDR_LEN) {
        const uint32_t hdr =
            (static_cast<uint32_t>(msg.data[0]) << 24) |
            (static_cast<uint32_t>(msg.data[1]) << 16) |
            (static_cast<uint32_t>(msg.data[2]) << 8) |
             static_cast<uint32_t>(msg.data[3]);
        for (uint32_t i = 0; i < t->chan_count; ++i) {
            if ((hdr & ECUIO_CAN_ID_MAX) == t->chans[i].rx_id) {
                frame_from_msg(msg, J2534_ADDR_LEN, t->chans[i].rx_id, at_us,
                               &frame);
                mark_overflow(t, &frame);
                o.matched = true;
                o.chan = i;
                o.len = frame.data_length;
                o.payload = frame.data;
                ecuio_session_dispatch_callbacks(t->session, &frame);
                return o;
            }
        }
    }
    /* no match: a frame for another ID got past the adapter's filter.
     * Dropped, but it still proves the adapter is reading */
    return o;
}

/* A read's outcome in link health's terms (link_health.h). pump_one
 * guarantees got implies ECUIO_OK, so any status other than ECUIO_OK
 * and the vehicle-timeout translation is hard. Everything else the
 * adapter answered is clean: an empty read, ERR_TIMEOUT (the same
 * thing: it had nothing; the VX answers every empty read
 * ERR_BUFFER_EMPTY, which the read paths map to an empty ECUIO_OK), or
 * a frame for another ID. */
bool hard_read(const read_outcome& o)
{
    return o.status != ECUIO_OK && o.status != ECUIO_ERR_VEHICLE_TIMEOUT;
}

void update_health(link_health& h, const read_outcome& o, bool reads_count)
{
    link_on_read(h, hard_read(o), o.got && o.matched, reads_count);
}

/* Writes a request to its channel's tx ID: [4-byte tx ID][service][key],
 * sent with ISO15765_FRAME_PAD, so a Service 01 request is 6 bytes and a
 * Service 0x22 one 7. The adapter adds the ISO-TP PCI byte and pads the
 * CAN frame; ISO 15765-4 has no data checksum. Verified: Opus IVS (the
 * RPM request is [ID][01][0C], DataSize 6), Quantex PassThruWriteMsgs
 * docs, and the first vehicle
 * (docs/bench-sessions/2026-10-22-vehicle.md). Then the write side of
 * link health. The vendor's written count is judged with the status:
 * STATUS_NOERROR with nothing written is a vendor reporting success for
 * a message it never sent, so it's a failed write (internal.h's
 * contract for ecuio_j2534_write_frame). */
bool send_request(ecuio_j2534_transport* t, const ecuio_request& r,
                  link_health& h, bool once = false)
{
    const j2534_channel& ch = t->chans[r.channel];
    uint32_t len = 0; /* a talk's whole request: the adapter segments it */
    const uint8_t* pdu = ecuio_engine_pdu_of(t->engine.chans[r.channel].talk,
                                             once, r.bytes, r.len, &len);
    uint8_t msg[J2534_ADDR_LEN + ECUIO_ENGINE_PDU_MAX];
    pack_be32(msg, ch.tx_id);
    memcpy(msg + J2534_ADDR_LEN, pdu, len);
    uint32_t written = 0;
    const ecuio_status_t wst = ecuio_j2534_write_frame(
        t->handle, t->channel, msg, J2534_ADDR_LEN + len,
        ch.addr_flags | /* a 29-bit channel's CAN_29BIT_ID */
            ECUIO_PT_TX_ISO15765_FRAME_PAD,
        0, &written);
    const bool sent = (wst == ECUIO_OK && written >= 1);
    link_on_write(h, sent);
    return sent;
}

/* Backoff-aware sleep, abortable within 20 ms of a stop request:
 * a 5 s capped backoff must not turn into a 5 s join at detach.
 * Returns true if stop was requested (caller should exit promptly). */
bool backoff_sleep(ecuio_j2534_transport* t, uint32_t ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(ms);
    while (!t->stop_requested.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        auto chunk = deadline - now;
        if (chunk > std::chrono::milliseconds(20)) {
            chunk = std::chrono::milliseconds(20);
        }
        std::this_thread::sleep_for(chunk);
    }
    return true;
}

/* The engine's clock: microseconds since attach. */
int64_t engine_now_us(const ecuio_j2534_transport* t)
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            host_clock::now() - t->epoch).count());
}

/* A read's timeout from the engine's next event (INT64_MAX: nothing to
 * ask). With something to ask, until that event, within 1 ms and the
 * slice; never 0, which asks a vendor for a read that doesn't wait at
 * all. With nothing, passive mode's. */
uint32_t read_timeout_ms(int64_t next_us)
{
    if (next_us == INT64_MAX) {
        return J2534_IDLE_READ_TIMEOUT_MS;
    }
    const int64_t slice_us = int64_t{J2534_READ_SLICE_MS} * 1000;
    int64_t us = next_us;
    if (us > slice_us) {
        us = slice_us;
    }
    const int64_t ms = (us + 999) / 1000;
    return (ms < 1) ? 1u : static_cast<uint32_t>(ms);
}

/* The close's waits, each the engine's reply wait (P2 plus the adapter's
 * margin): a request in flight, then 10 01's answer, whatever a 78 says. */
constexpr int64_t J2534_LEAVE_WAIT_US = ECUIO_ENGINE_WAIT_US;
constexpr int64_t J2534_CLOSE_US = 2 * J2534_LEAVE_WAIT_US;

/* A read at the close: true when the link failed hard, which ends it. */
bool close_read(ecuio_j2534_transport* t, int64_t left_us, read_outcome* o)
{
    const int64_t ms = (left_us + 999) / 1000;
    const uint32_t timeout_ms = (ms < 1) ? 1u
                                : (ms > J2534_READ_SLICE_MS)
                                    ? J2534_READ_SLICE_MS
                                    : static_cast<uint32_t>(ms);
    const auto start = std::chrono::steady_clock::now();
    *o = pump_one(t, timeout_ms);
    if (o->status != ECUIO_OK && o->status != ECUIO_ERR_VEHICLE_TIMEOUT) {
        return true;
    }
    if (!o->got) {
        /* a vendor that returns at once mustn't make this a spin */
        std::this_thread::sleep_until(start +
                                      std::chrono::milliseconds(timeout_ms));
    }
    return false;
}

/* Whether payload ends a 10 01: 50 01, or 7F 10 with any code but 78
 * (still working). */
bool ends_leave(const uint8_t* p, uint16_t len)
{
    if (len >= 2 && p[0] == ECUIO_SESSION_CONTROL + 0x40 &&
        p[1] == ECUIO_SESSION_DEFAULT) {
        return true;
    }
    return len >= 3 && p[0] == ECUIO_NEGATIVE_RESPONSE &&
           p[1] == ECUIO_SESSION_CONTROL && p[2] != ECUIO_NRC_RESPONSE_PENDING;
}

/* ADR-0060's close: 10 01 to each channel in the extended session, its
 * answer read before the filter stops (docs/bench-sessions/2026-11-11-ext-session-car.md). */
void leave_sessions(ecuio_j2534_transport* t, link_health& h)
{
    ecuio_request_engine& e = t->engine;
    ecuio_request q[ECUIO_MAX_CHANNELS];
    bool leave[ECUIO_MAX_CHANNELS] = {};
    bool any = false;
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        leave[c] = ecuio_engine_session_leave(&e, c, &q[c]);
        any = any || leave[c];
    }
    if (!any) {
        return;
    }
    const int64_t start = engine_now_us(t);
    const int64_t close_by = start + J2534_CLOSE_US;
    read_outcome o;
    for (;;) {
        const int64_t now = engine_now_us(t);
        const int64_t until = ecuio_engine_leave_wait_until(&e, leave, start);
        if (until <= now) {
            break;
        }
        if (close_read(t, until - now, &o)) {
            return;
        }
        if (o.matched) {
            (void)ecuio_engine_on_frame(&e, o.chan, o.payload, o.len,
                                        engine_now_us(t), nullptr);
        }
    }
    bool awaiting[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        awaiting[c] = leave[c] && send_request(t, q[c], h);
    }
    int64_t until = engine_now_us(t) + J2534_LEAVE_WAIT_US;
    until = (until < close_by) ? until : close_by;
    for (;;) {
        bool waiting = false;
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            waiting = waiting || awaiting[c];
        }
        const int64_t now = engine_now_us(t);
        if (!waiting || now >= until || close_read(t, until - now, &o)) {
            return;
        }
        if (o.matched && ends_leave(o.payload, o.len)) {
            awaiting[o.chan] = false;
        }
    }
}

/* The pass's sends (j2534_hold_pass): each wait that ran out, and what
 * the hold gives each channel to write (due's request, unless a segmented
 * reply keeps it or another waiting) */
void ask_channels(ecuio_j2534_transport* t, ecuio_request_engine& e,
                  link_health& h, j2534_hold& hold)
{
    j2534_hold_pass(
        hold, &e, engine_now_us(t),
        [&](uint32_t) { ecuio_request_timed_out(e, h); },
        [&](uint32_t c, int32_t r) {
            const int64_t start = engine_now_us(t);
            const bool sent = send_request(t, ecuio_engine_request_of(&e, c, r),
                                           h, r == ECUIO_ENGINE_ONCE);
            ecuio_engine_sent(&e, c, r, sent, engine_now_us(t), start);
        });
}

/* The worker's locals, in the order the loop declared them */
struct worker_state {
    ecuio_request_engine& e;
    const bool requests;
    /* A requesting worker's waits end on time, not on Windows' 15.6 ms
     * step (precise_wait.h): the floor between requests is the engine's */
    ecuio_precise_wait precise;
    link_health h;
    ecuio_unpicked_check unpicked_check;
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_diagnostics[ECUIO_MAX_CHANNELS] = {};
    uint32_t diag_seen = 0;
    uint32_t clear_seen = 0;     /* ecuio_take_clear_requests' count */
    ecuio_focus_seen focus_seen; /* what ecuio_take_focus took */
    ecuio_modules_seen modules_seen; /* what ecuio_take_modules took */
    int64_t timing_at = 0;
    /* how far the last read covered (ecuio_engine_wait_unread): an empty
     * read that waited out its timeout to its end, any other its start */
    int64_t read_us = 0;
    /* made after attach's ecuio_engine_init: its chosen rows are this
     * table's, and it ends with the worker */
    j2534_hold hold;

    explicit worker_state(ecuio_j2534_transport* t)
        : e(t->engine), requests(j2534_asks(t)), precise(requests),
          h(t->session->last_error, t->session->error_count,
            t->session->record_link.get(), t->session, t->epoch,
            &t->clock.last_us, &t->clock.last_host_us)
    {
        hold.on = t->hold;
    }
};

/* What read_pass leaves for the steps after it */
struct read_step {
    read_outcome o;
    bool timer_only = false;
    uint32_t timeout_ms = 0;
    std::chrono::steady_clock::time_point start;
    bool stopped = false;
};

void begin_worker(ecuio_j2534_transport* t, worker_state& w)
{
    ecuio_request_engine& e = w.e;
    const bool requests = w.requests;
    if (requests && t->probe) {
        ecuio_engine_probe(&e);
    }
    if (requests) {
        /* A focus set before start applies from the first request */
        ecuio_take_focus(t->session, &e, w.focus_seen);
        (void)ecuio_take_modules(t->session, &e, w.modules_seen,
                                 engine_now_us(t));
        (void)j2534_filters_follow(t, false);
        ecuio_conversations_start(t->session, &e);
    }
}

/* A read's matched frame, to the engine, the conversations and the hold */
void take_reply(ecuio_j2534_transport* t, worker_state& w,
                const read_outcome& o)
{
    if (!w.requests || !o.matched) {
        return;
    }
    ecuio_request_engine& e = w.e;
    /* the refresh interval on the adapter's clock (ADR-0042) */
    e.stamp_set = true;
    e.stamp_us = static_cast<int64_t>(t->rx_frame.timestamp_us);
    e.stamp_breaks = t->clock.host_steps;
    (void)ecuio_engine_on_frame(&e, o.chan, o.payload, o.len, engine_now_us(t),
                                nullptr);
    ecuio_conversations_frame(t->session, &e, o.chan, o.payload, o.len);
    j2534_hold_answered(w.hold, &e, o.chan);
}

/* Read first, then expire: a reply that reached the adapter after the
 * last read and by its deadline answers its request, though the worker
 * comes to the wait late (a host held up, a vendor whose empty read
 * returns at once). One read for each channel at most, until one is
 * empty; a read of 1 ms, since 0 asks for none that waits (the VX holds
 * it 50 ms) */
void read_before_expiry(ecuio_j2534_transport* t, worker_state& w)
{
    if (!ecuio_engine_wait_unread(&w.e, w.read_us, engine_now_us(t))) {
        return;
    }
    for (uint32_t k = 0; k < t->chan_count; ++k) {
        const read_outcome o = pump_one(t, 1);
        take_reply(t, w, o);
        update_health(w.h, o, false);
        if (!o.got) {
            break;
        }
    }
    w.read_us = engine_now_us(t);
}

void pass_start(ecuio_j2534_transport* t, worker_state& w)
{
    link_judge_waiting(w.h);
    if (w.requests) {
        ecuio_key_windows_judge(&w.e, w.h);
        read_before_expiry(t, w);
        ask_channels(t, w.e, w.h, w.hold);
    }
}

read_step read_pass(ecuio_j2534_transport* t, worker_state& w)
{
    ecuio_request_engine& e = w.e;
    const bool requests = w.requests;
    read_step rs;
    read_outcome& o = rs.o;
    /* With nothing due on any channel (a scan that found nothing
     * and polls nothing, a probe that dropped every request) no
     * request can time out, so the session reads as a passive one
     * does: empty reads count toward quiet, which can't tell an
     * ECU that went away from one with nothing left to answer. */
    int64_t next_us =
        requests ? j2534_hold_next_event_us(w.hold, &e, engine_now_us(t))
                 : INT64_MAX;
    if (requests) {
        next_us = (std::min)(next_us, ecuio_conversations_next_us(
                                          t->session, &e, engine_now_us(t)));
    }
    const bool asking = next_us != INT64_MAX;
    /* Every reply in and only a floor (or a focus's sweep) to wait
     * for: the worker's own timer, no vendor read, which the VX holds
     * about 10 ms past a short timeout
     * (docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md). Frames
     * that come meanwhile wait in the adapter for the next read. */
    rs.timer_only = requests && asking && next_us > 0 &&
                    w.h.backoff_ms == 0 && !ecuio_engine_awaiting(&e);
    rs.timeout_ms = read_timeout_ms(next_us);
    rs.start = std::chrono::steady_clock::now();
    if (rs.timer_only) {
        const int64_t slice_us = int64_t{J2534_READ_SLICE_MS} * 1000;
        if (w.precise.sleep_until(
                rs.start + std::chrono::microseconds(
                               next_us < slice_us ? next_us : slice_us),
                t->stop_requested)) {
            rs.stopped = true;
            return rs;
        }
    } else {
        const int64_t read_at = engine_now_us(t);
        o = pump_one(t, rs.timeout_ms);
        const int64_t read_end = engine_now_us(t);
        w.read_us = ecuio_engine_read_covers(
            read_at, read_end, int64_t{rs.timeout_ms} * 1000, o.got);
        take_reply(t, w, o);
        const bool unpicked = !asking && ecuio_engine_unpicked(&e);
        update_health(w.h, o, !asking && !unpicked);
        if (unpicked && !hard_read(o)) {
            ecuio_unpicked_pass(&e, w.h, w.unpicked_check, engine_now_us(t));
        }
    }
    return rs;
}

void after_read(ecuio_j2534_transport* t, worker_state& w)
{
    ecuio_request_engine& e = w.e;
    const bool requests = w.requests;
    if (w.h.recovered) {
        w.h.recovered = false;
        j2534_hold_recovered(w.hold);
        if (requests && t->probe) {
            ecuio_engine_probe(&e);
        }
        if (requests) {
            /* the scan's and identification's unanswered requests */
            ecuio_engine_rescan(&e);
        }
    }
}

void serve_session(ecuio_j2534_transport* t, worker_state& w)
{
    ecuio_request_engine& e = w.e;
    const bool requests = w.requests;
    if (requests) {
        ecuio_take_diag_request(t->session, &e, w.diag_seen);
        ecuio_take_clear_requests(t->session, &e, &w.clear_seen);
        ecuio_take_test_talk(t->session, &e, engine_now_us(t));
        ecuio_take_focus(t->session, &e, w.focus_seen);
        (void)ecuio_take_modules(t->session, &e, w.modules_seen,
                                 engine_now_us(t));
        (void)j2534_filters_follow(t, false);
        ecuio_step_conversations(t->session, &e, engine_now_us(t));
    }
    for (uint32_t c = 0; requests && c < t->chan_count; ++c) {
        ecuio_publish_support(t->session, e, c, t->chans[c].rx_id,
                              w.published[c]);
        ecuio_publish_identity(t->session, e, c, t->chans[c].rx_id,
                               w.published_identity[c]);
        ecuio_publish_diagnostics(t->session, e, c, t->chans[c].rx_id,
                                  w.published_diagnostics[c]);
    }
    ecuio_publish_timing(t->session, e, nullptr, engine_now_us(t),
                         w.timing_at, false);
}

/* True when the stop was seen while waiting */
bool pass_wait(ecuio_j2534_transport* t, worker_state& w, const read_step& rs)
{
    if (w.h.backoff_ms > 0) {
        if (backoff_sleep(t, w.h.backoff_ms)) {
            return true;
        }
        w.h.backoff_ms = link_backoff_next(w.h.backoff_ms);
    } else if (!rs.o.got && !rs.timer_only) {
        /* An empty read waits out its timeout, even from a vendor
         * that returns at once, so the loop never spins: on time
         * (precise_wait.h), since that timeout is the way to the
         * engine's next event. In waits of 20 ms at most with the
         * stop looked at between them: idle, the timeout is 100 ms,
         * and a close would otherwise wait it out. */
        if (w.precise.sleep_until(rs.start +
                                      std::chrono::milliseconds(rs.timeout_ms),
                                  t->stop_requested)) {
            return true;
        }
    }
    return false;
}

void end_worker(ecuio_j2534_transport* t, worker_state& w)
{
    if (w.requests) {
        ecuio_conversations_end(t->session, &w.e);
        leave_sessions(t, w.h);
    }
    ecuio_publish_timing(t->session, w.e, nullptr, engine_now_us(t),
                         w.timing_at, true);
}

} // namespace

/* Worker entrypoint: mirrors mock_worker's discipline: impl-owned
 * state, dispatch through the session snapshot path, stop via atom.
 *
 * One loop serves every mode (ADR-0015 section 1). With requests (the set's
 * rotation, or ?poll='s one), each pass ends the waits that ran out,
 * sends each idle channel's next request once its floor has passed (a
 * reply expected segmented alone on the bus, j2534_hold.h), then reads
 * once, for at most the slice, and gives a matched frame to
 * the engine, so the next request goes out when the reply is in. While
 * no reply may come (ecuio_engine_awaiting) it waits on its own timer
 * instead of reading.
 * Passive (no requests) only reads, and so does a session with nothing
 * left to ask. A read that brought a frame is followed by the next at
 * once, so every mode drains what has arrived; an empty one waits out
 * its timeout.
 *
 * With the probe on (section 4) the engine walks each channel's supported-PID
 * chain before its rotation, and the walk resumes, where it stopped,
 * whenever the link returns to OK: a connect with the ignition off
 * answers nothing, and the ECU's answers come once it wakes.
 *
 * Requests keep going out while the link is hard-failed or requests are
 * failing, because each is the recovery probe and replies only follow
 * writes; the backoff then spaces the passes. Distorting the cadence
 * while dead is the point: retrying a dead device at full rate is not. */
void ecuio_j2534::j2534_worker(ecuio_j2534_transport* t)
{
    worker_state w(t);
    begin_worker(t, w);
    while (!t->stop_requested.load()) {
        pass_start(t, w);
        const read_step rs = read_pass(t, w);
        if (rs.stopped) {
            break;
        }
        after_read(t, w);
        serve_session(t, w);
        if (pass_wait(t, w, rs)) {
            break;
        }
    }
    end_worker(t, w);
}

#endif /* _WIN32 */
