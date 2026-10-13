// File/layer: core/src/core.cpp | C++17, Qt-free, CI-validated headless build
// Testing: All 18 core tests must pass UNCHANGED — this is a behavior-
//          preserving refactor; any test modification here is a red flag

#include "qttune/version.h"
#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <utility>

namespace {
    std::atomic<bool> g_initialized{false};
} // namespace

extern "C" {

qttune_status_t qttune_init(void)
{
    bool expected = false;
    g_initialized.compare_exchange_strong(expected, true);
    return QT_OK;
}

void qttune_shutdown(void)
{
    g_initialized.store(false);
}

const char* qttune_version_string(void)
{
    return QTTUNE_VERSION_STRING;
}

const char* qttune_status_string(qttune_status_t status)
{
    switch (status) {
        case QT_OK:                  return "success";
        case QT_ERR_NULL_ARGUMENT:   return "null argument";
        case QT_ERR_NOT_IMPLEMENTED: return "not implemented yet";
        case QT_ERR_NO_TRANSPORT:    return "no transport available";
        case QT_ERR_OUT_OF_MEMORY:  return "out of memory";
        case QT_ERR_SESSION_CLOSED: return "session already closed";
        case QT_ERR_ALREADY_REGISTERED: return "already registered";
        case QT_ERR_LOAD_FAILED:    return "failed to load transport library";
        default:                     return "unknown status";
    }
}

qttune_status_t qttune_session_create(
    const char* transport_uri,
    qttune_session_t** out_session)
{
    if (out_session == nullptr || transport_uri == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }

    if (strncmp(transport_uri, "mock://", 7) != 0) {
        /* URI routing table grows here in Phase 3 (j2534:// etc.);
         * for now mock:// remains the only routable scheme. */
        return QT_ERR_NO_TRANSPORT;
    }

    try {
        *out_session = new qttune_session();
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }

    qttune_status_t status = qttune_mock_transport_attach(*out_session,
                                                           transport_uri);
    if (status != QT_OK) {
        delete *out_session;
        *out_session = nullptr;
        return status;
    }
    return QT_OK;
}

qttune_status_t qttune_session_start(qttune_session_t* session)
{
    if (session == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return QT_ERR_SESSION_CLOSED;
    }
    if (session->transport == nullptr) {
        return QT_ERR_NO_TRANSPORT;
    }
    return session->transport->ops->start(session, session->transport->impl);
}

qttune_status_t qttune_session_close(qttune_session_t* session)
{
    if (session == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }

    bool expected = false;
    if (!session->closed.compare_exchange_strong(expected, true)) {
        return QT_ERR_SESSION_CLOSED;
    }

    if (session->transport != nullptr) {
        session->transport->ops->detach(session, session->transport->impl);
        /* detach() frees the transport wrapper; NULL for double-close
         * defense even though the closed flag already gates it. */
        session->transport = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        session->callbacks.clear();
    }

    delete session;
    return QT_OK;
}

qttune_status_t qttune_session_send(
    qttune_session_t* session,
    const qttune_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (frame->data_length > sizeof(frame->data)) {
        return QT_ERR_NULL_ARGUMENT;
    }
    return QT_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_session_receive(
    qttune_session_t* session,
    qttune_frame_t* out_frame,
    uint32_t timeout_ms)
{
    (void)session;
    (void)out_frame;
    (void)timeout_ms;
    return QT_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_register_frame_callback(
    qttune_session_t* session,
    qttune_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr || callback == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return QT_ERR_SESSION_CLOSED;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);

    for (const auto& entry : session->callbacks) {
        if (entry.first == callback && entry.second == user_data) {
            return QT_OK;
        }
    }

    try {
        session->callbacks.emplace_back(callback, user_data);
        return QT_OK;
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }
}

qttune_status_t qttune_unregister_frame_callback(
    qttune_session_t* session,
    qttune_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);

    auto it = session->callbacks.begin();
    while (it != session->callbacks.end()) {
        if (it->first == callback && it->second == user_data) {
            it = session->callbacks.erase(it);
            break;
        } else {
            ++it;
        }
    }

    return QT_OK;
}

void qttune_session_dispatch_callbacks(
    qttune_session_t* session,
    const qttune_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return;
    }
    if (session->closed.load()) {
        return;
    }

    std::vector<std::pair<qttune_frame_callback_t, void*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        snapshot = session->callbacks;
    }

    for (const auto& [callback, user_data] : snapshot) {
        callback(frame, user_data);
    }
}

} // extern "C"
