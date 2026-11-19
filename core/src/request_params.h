// File/layer: core/src/request_params.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: HARNESS: J2534QueryParamTest (test_j2534_stream);
//          Elm327SessionTest (test_elm327); SocketCanRequestTest (test_vcan)
//
// The request keys a requesting transport reads from its URI, the first
// step of request_setup.h: their values, the strict number parse they
// share, and a refused value's words.

#ifndef ECUIO_REQUEST_PARAMS_H
#define ECUIO_REQUEST_PARAMS_H

#include "error_detail.h"
#include "request_engine.h"
#include "uri_query.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

constexpr uint32_t ECUIO_CAN_ID_MAX = 0x1FFFFFFFu; /* 29-bit */
constexpr uint32_t ECUIO_CAN_ID_11BIT_MAX = 0x7FFu;

/* The request keys' values: filter, txid, poll, probe, identify,
 * diagnose, pack, packframes, scan, gap, session and keepalive_ms. */
struct request_params {
    uint32_t rx_id = 0; /* 0 = no ?filter= override */
    uint32_t tx_id = 0; /* 0 = unset: profile's tx, else 0x7E0 */
    /* false = no ?poll= (passive or the set's rotation) */
    bool has_poll = false;
    uint8_t poll_pid = 0;    /* valid when has_poll; 0x00 allowed */
    bool probe = true;       /* ?probe=0 turns it off */
    bool identify = true;    /* ?identify=0 turns it off */
    bool diagnose = true;    /* ?diagnose=0 turns it off */
    bool pack = true;        /* ?pack=0 turns it off */
    uint8_t pack_frames = 0; /* ?packframes=; 0 = no limit */
    bool scan_first = false; /* ?scan=first: the DID scan before the rotation */
    uint8_t session = ECUIO_SESSION_DEFAULT; /* ?session=03 (ADR-0060) */
    uint32_t keepalive_ms = 0; /* ?keepalive_ms=; 0 = the engine's default */
    /* ?gap=: the floor between requests on a channel; -1 the transport's */
    int64_t gap_us = -1;
};

/* The most ?gap= takes, in ms */
constexpr unsigned long ECUIO_REQUEST_GAP_MAX_MS = 1000;

/* The most ?packframes= takes: ELM327's count digit stops at 15 */
constexpr unsigned long ECUIO_REQUEST_PACK_FRAMES_MAX = 15;

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
 *   probe    0 or 1: the supported-PID probe (ADR-0015 section 4), on by
 *            default
 *   identify 0 or 1: vehicle identification (ADR-0021), on by default
 *            with a set's requests
 *   diagnose 0 or 1: trouble codes, readiness and the freeze frame
 *            (ADR-0037), on by default with a set's requests
 *   pack     0 or 1: several Service 01 PIDs per request (ADR-0039
 *            Appendix A), on by default with the probe
 *   packframes 1..15, decimal: the most CAN frames a pack's reply may
 *            take; absent, no limit; never with ?pack=0
 *   scan     turns or first: uds-generic's DID scan taking turns with
 *            the rotation (the default), or asked back to back with the
 *            rotation waiting until it ends (ADR-0020)
 *   gap      0..1000, decimal ms: the least time between the starts of
 *            two requests on a channel (the engine's floor)
 *   session  01 or 03: uds-generic reads in the default session (01, the
 *            default: no session request at all) or enters the extended
 *            one first (ADR-0060); never 02
 *   keepalive_ms 500 to 4500: tester present after that long without a
 *            request, with ?session=03 only (default 2000)
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
    if (q.is("probe") || q.is("identify") || q.is("diagnose") ||
        q.is("pack")) {
        if (q.val_len != 1 || (q.val[0] != '0' && q.val[0] != '1')) {
            *wanted = "0 or 1";
        } else if (q.is("probe")) {
            p->probe = q.val[0] == '1';
        } else if (q.is("identify")) {
            p->identify = q.val[0] == '1';
        } else if (q.is("pack")) {
            p->pack = q.val[0] == '1';
        } else {
            p->diagnose = q.val[0] == '1';
        }
        return true;
    }
    if (q.is("packframes")) {
        /* decimal only: a count, not an ID */
        const bool hex = q.val_len >= 2 && q.val[0] == '0' &&
                         (q.val[1] == 'x' || q.val[1] == 'X');
        if (hex || !ecuio_parse_number(q.val, q.val_len, &v) || v == 0 ||
            v > ECUIO_REQUEST_PACK_FRAMES_MAX) {
            *wanted = "a frame count from 1 to 15";
        } else {
            p->pack_frames = static_cast<uint8_t>(v);
        }
        return true;
    }
    if (q.is("gap")) {
        /* decimal only: a time, not an ID */
        const bool hex = q.val_len >= 2 && q.val[0] == '0' &&
                         (q.val[1] == 'x' || q.val[1] == 'X');
        if (hex || !ecuio_parse_number(q.val, q.val_len, &v) ||
            v > ECUIO_REQUEST_GAP_MAX_MS) {
            *wanted = "a time in ms from 0 to 1000";
        } else {
            p->gap_us = static_cast<int64_t>(v) * 1000;
        }
        return true;
    }
    if (q.is("scan")) {
        if (q.val_len == 5 && memcmp(q.val, "first", 5) == 0) {
            p->scan_first = true;
        } else if (q.val_len == 5 && memcmp(q.val, "turns", 5) == 0) {
            p->scan_first = false;
        } else {
            *wanted = "turns or first";
        }
        return true;
    }
    if (q.is("session")) {
        if (!ecuio_parse_number(q.val, q.val_len, &v) ||
            (v != ECUIO_SESSION_DEFAULT && v != ECUIO_SESSION_EXTENDED)) {
            *wanted = "01 (the default session) or 03 (the extended one)";
        } else {
            p->session = static_cast<uint8_t>(v);
        }
        return true;
    }
    if (q.is("keepalive_ms")) {
        if (!ecuio_parse_number(q.val, q.val_len, &v) ||
            v < ECUIO_ENGINE_KEEPALIVE_MIN_US / 1000 ||
            v > ECUIO_ENGINE_KEEPALIVE_MAX_US / 1000) {
            *wanted = "a time from 500 to 4500 ms";
        } else {
            p->keepalive_ms = static_cast<uint32_t>(v);
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

#endif
