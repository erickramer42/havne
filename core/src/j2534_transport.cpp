// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — test_j2534_stream drives the full
//          InjectFrame -> ReadMsgs -> callback pipeline on a worker thread,
//          asserts ?baud=/?flags= values reach PassThruConnect, and the
//          ?filter=/txid=/poll= request loop: write framing recorded by the
//          fake, response received, 4-byte header stripped, arbitration_id
//          stamped, filter stopped on detach.
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there

#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"
#include "qttune/j2534.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32

#include "qttune/j2534_ptypes.h"   /* FLOW_CONTROL_FILTER, CAN_29BIT_ID */

namespace {

constexpr uint32_t J2534_POLL_INTERVAL_MS   = 20;
constexpr uint32_t J2534_READ_TIMEOUT_MS    = 100;
constexpr uint32_t J2534_REQUEST_CYCLE_MS   = 100;   /* poll cadence */

/* Address/filter field widths. 4-byte big-endian CAN ID is the J2534
 * convention for both filter patterns and ISO 15765 addressing bytes.
 * FILTER_MSG_LEN is 8 (not 4) because the Part 4 seam contract
 * requires fc_len == mask_len for FLOW_CONTROL filters, and the flow
 * control message needs its PCI byte: [ID BE][30 00 00][pad]. The
 * trailing zero mask bytes are don't-care, so ID matching is
 * unaffected. SEE the BENCH-prep note in STATUS.md — the equality
 * rule is believed stricter than J2534-1 and is a candidate for
 * relaxation once a real vendor rejects this shape. */
constexpr uint32_t J2534_ADDR_LEN       = 4;
constexpr uint32_t J2534_FILTER_MSG_LEN = 8;
constexpr uint8_t  J2534_FC_PCI_CTS     = 0x30;   /* FlowControl, CTS */
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

/* Classic OBD-II Mode-01 request on an ISO 15765 channel: the 4-byte
 * normal-addressing ID followed by an 8-byte classic CAN data field —
 * SF PCI(02) service(01) PID, zero-padded. 12 wire bytes total.
 *
 * NO CHECKSUM BYTE: ISO 15765-4 over CAN has no data-field checksum
 * (the CAN CRC is hardware-handled). A mod-256 sum here is a legacy
 * K-line/ISO-9141 convention and would be a spurious byte to a real
 * ECU. Padding is zero-filled — some drivers pad themselves, most
 * tolerate caller padding.
 *
 * ASSUMED (first BENCH session on the VX verifies): that the vendor
 * expects caller-supplied ISO-TP framing and 8-byte padding rather
 * than doing either in-driver. */
void build_mode01_request(uint8_t out[12], uint32_t tx_id, uint32_t pid)
{
    pack_be32(out, tx_id);
    out[4] = 0x02;   /* SF PCI — 2 data bytes follow */
    out[5] = 0x01;   /* Mode 01 */
    out[6] = static_cast<uint8_t>(pid);
    out[7] = 0; out[8] = 0; out[9] = 0; out[10] = 0; out[11] = 0;
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
     *   addr_flags   CAN_29BIT_ID when either ID exceeds 11 bits
     *   filter_active/filter_id  set at start; stopped BEST-EFFORT at
     *                detach BEFORE disconnect — vendor-compatibility
     *                priority (some drivers leak filter state if the
     *                stop is skipped; the spec says disconnect
     *                invalidates filters, so a failure here is benign) */
    uint32_t rx_id = 0;
    uint32_t tx_id = J2534_TX_ID_DEFAULT;
    uint32_t poll_pid = 0;
    uint32_t addr_flags = 0;
    bool filter_active = false;
    unsigned long filter_id = 0;
    std::atomic<bool> stop_requested{false};
    std::thread worker;
};

/* Worker entrypoint — mirrors mock_worker's discipline: impl-owned
 * state, dispatch through the session snapshot path, stop via atom.
 *
 * RECEIVE FRAMING: j2534_loader's read_frame copies PASSTHRU_MSG.Data
 * verbatim — ISO 15765 rides its 4-byte address header in-band. With
 * a filter registered (rx_id != 0) the worker verifies that header
 * against the filter ID (defensive: a conforming vendor delivers
 * matching frames only, so a mismatch or a runt frame is dropped, not
 * dispatched), strips the 4 bytes, and stamps arbitration_id — which
 * is what finally closes the bridge's message-ID gate (Issue #15).
 * Without a filter, frames dispatch verbatim with id 0: byte-for-byte
 * the pre-adoption contract, which is why every existing stream test
 * still passes unmodified. */
void j2534_worker(qttune_j2534_transport* t)
{
    while (!t->stop_requested.load()) {
        if (t->poll_pid != 0) {
            uint8_t req[12];
            build_mode01_request(req, t->tx_id, t->poll_pid);
            uint32_t written = 0;
            /* A failed send retries next cycle — the loop's job is
             * resilience. Persistent failure presents as a silent bus,
             * which is the BENCH-tier diagnostic story, not a
             * transport-layer error to synthesize here. */
            (void)qttune_j2534_write_frame(t->handle, t->channel, req,
                                           sizeof(req), t->addr_flags,
                                           0, &written);
        }

        qttune_frame_t frame{};
        bool got = false;
        qttune_status_t st = qttune_j2534_read_frame(t->handle, t->channel,
                                                     &frame,
                                                     J2534_READ_TIMEOUT_MS,
                                                     &got);
        if (st == QT_OK && got) {
            bool dispatch = false;
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
                        dispatch = true;
                    }
                }
            } else {
                dispatch = true;   /* no filter — verbatim, id 0 */
            }
            if (dispatch) {
                qttune_session_dispatch_callbacks(t->session, &frame);
            }
        }

        /* With polling, the request cycle paces the loop. The fake's
         * reads return instantly; a real device blocks inside ReadMsgs
         * up to the timeout, so the effective cadence is the larger of
         * the two either way. Without polling, the legacy 20 ms. */
        std::this_thread::sleep_for(std::chrono::milliseconds(
            (t->poll_pid != 0) ? J2534_REQUEST_CYCLE_MS
                               : J2534_POLL_INTERVAL_MS));
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
            qttune_j2534_free_devices(devices);
            return QT_ERR_NO_TRANSPORT;
        }
        *dev = devices[0];
        qttune_j2534_free_devices(devices);
        return QT_OK;
    }

    const bool looks_like_path =
        n >= 4 && _stricmp(selector + n - 4, ".dll") == 0;
    const bool has_separator =
        strchr(selector, '\\') != nullptr || strchr(selector, '/') != nullptr;

    if (looks_like_path || has_separator) {
        strncpy_s(dev->dll_path, sizeof(dev->dll_path), selector, _TRUNCATE);
        strncpy_s(dev->vendor_name, sizeof(dev->vendor_name), "direct",
                  _TRUNCATE);
        dev->vendor_id = 0;
        return QT_OK;
    }

    /* Vendor-name match, case-insensitive */
    qttune_status_t st = qttune_j2534_discover_devices(&devices, &count);
    if (st != QT_OK) {
        return st;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (_stricmp(devices[i].vendor_name, selector) == 0) {
            *dev = devices[i];
            qttune_j2534_free_devices(devices);
            return QT_OK;
        }
    }
    qttune_j2534_free_devices(devices);
    return QT_ERR_NO_TRANSPORT;
}

