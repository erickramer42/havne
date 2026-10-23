// File/layer: core/include/ecuio/core.h | C ABI, public interface
// Testing: Public API contract — every function needs a corresponding test

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

/* Frame structure — POD only, no pointers. Layout is pinned by the
 * assertions below: 88 bytes, no hidden padding on any supported
 * platform (ADR-0006).
 */
typedef struct ecuio_frame {
    uint64_t timestamp_us;    /* Microseconds since the session's transport
                                 attached, monotonic per channel. Mock: the
                                 host's steady clock. J2534: the host's at the
                                 first frame, then the adapter's own deltas,
                                 carried across its 32-bit wrap */
    uint32_t arbitration_id;  /* CAN identifier: <= 0x7FF standard, up to
                                 0x1FFFFFFF when FLAG_EXTENDED set */
    uint16_t channel_id;      /* Bus/channel index only — never multiplexes
                                 an arbitration ID */
    uint16_t data_length;     /* Bytes used in data[]: 0..64 */
    uint8_t  data[64];        /* Classic CAN (8) .. CAN-FD (64) payload */
    uint32_t flags;           /* ECUIO_FRAME_FLAG_* bitmask */
    uint32_t reserved;        /* Producers MUST leave zero; ABI growth space */
} ecuio_frame_t;

/* Compile-time ABI pinning. C++ and C11 paths; C99 falls back to the
 * negative-array-size idiom in tests/test_c_abi.c. */
#if defined(__cplusplus)
static_assert(sizeof(ecuio_frame_t) == 88, "ecuio_frame_t size is ABI — bump version if this moves");
static_assert(offsetof(ecuio_frame_t, data) == 16, "frame data offset is ABI");
static_assert(offsetof(ecuio_frame_t, flags) == 80, "frame flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_frame_t) == 88, "ecuio_frame_t size is ABI");
_Static_assert(offsetof(ecuio_frame_t, data) == 16, "frame data offset is ABI");
_Static_assert(offsetof(ecuio_frame_t, flags) == 80, "frame flags offset is ABI");
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
    ECUIO_ERR_LOAD_FAILED = 7,    /* the adapter DLL wouldn't load, or
                                     lacks a mandatory J2534 export */
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

    /* Append-only rule: new codes go HERE, never renumber */
} ecuio_status_t;

/* Lifecycle */
ecuio_status_t ecuio_init(void);
void ecuio_shutdown(void);
const char* ecuio_version_string(void);
const char* ecuio_status_string(ecuio_status_t status);

/* Session management.
 *
 * Lifecycle follows FILE/free discipline:
 * - create()  — allocates and attaches a transport (fopen)
 * - close()   — stops streaming, detaches the transport, releases worker
 *               and callback state. A second close returns
 *               ECUIO_ERR_SESSION_CLOSED; the session stays valid until
 *               destroy().
 * - destroy() — frees the session (free). The handle is INVALID afterward;
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

/* Send/receive - synchronous API */
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
 *   ecuio_session_destroy() — they synchronize with the worker
 * - register/unregister ARE permitted from inside a callback:
 *   dispatch snapshots the callback list under lock and invokes
 *   unlocked, so reentry cannot deadlock (tested:
 *   SelfUnregisterDuringLiveDispatch)
 * - Duplicate (callback, user_data) pairs are silently deduplicated
 * - unregister is idempotent—calling it twice is safe
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
 * Treat any other value as degraded: later cores may append codes.
 * error_count counts episodes, each entry into a request failure or a
 * hard failure. Callable from any thread. The mock transport never sets
 * them. */
ecuio_status_t ecuio_session_get_error_state(
    ecuio_session_t* session,
    int32_t* out_last_error,
    uint32_t* out_error_count
);

/* What a session runs on, as its connect found it (ADR-0019). The
 * values below are append-only, like the status codes. */
