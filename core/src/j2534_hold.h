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
// row the focus parked while it waited is dropped, not written. A channel
// out of step (its last request timed out) holds nothing: a silent
// module's learned segmented row would otherwise hold the others for its
// whole wait on every lap. A hold's chosen rows are its engine's table
// rows, so a hold lives no longer than the ecuio_engine_init it follows.
// The hold applies with two or more channels and ?hold= on (the default);
// otherwise j2534_hold_next is due and the order is the channels' own.

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

    j2534_hold()
    {
        for (int32_t& r : chosen) {
            r = ECUIO_ENGINE_NONE;
        }
    }
};

inline bool j2534_hold_applies(const j2534_hold& h,
                               const ecuio_request_engine* e)
{
    return h.on && e->chan_count > 1;
}

/* Whether a channel but c has a request out, or (segmented) one whose
 * reply is expected segmented on a channel in step */
inline bool j2534_hold_other_out(const ecuio_request_engine* e, uint32_t c,
                                 bool segmented)
{
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const int32_t out = e->chans[k].outstanding;
        if (k != c && out != ECUIO_ENGINE_NONE &&
            (!segmented || (e->chans[k].in_step &&
                            ecuio_engine_reply_segmented(e, k, out)))) {
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

/* What channel c writes this pass at now: its chosen request once no other
 * channel has one out (a row the focus parked meanwhile dropped); else,
 * unless another's segmented reply is due or another's chosen request waits
 * (c is then held, a bit in *held_now), due's request, kept chosen instead
 * when its reply is expected segmented while another channel has a request
 * out. NONE: nothing to write. */
inline int32_t j2534_hold_next(j2534_hold& h, ecuio_request_engine* e,
                               uint32_t c, int64_t now_us, uint32_t* held_now)
{
    if (!j2534_hold_applies(h, e)) {
        return ecuio_engine_due(e, c, now_us);
    }
    if (h.chosen[c] != ECUIO_ENGINE_NONE) {
        if (j2534_hold_other_out(e, c, false)) {
            return ECUIO_ENGINE_NONE;
        }
        const int32_t r = h.chosen[c];
        h.chosen[c] = ECUIO_ENGINE_NONE;
        if (r < 0 || !h.rotating[c] ||
            ecuio_engine_in_rotation(e->requests[r])) {
            return r;
        }
    }
    if (j2534_hold_other_out(e, c, true) || j2534_hold_other_chosen(h, e, c)) {
        *held_now |= 1u << c;
        return ECUIO_ENGINE_NONE;
    }
    const int32_t r = ecuio_engine_due(e, c, now_us);
    if (r == ECUIO_ENGINE_NONE || !ecuio_engine_reply_segmented(e, c, r) ||
        !j2534_hold_other_out(e, c, false)) {
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

#endif
