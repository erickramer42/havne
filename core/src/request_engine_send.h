// File/layer: core/src/request_engine_send.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// What a channel sends next (ecuio_engine_due), a talk's offer and turn,
// and the next event.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, EngineTalkTest, RequestTurnsTest
//          (test_obd2); HARNESS: every transport's suites

#ifndef ECUIO_REQUEST_ENGINE_SEND_H
#define ECUIO_REQUEST_ENGINE_SEND_H

#include "request_engine_pack.h"
#include "request_engine_focus.h"
#include "request_engine_scan.h"
#include "request_engine_walk.h"
#include "request_engine_diag.h"
#include "request_engine_session.h"
#include "request_engine_quiet.h"

/* Offers q as channel c's talk at now_us (request_talk.h's door, with the
 * channel's window). A functional channel's bound is one frame, and
 * security access goes to a module's own address: a seed or a key there
 * is refused, and so is a channel the engine lacks, and a seed anywhere
 * while a functional channel has a session
 * (ecuio_engine_functional_session). */
inline ecuio_talk_offer ecuio_engine_offer_talk(ecuio_request_engine* e,
                                                uint32_t c,
                                                const ecuio_talk_request& q,
                                                int64_t now_us)
{
    if (c >= e->chan_count) {
        return ecuio_talk_offer::refused;
    }
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.functional && q.kind != ecuio_talk_kind::plain) {
        return ecuio_talk_offer::refused;
    }
    if (q.kind == ecuio_talk_kind::seed && ecuio_engine_functional_session(e)) {
        return ecuio_talk_offer::refused;
    }
    const uint32_t bound = ch.functional && e->pdu_max > ECUIO_ENGINE_PDU_SINGLE
                               ? ECUIO_ENGINE_PDU_SINGLE
                               : e->pdu_max;
    return ecuio_engine_talk(ch.talk, ch.key, bound, q, now_us);
}

/* The offered talk takes channel c's one-shot */
inline int32_t ecuio_engine_talk_slot(ecuio_engine_channel& ch, uint32_t c)
{
    ch.once = ecuio_request{};
    ch.once.len = ecuio_talk_take_slot(ch.talk, ch.once.bytes);
    ch.once.channel = static_cast<uint8_t>(c);
    return ECUIO_ENGINE_ONCE;
}

/* due's lead: true when the floor, a key's window, a one-shot already in,
 * a key, the session's entry or the walk decides, with *r its answer. A key
 * whose window ended before this turn settles lapsed, and a walk out of
 * budget ends here. No session entry can be due ahead of a key: its
 * window ends with the session's step and inside S3. */
inline bool ecuio_engine_due_lead(ecuio_request_engine* e, uint32_t c,
                                  int64_t now_us, int32_t* r)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding != ECUIO_ENGINE_NONE || now_us < ch.next_send_us) {
        *r = ECUIO_ENGINE_NONE;
        return true;
    }
    const bool awaited = ecuio_engine_key_awaited(e, ch, now_us);
    if (ch.functional && awaited) {
        *r = ECUIO_ENGINE_NONE; /* every module hears this address */
        return true;
    }
    /* Taken before a probe started the walk: it goes first, so its reply
     * settles it and no other request's reply does. */
    if (ch.identifying || ch.diagnosing || ch.scanning || ch.clearing ||
        ch.sessioning || ch.talk.out) {
        *r = ECUIO_ENGINE_ONCE;
        return true;
    }
    (void)ecuio_talk_key_lapsed(ch.talk, ch.key, now_us);
    if (ecuio_talk_turn(ch.talk, true, false)) {
        *r = ecuio_engine_talk_slot(ch, c);
        return true;
    }
    if (awaited) {
        *r = ECUIO_ENGINE_NONE; /* nothing but the key, not yet offered */
        return true;
    }
    /* ADR-0060: the entry first; while 10 03 is due nothing else goes */
    if (ecuio_engine_session_entry_due(e, c, now_us)) {
        *r = ECUIO_ENGINE_ONCE;
        return true;
    }
    if (ch.walking) {
        if (!ch.walk_timed) {
            ch.walk_timed = true;
            ch.walk_until_us = now_us + ECUIO_ENGINE_PROBE_BUDGET_US;
        }
        if (now_us < ch.walk_until_us) {
            ch.once = ch.walk;
            *r = ECUIO_ENGINE_ONCE;
            return true;
        }
        ecuio_engine_end_walk(e, c);
    }
    return false;
}

