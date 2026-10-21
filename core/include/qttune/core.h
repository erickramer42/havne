// File/layer: core/include/qttune/core.h | C ABI, public interface
// Testing: Public API contract — every function needs a corresponding test

#ifndef QTTUNE_CORE_H
#define QTTUNE_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declared opaque handle types */
typedef struct qttune_session qttune_session_t;

/* Frame flag bitfield. A zeroed frame == plain classic CAN data frame.
 * Bit positions are ABI-stable; new flags APPEND only, never renumber.
 */
#define QTTUNE_FRAME_FLAG_EXTENDED       0x00000001u /* 29-bit arbitration ID */
#define QTTUNE_FRAME_FLAG_REMOTE          0x00000002u /* remote transmission request */
#define QTTUNE_FRAME_FLAG_FD              0x00000004u /* CAN-FD frame */
#define QTTUNE_FRAME_FLAG_BITRATE_SWITCH  0x00000008u /* FD bitrate switch (BRS) */
#define QTTUNE_FRAME_FLAG_ERROR           0x00000010u /* bus/controller error report */
#define QTTUNE_FRAME_FLAG_TX_ECHO         0x00000020u /* echo of our own transmit */

/* Frame structure — POD only, no pointers. Layout is pinned by the
 * assertions below: 88 bytes, no hidden padding on any supported
 * platform (ADR-0006).
 */
typedef struct qttune_frame {
    uint64_t timestamp_us;    /* Microseconds since session start (producer's
                                 steady clock, monotonic per channel). The
                                 J2534 transport passes the vendor's 32-bit
                                 timestamp through instead (Known Issue #12) */
    uint32_t arbitration_id;  /* CAN identifier: <= 0x7FF standard, up to
                                 0x1FFFFFFF when FLAG_EXTENDED set */
    uint16_t channel_id;      /* Bus/channel index only — never multiplexes
                                 an arbitration ID */
    uint16_t data_length;     /* Bytes used in data[]: 0..64 */
    uint8_t  data[64];        /* Classic CAN (8) .. CAN-FD (64) payload */
    uint32_t flags;           /* QTTUNE_FRAME_FLAG_* bitmask */
    uint32_t reserved;        /* Producers MUST leave zero; ABI growth space */
} qttune_frame_t;

/* Compile-time ABI pinning. C++ and C11 paths; C99 falls back to the
 * negative-array-size idiom in tests/test_c_abi.c. */
#if defined(__cplusplus)
static_assert(sizeof(qttune_frame_t) == 88, "qttune_frame_t size is ABI — bump version if this moves");
static_assert(offsetof(qttune_frame_t, data) == 16, "frame data offset is ABI");
static_assert(offsetof(qttune_frame_t, flags) == 80, "frame flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(qttune_frame_t) == 88, "qttune_frame_t size is ABI");
_Static_assert(offsetof(qttune_frame_t, data) == 16, "frame data offset is ABI");
_Static_assert(offsetof(qttune_frame_t, flags) == 80, "frame flags offset is ABI");
#endif

/* Callback function signature - called from transport thread */
typedef void (*qttune_frame_callback_t)(
    const qttune_frame_t* frame,
    void* user_data
);

/* Status codes - append-only evolution for ABI stability */
typedef enum {
    QT_OK = 0,
    QT_ERR_NULL_ARGUMENT = 1,
    QT_ERR_NOT_IMPLEMENTED = 2,
    QT_ERR_NO_TRANSPORT = 3,
    QT_ERR_OUT_OF_MEMORY = 4,
    QT_ERR_SESSION_CLOSED = 5,
    QT_ERR_ALREADY_REGISTERED = 6,  /* Not returned: a duplicate callback
                                       registration succeeds (pair identity,
                                       ADR-0002). Reserved, since codes are
                                       append-only */
    QT_ERR_LOAD_FAILED = 7,
    /* Link-health codes: published by the J2534 worker through
     * qttune_session_get_error_state while the session keeps running. */
    QT_ERR_VEHICLE_TIMEOUT = 8,         /* reads answer, but nothing arrives */
    QT_ERR_TRANSPORT_HARD_FAILURE = 9,  /* reads fail: the adapter isn't
                                           answering */
    QT_ERR_REQUEST_FAILED = 10,  /* writes fail while reads stay clean: no
                                    node ACKs the requests (ignition off,
                                    ECU asleep, no vehicle). The adapter is
                                    fine, so this isn't a hard failure */

    /* Append-only rule: new codes go HERE, never renumber */
} qttune_status_t;

/* Lifecycle */
qttune_status_t qttune_init(void);
void qttune_shutdown(void);
const char* qttune_version_string(void);
const char* qttune_status_string(qttune_status_t status);

/* Session management.
 *
 * Lifecycle follows FILE/free discipline:
 * - create()  — allocates and attaches a transport (fopen)
 * - close()   — stops streaming, detaches the transport, releases worker
 *               and callback state. A second close returns
 *               QT_ERR_SESSION_CLOSED; the session stays valid until
 *               destroy().
 * - destroy() — frees the session (free). The handle is INVALID afterward;
 *               calling any API with it is undefined behavior, exactly as
 *               free(p); *p. Null is a safe no-op. Auto-closes first if
 *               still open.
 *
 * Recommended order: close() then destroy(). Skip close() only when the
 * session was never started.
 */
qttune_status_t qttune_session_create(
    const char* transport_uri,
    qttune_session_t** out_session
);

qttune_status_t qttune_session_close(qttune_session_t* session);

qttune_status_t qttune_session_destroy(qttune_session_t* session);

qttune_status_t qttune_session_start(qttune_session_t* session);

/* Send/receive - synchronous API */
qttune_status_t qttune_session_send(
    qttune_session_t* session,
    const qttune_frame_t* frame
);

qttune_status_t qttune_session_receive(
    qttune_session_t* session,
    qttune_frame_t* out_frame,
    uint32_t timeout_ms
);

/*
 * Callback API - async frame delivery
 *
 * CONTRACTS:
 * - Callbacks execute on transport thread, NOT the calling thread
 * - Callbacks must not block (>10ms risks transport backlog)
 * - Callbacks must not call qttune_session_close() or
 *   qttune_session_destroy() — they synchronize with the worker
 * - register/unregister ARE permitted from inside a callback:
 *   dispatch snapshots the callback list under lock and invokes
 *   unlocked, so reentry cannot deadlock (tested:
 *   SelfUnregisterDuringLiveDispatch)
 * - Duplicate (callback, user_data) pairs are silently deduplicated
 * - unregister is idempotent—calling it twice is safe
 */
qttune_status_t qttune_register_frame_callback(
    qttune_session_t* session,
    qttune_frame_callback_t callback,
    void* user_data
);

qttune_status_t qttune_unregister_frame_callback(
    qttune_session_t* session,
    qttune_frame_callback_t callback,
    void* user_data
);

/* Link health, written by the transport worker while the session keeps
 * running. last_error is QT_OK, QT_ERR_VEHICLE_TIMEOUT (no frame for
 * about 1 s) or the status of the latest hard failure; error_count
 * counts hard-failure episodes. Callable from any thread. The mock
 * transport never sets them. */
qttune_status_t qttune_session_get_error_state(
    qttune_session_t* session,
    int32_t* out_last_error,
    uint32_t* out_error_count
);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_CORE_H */
