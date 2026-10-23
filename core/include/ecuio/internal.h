#ifndef ECUIO_INTERNAL_H
#define ECUIO_INTERNAL_H

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/j2534_ptypes.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ecuio_session_dispatch_callbacks(ecuio_session_t* session,
                                      const ecuio_frame_t* frame);

/*
 * Transport abstraction (internal): sessions own a transport instance
 * polymorphically. ops is a pointer to static, immutable vtable storage
 * owned by the transport implementation TU; impl is the implementation's
 * private state. Both start() and detach() are invoked by the session,
 * never by transports themselves — no re-entry into session lifecycle.
 *
 * Contract:
 * - start(): called from ecuio_session_start. Must be idempotent
 *   (second call on a running transport returns ECUIO_OK).
 * - detach(): called exactly once from ecuio_session_close, AFTER the
 *   closed flag is set. Must stop any worker threads and free impl +
 *   the ecuio_transport wrapper itself. May not touch session afterwards.
 */
typedef struct ecuio_transport_ops {
    ecuio_status_t (*start)(ecuio_session_t* session, void* impl);
    void (*detach)(ecuio_session_t* session, void* impl);
} ecuio_transport_ops_t;

typedef struct ecuio_transport {
    const ecuio_transport_ops_t* ops;
    void* impl;
} ecuio_transport_t;

/* A URI's query, split once by ecuio_session_create (core/src/uri_query.h,
 * where it's defined). Each transport's attach reads its values from it;
 * create has already refused any key not in the transport's list below,
 * so attach judges values only. */
struct ecuio_uri_query;

/* The keys each transport's query takes, NULL-terminated. profile and
 * did are core's, read by create (did under uds-generic, ADR-0020). */
extern const char* const ecuio_mock_query_keys[];
extern const char* const ecuio_j2534_query_keys[];

/* Mock transport factory — attaches a transport to a session
 * (idempotent). Its keys, profile and did, are core's, so it reads
 * nothing from the query; under uds-generic its worker answers the
 * session's DIDs with synthetic values. */
ecuio_status_t ecuio_mock_transport_attach(ecuio_session_t* session,
                                            const char* uri,
                                            const struct ecuio_uri_query* query);

/*
 * J2534 transport factory (Win32). URI grammar:
 *   j2534://[<selector>][?<key>=<value>[&<key>=<value>...]]
 *   keys: baud, flags, filter, txid, poll, probe, profile, did (any order)
 *
 * Selector:
 *   j2534://                        first discovered device
 *   j2534://<vendor-name>           discovered vendor, matched
 *                                   case-insensitively
 *   j2534://<path ending .dll or    direct DLL load (tests/dev)
 *            containing \ or />)
 *
 * Query parameters (strict — malformed URIs fail at session_create
 * with ECUIO_ERR_BAD_URI, before any DLL load):
 *   baud    decimal, nonzero; default resolved by connect_channel to
 *           the ISO 15765 500k default
 *   flags   0x-prefixed hex or decimal, passed to PassThruConnect
 *           verbatim — no bit is interpreted
 *   filter  CAN id 1..0x1FFFFFFF — receive ID; overrides the profile's
 *           declared channels with this single channel (registers a
 *           FLOW_CONTROL filter; frames matching it get their 4-byte
 *           address header stripped and arbitration_id stamped)
 *   txid    CAN id 1..0x1FFFFFFF — transmit ID; default is the
 *           profile's declared tx, else 0x7E0
 *   poll    PID 0x00..0xFF — one Service 01 request, repeated as the
 *           whole rotation (0x00 reads the supported-PIDs bitmap);
 *           replaces the profile's requests, sent as asked: no probe,
 *           nothing dropped. Needs a channel, from filter or the
 *           profile's declarations; rejected at attach (still before any
 *           DLL load) when neither supplies one
 *   probe   0 or 1 (default 1) — ADR-0015 §4: walk each ECU's
 *           supported-PID chain before the rotation and drop the
 *           requests it explicitly refuses; 0 sends the set as written
 *   profile name — validated by ecuio_session_create before attach
 *   did     the uds-generic profile's DIDs (core/src/uds_generic.h,
 *           ADR-0020), read by create: single DIDs join the rotation,
 *           ranges are scanned once; every did= key adds up
 *   lowercase keys only, unknown keys rejected (by create), duplicate
 *   keys last-wins (did excepted); the query grammar is the one every
 *   scheme shares (core/src/uri_query.h)
 *
 * Attaches a transport to the session (idempotent like mock).
 */
