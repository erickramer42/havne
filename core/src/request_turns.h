// File/layer: core/src/request_turns.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: RequestTurnsTest (test_obd2); HARNESS: Elm327TurnsTest
//          (test_elm327)
//
// Turns on an adapter that asks one channel at a time (ELM327: one command
// for the whole adapter; ADR-0015's appendix of 2026-11-18). Each channel's
// rotation is the request engine's; this header decides only which channel
// asks next. Taking turns channel by channel gives a channel with one
// request as many exchanges as one with twenty, so the busy channel's
// values go stale; here a channel keeps the turn, one exchange after
// another, until it has asked each row of its rotation once, so each row
// is asked about once a lap whichever channel it's on, and a channel's
// rows go in a row (no header switch between them).
//
// The channel holding the turn keeps it until:
//   1. it has asked as many rotation rows as its rotation held when the
//      turn began, a pack counting each of its rows and a one-shot (the
//      walk, identification, diagnostics, a scanned DID, a clear, a talk,
//      a session request) none;
//   2. it has nothing due but for a wait, a focus's gap between sweeps or
//      nothing to ask. Its own floor (?gap=) ends the turn only when another
//      channel asks before the floor passes and the floor's rest is longer
//      than ECUIO_ENGINE_GAP_US: a fast adapter's exchange can be shorter
//      than the default floor, and a long ?gap= mustn't idle the adapter.
//      A channel asking its walk always waits its floor out;
//   3. the turn has lasted ECUIO_ENGINE_TURN_US, unless the channel is
//      asking its walk (not a one-shot that holds the slot ahead of it): a
//      large table, a long scan, can hold another channel's clear, talk or
//      identification for that long at most. The engine's cursors stay
//      where they stopped, so the next turn goes on from there;
//   4. another channel's session entry or keep-alive is due (ADR-0060), so
//      a lap never runs past S3.
// Then the next channel in order with something due takes it. A channel
// the driver holds (held, a bit for each channel) is passed over. The walk
// keeps the turn until it ends, so its budget (each channel's own second)
// holds only its own exchanges.
//
// SocketCAN and J2534 don't use this: their channels' exchanges overlap
// (each channel's one outstanding request), so a busy channel's pace never
// waits on another's.

#ifndef ECUIO_REQUEST_TURNS_H
#define ECUIO_REQUEST_TURNS_H

#include "request_engine.h"

#include <cstdint>

/* The longest turn, rule 3 */
constexpr int64_t ECUIO_ENGINE_TURN_US = 1000000;

/* The turn: the driver's, from zero; only ecuio_engine_take_turn changes it */
struct ecuio_engine_turns {
    uint32_t chan = 0;    /* the channel holding the turn, or asked next */
    bool on = false;      /* chan holds a turn under way */
    uint32_t limit = 0;   /* its rotation's rows when the turn began */
    uint32_t asked = 0;   /* rotation rows asked in the turn */
    int64_t since_us = 0; /* when the turn began */
};

/* Whether channel ch is asking its walk: walking, with no other one-shot
 * holding the slot ahead of it (rules 2 and 3) */
inline bool ecuio_engine_turn_walking(const ecuio_engine_channel& ch)
{
    return ch.walking && !ch.identifying && !ch.diagnosing && !ch.scanning &&
           !ch.clearing && !ch.sessioning && !ch.talk.out;
}

/* When channel ch next has something to ask, by its floor, a focus's gap
 * between sweeps and its session; INT64_MAX while a request is out or
 * with nothing to ask */
inline int64_t ecuio_engine_turn_next_us(const ecuio_request_engine* e,
                                         const ecuio_engine_channel& ch)
{
    if (ch.outstanding != ECUIO_ENGINE_NONE) {
        return INT64_MAX;
    }
    int64_t at = INT64_MAX;
    if (ch.check_due || ecuio_engine_once_waiting(ch)) {
        at = ch.next_send_us;
    } else if (ch.requests > 0) {
        at = (e->min_sweep_us > 0 && ch.next_sweep_us > ch.next_send_us)
                 ? ch.next_sweep_us
                 : ch.next_send_us;
    }
    const int64_t s = ecuio_engine_session_next_us(e, ch);
    return s < at ? s : at;
}

