// File/layer: core/include/ecuio/profile.h | C ABI, public
// Testing: UNIT: CoreTest (test_core); RequestTimingTest (test_obd2); BRIDGE:
//          BridgeTest (test_bridge)

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
 * registered and until every session created with it is destroyed
 * (and every simulated ECU answering it, sim.h's profile=);
 * static storage is the simple way. Replacing or unregistering a name
 * leaves existing sessions on their set.
 *
 * Register, unregister and lookup are thread-safe.
 */

/* The longest profile name: what a ?profile= value can carry. */
#define ECUIO_PROFILE_NAME_MAX 63

/* The most requests a set's defs may derive (ADR-0015 section 2): one per
 * distinct request and channel, so defs read from one reply count
 * once. */
#define ECUIO_PROFILE_MAX_REQUESTS 256

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
 * ecuio_decode_text), in the set's order, then each COMPUTED def one
 * of them feeds (ADR-0051 section 5), in the set's order, all stamped
 * with the frame's session time. A def whose decode fails is left out,
 * as is a computed value with an input past its age bound or a result
 * that isn't finite; a frame with no value, or a raw session, delivers
 * no batch. Nothing is decoded while no values callback is
 * registered.
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
 *   call. Each TEXT or HEX value has bytes of its own; an ENUM's label
 *   is the set's own.
 * - a value's kind says what it holds, never its text pointer: an ENUM
 *   is a number that may carry its label.
 */

/* One decoded value. The pointer is last, so every other field's
 * offset is the same on 32- and 64-bit builds. */
