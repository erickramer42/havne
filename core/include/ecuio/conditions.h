// File/layer: core/include/ecuio/conditions.h | C ABI, public
// Testing: UNIT: ConditionFileTest (test_signals); c_abi; FUZZ: fuzz_condition_file

#ifndef ECUIO_CONDITIONS_H
#define ECUIO_CONDITIONS_H

#include "ecuio/core.h"
#include "ecuio/recording.h" /* ecuio_check_options_t, ECUIO_CHECK_* */
#include "ecuio/signals.h"   /* ecuio_signal_set_t, ecuio_signal_file_t */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Condition files (ADR-0063) ----
 * A condition is a named test of a session: one to four terms joined by
 * all or any, its holds, and what it is used for (an alert, a recording,
 * a marker). Conditions are data: JSON (format 1,
 * docs/condition-files.md), read from bytes, never a path, as a signal
 * file is. Everything a file gets wrong is refused here, at load. What
 * only a set of signals can settle (a reference the set lacks, a unit
 * that differs) is not refused: ecuio_condition_set_resolve reports it. */

/* A file's bounds */
#define ECUIO_CONDITION_FILE_FORMAT    1u       /* its "havne_conditions" */
#define ECUIO_CONDITION_FILE_MAX_BYTES 262144u  /* 256 KiB */
#define ECUIO_CONDITION_MAX_CONDITIONS 64u
#define ECUIO_CONDITION_MAX_TERMS      4u
#define ECUIO_CONDITION_MAX_REFERENCES 8u
#define ECUIO_CONDITION_KEY_MAX        31u      /* bytes, the NUL not counted */
#define ECUIO_CONDITION_NAME_MAX       40u
#define ECUIO_CONDITION_MAX_FOR_US     600000000u /* "for" and "clear_for" */
#define ECUIO_CONDITION_MAX_BEFORE_US  60000000u
#define ECUIO_CONDITION_MAX_AFTER_US   600000000u
/* A "checks" gap or settle, and its rate drop in hundredths */
#define ECUIO_CONDITION_MAX_CHECK_US        3600000000u
#define ECUIO_CONDITION_MAX_RATE_DROP_X100  100000u

typedef struct ecuio_condition_set ecuio_condition_set_t;

/* Reads a condition file from its bytes (UTF-8 JSON, length bytes, no
 * BOM). ECUIO_OK with *out a set the caller frees with
 * ecuio_condition_set_free; ECUIO_ERR_NULL_ARGUMENT for a NULL bytes
 * (with a length) or out; ECUIO_ERR_INVALID_ARGUMENT for a file it
 * refuses, with ecuio_last_error_detail() saying where and why ("line
 * 12, column 9: unknown key 'abov' in condition 'pull''s when", never a
 * path); ECUIO_ERR_OUT_OF_MEMORY. *out is NULL on any failure. The set
 * keeps the bytes it was read from. Thread-safe: each call is its own,
 * and a loaded set only reads. */
ecuio_status_t ecuio_condition_set_load(const char* bytes, size_t length,
                                        ecuio_condition_set_t** out);

/* Frees a set; NULL is a no-op */
void ecuio_condition_set_free(ecuio_condition_set_t* set);

/* The file's conditions, 0 to ECUIO_CONDITION_MAX_CONDITIONS; 0 for NULL */
uint32_t ecuio_condition_set_count(const ecuio_condition_set_t* set);

/* The file's "name", "" when it gives none or for NULL. Valid until
 * ecuio_condition_set_free. */
const char* ecuio_condition_set_name(const ecuio_condition_set_t* set);

/* The file's "checks" as the frames check takes them (recording.h): a
 * field the file leaves out is 0, the check's default
 * (ECUIO_CHECK_DEFAULT_*). ECUIO_ERR_NULL_ARGUMENT for a NULL argument. */
ecuio_status_t ecuio_condition_set_checks(const ecuio_condition_set_t* set,
                                          ecuio_check_options_t* out);

/* ecuio_condition_info_t.flags. Values are append-only. */
#define ECUIO_CONDITION_INFO_DISABLED 0x1u /* "enabled": false */
#define ECUIO_CONDITION_INFO_ANY      0x2u /* its terms join by any; clear,
                                              by all (one term reads the
                                              same either way) */

