// File/layer: cli/default_pick.h | havne-cli and the app, a session's default pick (ADR-0056)
// Testing: UNIT: CliPickTest (test_cli); BRIDGE: BridgeTest (test_bridge)
//
// A front end's default pick: the signals a session asks when nobody
// named them, set as a focus under ECUIO_FOCUS_DEFAULT_PICK. It is the
// set's ranked signals (plot_rank, the ones a UI plots first) and the
// signals the front end adds: the app's gauges and ticked rows, the
// CLI's --signal names. A set that ranks none is asked whole, so an
// empty pick means no focus: uds-generic, whose URI is already the
// user's pick, and a signal file with no plot keys ask what they did.
// A pick-only session (ECUIO_SESSION_INFO_PICK_ONLY) asks nothing
// without a focus, so there the pick is the ranked and added defs even
// when the set ranks none, and an empty pick asks nothing.
// A text def's rank doesn't count, as Live's default plot leaves text
// rows out. A condition's signals core keeps asked itself. Public
// headers only, so the app and havne-cli pick the same.

#ifndef HAVNE_CLI_DEFAULT_PICK_H
#define HAVNE_CLI_DEFAULT_PICK_H

#include "ecuio/signals.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cli {

// A def the default pick takes by its rank
inline bool ranked_def(const ecuio_signal_def_t& def)
{
    return def.plot_rank != 0 && def.kind != ECUIO_VALUE_TEXT &&
           def.kind != ECUIO_VALUE_HEX;
}

// Whether the set ranks any def: one that ranks none is asked whole
inline bool set_ranks(const ecuio_signal_set_t* set)
{
    if (set == nullptr || set->defs == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        if (ranked_def(set->defs[i])) {
            return true;
        }
    }
    return false;
}

// The pick in the set's order, each def once: its ranked defs and those
// whose id `added` names (an id the set lacks is passed by). Empty when
// the set ranks none: set no focus, and the whole set is asked; with
// `pick_only` the added defs are picked even then, and an empty result
// asks nothing.
inline std::vector<uint16_t> default_pick(const ecuio_signal_set_t* set,
                                          const std::vector<uint16_t>& added,
                                          bool pick_only = false)
{
    std::vector<uint16_t> pick;
    if (set == nullptr || set->defs == nullptr ||
        (!pick_only && !set_ranks(set))) {
        return pick;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (ranked_def(d) ||
            std::find(added.begin(), added.end(), d.id) != added.end()) {
            pick.push_back(d.id);
        }
    }
    return pick;
}

} // namespace cli

#endif
