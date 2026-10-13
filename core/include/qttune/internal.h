#ifndef QTTUNE_INTERNAL_H
#define QTTUNE_INTERNAL_H

#include "qttune/core.h"

#ifdef __cplusplus
extern "C" {
#endif

void qttune_session_dispatch_callbacks(qttune_session_t* session,
                                       const qttune_frame_t* frame);

/*
 * Transport abstraction (internal): sessions own a transport instance
 * polymorphically. ops is a pointer to static, immutable vtable storage
 * owned by the transport implementation TU; impl is the implementation's
 * private state. Both start() and detach() are invoked by the session,
 * never by transports themselves — no re-entry into session lifecycle.
 *
 * Contract:
 * - start(): called from qttune_session_start. Must be idempotent
 *   (second call on a running transport returns QT_OK).
 * - detach(): called exactly once from qttune_session_close, AFTER the
 *   closed flag is set. Must stop any worker threads and free impl +
 *   the qttune_transport wrapper itself. May not touch session afterwards.
 */
typedef struct qttune_transport_ops {
    qttune_status_t (*start)(qttune_session_t* session, void* impl);
    void (*detach)(qttune_session_t* session, void* impl);
} qttune_transport_ops_t;

typedef struct qttune_transport {
    const qttune_transport_ops_t* ops;
    void* impl;
} qttune_transport_t;

/* Mock transport factory — attaches a transport to a session (idempotent) */
qttune_status_t qttune_mock_transport_attach(qttune_session_t* session,
                                            const char* uri);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_INTERNAL_H */