/* ecuio_condition_info_t.uses. Values are append-only. */
#define ECUIO_CONDITION_USE_ALERT    0x1u
#define ECUIO_CONDITION_USE_RECORD   0x2u
#define ECUIO_CONDITION_USE_MARK     0x4u
#define ECUIO_CONDITION_ALERT_SOUND  0x10u /* with _USE_ALERT */
#define ECUIO_CONDITION_ALERT_BANNER 0x20u /* with _USE_ALERT */

/* ecuio_condition_info_t.alert_level. Values are append-only. */
#define ECUIO_ALERT_NONE    0u /* no alert */
#define ECUIO_ALERT_WARNING 1u
#define ECUIO_ALERT_DANGER  2u

/* One condition as the file wrote it. key and name are borrowed: valid
 * until ecuio_condition_set_free. */
typedef struct ecuio_condition_info {
    uint32_t flags;        /* ECUIO_CONDITION_INFO_*; any other bit zero */
    uint32_t uses;         /* ECUIO_CONDITION_USE_* and _ALERT_* */
    uint32_t alert_level;  /* ECUIO_ALERT_* */
    uint32_t term_count;   /* 1 to ECUIO_CONDITION_MAX_TERMS */
    uint32_t for_us;       /* its terms hold this long before ON */
    uint32_t clear_for_us; /* and are false this long before OFF */
    uint32_t before_us;    /* a recording's; 0 without _USE_RECORD */
    uint32_t after_us;
    uint64_t reserved[2];  /* zero */
    const char* key;
    const char* name;
} ecuio_condition_info_t;

/* ECUIO_OK with *out filled; ECUIO_ERR_NULL_ARGUMENT for a NULL set or
 * out; ECUIO_ERR_INVALID_ARGUMENT for an index past the count (out
 * untouched). */
ecuio_status_t ecuio_condition_set_get(const ecuio_condition_set_t* set,
                                       uint32_t index,
                                       ecuio_condition_info_t* out);

/* ecuio_condition_reference_t.kind. Values are append-only. */
#define ECUIO_REFERENCE_ID  0u /* "id:<n>", or "obd2.<name>" read as that
                                  def's id */
#define ECUIO_REFERENCE_KEY 1u /* "key:<key>": the key starts at text + 4 */

/* One of a signal term's references */
typedef struct ecuio_condition_reference {
    uint32_t kind;     /* ECUIO_REFERENCE_* */
    uint32_t id;       /* _ID: the def's id; 0 for _KEY */
    char     text[40]; /* as the file wrote it, terminated */
} ecuio_condition_reference_t;

/* ecuio_condition_term_t.kind. Values are append-only. */
#define ECUIO_TERM_SIGNAL 0u
#define ECUIO_TERM_LINK   1u
#define ECUIO_TERM_CHECK  2u

/* ecuio_condition_term_t.test of a signal term. Values are append-only. */
#define ECUIO_TERM_ABOVE   1u
#define ECUIO_TERM_BELOW   2u
#define ECUIO_TERM_EQUALS  3u
#define ECUIO_TERM_DIFFERS 4u

/* .test of a link term: the link state it names. Values are append-only. */
#define ECUIO_TERM_LINK_NOT_OK         1u /* any state but ECUIO_OK */
#define ECUIO_TERM_LINK_TIMEOUT        2u /* ECUIO_ERR_VEHICLE_TIMEOUT */
#define ECUIO_TERM_LINK_REQUEST_FAILED 3u /* ECUIO_ERR_REQUEST_FAILED */
#define ECUIO_TERM_LINK_HARD_FAILURE   4u /* ECUIO_ERR_TRANSPORT_HARD_FAILURE */

/* .test of a check term is the finding's kind (recording.h):
 * ECUIO_CHECK_GAP, _RATE_DROP, _LATE_ID, _OVERFLOW or _ISOTP. */

/* ecuio_condition_term_t.flags. Values are append-only. */
#define ECUIO_TERM_HAS_CLEAR 0x1u /* clear is given */
#define ECUIO_TERM_HAS_UNIT  0x2u /* the file wrote "unit" */

/* One term as the file wrote it.
 *
 * A signal term's unit is the unit its level is in, and it is held to
 * the signal it reads: when the set's def has another unit, byte for
 * byte, the term can't be judged (ECUIO_CONDITION_WHY_UNIT), so a level
 * in kPa is never compared with a value in psi. A term that leaves
 * "unit" out (only equals and differs may) has the unit "", which
 * matches a def with no unit and no other: equals on a signal in RPM
 * names "RPM" too. */
