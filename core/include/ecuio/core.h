// File/layer: core/include/ecuio/core.h | C ABI, public interface
// Testing: Public API contract: every function needs a corresponding test

#ifndef ECUIO_CORE_H
#define ECUIO_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declared opaque handle types */
typedef struct ecuio_session ecuio_session_t;

/* Frame flag bitfield. A zeroed frame == plain classic CAN data frame.
 * Bit positions are ABI-stable; new flags APPEND only, never renumber.
 */
#define ECUIO_FRAME_FLAG_EXTENDED        0x00000001u /* 29-bit arbitration ID */
#define ECUIO_FRAME_FLAG_REMOTE           0x00000002u /* remote transmission request */
#define ECUIO_FRAME_FLAG_FD               0x00000004u /* CAN-FD frame */
#define ECUIO_FRAME_FLAG_BITRATE_SWITCH   0x00000008u /* FD bitrate switch (BRS) */
#define ECUIO_FRAME_FLAG_ERROR            0x00000010u /* bus/controller error report */
#define ECUIO_FRAME_FLAG_TX_ECHO          0x00000020u /* echo of our own transmit */
#define ECUIO_FRAME_FLAG_TRUNCATED        0x00000040u /* the message was longer than
                                                         data[] holds; data_length
                                                         bytes of it are here */
