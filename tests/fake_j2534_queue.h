// File/layer: tests/fake_j2534_queue.h | the fake J2534 DLL's receive
//             queue, HARNESS tier; included by fake_j2534.cpp alone
// Testing: through the fake (tests-j2534.md); no cases of its own
//
// ReadMsgs, WriteMsgs' replies and InjectFrame share this FIFO under
// g_lock. A queued message has an arrival time; reads see only those
// that have arrived, in arrival order.

#pragma once

#include "ecuio/j2534_ptypes.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#define FAKE_QUEUE_DEPTH 32   /* a read's indication cap and more */

namespace fake_j2534 {

struct FrameSlot {
    unsigned char data[ECUIO_PT_MSG_DATA_SIZE];
    unsigned long size;
    unsigned long timestamp_us;
    unsigned long rx_status;   /* replayed verbatim into PASSTHRU_MSG.RxStatus */
    long long arrives_us;      /* steady clock; reads skip it until then */
    long long window_from_us;  /* a segmented reply's First Frame, 0 none */
    bool reply;                /* an ECU's answer (enqueue_reply_locked) */
};

inline std::mutex g_lock;
inline std::condition_variable g_arrived;      /* a message was queued */
inline FrameSlot g_queue[FAKE_QUEUE_DEPTH];    /* in arrival order */
inline unsigned long g_queue_len = 0;

inline long long now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Enqueue into the shared RX FIFO (mutex held by caller), arriving at
 * arrives_us (0: now). The queue stays in arrival order, so a message
 * injected now goes ahead of a reply still on its way. Drops on full
 * exactly like InjectFrame: documented, keeps counts honest. The slot,
 * or NULL when full. */
inline FrameSlot* enqueue_rx_locked(const unsigned char* data, unsigned long size,
                              unsigned long timestamp_us,
                              unsigned long rx_status, long long arrives_us = 0)
{
    if (g_queue_len >= FAKE_QUEUE_DEPTH) {
        return nullptr;
    }
    if (arrives_us == 0) {
        arrives_us = now_us();
    }
    unsigned long i = g_queue_len;
    while (i > 0 && g_queue[i - 1].arrives_us > arrives_us) {
        g_queue[i] = g_queue[i - 1];
        --i;
    }
    FrameSlot& slot = g_queue[i];
    memcpy(slot.data, data, size);
    slot.size = size;
    slot.timestamp_us = timestamp_us;
    slot.rx_status = rx_status;
    slot.arrives_us = arrives_us;
    slot.window_from_us = 0;
    slot.reply = false;
    ++g_queue_len;
    g_arrived.notify_all();
    return &slot;
}

/* How many queued messages have arrived by now: a prefix of the queue. */
inline unsigned long arrived_locked(long long now)
{
    unsigned long n = 0;
    while (n < g_queue_len && g_queue[n].arrives_us <= now) {
        ++n;
    }
    return n;
}

inline void pop_front_locked(unsigned long n)
{
    for (unsigned long i = n; i < g_queue_len; ++i) {
        g_queue[i - n] = g_queue[i];
    }
    g_queue_len -= n;
}

/* Known Issue #42's loss (ECUIO_FAKE_LOST_UNDER_SEGMENTS, the VX): a
 * segmented reply (over 4 + 7 bytes) is on the bus from its First Frame
 * to its whole arrival, and a Single Frame reply from another ID arriving
 * then is lost, whichever was written first. */
inline bool other_id(const FrameSlot& q, const unsigned char* data)
{
    return q.size >= 4 && memcmp(q.data, data, 4) != 0;
}

/* Whether a Single Frame from data's ID arriving at arrives_us falls in
 * a queued segmented reply's window of another ID */
inline bool under_a_window_locked(const unsigned char* data,
                                  long long arrives_us)
{
    for (unsigned long i = 0; i < g_queue_len; ++i) {
        const FrameSlot& q = g_queue[i];
        if (q.window_from_us != 0 && other_id(q, data) &&
            arrives_us >= q.window_from_us && arrives_us <= q.arrives_us) {
            return true;
        }
    }
    return false;
}

/* Removes the queued Single Frame replies of other IDs arriving inside
 * [from_us, to_us]; how many */
inline uint32_t drop_under_locked(const unsigned char* data, long long from_us,
                                  long long to_us)
{
    unsigned long kept = 0;
    uint32_t dropped = 0;
    for (unsigned long i = 0; i < g_queue_len; ++i) {
        const FrameSlot& q = g_queue[i];
        if (q.reply && q.window_from_us == 0 && other_id(q, data) &&
            q.arrives_us >= from_us && q.arrives_us <= to_us) {
            ++dropped;
            continue;
        }
        g_queue[kept++] = q;
    }
    g_queue_len = kept;
    return dropped;
}

/* An ECU's answer (size at least 4: the reply ID first), its First Frame
 * at first_us and whole at arrives_us; with lose, Known Issue #42's loss.
 * The replies lost, this one or those it lands on. */
inline uint32_t enqueue_reply_locked(const unsigned char* data,
                                     unsigned long size,
                                     unsigned long timestamp_us,
                                     long long first_us, long long arrives_us,
                                     bool lose)
{
    const bool segmented = size > 4 + 7;
    if (lose && !segmented && under_a_window_locked(data, arrives_us)) {
        return 1;
    }
    const uint32_t lost =
        lose && segmented ? drop_under_locked(data, first_us, arrives_us) : 0;
    FrameSlot* slot = enqueue_rx_locked(data, size, timestamp_us, 0, arrives_us);
    if (slot != nullptr) {
        slot->reply = true;
        slot->window_from_us = segmented ? first_us : 0;
    }
    return lost;
}

} // namespace fake_j2534
