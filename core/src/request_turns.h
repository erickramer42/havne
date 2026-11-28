// File/layer: core/src/request_turns.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: RequestTurnsTest, RequestSlotsTest (test_obd2); HARNESS:
//          Elm327TurnsTest (test_elm327)
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
//   4. another channel's session is urgent (ADR-0060): its entry due, or
//      its last request max(keepalive, S3 - ECUIO_ENGINE_S3_MARGIN_US)
//      ago (ecuio_engine_session_urgent_us), so a lap never runs past S3.
//      Before then any request of the channel's own keeps its session (a
//      row stands in for the keep-alive), so the turns stay whole;
//   5. another channel has a key offered in its window (request_talk.h):
//      the window can be shorter than a turn, and a key that waits one
//      out is never sent. One past its window is settled lapsed at the
//      next pass, whoever holds the turn.
// Then the next channel in order with something due takes it, starting
// at a channel with a key offered, else at the first whose session is
// urgent, so an urgent keep-alive waits on no channel between. A channel
// whose only request due is a keep-alive not yet urgent (bare: no rows,
// a focus that leaves it out, a held quiet channel between its steps) is
// passed over while any other channel has something due, so a picked
// signal outranks an idle module's tester present; it takes the turn
// when nothing else is due, or at its urgency. A channel
// the driver holds (held, a bit for each channel) is passed over. The
// walk keeps the turn until it ends or rule 4 or 5 takes it. Its budget
// (each channel's own second) stands still from a turn's end to the
// channel's next request, so it holds only the channel's own exchanges,
// whoever asks in between.
//
// SocketCAN and J2534 don't take turns: their channels' exchanges overlap
// (each channel's one outstanding request), so a busy channel's pace never
// waits on another's. They take free slots instead: at most the engine's
// cap of requests out at once, a free slot going to the channel whose
// next request has been due longest (ecuio_engine_next_slot, ADR-0065
// section 11); SocketCAN through it, J2534's plain pass too.

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
    /* A bit for each channel whose turn ended while it walked, and since
     * when: the walk's budget stands still until the channel asks again */
    uint32_t walk_away = 0;
    int64_t walk_away_us[ECUIO_MAX_CHANNELS] = {};
};

/* Whether channel ch is asking its walk: walking, with no other one-shot
 * holding the slot ahead of it (rules 2 and 3) */
inline bool ecuio_engine_turn_walking(const ecuio_engine_channel& ch)
{
    return ch.walking && !ch.identifying && !ch.diagnosing && !ch.scanning &&
           !ch.clearing && !ch.sessioning && !ch.talk.out;
}

/* When channel ch next has something to ask but its keep-alive, by its
 * floor, a focus's gap between sweeps and a held quiet channel's step
 * (an entry due is a one-shot waiting); INT64_MAX while a request is out
 * or with nothing else to ask */
inline int64_t ecuio_engine_turn_own_us(const ecuio_request_engine* e,
                                        const ecuio_engine_channel& ch)
{
    if (ch.outstanding != ECUIO_ENGINE_NONE || ch.part_written) {
        return INT64_MAX;
    }
    if (e->operation_hold) {
        return ecuio_engine_hold_asks(ch) ? ch.next_send_us : INT64_MAX;
    }
    int64_t at = INT64_MAX;
    if (ch.check_due || ecuio_engine_once_waiting(ch)) {
        at = ch.next_send_us;
    } else if (ch.requests > 0) {
        at = (e->min_sweep_us > 0 && ch.next_sweep_us > ch.next_send_us)
                 ? ch.next_sweep_us
                 : ch.next_send_us;
        if (ecuio_engine_quiet_held(e, ch)) {
            const int64_t q = ecuio_engine_quiet_due_us(e, ch);
            at = q > ch.next_send_us ? q : ch.next_send_us;
        }
    } else if (ch.quiet && ecuio_engine_quiet_asks_items(e, ch)) {
        const int64_t q = ecuio_engine_quiet_due_us(e, ch);
        at = q > ch.next_send_us ? q : ch.next_send_us;
    }
    return at;
}

/* When channel ch next has something to ask: its own time, its session's
 * and a key's window that holds it (none holds a key offered); INT64_MAX
 * while a request is out or with nothing to ask. A module not open asks
 * only what it owes as it closes. */
