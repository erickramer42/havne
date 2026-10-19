// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — test_j2534_stream drives the full
//          InjectFrame -> ReadMsgs -> callback pipeline on a worker thread,
//          asserts ?baud=/?flags= values reach PassThruConnect, and the
//          ?filter=/txid=/poll= request loop: write framing recorded by the
//          fake, response received, 4-byte header stripped, arbitration_id
//          stamped, filter stopped on detach. ADR-0013 slice 2 adds the
//          scheduler + drain path (multi-PID round-robin, #25 receive cap)
//          — covered end-to-end by the Phase 5 obd2 harness tests.
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there

#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"
#include "qttune/j2534.h"
#include "pid_scheduler.h"
#include "qttune/profile.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32

#include "qttune/j2534_ptypes.h"   /* FLOW_CONTROL_FILTER, CAN_29BIT_ID,
                                      TX_ISO15765_FRAME_PAD (N1) */

namespace {

constexpr uint32_t J2534_POLL_INTERVAL_MS   = 20;
constexpr uint32_t J2534_READ_TIMEOUT_MS    = 100;
constexpr uint32_t J2534_REQUEST_CYCLE_MS   = 100;   /* poll cadence */

/* ADR-0013 slice 2 (#25): scheduler-mode constants. The scheduler
 * paces requests (one per 100 ms cycle, rotating PIDs); the drain
 * paces receives. DRAIN_BUDGET caps reads at 10 per cycle, so the
 * worker cannot exceed 100 reads/s: the cycle is never shorter than
 * the 100 ms deadline, making 10 reads per cycle a hard ceiling
 * regardless of bus load. (Dropped frames count against the budget
 * too — it bounds loop time, not delivery.) The normal case is one
 * response per request: about 10 reads/s, with the v1 set's 8 PIDs
 * each refreshing every 800 ms. DRAIN_READ_TIMEOUT is deliberately
 * SHORT: on a real device ReadMsgs blocks up to the timeout when
 * the queue is empty, so draining with the legacy 100 ms timeout
 * would spend the whole cycle waiting for silence. 20 ms
 * accommodates slow ECUs answering the request we just sent while
 * keeping a quiet-drain bounded. First EMPTY read ends the drain —
 * the budget exists for bursts, not for silence. Both are BENCH
 * candidates pending VX observation. */
constexpr uint32_t J2534_DRAIN_BUDGET          = 10;
constexpr uint32_t J2534_DRAIN_READ_TIMEOUT_MS  = 20;

/* Address/filter field widths. The 4-byte big-endian CAN ID is the
 * J2534 convention for ISO 15765 filter patterns and in-band
 * addressing bytes.
 * N1 FIX (2026-10-19): FILTER_MSG_LEN is 4, not 8. On an ISO 15765
 * channel the mask/pattern/flow-control filter messages are BARE
 * 4-byte CAN IDs — the adapter's ISO-TP stack does the framing, so
 * no PCI byte belongs in the flow-control message either. The old
 * 8-byte shape carried a hand-built [30 00 00] CTS/BS/STmin triple
 * that a conforming adapter would have treated as address bytes.
 * SAE J2534-1 additionally requires all three filter messages to
 * share the same DataSize and TxFlags — satisfied trivially here.
 * This also retires the old BENCH-prep item 1 (the 8-byte shape
 * and its equality-rule concern): the spec shape has no such
 * tension. VERIFIED: Opus IVS "Constructing an OBDII Message"
 * (4-byte mask/pattern/FC), Quantex pt_start_msgfilt docs, dashlogic
 * J2534_v0404.h. */
constexpr uint32_t J2534_ADDR_LEN       = 4;
constexpr uint32_t J2534_FILTER_MSG_LEN = 4;
constexpr uint32_t J2534_TX_ID_DEFAULT  = 0x7E0;  /* OBD-II physical */
constexpr uint32_t J2534_CAN_ID_MAX     = 0x1FFFFFFFu;   /* 29-bit */
constexpr uint32_t J2534_PID_MAX        = 0xFFu;

void pack_be32(uint8_t* out, uint32_t v)
{
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

/* Classic OBD-II Mode-01 request on an ISO 15765 channel: the
 * 4-byte normal-addressing ID followed by the two logical request
 * bytes [Mode 01][PID] — 6 bytes total, TxFlags =
 * ISO15765_FRAME_PAD so the ADAPTER frames and zero-pads.
 *
 * N1 FIX (2026-10-19): on an ISO 15765 channel the adapter owns the
 * ISO-TP layer. PassThruWriteMsgs takes the LOGICAL message; the
 * SF PCI byte and 8-byte padding are the driver's job (padding is
 * controlled by the ISO15765_FRAME_PAD TxFlag). The previous shape
 * hand-built [ID][02][01][PID][pad-to-8] — a 12-byte write a
 * conforming adapter would transmit as a two-frame ISO-TP message
 * whose service byte reads as Mode 02 (freeze frame), and whose
 * padding duplicated the adapter's own (FRAME_PAD was never set).
 * VERIFIED against Opus IVS ("Constructing an OBDII Message":
 * RPM request is Data=[ID][01][0C], DataSize=6), Quantex
 * PassThruWriteMsgs docs (writes are [ID][SID][...] with
 * ISO15765_FRAME_PAD, no PCI), and dashlogic J2534_v0404.h
 * (ISO15765_FRAME_PAD = 0x0040).
 *
 * NO CHECKSUM BYTE: ISO 15765-4 over CAN has no data-field checksum
 * (the CAN CRC is hardware-handled); a K-line-style sum would be a
 * spurious byte to a real ECU.
 *
 * BENCH-remaining assumption: that the VX honors FRAME_PAD
 * zero-padding (the ISO 15765-4 convention) rather than demanding
 * unpadded transmissions — the flag is the documented knob either
 * way. */
void build_mode01_request(uint8_t out[6], uint32_t tx_id, uint32_t pid)
{
    pack_be32(out, tx_id);
    out[4] = 0x01;   /* Mode 01 */
    out[5] = static_cast<uint8_t>(pid);
}

struct qttune_j2534_transport {
    qttune_session_t* session = nullptr;
    qttune_j2534_handle_t* handle = nullptr;
    unsigned long channel = 0;
    /* Connect parameters from the URI query. baud == 0 is the
     * "unset" sentinel — the URI parser rejects an explicit baud=0 —
     * and j2534_loader's connect_channel substitutes the ISO 15765
     * default. flags pass through verbatim: 0 is both the default and
     * a legitimate value. Semantics of individual flag bits are NOT
     * interpreted here (BENCH tier, pending the vendor header diff). */
    unsigned long baud = 0;
    unsigned long flags = 0;
    /* Adoption parameters (all frozen before the worker starts, so
     * the worker reads them lock-free and they need no atomics):
     *   rx_id        0 = no filter registered (legacy verbatim-read
     *                mode); nonzero = FLOW_CONTROL filter registered
     *                on this ID and received frames header-stripped
     *   tx_id        transmit ID; defaults to 0x7E0 when the URI omits
     *   poll_pid     0 = passive receive (identity stamping only);
     *                nonzero = Mode-01 request loop for this PID
     *   addr_flags   CAN_29BIT_ID when either ID exceeds 11 bits;
     *                ORed with ISO15765_FRAME_PAD at every write and
     *                filter registration (N1)
     *   filter_active/filter_id  set at start; stopped BEST-EFFORT at
     *                detach BEFORE disconnect — vendor-compatibility
     *                priority (some drivers leak filter state if the
     *                stop is skipped; the spec says disconnect
     *                invalidates filters, so a failure here is benign)
     *   sched        ADR-0013 slice 2: round-robin PID scheduler,
     *                populated at j2534_start from the session's
     *                active_signal_set. NONEMPTY scheduler mode
     *                outranks ?poll= — a set that drives scheduling
     *                owns the request loop, and ?poll= remains the
     *                raw-profile debugging override. Frozen before the
     *                worker exists, hence lock-free on the worker. */
    uint32_t rx_id = 0;
    uint32_t tx_id = J2534_TX_ID_DEFAULT;
    uint32_t poll_pid = 0;
    uint32_t addr_flags = 0;
    bool filter_active = false;
    unsigned long filter_id = 0;
    qttune_pid_scheduler sched;
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    qttune_transport_t* self = nullptr;   /* NEW: wrapper back-pointer —
                                           * detach must free impl AND the
                                           * envelope (internal.h contract) */
};

/* Read one frame and, if it passes the strip/stamp gate, dispatch it
 * through the session snapshot path. Returns whether a frame was
 * read (dispatched or not).
 *
 * RECEIVE FRAMING: j2534_loader's read_frame copies PASSTHRU_MSG.Data
 * verbatim — ISO 15765 rides its 4-byte address header in-band. With
 * a filter registered (rx_id != 0) the worker verifies that header
 * against the filter ID (defensive: a conforming vendor delivers
 * matching frames only, so a mismatch or a runt frame is dropped, not
 * dispatched), strips the 4 bytes, and stamps arbitration_id — which
 * is what closes the bridge's message-ID gate (Issue #15). Without a
 * filter, frames dispatch verbatim with id 0: byte-for-byte the
 * pre-adoption contract, which is why every existing stream test
 * still passes unmodified.
 * N1: the post-strip payload is the ISO-TP LOGICAL data —
 * [0x41][PID][A][B]... with no PCI byte, matching the obd2 signal
 * set's mux-at-offset-0 grammar. */
bool pump_one(qttune_j2534_transport* t, uint32_t timeout_ms)
{
    qttune_frame_t frame{};
    bool got = false;
    qttune_status_t st = qttune_j2534_read_frame(t->handle, t->channel,
                                                 &frame, timeout_ms, &got);
    if (st != QT_OK || !got) {
        return false;
    }
    if (t->rx_id != 0) {
        if (frame.data_length >= J2534_ADDR_LEN) {
            const uint32_t hdr =
                (static_cast<uint32_t>(frame.data[0]) << 24) |
                (static_cast<uint32_t>(frame.data[1]) << 16) |
                (static_cast<uint32_t>(frame.data[2]) << 8) |
                 static_cast<uint32_t>(frame.data[3]);
            if ((hdr & J2534_CAN_ID_MAX) == t->rx_id) {
                frame.arbitration_id = t->rx_id;
                const uint16_t n = static_cast<uint16_t>(
                    frame.data_length - J2534_ADDR_LEN);
                memmove(frame.data, frame.data + J2534_ADDR_LEN, n);
                frame.data_length = n;
                qttune_session_dispatch_callbacks(t->session, &frame);
            }
        }
    } else {
        qttune_session_dispatch_callbacks(t->session, &frame);
        /* no filter — verbatim, id 0 */
    }
    return true;
}

/* Worker entrypoint — mirrors mock_worker's discipline: impl-owned
 * state, dispatch through the session snapshot path, stop via atom.
 *
 * Three modes, selected by adoption state (all frozen pre-thread):
 *   SCHEDULER (ADR-0013 slice 2): sched.count > 0. Per 100 ms cycle:
 *     next PID -> build Mode-01 request -> write -> DRAIN up to
 *     J2534_DRAIN_BUDGET frames (short timeout, stop at first empty)
 *     -> sleep the cycle REMAINDER against an absolute deadline, so
 *     drain cost does not accumulate as cadence drift.
 *   POLL (legacy): poll_pid != 0. One request, one read (100 ms
 *     timeout), 100 ms sleep — request shape is the N1 6-byte wire
 *     form shared with the scheduler.
 *   PASSIVE (legacy): read one frame per 20 ms, no requests. */
void j2534_worker(qttune_j2534_transport* t)
{
    const bool sched_mode = (t->sched.count > 0);
    const uint32_t write_flags = t->addr_flags |
                                 QTTUNE_PT_TX_ISO15765_FRAME_PAD;

    while (!t->stop_requested.load()) {
        const auto cycle_start = std::chrono::steady_clock::now();

        if (sched_mode) {
            uint8_t pid = 0;
            (void)qttune_sched_next(&t->sched, &pid);
            uint8_t req[6];
            build_mode01_request(req, t->tx_id, pid);
            uint32_t written = 0;
            /* A failed send retries next cycle — the loop's job is
             * resilience. Persistent failure presents as a silent bus,
             * which is the BENCH-tier diagnostic story, not a
             * transport-layer error to synthesize here. */
            (void)qttune_j2534_write_frame(t->handle, t->channel, req,
                                           sizeof(req), write_flags,
                                           0, &written);

            /* DRAIN: budgeted receive (#25). First empty read ends the
             * drain; the budget bounds bursts, not silence. Each
             * dispatched frame counts against the budget; dropped
             * (non-matching/runt) frames do too — the point is
             * bounding worker loop time, not guaranteed delivery. */
            for (uint32_t i = 0; i < J2534_DRAIN_BUDGET; ++i) {
                if (!pump_one(t, J2534_DRAIN_READ_TIMEOUT_MS)) {
                    break;
                }
            }

            /* Absolute-deadline sleep: 100 ms from cycle START, not
             * from drain END — accumulated drain cost cannot drift
             * the request cadence. A negative remainder (drain
             * somehow exceeded the cycle) clamps to zero. */
            const auto target = cycle_start +
                std::chrono::milliseconds(J2534_REQUEST_CYCLE_MS);
            const auto now = std::chrono::steady_clock::now();
            if (target > now) {
                std::this_thread::sleep_until(target);
            }
        } else if (t->poll_pid != 0) {
            uint8_t req[6];
            build_mode01_request(req, t->tx_id, t->poll_pid);
            uint32_t written = 0;
            (void)qttune_j2534_write_frame(t->handle, t->channel, req,
                                           sizeof(req), write_flags,
                                           0, &written);
            (void)pump_one(t, J2534_READ_TIMEOUT_MS);
            /* With polling, the request cycle paces the loop. The fake's
             * reads return instantly; a real device blocks inside ReadMsgs
             * up to the timeout, so the effective cadence is the larger of
             * the two either way. */
            std::this_thread::sleep_for(std::chrono::milliseconds(
                J2534_REQUEST_CYCLE_MS));
        } else {
            (void)pump_one(t, J2534_READ_TIMEOUT_MS);
            std::this_thread::sleep_for(std::chrono::milliseconds(
                J2534_POLL_INTERVAL_MS));
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

/* Collected URI query parameters — parse_query fills, resolve_uri
 * passes through, attach stores into the transport impl. One struct
 * rather than a six-pointer parse signature, and one place for the
 * coupling rule to live. */
struct j2534_uri_params {
    unsigned long baud = 0;
    unsigned long flags = 0;
    unsigned long rx_id = 0;     /* 0 = no filter (verbatim read mode) */
    unsigned long tx_id = 0;     /* 0 = unset; attach substitutes 0x7E0 */
    unsigned long poll_pid = 0;  /* 0 = passive receive */
};

/* URI query parser. Contract (strict — malformed means NO_TRANSPORT,
 * surfaces at session_create before any DLL load):
 *   - lowercase keys only:
 *       baud   decimal, nonzero
 *       flags  0x-hex or decimal; 0 legal
 *       filter CAN id 1..0x1FFFFFFF — receive ID (registers a
 *              FLOW_CONTROL filter; the ISO 15765 receive prerequisite)
 *       txid   CAN id 1..0x1FFFFFFF — transmit ID, default 0x7E0
 *       poll   PID 1..0xFF — Mode-01 request loop
 *   - unknown keys, missing '=', empty segments, trailing '&', and a
 *     bare trailing '?' on the URI are all rejected
 *   - duplicate keys: last occurrence wins
 *   - COUPLING: poll without filter is rejected. Per J2534-1 v4.04 an
 *     ISO 15765 channel receives NOTHING without a registered filter,
 *     so a poll loop would spin against guaranteed silence — a
 *     guaranteed-silent session is a delayed failure, not a feature.
 *     Fail fast at parse, like every other malformed URI. */
qttune_status_t parse_query(const char* query, j2534_uri_params* p)
{
    const char* cur = query;
    for (;;) {
        const char* amp = strchr(cur, '&');
        const char* end = (amp != nullptr) ? amp : cur + strlen(cur);

        const char* eq = static_cast<const char*>(
            memchr(cur, '=', static_cast<size_t>(end - cur)));
        if (eq == nullptr) {
            return QT_ERR_NO_TRANSPORT;   /* no '=' in segment */
        }

        const size_t key_len = static_cast<size_t>(eq - cur);
        const char* val = eq + 1;
        const size_t val_len = static_cast<size_t>(end - val);

        if (key_len == 4 && strncmp(cur, "baud", 4) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v) || v == 0) {
                return QT_ERR_NO_TRANSPORT;
            }
            p->baud = v;
        } else if (key_len == 4 && strncmp(cur, "txid", 4) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v) ||
                v == 0 || v > J2534_CAN_ID_MAX) {
                return QT_ERR_NO_TRANSPORT;
            }
            p->tx_id = v;
        } else if (key_len == 7 && strncmp(cur, "profile", 7) == 0) {
            /* ADR-0013: the profile NAME rides in the URI, but
             * selection/validation is core's — it runs in
             * qttune_session_create BEFORE this parser and before any
             * DLL load. Accept any nonempty value (<= 63 chars);
             * unknown names have already failed session_create. */
            if (val_len == 0 || val_len > 63) {
                return QT_ERR_NO_TRANSPORT;
            }
        } else if (key_len == 4 && strncmp(cur, "poll", 4) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v) ||
                v == 0 || v > J2534_PID_MAX) {
                return QT_ERR_NO_TRANSPORT;
            }
            p->poll_pid = v;
        } else if (key_len == 5 && strncmp(cur, "flags", 5) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v)) {
                return QT_ERR_NO_TRANSPORT;
            }
            p->flags = v;
        } else if (key_len == 6 && strncmp(cur, "filter", 6) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v) ||
                v == 0 || v > J2534_CAN_ID_MAX) {
                return QT_ERR_NO_TRANSPORT;
            }
            p->rx_id = v;
        } else {
            return QT_ERR_NO_TRANSPORT;   /* unknown parameter */
        }

        if (*end == '\0') {
            break;   /* end of query — fall through to the coupling rule */
        }
        cur = end + 1;
        if (*cur == '\0') {
            return QT_ERR_NO_TRANSPORT;   /* trailing '&' */
        }
    }

    if (p->poll_pid != 0 && p->rx_id == 0) {
        return QT_ERR_NO_TRANSPORT;   /* poll without filter */
    }
    return QT_OK;
}

