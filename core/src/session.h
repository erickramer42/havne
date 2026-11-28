// File/layer: core/src/session.h | internal, C++17, Qt-free; the session
//             struct behind the opaque ecuio_session_t
// Testing: UNIT: CoreTest, PickOnlyTest, RecorderTest, ConditionSessionTest,
//          ConditionRecordingTest (test_core)

#ifndef ECUIO_SESSION_H
#define ECUIO_SESSION_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "conversation_registry.h"
#include "security_store_owner.h"
#include "frame_decoder.h"
#include "host_awake.h"
#include "profile_hold.h"
#include "pull_queue.h"
#include "recorder.h"
#include "request_talk.h"
#include "uds_generic.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "condition_eval.h"

/* The transports' own cases' talk (ecuio_session_test_talk, internal.h;
 * talk_seam.h has the rules): one request on its way to the requesting
 * worker, then its result on the way back, handed over on state alone */
constexpr uint8_t ECUIO_TEST_TALK_IDLE = 0;
constexpr uint8_t ECUIO_TEST_TALK_ASKED = 1;   /* filled: the worker's */
constexpr uint8_t ECUIO_TEST_TALK_OUT = 2;     /* with the worker's engine */
constexpr uint8_t ECUIO_TEST_TALK_SETTLED = 3; /* its result: the caller's */

struct ecuio_test_talk_slot {
    std::atomic<uint8_t> state{ECUIO_TEST_TALK_IDLE};
    uint32_t channel = 0;
    uint32_t len = 0;
    uint8_t echo = 0;
    ecuio_talk_kind kind = ecuio_talk_kind::plain;
    uint32_t window_ms = 0; /* a seed's */
    uint8_t pdu[ECUIO_ENGINE_PDU_MAX] = {};
    ecuio_talk_offer offer = ecuio_talk_offer::taken;
    ecuio_talk_result result;
};

/* The worker's alone: a run asks (want moves on), the transport serves
 * between requests (served catches up). source: attach found a way. */
struct ecuio_battery_slot {
    uint32_t want = 0;
    uint32_t served = 0;
    uint32_t mv = 0;
    bool source = false;
    bool got = false;
};

/* The worker's alone: a run adds, the transport writes between requests */
struct ecuio_run_lines {
    static constexpr uint32_t kMost = 4;
    uint32_t count = 0;
    char text[kMost][127] = {};
};

struct ecuio_session_talk; /* conversation_bind.h */
void ecuio_session_talk_free(ecuio_session_talk* t) noexcept;

namespace ecuio_operation {
struct session_run; /* operation_session.h */
}
void ecuio_operation_free(ecuio_operation::session_run* o) noexcept;

