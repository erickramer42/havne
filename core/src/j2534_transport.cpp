// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: UNIT: CoreTest (test_core); HARNESS: test_j2534_connect;
//          test_j2534_stream; test_j2534_health; Cross-arch
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there;
//          Reply timing on the adapter's clock (ADR-0042):
//          J2534HealthTest.VxTimingReportsEachRequestOnTheAdaptersClock.
//          A recording's link lines (ADR-0049) come through link_health.h,
//          tested there (RecorderTest.ALinkTransition*) and through the
//          fake ELM327; this worker's argument by code reading.
//          ECUIO_FRAME_FLAG_REASSEMBLED on every message:
//          J2534StreamTest.DirectLoadUriDeliversInjectedFramesExactly,
//          J2534HealthTest.LongRepliesArriveWhole
//          The floor and the timer's wait: J2534HealthTest.TheFloor*,
//          .VxTheFloorsWaitReadsNothing; ?gap=: J2534QueryParamTest
//          A talk's request whole in one write: J2534TalkTest
//          Each module's filter (ADR-0065 step 5): J2534ModulesTest
//
// j2534:// attach (the URI, the channels, the requests, the DLL), start
// and detach, each module's filter, and the C call. The worker is j2534_worker.cpp;
// j2534_transport.h is what they share.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "ecuio/j2534.h"
#include "request_engine.h"
#include "request_setup.h"
#include "link_health.h"
#include "j2534_clock.h"
#include "j2534_transport.h"
#include "uri_query.h"
#include "error_detail.h"
#include "host_clock.h"
#include "isotp.h"
#include "request_probe.h"
#include "conversation_bind.h"
#include "ecuio/profile.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

/* The query keys parse_query reads, for create's check (internal.h);
 * every platform's, so create links where the transport is a stub. */
extern "C" const char* const ecuio_j2534_query_keys[] = {
    "baud",     "flags",    "filter",       "txid",       "poll", "probe",
    "identify", "diagnose", "pack",         "scan",       "gap",  "profile",
    "did",      "session",  "keepalive_ms", "packframes", "hold", nullptr};

#ifdef _WIN32

#include "ecuio/j2534_ptypes.h"    /* FLOW_CONTROL_FILTER, CAN_29BIT_ID,
                                      TX_ISO15765_FRAME_PAD */
#include "win_utf8.h"              /* ecuio_is_absolute_path */
#include "precise_wait.h"          /* the wait after an empty read */

