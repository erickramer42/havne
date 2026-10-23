// File/layer: core/src/request_engine.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its driver) and
//             profile_registry.cpp (the request limit)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — derivation, deduplication and the limit, the
//     read-only rule, rotation per channel, correlation, the wait, the
//     pending wait and the floor (RequestEngineTest, test_obd2); the
//     worker that drives it is HARNESS (J2534AdoptionTest,
//     J2534HealthTest), its timing on a vehicle BENCH.

#ifndef ECUIO_REQUEST_ENGINE_H
#define ECUIO_REQUEST_ENGINE_H

#include "ecuio/profile.h"
#include "ecuio/signals.h"

#include <stdint.h>

/* The request engine (ADR-0015): what to request, derived from the
 * defs, and when, paced by the replies. It does no I/O and reads no
 * clock: its driver passes the time in, writes the requests and hands
 * over what it reads, so every rule here is testable without a vendor
 * DLL.
 *
 * A request is [service ID][key]: 01 <PID>, or 22 <DID high> <DID low>.
 * Each belongs to one channel, an index into the driver's channel
 * table. A channel has at most one request outstanding, as ISO 15765
 * physical addressing expects, and walks its own rotation, so channels
 * progress independently. */

/* Timing (ADR-0015, appendix of 2026-10-23). The wait is the server's
 * response time P2 (50 ms) plus 50 ms for the adapter: the longest reply
 * on the first vehicle took 83 ms. A "response pending" reply extends it
 * to P2* (5 s) plus the same margin. The floor is the least time between
 * two requests on one channel, set before any vehicle has run the
 * engine. */
constexpr int64_t ECUIO_ENGINE_WAIT_US = 100000;
constexpr int64_t ECUIO_ENGINE_PENDING_WAIT_US = 5050000;
constexpr int64_t ECUIO_ENGINE_GAP_US = 20000;

/* Negative responses: 7F <service> <code> (ISO 14229-1). */
constexpr uint8_t ECUIO_NEGATIVE_RESPONSE = 0x7F;
constexpr uint8_t ECUIO_NRC_RESPONSE_PENDING = 0x78;

struct ecuio_request {
    uint8_t bytes[3];   /* the service ID, then the key */
    uint8_t len;        /* 2 or 3 */
    uint8_t channel;
};

struct ecuio_engine_channel {
    int32_t outstanding = -1;   /* the request awaiting its reply, or -1 */
    int64_t deadline_us = 0;    /* when that request's wait ends */
    int64_t next_send_us = 0;   /* the floor: no request before this */
    uint32_t cursor = 0;        /* where the rotation resumes */
    uint32_t requests = 0;      /* how many requests are this channel's */
};