#define ECUIO_FRAME_FLAG_OVERFLOW         0x00000080u /* the adapter's buffer
                                                         overflowed before this
                                                         frame: frames were lost
                                                         (J2534's BUFFER_OVERFLOW) */
#define ECUIO_FRAME_FLAG_REASSEMBLED      0x00000100u /* an ISO-TP message's
                                                         payload, reassembled
                                                         (core's or the
                                                         adapter's), not a
                                                         frame as it crossed
                                                         the bus (ADR-0049) */
#define ECUIO_FRAME_FLAG_FD_ESI           0x00000200u /* an FD frame's error
                                                         state indicator: its
                                                         sender was error
                                                         passive (ADR-0054) */
#define ECUIO_FRAME_FLAG_ACCESS_CUT       0x00000400u /* a recording's: a
                                                         security access's
                                                         27 or 67 on a bound
                                                         channel, its bytes
                                                         after the
                                                         sub-function zeroes,
                                                         their length and
                                                         SHA-256 in events.log's
                                                         cut line (ADR-0066
                                                         section 5) */

/* The most payload a frame carries: room for a whole ISO 15765-2 message
 * on classic CAN (4095 bytes, its 12-bit length), so a reassembled
 * reply arrives whole. */
#define ECUIO_FRAME_DATA_MAX 4096u

/* Frame structure: POD only, no pointers. Layout is pinned by the
 * assertions below: 4120 bytes, no hidden padding on any supported
 * platform (ADR-0006, appendix of 2026-10-23). The payload is inline,
 * so copying a frame copies all of it.
 */
typedef struct ecuio_frame {
    uint64_t timestamp_us;    /* Microseconds since the session's transport
                                 attached, monotonic per channel. Mock: the
                                 host's steady clock. J2534: the host's at the
                                 first frame, then the adapter's own deltas,
                                 carried across its 32-bit wrap */
    uint32_t arbitration_id;  /* CAN identifier: <= 0x7FF standard, up to
                                 0x1FFFFFFF when FLAG_EXTENDED set */
    uint16_t channel_id;      /* Bus/channel index only: never multiplexes
                                 an arbitration ID */
    uint16_t data_length;     /* Bytes used in data[]: 0..ECUIO_FRAME_DATA_MAX */
    uint8_t  data[ECUIO_FRAME_DATA_MAX];   /* a CAN frame's 8 or CAN-FD's 64
                                              bytes, or a whole ISO 15765
                                              message, CAN ID stripped */
    uint32_t flags;           /* ECUIO_FRAME_FLAG_* bitmask */
    uint32_t reserved;        /* Producers MUST leave zero; ABI growth space */
} ecuio_frame_t;

/* Compile-time ABI pinning. C++ and C11 paths; C99 falls back to the
 * negative-array-size idiom in tests/test_c_abi.c. */
#if defined(__cplusplus)
static_assert(sizeof(ecuio_frame_t) == 4120, "ecuio_frame_t size is ABI: bump version if this moves");
static_assert(offsetof(ecuio_frame_t, data) == 16, "frame data offset is ABI");
static_assert(offsetof(ecuio_frame_t, flags) == 4112, "frame flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_frame_t) == 4120, "ecuio_frame_t size is ABI");
_Static_assert(offsetof(ecuio_frame_t, data) == 16, "frame data offset is ABI");
_Static_assert(offsetof(ecuio_frame_t, flags) == 4112, "frame flags offset is ABI");
#endif

/* Callback function signature - called from transport thread */
typedef void (*ecuio_frame_callback_t)(
    const ecuio_frame_t* frame,
    void* user_data
);

/* Status codes - append-only evolution for ABI stability */
typedef enum {
    ECUIO_OK = 0,
    ECUIO_ERR_NULL_ARGUMENT = 1,
    ECUIO_ERR_NOT_IMPLEMENTED = 2,
    ECUIO_ERR_NO_TRANSPORT = 3,   /* nothing handles the URI: an unknown
                                     scheme, or no installed adapter
                                     matches its selector */
    ECUIO_ERR_OUT_OF_MEMORY = 4,
    ECUIO_ERR_SESSION_CLOSED = 5,
    ECUIO_ERR_ALREADY_REGISTERED = 6,   /* Not returned: a duplicate callback
                                           registration succeeds (pair
                                           identity, ADR-0002). Reserved,
                                           since codes are append-only */
    ECUIO_ERR_LOAD_FAILED = 7,    /* what the URI names couldn't be
                                     loaded: an adapter DLL that wouldn't
                                     load or lacks a mandatory J2534
                                     export, or a recording a replay
                                     can't play (ADR-0023) */
    /* Link-health codes: published by the J2534 worker through
     * ecuio_session_get_error_state while the session keeps running. */
    ECUIO_ERR_VEHICLE_TIMEOUT = 8,          /* reads answer, but nothing
                                               arrives: no ECU awake on a
                                               bus that still ACKs, as a
                                               vehicle with the ignition
                                               off can be */
    ECUIO_ERR_TRANSPORT_HARD_FAILURE = 9,   /* reads fail: the adapter isn't
                                               answering */
    ECUIO_ERR_REQUEST_FAILED = 10,   /* writes fail while reads stay clean:
                                        no node ACKs the requests (no
                                        vehicle, or a bus fully asleep).
                                        The adapter is fine, so this isn't
                                        a hard failure */
    ECUIO_ERR_INVALID_ARGUMENT = 11,   /* an argument is present but not
                                          acceptable: a signal set that fails
                                          validation, a malformed or built-in
                                          profile name (profile.h) */
    /* Why a connect failed, from ecuio_session_create or
     * ecuio_session_start; ecuio_last_error_detail says more. */
    ECUIO_ERR_BAD_URI = 12,           /* the transport URI is malformed: a
                                         query that breaks the grammar, a
                                         key the transport doesn't take, a
                                         bad value, or keys that don't fit
                                         together (?poll= with no channel) */
    ECUIO_ERR_UNKNOWN_PROFILE = 13,   /* ?profile= names no registered
                                         profile */
    ECUIO_ERR_VENDOR_REFUSED = 14,    /* the adapter's driver refused a call
                                         (PassThruOpen, PassThruConnect, a
                                         filter), or PassThruOpen never
                                         returned */
    ECUIO_ERR_SURROGATE_FAILED = 15,  /* the surrogate process that hosts an
                                         x86 adapter DLL couldn't start, or
                                         stopped answering */
    ECUIO_ERR_RECORDING_CHANGED = 16, /* a replay stopped early: its
                                         frames.csv changed or went away
                                         after the session opened it */
    ECUIO_ERR_REPORT_CHANGED = 17,    /* a clear named a diagnostics report
                                         that isn't the channel's now: not
                                         read to the end, a clear running,
                                         or another generation (ADR-0038);
                                         nothing was sent */
    ECUIO_ERR_RECORDING_FAILED = 18,  /* a recording's folder or files
                                         couldn't be made, or a write to
                                         them failed (ADR-0049, recording.h) */
    ECUIO_ERR_CANCELED = 19,          /* the caller's callback stopped a
                                         long call (a recording's open,
                                         ADR-0049; an export, an import
                                         or a health report, ADR-0054) */
    ECUIO_ERR_IN_USE = 20,            /* still in use: a session or a
                                         simulated ECU decodes with what
                                         was to be freed (a plugin,
                                         ADR-0052), or a recording holds
                                         what was to be changed (a
                                         session's conditions, conditions.h) */
    ECUIO_ERR_STREAM_ENDED = 21,      /* a pull queue's session has nothing
                                         more to give: a replay played its
                                         last frame (queue.h) */
    ECUIO_ERR_TOO_SOON = 22,          /* not yet: a module's delay after a
                                         refused key runs (its time left
                                         given), or its identity is still
                                         being read (no time given): ask
                                         again later (ADR-0066) */
    ECUIO_ERR_CONSENT_REQUIRED = 23,  /* no consent: the consent store
                                         holds none for the plugin, or the
                                         plugin names no publisher to
                                         consent to (plugin.h, ADR-0066) */

    /* Append-only rule: new codes go HERE, never renumber */
} ecuio_status_t;

/* Lifecycle. Core needs no global setup: sessions, the profile registry
 * (profile.h) and discovery (j2534.h, serial.h) work without ecuio_init. Both
 * calls are optional and do nothing; init always returns ECUIO_OK. A
 * caller may pair them, call either any number of times, or never call
 * them, and shutdown leaves open sessions working. */
ecuio_status_t ecuio_init(void);
void ecuio_shutdown(void);
const char* ecuio_version_string(void);
/* The same version as numbers, version.h's ECUIO_VERSION_MAJOR, _MINOR
 * and _PATCH as the library was built, for a program that loads core by
 * path and checks which one it got (ADR-0053). ECUIO_ERR_NULL_ARGUMENT,
 * writing nothing, when any of the three is NULL. */
ecuio_status_t ecuio_version(uint32_t* major, uint32_t* minor, uint32_t* patch);
const char* ecuio_status_string(ecuio_status_t status);

/* The host clock session time runs on (ADR-0033): microseconds that keep
 * counting while the machine sleeps, on every platform. Its origin is the
 * platform's, so only a difference means anything. Any thread. */
uint64_t ecuio_clock_us(void);
/* Microseconds the machine has slept since it started, on the same clock.
 * It never decreases, so a rise between two reads is a sleep between
 * them; 0 where the platform can't say. Any thread. */
uint64_t ecuio_clock_slept_us(void);

/* Session management.
 *
 * Lifecycle follows FILE/free discipline:
 * - create():  allocates and attaches a transport (fopen)
 * - close():   stops streaming, detaches the transport, releases worker
 *               and callback state. A second close returns
 *               ECUIO_ERR_SESSION_CLOSED; the session stays valid until
 *               destroy().
 * - destroy(): frees the session (free). The handle is INVALID afterward;
 *               calling any API with it is undefined behavior, exactly as
 *               free(p); *p. Null is a safe no-op. Auto-closes first if
 *               still open.
 *
 * Recommended order: close() then destroy(). Skip close() only when the
 * session was never started.
 */
ecuio_status_t ecuio_session_create(
    const char* transport_uri,
    ecuio_session_t** out_session
);

ecuio_status_t ecuio_session_close(ecuio_session_t* session);

ecuio_status_t ecuio_session_destroy(ecuio_session_t* session);

ecuio_status_t ecuio_session_start(ecuio_session_t* session);

/* Why the last ecuio_session_create or ecuio_session_start on the
 * calling thread failed, in words: which URI key, which vendor call and
 * its status, and the vendor's own description of the error. "" when
 * the last one succeeded, or when nothing more than the status is
 * known. Thread-local, like dlerror(): the text stays valid until the
 * next create or start on the same thread. Never NULL. */
const char* ecuio_last_error_detail(void);

/* Send/receive - synchronous API. Neither is built yet: send returns
 * ECUIO_ERR_INVALID_ARGUMENT for a data_length past
 * ECUIO_FRAME_DATA_MAX and ECUIO_ERR_NOT_IMPLEMENTED for any frame that
 * fits; receive returns ECUIO_ERR_NOT_IMPLEMENTED. */
ecuio_status_t ecuio_session_send(
    ecuio_session_t* session,
    const ecuio_frame_t* frame
);

ecuio_status_t ecuio_session_receive(
    ecuio_session_t* session,
    ecuio_frame_t* out_frame,
    uint32_t timeout_ms
);

/*
 * Callback API - async frame delivery
 *
 * CONTRACTS:
 * - Callbacks execute on transport thread, NOT the calling thread
 * - Callbacks must not block (>10ms risks transport backlog)
 * - Callbacks must not call ecuio_session_close() or
 *   ecuio_session_destroy(): they synchronize with the worker
 * - register/unregister ARE permitted from inside a callback:
 *   dispatch snapshots the callback list under lock and invokes
 *   unlocked, so reentry cannot deadlock (tested:
 *   SelfUnregisterDuringLiveDispatch)
 * - Duplicate (callback, user_data) pairs are silently deduplicated
 * - unregister is idempotent: calling it twice is safe
 * - The other side of the snapshot: after unregister returns, a
 *   dispatch already under way may call the callback once more, with
 *   its user_data. Keep user_data valid until ecuio_session_close
 *   returns, which joins the worker
 */
ecuio_status_t ecuio_register_frame_callback(
    ecuio_session_t* session,
    ecuio_frame_callback_t callback,
    void* user_data
);

ecuio_status_t ecuio_unregister_frame_callback(
    ecuio_session_t* session,
    ecuio_frame_callback_t callback,
    void* user_data
);

/* Link health, written by the transport worker while the session keeps
 * running. last_error is one of:
 *   ECUIO_OK                          replies arrive
 *   ECUIO_ERR_VEHICLE_TIMEOUT         the adapter answers, but no reply
 *                                     for about 1 s
 *   ECUIO_ERR_REQUEST_FAILED          writes fail while reads stay clean
 *   ECUIO_ERR_TRANSPORT_HARD_FAILURE  reads fail
 *   ECUIO_ERR_RECORDING_CHANGED       a replay stopped before the end of
 *                                     its file (set once; the session
 *                                     stays open and sends nothing more)
 * Treat any other value as degraded: later cores may append codes.
 * error_count counts episodes, each entry into a request failure, a hard
 * failure or a replay's early stop. Callable from any thread. The mock
 * transport never sets them; a replay that reaches the end of its file
 * leaves them at ECUIO_OK and 0. */
ecuio_status_t ecuio_session_get_error_state(
    ecuio_session_t* session,
    int32_t* out_last_error,
    uint32_t* out_error_count
);

/* What a session runs on, as its connect found it (ADR-0019). The
 * values below are append-only, like the status codes. */
#define ECUIO_SESSION_INFO_TEXT_LEN      80   /* PassThruReadVersion's
                                                 buffers (J2534-1 v04.04) */
#define ECUIO_SESSION_INFO_MAX_CHANNELS  4    /* the modules the info
                                                 lists itself; the rest
                                                 by profile.h's
                                                 ecuio_session_get_module */
#define ECUIO_SESSION_INFO_PROFILE_LEN   64   /* profile.h's
                                                 ECUIO_PROFILE_NAME_MAX + 1 */

/* backend */
#define ECUIO_BACKEND_MOCK               1u
#define ECUIO_BACKEND_J2534_INPROC       2u   /* retired: no DLL loads in
                                                 this process (ADR-0025) */
#define ECUIO_BACKEND_J2534_SURROGATE    3u   /* the adapter's DLL in a
                                                 surrogate process of its
                                                 architecture (ADR-0025) */
#define ECUIO_BACKEND_REPLAY             4u   /* a recording's frames.csv
                                                 (ADR-0023) */
#define ECUIO_BACKEND_ELM327             5u   /* an ELM327-compatible
                                                 adapter on a serial port
                                                 (ADR-0026) */
#define ECUIO_BACKEND_SOCKETCAN          6u   /* a Linux CAN interface,
                                                 listening (ADR-0032) */

/* dll_arch: the adapter DLL's PE machine, as discovery's reserved[0]
 * reports it (j2534.h) */
#define ECUIO_DLL_ARCH_UNKNOWN           0u
#define ECUIO_DLL_ARCH_X86               1u
#define ECUIO_DLL_ARCH_X64               2u

/* flags: which of the optional readings the adapter gave */
#define ECUIO_SESSION_INFO_VBATT         0x00000001u   /* vbatt_mv */
#define ECUIO_SESSION_INFO_VERSIONS      0x00000002u   /* the three
                                                          version strings */
#define ECUIO_SESSION_INFO_REPLAY_LOOP   0x00000004u   /* replay://?loop=1 */
#define ECUIO_SESSION_INFO_PENDING_OFF   0x00000008u   /* an STN adapter set
                                                          not to wait past a
                                                          7F xx 78 (its
                                                          parameter 2A), so a
                                                          pending reply's
                                                          answer is lost */
#define ECUIO_SESSION_INFO_LISTEN_ONLY   0x00000010u   /* a CAN controller in
                                                          listen-only mode,
                                                          which never
                                                          transmits (ADR-0032) */
#define ECUIO_SESSION_INFO_REPLAY_FAULTS 0x00000020u   /* replay://?faults=:
                                                          frames dropped,
                                                          corrupted or held
                                                          back on purpose
                                                          (ADR-0041) */
#define ECUIO_SESSION_INFO_REQUESTS      0x00000040u   /* the session asks an
                                                          ECU for what it
                                                          reads: a focus's
                                                          minimum time between
                                                          sweeps applies */
/* The set derives more requests than ECUIO_PROFILE_MAX_REQUESTS
 * (profile.h): the session asks only what a focus or an attached
 * condition picks, and nothing until one does (ADR-0065 section 2). Set
 * at create, and only with ECUIO_SESSION_INFO_REQUESTS. */
#define ECUIO_SESSION_INFO_PICK_ONLY     0x00000080u

/* POD, 432 bytes, no hidden padding on any supported platform; pinned
 * by the assertions below. New fields take the reserved room. */
typedef struct ecuio_session_info {
    uint32_t flags;           /* ECUIO_SESSION_INFO_* */
    uint32_t backend;         /* ECUIO_BACKEND_* */
    uint32_t dll_arch;        /* ECUIO_DLL_ARCH_*; UNKNOWN for mock */
    uint32_t protocol_id;     /* the J2534 ProtocolID the channel connected
                                 with (6 = ISO 15765; an ELM327's too); 0
                                 for mock */
    uint32_t baud;            /* the CAN bit/s it connected at; 0 for mock */
    uint32_t connect_flags;   /* PassThruConnect's Flags: ?flags=, plus
                                 CAN_29BIT_ID (0x100) for 29-bit channels */
    uint32_t vbatt_mv;        /* battery voltage at connect, millivolts
                                 (READ_VBATT; an ELM327's STVR or AT RV);
                                 with ECUIO_SESSION_INFO_VBATT */
    uint32_t channel_count;   /* how many of the session's modules rx_ids
                                 and tx_ids list: module_count, or
                                 ECUIO_SESSION_INFO_MAX_CHANNELS when
                                 there are more */
    uint32_t rx_ids[ECUIO_SESSION_INFO_MAX_CHANNELS];   /* each one's
                                                           receive ID */
    uint32_t tx_ids[ECUIO_SESSION_INFO_MAX_CHANNELS];   /* and transmit ID */
    /* The adapter's firmware, its DLL and the J2534 API version it
     * implements (PassThruReadVersion), each NUL-terminated on one line;
     * "" without ECUIO_SESSION_INFO_VERSIONS. An ELM327's firmware is its
     * AT I answer, then ", " and STI's on an STN chip; its other two are
     * "". */
    char firmware_version[ECUIO_SESSION_INFO_TEXT_LEN];
    char dll_version[ECUIO_SESSION_INFO_TEXT_LEN];
    char api_version[ECUIO_SESSION_INFO_TEXT_LEN];
    /* The profile the session decodes through (ADR-0013): ?profile=, or
     * the scheme's default. NUL-terminated. */
    char profile[ECUIO_SESSION_INFO_PROFILE_LEN];
    /* replay:// (ADR-0023): the recording's frames, its span from the
     * first frame to the last, and the speed in thousandths (1000 = real
     * time, 0 = max); 0 for every other backend */
    uint32_t replay_frames;
    uint32_t replay_span_ms;
    uint32_t replay_speed_milli;
    /* 1 when the recording's last row had no newline and was left out (a
     * write cut short, ADR-0049 section 7); 0 otherwise */
    uint32_t replay_rows_torn;
    uint32_t module_count;    /* CAN channels (modules) the session filters
                                 and requests on, up to signals.h's
                                 ECUIO_MAX_CHANNELS; 0 for a passive
                                 session, 1 for mock (its simulated engine
                                 ECU). The reports below are read by an
                                 index under it */
    uint32_t module_budget;   /* the modules live data may hold open at
                                 once, one filter kept back for a read of
                                 a closed module's codes (ADR-0065 step
                                 5); 0 when not limited (a session that
                                 sends no request) */
    uint32_t reserved[10];    /* zero; ABI growth space */
} ecuio_session_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_session_info_t) == 432, "ecuio_session_info_t size is ABI");
static_assert(offsetof(ecuio_session_info_t, firmware_version) == 64, "session info text offset is ABI");
static_assert(offsetof(ecuio_session_info_t, replay_frames) == 368, "session info replay offset is ABI");
static_assert(offsetof(ecuio_session_info_t, module_count) == 384, "session info module_count offset is ABI");
static_assert(offsetof(ecuio_session_info_t, module_budget) == 388, "session info module_budget offset is ABI");
static_assert(offsetof(ecuio_session_info_t, reserved) == 392, "session info reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_session_info_t) == 432, "ecuio_session_info_t size is ABI");
_Static_assert(offsetof(ecuio_session_info_t, firmware_version) == 64, "session info text offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, replay_frames) == 368, "session info replay offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, module_count) == 384, "session info module_count offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, module_budget) == 388, "session info module_budget offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, reserved) == 392, "session info reserved offset is ABI");
#endif

