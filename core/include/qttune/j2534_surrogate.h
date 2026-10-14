// File/layer: core/include/qttune/j2534_surrogate.h | C ABI, x86-surrogate client
// Testing: HARNESS — test_j2534_surrogate spawns the real x86 surrogate
//          hosting the x86-built fake DLL, end-to-end over the pipe+ring.
//          Non-Windows: stubs only.

#ifndef QTTUNE_J2534_SURROGATE_H
#define QTTUNE_J2534_SURROGATE_H

#include "qttune/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qttune_j2534_sur_handle qttune_j2534_sur_handle_t;

/* Spawn the x86 surrogate hosting dll_path. Creates pipe/ring/event,
 * spawns under a kill-on-close job, handshakes OP_OPEN. Blocking.
 * QT_ERR_LOAD_FAILED: exe missing, spawn failed, DLL load failed, or
 * PassThruOpen refused. QT_ERR_NO_TRANSPORT: handshake timeout. */
qttune_status_t qttune_j2534_sur_open(const char* dll_path,
                                      qttune_j2534_sur_handle_t** out_handle);

/* Channel lifecycle. baud==0 resolves to the ISO 15765 500k default,
 * mirroring the in-proc loader contract. */
qttune_status_t qttune_j2534_sur_connect_channel(qttune_j2534_sur_handle_t* handle,
                                                 unsigned long baud,
                                                 unsigned long flags,
                                                 unsigned long* out_channel);
qttune_status_t qttune_j2534_sur_disconnect_channel(qttune_j2534_sur_handle_t* handle,
                                                    unsigned long channel);

/* Drain one frame from the shared ring (the app-side data plane).
 * Same translation contract as the in-proc read path: arb id 0,
 * EXTENDED from rx_status bit, vendor timestamp passthrough, 64-clamp. */
qttune_status_t qttune_j2534_sur_read_frame(qttune_j2534_sur_handle_t* handle,
                                            unsigned long channel,
                                            qttune_frame_t* out_frame,
                                            uint32_t timeout_ms,
                                            bool* got_frame);

/* TEST-ONLY: forward an injected frame through the surrogate into the
 * loaded module's InjectFrame export (fake DLL). UNIT/HARNESS use. */
qttune_status_t qttune_j2534_sur_inject_test_frame(qttune_j2534_sur_handle_t* handle,
                                                   const uint8_t* data,
                                                   uint32_t len,
                                                   uint32_t timestamp,
                                                   uint32_t rx_flags);

/* Polite SHUTDOWN, bounded wait, then job-object kill. NULL-safe. */
qttune_status_t qttune_j2534_sur_close(qttune_j2534_sur_handle_t* handle);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_J2534_SURROGATE_H */
