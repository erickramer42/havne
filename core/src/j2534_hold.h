// File/layer: core/src/j2534_hold.h | internal, not ABI-exported; the
//             J2534 worker's hold, pure and header-only: no clock, no I/O
//             and no platform headers, so it builds and is tested on every
//             platform and the pacing bench calls it as the worker does
// Testing: UNIT: J2534HoldTest (test_obd2, tests/test_j2534_hold.cpp)
//
// Known Issue #42: through the OBDX Pro VX a module's Single Frame reply
// is lost while another module's segmented reply comes in, though the
// bus carries it (docs/bench-sessions/2026-11-11-monitor-car.md). So
// while one channel's reply is expected to take more than one frame
// (ecuio_engine_reply_segmented), the channels take turns:
//
//   - while a channel has such a request out, no other channel asks;
//   - such a request, once due returns it, waits as the channel's chosen
//     request until no other channel has a request out, and while it
//     waits no other channel starts one (a Single Frame asked first can
//     land inside a segmented reply asked a moment later);
//   - the channels a pass held go first in the next pass, so a channel
//     whose segmented exchanges keep coming never starves the rest.
//
// A chosen request is due's own: the engine moved past it and left the
// channel with nothing outstanding, so nothing changes in the engine
// while it waits, and ecuio_engine_sent records it once it is written. A
// row the focus parked while it waited is dropped, not written.
//
// What holds no one: a channel gone silent (its last request timed out,
// with no frame and no recovery since: j2534_hold_answered and
// j2534_hold_recovered clear it), whose requests neither hold the others
// nor keep their chosen requests waiting, or a silent module would cost
// them its whole wait on every lap; and a request in a pending wait (7F
// xx 78, up to 5 s a time), or the others' data and sessions would stop
// for it. A channel's due session request (10 03, 3E 00) goes through the
// hold, so no session ages past S3; a keep-alive due waits with a talk
// offered first, as due puts it behind one.
//
// A hold's chosen rows are its engine's table rows, so a hold lives no
// longer than the ecuio_engine_init it follows. The hold applies with two
// or more channels and ?hold= on (the default); otherwise the pass is the
// plain one, each channel's wait ended and its due request written in
// turn.

#ifndef ECUIO_J2534_HOLD_H
#define ECUIO_J2534_HOLD_H

#include "request_engine.h"

#include <cstdint>

struct j2534_hold {
    bool on = true;    /* ?hold= */
    uint32_t held = 0; /* a bit for each channel the last pass held */
    int32_t chosen[ECUIO_MAX_CHANNELS]; /* due's request kept, or NONE */
    bool rotating[ECUIO_MAX_CHANNELS] = {}; /* a chosen row in the rotation
                                               when chosen */
    uint32_t silent = 0; /* a bit for each channel gone silent */

    j2534_hold()
    {
        for (int32_t& r : chosen) {
            r = ECUIO_ENGINE_NONE;
        }
    }
};

inline bool j2534_hold_applies(const j2534_hold& h,
                               const ecuio_request_engine* e)
{
    return h.on && e->chan_count > 1;
}

/* Whether channel k has a request out that isn't in a pending wait */
inline bool j2534_hold_on_bus(const ecuio_request_engine* e, uint32_t k)
{
    const ecuio_engine_channel& ch = e->chans[k];
    return ch.outstanding != ECUIO_ENGINE_NONE &&
           ch.deadline_us - ch.sent_us <= ECUIO_ENGINE_WAIT_US;
}

/* Whether a channel but c, not gone silent, has a request on the bus, or
 * (segmented) one whose reply is expected segmented */
inline bool j2534_hold_other_out(const j2534_hold& h,
                                 const ecuio_request_engine* e, uint32_t c,
                                 bool segmented)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && ((h.silent >> k) & 1u) == 0 && j2534_hold_on_bus(e, k) &&
            (!segmented ||
             ecuio_engine_reply_segmented(e, k, e->chans[k].outstanding))) {
            return true;
        }
    }
    return false;
}

/* Whether a channel but c has a chosen request waiting */
inline bool j2534_hold_other_chosen(const j2534_hold& h,
                                    const ecuio_request_engine* e, uint32_t c)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && h.chosen[k] != ECUIO_ENGINE_NONE) {
            return true;
        }
    }
    return false;
}

/* The pass's order into order[] (e->chan_count entries): the channels the
 * last pass held, then the rest, each in channel order */
inline void j2534_hold_order(const j2534_hold& h, const ecuio_request_engine* e,
                             uint32_t* order)
{
    const uint32_t first = j2534_hold_applies(h, e) ? h.held : 0u;
    uint32_t n = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t c = 0; c < e->chan_count; ++c) {
            if (((first >> c) & 1u) == (pass == 0 ? 1u : 0u)) {
                order[n++] = c;
            }
        }
    }
}

/* Whether channel c's session request is due at now: taken already, or
 * due with nothing due hands out first (another one-shot taken, or a talk
 * offered first while the session needs only its keep-alive) */
