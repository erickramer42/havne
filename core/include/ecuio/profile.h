// File/layer: core/include/ecuio/profile.h | C ABI, public
// Testing: UNIT (registry lookup/overwrite); session-level selection and
//          the decoded values (CoreTest.Values*) and the absent signals
//          (CoreTest.AFieldPastEveryReplyIsAbsent) in test_core; the value
//          layouts in c_abi; BRIDGE (BridgeTest.BothDecodePathsAgreeOverTheFixture)

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
 * "raw" is the null profile: registered with a NULL signal set, no
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
 * the scheme default or ?profile=). NULL means "no decode": the raw
 * profile. Written once at create, read-only afterwards. */
const ecuio_signal_set_t* ecuio_session_signal_set(ecuio_session_t* session);

/*
 * Decoded values (ADR-0024).
 *
 * While a values callback is registered, the session decodes each frame
 * against its set on the transport's worker, right after the frame
 * callbacks have returned, and delivers what the frame decoded as one
 * batch: every def it matches and decodes (ecuio_def_applies_to_frame,
 * then ecuio_decode_signal or, for TEXT and HEX defs,
 * ecuio_decode_text), in the set's order, stamped with the frame's
 * session time. A def whose decode fails is left out; a frame with no
 * value, or a raw session, delivers no batch. Nothing is decoded while
 * no values callback is registered.
 *
 * The values callback keeps the frame callback's contract (core.h):
 * it runs on the worker and must not block, must never call
 * ecuio_session_close or ecuio_session_destroy, may register or
 * unregister reentrantly, sees the list as it stood when the frame
 * arrived (so after unregister returns, a dispatch under way may call
 * it once more: keep user_data valid until ecuio_session_close
 * returns), and is identified by the (callback, user_data) pair,
 * deduplicated, with an idempotent unregister. Besides:
 * - for each frame, every frame callback returns before any values
 *   callback is called with that frame's batch;
 * - the batch, its values and their text are valid only during the
 *   call. Each text value has bytes of its own.
 */

/* One decoded value. The pointer is last, so every other field's
 * offset is the same on 32- and 64-bit builds. */
typedef struct ecuio_value {
    uint16_t signal_id;    /* the def's id */
    uint8_t  kind;         /* the def's ECUIO_VALUE_* */
    uint8_t  flags;        /* 0; reserved */
    uint32_t text_len;     /* TEXT and HEX: the bytes at text, the NUL
                              not counted; 0 otherwise */
    double   value;        /* the numeric kinds' value; 0 for text */
    const char* text;      /* TEXT and HEX: NUL-terminated, valid during
                              the call; NULL otherwise */
} ecuio_value_t;

/* What one frame decoded to */
typedef struct ecuio_value_batch {
    uint64_t timestamp_us;      /* the frame's session time */
    uint32_t arbitration_id;    /* the frame's */
    uint32_t count;             /* values[0..count), at least 1 */
    uint32_t flags;             /* 0; reserved */
    uint32_t reserved;
    const ecuio_value_t* values;
} ecuio_value_batch_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_value_t, value) == 8, "value offset is ABI");
static_assert(offsetof(ecuio_value_t, text) == 16, "text offset is ABI");
static_assert(sizeof(void*) != 8 || sizeof(ecuio_value_t) == 24, "ecuio_value_t size is ABI");
static_assert(offsetof(ecuio_value_batch_t, values) == 24, "batch values offset is ABI");
static_assert(sizeof(void*) != 8 || sizeof(ecuio_value_batch_t) == 32, "ecuio_value_batch_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_value_t, value) == 8, "value offset is ABI");
_Static_assert(offsetof(ecuio_value_t, text) == 16, "text offset is ABI");
_Static_assert(sizeof(void*) != 8 || sizeof(ecuio_value_t) == 24, "ecuio_value_t size is ABI");
_Static_assert(offsetof(ecuio_value_batch_t, values) == 24, "batch values offset is ABI");
_Static_assert(sizeof(void*) != 8 || sizeof(ecuio_value_batch_t) == 32, "ecuio_value_batch_t size is ABI");
#endif

/* Called from the transport's worker with one frame's values */
typedef void (*ecuio_values_callback_t)(
    const ecuio_value_batch_t* batch,
    void* user_data
);

/* Register and unregister a values callback, as the frame callback's
 * pair does (core.h): ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL
 * session or callback (unregister: a NULL session);
 * ECUIO_ERR_SESSION_CLOSED to register on a closed session;
 * ECUIO_ERR_OUT_OF_MEMORY if the list can't grow. */
ecuio_status_t ecuio_register_values_callback(
    ecuio_session_t* session,
    ecuio_values_callback_t callback,
    void* user_data
);

ecuio_status_t ecuio_unregister_values_callback(
    ecuio_session_t* session,
    ecuio_values_callback_t callback,
    void* user_data
);

/*
 * The signals the session's frames name but don't carry (ADR-0039 §6):
 * a def whose key matched ECUIO_ABSENT_AFTER_FRAMES or more frames, each
 * ending before its field (ECUIO_DECODE_ERR_PAST_PAYLOAD), while none
 * carried it, as an ECU that answers PID 55 with bank 1's byte alone.
 * A signal that arrives later leaves the list. Judged from the frames
 * decoded for a values callback, so while none is registered nothing
 * is judged.
 *
 * Fills ids with up to capacity signal ids, in the set's order, and
 * *count with how many there are; capacity 0 (ids may be NULL) asks
 * the count. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL session or
 * count, or NULL ids with a capacity. Readable after close.
 */
#define ECUIO_ABSENT_AFTER_FRAMES 3u

ecuio_status_t ecuio_session_get_absent_signals(
    const ecuio_session_t* session,
    uint16_t* ids,
    uint32_t capacity,
    uint32_t* count
);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_PROFILE_H */
