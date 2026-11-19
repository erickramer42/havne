// File/layer: core/src/request_setup.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: CoreTest, PickOnlyTest, RecorderTest (test_core); HARNESS:
//          J2534QueryParamTest, J2534AdoptionTest (test_j2534_stream);
//          Elm327SessionTest (test_elm327); J2534HealthTest
//          (test_j2534_health); SocketCanRequestTest (test_vcan)
//
// What a requesting transport (J2534, ELM327) derives from its URI and
// profile before it opens anything: the request keys' values
// (request_params.h), the channels, the requests (request_engine.h),
// and the reports its worker publishes. One place, so a run sheet's
// query means the same on either scheme (ADR-0026 section 2).

#ifndef ECUIO_REQUEST_SETUP_H
#define ECUIO_REQUEST_SETUP_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "isotp.h"
#include "link_health.h"
#include "obd2_rates.h"
#include "pid_support.h"
#include "request_engine.h"
#include "request_params.h"
#include "session.h"
#include "talk_seam.h"
#include "uri_query.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

/* The channels, from the set plus the URI's overrides, into rx[] and
 * tx[] (ECUIO_MAX_CHANNELS each); returns how many. Never fails:
 * bounded by construction, and malformed set entries are skipped.
 * ?filter= wins outright (one channel); ?txid= alone rewrites every
 * derived tx. A log-only profile (raw: no set) with no ?filter= listens
 * on the default profile's channels, obd2's engine ECU (ADR-0013's
 * appendix of 2026-10-26): a channel with no receive ID hears nothing. A
 * set without channels, and a lone ?txid= on one, yields no channel. */
inline uint32_t ecuio_resolve_channels(const ecuio_signal_set_t* set,
                                       const request_params& params,
                                       uint32_t* rx, uint32_t* tx)
{
    if (set == nullptr && params.rx_id == 0) {
        set = ecuio_obd2_signal_set();
    }
    uint32_t n = 0;
    if (params.rx_id != 0) {
        uint32_t t = params.tx_id;
        if (t == 0 && set != nullptr && set->channel_count > 0) {
            t = set->channels[0].tx_id;
        }
        if (t == 0) {
            t = ECUIO_OBD2_TESTER_ARB_ID;
        }
        rx[0] = params.rx_id;
        tx[0] = t;
        n = 1;
    } else if (set != nullptr) {
        for (uint32_t i = 0; i < set->channel_count && n < ECUIO_MAX_CHANNELS;
             ++i) {
            const uint32_t r = set->channels[i].rx_id;
            if (r == 0) {
                continue;
            }
            uint32_t t =
                (params.tx_id != 0) ? params.tx_id : set->channels[i].tx_id;
            if (t == 0) {
                t = ECUIO_OBD2_TESTER_ARB_ID;
            }
            rx[n] = r;
            tx[n] = t;
            ++n;
        }
    }
    return n;
}

/* Whether a channel uses 29-bit IDs: either of its IDs past 11 bits. */
inline bool ecuio_channel_is_29bit(uint32_t rx, uint32_t tx)
{
    return rx > ECUIO_CAN_ID_11BIT_MAX || tx > ECUIO_CAN_ID_11BIT_MAX;
}

/* The channel ?poll='s request goes to, by the rule the set's rotation
 * follows (request_engine.h): that of a def whose value comes back in
 * its reply, else channel 0. */
inline uint32_t ecuio_poll_channel(const ecuio_signal_set_t* set, uint8_t pid,
                                   const uint32_t* rx_ids, uint32_t rx_count)
{
    if (set == nullptr || set->defs == nullptr) {
        return 0;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (ecuio_engine_request_for_def(&set->defs[i], req, &len) &&
            len == 2 && req[0] == 0x01 && req[1] == pid) {
            return ecuio_engine_channel_for(&set->defs[i], rx_ids, rx_count);
        }
    }
    return 0;
}

/* Whether table row r is the request req (len bytes) on channel c: how
 * a focus finds a picked def's row (ecuio_engine_request_for_def, the
 * mux key at offset 0 as ecuio_engine_derive enrols it, on the channel
 * of its message_id) */
