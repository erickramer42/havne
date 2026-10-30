// File/layer: core/src/profile_registry.cpp | C++17, Qt-free
// Testing: UNIT: lookup semantics (null set is a success), unknown
//          names fail, register overwrites, registration validates,
//          reports the validator's result, refuses a set with more
//          requests than the engine's table holds, protects the
//          built-ins, unregister

#include "ecuio/profile.h"
#include "request_engine.h"
#include "session.h"
#include "uds_generic.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using Registry = std::unordered_map<std::string, const ecuio_signal_set_t*>;

/* The profiles core ships. Registration can't replace or remove them,
 * so a URI that names one always means the same thing. */
const char* const kBuiltIns[] = {"mock", "raw", "obd2", ECUIO_UDS_GENERIC_NAME};

/* Function-local static: initialized on FIRST ACCESS, thread-safe
 * (C++11 magic statics), immune to translation-unit init-order
 * problems, and available with no ecuio_init, which core doesn't
 * need (core.h). */
Registry& registry()
{
    static Registry r = {
        {"mock", ecuio_mock_signal_set()},
        {"raw",  nullptr},   /* null profile: Log page only */
        /* ADR-0013: the OBD-II v1 set, mux-gated decode, plus the
         * requests and channels the j2534 transport takes from it at
         * attach. */
        {"obd2", ecuio_obd2_signal_set()},
        /* ADR-0020: an empty set stands for the name; each session gets
         * its own, built by create from the URI's ?did= entries. */
        {ECUIO_UDS_GENERIC_NAME, ecuio_uds_generic_template_set()},
    };
    return r;
}

std::mutex& registry_mutex()
{
    static std::mutex m;
    return m;
}

bool is_built_in(const char* name)
{
    for (const char* b : kBuiltIns) {
        if (strcmp(name, b) == 0) {
            return true;
        }
    }
    return false;
}

/* 1..ECUIO_PROFILE_NAME_MAX characters of [a-z0-9._-]: a name a
 * ?profile= value can carry ('&' would end it) and that reads the same
 * in every URI (keys and names are lowercase, ADR-0013 §2). */
bool is_valid_name(const char* name)
{
    size_t n = 0;
    for (const char* c = name; *c != '\0'; ++c, ++n) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                        *c == '.' || *c == '_' || *c == '-';
        if (!ok || n >= ECUIO_PROFILE_NAME_MAX) {
            return false;
        }
    }
    return n > 0;
}

/* Whether a validated set's requests fit the engine's table; out, when
 * not NULL, gets ECUIO_VALIDATE_ERR_REQUEST_COUNT and the first def
 * that didn't. */
bool fits_request_table(const ecuio_signal_set_t* set, ecuio_validation_t* out)
{
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t i = 0; i < set->channel_count; ++i) {
        rx_ids[i] = set->channels[i].rx_id;
    }
    ecuio_request_engine e;
    ecuio_engine_init(&e, set->channel_count);
    uint32_t overflow = UINT32_MAX;
    if (ecuio_engine_derive(&e, set, rx_ids, set->channel_count, &overflow)) {
        return true;
    }
    if (out != nullptr) {
        out->code = ECUIO_VALIDATE_ERR_REQUEST_COUNT;
        out->index = overflow;
    }
    return false;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_profile_register(const char* name,
                                      const ecuio_signal_set_t* signal_set,
                                      ecuio_validation_t* out)
{
    /* Code 0 and no index unless the validator runs below, so a
     * refusal with code 0 is the name's (profile.h). */
    if (out != nullptr) {
        ecuio_validation_t none = {};
        none.index = UINT32_MAX;
        *out = none;
    }
    if (name == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!is_valid_name(name) || is_built_in(name)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    /* Validated here, once, so the transport and the bridge can trust
     * every set they're handed (ADR-0013's plugin surface). */
    if (signal_set != nullptr &&
        ecuio_validate_signal_set(signal_set, out) != ECUIO_VALIDATE_OK) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    /* Every request the set derives must fit the engine's table, or a
     * session would never ask for the rest and nothing would say why.
     * Counted on the set's own channels: a session's ?filter= override
     * merges them into one, which can only dedupe more. */
    if (signal_set != nullptr && !fits_request_table(signal_set, out)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[std::string(name)] = signal_set;   /* last wins */
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_profile_unregister(const char* name)
{
    if (name == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (is_built_in(name)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry().erase(std::string(name));
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_profile_lookup(const char* name,
                                    const ecuio_signal_set_t** out_set)
{
    if (name == nullptr || out_set == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto it = registry().find(std::string(name));
        if (it == registry().end()) {
            return ECUIO_ERR_UNKNOWN_PROFILE;
        }
        *out_set = it->second;   /* may be NULL: that IS "raw" */
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

const ecuio_signal_set_t* ecuio_session_signal_set(ecuio_session_t* session)
{
    if (session == nullptr) {
        return nullptr;
    }
    return session->active_signal_set;
}

} // extern "C"
