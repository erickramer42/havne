// File/layer: core/src/recorder_control.cpp | C++17, Qt-free; what a caller
//             does to a recorder in flight (ADR-0049 sections 1 to 4)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// The focus, the caller's notes and marks, the state and folder getters and
// the close, with the test seams that reach into the recorder. The worker's
// puts are in recorder.cpp, the attach in recorder_attach.cpp.

#include "recorder.h"

#include "condition_eval.h"
#include "error_detail.h"
#include "recorder_impl.h"
#include "recorder_text.h"
#include "recording_lines.h" /* signal_line */
#include "recording_keys.h"  /* a set's keys */
#include "recording_rows.h"  /* kFormat4, the first line */
#include "session.h"
#include "sha256.h"
#include "signal_file_build.h" /* a loaded file's bytes */

#include "ecuio/internal.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ecuio_rec {

/* The session's focus; the caller holds focus_mutex */
focus_named focus_now(const ecuio_session_t* session)
{
    focus_named f;
    f.ids = session->focus_ids;
    f.pick = (session->focus_flags & ECUIO_FOCUS_DEFAULT_PICK) != 0;
    /* Only a session that asks has one: the others take it and change
     * nothing */
    f.min_sweep_ms = session->focus_requests ? session->focus_min_sweep_ms : 0;
    return f;
}

} // namespace ecuio_rec

namespace {

/* The focus's activity lines into the pinned recorder's note queue: the
 * count, the minimum when there is one, then the keys */
void push_focus(ecuio_recorder_t* r, const focus_named& f)
{
    size_t named = 0;
    const std::vector<std::string> parts = focus_parts(r->defs, f.ids, &named);
    std::string lines;
    if (parts.empty()) {
        lines = "none: every signal asked";
    }
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k == 0) {
            lines +=
                std::to_string(named) + (named == 1 ? " signal" : " signals");
            if (f.min_sweep_ms != 0) {
                lines += ", at least " + std::to_string(f.min_sweep_ms) +
                         " ms between sweeps";
            }
            lines += ": ";
        } else {
            lines += std::string(1, '\0') + "more: ";
        }
        lines += parts[k];
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
    std::lock_guard<std::mutex> g(r->note_m);
    if (r->items.size() >= r->note_cap) {
        r->notes_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        r->items.push_back(item{item_kind::focus, host_us,
                                f.pick ? "pick" : "focus", std::move(lines),
                                f});
    }
}

} // namespace

