// File/layer: core/src/request_setup.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: HARNESS: through both requesting transports:
//          J2534QueryParamTest and J2534AdoptionTest (the query keys,
//          the channels, raw on the engine ECU, ?poll=, uds-generic's
//          scan, identification, diagnostics, the support, identity
//          and diagnostics reports, Read again), Elm327SessionTest (the
//          same through an ELM327)
//
// What a requesting transport (J2534, ELM327) derives from its URI and
// profile before it opens anything: the request keys' values, the
// channels, the requests (request_engine.h), and the reports its worker
// publishes. One place, so a run sheet's query means the same on either
// scheme (ADR-0026 §2).

#ifndef ECUIO_REQUEST_SETUP_H
#define ECUIO_REQUEST_SETUP_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "request_engine.h"
#include "session.h"
#include "uri_query.h"

#include <cstdint>
#include <limits>
#include <mutex>

constexpr uint32_t ECUIO_CAN_ID_MAX = 0x1FFFFFFFu; /* 29-bit */
constexpr uint32_t ECUIO_CAN_ID_11BIT_MAX = 0x7FFu;

/* The request keys' values: filter, txid, poll, probe, identify and
 * diagnose. */
struct request_params {
    uint32_t rx_id = 0; /* 0 = no ?filter= override */
    uint32_t tx_id = 0; /* 0 = unset: profile's tx, else 0x7E0 */
    /* false = no ?poll= (passive or the set's rotation) */
    bool has_poll = false;
    uint8_t poll_pid = 0; /* valid when has_poll; 0x00 allowed */
    bool probe = true;    /* ?probe=0 turns it off */
    bool identify = true; /* ?identify=0 turns it off */
    bool diagnose = true; /* ?diagnose=0 turns it off */
};

/* Strict unsigned parse: decimal by default, '0x'/'0X' prefix enables
 * hex digits. Rejects empty digits, '+', '-', whitespace, and overflow.
 * No strtoul: its tolerance for signs/whitespace is a liability at
 * an ABI boundary where garbage must become an error, not a value. */
inline bool ecuio_parse_number(const char* s, size_t len,
                               unsigned long* out_val)
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
            return false; /* bare "0x" */
        }
    }

    const unsigned long max =
        (std::numeric_limits<unsigned long>::max)(); /* windows.h's max */
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
            return false; /* would overflow unsigned long */
        }
        value = value * base + digit;
    }

    *out_val = value;
    return true;
}

/* Reads q into p when it's a request key:
 *   filter   CAN id 1..0x1FFFFFFF: receive ID; replaces the profile's
 *            channels with this one
 *   txid     CAN id 1..0x1FFFFFFF: transmit ID; default is the
 *            profile's tx, else 0x7E0
 *   poll     PID 0x00..0xFF: one Service 01 request, repeated as the
 *            whole rotation (0x00 reads the supported-PIDs bitmap);
 *            replaces the profile's requests
 *   probe    0 or 1: the supported-PID probe (ADR-0015 §4), on by
 *            default
 *   identify 0 or 1: vehicle identification (ADR-0021), on by default
 *            with a set's requests
 *   diagnose 0 or 1: trouble codes, readiness and the freeze frame
 *            (ADR-0037), on by default with a set's requests
 * False when q is another key. A bad value leaves p alone and sets
 * *wanted to what it should have been; the caller words the detail. */
inline bool ecuio_request_param(const ecuio_uri::param& q, request_params* p,
                                const char** wanted)
{
    unsigned long v = 0;
    if (q.is("filter") || q.is("txid")) {
        if (!ecuio_parse_number(q.val, q.val_len, &v) || v == 0 ||
            v > ECUIO_CAN_ID_MAX) {
            *wanted = "a CAN ID from 1 to 0x1FFFFFFF";
        } else if (q.is("filter")) {
            p->rx_id = static_cast<uint32_t>(v);
        } else {
            p->tx_id = static_cast<uint32_t>(v);
        }
        return true;
    }
    if (q.is("poll")) {
        if (!ecuio_parse_number(q.val, q.val_len, &v) || v > 0xFFu) {
            *wanted = "a PID from 0x00 to 0xFF";
        } else {
            p->poll_pid = static_cast<uint8_t>(v);
            p->has_poll = true;
        }
        return true;
    }
    if (q.is("probe") || q.is("identify") || q.is("diagnose")) {
        if (q.val_len != 1 || (q.val[0] != '0' && q.val[0] != '1')) {
            *wanted = "0 or 1";
        } else if (q.is("probe")) {
            p->probe = q.val[0] == '1';
        } else if (q.is("identify")) {
            p->identify = q.val[0] == '1';
        } else {
            p->diagnose = q.val[0] == '1';
        }
        return true;
    }
    return false;
}