ecuio_status_t ecuio_j2534_transport_attach(ecuio_session_t* session,
                                            const char* uri,
                                            const struct ecuio_uri_query* query);

/* Vendor-call helpers implemented in j2534_loader.cpp —
 * j2534_transport.cpp policy layer goes through these so the vendor
 * ABI lives in exactly one TU. */
ecuio_status_t ecuio_j2534_connect_channel(ecuio_j2534_handle_t* handle,
                                           unsigned long baud,
                                           unsigned long flags,
                                           unsigned long* out_channel);
ecuio_status_t ecuio_j2534_disconnect_channel(ecuio_j2534_handle_t* handle,
                                              unsigned long channel);
/* One received message, as the vendor gave it (ecuio_pt_rx_t,
 * j2534_ptypes.h): RxStatus, the raw 32-bit stamp, and Data whole with
 * the 4-byte CAN ID in front. Indications are skipped on both backends;
 * the transport makes the message a frame (pump_one). */
ecuio_status_t ecuio_j2534_read_msg(ecuio_j2534_handle_t* handle,
                                    unsigned long channel,
                                    ecuio_pt_rx_t* out_msg,
                                    uint32_t timeout_ms,
                                    bool* got_msg);

/* ---- Write / ioctl / filter vendor-call helpers ----
 *
 * Same routing discipline as the helpers above: each dispatches
 * branch-on-backend inside j2534_loader.cpp and the policy layer stays
 * backend-blind. All SCONFIG traffic is FLATTENED (Parameter, Value)
 * u32 pairs, little-endian, 8 bytes per pair, max
 * ECUIO_PT_SCONFIG_MAX_PARAMS pairs — pointer-bearing structs
 * (SCONFIG_LIST, raw PASSTHRU_MSG*) never cross the internal seam so
 * the in-proc and surrogate signatures are identical by construction.
 *
 * Status codes, the same on both backends (ADR-0018, appendix of
 * 2026-10-23):
 *   ECUIO_ERR_NULL_ARGUMENT     a required pointer is NULL
 *   ECUIO_ERR_INVALID_ARGUMENT  a length or shape the call can't take,
 *                               refused before any vendor call
 *   ECUIO_ERR_NOT_IMPLEMENTED   an ioctl outside the whitelist, or a
 *                               DLL without an optional export
 *   ECUIO_ERR_VENDOR_REFUSED    the vendor's status wasn't
 *                               STATUS_NOERROR (through the surrogate,
 *                               also the child's own refusals, which
 *                               answer as a strict vendor would)
 *   ECUIO_ERR_SURROGATE_FAILED  the surrogate child didn't answer, or
 *                               answered past the wire's limits
 * Disconnect is best-effort and ignores the vendor's status, so it
 * returns only the first and the last. The read path keeps its own
 * mapping into link health (read_msg, above).
 */

/* Single-message PassThruWriteMsgs relay. data_len <= 64 (frame ABI
 * cap). tx_flags carries J2534 TxFlags verbatim (bit 8 =
 * CAN_29BIT_ID for 29-bit targets). timeout_ms passes through to the
 * vendor unchanged; 0 = non-blocking. out_num_written may be NULL;
 * *out_num_written < 1 after ECUIO_OK means the vendor reported
 * success-but-wrote-nothing — callers treat that as a failed send. */