struct ecuio_request_engine {
    ecuio_request requests[ECUIO_PROFILE_MAX_REQUESTS] = {};
    uint32_t count = 0;
    ecuio_engine_channel chans[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
};

/* How a frame on a channel relates to its outstanding request. */
enum class ecuio_reply {
    none,       /* answers no outstanding request */
    positive,   /* the service ID + 0x40, then the key: complete */
    negative,   /* 7F, the service ID, a code: complete */
    pending     /* 7F, the service ID, 0x78: still waiting, for longer */
};

enum class ecuio_engine_add_result { added, duplicate, refused, full };

/* §3: the services a request may carry. 0x01 (current data), 0x09
 * (vehicle information) and 0x22 (ReadDataByIdentifier); nothing that
 * changes a session, unlocks or writes. Every request enters the table
 * through ecuio_engine_add, which refuses the rest, so the rule holds
 * whatever path enrols a request. */
inline bool ecuio_engine_service_allowed(uint8_t sid)
{
    return sid == 0x01 || sid == 0x09 || sid == 0x22;
}

/* §2: the request whose positive reply carries a def's value. The def's
 * mux key sits at payload offset 0 and opens that reply: a 2-byte
 * 41 <PID> asks for 01 <PID>, a 3-byte 62 <DID> for 22 <DID>. Any other
 * key requests nothing, though it still decodes; Service 09 values are
 * read once, never rotated. Writes out and *out_len only on true. */
inline bool ecuio_engine_request_for_def(const ecuio_signal_def_t* d,
                                         uint8_t out[3], uint8_t* out_len)
{
    if (d->mux_offset != 0) {
        return false;
    }
    const uint32_t k = d->mux_key;
    if (d->mux_length == 2 && (k >> 8) == 0x41u) {
        out[0] = 0x01;
        out[1] = static_cast<uint8_t>(k);
        *out_len = 2;
        return true;
    }
    if (d->mux_length == 3 && (k >> 16) == 0x62u) {
        out[0] = 0x22;
        out[1] = static_cast<uint8_t>(k >> 8);
        out[2] = static_cast<uint8_t>(k);
        *out_len = 3;
        return true;
    }
    return false;
}

/* An empty table over chan_count channels, at least one: a set that
 * declares none still counts its requests on channel 0. */
inline void ecuio_engine_init(ecuio_request_engine* e, uint32_t chan_count)
{
    *e = ecuio_request_engine{};
    e->chan_count = (chan_count == 0) ? 1
                  : (chan_count > ECUIO_MAX_CHANNELS) ? ECUIO_MAX_CHANNELS
                  : chan_count;
}

/* Adds a request to channel's rotation, after the ones already there.
 * The same request on the same channel is kept once. Refused: a service
 * §3 doesn't allow, a length other than 2 or 3, a channel past the
 * table. */
inline ecuio_engine_add_result ecuio_engine_add(ecuio_request_engine* e,
                                                uint32_t channel,
                                                const uint8_t* bytes,
                                                uint8_t len)
{
    if (len < 2 || len > 3 || channel >= e->chan_count ||
        !ecuio_engine_service_allowed(bytes[0])) {
        return ecuio_engine_add_result::refused;
    }
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (r.channel == channel && r.len == len && r.bytes[0] == bytes[0] &&
            r.bytes[1] == bytes[1] && (len < 3 || r.bytes[2] == bytes[2])) {
            return ecuio_engine_add_result::duplicate;
        }
    }
    if (e->count >= ECUIO_PROFILE_MAX_REQUESTS) {
        return ecuio_engine_add_result::full;
    }
    ecuio_request& r = e->requests[e->count++];
    r = ecuio_request{};
    for (uint8_t i = 0; i < len; ++i) {
        r.bytes[i] = bytes[i];
    }
    r.len = len;
    r.channel = static_cast<uint8_t>(channel);
    ++e->chans[channel].requests;
    return ecuio_engine_add_result::added;
}

/* The channel a def's request goes to: the one whose receive ID is the
 * def's message_id, or channel 0 when none is (a ?filter= override
 * replaces the set's channels with one of its own). */
inline uint32_t ecuio_engine_channel_for(const ecuio_signal_def_t* d,
                                         const uint32_t* rx_ids,
                                         uint32_t rx_count)
{
    for (uint32_t i = 0; i < rx_count; ++i) {
        if (rx_ids[i] == d->message_id) {
            return i;
        }
    }
    return 0;
}

/* §2: enrols every request a set's defs derive, in def order, one per
 * distinct request and channel, so several defs read from one reply
 * cost one request. rx_ids holds each channel's receive ID. True when
 * every request fit; otherwise the table holds the first
 * ECUIO_PROFILE_MAX_REQUESTS, and *overflow_def (when not NULL) names
 * the first def whose request didn't. A NULL set enrols nothing. */
