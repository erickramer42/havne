// File/layer: core/src/request_engine_focus.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The focus (ADR-0056 section 2): the parked rows, the counts and the
// sweep with its minimum time.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, RequestTableTest (test_obd2),
//          CoreTest (test_core); HARNESS: each transport's AFocus cases

#ifndef ECUIO_REQUEST_ENGINE_FOCUS_H
#define ECUIO_REQUEST_ENGINE_FOCUS_H

#include "request_engine_table.h"

/* Each channel's count of requests in the rotation, after a focus
 * changed which are in it. A channel whose count changed moves its
 * generation on, so its support report is published again; one whose
 * count stayed keeps its generation: its report reads the same. */
inline void ecuio_engine_recount(ecuio_request_engine* e)
{
    uint32_t counts[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (ecuio_engine_in_rotation(r) && r.channel < e->chan_count) {
            ++counts[r.channel];
        }
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].requests != counts[c]) {
            e->chans[c].requests = counts[c];
            ++e->chans[c].generation;
        }
    }
}

/* Channel c's next request starts a sweep: nothing counted in one */
inline void ecuio_engine_clear_sweep(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.sweep_asked = 0;
    ch.sweep_dropped = 0;
    for (uint32_t i = 0; i < e->count; ++i) {
        if (e->requests[i].channel == c) {
            e->requests[i].swept = 0;
        }
    }
}

/* A focus was asked for: starts every channel's sweep over and sets its
 * minimum time between sweeps. A wait under way stands while the time is
 * the same, and ends with a new one, which judges only the sweeps after
 * it. */
inline void ecuio_engine_restart_sweeps(ecuio_request_engine* e,
                                        int64_t min_sweep_us)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        ecuio_engine_clear_sweep(e, c);
        if (e->min_sweep_us != min_sweep_us) {
            ch.next_sweep_us = 0;
        }
    }
    e->min_sweep_us = min_sweep_us;
}

/* ADR-0056 section 2: parks every request keep (one byte per table row,
 * nonzero kept) leaves out and runs the kept ones in the fast class, so
 * the rotation is as short as the picks allow. The walk, the scan,
 * identification, diagnostics, a clear and a request already out are
 * untouched: a parked request's reply still correlates by the table. A
 * dropped request stays dropped. Alone, it is the driver's answer to
 * focus_joined: the sweep under way and its wait stand, so DIDs found
 * faster than a sweep runs can't keep it from ever ending. */
inline void ecuio_engine_focus_keep(ecuio_request_engine* e,
                                    const uint8_t* keep)
{
    e->kept = 0;
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        r.parked = keep[i] != 0 ? 0 : 1;
        if (r.parked == 0) {
            r.slow = 0;
            ++e->kept;
            if (r.dropped == 0) {
                ecuio_engine_take_row(e, i); /* a dropped one is never asked */
            }
        }
    }
    e->focused = true;
    e->focus_joined = false;
    ecuio_engine_recount(e);
}

/* A focus asked for: ecuio_engine_focus_keep, with a sweep of the picks
 * starting at most once every min_sweep_us (section 4; 0: none) */
inline void ecuio_engine_focus(ecuio_request_engine* e, const uint8_t* keep,
                               int64_t min_sweep_us = 0)
{
    ecuio_engine_restart_sweeps(e, min_sweep_us);
    ecuio_engine_focus_keep(e, keep);
}

/* The focus's end: every parked request back in the rotation, each in
 * the class it was given. Nothing moves when no focus is on. A table
 * that asks only its picks (ecuio_engine_pick_only) parks every request
 * instead, and stays so until the next focus. */
inline void ecuio_engine_unfocus(ecuio_request_engine* e)
{
    if (!e->focused) {
        return;
    }
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        r.parked = e->pick_only ? 1 : 0;
        r.slow = r.slow_default;
    }
    e->focused = e->pick_only;
    e->kept = 0;
    e->focus_joined = false;
    ecuio_engine_restart_sweeps(e, 0);
    ecuio_engine_recount(e);
}

/* Rotation request r has just been chosen on channel c at now_us: counts
 * its rows toward the sweep. The rows are consecutive in the rotation's
 * one class (a focus runs its picks fast), so as many as the rotation
 * holds have asked every pick; a pack that runs past that asks some
 * twice, and its surplus counts for nothing, since the wait can only
 * fall between two requests. A row asked and then dropped
 * (ecuio_engine_drop) left ch.requests but stays in sweep_asked, so it
 * counts in sweep_dropped too. */
inline void ecuio_engine_count_sweep(ecuio_request_engine* e, uint32_t c,
                                     int32_t r, int64_t now_us)
{
    if (e->min_sweep_us == 0) {
        return;
    }
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.sweep_asked == 0) {
        ch.sweep_start_us = now_us;
    }
    if (r == ECUIO_ENGINE_PACK) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            e->requests[ch.pack_rows[k]].swept = 1;
        }
        ch.sweep_asked += ch.pack_count;
    } else if (r >= 0) {
        e->requests[r].swept = 1;
        ++ch.sweep_asked;
    }
    if (ch.sweep_asked >= ch.requests + ch.sweep_dropped) {
        ecuio_engine_clear_sweep(e, c);
        ch.next_sweep_us = ch.sweep_start_us + e->min_sweep_us;
    }
}

#endif