/* Copies what the session runs on into *out_info. Filled once, while
 * ecuio_session_create connects; nothing changes it afterwards, so it's
 * callable from any thread until destroy, close included. A battery or
 * version reading the adapter refuses leaves its flag clear and doesn't
 * fail the connect. */
ecuio_status_t ecuio_session_get_info(
    const ecuio_session_t* session,
    ecuio_session_info_t* out_info
);

/* What a replay's ?faults= has done so far (ADR-0041): the recording's
 * frames read, and how many of them each kind of fault met. A dropped
 * frame and one lost to an overflow weren't delivered; a corrupted one
 * had a bit flipped, a truncated one its data cut short; a stalled one
 * came that much later. POD, 64 bytes. */
typedef struct ecuio_replay_faults {
    uint64_t frames;     /* the recording's frames read */
    uint64_t dropped;    /* not delivered, nothing said */
    uint64_t overflowed; /* not delivered; the next one OVERFLOW */
    uint64_t corrupted;
    uint64_t truncated;
    uint64_t stalled;
    uint64_t reserved[2]; /* zero; ABI growth space */
} ecuio_replay_faults_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_replay_faults_t) == 64, "ecuio_replay_faults_t size is ABI");
static_assert(offsetof(ecuio_replay_faults_t, stalled) == 40, "replay faults offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_replay_faults_t) == 64, "ecuio_replay_faults_t size is ABI");
_Static_assert(offsetof(ecuio_replay_faults_t, stalled) == 40, "replay faults offset is ABI");
#endif

/* Copies the counts into *out; zeros for a session that isn't a replay
 * with faults. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL session or
 * out. Callable from any thread until destroy, close included. */
ecuio_status_t ecuio_session_get_replay_faults(
    const ecuio_session_t* session,
    ecuio_replay_faults_t* out
);

/* What each channel's ECU has said it supports (ADR-0015 section 4): its answers
 * to the supported-PID chain, and the requests left out of the rotation
 * because it refused them. The J2534 worker updates it as the answers
 * arrive. The values below are append-only. */
#define ECUIO_SUPPORT_MAX_DROPPED        8

/* flags */
#define ECUIO_SUPPORT_PROBED             0x00000001u   /* a walk of the chain has
                                                          ended, answered or not
                                                          (ranges 0: no bitmap
                                                          answered); clear while a
                                                          first walk is asking */
#define ECUIO_SUPPORT_COMPLETE           0x00000002u   /* answered to its end,
                                                          so every PID is judged */
#define ECUIO_SUPPORT_SCAN_COMPLETE      0x00000004u   /* every scanned DID was
                                                          answered, found or
                                                          refused (ADR-0020) */
#define ECUIO_SUPPORT_PACKED             0x00000008u   /* a request carrying
                                                          several PIDs was
                                                          answered with several
                                                          (ADR-0039 Appendix A) */
#define ECUIO_SUPPORT_UNPACKED           0x00000010u   /* such requests failed
                                                          three times in a row:
                                                          one PID per request
                                                          until the link
                                                          recovers */
#define ECUIO_SUPPORT_EXTENDED           0x00000020u   /* the channel reads in
                                                          the extended session
                                                          (ADR-0060) */
#define ECUIO_SUPPORT_CONVERSATION       0x00000040u   /* a plugin's code talks
                                                          to the channel's
                                                          module (ADR-0066),
                                                          its report
                                                          ecuio_session_get_
                                                          channel_conversation's */
#define ECUIO_SUPPORT_CLOSED             0x00000080u   /* no reason reaches
                                                          the module
                                                          (open_reasons 0):
                                                          nothing sent since,
                                                          the report what it
                                                          last read (ADR-0065
                                                          step 5) */
#define ECUIO_SUPPORT_NO_ROOM            0x00000100u   /* a reason wants the
                                                          module, but no
                                                          filter is free
                                                          (module_budget):
                                                          nothing sent until
                                                          one frees. Never
                                                          with CLOSED */

/* open_reasons: why a module is open, or wanted (ADR-0065 step 5) */
#define ECUIO_MODULE_REASON_RANGE        0x1u   /* it answered the start's
                                                   emissions request: always
                                                   open */
#define ECUIO_MODULE_REASON_READ         0x2u   /* a signal on it is read:
                                                   a pick, a gauge, an alert,
                                                   a condition, or a session
                                                   asking its table whole */
#define ECUIO_MODULE_REASON_CONVERSATION 0x4u   /* a conversation is bound
                                                   to it (ADR-0066) */
#define ECUIO_MODULE_REASON_VISIT        0x8u   /* a read or a clear of its
                                                   codes runs on it */

/* session_state: a channel's session (ADR-0060), with ?session=03 */
#define ECUIO_SESSION_STATE_NOT_ASKED    0u   /* no session asked: default */
#define ECUIO_SESSION_STATE_ENTERING     1u   /* 10 03 due or out */
#define ECUIO_SESSION_STATE_ENTERED      2u   /* 50 03: read in it */
#define ECUIO_SESSION_STATE_REFUSED      3u   /* 7F 10 <nrc>: read in
                                                 default, session_nrc
                                                 its code */
#define ECUIO_SESSION_STATE_UNANSWERED   4u   /* no answer: read in default,
                                                 asked again when the link
                                                 recovers */
#define ECUIO_SESSION_STATE_ABANDONED    5u   /* lost too often (7E or 7F
                                                 to its reads,
                                                 ECUIO_SESSION_LOSSES_MAX
                                                 times within
                                                 ECUIO_SESSION_LOSS_WINDOW_MS):
                                                 read in default,
                                                 session_nrc the last code */
#define ECUIO_SESSION_LOSSES_MAX         3u
#define ECUIO_SESSION_LOSS_WINDOW_MS     60000u

/* reason: why a request left the rotation */
#define ECUIO_DROP_BITMAP                1u   /* its PID's bit is clear */
#define ECUIO_DROP_NOT_SUPPORTED         2u   /* 7F <service> 12 or 31 */
#define ECUIO_DROP_SERVICE               3u   /* 7F <service> 11: not the
                                                 request, its whole service */
#define ECUIO_DROP_ACCESS                4u   /* 7F <service> 33 or 7F: it
                                                 needs security access or a
                                                 session other than the
                                                 extended one core enters on
                                                 request (ADR-0060) */

/* POD, 8 bytes */
typedef struct ecuio_dropped_request {
    uint8_t request[3];       /* the service ID, then the key */
    uint8_t length;           /* 2 or 3 */
    uint8_t reason;           /* ECUIO_DROP_* */
    uint8_t nrc;              /* the negative response code; 0 for a bitmap */
    uint8_t reserved[2];      /* zero */
} ecuio_dropped_request_t;