inline bool j2534_hold_session_due(const ecuio_request_engine* e, uint32_t c,
                                   int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    if (ch.sessioning) {
        return true;
    }
    if (ch.identifying || ch.diagnosing || ch.scanning || ch.clearing ||
        ch.talk.out) {
        return false;
    }
    const bool entering = ch.session_step == ECUIO_SESSION_STATE_ENTERING ||
                          now_us - ch.last_sent_us >= ECUIO_ENGINE_S3_US;
    if (ch.talk.offered && ch.talk.first && !entering) {
        return false;
    }
    return ecuio_engine_session_next_us(e, ch) <= now_us;
}

/* What channel c writes this pass at now: its chosen request once no other
 * channel has one on the bus (a row the focus parked meanwhile dropped);
 * else its due session request; else, unless another's segmented reply is
 * due or another's chosen request waits (c is then held, a bit in
 * *held_now), due's request, kept chosen instead when its reply is
 * expected segmented while another channel has one on the bus and c hasn't
 * gone silent. NONE: nothing to write. */
inline int32_t j2534_hold_next(j2534_hold& h, ecuio_request_engine* e,
                               uint32_t c, int64_t now_us, uint32_t* held_now)
{
    if (!j2534_hold_applies(h, e)) {
        return ecuio_engine_due(e, c, now_us);
    }
    if (h.chosen[c] != ECUIO_ENGINE_NONE) {
        if (j2534_hold_other_out(h, e, c, false)) {
            return ECUIO_ENGINE_NONE;
        }
        const int32_t r = h.chosen[c];
        h.chosen[c] = ECUIO_ENGINE_NONE;
        if (r < 0 || !h.rotating[c] ||
            ecuio_engine_in_rotation(e->requests[r])) {
            return r;
        }
    }
    if (j2534_hold_session_due(e, c, now_us)) {
        return ecuio_engine_due(e, c, now_us);
    }
    if (j2534_hold_other_out(h, e, c, true) ||
        j2534_hold_other_chosen(h, e, c)) {
        *held_now |= 1u << c;
        return ECUIO_ENGINE_NONE;
    }
    const int32_t r = ecuio_engine_due(e, c, now_us);
    if (r == ECUIO_ENGINE_NONE || ((h.silent >> c) & 1u) != 0 ||
        !ecuio_engine_reply_segmented(e, c, r) ||
        !j2534_hold_other_out(h, e, c, false)) {
        return r;
    }
    h.chosen[c] = r;
    h.rotating[c] = r >= 0 && ecuio_engine_in_rotation(e->requests[r]);
    return ECUIO_ENGINE_NONE;
}

/* The pass's end: the channels it held go first in the next */
inline void j2534_hold_pass_end(j2534_hold& h, uint32_t held_now)
{
    h.held = held_now;
}

/* A frame matched on channel c: it isn't silent */
inline void j2534_hold_answered(j2534_hold& h, uint32_t c)
{
    h.silent &= ~(1u << c);
}

/* The link recovered: every channel is asked again, none silent */
inline void j2534_hold_recovered(j2534_hold& h)
{
    h.silent = 0;
}

/* The worker's pass at now: timed_out(c) for each channel whose wait ran
 * out, write(c, r) for each request to write. Held, every wait is ended
 * first, then each channel in j2534_hold_order through j2534_hold_next;
 * otherwise each channel's wait is ended and its due request written in
 * turn, so a write's failure is seen by the next channel's timeout. */
template <typename TimedOut, typename Write>
inline void j2534_hold_pass(j2534_hold& h, ecuio_request_engine* e,
                            int64_t now_us, TimedOut&& timed_out, Write&& write)
{
    if (!j2534_hold_applies(h, e)) {
        for (uint32_t c = 0; c < e->chan_count; ++c) {
            if (ecuio_engine_expire(e, c, now_us)) {
                timed_out(c);
            }
            const int32_t r = ecuio_engine_due(e, c, now_us);
            if (r != ECUIO_ENGINE_NONE) {
                write(c, r);
            }
        }
        return;
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (ecuio_engine_expire(e, c, now_us)) {
            h.silent |= 1u << c;
            timed_out(c);
        }
    }
    uint32_t order[ECUIO_MAX_CHANNELS];
    j2534_hold_order(h, e, order);
    uint32_t held_now = 0;
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const uint32_t c = order[k];
        const int32_t r = j2534_hold_next(h, e, c, now_us, &held_now);
        if (r != ECUIO_ENGINE_NONE) {
            write(c, r);
        }
    }
    j2534_hold_pass_end(h, held_now);
}

/* The engine's next event, with the held channels and those whose chosen
 * request waits left out while the hold applies: they wait on another
 * channel's reply, which is in the figure. The whole figure when nothing
 * else is left. */
inline int64_t j2534_hold_next_event_us(const j2534_hold& h,
                                        const ecuio_request_engine* e,
                                        int64_t now_us)
{
    if (!j2534_hold_applies(h, e)) {
        return ecuio_engine_next_event_us(e, now_us);
    }
    uint32_t skip = h.held;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        skip |= h.chosen[c] != ECUIO_ENGINE_NONE ? 1u << c : 0u;
    }
    const int64_t next = ecuio_engine_next_event_us(e, now_us, skip);
    return next != INT64_MAX ? next : ecuio_engine_next_event_us(e, now_us);
}

#endif
