#ifndef QTTUNE_SESSION_H
#define QTTUNE_SESSION_H

#include "qttune/core.h"
#include "qttune/internal.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

struct qttune_session {
    std::atomic<bool> closed{false};
    std::mutex callback_mutex;
    std::vector<std::pair<qttune_frame_callback_t, void*>> callbacks;
    qttune_transport_t* transport = nullptr;
};

#endif /* QTTUNE_SESSION_H */
