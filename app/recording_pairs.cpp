// File/layer: app/recording_pairs.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)
//
// RecordingView::pairsWith: Compare's pairs, by the rule havne-cli view's
// delta takes (cli/signal_pairs.h), so both front ends pair alike.

#include "recordingview.h"

#include "signal_pairs.h" /* cli/: a compared signal's pair */

#include <new>
#include <vector>

QVariantList RecordingView::pairsWith(RecordingView* other) const
{
    const std::vector<Signal> none;
    const std::vector<Signal>& theirs =
        other != nullptr ? other->m_signals : none;
    QVariantList out;
    try {
        const std::vector<uint32_t> pairs = cli::pair_signals(
            static_cast<uint32_t>(m_signals.size()),
            [&](uint32_t k) -> const auto& { return m_signals[k].pub; },
            static_cast<uint32_t>(theirs.size()),
            [&](uint32_t k) -> const auto& { return theirs[k].pub; });
        out.reserve(static_cast<qsizetype>(pairs.size()));
        for (const uint32_t p : pairs) {
            out.append(p == cli::kNoPair ? -1 : static_cast<int>(p));
        }
    } catch (const std::bad_alloc&) {
        out.clear(); // no pairs: Compare draws nothing dashed
    }
    return out;
}
