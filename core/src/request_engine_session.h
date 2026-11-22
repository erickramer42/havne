// File/layer: core/src/request_engine_session.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// Session control (ADR-0060): entering, keeping, re-entering and
// leaving the extended session.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: SessionControlTest (test_obd2); HARNESS: each transport's
//          close cases

#ifndef ECUIO_REQUEST_ENGINE_SESSION_H
#define ECUIO_REQUEST_ENGINE_SESSION_H

#include "request_engine_services.h"
#include "request_engine_rotation.h"

/* ADR-0060: every channel enters session sub (only the extended session,
 * 03) before anything else goes out, and keeps it with tester present
 * after keepalive_us without a request (clamped to the bounds). False,
 * and nothing changed, for any other session. */
inline bool ecuio_engine_session(ecuio_request_engine* e, uint8_t sub,
                                 int64_t keepalive_us)
{
    if (sub != ECUIO_SESSION_EXTENDED ||
        !ecuio_engine_session_request_allowed(ECUIO_SESSION_CONTROL, sub)) {
        return false;
    }
    e->session = sub;
    e->keepalive_us = keepalive_us < ECUIO_ENGINE_KEEPALIVE_MIN_US
                          ? ECUIO_ENGINE_KEEPALIVE_MIN_US
                      : keepalive_us > ECUIO_ENGINE_KEEPALIVE_MAX_US
                          ? ECUIO_ENGINE_KEEPALIVE_MAX_US
                          : keepalive_us;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_session_step(e->chans[c], ECUIO_SESSION_STATE_ENTERING);
    }
    return true;
}

/* Whether channel c has a session to enter or keep */
inline bool ecuio_engine_session_live(const ecuio_request_engine* e,
                                      const ecuio_engine_channel& ch)
{
    return e->session != 0 &&
           (ch.session_step == ECUIO_SESSION_STATE_ENTERING ||
            ch.session_step == ECUIO_SESSION_STATE_ENTERED);
}

/* When the key's window that keeps channel ch from sending ends: its own,
 * or for a functional channel the latest of every channel's; 0 for none.
 * A window that ended by the time alone still reads here, in the past. */
inline int64_t ecuio_engine_key_held_until(const ecuio_request_engine* e,
                                           const ecuio_engine_channel& ch)
{
    if (!ch.functional) {
        return ch.key.until_us;
    }
    int64_t latest = 0;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].key.until_us > latest) {
            latest = e->chans[c].key.until_us;
        }
    }
    return latest;
}

/* Whether a functional channel was asked into a session. Another
 * channel's window would hold its keep-alive, a long one past S3, and
 * the 10 03 that follows returns every module that hears it to a new
 * session. So an engine with one takes no seed (ecuio_engine_offer_talk). */
inline bool ecuio_engine_functional_session(const ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        if (ch.functional && ch.session_step != ECUIO_SESSION_STATE_NOT_ASKED) {
            return true;
        }
    }
    return false;
}

/* Whether a key's window keeps channel ch from sending at now_us */
inline bool ecuio_engine_key_awaited(const ecuio_request_engine* e,
                                     const ecuio_engine_channel& ch,
                                     int64_t now_us)
{
    return now_us < ecuio_engine_key_held_until(e, ch);
}

/* Ends channel c's window: the caller's, when no key follows a seed (one
 * of all zeroes: the level is unlocked) */
inline void ecuio_engine_key_window_end(ecuio_request_engine* e, uint32_t c)
{
    e->chans[c].key.until_us = 0;
}

/* Ends every channel's window: the driver's call while its link isn't
 * OK, since a seed may not have outlived what failed */
inline void ecuio_engine_key_windows_end(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        e->chans[c].key.until_us = 0;
    }
}

/* When channel c's session next needs a request: at once while 10 03 is
 * due, keepalive_us after its last request while entered, neither before
 * a key's window that holds the channel ends; else never */
inline int64_t ecuio_engine_session_next_us(const ecuio_request_engine* e,
                                            const ecuio_engine_channel& ch)
{
    if (!ecuio_engine_session_live(e, ch)) {
        return INT64_MAX;
    }
    int64_t at = ch.next_send_us;
    if (ch.session_step == ECUIO_SESSION_STATE_ENTERED &&
        ch.last_sent_us + e->keepalive_us > at) {
        at = ch.last_sent_us + e->keepalive_us;
    }
    const int64_t held = ecuio_engine_key_held_until(e, ch);
    return held > at ? held : at;
}

/* When channel c's session is urgent (request_turns.h's rule 4): at once
 * while 10 03 is due; while entered, max(keepalive_us, S3 less
 * ECUIO_ENGINE_S3_MARGIN_US) after its last request, the margin left for
 * the exchanges ahead of its hand-over; neither before a key's window
 * that holds the channel ends; else never */
