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
        if (ch.visiting) {
            ch.visiting = false; /* the filter kept back is free again */
            e->visit_now = UINT32_MAX;
        }
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

/* The modules live data holds a filter for: open or closing, a visit's
 * left out (it holds the one kept back) */
inline uint32_t ecuio_engine_modules_held(const ecuio_request_engine* e)
{
    uint32_t n = 0;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        n += ecuio_engine_module_hears(e->chans[c]) && !e->chans[c].visiting
                 ? 1u
                 : 0u;
    }
    return n;
}

/* Opens the modules waiting for a filter while the budget has room, the
 * one waiting longest first */
inline void ecuio_engine_modules_fill(ecuio_request_engine* e)
{
    for (;;) {
        if (e->module_budget != 0 &&
            ecuio_engine_modules_held(e) >= e->module_budget) {
            return;
        }
        uint32_t next = UINT32_MAX;
        for (uint32_t c = 0; c < e->chan_count; ++c) {
            const ecuio_engine_channel& ch = e->chans[c];
            if (ch.module_state == ECUIO_MODULE_STATE_NO_ROOM &&
                (next == UINT32_MAX || ch.waiting < e->chans[next].waiting)) {
                next = c;
            }
        }
        if (next == UINT32_MAX) {
            return;
        }
        ecuio_engine_channel& ch = e->chans[next];
        ch.module_state = ECUIO_MODULE_STATE_OPEN;
        ch.waiting = 0;
        ++ch.generation;
        ecuio_engine_module_reopen(e, next);
    }
}

/* Module c, wanted and holding no filter, waits for one */
inline void ecuio_engine_module_wait(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.reasons != 0 && ch.module_state == ECUIO_MODULE_STATE_CLOSED) {
        ch.module_state = ECUIO_MODULE_STATE_NO_ROOM;
        ch.waiting = ++e->waiting_last;
        ++ch.generation;
    }
}

/* What module ch's support report says of it: its CLOSED or NO_ROOM flag,
 * and with reasons its open_reasons */
inline uint32_t ecuio_engine_module_view(const ecuio_engine_channel& ch,
                                         bool reasons)
{
    const uint32_t flag =
        ch.module_state == ECUIO_MODULE_STATE_CLOSED    ? 1u
        : ch.module_state == ECUIO_MODULE_STATE_NO_ROOM ? 2u
                                                        : 0u;
    return flag | (reasons ? uint32_t{ch.reasons} << 8 : 0u);
}

/* The worker's reasons for every module (reasons[c], ECUIO_ENGINE_REASON_*)
 * and the order the picks first read the modules in (order, count of
 * them). A module no reason wants starts its close (one waiting for a
 * filter closes at once); a closing one wanted again is open at once; a
 * module holding a filter keeps it, never closed for a newcomer. The rest
 * wait for a filter in the order RANGE, CONVERSATION, the picks' order,
 * then channel order, and the budget opens them in that order. The first
 * call holds no filter yet: nothing has been sent. A module's generation
 * moves once when what its report says changed (the first call's
 * reasons replace a placeholder, so only its flag counts then). */
inline void ecuio_engine_modules_apply(ecuio_request_engine* e,
                                       const uint8_t* reasons,
                                       const uint8_t* order, uint32_t count)
{
    const uint32_t n = e->chan_count;
    const bool first_take = !e->modules_taken;
    uint32_t view[ECUIO_MAX_CHANNELS] = {};
    uint32_t generation[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t c = 0; c < n; ++c) {
        view[c] = ecuio_engine_module_view(e->chans[c], !first_take);
        generation[c] = e->chans[c].generation;
    }
    for (uint32_t c = 0; c < n; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (first_take) {
            ch.module_state = ECUIO_MODULE_STATE_CLOSED;
        }
        /* a visit under way keeps its reason to its pass's end */
        ch.reasons = static_cast<uint8_t>(
            reasons[c] | (ch.reasons & ECUIO_ENGINE_REASON_VISIT));
        if (ch.reasons == 0 && ch.module_state == ECUIO_MODULE_STATE_NO_ROOM) {
            ch.module_state = ECUIO_MODULE_STATE_CLOSED;
            ch.waiting = 0;
            ++ch.generation;
        } else if (ch.reasons == 0 &&
                   ch.module_state == ECUIO_MODULE_STATE_OPEN) {
            ch.module_state = ECUIO_MODULE_STATE_CLOSING;
        } else if (ch.reasons != 0 &&
                   ch.module_state == ECUIO_MODULE_STATE_CLOSING) {
            ch.module_state = ECUIO_MODULE_STATE_OPEN;
            ecuio_engine_module_reopen(e, c);
        }
    }
    e->modules_taken = true;
    for (const uint8_t first :
         {ECUIO_ENGINE_REASON_RANGE, ECUIO_ENGINE_REASON_CONVERSATION}) {
        for (uint32_t c = 0; c < n; ++c) {
            if ((reasons[c] & first) != 0) {
                ecuio_engine_module_wait(e, c);
            }
        }
    }
    for (uint32_t k = 0; k < count; ++k) {
        if (order[k] < n) {
            ecuio_engine_module_wait(e, order[k]);
        }
    }
    for (uint32_t c = 0; c < n; ++c) {
        ecuio_engine_module_wait(e, c);
    }
    ecuio_engine_modules_fill(e);
    for (uint32_t c = 0; c < n; ++c) {
        const bool moved =
            ecuio_engine_module_view(e->chans[c], !first_take) != view[c];
        e->chans[c].generation = generation[c] + (moved ? 1u : 0u);
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
