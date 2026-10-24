// File/layer: core/src/request_engine.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its driver) and
//             profile_registry.cpp (the request limit)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — derivation, deduplication and the limit, the
//     read-only rule, rotation per channel, correlation, the wait, the
//     pending wait and the floor; the supported-PID walk, its budget and
//     its resumption (behind a one-shot already taken), and what each
//     answer drops; the DID scan's turns, outcomes and rescan;
//     identification's turns, outcomes, text and report
//     (RequestEngineTest, test_obd2); the worker that drives it is
//     HARNESS (J2534AdoptionTest, J2534HealthTest), its timing on a
//     vehicle BENCH.

#ifndef ECUIO_REQUEST_ENGINE_H
#define ECUIO_REQUEST_ENGINE_H

#include "ecuio/core.h"
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
 * progress independently.
 *
 * The probe (§4) runs on the same pacing. Each channel with Service 01
 * requests first walks the supported-PID chain (01 00, then 01 20 while
 * each bitmap's last bit says the next exists, up to 01 E0), one request
 * at a time through the channel's one-shot slot, and its rotation starts
 * when the walk ends. While the probe is on, a request leaves the
 * rotation only on an explicit answer: its PID's bit clear in an answered
 * bitmap, or a negative reply that won't change within the session.
 *
 * The DID scan (ADR-0020) runs on the same slot after the walk: each
 * scanned DID is asked once, 22 <DID>, taking turns with the channel's
 * rotation so the listed DIDs keep refreshing while a range is scanned.
 * A positive reply puts the DID in the rotation (while the table has
 * room); a negative one counts it refused; one that times out is asked
 * again when the driver calls ecuio_engine_rescan, as the link recovers.
 * The scan runs whether or not the probe is on.
 *
 * Identification (ADR-0021) runs on the same slot, ahead of the scan:
 * when the driver asks for it (ecuio_engine_identify), every channel
 * asks ECUIO_IDENT_REQUESTS once each, Service 09's vehicle information
 * and the 0x22 identification DIDs, taking turns with the rotation like
 * the scan. A positive reply's data is kept as text, a negative one as
 * its code, a timeout as unanswered, asked again on ecuio_engine_rescan.
 * Nothing it learns enters the rotation. */

/* Timing (ADR-0015, appendix of 2026-10-23). The wait is the server's
 * response time P2 (50 ms) plus 50 ms for the adapter: the longest reply
 * on the first vehicle took 83 ms. A "response pending" reply extends it
 * to P2* (5 s) plus the same margin. The floor is the least time between
 * two requests on one channel, set before any vehicle has run the
 * engine. The walk's budget fits the whole chain, eight bitmaps, at a
 * full wait each; no bitmap request goes out after it. */
constexpr int64_t ECUIO_ENGINE_WAIT_US = 100000;
constexpr int64_t ECUIO_ENGINE_PENDING_WAIT_US = 5050000;
constexpr int64_t ECUIO_ENGINE_GAP_US = 20000;
constexpr int64_t ECUIO_ENGINE_PROBE_BUDGET_US = 1000000;

/* Negative responses: 7F <service> <code> (ISO 14229-1). */
constexpr uint8_t ECUIO_NEGATIVE_RESPONSE = 0x7F;
constexpr uint8_t ECUIO_NRC_SERVICE_NOT_SUPPORTED = 0x11;
constexpr uint8_t ECUIO_NRC_SUBFUNCTION_NOT_SUPPORTED = 0x12;
constexpr uint8_t ECUIO_NRC_REQUEST_OUT_OF_RANGE = 0x31;
constexpr uint8_t ECUIO_NRC_SECURITY_ACCESS_DENIED = 0x33;
constexpr uint8_t ECUIO_NRC_RESPONSE_PENDING = 0x78;
constexpr uint8_t ECUIO_NRC_NOT_IN_ACTIVE_SESSION = 0x7F;

/* ecuio_engine_due's answers besides a table index. */
constexpr int32_t ECUIO_ENGINE_NONE = -1;   /* nothing to send */
constexpr int32_t ECUIO_ENGINE_ONCE = -2;   /* the channel's one-shot */

/* A bitmap reply: 41 <PID> and four bytes, one bit per PID. */
constexpr uint8_t ECUIO_BITMAP_REPLY_LEN = 6;

/* The most DIDs a session scans, over every channel (ADR-0020): a
 * 0x100-wide block four times over, at six bytes an entry. */
constexpr uint32_t ECUIO_ENGINE_SCAN_MAX = 1024;

/* What became of a scanned DID. */
constexpr uint8_t ECUIO_SCAN_UNASKED = 0;      /* not asked yet, or asked
                                                * again after a recovery */
constexpr uint8_t ECUIO_SCAN_FOUND = 1;        /* answered positive */
constexpr uint8_t ECUIO_SCAN_REFUSED = 2;      /* answered negative */
constexpr uint8_t ECUIO_SCAN_UNANSWERED = 3;   /* its wait ran out */

/* What identification asks each channel's ECU (ADR-0021), in this
 * order: SAE J1979 Service 09's vehicle information (the VIN, the
 * calibration IDs and their verification numbers, the ECU's name), then
 * ISO 14229-1's identification DIDs (Annex C: part, software and
 * hardware numbers, the supplier, the serial, the VIN again as F190).
 * The name is what the activity log shows beside the value. */
struct ecuio_ident_request {
    uint8_t bytes[3];
    uint8_t len;
    const char* name;
};

constexpr ecuio_ident_request ECUIO_IDENT_REQUESTS[] = {
    {{0x09, 0x02, 0x00}, 2, "VIN"},
    {{0x09, 0x04, 0x00}, 2, "CALID"},
    {{0x09, 0x06, 0x00}, 2, "CVN"},
    {{0x09, 0x0A, 0x00}, 2, "ECU name"},
    {{0x22, 0xF1, 0x87}, 3, "part number"},
    {{0x22, 0xF1, 0x88}, 3, "software number"},
    {{0x22, 0xF1, 0x89}, 3, "software version"},
    {{0x22, 0xF1, 0x8A}, 3, "supplier"},
    {{0x22, 0xF1, 0x8C}, 3, "ECU serial"},
    {{0x22, 0xF1, 0x90}, 3, "VIN (F190)"},
    {{0x22, 0xF1, 0x91}, 3, "hardware number"},
    {{0x22, 0xF1, 0x94}, 3, "supplier software number"},
    {{0x22, 0xF1, 0x95}, 3, "supplier software version"},
};
constexpr uint32_t ECUIO_IDENT_COUNT =
    sizeof(ECUIO_IDENT_REQUESTS) / sizeof(ECUIO_IDENT_REQUESTS[0]);
static_assert(ECUIO_IDENT_COUNT <= ECUIO_IDENTITY_MAX_ITEMS,
              "the identification list must fit the report");

/* The reply's data starts here for both services: after 49 <item> and
 * the item count, or after 62 <DID high> <DID low>. */
constexpr uint16_t ECUIO_IDENT_DATA_OFFSET = 3;

struct ecuio_request {
    uint8_t bytes[3];   /* the service ID, then the key */
    uint8_t len;        /* 2 or 3 */
    uint8_t channel;
    uint8_t dropped;    /* 0 in the rotation, else why not (ECUIO_DROP_*) */
    uint8_t nrc;        /* the negative reply's code that dropped it */
};

struct ecuio_scan_entry {
    uint16_t did;
    uint8_t channel;
    uint8_t outcome;    /* ECUIO_SCAN_* */
    uint8_t nrc;        /* the negative reply's code, when refused */
    uint8_t polled;     /* 1 once found and in the rotation */
};

struct ecuio_engine_channel {
    int32_t outstanding = ECUIO_ENGINE_NONE;   /* a table index, the
                                                * one-shot, or none */
    int64_t deadline_us = 0;    /* when that request's wait ends */
    int64_t next_send_us = 0;   /* the floor: no request before this */
    uint32_t cursor = 0;        /* where the rotation resumes */
    uint32_t requests = 0;      /* this channel's requests in the rotation */
    bool in_step = true;        /* no request has timed out since the last
                                 * positive reply: see ecuio_engine_on_frame */
    /* The one-shot: what goes out outside the rotation, one at a time,
     * whoever it's for (the walk's next bitmap, an identification item,
     * a scanned DID). identifying and scanning say it's taken; the walk
     * keeps its own next request below, so starting a walk never touches
     * a slot that's taken. */
    ecuio_request once = {};
    /* The supported-PID walk. Bits for PID p sit as the bitmaps carry
     * them: bit 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8. */
    bool walking = false;       /* the walk has a bitmap to ask */
    ecuio_request walk = {};    /* that bitmap; the one-shot copies it */
    bool probed = false;        /* a walk has started */
    bool walked = false;        /* the chain was answered to its end */
    uint8_t ranges = 0;         /* bit i: the bitmap at 0x20 * i answered */
    uint8_t known[32] = {};     /* PIDs whose support the ECU stated */
    uint8_t supported[32] = {}; /* of those, the ones it supports */
    /* The DID scan: the one-shot carries a scan entry while scanning. */
    bool scanning = false;      /* the one-shot is a scanned DID */
    bool scan_turn = false;     /* the scan sends next, not the rotation */
    uint32_t scan_cursor = 0;   /* where the scan looks for its next entry */
    uint32_t scan_index = 0;    /* the entry out, while scanning */
    uint32_t scan_pending = 0;  /* this channel's unasked entries */
    uint32_t generation = 0;    /* bumped whenever the above, or a
                                 * request's dropped, changes */
    /* Identification: the one-shot carries an item while identifying. */
    ecuio_identity_item_t ident[ECUIO_IDENTITY_MAX_ITEMS] = {};
    uint32_t ident_count = 0;   /* items in ident; 0 = not identifying */
    uint32_t ident_pending = 0; /* unasked items */
    bool identifying = false;   /* the one-shot is an item */
    uint32_t ident_index = 0;   /* the item out, while identifying */
    uint32_t ident_generation = 0;   /* bumped as each item settles */
};

struct ecuio_request_engine {
    ecuio_request requests[ECUIO_PROFILE_MAX_REQUESTS] = {};
    uint32_t count = 0;
    ecuio_engine_channel chans[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
    bool probe = false;         /* §4 on: walks run, explicit answers drop */
    int64_t walk_until_us = 0;  /* the walk's budget ends */
    ecuio_scan_entry scan[ECUIO_ENGINE_SCAN_MAX] = {};
    uint32_t scan_count = 0;
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

/* §4: what a negative reply's code says of its request, as
 * ECUIO_DROP_*: NOT_SUPPORTED for 0x12 (the first vehicle's answer to an
 * unsupported PID) and 0x31; SERVICE for 0x11; ACCESS for 0x33 and 0x7F,
 * since the engine never unlocks or changes the session; 0 for any code
 * that can pass, like 0x22 (conditions not correct) or 0x21 (busy). */
inline uint8_t ecuio_engine_drop_reason(uint8_t nrc)
{
    switch (nrc) {
    case ECUIO_NRC_SUBFUNCTION_NOT_SUPPORTED:
    case ECUIO_NRC_REQUEST_OUT_OF_RANGE:
        return ECUIO_DROP_NOT_SUPPORTED;
    case ECUIO_NRC_SERVICE_NOT_SUPPORTED:
        return ECUIO_DROP_SERVICE;
    case ECUIO_NRC_SECURITY_ACCESS_DENIED:
    case ECUIO_NRC_NOT_IN_ACTIVE_SESSION:
        return ECUIO_DROP_ACCESS;
    default:
        return 0;
    }
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
 * declares none still counts its requests on channel 0. The probe is
 * off until ecuio_engine_probe. */
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

/* The request r names on channel c: a table index, or the one-shot. */
inline const ecuio_request& ecuio_engine_request_of(
    const ecuio_request_engine* e, uint32_t c, int32_t r)
{
    return (r == ECUIO_ENGINE_ONCE) ? e->chans[c].once : e->requests[r];
}

/* Adds did to channel's scan (ADR-0020): asked once, 22 <DID>, outside
 * the rotation. The same DID on the same channel is kept once. False
 * when the scan is full or the channel is past the table. */
inline bool ecuio_engine_scan_add(ecuio_request_engine* e, uint32_t channel,
                                  uint16_t did)
{
    if (channel >= e->chan_count) {
        return false;
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        if (e->scan[i].channel == channel && e->scan[i].did == did) {
            return true;
        }
    }
    if (e->scan_count >= ECUIO_ENGINE_SCAN_MAX) {
        return false;
    }
    ecuio_scan_entry& s = e->scan[e->scan_count++];
    s = ecuio_scan_entry{};
    s.did = did;
    s.channel = static_cast<uint8_t>(channel);
    ++e->chans[channel].scan_pending;
    return true;
}

/* The next unasked scan entry of channel c, from its cursor round the
 * list; -1 when there is none. */
inline int32_t ecuio_engine_scan_next(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t k = 0; k < e->scan_count; ++k) {
        const uint32_t i = (ch.scan_cursor + k) % e->scan_count;
        if (e->scan[i].channel == c && e->scan[i].outcome == ECUIO_SCAN_UNASKED) {
            ch.scan_cursor = (i + 1) % e->scan_count;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* Settles the scan entry channel c has out: found (and in the rotation
 * while the table has room), refused with its code, or unanswered. */
inline void ecuio_engine_scan_settle(ecuio_request_engine* e, uint32_t c,
                                     uint8_t outcome, uint8_t nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_scan_entry& s = e->scan[ch.scan_index];
    ch.scanning = false;
    s.outcome = outcome;
    s.nrc = nrc;
    if (outcome == ECUIO_SCAN_FOUND) {
        const uint8_t req[3] = {0x22, static_cast<uint8_t>(s.did >> 8),
                                static_cast<uint8_t>(s.did)};
        const ecuio_engine_add_result r = ecuio_engine_add(e, c, req, 3);
        s.polled = (r == ecuio_engine_add_result::added ||
                    r == ecuio_engine_add_result::duplicate) ? 1 : 0;
    }
    if (ch.scan_pending > 0) {
        --ch.scan_pending;
    }
    ++ch.generation;
}

/* Asks every unanswered scan entry and identification item again: the
 * driver's call when the link returns to ok, as the walk resumes.
 * Silence wasn't an answer. */
inline void ecuio_engine_rescan(ecuio_request_engine* e)
{
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        if (s.outcome == ECUIO_SCAN_UNANSWERED) {
            s.outcome = ECUIO_SCAN_UNASKED;
            ++e->chans[s.channel].scan_pending;
        }
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        for (uint32_t i = 0; i < ch.ident_count; ++i) {
            if (ch.ident[i].outcome == ECUIO_IDENTITY_UNANSWERED) {
                ch.ident[i].outcome = ECUIO_IDENTITY_UNASKED;
                ++ch.ident_pending;
            }
        }
    }
}

/* Identification (ADR-0021): gives every channel the list to ask, each
 * item once, through the one-shot slot ahead of the scan. Idempotent:
 * a channel already holding the list keeps its answers. */
inline void ecuio_engine_identify(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.ident_count > 0) {
            continue;
        }
        for (uint32_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
            ecuio_identity_item_t& it = ch.ident[i];
            it = ecuio_identity_item_t{};
            const ecuio_ident_request& q = ECUIO_IDENT_REQUESTS[i];
            for (uint8_t k = 0; k < q.len; ++k) {
                it.request[k] = q.bytes[k];
            }
            it.length = q.len;
        }
        ch.ident_count = ECUIO_IDENT_COUNT;
        ch.ident_pending = ECUIO_IDENT_COUNT;
    }
}

/* The next unasked item of channel c, in list order; -1 when none. */
inline int32_t ecuio_engine_ident_next(const ecuio_request_engine* e,
                                       uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        if (ch.ident[i].outcome == ECUIO_IDENTITY_UNASKED) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* An identification reply's data as the report shows it
 * (ecuio_identity_item_t::text): ASCII when every byte is printable or
 * padding, the printable runs joined by ", " and trailing blanks
 * dropped; else hex pairs. Writes at most cap bytes, NUL included, cut
 * at a whole pair; cap is at least 1. Returns the text's length. */
inline uint8_t ecuio_identity_format(const uint8_t* data, uint32_t len,
                                     char* out, uint32_t cap)
{
    bool printable = false;
    bool ascii = true;
    for (uint32_t i = 0; i < len; ++i) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) {
            printable = true;
        } else if (data[i] != 0x00 && data[i] != 0xFF) {
            ascii = false;
        }
    }
    uint32_t w = 0;
    const uint32_t room = cap - 1;
    if (ascii && printable) {
        bool started = false;
        bool gap = false;
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t b = data[i];
            if (b < 0x20 || b > 0x7E) {
                gap = true;
                continue;
            }
            if (started && gap) {
                if (w + 2 > room) {
                    break;
                }
                out[w++] = ',';
                out[w++] = ' ';
            }
            if (w + 1 > room) {
                break;
            }
            out[w++] = static_cast<char>(b);
            started = true;
            gap = false;
        }
        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == ',')) {
            --w;
        }
    } else {
        static const char kHex[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < len; ++i) {
            const uint32_t need = (w == 0) ? 2u : 3u;
            if (w + need > room) {
                break;
            }
            if (w > 0) {
                out[w++] = ' ';
            }
            out[w++] = kHex[data[i] >> 4];
            out[w++] = kHex[data[i] & 0x0F];
        }
    }
    out[w] = '\0';
    return static_cast<uint8_t>(w);
}