/* POD, 152 bytes, no hidden padding on any supported platform; pinned by
 * the assertions below. New fields take the reserved room. */
typedef struct ecuio_channel_support {
    uint32_t flags;           /* ECUIO_SUPPORT_* */
    uint32_t rx_id;           /* the channel's receive ID: its ECU */
    uint32_t generation;      /* changes with every update; 0 until the
                                 channel has learned something */
    uint32_t requests;        /* the channel's requests, kept or dropped */
    uint32_t dropped;         /* how many of them were dropped */
    uint32_t ranges;          /* bit i: the ECU answered the bitmap at PID
                                 0x20 * i */
    /* The bitmaps as the replies carry them: PID p is bit
     * 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8, set when supported. */
    uint8_t pid_bitmap[32];
    ecuio_dropped_request_t dropped_list[ECUIO_SUPPORT_MAX_DROPPED]; /* the
                                 first ones dropped: those a negative
                                 reply refused, then those the bitmaps
                                 left out (ECUIO_DROP_BITMAP), each in
                                 set order */
    /* The DID scan (ADR-0020), when the session's URI asked for one on
     * this channel: each DID of a range is asked once. Found ones joined
     * the rotation when its table had room (polled); refused ones were
     * answered negative; the rest, asked but unanswered, are asked again
     * when the link recovers. done = found + refused. */
    uint16_t scan_total;      /* DIDs to scan on this channel; 0 = no scan */
    uint16_t scan_done;       /* answered, found or refused */
    uint16_t scan_found;      /* answered positive */
    uint16_t scan_refused;    /* answered negative */
    uint16_t scan_polled;     /* found ones now in the rotation */
    uint16_t scan_reserved;   /* zero */
    uint16_t pids_supported;  /* the data PIDs the bitmaps list: the
                                 chain's own (0x20, 0x40, ...) not
                                 counted */
    uint16_t pids_through;    /* the last PID the answered bitmaps cover,
                                 0x20 * (i + 1) for ranges' highest bit
                                 i; 0 with none answered */
    /* The session (ADR-0060): what was asked of this channel, and what it
     * reads in. 0 for both before its first report. */
    uint8_t session_asked;    /* 0 none (the default), 3 (?session=03 for
                                 every channel, or a conversation's
                                 module for its own) */
    uint8_t session;          /* in force: 1 default, 3 extended */
    uint8_t session_state;    /* ECUIO_SESSION_STATE_* */
    uint8_t session_nrc;      /* REFUSED or ABANDONED: the code */
    uint16_t session_entries; /* each 50 03, saturating at 65535 */
    uint16_t session_lost;    /* each time the session was entered again
                                 (a 7E or 7F, a timeout, S3, a link
                                 recovery), saturating at 65535 */
    uint32_t open_reasons;    /* ECUIO_MODULE_REASON_*: what wants the
                                 module, whether it holds a filter or not;
                                 0 with ECUIO_SUPPORT_CLOSED, and on a
                                 session that sends no request (every
                                 module open) */
    uint32_t reserved[1];     /* zero; ABI growth space */
} ecuio_channel_support_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_dropped_request_t) == 8, "ecuio_dropped_request_t size is ABI");
static_assert(sizeof(ecuio_channel_support_t) == 152, "ecuio_channel_support_t size is ABI");
static_assert(offsetof(ecuio_channel_support_t, pid_bitmap) == 24, "support bitmap offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, scan_total) == 120, "support scan offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, pids_supported) == 132, "support pids offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, session_asked) == 136, "support session offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, session_entries) == 140, "support session counts offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, open_reasons) == 144, "support open_reasons offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, reserved) == 148, "support reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_dropped_request_t) == 8, "ecuio_dropped_request_t size is ABI");
_Static_assert(sizeof(ecuio_channel_support_t) == 152, "ecuio_channel_support_t size is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, pid_bitmap) == 24, "support bitmap offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, scan_total) == 120, "support scan offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, pids_supported) == 132, "support pids offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, session_asked) == 136, "support session offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, session_entries) == 140, "support session counts offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, open_reasons) == 144, "support open_reasons offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, reserved) == 148, "support reserved offset is ABI");
#endif

/* Copies what channel `channel` has learned into *out_support: an index
 * into the session's modules, below the info's module_count, so a
 * passive session has none (ECUIO_ERR_INVALID_ARGUMENT); the mock's one
 * channel learns nothing. Callable from any thread until destroy; after
 * close it keeps the last update. A requesting session opens a module
 * only while a reason wants it (ADR-0065 step 5): its flags say when it
 * is closed or has no room, open_reasons what wants it. */
ecuio_status_t ecuio_session_get_channel_support(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_support_t* out_support
);

/* What each channel's ECU says it is (ADR-0021): the VIN, the
 * calibration identifiers and the part, software and hardware numbers
 * the standard services carry (Service 09 items 02, 04, 06 and 0A;
 * Service 0x22 DIDs F187 to F195), each asked once after connect and
 * again, when unanswered, as the link recovers. The J2534 worker fills
 * it as the answers arrive. The values below are append-only. */
#define ECUIO_IDENTITY_MAX_ITEMS         16
#define ECUIO_IDENTITY_TEXT_LEN          64

/* flags */
#define ECUIO_IDENTITY_COMPLETE          0x00000001u   /* every item answered,
                                                          refused or timed out */
#define ECUIO_IDENTITY_VIN               0x00000002u   /* the VIN is in, before
                                                          the list is complete
                                                          too: 09 02 answered
                                                          with one, or F190 did
                                                          and 09 02 has its
                                                          outcome; what
                                                          ecuio_identity_vin
                                                          gives (ADR-0027
                                                          section 3) */

/* outcome: what became of an item's request */
#define ECUIO_IDENTITY_UNASKED           0u   /* not asked yet, or asked again
                                                 after a recovery */
#define ECUIO_IDENTITY_ANSWERED          1u   /* a positive reply: text holds it */
#define ECUIO_IDENTITY_REFUSED           2u   /* a negative reply: nrc holds the code */
#define ECUIO_IDENTITY_UNANSWERED        3u   /* its wait ran out */

/* POD, 72 bytes. text is the reply's data after the service's header
 * (the item count for Service 09, the DID for 0x22), NUL-terminated:
 * as ASCII when every byte is printable or padding (NUL, 0xFF), the
 * printable runs joined by ", " (two calibration IDs read "A, B"); else
 * as hex pairs ("DE AD BE EF"). Cut to the field, at a whole pair. */
typedef struct ecuio_identity_item {
    uint8_t request[3];       /* the service ID, then the key */
    uint8_t length;           /* 2 or 3 */
    uint8_t outcome;          /* ECUIO_IDENTITY_* */
    uint8_t nrc;              /* the negative reply's code, when refused */
    uint8_t text_length;      /* strlen(text) */
    uint8_t reserved;         /* zero */
    char text[ECUIO_IDENTITY_TEXT_LEN];
} ecuio_identity_item_t;

/* POD, 1184 bytes, no hidden padding on any supported platform; pinned
 * by the assertions below. New fields take the reserved room. */
