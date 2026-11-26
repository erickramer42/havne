// File/layer: core/src/profile_registry.cpp | C++17, Qt-free
// Testing: UNIT: CoreTest (test_core); PluginTest (test_signals)

#include "ecuio/profile.h"
#include "error_detail.h"
#include "profile_hold.h"
#include "request_engine.h"
#include "session.h"
#include "uds_generic.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using Registry = std::unordered_map<std::string, const ecuio_signal_set_t*>;

/* The profiles core ships. Registration can't replace or remove them,
 * so a URI that names one always means the same thing. */
const char* const kBuiltIns[] = {"mock", "raw", "obd2", ECUIO_UDS_GENERIC_NAME};

/* Function-local static: thread-safe (C++11), immune to init-order
 * problems, and available with no ecuio_init (core.h). */
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

/* Each set's uses (profile_hold.h), under registry_mutex */
std::unordered_map<const ecuio_signal_set_t*, uint32_t>& uses()
{
    static std::unordered_map<const ecuio_signal_set_t*, uint32_t> u;
    return u;
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

/* 1..ECUIO_PROFILE_NAME_MAX characters of [a-z0-9._-]: what a ?profile=
 * value can carry ('&' would end it) and reads the same in every URI
 * (keys and names are lowercase, ADR-0013 section 2). */
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

/* Whether each module of a validated set derives no more requests than
 * ECUIO_MODULE_MAX_REQUESTS: OK, INVALID_ARGUMENT (out, when not NULL,
 * gets ECUIO_VALIDATE_ERR_REQUEST_COUNT and the first def past its
 * module's), or OUT_OF_MEMORY when the count doesn't allocate. */
ecuio_status_t fits_request_table(const ecuio_signal_set_t* set,
                                  ecuio_validation_t* out)
{
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t i = 0; i < set->channel_count; ++i) {
        rx_ids[i] = set->channels[i].rx_id;
    }
    ecuio_derived derived;
    switch (
        ecuio_engine_plan(set, rx_ids, set->channel_count, nullptr, &derived)) {
    case ecuio_plan_result::ok:
        return ECUIO_OK;
    case ecuio_plan_result::out_of_memory:
        return ECUIO_ERR_OUT_OF_MEMORY;
    case ecuio_plan_result::module_full:
        break;
    }
    if (out != nullptr) {
        out->code = ECUIO_VALIDATE_ERR_REQUEST_COUNT;
        out->index = derived.overflow_def;
    }
    return ECUIO_ERR_INVALID_ARGUMENT;
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
    if (signal_set != nullptr) {
        const int code = ecuio_validate_signal_set(signal_set, out);
        if (code == ECUIO_VALIDATE_ERR_OUT_OF_MEMORY) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        if (code != ECUIO_VALIDATE_OK) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    /* Every derived request must fit the engine's table, or the rest would
     * silently never be asked. Counted on the set's own channels: a
     * ?filter= override puts them all on one, which attach judges. */
    if (signal_set != nullptr) {
        const ecuio_status_t fits = fits_request_table(signal_set, out);
        if (fits != ECUIO_OK) {
            return fits;
        }
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
            ecuio_profile_set_unknown_detail(name);
            return ECUIO_ERR_UNKNOWN_PROFILE;
        }
        *out_set = it->second;   /* may be NULL: that IS "raw" */
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"

void ecuio_profile_set_unknown_detail(const char* name)
{
    if (std::strncmp(name, "merged.", 7) == 0) {
        ecuio_detail_set("no profile is named '%s': a merged set's name, made "
                         "from parts this program hasn't merged: merge the "
                         "parts again",
                         name);
    } else {
        ecuio_detail_set("no profile is named '%s'", name);
    }
}

ecuio_status_t ecuio_profile_acquire(const char* name,
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
        if (it->second != nullptr) {
            ++uses()[it->second];
        }
        *out_set = it->second;
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void ecuio_profile_release(const ecuio_signal_set_t* set) noexcept
{
    if (set == nullptr) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto it = uses().find(set);
        if (it != uses().end() && --it->second == 0) {
            uses().erase(it);
        }
    } catch (...) {
        /* A failed lock (std::system_error) leaves the use counted: the owner
         * is told it's in use, never freed under a reader */
        return;
    }
}

ecuio_status_t
ecuio_profile_unregister_unused(const ecuio_signal_set_t* const* sets,
                                size_t count)
{
    if (count > 0 && sets == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(registry_mutex());
        for (size_t i = 0; i < count; ++i) {
            if (sets[i] != nullptr && uses().count(sets[i]) != 0) {
                return ECUIO_ERR_IN_USE;
            }
        }
        for (auto it = registry().begin(); it != registry().end();) {
            const bool mine =
                it->second != nullptr && !is_built_in(it->first.c_str()) &&
                std::find(sets, sets + count, it->second) != sets + count;
            it = mine ? registry().erase(it) : std::next(it);
        }
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

extern "C" {

const ecuio_signal_set_t* ecuio_session_signal_set(ecuio_session_t* session)
{
    if (session == nullptr) {
        return nullptr;
    }
    return session->active_signal_set;
}

} // extern "C"
