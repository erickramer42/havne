// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — test_j2534_connect: connect failures and the
//          session info, also cross-arch. test_j2534_stream: URI parsing,
//          the vendor clock's unit cases, connect parameters,
//          filter registration, request framing and routing, header
//          strip and arbitration_id stamp, raw on the engine ECU's
//          channel without ?filter=, rotation, ?poll= precedence,
//          the probe's walk, its drops, ?probe=0 and its resumption on
//          recovery, the support report it publishes, uds-generic's
//          requests, ECU prefix and DID scan, and identification's
//          requests, ?identify=0 and the report it publishes.
//          test_j2534_health, also cross-arch: the request engine's
//          pacing (the floor, the wait, negative and pending replies,
//          two channels at once), the drain in passive mode, and the
//          link-health machine (link_health.h, UNIT in CoreTest.Link*)
//          through this worker: hard and quiet reads, untouched counts,
//          the write latch, its priority and its step-down, a zero
//          written count, unmatched frames, backoff growth, its end at
//          a clean read and its restart, and quiet reads once nothing
//          is left to ask.
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "ecuio/j2534.h"
#include "request_engine.h"
#include "link_health.h"
#include "j2534_clock.h"
#include "uri_query.h"
#include "error_detail.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>

/* The query keys parse_query reads, for create's check (internal.h);
 * every platform's, so create links where the transport is a stub. */
extern "C" const char* const ecuio_j2534_query_keys[] = {
    "baud", "flags", "filter", "txid", "poll", "probe", "identify", "profile",
    "did", nullptr};

#ifdef _WIN32

#include "ecuio/j2534_ptypes.h"    /* FLOW_CONTROL_FILTER, CAN_29BIT_ID,
                                      TX_ISO15765_FRAME_PAD */
#include "win_utf8.h"              /* ecuio_is_absolute_path */

