// File/layer: core/src/core.cpp | C++17, Qt-free, CI-validated headless build

#include "qttune/version.h"
#include "qttune/core.h"
#include "qttune/internal.h"
#include "qttune/profile.h"
#include "session.h"
#include "uri_query.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <utility>

namespace {
    std::atomic<bool> g_initialized{false};

    enum class ProfileQuery { Absent, Valid, Invalid };

    /* Extract the LAST `profile` value from the query of a URI body,
    * through the shared grammar (uri_query.h), so a structurally
    * malformed query fails here for every scheme. Other keys are the
    * transport's to judge. Tri-state: a present-but-malformed value
    * (empty or over-length) is INVALID and fails session_create — it
    * must NEVER silently downgrade to the scheme default, which would
    * hand a typo'd ?profile=raw a decoding session or vice versa. The
    * FIRST malformed occurrence fails; last-wins applies only across
    * valid values. out_val holds QTTUNE_PROFILE_NAME_MAX + 1 bytes. */
    ProfileQuery query_profile(const char* rest, char* out_val)
    {
        bool found = false;
        const bool ok = qttune_uri::for_each_param(
            qttune_uri::query_of(rest), [&](const qttune_uri::param& p) {
                if (!p.is("profile")) {
                    return true;
                }
                if (p.val_len == 0 || p.val_len > QTTUNE_PROFILE_NAME_MAX) {
                    return false;
                }
                memcpy(out_val, p.val, p.val_len);
                out_val[p.val_len] = '\0';
                found = true;
                return true;
            });
        if (!ok) {
            return ProfileQuery::Invalid;
        }
        return found ? ProfileQuery::Valid : ProfileQuery::Absent;
    }
} // namespace