typedef struct ecuio_value {
    uint16_t signal_id;    /* the def's id */
    uint8_t  kind;         /* the def's ECUIO_VALUE_* */
    uint8_t  flags;        /* 0; reserved */
    uint32_t text_len;     /* TEXT and HEX, and a labeled ENUM: the bytes
                              at text, the NUL not counted; 0 otherwise */
    double   value;        /* the numeric kinds' value (an ENUM's raw
                              number, a COMPUTED def's result, kind
                              UINT); 0 for text */
    const char* text;      /* TEXT and HEX: NUL-terminated, valid during
                              the call; an ENUM: its label
                              (ecuio_enum_label), NULL where none;
                              NULL otherwise */
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
 * The signals the session's frames name but don't carry (ADR-0039 section 6):
 * a def whose key matched ECUIO_ABSENT_AFTER_FRAMES or more frames, each
 * ending before its field (ECUIO_DECODE_ERR_PAST_PAYLOAD), while none
 * carried it, as an ECU that answers PID 55 with bank 1's byte alone.
 * A signal that arrives later leaves the list. Judged from the frames
 * decoded for a values callback, so while none is registered nothing
 * is judged. A computed signal (ECUIO_DEF_FLAG_COMPUTED) with such an
 * input is listed with it: it can't be computed.
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

/*
 * The Service 01 signals their ECU says it doesn't support (ADR-0039
 * section 6): a def keyed 41 <PID> at the payload's start whose PID the
 * supported-PID bitmaps of the channel its message ID names leave out
 * (ecuio_session_get_channel_support). A range whose bitmap was never
 * answered judges nothing, unless the walk completed with the chain
 * ended before it (the last answered bitmap's link to the next clear):
 * then every PID of that range is unsupported. A channel that has
 * learned nothing yet and every other def judge nothing. A rescan that
 * finds a PID takes its signals off the list. A computed signal
 * (ECUIO_DEF_FLAG_COMPUTED) with such an input is listed with it: it
 * can't be computed. mock://
 * answers the demo car's PIDs, and no PID for its other ECUs, so under
 * a profile with Service 01 signals it lists those the demo car doesn't
 * support and every one of another ECU's.
 *
 * Fills ids with up to capacity signal ids, in the set's order, and
 * *count with how many there are; capacity 0 (ids may be NULL) asks
 * the count. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL session or
 * count, or NULL ids with a capacity. Readable after close.
 */
ecuio_status_t ecuio_session_get_unsupported_signals(
    const ecuio_session_t* session,
    uint16_t* ids,
    uint32_t capacity,
    uint32_t* count
);

/*
 * Service 01 replies that carried more than their first PID's record
 * but wouldn't split into records (ADR-0039 Appendix A): a PID of no
 * known length, one twice, or records that don't end at the reply's
 * end. Each decoded its first record alone. Core splits a reply that
 * carries several PIDs (01 0C 0D ...) record by record, using J1979's
 * length for each PID, or the shorter one the ECU's single replies
 * show. Counted from the frames decoded for a values callback.
 *
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL session or count.
 * Readable after close.
 */
ecuio_status_t ecuio_session_get_unsplit_replies(
    const ecuio_session_t* session,
    uint64_t* count
);

/*
 * Reply timing per request (ADR-0042): for each request in a session's
 * rotation (a PID, or a DID under uds-generic), its outcomes, its reply
 * latency (the request's write to the reply that completes it, on the
 * host's clock) and its refresh interval (one answer to the next, on
 * the frames' own stamps: interval_clock says whose). Quantiles read at
 * most 12.5 % above the true value; a P99 needs ECUIO_TIMING_P99_SAMPLES
 * samples and a P99.9 ECUIO_TIMING_P999_SAMPLES, and below that it is 0
 * with its flag clear. Figures are cumulative from the connect; the
 * worker publishes them at most once a second and once as it stops.
 * The values below are append-only.
 */
#define ECUIO_TIMING_P99_SAMPLES   100u
#define ECUIO_TIMING_P999_SAMPLES  1000u

/* interval_clock; KERNEL is the kernel's receive times, as wall-clock
 * deltas */
#define ECUIO_TIMING_CLOCK_NONE    0u   /* no rotation */
#define ECUIO_TIMING_CLOCK_HOST    1u   /* as the worker read them (ELM327) */
#define ECUIO_TIMING_CLOCK_ADAPTER 2u   /* the adapter's (J2534) */
#define ECUIO_TIMING_CLOCK_KERNEL  3u   /* the kernel's (SocketCAN) */

/* ecuio_session_timing_t flags */
#define ECUIO_TIMING_ISOTP         0x00000001u   /* core reassembles */

/* ecuio_request_timing_t flags: a quantile with enough samples */
#define ECUIO_TIMING_LATENCY_P99   0x00000001u
#define ECUIO_TIMING_LATENCY_P999  0x00000002u
#define ECUIO_TIMING_INTERVAL_P99  0x00000004u
#define ECUIO_TIMING_INTERVAL_P999 0x00000008u

/* The columns of a recording's latency.csv and the keys of havne-cli's
 * timing lines, in order (ADR-0042 section 3): a snapshot's session
 * time, the channel's reply ID, the request's bytes, then a row's
 * fields, then the interval's clock. One list, so the two writers stay
 * alike. */
#define ECUIO_TIMING_COLUMNS                                               \
    "t_s,ecu,request,answered,refused,timeouts,packed,"                    \
    "latency_p50_us,latency_p99_us,latency_p999_us,latency_max_us,"        \
    "intervals,intervals_mixed,interval_p50_us,interval_p99_us,"           \
    "interval_p999_us,interval_max_us,interval_clock"

typedef struct ecuio_session_timing {
    uint64_t snapshot_us;        /* the session time the rows were published */
    uint32_t flags;              /* ECUIO_TIMING_ISOTP */
    uint32_t interval_clock;     /* ECUIO_TIMING_CLOCK_* */
    uint32_t rows;               /* requests with a row */
    /* ISO-TP's receiver, where core reassembles (ADR-0040 section 5) */
    uint32_t isotp_first_frames; /* segmented messages begun */
    uint32_t isotp_completed;    /* messages finished, single frames too */
    uint32_t isotp_dropped;      /* begun and never finished */
    uint32_t isotp_malformed;    /* frames no message could use */
    uint32_t reserved[7];
} ecuio_session_timing_t;

typedef struct ecuio_request_timing {
    uint8_t  channel;
    uint8_t  length;             /* request[0..length): the service, the key */
    uint8_t  request[3];
    uint8_t  reserved0[3];
    uint32_t flags;              /* ECUIO_TIMING_LATENCY_P99 ... */
    uint32_t answered;           /* positive replies */
    uint32_t refused;            /* negative replies */
    uint32_t timeouts;           /* waits that ended with nothing */
    uint32_t packed;             /* answers and refusals that came to a pack */
    uint32_t latency_p50_us;     /* over answers and refusals */
    uint32_t latency_p99_us;
    uint32_t latency_p999_us;
    uint32_t latency_max_us;
    uint32_t intervals;          /* between answers, on interval_clock */
    uint32_t intervals_mixed;    /* left out: a fallback to the host's
                                    delta between their two answers */
    uint32_t interval_p50_us;
    uint32_t interval_p99_us;
    uint32_t interval_p999_us;
    uint32_t interval_max_us;
    uint32_t reserved[3];
} ecuio_request_timing_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_session_timing_t) == 64, "ecuio_session_timing_t size is ABI");
static_assert(sizeof(ecuio_request_timing_t) == 80, "ecuio_request_timing_t size is ABI");
static_assert(offsetof(ecuio_request_timing_t, flags) == 8, "timing flags offset is ABI");
static_assert(offsetof(ecuio_request_timing_t, intervals) == 44, "intervals offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_session_timing_t) == 64, "ecuio_session_timing_t size is ABI");
_Static_assert(sizeof(ecuio_request_timing_t) == 80, "ecuio_request_timing_t size is ABI");
_Static_assert(offsetof(ecuio_request_timing_t, flags) == 8, "timing flags offset is ABI");
_Static_assert(offsetof(ecuio_request_timing_t, intervals) == 44, "intervals offset is ABI");
#endif

/*
 * Copies the session's timing report and up to capacity rows, in the
 * engine's table order, and sets *count to how many rows there are;
 * capacity 0 (rows may be NULL) asks the count. A session with no
 * rotation (mock, replay) has none. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for
 * a NULL session, timing or count, or NULL rows with a capacity.
 * Readable at any time and after close.
 */
ecuio_status_t ecuio_session_get_timing(
    const ecuio_session_t* session,
    ecuio_session_timing_t* timing,
    ecuio_request_timing_t* rows,
    uint32_t capacity,
    uint32_t* count
);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_PROFILE_H */