/* Settles the item channel c has out: answered, with the reply's data
 * (the whole frame, from which the text is taken past the header),
 * refused with its code, or unanswered. */
inline void ecuio_engine_ident_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_identity_item_t& it = ch.ident[ch.ident_index];
    ch.identifying = false;
    it.outcome = outcome;
    it.nrc = nrc;
    it.text[0] = '\0';
    it.text_length = 0;
    if (outcome == ECUIO_IDENTITY_ANSWERED && len > ECUIO_IDENT_DATA_OFFSET) {
        it.text_length = ecuio_identity_format(
            data + ECUIO_IDENT_DATA_OFFSET,
            static_cast<uint32_t>(len - ECUIO_IDENT_DATA_OFFSET),
            it.text, sizeof(it.text));
    }
    if (ch.ident_pending > 0) {
        --ch.ident_pending;
    }
    ++ch.ident_generation;
}

/* Takes table entry i out of the rotation, for reason (ECUIO_DROP_*). */
inline void ecuio_engine_drop(ecuio_request_engine* e, uint32_t i,
                              uint8_t reason, uint8_t nrc)
{
    ecuio_request& r = e->requests[i];
    if (r.dropped != 0) {
        return;
    }
    r.dropped = reason;
    r.nrc = nrc;
    ecuio_engine_channel& ch = e->chans[r.channel];
    --ch.requests;
    ++ch.generation;
}