typedef struct ecuio_channel_identity {
    uint32_t flags;           /* ECUIO_IDENTITY_* */
    uint32_t rx_id;           /* the channel's receive ID: its ECU */
    uint32_t generation;      /* changes with every item settled, but one
                                 unanswered again; 0 until one has been */
    uint32_t items;           /* entries of list in use; 0 when the session
                                 identifies nothing (?identify=0, ?poll=,
                                 a passive session) */
    uint32_t reserved[4];     /* zero; ABI growth space */
    ecuio_identity_item_t list[ECUIO_IDENTITY_MAX_ITEMS];   /* in the order
                                 asked */
} ecuio_channel_identity_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_identity_item_t) == 72, "ecuio_identity_item_t size is ABI");
static_assert(sizeof(ecuio_channel_identity_t) == 1184, "ecuio_channel_identity_t size is ABI");
static_assert(offsetof(ecuio_channel_identity_t, list) == 32, "identity list offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_identity_item_t) == 72, "ecuio_identity_item_t size is ABI");
_Static_assert(sizeof(ecuio_channel_identity_t) == 1184, "ecuio_channel_identity_t size is ABI");
_Static_assert(offsetof(ecuio_channel_identity_t, list) == 32, "identity list offset is ABI");
#endif

/* Copies what channel `channel` has learned of its ECU into
 * *out_identity, under the same rules as the support report: the channel
 * is an index below the session info's module_count
 * (ECUIO_ERR_INVALID_ARGUMENT otherwise), callable from any thread until
 * destroy, and after close it keeps the last update. */
ecuio_status_t ecuio_session_get_channel_identity(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_identity_t* out_identity
);

/* The name of an identification request, for display: "VIN" for
 * 09 02, "CALID" for 09 04, and so on through the list the worker asks.
 * "" for a request that isn't one of them. Static, never NULL. */
const char* ecuio_identity_request_name(
    const uint8_t* request,
    uint8_t length
);

/* The vehicle's VIN in an identity report (ADR-0027 section 3): 09 02's
 * answer, else 22 F190's when 09 02 wasn't answered, each kept to its
 * ASCII letters and digits in upper case and taken only at 17 of them;
 * no other item is (a CALID of 17 characters isn't a VIN). Writes the
 * VIN and a NUL into out, or "" when the report holds none; the report's
 * ECUIO_IDENTITY_VIN says it is in, which holds until a recovery asks
 * 09 02 again, and that may answer a different VIN. ECUIO_OK;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL identity or out;
 * ECUIO_ERR_INVALID_ARGUMENT for an out_cap under ECUIO_VIN_TEXT_LEN,
 * out untouched. */
#define ECUIO_VIN_TEXT_LEN 18

ecuio_status_t ecuio_identity_vin(
    const ecuio_channel_identity_t* identity,
    char* out,
    uint32_t out_cap
);

/* What each channel's ECU reports of its emissions diagnostics
 * (ADR-0037): the MIL and the readiness monitors (Service 01 PID 01,
 * and PID 41 for this drive cycle), the stored, pending and permanent trouble codes (Services 03, 07 and
 * 0A) and the freeze frame (Service 02). The values below are
 * append-only. */
#define ECUIO_DIAG_MAX_DTCS              32
#define ECUIO_DIAG_MAX_MONITORS          12
#define ECUIO_DIAG_MAX_FREEZE            16

/* flags */
#define ECUIO_DIAG_COMPLETE              0x00000001u   /* every item answered,
                                                          refused or timed out */
#define ECUIO_DIAG_READINESS             0x00000002u   /* PID 01 answered:
                                                          dtc_count and the
                                                          monitors hold it */
#define ECUIO_DIAG_MIL_ON                0x00000004u   /* PID 01's A7 */
#define ECUIO_DIAG_COMPRESSION           0x00000008u   /* PID 01's B3: a
                                                          compression-ignition
                                                          engine, whose monitors
                                                          C and D name otherwise */
#define ECUIO_DIAG_FREEZE_FRAME          0x00000010u   /* a freeze frame is
                                                          stored: freeze_dtc
                                                          names its code */
#define ECUIO_DIAG_ASKED                 0x00000020u   /* the session reads
                                                          this channel's
                                                          diagnostics; unset
                                                          under ?diagnose=0,
                                                          ?poll= or a passive
                                                          session, whose
                                                          report stays empty */
#define ECUIO_DIAG_CLEARING              0x00000040u   /* a clear runs on this
                                                          channel (ADR-0038) */
#define ECUIO_DIAG_CLEAR_RPM             0x00000080u   /* the last clear read
                                                          the engine speed:
                                                          clear_rpm holds it */
#define ECUIO_DIAG_CLEAR_KMH             0x00000100u   /* the last clear read
                                                          the vehicle speed:
                                                          clear_kmh holds it */
#define ECUIO_DIAG_CYCLE                 0x00000200u   /* PID 41 answered: the
                                                          monitors' CYCLE flags
                                                          hold this drive
                                                          cycle */
#define ECUIO_DIAG_NOT_OPENED            0x00000400u   /* the last read found
                                                          its closed module
                                                          could not be opened
                                                          (no filter free):
                                                          COMPLETE with every
                                                          item unasked
                                                          (ADR-0065 step 5) */

/* lists[]: one per service */
#define ECUIO_DIAG_STORED                0u   /* Service 03: confirmed */
#define ECUIO_DIAG_PENDING               1u   /* Service 07: not yet confirmed */
#define ECUIO_DIAG_PERMANENT             2u   /* Service 0A: cleared only by
                                                 the ECU's own monitor */
#define ECUIO_DIAG_LISTS                 3u

/* outcome: what became of an item's request, as ECUIO_IDENTITY_* */
#define ECUIO_DIAG_UNASKED               0u
#define ECUIO_DIAG_ANSWERED              1u
#define ECUIO_DIAG_REFUSED               2u   /* nrc holds the code */
#define ECUIO_DIAG_UNANSWERED            3u
#define ECUIO_DIAG_UNSUPPORTED           4u   /* not asked: the ECU's
                                                 support bitmaps leave its
                                                 PID out */

/* clear: what became of the channel's last clear (ADR-0038) */
#define ECUIO_CLEAR_NONE                 0u   /* none yet */
#define ECUIO_CLEAR_CLEARED              1u   /* 44: the ECU cleared */
#define ECUIO_CLEAR_REFUSED              2u   /* clear_nrc holds the code;
                                                 nothing changed */
#define ECUIO_CLEAR_UNANSWERED           3u   /* 04 went unanswered: the
                                                 read after it shows whether
                                                 it took */
#define ECUIO_CLEAR_ENGINE_RUNNING       4u   /* 01 0C read clear_rpm above
                                                 0: 04 not sent */
#define ECUIO_CLEAR_NO_ENGINE_SPEED      5u   /* 01 0C refused (clear_nrc)
                                                 or unanswered: 04 not sent */
#define ECUIO_CLEAR_REPORT_CHANGED       6u   /* the report wasn't the one
                                                 the caller saved: 04 not
                                                 sent */
#define ECUIO_CLEAR_MOVING               7u   /* 01 0D read clear_kmh above
                                                 0 and the call didn't allow
                                                 a moving vehicle: 04 not
                                                 sent */
#define ECUIO_CLEAR_NO_VEHICLE_SPEED     8u   /* 01 0D refused (clear_nrc)
                                                 or unanswered and the call
                                                 didn't allow a moving
                                                 vehicle: 04 not sent */
#define ECUIO_CLEAR_NOT_EMISSIONS        9u   /* the report holds no
                                                 positive answer to 01 00
                                                 or 03: no emissions
                                                 module, 04 not sent */
#define ECUIO_CLEAR_NOT_OPENED                                                 \
    10u /* the module wasn't open
                                                 and no filter was free to
                                                 visit it for the clear:
                                                 04 not sent */

/* ecuio_session_clear_diagnostics_with's flags */
#define ECUIO_CLEAR_ALLOW_RUNNING        0x00000001u   /* send 04 at any engine
                                                          speed; 01 0D is asked
                                                          after 01 0C */
#define ECUIO_CLEAR_ALLOW_MOVING         0x00000002u   /* send 04 at any vehicle
                                                          speed too */

/* ecuio_monitor_t flags */
#define ECUIO_MONITOR_COMPLETE           0x01u   /* complete, or not
                                                    applicable (J1979) */
#define ECUIO_MONITOR_CYCLE_ENABLED      0x02u   /* PID 41: enabled this
                                                    drive cycle */
#define ECUIO_MONITOR_CYCLE_COMPLETE     0x04u   /* PID 41: complete this
                                                    drive cycle */

/* POD, 68 bytes. A code is its two bytes, the first high: A7-A6 the
 * letter (P, C, B, U), then four hex digits (ecuio_dtc_format).
 * reported is the reply's count byte; held is how many follow in codes,
 * fewer when the count passes ECUIO_DIAG_MAX_DTCS or the reply ends
 * first. */
typedef struct ecuio_dtc_list {
    uint8_t outcome;          /* ECUIO_DIAG_UNASKED .. _UNANSWERED */
    uint8_t nrc;              /* the negative reply's code, when refused */
    uint8_t reported;
    uint8_t held;
    uint16_t codes[ECUIO_DIAG_MAX_DTCS];
} ecuio_dtc_list_t;

/* POD, 4 bytes: one monitor the ECU has (its availability bit set). id
 * is that bit's place, 0-7 in PID 01's byte B, 8-15 in byte C
 * (ecuio_monitor_name). */
typedef struct ecuio_monitor {
    uint8_t id;
    uint8_t flags;            /* ECUIO_MONITOR_* */
    uint8_t reserved[2];      /* zero */
} ecuio_monitor_t;

/* POD, 16 bytes: one freeze-frame value, decoded by the obd2 set's def
 * def_id (ADR-0037 section 4), in that def's unit. */
typedef struct ecuio_freeze_value {
    uint32_t def_id;
    uint32_t reserved;        /* zero */
    double value;
} ecuio_freeze_value_t;

/* POD, 552 bytes, no hidden padding on any supported platform; pinned
 * by the assertions below. New fields take the reserved room. */
typedef struct ecuio_channel_diagnostics {
    uint32_t flags;           /* ECUIO_DIAG_* */
    uint32_t rx_id;           /* the channel's receive ID: its ECU */
    uint32_t generation;      /* changes with every item settled, but one
                                 unanswered again; 0 until one has been */
    uint8_t readiness;        /* 01 01's outcome (ECUIO_DIAG_UNASKED ..) */
    uint8_t readiness_nrc;
    uint8_t dtc_count;        /* PID 01's A6-A0: the confirmed codes */
    uint8_t monitor_count;    /* entries of monitors in use */
    uint8_t freeze;           /* 02 00 00's outcome: the freeze frame */
    uint8_t freeze_nrc;
    uint16_t freeze_dtc;      /* the code that stored it (02 02 00); 0 =
                                 none */
    uint8_t freeze_count;     /* entries of freeze_values in use */
    uint8_t clear;            /* the last clear's outcome (ECUIO_CLEAR_*);
                                 kept across reads until the next clear */
    uint8_t clear_nrc;        /* its negative reply's code */
    uint8_t cycle;            /* 01 41's outcome: this drive cycle's
                                 monitors (a refusal's code isn't kept) */
    uint32_t clear_rpm;       /* the engine speed its 01 0C read, rounded
                                 up, so a turning engine never reads 0
                                 (ECUIO_DIAG_CLEAR_RPM: one was read) */
    uint32_t clear_kmh;       /* the vehicle speed its 01 0D read, km/h
                                 (ECUIO_DIAG_CLEAR_KMH: one was read) */
    uint32_t reserved[2];     /* zero; ABI growth space */
    ecuio_dtc_list_t lists[ECUIO_DIAG_LISTS];   /* ECUIO_DIAG_STORED .. */
    ecuio_monitor_t monitors[ECUIO_DIAG_MAX_MONITORS];
    uint32_t reserved1;       /* zero; aligns freeze_values */
    ecuio_freeze_value_t freeze_values[ECUIO_DIAG_MAX_FREEZE];
} ecuio_channel_diagnostics_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_dtc_list_t) == 68, "ecuio_dtc_list_t size is ABI");
static_assert(sizeof(ecuio_monitor_t) == 4, "ecuio_monitor_t size is ABI");
static_assert(sizeof(ecuio_freeze_value_t) == 16, "ecuio_freeze_value_t size is ABI");
static_assert(sizeof(ecuio_channel_diagnostics_t) == 552, "ecuio_channel_diagnostics_t size is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, clear) == 21, "diagnostics clear offset is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, cycle) == 23, "diagnostics cycle offset is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, clear_rpm) == 24, "diagnostics clear_rpm offset is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, clear_kmh) == 28, "diagnostics clear_kmh offset is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, lists) == 40, "diagnostics lists offset is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, monitors) == 244, "diagnostics monitors offset is ABI");
static_assert(offsetof(ecuio_channel_diagnostics_t, freeze_values) == 296, "diagnostics freeze offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_dtc_list_t) == 68, "ecuio_dtc_list_t size is ABI");
_Static_assert(sizeof(ecuio_monitor_t) == 4, "ecuio_monitor_t size is ABI");
_Static_assert(sizeof(ecuio_freeze_value_t) == 16, "ecuio_freeze_value_t size is ABI");
_Static_assert(sizeof(ecuio_channel_diagnostics_t) == 552, "ecuio_channel_diagnostics_t size is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, clear) == 21, "diagnostics clear offset is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, cycle) == 23, "diagnostics cycle offset is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, clear_rpm) == 24, "diagnostics clear_rpm offset is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, clear_kmh) == 28, "diagnostics clear_kmh offset is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, lists) == 40, "diagnostics lists offset is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, monitors) == 244, "diagnostics monitors offset is ABI");
_Static_assert(offsetof(ecuio_channel_diagnostics_t, freeze_values) == 296, "diagnostics freeze offset is ABI");
#endif

