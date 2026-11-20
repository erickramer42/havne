// File/layer: core/src/core_getters.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest, DiagnosticsTest, InspectionTest (test_core, test_obd2); c_abi

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "request_engine.h"   /* ECUIO_IDENT_REQUESTS, the names and the VIN */
#include "diagnostics.h"      /* the trouble codes' and monitors' words */
#include "pid_support.h"     /* the signals their ECU doesn't support */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {

ecuio_status_t ecuio_session_get_error_state(
    ecuio_session_t* session,
    int32_t* out_last_error,
    uint32_t* out_error_count)
{
    if (session == nullptr || out_last_error == nullptr ||
        out_error_count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_last_error = session->last_error.load();
    *out_error_count = session->error_count.load();
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_info(
    const ecuio_session_t* session,
    ecuio_session_info_t* out_info)
{
    if (session == nullptr || out_info == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_info = session->info;
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_replay_faults(const ecuio_session_t* session,
                                               ecuio_replay_faults_t* out)
{
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = ecuio_replay_faults_t{};
    const auto get = [](const std::atomic<uint64_t>& a) {
        return a.load(std::memory_order_relaxed);
    };
    out->frames = get(session->replay_frames_read);
    out->dropped = get(session->replay_dropped);
    out->overflowed = get(session->replay_overflowed);
    out->corrupted = get(session->replay_corrupted);
    out->truncated = get(session->replay_truncated);
    out->stalled = get(session->replay_stalled);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_channel_support(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_support_t* out_support)
{
    if (session == nullptr || out_support == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (channel >= session->support_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_support = session->support[channel];
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_absent_signals(
    const ecuio_session_t* session,
    uint16_t* ids,
    uint32_t capacity,
    uint32_t* count)
{
    if (session == nullptr || count == nullptr ||
        (ids == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    const ecuio_signal_set_t* set = session->active_signal_set;
    if (set == nullptr || set->defs == nullptr || !session->decoder.carried) {
        return ECUIO_OK;
    }
    const uint32_t defs = static_cast<uint32_t>(session->decoder.values.size());
    const auto absent = [&](uint32_t i) {
        return i < defs &&
               session->decoder.carried[i].load(std::memory_order_relaxed) ==
                   0 &&
               session->decoder.short_frames[i].load(
                   std::memory_order_relaxed) >= ECUIO_ABSENT_AFTER_FRAMES;
    };
    for (uint32_t i = 0; i < defs; ++i) {
        /* A computed def with an absent input never has a value either */
        if (absent(i) ||
            ecuio_computed_input_gone(*set, set->defs[i], absent)) {
            if (*count < capacity) {
                ids[*count] = set->defs[i].id;
            }
            ++*count;
        }
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_unsupported_signals(
    const ecuio_session_t* session,
    uint16_t* ids,
    uint32_t capacity,
    uint32_t* count)
{
    if (session == nullptr || count == nullptr ||
        (ids == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    const ecuio_signal_set_t* set = session->active_signal_set;
    if (set == nullptr) {
        return ECUIO_OK;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    *count = ecuio_unsupported_signals(*set, session->support.data(),
                                       session->support_count, ids, capacity);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_unasked_signals(const ecuio_session_t* session,
                                                 uint16_t* ids,
                                                 uint32_t capacity,
                                                 uint32_t* count)
{
    if (session == nullptr || count == nullptr ||
        (ids == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    const ecuio_signal_set_t* set = session->active_signal_set;
    if (set == nullptr || set->defs == nullptr) {
        return ECUIO_OK;
    }
    std::lock_guard<std::mutex> lock(session->focus_mutex);
    const size_t n = session->focus_unasked.size();
    for (uint32_t i = 0; i < set->count && i < n; ++i) {
        if (session->focus_unasked[i] != 0) {
            if (*count < capacity) {
                ids[*count] = set->defs[i].id;
            }
            ++*count;
        }
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_unsplit_replies(const ecuio_session_t* session,
                                                uint64_t* count)
{
    if (session == nullptr || count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = session->decoder.unsplit_replies.load(std::memory_order_relaxed);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_timing(const ecuio_session_t* session,
                                        ecuio_session_timing_t* timing,
                                        ecuio_request_timing_t* rows,
                                        uint32_t capacity, uint32_t* count)
{
    if (session == nullptr || timing == nullptr || count == nullptr ||
        (rows == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->timing_mutex);
    *timing = session->timing;
    *count = session->timing.rows;
    for (uint32_t i = 0; i < capacity && i < session->timing.rows; ++i) {
        rows[i] = session->timing_rows[i];
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_module(const ecuio_session_t* session,
                                        uint32_t index,
                                        ecuio_channel_decl_t* out)
{
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= session->info.module_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out = ecuio_channel_decl_t{};
    out->rx_id = session->module_rx[index];
    out->tx_id = session->module_tx[index];
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_channel_identity(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_identity_t* out_identity)
{
    if (session == nullptr || out_identity == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (channel >= session->support_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_identity = session->identity[channel];
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_channel_diagnostics(
    const ecuio_session_t* session, uint32_t channel,
    ecuio_channel_diagnostics_t* out_diagnostics)
{
    if (session == nullptr || out_diagnostics == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (channel >= session->support_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_diagnostics = session->diagnostics[channel];
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_read_diagnostics(ecuio_session_t* session)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    session->diag_requests.fetch_add(1);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_clear_diagnostics(ecuio_session_t* session,
                                               uint32_t channel,
                                               uint32_t generation)
{
    return ecuio_session_clear_diagnostics_with(session, channel, generation,
                                                0);
}

ecuio_status_t ecuio_session_clear_diagnostics_with(ecuio_session_t* session,
                                                    uint32_t channel,
                                                    uint32_t generation,
                                                    uint32_t flags)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if ((flags & ~(ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING)) !=
        0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->support_mutex);
        if (channel >= session->support_count) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        const ecuio_channel_diagnostics_t& d = session->diagnostics[channel];
        if ((d.flags & ECUIO_DIAG_COMPLETE) == 0 ||
            (d.flags & ECUIO_DIAG_CLEARING) != 0 ||
            d.generation != generation) {
            return ECUIO_ERR_REPORT_CHANGED;
        }
    }
    /* The generation and flags first: a worker that sees the count moved
     * reads them */
    session->clear_generation[channel].store(generation);
    session->clear_flags[channel].store(flags);
    session->clear_requests[channel].fetch_add(1);
    return ECUIO_OK;
}

const char* ecuio_identity_request_name(const uint8_t* request, uint8_t length)
{
    if (request == nullptr) {
        return "";
    }
    for (const ecuio_ident_request& q : ECUIO_IDENT_REQUESTS) {
        if (q.len == length && q.bytes[0] == request[0] &&
            q.bytes[1] == request[1] &&
            (length < 3 || q.bytes[2] == request[2])) {
            return q.name;
        }
    }
    return "";
}

ecuio_status_t ecuio_identity_vin(const ecuio_channel_identity_t* identity,
                                  char* out, uint32_t out_cap)
{
    if (identity == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out_cap < ECUIO_VIN_TEXT_LEN) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_ident_vin vin = ecuio_identity_vin_of(*identity);
    std::memcpy(out, vin.text, ECUIO_VIN_TEXT_LEN);
    return ECUIO_OK;
}

ecuio_status_t ecuio_dtc_format(uint16_t code, char* out, uint32_t out_cap)
{
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out_cap < 6) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_dtc_text(code, out);
    return ECUIO_OK;
}

const char* ecuio_monitor_name(uint8_t id, uint32_t diag_flags)
{
    const ecuio_monitor_def* m = ecuio_monitor_find(id, diag_flags);
    return (m != nullptr) ? m->name : "";
}

const char* ecuio_monitor_short_name(uint8_t id, uint32_t diag_flags)
{
    const ecuio_monitor_def* m = ecuio_monitor_find(id, diag_flags);
    return (m != nullptr) ? m->short_name : "";
}

/* 40 CFR 85.2222(c)(2) and (d)(1) over the car's reports (ADR-0045) */
ecuio_status_t ecuio_diag_inspect(const ecuio_channel_diagnostics_t* reports,
                                  uint32_t count, uint32_t model_year,
                                  ecuio_inspection_t* out)
{
    if (out == nullptr || (reports == nullptr && count > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = ecuio_inspection_t{};
    bool reading = false;
    bool mil = false;
    for (uint32_t r = 0; r < count; ++r) {
        const ecuio_channel_diagnostics_t& d = reports[r];
        if ((d.flags & ECUIO_DIAG_ASKED) != 0 &&
            (d.flags & ECUIO_DIAG_COMPLETE) == 0) {
            reading = true;
        }
        if ((d.flags & ECUIO_DIAG_READINESS) == 0) {
            continue;
        }
        ++out->reports;
        mil = mil || (d.flags & ECUIO_DIAG_MIL_ON) != 0;
        out->diag_flags |= d.flags & ECUIO_DIAG_COMPRESSION;
        const uint32_t n =
            std::min<uint32_t>(d.monitor_count, ECUIO_DIAG_MAX_MONITORS);
        for (uint32_t i = 0; i < n; ++i) {
            const ecuio_monitor_t& m = d.monitors[i];
            if (m.id < 16 && (m.flags & ECUIO_MONITOR_COMPLETE) == 0) {
                out->incomplete_ids =
                    static_cast<uint16_t>(out->incomplete_ids | (1u << m.id));
            }
        }
    }
    for (uint32_t id = 0; id < 16; ++id) {
        out->incomplete = static_cast<uint8_t>(
            out->incomplete + ((out->incomplete_ids >> id) & 1u));
    }
    if (model_year >= 2001) {
        out->allowed = 1;
    } else if (model_year >= 1996) {
        out->allowed = 2;
    }
    if (reading) {
        out->verdict = ECUIO_INSPECTION_READING;
    } else if (out->reports == 0) {
        out->verdict = ECUIO_INSPECTION_UNKNOWN;
    } else if (model_year != 0 && model_year < 1996) {
        out->verdict = ECUIO_INSPECTION_NOT_COVERED;
    } else if (mil) {
        out->verdict = ECUIO_INSPECTION_MIL_ON;
    } else if (model_year == 0) {
        /* one incomplete passes either rule, three fail both */
        out->verdict = out->incomplete <= 1   ? ECUIO_INSPECTION_READY
                       : out->incomplete == 2 ? ECUIO_INSPECTION_YEAR_NEEDED
                                              : ECUIO_INSPECTION_NOT_READY;
    } else {
        out->verdict = out->incomplete <= out->allowed
                           ? ECUIO_INSPECTION_READY
                           : ECUIO_INSPECTION_NOT_READY;
    }
    return ECUIO_OK;
}

} // extern "C"