typedef struct ecuio_condition_term {
    uint32_t kind;            /* ECUIO_TERM_* */
    uint32_t test;            /* per kind, from the lists above */
    double   level;           /* a signal term's; 0 otherwise */
    double   clear;           /* with ECUIO_TERM_HAS_CLEAR; 0 otherwise */
    uint32_t max_age_us;      /* a signal term's age bound,
                                 ECUIO_COMPUTED_MIN_AGE_US to _MAX_; 0 for
                                 ECUIO_VALUE_STALE_US */
    uint32_t flags;           /* ECUIO_TERM_HAS_*; any other bit zero */
    uint32_t reference_count; /* a signal term: 1 to
                                 ECUIO_CONDITION_MAX_REFERENCES; else 0 */
    uint32_t reserved0;       /* zero */
    char     unit[16];        /* UTF-8, terminated; "" with none */
    uint64_t reserved[2];     /* zero */
    ecuio_condition_reference_t references[ECUIO_CONDITION_MAX_REFERENCES];
} ecuio_condition_term_t;

/* ECUIO_OK with *out filled; ECUIO_ERR_NULL_ARGUMENT for a NULL set or
 * out; ECUIO_ERR_INVALID_ARGUMENT for a condition past the count or a
 * term past the condition's (out untouched). */
ecuio_status_t ecuio_condition_set_get_term(const ecuio_condition_set_t* set,
                                            uint32_t condition, uint32_t term,
                                            ecuio_condition_term_t* out);

/* Why a condition can't be judged against a set of signals. Values are
 * append-only. */
#define ECUIO_CONDITION_WHY_NONE         0u
#define ECUIO_CONDITION_WHY_DISABLED     1u /* "enabled": false */
#define ECUIO_CONDITION_WHY_NO_SIGNAL    2u /* none of a term's references
                                               names a signal of the set */
#define ECUIO_CONDITION_WHY_UNIT         3u /* the signal's unit isn't the
                                               term's */
#define ECUIO_CONDITION_WHY_NOT_A_NUMBER 4u /* a text or hex signal */
#define ECUIO_CONDITION_WHY_ENUM_ORDER   5u /* above or below on an
                                               enumeration */

/* How one condition's terms meet a set of signals */
typedef struct ecuio_condition_binding {
    uint32_t why;        /* ECUIO_CONDITION_WHY_*; _NONE: every term binds */
    uint32_t why_term;   /* the first term that doesn't; UINT32_MAX for
                            _NONE and _DISABLED */
    uint32_t term_count; /* the condition's */
    uint32_t reserved0;  /* zero */
    /* For each term: which of its references was taken, and the id of
     * the def it names; UINT32_MAX in both for a link or check term, one
     * past term_count, and a signal term no reference of which the set
     * provides. A term that bound to a def it can't be judged on (its
     * unit, its kind) still names that def. */
    uint32_t reference[ECUIO_CONDITION_MAX_TERMS];
    uint32_t signal_id[ECUIO_CONDITION_MAX_TERMS];
    uint64_t reserved[2]; /* zero */
} ecuio_condition_binding_t;

/* Resolves condition index against a set of signals, as a session binds
 * it. Each signal term takes the first of its references the set
 * provides and looks no further:
 * - "id:<n>" names the def with that id, a base def a signal file
 *   replaced included. An id from 20000 up names nothing: from there
 *   ids are numbered by order (uds-generic's by its URI's DIDs, a signal
 *   file's own by its list), so none is a stable name, and a file's own
 *   signal is named by its key.
 * - "key:<key>" names file's signal with that key, and nothing without
 *   a file.
 * The def taken must be a number, an enumeration (equals and differs
 * only) or a computed signal, in the term's unit. A link or a check
 * term binds to any set.
 *
 * signals is the set (ecuio_profile_lookup's, ecuio_signal_file_set's,
 * ecuio_session_signal_set's), or NULL for none, which provides no
 * signal. file is the signal file whose set signals is, or NULL.
 * ECUIO_OK with *out filled; ECUIO_ERR_NULL_ARGUMENT for a NULL set or
 * out; ECUIO_ERR_INVALID_ARGUMENT for an index past the count or a file
 * whose set isn't signals (out untouched). */
ecuio_status_t ecuio_condition_set_resolve(const ecuio_condition_set_t* set,
                                           uint32_t index,
                                           const ecuio_signal_set_t* signals,
                                           const ecuio_signal_file_t* file,
                                           ecuio_condition_binding_t* out);

