#ifndef QTTUNE_SESSION_H
#define QTTUNE_SESSION_H

#include "qttune/core.h"
#include "qttune/internal.h"
#include "qttune/signals.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

struct qttune_session {
    std::atomic<bool> closed{false};
    std::mutex callback_mutex;
    std::vector<std::pair<qttune_frame_callback_t, void*>> callbacks;
    qttune_transport_t* transport = nullptr;
    const QttuneSignalSet* active_signal_set = nullptr;
    std::atomic<int32_t> last_error{QT_OK};
    std::atomic<uint32_t> error_count{0};
};

#endif /* QTTUNE_SESSION_H */
