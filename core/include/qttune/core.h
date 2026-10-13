// File/layer: core/include/qttune/core.h | C ABI, public interface
// Testing: Public API contract—every function needs corresponding test

#ifndef QTTUNE_CORE_H
#define QTTUNE_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declared opaque handle types */
typedef struct qttune_session qttune_session_t;

/* Frame structure - POD only, no pointers except length */
typedef struct qttune_frame {
    uint64_t timestamp_us;    /* Microseconds since epoch or session start */
    uint32_t channel_id;      /* Logical channel (e.g., CAN bus ID) */
    uint8_t data[64];         /* Max CAN-FD payload size */
    uint8_t data_length;      /* Actual bytes used (0-64) */
    uint8_t is_extended;      /* Extended frame flag (29-bit address) */
    uint8_t rsvd[3];          /* Padding for alignment */
} qttune_frame_t;

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
    QT_ERR_ALREADY_REGISTERED = 6,  /* Kept for backward compat if needed */
    QT_ERR_LOAD_FAILED = 7          /* New: DLL load/export resolution failed */
} qttune_status_t;

/* Lifecycle */
qttune_status_t qttune_init(void);
void qttune_shutdown(void);
const char* qttune_version_string(void);
const char* qttune_status_string(qttune_status_t status);

/* Session management */
qttune_status_t qttune_session_create(
    const char* transport_uri,
    qttune_session_t** out_session
);

qttune_status_t qttune_session_close(qttune_session_t* session);

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
 * - Callbacks must not call qttune_* APIs that acquire session lock
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

/* Optional: public transport API if you want to expose it */
qttune_status_t qttune_transport_attach(
    qttune_session_t* session,
    const char* transport_uri
);

void qttune_transport_detach(qttune_session_t* session);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_CORE_H */
