// File/layer: core/src/core_dispatch.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest, ConditionSessionTest (test_core)

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "conversation_bind.h"
#include "session.h"
#include "core_callbacks.h"
#include "condition_eval.h"
#include "probes.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <utility>

namespace {
/* ecuio_session_test_after_dispatch_takes's (internal.h) */
std::atomic<void (*)(void*)> g_after_dispatch_takes{nullptr};
std::atomic<void*> g_after_dispatch_takes_user{nullptr};
} // namespace

extern "C" {

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

void ecuio_session_test_after_dispatch_takes(void (*hook)(void* user),
                                             void* user)
{
    /* The user before the hook, and a clear leaves the user: a reader
     * that took the old hook still finds its own */
    if (hook != nullptr) {
        g_after_dispatch_takes_user.store(user);
    }
    g_after_dispatch_takes.store(hook);
}

ecuio_status_t
ecuio_register_conditions_callback(ecuio_session_t* session,
                                   ecuio_conditions_callback_t callback,
                                   void* user_data)
{
    if (session == nullptr || callback == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    std::lock_guard<std::mutex> lock(session->callback_mutex);
    return add_callback(session->conditions_callbacks, callback, user_data);
}

ecuio_status_t
ecuio_unregister_conditions_callback(ecuio_session_t* session,
                                     ecuio_conditions_callback_t callback,
                                     void* user_data)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->callback_mutex);
    remove_callback(session->conditions_callbacks, callback, user_data);
    return ECUIO_OK;
}