/* A clear's next request into channel c's one-shot */
inline void ecuio_engine_take_clear(ecuio_engine_channel& ch, uint32_t c)
{
    ch.once = ecuio_request{};
    if (ch.clear_step == ECUIO_CLEAR_STEP_SPEED) {
        ch.once.bytes[0] = ECUIO_CLEAR_SPEED_REQUEST[0];
        ch.once.bytes[1] = ECUIO_CLEAR_SPEED_REQUEST[1];
        ch.once.len = 2;
    } else if (ch.clear_step == ECUIO_CLEAR_STEP_VEHICLE) {
        ch.once.bytes[0] = ECUIO_CLEAR_VEHICLE_SPEED_REQUEST[0];
        ch.once.bytes[1] = ECUIO_CLEAR_VEHICLE_SPEED_REQUEST[1];
        ch.once.len = 2;
    } else {
        ch.once.bytes[0] = ECUIO_CLEAR_SERVICE;
        ch.once.len = 1;
    }
    ch.once.channel = static_cast<uint8_t>(c);
    ch.clearing = true;
    ch.scan_turn = false;
}

/* Identification item i into channel c's one-shot */
inline void ecuio_engine_take_ident_at(ecuio_request_engine* e, uint32_t c,
                                       uint32_t i)
{
    ecuio_engine_channel& ch = e->chans[c];
    const ecuio_identity_item_t& it = ch.ident[i];
    ch.once = ecuio_request{};
    for (uint8_t k = 0; k < it.length; ++k) {
        ch.once.bytes[k] = it.request[k];
    }
    ch.once.len = it.length;
    ch.once.channel = static_cast<uint8_t>(c);
    ch.identifying = true;
    ch.ident_index = i;
    ch.scan_turn = false;
}

/* The next identification item into channel c's one-shot: false when
 * none was unasked */
inline bool ecuio_engine_take_ident(ecuio_request_engine* e, uint32_t c)
{
    const int32_t i = ecuio_engine_ident_next(e, c);
    if (i >= 0) {
        ecuio_engine_take_ident_at(e, c, static_cast<uint32_t>(i));
        return true;
    }
    e->chans[c].ident_pending = 0; /* the count had drifted; none unasked */
    return false;
}

/* Diagnostics item i into channel c's one-shot */
inline void ecuio_engine_take_diag_at(ecuio_request_engine* e, uint32_t c,
                                      uint32_t i)
{
    ecuio_engine_channel& ch = e->chans[c];
    const ecuio_diag_item& it = ch.diag_items[i];
    ch.once = ecuio_request{};
    for (uint8_t k = 0; k < it.len; ++k) {
        ch.once.bytes[k] = it.request[k];
    }
    ch.once.len = it.len;
    ch.once.channel = static_cast<uint8_t>(c);
    ch.diagnosing = true;
    ch.diag_index = i;
    ch.scan_turn = false;
}

/* The next diagnostics item into channel c's one-shot: false when none
 * was unasked */
inline bool ecuio_engine_take_diag(ecuio_request_engine* e, uint32_t c)
{
    const int32_t i = ecuio_engine_diag_next(e, c);
    if (i >= 0) {
        ecuio_engine_take_diag_at(e, c, static_cast<uint32_t>(i));
        return true;
    }
    e->chans[c].diag_pending = 0; /* the count had drifted; none unasked */
    return false;
}

