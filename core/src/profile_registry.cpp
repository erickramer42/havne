// File/layer: core/src/profile_registry.cpp | C++17, Qt-free
// Testing: UNIT — lookup semantics (null set is a success), unknown
//          names fail, register overwrites, registration validates and
//          protects the built-ins, unregister

#include "qttune/profile.h"
#include "session.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using Registry = std::unordered_map<std::string, const QttuneSignalSet*>;

/* The profiles core ships. Registration can't replace or remove them,
 * so a URI that names one always means the same thing. */
const char* const kBuiltIns[] = {"mock", "raw", "obd2"};

/* Function-local static: initialized on FIRST ACCESS, thread-safe
 * (C++11 magic statics), immune to translation-unit init-order
 * problems, and available whether or not qttune_init ever ran —
 * session_create must keep working without init, as it always has. */
Registry& registry()
{
    static Registry r = {
        {"mock", qttune_mock_signal_set()},
        {"raw",  nullptr},   /* null profile — Log page only */
        /* ADR-0013: the OBD-II v1 set — mux-gated decode, plus the
         * multi-PID scheduler and channels the j2534 transport takes
         * from it at attach. */
        {"obd2", qttune_obd2_signal_set()},
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

/* 1..QTTUNE_PROFILE_NAME_MAX characters of [a-z0-9._-]: a name a
 * ?profile= value can carry ('&' would end it) and that reads the same
 * in every URI (keys and names are lowercase, ADR-0013 §2). */
bool is_valid_name(const char* name)
{
    size_t n = 0;
    for (const char* c = name; *c != '\0'; ++c, ++n) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                        *c == '.' || *c == '_' || *c == '-';
        if (!ok || n >= QTTUNE_PROFILE_NAME_MAX) {
            return false;
        }
    }
    return n > 0;
}

} // namespace

extern "C" {

qttune_status_t qttune_profile_register(const char* name,
                                        const QttuneSignalSet* signal_set)
{
    if (name == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (!is_valid_name(name) || is_built_in(name)) {
        return QTTUNE_ERR_INVALID_ARGUMENT;
    }
    /* Validated here, once, so the transport and the bridge can trust
     * every set they're handed (ADR-0013's plugin surface). */
    if (signal_set != nullptr &&
        qttune_validate_signal_set(signal_set) != QTTUNE_VALIDATE_OK) {
        return QTTUNE_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[std::string(name)] = signal_set;   /* last wins */
        return QTTUNE_OK;
    } catch (...) {
        return QTTUNE_ERR_OUT_OF_MEMORY;
    }
}

qttune_status_t qttune_profile_unregister(const char* name)
{
    if (name == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (is_built_in(name)) {
        return QTTUNE_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry().erase(std::string(name));
        return QTTUNE_OK;
    } catch (...) {
        return QTTUNE_ERR_OUT_OF_MEMORY;
    }
}

qttune_status_t qttune_profile_lookup(const char* name,
                                      const QttuneSignalSet** out_set)
{
    if (name == nullptr || out_set == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto it = registry().find(std::string(name));
        if (it == registry().end()) {
            return QTTUNE_ERR_NO_TRANSPORT;
        }
        *out_set = it->second;   /* may be NULL — that IS "raw" */
        return QTTUNE_OK;
    } catch (...) {
        return QTTUNE_ERR_OUT_OF_MEMORY;
    }
}

const QttuneSignalSet* qttune_session_signal_set(qttune_session_t* session)
{
    if (session == nullptr) {
        return nullptr;
    }
    return session->active_signal_set;
}

} // extern "C"
