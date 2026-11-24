// File/layer: core/src/core_consent.cpp | C ABI, C++17, Qt-free, every
//             platform
// Testing: UNIT: ConsentSessionTest (test_core)
//
// A consent applied to a session between create and start (ADR-0066's
// appendix of 2026-11-23): the one way a user's session binds a plugin's
// module (consent_gate.h). The store is read through its public call, so
// a session holds the record's fingerprint and second, never the store.

#include "ecuio/plugin.h"

#include "consent_gate.h"
#include "conversation_bind.h"
#include "error_detail.h"
#include "session.h"

#include <cstring>
#include <mutex>

namespace {

bool recording(const ecuio_session_t& s)
{
    const std::lock_guard<std::mutex> attach(s.record_link->m);
    return s.record_link->active.load() != nullptr;
}

} // namespace

extern "C" ecuio_status_t
ecuio_session_apply_consent(ecuio_session_t* session,
                            const ecuio_consent_store_t* store,
                            const uint8_t* fingerprint)
{
    ecuio_detail_clear();
    if (session == nullptr || store == nullptr || fingerprint == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if (session->started) {
        ecuio_detail_set("consent is applied before the session starts");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    /* An attached recorder wrote its header without the consent line */
    if (recording(*session)) {
        ecuio_detail_set("consent is applied before a recorder attaches");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (session->conversation == nullptr) {
        ecuio_detail_set("no plugin of this session's signals asks consent");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_conversation_part& p = *session->conversation;
    if (!p.has_publisher) {
        ecuio_detail_set("plugin %s names no publisher, so no consent can be "
                         "given to it: a plugin of format 3 names one",
                         p.plugin_id.c_str());
        return ECUIO_ERR_CONSENT_REQUIRED;
    }
    if (std::memcmp(p.fingerprint, fingerprint, sizeof p.fingerprint) != 0) {
        ecuio_detail_set("the fingerprint isn't plugin %s %s's",
                         p.plugin_id.c_str(), p.plugin_version.c_str());
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_consent_record_t record{};
    record.struct_size = sizeof record;
    const ecuio_status_t found =
        ecuio_consent_store_find(store, fingerprint, &record);
    if (found == ECUIO_ERR_CONSENT_REQUIRED) {
        ecuio_detail_set("the consent store holds no consent to plugin %s %s",
                         p.plugin_id.c_str(), p.plugin_version.c_str());
    }
    if (found != ECUIO_OK) {
        return found;
    }
    session->consent_applied = true;
    std::memcpy(session->consent_fingerprint, fingerprint,
                sizeof session->consent_fingerprint);
    session->consent_given_utc = record.given_utc;
    session->conversation_consent = ecuio_plugin_code_may_run(session);
    /* The worker hasn't started: no lock, as at create */
    if (ecuio_session_may_talk(session)) {
        session->info.flags |= ECUIO_SESSION_INFO_REQUESTS;
    }
    return ECUIO_OK;
}