qttune_status_t j2534_start(qttune_session_t* /*session*/, void* impl)
{
    qttune_j2534_transport* t =
        static_cast<qttune_j2534_transport*>(impl);
    if (t == nullptr || t->handle == nullptr) {
        return QT_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return QT_OK;   /* already running — idempotent per ops contract */
    }

    unsigned long channel = 0;
    qttune_status_t st = qttune_j2534_connect_channel(t->handle,
                                                      t->baud, t->flags,
                                                      &channel);
    if (st != QT_OK) {
        return QT_ERR_NO_TRANSPORT;
    }
    t->channel = channel;

    /* SPEC ORDER: filters are registered before any traffic flows, so
     * a response can never arrive unfiltered and be discarded by the
     * driver. A registration failure is fatal to the session rather
     * than degraded — an unfiltered ISO 15765 channel receives nothing,
     * so continuing would hand the caller a silently dead session. */
    if (t->rx_id != 0) {
        uint8_t mask[J2534_FILTER_MSG_LEN]    = {0xFF, 0xFF, 0xFF, 0xFF,
                                                 0, 0, 0, 0};
        uint8_t pattern[J2534_FILTER_MSG_LEN] = {0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t fc[J2534_FILTER_MSG_LEN]      = {0, 0, 0, 0, 0, 0, 0, 0};
        pack_be32(pattern, t->rx_id);
        pack_be32(fc, t->tx_id);
        fc[4] = J2534_FC_PCI_CTS;   /* 30 00 00 — CTS, BS=0, STmin=0 */

        unsigned long filter_id = 0;
        st = qttune_j2534_start_msg_filter(t->handle, t->channel,
                                           QTTUNE_PT_FLOW_CONTROL_FILTER,
                                           mask, J2534_FILTER_MSG_LEN,
                                           pattern, J2534_FILTER_MSG_LEN,
                                           fc, J2534_FILTER_MSG_LEN,
                                           t->addr_flags, &filter_id);
        if (st != QT_OK) {
            /* Undo the connect so the failed-start state is identical
             * to a failed-connect state: channel 0, nothing for detach
             * to disconnect twice. */
            qttune_j2534_disconnect_channel(t->handle, t->channel);
            t->channel = 0;
            return QT_ERR_NO_TRANSPORT;
        }
        t->filter_id = filter_id;
        t->filter_active = true;
    }

    try {
        t->worker = std::thread(j2534_worker, t);
        return QT_OK;
    } catch (...) {
        if (t->filter_active && t->channel != 0) {
            (void)qttune_j2534_stop_msg_filter(t->handle, t->channel,
                                               t->filter_id);
            t->filter_active = false;
        }
        if (t->channel != 0) {
            qttune_j2534_disconnect_channel(t->handle, t->channel);
            t->channel = 0;
        }
        return QT_ERR_OUT_OF_MEMORY;
    }
}

void j2534_detach(qttune_session_t* session, void* impl)
{
    qttune_j2534_transport* t =
        static_cast<qttune_j2534_transport*>(impl);
    if (t == nullptr) {
        return;
    }

    qttune_transport_t* wrapper = session->transport;
    session->transport = nullptr;

    /* ORDER: stop + join the worker BEFORE touching the handle — the
     * worker calls read_msgs through function pointers that dangle the
     * moment j2534_close unmaps the module. Then stop the filter
     * BEFORE disconnect (vendor-compatibility priority: the spec says
     * disconnect invalidates filters, but drivers that leak filter
     * state on a skipped stop are common enough that the explicit stop
     * is worth the extra call). Best-effort — a failure here must not
     * block teardown, and the subsequent disconnect covers it. */
    t->stop_requested.store(true);
    if (t->worker.joinable()) {
        t->worker.join();
    }
    if (t->handle != nullptr) {
        if (t->filter_active && t->channel != 0) {
            (void)qttune_j2534_stop_msg_filter(t->handle, t->channel,
                                               t->filter_id);
            t->filter_active = false;
        }
        if (t->channel != 0) {
            qttune_j2534_disconnect_channel(t->handle, t->channel);
        }
        qttune_j2534_close(t->handle);
    }
    delete t;
    delete wrapper;
}

const qttune_transport_ops_t kJ2534TransportOps = {
    j2534_start,
    j2534_detach
};

} // namespace