namespace {

/* Passive mode reads with this timeout. An empty read waits it out even
 * from a vendor that returns at once, so a quiet run takes the same
 * time on every adapter. */
constexpr uint32_t J2534_IDLE_READ_TIMEOUT_MS = 100;

/* With requests to serve, no read holds longer than this. The VX holds
 * every read for its whole timeout unless the batch fills, and the
 * surrogate asks for 16 messages, so a longer read would sit on a reply
 * that came back in 4 ms until its timeout ended
 * (docs/bench-sessions/2026-10-22.md). A reply is seen within one slice
 * of arriving. */
constexpr uint32_t J2534_READ_SLICE_MS = 10;

/* The request engine's timing is in request_engine.h; the link-health
 * rules, their quiet run and the failure backoff in link_health.h. */

/* On ISO 15765 channels J2534 carries the CAN ID in-band as 4
 * big-endian bytes, and the filter mask, pattern and flow-control
 * messages are bare 4-byte IDs: the adapter's ISO-TP stack does the
 * framing, so no PCI bytes. J2534-1 requires the three filter
 * messages to share DataSize and TxFlags. Verified: Opus IVS
 * "Constructing an OBDII Message", Quantex PassThruStartMsgFilter
 * docs. */
constexpr uint32_t J2534_ADDR_LEN       = 4;
constexpr uint32_t J2534_FILTER_MSG_LEN = 4;
constexpr uint32_t J2534_TX_ID_DEFAULT = ECUIO_OBD2_TESTER_ARB_ID;   /* shared
                                       * macro — cannot drift from the
                                       * obd2 set's own declaration */
constexpr uint32_t J2534_CAN_ID_MAX     = 0x1FFFFFFFu;   /* 29-bit */
constexpr uint32_t J2534_PID_MAX        = 0xFFu;

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

void pack_be32(uint8_t* out, uint32_t v)
{
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

/* One resolved receive/transmit channel. rx_id doubles as the
 * strip/stamp identity in pump_one: a received frame's 4-byte
 * address header matching chans[i].rx_id is stripped and stamped
 * with that rx_id before dispatch. */
struct j2534_channel {
    uint32_t rx_id = 0;
    uint32_t tx_id = 0;
    unsigned long filter_id = 0;
    bool filter_active = false;
    uint32_t addr_flags = 0;   /* CAN_29BIT_ID when either id > 11 bits */
};

struct ecuio_j2534_transport {
    ecuio_session_t* session = nullptr;
    ecuio_j2534_handle_t* handle = nullptr;
    unsigned long channel = 0;
    /*   chans        resolved channel table. Derived from the
     *                profile's declared channels at attach, with
     *                ?filter=/?txid= as overrides. ?filter= wins
     *                outright (single-channel shorthand); ?txid=
     *                alone rewrites every derived tx. Bounded by
     *                ECUIO_MAX_CHANNELS; each entry owns its filter
     *                id and CAN_29BIT flags. Frozen pre-thread.
     *   engine       the requests and their pacing (request_engine.h):
     *                the set's rotation, uds-generic's DID scan and the
     *                identification items, or the one ?poll= request,
     *                which outranks them all; empty for passive receive.
     *                Built at attach, the worker's alone after.
     *   probe        ADR-0015 §4 for the set's rotation: on unless
     *                ?probe=0 or ?poll=. Frozen pre-thread.
     *   identify     ADR-0021: each channel asks its ECU's identity
     *                once; on with the set's requests unless ?identify=0
     *                or ?poll=. Frozen pre-thread. */
    j2534_channel chans[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
    ecuio_request_engine engine;
    bool probe = false;
    bool identify = false;
    /* Frame time (j2534_clock.h): the host's at the first frame, counted
     * from epoch, the end of attach; the vendor's deltas after that.
     * Worker-only, through pump_one. */
    std::chrono::steady_clock::time_point epoch{};
    j2534_clock clock;
    /* The frame pump_one builds from each read, dispatched from here;
     * worker-only, and 4 KB, so it lives with the transport rather than
     * on the worker's stack. read_outcome::payload points into it. */
    ecuio_frame_t rx_frame{};
    std::atomic<bool> stop_requested{false};
    std::thread worker;
};

/* Collected URI query parameters — parse_query fills, resolve_uri
 * passes through, attach stores into the transport impl. One struct
 * rather than a six-pointer parse signature, and one place for the
 * coupling rule to live.
 *
 * baud == 0 is the "unset" sentinel (the parser rejects an explicit
 * baud=0), and j2534_loader's connect_channel substitutes the ISO 15765
 * default. flags pass through verbatim: 0 is both the default and a
 * legitimate value, and no bit is interpreted. */
struct j2534_uri_params {
    unsigned long baud = 0;
    unsigned long flags = 0;
    unsigned long rx_id = 0;     /* 0 = no ?filter= override */
    unsigned long tx_id = 0;     /* 0 = unset: profile's tx, else 0x7E0 */
    bool has_poll = false;       /* false = no ?poll= (passive or the
                                  * set's rotation) */
    unsigned long poll_pid = 0;  /* valid when has_poll; 0x00 allowed */
    bool probe = true;           /* ?probe=0 turns it off */
    bool identify = true;        /* ?identify=0 turns it off */
};

/* The channel ?poll='s request goes to, by the rule the set's rotation
 * follows (request_engine.h): that of a def whose value comes back in
 * its reply, else channel 0. */
uint32_t poll_channel(const ecuio_signal_set_t* set, uint8_t pid,
                      const uint32_t* rx_ids, uint32_t rx_count)
{
    if (set == nullptr || set->defs == nullptr) {
        return 0;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (ecuio_engine_request_for_def(&set->defs[i], req, &len) &&
            len == 2 && req[0] == 0x01 && req[1] == pid) {
            return ecuio_engine_channel_for(&set->defs[i], rx_ids, rx_count);
        }
    }
    return 0;
}

/* Derive the channel table from the set plus URI overrides. Never
 * fails: bounded by construction, and malformed set entries are
 * skipped. A log-only profile (raw: no set) with no ?filter= listens
 * on the default profile's channels, obd2's engine ECU (ADR-0013's
 * appendix of 2026-10-26): an ISO 15765 channel with no filter hears
 * nothing. A set without channels, and a lone ?txid= on one, yields
 * no channel — there is no receive ID to register a filter for. */
void resolve_channels(ecuio_j2534_transport* t,
                      const ecuio_signal_set_t* set,
                      const j2534_uri_params& params)
{
    if (set == nullptr && params.rx_id == 0) {
        set = ecuio_obd2_signal_set();
    }
    uint32_t n = 0;
    if (params.rx_id != 0) {
        uint32_t tx = static_cast<uint32_t>(params.tx_id);
        if (tx == 0 && set != nullptr && set->channel_count > 0) {
            tx = set->channels[0].tx_id;
        }
        if (tx == 0) {
            tx = J2534_TX_ID_DEFAULT;
        }
        t->chans[0].rx_id = static_cast<uint32_t>(params.rx_id);
        t->chans[0].tx_id = tx;
        n = 1;
    } else if (set != nullptr) {
        for (uint32_t i = 0; i < set->channel_count &&
                            n < ECUIO_MAX_CHANNELS; ++i) {
            const uint32_t rx = set->channels[i].rx_id;
            if (rx == 0) {
                continue;
            }
            uint32_t tx = (params.tx_id != 0)
                            ? static_cast<uint32_t>(params.tx_id)
                            : set->channels[i].tx_id;
            if (tx == 0) {
                tx = J2534_TX_ID_DEFAULT;
            }
            t->chans[n].rx_id = rx;
            t->chans[n].tx_id = tx;
            ++n;
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        t->chans[i].addr_flags =
            (t->chans[i].rx_id > 0x7FF || t->chans[i].tx_id > 0x7FF)
                ? ECUIO_PT_CAN_29BIT_ID : 0;
    }
    t->chan_count = n;
}

/* A received message as the frame the session delivers: skip bytes
 * dropped from the front (the CAN ID, once it matched a channel), the
 * rest cut at ECUIO_FRAME_DATA_MAX. EXTENDED comes from RxStatus, and
 * TRUNCATED says the frame holds less than the message. */
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
    f->flags = (m.rx_status & ECUIO_PT_CAN_29BIT_ID)
               ? ECUIO_FRAME_FLAG_EXTENDED : 0u;
    if (n < have) {
        f->flags |= ECUIO_FRAME_FLAG_TRUNCATED;
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
            std::chrono::steady_clock::now() - t->epoch).count());
    const uint64_t at_us = j2534_clock_step(t->clock, msg.timestamp,
                                            host_us);
    ecuio_frame_t& frame = t->rx_frame;

    if (t->chan_count == 0) {
        /* no channels — verbatim, id 0 (raw passive) counts as matched
         * since we explicitly requested verbatim delivery. */
        frame_from_msg(msg, 0, 0, at_us, &frame);
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
            if ((hdr & J2534_CAN_ID_MAX) == t->chans[i].rx_id) {
                frame_from_msg(msg, J2534_ADDR_LEN, t->chans[i].rx_id, at_us,
                               &frame);
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
void update_health(link_health& h, const read_outcome& o, bool reads_count)
{
    const bool hard =
        o.status != ECUIO_OK && o.status != ECUIO_ERR_VEHICLE_TIMEOUT;
    link_on_read(h, hard, o.got && o.matched, reads_count);
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
                  link_health& h)
{
    const j2534_channel& ch = t->chans[r.channel];
    uint8_t msg[J2534_ADDR_LEN + sizeof(r.bytes)];
    pack_be32(msg, ch.tx_id);
    memcpy(msg + J2534_ADDR_LEN, r.bytes, r.len);
    uint32_t written = 0;
    const ecuio_status_t wst = ecuio_j2534_write_frame(
        t->handle, t->channel, msg, J2534_ADDR_LEN + r.len,
        ch.addr_flags |   /* a 29-bit channel's CAN_29BIT_ID */
            ECUIO_PT_TX_ISO15765_FRAME_PAD,
        0, &written);
    const bool sent = (wst == ECUIO_OK && written >= 1);
    link_on_write(h, sent);
    return sent;
}

/* Backoff-aware sleep, abortable within 20 ms of a stop request —
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
            std::chrono::steady_clock::now() - t->epoch).count());
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

/* Hands what channel c's ECU has said it supports to the session, for
 * ecuio_session_get_channel_support, when the engine has learned
 * something new. */
void publish_support(ecuio_j2534_transport* t, uint32_t c,
                     uint32_t& published)
{
    const ecuio_request_engine& e = t->engine;
    if (e.chans[c].generation == published) {
        return;
    }
    published = e.chans[c].generation;
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, c, &s);
    s.rx_id = t->chans[c].rx_id;
    std::lock_guard<std::mutex> lock(t->session->support_mutex);
    t->session->support[c] = s;
}

/* Hands what channel c's ECU has said it is to the session, for
 * ecuio_session_get_channel_identity, when an item has settled. */
void publish_identity(ecuio_j2534_transport* t, uint32_t c,
                      uint32_t& published)
{
    const ecuio_request_engine& e = t->engine;
    if (e.chans[c].ident_generation == published) {
        return;
    }
    published = e.chans[c].ident_generation;
    ecuio_channel_identity_t s;
    ecuio_engine_identity(&e, c, &s);
    s.rx_id = t->chans[c].rx_id;
    std::lock_guard<std::mutex> lock(t->session->support_mutex);
    t->session->identity[c] = s;
}

/* Worker entrypoint — mirrors mock_worker's discipline: impl-owned
 * state, dispatch through the session snapshot path, stop via atom.
 *
 * One loop serves every mode (ADR-0015 §1). With requests (the set's
 * rotation, or ?poll='s one), each pass ends the waits that ran out,
 * sends each idle channel's next request once its floor has passed,
 * then reads once, for at most the slice, and gives a matched frame to
 * the engine, so the next request goes out when the reply is in.
 * Passive (no requests) only reads, and so does a session with nothing
 * left to ask. A read that brought a frame is followed by the next at
 * once, so every mode drains what has arrived; an empty one waits out
 * its timeout.
 *
 * With the probe on (§4) the engine walks each channel's supported-PID
 * chain before its rotation, and the walk resumes, where it stopped,
 * whenever the link returns to OK: a connect with the ignition off
 * answers nothing, and the ECU's answers come once it wakes.
 *
 * Requests keep going out while the link is hard-failed or requests are
 * failing, because each is the recovery probe and replies only follow
 * writes; the backoff then spaces the passes. Distorting the cadence
 * while dead is the point: retrying a dead device at full rate is not. */
void j2534_worker(ecuio_j2534_transport* t)
{
    ecuio_request_engine& e = t->engine;
    const bool requests = e.count > 0 || e.scan_count > 0;
    link_health h(t->session->last_error, t->session->error_count);
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    if (requests && t->probe) {
        ecuio_engine_probe(&e, engine_now_us(t));
    }

    while (!t->stop_requested.load()) {
        if (requests) {
            const int64_t now = engine_now_us(t);
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                if (ecuio_engine_expire(&e, c, now)) {
                    link_on_request_timeout(h);
                }
                const int32_t r = ecuio_engine_due(&e, c, now);
                if (r != ECUIO_ENGINE_NONE) {
                    const bool sent = send_request(
                        t, ecuio_engine_request_of(&e, c, r), h);
                    ecuio_engine_sent(&e, c, r, sent, engine_now_us(t));
                }
            }
        }

        /* With nothing due on any channel (a scan that found nothing
         * and polls nothing, a probe that dropped every request) no
         * request can time out, so the session reads as a passive one
         * does: empty reads count toward quiet, which can't tell an
         * ECU that went away from one with nothing left to answer. */
        const int64_t next_us = requests
            ? ecuio_engine_next_event_us(&e, engine_now_us(t))
            : INT64_MAX;
        const bool asking = next_us != INT64_MAX;
        const uint32_t timeout_ms = read_timeout_ms(next_us);
        const auto read_start = std::chrono::steady_clock::now();
        const read_outcome o = pump_one(t, timeout_ms);
        if (requests && o.matched) {
            (void)ecuio_engine_on_frame(&e, o.chan, o.payload, o.len,
                                        engine_now_us(t), nullptr);
        }
        update_health(h, o, !asking);
        if (h.recovered) {
            h.recovered = false;
            if (requests && t->probe) {
                ecuio_engine_probe(&e, engine_now_us(t));
            }
            if (requests) {
                /* the scan's and identification's unanswered requests */
                ecuio_engine_rescan(&e);
            }
        }
        for (uint32_t c = 0; requests && c < t->chan_count; ++c) {
            publish_support(t, c, published[c]);
            publish_identity(t, c, published_identity[c]);
        }

        if (h.backoff_ms > 0) {
            if (backoff_sleep(t, h.backoff_ms)) {
                break;
            }
            h.backoff_ms = link_backoff_next(h.backoff_ms);
        } else if (!o.got) {
            /* An empty read waits out its timeout, even from a vendor
             * that returns at once, so the loop never spins. */
            const auto waited = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - read_start).count();
            if (waited < timeout_ms &&
                backoff_sleep(t, timeout_ms - static_cast<uint32_t>(waited))) {
                break;
            }
        }
    }
}

/* Strict unsigned parse: decimal by default, '0x'/'0X' prefix enables
 * hex digits. Rejects empty digits, '+', '-', whitespace, and overflow.
 * No strtoul — its tolerance for signs/whitespace is a liability at
 * an ABI boundary where garbage must become an error, not a value. */
bool parse_number(const char* s, size_t len, unsigned long* out_val)
{
    if (len == 0) {
        return false;
    }

    bool hex = false;
    size_t i = 0;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = true;
        i = 2;
        if (i == len) {
            return false;   /* bare "0x" */
        }
    }

    const unsigned long max =
        (std::numeric_limits<unsigned long>::max)();   /* windows.h's max */
    const unsigned long base = hex ? 16ul : 10ul;
    unsigned long value = 0;
    for (; i < len; ++i) {
        unsigned long digit;
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned long>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned long>(c - 'a') + 10;
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned long>(c - 'A') + 10;
        } else {
            return false;
        }
        if (value > (max - digit) / base) {
            return false;   /* would overflow unsigned long */
        }
        value = value * base + digit;
    }