inline bool ecuio_engine_pid_bit(const uint8_t* bits, uint8_t pid)
{
    return (bits[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) != 0;
}

/* Ends channel c's walk where it stands, answered or not, and drops each
 * of its Service 01 requests whose PID the answers mark unsupported.
 * PIDs it didn't reach stay: silence isn't an answer. */
inline void ecuio_engine_end_walk(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.walking = false;
    ++ch.generation;
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (r.channel == c && r.dropped == 0 && r.bytes[0] == 0x01 &&
            r.bytes[1] != 0 && ecuio_engine_pid_bit(ch.known, r.bytes[1]) &&
            !ecuio_engine_pid_bit(ch.supported, r.bytes[1])) {
            ecuio_engine_drop(e, i, ECUIO_DROP_BITMAP, 0);
        }
    }
}

/* A bitmap reply to the walk on channel c: 41 <PID> b0 b1 b2 b3, PID
 * 0x20 * i covering PIDs 0x20 * i + 1 to 0x20 * i + 0x20, the last of
 * them the next bitmap. A clear last bit ends the chain, and with it
 * says every later PID is unsupported. */
inline void ecuio_engine_take_bitmap(ecuio_request_engine* e, uint32_t c,
                                     const uint8_t* data, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    const uint32_t range = ch.walk.bytes[1] / 0x20u;
    for (uint32_t b = 0; b < 4; ++b) {
        ch.known[range * 4 + b] = 0xFF;
        ch.supported[range * 4 + b] = data[2 + b];
    }
    ch.ranges = static_cast<uint8_t>(ch.ranges | (1u << range));
    const bool next = (data[5] & 0x01u) != 0;
    if (!next) {
        for (uint32_t b = (range + 1) * 4; b < sizeof(ch.known); ++b) {
            ch.known[b] = 0xFF;
        }
    }
    if (!next || range == 7) {
        ch.walked = true;
        ecuio_engine_end_walk(e, c);
        return;
    }
    ch.walk.bytes[1] = static_cast<uint8_t>((range + 1) * 0x20u);
    if (now_us >= e->walk_until_us) {
        ecuio_engine_end_walk(e, c);
    }
}