#define ECUIO_SESSION_INFO_TEXT_LEN      80   /* PassThruReadVersion's
                                                 buffers (J2534-1 v04.04) */
#define ECUIO_SESSION_INFO_MAX_CHANNELS  4    /* signals.h's
                                                 ECUIO_MAX_CHANNELS */
#define ECUIO_SESSION_INFO_PROFILE_LEN   64   /* profile.h's
                                                 ECUIO_PROFILE_NAME_MAX + 1 */

/* backend */
#define ECUIO_BACKEND_MOCK               1u
#define ECUIO_BACKEND_J2534_INPROC       2u   /* the adapter's DLL loaded
                                                 in this process */
#define ECUIO_BACKEND_J2534_SURROGATE    3u   /* in the surrogate process
                                                 (ADR-0008) */

/* dll_arch: the adapter DLL's PE machine, as discovery's reserved[0]
 * reports it (j2534.h) */
#define ECUIO_DLL_ARCH_UNKNOWN           0u
#define ECUIO_DLL_ARCH_X86               1u
#define ECUIO_DLL_ARCH_X64               2u

/* flags: which of the optional readings the adapter gave */
#define ECUIO_SESSION_INFO_VBATT         0x00000001u   /* vbatt_mv */
#define ECUIO_SESSION_INFO_VERSIONS      0x00000002u   /* the three
                                                          version strings */

/* POD, 432 bytes, no hidden padding on any supported platform; pinned
 * by the assertions below. New fields take the reserved room. */
typedef struct ecuio_session_info {
    uint32_t flags;           /* ECUIO_SESSION_INFO_* */
    uint32_t backend;         /* ECUIO_BACKEND_* */
    uint32_t dll_arch;        /* ECUIO_DLL_ARCH_*; UNKNOWN for mock */
    uint32_t protocol_id;     /* the J2534 ProtocolID the channel connected
                                 with (6 = ISO 15765); 0 for mock */
    uint32_t baud;            /* bit/s it connected at; 0 for mock */
    uint32_t connect_flags;   /* PassThruConnect's Flags: ?flags=, plus
                                 CAN_29BIT_ID (0x100) for 29-bit channels */
    uint32_t vbatt_mv;        /* battery voltage at connect, millivolts
                                 (READ_VBATT); with ECUIO_SESSION_INFO_VBATT */
    uint32_t channel_count;   /* CAN channels the session filters and
                                 requests on; 0 for a passive session */
    uint32_t rx_ids[ECUIO_SESSION_INFO_MAX_CHANNELS];   /* each one's
                                                           receive ID */
    uint32_t tx_ids[ECUIO_SESSION_INFO_MAX_CHANNELS];   /* and transmit ID */
    /* The adapter's firmware, its DLL and the J2534 API version it
     * implements (PassThruReadVersion), each NUL-terminated on one line;
     * "" without ECUIO_SESSION_INFO_VERSIONS. */
    char firmware_version[ECUIO_SESSION_INFO_TEXT_LEN];
    char dll_version[ECUIO_SESSION_INFO_TEXT_LEN];
    char api_version[ECUIO_SESSION_INFO_TEXT_LEN];
    /* The profile the session decodes through (ADR-0013): ?profile=, or
     * the scheme's default. NUL-terminated. */
    char profile[ECUIO_SESSION_INFO_PROFILE_LEN];
    uint32_t reserved[16];    /* zero; ABI growth space */
} ecuio_session_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_session_info_t) == 432, "ecuio_session_info_t size is ABI");
static_assert(offsetof(ecuio_session_info_t, firmware_version) == 64, "session info text offset is ABI");
static_assert(offsetof(ecuio_session_info_t, reserved) == 368, "session info reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_session_info_t) == 432, "ecuio_session_info_t size is ABI");
_Static_assert(offsetof(ecuio_session_info_t, firmware_version) == 64, "session info text offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, reserved) == 368, "session info reserved offset is ABI");
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

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_CORE_H */
