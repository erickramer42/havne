// File/layer: core/src/core.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT — CoreTest: session create (routing, the query split,
//          profiles and every refusal), start, close and destroy,
//          callbacks and their snapshot dispatch, the error strings;
//          HARNESS — the J2534 suites through create; test_c_abi links
//          the public functions

#include "ecuio/version.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "session.h"
#include "uds_generic.h"
#include "request_engine.h"   /* ECUIO_IDENT_REQUESTS, for the names */
#include "uri_query.h"
#include "error_detail.h"
#include "ecuio/j2534_ptypes.h"   /* ecuio_pt_status_name; plain C, any OS */

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <utility>

namespace {
    /* ecuio_last_error_detail's text (error_detail.h), per thread. */
    thread_local char t_detail[512] = "";

    enum class ProfileQuery { Absent, Valid, BadValue };

    /* The LAST `profile` value of a split query. Tri-state: a
     * present-but-malformed value (empty or over-length) is INVALID and
     * fails session_create — it must NEVER silently downgrade to the
     * scheme default, which would hand a typo'd ?profile=raw a decoding
     * session or vice versa. Any malformed occurrence fails; last-wins
     * applies only across valid values. out_val holds
     * ECUIO_PROFILE_NAME_MAX + 1 bytes. */
    ProfileQuery query_profile(const ecuio_uri_query& q, char* out_val)
    {
        bool found = false;
        for (size_t i = 0; i < q.count; ++i) {
            const ecuio_uri::param& p = q.params[i];
            if (!p.is("profile")) {
                continue;
            }
            if (p.val_len == 0 || p.val_len > ECUIO_PROFILE_NAME_MAX) {
                return ProfileQuery::BadValue;
            }
            memcpy(out_val, p.val, p.val_len);
            out_val[p.val_len] = '\0';
            found = true;
        }
        return found ? ProfileQuery::Valid : ProfileQuery::Absent;
    }

    /* "a", "a and b", "a, b and c": a key list for a detail line. */
    void join_keys(const char* const* keys, char* out, size_t cap)
    {
        size_t n = 0;
        while (keys[n] != nullptr) {
            ++n;
        }
        size_t used = 0;
        out[0] = '\0';
        for (size_t i = 0; i < n && used < cap; ++i) {
            const char* sep = (i == 0) ? "" : (i + 1 == n) ? " and " : ", ";
            const int w = snprintf(out + used, cap - used, "%s%s", sep, keys[i]);
            if (w < 0) {
                break;
            }
            used += static_cast<size_t>(w);
        }
    }
} // namespace

void ecuio_detail_clear()
{
    t_detail[0] = '\0';
}

void ecuio_detail_set(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_detail, sizeof(t_detail), fmt, ap);
    va_end(ap);
}

void ecuio_text_one_line(char* text)
{
    size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        if (static_cast<unsigned char>(text[n]) < 0x20) {
            text[n] = ' ';
        }
    }
    while (n > 0 && text[n - 1] == ' ') {
        --n;
    }
    text[n] = '\0';
}

void ecuio_detail_set_vendor(const char* call, long status,
                             const char* vendor_text)
{
    /* The vendor's text on one line: it lands in a single UI line and a
     * log line. */
    char text[2 * ECUIO_PT_TEXT_LEN] = "";
    if (vendor_text != nullptr) {
        size_t n = 0;
        for (; vendor_text[n] != '\0' && n + 1 < sizeof(text); ++n) {
            text[n] = vendor_text[n];
        }
        text[n] = '\0';
        ecuio_text_one_line(text);
    }
    const char* name = ecuio_pt_status_name(status);
    if (name == nullptr) {
        name = "vendor-specific";
    }
    if (text[0] != '\0') {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s): %s", call, status,
                         static_cast<unsigned long>(status), name, text);
    } else {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s)", call, status,
                         static_cast<unsigned long>(status), name);
    }
}