inline bool ecuio_engine_derive(ecuio_request_engine* e,
                                const ecuio_signal_set_t* set,
                                const uint32_t* rx_ids, uint32_t rx_count,
                                uint32_t* overflow_def)
{
    if (set == nullptr || set->defs == nullptr) {
        return true;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (!ecuio_engine_request_for_def(&set->defs[i], req, &len)) {
            continue;
        }
        const uint32_t c = ecuio_engine_channel_for(&set->defs[i], rx_ids,
                                                    rx_count);
        if (ecuio_engine_add(e, c, req, len) ==
            ecuio_engine_add_result::full) {
            if (overflow_def != nullptr) {
                *overflow_def = i;
            }
            return false;
        }
    }
    return true;
}

/* The request channel c sends next, or -1: while one is outstanding,
 * before the floor, or when the channel has none. Moves the rotation
 * past the one it returns; ecuio_engine_sent records what became of
 * it. */
inline int32_t ecuio_engine_due(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding >= 0 || now_us < ch.next_send_us ||
        ch.requests == 0) {
        return -1;
    }
    for (uint32_t k = 0; k < e->count; ++k) {
        const uint32_t i = (ch.cursor + k) % e->count;
        if (e->requests[i].channel == c) {
            ch.cursor = (i + 1) % e->count;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* After the driver's write of request r on channel c, at now_us: a sent
 * request waits for its reply until the wait ends; one that wasn't sent
 * leaves nothing outstanding. The floor runs from now either way. */
inline void ecuio_engine_sent(ecuio_request_engine* e, uint32_t c,
                              int32_t r, bool sent, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.outstanding = sent ? r : -1;
    ch.deadline_us = now_us + ECUIO_ENGINE_WAIT_US;
    ch.next_send_us = now_us + ECUIO_ENGINE_GAP_US;
}

/* Ends channel c's request once its wait has run out: true when one
 * timed out, which is that request's outcome. */
inline bool ecuio_engine_expire(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding < 0 || now_us < ch.deadline_us) {
        return false;
    }
    ch.outstanding = -1;
    return true;
}

/* A frame received on channel c, its payload after the CAN ID (only the
 * first three bytes are read). A positive or negative reply completes
 * the outstanding request; a pending one keeps it outstanding and
 * extends its wait to the pending wait from now. *out_nrc (when not
 * NULL) gets the code of a negative or pending reply. */
inline ecuio_reply ecuio_engine_on_frame(ecuio_request_engine* e, uint32_t c,
                                         const uint8_t* data, uint16_t len,
                                         int64_t now_us, uint8_t* out_nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding < 0 || len == 0) {
        return ecuio_reply::none;
    }
    const ecuio_request& r = e->requests[ch.outstanding];
    if (len >= r.len && data[0] == static_cast<uint8_t>(r.bytes[0] + 0x40) &&
        data[1] == r.bytes[1] && (r.len < 3 || data[2] == r.bytes[2])) {
        ch.outstanding = -1;
        return ecuio_reply::positive;
    }
    if (len >= 3 && data[0] == ECUIO_NEGATIVE_RESPONSE &&
        data[1] == r.bytes[0]) {
        if (out_nrc != nullptr) {
            *out_nrc = data[2];
        }
        if (data[2] == ECUIO_NRC_RESPONSE_PENDING) {
            ch.deadline_us = now_us + ECUIO_ENGINE_PENDING_WAIT_US;
            return ecuio_reply::pending;
        }
        ch.outstanding = -1;
        return ecuio_reply::negative;
    }
    return ecuio_reply::none;
}

/* Microseconds until the engine next has something to do: a wait
 * ending, or an idle channel's floor passing; 0 when something is due
 * now, INT64_MAX when no channel has a request. */
inline int64_t ecuio_engine_next_event_us(const ecuio_request_engine* e,
                                          int64_t now_us)
{
    int64_t next = INT64_MAX;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        if (ch.requests == 0) {
            continue;
        }
        const int64_t at = (ch.outstanding >= 0) ? ch.deadline_us
                                                 : ch.next_send_us;
        const int64_t in = (at > now_us) ? at - now_us : 0;
        if (in < next) {
            next = in;
        }
    }
    return next;
}

#endif /* ECUIO_REQUEST_ENGINE_H */
