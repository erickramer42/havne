// File/layer: core/src/core.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest, DefIndexTest (test_core); PidSupportTest,
//          DiagnosticsTest, InspectionTest (test_obd2); ProbeNotesTest
//          (ctest); c_abi; HARNESS: Elm327SessionTest (test_elm327)

#include "ecuio/version.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "session.h"
#include "uds_generic.h"
#include "request_engine.h"   /* ECUIO_IDENT_REQUESTS, for the names */
#include "diagnostics.h"      /* the trouble codes' and monitors' words */
#include "pid_support.h"     /* the signals their ECU doesn't support */
#include "uri_query.h"
#include "error_detail.h"
#include "probes.h"
#include "ecuio/j2534_ptypes.h"   /* ecuio_pt_status_name; plain C, any OS */

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <utility>

namespace {
    /* ecuio_last_error_detail's text (error_detail.h), per thread. */
    thread_local char t_detail[512] = "";

    enum class ProfileQuery { Absent, Valid, BadValue };

    /* The last `profile` value of a split query. A malformed one (empty or
     * over-length) fails session_create, never falls back to the scheme
     * default (a typo'd raw would decode); last-wins applies across valid
     * values only. out_val holds ECUIO_PROFILE_NAME_MAX + 1 bytes. */
    ProfileQuery query_profile(const ecuio_uri_query& q, char* out_val)
    {
        bool found = false;
        for (size_t i = 0; i < q.count; ++i) {
            const ecuio_uri::param& p = q.params[i];
            if (!p.is("profile")) {
                continue;
            }
            if (p.val_len == 0 || p.val_len > ECUIO_PROFILE_NAME_MAX) {
                return ProfileQuery::BadValue;
            }
            memcpy(out_val, p.val, p.val_len);
            out_val[p.val_len] = '\0';
            found = true;
        }
        return found ? ProfileQuery::Valid : ProfileQuery::Absent;
    }

    /* "a", "a and b", "a, b and c": a key list for a detail line. */
    void join_keys(const char* const* keys, char* out, size_t cap)
    {
        size_t n = 0;
        while (keys[n] != nullptr) {
            ++n;
        }
        size_t used = 0;
        out[0] = '\0';
        for (size_t i = 0; i < n && used < cap; ++i) {
            const char* sep = (i == 0) ? "" : (i + 1 == n) ? " and " : ", ";
            const int w = snprintf(out + used, cap - used, "%s%s", sep, keys[i]);
            if (w < 0) {
                break;
            }
            used += static_cast<size_t>(w);
        }
    }

    /* The list a dispatch calls (ADR-0002): copied under the callback
     * mutex, called unlocked. On the stack while it fits, as the app's one
     * callback does (no allocation per frame), else on the heap. */
    template <typename Callback>
    class snapshot_of {
    public:
        using entry = std::pair<Callback, void*>;

        /* Copies list; the caller holds the mutex. False when the heap
         * copy failed: the caller drops the delivery (no bad_alloc may
         * leave an extern "C" call). */
        bool take(const std::vector<entry>& list)
        {
            count_ = list.size();
            if (count_ <= kInline) {
                for (size_t i = 0; i < count_; ++i) {
                    inline_[i] = list[i];
                }
                return true;
            }
            try {
                heap_ = list;
            } catch (...) {
                count_ = 0;
                return false;
            }
            data_ = heap_.data();
            return true;
        }

        size_t size() const { return count_; }
        const entry& operator[](size_t i) const { return data_[i]; }

    private:
        static constexpr size_t kInline = 4;
        entry inline_[kInline];
        std::vector<entry> heap_;
        const entry* data_ = inline_;
        size_t count_ = 0;
    };

    /* Adds (callback, user_data) to list once; the caller holds the
     * mutex */
    template <typename Callback>
    ecuio_status_t add_callback(std::vector<std::pair<Callback, void*>>& list,
                                Callback callback, void* user_data)
    {
        for (const auto& entry : list) {
            if (entry.first == callback && entry.second == user_data) {
                return ECUIO_OK;
            }
        }
        try {
            list.emplace_back(callback, user_data);
            return ECUIO_OK;
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
    }

    /* Removes (callback, user_data) from list, if there; the caller
     * holds the mutex */
    template <typename Callback>
    void remove_callback(std::vector<std::pair<Callback, void*>>& list,
                         Callback callback, void* user_data)
    {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->first == callback && it->second == user_data) {
                list.erase(it);
                return;
            }
        }
    }
} // namespace

