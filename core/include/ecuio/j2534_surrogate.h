// File/layer: core/include/ecuio/j2534_surrogate.h | C ABI, surrogate client (x86 and x64)
// Testing: HARNESS — test_j2534_surrogate spawns the real surrogate
//          hosting the fake DLL, end to end over the pipe.
//          Non-Windows: stubs only.

#ifndef ECUIO_J2534_SURROGATE_H
#define ECUIO_J2534_SURROGATE_H

#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ecuio_j2534_sur_handle ecuio_j2534_sur_handle_t;

/* Spawn the surrogate hosting dll_path under a kill-on-close job and
 * handshake with OP_OPEN. Blocking. dll_arch (ECUIO_DLL_ARCH_*, the
 * loader's sniff) picks the exe: x86 runs ecuio-j2534-surrogate.exe
 * (relocated by ECUIO_SURROGATE_EXE), anything else
 * ecuio-j2534-surrogate-x64.exe (ECUIO_SURROGATE_X64_EXE), ADR-0025. A DLL the child couldn't load or
 * resolve (exit 4 or 5) returns ECUIO_ERR_LOAD_FAILED; a refused or
 * never-returning PassThruOpen ECUIO_ERR_VENDOR_REFUSED; a missing exe,
 * a failed spawn or handshake, or a child from other shared headers
 * ECUIO_ERR_SURROGATE_FAILED. ecuio_last_error_detail says which. */
ecuio_status_t ecuio_j2534_sur_open(const char* dll_path, uint32_t dll_arch,
                                    ecuio_j2534_sur_handle_t** out_handle);

/* Channel lifecycle. baud==0 resolves to the ISO 15765 500k default
 * (the transport's "unset"). */
ecuio_status_t ecuio_j2534_sur_connect_channel(ecuio_j2534_sur_handle_t* handle,
                                               unsigned long baud,
                                               unsigned long flags,
                                               unsigned long* out_channel);
ecuio_status_t ecuio_j2534_sur_disconnect_channel(ecuio_j2534_sur_handle_t* handle,
                                                  unsigned long channel);

/* ---- Write / ioctl / filter relay ----
 *
 * These return the loader seam's codes (internal.h, ADR-0018 appendix
 * of 2026-10-23): ECUIO_ERR_NULL_ARGUMENT for a NULL pointer,
 * ECUIO_ERR_INVALID_ARGUMENT for a length or shape refused before the
 * pipe, ECUIO_ERR_VENDOR_REFUSED for a status other than
 * STATUS_NOERROR (the vendor's, or the child's own refusal, given as a
 * strict vendor would), and ECUIO_ERR_SURROGATE_FAILED when the child
 * doesn't answer or answers past the wire's limits. Disconnect ignores
 * the vendor's status; only a failed transaction fails it. */

/* One PassThruWriteMsgs call. data_len must be <= 64 (the wire limit).
 * out_num_written is optional (NULL ok) and receives the vendor's
 * actual-written count — callers should treat <1 as a failed send. */
ecuio_status_t ecuio_j2534_sur_write_frame(ecuio_j2534_sur_handle_t* handle,
                                              unsigned long channel,
                                              const uint8_t* data,
                                              uint32_t len,
                                              uint32_t tx_flags,
                                              uint32_t timeout_ms,
                                              uint32_t* out_num_written);

/* Generic ioctl relay. input is a FLATTENED SCONFIG array (u32
 * Parameter/Value pairs, little-endian) for GET_CONFIG/SET_CONFIG;
 * input_len==0 for ioctls taking NULL input. output receives the first
 * output_cap bytes of the response payload (GET_CONFIG write-back or
 * READ_VBATT's ULONG), and out_len its full length, so a caller spots a
 * cut by comparing. A reply that claims more than the wire carries (64
 * bytes) is rejected as a protocol skew (ECUIO_ERR_SURROGATE_FAILED). */
ecuio_status_t ecuio_j2534_sur_ioctl(ecuio_j2534_sur_handle_t* handle,
                                     unsigned long channel,
                                     uint32_t ioctl_id,
                                     const uint8_t* input,
                                     uint32_t input_len,
                                     uint8_t* output,
                                     uint32_t output_cap,
                                     uint32_t* out_len);

/* PassThruReadVersion relay: the firmware, DLL and API versions into
 * three 80-byte buffers, each NUL-terminated as the vendor wrote it.
 * ECUIO_ERR_NOT_IMPLEMENTED when the DLL doesn't export the call,
 * ECUIO_ERR_VENDOR_REFUSED when the vendor refuses it,
 * ECUIO_ERR_SURROGATE_FAILED when the child doesn't answer; the buffers
 * then hold "". */
ecuio_status_t ecuio_j2534_sur_read_version(ecuio_j2534_sur_handle_t* handle,
                                            char* firmware,
                                            char* dll,
                                            char* api);

/* PassThruStartMsgFilter relay. mask/pattern(/flow_control) are flat
 * byte arrays (1..12 bytes each, spec max); fc may be NULL when
 * fc_len==0 (PASS/BLOCK filters). tx_flags carries the filter
 * messages' TxFlags (CAN_29BIT_ID for 29-bit matching). */
ecuio_status_t ecuio_j2534_sur_start_msg_filter(ecuio_j2534_sur_handle_t* handle,
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

ecuio_status_t ecuio_j2534_sur_stop_msg_filter(ecuio_j2534_sur_handle_t* handle,
                                                  unsigned long channel,
                                                  unsigned long filter_id);

/* Return one received message: from the cached batch if one is
 * pending, otherwise via one OP_READ (up to 16 messages). The contract
 * of ecuio_j2534_read_msg: ERR_TIMEOUT -> ECUIO_ERR_VEHICLE_TIMEOUT,
 * ERR_BUFFER_EMPTY -> no message, other statuses or a pipe failure ->
 * ECUIO_ERR_TRANSPORT_HARD_FAILURE; the message as the vendor gave it
 * (ecuio_pt_rx_t, j2534_ptypes.h): RxStatus, the raw timestamp, and
 * Data whole with the CAN ID in front. */
ecuio_status_t ecuio_j2534_sur_read_msg(ecuio_j2534_sur_handle_t* handle,
                                        unsigned long channel,
                                        ecuio_pt_rx_t* out_msg,
                                        uint32_t timeout_ms,
                                        bool* got_msg);

/* TEST-ONLY: forward an injected frame through the surrogate into the
 * loaded module's InjectFrame export (fake DLL). UNIT/HARNESS use. */
ecuio_status_t ecuio_j2534_sur_inject_test_frame(ecuio_j2534_sur_handle_t* handle,
                                                 const uint8_t* data,
                                                 uint32_t len,
                                                 uint32_t timestamp,
                                                 uint32_t rx_flags);

/* Polite SHUTDOWN, bounded wait, then job-object kill. NULL-safe. */
ecuio_status_t ecuio_j2534_sur_close(ecuio_j2534_sur_handle_t* handle);

/* Path of this session's log, which holds the child's lines and the
 * client's. Usually %TEMP%\ecuio-surrogate.log, and the log it replaces
 * is kept as %TEMP%\ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log, named
 * after its last write (the newest 50 are kept). A session opened while
 * another holds that file gets %TEMP%\ecuio-surrogate-<pid>-<n>.log.
 * "" when the session has no log (or handle is NULL, or not Windows).
 * Valid until ecuio_j2534_sur_close. */
const char* ecuio_j2534_sur_log_path(const ecuio_j2534_sur_handle_t* handle);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_J2534_SURROGATE_H */