/* A trouble code as it's written: the letter from its first byte's top
 * two bits (P, C, B, U), then four hex digits ("P0301" for 0x0301,
 * "U0100" for 0xC100), NUL-terminated in out, which holds at least 6
 * bytes. ECUIO_ERR_NULL_ARGUMENT for a NULL out,
 * ECUIO_ERR_INVALID_ARGUMENT for a smaller out_cap (out untouched). */
ecuio_status_t ecuio_dtc_format(
    uint16_t code,
    char* out,
    uint32_t out_cap
);

/* The other way (ADR-0044): "P0301" to 0x0301, "U0100" to 0xC100. text
 * is a letter, P, C, B or U, then a digit 0-3 and three hex digits, all
 * upper case, then a NUL. ECUIO_ERR_NULL_ARGUMENT for a NULL text or
 * out; ECUIO_ERR_INVALID_ARGUMENT for anything else (out untouched). */
ecuio_status_t ecuio_dtc_parse(
    const char* text,
    uint16_t* out
);

/* A code's title (ADR-0044): SAE J2012's name for a generic powertrain
 * or network code (P0, P2, P34-P39, U0 and U3), "Cylinder 1 Misfire
 * Detected" for 0x0301, from Wal33D's dtc-database (MIT; its notice is
 * in the third-party notices). "" for any other code: a manufacturer's
 * range, a body or chassis code, or one the list lacks. Static, never
 * NULL. */
const char* ecuio_dtc_title(
    uint16_t code
);

/* Where the titles come from: "Wal33D/dtc-database 04c43d7" (the
 * project and its commit). Static, never NULL. */
const char* ecuio_dtc_title_source(void);

/* The list's copyright and permission notice, its MIT license whole,
 * lines ending in a newline: carried in every binary that holds the
 * titles, for a program to show. Static, never NULL. */
const char* ecuio_dtc_title_notice(void);

/* A readiness monitor's name, for display ("Catalyst", "Evaporative
 * system"), and its short form for a log line ("catalyst", "EVAP"). C's
 * and D's bits name other monitors on a compression-ignition engine:
 * pass the report's flags, whose ECUIO_DIAG_COMPRESSION picks them. ""
 * for an id that names no monitor (a reserved bit). Static, never NULL. */
const char* ecuio_monitor_name(
    uint8_t id,
    uint32_t diag_flags
);
const char* ecuio_monitor_short_name(
    uint8_t id,
    uint32_t diag_flags
);

/* Whether a car's readiness would pass the federal OBD inspection rule
 * (ADR-0045): 40 CFR 85.2222(c), a model year 1996-2000 car with two
 * monitors incomplete at most, a 2001 or newer one with one, and (d) a
 * commanded MIL fails. A state's program sets its own rule, and some
 * are stricter. The values below are append-only. */
#define ECUIO_INSPECTION_UNKNOWN         0u   /* no report counted: none
                                                 asked, or no ECU answered
                                                 PID 01 */
#define ECUIO_INSPECTION_READY           1u   /* MIL off, incomplete <=
                                                 allowed */
#define ECUIO_INSPECTION_NOT_READY       2u   /* MIL off, incomplete >
                                                 allowed: (c)(1) sends the
                                                 car away to be driven */
#define ECUIO_INSPECTION_MIL_ON          3u   /* an ECU commands the MIL:
                                                 (d)(1) fails it */
#define ECUIO_INSPECTION_YEAR_NEEDED     4u   /* MIL off, two incomplete,
                                                 no model year: 1996-2000
                                                 allows them, 2001 on not */
#define ECUIO_INSPECTION_NOT_COVERED     5u   /* a model year before 1996,
                                                 which the rule doesn't
                                                 cover */
#define ECUIO_INSPECTION_READING         6u   /* a report asked is still
                                                 being read */