struct ecuio_session {
    /* The registry's set, held from create (profile_hold.h); first, so
     * it's released after every member that reads the set */
    ecuio_profile_hold profile_hold;
    /* The set's plugin's conversation part (conversation_registry.h),
     * taken with the hold; NULL for none */
    ecuio_conversation_part_ref conversation;
    /* The part bound at start (conversation_bind.h): its instances freed
     * at close once the worker joined, its reports at destroy; NULL when
     * nothing is bound */
    std::unique_ptr<ecuio_session_talk, void (*)(ecuio_session_talk*)> talk{
        nullptr, ecuio_session_talk_free};
    /* A run of one of the plugin's operations (operation_session.h): made
     * at the first start under conversation_mutex and kept until destroy.
     * operation_live is the same object for the worker, set once. */
    std::unique_ptr<ecuio_operation::session_run,
                    void (*)(ecuio_operation::session_run*)>
        operation{nullptr, ecuio_operation_free};
    std::atomic<ecuio_operation::session_run*> operation_live{nullptr};
    /* Held across each conversation call (the unlock, the getters, the
     * bind's hand-over and close's release); never by a worker */
    mutable std::mutex conversation_mutex;
    /* The security store a bind hands the conversations, set before
     * start; empty for none */
    ecuio_security::store_ref conversation_store;
    /* The store's fallback name for the car (ecuio_session_security_store),
     * used when the module gives no serial and the car no VIN */
    std::string security_label;
    /* A consent applied before start (ecuio_session_apply_consent): the
     * record's fingerprint and second, the caller's alone until start */
    uint8_t consent_fingerprint[32] = {};
    uint64_t consent_given_utc = 0;
    std::atomic<bool> closed{false};
    /* The worker was started: a start after it is the transport's no-op,
     * and the session_start probe marks the first only (ADR-0034 section 4).
     * The session's one caller alone reads and writes it. */
    bool started = false;
    /* The consent create found (consent_gate.h), or apply's; read at the
     * bind */
    bool conversation_consent = false;
    bool consent_applied = false; /* the fingerprint and second above hold */
    /* The link is one no car is on (SocketCAN's vcan), set by attach */
    bool no_car_bus = false;
    /* Each declared module's parts a consent can name (create's), and a
     * bit for each channel whose declared session a consent allows */
    struct declared_print {
        uint32_t rx = 0;
        uint8_t fingerprint[32] = {};
    };
    std::vector<declared_print> declared_prints;
    uint32_t declared_allowed = 0;
    /* started's copy for any thread: the conditions' states judge what the
     * session gets only once it runs */
    std::atomic<bool> ran{false};
    std::mutex callback_mutex;
    std::vector<std::pair<ecuio_frame_callback_t, void*>> callbacks;
    /* Under the same mutex: the values callbacks (ADR-0024) */
    std::vector<std::pair<ecuio_values_callback_t, void*>> values_callbacks;
    /* Under the same mutex: the conditions callbacks, and the conditions
     * attached (ADR-0063 section 3, condition_eval.h) in three slots.
     * `conditions` is the worker's: only dispatch replaces it, inside the
     * lock it takes for each frame, and reads it outside. The caller
     * (ecuio_session_set_conditions) puts the next set in
     * conditions_pending and sets conditions_swap; dispatch then moves
     * its own into conditions_retired and takes the pending one. The
     * caller frees a retired set and a pending one the worker never took
     * at its next call, so the worker frees nothing and the caller never
     * frees one the worker holds. */
    std::vector<std::pair<ecuio_conditions_callback_t, void*>>
        conditions_callbacks;
    std::unique_ptr<ecuio_condition_eval> conditions;
    std::unique_ptr<ecuio_condition_eval> conditions_pending;
    std::unique_ptr<ecuio_condition_eval> conditions_retired;
    bool conditions_swap = false;
    /* conditions_swap's copy for a worker's look outside the lock (set
     * with it, cleared by the take): ecuio_session_conditions_waiting */
    std::atomic<bool> conditions_waiting{false};
    /* Under the same mutex: the attaches so far, and what a recording
     * keeps of the newest attach's set (recorder.h; NULL with none) */
    uint32_t conditions_generation = 0;
    std::shared_ptr<const ecuio_condition_record> conditions_record;
    /* The states of the newest attach (ecuio_session_get_condition_states),
     * under their mutex: set whole by the attach before the worker can
     * take its set (their mutex inside callback_mutex, never the other
     * way), then each one by the worker when it changes, which a set
     * since replaced no longer does */
    mutable std::mutex condition_states_mutex;
    std::vector<ecuio_condition_state_t> condition_states;
    uint32_t condition_states_generation = 0;
    /* Set with the states: each condition's signal terms' def ids and its
     * join, for the getter's ECUIO_CONDITION_WHY_NOT_RECEIVED (count 0 for
     * one the bind left unavailable), and its terms sampled since the
     * attach (a bit each), which the worker sets at a signal's first
     * sample (ecuio_condition_eval::sampled_terms) */
    struct condition_read {
        uint16_t ids[ECUIO_CONDITION_MAX_TERMS] = {};
        uint8_t count = 0;
        uint8_t any = 0;
        uint8_t sampled = 0; /* a bit per term */
    };
    static_assert(ECUIO_CONDITION_MAX_TERMS <= 8,
                  "condition_read's sampled holds a bit per term");
    std::vector<condition_read> condition_reads;
    ecuio_transport_t* transport = nullptr;
    /* The profile's set: the registry's static one, or, under
     * uds-generic, the session's own below (ADR-0020). */
    const ecuio_signal_set_t* active_signal_set = nullptr;
    /* uds-generic's set, built by create from the URI's ?did= entries and
     * owned here; the J2534 transport reads its scan list at attach. NULL
     * under any other profile. */
    std::unique_ptr<ecuio_uds_session> uds;
    /* The decode (frame_decoder.h, ADR-0024 section 4), prepared at create
     * from the set, which never changes; only dispatch decodes (the
     * worker), the absent and unsplit counts are read on the caller's
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
    /* Set by the attach of a transport whose worker judges its link
     * (link_health.h: J2534, ELM327, SocketCAN), before create returns:
     * the conditions' link terms bind only then */
    bool judges_link = false;
    /* Written once by the transport's attach, then the profile by
     * create, before create returns; read-only afterwards, so
     * ecuio_session_get_info needs no lock. */
    ecuio_session_info_t info{};
    /* The session's modules, every one (the info lists the first
     * ECUIO_SESSION_INFO_MAX_CHANNELS): set by attach through
     * ecuio_session_set_modules before create returns, read-only
     * afterwards (ecuio_session_get_module) */
    std::vector<uint32_t> module_rx;
    std::vector<uint32_t> module_tx;
    /* Each module's support, identity (ADR-0021) and diagnostics
     * (ADR-0037), sized with the modules: the count and rx_ids set by
     * attach, the rest by the worker, all under the mutex
     * (ecuio_session_get_channel_support, _identity, _diagnostics). */
    mutable std::mutex support_mutex;
    std::vector<ecuio_channel_support_t> support;
    std::vector<ecuio_channel_identity_t> identity;
    std::vector<ecuio_channel_diagnostics_t> diagnostics;
    uint32_t support_count = 0;
    /* ecuio_session_read_diagnostics's count, from any thread; a worker
     * that reads diagnostics asks again when it moves
     * (ecuio_take_diag_request) */
    std::atomic<uint32_t> diag_requests{0};
    /* ecuio_session_clear_diagnostics_set's request, from any thread,
     * under clear_mutex: its targets and flags, the requests made, and
     * the last one the worker has seen to its end
     * (ecuio_take_clear_requests); a request runs while the two differ */
    std::mutex clear_mutex;
    ecuio_clear_target_t clear_targets[ECUIO_MAX_CHANNELS] = {};
    uint32_t clear_count = 0;
    uint32_t clear_flags = 0;
    uint32_t clear_requests = 0;
    uint32_t clear_ended = 0;
    ecuio_battery_slot battery;
    ecuio_run_lines run_lines;
    /* ecuio_session_set_focus's picks (ADR-0056 section 2): the ids in the
     * caller's order and one byte per def (1 picked), under the mutex;
     * focus_generation moves with each call and a requesting worker takes
     * the picks (ecuio_take_focus). No ids is no focus. */
    /* Also under the mutex: focus_requests (the worker asks requests) and
     * focus_askable (one byte per def: its request is in the table, not
     * dropped), which refuse a focus with no askable pick;
     * focus_min_sweep_ms is the minimum time between sweeps (section 4), 0
     * for none; condition_defs (one byte per def, empty with no conditions)
     * the attached conditions' signals, a computed one's inputs among them,
     * which any focus keeps asked (ADR-0063 section 7). */
    mutable std::mutex focus_mutex;
    std::vector<uint16_t> focus_ids;
    std::vector<uint8_t> focus_defs;
    std::vector<uint8_t> condition_defs;
    uint32_t focus_min_sweep_ms = 0;
    /* ecuio_session_set_focus_with's flags (ECUIO_FOCUS_DEFAULT_PICK), for
     * a recording's header word */
    uint32_t focus_flags = 0;
    std::atomic<uint32_t> focus_generation{0};
    bool focus_requests = false;
    std::vector<uint8_t> focus_askable;
    /* Set with focus_askable by attach (ecuio_build_requests): each def's
     * row in the worker's table (ECUIO_ROW_NONE: none), which the worker
     * adds to as a scanned DID joins; whether the session asks only its
     * picks (ADR-0065 section 2: the set derives more requests than a
     * rotation holds) and that table's rows, which never grow */
    std::vector<uint32_t> focus_rows;
    bool focus_pick_only = false;
    uint32_t focus_table = 0;
    /* One byte per def, sized with focus_askable: 1 for a def the focus the
     * worker took leaves unasked (ecuio_focus_unasked), under the mutex */
    std::vector<uint8_t> focus_unasked;
    /* The modules the picks and the attached conditions read (ADR-0065
     * step 5's READ, ecuio_modules_wanted), worked out with them under
     * the mutex before focus_generation moves: one bit per module, and
     * those modules in the order of their first pick */
    uint32_t focus_modules = 0;
    uint8_t focus_module_order[ECUIO_MAX_CHANNELS] = {};
    uint32_t focus_module_count = 0;
    /* The modules that answered the start's emissions probe (RANGE,
     * request_probe.h), a bit each; every module when no probe ran (the
     * mock, ?poll=). Set by attach, read-only afterwards. */
    uint32_t module_range = UINT32_MAX;
    /* The modules closed and those with no room, a bit each, as the
     * requesting worker's last take left them (ecuio_take_modules); a
     * recording's attach reads them for its header. None for a transport
     * that sends no request. */
    std::atomic<uint32_t> modules_closed{0};
    std::atomic<uint32_t> modules_no_room{0};
    /* The URI create was given, for a recording's header */
    std::string uri;
    /* A replay's: its source's frame flags line (ADR-0049 section 2),
     * set by attach, read by a recording's attach */
    std::string source_frame_flags;
    /* The recorder's link (ADR-0049, recorder.h): made at create and kept
     * until the session is freed, so the worker reads it without a lock;
     * ecuio_session_record puts its recorder in it, before or after start
     * (ADR-0056 section 3). Its mutex comes before callback_mutex:
     * ecuio_session_set_conditions takes both, so a set never changes
     * under an attaching recorder */
    std::shared_ptr<ecuio_record_link> record_link;
    /* The pull queues' links (queue.h, pull_queue.h), set before start and
     * kept until the session is freed (lock-free reads); value_queues
     * counts the values queues, for which dispatch decodes */
    std::array<std::shared_ptr<ecuio_queue_link>, ECUIO_QUEUE_MAX_PER_SESSION>
        queues;
    uint32_t queue_count = 0;
    uint32_t value_queues = 0;
    ecuio_test_talk_slot test_talk;
    /* The host held awake (host_awake.h): the caller's claim and core's
     * own, released at close after the detach */
    ecuio_awake_state awake;
};

#endif /* ECUIO_SESSION_H */
