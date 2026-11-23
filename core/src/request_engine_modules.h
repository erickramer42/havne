// File/layer: core/src/request_engine_modules.h | internal, not
//             ABI-exported; a part of the request engine, included by
//             request_engine.h, whose description covers it
// Which modules are open and how many requests they have out (ADR-0065
// step 5): the reasons a module is open, its close once what it began
// settles and its reopen; the cap and the transport's ceiling (sections
// 5 and 11). The gate every send passes, ecuio_engine_module_open, is in
// request_engine_state.h, the closing module's due in _send.h, and the
// free slot's picker, ecuio_engine_next_slot, beside the turns in
// request_turns.h.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestModulesTest, RequestSlotsTest (test_obd2);
//          HARNESS: J2534HealthTest (test_j2534_health)

#ifndef ECUIO_REQUEST_ENGINE_MODULES_H
#define ECUIO_REQUEST_ENGINE_MODULES_H

#include "request_engine_reply.h"

#include <cstdint>

/* The cap on requests out at once and the transport's ceiling, each at
 * least 1: attach calls it after ecuio_engine_init */
inline void ecuio_engine_slots(ecuio_request_engine* e, uint32_t cap,
                               uint32_t ceiling)
{
    e->cap = cap > 0 ? cap : 1u;
    e->ceiling = ceiling > 0 ? ceiling : 1u;
}

/* Whether closing module c is done: nothing out, nothing it began still
 * owed, and no key's window standing */
inline bool ecuio_engine_closing_done(const ecuio_request_engine* e,
                                      uint32_t c, int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    return ch.outstanding == ECUIO_ENGINE_NONE &&
           !ecuio_engine_closing_owes(e, ch) &&
           !ecuio_key_window_open(ch.key, now_us);
}

/* Each closing module that is done becomes closed: its answering bit,
 * its quiet run and heard end, a session it was entering is let go, and
 * what it learned stays. The modules closed now, a bit each (the worker
 * drops their filters). */
inline uint32_t ecuio_engine_modules_settle(ecuio_request_engine* e,
                                            int64_t now_us)
{
    uint32_t closed = 0;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.module_state != ECUIO_MODULE_STATE_CLOSING ||
            !ecuio_engine_closing_done(e, c, now_us)) {
            continue;
        }
        ch.module_state = ECUIO_MODULE_STATE_CLOSED;
        e->answering &= ~(1u << c);
        ch.quiet_run = 0;
        ch.quiet = false;
        ch.quiet_settled = false;
        ch.heard = false;
        ch.check_due = false;
        if (e->session != 0) {
            ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_NOT_ASKED);
        }
        ++ch.generation;
        closed |= 1u << c;
    }
    return closed;
}

/* Module c opened again: its unanswered identification items and scan
 * entries are asked again, never its diagnostics (a complete report
 * keeps its generation, which a saved clear names), and the session is
 * entered again only for a read (L10 makes the entry follow READ) */
inline void ecuio_engine_module_reopen(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        if (s.channel == c && s.outcome == ECUIO_SCAN_UNANSWERED) {
            s.outcome = ECUIO_SCAN_UNASKED;
            s.retried = 0;
            ++ch.scan_pending;
        }
    }
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        if (ch.ident[i].outcome == ECUIO_IDENTITY_UNANSWERED) {
            ch.ident[i].outcome = ECUIO_IDENTITY_UNASKED;
            ++ch.ident_pending;
        }
    }
    if (e->session != 0 && (ch.reasons & ECUIO_ENGINE_REASON_READ) != 0 &&
        ch.session_step == ECUIO_SESSION_STATE_NOT_ASKED) {
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ENTERING);
    }
}

/* The reasons that want module c (ECUIO_ENGINE_REASON_*), worked out by
 * the worker: any opens it (asking again only what it never answered),
 * none starts its close, which ends once what it began has settled
 * (ecuio_engine_modules_settle). A closing module wanted again is open
 * at once. */
inline void ecuio_engine_module_want(ecuio_request_engine* e, uint32_t c,
                                     uint8_t reasons)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.reasons = reasons;
    if (reasons == 0) {
        if (ch.module_state == ECUIO_MODULE_STATE_OPEN) {
            ch.module_state = ECUIO_MODULE_STATE_CLOSING;
        }
        return;
    }
    if (ch.module_state == ECUIO_MODULE_STATE_OPEN) {
        return;
    }
    const bool closed = ch.module_state != ECUIO_MODULE_STATE_CLOSING;
    ch.module_state = ECUIO_MODULE_STATE_OPEN;
    if (closed) {
        ++ch.generation;
    }
    ecuio_engine_module_reopen(e, c);
}

#endif
