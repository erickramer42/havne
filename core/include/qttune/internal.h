#ifndef QTTUNE_INTERNAL_H
#define QTTUNE_INTERNAL_H

#include "qttune/core.h"
#include "qttune/j2534.h"

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

/*
 * J2534 transport factory (Win32). URI grammar:
 *   j2534://[<selector>][?baud=<decimal>[&flags=<0xHEX|decimal>]]
 *
 * Selector (unchanged):
 *   j2534://                        first discovered device
 *   j2534://<vendor-name>           discovered vendor, matched
 *                                   case-insensitively; no match =
 *                                   QT_ERR_NO_TRANSPORT
 *   j2534://<path ending .dll or    direct DLL load (tests/dev)
 *            containing \ or />)
 *
 * Query parameters (strict — malformed URIs fail at session_create
 * with QT_ERR_NO_TRANSPORT, before any DLL load):
 *   baud    decimal, nonzero; default resolved by connect_channel to
 *           the ISO 15765 500k default
 *   flags   0x-prefixed hex or decimal, passed to PassThruConnect
 *           verbatim — bit SEMANTICS are not interpreted pre-BENCH
 *           (pending the vendor j2534.h diff); lowercase keys only,
 *           unknown keys rejected, duplicate keys last-wins
 *
 * Attaches a transport to the session (idempotent like mock).
 */
qttune_status_t qttune_j2534_transport_attach(qttune_session_t* session,
                                              const char* uri);

/* Vendor-call helpers implemented in j2534_loader.cpp —
 * j2534_transport.cpp policy layer goes through these so the vendor
 * ABI lives in exactly one TU. */
qttune_status_t qttune_j2534_connect_channel(qttune_j2534_handle_t* handle,
                                             unsigned long baud,
                                             unsigned long flags,
                                             unsigned long* out_channel);
qttune_status_t qttune_j2534_disconnect_channel(qttune_j2534_handle_t* handle,
                                                unsigned long channel);
qttune_status_t qttune_j2534_read_frame(qttune_j2534_handle_t* handle,
                                       unsigned long channel,
                                       qttune_frame_t* out_frame,
                                       uint32_t timeout_ms,
                                       bool* got_frame);
/* Backend introspection (routing diagnostics + tests): "inproc" or
 * "surrogate". NULL for a NULL handle. */
const char* qttune_j2534_backend_name(const qttune_j2534_handle_t* handle);

/* TEST-ONLY: inject a frame into the loaded module's InjectFrame hook
 * (fake DLL), routed by backend. UNIT/HARNESS use — never in product
 * paths; real frames arrive via qttune_j2534_read_frame. */
qttune_status_t qttune_j2534_inject_test_frame(qttune_j2534_handle_t* handle,
                                               const uint8_t* data,
                                               uint32_t len,
                                               uint32_t timestamp,
                                               uint32_t rx_flags);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_INTERNAL_H */
