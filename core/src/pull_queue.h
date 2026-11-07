// File/layer: core/src/pull_queue.h | C++17, Qt-free, internal
// Testing: UNIT: QueueTest (test_core)
//
// The pull queue's side in core (ecuio/queue.h, ADR-0053's appendix "the
// pull queue as built"): the link a session and one queue share, as the
// recorder's (recorder.h). The session's caller sets it before start;
// the worker pins it for each frame, so a queue closing detaches it and
// waits for the pin before it's freed, and the session's close detaches
// what's left once the worker has stopped.

#ifndef ECUIO_PULL_QUEUE_H
#define ECUIO_PULL_QUEUE_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/queue.h"

#include <atomic>
#include <cstdint>
#include <mutex>

struct ecuio_queue_link {
    std::atomic<ecuio_queue_t*> active{nullptr};
    std::atomic<uint32_t> pins{0};
    /* ECUIO_QUEUE_FRAMES or _VALUES; set at open, read-only afterwards */
    uint32_t kind = 0;
    /* Held by either side's detach, so the session's close and the
     * queue's own close never both reach the queue */
    std::mutex m;
};

/* The queue taking entries, pinned; NULL (and unpinned) when none */
inline ecuio_queue_t* ecuio_queue_pin(ecuio_queue_link& link)
{
    link.pins.fetch_add(1, std::memory_order_seq_cst);
    ecuio_queue_t* q = link.active.load(std::memory_order_seq_cst);
    if (q == nullptr) {
        link.pins.fetch_sub(1, std::memory_order_release);
    }
    return q;
}

inline void ecuio_queue_unpin(ecuio_queue_link& link)
{
    link.pins.fetch_sub(1, std::memory_order_release);
}

/* The worker's copies, on a pinned queue: no allocation, the queue's
 * mutex held for the copy alone */
void ecuio_queue_put_frame(ecuio_queue_t* queue, const ecuio_frame_t* frame);
void ecuio_queue_put_values(ecuio_queue_t* queue,
                            const ecuio_value_batch_t* batch);

/* The session's close, after the worker has stopped: the queue takes no
 * more entries, and its readers are woken to read what's left */
void ecuio_queue_detach(ecuio_queue_link& link);

#endif /* ECUIO_PULL_QUEUE_H */
