// File/layer: core/include/qttune/profile.h | C ABI, public
// Testing: UNIT (registry lookup/overwrite); session-level selection in test_core

#ifndef QTTUNE_PROFILE_H
#define QTTUNE_PROFILE_H

#include "qttune/core.h"
#include "qttune/signals.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Protocol profile registry (ADR-0013).
 *
 * A profile binds a string name to the signal set its sessions use.
 * "raw" is the null profile: registered with a NULL signal set — no
 * decode, Log page only. "mock" binds the mock set.
 *
 * Names are the registry key; the set pointer is data, NOT evidence of
 * registration. Use qttune_profile_lookup's status, never a NULL check.
 *
 * Built-ins are always present (initialized on first registry access,
 * independent of qttune_init so sessions created without init behave
 * as they always have).
 */
qttune_status_t qttune_profile_register(const char* name,
                                        const QttuneSignalSet* signal_set);

/* Lookup. Returns QT_OK and stores the set (POSSIBLY NULL for "raw")
 * when the name is registered; QT_ERR_NO_TRANSPORT when it isn't.
 * *out_set is untouched on failure. Thread-safe. */
qttune_status_t qttune_profile_lookup(const char* name,
                                      const QttuneSignalSet** out_set);

/* The session's active signal set (selected at session_create from
 * the scheme default or ?profile=). NULL means "no decode" — the raw
 * profile. Written once at create, read-only afterwards. */
const QttuneSignalSet* qttune_session_signal_set(qttune_session_t* session);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_PROFILE_H */
