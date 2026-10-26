// File/layer: core/src/session.h | internal, C++17, Qt-free; the session
//             struct behind the opaque ecuio_session_t
// Testing: UNIT — CoreTest (sessions, callbacks, the decoded values and
//          their buffers, the info, support and identity copies);
//          HARNESS — the J2534 suites, whose worker writes support[] and
//          identity[]

#ifndef ECUIO_SESSION_H
#define ECUIO_SESSION_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "uds_generic.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

struct ecuio_session {
    std::atomic<bool> closed{false};
    std::mutex callback_mutex;
    std::vector<std::pair<ecuio_frame_callback_t, void*>> callbacks;
    /* Under the same mutex: the values callbacks (ADR-0024) */
    std::vector<std::pair<ecuio_values_callback_t, void*>> values_callbacks;
    ecuio_transport_t* transport = nullptr;
    /* The profile's set: the registry's static one, or, under
     * uds-generic, the session's own below (ADR-0020). */
    const ecuio_signal_set_t* active_signal_set = nullptr;
    /* uds-generic's set, built by create from the URI's ?did= entries
     * and owned here so it lives as long as the session; the J2534
     * transport reads its scan list at attach. NULL under any other
     * profile. */
    std::unique_ptr<ecuio_uds_session> uds;
    /* The decode's buffers (ADR-0024 §4, §6), sized by create from the
     * set, which never changes: one value per def, and text_slots
     * slots of ECUIO_DECODE_TEXT_MAX bytes, one per text value a frame
     * can decode. Only dispatch touches them, on the session's one
     * worker. */
    std::vector<ecuio_value_t> values;
    std::vector<char> text_scratch;
    uint32_t text_slots = 0;
    std::atomic<int32_t> last_error{ECUIO_OK};
    std::atomic<uint32_t> error_count{0};
    /* Written once by the transport's attach, then the profile by
     * create, before create returns; read-only afterwards, so
     * ecuio_session_get_info needs no lock. */
    ecuio_session_info_t info{};
    /* What each channel's ECU said it supports, and what it said it is
     * (ADR-0021): the count and each rx_id set by attach, the rest
     * updated by the worker, all under the mutex
     * (ecuio_session_get_channel_support, _identity). */
    mutable std::mutex support_mutex;
    ecuio_channel_support_t support[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    ecuio_channel_identity_t identity[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
    uint32_t support_count = 0;
};

#endif /* ECUIO_SESSION_H */