#if defined(__cplusplus)
static_assert(sizeof(ecuio_condition_info_t) == 48 + 2 * sizeof(void*), "ecuio_condition_info_t size is ABI");
static_assert(offsetof(ecuio_condition_info_t, uses) == 4, "info uses offset is ABI");
static_assert(offsetof(ecuio_condition_info_t, term_count) == 12, "info term_count offset is ABI");
static_assert(offsetof(ecuio_condition_info_t, for_us) == 16, "info for_us offset is ABI");
static_assert(offsetof(ecuio_condition_info_t, before_us) == 24, "info before_us offset is ABI");
static_assert(offsetof(ecuio_condition_info_t, reserved) == 32, "info reserved offset is ABI");
static_assert(offsetof(ecuio_condition_info_t, key) == 48, "info key offset is ABI");
static_assert(offsetof(ecuio_condition_info_t, name) == 48 + sizeof(void*), "info name offset is ABI");
static_assert(sizeof(ecuio_condition_reference_t) == 48, "ecuio_condition_reference_t size is ABI");
static_assert(offsetof(ecuio_condition_reference_t, id) == 4, "reference id offset is ABI");
static_assert(offsetof(ecuio_condition_reference_t, text) == 8, "reference text offset is ABI");
static_assert(sizeof(ecuio_condition_term_t) == 456, "ecuio_condition_term_t size is ABI");
static_assert(offsetof(ecuio_condition_term_t, test) == 4, "term test offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, level) == 8, "term level offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, clear) == 16, "term clear offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, max_age_us) == 24, "term max_age_us offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, reference_count) == 32, "term reference_count offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, unit) == 40, "term unit offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, reserved) == 56, "term reserved offset is ABI");
static_assert(offsetof(ecuio_condition_term_t, references) == 72, "term references offset is ABI");
static_assert(sizeof(ecuio_condition_binding_t) == 64, "ecuio_condition_binding_t size is ABI");
static_assert(offsetof(ecuio_condition_binding_t, why_term) == 4, "binding why_term offset is ABI");
static_assert(offsetof(ecuio_condition_binding_t, reference) == 16, "binding reference offset is ABI");
static_assert(offsetof(ecuio_condition_binding_t, signal_id) == 32, "binding signal_id offset is ABI");
static_assert(offsetof(ecuio_condition_binding_t, reserved) == 48, "binding reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_condition_info_t) == 48 + 2 * sizeof(void*), "ecuio_condition_info_t size is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, uses) == 4, "info uses offset is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, term_count) == 12, "info term_count offset is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, for_us) == 16, "info for_us offset is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, before_us) == 24, "info before_us offset is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, reserved) == 32, "info reserved offset is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, key) == 48, "info key offset is ABI");
_Static_assert(offsetof(ecuio_condition_info_t, name) == 48 + sizeof(void*), "info name offset is ABI");
_Static_assert(sizeof(ecuio_condition_reference_t) == 48, "ecuio_condition_reference_t size is ABI");
_Static_assert(offsetof(ecuio_condition_reference_t, id) == 4, "reference id offset is ABI");
_Static_assert(offsetof(ecuio_condition_reference_t, text) == 8, "reference text offset is ABI");
_Static_assert(sizeof(ecuio_condition_term_t) == 456, "ecuio_condition_term_t size is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, test) == 4, "term test offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, level) == 8, "term level offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, clear) == 16, "term clear offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, max_age_us) == 24, "term max_age_us offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, reference_count) == 32, "term reference_count offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, unit) == 40, "term unit offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, reserved) == 56, "term reserved offset is ABI");
_Static_assert(offsetof(ecuio_condition_term_t, references) == 72, "term references offset is ABI");
_Static_assert(sizeof(ecuio_condition_binding_t) == 64, "ecuio_condition_binding_t size is ABI");
_Static_assert(offsetof(ecuio_condition_binding_t, why_term) == 4, "binding why_term offset is ABI");
_Static_assert(offsetof(ecuio_condition_binding_t, reference) == 16, "binding reference offset is ABI");
_Static_assert(offsetof(ecuio_condition_binding_t, signal_id) == 32, "binding signal_id offset is ABI");
_Static_assert(offsetof(ecuio_condition_binding_t, reserved) == 48, "binding reserved offset is ABI");
#endif

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_CONDITIONS_H */
