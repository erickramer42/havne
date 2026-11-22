// File/layer: core/src/j2534_hold.h | internal, not ABI-exported; the
//             J2534 worker's hold, pure and header-only: no clock, no I/O
//             and no platform headers, so it builds and is tested on every
//             platform and the pacing bench calls it as the worker does
// Testing: UNIT: J2534HoldTest (test_obd2, tests/test_j2534_hold*.cpp)
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
//   - each pass takes first the channels whose last request was a Single
//     Frame and that hold no chosen row, together; then the rest, the
//     most overdue first: the one whose next (or chosen) row was last
//     written longest ago (j2534_hold_age_us). A module with more rows so
//     takes more turns and each signal is asked about as often;
//   - a Single Frame channel whose next row, answered in one frame, is
//     older than every chosen row goes ahead of them once more
//     (j2534_hold_extra_turn), at most one such pass for each segmented
//     write, so a big Single Frame module keeps up with the segmented
//     ones. A chosen row so waits for the Single Frames in flight, one
//     extra pass and each row older than its own once; a waiting row only
//     grows older, so none starves. A scan's new rows, never asked, rank
//     oldest and take turns ahead of the rest until each is asked once.
//
// A chosen request is due's own: the engine moved past it and left the
// channel with nothing outstanding, so nothing changes in the engine
// while it waits, and ecuio_engine_sent records it once it is written. A
// row the focus parked while it waited is dropped, not written, and so is
// a pack holding one, so no value goes out for a signal the pick left.
//
// What holds no one: a silent channel, whose requests neither hold the
// others nor keep their chosen requests waiting, or an absent module
// would cost them its whole wait on every lap; and a request in a pending
// wait (7F xx 78, up to 5 s a time), or the others' data and sessions
// would stop for it. A channel is silent from its second wait in a row
// that runs out (j2534_hold_answered and j2534_hold_recovered end the
// run), so one lost reply leaves it protected; and the request after
// every eighth is asked under the hold all the same, so a module that
// answers late, inside another's segmented reply, is found again. That
// probe counts as one frame whatever its defs read, and so does every
// request of a channel never heard once a wait of its own has run out
// (j2534_hold_long): each module's first ask is judged by its defs, so a
// present module costs no reply at connect, and an absent one holds the
// bus only for that first ask's wait, 100 ms once if it reads long (four
// present modules beside 14 absent ones of long rows answer 40% of their
// own count in the first 3 s, 92% when those rows are short). The cost: a
// module that wakes with a long reply can lose another module's Single
// Frame once. A channel's first frame
// after a run of J2534_HOLD_ASLEEP_MISSES waits, and at least its whole
// rotation, takes back its rows' waits run out before any answer
// (ecuio_engine_forget_ran_out), as a recovery's rescan does, so the defs
// judge each row's first answer again; a shorter run is rows the module
// never answers, which keep their one frame.
//
// A session request (10 03, 3E 00) waits for the hold like any request,
// since its reply can be lost too, except in its session with S3 300 ms
// or less away (or past): then it goes through, so no session ages past
// S3.
//
// A key (request_talk.h) has a window to go in, so it goes ahead of every
// other channel's next request: while one is offered with its window
// open and its channel's floor before the window's end (?gap= can put it
// past) no other channel starts a request but a key of its own or its
// session request near S3 (both Single Frames), a chosen one waits on
// too, and the key itself waits only for a segmented exchange already on
// the bus (its Single Frame reply could be lost beside it), so for one
// exchange at most. A key whose window passes while it waits is settled
// lapsed at that pass and never written. The window is over before its
// own channel's session gate opens, so no key is offered on a channel
// whose session request goes through.
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
    bool extra = false; /* a Single Frame channel went ahead of a chosen
                           row since the last segmented write */
    uint32_t held = 0; /* a bit for each channel the last pass held */
    uint32_t segmented = 0; /* a bit for each channel whose last write was
                               expected segmented */
    uint32_t from = 0; /* past the last segmented write: equal ages from it */
    int32_t chosen[ECUIO_MAX_CHANNELS]; /* due's request kept, or NONE */
    bool rotating[ECUIO_MAX_CHANNELS] = {}; /* a chosen row in the rotation
                                               when chosen */
    uint32_t misses[ECUIO_MAX_CHANNELS] = {}; /* waits run out in a row */
    uint32_t heard = 0; /* a bit for each channel a frame has come from */

    j2534_hold()
    {
        for (int32_t& r : chosen) {
            r = ECUIO_ENGINE_NONE;
        }
    }
};

