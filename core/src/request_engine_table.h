// File/layer: core/src/request_engine_table.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The request table: init, a pick-only table, the check, enrolment
// from a set's defs and the plan that sizes it.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestTableTest, RequestEngineTest (test_obd2),
//          CoreTest, PickOnlyTest (test_core)

#ifndef ECUIO_REQUEST_ENGINE_TABLE_H
#define ECUIO_REQUEST_ENGINE_TABLE_H

#include "request_engine_services.h"

/* Section 2: the request whose positive reply carries a def's value. The def's
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

/* An empty table of room rows over chan_count channels, at least one: a
 * set that declares none still counts its requests on channel 0. The
 * probe is off until ecuio_engine_probe. Allocates the channels, the
 * table and what is sized with it, so it is attach's to call, never a
 * worker's; false, with no table and nothing thrown, when they don't
 * allocate. */
inline bool ecuio_engine_init(ecuio_request_engine* e, uint32_t chan_count,
                              uint32_t room = ECUIO_PROFILE_MAX_REQUESTS)
{
    *e = ecuio_request_engine{};
    const uint32_t count = (chan_count == 0) ? 1
                         : (chan_count > ECUIO_MAX_CHANNELS) ? ECUIO_MAX_CHANNELS
                         : chan_count;
    const uint32_t pool = room < ECUIO_PROFILE_MAX_REQUESTS
                              ? room
                              : uint32_t{ECUIO_PROFILE_MAX_REQUESTS};
    try {
        e->chans.resize(count);
        e->requests.resize(room);
        e->timing_of.assign(room, ECUIO_TIMING_NONE);
        e->timing_rows.assign(pool, 0);
        e->marks.assign(room, 0);
        e->asked_us.assign(room, 0);
    } catch (...) {
        *e = ecuio_request_engine{};
        return false;
    }
    e->chan_count = count;
    return true;
}

/* The table asks only what a focus keeps (ADR-0065 section 2), for a set
 * that derives more requests than the pool holds: every request joins
 * parked and takes its pool row when first kept, and the focus's end
 * parks them all again. Attach's call, on an empty table. */
inline void ecuio_engine_pick_only(ecuio_request_engine* e)
{
    e->pick_only = true;
    e->focused = true;
}

/* A table that asks only its picks, with nothing picked: its silence is
 * the caller's choice and says nothing about the link, so a worker's
 * idle pass counts no quiet read. Picks that were all dropped are
 * another matter: those were asked, and count as any table's. */
inline bool ecuio_engine_unpicked(const ecuio_request_engine* e)
{
    return e->pick_only && e->kept == 0;
}

/* Such a table while some ECU answers: a wait that ran out then says
 * its own module is silent (absent from this car, or asleep), not the
 * vehicle, and the driver counts it toward no timeout. With a rotation
 * the other modules' replies between two timeouts say the same. */
inline bool ecuio_engine_unpicked_answering(const ecuio_request_engine* e)
{
    return ecuio_engine_unpicked(e) && e->answering != 0;
}
static_assert(ECUIO_MAX_CHANNELS <= 32, "answering has a bit for each channel");

/* The request channel c's next check asks: its first that isn't
 * dropped from check_from on, around the table once. check_from is the
 * row after the channel's last whose wait ran out (ecuio_engine_expire):
 * a request the ECU is silent on isn't the only one ever tried, and one
 * answered, never sent or taken back is asked again. ECUIO_ENGINE_NONE
 * with none. */
inline int32_t ecuio_engine_check_row(const ecuio_request_engine* e, uint32_t c)
{
    for (uint32_t k = 0; k < e->count; ++k) {
        const uint32_t i = (e->chans[c].check_from + k) % e->count;
        if (e->requests[i].channel == c && e->requests[i].dropped == 0) {
            return static_cast<int32_t>(i);
        }
    }
    return ECUIO_ENGINE_NONE;
}

/* Has channel c ask one request once (ecuio_engine_check_row), parked
 * or not and outside the rotation: the driver's check that an ECU
 * answers, for a table that asks nothing (ecuio_engine_unpicked) while
 * the link is down. Its reply is the request's own, timed and decoded
 * as any. A pick or the link's recovery before it goes out withdraws
 * it. False, nothing changed, for a channel with no such request. */