/* §4: turns the probe on and starts the walk on every channel that has
 * Service 01 requests in its rotation and whose chain isn't answered to
 * its end yet. A walk resumes at the first bitmap it hasn't had, so the
 * one at connect and each after a recovery (the driver's call) together
 * ask each bitmap until it's answered. The budget starts now. A channel
 * whose one-shot is taken by an identification item or a scanned DID
 * keeps it: the walk waits in ch.walk until that one has settled
 * (ecuio_engine_due). */
inline void ecuio_engine_probe(ecuio_request_engine* e, int64_t now_us)
{
    e->probe = true;
    e->walk_until_us = now_us + ECUIO_ENGINE_PROBE_BUDGET_US;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.walked || ch.walking) {
            continue;
        }
        bool asks_pids = false;
        for (uint32_t i = 0; i < e->count && !asks_pids; ++i) {
            const ecuio_request& r = e->requests[i];
            asks_pids = r.channel == c && r.dropped == 0 && r.bytes[0] == 0x01;
        }
        if (!asks_pids) {
            continue;
        }
        uint32_t range = 0;
        while (range < 8 && (ch.ranges & (1u << range)) != 0) {
            ++range;
        }
        if (range == 8) {
            continue;   /* every bitmap answered, so walked is set */
        }
        ch.walk = ecuio_request{};
        ch.walk.bytes[0] = 0x01;
        ch.walk.bytes[1] = static_cast<uint8_t>(range * 0x20u);
        ch.walk.len = 2;
        ch.walk.channel = static_cast<uint8_t>(c);
        ch.walking = true;
        ch.probed = true;
    }
}