/* Scan entry i into channel c's one-shot */
inline void ecuio_engine_take_scan_at(ecuio_request_engine* e, uint32_t c,
                                      uint32_t i)
{
    ecuio_engine_channel& ch = e->chans[c];
    const ecuio_scan_entry& s = e->scan[i];
    ch.once = ecuio_request{};
    ch.once.bytes[0] = 0x22;
    ch.once.bytes[1] = static_cast<uint8_t>(s.did >> 8);
    ch.once.bytes[2] = static_cast<uint8_t>(s.did);
    ch.once.len = 3;
    ch.once.channel = static_cast<uint8_t>(c);
    ch.scanning = true;
    ch.scan_index = i;
    ch.scan_turn = false;
}

/* The next scanned DID into channel c's one-shot: false when none was
 * unasked */
inline bool ecuio_engine_take_scan(ecuio_request_engine* e, uint32_t c)
{
    const int32_t i = ecuio_engine_scan_next(e, c);
    if (i >= 0) {
        ecuio_engine_take_scan_at(e, c, static_cast<uint32_t>(i));
        return true;
    }
    e->chans[c].scan_pending = 0; /* the count had drifted; none unasked */
    return false;
}

/* due's ask for held channel c with no rows (request_engine_quiet.h's
 * rule 3): once its step has passed, an unanswered item into the
 * one-shot, its outcome standing until the item settles again */
inline int32_t ecuio_engine_due_quiet_item(ecuio_request_engine* e, uint32_t c,
                                           int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    const int32_t found = ecuio_engine_quiet_item(e, c);
    if (found < 0 || now_us < ecuio_engine_quiet_due_us(e, ch)) {
        return ECUIO_ENGINE_NONE;
    }
    const uint32_t i = static_cast<uint32_t>(found);
    ch.quiet_item = i + 1;
    if (i < ch.ident_count) {
        ecuio_engine_take_ident_at(e, c, i);
    } else if (i - ch.ident_count < ch.diag_count) {
        ecuio_engine_take_diag_at(e, c, i - ch.ident_count);
    } else {
        ecuio_engine_take_scan_at(e, c, i - ch.ident_count - ch.diag_count);
    }
    ecuio_engine_quiet_asked(e, c, now_us);
    return ECUIO_ENGINE_ONCE;
}

/* due's one-shot lists in their turn: a clear's next request, the next
 * identification item, the next diagnostics item, then a talk and the
 * next scanned DID in turns; ECUIO_ENGINE_ONCE when one went into the
 * one-shot, else ECUIO_ENGINE_NONE */
inline int32_t ecuio_engine_due_once(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    const bool once_turn = ch.requests == 0 || ch.scan_turn ||
                           (e->scan_first && ch.scan_pending > 0);
    if (ch.clear_step != ECUIO_CLEAR_STEP_NONE && once_turn) {
        ecuio_engine_take_clear(ch, c);
        return ECUIO_ENGINE_ONCE;
    }
    if (ch.ident_pending > 0 && once_turn && ecuio_engine_take_ident(e, c)) {
        return ECUIO_ENGINE_ONCE;
    }
    if (ch.diag_pending > 0 && once_turn) {
        ecuio_engine_diag_skip_unsupported(e, c);
    }
    if (ch.diag_pending > 0 && once_turn && ecuio_engine_take_diag(e, c)) {
        return ECUIO_ENGINE_ONCE;
    }
    if (once_turn && ecuio_talk_turn(ch.talk, false, ch.scan_pending > 0)) {
        ch.scan_turn = false;
        return ecuio_engine_talk_slot(ch, c);
    }
    if (ch.scan_pending > 0 && once_turn && ecuio_engine_take_scan(e, c)) {
        return ECUIO_ENGINE_ONCE;
    }
    return ECUIO_ENGINE_NONE;
}

/* The driver's check (ecuio_engine_ask_check), once: its row, else
 * ECUIO_ENGINE_NONE. No sweep counts it, and a pick taken since it fell
 * due withdraws it. */
inline int32_t ecuio_engine_due_check(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (!ch.check_due) {
        return ECUIO_ENGINE_NONE;
    }
    ch.check_due = false;
    return ecuio_engine_unpicked(e) ? ecuio_engine_check_row(e, c)
                                    : ECUIO_ENGINE_NONE;
}

