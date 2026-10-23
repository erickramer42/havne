// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — test_j2534_stream: URI parsing, connect parameters,
//          filter registration, request framing and routing, header
//          strip and arbitration_id stamp, scheduler rotation and drain,
//          ?poll= precedence. test_j2534_health: the link-health
//          machine (Known Issue #11) on both backends: hard and quiet
//          reads, untouched counts, the write latch and its priority,
//          unmatched frames, backoff growth.
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "ecuio/j2534.h"
#include "pid_scheduler.h"
#include "j2534_backoff.h"
#include "uri_query.h"
#include "error_detail.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32

#include "ecuio/j2534_ptypes.h"    /* FLOW_CONTROL_FILTER, CAN_29BIT_ID,
                                      TX_ISO15765_FRAME_PAD */

namespace {

constexpr uint32_t J2534_POLL_INTERVAL_MS   = 20;
constexpr uint32_t J2534_READ_TIMEOUT_MS    = 100;
constexpr uint32_t J2534_REQUEST_CYCLE_MS   = 100;   /* poll cadence */

/* Scheduler mode: one request per 100 ms cycle (rotating PIDs), then
 * a drain of at most DRAIN_BUDGET reads, so the worker never exceeds
 * 100 reads/s whatever the bus load. Dropped frames count against the
 * budget too: it bounds loop time, not delivery. Normally there is one
 * response per request, so each of a set's N PIDs refreshes every
 * N × 100 ms. The drain timeout is short because ReadMsgs blocks for the
 * whole timeout on an empty queue; 20 ms leaves room for a slow ECU
 * without spending the cycle waiting. The first empty read ends the
 * drain. Both values are BENCH candidates. */
constexpr uint32_t J2534_DRAIN_BUDGET          = 10;
constexpr uint32_t J2534_DRAIN_READ_TIMEOUT_MS  = 20;

/* The failure backoff's constants and step are in j2534_backoff.h. */

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
/* Every drain ends on an empty read, which a conforming vendor reports
 * as ERR_TIMEOUT, so one timeout means nothing. "Not responding" is
 * this many consecutive empty or timed-out reads (counted per read;
 * about 1 s in scheduler mode). */
constexpr uint32_t J2534_HEALTH_QUIET_CYCLES = 10;

/* A read's outcome. "matched" (stripped and dispatched to one of our
 * channels) is kept apart from "delivered but unmatched" (the adapter
 * is reading the bus, but the frame isn't a reply to us), because only
 * a matched frame proves the link works end to end. An unmatched one
 * clears hard failure and the backoff but reads as VEHICLE_TIMEOUT,
 * not OK, and leaves the quiet count as it is. */
struct read_outcome {
    ecuio_status_t status;
    bool got;
    bool matched;
};

void pack_be32(uint8_t* out, uint32_t v)
{
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

/* Mode-01 request on an ISO 15765 channel: [4-byte tx ID][01][PID],
 * 6 bytes, sent with ISO15765_FRAME_PAD. The adapter adds the ISO-TP
 * PCI byte and pads the CAN frame; ISO 15765-4 has no data checksum.
 * Verified: Opus IVS (the RPM request is [ID][01][0C], DataSize 6) and
 * Quantex PassThruWriteMsgs docs. Still to confirm on a real adapter:
 * that it honours FRAME_PAD. */
void build_mode01_request(uint8_t out[6], uint32_t tx_id, uint32_t pid)
{
    pack_be32(out, tx_id);
    out[4] = 0x01;   /* Mode 01 */
    out[5] = static_cast<uint8_t>(pid);
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
    /* Connect parameters from the URI query. baud == 0 is the
     * "unset" sentinel — the URI parser rejects an explicit baud=0 —
     * and j2534_loader's connect_channel substitutes the ISO 15765
     * default. flags pass through verbatim: 0 is both the default and
     * a legitimate value. Individual flag bits are not interpreted. */
    unsigned long baud = 0;
    unsigned long flags = 0;
    /*   has_poll     a ?poll= URI: a single-PID Mode-01 request loop
     *                for poll_pid, which may be 0x00 (the supported-
     *                PIDs bitmap), hence the presence flag. Outranks
     *                the set's rotation: a ?poll= URI never builds the
     *                set scheduler, so "just this PID" means what it
     *                says. Needs a channel to receive on — ?filter=
     *                or the profile's own (enforced at attach).
     *   chans        resolved channel table. Derived from the
     *                profile's declared channels at attach, with
     *                ?filter=/?txid= as overrides. ?filter= wins
     *                outright (single-channel shorthand); ?txid=
     *                alone rewrites every derived tx. Bounded by
     *                ECUIO_MAX_CHANNELS; each entry owns its filter
     *                id and CAN_29BIT flags. Frozen pre-thread. */
    bool has_poll = false;
    uint32_t poll_pid = 0;
    j2534_channel chans[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
    const ecuio_signal_set_t* set = nullptr; /* frozen at attach;
                                             * worker routes PID->tx */
    ecuio_pid_scheduler sched;
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    ecuio_transport_t* self = nullptr;
};

/* Collected URI query parameters — parse_query fills, resolve_uri
 * passes through, attach stores into the transport impl. One struct
 * rather than a six-pointer parse signature, and one place for the
 * coupling rule to live. */
struct j2534_uri_params {
    unsigned long baud = 0;
    unsigned long flags = 0;
    unsigned long rx_id = 0;     /* 0 = no ?filter= override */
    unsigned long tx_id = 0;     /* 0 = unset: profile's tx, else 0x7E0 */
    bool has_poll = false;       /* false = no ?poll= (passive or the
                                  * set's rotation) */
    unsigned long poll_pid = 0;  /* valid when has_poll; 0x00 allowed */
};

/* PID -> def message_id: a Mode-01 def names its ECU via message_id
 * (= the channel's rx_id). ecuio_mode01_pid is the test the scheduler
 * enrolls by, so a request goes where its PID was enrolled from.
 * O(set->count) per request — a few dozen defs at most; this
 * deliberately keeps pid_scheduler.h per-PID-rx-free so the scheduler
 * stays profile-agnostic. */
uint32_t rx_for_pid(const ecuio_signal_set_t* set, uint8_t pid)
{
    if (set == nullptr) {
        return 0;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t* d = &set->defs[i];
        uint8_t def_pid = 0;
        if (ecuio_mode01_pid(d, &def_pid) && def_pid == pid) {
            return d->message_id;
        }
    }
    return 0;   /* no Mode-01 def for this PID — raw/poll territory */
}

/* Channel serving a receive id; falls back to channel 0 (single-
 * channel sets always match on the first iteration anyway). */
uint32_t channel_index_for_rx(const ecuio_j2534_transport* t, uint32_t rx)
{
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        if (t->chans[i].rx_id == rx) {
            return i;
        }
    }
    return 0;
}

/* Derive the channel table from the set plus URI overrides. Never
 * fails: bounded by construction, and malformed set entries are
 * skipped. A lone ?txid= on a profile without channels yields no
 * channel — there is no receive ID to register a filter for. */
void resolve_channels(ecuio_j2534_transport* t,
                      const ecuio_signal_set_t* set,
                      const j2534_uri_params& params)
{
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

/* Read one frame and dispatch it if it matches a channel. Returns the
 * outcome: status, whether a frame arrived, and whether it matched. An
 * unmatched delivery is kept distinct from silence (see read_outcome). */
read_outcome pump_one(ecuio_j2534_transport* t, uint32_t timeout_ms)
{
    bool got = false;
    ecuio_frame_t frame{};
    ecuio_status_t st = ecuio_j2534_read_frame(t->handle, t->channel,
                                               &frame, timeout_ms,
                                               &got);
    if (st != ECUIO_OK || !got) {
        return {st, false, false};   /* no match if no frame */
    }

    if (t->chan_count == 0) {
        ecuio_session_dispatch_callbacks(t->session, &frame);
        /* no channels — verbatim, id 0 (raw passive) counts as matched
         * since we explicitly requested verbatim delivery. */
        return {ECUIO_OK, true, true};
    }

    if (frame.data_length >= J2534_ADDR_LEN) {
        const uint32_t hdr =
            (static_cast<uint32_t>(frame.data[0]) << 24) |
            (static_cast<uint32_t>(frame.data[1]) << 16) |
            (static_cast<uint32_t>(frame.data[2]) << 8) |
             static_cast<uint32_t>(frame.data[3]);
        for (uint32_t i = 0; i < t->chan_count; ++i) {
            if ((hdr & J2534_CAN_ID_MAX) == t->chans[i].rx_id) {
                frame.arbitration_id = t->chans[i].rx_id;
                const uint16_t n = static_cast<uint16_t>(
                    frame.data_length - J2534_ADDR_LEN);
                memmove(frame.data, frame.data + J2534_ADDR_LEN, n);
                frame.data_length = n;
                ecuio_session_dispatch_callbacks(t->session, &frame);
                return {ECUIO_OK, true, true};    /* matched */
            }
        }
        /* no match: a frame for another ID got past the adapter's
         * filter. Dropped, but it still proves the adapter is reading */
        return {ECUIO_OK, true, false};
    }
    return {ECUIO_OK, true, false};
}

/* Read-side link health (Known Issue #11; full table in docs/j2534.md).
 * Session atomics are written only on transitions; health, backoff_ms
 * and quiet_runs are worker-local. First match wins:
 *   hard read failure  -> TRANSPORT_HARD_FAILURE, one episode, backoff
 *   write latch set    -> no change (REQUEST_FAILED belongs to writes)
 *   matched frame      -> OK, backoff and quiet count cleared
 *   unmatched frame    -> VEHICLE_TIMEOUT at once, backoff cleared
 *   quiet read         -> after J2534_HEALTH_QUIET_CYCLES in a row,
 *                         VEHICLE_TIMEOUT and backoff cleared */
void update_health(ecuio_j2534_transport* t, int32_t& health,
                   uint32_t& backoff_ms, uint32_t& quiet_runs,
                   bool write_latch, ecuio_status_t st,
                   bool got, bool matched)
{
    /* Ordering: a hard READ failure outranks everything, including a
     * latched write failure, so it's evaluated FIRST. pump_one
     * guarantees got implies st == ECUIO_OK, so any status other than
     * ECUIO_OK and the vehicle-timeout translation is hard. */
    if (st != ECUIO_OK && st != ECUIO_ERR_VEHICLE_TIMEOUT) {
        if (health != ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
            t->session->last_error.store(ECUIO_ERR_TRANSPORT_HARD_FAILURE);
            health = ECUIO_ERR_TRANSPORT_HARD_FAILURE;
            t->session->error_count.fetch_add(1);
            backoff_ms = J2534_BACKOFF_BASE_MS;
        }
        quiet_runs = 0;
        return;
    }

    /* Latched write failure: good, quiet and unmatched reads must not
     * move health, because REQUEST_FAILED clears only on a
     * successful write (handle_write_result). Hard failures already
     * returned above, so the latch cannot suppress them. */
    if (write_latch) {
        return;
    }

    /* Remaining statuses are ECUIO_OK with/without a frame, or
     * ECUIO_ERR_VEHICLE_TIMEOUT (never with a frame, per pump_one). */
    if (got) {
        if (matched) {
            /* Matched response: full recovery */
            if (health != ECUIO_OK) {
                t->session->last_error.store(ECUIO_OK);
                health = ECUIO_OK;
            }
            backoff_ms = 0;
            quiet_runs = 0;
        } else {
            /* Delivered but unmatched: the adapter read the bus, so
             * hard failure and backoff clear — but this
             * is not a response to us: transition to quiet at once
             * (no debounce), quiet_runs left as it is, no episode. */
            if (health != ECUIO_ERR_VEHICLE_TIMEOUT) {
                t->session->last_error.store(ECUIO_ERR_VEHICLE_TIMEOUT);
                health = ECUIO_ERR_VEHICLE_TIMEOUT;
            }
            backoff_ms = 0;
        }
        return;
    }

    /* Quiet: an empty NOERROR read and an ERR_TIMEOUT read mean the
     * same thing — the adapter answered with nothing. One debounce
     * covers both, and the backoff clears when it fires. That matters
     * on the VX, which answers every empty read with NOERROR. */
    ++quiet_runs;
    if (quiet_runs < J2534_HEALTH_QUIET_CYCLES) {
        return;
    }
    if (health != ECUIO_ERR_VEHICLE_TIMEOUT) {
        t->session->last_error.store(ECUIO_ERR_VEHICLE_TIMEOUT);
        health = ECUIO_ERR_VEHICLE_TIMEOUT;
    }
    backoff_ms = 0;
}

/* Write-side link health, separate from reads, because a bus where no
 * node ACKs (ignition off, ECU asleep) fails writes while the adapter
 * still reads cleanly; calling that a hard failure would blame the
 * adapter. A failed write latches REQUEST_FAILED with ONE episode; a
 * successful write clears it at once, since an accepted write proves
 * requests flow. A hard READ failure outranks it; quiet reads can't
 * downgrade it. */
void handle_write_result(ecuio_j2534_transport* t,
                         int32_t& health, uint32_t& backoff_ms,
                         bool& write_failed, ecuio_status_t wst)
{
    if (wst == ECUIO_OK) {
        if (write_failed) {
            write_failed = false;
            if (health == ECUIO_ERR_REQUEST_FAILED) {
                t->session->last_error.store(ECUIO_OK);
                health = ECUIO_OK;
                backoff_ms = 0;
            }
        }
        return;
    }

    write_failed = true;
    if (health == ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
        return;  /* hard read failure outranks, don't change state */
    }
    if (health != ECUIO_ERR_REQUEST_FAILED) {
        /* Transition INTO requests-failing: ONE episode, backoff at base */
        t->session->last_error.store(ECUIO_ERR_REQUEST_FAILED);
        health = ECUIO_ERR_REQUEST_FAILED;
        t->session->error_count.fetch_add(1);
        backoff_ms = J2534_BACKOFF_BASE_MS;
    }
    /* Already REQUEST_FAILED: leave backoff alone, it doubles at sleep site */
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

/* Worker entrypoint — mirrors mock_worker's discipline: impl-owned
 * state, dispatch through the session snapshot path, stop via atom.
 *
 * Three modes, fixed before the thread starts:
 *   SCHEDULER (ADR-0013): sched.count > 0. Per 100 ms cycle: next PID
 *     -> Mode-01 request -> write -> drain up to J2534_DRAIN_BUDGET
 *     frames (short timeout, stop at first empty) -> sleep the cycle
 *     remainder against an absolute deadline, so drain cost does not
 *     drift the cadence.
 *   POLL: has_poll. One request, one read (100 ms timeout), then
 *     a 100 ms sleep; same request shape as the scheduler.
 *   PASSIVE: one read (100 ms timeout) then a 20 ms sleep; no
 *     requests.
 * Both request modes keep writing while hard-failed or request-failed:
 * the request is the recovery probe, and replies only follow writes.
 *
 * A write failure latches REQUEST_FAILED; only a successful write
 * clears it — reads cannot, and a hard read failure outranks it
 * (update_health evaluates hard first). */
void j2534_worker(ecuio_j2534_transport* t)
{
    const bool sched_mode = !t->has_poll && (t->sched.count > 0);

    int32_t health = ECUIO_OK;
    bool write_failed = false;   /* latched and cleared only by
                                  * handle_write_result */
    uint32_t backoff_ms = 0;   /* 0 = healthy cadence */
    uint32_t quiet_runs = 0;

    while (!t->stop_requested.load()) {
        const auto cycle_start = std::chrono::steady_clock::now();

        if (sched_mode) {
            /* The request doubles as the recovery probe, so it is sent
             * even while hard-failed: replies only follow writes.
             * Backoff stretches the cycle, which slows the probing. A
             * failed write latches ECUIO_ERR_REQUEST_FAILED through
             * handle_write_result; it is never a hard failure. */
            uint8_t pid = 0;
            (void)ecuio_sched_next(&t->sched, &pid);
            const uint32_t ci =
                channel_index_for_rx(t, rx_for_pid(t->set, pid));
            uint8_t req[6];
            build_mode01_request(req, t->chans[ci].tx_id, pid);
            uint32_t written = 0;
            ecuio_status_t wst = ecuio_j2534_write_frame(
                t->handle, t->channel, req, sizeof(req),
                t->chans[ci].addr_flags |
                    ECUIO_PT_TX_ISO15765_FRAME_PAD,
                0, &written);
            handle_write_result(t, health, backoff_ms, write_failed, wst);

            /* Drain: health comes from these reads. The first empty or
             * failing read ends the drain; the budget bounds the rest. */
            for (uint32_t i = 0; i < J2534_DRAIN_BUDGET; ++i) {
                read_outcome o = pump_one(t,
                                          J2534_DRAIN_READ_TIMEOUT_MS);
                update_health(t, health, backoff_ms, quiet_runs, write_failed,
                              o.status, o.got, o.matched);
                if (!o.got) {
                    break;
                }
            }

            /* Sleep: normal = cycle remainder vs absolute deadline.
             * Hard failure = backoff stretch, doubling after each
             * slept interval, capped. Cadence distortion while dead
             * is the POINT — retrying a dead device at 10 Hz is not. */
            if (backoff_sleep(t, backoff_ms) ) {
                break;
            }
            if (backoff_ms > 0) {
                backoff_ms = j2534_backoff_next(backoff_ms);
            } else {
                const auto target = cycle_start +
                    std::chrono::milliseconds(J2534_REQUEST_CYCLE_MS);
                const auto now = std::chrono::steady_clock::now();
                if (target > now) {
                    std::this_thread::sleep_until(target);
                }
            }
        } else if (t->has_poll) {
            /* POLL: write, then read this request's response. The
             * write is always sent, as in scheduler mode, and a failed
             * write feeds the health machine the same way. */
            uint8_t req[6];
            const uint32_t ci = channel_index_for_rx(
                t, rx_for_pid(t->set, static_cast<uint8_t>(t->poll_pid)));
            build_mode01_request(req, t->chans[ci].tx_id, t->poll_pid);
            uint32_t written = 0;
            ecuio_status_t wst = ecuio_j2534_write_frame(
                t->handle, t->channel, req, sizeof(req),
                t->chans[ci].addr_flags |   /* same channel as tx_id,
                                             * so a 29-bit channel gets
                                             * its CAN_29BIT_ID flag */
                    ECUIO_PT_TX_ISO15765_FRAME_PAD,
                0, &written);
            handle_write_result(t, health, backoff_ms, write_failed, wst);

            read_outcome o = pump_one(t,
                                      J2534_READ_TIMEOUT_MS);
            update_health(t, health, backoff_ms, quiet_runs, write_failed,
                          o.status, o.got, o.matched);

            if (backoff_sleep(t, backoff_ms)) {
                break;
            }
            if (backoff_ms > 0) {
                backoff_ms = j2534_backoff_next(backoff_ms);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    J2534_REQUEST_CYCLE_MS));
            }
        } else {
            /* PASSIVE: 100 ms read, then a 20 ms sleep. */
            read_outcome o = pump_one(t, J2534_READ_TIMEOUT_MS);
            update_health(t, health, backoff_ms, quiet_runs, write_failed,
                          o.status, o.got, o.matched);

            if (backoff_sleep(t, backoff_ms)) {
                break;
            }
            if (backoff_ms > 0) {
                backoff_ms = j2534_backoff_next(backoff_ms);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    J2534_POLL_INTERVAL_MS));
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
        std::numeric_limits<unsigned long>::max();
    const unsigned long base = hex ? 16ul : 10ul;
    unsigned long value = 0;
    for (; i < len; ++i) {
        unsigned long digit;
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned long>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned long>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned long>(c - 'A' + 10);
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

/* URI query parser. Contract (strict — malformed means NO_TRANSPORT,
 * surfaces at session_create before any DLL load):
 *   - lowercase keys only:
 *       baud   decimal, nonzero
 *       flags  0x-hex or decimal; 0 legal
 *       filter CAN id 1..0x1FFFFFFF — receive ID; replaces the
 *              profile's channels (registers a FLOW_CONTROL filter)
 *       txid   CAN id 1..0x1FFFFFFF — transmit ID; default is the
 *              profile's tx, else 0x7E0
 *       poll   PID 1..0xFF — single-PID Mode-01 loop; overrides the
 *              profile's rotation
 *       profile core's key: ecuio_session_create has validated it
 *              before attach
 *   - unknown keys, and the shared grammar's malformed forms (missing
 *     '=', empty segments, a trailing '&', a bare trailing '?';
 *     uri_query.h), are all rejected
 *   - duplicate keys: last occurrence wins
 *   - poll needs a channel to receive on, but the channel may come
 *     from the profile: ?filter= supplies one directly, otherwise the
 *     profile's declared channels do. Enforced in j2534_attach, where
 *     both are known. */
ecuio_status_t parse_query(const char* query, j2534_uri_params* p)
{
    /* What a refused key should have held; nullptr for an unknown key. */
    const char* wanted = nullptr;
    const ecuio_uri::param* refused = nullptr;
    ecuio_uri::param last = {};
    const bool ok = ecuio_uri::for_each_param(query, [&](const ecuio_uri::param& q) {
        unsigned long v = 0;
        last = q;
        refused = &last;
        if (q.is("baud")) {
            wanted = "a nonzero decimal";
            if (!parse_number(q.val, q.val_len, &v) || v == 0) {
                return false;
            }
            p->baud = v;
        } else if (q.is("txid")) {
            wanted = "a CAN ID from 1 to 0x1FFFFFFF";
            if (!parse_number(q.val, q.val_len, &v) ||
                v == 0 || v > J2534_CAN_ID_MAX) {
                return false;
            }
            p->tx_id = v;
        } else if (q.is("profile")) {
            /* ADR-0013: the profile NAME rides in the URI, but
             * selection/validation is core's — it runs in
             * ecuio_session_create BEFORE this parser and before any
             * DLL load, so an unknown or malformed name never gets
             * here. */
        } else if (q.is("poll")) {
            wanted = "a PID from 0x00 to 0xFF";
            if (!parse_number(q.val, q.val_len, &v) || v > J2534_PID_MAX) {
                return false;
            }
            p->poll_pid = v;
            p->has_poll = true;
        } else if (q.is("flags")) {
            wanted = "a decimal or 0x-hex number";
            if (!parse_number(q.val, q.val_len, &v)) {
                return false;
            }
            p->flags = v;
        } else if (q.is("filter")) {
            wanted = "a CAN ID from 1 to 0x1FFFFFFF";
            if (!parse_number(q.val, q.val_len, &v) ||
                v == 0 || v > J2534_CAN_ID_MAX) {
                return false;
            }
            p->rx_id = v;
        } else {
            wanted = nullptr;
            return false;   /* unknown parameter */
        }
        refused = nullptr;
        return true;
    });
    if (!ok) {
        if (refused == nullptr) {
            ecuio_detail_set("malformed query: expected key=value pairs "
                             "joined by '&'");
        } else if (wanted == nullptr) {
            ecuio_detail_set("j2534:// takes no query key '%.*s'",
                             static_cast<int>(refused->key_len), refused->key);
        } else {
            const int shown = static_cast<int>(
                (refused->val_len < 32) ? refused->val_len : 32);
            ecuio_detail_set("?%.*s= needs %s, not '%.*s'",
                             static_cast<int>(refused->key_len), refused->key,
                             wanted, shown, refused->val);
        }
        return ECUIO_ERR_BAD_URI;
    }

    /* The poll-needs-a-channel rule lives in j2534_attach, not here:
     * this parser can't see the profile's declared channels, and
     * rejecting ?poll= without ?filter= here would refuse a profile
     * that declares its own (?profile=obd2&poll=0x0C). Syntax is this
     * function's job; coupling is attach's — and attach still fires
     * before any DLL load, so malformed URIs stay fail-fast. */
    return ECUIO_OK;
}

/* Resolve a j2534:// URI to a concrete dll_path + connect/adoption
 * params. Grammar (see internal.h): [selector][?query]. The query is
 * split off and parsed before the selector is resolved, so it never
 * reaches LoadLibraryEx or the vendor-name match. Selector: empty =
 * first device; path-like = direct load; else vendor-name match. All
 * misses are ECUIO_ERR_NO_TRANSPORT. */
ecuio_status_t resolve_uri(const char* uri, ecuio_j2534_device_t* dev,
                           j2534_uri_params* params)
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

    if (q != nullptr) {
        ecuio_status_t st = parse_query(q + 1, params);
        if (st != ECUIO_OK) {
            return st;   /* fail fast — no DLL load on malformed URIs */
        }
    }

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
     * Reaches LoadLibraryEx verbatim (discover-not-involved). */
    if (strchr(selector, '\\') != nullptr ||
        strchr(selector, '/') != nullptr ||
        (strlen(selector) >= 4 &&
         _stricmp(selector + strlen(selector) - 4, ".dll") == 0)) {
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
 * stop best-effort before disconnect, then free impl AND the wrapper. */
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

    ecuio_transport_t* wrap = t->self;
    delete t;      /* impl first */
    delete wrap;   /* then the envelope — internal.h contract */
}

/* Ops table: static storage owned by this TU (internal.h contract),
 * defined before attach references it. */
const ecuio_transport_ops_t kJ2534Ops = {
    j2534_start,
    j2534_detach
};

ecuio_status_t j2534_attach(ecuio_session_t* session, const char* uri)
{
    /* Idempotence (internal.h contract, mock parity): checked FIRST,
     * before any device open, so the early-out leaks nothing. */
    if (session->transport != nullptr) {
        return ECUIO_OK;
    }

    ecuio_j2534_device_t dev = {};
    j2534_uri_params params = {};
    ecuio_status_t st = resolve_uri(uri, &dev, &params);
    if (st != ECUIO_OK) {
        return st;
    }

    /* Channels and scheduler are resolved before the device opens.
     * core.cpp installs the profile's set before calling attach. */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);

    auto* t = new (std::nothrow) ecuio_j2534_transport();
    if (t == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    t->set = set;
    resolve_channels(t, set, params);

    /* Request mode, fixed before the thread starts:
     *   ?poll=    -> single-PID loop; the set's rotation is NOT built,
     *                so the PID in the URI is the only one requested
     *   set       -> the set's full rotation
     *   neither   -> passive receive
     * The URI outranks the profile: building the scheduler anyway
     * would make the worker's scheduler branch win and silently drop
     * the ?poll= PID. */
    const bool sched_mode = !params.has_poll
                            && ecuio_sched_from_set(&t->sched, set);

    /* Coupling check: a request loop needs at least one channel to
     * receive on, or it writes against guaranteed silence forever (an
     * ISO 15765 channel with no filter receives nothing). Scheduled
     * PIDs get theirs from the set; ?poll= may take ?filter= or the
     * profile's declared channels. Shipped profiles declare channels,
     * so the scheduled arm is reachable only by a future set author
     * omitting them. Fires before any hardware is touched. */
    if ((sched_mode || params.has_poll) && t->chan_count == 0) {
        delete t;
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
        delete t;
        ecuio_detail_set("the channels mix 11-bit and 29-bit IDs, which "
                         "one connection can't carry");
        return ECUIO_ERR_BAD_URI;
    }
    const unsigned long connect_flags =
        params.flags | ((wide_chans != 0) ? ECUIO_PT_CAN_29BIT_ID : 0UL);

    ecuio_j2534_handle_t* handle = nullptr;
    st = ecuio_j2534_open(&dev, &handle);
    if (st != ECUIO_OK) {
        delete t;
        return st;
    }

    unsigned long channel = 0;
    st = ecuio_j2534_connect_channel(handle, params.baud, connect_flags,
                                     &channel);
    if (st != ECUIO_OK) {
        ecuio_j2534_close(handle);
        delete t;
        return st;
    }

    /* The envelope: session owns it polymorphically; detach frees both. */
    auto* wrap = new (std::nothrow) ecuio_transport_t();
    if (wrap == nullptr) {
        ecuio_j2534_disconnect_channel(handle, channel);
        ecuio_j2534_close(handle);
        delete t;
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    wrap->ops = &kJ2534Ops;
    wrap->impl = t;

    t->session = session;
    t->handle = handle;
    t->channel = channel;
    t->self = wrap;
    t->baud = params.baud;
    t->flags = connect_flags;
    t->has_poll = params.has_poll;
    t->poll_pid = static_cast<uint32_t>(params.poll_pid);

    session->transport = wrap;   /* session.h member — direct install */
    return ECUIO_OK;
}

} // namespace

extern "C" ecuio_status_t ecuio_j2534_transport_attach(
    ecuio_session_t* session, const char* uri)
{
    return j2534_attach(session, uri);
}

#else /* !_WIN32 */

/* Linking stub — core.cpp routes j2534:// only on _WIN32, but the
 * symbol must exist for the headless-Unix CI build (core links
 * without Win32 J2534). */
extern "C" ecuio_status_t ecuio_j2534_transport_attach(
    ecuio_session_t* session, const char* uri)
{
    (void)session;
    (void)uri;
    return ECUIO_ERR_NO_TRANSPORT;
}

#endif /* _WIN32 */
