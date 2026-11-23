// File/layer: core/src/core.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest, PickOnlyTest, DefIndexTest, ConditionSessionTest,
//          RecorderTest, ConditionRecordingTest (test_core); PidSupportTest,
//          DiagnosticsTest, InspectionTest (test_obd2); ProbeNotesTest
//          (ctest); c_abi; HARNESS: Elm327SessionTest (test_elm327)


#include "ecuio/version.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "session.h"
#include "conversation_bind.h"
#include "j2534_clock.h"
#include "uds_generic.h"
#include "request_engine.h"   /* ECUIO_IDENT_REQUESTS, the names and the VIN */
#include "request_setup.h"    /* the unpicked check's interval */
#include "diagnostics.h"      /* the trouble codes' and monitors' words */
#include "pid_support.h"     /* the signals their ECU doesn't support */
#include "computed.h"        /* a computed pick's leaves, for a focus */
#include "condition_eval.h"
#include "condition_file.h" /* the set's bytes and keys, for a recording */
#include "sha256.h"
#include "uri_query.h"
#include "error_detail.h"
#include "probes.h"
#include "ecuio/j2534_ptypes.h"   /* ecuio_pt_status_name; plain C, any OS */

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <utility>

namespace {
    enum class ProfileQuery { Absent, Valid, BadValue };

    /* The last `profile` value of a split query. A malformed one (empty or
     * over-length) fails session_create, never falls back to the scheme
     * default (a typo'd raw would decode); last-wins applies across valid
     * values only. out_val holds ECUIO_PROFILE_NAME_MAX + 1 bytes. */
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

extern "C" {

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

ecuio_status_t ecuio_version(uint32_t* major, uint32_t* minor, uint32_t* patch)
{
    if (!major || !minor || !patch) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *major = ECUIO_VERSION_MAJOR;
    *minor = ECUIO_VERSION_MINOR;
    *patch = ECUIO_VERSION_PATCH;
    return ECUIO_OK;
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
        case ECUIO_ERR_LOAD_FAILED:             return "couldn't open what was named";
        case ECUIO_ERR_VEHICLE_TIMEOUT:         return "vehicle not responding (timeout)";
        case ECUIO_ERR_TRANSPORT_HARD_FAILURE: return "transport hard failure";
        case ECUIO_ERR_REQUEST_FAILED:          return "request failed (vehicle not accepting requests)";
        case ECUIO_ERR_INVALID_ARGUMENT:        return "invalid argument";
        case ECUIO_ERR_BAD_URI:                 return "malformed transport URI";
        case ECUIO_ERR_UNKNOWN_PROFILE:         return "unknown profile";
        case ECUIO_ERR_VENDOR_REFUSED:          return "the adapter's driver refused";
        case ECUIO_ERR_SURROGATE_FAILED:        return "surrogate process failed";
        case ECUIO_ERR_RECORDING_CHANGED:       return "the recording changed after it was opened";
        case ECUIO_ERR_REPORT_CHANGED:          return "the diagnostics report changed since it was saved";
        case ECUIO_ERR_RECORDING_FAILED:        return "the recording couldn't be made or written";
        case ECUIO_ERR_CANCELED:                return "canceled";
        case ECUIO_ERR_IN_USE:                  return "still in use";
        case ECUIO_ERR_STREAM_ENDED:            return "the stream has ended";
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
        s->record_link = std::make_shared<ecuio_record_link>();
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
    const bool is_elm327 = strncmp(transport_uri, "elm327://", 9) == 0;
    const bool is_socketcan = ECUIO_HAVE_SOCKETCAN &&
                              strncmp(transport_uri, "socketcan://", 12) == 0;
    if (!is_mock && !is_replay && !is_j2534 && !is_elm327 && !is_socketcan) {
#ifndef _WIN32
        if (strncmp(transport_uri, "j2534://", 8) == 0) {
            ecuio_detail_set("j2534:// needs Windows, where J2534 adapters "
                             "have their drivers");
            return ECUIO_ERR_NO_TRANSPORT;
        }
#endif
        if (strncmp(transport_uri, "socketcan://", 12) == 0) {
            ecuio_detail_set("socketcan:// needs Linux, where a CAN "
                             "interface is a network device");
            return ECUIO_ERR_NO_TRANSPORT;
        }
        ecuio_detail_set("unsupported URI scheme: expected mock://, "
                         "replay://, j2534://, elm327:// or socketcan://");
        return ECUIO_ERR_NO_TRANSPORT;
    }

    /* The query, split once (uri_query.h) and checked against the scheme's
     * keys before anything attaches: a bad query or an unknown key fails
     * with no DLL load and no discovery, the same way on every scheme. */
    const char* scheme = is_mock        ? "mock://"
                         : is_replay    ? "replay://"
                         : is_elm327    ? "elm327://"
                         : is_socketcan ? "socketcan://"
                                        : "j2534://";
    const char* const* keys = is_mock        ? ecuio_mock_query_keys
                              : is_replay    ? ecuio_replay_query_keys
                              : is_elm327    ? ecuio_elm327_query_keys
                              : is_socketcan ? ecuio_socketcan_query_keys
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
        char known[ECUIO_QUERY_KEYS_TEXT_MAX];
        join_keys(keys, known, sizeof(known));
        ecuio_detail_set("%s takes no query key '%.*s', only %s", scheme,
                         static_cast<int>(bad->key_len), bad->key, known);
        return ECUIO_ERR_BAD_URI;
    }

    /* ADR-0013 profile selection (a default per scheme, ?profile= over it),
     * resolved before any attach so a bad profile fails with no DLL load
     * and no discovery. */
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
            /* Scheme defaults: mock:// -> mock; j2534:// and elm327://
             * -> obd2 (decode, the Service 01 requests and the declared
             * 7E0/7E8 channel); replay:// -> raw, which can't decode a
             * recording wrongly (ADR-0023); socketcan:// -> raw, the
             * whole bus as it is (ADR-0032). ?profile=raw gives a
             * log-only session. */
            chosen = is_mock                    ? "mock"
                     : is_replay || is_socketcan ? "raw"
                                                 : "obd2";
            break;
    }

