// File/layer: core/src/session.h | internal, C++17, Qt-free; the session
//             struct behind the opaque ecuio_session_t
// Testing: UNIT: CoreTest, RecorderTest (test_core)

#ifndef ECUIO_SESSION_H
#define ECUIO_SESSION_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "frame_decoder.h"
#include "profile_hold.h"
#include "pull_queue.h"
#include "recorder.h"
#include "uds_generic.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct ecuio_session {
    /* The registry's set, held from create (profile_hold.h); first, so
     * it's released after every member that reads the set */
    ecuio_profile_hold profile_hold;
    std::atomic<bool> closed{false};
    /* The worker was started: a start after it is the transport's no-op,
     * and the session_start probe marks the first only (ADR-0034 section 4).
     * The session's one caller alone reads and writes it. */
    bool started = false;
    std::mutex callback_mutex;
    std::vector<std::pair<ecuio_frame_callback_t, void*>> callbacks;
    /* Under the same mutex: the values callbacks (ADR-0024) */
    std::vector<std::pair<ecuio_values_callback_t, void*>> values_callbacks;
    ecuio_transport_t* transport = nullptr;
    /* The profile's set: the registry's static one, or, under
     * uds-generic, the session's own below (ADR-0020). */
    const ecuio_signal_set_t* active_signal_set = nullptr;
    /* uds-generic's set, built by create from the URI's ?did= entries
     * and owned here so it lives as long as the session; the J2534
     * transport reads its scan list at attach. NULL under any other
     * profile. */
    std::unique_ptr<ecuio_uds_session> uds;
    /* The decode (frame_decoder.h, ADR-0024 section 4), prepared by create
     * from the set, which never changes, with the absent signals' counts.
     * Only dispatch decodes, on the session's one worker; the absent
     * signals' and unsplit replies' counts are read on the caller's
     * thread. */
    ecuio_frame_decoder decoder;
    /* replay://?faults= (ADR-0041): written by the replay worker, read by
     * ecuio_session_get_replay_faults on the caller's thread */
    std::atomic<uint64_t> replay_frames_read{0};
    std::atomic<uint64_t> replay_dropped{0};
    std::atomic<uint64_t> replay_overflowed{0};
    std::atomic<uint64_t> replay_corrupted{0};
    std::atomic<uint64_t> replay_truncated{0};
    std::atomic<uint64_t> replay_stalled{0};
    /* Reply timing (ADR-0042): the rows a requesting worker publishes at
     * most once a second, read by ecuio_session_get_timing on the
     * caller's thread, both under the mutex */
    mutable std::mutex timing_mutex;
    ecuio_session_timing_t timing{};
    ecuio_request_timing_t timing_rows[ECUIO_PROFILE_MAX_REQUESTS]{};
    std::atomic<int32_t> last_error{ECUIO_OK};
    std::atomic<uint32_t> error_count{0};
    /* Written once by the transport's attach, then the profile by
     * create, before create returns; read-only afterwards, so
     * ecuio_session_get_info needs no lock. */
    ecuio_session_info_t info{};
    /* What each channel's ECU said it supports, what it said it is
     * (ADR-0021) and what it reported of its diagnostics (ADR-0037):
     * the count and each rx_id set by attach, the rest updated by the
     * worker, all under the mutex (ecuio_session_get_channel_support,
     * _identity, _diagnostics). */
    mutable std::mutex support_mutex;
    ecuio_channel_support_t support[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    ecuio_channel_identity_t identity[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    ecuio_channel_diagnostics_t diagnostics[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    uint32_t support_count = 0;
    /* ecuio_session_read_diagnostics's count, from any thread; a worker
     * that reads diagnostics asks again when it moves
     * (ecuio_take_diag_request) */
    std::atomic<uint32_t> diag_requests{0};
    /* ecuio_session_clear_diagnostics's requests, per channel, from any
     * thread: the generation the caller saved, then the count moved on;
     * a worker that reads diagnostics takes each new one
     * (ecuio_take_clear_requests) */
    std::atomic<uint32_t> clear_generation[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    std::atomic<uint32_t> clear_flags[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    std::atomic<uint32_t> clear_requests[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    /* ecuio_session_set_focus's picks (ADR-0056 section 2): the ids in
     * the caller's order and one byte per def of the set (1 picked), both
     * under the mutex; focus_generation moves on with each call, and a
     * requesting worker that sees it move takes the picks
     * (ecuio_take_focus). No ids is no focus. Under the same mutex,
     * what the call judges a focus by: focus_requests, the session's
     * worker asks requests (ecuio_build_requests found some), and
     * focus_askable, one byte per def (1: its request is in the table and
     * not dropped), set by ecuio_build_requests and kept by the worker
     * as the table changes; a requesting session refuses a focus whose
     * picks are none of them askable. */
    std::mutex focus_mutex;
    std::vector<uint16_t> focus_ids;
    std::vector<uint8_t> focus_defs;
    std::atomic<uint32_t> focus_generation{0};
    bool focus_requests = false;
    std::vector<uint8_t> focus_askable;
    /* The URI create was given, for a recording's header */
    std::string uri;
    /* A replay's: its source's frame flags line (ADR-0049 section 2),
     * set by attach, read by a recording's attach */
    std::string source_frame_flags;
    /* The recorder's link (ADR-0049, recorder.h), made by
     * ecuio_session_create and kept until the session is freed, so the
     * worker reads it without a lock; ecuio_session_record puts its
     * recorder in it, before or after start (ADR-0056 section 3) */
    std::shared_ptr<ecuio_record_link> record_link;
    /* The pull queues' links (ecuio/queue.h, pull_queue.h), set by
     * ecuio_queue_open before start and kept until the session is freed,
     * so the worker reads them without a lock; value_queues counts the
     * values queues among them, for which dispatch decodes */
    std::array<std::shared_ptr<ecuio_queue_link>, ECUIO_QUEUE_MAX_PER_SESSION>
        queues;
    uint32_t queue_count = 0;
    uint32_t value_queues = 0;
};

#endif /* ECUIO_SESSION_H */
