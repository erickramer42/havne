// File/layer: surrogate/surrogate_main.cpp | Win32, x86, Qt-free; rebuild build-win32
// Testing: HARNESS — full suite

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "qttune/j2534.h"
#include "qttune/j2534_ptypes.h"
#include "qttune/surrogate_protocol.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

static HMODULE g_module = nullptr;
static unsigned long g_device_id = 0;
static unsigned long g_channel = 0;

typedef long (__stdcall *fn_pt_open)(void*, unsigned long*);
typedef long (__stdcall *fn_pt_close)(unsigned long);
typedef long (__stdcall *fn_pt_connect)(unsigned long, unsigned long, unsigned long,
                                        unsigned long, unsigned long*);
typedef long (__stdcall *fn_pt_disconnect)(unsigned long);
typedef long (__stdcall *fn_pt_read_msgs)(unsigned long, PASSTHRU_MSG*,
                                          unsigned long*, unsigned long);
typedef long (__stdcall *fn_pt_write_msgs)(unsigned long, PASSTHRU_MSG*,
                                           unsigned long*, unsigned long);
typedef long (__stdcall *fn_pt_ioctl)(unsigned long, long, void*, void*);
typedef long (__stdcall *fn_pt_start_filter)(unsigned long, unsigned long,
                                              PASSTHRU_MSG*, PASSTHRU_MSG*,
                                              PASSTHRU_MSG*, unsigned long*);
typedef long (__stdcall *fn_pt_stop_filter)(unsigned long, unsigned long);

static fn_pt_open         g_open;
static fn_pt_close        g_close;
static fn_pt_connect      g_connect;
static fn_pt_disconnect   g_disconnect;
static fn_pt_read_msgs    g_read_msgs;
static fn_pt_write_msgs   g_write_msgs;
static fn_pt_ioctl        g_ioctl;
static fn_pt_start_filter g_start_filter;
static fn_pt_stop_filter  g_stop_filter;

static HANDLE g_pipe = INVALID_HANDLE_VALUE;

/* BATCHED read: one vendor call returns up to MAX frames. The array
 * is STATIC, not stack: PASSTHRU_MSG is ~4.2KB each, so 16 of them
 * is ~67KB — legal on a 1MB default stack but needlessly close to
 * the edge. The child is single-threaded; no reentrancy exists. */
static PASSTHRU_MSG s_read_batch[QTTUNE_SUR_MAX_FRAMES_PER_READ];

