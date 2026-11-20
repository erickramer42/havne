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
// What holds no one: a silent channel, whose requests neither hold the
// others nor keep their chosen requests waiting, or an absent module
// would cost them its whole wait on every lap; and a request in a pending
// wait (7F xx 78, up to 5 s a time), or the others' data and sessions
// would stop for it. A channel is silent from its second wait in a row
// that runs out (j2534_hold_answered and j2534_hold_recovered end the
// run), so one lost reply leaves it protected; and the request after
// every eighth is asked under the hold all the same, so a module that
// answers late, inside another's segmented reply, is found again.
//
// A session request (10 03, 3E 00) waits for the hold like any request,
// since its reply can be lost too, except in its session with S3 300 ms
// or less away (or past): then it goes through, so no session ages past
// S3. A key (request_talk.h) waits for the hold like any request too: its
// window is over before that gate opens, so a key still offered then is
// settled unsent by due, which hands out the session's request.
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
    uint32_t misses[ECUIO_MAX_CHANNELS] = {}; /* waits run out in a row */

    j2534_hold()
    {
        for (int32_t& r : chosen) {
            r = ECUIO_ENGINE_NONE;
        }
    }
};

constexpr uint32_t J2534_HOLD_SILENT_MISSES = 2; /* waits in a row */
constexpr uint32_t J2534_HOLD_PROBE_EVERY = 8;   /* a silent channel's */
/* A hold lasts about a segmented wait; the window opens past the longest
 * keep-alive, so no keep-alive is let through at its own due time */
constexpr int64_t J2534_HOLD_S3_MARGIN_US = 300000;
static_assert(ECUIO_ENGINE_S3_US - J2534_HOLD_S3_MARGIN_US >
                  ECUIO_ENGINE_KEEPALIVE_MAX_US,
              "the session gate opens before the longest keep-alive is due");
static_assert(ECUIO_KEY_WINDOW_SESSION_US <=
                  ECUIO_ENGINE_S3_US - J2534_HOLD_S3_MARGIN_US,
              "a key's window is over before the session gate opens");

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

/* Whether channel k is silent: its waits run out in a row from the
 * second, but for the request after every eighth */
inline bool j2534_hold_silent(const j2534_hold& h, uint32_t k)
{
    const uint32_t m = h.misses[k];
    return m >= J2534_HOLD_SILENT_MISSES && m % J2534_HOLD_PROBE_EVERY != 0;
}

/* Whether a channel but c, not silent, has a request on the bus, or
 * (segmented) one whose reply is expected segmented */
inline bool j2534_hold_other_out(const j2534_hold& h,
                                 const ecuio_request_engine* e, uint32_t c,
                                 bool segmented)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && !j2534_hold_silent(h, k) && j2534_hold_on_bus(e, k) &&
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

/* Whether channel c's session request goes through the hold at now: in
 * its session with S3 near or past, and due's next request (a session
 * request taken already, or one due with nothing due hands out first:
 * another one-shot taken). A key offered isn't ahead of it: its window
 * ended before the gate opened, and due settles it unsent. */
inline bool j2534_hold_session_due(const ecuio_request_engine* e, uint32_t c,
                                   int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    const int64_t since = now_us - ch.last_sent_us;
    if (ch.session_step != ECUIO_SESSION_STATE_ENTERED ||
        since < ECUIO_ENGINE_S3_US - J2534_HOLD_S3_MARGIN_US) {
        return false;
    }
    if (ch.sessioning) {
        return true;
    }
    if (ch.identifying || ch.diagnosing || ch.scanning || ch.clearing ||
        ch.talk.out) {
        return false;
    }
    return ecuio_engine_session_next_us(e, ch) <= now_us;
}

/* What channel c writes this pass at now: nothing while its own request
 * is out (and it isn't held); its chosen request once no other channel
 * has one on the bus (a row the focus parked meanwhile dropped); else its
 * session request near S3; else, unless another's segmented reply is due
 * or another's chosen request waits (c is then held, a bit in *held_now),
 * due's request, kept chosen instead when its reply is expected segmented
 * while another channel has one on the bus and c isn't silent. NONE:
 * nothing to write. */
inline int32_t j2534_hold_next(j2534_hold& h, ecuio_request_engine* e,
                               uint32_t c, int64_t now_us, uint32_t* held_now)
{
    if (!j2534_hold_applies(h, e)) {
        return ecuio_engine_due(e, c, now_us);
    }
    if (e->chans[c].outstanding != ECUIO_ENGINE_NONE) {
        return ECUIO_ENGINE_NONE;
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
    if (r == ECUIO_ENGINE_NONE || j2534_hold_silent(h, c) ||
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

/* A frame matched on channel c: its run of waits run out ends */
inline void j2534_hold_answered(j2534_hold& h, uint32_t c)
{
    h.misses[c] = 0;
}

/* The link recovered: every channel is asked again, none silent */
inline void j2534_hold_recovered(j2534_hold& h)
{
    for (uint32_t& m : h.misses) {
        m = 0;
    }
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
            ++h.misses[c];
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