/* The detail for a refused value: "?key= needs <wanted>, not '<value>'",
 * the value cut at 32 characters. */
inline void ecuio_detail_bad_value(const ecuio_uri::param& q,
                                   const char* wanted)
{
    const int shown = static_cast<int>((q.val_len < 32) ? q.val_len : 32);
    ecuio_detail_set("?%.*s= needs %s, not '%.*s'", static_cast<int>(q.key_len),
                     q.key, wanted, shown, q.val);
}

/* The channels, from the set plus the URI's overrides, into rx[] and
 * tx[] (ECUIO_MAX_CHANNELS each); returns how many. Never fails:
 * bounded by construction, and malformed set entries are skipped.
 * ?filter= wins outright (one channel); ?txid= alone rewrites every
 * derived tx. A log-only profile (raw: no set) with no ?filter= listens
 * on the default profile's channels, obd2's engine ECU (ADR-0013's
 * appendix of 2026-10-26): a channel with no receive ID hears nothing. A
 * set without channels, and a lone ?txid= on one, yields no channel. */
inline uint32_t ecuio_resolve_channels(const ecuio_signal_set_t* set,
                                       const request_params& params,
                                       uint32_t* rx, uint32_t* tx)
{
    if (set == nullptr && params.rx_id == 0) {
        set = ecuio_obd2_signal_set();
    }
    uint32_t n = 0;
    if (params.rx_id != 0) {
        uint32_t t = params.tx_id;
        if (t == 0 && set != nullptr && set->channel_count > 0) {
            t = set->channels[0].tx_id;
        }
        if (t == 0) {
            t = ECUIO_OBD2_TESTER_ARB_ID;
        }
        rx[0] = params.rx_id;
        tx[0] = t;
        n = 1;
    } else if (set != nullptr) {
        for (uint32_t i = 0; i < set->channel_count && n < ECUIO_MAX_CHANNELS;
             ++i) {
            const uint32_t r = set->channels[i].rx_id;
            if (r == 0) {
                continue;
            }
            uint32_t t =
                (params.tx_id != 0) ? params.tx_id : set->channels[i].tx_id;
            if (t == 0) {
                t = ECUIO_OBD2_TESTER_ARB_ID;
            }
            rx[n] = r;
            tx[n] = t;
            ++n;
        }
    }
    return n;
}

/* Whether a channel uses 29-bit IDs: either of its IDs past 11 bits. */
inline bool ecuio_channel_is_29bit(uint32_t rx, uint32_t tx)
{
    return rx > ECUIO_CAN_ID_11BIT_MAX || tx > ECUIO_CAN_ID_11BIT_MAX;
}

/* The channel ?poll='s request goes to, by the rule the set's rotation
 * follows (request_engine.h): that of a def whose value comes back in
 * its reply, else channel 0. */