inline bool ecuio_focus_row_is(const ecuio_request& r, uint32_t c,
                               const uint8_t* req, uint8_t len)
{
    return r.channel == c && r.len == len && r.bytes[0] == req[0] &&
           r.bytes[1] == req[1] && (len < 3 || r.bytes[2] == req[2]);
}

/* The def-to-row map (rows, n of them, in place: no allocation): each
 * def with no row yet whose request is a row of e's table from `from` on
 * gets it. The rows before were resolved when they joined, so a DID the
 * scan finds costs its own rows times the defs still without one, never
 * the whole table. */
inline void ecuio_focus_resolve(const ecuio_signal_set_t* set,
                                const ecuio_request_engine& e,
                                const uint32_t* rx_ids, uint32_t chan_count,
                                uint32_t* rows, size_t n, uint32_t from)
{
    if (set == nullptr || set->defs == nullptr || from >= e.count) {
        return;
    }
    for (size_t i = 0; i < n && i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (rows[i] != ECUIO_ROW_NONE ||
            !ecuio_engine_request_for_def(&set->defs[i], req, &len)) {
            continue;
        }
        const uint32_t c =
            ecuio_engine_channel_for(&set->defs[i], rx_ids, chan_count);
        for (uint32_t k = from; k < e.count; ++k) {
            if (ecuio_focus_row_is(e.requests[k], c, req, len)) {
                rows[i] = k;
                break;
            }
        }
    }
}

/* Each def's byte in askable (n of them, in place: no allocation): 1
 * when its request is a row of e's table (rows, the def-to-row map) that
 * isn't dropped, parked or not, since a focus can unpark it; 0 for a def
 * asked nowhere (a passive def, Service 09's, a request the probe
 * dropped, a DID the scan hasn't found, a PID ?poll= doesn't ask). What
 * ecuio_session_set_focus judges a focus by (ADR-0056 section 2). */
inline void ecuio_focus_askable(const ecuio_request_engine& e,
                                const uint32_t* rows, uint8_t* askable,
                                size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        askable[i] =
            rows[i] < e.count && e.requests[rows[i]].dropped == 0 ? 1 : 0;
    }
}

/* Each def's byte in unasked (n of them, in place): 1 when a focus
 * parks its request (its row of e's table by rows, parked and not
 * dropped), or when it is computed and an input is so, followed through
 * its chain; 0 for every def with no focus on (nothing is parked), and
 * for a def asked nowhere (a passive def, Service 09's, a dropped
 * request, a DID the scan hasn't found). A def that shares a picked
 * def's reply shares its row, so it is asked. What
 * ecuio_session_get_unasked_signals lists. */
inline void ecuio_focus_unasked(const ecuio_signal_set_t* set,
                                const ecuio_request_engine& e,
                                const uint32_t* rows, uint8_t* unasked,
                                size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unasked[i] = 0;
        if (rows[i] < e.count) {
            const ecuio_request& r = e.requests[rows[i]];
            unasked[i] = r.parked != 0 && r.dropped == 0 ? 1 : 0;
        }
    }
    for (size_t i = 0;
         set != nullptr && set->defs != nullptr && i < n && i < set->count;
         ++i) {
        if (ecuio_computed_input_gone(*set, set->defs[i], [&](uint32_t k) {
                return k < n && unasked[k] != 0;
            })) {
            unasked[i] = 1;
        }
    }
}

/* ?packframes= sizes Service 01 packs, so it is refused (the detail set)
 * where nothing packs: e->pack's rule below, ?pack=0, ?probe=0 or ?poll= */
inline bool ecuio_pack_frames_fit(const request_params& params)
{
    if (params.pack_frames == 0 ||
        (params.pack && params.probe && !params.has_poll)) {
        return true;
    }
    ecuio_detail_set("?packframes= sizes Service 01 packs: it can't go with %s",
                     !params.pack    ? "?pack=0"
                     : !params.probe ? "?probe=0"
                                     : "?poll=");
    return false;
}

