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
#include "sha256.h"
#include "signal_file_build.h"

#include <cstring>
#include <mutex>
#include <new>

namespace {

bool recording(const ecuio_session_t& s)
{
    const std::lock_guard<std::mutex> attach(s.record_link->m);
    return s.record_link->active.load() != nullptr;
}

void add_print(uint32_t rx, const uint8_t* fingerprint,
               std::vector<ecuio_session::declared_print>* out)
{
    ecuio_session::declared_print d;
    d.rx = rx;
    std::memcpy(d.fingerprint, fingerprint, sizeof d.fingerprint);
    out->push_back(d);
}

/* The channels of session's declared modules whose parts include
 * fingerprint, a bit each */
uint32_t declared_by(const ecuio_session_t& s, const uint8_t* fingerprint)
{
    uint32_t bits = 0;
    for (const ecuio_session::declared_print& d : s.declared_prints) {
        if (std::memcmp(d.fingerprint, fingerprint, 32) != 0) {
            continue;
        }
        for (uint32_t c = 0; c < s.module_rx.size() && c < 32; ++c) {
            if (s.module_rx[c] == d.rx) {
                bits |= 1u << c;
            }
        }
    }
    return bits;
}

} // namespace

bool ecuio_declared_prints_of(const ecuio_signal_set_t* set,
                              std::vector<ecuio_session::declared_print>* out)
{
    out->clear();
    const ecuio_signal_file* f = ecuio_sf::file_of_set(set);
    if (f == nullptr) {
        return true;
    }
    try {
        for (size_t k = 0; k < f->channels.size() && k < f->modules.size();
             ++k) {
            const uint32_t rx = f->channels[k].rx_id;
            if (f->channels[k].session != ECUIO_CHANNEL_SESSION_EXTENDED) {
                continue;
            }
            if (f->parts_merged_here) {
                for (const uint32_t p : f->modules[k].session_parts) {
                    if (p < f->parts.size() && f->parts[p].has_fingerprint) {
                        add_print(rx, f->parts[p].fingerprint, out);
                    }
                }
            } else if (!f->plugin.empty()) {
                if (f->consent_fingerprint) {
                    add_print(rx, f->fingerprint, out);
                }
            } else {
                ecuio_sha256::hasher h;
                h.add(f->bytes.data(), f->bytes.size());
                uint8_t d[32];
                h.finish(d);
                add_print(rx, d, out);
            }
        }
    } catch (const std::bad_alloc&) {
        out->clear();
        return false;
    }
    return true;
}

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
    const uint32_t declared = declared_by(*session, fingerprint);
    if (session->conversation == nullptr && declared == 0) {
        ecuio_detail_set(
            session->declared_prints.empty()
                ? "no plugin of this session's signals asks consent"
                : "the fingerprint is no part's that declares a "
                  "module's session");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (session->conversation == nullptr ||
        (declared != 0 && std::memcmp(session->conversation->fingerprint,
                                      fingerprint, 32) != 0)) {
        ecuio_consent_record_t record{};
        record.struct_size = sizeof record;
        const ecuio_status_t found =
            ecuio_consent_store_find(store, fingerprint, &record);
        if (found == ECUIO_ERR_CONSENT_REQUIRED) {
            ecuio_detail_set("the consent store holds no consent to the part "
                             "that declares a module's session");
        }
        if (found != ECUIO_OK) {
            return found;
        }
        session->declared_allowed |= declared;
        return ECUIO_OK;
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
    session->declared_allowed |= declared;
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

extern "C" ecuio_status_t
ecuio_session_wants_consent(const ecuio_session_t* session, uint32_t* out)
{
    ecuio_detail_clear();
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = 0;
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    for (uint32_t c = 0; c < session->module_rx.size(); ++c) {
        if (ecuio_session_channel_wants_consent(session, c)) {
            *out = 1;
        }
    }
    return ECUIO_OK;
}
