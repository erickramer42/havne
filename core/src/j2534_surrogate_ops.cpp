// File/layer: core/src/j2534_surrogate_ops.cpp | C++17, Qt-free, Win32-guarded:
//             the calls after open: channels, frames, filters, close
// Testing: UNIT: CoreTest (test_core); HARNESS: every test_j2534_* binary;
//          Cross-arch

#include "ecuio/j2534_surrogate.h"
#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/sur_fingerprint.h"

#include <cstring>
#include <cstdint>
#include <new>
#include <cstdlib>

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "win_utf8.h"
#include "error_detail.h"
#include "j2534_sur_reply.h"
#include "j2534_surrogate_client.h"
#include "j2534_surrogate_log.h"

using ecuio_sur_client::fetch_vendor_text;
using ecuio_sur_client::sur_log;
using ecuio_sur_client::transact;

extern "C" {

ecuio_status_t ecuio_j2534_sur_connect_channel(ecuio_j2534_sur_handle_t* h,
                                               unsigned long baud,
                                               unsigned long flags,
                                               unsigned long* out_channel)
{
    if (h == nullptr || out_channel == nullptr) return ECUIO_ERR_NULL_ARGUMENT;
    if (baud == 0) baud = ECUIO_PT_BAUD_ISO15765_500K;

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_CONNECT;
    req.u.connect.protocol_id = ECUIO_PT_PROTOCOL_ISO15765;
    req.u.connect.flags = flags;
    req.u.connect.baud = baud;

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        ecuio_detail_set("the surrogate stopped answering during "
                         "PassThruConnect");
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        char text[ECUIO_PT_TEXT_LEN + 1];
        fetch_vendor_text(h, text);
        SUR_LOG(h, "connect: PassThruConnect refused (vendor status %d)%s%s\n",
                resp.status, text[0] != '\0' ? ": " : "", text);
        char call[64];
        snprintf(call, sizeof(call), "PassThruConnect (ISO 15765 at %lu baud)",
                 baud);
        ecuio_detail_set_vendor(call, resp.status, text);
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    *out_channel = resp.handle_or_channel;
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_disconnect_channel(ecuio_j2534_sur_handle_t* h,
                                                  unsigned long channel)
{
    if (h == nullptr) return ECUIO_ERR_NULL_ARGUMENT;
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_DISCONNECT;
    req.u.disconnect.channel_id = channel;
    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_write_frame(ecuio_j2534_sur_handle_t* h,
                                           unsigned long channel,
                                           const uint8_t* data,
                                           uint32_t len,
                                           uint32_t tx_flags,
                                           uint32_t timeout_ms,
                                           uint32_t* out_num_written)
{
    if (h == nullptr || data == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (len == 0 || len > ECUIO_SUR_LONG_MSG_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (out_num_written != nullptr) *out_num_written = 0;

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    const bool long_one = len > ECUIO_SUR_MSG_MAX;
    if (long_one) {
        req.opcode = ECUIO_SUR_OP_WRITE_LONG;
        req.u.write_long.channel_id = channel;
        req.u.write_long.data_len = len;
        req.u.write_long.tx_flags = tx_flags;
        req.u.write_long.timeout_ms = timeout_ms;
    } else {
        req.opcode = ECUIO_SUR_OP_WRITE;
        req.u.write.channel_id = channel;
        memcpy(req.u.write.data, data, len);
        req.u.write.data_len = len;
        req.u.write.tx_flags = tx_flags;
        req.u.write.timeout_ms = timeout_ms;
    }

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms, nullptr,
                  long_one ? data : nullptr, long_one ? len : 0)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    if (out_num_written != nullptr) *out_num_written = resp.aux;
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_ioctl(ecuio_j2534_sur_handle_t* h,
                                     unsigned long channel,
                                     uint32_t ioctl_id,
                                     const uint8_t* input,
                                     uint32_t input_len,
                                     uint8_t* output,
                                     uint32_t output_cap,
                                     uint32_t* out_len)
{
    if (h == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (input_len > ECUIO_SUR_IOCTL_DATA_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (input == nullptr && input_len != 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out_len != nullptr) *out_len = 0;

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_IOCTL;
    req.u.ioctl.channel_id = channel;
    req.u.ioctl.ioctl_id = ioctl_id;
    if (input_len > 0) memcpy(req.u.ioctl.input, input, input_len);
    req.u.ioctl.input_len = input_len;

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    /* Trust boundary, as for read's num_frames: a reply could claim more
     * than resp.data holds. */
    if (!sur_ioctl_payload(resp, output, output_cap, out_len)) {
        SUR_LOG(h, "ioctl: data_len=%u exceeds the wire limit (skew?)\n",
                resp.data_len);
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_read_version(ecuio_j2534_sur_handle_t* h,
                                            char* firmware,
                                            char* dll,
                                            char* api)
{
    if (h == nullptr || firmware == nullptr || dll == nullptr ||
        api == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    char* const out[ECUIO_SUR_TEXT_FIELDS] = {firmware, dll, api};
    for (char* o : out) o[0] = '\0';

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_READ_VERSION;
    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status == static_cast<int32_t>(ECUIO_PT_STATUS_NOT_SUPPORTED)) {
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    for (int i = 0; i < ECUIO_SUR_TEXT_FIELDS; ++i) {
        sur_reply_text(resp, i, out[i]);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_start_msg_filter(ecuio_j2534_sur_handle_t* h,
                                                unsigned long channel,
                                                uint32_t filter_type,
                                                const uint8_t* mask,
                                                uint32_t mask_len,
                                                const uint8_t* pattern,
                                                uint32_t pattern_len,
                                                const uint8_t* flow_control,
                                                uint32_t fc_len,
                                                uint32_t tx_flags,
                                                unsigned long* out_filter_id)
{
    if (h == nullptr || mask == nullptr || pattern == nullptr ||
        out_filter_id == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (filter_type == ECUIO_PT_FLOW_CONTROL_FILTER && flow_control == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (mask_len < 1 || mask_len > ECUIO_SUR_FILTER_PATTERN_MAX ||
        mask_len != pattern_len) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (fc_len > ECUIO_SUR_FILTER_PATTERN_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (filter_type == ECUIO_PT_FLOW_CONTROL_FILTER && fc_len != mask_len) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (filter_type != ECUIO_PT_FLOW_CONTROL_FILTER && fc_len != 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_filter_id = 0;

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_START_FILTER;
    req.u.start_filter.channel_id = channel;
    req.u.start_filter.filter_type = filter_type;
    req.u.start_filter.tx_flags = tx_flags;
    memcpy(req.u.start_filter.mask, mask, mask_len);
    memcpy(req.u.start_filter.pattern, pattern, pattern_len);
    if (fc_len > 0 && flow_control != nullptr) {
        memcpy(req.u.start_filter.flow_control, flow_control, fc_len);
    }
    req.u.start_filter.mask_len = mask_len;
    req.u.start_filter.pattern_len = pattern_len;
    req.u.start_filter.fc_len = fc_len;

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        ecuio_detail_set("the surrogate stopped answering during "
                         "PassThruStartMsgFilter");
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        char text[ECUIO_PT_TEXT_LEN + 1];
        fetch_vendor_text(h, text);
        SUR_LOG(h, "start_filter: PassThruStartMsgFilter refused (vendor "
                "status %d)%s%s\n", resp.status,
                text[0] != '\0' ? ": " : "", text);
        ecuio_detail_set_vendor("PassThruStartMsgFilter", resp.status, text);
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    *out_filter_id = resp.handle_or_channel;
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_stop_msg_filter(ecuio_j2534_sur_handle_t* h,
                                                  unsigned long channel,
                                                  unsigned long filter_id)
{
    if (h == nullptr) return ECUIO_ERR_NULL_ARGUMENT;
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_STOP_FILTER;
    req.u.stop_filter.channel_id = channel;
    req.u.stop_filter.filter_id = filter_id;

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_read_msg(ecuio_j2534_sur_handle_t* h,
                                        unsigned long channel,
                                        ecuio_pt_rx_t* out_msg,
                                        uint32_t timeout_ms,
                                        bool* got_msg)
{
    if (h == nullptr || out_msg == nullptr || got_msg == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    (void)channel; /* the child reads its one open channel */
    /* From the batch first: no pipe traffic mid-batch. Otherwise one READ,
     * with a margin over the vendor timeout: the child may spend all of
     * timeout_ms in ReadMsgs, so a transact timeout means hung, not busy. */
    const auto read = [h, timeout_ms](ecuio_sur_response_t* resp,
                                      ecuio_pt_rx_t* frames) {
        ecuio_sur_request_t req = {};
        req.request_id = h->next_request_id++;
        req.opcode = ECUIO_SUR_OP_READ;
        req.u.read.timeout_ms = timeout_ms;
        return transact(h, req, resp, timeout_ms + 500, frames);
    };
    return sur_read_msg(&h->batch, read, out_msg, got_msg);
}

ecuio_status_t ecuio_j2534_sur_inject_test_frame(ecuio_j2534_sur_handle_t* h,
                                                 const uint8_t* data,
                                                 uint32_t len,
                                                 uint32_t timestamp,
                                                 uint32_t rx_flags)
{
    if (h == nullptr || data == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (len == 0 || len > ECUIO_SUR_MSG_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_INJECT;
    memcpy(req.u.inject.data, data, len);
    req.u.inject.len = len;
    req.u.inject.timestamp = timestamp;
    req.u.inject.rx_flags = rx_flags;

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_sur_close(ecuio_j2534_sur_handle_t* h)
{
    if (h == nullptr) return ECUIO_OK;
    /* OK only for a child that lived to the end: running, answering
     * OP_SHUTDOWN, exiting 0. Anything else is SURROGATE_FAILED, so a test's
     * close asserts the child survived every refusal before it (ADR-0014). */
    bool clean = false;
    const bool alive = h->process != nullptr &&
                       WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0;
    if (h->pipe_connected && h->pipe != INVALID_HANDLE_VALUE && alive) {
        ecuio_sur_request_t req = {};
        req.request_id = h->next_request_id++;
        req.opcode = ECUIO_SUR_OP_SHUTDOWN;
        ecuio_sur_response_t resp = {};
        /* fails at once if unresponsive */
        clean = transact(h, req, &resp, 1500) &&
                resp.status == ECUIO_PT_STATUS_NOERROR;
    }
    /* An unresponsive child won't exit on its own: the job close kills it. */
    DWORD code = STILL_ACTIVE;
    if (h->process != nullptr && (!alive || !h->unresponsive) &&
        WaitForSingleObject(h->process, 1500) == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(h->process, &code);
    }
    clean = clean && code == 0;
    if (h->process != nullptr && !clean) {
        if (!alive) {
            sur_log(h, "close: the child had already exited (code %lu)",
                    static_cast<unsigned long>(code));
        } else if (h->unresponsive || code == STILL_ACTIVE) {
            sur_log(h, "close: the child didn't shut down cleanly (%s)",
                    h->unresponsive ? "unresponsive, killed"
                                    : "still running, killed");
        } else {
            sur_log(h,
                    "close: the child didn't shut down cleanly (exit code %lu)",
                    static_cast<unsigned long>(code));
        }
    }
    if (h->pipe != INVALID_HANDLE_VALUE) CloseHandle(h->pipe);
    if (h->io_event != nullptr) CloseHandle(h->io_event);
    if (h->process != nullptr) CloseHandle(h->process);
    if (h->job != nullptr) CloseHandle(h->job);
    if (h->log != INVALID_HANDLE_VALUE) CloseHandle(h->log);
    const bool had_child = h->process != nullptr;
    delete h;
    return clean || !had_child ? ECUIO_OK : ECUIO_ERR_SURROGATE_FAILED;
}

const char* ecuio_j2534_sur_log_path(const ecuio_j2534_sur_handle_t* h)
{
    return (h != nullptr) ? h->log_path : "";
}

void ecuio_j2534_sur_set_transact_ms(ecuio_j2534_sur_handle_t* h, uint32_t ms)
{
    h->transact_ms = (ms != 0) ? ms : kTransactTimeoutMs;
}

uint32_t ecuio_j2534_sur_get_transact_ms(const ecuio_j2534_sur_handle_t* h)
{
    return h->transact_ms;
}

} // extern "C"

#else

extern "C" {

ecuio_status_t ecuio_j2534_sur_connect_channel(ecuio_j2534_sur_handle_t*,
                                               unsigned long, unsigned long,
                                               unsigned long*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_disconnect_channel(ecuio_j2534_sur_handle_t*, unsigned long)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_write_frame(ecuio_j2534_sur_handle_t*, unsigned long,
                                           const uint8_t*, uint32_t, uint32_t,
                                           uint32_t, uint32_t*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_ioctl(ecuio_j2534_sur_handle_t*, unsigned long,
                                     uint32_t, const uint8_t*, uint32_t,
                                     uint8_t*, uint32_t, uint32_t*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_read_version(ecuio_j2534_sur_handle_t*,
                                            char*, char*, char*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_start_msg_filter(ecuio_j2534_sur_handle_t*, unsigned long,
                                                uint32_t, const uint8_t*, uint32_t,
                                                const uint8_t*, uint32_t,
                                                const uint8_t*, uint32_t, uint32_t,
                                                unsigned long*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_stop_msg_filter(ecuio_j2534_sur_handle_t*, unsigned long,
                                                  unsigned long)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_read_msg(ecuio_j2534_sur_handle_t*, unsigned long,
                                        ecuio_pt_rx_t*, uint32_t, bool*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_inject_test_frame(ecuio_j2534_sur_handle_t*, const uint8_t*,
                                                  uint32_t, uint32_t, uint32_t)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
ecuio_status_t ecuio_j2534_sur_close(ecuio_j2534_sur_handle_t*)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
const char* ecuio_j2534_sur_log_path(const ecuio_j2534_sur_handle_t*)
{ return ""; }
void ecuio_j2534_sur_set_transact_ms(ecuio_j2534_sur_handle_t*, uint32_t) {}
uint32_t ecuio_j2534_sur_get_transact_ms(const ecuio_j2534_sur_handle_t*)
{
    return 0;
}

} // extern "C"

#endif /* _WIN32 */
