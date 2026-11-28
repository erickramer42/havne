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
#include "consent_gate.h"
#include "conversation_bind.h"
#include "request_engine.h"
#include "session.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/* The session's modules as attach resolved them: every one's IDs, the
 * info's count and its first ECUIO_SESSION_INFO_MAX_CHANNELS, and room
 * for each one's reports, each report naming its ECU. Before create returns; nothing sizes them afterwards. Out of
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
    try {
        rx.assign(rx_ids, rx_ids + count);
        tx.assign(tx_ids, tx_ids + count);
        support.assign(count, ecuio_channel_support_t{});
        identity.assign(count, ecuio_channel_identity_t{});
        diagnostics.assign(count, ecuio_channel_diagnostics_t{});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    session->module_rx.swap(rx);
    session->module_tx.swap(tx);
    session->support.swap(support);
    session->identity.swap(identity);
    session->diagnostics.swap(diagnostics);
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

/* The modules a pick and the attached conditions read (ADR-0065 step 5's
 * READ): a bit for each, and the modules in the order of their first
 * pick (the caller's ids, then a computed pick's leaves and the
 * conditions' signals in set order) */
struct ecuio_modules_read {
    uint32_t mask = 0;
    uint8_t order[ECUIO_MAX_CHANNELS] = {};
    uint32_t count = 0;
};

/* Adds the module def i is asked on, by the def (a scanned DID not yet
 * found still names its module); a def that asks nothing (a passive
 * one, a computed one) reads none */
inline void ecuio_modules_add_def(ecuio_modules_read& m,
                                  const ecuio_signal_set_t* set, uint32_t i,
                                  const uint32_t* module_rx,
                                  uint32_t module_count)
{
    uint8_t q[3];
    uint8_t len = 0;
    if (!ecuio_engine_request_for_def(&set->defs[i], q, &len)) {
        return;
    }
    const uint32_t c =
        ecuio_engine_channel_for(&set->defs[i], module_rx, module_count);
    if (c < module_count && c < ECUIO_MAX_CHANNELS &&
        ((m.mask >> c) & 1u) == 0) {
        m.mask |= 1u << c;
        m.order[m.count++] = static_cast<uint8_t>(c);
    }
}

/* The modules picked (one byte per def, 1 picked, a computed pick's
 * leaves marked) and conditions (the attached conditions' defs, or NULL)
 * read, ids the picks in the caller's order; defs bytes each. Allocates
 * nothing: the caller's thread, under focus_mutex. */
inline ecuio_modules_read
ecuio_modules_wanted(const ecuio_signal_set_t* set, const uint32_t* module_rx,
                     uint32_t module_count, const uint16_t* ids,
                     uint32_t id_count, const uint8_t* picked,
                     const uint8_t* conditions, size_t defs)
{
    ecuio_modules_read m;
    if (set == nullptr || set->defs == nullptr || module_count == 0) {
        return m;
    }
    const size_t n = defs < set->count ? defs : set->count;
    for (uint32_t k = 0; k < id_count && picked != nullptr; ++k) {
        for (size_t i = 0; i < n; ++i) {
            if (picked[i] != 0 && set->defs[i].id == ids[k]) {
                ecuio_modules_add_def(m, set, static_cast<uint32_t>(i),
                                      module_rx, module_count);
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if ((picked != nullptr && picked[i] != 0) ||
            (conditions != nullptr && conditions[i] != 0)) {
            ecuio_modules_add_def(m, set, static_cast<uint32_t>(i), module_rx,
                                  module_count);
        }
    }
    return m;
}

/* What ecuio_take_modules has taken: the focus's count it worked the
 * reasons out at, and whether it has */
struct ecuio_modules_seen {
    uint32_t generation = 0;
    bool taken = false;
    uint32_t closed = 0; /* the states last published */
    uint32_t no_room = 0;
};

/* The modules' states published when one changed: the session's masks,
 * and a record for a recording, with each module's reasons */
inline void ecuio_publish_modules(ecuio_session_t* session,
                                  const ecuio_request_engine* e,
                                  ecuio_modules_seen& seen)
{
    uint32_t closed = 0;
    uint32_t no_room = 0;
    uint8_t reasons[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t c = 0; c < e->chan_count && c < ECUIO_MAX_CHANNELS; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        closed |= ch.module_state == ECUIO_MODULE_STATE_CLOSED ? 1u << c : 0u;
        no_room |= ch.module_state == ECUIO_MODULE_STATE_NO_ROOM ? 1u << c : 0u;
        reasons[c] = ch.reasons;
    }
    if (closed == seen.closed && no_room == seen.no_room) {
        return;
    }
    seen.closed = closed;
    seen.no_room = no_room;
    session->modules_closed.store(closed);
    session->modules_no_room.store(no_room);
    ecuio_record_modules(session->record_link.get(), closed, no_room, reasons,
                         e->chan_count);
}

/* Each module's reasons, worked out again when the picks or the
 * conditions moved (and once before the worker's loop): RANGE for the
 * start's probe's answerers (session->module_range), CONVERSATION for a
 * bound conversation's module, READ for what the picks and conditions
 * read, every module's on a table asked whole with no focus, none on a
 * pick-only table with nothing picked. The engine then opens what fits
 * its budget (ecuio_engine_modules_apply).
 * Every pass, the closing modules that are done close, a freed filter
 * opens the module waiting longest, and a visit ends or the next begins
 * (ecuio_engine_visits_step). The modules closed now, a bit each.
 * No allocation: the worker's thread. */
inline uint32_t ecuio_take_modules(ecuio_session_t* session,
                                   ecuio_request_engine* e,
                                   ecuio_modules_seen& seen, int64_t now_us)
{
    const uint32_t asked = session->focus_generation.load();
    for (uint32_t c = 0; !seen.taken && c < e->chan_count; ++c) {
        ecuio_engine_session_allow(e, c,
                                   ecuio_declared_session_allowed(session, c));
    }
    if (!seen.taken || asked != seen.generation) {
        seen.taken = true;
        seen.generation = asked;
        uint8_t reasons[ECUIO_MAX_CHANNELS] = {};
        uint8_t order[ECUIO_MAX_CHANNELS] = {};
        uint32_t read = 0;
        uint32_t count = 0;
        {
            std::lock_guard<std::mutex> lock(session->focus_mutex);
            const bool whole = session->focus_ids.empty() && !e->pick_only;
            read = whole ? UINT32_MAX : session->focus_modules;
            count = whole ? 0u : session->focus_module_count;
            for (uint32_t k = 0; k < count && k < ECUIO_MAX_CHANNELS; ++k) {
                order[k] = session->focus_module_order[k];
            }
        }
        const ecuio_session_talk* talk = session->talk.get();
        for (uint32_t k = 0; talk != nullptr && k < talk->count; ++k) {
            const uint32_t c = talk->bound[k].conversation.channel;
            if (c < e->chan_count) {
                reasons[c] |= ECUIO_ENGINE_REASON_CONVERSATION;
            }
        }
        for (uint32_t c = 0; c < e->chan_count && c < ECUIO_MAX_CHANNELS; ++c) {
            if (((session->module_range >> c) & 1u) != 0) {
                reasons[c] |= ECUIO_ENGINE_REASON_RANGE;
            }
            if (((read >> c) & 1u) != 0) {
                reasons[c] |= ECUIO_ENGINE_REASON_READ;
            }
        }
        ecuio_engine_modules_apply(e, reasons, order, count);
    }
    const uint32_t closed = ecuio_engine_modules_settle(e, now_us);
    if (closed != 0) {
        ecuio_engine_modules_fill(e);
    }
    ecuio_engine_visits_step(e);
    ecuio_publish_modules(session, e, seen);
    return closed;
}

/* Each channel's ECU, its requests, its identification and diagnostics
 * items, nothing learned yet: what attach hands the session before the
 * worker runs, after ecuio_session_set_modules, whose modules are the
 * only ones seeded, each module open or not as the picks set before
 * start and its range have it (ecuio_take_modules: the reports start at
 * generation 0 saying so); and the info's module_budget, after the
 * attach's describe_session. */
inline void ecuio_seed_reports(ecuio_session_t* session,
                               ecuio_request_engine& e,
                               const uint32_t* rx_ids, uint32_t chan_count)
{
    ecuio_modules_seen seen;
    (void)ecuio_take_modules(session, &e, seen, 0);
    session->info.module_budget = e.module_budget;
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