namespace {
using namespace ecuio_j2534;

/* Collected URI query parameters: parse_query fills, attach stores into
 * the transport impl. One struct rather than a six-pointer parse
 * signature, and one place for the coupling rule to live.
 *
 * baud == 0 is the "unset" sentinel (the parser rejects an explicit
 * baud=0), and j2534_loader's connect_channel substitutes the ISO 15765
 * default. flags pass through verbatim: 0 is both the default and a
 * legitimate value, and no bit is interpreted. The request keys are
 * every requesting transport's (request_setup.h). */
struct j2534_uri_params {
    unsigned long baud = 0;
    unsigned long flags = 0;
    bool hold = true;
    request_params req;
};

/* The channel table, from the set plus URI overrides
 * (ecuio_resolve_channels), each with its filter's CAN_29BIT_ID: an
 * ISO 15765 channel with no filter hears nothing, so a channel is a
 * receive ID to register one for. */
void resolve_channels(ecuio_j2534_transport* t, const ecuio_signal_set_t* set,
                      const request_params& params)
{
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
    uint32_t tx[ECUIO_MAX_CHANNELS] = {};
    const uint32_t n = ecuio_resolve_channels(set, params, rx, tx);
    for (uint32_t i = 0; i < n; ++i) {
        t->chans[i].rx_id = rx[i];
        t->chans[i].tx_id = tx[i];
        t->chans[i].addr_flags =
            ecuio_channel_is_29bit(rx[i], tx[i]) ? ECUIO_PT_CAN_29BIT_ID : 0;
    }
    t->chan_count = n;
}

/* The query's values (ecuio_j2534_query_keys). Contract (strict: a bad
 * value means BAD_URI, surfacing at session_create before any DLL
 * load):
 *   - lowercase keys only:
 *       baud   decimal, nonzero
 *       flags  0x-hex or decimal; 0 legal
 *       hold   0 or 1: the hold for Known Issue #42 (j2534_hold.h), on
 *              by default
 *       the request keys (ecuio_request_param lists them); ?filter='s
 *              channel registers a FLOW_CONTROL filter; without ?gap=
 *              the floor is J2534_GAP_US
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
            if (!ecuio_parse_number(q.val, q.val_len, &v) || v == 0) {
                wanted = "a nonzero decimal";
            } else {
                p->baud = v;
            }
        } else if (q.is("flags")) {
            if (!ecuio_parse_number(q.val, q.val_len, &v)) {
                wanted = "a decimal or 0x-hex number";
            } else {
                p->flags = v;
            }
        } else if (q.is("hold")) {
            if (q.val_len != 1 || (q.val[0] != '0' && q.val[0] != '1')) {
                wanted = "0 or 1";
            } else {
                p->hold = q.val[0] == '1';
            }
        } else {
            /* the request keys; profile and did are core's, read and
             * validated by create */
            (void)ecuio_request_param(q, &p->req, &wanted);
        }
        if (wanted != nullptr) {
            ecuio_detail_bad_value(q, wanted);
            return ECUIO_ERR_BAD_URI;
        }
    }

    /* The poll-needs-a-channel rule lives in j2534_attach, not here:
     * this parser can't see the profile's declared channels, and
     * rejecting ?poll= without ?filter= here would refuse a profile
     * that declares its own (?profile=obd2&poll=0x0C). Syntax is this
     * function's job; coupling is attach's, and attach still fires
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
        *dev = devices[0];   /* first device: documented default */
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
    auto* t = static_cast<ecuio_j2534_transport*>(impl);
    if (t->worker.joinable()) {
        return ECUIO_OK;    /* internal.h: second start on a live
                         * transport returns ECUIO_OK */
    }

    ecuio_status_t st = ECUIO_OK;
    if (j2534_asks(t)) {
        ecuio_modules_seen seen; /* the picks set before start first */
        (void)ecuio_take_modules(session, &t->engine, seen, 0);
        st = j2534_filters_follow(t, true);
    } else {
        for (uint32_t c = 0; c < t->chan_count && st == ECUIO_OK; ++c) {
            st = j2534_filter_open(t, c);
        }
    }
    if (st != ECUIO_OK) {
        /* no partial filter state survives a failed start */
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            j2534_filter_close(t, c);
        }
        return st;
    }
    char line[ECUIO_CONSENT_LOG_LINE_MAX];
    if (ecuio_consent_log_line(session, line, sizeof line)) {
        ecuio_j2534_log_line(t->handle, line);
    }

    t->stop_requested.store(false);
    try {
        t->worker = std::thread(j2534_worker, t);
    } catch (...) {
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            j2534_filter_close(t, c);
        }
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

/* detach(session, impl): void per internal.h, called exactly once from
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
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        j2534_filter_close(t, c);
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

/* The requests for the channels as they stand (ecuio_build_requests),
 * their IDs into rx_ids and tx_ids, with the adapter's bounds: ISO-TP
 * sent whole, J2534's guaranteed filters */
