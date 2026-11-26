// File/layer: core/src/signal_set_module.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalSetModuleTest (test_signals)
//
// The module a def of a set is read on (signals.h,
// ecuio_signal_set_def_module): the enrolment's channel rule
// (request_engine_table.h) for a def a session asks, the reply ID for one
// nothing asks, and a computed def's leaves (computed.h's walk) when they
// share one, so a front end's filter agrees with what a session opens.

#include "ecuio/signals.h"

#include "computed.h"
#include "request_engine_table.h"

#include <cstdint>
#include <functional>
#include <new>
#include <vector>

namespace {

/* A def that isn't computed: where it is read, or NONE */
uint32_t leaf_module(const ecuio_signal_set_t& set, const ecuio_signal_def_t& d,
                     const uint32_t* rx, uint32_t n)
{
    uint8_t request[3];
    uint8_t length = 0;
    if (ecuio_engine_request_for_def(&d, request, &length)) {
        return ecuio_engine_channel_for(&d, rx, n);
    }
    for (uint32_t c = 0; c < set.channel_count; ++c) {
        if (rx[c] == d.message_id) {
            return c;
        }
    }
    return ECUIO_SIGNAL_MODULE_NONE;
}

/* The module every number def under the computed def at index is read
 * on, or NONE */
uint32_t computed_module(const ecuio_signal_set_t& set, uint32_t index,
                         const uint32_t* rx, uint32_t n)
{
    std::vector<uint8_t> marks;
    try {
        marks.assign(set.count, 0);
    } catch (const std::bad_alloc&) {
        return ECUIO_SIGNAL_MODULE_NONE;
    }
    marks[index] = 1;
    ecuio_computed_mark_leaves(&set, marks.data());
    uint32_t shared = ECUIO_SIGNAL_MODULE_NONE;
    for (uint32_t i = 0; i < set.count; ++i) {
        const ecuio_signal_def_t& d = set.defs[i];
        if (marks[i] == 0 || (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue;
        }
        const uint32_t m = leaf_module(set, d, rx, n);
        if (m == ECUIO_SIGNAL_MODULE_NONE ||
            (shared != ECUIO_SIGNAL_MODULE_NONE && m != shared)) {
            return ECUIO_SIGNAL_MODULE_NONE;
        }
        shared = m;
    }
    return shared;
}

} // namespace

extern "C" uint32_t ecuio_signal_set_def_module(const ecuio_signal_set_t* set,
                                                const ecuio_signal_def_t* def)
{
    if (set == nullptr || def == nullptr || set->defs == nullptr ||
        set->channels == nullptr || set->channel_count == 0 ||
        set->channel_count > ECUIO_MAX_CHANNELS ||
        std::less<const ecuio_signal_def_t*>()(def, set->defs) ||
        !std::less<const ecuio_signal_def_t*>()(def, set->defs + set->count)) {
        return ECUIO_SIGNAL_MODULE_NONE;
    }
    uint32_t rx[ECUIO_MAX_CHANNELS];
    for (uint32_t c = 0; c < set->channel_count; ++c) {
        rx[c] = set->channels[c].rx_id;
    }
    const uint32_t index = static_cast<uint32_t>(def - set->defs);
    if ((def->flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
        return computed_module(*set, index, rx, set->channel_count);
    }
    return leaf_module(*set, *def, rx, set->channel_count);
}
