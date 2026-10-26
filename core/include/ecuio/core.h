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
#define ECUIO_FRAME_FLAG_TRUNCATED        0x00000040u /* the message was longer than
                                                         data[] holds; data_length
                                                         bytes of it are here */

/* The most payload a frame carries: room for a whole ISO 15765-2 message
 * on classic CAN (4095 bytes, its 12-bit length), so a reassembled
 * reply arrives whole. */
#define ECUIO_FRAME_DATA_MAX 4096u

/* Frame structure — POD only, no pointers. Layout is pinned by the
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
    uint16_t channel_id;      /* Bus/channel index only — never multiplexes
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
static_assert(sizeof(ecuio_frame_t) == 4120, "ecuio_frame_t size is ABI — bump version if this moves");
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

    /* Append-only rule: new codes go HERE, never renumber */
} ecuio_status_t;

/* Lifecycle. Core needs no global setup: sessions, the profile registry
 * (profile.h) and discovery (j2534.h) work without ecuio_init. Both
 * calls are optional and do nothing; init always returns ECUIO_OK. A
 * caller may pair them, call either any number of times, or never call
 * them, and shutdown leaves open sessions working. */
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
 *   ecuio_session_destroy() — they synchronize with the worker
 * - register/unregister ARE permitted from inside a callback:
 *   dispatch snapshots the callback list under lock and invokes
 *   unlocked, so reentry cannot deadlock (tested:
 *   SelfUnregisterDuringLiveDispatch)
 * - Duplicate (callback, user_data) pairs are silently deduplicated
 * - unregister is idempotent—calling it twice is safe
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
#define ECUIO_BACKEND_REPLAY             4u   /* a recording's frames.csv
                                                 (ADR-0023) */

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
    /* replay:// (ADR-0023): the recording's frames, its span from the
     * first frame to the last, and the speed in thousandths (1000 = real
     * time, 0 = max); 0 for every other backend */
    uint32_t replay_frames;
    uint32_t replay_span_ms;
    uint32_t replay_speed_milli;
    uint32_t reserved[13];    /* zero; ABI growth space */
} ecuio_session_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_session_info_t) == 432, "ecuio_session_info_t size is ABI");
static_assert(offsetof(ecuio_session_info_t, firmware_version) == 64, "session info text offset is ABI");
static_assert(offsetof(ecuio_session_info_t, replay_frames) == 368, "session info replay offset is ABI");
static_assert(offsetof(ecuio_session_info_t, reserved) == 380, "session info reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_session_info_t) == 432, "ecuio_session_info_t size is ABI");
_Static_assert(offsetof(ecuio_session_info_t, firmware_version) == 64, "session info text offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, replay_frames) == 368, "session info replay offset is ABI");
_Static_assert(offsetof(ecuio_session_info_t, reserved) == 380, "session info reserved offset is ABI");
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

/* What each channel's ECU has said it supports (ADR-0015 §4): its answers
 * to the supported-PID chain, and the requests left out of the rotation
 * because it refused them. The J2534 worker updates it as the answers
 * arrive. The values below are append-only. */
#define ECUIO_SUPPORT_MAX_DROPPED        8

/* flags */
#define ECUIO_SUPPORT_PROBED             0x00000001u   /* the chain was asked */
#define ECUIO_SUPPORT_COMPLETE           0x00000002u   /* answered to its end,
                                                          so every PID is judged */
#define ECUIO_SUPPORT_SCAN_COMPLETE      0x00000004u   /* every scanned DID was
                                                          answered, found or
                                                          refused (ADR-0020) */

/* reason: why a request left the rotation */
#define ECUIO_DROP_BITMAP                1u   /* its PID's bit is clear */
#define ECUIO_DROP_NOT_SUPPORTED         2u   /* 7F <service> 12 or 31 */
#define ECUIO_DROP_SERVICE               3u   /* 7F <service> 11: not the
                                                 request, its whole service */
#define ECUIO_DROP_ACCESS                4u   /* 7F <service> 33 or 7F: it
                                                 needs security access or
                                                 another diagnostic session,
                                                 which core never asks for */

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
    ecuio_dropped_request_t dropped_list[ECUIO_SUPPORT_MAX_DROPPED];   /* the
                                 first ones dropped, in set order */
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
    uint32_t reserved[5];     /* zero; ABI growth space */
} ecuio_channel_support_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_dropped_request_t) == 8, "ecuio_dropped_request_t size is ABI");
static_assert(sizeof(ecuio_channel_support_t) == 152, "ecuio_channel_support_t size is ABI");
static_assert(offsetof(ecuio_channel_support_t, pid_bitmap) == 24, "support bitmap offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, scan_total) == 120, "support scan offset is ABI");
static_assert(offsetof(ecuio_channel_support_t, reserved) == 132, "support reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_dropped_request_t) == 8, "ecuio_dropped_request_t size is ABI");
_Static_assert(sizeof(ecuio_channel_support_t) == 152, "ecuio_channel_support_t size is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, pid_bitmap) == 24, "support bitmap offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, scan_total) == 120, "support scan offset is ABI");
_Static_assert(offsetof(ecuio_channel_support_t, reserved) == 132, "support reserved offset is ABI");
#endif

/* Copies what channel `channel` has learned into *out_support: an index
 * into the session info's channels, below its channel_count, so a mock
 * or passive session has none (ECUIO_ERR_INVALID_ARGUMENT). Callable from
 * any thread until destroy; after close it keeps the last update. */
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
    uint32_t generation;      /* changes with every item settled; 0 until
                                 one has been */
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
 * is an index below the session info's channel_count
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

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_CORE_H */