/* Microseconds the turn on channel c waits for its own floor (rule 2), 0
 * for none: while the floor alone holds what it has to ask, and the rest
 * is short, the channel is asking its walk, or no channel but held ones
 * asks before the floor passes */
inline int64_t ecuio_engine_turn_floor_us(const ecuio_request_engine* e,
                                          uint32_t c, uint32_t held,
                                          int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    const int64_t at = ecuio_engine_turn_next_us(e, ch);
    if (at <= now_us || at != ch.next_send_us) {
        return 0;
    }
    if (at - now_us <= ECUIO_ENGINE_GAP_US || ecuio_engine_turn_walking(ch)) {
        return at - now_us;
    }
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && (held & (1u << k)) == 0 &&
            ecuio_engine_turn_next_us(e, e->chans[k]) < at) {
            return 0;
        }
    }
    return at - now_us;
}

/* Whether the turn on channel c is over by rule 1, 3 or 4 at now_us */
inline bool ecuio_engine_turn_over(const ecuio_request_engine* e,
                                   const ecuio_engine_turns& t, uint32_t c,
                                   uint32_t held, int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    if (t.limit > 0 && t.asked >= t.limit) {
        return true;
    }
    if (!ecuio_engine_turn_walking(ch) &&
        now_us - t.since_us >= ECUIO_ENGINE_TURN_US) {
        return true;
    }
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const ecuio_engine_channel& o = e->chans[k];
        if (k != c && (held & (1u << k)) == 0 &&
            o.outstanding == ECUIO_ENGINE_NONE &&
            ecuio_engine_session_next_us(e, o) <= now_us) {
            return true;
        }
    }
    return false;
}

/* Request r just chosen on channel c counts toward the turn (rule 1) */
inline void ecuio_engine_turn_count(const ecuio_request_engine* e,
                                    ecuio_engine_turns& t, uint32_t c,
                                    int32_t r)
{
    if (r == ECUIO_ENGINE_PACK) {
        t.asked += e->chans[c].pack_count;
    } else if (r >= 0) {
        ++t.asked;
    }
}

/* What goes out next and on which channel (*c), at now_us: the turn's
 * channel's next request while its turn lasts, else the first of the
 * channels after it with something due, which takes the turn. NONE when
 * nothing is due; *wait_us is then the time until the turn's channel's
 * floor passes when the turn waits it out (rule 2), else 0, and the driver
 * waits for the engine's next event. Like ecuio_engine_due, it moves the
 * rotation past what it returns. */
inline int32_t ecuio_engine_take_turn(ecuio_request_engine* e,
                                      ecuio_engine_turns& t, uint32_t held,
                                      int64_t now_us, uint32_t* c,
                                      int64_t* wait_us)
{
    *wait_us = 0;
    const uint32_t n = e->chan_count;
    if (t.on) {
        const uint32_t k = t.chan;
        if ((held & (1u << k)) == 0 &&
            !ecuio_engine_turn_over(e, t, k, held, now_us)) {
            const int64_t floor_us =
                ecuio_engine_turn_floor_us(e, k, held, now_us);
            if (floor_us > 0) {
                *wait_us = floor_us;
                return ECUIO_ENGINE_NONE;
            }
            const int32_t r = ecuio_engine_due(e, k, now_us);
            if (r != ECUIO_ENGINE_NONE) {
                ecuio_engine_turn_count(e, t, k, r);
                *c = k;
                return r;
            }
        }
        t.on = false;
        t.chan = (k + 1) % n;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = (t.chan + i) % n;
        if ((held & (1u << k)) != 0) {
            continue;
        }
        const uint32_t limit = e->chans[k].requests;
        const int32_t r = ecuio_engine_due(e, k, now_us);
        if (r != ECUIO_ENGINE_NONE) {
            t.chan = k;
            t.on = true;
            t.limit = limit;
            t.asked = 0;
            t.since_us = now_us;
            ecuio_engine_turn_count(e, t, k, r);
            *c = k;
            return r;
        }
    }
    return ECUIO_ENGINE_NONE;
}

#endif