/* due's rotation: channel c's next request by its rate class, packed,
 * or ECUIO_ENGINE_NONE with none or before a focus's next sweep; held
 * quiet, one row alone once its step has passed */
inline int32_t ecuio_engine_due_rotation(ecuio_request_engine* e, uint32_t c,
                                         int64_t now_us, bool held)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.requests == 0) {
        return ECUIO_ENGINE_NONE;
    }
    if (held ? now_us < ecuio_engine_quiet_due_us(e, ch)
             : e->min_sweep_us > 0 && now_us < ch.next_sweep_us) {
        return ECUIO_ENGINE_NONE;
    }
    const bool slow_turn = ch.fast_run >= ECUIO_ENGINE_FAST_PER_SLOW;
    int32_t i = ecuio_engine_next_of(e, c, slow_turn);
    if (i < 0) {
        i = ecuio_engine_next_of(e, c, !slow_turn);
    }
    if (i < 0) {
        return ECUIO_ENGINE_NONE;
    }
    if (e->requests[i].slow != 0) {
        ch.fast_run = 0;
    } else if (ch.fast_run < ECUIO_ENGINE_FAST_PER_SLOW) {
        ++ch.fast_run;
    }
    if (held) {
        ecuio_engine_quiet_asked(e, c, now_us);
    }
    const int32_t asked = held ? i : ecuio_engine_gather(e, c, i);
    ecuio_engine_count_sweep(e, c, asked, now_us);
    return asked;
}

/* What channel c sends next: ECUIO_ENGINE_ONCE for an identification
 * item, a diagnostics item, a clear's request or a scanned DID already
 * in the one-shot (its write failed), then for a key, and nothing more
 * while a key's window holds the channel; then for the session's entry;
 * then while its walk has a bitmap
 * to ask, then, taking turns with the rotation, for a clear's next
 * request, the next identification item, then the next diagnostics
 * item, then a talk and the next scanned DID in turns, else the next
 * request of its rotation, else its keep-alive, or ECUIO_ENGINE_NONE while
 * one is outstanding, before the floor, or when there is nothing to
 * send; the rotation's own turn also waits for a focus's next sweep. A
 * walk out of budget ends here. A quiet channel held back
 * (request_engine_quiet.h) settles its lists unasked and asks a row at
 * each step, with no rows an item. Moves the rotation past the request
 * it returns; ecuio_engine_sent records what became of it. */
inline int32_t ecuio_engine_due(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    int32_t r = ECUIO_ENGINE_NONE;
    if (ecuio_engine_due_lead(e, c, now_us, &r)) {
        return r;
    }
    const bool held = ecuio_engine_quiet_held(e, e->chans[c]);
    if (held) {
        ecuio_engine_quiet_settle(e, c);
    }
    r = ecuio_engine_due_once(e, c);
    if (r != ECUIO_ENGINE_NONE) {
        return r;
    }
    e->chans[c].scan_turn = true;
    r = ecuio_engine_due_check(e, c);
    if (r >= 0) {
        return r;
    }
    r = held && e->chans[c].requests == 0
            ? ecuio_engine_due_quiet_item(e, c, now_us)
            : ecuio_engine_due_rotation(e, c, now_us, held);
    if (r == ECUIO_ENGINE_NONE && ecuio_engine_keepalive_due(e, c, now_us)) {
        return ECUIO_ENGINE_ONCE; /* nothing stood in for it */
    }
    return r;
}

/* Channel c's session request alone (the J2534 hold's gate, where a row
 * in its place would pass the hold): a one-shot already in, else the
 * entry, else the keep-alive; ECUIO_ENGINE_NONE while one is outstanding,
 * before the floor or with neither due */
inline int32_t ecuio_engine_due_session(ecuio_request_engine* e, uint32_t c,
                                        int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding != ECUIO_ENGINE_NONE || now_us < ch.next_send_us) {
        return ECUIO_ENGINE_NONE;
    }
    if (ch.identifying || ch.diagnosing || ch.scanning || ch.clearing ||
        ch.sessioning || ch.talk.out ||
        ecuio_engine_session_entry_due(e, c, now_us) ||
        ecuio_engine_keepalive_due(e, c, now_us)) {
        return ECUIO_ENGINE_ONCE;
    }
    return ECUIO_ENGINE_NONE;
}

