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
 *   j2534://[<selector>][?baud=<decimal>[&flags=<0xHEX|decimal>]
 *                           [&filter=<CAN_ID>[&txid=<CAN_ID>]
 *                           [&poll=<PID>]]]
 *
 * Selector (unchanged):
 *   j2534://                        first discovered device
 *   j2534://<vendor-name>           discovered vendor, matched
 *                                   case-insensitively
 *   j2534://<path ending .dll or    direct DLL load (tests/dev)
 *            containing \ or />)
 *
 * Query parameters (strict — malformed URIs fail at session_create
 * with QT_ERR_NO_TRANSPORT, before any DLL load):
 *   baud    decimal, nonzero; default resolved by connect_channel to
 *           the ISO 15765 500k default
 *   flags   0x-prefixed hex or decimal, passed to PassThruConnect
 *           verbatim — bit SEMANTICS are not interpreted pre-BENCH
 *   filter  CAN id 1..0x1FFFFFFF — receive ID (registers a
 *           FLOW_CONTROL filter on this ID; ISO 15765 receive
 *           prerequisite). Frames matching this ID get their
 *           4-byte address header stripped and arbitration_id stamped.
 *   txid    CAN id 1..0x1FFFFFFF — transmit ID; default 0x7E0
 *   poll    PID 1..0xFF — Mode-01 request loop (100 ms cadence)
 *           COUPLING: poll without filter is rejected at parse time.
 *   lowercase keys only, unknown keys rejected, duplicate keys last-wins
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

/* ---- Part 4: WRITE / IOCTL / FILTER vendor-call helpers ----
 *
 * Same routing discipline as the helpers above: each dispatches
 * branch-on-backend inside j2534_loader.cpp and the policy layer stays
 * backend-blind. All SCONFIG traffic is FLATTENED (Parameter, Value)
 * u32 pairs, little-endian, 8 bytes per pair, max
 * QTTUNE_PT_SCONFIG_MAX_PARAMS pairs — pointer-bearing structs
 * (SCONFIG_LIST, raw PASSTHRU_MSG*) never cross the internal seam so
 * the in-proc and surrogate signatures are identical by construction.
 */

/* Single-message PassThruWriteMsgs relay. data_len <= 64 (frame ABI
 * cap; ISO-TP multiframe segmentation is transport-layer work, not
 * loader work). tx_flags carries J2534 TxFlags verbatim (bit 8 =
 * CAN_29BIT_ID for 29-bit targets). timeout_ms passes through to the
 * vendor unchanged; 0 = non-blocking. out_num_written may be NULL;
 * *out_num_written < 1 after QT_OK means the vendor reported
 * success-but-wrote-nothing — callers treat that as a failed send. */
qttune_status_t qttune_j2534_write_frame(qttune_j2534_handle_t* handle,
                                         unsigned long channel,
                                         const uint8_t* data,
                                         uint32_t len,
                                         uint32_t tx_flags,
                                         uint32_t timeout_ms,
                                         uint32_t* out_num_written);

/* Generic PassThruIoCtl relay. For GET_CONFIG/SET_CONFIG: input is a
 * flattened SCONFIG array as described above, input_len a nonzero
 * multiple of 8; for GET_CONFIG the vendor-updated pairs return in
 * output. For READ_VBATT: input_len 0, 4-byte result in output.
 * Pointer-bearing ioctls (FAST_INIT etc.) are out of contract — the
 * relay refuses them at this seam rather than guessing layouts.
 * output_cap bounds the copy; *out_len receives the actual length. */
qttune_status_t qttune_j2534_ioctl(qttune_j2534_handle_t* handle,
                                  unsigned long channel,
                                  uint32_t ioctl_id,
                                  const uint8_t* input,
                                  uint32_t input_len,
                                  uint8_t* output,
                                  uint32_t output_cap,
                                  uint32_t* out_len);

/* PassThruStartMsgFilter relay. mask/pattern(/flow_control) are flat
 * byte arrays, 1..12 bytes each (J2534-1 spec max); mask_len MUST
 * equal pattern_len; flow_control participates only with
 * QTTUNE_PT_FLOW_CONTROL_FILTER (fc_len must then equal mask_len;
 * for PASS/BLOCK filters pass fc_len 0 and flow_control NULL).
 * tx_flags stamps the filter messages' TxFlags (CAN_29BIT_ID for
 * 29-bit matching — mask and pattern are compared against the frame
 * ID in the filter messages' Data[], big-endian, per J2534-1 §5.7). */
qttune_status_t qttune_j2534_start_msg_filter(qttune_j2534_handle_t* handle,
                                              unsigned long channel,
                                              uint32_t filter_type,
                                              const uint8_t* mask,
                                              uint32_t mask_len,
                                              const uint8_t* pattern,
                                              uint32_t pattern_len,
                                              const uint8_t* flow_control,
                                              uint32_t fc_len,
                                              uint32_t tx_flags,
                                              unsigned long* out_filter_id);

/* PassThruStopMsgFilter relay. Idempotence is the VENDOR'S contract
 * (stopping a stopped filter returns ERR_FAILED) — we surface the
 * vendor status verbatim rather than second-guessing it. */
qttune_status_t qttune_j2534_stop_msg_filter(qttune_j2534_handle_t* handle,
                                             unsigned long channel,
                                             unsigned long filter_id);

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
