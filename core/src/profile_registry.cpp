// File/layer: core/src/profile_registry.cpp | C++17, Qt-free
// Testing: UNIT — lookup semantics (null set is a success), unknown
//          names fail, register overwrites

#include "qttune/profile.h"
#include "session.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using Registry = std::unordered_map<std::string, const QttuneSignalSet*>;

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

} // namespace

extern "C" {

qttune_status_t qttune_profile_register(const char* name,
                                        const QttuneSignalSet* signal_set)
{
    if (name == nullptr || name[0] == '\0') {
        return QT_ERR_NULL_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[std::string(name)] = signal_set;   /* last wins */
        return QT_OK;
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }
}

qttune_status_t qttune_profile_lookup(const char* name,
                                      const QttuneSignalSet** out_set)
{
    if (name == nullptr || out_set == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto it = registry().find(std::string(name));
        if (it == registry().end()) {
            return QT_ERR_NO_TRANSPORT;
        }
        *out_set = it->second;   /* may be NULL — that IS "raw" */
        return QT_OK;
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
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