    *out_val = value;
    return true;
}

/* The query's values (ecuio_j2534_query_keys). Contract (strict — a bad
 * value means BAD_URI, surfacing at session_create before any DLL
 * load):
 *   - lowercase keys only:
 *       baud   decimal, nonzero
 *       flags  0x-hex or decimal; 0 legal
 *       filter CAN id 1..0x1FFFFFFF — receive ID; replaces the
 *              profile's channels (registers a FLOW_CONTROL filter)
 *       txid   CAN id 1..0x1FFFFFFF — transmit ID; default is the
 *              profile's tx, else 0x7E0
 *       poll   PID 0x00..0xFF — single-PID Mode-01 loop (0x00 reads
 *              the supported-PIDs bitmap); overrides the profile's
 *              rotation
 *       probe  0 or 1 — the supported-PID probe (ADR-0015 §4), on by
 *              default
 *       identify 0 or 1 — vehicle identification (ADR-0021), on by
 *              default with a set's requests
 *       profile core's key, read by ecuio_session_create
 *   - every occurrence is judged, and of good ones the last wins
 *   - create has split the query by the shared grammar and refused
 *     any other key (uri_query.h), so neither reaches here
 *   - poll needs a channel to receive on, but the channel may come
 *     from the profile: ?filter= supplies one directly, otherwise the
 *     profile's declared channels do. Enforced in j2534_attach, where
 *     both are known. */
