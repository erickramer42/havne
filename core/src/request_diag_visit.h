// File/layer: core/src/request_diag_visit.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// A visit (ADR-0065 step 5): a read of a closed module's codes opens it,
// one module at a time in the filter kept back from live data, asks its
// pass (identification still unsettled, then the diagnostics read; never
// the walk, a row, a scanned DID or a session), and closes it after. A
// visit that finds no filter free ends the read at once: COMPLETE, every
// item unasked, ECUIO_DIAG_NOT_OPENED. The visiting module's own due is
// ecuio_engine_due_visit (_send.h); the worker queues and steps visits
// from request_setup.h and request_modules.h.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: DiagVisitTest (test_obd2)

#ifndef ECUIO_REQUEST_DIAG_VISIT_H
#define ECUIO_REQUEST_DIAG_VISIT_H

#include "request_engine_modules.h"

#include <cstdint>

/* A read is wanted of module c's codes: an open module reads in place,
 * a closed one (or one waiting for a filter) joins the visits' queue,
 * once, in the order asked */
inline void ecuio_engine_visit_want(ecuio_request_engine* e, uint32_t c)
{
    if (c >= e->chan_count || c >= ECUIO_MAX_CHANNELS ||
        ecuio_engine_module_hears(e->chans[c]) ||
        ((e->visit_want >> c) & 1u) != 0) {
        return;
    }
    e->visit_want |= 1u << c;
    e->visit_order[e->visit_count++] = static_cast<uint8_t>(c);
}

/* Every module not open that owes a diagnostics read is visited: the
 * session-wide read's (ecuio_take_diag_request) */
inline void ecuio_engine_visit_closed(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].diag_pending > 0) {
            ecuio_engine_visit_want(e, c);
        }
    }
}

/* Module c's read can't open: its report is COMPLETE now, its items left
 * unasked, ECUIO_DIAG_NOT_OPENED set, the generation moved */
inline void ecuio_engine_visit_not_opened(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.diag_pending = 0;
    ch.diag.flags |= ECUIO_DIAG_COMPLETE | ECUIO_DIAG_NOT_OPENED;
    ++ch.diag.generation;
}

/* The first queued visit, taken off the queue; UINT32_MAX for none */
inline uint32_t ecuio_engine_visit_next(ecuio_request_engine* e)
{
    if (e->visit_count == 0) {
        return UINT32_MAX;
    }
    const uint32_t c = e->visit_order[0];
    for (uint32_t k = 1; k < e->visit_count; ++k) {
        e->visit_order[k - 1] = e->visit_order[k];
    }
    --e->visit_count;
    e->visit_want &= ~(1u << c);
    return c;
}

/* The visiting module's pass is done: the VISIT reason drops, and it
 * closes (holding the filter kept back until it has), or, wanted for
 * another reason, waits for a live filter (in its old place when it was
 * waiting before) and opens if one is free */
inline void ecuio_engine_visit_end(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.reasons = static_cast<uint8_t>(ch.reasons & ~ECUIO_ENGINE_REASON_VISIT);
    ++ch.generation;
    if (ch.reasons == 0) {
        ch.module_state = ECUIO_MODULE_STATE_CLOSING;
        return;
    }
    ch.module_state = ECUIO_MODULE_STATE_NO_ROOM;
    if (!ch.visit_from_no_room) {
        ch.waiting = ++e->waiting_last;
    }
    ch.visiting = false;
    e->visit_now = UINT32_MAX;
    ecuio_engine_modules_fill(e);
}

/* The worker's step, every pass: the visit under way ends once its pass
 * is done; with none under way the next queued one opens in the filter
 * kept back, or, with none free, ends its read NOT_OPENED and the next is
 * tried. A module opened meanwhile reads in place and leaves the queue. */
inline void ecuio_engine_visits_step(ecuio_request_engine* e)
{
    if (e->visit_now != UINT32_MAX) {
        const uint32_t c = e->visit_now;
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.module_state == ECUIO_MODULE_STATE_OPEN &&
            ch.outstanding == ECUIO_ENGINE_NONE &&
            !ecuio_engine_visit_owes(ch)) {
            ecuio_engine_visit_end(e, c);
        }
        return;
    }
    for (;;) {
        const uint32_t c = ecuio_engine_visit_next(e);
        if (c == UINT32_MAX) {
            return;
        }
        ecuio_engine_channel& ch = e->chans[c];
        if (ecuio_engine_module_hears(ch)) {
            continue;
        }
        if (e->filters_most != 0 &&
            ecuio_engine_modules_held(e) >= e->filters_most) {
            ecuio_engine_visit_not_opened(e, c);
            continue;
        }
        ch.visit_from_no_room = ch.module_state == ECUIO_MODULE_STATE_NO_ROOM;
        ch.module_state = ECUIO_MODULE_STATE_OPEN;
        ch.reasons =
            static_cast<uint8_t>(ch.reasons | ECUIO_ENGINE_REASON_VISIT);
        ch.visiting = true;
        e->visit_now = c;
        ++ch.generation;
        return;
    }
}

#endif