constexpr uint32_t J2534_HOLD_SILENT_MISSES = 2; /* waits in a row */
constexpr uint32_t J2534_HOLD_PROBE_EVERY = 8;   /* a silent channel's */
/* A run this long is the module asleep, not rows it leaves unanswered */
constexpr uint32_t J2534_HOLD_ASLEEP_MISSES = 8;
static_assert(J2534_HOLD_ASLEEP_MISSES > J2534_HOLD_SILENT_MISSES,
              "a module asleep is silent first");
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

/* Whether channel ch of e is asking a quiet module's step as a probe
 * (request_engine_quiet.h), so a waking module's first answer isn't lost:
 * every step of one heard before (it can wake), the first four of one
 * never heard (an absent module holds no one past them) */
inline bool j2534_hold_quiet_probe(const ecuio_engine_channel& ch)
{
    return ch.quiet && ch.quiet_asks > 0 &&
           (ch.heard || ch.quiet_asks < ECUIO_ENGINE_QUIET_STEP_COUNT);
}

/* Whether channel k is silent: its waits run out in a row from the
 * second, but for the request after every eighth and a quiet module's
 * probes */
inline bool j2534_hold_silent(const j2534_hold& h,
                              const ecuio_request_engine* e, uint32_t k)
{
    const uint32_t m = h.misses[k];
    return m >= J2534_HOLD_SILENT_MISSES && m % J2534_HOLD_PROBE_EVERY != 0 &&
           !j2534_hold_quiet_probe(e->chans[k]);
}

/* Whether channel k's request r holds the others as segmented: the
 * engine's judgment, but a channel never heard once a wait of its own has
 * run out, or past its first waits run out (its probe), counts one frame */
inline bool j2534_hold_long(const j2534_hold& h, const ecuio_request_engine* e,
                            uint32_t k, int32_t r)
{
    return (((h.heard >> k) & 1u) != 0 || h.misses[k] == 0) &&
           h.misses[k] < J2534_HOLD_SILENT_MISSES &&
           ecuio_engine_reply_segmented(e, k, r);
}

/* Whether a channel but c, not silent, has a request on the bus, or
 * (segmented) one whose reply is expected segmented */
inline bool j2534_hold_other_out(const j2534_hold& h,
                                 const ecuio_request_engine* e, uint32_t c,
                                 bool segmented)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && !j2534_hold_silent(h, e, k) && j2534_hold_on_bus(e, k) &&
            (!segmented || j2534_hold_long(h, e, k, e->chans[k].outstanding))) {
            return true;
        }
    }
    return false;
}

/* Whether channel k's key is offered with its window open at now */
inline bool j2534_hold_key_waits(const ecuio_request_engine* e, uint32_t k,
                                 int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[k];
    return ecuio_talk_key_waits(ch.talk) &&
           ecuio_key_window_open(ch.key, now_us);
}

/* Whether channel k's key waits at now and its floor comes before its
 * window's end: one that can't go in its window holds no one */
inline bool j2534_hold_key_can_go(const ecuio_request_engine* e, uint32_t k,
                                  int64_t now_us)
{
    return j2534_hold_key_waits(e, k, now_us) &&
           e->chans[k].next_send_us < e->chans[k].key.until_us;
}

/* Whether a channel but c has a key waiting at now that can still go */
inline bool j2534_hold_other_key(const ecuio_request_engine* e, uint32_t c,
                                 int64_t now_us)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && j2534_hold_key_can_go(e, k, now_us)) {
            return true;
        }
    }
    return false;
}

/* Whether a channel but c has a chosen row */
inline bool j2534_hold_other_row(const j2534_hold& h,
                                 const ecuio_request_engine* e, uint32_t c)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && h.chosen[k] != ECUIO_ENGINE_NONE) {
            return true;
        }
    }
    return false;
}

/* Whether a channel but c has a chosen request, or a key that can still
 * go, at now: what keeps c from starting a request */
inline bool j2534_hold_other_chosen(const j2534_hold& h,
                                    const ecuio_request_engine* e, uint32_t c,
                                    int64_t now_us)
{
    return j2534_hold_other_row(h, e, c) || j2534_hold_other_key(e, c, now_us);
}

/* How long channel c's next request has waited at now: its chosen row's
 * time since that row's last write; else its next fast row's (a slow
 * row's when it has none: slow rows take their share of the channel's
 * own turns, so they never rank it); with a one-shot to send, the older
 * of that and the channel's last write, since the two take turns */
