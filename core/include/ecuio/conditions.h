// File/layer: core/include/ecuio/conditions.h | C ABI, public
// Testing: UNIT: ConditionFileTest, ConditionEvalTest (test_signals),
//          ConditionSessionTest, ConditionRecordingTest, QueueTest
//          (test_core); c_abi; FUZZ: fuzz_condition_file

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
                                 its signal's pace (below) */
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
#define ECUIO_CONDITION_WHY_NO_LINK      6u /* a link term, and the session
                                               gives it no link state to
                                               read (mock://, replay://) */
#define ECUIO_CONDITION_WHY_NO_CHECK     7u /* a check term, and the session
                                               doesn't run that check on its
                                               frames */
#define ECUIO_CONDITION_WHY_NOT_RECEIVED 8u /* a state's alone: a term reads
                                               a signal the session reports
                                               it doesn't get (the states'
                                               getter, below) */

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
 * signal_set is the set (ecuio_profile_lookup's, ecuio_signal_file_set's,
 * ecuio_session_signal_set's), or NULL for none, which provides no
 * signal. file is the signal file whose set signal_set is, or NULL.
 * ECUIO_OK with *out filled; ECUIO_ERR_NULL_ARGUMENT for a NULL set or
 * out; ECUIO_ERR_INVALID_ARGUMENT for an index past the count or a file
 * whose set isn't signal_set (out untouched). The parameter isn't named
 * signals, which a Qt source defines as a macro. */
ecuio_status_t ecuio_condition_set_resolve(const ecuio_condition_set_t* set,
                                           uint32_t index,
                                           const ecuio_signal_set_t* signal_set,
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

/* ---- Conditions in a session (ADR-0063 sections 2, 3 and 6) ----
 * A set attached to a session is judged on its worker, frame by frame,
 * on the frames' own time: a term keeps the truth of its newest sample,
 * a sample older than the term's age bound can't be judged, and a
 * condition whose terms can't be judged keeps its state and is STALE.
 * The bound is the term's max_age, or else its signal's pace: three
 * times the larger of the last gap between its samples and the gaps'
 * running average (each new gap weighted a quarter, the first its
 * start, each counted up to ECUIO_COMPUTED_MAX_AGE_US), from
 * ECUIO_VALUE_STALE_US to three times ECUIO_COMPUTED_MAX_AGE_US, and
 * ECUIO_VALUE_STALE_US alone before a second sample. So a slow sweep
 * isn't STALE between its samples, and a signal that stops still is.
 * A value that is no number (a NaN) is no sample; an infinity is one.
 * Time never goes back: a frame stamped earlier than the one before it
 * (two channels' frames can arrive a little out of order) is judged at
 * that one's time.
 * A hold's end and an age bound are settled by the next frame, at their
 * own time, so the same frames give the same events however fast they
 * are played. A bus that goes wholly quiet settles nothing until its
 * next frame.
 * A link term reads the session's link state, the code
 * ecuio_session_get_error_state gives, on a session that judges its
 * link (j2534://, elm327://, socketcan://). Each change of that state is
 * judged where the session's worker publishes it, at the session's time
 * of the change, as a frame is: every deadline before it first, on the
 * state before. A change that ends a fault is stamped no earlier than the
 * fault's start plus the host's time since, whatever the stamps of frames
 * an adapter held through it. A link term has no age bound, and a set
 * attached starts from the state as it stands. A change of the link
 * settles a hold's end and an age bound as a frame does, so on a link that
 * has gone quiet a hold on a link term ends at the next frame or change of
 * the link. */

/* A condition's state (an event's and a state's). Values are
 * append-only. */
#define ECUIO_CONDITION_OFF 0u
#define ECUIO_CONDITION_ON  1u

/* An event's and a state's flags. Values are append-only. */
#define ECUIO_CONDITION_STALE       0x1u /* an input is too old to judge:
                                            the state is the one kept */
#define ECUIO_CONDITION_FIRED       0x2u /* an event's alone: this change
                                            is OFF to ON */
#define ECUIO_CONDITION_UNAVAILABLE 0x4u /* a state's alone: the session
                                            can't judge it to ON, and why
                                            says why: a bind's reason
                                            (never judged), or the states'
                                            getter's NOT_RECEIVED, which
                                            comes and goes while the
                                            condition's events still come */
#define ECUIO_CONDITION_HELD        0x8u /* a recording's event alone: the
                                            state the condition stood in
                                            when the recording began, no
                                            change (ecuio/recording.h) */
#define ECUIO_CONDITION_LINK        0x10u /* a session's event alone: a
                                            change of the link's state,
                                            or a set's first judgment on
                                            it, settled it, with no frame
                                            of its own (a recording's
                                            event never carries it) */

/* Binds the set to the session's signals, as ecuio_condition_set_resolve
 * does, and judges it from the next frame; on a session that judges its
 * link, a set with a link term is judged on the link's state at the
 * worker's next pass when no frame comes first (a pass waits out the
 * link's backoff and the adapter's own waits), so one attached during a
 * fault reports it. A NULL set takes the conditions away. file is the signal file whose set the session
 * decodes with, for key: references, or NULL. The session copies what it
 * needs: the set may be freed once this returns. Before or after
 * ecuio_session_start. Every state starts OFF and STALE, or OFF and
 * UNAVAILABLE (with no STALE) for a condition that can't be judged, and
 * each call that succeeds begins a new generation: 1 for a session's
 * first, one more for each after it, a removal included.
 *
 * A check term is not judged: its condition is unavailable
 * (ECUIO_CONDITION_WHY_NO_CHECK). A link term is judged on a session
 * that judges its link and unavailable on any other
 * (ECUIO_CONDITION_WHY_NO_LINK: mock://, replay://).
 *
 * Under a focus (ecuio_session_set_focus) the session keeps asking the
 * signals the conditions it judges read, a computed one's inputs among
 * them, whether or not the focus picks them; a disabled or unavailable
 * condition keeps nothing asked. The focus's own picks, and the
 * recording's focus line, stay the caller's.
 *
 * A recorder attached holds the set for the recording's length: the set
 * and every event it gives go into the recording (conditions.json, the
 * cond lines of events.log, a marker for each firing of a condition with
 * mark; ecuio/recording.h). A full ring's events wait in a carry of 256;
 * past it they are counted in the recorder's conditions_lost and said in
 * a drop line.
 *
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL session;
 * ECUIO_ERR_SESSION_CLOSED after close; ECUIO_ERR_INVALID_ARGUMENT for a
 * file whose set isn't the session's; ECUIO_ERR_IN_USE while a recorder
 * is attached, since a recording holds one set for its whole length
 * (ADR-0063 section 12's step 7 decides a change of set within one);
 * ECUIO_ERR_OUT_OF_MEMORY. Nothing changes on a failure, and
 * ecuio_last_error_detail() says why. */
ecuio_status_t ecuio_session_set_conditions(ecuio_session_t* session,
                                            const ecuio_condition_set_t* set,
                                            const ecuio_signal_file_t* file);

/* One change of a condition's state or of its STALE flag; changes at one
 * time that one frame settles are one event. The first judgment after
 * an attach is an event (OFF with STALE clear, or ON with FIRED), an
 * attach itself is none. */
typedef struct ecuio_condition_event {
    uint64_t timestamp_us;   /* session time of the change: for a hold's
                                end or an age bound, the deadline's own
                                time, earlier than the frame that settled
                                it */
    uint32_t condition;      /* its index in the set attached */
    uint32_t state;          /* ECUIO_CONDITION_OFF, _ON */
    uint32_t flags;          /* ECUIO_CONDITION_STALE, _FIRED */
    uint32_t term;           /* the term that settled it: the one whose
                                sample or link state changed the join,
                                started the hold that ended, or passed
                                its age bound */
    double   value;          /* that term's newest sample; 0 for a link
                                term */
    uint32_t arbitration_id; /* 0 */
    int32_t  code;           /* a link term's: the link's state then
                                (ecuio_session_get_error_state's code);
                                0 for any other */
    uint32_t generation;     /* the attach it belongs to: an event of a
                                set since replaced carries the old one */
    uint32_t reserved0;      /* zero */
    uint64_t reserved;       /* zero */
} ecuio_condition_event_t;

/* A frame's events, on the session's worker, under the frame callback's
 * whole contract (core.h): after that frame's values callbacks, in one
 * call, ordered by time and then by index. A change of the link's state
 * gives its own call, on the same worker under the same contract, each
 * of its events flagged ECUIO_CONDITION_LINK. events is valid during the
 * call. */
typedef void (*ecuio_conditions_callback_t)(
    const ecuio_condition_event_t* events, uint32_t count, void* user_data);

/* Register and unregister a conditions callback, as the frame callback's
 * pair does (core.h): ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL
 * session or callback (unregister: a NULL session);
 * ECUIO_ERR_SESSION_CLOSED to register on a closed session (unregister
 * is allowed after the close, and idempotent); ECUIO_ERR_OUT_OF_MEMORY
 * if the list can't grow. */
ecuio_status_t ecuio_register_conditions_callback(
    ecuio_session_t* session, ecuio_conditions_callback_t callback,
    void* user_data);
ecuio_status_t ecuio_unregister_conditions_callback(
    ecuio_session_t* session, ecuio_conditions_callback_t callback,
    void* user_data);

typedef struct ecuio_condition_state {
    uint32_t state;        /* ECUIO_CONDITION_OFF, _ON */
    uint32_t flags;        /* ECUIO_CONDITION_STALE, _UNAVAILABLE */
    uint64_t since_us;     /* session time of its last event; 0 before one */
    uint64_t fired;        /* firings since the attach */
    double   value;        /* the settling term's sample at its last
                              event; 0 for a link term */
    uint32_t why;          /* with _UNAVAILABLE: ECUIO_CONDITION_WHY_* */
    uint32_t generation;   /* the attach these states belong to */
    uint64_t reserved[2];  /* zero */
} ecuio_condition_state_t;

/* Every condition's state, in the set's order: *out_count is how many
 * the set attached holds (0 with none), and the first capacity are
 * copied. Any thread; readable after close, when the states are the
 * last the session held. ECUIO_ERR_NULL_ARGUMENT for a NULL session or
 * out_count, or a NULL out with a capacity.
 *
 * Once the session has started, a condition reads
 * ECUIO_CONDITION_UNAVAILABLE with ECUIO_CONDITION_WHY_NOT_RECEIVED while
 * a term reads a signal the session reports it doesn't get and has had
 * no sample of since the attach: the signal is unsupported or absent
 * (ecuio/profile.h's lists), its request was dropped (a refusal, the
 * probe, a DID the scan hasn't found, a PID ?poll= doesn't ask), or it is
 * computed from such a signal. Any such term does it, since an all can't
 * turn ON without it, whatever its other terms read; with
 * ECUIO_CONDITION_INFO_ANY every term must be such, so an any with a term
 * on a signal received reads as that term judges it (STALE while it is
 * false). A term sampled once is judged as any other, and goes STALE if
 * its signal stops. Judged at each call from the lists as they stand, so
 * availability can change after the attach (a scanned DID found, a
 * re-walk): read it again rather than keep it. A term's later references
 * are never tried (ADR-0063 section 2: the first the set provides is the
 * one read). */
ecuio_status_t ecuio_session_get_condition_states(
    ecuio_session_t* session, ecuio_condition_state_t* out, uint32_t capacity,
    uint32_t* out_count);

#if defined(__cplusplus)
static_assert(sizeof(ecuio_condition_event_t) == 56, "ecuio_condition_event_t size is ABI");
static_assert(offsetof(ecuio_condition_event_t, condition) == 8, "event condition offset is ABI");
static_assert(offsetof(ecuio_condition_event_t, flags) == 16, "event flags offset is ABI");
static_assert(offsetof(ecuio_condition_event_t, value) == 24, "event value offset is ABI");
static_assert(offsetof(ecuio_condition_event_t, arbitration_id) == 32, "event arbitration_id offset is ABI");
static_assert(offsetof(ecuio_condition_event_t, generation) == 40, "event generation offset is ABI");
static_assert(offsetof(ecuio_condition_event_t, reserved) == 48, "event reserved offset is ABI");
static_assert(sizeof(ecuio_condition_state_t) == 56, "ecuio_condition_state_t size is ABI");
static_assert(offsetof(ecuio_condition_state_t, since_us) == 8, "state since_us offset is ABI");
static_assert(offsetof(ecuio_condition_state_t, fired) == 16, "state fired offset is ABI");
static_assert(offsetof(ecuio_condition_state_t, value) == 24, "state value offset is ABI");
static_assert(offsetof(ecuio_condition_state_t, why) == 32, "state why offset is ABI");
static_assert(offsetof(ecuio_condition_state_t, generation) == 36, "state generation offset is ABI");
static_assert(offsetof(ecuio_condition_state_t, reserved) == 40, "state reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_condition_event_t) == 56, "ecuio_condition_event_t size is ABI");
_Static_assert(offsetof(ecuio_condition_event_t, condition) == 8, "event condition offset is ABI");
_Static_assert(offsetof(ecuio_condition_event_t, flags) == 16, "event flags offset is ABI");
_Static_assert(offsetof(ecuio_condition_event_t, value) == 24, "event value offset is ABI");
_Static_assert(offsetof(ecuio_condition_event_t, arbitration_id) == 32, "event arbitration_id offset is ABI");
_Static_assert(offsetof(ecuio_condition_event_t, generation) == 40, "event generation offset is ABI");
_Static_assert(offsetof(ecuio_condition_event_t, reserved) == 48, "event reserved offset is ABI");
_Static_assert(sizeof(ecuio_condition_state_t) == 56, "ecuio_condition_state_t size is ABI");
_Static_assert(offsetof(ecuio_condition_state_t, since_us) == 8, "state since_us offset is ABI");
_Static_assert(offsetof(ecuio_condition_state_t, fired) == 16, "state fired offset is ABI");
_Static_assert(offsetof(ecuio_condition_state_t, value) == 24, "state value offset is ABI");
_Static_assert(offsetof(ecuio_condition_state_t, why) == 32, "state why offset is ABI");
_Static_assert(offsetof(ecuio_condition_state_t, generation) == 36, "state generation offset is ABI");
_Static_assert(offsetof(ecuio_condition_state_t, reserved) == 40, "state reserved offset is ABI");
#endif

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_CONDITIONS_H */