/* Resolve a j2534:// URI to a concrete dll_path + connect/adoption
 * params. Grammar (see internal.h): [selector][?query]. The query is
 * split off and parsed BEFORE selector resolution — previously a query
 * glued onto a path reached LoadLibraryEx as part of the filename, and
 * a query on a vendor name defeated the discovery match. Selector
 * routing itself is unchanged: empty = first device; path-like =
 * direct; else vendor-name match. All misses are QT_ERR_NO_TRANSPORT. */
qttune_status_t resolve_uri(const char* uri, qttune_j2534_device_t* dev,
                            j2534_uri_params* params)
{
    const char* rest = uri + 8;   /* core.cpp verified the prefix */

    const char* q = strchr(rest, '?');
    if (q != nullptr && *(q + 1) == '\0') {
        return QT_ERR_NO_TRANSPORT;   /* bare trailing '?' */
    }

    char selector[QTTUNE_J2534_DLL_PATH_LEN];
    const size_t n = (q != nullptr)
                     ? static_cast<size_t>(q - rest)
                     : strlen(rest);
    if (n >= sizeof(selector)) {
        return QT_ERR_NO_TRANSPORT;   /* longer than any legal selector */
    }
    memcpy(selector, rest, n);
    selector[n] = '\0';

    if (q != nullptr) {
        qttune_status_t st = parse_query(q + 1, params);
        if (st != QT_OK) {
            return st;   /* fail fast — no DLL load on malformed URIs */
        }
    }

    qttune_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    if (selector[0] == '\0') {
        qttune_status_t st = qttune_j2534_discover_devices(&devices, &count);
        if (st != QT_OK) {
            return st;
        }
        if (count == 0) {
            return QT_ERR_NO_TRANSPORT;   /* no vendors installed */
        }
        *dev = devices[0];   /* first device — documented default */
        qttune_j2534_free_devices(devices);
        return QT_OK;
    }

    /* Path-like selector: contains a separator or a .dll suffix.
     * Reaches LoadLibraryEx verbatim (discover-not-involved). */
    if (strchr(selector, '\\') != nullptr ||
        strchr(selector, '/') != nullptr ||
        (strlen(selector) >= 4 &&
         _stricmp(selector + strlen(selector) - 4, ".dll") == 0)) {
        if (strlen(selector) >= QTTUNE_J2534_DLL_PATH_LEN) {
            return QT_ERR_NO_TRANSPORT;
        }
        memcpy(dev->dll_path, selector, strlen(selector) + 1);
        return QT_OK;
    }

    /* Otherwise: vendor-name match against discovery, case-insensitive. */
    qttune_status_t st = qttune_j2534_discover_devices(&devices, &count);
    if (st != QT_OK) {
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
    qttune_j2534_free_devices(devices);
    if (!matched) {
        return QT_ERR_NO_TRANSPORT;
    }
    return QT_OK;
}

qttune_status_t j2534_start(qttune_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<qttune_j2534_transport*>(impl);
    if (t->worker.joinable()) {
        return QT_OK;   /* internal.h: second start on a live
                         * transport returns QT_OK */
    }

    /* ORDERING NOTE (N3, open by design — channel-decl slice owns the
     * fix): the scheduler-without-filter coupling fires HERE, after
     * attach already opened the device. ADR-0013 §4 channel-decls
     * supply 7E0/7E8 from the profile and demote this to a backstop. */
    const QttuneSignalSet* set = qttune_session_signal_set(t->session);
    const bool sched_mode = qttune_sched_from_set(&t->sched, set);
    if (sched_mode && t->rx_id == 0) {
        return QT_ERR_NO_TRANSPORT;   /* scheduled PIDs need ?filter= */
    }

    if (t->rx_id != 0) {
        uint8_t mask[J2534_FILTER_MSG_LEN];
        uint8_t pattern[J2534_FILTER_MSG_LEN];
        uint8_t fc[J2534_FILTER_MSG_LEN];
        pack_be32(mask, 0xFFFFFFFFu);   /* all-ones over all 4 bytes: exact ID
                                        * match; the pattern's leading zeros
                                        * carry the address-width distinction */
        pack_be32(pattern, t->rx_id);
        pack_be32(fc, t->tx_id);

        qttune_status_t st = qttune_j2534_start_msg_filter(
            t->handle, t->channel,
            QTTUNE_PT_FLOW_CONTROL_FILTER,
            mask, J2534_FILTER_MSG_LEN,
            pattern, J2534_FILTER_MSG_LEN,
            fc, J2534_FILTER_MSG_LEN,
            t->addr_flags | QTTUNE_PT_TX_ISO15765_FRAME_PAD,
            &t->filter_id);
        if (st != QT_OK) {
            return st;
        }
        t->filter_active = true;
    }

    t->stop_requested.store(false);
    try {
        t->worker = std::thread(j2534_worker, t);
    } catch (...) {
        if (t->filter_active) {
            (void)qttune_j2534_stop_msg_filter(t->handle, t->channel,
                                               t->filter_id);
            t->filter_active = false;
        }
        return QT_ERR_OUT_OF_MEMORY;
    }
    return QT_OK;
}

/* detach(session, impl) — void per internal.h, called exactly once from
 * qttune_session_close AFTER the closed flag is set. Worker joined
 * FIRST (never alive while vendor code could be unmapped), filter
 * stop best-effort before disconnect, then free impl AND the wrapper. */
void j2534_detach(qttune_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<qttune_j2534_transport*>(impl);
    if (t->worker.joinable()) {
        t->stop_requested.store(true);
        t->worker.join();
    }
    if (t->filter_active) {
        (void)qttune_j2534_stop_msg_filter(t->handle, t->channel,
                                           t->filter_id);
        t->filter_active = false;
    }
    (void)qttune_j2534_disconnect_channel(t->handle, t->channel);
    (void)qttune_j2534_close(t->handle);

    qttune_transport_t* wrap = t->self;
    delete t;      /* impl first */
    delete wrap;   /* then the envelope — internal.h contract */
}

/* Ops table: static storage owned by this TU (internal.h contract),
 * defined before attach references it. */
const qttune_transport_ops_t kJ2534Ops = {
    j2534_start,
    j2534_detach
};

qttune_status_t j2534_attach(qttune_session_t* session, const char* uri)
{
    /* Idempotence (internal.h contract, mock parity): checked FIRST,
     * before any device open, so the early-out leaks nothing. */
    if (session->transport != nullptr) {
        return QT_OK;
    }

    qttune_j2534_device_t dev = {};
    j2534_uri_params params = {};
    qttune_status_t st = resolve_uri(uri, &dev, &params);
    if (st != QT_OK) {
        return st;
    }

    qttune_j2534_handle_t* handle = nullptr;
    st = qttune_j2534_open(&dev, &handle);
    if (st != QT_OK) {
        return st;
    }

    auto* t = new (std::nothrow) qttune_j2534_transport();
    if (t == nullptr) {
        qttune_j2534_close(handle);
        return QT_ERR_OUT_OF_MEMORY;
    }

    unsigned long channel = 0;
    st = qttune_j2534_connect_channel(handle, params.baud, params.flags,
                                      &channel);
    if (st != QT_OK) {
        qttune_j2534_close(handle);
        delete t;
        return st;
    }

    /* The envelope: session owns it polymorphically; detach frees both. */
    auto* wrap = new (std::nothrow) qttune_transport_t();
    if (wrap == nullptr) {
        qttune_j2534_disconnect_channel(handle, channel);
        qttune_j2534_close(handle);
        delete t;
        return QT_ERR_OUT_OF_MEMORY;
    }
    wrap->ops = &kJ2534Ops;
    wrap->impl = t;

    t->session = session;
    t->handle = handle;
    t->channel = channel;
    t->self = wrap;
    t->baud = params.baud;
    t->flags = params.flags;
    t->rx_id = static_cast<uint32_t>(params.rx_id);
    t->tx_id = (params.tx_id != 0) ? static_cast<uint32_t>(params.tx_id)
                                   : J2534_TX_ID_DEFAULT;
    t->poll_pid = static_cast<uint32_t>(params.poll_pid);
    if (t->rx_id > 0x7FF || t->tx_id > 0x7FF) {
        t->addr_flags = QTTUNE_PT_CAN_29BIT_ID;
    }

    session->transport = wrap;   /* session.h member — direct install */
    return QT_OK;
}

} // namespace

extern "C" qttune_status_t qttune_j2534_transport_attach(
    qttune_session_t* session, const char* uri)
{
    return j2534_attach(session, uri);
}

#else /* !_WIN32 */

/* Linking stub — core.cpp routes j2534:// only on _WIN32, but the
 * symbol must exist for the headless-Unix CI build (core links
 * without Win32 J2534). */
extern "C" qttune_status_t qttune_j2534_transport_attach(
    qttune_session_t* session, const char* uri)
{
    (void)session;
    (void)uri;
    return QT_ERR_NO_TRANSPORT;
}

#endif /* _WIN32 */