ecuio_status_t parse_query(const ecuio_uri_query* query, j2534_uri_params* p)
{
    for (size_t i = 0; i < query->count; ++i) {
        const ecuio_uri::param& q = query->params[i];
        unsigned long v = 0;
        /* What a refused value should have been. */
        const char* wanted = nullptr;
        if (q.is("baud")) {
            if (!parse_number(q.val, q.val_len, &v) || v == 0) {
                wanted = "a nonzero decimal";
            } else {
                p->baud = v;
            }
        } else if (q.is("txid")) {
            if (!parse_number(q.val, q.val_len, &v) ||
                v == 0 || v > J2534_CAN_ID_MAX) {
                wanted = "a CAN ID from 1 to 0x1FFFFFFF";
            } else {
                p->tx_id = v;
            }
        } else if (q.is("poll")) {
            if (!parse_number(q.val, q.val_len, &v) || v > J2534_PID_MAX) {
                wanted = "a PID from 0x00 to 0xFF";
            } else {
                p->poll_pid = v;
                p->has_poll = true;
            }
        } else if (q.is("flags")) {
            if (!parse_number(q.val, q.val_len, &v)) {
                wanted = "a decimal or 0x-hex number";
            } else {
                p->flags = v;
            }
        } else if (q.is("filter")) {
            if (!parse_number(q.val, q.val_len, &v) ||
                v == 0 || v > J2534_CAN_ID_MAX) {
                wanted = "a CAN ID from 1 to 0x1FFFFFFF";
            } else {
                p->rx_id = v;
            }
        } else if (q.is("probe")) {
            if (q.val_len != 1 || (q.val[0] != '0' && q.val[0] != '1')) {
                wanted = "0 or 1";
            } else {
                p->probe = q.val[0] == '1';
            }
        } else if (q.is("identify")) {
            if (q.val_len != 1 || (q.val[0] != '0' && q.val[0] != '1')) {
                wanted = "0 or 1";
            } else {
                p->identify = q.val[0] == '1';
            }
        }   /* else profile or did: core's, read and validated by create */
        if (wanted != nullptr) {
            const int shown = static_cast<int>(
                (q.val_len < 32) ? q.val_len : 32);
            ecuio_detail_set("?%.*s= needs %s, not '%.*s'",
                             static_cast<int>(q.key_len), q.key,
                             wanted, shown, q.val);
            return ECUIO_ERR_BAD_URI;
        }
    }

    /* The poll-needs-a-channel rule lives in j2534_attach, not here:
     * this parser can't see the profile's declared channels, and
     * rejecting ?poll= without ?filter= here would refuse a profile
     * that declares its own (?profile=obd2&poll=0x0C). Syntax is this
     * function's job; coupling is attach's — and attach still fires
     * before any DLL load, so malformed URIs stay fail-fast. */
    return ECUIO_OK;
}

