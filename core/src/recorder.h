// File/layer: core/src/recorder.h | internal, C++17, Qt-free; what the
//             session and its recorder share (ADR-0049 section 1)
// Testing: HARNESS: RecorderTest (test_core), CoreTest's no-allocation
//          case with a recorder attached
//
// A session and its recorder each outlive the other in some teardowns,
// so neither reaches into the other's memory: both hold a link, and the
// worker reaches the recorder only through it. `active` is the
// recorder while it takes frames; every use of it is pinned (`pins`
// raised before the load and lowered after the use), and detaching
// clears it and waits for `pins` to reach zero, so the recorder is never
// freed under a worker still writing to it. `m` orders the two sides'
// detaches.
#ifndef ECUIO_RECORDER_H
#define ECUIO_RECORDER_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"

#include <atomic>
#include <cstdint>
#include <mutex>

struct ecuio_record_link {
    std::atomic<ecuio_recorder_t*> active{nullptr};
    std::atomic<uint32_t> pins{0};
    /* ecuio_clock_us at attach: host_us counts from it. Written before
     * the session starts, read-only afterwards. */
    uint64_t zero_us = 0;
    std::mutex m;
};

/* The recorder taking frames, pinned; NULL (and unpinned) when none */
inline ecuio_recorder_t* ecuio_record_pin(ecuio_record_link& link)
{
    link.pins.fetch_add(1, std::memory_order_seq_cst);
    ecuio_recorder_t* r = link.active.load(std::memory_order_seq_cst);
    if (r == nullptr) {
        link.pins.fetch_sub(1, std::memory_order_release);
    }
    return r;
}

inline void ecuio_record_unpin(ecuio_record_link& link)
{
    link.pins.fetch_sub(1, std::memory_order_release);
}

/* Either side: the recorder stops taking frames, once no worker holds
 * it. Safe from any thread, any number of times. */
void ecuio_record_detach(ecuio_record_link& link);

/* On the worker, pinned: one frame and the values it decoded to, as one
 * record or none. Never blocks, never allocates. */
void ecuio_recorder_put_frame(ecuio_recorder_t* r, uint64_t host_us,
                              const ecuio_frame_t* frame,
                              const ecuio_value_t* values, uint32_t count);

/* On the worker that judges the link, at each transition: its state and
 * episode count at their time, pinned here; link may be NULL. One that
 * doesn't fit is carried, the latest state, and goes in ahead of the
 * next record that fits, so a full ring loses frames but never the
 * link's state. Never blocks, never allocates. */
void ecuio_record_link_state(ecuio_record_link* link, int32_t state,
                             uint32_t episodes);

/* On a requesting worker, as it publishes reply timing: the rows every
 * 10 s of session time and when final, pinned here; link may be NULL.
 * The final one, the worker's last record, waits up to 1 s for room and
 * is dropped and counted only if none comes. Never allocates. */
void ecuio_record_timing(ecuio_record_link* link,
                         const ecuio_session_timing_t& timing,
                         const ecuio_request_timing_t* rows, uint32_t count,
                         bool final);

#endif /* ECUIO_RECORDER_H */