ecuio_status_t ecuio_j2534_write_frame(ecuio_j2534_handle_t* handle,
                                       unsigned long channel,
                                       const uint8_t* data,
                                       uint32_t len,
                                       uint32_t tx_flags,
                                       uint32_t timeout_ms,
                                       uint32_t* out_num_written);

/* Generic PassThruIoctl relay. For GET_CONFIG/SET_CONFIG: input is a
 * flattened SCONFIG array as described above, input_len a nonzero
 * multiple of 8; for GET_CONFIG the vendor-updated pairs return in
 * output. For READ_VBATT: input_len 0, 4-byte result in output.
 * Pointer-bearing ioctls (FAST_INIT etc.) are out of contract — the
 * relay refuses them at this seam rather than guessing layouts.
 * output receives the first output_cap bytes of the result on either
 * backend, and *out_len its full length, so a caller spots a cut by
 * comparing. */
ecuio_status_t ecuio_j2534_ioctl(ecuio_j2534_handle_t* handle,
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
 * ECUIO_PT_FLOW_CONTROL_FILTER (fc_len must then equal mask_len;
 * for PASS/BLOCK filters pass fc_len 0 and flow_control NULL).
 * tx_flags stamps the filter messages' TxFlags (CAN_29BIT_ID for
 * 29-bit matching — mask and pattern are compared against the frame
 * ID in the filter messages' Data[], big-endian, per J2534-1 §5.7). */
ecuio_status_t ecuio_j2534_start_msg_filter(ecuio_j2534_handle_t* handle,
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
 * (a strict one answers a second stop with ERR_FAILED, so
 * ECUIO_ERR_VENDOR_REFUSED) — we pass the refusal on rather than
 * second-guessing it. */
ecuio_status_t ecuio_j2534_stop_msg_filter(ecuio_j2534_handle_t* handle,
                                           unsigned long channel,
                                           unsigned long filter_id);

/* PassThruReadVersion relay: the adapter's firmware, its DLL and the
 * J2534 API versions into three buffers of ECUIO_SESSION_INFO_TEXT_LEN
 * bytes, each NUL-terminated on one line (control characters become
 * spaces). The export is optional, as PassThruGetLastError's is:
 * ECUIO_ERR_NOT_IMPLEMENTED without it; on any failure the buffers
 * hold "". */
ecuio_status_t ecuio_j2534_read_version(ecuio_j2534_handle_t* handle,
                                        char* firmware,
                                        char* dll,
                                        char* api);

/* Backend introspection (routing diagnostics + tests): "inproc" or
 * "surrogate". NULL for a NULL handle. */
const char* ecuio_j2534_backend_name(const ecuio_j2534_handle_t* handle);

/* The same choice as ECUIO_BACKEND_J2534_INPROC or _SURROGATE, and the
 * DLL's PE machine as ECUIO_DLL_ARCH_* (core.h), for
 * ecuio_session_info_t. A forced surrogate (below) reports the x64 fake
 * as x64. 0 for a NULL handle. */
uint32_t ecuio_j2534_backend(const ecuio_j2534_handle_t* handle);
uint32_t ecuio_j2534_dll_arch(const ecuio_j2534_handle_t* handle);

/* TEST-ONLY: inject a frame into the loaded module's InjectFrame hook
 * (fake DLL), routed by backend. UNIT/HARNESS use — never in product
 * paths; real messages arrive via ecuio_j2534_read_msg. */
ecuio_status_t ecuio_j2534_inject_test_frame(ecuio_j2534_handle_t* handle,
                                             const uint8_t* data,
                                             uint32_t len,
                                             uint32_t timestamp,
                                             uint32_t rx_flags);

/* TEST-ONLY: while on, ecuio_j2534_open sends every DLL to the
 * surrogate, x64 ones included, so one HARNESS test can run a session
 * through both backends against the x64 fake. The surrogate exe must
 * match the DLL's architecture (ECUIO_SURROGATE_EXE names the x64
 * tree's). Process-wide and off by default; product code never calls
 * it. */
void ecuio_j2534_force_surrogate_for_testing(bool on);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_INTERNAL_H */
