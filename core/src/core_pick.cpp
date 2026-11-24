// File/layer: core/src/core_pick.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: PickOnlyTest, DefIndexTest, ConditionSessionTest (test_core)

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "session.h"
#include "request_engine.h"   /* ECUIO_IDENT_REQUESTS, the names and the VIN */
#include "request_setup.h"    /* the unpicked check's interval */
#include "computed.h"        /* a computed pick's leaves, for a focus */
#include "condition_eval.h"
#include "condition_file.h" /* the set's bytes and keys, for a recording */
#include "sha256.h"
#include "probes.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <utility>

namespace {


/* The session's table rows, when a rotation could pass its most
 * (ECUIO_PROFILE_MAX_REQUESTS): 0 for a table that fits one whole, which
 * needs no count. The size of rotation_rows' marks. */
size_t rotation_table(const ecuio_session_t* session)
{
    std::lock_guard<std::mutex> lock(session->focus_mutex);
    return session->focus_table > ECUIO_PROFILE_MAX_REQUESTS
               ? session->focus_table
               : 0;
}

/* The requests a rotation of these defs holds: the table rows of a's and
 * b's marked defs (one byte for each def; either may be short or empty)
 * through the session's def-to-row map, each row once. marks holds one
 * zeroed byte for each table row. Under focus_mutex. */
uint32_t rotation_rows(const ecuio_session_t* session,
                       const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b,
                       std::vector<uint8_t>& marks)
{
    uint32_t rows = 0;
    for (size_t i = 0; i < session->focus_rows.size(); ++i) {
        const uint32_t row = session->focus_rows[i];
        const bool marked =
            (i < a.size() && a[i] != 0) || (i < b.size() && b[i] != 0);
        if (marked && row < marks.size() && marks[row] == 0) {
            marks[row] = 1;
            ++rows;
        }
    }
    return rows;
}

} // namespace

namespace {
void store_modules(ecuio_session_t* session);

/* The picks handed over, under focus_mutex, with the modules they read */
void store_picks(ecuio_session_t* session, std::vector<uint16_t>& ids,
                 std::vector<uint8_t>& picked, uint32_t min_sweep_ms,
                 uint32_t flags)
{
    session->focus_ids.swap(ids);
    session->focus_defs.swap(picked);
    session->focus_min_sweep_ms = min_sweep_ms;
    session->focus_flags = flags;
    store_modules(session);
}

/* The conditions' signals handed over, under focus_mutex, with the
 * modules they read; whether the map moved */
bool store_watched(ecuio_session_t* session, std::vector<uint8_t>& watched)
{
    const bool moved = watched != session->condition_defs;
    session->condition_defs.swap(watched);
    store_modules(session);
    return moved;
}

/* The modules the stored picks and conditions read (ADR-0065 step 5),
 * under focus_mutex, before focus_generation moves */
void store_modules(ecuio_session_t* session)
{
    const ecuio_signal_set_t* set = session->active_signal_set;
    const size_t defs =
        (set != nullptr && set->defs != nullptr) ? set->count : 0;
    const auto bytes = [defs](const std::vector<uint8_t>& v) {
        return v.size() >= defs && defs > 0 ? v.data() : nullptr;
    };
    const ecuio_modules_read m = ecuio_modules_wanted(
        set, session->module_rx.data(),
        static_cast<uint32_t>(session->module_rx.size()),
        session->focus_ids.data(),
        static_cast<uint32_t>(session->focus_ids.size()),
        bytes(session->focus_defs), bytes(session->condition_defs), defs);
    session->focus_modules = m.mask;
    session->focus_module_count = m.count;
    for (uint32_t k = 0; k < m.count; ++k) {
        session->focus_module_order[k] = m.order[k];
    }
}

/* ecuio_session_test_after_conditions_handover's (internal.h) */
std::atomic<void (*)(void*)> g_after_conditions_handover{nullptr};
std::atomic<void*> g_after_conditions_handover_user{nullptr};
} // namespace

