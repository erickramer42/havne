// File/layer: core/src/request_modules.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: CoreTest (test_core); HARNESS: Elm327SessionTest
//          (test_elm327), J2534HealthTest (test_j2534_health)
//
// The session's modules: the tables attach sizes once and the reports it
// seeds before the worker runs (ADR-0065), which request_setup.h includes.

#ifndef ECUIO_REQUEST_MODULES_H
#define ECUIO_REQUEST_MODULES_H

#include "ecuio/core.h"
#include "request_engine.h"
#include "session.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/* The session's modules as attach resolved them: every one's IDs, the
 * info's count and its first ECUIO_SESSION_INFO_MAX_CHANNELS, and room
 * for each one's reports and clear requests, each report naming its
 * ECU. Before create returns; nothing sizes them afterwards. Out of
 * memory is ECUIO_ERR_OUT_OF_MEMORY, the session unchanged. */
inline ecuio_status_t ecuio_session_set_modules(ecuio_session_t* session,
                                                const uint32_t* rx_ids,
                                                const uint32_t* tx_ids,
                                                uint32_t count)
{
    /* Made whole first, then handed over: a table that doesn't
     * allocate leaves the session as it was */
    std::vector<uint32_t> rx;
    std::vector<uint32_t> tx;
    std::vector<ecuio_channel_support_t> support;
    std::vector<ecuio_channel_identity_t> identity;
    std::vector<ecuio_channel_diagnostics_t> diagnostics;
    std::unique_ptr<std::atomic<uint32_t>[]> generation;
    std::unique_ptr<std::atomic<uint32_t>[]> flags;
    std::unique_ptr<std::atomic<uint32_t>[]> requests;
    try {
        rx.assign(rx_ids, rx_ids + count);
        tx.assign(tx_ids, tx_ids + count);
        support.assign(count, ecuio_channel_support_t{});
        identity.assign(count, ecuio_channel_identity_t{});
        diagnostics.assign(count, ecuio_channel_diagnostics_t{});
        generation.reset(new std::atomic<uint32_t>[count]());
        flags.reset(new std::atomic<uint32_t>[count]());
        requests.reset(new std::atomic<uint32_t>[count]());
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    session->module_rx.swap(rx);
    session->module_tx.swap(tx);
    session->support.swap(support);
    session->identity.swap(identity);
    session->diagnostics.swap(diagnostics);
    session->clear_generation = std::move(generation);
    session->clear_flags = std::move(flags);
    session->clear_requests = std::move(requests);
    session->support_count = count;
    session->info.module_count = count;
    session->info.channel_count =
        count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? count
            : uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS};
    for (uint32_t c = 0; c < count; ++c) {
        if (c < ECUIO_SESSION_INFO_MAX_CHANNELS) {
            session->info.rx_ids[c] = rx_ids[c];
            session->info.tx_ids[c] = tx_ids[c];
        }
        session->support[c].rx_id = rx_ids[c];
        session->identity[c].rx_id = rx_ids[c];
        session->diagnostics[c].rx_id = rx_ids[c];
    }
    return ECUIO_OK;
}

/* Each channel's ECU, its requests, its identification and diagnostics
 * items, nothing learned yet: what attach hands the session before the
 * worker runs, after ecuio_session_set_modules, whose modules are the
 * only ones seeded. */
inline void ecuio_seed_reports(ecuio_session_t* session,
                               const ecuio_request_engine& e,
                               const uint32_t* rx_ids, uint32_t chan_count)
{
    std::lock_guard<std::mutex> lock(session->support_mutex);
    const uint32_t modules = static_cast<uint32_t>(session->support.size());
    const uint32_t count = chan_count < modules ? chan_count : modules;
    session->support_count = count;
    for (uint32_t c = 0; c < count; ++c) {
        ecuio_engine_support(&e, c, &session->support[c]);
        session->support[c].rx_id = rx_ids[c];
        ecuio_engine_identity(&e, c, &session->identity[c]);
        session->identity[c].rx_id = rx_ids[c];
        ecuio_engine_diagnostics(&e, c, &session->diagnostics[c]);
        session->diagnostics[c].rx_id = rx_ids[c];
    }
}

#endif /* ECUIO_REQUEST_MODULES_H */
