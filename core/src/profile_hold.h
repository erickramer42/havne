// File/layer: core/src/profile_hold.h | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest (test_signals)
//
// A use of a registered set (ADR-0052 section 11): a session, or a
// simulated ECU, counts one from the lookup that hands it the set until
// it's destroyed, so whoever owns the set (a plugin) can be told it's
// still in use. Counted by the set's pointer under the registry's lock,
// so no lookup can come between a check and the unregister after it.

#ifndef ECUIO_PROFILE_HOLD_H
#define ECUIO_PROFILE_HOLD_H

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cstddef>

/* ecuio_profile_lookup that also counts a use of the set (none for a
 * NULL set: raw). The caller releases it once nothing reads the set. */
ecuio_status_t ecuio_profile_acquire(const char* name,
                                     const ecuio_signal_set_t** out_set);

/* One use fewer; NULL is a no-op */
void ecuio_profile_release(const ecuio_signal_set_t* set) noexcept;

/* Removes every name registered to one of the sets (a name registered
 * to another set since is left alone), or none at all with
 * ECUIO_ERR_IN_USE when any of the sets has a use. */
ecuio_status_t
ecuio_profile_unregister_unused(const ecuio_signal_set_t* const* sets,
                                size_t count);

/* A use held for an object's life: released when it's destroyed */
struct ecuio_profile_hold {
    const ecuio_signal_set_t* set = nullptr;

    ecuio_profile_hold() = default;
    ecuio_profile_hold(const ecuio_profile_hold&) = delete;
    ecuio_profile_hold& operator=(const ecuio_profile_hold&) = delete;
    ecuio_profile_hold(ecuio_profile_hold&& other) noexcept : set(other.set)
    {
        other.set = nullptr;
    }
    ecuio_profile_hold& operator=(ecuio_profile_hold&& other) noexcept
    {
        if (this != &other) {
            ecuio_profile_release(set);
            set = other.set;
            other.set = nullptr;
        }
        return *this;
    }
    ~ecuio_profile_hold() { ecuio_profile_release(set); }
};

#endif /* ECUIO_PROFILE_HOLD_H */