/* POD, 24 bytes, no hidden padding; pinned by the assertions below. */
typedef struct ecuio_inspection {
    uint8_t verdict;          /* ECUIO_INSPECTION_* */
    uint8_t incomplete;       /* monitors some ECU reports incomplete */
    uint8_t allowed;          /* incomplete monitors the rule allows the
                                 model year: 2 for 1996-2000, 1 for 2001
                                 on, 0 for none given or before 1996 */
    uint8_t reports;          /* reports counted: PID 01 answered */
    uint16_t incomplete_ids;  /* bit n: monitor id n is incomplete */
    uint16_t reserved0;       /* zero */
    uint32_t diag_flags;      /* ECUIO_DIAG_COMPRESSION when a counted
                                 report's engine is compression ignition:
                                 the flags ecuio_monitor_name takes */
    uint32_t reserved[3];     /* zero; ABI growth space */
} ecuio_inspection_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_inspection_t) == 24, "ecuio_inspection_t size is ABI");
static_assert(offsetof(ecuio_inspection_t, diag_flags) == 8, "inspection diag_flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_inspection_t) == 24, "ecuio_inspection_t size is ABI");
_Static_assert(offsetof(ecuio_inspection_t, diag_flags) == 8, "inspection diag_flags offset is ABI");
#endif

/* Judges count reports together, as one car (ADR-0045): a monitor is
 * incomplete when any ECU that has it says so, and the MIL is on when
 * any ECU commands it. A report counts once PID 01 has answered
 * (ECUIO_DIAG_READINESS); while one asked (ECUIO_DIAG_ASKED) isn't
 * complete the verdict is ECUIO_INSPECTION_READING. model_year is the
 * car's (ecuio_vin_decode), 0 when none is known. In order: READING,
 * UNKNOWN, NOT_COVERED, MIL_ON, then the count against the model year's
 * allowance; with no model year, two incomplete is YEAR_NEEDED. The
 * counts are filled in for every verdict. ECUIO_ERR_NULL_ARGUMENT for a
 * NULL out, or a NULL reports with count above 0. */
ecuio_status_t ecuio_diag_inspect(
    const ecuio_channel_diagnostics_t* reports,
    uint32_t count,
    uint32_t model_year,
    ecuio_inspection_t* out
);

/* Copies what channel `channel`'s ECU has reported of its diagnostics
 * (ADR-0037) into *out_diagnostics, under the identity report's rules:
 * the channel is an index below the session info's module_count
 * (ECUIO_ERR_INVALID_ARGUMENT otherwise), callable from any thread until
 * destroy, and after close it keeps the last update. */
ecuio_status_t ecuio_session_get_channel_diagnostics(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_diagnostics_t* out_diagnostics
);

/* Asks every channel's ECU for its diagnostics again ("Read again"):
 * each channel whose last read has settled starts over, its report
 * emptied and its generation moved on; a channel mid-read carries on.
 * The worker takes the request on its next pass, so it returns at once,
 * from any thread. Does nothing on a session that reads no diagnostics
 * (no channel has ECUIO_DIAG_ASKED). ECUIO_ERR_SESSION_CLOSED after
 * close. */
ecuio_status_t ecuio_session_read_diagnostics(ecuio_session_t* session);

/* Clears channel `channel`'s trouble codes (ADR-0038): SAE J1979 Service
 * 04, which on the ECU that obeys it also erases the freeze frame, turns
 * the MIL off and sets the readiness monitors back to incomplete;
 * permanent codes stay. generation names the report the caller saved
 * first (ecuio_session_get_channel_diagnostics): it must be read to the
 * end, with no clear running, and still be the channel's, or the call
 * is ECUIO_ERR_REPORT_CHANGED and nothing is sent. On its next pass the
 * worker checks the generation again and asks the engine module's speed
 * (01 0C); 04 goes out only at 0 rpm. The report's clear field says
 * what became of it (ECUIO_CLEAR_*), and after a clear the report is
 * read again. Returns at once, from any thread. A clear request of one
 * target (ecuio_session_clear_diagnostics_set's rules).
 * ECUIO_ERR_INVALID_ARGUMENT for a channel at or past the session info's
 * module_count, ECUIO_ERR_SESSION_CLOSED after close. The same as
 * ecuio_session_clear_diagnostics_with and flags 0. */
ecuio_status_t ecuio_session_clear_diagnostics(
    ecuio_session_t* session,
    uint32_t channel,
    uint32_t generation
);

/* ecuio_session_clear_diagnostics, with the caller's word on the
 * engine (ADR-0038's appendix of 2026-11-01). ECUIO_CLEAR_ALLOW_RUNNING:
 * 04 goes out at any engine speed, the speed only recorded, and an ECU
 * that gives none is cleared too; 01 0D (vehicle speed) is asked next,
 * and 04 goes out at 0 km/h, or at any speed with
 * ECUIO_CLEAR_ALLOW_MOVING; otherwise the clear ends as
 * ECUIO_CLEAR_MOVING or ECUIO_CLEAR_NO_VEHICLE_SPEED, nothing sent.
 * clear_rpm and clear_kmh hold what was read (ECUIO_DIAG_CLEAR_RPM,
 * _KMH). ECUIO_ERR_INVALID_ARGUMENT for any other flag. */
ecuio_status_t ecuio_session_clear_diagnostics_with(
    ecuio_session_t* session,
    uint32_t channel,
    uint32_t generation,
    uint32_t flags
);

/* One target of a clear request: the channel, and the generation of its
 * report the caller saved */
typedef struct ecuio_clear_target {
    uint32_t channel;
    uint32_t generation;
    uint32_t reserved[2]; /* zero */
} ecuio_clear_target_t;

/* One clear request over count targets (ADR-0038): the engine and the
 * vehicle speed are judged once for them all, then each target's 04
 * goes out. The rpm is the engine module's (the channel the session's
 * signals read PID 0C from), target or not; none open reads as no rpm.
 * The vehicle speed is the first answer to 01 0D, asked of the engine
 * module, then of the targets in channel order. flags are
 * ecuio_session_clear_diagnostics_with's, for the whole request, and
 * every target ends as one judgment says. A target whose report holds
 * no positive answer to 01 00 or 03 ends ECUIO_CLEAR_NOT_EMISSIONS; one
 * whose report moved before the worker took the request ends
 * ECUIO_CLEAR_REPORT_CHANGED; nothing is sent for either, and the other
 * targets go on. A target that isn't open is visited for its 04 and the
 * read after it, one at a time, in the filter kept back from live data;
 * with none free it ends ECUIO_CLEAR_NOT_OPENED, nothing sent. Returns at
 * once, from any thread.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL targets; ECUIO_ERR_INVALID_ARGUMENT
 * for count 0 or past ECUIO_MAX_CHANNELS, a channel at or past the
 * session info's module_count or named twice, nonzero reserved, or any
 * other flag; ECUIO_ERR_REPORT_CHANGED when a target's report isn't read
 * to the end, is clearing or has another generation, nothing requested;
 * ECUIO_ERR_IN_USE while the session's last request still runs;
 * ECUIO_ERR_SESSION_CLOSED after close. */
ecuio_status_t ecuio_session_clear_diagnostics_set(
    ecuio_session_t* session,
    const ecuio_clear_target_t* targets,
    uint32_t count,
    uint32_t flags
);

/*
 * The analysis log's focus (ADR-0056 section 2): asks only the requests
 * that carry these signals, until called again; count 0 ends the focus
 * and asks the whole set again. The picked requests run in the fast
 * class, so the rotation is as short as the picks allow; the rest are
 * parked: kept in the table, not refused, asked again on a count of 0. The probe, the DID
 * scan, identification and diagnostics are untouched, and a request the
 * probe dropped stays dropped. A DID the scan finds while a focus is on
 * is asked when the focus picks it, parked otherwise. The signals an
 * attached condition reads stay asked under any focus (ADR-0063 section
 * 7; ecuio_session_set_conditions). Whether a focus leaves something to
 * request is judged by its picks alone. A session that asks
 * requests refuses a focus none of whose signals it can ask (passive
 * signals, broadcast frames a listener hears; Service 09's; a request
 * the probe dropped; a DID the scan hasn't found; under ?poll=, any but
 * the polled PID's). The focus is judged against the session's requests
 * when the call is made: a pick dropped later (by the probe's walk, an
 * NRC, or the first walk after a focus set before start) can leave a
 * requesting session asking nothing until the focus changes or ends.
 * A session that asks only its picks (ECUIO_SESSION_INFO_PICK_ONLY: its
 * set derives more than ECUIO_PROFILE_MAX_REQUESTS requests, profile.h)
 * asks nothing with no focus but an attached condition's signals: there
 * a count of 0 is taken as on any session and parks every request a
 * condition doesn't read, and a focus whose requests, with the attached
 * conditions', number more than ECUIO_PROFILE_MAX_REQUESTS is refused,
 * the counts in the detail. On a session without the flag the whole set
 * fits a rotation, so that refusal never comes.
 * Takes effect on the worker between two requests, never inside an
 * exchange; before start it applies from the first pass. mock://,
 * replay:// and a listening session ask nothing, so they accept any
 * focus and change nothing. An id given twice is picked once, at its
 * first place. A recorder taking the session's frames gets each focus
 * set or ended as an activity line (ecuio_session_record). Returns at
 * once. The session's one caller may call it from any thread, while the
 * worker runs (the picks reach it under a lock), but not at the same
 * time as another call on the session, ecuio_session_close among them,
 * other than one this header lets any thread make (the getters,
 * ecuio_session_security_unlock).
 * The detail (ecuio_last_error_detail) is cleared on entry.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL session, or NULL ids with a count;
 * ECUIO_ERR_INVALID_ARGUMENT, nothing changed and the reason in the
 * detail, for an id the session's set lacks, a focus that leaves
 * nothing to request or one of more requests than a rotation holds;
 * ECUIO_ERR_SESSION_CLOSED after close.
 */
ecuio_status_t ecuio_session_set_focus(
    ecuio_session_t* session,
    const uint16_t* signal_ids,
    uint32_t count
);

/* The longest minimum time between sweeps, in ms */
#define ECUIO_FOCUS_MIN_SWEEP_MAX_MS 60000u

/* ecuio_session_set_focus_with's flag: the signal ids given are a front
 * end's default pick (the set's ranked signals, the gauges, the user's
 * ticks), not an analysis log's choice. The ids are the pick, asked as any
 * focus is; the flag changes only a recording's word for it, "pick" in
 * place of "focus", on the header's lines and on a change's activity
 * line, so a reader tells a session's default from a log. Each call sets
 * the flag afresh: a call without it (ecuio_session_set_focus among
 * them) clears it */
#define ECUIO_FOCUS_DEFAULT_PICK 0x1u

/*
 * ecuio_session_set_focus, with a minimum time between sweeps (ADR-0056
 * section 4), for a slow adapter or a busy ECU; min_sweep_ms 0 is none,
 * and the same as ecuio_session_set_focus. Each channel starts a sweep of
 * the picks at most once every min_sweep_ms, measured from one sweep's
 * first request to the next's, so a sweep that takes longer than that
 * waits for nothing. Only the picks' rotation waits: the probe, the DID
 * scan, identification, diagnostics and a clear go out in the gap. It
 * lasts as long as the focus; a session with no focus has no minimum. A
 * session that asks nothing (the mock, a replay, listening;
 * ECUIO_SESSION_INFO_REQUESTS clear) takes it and changes nothing.
 * "Not responding" is ten unanswered requests in a row whatever the
 * minimum, so with one request a sweep an ECU that stops answering is
 * reported after ten times min_sweep_ms.
 * flags: 0, or ECUIO_FOCUS_DEFAULT_PICK.
 * ECUIO_ERR_INVALID_ARGUMENT, nothing changed and the reason in the
 * detail, for any other flag, a time past ECUIO_FOCUS_MIN_SWEEP_MAX_MS, a
 * time with no signals picked, ECUIO_FOCUS_DEFAULT_PICK with no signals
 * picked (a front end whose pick is empty sets no focus, and the whole set
 * is asked, or nothing on a session that asks only its picks), and
 * whatever ecuio_session_set_focus refuses.
 */
ecuio_status_t ecuio_session_set_focus_with(
    ecuio_session_t* session,
    const uint16_t* signal_ids,
    uint32_t count,
    uint32_t min_sweep_ms,
    uint32_t flags
);

/* What ecuio_session_keep_awake holds off (ADR-0033's appendix "the host
 * kept awake while a session reads") */
#define ECUIO_AWAKE_SYSTEM   0x00000001u   /* the machine's sleep on its
                                              idle timer */
#define ECUIO_AWAKE_WHY_MAX  128           /* bytes of `why`, its NUL
                                              included */

/*
 * Keeps the host from sleeping on its idle timer while this session is
 * open: from this call until ecuio_session_close or ecuio_session_destroy,
 * or a call with what = 0, which never waits. `why` is the reason the
 * platform lists (NULL: a neutral default). Only a live transport holds
 * (elm327://, j2534://, socketcan://): under mock:// and replay:// the
 * call returns ECUIO_OK and holds nothing. A second call while held
 * changes nothing. A sleep the user asks for, a closed lid and a critical
 * battery still sleep the machine.
 * ECUIO_ERR_NOT_IMPLEMENTED where core can't hold (Android and iOS, whose
 * apps hold their own; a Linux with neither logind nor a desktop's
 * inhibit), with ecuio_last_error_detail() saying why; the session runs
 * on either way. ECUIO_ERR_INVALID_ARGUMENT for a bit not listed above or
 * a `why` of ECUIO_AWAKE_WHY_MAX bytes or more; ECUIO_ERR_SESSION_CLOSED
 * after close. The detail is cleared on entry. On Linux a take waits up
 * to 1 s for the buses, on the calling thread, holding no lock: a call
 * with what = 0 or the close on another thread meanwhile wins, and the
 * take returns ECUIO_OK (ECUIO_ERR_SESSION_CLOSED after the close)
 * holding nothing.
 */
ecuio_status_t ecuio_session_keep_awake(
    ecuio_session_t* session,
    uint32_t what,
    const char* why
);

/* The ECUIO_AWAKE_* held for this session now, whoever asked (a caller,
 * or core itself through a write): 0 under mock:// and replay://, after a
 * refusal and after close. Any thread. */
ecuio_status_t ecuio_session_get_awake(
    const ecuio_session_t* session,
    uint32_t* out_what
);

/* A plugin's code talking to one channel's module (ADR-0066 section
 * 2.4): bound at start behind the conversation tier's consent, each
 * unlock attempt the caller's word. */

/* ecuio_channel_conversation_t's state */
#define ECUIO_CONVERSATION_NONE          0u   /* no plugin's code talks here */
#define ECUIO_CONVERSATION_UNBEGUN       1u   /* bound, not yet begun */
#define ECUIO_CONVERSATION_TALKING       2u   /* its begin took a session */
#define ECUIO_CONVERSATION_DECLINED      3u   /* its begin declined: data only */
#define ECUIO_CONVERSATION_FAULTED       4u   /* a fault ended it (fault says
                                                 which); the rotation reads on */