inline bool ecuio_engine_ask_check(ecuio_request_engine* e, uint32_t c)
{
    if (c >= e->chan_count || ecuio_engine_check_row(e, c) < 0) {
        return false;
    }
    e->chans[c].check_due = true;
    return true;
}

/* Table row i's pool row, taken as the row first enters the rotation;
 * with the pool spent the row goes without one for good, counted once
 * in untimed */
inline void ecuio_engine_take_row(ecuio_request_engine* e, uint32_t i)
{
    if (e->timing_of[i] != ECUIO_TIMING_NONE) {
        return;
    }
    if (e->timing_taken >= e->timing_rows.size()) {
        e->timing_of[i] = ECUIO_TIMING_SPENT;
        ++e->untimed;
        return;
    }
    e->timing_of[i] = static_cast<uint16_t>(e->timing_taken);
    e->timing_rows[e->timing_taken++] = i;
}

/* A new row at the table's end, whatever is there already: add's, and
 * derive's when it knows the request is new. The caller checked the
 * request and the room. */
inline void ecuio_engine_append(ecuio_request_engine* e, uint32_t channel,
                                const uint8_t* bytes, uint8_t len)
{
    ecuio_request& r = e->requests[e->count];
    r = ecuio_request{};
    for (uint8_t i = 0; i < len; ++i) {
        r.bytes[i] = bytes[i];
    }
    r.len = len;
    r.channel = static_cast<uint8_t>(channel);
    /* Under a focus a request added later (a scanned DID found) wasn't
     * there when the focus resolved its picks: it joins parked, and
     * focus_joined has the driver resolve them again, which unparks it
     * when a pick names it */
    r.parked = e->focused ? 1 : 0;
    if (!e->pick_only) {
        ecuio_engine_take_row(e, e->count);
    }
    ++e->count;
    if (r.parked == 0) {
        ++e->chans[channel].requests;
    } else {
        e->focus_joined = true;
    }
}

/* Whether the table takes one more row: room in it, and, unless it asks
 * only its picks, a pool row for the request, which it takes as it joins
 * so that the focus's end can ask every row */
inline bool ecuio_engine_has_room(const ecuio_request_engine* e)
{
    return e->count < e->requests.size() &&
           (e->pick_only || e->timing_taken < e->timing_rows.size());
}

/* Adds a request to channel's rotation, after the ones already there.
 * The same request on the same channel is kept once. Refused: a service
 * section 3 doesn't allow, a length other than 2 or 3, a channel past the
 * table. Full: no room (ecuio_engine_has_room). */
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
    if (!ecuio_engine_has_room(e)) {
        return ecuio_engine_add_result::full;
    }
    ecuio_engine_append(e, channel, bytes, len);
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

/* What a set's defs derive, worked out with no table (ecuio_engine_plan) */
struct ecuio_derived {
    uint32_t total = 0; /* distinct requests, every module's */
    /* Each channel's; a set with no channel counts on channel 0 */
    uint32_t module[ECUIO_MAX_CHANNELS] = {};
    /* The first def whose request is past its module's
     * ECUIO_MODULE_MAX_REQUESTS; ECUIO_ROW_NONE when none is */
    uint32_t overflow_def = ECUIO_ROW_NONE;
};

enum class ecuio_plan_result { ok, module_full, out_of_memory };

/* A request and its channel as one number, for the plan's lookup: 5
 * bits of channel, then Service 22's DID or Service 01's PID */
inline uint32_t ecuio_engine_request_code(uint32_t channel, const uint8_t* req,
                                          uint8_t len)
{
    const uint32_t key = len == 3 ? 0x10000u | (uint32_t{req[1]} << 8) | req[2]
                                  : uint32_t{req[1]};
    return (channel << 17) | key;
}
static_assert(ECUIO_MAX_CHANNELS <= 32,
              "a request's code holds 5 bits of channel");