ecuio_status_t build_channels(ecuio_session_t* session,
                              const ecuio_signal_set_t* set,
                              const j2534_uri_params& params,
                              ecuio_j2534_transport* t, uint32_t* rx_ids,
                              uint32_t* tx_ids)
{
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        rx_ids[i] = t->chans[i].rx_id;
        tx_ids[i] = t->chans[i].tx_id;
    }
    const ecuio_status_t st = ecuio_build_requests(
        session, set, params.req, rx_ids, tx_ids, t->chan_count, &t->engine,
        &t->probe, &t->identify);
    if (st != ECUIO_OK) {
        return st;
    }
    ecuio_attach_timing(t->engine, t->timing, ECUIO_TIMING_CLOCK_ADAPTER);
    t->engine.pdu_max = ECUIO_ENGINE_PDU_MAX; /* the adapter sends ISO-TP */
    t->engine.filters_most = J2534_FILTERS_GUARANTEED;
    t->engine.module_budget = J2534_FILTERS_GUARANTEED - 1;
    return ECUIO_OK;
}

/* A probe group's replies, read until the one-shot's wait has passed */
void probe_read(ecuio_j2534_handle_t* handle, unsigned long channel,
                ecuio_probe* p)
{
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::microseconds(ECUIO_ENGINE_WAIT_US);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return;
        }
        ecuio_pt_rx_t rx{};
        bool got = false;
        if (ecuio_j2534_read_msg(handle, channel, &rx,
                                 static_cast<uint32_t>(left.count()),
                                 &got) != ECUIO_OK) {
            return;
        }
        if (got && rx.data_size > J2534_ADDR_LEN) {
            const uint32_t id =
                ((uint32_t{rx.data[0]} << 24) | (uint32_t{rx.data[1]} << 16) |
                 (uint32_t{rx.data[2]} << 8) | rx.data[3]) &
                ECUIO_CAN_ID_MAX;
            ecuio_probe_take(*p, id, rx.data + J2534_ADDR_LEN,
                             rx.data_size - J2534_ADDR_LEN);
        }
    }
}

/* The start's emissions probe (request_probe.h) on the connected channel,
 * a best try: for each group a flow-control filter for each reply ID, one
 * functional 01 00, its replies read, the filters stopped. A refused
 * vendor call ends it with what it heard; the session log has the call. */
void run_probe(ecuio_j2534_handle_t* handle, unsigned long channel,
               const ecuio_j2534_transport& t, unsigned long wide,
               ecuio_probe* p)
{
    const auto start = std::chrono::steady_clock::now();
    const uint32_t tx = wide != 0 ? ECUIO_PROBE_TX_29 : ECUIO_PROBE_TX_11;
    uint32_t rx_ids[ECUIO_MAX_CHANNELS];
    for (uint32_t i = 0; i < t.chan_count; ++i) {
        rx_ids[i] = t.chans[i].rx_id;
    }
    uint32_t group[ECUIO_MAX_CHANNELS];
    for (uint32_t g = 0;; ++g) {
        const uint32_t n = ecuio_probe_group(rx_ids, t.chan_count, wide != 0,
                                             t.engine.module_budget, g, group);
        if (n == 0) {
            break;
        }
        unsigned long ids[ECUIO_MAX_CHANNELS];
        uint32_t started = 0;
        for (; started < n; ++started) {
            uint8_t mask[J2534_FILTER_MSG_LEN];
            uint8_t pattern[J2534_FILTER_MSG_LEN];
            uint8_t fc[J2534_FILTER_MSG_LEN];
            pack_be32(mask, 0xFFFFFFFFu);
            pack_be32(pattern, group[started]);
            pack_be32(fc, isotp_flow_control_id(group[started], tx));
            if (ecuio_j2534_start_msg_filter(
                    handle, channel, ECUIO_PT_FLOW_CONTROL_FILTER, mask,
                    J2534_FILTER_MSG_LEN, pattern, J2534_FILTER_MSG_LEN, fc,
                    J2534_FILTER_MSG_LEN, wide | ECUIO_PT_TX_ISO15765_FRAME_PAD,
                    &ids[started]) != ECUIO_OK) {
                break;
            }
        }
        uint8_t msg[J2534_ADDR_LEN + 2];
        pack_be32(msg, tx);
        msg[J2534_ADDR_LEN] = ECUIO_PROBE_REQUEST[0];
        msg[J2534_ADDR_LEN + 1] = ECUIO_PROBE_REQUEST[1];
        uint32_t written = 0;
        if (started == n &&
            ecuio_j2534_write_frame(handle, channel, msg, sizeof msg,
                                    wide | ECUIO_PT_TX_ISO15765_FRAME_PAD, 0,
                                    &written) == ECUIO_OK &&
            written >= 1) {
            ++p->requests;
            probe_read(handle, channel, p);
        }
        for (uint32_t k = 0; k < started; ++k) {
            (void)ecuio_j2534_stop_msg_filter(handle, channel, ids[k]);
        }
        if (started != n) {
            break;
        }
    }
    p->took_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
}