    const ecuio_signal_set_t* profile_set = nullptr;
    const ecuio_status_t found = ecuio_profile_acquire(chosen, &profile_set);
    s->profile_hold.set = profile_set;
    if (found != ECUIO_OK) {
        if (found == ECUIO_ERR_UNKNOWN_PROFILE) {
            ecuio_detail_set("no profile is named '%s'", chosen);
        }
        return found;
    }
    s->conversation = ecuio_conversation_of(profile_set);

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

    /* Set before attach: the requesting transports derive their
     * requests and channels from it during attach. Single writer: attach
     * and the worker both run after this store. */
    s->active_signal_set = profile_set;

    /* The decode's buffers (ADR-0024 section 4, section 6), sized once: the set never
     * changes after this */
    try {
        s->decoder.prepare(profile_set, true);
        /* Before attach: nothing after it allocates, so no refusal
         * frees a session with a transport attached */
        s->uri = transport_uri;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    ecuio_status_t status = ECUIO_ERR_NO_TRANSPORT;
    if (is_mock) {
        status = ecuio_mock_transport_attach(s.get(), transport_uri,
                                             &query);
    } else if (is_replay) {
        status = ecuio_replay_transport_attach(s.get(), transport_uri, &query);
    } else if (is_elm327) {
        status = ecuio_elm327_transport_attach(s.get(), transport_uri, &query);
    } else if (is_socketcan) {
#if ECUIO_HAVE_SOCKETCAN
        status = ecuio_socketcan_transport_attach(s.get(), transport_uri,
                                                  &query);
#endif
    } else if (is_j2534) {
#ifdef _WIN32
        status = ecuio_j2534_transport_attach(s.get(), transport_uri, &query);
#endif
    }

    if (status != ECUIO_OK) {
        return status;
    }
    /* After attach, which fills the rest of the info; the worker hasn't
     * started, so no lock */
    if (s->focus_requests || ecuio_session_may_talk(s.get())) {
        s->info.flags |= ECUIO_SESSION_INFO_REQUESTS;
    }
    if (s->focus_requests && s->focus_pick_only) {
        s->info.flags |= ECUIO_SESSION_INFO_PICK_ONLY;
    }
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
    if (!session->started) {
        const ecuio_status_t bound = ecuio_conversation_bind(session);
        if (bound != ECUIO_OK) {
            return bound;
        }
    }
    const ecuio_status_t st =
        session->transport->ops->start(session, session->transport->impl);
    if (st == ECUIO_OK && !session->started) {
        session->started = true;
        session->ran.store(true);
        ECUIO_PROBE2(session_start, session, session->info.backend);
    }
    return st;
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
    ecuio_conversation_unbind(session); /* the worker has joined */
    /* The worker has stopped: the recorder takes no more frames, and its
     * notes go on until the caller closes it (ADR-0049 section 1) */
    ecuio_record_detach(*session->record_link);
    /* So do the pull queues: what's waiting is still read, then their
     * reads end (ecuio/queue.h) */
    for (uint32_t i = 0; i < session->queue_count; ++i) {
        ecuio_queue_detach(*session->queues[i]);
    }

    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        session->callbacks.clear();
        session->values_callbacks.clear();
    }

    ECUIO_PROBE1(session_close, session);
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

void ecuio_set_j2534_clock_lag_us(uint64_t us)
{
    j2534_clock_lag_override_us().store(us);
}

void ecuio_set_unpicked_check_ms(uint32_t ms)
{
    ecuio_unpicked_check_us().store(ms == 0 ? ECUIO_UNPICKED_CHECK_US
                                            : static_cast<int64_t>(ms) * 1000);
}

} // extern "C"
