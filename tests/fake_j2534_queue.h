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
 * exactly like InjectFrame: documented, keeps counts honest. */
inline void enqueue_rx_locked(const unsigned char* data, unsigned long size,
                              unsigned long timestamp_us,
                              unsigned long rx_status, long long arrives_us = 0)
{
    if (g_queue_len >= FAKE_QUEUE_DEPTH) {
        return;
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
    ++g_queue_len;
    g_arrived.notify_all();
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

} // namespace fake_j2534