/* Resolve a j2534:// URI's selector to a concrete dll_path. Grammar (see
 * internal.h): [selector][?query]. The query is cut off here, and
 * attach has read its values before this runs, so it never reaches
 * LoadLibraryEx or the vendor-name match. Selector: empty = first
 * device; path-like = direct load, absolute only; else vendor-name
 * match. A relative or too-long path is ECUIO_ERR_BAD_URI; every miss
 * ECUIO_ERR_NO_TRANSPORT. */
ecuio_status_t resolve_uri(const char* uri, ecuio_j2534_device_t* dev)
{
    const char* rest = uri + 8;   /* core.cpp verified the prefix */

    const char* q = strchr(rest, '?');

    char selector[ECUIO_J2534_DLL_PATH_LEN];
    const size_t n = (q != nullptr)
                     ? static_cast<size_t>(q - rest)
                     : strlen(rest);
    if (n >= sizeof(selector)) {
        ecuio_detail_set("the adapter selector is longer than any DLL path");
        return ECUIO_ERR_BAD_URI;
    }
    memcpy(selector, rest, n);
    selector[n] = '\0';

    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    if (selector[0] == '\0') {
        ecuio_status_t st = ecuio_j2534_discover_devices(&devices, &count);
        if (st != ECUIO_OK) {
            ecuio_detail_set("couldn't list the installed J2534 adapters");
            return st;
        }
        if (count == 0) {
            ecuio_detail_set("no J2534 adapter is installed");
            return ECUIO_ERR_NO_TRANSPORT;
        }
        *dev = devices[0];   /* first device — documented default */
        ecuio_j2534_free_devices(devices);
        return ECUIO_OK;
    }

    /* Path-like selector: contains a separator or a .dll suffix.
     * Reaches LoadLibraryEx verbatim (discover-not-involved), so it must
     * be absolute: with LOAD_WITH_ALTERED_SEARCH_PATH a relative path
     * follows the standard search order, the current directory included,
     * and could load another DLL of that name. */
    if (strchr(selector, '\\') != nullptr ||
        strchr(selector, '/') != nullptr ||
        (strlen(selector) >= 4 &&
         _stricmp(selector + strlen(selector) - 4, ".dll") == 0)) {
        if (!ecuio_is_absolute_path(selector)) {
            ecuio_detail_set("the DLL path must be absolute, like "
                             "C:\\path\\to\\vendor.dll");
            return ECUIO_ERR_BAD_URI;
        }
        if (strlen(selector) >= ECUIO_J2534_DLL_PATH_LEN) {
            ecuio_detail_set("the DLL path is too long");
            return ECUIO_ERR_BAD_URI;
        }
        memcpy(dev->dll_path, selector, strlen(selector) + 1);
        return ECUIO_OK;
    }

    /* Otherwise: vendor-name match against discovery, case-insensitive. */
    ecuio_status_t st = ecuio_j2534_discover_devices(&devices, &count);
    if (st != ECUIO_OK) {
        ecuio_detail_set("couldn't list the installed J2534 adapters");
        return st;
    }
    bool matched = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (_stricmp(devices[i].vendor_name, selector) == 0) {
            *dev = devices[i];
            matched = true;
            break;
        }
    }
    ecuio_j2534_free_devices(devices);
    if (!matched) {
        ecuio_detail_set("no installed J2534 adapter is named '%s'", selector);
        return ECUIO_ERR_NO_TRANSPORT;
    }
    return ECUIO_OK;
}