/* The requests, into e (initialized here for chan_count channels):
 *   ?poll=        -> that one Service 01 request; the set's rotation
 *                    isn't built, so the PID in the URI is the only one
 *                    asked for. It may be 0x00 (the supported-PIDs
 *                    bitmap), hence the presence flag
 *   uds-generic   -> the rotation from the set's polled head, and the
 *                    scan of its ranges (ADR-0020)
 *   any other set -> the set's rotation (request_engine.h), which
 *                    registration kept within the table; the obd2
 *                    set's in its two rate classes (obd2_rates.h)
 *   no requests   -> passive receive
 * then identification (ADR-0021) when the session already requests and
 * neither ?identify=0 nor ?poll=, and diagnostics (ADR-0037, decoded by
 * the obd2 set whatever the profile) by the same rule with ?diagnose=0.
 * *probe and *identify get what the
 * worker runs: ?poll= sends its one request as asked, whatever the ECU
 * says. Then the coupling check: requests need a channel to receive on,
 * or they go out against guaranteed silence forever. A set's requests
 * get theirs from the set; ?poll= may take ?filter= or the profile's
 * declared channels. BAD_URI with the detail set when they have none,
 * or for ?scan=first outside uds-generic, which alone scans, before any
 * hardware is touched. ?session=03 (ADR-0060) belongs to uds-generic too,
 * and ?keepalive_ms= to it. */