extern "C" {

qttune_status_t qttune_session_get_error_state(
    qttune_session_t* session,
    int32_t* out_last_error,
    uint32_t* out_error_count)
{
    if (session == nullptr || out_last_error == nullptr ||
        out_error_count == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    *out_last_error = session->last_error.load();
    *out_error_count = session->error_count.load();
    return QTTUNE_OK;
}

qttune_status_t qttune_init(void)
{
    bool expected = false;
    g_initialized.compare_exchange_strong(expected, true);
    return QTTUNE_OK;
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
        case QTTUNE_OK:                         return "success";
        case QTTUNE_ERR_NULL_ARGUMENT:          return "null argument";
        case QTTUNE_ERR_NOT_IMPLEMENTED:        return "not implemented yet";
        case QTTUNE_ERR_NO_TRANSPORT:           return "no transport available";
        case QTTUNE_ERR_OUT_OF_MEMORY:          return "out of memory";
        case QTTUNE_ERR_SESSION_CLOSED:         return "session already closed";
        case QTTUNE_ERR_ALREADY_REGISTERED:     return "already registered";
        case QTTUNE_ERR_LOAD_FAILED:            return "failed to load transport library";
        case QTTUNE_ERR_VEHICLE_TIMEOUT:        return "vehicle not responding (timeout)";
        case QTTUNE_ERR_TRANSPORT_HARD_FAILURE: return "transport hard failure";
        case QTTUNE_ERR_REQUEST_FAILED:         return "request failed (vehicle not accepting requests)";
        case QTTUNE_ERR_INVALID_ARGUMENT:       return "invalid argument";
        default:                                return "unknown status";
    }
}

qttune_status_t qttune_session_create(
    const char* transport_uri,
    qttune_session_t** out_session)
{
    if (out_session == nullptr || transport_uri == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }

    try {
        *out_session = new qttune_session();
    } catch (...) {
        return QTTUNE_ERR_OUT_OF_MEMORY;
    }

    /* Scheme routing (j2534 is Win32-only). */
#ifdef _WIN32
    const bool is_j2534 = strncmp(transport_uri, "j2534://", 8) == 0;
#else
    const bool is_j2534 = false;
#endif
    const bool is_mock = strncmp(transport_uri, "mock://", 7) == 0;
    if (!is_mock && !is_j2534) {
        delete *out_session;
        *out_session = nullptr;
        return QTTUNE_ERR_NO_TRANSPORT;
    }

    /* ADR-0013 profile selection: default per scheme, ?profile=
     * override. Resolved and validated BEFORE any transport attach so
     * an unknown or MALFORMED profile fails fast with no DLL load and
     * no discovery. */
    const char* rest = transport_uri + (is_mock ? 7 : 8);
    char profile_name[QTTUNE_PROFILE_NAME_MAX + 1];
    const char* chosen = nullptr;
    switch (query_profile(rest, profile_name)) {
        case ProfileQuery::Valid:
            chosen = profile_name;
            break;
        case ProfileQuery::Invalid:
            delete *out_session;
            *out_session = nullptr;
            return QTTUNE_ERR_NO_TRANSPORT;  /* malformed query or ?profile= — strict */
        case ProfileQuery::Absent:
        default:
            /* Scheme defaults: mock:// -> mock; j2534:// -> obd2
             * (decode, the multi-PID scheduler and the declared
             * 7E0/7E8 channel). ?profile=raw gives a log-only session. */
            chosen = is_mock ? "mock" : "obd2";
            break;
    }

    const QttuneSignalSet* profile_set = nullptr;
    if (qttune_profile_lookup(chosen, &profile_set) != QTTUNE_OK) {
        delete *out_session;
        *out_session = nullptr;
        return QTTUNE_ERR_NO_TRANSPORT;   /* unknown profile name — strict */
    }

    /* Set before attach: the j2534 transport derives its scheduler
     * PIDs and channels from it during attach. Single writer — attach
     * and the worker both run after this store. */
    (*out_session)->active_signal_set = profile_set;

    qttune_status_t status = QTTUNE_ERR_NO_TRANSPORT;
#ifdef _WIN32
    if (is_j2534) {
        status = qttune_j2534_transport_attach(*out_session, transport_uri);
    } else
#endif
    if (is_mock) {
        status = qttune_mock_transport_attach(*out_session, transport_uri);
    }

    if (status != QTTUNE_OK) {
        delete *out_session;
        *out_session = nullptr;
        return status;
    }
    return QTTUNE_OK;
}

qttune_status_t qttune_session_start(qttune_session_t* session)
{
    if (session == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return QTTUNE_ERR_SESSION_CLOSED;
    }
    if (session->transport == nullptr) {
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    return session->transport->ops->start(session, session->transport->impl);
}

qttune_status_t qttune_session_close(qttune_session_t* session)
{
    if (session == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }

    /* First-closer wins the CAS; everyone else gets a clean error on
     * LIVE memory. */
    bool expected = false;
    if (!session->closed.compare_exchange_strong(expected, true)) {
        return QTTUNE_ERR_SESSION_CLOSED;
    }

    if (session->transport != nullptr) {
        session->transport->ops->detach(session, session->transport->impl);
        session->transport = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        session->callbacks.clear();
    }

    return QTTUNE_OK;
}

qttune_status_t qttune_session_destroy(qttune_session_t* session)
{
    if (session == nullptr) {
        return QTTUNE_OK;   /* free(nullptr) semantics */
    }

    (void)qttune_session_close(session);
    delete session;
    return QTTUNE_OK;
}

qttune_status_t qttune_session_send(
    qttune_session_t* session,
    const qttune_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return QTTUNE_ERR_SESSION_CLOSED;
    }
    if (frame->data_length > sizeof(frame->data)) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    return QTTUNE_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_session_receive(
    qttune_session_t* session,
    qttune_frame_t* out_frame,
    uint32_t timeout_ms)
{
    (void)session;
    (void)out_frame;
    (void)timeout_ms;
    return QTTUNE_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_register_frame_callback(
    qttune_session_t* session,
    qttune_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr || callback == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return QTTUNE_ERR_SESSION_CLOSED;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);

    for (const auto& entry : session->callbacks) {
        if (entry.first == callback && entry.second == user_data) {
            return QTTUNE_OK;
        }
    }

    try {
        session->callbacks.emplace_back(callback, user_data);
        return QTTUNE_OK;
    } catch (...) {
        return QTTUNE_ERR_OUT_OF_MEMORY;
    }
}

qttune_status_t qttune_unregister_frame_callback(
    qttune_session_t* session,
    qttune_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
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

    return QTTUNE_OK;
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