/* What channel c sends next: ECUIO_ENGINE_ONCE for an identification
 * item or a scanned DID already in the one-shot (its write failed), then
 * while its walk has a bitmap to ask, then, taking turns with the
 * rotation, for the next identification item, then for the next scanned
 * DID, else the next request of its rotation, or ECUIO_ENGINE_NONE while
 * one is outstanding, before the floor, or when there is nothing to
 * send. A walk out of budget ends here. Moves the rotation past the
 * request it returns; ecuio_engine_sent records what became of it. */
inline int32_t ecuio_engine_due(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding != ECUIO_ENGINE_NONE || now_us < ch.next_send_us) {
        return ECUIO_ENGINE_NONE;
    }
    /* Taken before a probe started the walk: it goes first, so its reply
     * settles it and no other request's reply does. */
    if (ch.identifying || ch.scanning) {
        return ECUIO_ENGINE_ONCE;
    }
    if (ch.walking) {
        if (now_us < e->walk_until_us) {
            ch.once = ch.walk;
            return ECUIO_ENGINE_ONCE;
        }
        ecuio_engine_end_walk(e, c);
    }
    const bool once_turn = ch.requests == 0 || ch.scan_turn;
    if (ch.ident_pending > 0 && once_turn) {
        const int32_t i = ecuio_engine_ident_next(e, c);
        if (i >= 0) {
            const ecuio_identity_item_t& it = ch.ident[i];
            ch.once = ecuio_request{};
            for (uint8_t k = 0; k < it.length; ++k) {
                ch.once.bytes[k] = it.request[k];
            }
            ch.once.len = it.length;
            ch.once.channel = static_cast<uint8_t>(c);
            ch.identifying = true;
            ch.ident_index = static_cast<uint32_t>(i);
            ch.scan_turn = false;
            return ECUIO_ENGINE_ONCE;
        }
        ch.ident_pending = 0;   /* the count had drifted; nothing is unasked */
    }
    if (ch.scan_pending > 0 && once_turn) {
        const int32_t i = ecuio_engine_scan_next(e, c);
        if (i >= 0) {
            const ecuio_scan_entry& s = e->scan[i];
            ch.once = ecuio_request{};
            ch.once.bytes[0] = 0x22;
            ch.once.bytes[1] = static_cast<uint8_t>(s.did >> 8);
            ch.once.bytes[2] = static_cast<uint8_t>(s.did);
            ch.once.len = 3;
            ch.once.channel = static_cast<uint8_t>(c);
            ch.scanning = true;
            ch.scan_index = static_cast<uint32_t>(i);
            ch.scan_turn = false;
            return ECUIO_ENGINE_ONCE;
        }
        ch.scan_pending = 0;   /* the count had drifted; nothing is unasked */
    }
    ch.scan_turn = true;
    if (ch.requests == 0) {
        return ECUIO_ENGINE_NONE;
    }
    for (uint32_t k = 0; k < e->count; ++k) {
        const uint32_t i = (ch.cursor + k) % e->count;
        if (e->requests[i].channel == c && e->requests[i].dropped == 0) {
            ch.cursor = (i + 1) % e->count;
            return static_cast<int32_t>(i);
        }
    }
    return ECUIO_ENGINE_NONE;
}

