// File/layer: core/src/queue.cpp | C++17, Qt-free
// Testing: UNIT: QueueTest (test_core); c_abi, c_abi_shared
//
// ecuio/queue.h (ADR-0053 section 4, its appendix "the pull queue as
// built"): a ring of fixed slots made at open, so the worker's copy never
// allocates. The worker copies under the queue's mutex and notifies only
// when a reader waits; a reader copies out in chunks, the mutex let go
// between them, so a large read never holds the worker up for long. A
// full ring drops its oldest entry.

#include "ecuio/queue.h"
#include "ecuio/internal.h" /* ecuio_queue_test_waiting */

#include "pull_queue.h"
#include "session.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

namespace {

/* Entries a reader copies out per hold of the mutex: about 130 KB of
 * frames, 40 KB of values */
constexpr uint32_t kFrameChunk = 32;
constexpr uint32_t kValueChunk = 256;

} // namespace

struct ecuio_queue {
    uint32_t kind = 0;
    uint32_t capacity = 0;
    std::shared_ptr<ecuio_queue_link> link;
    std::unique_ptr<ecuio_frame_t[]> frames;
    std::unique_ptr<ecuio_queue_value_t[]> values;

    std::mutex m;
    std::condition_variable cv;
    uint32_t head = 0; /* the oldest entry's slot */
    uint32_t size = 0; /* entries waiting */
    uint64_t dropped = 0;
    uint32_t waiting = 0; /* readers waiting for an entry */
    uint32_t readers = 0; /* reads under way */
    bool ended = false;   /* the session has closed */
    bool closing = false; /* ecuio_queue_close has begun */

    /* The slot the next entry goes in, the oldest dropped when full */
    uint32_t take_slot()
    {
        if (size == capacity) {
            head = (head + 1) % capacity;
            --size;
            ++dropped;
        }
        return (head + size++) % capacity;
    }

    void wake_readers(std::unique_lock<std::mutex>& lock)
    {
        const bool wake = waiting > 0;
        lock.unlock();
        if (wake) {
            cv.notify_all();
        }
    }
};

namespace {

void copy_frame(ecuio_frame_t& to, const ecuio_frame_t& from)
{
    const uint32_t length =
        (std::min)(uint32_t{from.data_length}, ECUIO_FRAME_DATA_MAX);
    to.timestamp_us = from.timestamp_us;
    to.arbitration_id = from.arbitration_id;
    to.channel_id = from.channel_id;
    to.data_length = static_cast<uint16_t>(length);
    std::memcpy(to.data, from.data, length);
    to.flags = from.flags;
    to.reserved = from.reserved;
}

void copy_value(ecuio_queue_value_t& to, const ecuio_value_batch_t& batch,
                const ecuio_value_t& v)
{
    to.timestamp_us = batch.timestamp_us;
    to.arbitration_id = batch.arbitration_id;
    to.signal_id = v.signal_id;
    to.kind = v.kind;
    to.flags = 0;
    to.value = v.value;
    to.reserved = 0;
    uint32_t length = 0;
    if (v.text != nullptr) {
        length = v.text_len;
        if (length > ECUIO_QUEUE_TEXT_MAX - 1) {
            length = ECUIO_QUEUE_TEXT_MAX - 1;
            to.flags |= ECUIO_QUEUE_VALUE_TEXT_CUT;
        }
        std::memcpy(to.text, v.text, length);
    }
    to.text_len = length;
    /* The NUL and past it zero, so no older entry's text shows */
    std::memset(to.text + length, 0, ECUIO_QUEUE_TEXT_MAX - length);
}

/* One read, either kind: waits for the first entry, then copies out in
 * chunks through `copy(slot, out index)` */
template <typename Copy>
ecuio_status_t read_queue(ecuio_queue_t* q, uint32_t capacity, uint32_t chunk,
                          uint32_t* count, uint64_t* dropped,
                          uint32_t timeout_ms, Copy copy)
{
    *count = 0;
    std::unique_lock<std::mutex> lock(q->m);
    ++q->readers;
    if (q->size == 0 && timeout_ms > 0 && !q->ended && !q->closing) {
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(timeout_ms);
        ++q->waiting;
        q->cv.wait_until(lock, until,
                         [q] { return q->size > 0 || q->ended || q->closing; });
        --q->waiting;
    }
    uint32_t taken = 0;
    while (taken < capacity && q->size > 0 && !q->closing) {
        const uint32_t n = (std::min)({chunk, capacity - taken, q->size});
        for (uint32_t i = 0; i < n; ++i) {
            copy(q->head, taken + i);
            q->head = (q->head + 1) % q->capacity;
            --q->size;
        }
        taken += n;
        if (taken < capacity && q->size > 0) {
            /* Let the worker in between chunks */
            lock.unlock();
            lock.lock();
        }
    }
    *count = taken;
    *dropped = q->dropped;
    const bool over = taken == 0 && (q->ended || q->closing);
    --q->readers;
    /* The last read out of a closing queue wakes the close, holding the
     * mutex: the close can't return and free the queue until the lock's
     * release below, the read's last touch of it */
    if (q->closing && q->readers == 0) {
        q->cv.notify_all();
    }
    return over ? ECUIO_ERR_SESSION_CLOSED : ECUIO_OK;
}

} // namespace

