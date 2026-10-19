// File/layer: core/include/qttune/j2534_surrogate.h | C ABI, x86-surrogate client
// Testing: HARNESS — test_j2534_surrogate spawns the real surrogate
//          hosting the fake DLL, end to end over the pipe.
//          Non-Windows: stubs only.

#ifndef QTTUNE_J2534_SURROGATE_H
#define QTTUNE_J2534_SURROGATE_H

#include "qttune/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qttune_j2534_sur_handle qttune_j2534_sur_handle_t;

/* Spawn the surrogate hosting dll_path under a kill-on-close job and
 * handshake with OP_OPEN. Blocking. Every startup failure (exe
 * missing, spawn failed, DLL load failed, handshake timeout,
 * PassThruOpen refused) returns QT_ERR_LOAD_FAILED. */
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

/* ---- Write / ioctl / filter relay ---- */

/* One PassThruWriteMsgs call. data_len must be <= 64 (the wire limit).
 * out_num_written is optional (NULL ok) and receives the vendor's
 * actual-written count — callers should treat <1 as a failed send. */
qttune_status_t qttune_j2534_sur_write_frame(qttune_j2534_sur_handle_t* handle,
                                              unsigned long channel,
                                              const uint8_t* data,
                                              uint32_t len,
                                              uint32_t tx_flags,
                                              uint32_t timeout_ms,
                                              uint32_t* out_num_written);

/* Generic ioctl relay. input is a FLATTENED SCONFIG array (u32
 * Parameter/Value pairs, little-endian) for GET_CONFIG/SET_CONFIG;
 * input_len==0 for ioctls taking NULL input. output receives up to
 * output_cap bytes of response payload (GET_CONFIG write-back or
 * READ_VBATT's ULONG); out_len receives the actual length. Caps at
 * 64 bytes — the wire limit; keep caller buffers at least that. */
qttune_status_t qttune_j2534_sur_ioctl(qttune_j2534_sur_handle_t* handle,
                                       unsigned long channel,
                                       uint32_t ioctl_id,
                                       const uint8_t* input,
                                       uint32_t input_len,
                                       uint8_t* output,
                                       uint32_t* out_len);

/* PassThruStartMsgFilter relay. mask/pattern(/flow_control) are flat
 * byte arrays (1..12 bytes each, spec max); fc may be NULL when
 * fc_len==0 (PASS/BLOCK filters). tx_flags carries the filter
 * messages' TxFlags (CAN_29BIT_ID for 29-bit matching). */
qttune_status_t qttune_j2534_sur_start_msg_filter(qttune_j2534_sur_handle_t* handle,
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

qttune_status_t qttune_j2534_sur_stop_msg_filter(qttune_j2534_sur_handle_t* handle,
                                                  unsigned long channel,
                                                  unsigned long filter_id);

/* Return one frame: from the cached batch if one is pending, otherwise
 * via one OP_READ (up to 16 frames). Same contract as the in-proc read
 * path: ERR_TIMEOUT -> QT_ERR_VEHICLE_TIMEOUT, ERR_BUFFER_EMPTY -> no
 * frame, other statuses or a pipe failure -> QT_ERR_TRANSPORT_HARD_FAILURE;
 * arb id and channel id 0, EXTENDED from RxStatus, vendor timestamp
 * passed through, payload clamped to 64 bytes. */
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