inline uint32_t ecuio_poll_channel(const ecuio_signal_set_t* set, uint8_t pid,
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

/* The requests, into e (initialized here for chan_count channels):
 *   ?poll=        -> that one Service 01 request; the set's rotation
 *                    isn't built, so the PID in the URI is the only one
 *                    asked for. It may be 0x00 (the supported-PIDs
 *                    bitmap), hence the presence flag
 *   uds-generic   -> the rotation from the set's polled head, and the
 *                    scan of its ranges (ADR-0020)
 *   any other set -> the set's rotation (request_engine.h), which
 *                    registration kept within the table
 *   no requests   -> passive receive
 * then identification (ADR-0021) when the session already requests and
 * neither ?identify=0 nor ?poll=, and diagnostics (ADR-0037, decoded by
 * the obd2 set whatever the profile) by the same rule with ?diagnose=0.
 * *probe and *identify get what the
 * worker runs: ?poll= sends its one request as asked, whatever the ECU
 * says. Then the coupling check: requests need a channel to receive on,
 * or they go out against guaranteed silence forever. A set's requests
 * get theirs from the set; ?poll= may take ?filter= or the profile's
 * declared channels. BAD_URI with the detail set when they have none,
 * before any hardware is touched. */
inline ecuio_status_t ecuio_build_requests(
    const ecuio_session_t* session, const ecuio_signal_set_t* set,
    const request_params& params, const uint32_t* rx_ids, uint32_t chan_count,
    ecuio_request_engine* e, bool* probe, bool* identify)
{
    ecuio_engine_init(e, chan_count);
    if (params.has_poll) {
        const uint8_t req[2] = {0x01, params.poll_pid};
        (void)ecuio_engine_add(
            e, ecuio_poll_channel(set, req[1], rx_ids, chan_count), req,
            sizeof(req));
    } else if (session->uds == nullptr) {
        (void)ecuio_engine_derive(e, set, rx_ids, chan_count, nullptr);
    } else {
        /* uds-generic (ADR-0020): the rotation from the set's polled
         * head, then each scanned DID once, on the channel of its ECU by
         * the rule the rotation's requests follow (ecuio_engine_channel_for:
         * channel 0 under a ?filter= override). Create's limits keep both
         * within the engine's tables. */
        const ecuio_signal_set_t head = ecuio_uds_polled_set(*session->uds);
        (void)ecuio_engine_derive(e, &head, rx_ids, chan_count, nullptr);
        for (const ecuio_uds_scan_target& s : session->uds->scan) {
            uint32_t c = 0;
            for (uint32_t i = 0; i < chan_count; ++i) {
                if (rx_ids[i] == s.rx_id) {
                    c = i;
                }
            }
            (void)ecuio_engine_scan_add(e, c, s.did);
        }
    }
    *probe = params.probe && !params.has_poll;
    const bool requests = e->count > 0 || e->scan_count > 0;
    *identify = params.identify && !params.has_poll && requests;
    if (*identify) {
        ecuio_engine_identify(e);
    }
    if (params.diagnose && !params.has_poll && requests) {
        ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
    }

    if (requests && chan_count == 0) {
        if (params.has_poll) {
            ecuio_detail_set("?poll= needs a channel to receive on: add "
                             "?filter=, or use a profile that declares one");
        } else {
            ecuio_detail_set("the profile requests PIDs but declares no "
                             "channel to receive on");
        }
        return ECUIO_ERR_BAD_URI;
    }
    return ECUIO_OK;
}

/* Hands what channel c's ECU has said it supports to the session, for
 * ecuio_session_get_channel_support, when the engine has learned
 * something new. */
inline void ecuio_publish_support(ecuio_session_t* session,
                                  const ecuio_request_engine& e, uint32_t c,
                                  uint32_t rx_id, uint32_t& published)
{
    if (e.chans[c].generation == published) {
        return;
    }
    published = e.chans[c].generation;
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, c, &s);
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    session->support[c] = s;
}

/* Hands what channel c's ECU has said it is to the session, for
 * ecuio_session_get_channel_identity, when an item has settled. */
inline void ecuio_publish_identity(ecuio_session_t* session,
                                   const ecuio_request_engine& e, uint32_t c,
                                   uint32_t rx_id, uint32_t& published)
{
    if (e.chans[c].ident_generation == published) {
        return;
    }
    published = e.chans[c].ident_generation;
    ecuio_channel_identity_t s;
    ecuio_engine_identity(&e, c, &s);
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    session->identity[c] = s;
}

/* Hands what channel c's ECU has reported of its diagnostics to the
 * session, for ecuio_session_get_channel_diagnostics, when an item has
 * settled or a read has started over. */
inline void ecuio_publish_diagnostics(ecuio_session_t* session,
                                      const ecuio_request_engine& e, uint32_t c,
                                      uint32_t rx_id, uint32_t& published)
{
    if (e.chans[c].diag.generation == published) {
        return;
    }
    published = e.chans[c].diag.generation;
    ecuio_channel_diagnostics_t s;
    ecuio_engine_diagnostics(&e, c, &s);
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    session->diagnostics[c] = s;
}

/* Starts the diagnostics over (ecuio_engine_diagnose_again) when
 * ecuio_session_read_diagnostics has been called since the worker last
 * looked; seen is the worker's count. */
inline void ecuio_take_diag_request(ecuio_session_t* session,
                                    ecuio_request_engine* e, uint32_t& seen)
{
    const uint32_t asked = session->diag_requests.load();
    if (asked != seen) {
        seen = asked;
        ecuio_engine_diagnose_again(e);
    }
}

/* Each channel's ECU, its requests, its identification and diagnostics
 * items, nothing learned yet: what attach hands the session before the
 * worker runs. */
inline void ecuio_seed_reports(ecuio_session_t* session,
                               const ecuio_request_engine& e,
                               const uint32_t* rx_ids, uint32_t chan_count)
{
    std::lock_guard<std::mutex> lock(session->support_mutex);
    session->support_count = chan_count;
    for (uint32_t c = 0; c < chan_count; ++c) {
        ecuio_engine_support(&e, c, &session->support[c]);
        session->support[c].rx_id = rx_ids[c];
        ecuio_engine_identity(&e, c, &session->identity[c]);
        session->identity[c].rx_id = rx_ids[c];
        ecuio_engine_diagnostics(&e, c, &session->diagnostics[c]);
        session->diagnostics[c].rx_id = rx_ids[c];
    }
}

#endif /* ECUIO_REQUEST_SETUP_H */
