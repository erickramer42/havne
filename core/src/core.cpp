// File/layer: core/src/core.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest: session create (routing, the query split,
//          profiles and every refusal), start, close and destroy,
//          callbacks and their snapshot dispatch (every callback at any
//          list length, the list as it stood, no allocation up to
//          four), the decoded values (CoreTest.Values*: the batch
//          against direct decodes, a text slot each, the frame
//          callbacks first; the defs each frame is checked against:
//          DefIndexTest; enum labels and computed values with their
//          ages: CoreTest.Computed*, .AComputedInputAtItsBoundStillCounts),
//          the absent signals
//          (CoreTest.AFieldPastEveryReplyIsAbsent), the identity
//          report's VIN (CoreTest.TheVinIsService09sThenF190s), the
//          unsupported signals
//          (PidSupportTest, test_obd2; the call's contract:
//          CoreTest.TheMockHasNoUnsupportedSignals), a packed reply's
//          records (CoreTest.APacked*), the error strings, a clear's refusals
//          (CoreTest.AClearNeedsTheReportTheCallerSaved); the trouble code's and
//          the monitors' names (DiagnosticsTest, test_obd2); the
//          inspection verdict (InspectionTest, test_obd2);
//          HARNESS: the J2534 suites and Elm327SessionTest through
//          create; test_c_abi links the public functions; the USDT
//          probes: ProbeNotesTest (their notes) and CI's bpftrace step;
//          the timing report (CoreTest.ASessionWithNoRotationHasNoTimingRows)