extern "C" {

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
    if ((flags & ~ECUIO_FOCUS_DEFAULT_PICK) != 0) {
        ecuio_detail_set(
            "the focus takes no flag 0x%x",
            static_cast<unsigned>(flags & ~ECUIO_FOCUS_DEFAULT_PICK));
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
    if ((flags & ECUIO_FOCUS_DEFAULT_PICK) != 0 && count == 0) {
        ecuio_detail_set("a default pick goes with signals picked: with none, "
                         "set no focus and the whole set is asked");
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
    std::vector<uint8_t> marks; /* rotation_rows' */
    try {
        /* Each pick once, at its first place: the recording names it once */
        std::vector<uint8_t> seen(count > 0 ? 65536u : 0u, 0);
        for (uint32_t k = 0; k < count; ++k) {
            if (seen[signal_ids[k]] == 0) {
                seen[signal_ids[k]] = 1;
                ids.push_back(signal_ids[k]);
            }
        }
        picked.assign(defs, 0);
        marks.assign(count > 0 ? rotation_table(session) : 0, 0);
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
    /* A computed pick carries no request: the number defs under it are
     * what the worker keeps asked (ADR-0062). focus_ids keeps the picks
     * as given. */
    if (defs > 0) {
        ecuio_computed_mark_leaves(set, picked.data());
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
        /* The picks' rotation, the conditions' signals in it (ADR-0065
         * section 3) */
        if (!marks.empty()) {
            const uint32_t rows =
                rotation_rows(session, picked, session->condition_defs, marks);
            if (rows > ECUIO_PROFILE_MAX_REQUESTS) {
                ecuio_detail_set(
                    "the picked signals, with the attached conditions', ask "
                    "%u requests: a session asks %u at a time",
                    static_cast<unsigned>(rows),
                    static_cast<unsigned>(ECUIO_PROFILE_MAX_REQUESTS));
                return ECUIO_ERR_INVALID_ARGUMENT;
            }
        }
        store_picks(session, ids, picked, min_sweep_ms, flags);
    }
    /* The picks first: a worker that sees the count moved reads them */
    session->focus_generation.fetch_add(1);
    ecuio_record_focus(session);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_set_conditions(ecuio_session_t* session,
                                            const ecuio_condition_set_t* set,
                                            const ecuio_signal_file_t* file)
{
    ecuio_detail_clear();
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    const ecuio_signal_set_t* signals = session->active_signal_set;
    if (file != nullptr && ecuio_signal_file_set(file) != signals) {
        ecuio_detail_set("the signal file isn't the one the session decodes "
                         "with");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<ecuio_condition_eval> next;
    std::shared_ptr<const ecuio_condition_record> record;
    std::vector<ecuio_condition_state_t> states;
    std::vector<ecuio_session::condition_read> reads;
    /* The signals the conditions read, for the focus: the evaluator holds
     * only those of the conditions it judges */
    std::vector<uint8_t> watched;
    try {
        if (set != nullptr) {
            next = ecuio_condition_eval::make(set, signals, file, 0,
                                              session->judges_link);
            /* A recorder takes this under the locks by pointer, so the
             * hash and the copies are made here */
            auto made = std::make_shared<ecuio_condition_record>();
            made->bytes = set->bytes;
            made->sha256 =
                ecuio_sha256::hex_of(set->bytes.data(), set->bytes.size());
            for (const ecuio_condition_set::condition& c : set->conditions) {
                const bool records =
                    (c.info.uses & ECUIO_CONDITION_USE_RECORD) != 0 &&
                    (c.info.flags & ECUIO_CONDITION_INFO_DISABLED) == 0;
                made->conditions.push_back(
                    {c.key, c.name,
                     (c.info.uses & ECUIO_CONDITION_USE_MARK) != 0, records,
                     c.info.before_us, c.info.after_us});
            }
            record = std::move(made);
            states.reserve(next->conditions.size());
            reads.resize(next->conditions.size());
            for (uint32_t i = 0; i < next->conditions.size(); ++i) {
                states.push_back(next->state_of(i));
                const ecuio_condition_eval::condition& c = next->conditions[i];
                reads[i].any = c.any;
                for (uint32_t k = 0; c.available != 0 && k < c.term_count &&
                                     k < ECUIO_CONDITION_MAX_TERMS;
                     ++k) {
                    const auto& t = next->terms[c.first_term + k];
                    if (t.link != 0) {
                        /* The link's state is always received */
                        reads[i].sampled =
                            static_cast<uint8_t>(reads[i].sampled | 1u << k);
                    } else {
                        reads[i].ids[k] = next->signals[t.signal].id;
                    }
                    reads[i].count = static_cast<uint8_t>(k + 1);
                }
            }
            if (!next->signals.empty() && signals != nullptr &&
                signals->defs != nullptr) {
                watched.assign(signals->count, 0);
                for (const ecuio_condition_eval::signal& w : next->signals) {
                    for (uint32_t i = 0; i < signals->count; ++i) {
                        if (signals->defs[i].id == w.id) {
                            watched[i] = 1;
                        }
                    }
                }
                ecuio_computed_mark_leaves(signals, watched.data());
            }
        }
        /* The rotation with these signals in it (ADR-0065 section 3):
         * judged before anything is handed over, the focus's picks
         * counted while a focus is on. The session's one caller makes
         * no focus call meanwhile. */
        std::vector<uint8_t> marks(rotation_table(session), 0);
        if (!marks.empty()) {
            std::lock_guard<std::mutex> lock(session->focus_mutex);
            static const std::vector<uint8_t> none;
            const uint32_t rows = rotation_rows(
                session,
                session->focus_ids.empty() ? none : session->focus_defs,
                watched, marks);
            if (rows > ECUIO_PROFILE_MAX_REQUESTS) {
                ecuio_detail_set(
                    "the conditions' signals, with the focus's, ask %u "
                    "requests: a session asks %u at a time",
                    static_cast<unsigned>(rows),
                    static_cast<unsigned>(ECUIO_PROFILE_MAX_REQUESTS));
                return ECUIO_ERR_INVALID_ARGUMENT;
            }
        }
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    /* What the worker left behind and what it never took are freed here,
     * outside the lock; the set it holds stays its own */
    std::unique_ptr<ecuio_condition_eval> retired;
    std::unique_ptr<ecuio_condition_eval> untaken;
    {
        /* The recorder's attach takes the link's mutex too, so the check
         * and the handover are one step to it (session.h's lock order) */
        std::lock_guard<std::mutex> attach(session->record_link->m);
        if (session->record_link->active.load() != nullptr) {
            ecuio_detail_set("a recording is under way, and it holds one set "
                             "of conditions for its whole length");
            return ECUIO_ERR_IN_USE;
        }
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        const uint32_t generation = ++session->conditions_generation;
        if (next != nullptr) {
            next->generation = generation;
        }
        for (ecuio_condition_state_t& s : states) {
            s.generation = generation;
        }
        {
            /* The states first: the worker may judge a frame the moment
             * the lock goes, and writes only its own generation's */
            std::lock_guard<std::mutex> states_lock(
                session->condition_states_mutex);
            session->condition_states.swap(states);
            session->condition_reads.swap(reads);
            session->condition_states_generation = generation;
        }
        /* The record before goes with the locks, freed outside them */
        session->conditions_record.swap(record);
        retired = std::move(session->conditions_retired);
        untaken = std::move(session->conditions_pending);
        session->conditions_pending = std::move(next);
        session->conditions_swap = true;
        session->conditions_waiting.store(true);
    }
    {
        /* A focus keeps the conditions' signals asked (ADR-0063 section 7),
         * and a session that asks only its picks asks them with no focus
         * (ADR-0065 section 8). A new generation restarts every sweep, so
         * only a changed map moves it, under a focus or on such a session;
         * otherwise the next focus call moves it. */
        std::lock_guard<std::mutex> lock(session->focus_mutex);
        if (store_watched(session, watched) &&
            (!session->focus_ids.empty() || session->focus_pick_only)) {
            session->focus_generation.fetch_add(1);
        }
    }
    if (const auto hook = g_after_conditions_handover.load()) {
        hook(g_after_conditions_handover_user.load());
    }
    return ECUIO_OK;
}

void ecuio_session_test_after_conditions_handover(void (*hook)(void* user),
                                                  void* user)
{
    /* The user before the hook, and a clear leaves the user: a reader
     * that took the old hook still finds its own */
    if (hook != nullptr) {
        g_after_conditions_handover_user.store(user);
    }
    g_after_conditions_handover.store(hook);
}

namespace {

/* Available, with a term whose signal has no sample since the attach */
bool unsampled(const ecuio_condition_state_t& s,
               const ecuio_session::condition_read& r)
{
    return (s.flags & ECUIO_CONDITION_UNAVAILABLE) == 0 && r.count != 0 &&
           r.sampled != (1u << r.count) - 1u;
}

/* One byte per def of the session's set: 1 for a def the session reports
 * it doesn't get (unsupported, absent, its request dropped), or computed
 * from one. On the caller's thread, from the lists as they stand. */
void defs_not_received(const ecuio_session_t* session,
                       std::vector<uint8_t>& gone)
{
    const ecuio_signal_set_t* set = session->active_signal_set;
    gone.clear();
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        return;
    }
    gone.assign(set->count, 0);
    std::vector<std::pair<uint16_t, uint32_t>> by_id;
    by_id.reserve(set->count);
    for (uint32_t i = 0; i < set->count; ++i) {
        by_id.emplace_back(set->defs[i].id, i);
    }
    std::sort(by_id.begin(), by_id.end());
    const auto mark = [&](const uint16_t* ids, uint32_t n) {
        for (uint32_t k = 0; k < n; ++k) {
            const auto it = std::lower_bound(by_id.begin(), by_id.end(),
                                             std::make_pair(ids[k], 0u));
            if (it != by_id.end() && it->first == ids[k]) {
                gone[it->second] = 1;
            }
        }
    };
    using list_fn = ecuio_status_t (*)(const ecuio_session_t*, uint16_t*,
                                       uint32_t, uint32_t*);
    for (const list_fn list : {&ecuio_session_get_unsupported_signals,
                               &ecuio_session_get_absent_signals}) {
        uint32_t n = 0;
        if (list(session, nullptr, 0, &n) != ECUIO_OK || n == 0) {
            continue;
        }
        std::vector<uint16_t> ids(n);
        uint32_t got = 0;
        if (list(session, ids.data(), n, &got) == ECUIO_OK) {
            mark(ids.data(), std::min(got, n));
        }
    }
    /* A def with a request whose row is gone from the table */
    std::vector<uint8_t> dropped(set->count, 0);
    {
        std::lock_guard<std::mutex> lock(session->focus_mutex);
        for (uint32_t i = 0; session->focus_requests && i < set->count &&
                             i < session->focus_askable.size();
             ++i) {
            uint8_t req[3] = {};
            uint8_t len = 0;
            dropped[i] =
                session->focus_askable[i] == 0 &&
                        ecuio_engine_request_for_def(&set->defs[i], req, &len)
                    ? 1
                    : 0;
        }
    }
    /* A computed def judged over every leaf not received at once, so
     * inputs gone for different reasons count together */
    std::vector<uint8_t> leaves(set->count, 0);
    for (uint32_t i = 0; i < set->count; ++i) {
        leaves[i] = gone[i] != 0 || dropped[i] != 0 ? 1 : 0;
    }
    const auto not_got = [&](uint32_t i) { return leaves[i] != 0; };
    for (uint32_t i = 0; i < set->count; ++i) {
        if (leaves[i] != 0 ||
            ecuio_computed_input_gone(*set, set->defs[i], not_got)) {
            gone[i] = 1;
        }
    }
}

/* ECUIO_CONDITION_WHY_NOT_RECEIVED for each condition whose terms read a
 * def not received and unsampled since the attach: any term (an all can't
 * turn ON), or every term of an ANY */
void mark_not_received(const ecuio_session_t* session,
                       const std::vector<ecuio_session::condition_read>& reads,
                       ecuio_condition_state_t* out, uint32_t n)
{
    std::vector<uint8_t> gone;
    defs_not_received(session, gone);
    const ecuio_signal_set_t* set = session->active_signal_set;
    if (gone.empty()) {
        return;
    }
    const auto received = [&](uint16_t id) {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].id == id) {
                return gone[i] == 0;
            }
        }
        return true;
    };
    for (uint32_t i = 0; i < n && i < reads.size(); ++i) {
        const ecuio_session::condition_read& r = reads[i];
        if (!unsampled(out[i], r)) {
            continue;
        }
        uint32_t missing = 0;
        for (uint32_t k = 0; k < r.count; ++k) {
            const bool sampled = ((r.sampled >> k) & 1u) != 0;
            missing += sampled || received(r.ids[k]) ? 0u : 1u;
        }
        if (r.any != 0 ? missing == r.count : missing > 0) {
            out[i].flags = ECUIO_CONDITION_UNAVAILABLE;
            out[i].why = ECUIO_CONDITION_WHY_NOT_RECEIVED;
        }
    }
}

} // namespace

ecuio_status_t ecuio_session_get_condition_states(
    const ecuio_session_t* session, ecuio_condition_state_t* out,
    uint32_t capacity, uint32_t* out_count)
{
    if (session == nullptr || out_count == nullptr ||
        (out == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::vector<ecuio_session::condition_read> reads;
    uint32_t count = 0;
    /* Before start the session has received nothing to judge by */
    bool unjudged = false;
    const bool ran = session->ran.load();
    {
        std::lock_guard<std::mutex> lock(session->condition_states_mutex);
        count = static_cast<uint32_t>(session->condition_states.size());
        const std::vector<ecuio_session::condition_read>& now =
            session->condition_reads;
        for (uint32_t i = 0; i < count && i < capacity; ++i) {
            out[i] = session->condition_states[i];
            unjudged = unjudged ||
                       (ran && i < now.size() && unsampled(out[i], now[i]));
        }
        try {
            if (unjudged) {
                reads = session->condition_reads;
            }
        } catch (const std::bad_alloc&) {
            unjudged = false; /* the states as the worker left them */
        }
    }
    *out_count = count;
    if (unjudged) {
        try {
            mark_not_received(session, reads, out, std::min(count, capacity));
        } catch (const std::bad_alloc&) {
            return ECUIO_OK; /* the states as copied: none marked yet */
        }
    }
    return ECUIO_OK;
}

} // extern "C"
