// File/layer: core/src/profile_registry.cpp | C++17, Qt-free
// Testing: UNIT — lookup semantics (null set is a success), unknown
//          names fail, register overwrites, registration validates,
//          reports the validator's result and protects the built-ins,
//          unregister

#include "ecuio/profile.h"
#include "session.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using Registry = std::unordered_map<std::string, const ecuio_signal_set_t*>;

/* The profiles core ships. Registration can't replace or remove them,
 * so a URI that names one always means the same thing. */
const char* const kBuiltIns[] = {"mock", "raw", "obd2"};

/* Function-local static: initialized on FIRST ACCESS, thread-safe
 * (C++11 magic statics), immune to translation-unit init-order
 * problems, and available whether or not ecuio_init ever ran —
 * session_create must keep working without init, as it always has. */
Registry& registry()
{
    static Registry r = {
        {"mock", ecuio_mock_signal_set()},
        {"raw",  nullptr},   /* null profile — Log page only */
        /* ADR-0013: the OBD-II v1 set — mux-gated decode, plus the
         * multi-PID scheduler and channels the j2534 transport takes
         * from it at attach. */
        {"obd2", ecuio_obd2_signal_set()},
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
            return ECUIO_ERR_NO_TRANSPORT;
        }
        *out_set = it->second;   /* may be NULL — that IS "raw" */
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