extern "C" {

const char* ecuio_last_error_detail(void)
{
    return t_detail;
}

ecuio_status_t ecuio_session_get_error_state(
    ecuio_session_t* session,
    int32_t* out_last_error,
    uint32_t* out_error_count)
{
    if (session == nullptr || out_last_error == nullptr ||
        out_error_count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_last_error = session->last_error.load();
    *out_error_count = session->error_count.load();
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_info(
    const ecuio_session_t* session,
    ecuio_session_info_t* out_info)
{
    if (session == nullptr || out_info == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_info = session->info;
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_channel_support(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_support_t* out_support)
{
    if (session == nullptr || out_support == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (channel >= session->support_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_support = session->support[channel];
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_channel_identity(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_identity_t* out_identity)
{
    if (session == nullptr || out_identity == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (channel >= session->support_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_identity = session->identity[channel];
    return ECUIO_OK;
}

const char* ecuio_identity_request_name(const uint8_t* request, uint8_t length)
{
    if (request == nullptr) {
        return "";
    }
    for (const ecuio_ident_request& q : ECUIO_IDENT_REQUESTS) {
        if (q.len == length && q.bytes[0] == request[0] &&
            q.bytes[1] == request[1] &&
            (length < 3 || q.bytes[2] == request[2])) {
            return q.name;
        }
    }
    return "";
}

/* Optional no-ops (core.h): core holds no global state that needs
 * them. */
ecuio_status_t ecuio_init(void)
{
    return ECUIO_OK;
}

void ecuio_shutdown(void)
{
}

const char* ecuio_version_string(void)
{
    return ECUIO_VERSION_STRING;
}

const char* ecuio_status_string(ecuio_status_t status)
{
    switch (status) {
        case ECUIO_OK:                          return "success";
        case ECUIO_ERR_NULL_ARGUMENT:           return "null argument";
        case ECUIO_ERR_NOT_IMPLEMENTED:         return "not implemented yet";
        case ECUIO_ERR_NO_TRANSPORT:            return "no transport available";
        case ECUIO_ERR_OUT_OF_MEMORY:           return "out of memory";
        case ECUIO_ERR_SESSION_CLOSED:          return "session already closed";
        case ECUIO_ERR_ALREADY_REGISTERED:      return "already registered";
        case ECUIO_ERR_LOAD_FAILED:             return "failed to load transport library";
        case ECUIO_ERR_VEHICLE_TIMEOUT:         return "vehicle not responding (timeout)";
        case ECUIO_ERR_TRANSPORT_HARD_FAILURE: return "transport hard failure";
        case ECUIO_ERR_REQUEST_FAILED:          return "request failed (vehicle not accepting requests)";
        case ECUIO_ERR_INVALID_ARGUMENT:        return "invalid argument";
        case ECUIO_ERR_BAD_URI:                 return "malformed transport URI";
        case ECUIO_ERR_UNKNOWN_PROFILE:         return "unknown profile";
        case ECUIO_ERR_VENDOR_REFUSED:          return "the adapter's driver refused";
        case ECUIO_ERR_SURROGATE_FAILED:        return "surrogate process failed";
        case ECUIO_ERR_RECORDING_CHANGED:       return "the recording changed after it was opened";
        default:                                return "unknown status";
    }
}

ecuio_status_t ecuio_session_create(
    const char* transport_uri,
    ecuio_session_t** out_session)
{
    ecuio_detail_clear();
    if (out_session == nullptr || transport_uri == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    /* Held here until it's whole: every refusal below just returns,
     * and the caller's pointer is set only on success. */
    *out_session = nullptr;
    std::unique_ptr<ecuio_session> s;
    try {
        s = std::make_unique<ecuio_session>();
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    /* Scheme routing (j2534 is Win32-only). */
#ifdef _WIN32
    const bool is_j2534 = strncmp(transport_uri, "j2534://", 8) == 0;
#else
    const bool is_j2534 = false;
#endif
    const bool is_mock = strncmp(transport_uri, "mock://", 7) == 0;
    const bool is_replay = strncmp(transport_uri, "replay://", 9) == 0;
    if (!is_mock && !is_replay && !is_j2534) {
#ifndef _WIN32
        if (strncmp(transport_uri, "j2534://", 8) == 0) {
            ecuio_detail_set("j2534:// needs Windows, where J2534 adapters "
                             "have their drivers");
            return ECUIO_ERR_NO_TRANSPORT;
        }
#endif
        ecuio_detail_set("unsupported URI scheme: expected mock://, "
                         "replay:// or j2534://");
        return ECUIO_ERR_NO_TRANSPORT;
    }

    /* The query, split once (uri_query.h) and checked against the keys
     * the scheme's transport reads, before anything attaches: a
     * malformed query or an unknown key fails with no DLL load and no
     * discovery, the same way on every scheme. */
    const char* scheme = is_mock     ? "mock://"
                         : is_replay ? "replay://"
                                     : "j2534://";
    const char* const* keys = is_mock     ? ecuio_mock_query_keys
                              : is_replay ? ecuio_replay_query_keys
                                          : ecuio_j2534_query_keys;
    const char* rest = transport_uri + strlen(scheme);
    ecuio_uri_query query;
    switch (ecuio_uri::split(ecuio_uri::query_of(rest), &query)) {
        case ecuio_uri::split_result::malformed:
            ecuio_detail_set("malformed query: expected key=value pairs "
                             "joined by '&'");
            return ECUIO_ERR_BAD_URI;
        case ecuio_uri::split_result::too_long:
            ecuio_detail_set("the query has more than %d key=value pairs",
                             static_cast<int>(ECUIO_URI_MAX_PARAMS));
            return ECUIO_ERR_BAD_URI;
        case ecuio_uri::split_result::ok:
        default:
            break;
    }
    if (const ecuio_uri::param* bad = ecuio_uri::first_unknown(query, keys)) {
        char known[128];
        join_keys(keys, known, sizeof(known));
        ecuio_detail_set("%s takes no query key '%.*s', only %s", scheme,
                         static_cast<int>(bad->key_len), bad->key, known);
        return ECUIO_ERR_BAD_URI;
    }

    /* ADR-0013 profile selection: default per scheme, ?profile=
     * override. Resolved and validated BEFORE any transport attach so
     * an unknown or MALFORMED profile fails fast with no DLL load and
     * no discovery. */
    char profile_name[ECUIO_PROFILE_NAME_MAX + 1];
    const char* chosen = nullptr;
    switch (query_profile(query, profile_name)) {
        case ProfileQuery::Valid:
            chosen = profile_name;
            break;
        case ProfileQuery::BadValue:
            ecuio_detail_set("?profile= needs a profile name of 1 to %d "
                             "characters", ECUIO_PROFILE_NAME_MAX);
            return ECUIO_ERR_BAD_URI;
        case ProfileQuery::Absent:
        default:
            /* Scheme defaults: mock:// -> mock; j2534:// -> obd2
             * (decode, the Service 01 requests and the declared
             * 7E0/7E8 channel); replay:// -> raw, which can't decode a
             * recording wrongly (ADR-0023). ?profile=raw gives a
             * log-only session. */
            chosen = is_mock ? "mock" : is_replay ? "raw" : "obd2";
            break;
    }

    const ecuio_signal_set_t* profile_set = nullptr;
    const ecuio_status_t found = ecuio_profile_lookup(chosen, &profile_set);
    if (found != ECUIO_OK) {
        if (found == ECUIO_ERR_UNKNOWN_PROFILE) {
            ecuio_detail_set("no profile is named '%s'", chosen);
        }
        return found;
    }

    /* uds-generic (ADR-0020) reads its DIDs from the query and gets a set
     * of its own, owned by the session; no other profile takes ?did=.
     * Both refusals come before attach, like a bad ?profile=. */
    if (strcmp(chosen, ECUIO_UDS_GENERIC_NAME) == 0) {
        if (!ecuio_uds_query_has_did(query)) {
            ecuio_detail_set("?profile=uds-generic needs ?did= to say which "
                             "DIDs to read");
            return ECUIO_ERR_BAD_URI;
        }
        try {
            s->uds = std::make_unique<ecuio_uds_session>();
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        const ecuio_status_t built =
            ecuio_uds_build(query, s->uds.get());
        if (built != ECUIO_OK) {
            return built;
        }
        profile_set = &s->uds->set;
    } else if (ecuio_uds_query_has_did(query)) {
        ecuio_detail_set("?did= belongs to the uds-generic profile: add "
                         "?profile=uds-generic");
        return ECUIO_ERR_BAD_URI;
    }

    /* Set before attach: the j2534 transport derives its requests
     * and channels from it during attach. Single writer — attach
     * and the worker both run after this store. */
    s->active_signal_set = profile_set;

    ecuio_status_t status = ECUIO_ERR_NO_TRANSPORT;
#ifdef _WIN32
    if (is_j2534) {
        status = ecuio_j2534_transport_attach(s.get(), transport_uri,
                                              &query);
    } else
#endif
    if (is_mock) {
        status = ecuio_mock_transport_attach(s.get(), transport_uri,
                                             &query);
    } else if (is_replay) {
        status = ecuio_replay_transport_attach(s.get(), transport_uri, &query);
    }

    if (status != ECUIO_OK) {
        return status;
    }
    /* After attach, which fills the rest of the info. */
    static_assert(ECUIO_PROFILE_NAME_MAX < ECUIO_SESSION_INFO_PROFILE_LEN,
                  "the session info holds any profile name");
    char* profile = s->info.profile;
    const size_t n = strlen(chosen);
    memcpy(profile, chosen, n);
    profile[n] = '\0';
    *out_session = s.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_start(ecuio_session_t* session)
{
    ecuio_detail_clear();
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if (session->transport == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    return session->transport->ops->start(session, session->transport->impl);
}

ecuio_status_t ecuio_session_close(ecuio_session_t* session)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    /* First-closer wins the CAS; everyone else gets a clean error on
     * LIVE memory. */
    bool expected = false;
    if (!session->closed.compare_exchange_strong(expected, true)) {
        return ECUIO_ERR_SESSION_CLOSED;
    }

    /* The transport frees its impl; the wrapper is the session's. */
    if (session->transport != nullptr) {
        session->transport->ops->detach(session, session->transport->impl);
        delete session->transport;
        session->transport = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        session->callbacks.clear();
    }

    return ECUIO_OK;
}

ecuio_status_t ecuio_session_destroy(ecuio_session_t* session)
{
    if (session == nullptr) {
        return ECUIO_OK;    /* free(nullptr) semantics */
    }

    (void)ecuio_session_close(session);
    delete session;
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_send(
    ecuio_session_t* session,
    const ecuio_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if (frame->data_length > sizeof(frame->data)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_session_receive(
    ecuio_session_t* session,
    ecuio_frame_t* out_frame,
    uint32_t timeout_ms)
{
    (void)session;
    (void)out_frame;
    (void)timeout_ms;
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_register_frame_callback(
    ecuio_session_t* session,
    ecuio_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr || callback == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);

    for (const auto& entry : session->callbacks) {
        if (entry.first == callback && entry.second == user_data) {
            return ECUIO_OK;
        }
    }

    try {
        session->callbacks.emplace_back(callback, user_data);
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_unregister_frame_callback(
    ecuio_session_t* session,
    ecuio_frame_callback_t callback,
    void* user_data)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
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

    return ECUIO_OK;
}

void ecuio_session_dispatch_callbacks(
    ecuio_session_t* session,
    const ecuio_frame_t* frame)
{
    if (session == nullptr || frame == nullptr) {
        return;
    }
    if (session->closed.load()) {
        return;
    }

    std::vector<std::pair<ecuio_frame_callback_t, void*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        snapshot = session->callbacks;
    }

    for (const auto& [callback, user_data] : snapshot) {
        callback(frame, user_data);
    }
}

} // extern "C"