/* Whether channel c has anything to send but its rotation: what a
 * focus's wait between sweeps doesn't hold back. */
inline bool ecuio_engine_once_waiting(const ecuio_engine_channel& ch)
{
    return ch.walking || ch.scan_pending > 0 || ch.ident_pending > 0 ||
           ch.diag_pending > 0 || ch.clear_step != ECUIO_CLEAR_STEP_NONE ||
           ch.identifying || ch.diagnosing || ch.scanning || ch.clearing ||
           ch.sessioning || ch.session_step == ECUIO_SESSION_STATE_ENTERING ||
           ch.talk.offered || ch.talk.out;
}

/* Microseconds until the engine next has something to do: a wait
 * ending, or a channel's floor passing while it has something to send
 * (with only its rotation left, a focus's next sweep, or held quiet its
 * step, when that is later; with a key's window holding it and no key offered, the
 * window's end); 0 when something is due now, INT64_MAX when nothing
 * is. A channel whose window is open with nothing to send after it
 * reports the window's end too: a driver counts quiet reads only at
 * INT64_MAX, and a link judged silent would end the window. The
 * channels with a bit in skip are left out. */
inline int64_t ecuio_engine_next_event_us(const ecuio_request_engine* e,
                                          int64_t now_us, uint32_t skip = 0)
{
    int64_t next = INT64_MAX;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        if (((skip >> c) & 1u) != 0) {
            continue;
        }
        const bool window = ecuio_key_window_open(ch.key, now_us);
        const bool items = ch.quiet && ecuio_engine_quiet_asks_items(e, ch);
        if (ch.outstanding == ECUIO_ENGINE_NONE && ch.requests == 0 && !items &&
            !ch.walking && ch.scan_pending == 0 && ch.ident_pending == 0 &&
            ch.diag_pending == 0 && ch.clear_step == ECUIO_CLEAR_STEP_NONE &&
            !ch.talk.offered && !ecuio_engine_session_live(e, ch) && !window) {
            continue;
        }
        int64_t at = (ch.outstanding != ECUIO_ENGINE_NONE) ? ch.deadline_us
                                                           : ch.next_send_us;
        const bool asks_more =
            ch.requests > 0 || items || ch.scan_pending > 0 ||
            ch.ident_pending > 0 || ch.diag_pending > 0 || ch.walking ||
            ch.talk.offered || ch.clear_step != ECUIO_CLEAR_STEP_NONE;
        if (ch.outstanding == ECUIO_ENGINE_NONE && e->min_sweep_us > 0 &&
            !ecuio_engine_once_waiting(ch) && ch.next_sweep_us > at) {
            at = ch.next_sweep_us;
        }
        if (ch.outstanding == ECUIO_ENGINE_NONE && !ch.check_due &&
            !ecuio_engine_once_waiting(ch) && ecuio_engine_quiet_held(e, ch)) {
            const int64_t q = ecuio_engine_quiet_due_us(e, ch);
            at = q > at ? q : at;
        }
        if (ch.outstanding == ECUIO_ENGINE_NONE && !asks_more) {
            at = INT64_MAX; /* only the session's keepalive is left */
        }
        if (ch.outstanding == ECUIO_ENGINE_NONE) {
            const int64_t s = ecuio_engine_session_next_us(e, ch);
            if (s < at) {
                at = s;
            }
        }
        if (ch.outstanding == ECUIO_ENGINE_NONE && at != INT64_MAX &&
            !ecuio_talk_key_waits(ch.talk)) {
            const int64_t held = ecuio_engine_key_held_until(e, ch);
            at = held > at ? held : at;
        } else if (window && at == INT64_MAX) {
            at = ch.key.until_us;
        }
        const int64_t in = (at > now_us) ? at - now_us : 0;
        if (in < next) {
            next = in;
        }
    }
    return next;
}

#endif