ecuio_status_t j2534_start(ecuio_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<ecuio_j2534_transport*>(impl);
    if (t->worker.joinable()) {
        return ECUIO_OK;    /* internal.h: second start on a live
                         * transport returns ECUIO_OK */
    }

    for (uint32_t i = 0; i < t->chan_count; ++i) {
        uint8_t mask[J2534_FILTER_MSG_LEN];
        uint8_t pattern[J2534_FILTER_MSG_LEN];
        uint8_t fc[J2534_FILTER_MSG_LEN];
        pack_be32(mask, 0xFFFFFFFFu);   /* exact-ID match, all 4 bytes */
        pack_be32(pattern, t->chans[i].rx_id);
        pack_be32(fc, t->chans[i].tx_id);
        ecuio_status_t st = ecuio_j2534_start_msg_filter(
            t->handle, t->channel,
            ECUIO_PT_FLOW_CONTROL_FILTER,
            mask, J2534_FILTER_MSG_LEN,
            pattern, J2534_FILTER_MSG_LEN,
            fc, J2534_FILTER_MSG_LEN,
            t->chans[i].addr_flags | ECUIO_PT_TX_ISO15765_FRAME_PAD,
            &t->chans[i].filter_id);
        if (st != ECUIO_OK) {
            /* Unwind the ones already registered — no partial
             * filter state survives a failed start. */
            for (uint32_t j = 0; j < i; ++j) {
                if (t->chans[j].filter_active) {
                    (void)ecuio_j2534_stop_msg_filter(
                        t->handle, t->channel, t->chans[j].filter_id);
                    t->chans[j].filter_active = false;
                }
            }
            return st;
        }
        t->chans[i].filter_active = true;
    }

    t->stop_requested.store(false);
    try {
        t->worker = std::thread(j2534_worker, t);
    } catch (...) {
        for (uint32_t i = 0; i < t->chan_count; ++i) {
            if (t->chans[i].filter_active) {
                (void)ecuio_j2534_stop_msg_filter(t->handle, t->channel,
                                                  t->chans[i].filter_id);
                t->chans[i].filter_active = false;
            }
        }
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

/* detach(session, impl) — void per internal.h, called exactly once from
 * ecuio_session_close AFTER the closed flag is set. Worker joined
 * FIRST (never alive while vendor code could be unmapped), filter
 * stop best-effort before disconnect, then free impl (the session
 * frees the wrapper). */
void j2534_detach(ecuio_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<ecuio_j2534_transport*>(impl);
    if (t->worker.joinable()) {
        t->stop_requested.store(true);
        t->worker.join();
    }
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        if (t->chans[i].filter_active) {
            (void)ecuio_j2534_stop_msg_filter(t->handle, t->channel,
                                              t->chans[i].filter_id);
            t->chans[i].filter_active = false;
        }
    }
    (void)ecuio_j2534_disconnect_channel(t->handle, t->channel);
    (void)ecuio_j2534_close(t->handle);
    delete t;
}

/* The session info (ADR-0019): what the channel connected with, then
 * the two readings the adapter may give. Either reading can fail without
 * failing the connect: its flag stays clear, and the session log (on
 * the surrogate) has the vendor's status. Both are device calls, made
 * once here; nothing reads the battery again while the session runs. */
static_assert(ECUIO_MAX_CHANNELS <= ECUIO_SESSION_INFO_MAX_CHANNELS,
              "the session info lists every channel");

void describe_session(const ecuio_j2534_transport* t, unsigned long baud,
                      unsigned long connect_flags, ecuio_session_info_t* info)
{
    *info = ecuio_session_info_t{};
    info->backend = ecuio_j2534_backend(t->handle);
    info->dll_arch = ecuio_j2534_dll_arch(t->handle);
    info->protocol_id = static_cast<uint32_t>(ECUIO_PT_PROTOCOL_ISO15765);
    /* 0 is unset: connect_channel connected at the ISO 15765 default. */
    info->baud = static_cast<uint32_t>(
        (baud != 0) ? baud : ECUIO_PT_BAUD_ISO15765_500K);
    info->connect_flags = static_cast<uint32_t>(connect_flags);
    info->channel_count = t->chan_count;
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        info->rx_ids[i] = t->chans[i].rx_id;
        info->tx_ids[i] = t->chans[i].tx_id;
    }

    uint8_t mv[4] = {};
    uint32_t mv_len = 0;
    if (ecuio_j2534_ioctl(t->handle, t->channel, ECUIO_PT_IOCTL_READ_VBATT,
                          nullptr, 0, mv, sizeof(mv), &mv_len) == ECUIO_OK &&
        mv_len == sizeof(mv)) {
        memcpy(&info->vbatt_mv, mv, sizeof(mv));
        info->flags |= ECUIO_SESSION_INFO_VBATT;
    }
    if (ecuio_j2534_read_version(t->handle, info->firmware_version,
                                 info->dll_version,
                                 info->api_version) == ECUIO_OK) {
        info->flags |= ECUIO_SESSION_INFO_VERSIONS;
    }
}