inline int64_t ecuio_engine_turn_next_us(const ecuio_request_engine* e,
                                         const ecuio_engine_channel& ch)
{
    if (ch.outstanding != ECUIO_ENGINE_NONE) {
        return INT64_MAX;
    }
    if (!ecuio_engine_module_open(ch) || ch.visiting) {
        return ecuio_engine_limited_next_us(e, ch);
    }
    int64_t at = ecuio_engine_turn_own_us(e, ch);
    const int64_t s = ecuio_engine_session_next_us(e, ch);
    at = s < at ? s : at;
    if (at != INT64_MAX && !ecuio_talk_key_waits(ch.talk)) {
        const int64_t held = ecuio_engine_key_held_until(e, ch);
        at = held > at ? held : at;
    }
    return at;
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

/* Every channel's key that is past its window at now_us is settled
 * lapsed: take_turn's first step, whoever holds the turn, since the
 * key's own channel may not ask for a second or more */
inline void ecuio_engine_turn_lapse(ecuio_request_engine* e, int64_t now_us)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        (void)ecuio_talk_key_lapsed(e->chans[k].talk, e->chans[k].key, now_us);
    }
}

/* The first channel not held whose key is offered (rule 5); the channel
 * count for none. Its window is open: take_turn settled the rest. */
inline uint32_t ecuio_engine_turn_key(const ecuio_request_engine* e,
                                      uint32_t held)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if ((held & (1u << k)) == 0 && ecuio_talk_key_waits(e->chans[k].talk)) {
            return k;
        }
    }
    return e->chan_count;
}

/* The first channel not held, but for skip, whose session is urgent at
 * now_us (rule 4); the channel count for none */
inline uint32_t ecuio_engine_turn_session(const ecuio_request_engine* e,
                                          uint32_t held, uint32_t skip,
                                          int64_t now_us)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const ecuio_engine_channel& o = e->chans[k];
        if (k != skip && (held & (1u << k)) == 0 &&
            o.outstanding == ECUIO_ENGINE_NONE &&
            ecuio_engine_session_urgent_us(e, o) <= now_us) {
            return k;
        }
    }
    return e->chan_count;
}

/* Whether channel ch is bare at now_us: nothing of its own due and its
 * session not urgent, so all it could ask is a keep-alive that can wait */
inline bool ecuio_engine_turn_bare(const ecuio_request_engine* e,
                                   const ecuio_engine_channel& ch,
                                   int64_t now_us)
{
    return ecuio_engine_turn_own_us(e, ch) > now_us &&
           ecuio_engine_session_urgent_us(e, ch) > now_us;
}

/* Channel k's turn ended at now_us: a walk it was asking is away from
 * then */
inline void ecuio_engine_turn_left(const ecuio_request_engine* e,
                                   ecuio_engine_turns& t, uint32_t k,
                                   int64_t now_us)
{
    if (e->chans[k].walking) {
        t.walk_away |= 1u << k;
        t.walk_away_us[k] = now_us;
    }
}

/* ecuio_engine_due for a channel that doesn't hold the turn. The time
 * since k's turn ended mid-walk isn't the walk's: its budget moves on by
 * it before due reads it, each stretch once, and k is away no more once
 * it asks. */
inline int32_t ecuio_engine_turn_due(ecuio_request_engine* e,
                                     ecuio_engine_turns& t, uint32_t k,
                                     int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[k];
    if ((t.walk_away & (1u << k)) != 0) {
        if (ch.walking && ch.walk_timed) {
            ch.walk_until_us += now_us - t.walk_away_us[k];
        }
        t.walk_away_us[k] = now_us;
    }
    const int32_t r = ecuio_engine_due(e, k, now_us);
    if (r != ECUIO_ENGINE_NONE) {
        t.walk_away &= ~(1u << k);
    }
    return r;
}

/* Whether the turn on channel c is over by rule 1, 3, 4 or 5 at now_us */
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
    const uint32_t keyed = ecuio_engine_turn_key(e, held);
    if (keyed < e->chan_count && keyed != c) {
        return true;
    }
    return ecuio_engine_turn_session(e, held, c, now_us) < e->chan_count;
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

