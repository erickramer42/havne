// File/layer: core/include/ecuio/profile.h | C ABI, public
// Testing: UNIT (registry lookup/overwrite); session-level selection in test_core

#ifndef ECUIO_PROFILE_H
#define ECUIO_PROFILE_H

#include "ecuio/core.h"
#include "ecuio/signals.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Protocol profile registry (ADR-0013).
 *
 * A profile binds a string name to the signal set its sessions use.
 * "raw" is the null profile: registered with a NULL signal set — no
 * decode, Log page only. "mock" binds the mock set, "obd2" the OBD-II
 * set. "uds-generic" (ADR-0020) binds an empty set: each of its
 * sessions gets a set of its own, built by ecuio_session_create from
 * the URI's ?did= entries and owned by the session, which
 * ecuio_session_signal_set returns like any other.
 *
 * Names are the registry key; the set pointer is data, NOT evidence of
 * registration. Use ecuio_profile_lookup's status, never a NULL check.
 *
 * Built-ins are always present (initialized on first registry access;
 * core needs no ecuio_init, core.h) and can't be replaced or removed.
 *
 * Lifetime: the registry keeps the pointer, not a copy, and each
 * session keeps the set it was created with. The set, its defs and
 * its channels must stay valid and unchanged while the name is
 * registered and until every session created with it is destroyed;
 * static storage is the simple way. Replacing or unregistering a name
 * leaves existing sessions on their set.
 *
 * Register, unregister and lookup are thread-safe.
 */

/* The longest profile name: what a ?profile= value can carry. */
#define ECUIO_PROFILE_NAME_MAX 63

/* The most requests a set's defs may derive (ADR-0015 §2): one per
 * distinct request and channel, so defs read from one reply count
 * once. */
#define ECUIO_PROFILE_MAX_REQUESTS 128

/* Register a name for a signal set, replacing a non-built-in profile of
 * the same name (last wins). The name is 1..ECUIO_PROFILE_NAME_MAX
 * characters of [a-z0-9._-], so a URI can select it. A non-NULL set
 * must pass ecuio_validate_signal_set and derive at most
 * ECUIO_PROFILE_MAX_REQUESTS requests; NULL registers a log-only
 * profile, like "raw". Returns ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for
 * a NULL name; ECUIO_ERR_INVALID_ARGUMENT for a malformed or built-in
 * name, an invalid set or one with too many requests, leaving the
 * registry unchanged.
 *
 * out may be NULL. Otherwise it receives the validator's result when
 * the set was checked: which check failed, and on which def or
 * channel; ECUIO_VALIDATE_ERR_REQUEST_COUNT names the first def whose
 * request didn't fit. When it wasn't (a NULL set, or a name refused
 * first) out holds code 0 and index UINT32_MAX, so a refusal with code
 * 0 is the name's. */
ecuio_status_t ecuio_profile_register(const char* name,
                                      const ecuio_signal_set_t* signal_set,
                                      ecuio_validation_t* out);

/* Remove a registered profile. Idempotent: an unknown name returns
 * ECUIO_OK. ECUIO_ERR_NULL_ARGUMENT for a NULL name;
 * ECUIO_ERR_INVALID_ARGUMENT for a built-in. */
ecuio_status_t ecuio_profile_unregister(const char* name);

/* Lookup. Returns ECUIO_OK and stores the set (POSSIBLY NULL for "raw")
 * when the name is registered; ECUIO_ERR_UNKNOWN_PROFILE when it isn't,
 * the code ecuio_session_create returns for that ?profile=;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL argument. *out_set is untouched on
 * failure. */
ecuio_status_t ecuio_profile_lookup(const char* name,
                                    const ecuio_signal_set_t** out_set);

/* The session's active signal set (selected at session_create from
 * the scheme default or ?profile=). NULL means "no decode" — the raw
 * profile. Written once at create, read-only afterwards. */
const ecuio_signal_set_t* ecuio_session_signal_set(ecuio_session_t* session);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_PROFILE_H */