/* Ops table: static storage owned by this TU (internal.h contract),
 * defined before attach references it. */
const ecuio_transport_ops_t kJ2534Ops = {
    j2534_start,
    j2534_detach
};

ecuio_status_t j2534_attach(ecuio_session_t* session, const char* uri,
                            const ecuio_uri_query* query)
{
    /* Idempotence (internal.h contract, mock parity): checked FIRST,
     * before any device open, so the early-out leaks nothing. */
    if (session->transport != nullptr) {
        return ECUIO_OK;
    }

    /* The values before the selector: a bad one fails fast, with no
     * discovery and no DLL load. */
    j2534_uri_params params = {};
    ecuio_status_t st = parse_query(query, &params);
    if (st != ECUIO_OK) {
        return st;
    }
    ecuio_j2534_device_t dev = {};
    st = resolve_uri(uri, &dev);
    if (st != ECUIO_OK) {
        return st;
    }

    /* Channels and requests are resolved before the device opens.
     * core.cpp installs the profile's set before calling attach. */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);

    /* Held here until attach succeeds: every refusal below just
     * returns. */
    std::unique_ptr<ecuio_j2534_transport> t(
        new (std::nothrow) ecuio_j2534_transport());
    if (t == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    resolve_channels(t.get(), set, params);

    /* Requests, fixed before the thread starts:
     *   ?poll=        -> that one Service 01 request; the set's rotation
     *                    isn't built, so the PID in the URI is the only
     *                    one asked for. It may be 0x00 (the
     *                    supported-PIDs bitmap), hence the presence flag
     *   uds-generic   -> the rotation from the set's polled head, and
     *                    the scan of its ranges (ADR-0020)
     *   any other set -> the set's rotation (request_engine.h), which
     *                    registration kept within the table
     *   no requests   -> passive receive */
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        rx_ids[i] = t->chans[i].rx_id;
    }
    ecuio_engine_init(&t->engine, t->chan_count);
    if (params.has_poll) {
        const uint8_t req[2] = {0x01, static_cast<uint8_t>(params.poll_pid)};
        (void)ecuio_engine_add(&t->engine,
                               poll_channel(set, req[1], rx_ids, t->chan_count),
                               req, sizeof(req));
    } else if (session->uds == nullptr) {
        (void)ecuio_engine_derive(&t->engine, set, rx_ids, t->chan_count,
                                  nullptr);
    } else {
        /* uds-generic (ADR-0020): the rotation from the set's polled
         * head, then each scanned DID once, on the channel of its ECU by
         * the rule the rotation's requests follow (ecuio_engine_channel_for:
         * channel 0 under a ?filter= override). Create's limits keep both
         * within the engine's tables. */
        const ecuio_signal_set_t head = ecuio_uds_polled_set(*session->uds);
        (void)ecuio_engine_derive(&t->engine, &head, rx_ids, t->chan_count,
                                  nullptr);
        {
            for (const ecuio_uds_scan_target& s : session->uds->scan) {
                uint32_t c = 0;
                for (uint32_t i = 0; i < t->chan_count; ++i) {
                    if (rx_ids[i] == s.rx_id) {
                        c = i;
                    }
                }
                (void)ecuio_engine_scan_add(&t->engine, c, s.did);
            }
        }
    }
    /* ?poll= sends its one request as asked, whatever the ECU says. */
    t->probe = params.probe && !params.has_poll;
    /* Identification (ADR-0021) rides on a session that already
     * requests: a passive session sends nothing, and ?poll= asks its one
     * PID alone. Every channel asks the list once. */
    t->identify = params.identify && !params.has_poll &&
                  (t->engine.count > 0 || t->engine.scan_count > 0);
    if (t->identify) {
        ecuio_engine_identify(&t->engine);
    }

    /* Coupling check: requests need at least one channel to receive on,
     * or they go out against guaranteed silence forever (an ISO 15765
     * channel with no filter receives nothing). A set's requests get
     * theirs from the set; ?poll= may take ?filter= or the profile's
     * declared channels. Shipped profiles declare channels, so the set's
     * arm is reachable only by a set author omitting them. Fires before
     * any hardware is touched. */
    if ((t->engine.count > 0 || t->engine.scan_count > 0) &&
        t->chan_count == 0) {
        if (params.has_poll) {
            ecuio_detail_set("?poll= needs a channel to receive on: add "
                             "?filter=, or use a profile that declares one");
        } else {
            ecuio_detail_set("the profile requests PIDs but declares no "
                             "channel to receive on");
        }
        return ECUIO_ERR_BAD_URI;
    }

    /* Connect flags: ?flags= as given, plus CAN_29BIT_ID when the
     * channels use 29-bit IDs, so the channel opens at the width its
     * filters and writes use (resolve_channels sets each channel's
     * addr_flags by the same rule). Channels mixing 11- and 29-bit IDs
     * would need CAN_ID_BOTH, which this transport doesn't request, so
     * that mix is rejected here, before any DLL loads. */
    uint32_t wide_chans = 0;
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        if ((t->chans[i].addr_flags & ECUIO_PT_CAN_29BIT_ID) != 0) {
            ++wide_chans;
        }
    }
    if (wide_chans != 0 && wide_chans != t->chan_count) {
        ecuio_detail_set("the channels mix 11-bit and 29-bit IDs, which "
                         "one connection can't carry");
        return ECUIO_ERR_BAD_URI;
    }
    const unsigned long connect_flags =
        params.flags | ((wide_chans != 0) ? ECUIO_PT_CAN_29BIT_ID : 0UL);

    ecuio_j2534_handle_t* handle = nullptr;
    st = ecuio_j2534_open(&dev, &handle);
    if (st != ECUIO_OK) {
        return st;
    }

    unsigned long channel = 0;
    st = ecuio_j2534_connect_channel(handle, params.baud, connect_flags,
                                     &channel);
    if (st != ECUIO_OK) {
        ecuio_j2534_close(handle);
        return st;
    }

    /* The envelope, which the session owns and frees (internal.h). */
    auto* wrap = new (std::nothrow) ecuio_transport_t();
    if (wrap == nullptr) {
        ecuio_j2534_disconnect_channel(handle, channel);
        ecuio_j2534_close(handle);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    wrap->ops = &kJ2534Ops;

    t->session = session;
    t->handle = handle;
    t->channel = channel;
    describe_session(t.get(), params.baud, connect_flags, &session->info);
    {
        /* Each channel's ECU, its requests and its identification
         * items, nothing learned yet. */
        std::lock_guard<std::mutex> lock(session->support_mutex);
        session->support_count = t->chan_count;
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            ecuio_engine_support(&t->engine, c, &session->support[c]);
            session->support[c].rx_id = t->chans[c].rx_id;
            ecuio_engine_identity(&t->engine, c, &session->identity[c]);
            session->identity[c].rx_id = t->chans[c].rx_id;
        }
    }
    t->epoch = std::chrono::steady_clock::now();   /* as mock: end of attach */

    wrap->impl = t.release();
    session->transport = wrap;   /* session.h member — direct install */
    return ECUIO_OK;
}

} // namespace

extern "C" ecuio_status_t ecuio_j2534_transport_attach(
    ecuio_session_t* session, const char* uri, const ecuio_uri_query* query)
{
    return j2534_attach(session, uri, query);
}

#else /* !_WIN32 */

/* Linking stub — core.cpp routes j2534:// only on _WIN32, but the
 * symbol must exist for the headless-Unix CI build (core links
 * without Win32 J2534). */
extern "C" ecuio_status_t ecuio_j2534_transport_attach(
    ecuio_session_t* session, const char* uri, const ecuio_uri_query* query)
{
    (void)session;
    (void)uri;
    (void)query;
    return ECUIO_ERR_NO_TRANSPORT;
}

#endif /* _WIN32 */
