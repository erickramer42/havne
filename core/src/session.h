#ifndef ECUIO_SESSION_H
#define ECUIO_SESSION_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/signals.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

struct ecuio_session {
    std::atomic<bool> closed{false};
    std::mutex callback_mutex;
    std::vector<std::pair<ecuio_frame_callback_t, void*>> callbacks;
    ecuio_transport_t* transport = nullptr;
    const ecuio_signal_set_t* active_signal_set = nullptr;
    std::atomic<int32_t> last_error{ECUIO_OK};
    std::atomic<uint32_t> error_count{0};
    /* Written once by the transport's attach, then the profile by
     * create, before create returns; read-only afterwards, so
     * ecuio_session_get_info needs no lock. */
    ecuio_session_info_t info{};
    /* What each channel's ECU said it supports: the count and each
     * rx_id set by attach, the rest updated by the worker, all under
     * the mutex (ecuio_session_get_channel_support). */
    mutable std::mutex support_mutex;
    ecuio_channel_support_t support[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    uint32_t support_count = 0;
};

#endif /* ECUIO_SESSION_H */