inline ecuio_status_t
ecuio_build_requests(ecuio_session_t* session, const ecuio_signal_set_t* set,
                     const request_params& params, const uint32_t* rx_ids,
                     uint32_t chan_count, ecuio_request_engine* e, bool* probe,
                     bool* identify)
{
    /* The table's room (ADR-0065 section 3): what the set derives, each
     * def's row with it, and each DID the scan can find */
    const size_t defs = set != nullptr && set->defs != nullptr ? set->count : 0;
    const ecuio_signal_set_t head = session->uds != nullptr
                                        ? ecuio_uds_polled_set(*session->uds)
                                        : ecuio_signal_set_t{};
    const ecuio_signal_set_t* asked = params.has_poll           ? nullptr
                                      : session->uds != nullptr ? &head
                                                                : set;
    std::vector<uint32_t> rows;
    ecuio_derived derived;
    try {
        rows.assign(defs, ECUIO_ROW_NONE);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (asked != nullptr && asked->count > defs) {
        asked = nullptr; /* the head is the set's own first defs */
    }
    switch (
        ecuio_engine_plan(asked, rx_ids, chan_count, rows.data(), &derived)) {
    case ecuio_plan_result::out_of_memory:
        return ECUIO_ERR_OUT_OF_MEMORY;
    case ecuio_plan_result::module_full:
        /* Registration holds each of a set's modules to it; a ?filter=
         * puts every signal on one, which is the URI's doing */
        if (params.rx_id != 0) {
            ecuio_detail_set("?filter= asks every signal of the profile of "
                             "one ECU, and they ask more than %u requests",
                             static_cast<unsigned>(ECUIO_MODULE_MAX_REQUESTS));
            return ECUIO_ERR_BAD_URI;
        }
        ecuio_detail_set("the profile's signals ask more than %u requests of "
                         "one ECU",
                         static_cast<unsigned>(ECUIO_MODULE_MAX_REQUESTS));
        return ECUIO_ERR_INVALID_ARGUMENT;
    case ecuio_plan_result::ok:
        break;
    }
    const size_t scanned =
        session->uds != nullptr ? session->uds->scan.size() : 0;
    const size_t room = params.has_poll ? 1 : derived.total + scanned;
    if (!ecuio_engine_init(e, chan_count, static_cast<uint32_t>(room))) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    /* More than a session asks: nothing until a focus picks (section 2) */
    const bool pick_only =
        session->uds == nullptr && derived.total > ECUIO_PROFILE_MAX_REQUESTS;
    if (pick_only) {
        ecuio_engine_pick_only(e);
    }
    if (params.gap_us >= 0) {
        e->gap_us = params.gap_us;
    }
    if (params.scan_first && session->uds == nullptr) {
        ecuio_detail_set("?scan=first belongs to profile=uds-generic, the "
                         "profile that scans DIDs");
        return ECUIO_ERR_BAD_URI;
    }
    e->scan_first = params.scan_first;
    if (params.session == ECUIO_SESSION_EXTENDED && session->uds == nullptr) {
        ecuio_detail_set("?session=03 belongs to profile=uds-generic, the "
                         "profile that reads DIDs");
        return ECUIO_ERR_BAD_URI;
    }
    if (params.keepalive_ms != 0 && params.session != ECUIO_SESSION_EXTENDED) {
        ecuio_detail_set("?keepalive_ms= keeps the extended session alive: "
                         "it needs ?session=03");
        return ECUIO_ERR_BAD_URI;
    }
    if (!ecuio_pack_frames_fit(params)) {
        return ECUIO_ERR_BAD_URI;
    }
    if (params.session == ECUIO_SESSION_EXTENDED) {
        const int64_t keepalive_us =
            params.keepalive_ms != 0
                ? static_cast<int64_t>(params.keepalive_ms) * 1000
                : ECUIO_ENGINE_KEEPALIVE_US;
        (void)ecuio_engine_session(e, ECUIO_SESSION_EXTENDED, keepalive_us);
    }
    if (params.has_poll) {
        const uint8_t req[2] = {0x01, params.poll_pid};
        (void)ecuio_engine_add(
            e, ecuio_poll_channel(set, req[1], rx_ids, chan_count), req,
            sizeof(req));
    } else if (session->uds == nullptr) {
        (void)ecuio_engine_derive_planned(e, set, rx_ids, chan_count,
                                          rows.data());
        if (set == ecuio_obd2_signal_set()) {
            ecuio_engine_classify(e, ecuio_obd2_pid_fast); /* ADR-0039 */
            ecuio_engine_set_duplicates(
                e, &ECUIO_OBD2_DUPLICATE_PIDS[0][0],
                sizeof(ECUIO_OBD2_DUPLICATE_PIDS) /
                    sizeof(ECUIO_OBD2_DUPLICATE_PIDS[0]));
        }
    } else {
        /* uds-generic (ADR-0020): the rotation from the set's polled
         * head, then each scanned DID once, on the channel of its ECU by
         * the rule the rotation's requests follow (ecuio_engine_channel_for:
         * channel 0 under a ?filter= override). Create's limits keep both
         * within the engine's tables. */
        (void)ecuio_engine_derive_planned(e, &head, rx_ids, chan_count,
                                          rows.data());
        for (const ecuio_uds_scan_target& s : session->uds->scan) {
            uint32_t c = 0;
            for (uint32_t i = 0; i < chan_count; ++i) {
                if (rx_ids[i] == s.rx_id) {
                    c = i;
                }
            }
            (void)ecuio_engine_scan_add(e, c, s.did);
        }
    }
    *probe = params.probe && !params.has_poll;
    /* Packing needs the bitmaps: it packs only the PIDs they list */
    e->pack = params.pack && *probe;
    e->pack_frames = params.pack_frames;
    const bool requests = e->count > 0 || e->scan_count > 0;
    *identify = params.identify && !params.has_poll && requests;
    if (*identify) {
        ecuio_engine_identify(e);
    }
    if (params.diagnose && !params.has_poll && requests) {
        ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
    }

    if (requests && chan_count == 0) {
        if (params.has_poll) {
            ecuio_detail_set("?poll= needs a channel to receive on: add "
                             "?filter=, or use a profile that declares one");
        } else {
            ecuio_detail_set("the profile requests PIDs but declares no "
                             "channel to receive on");
        }
        return ECUIO_ERR_BAD_URI;
    }
    /* What a focus is judged by (ADR-0056 section 2): whether the
     * session asks anything, each def's row and which defs it can ask,
     * sized here so the worker keeps the maps in place (ecuio_take_focus).
     * ?poll='s one request is no plan's, so its defs are found here. A
     * session that asks only its picks lists every signal unasked from
     * the start. Made whole first, then handed over. */
    std::vector<uint8_t> askable;
    std::vector<uint8_t> unasked;
    try {
        ecuio_focus_resolve(set, *e, rx_ids, chan_count, rows.data(), defs, 0);
        askable.assign(defs, 0);
        ecuio_focus_askable(*e, rows.data(), askable.data(), defs);
        unasked.assign(defs, 0);
        if (pick_only) {
            ecuio_focus_unasked(set, *e, rows.data(), unasked.data(), defs);
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    std::lock_guard<std::mutex> lock(session->focus_mutex);
    session->focus_requests = requests;
    session->focus_pick_only = pick_only;
    session->focus_rows.swap(rows);
    session->focus_askable.swap(askable);
    session->focus_unasked.swap(unasked);
    session->focus_table = e->count;
    return ECUIO_OK;
}

/* Hands what channel c's ECU has said it supports to the session, for
 * ecuio_session_get_channel_support, when the engine has learned
 * something new. */
inline void ecuio_publish_support(ecuio_session_t* session,
                                  const ecuio_request_engine& e, uint32_t c,
                                  uint32_t rx_id, uint32_t& published)
{
    if (e.chans[c].generation == published) {
        return;
    }
    published = e.chans[c].generation;
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, c, &s);
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (c < session->support.size()) {
        session->support[c] = s;
    }
}

/* Reply timing (ADR-0042) for a requesting session's rotation: the
 * figures in owner, the engine pointing at them, the intervals on clock
 * (ECUIO_TIMING_CLOCK_*). A session with no rotation gets none, and so
 * does one whose figures won't allocate: timing is a report, never a
 * reason to refuse a connect. */
inline void ecuio_attach_timing(ecuio_request_engine& e,
                                std::unique_ptr<ecuio_timing_table>& owner,
                                uint32_t clock)
{
    if (e.count == 0) {
        return;
    }
    owner.reset(new (std::nothrow) ecuio_timing_table());
    if (owner) {
        owner->interval_clock = clock;
        e.timing = owner.get();
    }
}

/* Reply timing (ADR-0042): the engine's figures handed to the session as
 * rows, for ecuio_session_get_timing, at most once a second, and always
 * when final (the worker stopping). iso is ISO-TP's receiver where core
 * reassembles, NULL where the adapter does. published_at is the
 * worker's own. */
constexpr int64_t ECUIO_TIMING_PUBLISH_US = 1000000;

inline void ecuio_publish_timing(ecuio_session_t* session,
                                 const ecuio_request_engine& e,
                                 const isotp_rx* iso, int64_t now_us,
                                 int64_t& published_at, bool final)
{
    if (e.timing == nullptr ||
        (!final && published_at != 0 &&
         now_us - published_at < ECUIO_TIMING_PUBLISH_US)) {
        return;
    }
    published_at = now_us;
    std::unique_lock<std::mutex> lock(session->timing_mutex);
    ecuio_session_timing_t& t = session->timing;
    t.snapshot_us = now_us > 0 ? static_cast<uint64_t>(now_us) : 0;
    t.interval_clock = e.timing->interval_clock;
    /* The pool's rows, in the order taken (ADR-0065 section 3) */
    const uint32_t taken = e.timing_taken;
    t.rows = taken;
    t.untimed = e.untimed;
    if (iso != nullptr) {
        t.flags |= ECUIO_TIMING_ISOTP;
        t.isotp_first_frames = iso->first_frames;
        t.isotp_completed = iso->completed;
        t.isotp_dropped = iso->dropped;
        t.isotp_malformed = iso->malformed;
    }
    for (uint32_t i = 0; i < taken; ++i) {
        const ecuio_request& q = e.requests[e.timing_rows[i]];
        ecuio_request_timing_t& row = session->timing_rows[i];
        row = timing_public(e.timing->rows[i]);
        row.channel = q.channel;
        row.length = q.len < 3 ? q.len : 3;
        for (uint8_t k = 0; k < row.length; ++k) {
            row.request[k] = q.bytes[k];
        }
    }
    lock.unlock();
    /* The recording's copy (ADR-0049 section 1), unlocked: only this
     * worker writes the rows, and the final copy may wait for room */
    ecuio_record_timing(session->record_link.get(), t, session->timing_rows,
                        taken, final);
}

/* Hands what channel c's ECU has said it is to the session, for
 * ecuio_session_get_channel_identity, when an item has settled. */
inline void ecuio_publish_identity(ecuio_session_t* session,
                                   const ecuio_request_engine& e, uint32_t c,
                                   uint32_t rx_id, uint32_t& published)
{
    if (e.chans[c].ident_generation == published) {
        return;
    }
    published = e.chans[c].ident_generation;
    ecuio_channel_identity_t s;
    ecuio_engine_identity(&e, c, &s);
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (c < session->identity.size()) {
        session->identity[c] = s;
    }
}

/* Hands what channel c's ECU has reported of its diagnostics to the
 * session, for ecuio_session_get_channel_diagnostics, when an item has
 * settled or a read has started over. */
inline void ecuio_publish_diagnostics(ecuio_session_t* session,
                                      const ecuio_request_engine& e, uint32_t c,
                                      uint32_t rx_id, uint32_t& published)
{
    if (e.chans[c].diag.generation == published) {
        return;
    }
    published = e.chans[c].diag.generation;
    ecuio_channel_diagnostics_t s;
    ecuio_engine_diagnostics(&e, c, &s);
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (c < session->diagnostics.size()) {
        session->diagnostics[c] = s;
    }
}

/* Starts the diagnostics over (ecuio_engine_diagnose_again) when
 * ecuio_session_read_diagnostics has been called since the worker last
 * looked; seen is the worker's count. */
inline void ecuio_take_diag_request(ecuio_session_t* session,
                                    ecuio_request_engine* e, uint32_t& seen)
{
    const uint32_t asked = session->diag_requests.load();
    if (asked != seen) {
        seen = asked;
        ecuio_engine_diagnose_again(e);
    }
}

/* Starts a clear (ecuio_engine_clear, ADR-0038) on each channel whose
 * ecuio_session_clear_diagnostics request the worker hasn't taken, at
 * the generation the caller saved and with its flags; seen holds the
 * worker's count per channel. The engine checks the generation again, so a read that
 * settled since makes it ECUIO_CLEAR_REPORT_CHANGED, sending nothing. */
inline void ecuio_take_clear_requests(ecuio_session_t* session,
                                      ecuio_request_engine* e, uint32_t* seen)
{
    for (uint32_t c = 0; c < e->chan_count && c < session->info.module_count;
         ++c) {
        const uint32_t asked = session->clear_requests[c].load();
        if (asked != seen[c]) {
            seen[c] = asked;
            (void)ecuio_engine_clear(e, c, session->clear_generation[c].load(),
                                     session->clear_flags[c].load());
        }
    }
}

/* What ecuio_take_focus has taken, the worker's own: the count of
 * focus_generation, and the table's stamp the askable map was worked out
 * at (ecuio_focus_table_stamp) */
struct ecuio_focus_seen {
    uint32_t generation = 0;
    uint64_t table = 0;
    uint32_t rows = 0; /* the table rows the def-to-row map has seen */
};

/* A number that moves whenever the table's rows or their drops can have
 * changed: its size (a scanned DID joins) and every channel's
 * generation (a drop moves it). Both only grow, so the sum never
 * repeats. */
inline uint64_t ecuio_focus_table_stamp(const ecuio_request_engine& e)
{
    uint64_t stamp = e.count;
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        stamp += e.chans[c].generation;
    }
    return stamp;
}

/* Takes a focus (ecuio_session_set_focus, ADR-0056 section 2) the
 * worker hasn't taken, or resolves the stored picks again when a request
 * joined parked under it (focus_joined: a scanned DID found): under the
 * session's lock, each picked def's row (focus_rows, the def-to-row map)
 * is marked kept, and so is each attached condition's signal's
 * (condition_defs); then the engine parks the rest, or ends the focus on
 * a count of 0. A table that asks only its picks keeps the conditions'
 * rows with no focus too (ADR-0065 section 8). First, when the table
 * changed, the map takes the rows that joined and the askable map is
 * worked out again in place, and, while a focus is on, the unasked map
 * after it and after each focus taken. The marks are the engine's, sized
 * with its table: no allocation on the worker. */
inline void ecuio_take_focus(ecuio_session_t* session, ecuio_request_engine* e,
                             ecuio_focus_seen& seen)
{
    const auto unasked = [&] {
        ecuio_focus_unasked(
            session->active_signal_set, *e, session->focus_rows.data(),
            session->focus_unasked.data(), session->focus_unasked.size());
    };
    const uint64_t table = ecuio_focus_table_stamp(*e);
    if (table != seen.table) {
        seen.table = table;
        std::lock_guard<std::mutex> lock(session->focus_mutex);
        ecuio_focus_resolve(
            session->active_signal_set, *e, session->module_rx.data(),
            session->info.module_count, session->focus_rows.data(),
            session->focus_rows.size(), seen.rows);
        seen.rows = e->count;
        ecuio_focus_askable(*e, session->focus_rows.data(),
                            session->focus_askable.data(),
                            session->focus_askable.size());
        if (e->focused) {
            unasked();
        }
    }
    const uint32_t asked = session->focus_generation.load();
    if (asked == seen.generation && !e->focus_joined) {
        return;
    }
    const bool asked_again = asked != seen.generation;
    seen.generation = asked;
    uint8_t* const keep = e->marks.data();
    if (e->count > 0) {
        std::memset(keep, 0, e->count);
    }
    bool focused = false;
    int64_t min_sweep_us = 0;
    {
        std::lock_guard<std::mutex> lock(session->focus_mutex);
        const bool picks = !session->focus_ids.empty();
        focused = picks || e->pick_only;
        min_sweep_us = int64_t{session->focus_min_sweep_ms} * 1000;
        const size_t defs = session->focus_defs.size();
        const size_t watched = session->condition_defs.size();
        const size_t rows = session->focus_rows.size();
        for (size_t i = 0; focused && i < rows; ++i) {
            const bool kept =
                (picks && i < defs && session->focus_defs[i] != 0) ||
                (i < watched && session->condition_defs[i] != 0);
            if (kept && session->focus_rows[i] < e->count) {
                keep[session->focus_rows[i]] = 1;
            }
        }
    }
    const bool was_focused = e->focused;
    if (focused && asked_again) {
        ecuio_engine_focus(e, keep, min_sweep_us);
    } else if (focused) {
        ecuio_engine_focus_keep(e, keep); /* only a request joined */
    } else {
        ecuio_engine_unfocus(e);
    }
    /* With no focus the map stays zeroed: once more as a focus ends */
    if (e->focused || was_focused) {
        std::lock_guard<std::mutex> lock(session->focus_mutex);
        unasked();
    }
}

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

/* How often a session with nothing picked and its link down asks one
 * request to see whether an ECU answers: the failure backoff's cap */
constexpr int64_t ECUIO_UNPICKED_CHECK_US =
    static_cast<int64_t>(LINK_BACKOFF_CAP_MS) * 1000;

/* That interval as the workers read it: a test's own while one is set
 * (ecuio_set_unpicked_check_ms, internal.h) */
inline std::atomic<int64_t>& ecuio_unpicked_check_us()
{
    static std::atomic<int64_t> us{ECUIO_UNPICKED_CHECK_US};
    return us;
}

/* A worker's place in those checks: when the next is due (0: none
 * waited for) and the channel it starts looking at */
struct ecuio_unpicked_check {
    int64_t next_us = 0;
    uint32_t channel = 0;
};

/* A wait on a request ran out (ecuio_engine_expire answered true): the
 * link counts it, unless the session has nothing picked and another
 * module answers (ecuio_engine_unpicked_answering), where the silence
 * is one module's */
inline void ecuio_request_timed_out(const ecuio_request_engine& e,
                                    link_health& h)
{
    if (!ecuio_engine_unpicked_answering(&e)) {
        link_on_request_timeout(h);
    }
}

/* A worker's idle pass on a session that asks only its picks with
 * nothing picked (ecuio_engine_unpicked), after a read that didn't
 * fail: link_on_unpicked, and while the link then isn't OK one request
 * every ecuio_unpicked_check_us(), the first an interval after the
 * first such pass, each module in turn (ecuio_engine_ask_check).
 * Nothing else is asked, so nothing else could show an ECU awake. Its
 * reply is a matched frame like any; its silence changes nothing. */
inline void ecuio_unpicked_pass(ecuio_request_engine* e, link_health& h,
                                ecuio_unpicked_check& check, int64_t now_us)
{
    link_on_unpicked(h);
    if (h.state == ECUIO_OK) {
        check.next_us = 0;
        return;
    }
    const int64_t every =
        ecuio_unpicked_check_us().load(std::memory_order_relaxed);
    if (check.next_us == 0) {
        check.next_us = now_us + every;
        return;
    }
    if (now_us < check.next_us) {
        return;
    }
    check.next_us = now_us + every;
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        const uint32_t c = (check.channel + k) % e->chan_count;
        if (ecuio_engine_ask_check(e, c)) {
            check.channel = (c + 1) % e->chan_count;
            return;
        }
    }
}

#endif /* ECUIO_REQUEST_SETUP_H */