#include "ecuio/version.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "session.h"
#include "uds_generic.h"
#include "request_engine.h"   /* ECUIO_IDENT_REQUESTS, for the names */
#include "diagnostics.h"      /* the trouble codes' and monitors' words */
#include "obd2_reply.h"      /* a packed Service 01 reply's records */
#include "computed.h"        /* computed signals' programs */
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

    /* The LAST `profile` value of a split query. Tri-state: a
     * present-but-malformed value (empty or over-length) is INVALID and
     * fails session_create: it must NEVER silently downgrade to the
     * scheme default, which would hand a typo'd ?profile=raw a decoding
     * session or vice versa. Any malformed occurrence fails; last-wins
     * applies only across valid values. out_val holds
     * ECUIO_PROFILE_NAME_MAX + 1 bytes. */
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

    bool is_text(const ecuio_signal_def_t& def)
    {
        return def.kind == ECUIO_VALUE_TEXT || def.kind == ECUIO_VALUE_HEX;
    }

    /* The most text values one frame can decode to against set
     * (ADR-0024 section 4). On one message ID, every unkeyed text def matches
     * every frame; of the keyed ones, one frame holds one key of each
     * shape (offset and length), so each shape adds its largest group
     * sharing one key. The answer is the largest such total over the
     * IDs. Runs once, at create; throws bad_alloc, which create
     * catches. */
    uint32_t text_slots_for(const ecuio_signal_set_t* set)
    {
        struct text_key {
            uint32_t message_id;
            uint32_t shape;   /* 0 unkeyed; else offset << 8 | length */
            uint32_t key;
        };
        std::vector<text_key> keys;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (!is_text(d)) {
                continue;
            }
            const bool keyed = d.mux_length > 0;
            keys.push_back(text_key{
                d.message_id,
                keyed ? (uint32_t{d.mux_offset} << 8 | d.mux_length) : 0u,
                keyed ? d.mux_key : 0u});
        }
        std::sort(keys.begin(), keys.end(),
                  [](const text_key& a, const text_key& b) {
                      if (a.message_id != b.message_id) {
                          return a.message_id < b.message_id;
                      }
                      if (a.shape != b.shape) {
                          return a.shape < b.shape;
                      }
                      return a.key < b.key;
                  });
        uint32_t best = 0;
        size_t i = 0;
        while (i < keys.size()) {
            const uint32_t id = keys[i].message_id;
            uint32_t total = 0;
            while (i < keys.size() && keys[i].message_id == id) {
                const uint32_t shape = keys[i].shape;
                uint32_t largest = 0;
                while (i < keys.size() && keys[i].message_id == id &&
                       keys[i].shape == shape) {
                    uint32_t run = 0;
                    const uint32_t key = keys[i].key;
                    while (i < keys.size() && keys[i].message_id == id &&
                           keys[i].shape == shape && keys[i].key == key) {
                        ++run;
                        ++i;
                    }
                    /* Unkeyed defs all match: their shape is one run */
                    largest = std::max(largest, run);
                }
                total += largest;
            }
            best = std::max(best, total);
        }
        return best;
    }

    /* The list a dispatch calls (ADR-0002): copied under the callback
     * mutex, called unlocked. It sits on the stack while the list fits,
     * as the app's one callback does, so a frame costs the worker no
     * allocation; a longer list is copied to the heap. */
    template <typename Callback>
    class snapshot_of {
    public:
        using entry = std::pair<Callback, void*>;

        /* Copies list; the caller holds the mutex. False when the heap
         * copy failed: the caller drops what it would have delivered,
         * since a bad_alloc can't leave an extern "C" call on the
         * worker. */
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

    /* ADR-0039 section 6: def i's decode of a frame its key matched, for
     * ecuio_session_get_absent_signals: carried, or one more frame too
     * short for it (saturating at the threshold). Other failures say
     * nothing about the reply's length. */
    void note_decode(ecuio_session_t* session, uint32_t i, int rc)
    {
        if (!session->carried) {
            return;
        }
        if (rc == ECUIO_DECODE_OK) {
            if (session->carried[i].load(std::memory_order_relaxed) == 0) {
                session->carried[i].store(1, std::memory_order_relaxed);
            }
        } else if (rc == ECUIO_DECODE_ERR_PAST_PAYLOAD) {
            const uint8_t n =
                session->short_frames[i].load(std::memory_order_relaxed);
            if (n < ECUIO_ABSENT_AFTER_FRAMES) {
                session->short_frames[i].store(static_cast<uint8_t>(n + 1),
                                               std::memory_order_relaxed);
            }
        }
    }

    /* Decodes frame (len bytes of it) against set into the session's
     * buffers from value n and text slot slot on, each def in the set's
     * order; only the defs the index finds for the frame are checked */
    void decode_frame(ecuio_session_t* session, const ecuio_signal_set_t* set,
                      const ecuio_frame_t* frame, uint16_t len, uint32_t& n,
                      uint32_t& slot)
    {
        uint32_t* found = session->def_found.data();
        const uint32_t count =
            ecuio_def_index_find(session->def_index, *frame, found);
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t i = found[k];
            const ecuio_signal_def_t& def = set->defs[i];
            if (!ecuio_def_applies_to_frame(&def, frame)) {
                continue;
            }
            ecuio_value_t v{};
            v.signal_id = def.id;
            v.kind = def.kind;
            if (is_text(def)) {
                if (slot >= session->text_slots) {
                    continue;
                }
                char* out = session->text_scratch.data() +
                            size_t{slot} * ECUIO_DECODE_TEXT_MAX;
                const int rc = ecuio_decode_text(&def, frame->data, len, out,
                                                 ECUIO_DECODE_TEXT_MAX);
                note_decode(session, i, rc);
                if (rc != ECUIO_DECODE_OK) {
                    continue;
                }
                v.text = out;
                v.text_len = static_cast<uint32_t>(strlen(out));
                ++slot;
            } else {
                const int rc =
                    ecuio_decode_signal(&def, frame->data, len, &v.value);
                note_decode(session, i, rc);
                if (rc != ECUIO_DECODE_OK) {
                    continue;
                }
                if (def.kind == ECUIO_VALUE_ENUM) {
                    /* The raw number, and its label from the set's
                     * table, which outlives the call */
                    v.text = ecuio_enum_label(set, &def,
                                              static_cast<uint32_t>(v.value));
                    v.text_len = v.text != nullptr
                                     ? static_cast<uint32_t>(strlen(v.text))
                                     : 0u;
                }
                if (!session->latest.empty()) {
                    session->latest[i] = {v.value, frame->timestamp_us,
                                          session->decodes};
                }
            }
            session->values[n++] = v;
        }
    }

    /* Appends each computed value one of the frame's values feeds
     * (ADR-0051 section 5): from each input's newest value, when every
     * input has one no older than its bound at the frame's time. A
     * division by zero or a result that isn't finite gives none. */
    void compute_values(ecuio_session_t* session, const ecuio_signal_set_t* set,
                        const ecuio_frame_t* frame, uint32_t& n)
    {
        for (const ecuio_session_t::computed_plan& c : session->computed) {
            const uint32_t inputs = c.program->input_count;
            bool fed = false;
            bool fresh = true;
            double in[ECUIO_COMPUTED_MAX_INPUTS] = {};
            for (uint32_t k = 0; k < inputs; ++k) {
                const ecuio_session_t::latest_value& l =
                    session->latest[c.inputs[k]];
                fed = fed || l.decode == session->decodes;
                if (l.decode == 0 || l.time_us > frame->timestamp_us ||
                    frame->timestamp_us - l.time_us > c.max_age_us[k]) {
                    fresh = false;
                }
                in[k] = l.value;
            }
            ecuio_value_t v{};
            if (!fed || !fresh ||
                !ecuio_computed_run(*c.program, in, &v.value)) {
                continue;
            }
            const ecuio_signal_def_t& def = set->defs[c.def];
            v.signal_id = def.id;
            v.kind = def.kind;
            session->values[n++] = v;
        }
    }

    /* Decodes frame against the session's set into its buffers
     * (ADR-0024 section 4): the number of values, in the set's order, or for a
     * packed Service 01 reply record by record (ADR-0039 Appendix A),
     * each record a frame of its own in record_frame, so each def
     * decodes at most once. Each text value takes the next slot, from
     * the first again each frame; create gave the session a slot for
     * every text value a frame can decode to. */
    uint32_t decode_values(ecuio_session_t* session,
                           const ecuio_frame_t* frame)
    {
        const ecuio_signal_set_t* set = session->active_signal_set;
        if (set == nullptr || set->defs == nullptr ||
            session->values.size() < set->count) {
            return 0;
        }
        const uint16_t len = (frame->data_length <= sizeof(frame->data))
                                 ? frame->data_length
                                 : static_cast<uint16_t>(sizeof(frame->data));
        uint32_t n = 0;
        uint32_t slot = 0;
        ++session->decodes;
        const std::vector<uint32_t>& ids = session->service01_ids;
        if (session->record_frame &&
            (frame->flags & ECUIO_FRAME_FLAG_ERROR) == 0 &&
            std::find(ids.begin(), ids.end(), frame->arbitration_id) !=
                ids.end()) {
            ecuio_obd2_record records[ECUIO_OBD2_MAX_RECORDS];
            uint32_t count = 0;
            const ecuio_obd2_reply reply =
                ecuio_obd2_split(frame->data, len, session->learned_lengths,
                                 records, &count);
            if (reply == ecuio_obd2_reply::split) {
                ecuio_frame_t& r = *session->record_frame;
                r.arbitration_id = frame->arbitration_id;
                r.flags = frame->flags;
                r.timestamp_us = frame->timestamp_us;
                r.data[0] = 0x41u;
                for (uint32_t k = 0; k < count; ++k) {
                    r.data[1] = records[k].pid;
                    memcpy(r.data + 2, frame->data + records[k].start,
                           records[k].bytes);
                    r.data_length = static_cast<uint16_t>(2 + records[k].bytes);
                    decode_frame(session, set, &r, r.data_length, n, slot);
                }
                compute_values(session, set, frame, n);
                return n;
            }
            if (reply == ecuio_obd2_reply::unsplit) {
                session->unsplit_replies.fetch_add(1, std::memory_order_relaxed);
            }
        }
        decode_frame(session, set, frame, len, n, slot);
        compute_values(session, set, frame, n);
        return n;
    }

    /* The computed defs' plans (ADR-0051 section 5), from a set the
     * validator passed: each input's def index and age bound. Runs once,
     * at create; throws bad_alloc, which create catches. */
    void plan_computed(ecuio_session_t* s, const ecuio_signal_set_t* set)
    {
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
                continue;
            }
            ecuio_session_t::computed_plan c;
            c.def = i;
            c.program = &set->computed[d.table_first];
            for (uint32_t k = 0; k < c.program->input_count; ++k) {
                for (uint32_t j = 0; j < set->count; ++j) {
                    if (set->defs[j].id == c.program->input_ids[k]) {
                        c.inputs[k] = j;
                        break;
                    }
                }
                const uint32_t age = c.program->max_age_us[k];
                c.max_age_us[k] = age != 0 ? age : ECUIO_VALUE_STALE_US;
            }
            s->computed.push_back(c);
        }
        if (!s->computed.empty()) {
            s->latest.resize(set->count);
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
    /* An open refused with a status of the vendor's own and no text says
     * nothing a user can act on; the two usual causes, for any vendor,
     * are no adapter and another program holding it (the VX answers 34
     * to both: docs/bench-sessions/2026-11-02-vehicle.md). The vendor's
     * text, when it gave one, goes first below. */
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
    if (set == nullptr || set->defs == nullptr || !session->carried) {
        return ECUIO_OK;
    }
    const uint32_t defs = static_cast<uint32_t>(session->values.size());
    for (uint32_t i = 0; i < defs; ++i) {
        if (session->carried[i].load(std::memory_order_relaxed) == 0 &&
            session->short_frames[i].load(std::memory_order_relaxed) >=
                ECUIO_ABSENT_AFTER_FRAMES) {
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
    *count = session->unsplit_replies.load(std::memory_order_relaxed);
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

    /* The query, split once (uri_query.h) and checked against the keys
     * the scheme's transport reads, before anything attaches: a
     * malformed query or an unknown key fails with no DLL load and no
     * discovery, the same way on every scheme. */
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

    /* ADR-0013 profile selection: default per scheme, ?profile=
     * override. Resolved and validated BEFORE any transport attach so
     * an unknown or MALFORMED profile fails fast with no DLL load and
     * no discovery. */
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
    const ecuio_status_t found = ecuio_profile_lookup(chosen, &profile_set);
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
    if (profile_set != nullptr && profile_set->defs != nullptr &&
        profile_set->count > 0) {
        try {
            s->values.resize(profile_set->count);
            s->short_frames.reset(
                new std::atomic<uint8_t>[profile_set->count]());
            s->carried.reset(new std::atomic<uint8_t>[profile_set->count]());
            s->text_slots = text_slots_for(profile_set);
            s->text_scratch.resize(size_t{s->text_slots} *
                                   ECUIO_DECODE_TEXT_MAX);
            ecuio_def_index_build(s->def_index, profile_set->defs,
                                  profile_set->count);
            s->def_found.resize(profile_set->count);
            for (uint32_t i = 0; i < profile_set->count; ++i) {
                const ecuio_signal_def_t& d = profile_set->defs[i];
                if (ecuio_service01_keyed(d) &&
                    std::find(s->service01_ids.begin(), s->service01_ids.end(),
                              d.message_id) == s->service01_ids.end()) {
                    s->service01_ids.push_back(d.message_id);
                }
            }
            if (!s->service01_ids.empty()) {
                s->record_frame.reset(new ecuio_frame_t{});
            }
            plan_computed(s.get(), profile_set);
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
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
    /* After attach, which fills the rest of the info. */
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
    if (session->record_link != nullptr) {
        ecuio_record_detach(*session->record_link);
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

    /* A recording's host_us, stamped before any callback (ADR-0049
     * section 1); the link is set before start and kept until the
     * session is freed */
    ecuio_record_link* link = session->record_link.get();
    uint64_t host_us = 0;
    if (link != nullptr &&
        link->active.load(std::memory_order_relaxed) != nullptr) {
        const uint64_t now = ecuio_clock_us();
        host_us = now > link->zero_us ? now - link->zero_us : 0;
    }

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

    /* Then the frame's values (ADR-0024), decoded for a values callback
     * or a recording; the recording takes the frame and its values as
     * one record */
    const bool want_values = values_taken && values.size() > 0;
    ecuio_recorder_t* recorder =
        link != nullptr ? ecuio_record_pin(*link) : nullptr;
    if (!want_values && recorder == nullptr) {
        return;
    }
    const uint32_t count = decode_values(session, frame);
    if (recorder != nullptr) {
        ecuio_recorder_put_frame(recorder, host_us, frame,
                                 session->values.data(), count);
        ecuio_record_unpin(*link);
    }
    if (!want_values || count == 0) {
        return;
    }
    ecuio_value_batch_t batch{};
    batch.timestamp_us = frame->timestamp_us;
    batch.arbitration_id = frame->arbitration_id;
    batch.count = count;
    batch.values = session->values.data();
    for (size_t i = 0; i < values.size(); ++i) {
        values[i].first(&batch, values[i].second);
    }
}

} // extern "C"