static void stage(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[surrogate] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static FARPROC resolve_export(HMODULE mod, const char* primary,
                              const char* alternate)
{
    FARPROC p = GetProcAddress(mod, primary);
    if (p == nullptr && alternate != nullptr) {
        p = GetProcAddress(mod, alternate);
    }
    return p;
}

static int handle_op_open(void)
{
    stage("OP_OPEN: calling PassThruOpen");
    unsigned long device = 0;
    long rc = g_open(nullptr, &device);
    stage("OP_OPEN: vendor status %ld, device_id=%lu", rc, device);
    if (rc == QTTUNE_PT_STATUS_NOERROR) {
        g_device_id = device;
    }
    return static_cast<int>(rc);
}

static int handle_op_connect(uint32_t protocol_id, uint32_t flags, uint32_t baud,
                             uint32_t* out_ch)
{
    unsigned long ch = 0;
    long rc = g_connect(g_device_id, protocol_id, flags, baud, &ch);
    stage("OP_CONNECT: vendor status %ld, channel=%lu (device=%lu)",
          rc, ch, g_device_id);
    if (rc == QTTUNE_PT_STATUS_NOERROR) g_channel = ch;
    if (out_ch != nullptr) *out_ch = static_cast<uint32_t>(ch);
    return static_cast<int>(rc);
}

static int handle_op_disconnect(unsigned long channel)
{
    long rc = g_disconnect(channel);
    stage("OP_DISCONNECT: vendor status %ld, channel=%lu", rc, channel);
    if (rc == QTTUNE_PT_STATUS_NOERROR) g_channel = 0;
    return static_cast<int>(rc);
}

static int handle_op_read(unsigned long channel, uint32_t timeout_ms,
                          qttune_sur_response_t* resp)
{
    (void)channel;   /* child uses g_channel exclusively */
    resp->num_frames = 0;
    if (g_channel == 0 || g_read_msgs == nullptr) {
        return static_cast<int>(QTTUNE_PT_STATUS_NOERROR);
    }

    unsigned long num = QTTUNE_SUR_MAX_FRAMES_PER_READ;
    const long rc = g_read_msgs(g_channel, s_read_batch, &num, timeout_ms);

    /* Vendor status rides home VERBATIM in resp->status */
    if (rc != QTTUNE_PT_STATUS_NOERROR && num == 0) {
        return static_cast<int>(rc);
    }

    uint32_t n = 0;
    const unsigned long lim = (num < QTTUNE_SUR_MAX_FRAMES_PER_READ)
                              ? num : QTTUNE_SUR_MAX_FRAMES_PER_READ;
    for (unsigned long i = 0; i < lim; ++i) {
        const PASSTHRU_MSG& m = s_read_batch[i];
        /* Drop TX indications/echoes — they are transport bookkeeping */
        if (m.RxStatus & (QTTUNE_PT_TX_INDICATION |
                          QTTUNE_PT_TX_MSG_TYPE)) {
            continue;
        }
        uint32_t dsz = m.DataSize;
        if (dsz > QTTUNE_SUR_MSG_MAX) dsz = QTTUNE_SUR_MSG_MAX;
        resp->frames[n].arbitration_id = 0;
        resp->frames[n].timestamp = m.Timestamp;
        resp->frames[n].rx_flags = m.RxStatus;
        resp->frames[n].data_len = static_cast<uint8_t>(dsz);
        memcpy(resp->frames[n].data, m.Data, dsz);
        ++n;
    }
    resp->num_frames = n;
    return static_cast<int>(rc);
}

static int handle_op_write(unsigned long channel, const uint8_t* data,
                           uint32_t len, uint32_t tx_flags,
                           uint32_t timeout_ms, uint32_t* out_num)
{
    PASSTHRU_MSG msg = {};
    msg.ProtocolID = QTTUNE_PT_PROTOCOL_ISO15765;
    msg.TxFlags = tx_flags;
    msg.DataSize = len;
    if (len > 0) memcpy(msg.Data, data, len);
    msg.ExtraDataIndex = len;

    unsigned long num = 1;
    long rc = g_write_msgs(channel, &msg, &num, timeout_ms);
    if (out_num != nullptr) *out_num = static_cast<uint32_t>(num);
    stage("OP_WRITE: status %ld, written %lu", rc, num);
    return static_cast<int>(rc);
}

/* SCONFIG wire format (internal seam — identical to the in-proc
 * loader's unflatten_sconfig): a flat array of (Parameter, Value)
 * u32 LE pairs, 8 bytes each. The child reconstructs a real
 * SCONFIG_LIST because ConfigPtr must point into THIS process's
 * address space; the flattened form cannot be handed to a vendor
 * directly. On GET, the vendor-updated values are re-flattened into
 * the response payload. */
static int handle_op_ioctl(uint32_t channel_id, uint32_t ioctl_id,
                           const uint8_t* input, uint32_t input_len,
                           uint8_t* output, uint32_t* out_len)
{
    if (ioctl_id == QTTUNE_PT_IOCTL_READ_VBATT) {
        unsigned long vbatt = 0;
        /* v04.04: READ_VBATT is device-scoped, not channel-scoped.
         * The fake pins this (device 1, channel 0x55) so a caller
         * passing the channel fails instead of colliding at 1 == 1. */
        long rc = g_ioctl(g_device_id, static_cast<long>(ioctl_id),
                          nullptr, &vbatt);
        if (rc == QTTUNE_PT_STATUS_NOERROR && output != nullptr) {
            memcpy(output, &vbatt, 4);
            if (out_len != nullptr) *out_len = 4;
        }
        stage("OP_IOCTL: id 0x%X (VBATT), status %ld", ioctl_id, rc);
        return static_cast<int>(rc);
    }
    if (ioctl_id == QTTUNE_PT_IOCTL_CLEAR_TX_BUFFER ||
        ioctl_id == QTTUNE_PT_IOCTL_CLEAR_RX_BUFFER ||
        ioctl_id == QTTUNE_PT_IOCTL_CLEAR_MSG_FILTERS) {
        long rc = g_ioctl(channel_id, static_cast<long>(ioctl_id),
                          nullptr, nullptr);
        stage("OP_IOCTL: id 0x%X (CLEAR), status %ld", ioctl_id, rc);
        return static_cast<int>(rc);
    }
    if (ioctl_id == QTTUNE_PT_IOCTL_SET_CONFIG ||
        ioctl_id == QTTUNE_PT_IOCTL_GET_CONFIG) {
        /* Trust boundary: the pipe is untrusted. Shape-check before
         * touching anything — a malformed length must never reach the
         * vendor, and the pair count must fit both the SCONFIG array
         * and the response payload the client sized for this request. */
        if (input == nullptr || input_len == 0 || (input_len % 8) != 0) {
            stage("OP_IOCTL: SCONFIG malformed input_len=%u", input_len);
            return static_cast<int>(QTTUNE_PT_STATUS_FAILED);
        }
        const uint32_t num_params = input_len / 8;
        if (num_params > QTTUNE_PT_SCONFIG_MAX_PARAMS) {
            stage("OP_IOCTL: SCONFIG too many params=%u (max %u)",
                  num_params, static_cast<unsigned>(QTTUNE_PT_SCONFIG_MAX_PARAMS));
            return static_cast<int>(QTTUNE_PT_STATUS_FAILED);
        }

        /* Unflatten the wire pairs into a vendor-shaped array. */
        SCONFIG cfg[QTTUNE_PT_SCONFIG_MAX_PARAMS] = {};
        for (uint32_t i = 0; i < num_params; ++i) {
            uint32_t param = 0, value = 0;
            memcpy(&param, input + i * 8, 4);
            memcpy(&value, input + i * 8 + 4, 4);
            cfg[i].Parameter = param;
            cfg[i].Value = value;
        }

        /* Reconstruct the descriptor the vendor expects. */
        SCONFIG_LIST list;
        list.NumOfParams = num_params;
        list.ConfigPtr = cfg;

        long rc = g_ioctl(channel_id, static_cast<long>(ioctl_id),
                          &list, nullptr);

        if (rc == QTTUNE_PT_STATUS_NOERROR &&
            ioctl_id == QTTUNE_PT_IOCTL_GET_CONFIG &&
            output != nullptr) {
            /* Re-flatten the vendor-updated values. Length is exactly
             * input_len (same pair count), so it always fits the
             * payload the client allocated for this request. */
            for (uint32_t i = 0; i < num_params; ++i) {
                const uint32_t param = static_cast<uint32_t>(cfg[i].Parameter);
                const uint32_t value = static_cast<uint32_t>(cfg[i].Value);
                memcpy(output + i * 8, &param, 4);
                memcpy(output + i * 8 + 4, &value, 4);
            }
            if (out_len != nullptr) *out_len = num_params * 8;
        }
        stage("OP_IOCTL: id 0x%X (SCONFIG %u params), status %ld",
              ioctl_id, num_params, rc);
        return static_cast<int>(rc);
    }
    /* Out-of-contract ioctl: refuse like a strict vendor. Pointer-
     * bearing ids (FAST_INIT, FIVE_BAUD_INIT...) cannot be shaped
     * across this seam — refusing beats guessing. */
    stage("OP_IOCTL: id 0x%X refused (whitelist)", ioctl_id);
    return static_cast<int>(QTTUNE_PT_STATUS_FAILED);
}

static int handle_op_start_filter(unsigned long channel_id, uint32_t filter_type,
                                  const uint8_t* mask, uint32_t mask_len,
                                  const uint8_t* pattern, uint32_t pattern_len,
                                  const uint8_t* fc, uint32_t fc_len,
                                  uint32_t tx_flags, uint32_t* out_fid)
{
    PASSTHRU_MSG mask_msg = {}, pattern_msg = {}, fc_msg = {};
    mask_msg.ProtocolID = QTTUNE_PT_PROTOCOL_ISO15765;
    mask_msg.TxFlags = tx_flags;
    mask_msg.DataSize = mask_len;
    if (mask_len > 0) memcpy(mask_msg.Data, mask, mask_len);
    pattern_msg.ProtocolID = QTTUNE_PT_PROTOCOL_ISO15765;
    pattern_msg.TxFlags = tx_flags;
    pattern_msg.DataSize = pattern_len;
    if (pattern_len > 0) memcpy(pattern_msg.Data, pattern, pattern_len);
    PASSTHRU_MSG* fc_ptr = nullptr;
    if (fc_len > 0) {
        fc_msg.ProtocolID = QTTUNE_PT_PROTOCOL_ISO15765;
        fc_msg.TxFlags = tx_flags;
        fc_msg.DataSize = fc_len;
        memcpy(fc_msg.Data, fc, fc_len);
        fc_ptr = &fc_msg;
    }
    unsigned long filter_id = 0;
    long rc = g_start_filter(channel_id, filter_type, &mask_msg, &pattern_msg,
                             fc_ptr, &filter_id);
    if (out_fid != nullptr) *out_fid = static_cast<uint32_t>(filter_id);
    stage("OP_START_FILTER: type %u, status %ld, filter_id %lu", filter_type, rc, filter_id);
    return static_cast<int>(rc);
}

static int handle_op_stop_filter(unsigned long channel_id, unsigned long filter_id)
{
    long rc = g_stop_filter(channel_id, filter_id);
    stage("OP_STOP_FILTER: fid %lu, status %ld", filter_id, rc);
    return static_cast<int>(rc);
}

int main(int argc, char* argv[])
{
    /* CLI: surrogate.exe <dll_path> <pipe_name> — two args, argc==3.
     * The map/event names of the ring era are gone (D1); 1 = bad args. */
    if (argc < 3) return 1;
    const char* dll_path = argv[1];
    const char* pipe_name = argv[2];

    for (int i = 0; i < 200; ++i) {
        if (WaitNamedPipeA(pipe_name, 50)) break;
    }
    g_pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) return 7;
    stage("pipe connected");

    g_module = LoadLibraryExA(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_module == nullptr) {
        stage("LoadLibraryExA failed for '%s' (err=%lu)", dll_path,
              GetLastError());
        return 4;
    }
    stage("dll loaded");

    g_open          = reinterpret_cast<fn_pt_open>(GetProcAddress(g_module, "PassThruOpen"));
    g_close         = reinterpret_cast<fn_pt_close>(GetProcAddress(g_module, "PassThruClose"));
    g_connect       = reinterpret_cast<fn_pt_connect>(GetProcAddress(g_module, "PassThruConnect"));
    g_disconnect    = reinterpret_cast<fn_pt_disconnect>(GetProcAddress(g_module, "PassThruDisconnect"));
    g_read_msgs     = reinterpret_cast<fn_pt_read_msgs>(GetProcAddress(g_module, "PassThruReadMsgs"));
    g_write_msgs    = reinterpret_cast<fn_pt_write_msgs>(GetProcAddress(g_module, "PassThruWriteMsgs"));
    g_ioctl         = reinterpret_cast<fn_pt_ioctl>(
                          resolve_export(g_module, "PassThruIoctl", "PassThruIoCtl"));
    g_start_filter  = reinterpret_cast<fn_pt_start_filter>(GetProcAddress(g_module, "PassThruStartMsgFilter"));
    g_stop_filter   = reinterpret_cast<fn_pt_stop_filter>(GetProcAddress(g_module, "PassThruStopMsgFilter"));

    if (!g_open || !g_close || !g_connect || !g_disconnect || !g_read_msgs ||
        !g_write_msgs || !g_ioctl || !g_start_filter || !g_stop_filter) {
        stage("mandatory export missing");
        return 5;
    }
    stage("exports resolved");

    while (true) {
        qttune_sur_request_t req = {};
        DWORD rd = 0;
        if (!ReadFile(g_pipe, &req, sizeof(req), &rd, nullptr) ||
            rd != sizeof(req)) break;

        qttune_sur_response_t resp = {};
        resp.request_id = req.request_id;
        resp.status = QTTUNE_PT_STATUS_FAILED;

        /* Trust-boundary checks BEFORE calling handlers —
         * the pipe is an untrusted boundary */
        switch (req.opcode) {
        case QTTUNE_SUR_OP_OPEN:
            resp.status = handle_op_open();
            break;
        case QTTUNE_SUR_OP_CONNECT:
            resp.status = handle_op_connect(req.u.connect.protocol_id,
                                            req.u.connect.flags,
                                            req.u.connect.baud,
                                            &resp.handle_or_channel);
            break;
        case QTTUNE_SUR_OP_DISCONNECT:
            resp.status = handle_op_disconnect(req.u.disconnect.channel_id);
            break;
        case QTTUNE_SUR_OP_READ:
            resp.status = handle_op_read(g_channel,
                                         req.u.read.timeout_ms,
                                         &resp);
            break;
        case QTTUNE_SUR_OP_WRITE:
            /* Trust boundary: validate length before memcpy */
            if (req.u.write.data_len > QTTUNE_SUR_MSG_MAX) {
                resp.status = QTTUNE_PT_STATUS_FAILED;
                break;
            }
            resp.status = handle_op_write(req.u.write.channel_id,
                                          req.u.write.data,
                                          req.u.write.data_len,
                                          req.u.write.tx_flags,
                                          req.u.write.timeout_ms,
                                          &resp.aux);
            break;
        case QTTUNE_SUR_OP_IOCTL:
            /* Trust boundary: validate input length */
            if (req.u.ioctl.input_len > QTTUNE_SUR_IOCTL_DATA_MAX) {
                resp.status = QTTUNE_PT_STATUS_FAILED;
                break;
            }
            resp.status = handle_op_ioctl(req.u.ioctl.channel_id,
                                          req.u.ioctl.ioctl_id,
                                          req.u.ioctl.input,
                                          req.u.ioctl.input_len,
                                          resp.data, &resp.data_len);
            break;
        case QTTUNE_SUR_OP_START_FILTER:
            /* Trust boundary: validate filter pattern lengths */
            if (req.u.start_filter.mask_len > QTTUNE_SUR_FILTER_PATTERN_MAX ||
                req.u.start_filter.pattern_len > QTTUNE_SUR_FILTER_PATTERN_MAX ||
                req.u.start_filter.fc_len > QTTUNE_SUR_FILTER_PATTERN_MAX) {
                resp.status = QTTUNE_PT_STATUS_FAILED;
                break;
            }
            resp.status = handle_op_start_filter(
                req.u.start_filter.channel_id,
                req.u.start_filter.filter_type,
                req.u.start_filter.mask,
                req.u.start_filter.mask_len,
                req.u.start_filter.pattern,
                req.u.start_filter.pattern_len,
                req.u.start_filter.flow_control,
                req.u.start_filter.fc_len,
                req.u.start_filter.tx_flags,
                &resp.handle_or_channel);
            break;
        case QTTUNE_SUR_OP_STOP_FILTER:
            resp.status = handle_op_stop_filter(
                req.u.stop_filter.channel_id,
                req.u.stop_filter.filter_id);
            break;
        case QTTUNE_SUR_OP_INJECT: {
            typedef void (*fn_inject)(const unsigned char*, unsigned long,
                                      unsigned long, unsigned long, unsigned long);
            fn_inject inject = reinterpret_cast<fn_inject>(
                GetProcAddress(g_module, "InjectFrame"));
            if (inject != nullptr) {
                inject(req.u.inject.data, req.u.inject.len,
                       req.u.inject.timestamp, req.u.inject.rx_flags, 0);
                resp.status = QTTUNE_PT_STATUS_NOERROR;
            }
            break;
        }
        case QTTUNE_SUR_OP_SHUTDOWN:
            resp.status = QTTUNE_PT_STATUS_NOERROR;
            goto shutdown;
        default:
            break;
        }

        /* Per-op status is DATA, not a control signal (D1): the child
         * stays alive across ERR_TIMEOUT, ERR_BUFFER_EMPTY, refused
         * ioctls, failed writes and unknown opcodes — the client maps
         * each to a transport status. Exiting on non-zero killed the
         * session on the first quiet-bus read against a real vendor
         * and made the client's tri-state arms unreachable. Only a
         * broken pipe or OP_SHUTDOWN ends the loop. */
        DWORD wr = 0;
        if (!WriteFile(g_pipe, &resp, sizeof(resp), &wr, nullptr) ||
            wr != sizeof(resp)) {
            break;
        }
    }

shutdown:
    if (g_channel != 0) g_disconnect(g_channel);
    if (g_device_id != 0) g_close(g_device_id);
    FreeLibrary(g_module);
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    return 0;
}
