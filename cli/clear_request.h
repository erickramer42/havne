// File/layer: cli/clear_request.h | havne-cli and the app's bridge: one
//             clear request over many ECUs; public headers only
// Testing: HARNESS: CliClearRunTest.TwoEcusAreOneRequest (tests/
//          cli_clear.py); BridgeTest.ClearCodesAsksEveryEcuInOneRequest
//
// Core takes one clear request at a time and refuses a whole request
// when one target's report has moved since the caller read it
// (ADR-0038, ecuio_session_clear_diagnostics_set). A front end clearing
// every ECU it read asks once through ask_clears, which leaves those
// targets out and asks again for the rest, so each ECU still gets an
// outcome of its own.

#ifndef HAVNE_CLI_CLEAR_REQUEST_H
#define HAVNE_CLI_CLEAR_REQUEST_H

#include "ecuio/core.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cli {

/* Whether target's report is still the one its generation names: read
 * to the end and not clearing */
inline bool clear_report_still(ecuio_session_t* s,
                               const ecuio_clear_target_t& target)
{
    ecuio_channel_diagnostics_t d{};
    return ecuio_session_get_channel_diagnostics(s, target.channel, &d) ==
               ECUIO_OK &&
           d.generation == target.generation &&
           (d.flags & ECUIO_DIAG_COMPLETE) != 0 &&
           (d.flags & ECUIO_DIAG_CLEARING) == 0;
}

/* One clear request (ADR-0038) for targets under flags; each target's
 * status, in their order, OK when taken. Core refuses the whole request
 * when one target's report moved, so those targets are left out
 * (ECUIO_ERR_REPORT_CHANGED) and the rest asked again; any other refusal
 * is every target's. */
inline std::vector<ecuio_status_t>
ask_clears(ecuio_session_t* s, std::vector<ecuio_clear_target_t> targets,
           uint32_t flags)
{
    std::vector<ecuio_status_t> st(targets.size(), ECUIO_ERR_REPORT_CHANGED);
    std::vector<size_t> at(targets.size());
    for (size_t k = 0; k < at.size(); ++k) {
        at[k] = k;
    }
    while (!targets.empty()) {
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_set(
            s, targets.data(), static_cast<uint32_t>(targets.size()), flags);
        std::vector<ecuio_clear_target_t> kept;
        std::vector<size_t> kept_at;
        for (size_t k = 0; rc == ECUIO_ERR_REPORT_CHANGED && k < at.size();
             ++k) {
            if (clear_report_still(s, targets[k])) {
                kept.push_back(targets[k]);
                kept_at.push_back(at[k]);
            }
        }
        if (rc == ECUIO_ERR_REPORT_CHANGED && kept.size() < targets.size()) {
            targets.swap(kept);
            at.swap(kept_at);
            continue;
        }
        for (const size_t k : at) {
            st[k] = rc;
        }
        break;
    }
    return st;
}

} // namespace cli

#endif /* HAVNE_CLI_CLEAR_REQUEST_H */