/* The hand-over: the first channel from `from` on, not held and, unless
 * bare_ok, not bare, with something due, which takes the turn; its
 * request, NONE when none has one */
inline int32_t ecuio_engine_turn_pick(ecuio_request_engine* e,
                                      ecuio_engine_turns& t, uint32_t held,
                                      uint32_t from, int64_t now_us,
                                      bool bare_ok, uint32_t* c)
{
    const uint32_t n = e->chan_count;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = (from + i) % n;
        if ((held & (1u << k)) != 0 ||
            (!bare_ok && ecuio_engine_turn_bare(e, e->chans[k], now_us))) {
            continue;
        }
        const uint32_t limit = e->chans[k].requests;
        const int32_t r = ecuio_engine_turn_due(e, t, k, now_us);
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

/* What goes out next and on which channel (*c), at now_us: the turn's
 * channel's next request while its turn lasts, else the first of the
 * channels after it with something due, which takes the turn; a channel
 * with a key offered, then one whose session is urgent, is asked ahead
 * of that order, and a bare one only when no other has anything due.
 * NONE when
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
    ecuio_engine_turn_lapse(e, now_us);
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
        ecuio_engine_turn_left(e, t, k, now_us);
        t.on = false;
        t.chan = (k + 1) % n;
    }
    const uint32_t keyed = ecuio_engine_turn_key(e, held);
    const uint32_t sessioned = ecuio_engine_turn_session(e, held, n, now_us);
    const uint32_t from = keyed < n       ? keyed
                          : sessioned < n ? sessioned
                                          : t.chan;
    const int32_t r =
        ecuio_engine_turn_pick(e, t, held, from, now_us, false, c);
    return r != ECUIO_ENGINE_NONE
               ? r
               : ecuio_engine_turn_pick(e, t, held, from, now_us, true, c);
}

/* The channel not in skip, with nothing out, whose next request has been
 * due longest (the least ecuio_engine_turn_next_us at or before now),
 * equal times from e->slot_from on; UINT32_MAX for none */
inline uint32_t ecuio_engine_longest_due(const ecuio_request_engine* e,
                                         int64_t now_us, uint32_t skip)
{
    uint32_t best = UINT32_MAX;
    int64_t best_at = INT64_MAX;
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const uint32_t c = (e->slot_from + k) % e->chan_count;
        const int64_t at = ecuio_engine_turn_next_us(e, e->chans[c]);
        if (((skip >> c) & 1u) == 0 && at <= now_us && at < best_at) {
            best = c;
            best_at = at;
        }
    }
    return best;
}

/* A free slot's request at now, its channel in *out_c, or NONE with every
 * slot taken or nothing due (ADR-0065 section 11). The channel due
 * longest goes first; one its time names but due passes by is skipped;
 * then any other channel due gives, from slot_from on (an unpicked
 * session's check, which no time names). A one-shot takes a slot as a
 * row does. */
inline int32_t ecuio_engine_next_slot(ecuio_request_engine* e, int64_t now_us,
                                      uint32_t skip, uint32_t* out_c)
{
    if (e->chan_count == 0 || ecuio_engine_slots_full(e)) {
        return ECUIO_ENGINE_NONE;
    }
    for (;;) {
        const uint32_t c = ecuio_engine_longest_due(e, now_us, skip);
        if (c == UINT32_MAX) {
            break;
        }
        skip |= 1u << c;
        const int32_t r = ecuio_engine_due(e, c, now_us);
        if (r != ECUIO_ENGINE_NONE) {
            e->slot_from = (c + 1) % e->chan_count;
            *out_c = c;
            return r;
        }
    }
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const uint32_t c = (e->slot_from + k) % e->chan_count;
        if (((skip >> c) & 1u) != 0 ||
            e->chans[c].outstanding != ECUIO_ENGINE_NONE) {
            continue;
        }
        const int32_t r = ecuio_engine_due(e, c, now_us);
        if (r != ECUIO_ENGINE_NONE) {
            e->slot_from = (c + 1) % e->chan_count;
            *out_c = c;
            return r;
        }
    }
    return ECUIO_ENGINE_NONE;
}

#endif