void ecuio_queue_put_frame(ecuio_queue_t* q, const ecuio_frame_t* frame)
{
    std::unique_lock<std::mutex> lock(q->m);
    copy_frame(q->frames[q->take_slot()], *frame);
    q->wake_readers(lock);
}

void ecuio_queue_put_values(ecuio_queue_t* q, const ecuio_value_batch_t* batch)
{
    std::unique_lock<std::mutex> lock(q->m);
    for (uint32_t i = 0; i < batch->count; ++i) {
        copy_value(q->values[q->take_slot()], *batch, batch->values[i]);
    }
    q->wake_readers(lock);
}

void ecuio_queue_detach(ecuio_queue_link& link)
{
    std::lock_guard<std::mutex> g(link.m);
    ecuio_queue_t* q = link.active.exchange(nullptr, std::memory_order_seq_cst);
    while (link.pins.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    if (q == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(q->m);
        q->ended = true;
    }
    q->cv.notify_all();
}

extern "C" {

ecuio_status_t ecuio_queue_open(ecuio_session_t* session, uint32_t kind,
                                uint32_t capacity, ecuio_queue_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    const uint32_t ceiling = kind == ECUIO_QUEUE_FRAMES ? ECUIO_QUEUE_MAX_FRAMES
                             : kind == ECUIO_QUEUE_VALUES
                                 ? ECUIO_QUEUE_MAX_VALUES
                                 : 0;
    if (ceiling == 0 || capacity == 0 || capacity > ceiling ||
        session->started ||
        session->queue_count >= ECUIO_QUEUE_MAX_PER_SESSION) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<ecuio_queue_t> q(new (std::nothrow) ecuio_queue_t);
    if (q == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    q->kind = kind;
    q->capacity = capacity;
    if (kind == ECUIO_QUEUE_FRAMES) {
        q->frames.reset(new (std::nothrow) ecuio_frame_t[capacity]);
    } else {
        q->values.reset(new (std::nothrow) ecuio_queue_value_t[capacity]);
    }
    if (q->frames == nullptr && q->values == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    try {
        q->link = std::make_shared<ecuio_queue_link>();
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    q->link->kind = kind;
    q->link->active.store(q.get());
    session->queues[session->queue_count++] = q->link;
    if (kind == ECUIO_QUEUE_VALUES) {
        ++session->value_queues;
    }
    *out = q.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_queue_read_frames(ecuio_queue_t* queue, ecuio_frame_t* out,
                                       uint32_t capacity, uint32_t* count,
                                       uint64_t* dropped, uint32_t timeout_ms)
{
    if (queue == nullptr || out == nullptr || count == nullptr ||
        dropped == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (capacity == 0 || queue->kind != ECUIO_QUEUE_FRAMES) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return read_queue(queue, capacity, kFrameChunk, count, dropped, timeout_ms,
                      [queue, out](uint32_t slot, uint32_t at) {
                          const ecuio_frame_t& from = queue->frames[slot];
                          copy_frame(out[at], from);
                          std::memset(out[at].data + from.data_length, 0,
                                      ECUIO_FRAME_DATA_MAX - from.data_length);
                      });
}

ecuio_status_t ecuio_queue_read_values(ecuio_queue_t* queue,
                                       ecuio_queue_value_t* out,
                                       uint32_t capacity, uint32_t* count,
                                       uint64_t* dropped, uint32_t timeout_ms)
{
    if (queue == nullptr || out == nullptr || count == nullptr ||
        dropped == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (capacity == 0 || queue->kind != ECUIO_QUEUE_VALUES) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return read_queue(queue, capacity, kValueChunk, count, dropped, timeout_ms,
                      [queue, out](uint32_t slot, uint32_t at) {
                          out[at] = queue->values[slot];
                      });
}

uint32_t ecuio_queue_test_waiting(ecuio_queue_t* queue)
{
    std::lock_guard<std::mutex> lock(queue->m);
    return queue->waiting;
}

void ecuio_queue_close(ecuio_queue_t* queue)
{
    if (queue == nullptr) {
        return;
    }
    /* Off the session first: once the pins are gone the worker can't
     * reach it */
    {
        std::lock_guard<std::mutex> g(queue->link->m);
        queue->link->active.exchange(nullptr, std::memory_order_seq_cst);
        while (queue->link->pins.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }
    /* Then the reads under way: each returns, the last one wakes us */
    {
        std::unique_lock<std::mutex> lock(queue->m);
        queue->closing = true;
        queue->cv.notify_all();
        queue->cv.wait(lock, [queue] { return queue->readers == 0; });
    }
    delete queue;
}

} // extern "C"