namespace {

/* The worker's: a set attached since the last frame or link change, taken
 * under the lock it takes anyway; true when one was */
bool take_conditions(ecuio_session_t* session)
{
    if (!session->conditions_swap) {
        return false;
    }
    session->conditions_retired.swap(session->conditions);
    session->conditions.swap(session->conditions_pending);
    session->conditions_swap = false;
    session->conditions_waiting.store(false);
    return true;
}

/* The worker's, last: what a frame or a link change changed among the
 * conditions, into the states for the attach they still belong to, then
 * the callbacks and queues */
void deliver_condition_events(ecuio_session_t* session,
                              const ecuio_condition_eval& conditions,
                              uint32_t changed)
{
    if (changed == 0) {
        return;
    }
    const ecuio_condition_event_t* events = conditions.events.data();
    {
        std::lock_guard<std::mutex> lock(session->condition_states_mutex);
        if (session->condition_states_generation == conditions.generation) {
            for (uint32_t i = 0; i < changed; ++i) {
                session->condition_states[events[i].condition] =
                    conditions.state_of(events[i].condition);
            }
        }
    }
    /* Their list is taken only by a call that changed something, so a
     * frame with no event pays nothing for it */
    snapshot_of<ecuio_conditions_callback_t> changes;
    bool changes_taken = false;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        changes_taken = changes.take(session->conditions_callbacks);
    }
    if (changes_taken) {
        for (size_t i = 0; i < changes.size(); ++i) {
            changes[i].first(events, changed, changes[i].second);
        }
    }
    for (uint32_t i = 0; i < session->queue_count; ++i) {
        ecuio_queue_link& q = *session->queues[i];
        if (q.kind == ECUIO_QUEUE_CONDITIONS) {
            if (ecuio_queue_t* queue = ecuio_queue_pin(q)) {
                ecuio_queue_put_conditions(queue, events, changed);
                ecuio_queue_unpin(q);
            }
        }
    }
}

} // namespace

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
                       ? ecuio_record_clock_us()
                       : 0;

    /* The lists as they stand when the frame arrives, and the set to
     * judge it by: one attached since the last frame is taken here */
    snapshot_of<ecuio_frame_callback_t> frames;
    snapshot_of<ecuio_values_callback_t> values;
    bool frames_taken = false;
    bool values_taken = false;
    ecuio_condition_eval* conditions = nullptr;
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        frames_taken = frames.take(session->callbacks);
        values_taken = values.take(session->values_callbacks);
        took = take_conditions(session);
        conditions = session->conditions.get();
    }
    /* A set taken starts from the link's state: this worker published it */
    if (took && conditions != nullptr) {
        conditions->set_link(session->last_error.load());
    }
    if (const auto hook = g_after_dispatch_takes.load()) {
        hook(g_after_dispatch_takes_user.load());
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

    /* Then the frame's values (ADR-0024), decoded for a values consumer,
     * a recording (frame and values one record) or the conditions */
    const bool want_values =
        (values_taken && values.size() > 0) || session->value_queues > 0;
    ecuio_recorder_t* recorder = ecuio_record_pin(*link);
    if (!want_values && recorder == nullptr && conditions == nullptr) {
        return;
    }
    const uint32_t count = session->decoder.decode(frame);
    /* A recording begun after the set judged frames starts from the states
     * they left: taken before this frame moves them */
    if (recorder != nullptr && conditions != nullptr) {
        ecuio_recorder_hold_states(recorder, *conditions);
    }
    const uint32_t changed =
        conditions != nullptr
            ? conditions->judge(frame->timestamp_us,
                                session->decoder.values.data(), count)
            : 0;
    if (recorder != nullptr) {
        now = now != 0 ? now : ecuio_record_clock_us(); /* attached since */
        const uint64_t zero = link->zero_us.load(std::memory_order_relaxed);
        const uint64_t host_us = now > zero ? now - zero : 0;
        ecuio_recorder_put_frame(
            recorder, host_us, frame, session->decoder.values.data(), count,
            changed > 0 ? conditions->events.data() : nullptr, changed,
            conditions != nullptr ? conditions->generation : 0,
            ecuio_session_cuts_access(session, frame));
        ecuio_record_unpin(*link);
    }
    if (want_values && count > 0) {
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

    /* A signal's first sample since the attach: the terms sampled, for
     * the states' getter, while the attach is still the newest */
    if (conditions != nullptr &&
        conditions->first_samples != conditions->first_samples_published) {
        conditions->first_samples_published = conditions->first_samples;
        std::lock_guard<std::mutex> lock(session->condition_states_mutex);
        if (session->condition_states_generation == conditions->generation) {
            std::vector<ecuio_session::condition_read>& reads =
                session->condition_reads;
            for (uint32_t i = 0;
                 i < reads.size() && i < conditions->conditions.size(); ++i) {
                reads[i].sampled = conditions->sampled_terms(i);
            }
        }
    }
    /* Last, what the frame changed among the conditions */
    if (conditions != nullptr) {
        deliver_condition_events(session, *conditions, changed);
    }
}

int ecuio_session_conditions_waiting(const ecuio_session_t* session)
{
    return session != nullptr &&
                   session->conditions_waiting.load(std::memory_order_relaxed)
               ? 1
               : 0;
}

void ecuio_session_dispatch_link(ecuio_session_t* session, int32_t state,
                                 uint64_t timestamp_us)
{
    if (session == nullptr || session->closed.load()) {
        return;
    }
    ecuio_condition_eval* conditions = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        (void)take_conditions(session);
        conditions = session->conditions.get();
    }
    if (conditions == nullptr || conditions->link_terms.empty()) {
        return;
    }
    /* The link line went in first (link_set_state), then its events: a
     * recording holds what the conditions did after what the link did */
    ecuio_record_link* link = session->record_link.get();
    ecuio_recorder_t* recorder = ecuio_record_pin(*link);
    if (recorder != nullptr) {
        ecuio_recorder_hold_states(recorder, *conditions);
    }
    const uint32_t changed = conditions->judge_link(timestamp_us, state);
    if (recorder != nullptr) {
        const uint64_t now = ecuio_record_clock_us();
        const uint64_t zero = link->zero_us.load(std::memory_order_relaxed);
        ecuio_recorder_put_events(
            recorder, now > zero ? now - zero : 0, conditions->newest_us,
            conditions->events.data(), changed, conditions->generation);
        ecuio_record_unpin(*link);
    }
    deliver_condition_events(session, *conditions, changed);
}

} // extern "C"