extern "C" {

qttune_status_t qttune_j2534_transport_attach(qttune_session_t* session,
                                             const char* uri)
{
    if (session == nullptr || uri == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (session->transport != nullptr) {
        return QT_OK;   /* idempotent, mirroring mock attach */
    }

    qttune_j2534_device_t dev = {};
    j2534_uri_params params;
    qttune_status_t st = resolve_uri(uri, &dev, &params);
    if (st != QT_OK) {
        return st;
    }

    /* Handle opens eagerly at ATTACH (not start) so load failures
     * surface at session_create, where the caller can pick another
     * URI — matching the mock's failure-point ergonomics. */
    qttune_j2534_handle_t* handle = nullptr;
    st = qttune_j2534_open(&dev, &handle);
    if (st != QT_OK) {
        return st;
    }

    try {
        qttune_j2534_transport* impl = new qttune_j2534_transport();
        try {
            session->transport =
                new qttune_transport_t{&kJ2534TransportOps, impl};
        } catch (...) {
            delete impl;
            qttune_j2534_close(handle);
            return QT_ERR_OUT_OF_MEMORY;
        }
        impl->session = session;
        impl->handle = handle;
        impl->baud = params.baud;
        impl->flags = params.flags;
        impl->rx_id = static_cast<uint32_t>(params.rx_id);
        impl->tx_id = (params.tx_id != 0)
                      ? static_cast<uint32_t>(params.tx_id)
                      : J2534_TX_ID_DEFAULT;
        impl->poll_pid = static_cast<uint32_t>(params.poll_pid);
        /* 29-bit addressing is implied by either ID exceeding 11 bits —
         * the same bit stamps the filter messages and the write. */
        impl->addr_flags = (impl->rx_id > 0x7FFu || impl->tx_id > 0x7FFu)
                           ? QTTUNE_PT_CAN_29BIT_ID : 0u;
    } catch (...) {
        qttune_j2534_close(handle);
        return QT_ERR_OUT_OF_MEMORY;
    }
    return QT_OK;
}

} // extern "C"

#else

extern "C" {

qttune_status_t qttune_j2534_transport_attach(qttune_session_t* /*session*/,
                                             const char* /*uri*/)
{
    return QT_ERR_NOT_IMPLEMENTED;
}

} // extern "C"

#endif /* _WIN32 */