/* After the driver's write of request r on channel c, at now_us: a sent
 * request waits for its reply until the wait ends; one that wasn't sent
 * leaves nothing outstanding, and a one-shot stays due. The floor runs
 * from now either way. */
inline void ecuio_engine_sent(ecuio_request_engine* e, uint32_t c,
                              int32_t r, bool sent, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.outstanding = sent ? r : ECUIO_ENGINE_NONE;
    ch.deadline_us = now_us + ECUIO_ENGINE_WAIT_US;
    ch.next_send_us = now_us + ECUIO_ENGINE_GAP_US;
}

/* Ends channel c's request once its wait has run out: true when one
 * timed out, which is that request's outcome. Its reply may still come,
 * so the channel is out of step until a positive reply. A bitmap that
 * times out ends the walk; a scanned DID or an identification item that
 * does is unanswered. */
inline bool ecuio_engine_expire(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding == ECUIO_ENGINE_NONE || now_us < ch.deadline_us) {
        return false;
    }
    const bool once = ch.outstanding == ECUIO_ENGINE_ONCE;
    ch.outstanding = ECUIO_ENGINE_NONE;
    ch.in_step = false;
    if (once && ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_UNANSWERED, 0,
                                  nullptr, 0);
    } else if (once && ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_UNANSWERED, 0);
    } else if (once) {
        ecuio_engine_end_walk(e, c);
    }
    return true;
}