#define ECUIO_CONVERSATION_ENDED         5u   /* the session closed */

/* ecuio_channel_conversation_t's flags */
#define ECUIO_CONVERSATION_NOT_KEPT      0x00000001u   /* no security store:
                                                          the counts are the
                                                          session's alone */

/* ecuio_channel_conversation_t's unlock: where the last word stands */
#define ECUIO_UNLOCK_NONE                0u   /* no word given */
#define ECUIO_UNLOCK_WAITING             1u   /* given, not yet taken */
#define ECUIO_UNLOCK_SEED                2u   /* its seed is asked */
#define ECUIO_UNLOCK_KEY                 3u   /* its key is out: its end
                                                 counts */
#define ECUIO_UNLOCK_UNLOCKED            4u   /* the level is open */
#define ECUIO_UNLOCK_FAILED              5u   /* the key's end was no 67:
                                                 counted, the delay runs */
#define ECUIO_UNLOCK_ENDED               6u   /* ended before a key went:
                                                 nothing counted (nrc a
                                                 refused seed's code) */
#define ECUIO_UNLOCK_LOST                7u   /* the session was lost: every
                                                 level is locked again */
#define ECUIO_UNLOCK_NO_LEVEL            8u   /* refused: no such level */
#define ECUIO_UNLOCK_KEY_TOO_LONG        9u   /* refused: the key doesn't
                                                 fit one request */
#define ECUIO_UNLOCK_BUSY                10u  /* refused: an attempt ran */
#define ECUIO_UNLOCK_DELAYED             11u  /* refused: the delay ran */
#define ECUIO_UNLOCK_ALREADY_UNLOCKED    12u  /* refused: open already */
#define ECUIO_UNLOCK_REFUSED             13u  /* refused: no seed may be
                                                 asked (a functional
                                                 channel's session, or the
                                                 code not talking) */

/* ecuio_channel_conversation_t's fault */
#define ECUIO_CONVERSATION_FAULT_NONE        0u
#define ECUIO_CONVERSATION_FAULT_BUDGET      1u   /* a call ran out of
                                                     instructions */
#define ECUIO_CONVERSATION_FAULT_TRAP        2u
#define ECUIO_CONVERSATION_FAULT_BUFFER      3u   /* its exchange buffer left
                                                     its memory */
#define ECUIO_CONVERSATION_FAULT_BEGIN       4u   /* begin's answer */
#define ECUIO_CONVERSATION_FAULT_LENGTH      5u   /* a request's length */
#define ECUIO_CONVERSATION_FAULT_BOUND       6u   /* past the channel's bound */
#define ECUIO_CONVERSATION_FAULT_SERVICE     7u   /* outside the guest's list */
#define ECUIO_CONVERSATION_FAULT_UNDECLARED  8u   /* not in its manifest */
#define ECUIO_CONVERSATION_FAULT_DOOR        9u   /* the engine's door refused */
#define ECUIO_CONVERSATION_FAULT_KEY         10u  /* a key of the wrong length */
#define ECUIO_CONVERSATION_FAULT_WINDOW      11u  /* the door refused the key
                                                     inside its window */
#define ECUIO_CONVERSATION_FAULT_CHANNEL     12u  /* no such channel */

/* POD, 64 bytes, the same on every pointer width */
typedef struct ecuio_channel_conversation {
    uint32_t flags;          /* ECUIO_CONVERSATION_NOT_KEPT */
    uint32_t rx_id;          /* the channel's receive ID: its module */
    uint32_t generation;     /* moves with every change of the report;
                                ecuio_session_security_unlock names it */
    uint8_t state;           /* ECUIO_CONVERSATION_* */
    uint8_t session;         /* what begin asked: 0 default, 3 extended */
    uint8_t fault;           /* ECUIO_CONVERSATION_FAULT_* */
    uint8_t fault_len;       /* of fault_bytes */
    uint8_t fault_bytes[4];  /* a refused request's first bytes */
    uint8_t level;           /* the last word's level */
    uint8_t unlock;          /* ECUIO_UNLOCK_* */
    uint8_t nrc;             /* a refused seed's or key's code; 0 none */
    uint8_t reserved0;       /* zero */
    uint32_t delay_left_ms;  /* level's delay left now; 0 for none */
    uint32_t words_ended;    /* each word's end */
    uint32_t requests;       /* the code's requests the door took */
    uint32_t failed;         /* level's failed attempts since its last
                                right key, as Havne counted them */
    uint32_t reserved[6];    /* zero; ABI growth space */
} ecuio_channel_conversation_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_channel_conversation_t) == 64, "ecuio_channel_conversation_t size is ABI");
static_assert(offsetof(ecuio_channel_conversation_t, state) == 12, "conversation state offset is ABI");
static_assert(offsetof(ecuio_channel_conversation_t, level) == 20, "conversation level offset is ABI");
static_assert(offsetof(ecuio_channel_conversation_t, delay_left_ms) == 24, "conversation delay offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_channel_conversation_t) == 64, "ecuio_channel_conversation_t size is ABI");
_Static_assert(offsetof(ecuio_channel_conversation_t, state) == 12, "conversation state offset is ABI");
_Static_assert(offsetof(ecuio_channel_conversation_t, level) == 20, "conversation level offset is ABI");
_Static_assert(offsetof(ecuio_channel_conversation_t, delay_left_ms) == 24, "conversation delay offset is ABI");
#endif

/* Copies channel `channel`'s conversation report into *out: state NONE,
 * and the rest zero but rx_id, where no plugin's code talks. NULL
 * session or out: ECUIO_ERR_NULL_ARGUMENT; a channel at or past the
 * info's module_count: ECUIO_ERR_INVALID_ARGUMENT, out untouched. Any
 * thread until destroy; after close it keeps the last report. */
ecuio_status_t ecuio_session_get_channel_conversation(
    const ecuio_session_t* session,
    uint32_t channel,
    ecuio_channel_conversation_t* out
);

/* Names the security store (ADR-0066 section 2.3) the session's
 * conversations count their attempts in, before start. label is the
 * car's name the host gives: the store's key for a module only when the
 * module answers no serial (22 F18C) and the car no VIN, trimmed and
 * case folded (NULL or "" for none). The file is read here: one that
 * doesn't read is ECUIO_ERR_LOAD_FAILED with words naming no path. NULL
 * session or path: ECUIO_ERR_NULL_ARGUMENT; after start:
 * ECUIO_ERR_INVALID_ARGUMENT. With no store the counts live in the
 * session and the report says ECUIO_CONVERSATION_NOT_KEPT. A label can
 * be found again from the file by trying names, a VIN by trying VINs:
 * the store stays on this machine, never in a report. */
ecuio_status_t ecuio_session_security_store(
    ecuio_session_t* session,
    const char* path,
    const char* label
);

/* The caller's word for one attempt at security access level `level`
 * on channel `channel`, against the report it was shown (`generation`,
 * else ECUIO_ERR_REPORT_CHANGED): the attempt's mark is saved in the
 * store, then the worker asks the seed, the plugin's code computes the
 * key and core sends it once. Never asked again by core. Any thread.
 * Refused, with nothing sent and nothing counted:
 *   ECUIO_ERR_INVALID_ARGUMENT with words: no code talks on the channel,
 *     it isn't talking, no such level, a key that doesn't fit one
 *     request on the adapter, a functional channel's session, an attempt
 *     under way, or no name for the module in the store;
 *   ECUIO_ERR_TOO_SOON: the level's delay runs, *out_wait_ms its time
 *     left (above 0); or the module's identity is still being read,
 *     *out_wait_ms 0;
 *   ECUIO_ERR_LOAD_FAILED: the store can't save the mark, in its words;
 *   ECUIO_ERR_SESSION_CLOSED, ECUIO_ERR_OUT_OF_MEMORY.
 * out_wait_ms may be NULL. How the attempt ended is the report's. A
 * close after the seed was asked and before the key's answer leaves the
 * mark in the store: the next load counts it as a failed attempt, so a
 * key the ECU may have judged is never left uncounted. */
ecuio_status_t ecuio_session_security_unlock(
    ecuio_session_t* session,
    uint32_t channel,
    uint8_t level,
    uint32_t generation,
    uint32_t* out_wait_ms
);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_CORE_H */