inline int64_t j2534_hold_age_us(const j2534_hold& h,
                                 const ecuio_request_engine* e, uint32_t c,
                                 int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    int32_t r = h.chosen[c];
    if (r == ECUIO_ENGINE_PACK) {
        r = static_cast<int32_t>(ch.pack_rows[0]);
    } else if (r == ECUIO_ENGINE_NONE) {
        r = ecuio_engine_peek_of(e, c, false);
        r = r >= 0 ? r : ecuio_engine_peek_of(e, c, true);
        if (ecuio_engine_once_waiting(ch)) {
            const int64_t row = r >= 0 ? e->asked_us[r] : ch.last_sent_us;
            return now_us - (row < ch.last_sent_us ? row : ch.last_sent_us);
        }
    }
    return now_us - (r >= 0 ? e->asked_us[r] : ch.last_sent_us);
}

/* Whether channel c may go ahead of the chosen rows at now: no Single
 * Frame channel went ahead since the last segmented write, no key that
 * can still go waits, c's last write was a Single Frame, and its next
 * fast row was answered in one frame or had a wait run out before any
 * answer (a first ask's length is unknown) and is older than every chosen
 * row. A segmented slow row or pack due then hands c is kept chosen, and
 * goes ahead of the chosen rows only as the older (j2534_hold_order ranks
 * it with them). So a chosen row waits for the Single Frames in flight,
 * one extra pass and each row older than its own once, and the biggest
 * Single Frame modules take the extra turns */
inline bool j2534_hold_extra_turn(const j2534_hold& h,
                                  const ecuio_request_engine* e, uint32_t c,
                                  int64_t now_us)
{
    if (h.extra || ((h.segmented >> c) & 1u) != 0 ||
        ecuio_engine_once_waiting(e->chans[c]) ||
        j2534_hold_other_key(e, c, now_us)) {
        return false;
    }
    const int32_t r = ecuio_engine_peek_of(e, c, false);
    if (r < 0 || e->requests[r].reply_frames != 1) {
        return false;
    }
    const int64_t age = now_us - e->asked_us[r];
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (k != c && h.chosen[k] != ECUIO_ENGINE_NONE &&
            j2534_hold_age_us(h, e, k, now_us) >= age) {
            return false;
        }
    }
    return true;
}

/* The pass's order at now into order[] (e->chan_count entries): the
 * channels whose last write was a Single Frame and with no chosen row, in
 * channel order, then the rest (a segmented last write or a chosen row),
 * the oldest age first, equal ages from h.from on around the channels. A
 * chosen row so goes ahead of another only as the older. With no
 * segmented write it is channel order. */
inline void j2534_hold_order(const j2534_hold& h, const ecuio_request_engine* e,
                             int64_t now_us, uint32_t* order)
{
    uint32_t seg = 0;
    for (uint32_t c = 0; j2534_hold_applies(h, e) && c < e->chan_count; ++c) {
        const bool chosen = h.chosen[c] != ECUIO_ENGINE_NONE;
        seg |= ((h.segmented >> c) & 1u) != 0 || chosen ? 1u << c : 0u;
    }
    uint32_t n = 0;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (((seg >> c) & 1u) == 0) {
            order[n++] = c;
        }
    }
    const uint32_t first = n;
    int64_t age[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t i = 0; i < e->chan_count; ++i) {
        const uint32_t c = (h.from + i) % e->chan_count;
        if (((seg >> c) & 1u) == 0) {
            continue;
        }
        const int64_t a = j2534_hold_age_us(h, e, c, now_us);
        uint32_t k = n++;
        for (; k > first && age[k - 1] < a; --k) {
            order[k] = order[k - 1];
            age[k] = age[k - 1];
        }
        order[k] = c;
        age[k] = a;
    }
}

/* Whether channel c's session request goes through the hold at now: in
 * its session with S3 near or past, and due's next request (a session
 * request taken already, or one due with nothing due hands out first:
 * another one-shot taken). No key is offered then: its window ended
 * before the gate opened, and the pass settled it lapsed. */
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

/* Whether chosen request r on channel c may still go: a row that was in
 * the rotation when chosen while it still is, a pack while each of its
 * rows is, anything else always */
inline bool j2534_hold_still_asked(const j2534_hold& h,
                                   const ecuio_request_engine* e, uint32_t c,
                                   int32_t r)
{
    if (r == ECUIO_ENGINE_PACK) {
        const ecuio_engine_channel& ch = e->chans[c];
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            if (!ecuio_engine_in_rotation(e->requests[ch.pack_rows[k]])) {
                return false;
            }
        }
        return true;
    }
    return r < 0 || !h.rotating[c] || ecuio_engine_in_rotation(e->requests[r]);
}

/* What channel c writes this pass at now: nothing while its own request
 * is out (and it isn't held); its chosen request once no other channel
 * has one on the bus or a key that can still go (a row the focus parked
 * meanwhile dropped, a pack holding one too); else its session request
 * near S3; else, unless another's segmented reply is due, another's
 * chosen request waits (unless c takes an extra turn ahead of it) or
 * another has a key that can still go (c is then held, a bit in
 * *held_now), due's request, kept chosen
 * instead when its reply is expected segmented while another channel has
 * one on the bus or a chosen row, and c isn't silent. c's own key waits
 * only for another's segmented reply, and one past its window is settled
 * lapsed first. NONE: nothing to write. */