/* Section 2, with no table: counts the requests a set's defs derive, one
 * for each distinct request and channel, in def order. rows, when not
 * NULL, holds one entry for each def and gets the place of the def's
 * request among them, the table row ecuio_engine_derive_planned gives
 * it, or ECUIO_ROW_NONE for a def that derives none. module_full, with
 * the def named, at the first request past its module's
 * ECUIO_MODULE_MAX_REQUESTS: the count stops there and rows holds
 * nothing to read. A NULL set derives nothing. Allocates (a list sorted
 * by request, so its cost grows as n log n), so never a worker's call. */
inline ecuio_plan_result ecuio_engine_plan(const ecuio_signal_set_t* set,
                                           const uint32_t* rx_ids,
                                           uint32_t rx_count, uint32_t* rows,
                                           ecuio_derived* out)
{
    *out = ecuio_derived{};
    if (set == nullptr || set->defs == nullptr) {
        return ecuio_plan_result::ok;
    }
    try {
        /* Each asking def under its request's code, sorted so the defs
         * of one request sit together, the first of them first */
        std::vector<std::pair<uint32_t, uint32_t>> asked;
        asked.reserve(set->count);
        std::vector<uint32_t> own;
        if (rows == nullptr) {
            own.resize(set->count);
            rows = own.data();
        }
        for (uint32_t i = 0; i < set->count; ++i) {
            rows[i] = ECUIO_ROW_NONE;
            uint8_t req[3] = {};
            uint8_t len = 0;
            if (!ecuio_engine_request_for_def(&set->defs[i], req, &len)) {
                continue;
            }
            const uint32_t c =
                ecuio_engine_channel_for(&set->defs[i], rx_ids, rx_count);
            if (c < ECUIO_MAX_CHANNELS) {
                asked.emplace_back(ecuio_engine_request_code(c, req, len), i);
            }
        }
        std::sort(asked.begin(), asked.end());
        /* rows[def] is first the def its request is first met at, then,
         * walking the defs in order, that def's row: a first def always
         * comes before the defs sharing its request */
        for (size_t k = 0; k < asked.size(); ++k) {
            const bool first = k == 0 || asked[k].first != asked[k - 1].first;
            rows[asked[k].second] =
                first ? asked[k].second : rows[asked[k - 1].second];
        }
        for (uint32_t i = 0; i < set->count; ++i) {
            if (rows[i] == ECUIO_ROW_NONE) {
                continue;
            }
            if (rows[i] != i) {
                rows[i] = rows[rows[i]];
                continue;
            }
            const uint32_t c =
                ecuio_engine_channel_for(&set->defs[i], rx_ids, rx_count);
            if (out->module[c] >= ECUIO_MODULE_MAX_REQUESTS) {
                out->overflow_def = i;
                return ecuio_plan_result::module_full;
            }
            ++out->module[c];
            rows[i] = out->total++;
        }
    } catch (...) {
        return ecuio_plan_result::out_of_memory;
    }
    return ecuio_plan_result::ok;
}

/* ecuio_engine_derive for an empty table and a set ecuio_engine_plan
 * gave rows for: each def whose row is the table's next enrols its
 * request, so the table is derive's without add's search of it, which
 * grows with the square of a car-sized set. False when the table's room
 * ran out first. */
inline bool ecuio_engine_derive_planned(ecuio_request_engine* e,
                                        const ecuio_signal_set_t* set,
                                        const uint32_t* rx_ids,
                                        uint32_t rx_count, const uint32_t* rows)
{
    if (set == nullptr || set->defs == nullptr) {
        return true;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (rows[i] != e->count ||
            !ecuio_engine_request_for_def(&set->defs[i], req, &len)) {
            continue;
        }
        const uint32_t c =
            ecuio_engine_channel_for(&set->defs[i], rx_ids, rx_count);
        if (c >= e->chan_count || !ecuio_engine_has_room(e)) {
            return false;
        }
        ecuio_engine_append(e, c, req, len);
    }
    return true;
}

/* Section 2: enrols every request a set's defs derive, in def order, one per
 * distinct request and channel, so several defs read from one reply
 * cost one request. rx_ids holds each channel's receive ID. True when
 * every request fit; otherwise the table holds those that did, and
 * *overflow_def (when not NULL) names the first def whose request
 * didn't. A NULL set enrols nothing. */
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

#endif