void ecuio_detail_clear()
{
    t_detail[0] = '\0';
}

void ecuio_detail_set(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_detail, sizeof(t_detail), fmt, ap);
    va_end(ap);
}

void ecuio_text_one_line(char* text)
{
    size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        if (static_cast<unsigned char>(text[n]) < 0x20) {
            text[n] = ' ';
        }
    }
    while (n > 0 && text[n - 1] == ' ') {
        --n;
    }
    text[n] = '\0';
}

void ecuio_detail_set_vendor(const char* call, long status,
                             const char* vendor_text)
{
    /* The vendor's text on one line: it lands in a single UI line and a
     * log line. */
    char text[2 * ECUIO_PT_TEXT_LEN] = "";
    if (vendor_text != nullptr) {
        size_t n = 0;
        for (; vendor_text[n] != '\0' && n + 1 < sizeof(text); ++n) {
            text[n] = vendor_text[n];
        }
        text[n] = '\0';
        ecuio_text_one_line(text);
    }
    const char* name = ecuio_pt_status_name(status);
    /* A vendor status with no text isn't actionable; the usual causes are
     * no adapter or another program holding it (the VX answers 34 to both:
     * docs/bench-sessions/2026-11-02-vehicle.md). The vendor's text goes
     * first below. */
    const bool open_hint = name == nullptr && strcmp(call, "PassThruOpen") == 0;
    if (name == nullptr) {
        name = "vendor-specific";
    }
    if (text[0] != '\0') {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s): %s", call, status,
                         static_cast<unsigned long>(status), name, text);
    } else if (open_hint) {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s): is the adapter "
                         "attached, and not open in another program?",
                         call, status, static_cast<unsigned long>(status),
                         name);
    } else {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s)", call, status,
                         static_cast<unsigned long>(status), name);
    }
}