inline int32_t j2534_hold_next(j2534_hold& h, ecuio_request_engine* e,
                               uint32_t c, int64_t now_us, uint32_t* held_now)
{
    if (!j2534_hold_applies(h, e)) {
        return ecuio_engine_due(e, c, now_us);
    }
    (void)ecuio_talk_key_lapsed(e->chans[c].talk, e->chans[c].key, now_us);
    if (e->chans[c].outstanding != ECUIO_ENGINE_NONE) {
        return ECUIO_ENGINE_NONE;
    }
    if (h.chosen[c] != ECUIO_ENGINE_NONE) {
        if (j2534_hold_other_out(h, e, c, false) ||
            j2534_hold_other_key(e, c, now_us)) {
            return ECUIO_ENGINE_NONE;
        }
        const int32_t r = h.chosen[c];
        h.chosen[c] = ECUIO_ENGINE_NONE;
        if (j2534_hold_still_asked(h, e, c, r)) {
            return r;
        }
    }
    if (j2534_hold_session_due(e, c, now_us)) {
        return ecuio_engine_due_session(e, c, now_us);
    }
    if (j2534_hold_other_out(h, e, c, true) ||
        (!j2534_hold_key_waits(e, c, now_us) &&
         j2534_hold_other_chosen(h, e, c, now_us) &&
         !j2534_hold_extra_turn(h, e, c, now_us))) {
        *held_now |= 1u << c;
        return ECUIO_ENGINE_NONE;
    }
    const int32_t r = ecuio_engine_due(e, c, now_us);
    if (r == ECUIO_ENGINE_NONE || j2534_hold_silent(h, e, c) ||
        !j2534_hold_long(h, e, c, r) ||
        !(j2534_hold_other_out(h, e, c, false) ||
          j2534_hold_other_row(h, e, c))) {
        return r;
    }
    h.chosen[c] = r;
    h.rotating[c] = r >= 0 && ecuio_engine_in_rotation(e->requests[r]);
    return ECUIO_ENGINE_NONE;
}

/* The pass's end: the channels it held, which the next event leaves out */
inline void j2534_hold_pass_end(j2534_hold& h, uint32_t held_now)
{
    h.held = held_now;
}

/* A frame matched on channel c: its run of waits run out ends, the
 * channel is heard, and after a run that long its rows' waits run out
 * are taken back. A run shorter than the channel's rotation is rows it
 * never answers, not the module asleep: taken back, they would hold the
 * bus again every lap */
inline void j2534_hold_answered(j2534_hold& h, ecuio_request_engine* e,
                                uint32_t c)
{
    if (h.misses[c] >= J2534_HOLD_ASLEEP_MISSES &&
        h.misses[c] >= e->chans[c].requests) {
        ecuio_engine_forget_ran_out(e, c);
    }
    h.misses[c] = 0;
    h.heard |= 1u << c;
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
    j2534_hold_order(h, e, now_us, order);
    uint32_t held_now = 0;
    bool ahead = false; /* a write went ahead of a chosen row */
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const uint32_t c = order[k];
        const int32_t r = j2534_hold_next(h, e, c, now_us, &held_now);
        if (r == ECUIO_ENGINE_NONE) {
            continue;
        }
        const bool segmented = j2534_hold_long(h, e, c, r);
        ahead = ahead || (!segmented && j2534_hold_other_row(h, e, c));
        write(c, r);
        h.segmented = segmented ? h.segmented | 1u << c
                                : h.segmented & ~(1u << c);
        if (segmented) {
            h.from = (c + 1) % e->chan_count;
            h.extra = false;
        }
    }
    h.extra = h.extra || ahead;
    j2534_hold_pass_end(h, held_now);
}

/* The engine's next event, with the held channels and those whose chosen
 * request waits left out while the hold applies: they wait on another
 * channel's reply, which is in the figure. The whole figure when nothing
 * else is left. A waiting key keeps one event of its own, its window's
 * end, where the pass settles it, held or behind its own floor. */
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
    int64_t next = ecuio_engine_next_event_us(e, now_us, skip);
    if (next == INT64_MAX) {
        next = ecuio_engine_next_event_us(e, now_us);
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (j2534_hold_key_waits(e, c, now_us) &&
            e->chans[c].key.until_us - now_us < next) {
            next = e->chans[c].key.until_us - now_us;
        }
    }
    return next;
}

#endif