/* The probe on a session that asks anything: its answerers are RANGE
 * (the session's module_range), and an undeclared 11-bit one joins the
 * channels (never under ?filter= or ?poll=), whose requests are then
 * built again */
ecuio_status_t probe_modules(ecuio_session_t* session,
                             const ecuio_signal_set_t* set,
                             const j2534_uri_params& params,
                             ecuio_j2534_transport* t, unsigned long wide,
                             uint32_t* rx_ids, uint32_t* tx_ids)
{
    if (t->engine.count == 0 && t->engine.scan_count == 0) {
        return ECUIO_OK;
    }
    ecuio_probe p;
    if (ecuio_start_probe_on().load()) {
        run_probe(t->handle, t->channel, *t, wide, &p);
    }
    const uint32_t before = t->chan_count;
    if (params.req.rx_id == 0 && !params.req.has_poll &&
        ecuio_probe_join(p, rx_ids, tx_ids, &t->chan_count) > 0) {
        for (uint32_t i = before; i < t->chan_count; ++i) {
            t->chans[i].rx_id = rx_ids[i];
            t->chans[i].tx_id = tx_ids[i];
            t->chans[i].addr_flags = 0;
        }
        const ecuio_status_t st =
            build_channels(session, set, params, t, rx_ids, tx_ids);
        if (st != ECUIO_OK) {
            return st;
        }
    }
    session->module_range = ecuio_probe_range(
        p, rx_ids, t->chan_count, ecuio_engine_rpm_module_of(&t->engine));
    if (p.requests > 0) {
        char line[160];
        ecuio_probe_describe(p, line, sizeof line);
        ecuio_j2534_log_line(t->handle, line);
    }
    return ECUIO_OK;
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
    if (params.req.gap_us < 0) {
        params.req.gap_us = J2534_GAP_US;
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
    resolve_channels(t.get(), set, params.req);

    /* Requests, fixed before the thread starts (ecuio_build_requests):
     * ?poll='s one, uds-generic's rotation and scan, or the set's
     * rotation, then identification; none is passive receive. Shipped
     * profiles declare channels, so its coupling check fires for a set
     * author omitting them, or ?poll= without one; before any hardware
     * is touched. */
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    uint32_t tx_ids[ECUIO_MAX_CHANNELS] = {};
    st = build_channels(session, set, params, t.get(), rx_ids, tx_ids);
    t->hold = params.hold;
    if (st != ECUIO_OK) {
        return st;
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
    st = probe_modules(session, set, params, t.get(),
                       wide_chans != 0 ? ECUIO_PT_CAN_29BIT_ID : 0UL, rx_ids,
                       tx_ids);
    if (st == ECUIO_OK) {
        describe_session(t.get(), params.baud, connect_flags, &session->info);
        session->battery.source = true;
        st = ecuio_session_set_modules(session, rx_ids, tx_ids, t->chan_count);
    }
    if (st != ECUIO_OK) {
        delete wrap;
        ecuio_j2534_disconnect_channel(handle, channel);
        ecuio_j2534_close(handle);
        return st;
    }
    ecuio_seed_reports(session, t->engine, rx_ids, t->chan_count);
    t->epoch = host_clock::now();   /* as mock: end of attach */
    session->judges_link = true;

    wrap->impl = t.release();
    session->transport = wrap;   /* session.h member: direct install */
    return ECUIO_OK;
}

} // namespace

bool ecuio_j2534::j2534_asks(const ecuio_j2534_transport* t)
{
    return t->engine.count > 0 || t->engine.scan_count > 0 ||
           ecuio_session_may_talk(t->session);
}

ecuio_status_t ecuio_j2534::j2534_filter_open(ecuio_j2534_transport* t,
                                              uint32_t c)
{
    j2534_channel& ch = t->chans[c];
    uint8_t mask[J2534_FILTER_MSG_LEN];
    uint8_t pattern[J2534_FILTER_MSG_LEN];
    uint8_t fc[J2534_FILTER_MSG_LEN];
    pack_be32(mask, 0xFFFFFFFFu); /* exact-ID match, all 4 bytes */
    pack_be32(pattern, ch.rx_id);
    /* A functional request's reply takes its Flow Control on the physical
     * request ID (isotp_flow_control_id); the request itself is one frame,
     * which J2534-1 (DEC2004) Appendix A.3 sends with no filter naming
     * its ID */
    pack_be32(fc, isotp_flow_control_id(ch.rx_id, ch.tx_id));
    const ecuio_status_t st = ecuio_j2534_start_msg_filter(
        t->handle, t->channel, ECUIO_PT_FLOW_CONTROL_FILTER, mask,
        J2534_FILTER_MSG_LEN, pattern, J2534_FILTER_MSG_LEN, fc,
        J2534_FILTER_MSG_LEN, ch.addr_flags | ECUIO_PT_TX_ISO15765_FRAME_PAD,
        &ch.filter_id);
    ch.filter_active = st == ECUIO_OK;
    return st;
}

void ecuio_j2534::j2534_filter_close(ecuio_j2534_transport* t, uint32_t c)
{
    j2534_channel& ch = t->chans[c];
    if (ch.filter_active) {
        (void)ecuio_j2534_stop_msg_filter(t->handle, t->channel, ch.filter_id);
        ch.filter_active = false;
    }
}

ecuio_status_t ecuio_j2534::j2534_filters_follow(ecuio_j2534_transport* t,
                                                 bool at_start)
{
    ecuio_request_engine& e = t->engine;
    uint32_t held = 0;
    uint32_t holding = 0; /* a bit for each module holding a filter */
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        if (!ecuio_engine_module_hears(e.chans[c])) {
            j2534_filter_close(t, c);
        }
        held += t->chans[c].filter_active ? 1u : 0u;
        holding |= t->chans[c].filter_active ? 1u << c : 0u;
    }
    /* RANGE first: a refusal falls on a pick, never on an emissions module */
    for (const bool range : {true, false}) {
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            const ecuio_engine_channel& ch = e.chans[c];
            if (t->chans[c].filter_active || !ecuio_engine_module_hears(ch) ||
                ((ch.reasons & ECUIO_ENGINE_REASON_RANGE) != 0) != range) {
                continue;
            }
            const ecuio_status_t st = j2534_filter_open(t, c);
            if (st == ECUIO_OK) {
                ++held;
                holding |= 1u << c;
            } else if (at_start && held == 0) {
                return st;
            } else {
                ecuio_engine_filter_refused(&e, c, holding);
            }
        }
    }
    return ECUIO_OK;
}

extern "C" ecuio_status_t ecuio_j2534_transport_attach(
    ecuio_session_t* session, const char* uri, const ecuio_uri_query* query)
{
    return j2534_attach(session, uri, query);
}

#else /* !_WIN32 */

/* Linking stub: core.cpp routes j2534:// only on _WIN32, but the
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