inline int64_t ecuio_engine_session_urgent_us(const ecuio_request_engine* e,
                                              const ecuio_engine_channel& ch)
{
    if (!ecuio_engine_session_live(e, ch)) {
        return INT64_MAX;
    }
    int64_t at = ch.next_send_us;
    if (ch.session_step == ECUIO_SESSION_STATE_ENTERED) {
        const int64_t s3 = ECUIO_ENGINE_S3_US - ECUIO_ENGINE_S3_MARGIN_US;
        const int64_t wait = e->keepalive_us > s3 ? e->keepalive_us : s3;
        at = ch.last_sent_us + wait > at ? ch.last_sent_us + wait : at;
    }
    const int64_t held = ecuio_engine_key_held_until(e, ch);
    return held > at ? held : at;
}

/* Session request sid sub into channel c's one-shot, through the
 * allow-list: a request it refuses goes nowhere, and the channel reads in
 * default. False when it was refused. */
inline bool ecuio_engine_session_fill(ecuio_request_engine* e, uint32_t c,
                                      uint8_t sid, uint8_t sub)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (!ecuio_engine_session_request_allowed(sid, sub)) {
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_REFUSED);
        ch.session_nrc = 0;
        ++ch.generation;
        return false;
    }
    ch.once = ecuio_request{};
    ch.once.bytes[0] = sid;
    ch.once.bytes[1] = sub;
    ch.once.len = 2;
    ch.once.channel = static_cast<uint8_t>(c);
    ch.sessioning = true;
    return true;
}

/* Channel c's session entry, when due at now_us: 10 <session> to enter,
 * and again once S3 has passed with nothing sent (the session lost). Into
 * the one-shot; true when it went there. */
inline bool ecuio_engine_session_entry_due(ecuio_request_engine* e, uint32_t c,
                                           int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (!ecuio_engine_session_live(e, ch)) {
        return false;
    }
    if (ch.session_step == ECUIO_SESSION_STATE_ENTERED &&
        now_us - ch.last_sent_us >= ECUIO_ENGINE_S3_US) {
        ecuio_engine_session_step(ch,
                                  ECUIO_SESSION_STATE_ENTERING); /* lapsed */
        ecuio_engine_count16(ch.session_lost);
        ++ch.generation;
    }
    return ch.session_step == ECUIO_SESSION_STATE_ENTERING &&
           ecuio_engine_session_fill(e, c, ECUIO_SESSION_CONTROL, e->session);
}

/* Channel c's keep-alive, when due at now_us: 3E 00 once entered with
 * nothing sent for keepalive_us. ecuio_engine_due asks it last, so any
 * other request due stands in for it (ADR-0060: any request restarts
 * S3). Into the one-shot; true when it went there. */
inline bool ecuio_engine_keepalive_due(ecuio_request_engine* e, uint32_t c,
                                       int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    return e->session != 0 && ch.session_step == ECUIO_SESSION_STATE_ENTERED &&
           now_us - ch.last_sent_us >= e->keepalive_us &&
           ecuio_engine_session_fill(e, c, ECUIO_TESTER_PRESENT, 0x00);
}

/* Settles channel c's session request: a positive reply to 10 xx enters
 * the session; a refusal of it reads on in default with its code; one
 * unanswered reads on in default until ecuio_engine_rescan; tester
 * present unanswered means the session may be gone, so it's entered
 * again, and refused leaves it standing (the other requests keep it). */
inline void ecuio_engine_session_settle(ecuio_request_engine* e, uint32_t c,
                                        uint8_t outcome, uint8_t nrc,
                                        int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.sessioning = false;
    if (ch.once.bytes[0] == ECUIO_TESTER_PRESENT) {
        if (outcome == ECUIO_DIAG_UNANSWERED) {
            ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ENTERING);
            ecuio_engine_count16(ch.session_lost);
            ++ch.generation;
        }
        return;
    }
    if (outcome == ECUIO_DIAG_ANSWERED) {
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ENTERED);
        ch.session_nrc = 0;
        ch.session_fresh = true;
        ch.last_sent_us = now_us;
        ecuio_engine_count16(ch.session_entries);
    } else if (outcome == ECUIO_DIAG_REFUSED) {
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_REFUSED);
        ch.session_nrc = nrc;
    } else {
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_UNANSWERED);
    }
    ++ch.generation;
}

/* Whether a and b ask the same thing: the session's memory of a refused
 * request, which may have been a one-shot then and a rotation row now */
inline bool ecuio_engine_same_request(const ecuio_request& a,
                                      const ecuio_request& b)
{
    if (a.len == 0 || a.len != b.len) {
        return false;
    }
    for (uint8_t k = 0; k < a.len; ++k) {
        if (a.bytes[k] != b.bytes[k]) {
            return false;
        }
    }
    return true;
}