/* A frame received on channel c, its payload after the CAN ID (at most
 * the first six bytes are read, except that an identification reply's
 * data becomes the item's text). A positive or negative reply completes
 * the outstanding request; a pending one keeps it outstanding and
 * extends its wait to the pending wait from now. A bitmap carries on the
 * walk; while the probe is on, a negative reply drops what its code says
 * (ecuio_engine_drop_reason): its request, or for 0x11 every request of
 * its service on the channel. A negative reply names the service but not
 * the key, so one that came late, after its request timed out, would
 * name the request now outstanding: it drops only while the channel is
 * in step. An ECU answers in order, so a positive reply, which carries
 * the key, puts the channel back in step. A scanned DID is settled by
 * either reply, in step or not: its answer counts once, and a late
 * negative reply can at worst count the DID out now as refused, while
 * the reply that was really its own still decodes through its def. An
 * identification item is settled the same way. *out_nrc (when not NULL)
 * gets the code of a negative or pending reply. */
inline ecuio_reply ecuio_engine_on_frame(ecuio_request_engine* e, uint32_t c,
                                         const uint8_t* data, uint16_t len,
                                         int64_t now_us, uint8_t* out_nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding == ECUIO_ENGINE_NONE || len == 0) {
        return ecuio_reply::none;
    }
    const int32_t out = ch.outstanding;
    const ecuio_request& r = ecuio_engine_request_of(e, c, out);
    if (len >= r.len && data[0] == static_cast<uint8_t>(r.bytes[0] + 0x40) &&
        data[1] == r.bytes[1] && (r.len < 3 || data[2] == r.bytes[2])) {
        ch.outstanding = ECUIO_ENGINE_NONE;
        ch.in_step = true;
        if (out == ECUIO_ENGINE_ONCE && ch.identifying) {
            ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_ANSWERED, 0,
                                      data, len);
        } else if (out == ECUIO_ENGINE_ONCE && ch.scanning) {
            ecuio_engine_scan_settle(e, c, ECUIO_SCAN_FOUND, 0);
        } else if (out == ECUIO_ENGINE_ONCE) {
            if (len >= ECUIO_BITMAP_REPLY_LEN) {
                ecuio_engine_take_bitmap(e, c, data, now_us);
            } else {
                ecuio_engine_end_walk(e, c);
            }
        }
        return ecuio_reply::positive;
    }
    if (len < 3 || data[0] != ECUIO_NEGATIVE_RESPONSE ||
        data[1] != r.bytes[0]) {
        return ecuio_reply::none;
    }
    const uint8_t code = data[2];
    if (out_nrc != nullptr) {
        *out_nrc = code;
    }
    if (code == ECUIO_NRC_RESPONSE_PENDING) {
        ch.deadline_us = now_us + ECUIO_ENGINE_PENDING_WAIT_US;
        return ecuio_reply::pending;
    }
    ch.outstanding = ECUIO_ENGINE_NONE;
    const uint8_t service = r.bytes[0];
    const uint8_t reason = (e->probe && ch.in_step)
                               ? ecuio_engine_drop_reason(code) : 0;
    if (reason == ECUIO_DROP_SERVICE) {
        for (uint32_t i = 0; i < e->count; ++i) {
            if (e->requests[i].channel == c &&
                e->requests[i].bytes[0] == service) {
                ecuio_engine_drop(e, i, reason, code);
            }
        }
    } else if (reason != 0 && out != ECUIO_ENGINE_ONCE) {
        ecuio_engine_drop(e, static_cast<uint32_t>(out), reason, code);
    }
    if (out == ECUIO_ENGINE_ONCE && ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_REFUSED, code,
                                  nullptr, 0);
    } else if (out == ECUIO_ENGINE_ONCE && ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_REFUSED, code);
    } else if (out == ECUIO_ENGINE_ONCE) {
        ecuio_engine_end_walk(e, c);
    }
    return ecuio_reply::negative;
}