extern "C" {

const char* ecuio_last_error_detail(void)
{
    return t_detail;
}

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
    *count = ecuio_unsupported_signals(*set, session->support,
                                       session->support_count, ids, capacity);
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

ecuio_status_t ecuio_session_set_focus(ecuio_session_t* session,
                                       const uint16_t* signal_ids,
                                       uint32_t count)
{
    return ecuio_session_set_focus_with(session, signal_ids, count, 0, 0);
}

ecuio_status_t ecuio_session_set_focus_with(ecuio_session_t* session,
                                            const uint16_t* signal_ids,
                                            uint32_t count,
                                            uint32_t min_sweep_ms,
                                            uint32_t flags)
{
    ecuio_detail_clear();
    if (session == nullptr || (signal_ids == nullptr && count > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (flags != 0) {
        ecuio_detail_set("the focus takes no flags yet");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (min_sweep_ms > ECUIO_FOCUS_MIN_SWEEP_MAX_MS) {
        ecuio_detail_set("a minimum time between sweeps is at most %u ms",
                         static_cast<unsigned>(ECUIO_FOCUS_MIN_SWEEP_MAX_MS));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (min_sweep_ms != 0 && count == 0) {
        ecuio_detail_set("a minimum time between sweeps goes with a focus: "
                         "no signals were picked");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    const ecuio_signal_set_t* set = session->active_signal_set;
    const uint32_t defs =
        (set != nullptr && set->defs != nullptr) ? set->count : 0;
    std::vector<uint16_t> ids;
    std::vector<uint8_t> picked;
    try {
        if (count > 0) {
            ids.assign(signal_ids, signal_ids + count);
        }
        picked.assign(defs, 0);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t k = 0; k < count; ++k) {
        bool found = false;
        for (uint32_t i = 0; i < defs; ++i) {
            if (set->defs[i].id == signal_ids[k]) {
                picked[i] = 1;
                found = true;
            }
        }
        if (!found) {
            ecuio_detail_set("no signal with id %u in profile %s",
                             static_cast<unsigned>(signal_ids[k]),
                             session->info.profile);
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    /* A computed pick keeps its inputs asked (ADR-0051's appendix of
     * 2026-11-10): they join the picked defs, never the caller's ids, so the
     * recorder's focus line names what the caller picked. Inputs are never
     * computed themselves, so one pass is all there is. */
    if (set != nullptr && set->computed != nullptr) {
        for (uint32_t i = 0; i < defs; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (picked[i] == 0 || (d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 ||
                d.table_first >= set->computed_count) {
                continue;
            }
            const ecuio_computed_t& c = set->computed[d.table_first];
            for (uint8_t k = 0; k < c.input_count && k < 4; ++k) {
                for (uint32_t j = 0; j < defs; ++j) {
                    if (set->defs[j].id == c.input_ids[k]) {
                        picked[j] = 1;
                    }
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(session->focus_mutex);
        /* A session that asks requests refuses a focus with no askable
         * pick; one that asks nothing (mock, replay, listening) takes any. */
        if (count > 0 && session->focus_requests) {
            bool asks = false;
            for (uint32_t i = 0;
                 i < defs && i < session->focus_askable.size() && !asks; ++i) {
                asks = picked[i] != 0 && session->focus_askable[i] != 0;
            }
            if (!asks) {
                ecuio_detail_set("the focus leaves nothing to request: none "
                                 "of the picked signals is asked on this "
                                 "session");
                return ECUIO_ERR_INVALID_ARGUMENT;
            }
        }
        session->focus_ids.swap(ids);
        session->focus_defs.swap(picked);
        session->focus_min_sweep_ms = min_sweep_ms;
    }
    /* The picks first: a worker that sees the count moved reads them */
    session->focus_generation.fetch_add(1);
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
    char from_service09[ECUIO_VIN_TEXT_LEN] = "";
    char from_f190[ECUIO_VIN_TEXT_LEN] = "";
    const uint32_t items =
        std::min<uint32_t>(identity->items, ECUIO_IDENTITY_MAX_ITEMS);
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = identity->list[i];
        char* into = nullptr;
        if (it.length == 2 && it.request[0] == 0x09 && it.request[1] == 0x02) {
            into = from_service09;
        } else if (it.length == 3 && it.request[0] == 0x22 &&
                   it.request[1] == 0xF1 && it.request[2] == 0x90) {
            into = from_f190;
        }
        if (into == nullptr || it.outcome != ECUIO_IDENTITY_ANSWERED) {
            continue;
        }
        char vin[ECUIO_VIN_TEXT_LEN];
        uint32_t n = 0;
        bool fits = true;
        for (uint32_t k = 0; k < it.text_length && k < ECUIO_IDENTITY_TEXT_LEN;
             ++k) {
            char c = it.text[k];
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            } else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                continue;
            }
            if (n == ECUIO_VIN_TEXT_LEN - 1) {
                fits = false;
                break;
            }
            vin[n++] = c;
        }
        if (fits && n == ECUIO_VIN_TEXT_LEN - 1) {
            vin[n] = '\0';
            std::memcpy(into, vin, sizeof vin);
        }
    }
    const char* vin = from_service09[0] != '\0' ? from_service09 : from_f190;
    std::memcpy(out, vin, ECUIO_VIN_TEXT_LEN);
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

/* Optional no-ops (core.h): core holds no global state that needs
 * them. */
ecuio_status_t ecuio_init(void)
{
    return ECUIO_OK;
}

void ecuio_shutdown(void)
{
}

const char* ecuio_version_string(void)
{
    return ECUIO_VERSION_STRING;
}

ecuio_status_t ecuio_version(uint32_t* major, uint32_t* minor, uint32_t* patch)
{
    if (!major || !minor || !patch) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *major = ECUIO_VERSION_MAJOR;
    *minor = ECUIO_VERSION_MINOR;
    *patch = ECUIO_VERSION_PATCH;
    return ECUIO_OK;
}

const char* ecuio_status_string(ecuio_status_t status)
{
    switch (status) {
        case ECUIO_OK:                          return "success";
        case ECUIO_ERR_NULL_ARGUMENT:           return "null argument";
        case ECUIO_ERR_NOT_IMPLEMENTED:         return "not implemented yet";
        case ECUIO_ERR_NO_TRANSPORT:            return "no transport available";
        case ECUIO_ERR_OUT_OF_MEMORY:           return "out of memory";
        case ECUIO_ERR_SESSION_CLOSED:          return "session already closed";
        case ECUIO_ERR_ALREADY_REGISTERED:      return "already registered";
        case ECUIO_ERR_LOAD_FAILED:             return "couldn't open what was named";
        case ECUIO_ERR_VEHICLE_TIMEOUT:         return "vehicle not responding (timeout)";
        case ECUIO_ERR_TRANSPORT_HARD_FAILURE: return "transport hard failure";
        case ECUIO_ERR_REQUEST_FAILED:          return "request failed (vehicle not accepting requests)";
        case ECUIO_ERR_INVALID_ARGUMENT:        return "invalid argument";
        case ECUIO_ERR_BAD_URI:                 return "malformed transport URI";
        case ECUIO_ERR_UNKNOWN_PROFILE:         return "unknown profile";
        case ECUIO_ERR_VENDOR_REFUSED:          return "the adapter's driver refused";
        case ECUIO_ERR_SURROGATE_FAILED:        return "surrogate process failed";
        case ECUIO_ERR_RECORDING_CHANGED:       return "the recording changed after it was opened";
        case ECUIO_ERR_REPORT_CHANGED:          return "the diagnostics report changed since it was saved";
        case ECUIO_ERR_RECORDING_FAILED:        return "the recording couldn't be made or written";
        case ECUIO_ERR_CANCELED:                return "canceled";
        case ECUIO_ERR_IN_USE:                  return "a session still uses it";
        case ECUIO_ERR_STREAM_ENDED:            return "the stream has ended";
        default:                                return "unknown status";
    }
}

ecuio_status_t ecuio_session_create(
    const char* transport_uri,
    ecuio_session_t** out_session)
{
    ecuio_detail_clear();
    if (out_session == nullptr || transport_uri == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    /* Held here until it's whole: every refusal below just returns,
     * and the caller's pointer is set only on success. */
    *out_session = nullptr;
    std::unique_ptr<ecuio_session> s;
    try {
        s = std::make_unique<ecuio_session>();
        s->record_link = std::make_shared<ecuio_record_link>();
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    /* Scheme routing (j2534 is Win32-only). */
#ifdef _WIN32
    const bool is_j2534 = strncmp(transport_uri, "j2534://", 8) == 0;
#else
    const bool is_j2534 = false;
#endif
    const bool is_mock = strncmp(transport_uri, "mock://", 7) == 0;
    const bool is_replay = strncmp(transport_uri, "replay://", 9) == 0;
    const bool is_elm327 = strncmp(transport_uri, "elm327://", 9) == 0;
    const bool is_socketcan = ECUIO_HAVE_SOCKETCAN &&
                              strncmp(transport_uri, "socketcan://", 12) == 0;
    if (!is_mock && !is_replay && !is_j2534 && !is_elm327 && !is_socketcan) {
#ifndef _WIN32
        if (strncmp(transport_uri, "j2534://", 8) == 0) {
            ecuio_detail_set("j2534:// needs Windows, where J2534 adapters "
                             "have their drivers");
            return ECUIO_ERR_NO_TRANSPORT;
        }
#endif
        if (strncmp(transport_uri, "socketcan://", 12) == 0) {
            ecuio_detail_set("socketcan:// needs Linux, where a CAN "
                             "interface is a network device");
            return ECUIO_ERR_NO_TRANSPORT;
        }
        ecuio_detail_set("unsupported URI scheme: expected mock://, "
                         "replay://, j2534://, elm327:// or socketcan://");
        return ECUIO_ERR_NO_TRANSPORT;
    }

    /* The query, split once (uri_query.h) and checked against the scheme's
     * keys before anything attaches: a bad query or an unknown key fails
     * with no DLL load and no discovery, the same way on every scheme. */
    const char* scheme = is_mock        ? "mock://"
                         : is_replay    ? "replay://"
                         : is_elm327    ? "elm327://"
                         : is_socketcan ? "socketcan://"
                                        : "j2534://";
    const char* const* keys = is_mock        ? ecuio_mock_query_keys
                              : is_replay    ? ecuio_replay_query_keys
                              : is_elm327    ? ecuio_elm327_query_keys
                              : is_socketcan ? ecuio_socketcan_query_keys
                                             : ecuio_j2534_query_keys;
    const char* rest = transport_uri + strlen(scheme);
    ecuio_uri_query query;
    switch (ecuio_uri::split(ecuio_uri::query_of(rest), &query)) {
        case ecuio_uri::split_result::malformed:
            ecuio_detail_set("malformed query: expected key=value pairs "
                             "joined by '&'");
            return ECUIO_ERR_BAD_URI;
        case ecuio_uri::split_result::too_long:
            ecuio_detail_set("the query has more than %d key=value pairs",
                             static_cast<int>(ECUIO_URI_MAX_PARAMS));
            return ECUIO_ERR_BAD_URI;
        case ecuio_uri::split_result::ok:
        default:
            break;
    }
    if (const ecuio_uri::param* bad = ecuio_uri::first_unknown(query, keys)) {
        char known[128];
        join_keys(keys, known, sizeof(known));
        ecuio_detail_set("%s takes no query key '%.*s', only %s", scheme,
                         static_cast<int>(bad->key_len), bad->key, known);
        return ECUIO_ERR_BAD_URI;
    }

    /* ADR-0013 profile selection (a default per scheme, ?profile= over it),
     * resolved before any attach so a bad profile fails with no DLL load
     * and no discovery. */
    char profile_name[ECUIO_PROFILE_NAME_MAX + 1];
    const char* chosen = nullptr;
    switch (query_profile(query, profile_name)) {
        case ProfileQuery::Valid:
            chosen = profile_name;
            break;
        case ProfileQuery::BadValue:
            ecuio_detail_set("?profile= needs a profile name of 1 to %d "
                             "characters", ECUIO_PROFILE_NAME_MAX);
            return ECUIO_ERR_BAD_URI;
        case ProfileQuery::Absent:
        default:
            /* Scheme defaults: mock:// -> mock; j2534:// and elm327://
             * -> obd2 (decode, the Service 01 requests and the declared
             * 7E0/7E8 channel); replay:// -> raw, which can't decode a
             * recording wrongly (ADR-0023); socketcan:// -> raw, the
             * whole bus as it is (ADR-0032). ?profile=raw gives a
             * log-only session. */
            chosen = is_mock                    ? "mock"
                     : is_replay || is_socketcan ? "raw"
                                                 : "obd2";
            break;
    }

    const ecuio_signal_set_t* profile_set = nullptr;
    const ecuio_status_t found = ecuio_profile_acquire(chosen, &profile_set);
    s->profile_hold.set = profile_set;
    if (found != ECUIO_OK) {
        if (found == ECUIO_ERR_UNKNOWN_PROFILE) {
            ecuio_detail_set("no profile is named '%s'", chosen);
        }
        return found;
    }

    /* uds-generic (ADR-0020) reads its DIDs from the query and gets a set
     * of its own, owned by the session; no other profile takes ?did=.
     * Both refusals come before attach, like a bad ?profile=. */
    if (strcmp(chosen, ECUIO_UDS_GENERIC_NAME) == 0) {
        if (!ecuio_uds_query_has_did(query)) {
            ecuio_detail_set("?profile=uds-generic needs ?did= to say which "
                             "DIDs to read");
            return ECUIO_ERR_BAD_URI;
        }
        try {
            s->uds = std::make_unique<ecuio_uds_session>();
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        const ecuio_status_t built =
            ecuio_uds_build(query, s->uds.get());
        if (built != ECUIO_OK) {
            return built;
        }
        profile_set = &s->uds->set;
    } else if (ecuio_uds_query_has_did(query)) {
        ecuio_detail_set("?did= belongs to the uds-generic profile: add "
                         "?profile=uds-generic");
        return ECUIO_ERR_BAD_URI;
    }

    /* Set before attach: the requesting transports derive their
     * requests and channels from it during attach. Single writer: attach
     * and the worker both run after this store. */
    s->active_signal_set = profile_set;

    /* The decode's buffers (ADR-0024 section 4, section 6), sized once: the set never
     * changes after this */
    try {
        s->decoder.prepare(profile_set, true);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    ecuio_status_t status = ECUIO_ERR_NO_TRANSPORT;
    if (is_mock) {
        status = ecuio_mock_transport_attach(s.get(), transport_uri,
                                             &query);
    } else if (is_replay) {
        status = ecuio_replay_transport_attach(s.get(), transport_uri, &query);
    } else if (is_elm327) {
        status = ecuio_elm327_transport_attach(s.get(), transport_uri, &query);
    } else if (is_socketcan) {
#if ECUIO_HAVE_SOCKETCAN
        status = ecuio_socketcan_transport_attach(s.get(), transport_uri,
                                                  &query);
#endif
    } else if (is_j2534) {
#ifdef _WIN32
        status = ecuio_j2534_transport_attach(s.get(), transport_uri, &query);
#endif
    }

    if (status != ECUIO_OK) {
        return status;
    }
    /* After attach, which fills the rest of the info; the worker hasn't
     * started, so no lock */
    if (s->focus_requests) {
        s->info.flags |= ECUIO_SESSION_INFO_REQUESTS;
    }
    static_assert(ECUIO_PROFILE_NAME_MAX < ECUIO_SESSION_INFO_PROFILE_LEN,
                  "the session info holds any profile name");
    char* profile = s->info.profile;
    const size_t n = strlen(chosen);
    memcpy(profile, chosen, n);
    profile[n] = '\0';
    try {
        s->uri = transport_uri;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out_session = s.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_start(ecuio_session_t* session)
{
    ecuio_detail_clear();
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if (session->transport == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    const ecuio_status_t st =
        session->transport->ops->start(session, session->transport->impl);
    if (st == ECUIO_OK && !session->started) {
        session->started = true;
        ECUIO_PROBE2(session_start, session, session->info.backend);
    }
    return st;
}

ecuio_status_t ecuio_session_close(ecuio_session_t* session)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    /* First-closer wins the CAS; everyone else gets a clean error on
     * LIVE memory. */
    bool expected = false;
    if (!session->closed.compare_exchange_strong(expected, true)) {
        return ECUIO_ERR_SESSION_CLOSED;
    }

    /* The transport frees its impl; the wrapper is the session's. */
    if (session->transport != nullptr) {
        session->transport->ops->detach(session, session->transport->impl);
        delete session->transport;
        session->transport = nullptr;
    }
    /* The worker has stopped: the recorder takes no more frames, and its
     * notes go on until the caller closes it (ADR-0049 section 1) */
    ecuio_record_detach(*session->record_link);
    /* So do the pull queues: what's waiting is still read, then their
     * reads end (ecuio/queue.h) */
    for (uint32_t i = 0; i < session->queue_count; ++i) {
        ecuio_queue_detach(*session->queues[i]);
    }

    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        session->callbacks.clear();
        session->values_callbacks.clear();
    }

    ECUIO_PROBE1(session_close, session);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_destroy(ecuio_session_t* session)
{
    if (session == nullptr) {
        return ECUIO_OK;    /* free(nullptr) semantics */
    }

    (void)ecuio_session_close(session);
    delete session;
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_send(
    ecuio_session_t* session,
    const ecuio_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if (frame->data_length > sizeof(frame->data)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_session_receive(
    ecuio_session_t* session,
    ecuio_frame_t* out_frame,
    uint32_t timeout_ms)
{
    (void)session;
    (void)out_frame;
    (void)timeout_ms;
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_register_frame_callback(
    ecuio_session_t* session,
    ecuio_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr || callback == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    return add_callback(session->callbacks, callback, user_data);
}

ecuio_status_t ecuio_unregister_frame_callback(
    ecuio_session_t* session,
    ecuio_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    remove_callback(session->callbacks, callback, user_data);
    return ECUIO_OK;
}

ecuio_status_t ecuio_register_values_callback(
    ecuio_session_t* session,
    ecuio_values_callback_t callback,
    void* user_data)
{
    if (session == nullptr || callback == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    return add_callback(session->values_callbacks, callback, user_data);
}

ecuio_status_t ecuio_unregister_values_callback(
    ecuio_session_t* session,
    ecuio_values_callback_t callback,
    void* user_data)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    remove_callback(session->values_callbacks, callback, user_data);
    return ECUIO_OK;
}

void ecuio_session_dispatch_callbacks(
    ecuio_session_t* session,
    const ecuio_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return;
    }
    if (session->closed.load()) {
        return;
    }
    ECUIO_PROBE5(frame, session, frame->arbitration_id, frame->flags,
                 frame->data_length, frame->timestamp_us);

    /* A recording's time, read before any callback (ADR-0049 section 1);
     * a recorder attaches at any time (ADR-0056 section 3), so the zero is
     * read once it is pinned below: one that comes or goes meanwhile lends
     * its zero to no other's frame */
    ecuio_record_link* link = session->record_link.get();
    uint64_t now = link->active.load(std::memory_order_relaxed) != nullptr
                       ? ecuio_clock_us()
                       : 0;

    /* Both lists as they stand when the frame arrives */
    snapshot_of<ecuio_frame_callback_t> frames;
    snapshot_of<ecuio_values_callback_t> values;
    bool frames_taken = false;
    bool values_taken = false;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        frames_taken = frames.take(session->callbacks);
        values_taken = values.take(session->values_callbacks);
    }

    if (frames_taken) {
        for (size_t i = 0; i < frames.size(); ++i) {
            frames[i].first(frame, frames[i].second);
        }
    }
    /* The pull queues' copies, after the callbacks (ecuio/queue.h); the
     * links are set before start and kept until the session is freed */
    for (uint32_t i = 0; i < session->queue_count; ++i) {
        ecuio_queue_link& q = *session->queues[i];
        if (q.kind == ECUIO_QUEUE_FRAMES) {
            if (ecuio_queue_t* queue = ecuio_queue_pin(q)) {
                ecuio_queue_put_frame(queue, frame);
                ecuio_queue_unpin(q);
            }
        }
    }

    /* Then the frame's values (ADR-0024), decoded for a values callback,
     * a values queue or a recording; the recording takes the frame and
     * its values as one record */
    const bool want_values =
        (values_taken && values.size() > 0) || session->value_queues > 0;
    ecuio_recorder_t* recorder = ecuio_record_pin(*link);
    if (!want_values && recorder == nullptr) {
        return;
    }
    const uint32_t count = session->decoder.decode(frame);
    if (recorder != nullptr) {
        if (now == 0) {
            now = ecuio_clock_us(); /* attached since the first look */
        }
        const uint64_t zero = link->zero_us.load(std::memory_order_relaxed);
        const uint64_t host_us = now > zero ? now - zero : 0;
        ecuio_recorder_put_frame(recorder, host_us, frame,
                                 session->decoder.values.data(), count);
        ecuio_record_unpin(*link);
    }
    if (!want_values || count == 0) {
        return;
    }
    ecuio_value_batch_t batch{};
    batch.timestamp_us = frame->timestamp_us;
    batch.arbitration_id = frame->arbitration_id;
    batch.count = count;
    batch.values = session->decoder.values.data();
    if (values_taken) {
        for (size_t i = 0; i < values.size(); ++i) {
            values[i].first(&batch, values[i].second);
        }
    }
    for (uint32_t i = 0; i < session->queue_count; ++i) {
        ecuio_queue_link& q = *session->queues[i];
        if (q.kind == ECUIO_QUEUE_VALUES) {
            if (ecuio_queue_t* queue = ecuio_queue_pin(q)) {
                ecuio_queue_put_values(queue, &batch);
                ecuio_queue_unpin(q);
            }
        }
    }
}

} // extern "C"