/* A 7E or 7F reply on channel c in its session, to a request other than
 * the session's: the ECU has left the session (a reset, S3 run out on its
 * side), unless it was entered and has answered nothing else since, or
 * the request is the one whose refusal made the last re-entry and hasn't
 * answered since: then it's the request's own answer, and the loss that
 * refusal counted is given back once. Otherwise the session is entered
 * again and the request isn't counted against itself: it's asked first
 * once back in (a one-shot goes back unasked, a rotation request's
 * cursor goes back to it), so each DID kept to another session is
 * settled before the next can add a loss. The
 * ECUIO_SESSION_LOSSES_MAX-th within the window abandons the session
 * instead: a module that keeps leaving it reads in default. True when it
 * was entered again; false leaves the reply to the usual rules. */
inline bool ecuio_engine_session_reenter(ecuio_request_engine* e, uint32_t c,
                                         int32_t out, uint8_t code,
                                         int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (e->session == 0 || ch.session_step != ECUIO_SESSION_STATE_ENTERED ||
        ch.sessioning || ch.clearing || out == ECUIO_ENGINE_PACK ||
        (code != ECUIO_NRC_SUBFUNCTION_NOT_IN_ACTIVE_SESSION &&
         code != ECUIO_NRC_NOT_IN_ACTIVE_SESSION)) {
        return false;
    }
    const ecuio_request& r = ecuio_engine_request_of(e, c, out);
    const bool again = ecuio_engine_same_request(ch.session_refused, r);
    if (ch.session_fresh || again) {
        if (again && !ch.refused_own) {
            ch.refused_own = true;
            if (ch.refused_losses > 0) {
                --ch.refused_losses;
            }
            if (ch.session_lost > 0 && ch.session_lost < UINT16_MAX) {
                --ch.session_lost; /* a saturated count stays */
            }
            ++ch.generation;
        }
        return false;
    }
    if (ch.refused_losses == 0 ||
        now_us - ch.loss_window_us >= ECUIO_ENGINE_LOSS_WINDOW_US) {
        ch.refused_losses = 0;
        ch.loss_window_us = now_us;
    }
    if (++ch.refused_losses >= ECUIO_SESSION_LOSSES_MAX) {
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ABANDONED);
        ch.session_nrc = code;
        ++ch.generation;
        return false;
    }
    ch.session_refused = r;
    ch.refused_own = false;
    ch.outstanding = ECUIO_ENGINE_NONE;
    ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ENTERING);
    ecuio_engine_count16(ch.session_lost);
    ++ch.generation;
    if (out != ECUIO_ENGINE_ONCE) {
        ecuio_request& row = e->requests[out];
        (row.slow != 0 ? ch.slow_cursor : ch.cursor) =
            static_cast<uint32_t>(out);
        if (row.swept != 0 && ch.sweep_asked > 0) {
            row.swept = 0; /* counted at its ask after the entry instead */
            --ch.sweep_asked;
        }
        return true;
    }
    if (ch.scanning) {
        ch.scanning = false;
        ch.scan_cursor = ch.scan_index; /* asked first once back in */
    }
    ch.identifying = false; /* the item is still unasked */
    ch.diagnosing = false;
    if (ch.talk.out) { /* not asked again: its unlock went with the session */
        ecuio_talk_settle(ch.talk, ch.key, ecuio_talk_end::refused, code,
                          now_us);
    }
    return true;
}

/* At a close: the request that returns channel c to the default session
 * (10 01), when it entered one, into *out. False when it never did. */
inline bool ecuio_engine_session_leave(ecuio_request_engine* e, uint32_t c,
                                       ecuio_request* out)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (e->session == 0 || ch.session_entries == 0 ||
        !ecuio_engine_session_request_allowed(ECUIO_SESSION_CONTROL,
                                              ECUIO_SESSION_DEFAULT)) {
        return false;
    }
    *out = ecuio_request{};
    out->bytes[0] = ECUIO_SESSION_CONTROL;
    out->bytes[1] = ECUIO_SESSION_DEFAULT;
    out->len = 2;
    out->channel = static_cast<uint8_t>(c);
    ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_NOT_ASKED);
    return true;
}

/* At a close begun at start_us, how long a request still out on a
 * channel leave[] marks is read for before its 10 01: to the latest such
 * deadline, never past start_us + ECUIO_ENGINE_WAIT_US, so a 78 extends
 * nothing. 0 when none is out. */
inline int64_t ecuio_engine_leave_wait_until(const ecuio_request_engine* e,
                                             const bool* leave,
                                             int64_t start_us)
{
    int64_t until = 0;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        if (leave[c] && ch.outstanding != ECUIO_ENGINE_NONE &&
            ch.deadline_us > until) {
            until = ch.deadline_us;
        }
    }
    const int64_t bound = start_us + ECUIO_ENGINE_WAIT_US;
    return (until > bound) ? bound : until;
}

#endif