/* Microseconds until the engine next has something to do: a wait
 * ending, or a channel's floor passing while it has something to send;
 * 0 when something is due now, INT64_MAX when nothing is. */
inline int64_t ecuio_engine_next_event_us(const ecuio_request_engine* e,
                                          int64_t now_us)
{
    int64_t next = INT64_MAX;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        if (ch.outstanding == ECUIO_ENGINE_NONE && ch.requests == 0 &&
            !ch.walking && ch.scan_pending == 0 && ch.ident_pending == 0) {
            continue;
        }
        const int64_t at = (ch.outstanding != ECUIO_ENGINE_NONE)
                               ? ch.deadline_us : ch.next_send_us;
        const int64_t in = (at > now_us) ? at - now_us : 0;
        if (in < next) {
            next = in;
        }
    }
    return next;
}

/* What channel c has learned, for ecuio_session_get_channel_support;
 * the driver adds the channel's rx_id. */
inline void ecuio_engine_support(const ecuio_request_engine* e, uint32_t c,
                                 ecuio_channel_support_t* out)
{
    const ecuio_engine_channel& ch = e->chans[c];
    *out = ecuio_channel_support_t{};
    out->flags = (ch.probed ? ECUIO_SUPPORT_PROBED : 0u) |
                 (ch.walked ? ECUIO_SUPPORT_COMPLETE : 0u);
    out->generation = ch.generation;
    out->ranges = ch.ranges;
    for (uint32_t b = 0; b < sizeof(out->pid_bitmap); ++b) {
        out->pid_bitmap[b] = ch.supported[b];
    }
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (r.channel != c) {
            continue;
        }
        ++out->requests;
        if (r.dropped == 0) {
            continue;
        }
        if (out->dropped < ECUIO_SUPPORT_MAX_DROPPED) {
            ecuio_dropped_request_t& d = out->dropped_list[out->dropped];
            for (uint8_t k = 0; k < r.len; ++k) {
                d.request[k] = r.bytes[k];
            }
            d.length = r.len;
            d.reason = r.dropped;
            d.nrc = r.nrc;
        }
        ++out->dropped;
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        const ecuio_scan_entry& s = e->scan[i];
        if (s.channel != c) {
            continue;
        }
        ++out->scan_total;
        if (s.outcome == ECUIO_SCAN_FOUND) {
            ++out->scan_done;
            ++out->scan_found;
            out->scan_polled = static_cast<uint16_t>(out->scan_polled + s.polled);
        } else if (s.outcome == ECUIO_SCAN_REFUSED) {
            ++out->scan_done;
            ++out->scan_refused;
        }
    }
    if (out->scan_total > 0 && out->scan_done == out->scan_total) {
        out->flags |= ECUIO_SUPPORT_SCAN_COMPLETE;
    }
}

/* What channel c's ECU has said it is, for
 * ecuio_session_get_channel_identity; the driver adds the channel's
 * rx_id. Complete once every item has an outcome. */
inline void ecuio_engine_identity(const ecuio_request_engine* e, uint32_t c,
                                  ecuio_channel_identity_t* out)
{
    const ecuio_engine_channel& ch = e->chans[c];
    *out = ecuio_channel_identity_t{};
    out->generation = ch.ident_generation;
    out->items = ch.ident_count;
    bool complete = ch.ident_count > 0;
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        out->list[i] = ch.ident[i];
        complete = complete && ch.ident[i].outcome != ECUIO_IDENTITY_UNASKED;
    }
    if (complete) {
        out->flags |= ECUIO_IDENTITY_COMPLETE;
    }
}

#endif /* ECUIO_REQUEST_ENGINE_H */