void ecuio_record_focus(ecuio_session_t* session)
{
    ecuio_record_link& link = *session->record_link;
    ecuio_recorder_t* r = nullptr;
    {
        std::lock_guard<std::mutex> g(link.m);
        r = ecuio_record_pin(link);
    }
    if (r == nullptr) {
        return;
    }
    try {
        focus_named f;
        bool changed = false;
        {
            std::lock_guard<std::mutex> g(session->focus_mutex);
            f = focus_now(session);
            changed = !(f == r->named_focus);
            if (changed) {
                r->named_focus = f;
            }
        }
        if (changed) {
            push_focus(r, f);
        }
    } catch (...) {
        /* The focus stands; its line is lost, counted as a note */
        r->notes_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ecuio_record_unpin(link);
}

extern "C" {

ecuio_status_t ecuio_recorder_note(ecuio_recorder_t* recorder,
                                   const char* utf8_line)
{
    if (recorder == nullptr || utf8_line == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!recorder->attached.load(std::memory_order_acquire)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->failed()) {
        return ECUIO_ERR_RECORDING_FAILED;
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us =
        now > recorder->zero_us ? now - recorder->zero_us : 0;
    try {
        std::string line = one_line(utf8_line);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::note, host_us, std::move(line), {}, {}});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_mark(ecuio_recorder_t* recorder,
                                   const char* utf8_label)
{
    if (recorder == nullptr || utf8_label == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!recorder->attached.load(std::memory_order_acquire)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->failed()) {
        return ECUIO_ERR_RECORDING_FAILED;
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us =
        now > recorder->zero_us ? now - recorder->zero_us : 0;
    try {
        std::string label = one_line(utf8_label);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::mark, host_us, std::move(label), {}, {}});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_get_state(const ecuio_recorder_t* recorder,
                                        ecuio_recorder_state_t* out)
{
    if (recorder == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::memset(out, 0, sizeof *out);
    if (recorder->drained.load(std::memory_order_acquire)) {
        out->flags |= ECUIO_RECORDER_DRAINED;
    }
    const uint32_t state = recorder->state.load(std::memory_order_acquire);
    out->state = state == ECUIO_RECORDING_FAILED ? ECUIO_RECORDING_FAILED
                 : recorder->detached.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_DETACHED
                 : recorder->armed &&
                         !recorder->window_open.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_ARMED
                     : ECUIO_RECORDING_ACTIVE;
    out->frame_rows = recorder->frame_rows.load(std::memory_order_relaxed);
    out->value_rows = recorder->value_rows.load(std::memory_order_relaxed);
    out->records_dropped =
        recorder->records_dropped.load(std::memory_order_relaxed);
    out->notes_dropped =
        recorder->notes_dropped.load(std::memory_order_relaxed);
    out->conditions_lost =
        recorder->conditions_lost.load(std::memory_order_relaxed);
    out->windows = recorder->windows_opened.load(std::memory_order_acquire);
    out->preroll_short =
        recorder->prerolls_short.load(std::memory_order_relaxed);
    out->frames_passed =
        recorder->frames_passed.load(std::memory_order_relaxed);
    if (state == ECUIO_RECORDING_FAILED) {
        std::lock_guard<std::mutex> g(recorder->reason_m);
        const size_t n =
            (std::min)(recorder->reason.size(), sizeof out->reason - 1);
        std::memcpy(out->reason, recorder->reason.data(), n);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_get_folder(const ecuio_recorder_t* recorder,
                                         char* utf8, uint32_t capacity,
                                         uint32_t* out_len)
{
    if (recorder == nullptr || out_len == nullptr ||
        (utf8 == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> g(recorder->folder_m);
    const std::string& f = recorder->published;
    *out_len = static_cast<uint32_t>(f.size());
    if (capacity > 0) {
        const size_t n =
            (std::min)(f.size(), static_cast<size_t>(capacity) - 1);
        std::memcpy(utf8, f.data(), n);
        utf8[n] = '\0';
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_close(ecuio_recorder_t* recorder,
                                    const char* utf8_why)
{
    if (recorder == nullptr) {
        return ECUIO_OK;
    }
    std::unique_ptr<ecuio_recorder> r(recorder);
    if (r->link != nullptr) {
        ecuio_record_detach(*r->link, r.get());
    }
    {
        std::lock_guard<std::mutex> g(r->wake_m);
        r->stop = true;
    }
    r->wake_cv.notify_all();
    if (r->writer.joinable()) {
        r->writer.join();
    }
    /* The writer has drained everything; the summary is the last line */
    if (r->attached.load() && r->armed) {
        /* The window still open, if one is: what the worker carried, then
         * its end, at the close while a record condition is ON */
        if (r->in_window && !r->windows_q.empty()) {
            const ecuio_recorder::window w = r->windows_q.front();
            const uint64_t now = ecuio_record_clock_us();
            const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
            /* A window whose conditions cleared ends at its last record,
             * one still ON at the close; the carry goes in up to its end */
            uint64_t end_host = w.open_end ? host_us : r->win_last_host;
            for (uint32_t k = 0; k < r->carry_count; ++k) {
                const event_body& e = r->carry[r->carry_start + k];
                if (w.open_end || e.t_us <= w.end_us) {
                    r->take_event(e);
                    end_host = (std::max)(end_host, e.host_us);
                }
            }
            if (w.open_end) {
                if (r->carry_lost > 0) {
                    r->lost_line(r->carry_lost_us, r->carry_lost);
                }
                if (r->link_pending) {
                    r->event_line(r->pending_link.host_us, "link",
                                  link_text(r->pending_link.state,
                                            r->pending_link.episodes));
                }
                if (r->pending_dropped > 0) {
                    r->drop_line(end_host, r->pending_dropped);
                    r->win_dropped += r->pending_dropped;
                }
            }
            r->close_window(
                end_host,
                w.open_end ? one_line(utf8_why != nullptr ? utf8_why : "closed")
                           : std::string(kWindowEnded));
        }
    } else if (r->attached.load()) {
        std::string why = one_line(utf8_why != nullptr ? utf8_why : "closed");
        const uint64_t now = ecuio_record_clock_us();
        const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
        /* What the worker couldn't queue before it let go: the detach
         * waited for it, so its fields are this thread's now */
        for (uint32_t k = 0; k < r->carry_count; ++k) {
            r->take_event(r->carry[r->carry_start + k]);
        }
        if (r->carry_lost > 0) {
            r->lost_line(r->carry_lost_us, r->carry_lost);
        }
        if (r->link_pending) {
            r->event_line(
                r->pending_link.host_us, "link",
                link_text(r->pending_link.state, r->pending_link.episodes));
        }
        if (r->pending_dropped > 0) {
            r->drop_line(host_us, r->pending_dropped);
        }
        r->event_out += '\n';
        r->event_line(
            host_us, "end",
            why + ": " + std::to_string(r->frame_rows.load()) + " frames, " +
                std::to_string(r->value_rows.load()) + " values, " +
                std::to_string(r->records_dropped.load()) +
                " records dropped, " + std::to_string(r->notes_dropped.load()) +
                " notes dropped");
        r->write(r->events, r->event_out);
        r->write(r->markers, r->marker_out);
    }
    r->sync_all();
    r->close_files();
    return r->failed() ? ECUIO_ERR_RECORDING_FAILED : ECUIO_OK;
}

void ecuio_recorder_test_hold(ecuio_recorder_t* recorder, int hold)
{
    if (recorder != nullptr) {
        recorder->hold.store(hold != 0, std::memory_order_release);
    }
}

void ecuio_recorder_test_fail_writes(ecuio_recorder_t* recorder)
{
    if (recorder != nullptr) {
        recorder->fail_next.store(true, std::memory_order_release);
    }
}

void ecuio_recorder_test_add_slept(ecuio_recorder_t* recorder, uint64_t us)
{
    if (recorder != nullptr) {
        recorder->slept_added.fetch_add(us, std::memory_order_relaxed);
    }
}

uint64_t ecuio_recorder_test_wakes(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->wakes.load(std::memory_order_relaxed)
                               : 0;
}

uint64_t ecuio_recorder_test_syncs(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->syncs.load(std::memory_order_relaxed)
                               : 0;
}

} // extern "C"
