// File/layer: surrogate/surrogate_main.cpp | Win32, x86, Qt-free; rebuild build-win32
// Testing: HARNESS: test_j2534_surrogate, plus the cross-arch routing
//          case against the Win32-tree build (ADR-0014)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ecuio/j2534.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/sur_fingerprint.h"

#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

static HMODULE g_module = nullptr;
static unsigned long g_device_id = 0;
static unsigned long g_channel = 0;
/* Presence flags, not 0-sentinels: J2534 does not reserve 0 as an
 * invalid DeviceID or ChannelID, so a vendor returning 0 would be
 * treated as "not open" and never read or closed. */
static bool g_have_device = false;
static bool g_have_channel = false;

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
typedef long (__stdcall *fn_pt_get_last_error)(char*);
typedef long (__stdcall *fn_pt_read_version)(unsigned long, char*, char*, char*);

static fn_pt_open         g_open;
static fn_pt_close        g_close;
static fn_pt_connect      g_connect;
static fn_pt_disconnect   g_disconnect;
static fn_pt_read_msgs    g_read_msgs;
static fn_pt_write_msgs   g_write_msgs;
static fn_pt_ioctl        g_ioctl;
static fn_pt_start_filter g_start_filter;
static fn_pt_stop_filter  g_stop_filter;
static fn_pt_get_last_error g_get_last_error;   /* optional: may be null */
static fn_pt_read_version g_read_version;       /* optional: may be null */

static HANDLE g_pipe = INVALID_HANDLE_VALUE;

/* Batched read buffer: one ReadMsgs call returns up to 16 messages.
 * Static rather than on the stack, because 16 PASSTHRU_MSGs are about
 * 67 KB. Safe because the child is single-threaded. */
static PASSTHRU_MSG s_read_batch[ECUIO_SUR_MAX_FRAMES_PER_READ];

/* The messages a READ's reply carries after the response, as the wire
 * sends them (surrogate_protocol.h). Static for the same reason. */
static ecuio_pt_rx_t s_read_frames[ECUIO_SUR_MAX_FRAMES_PER_READ];

/* ECUIO_SUR_LOG_FRAMES=1 logs every message the vendor returns and
 * every one written, bytes verbatim, with RxStatus or TxFlags and the
 * vendor timestamp: the payload evidence a BENCH or vehicle session
 * records. Off by default, because a polling session then adds a line
 * per request and per reply. Read once at startup; the child's
 * environment is fixed at spawn. */
static bool g_log_frames = false;

/* This build's fingerprint of the shared headers, which OP_OPEN must
 * carry. ECUIO_SUR_TEST_FINGERPRINT replaces it, so a test can stand in
 * for a child built from other headers; it can only make opens fail. */
static uint32_t g_fingerprint = ECUIO_SUR_FINGERPRINT;

/* ECUIO_SUR_TEST_REPLY_TWICE=<opcode> sends the first reply to that
 * opcode twice, standing in for a request the client thinks it never
 * sent, so a test can put the pipe out of step. Never OP_READ, whose
 * reply carries frames. 0 (the default) is off. */
static uint32_t g_reply_twice = 0;

static void stage(const char* fmt, ...)
{
    /* Local wall-clock time, the same stamp the client puts on its
     * "[sur ...]" lines in this file, so the session log reads as one
     * timeline. The line is formatted whole
     * and written with one WriteFile, so a client line can't land in
     * the middle of it. Lines still start with "[surrogate", so a grep
     * for that prefix matches. */
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[512];
    const int n = snprintf(line, sizeof(line) - 2,
                           "[surrogate %02u:%02u:%02u.%03u] ",
                           static_cast<unsigned>(t.wHour),
                           static_cast<unsigned>(t.wMinute),
                           static_cast<unsigned>(t.wSecond),
                           static_cast<unsigned>(t.wMilliseconds));
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line + n, sizeof(line) - 2 - static_cast<size_t>(n),
                    fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    line[len++] = '\r';
    line[len++] = '\n';
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), line,
                    static_cast<DWORD>(len), &written, nullptr);
}

/* "41 0C 1F 40" for the first ECUIO_SUR_MSG_MAX bytes, then " +N" for
 * the rest, so a long message can't overrun stage()'s line. */
static void format_hex(char* out, size_t cap, const unsigned char* data,
                       unsigned long size)
{
    const unsigned long shown = (size < ECUIO_SUR_MSG_MAX)
                                ? size : ECUIO_SUR_MSG_MAX;
    size_t pos = 0;
    out[0] = '\0';
    for (unsigned long i = 0; i < shown && pos + 4 < cap; ++i) {
        pos += static_cast<size_t>(snprintf(out + pos, cap - pos,
                                            i ? " %02X" : "%02X", data[i]));
    }
    if (size > shown && pos < cap) {
        (void)snprintf(out + pos, cap - pos, " +%lu", size - shown);
    }
}

/* A vendor's text into one reply field: at most its first
 * ECUIO_SUR_TEXT_MAX - 1 bytes, terminated. src may lack a terminator
 * within cap. */
static void copy_text(char* dst, const char* src, size_t cap)
{
    size_t n = 0;
    while (n < cap && n < ECUIO_SUR_TEXT_MAX - 1 && src[n] != '\0') ++n;
    memcpy(dst, src, n);
    dst[n] = '\0';
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

/* Order check at the trust boundary: an op that passes the device ID
 * needs a successful OP_OPEN, and one that passes a channel ID a
 * successful OP_CONNECT, because until then the ID means nothing to the
 * vendor. The client always opens and connects first, so a refusal here
 * is a broken or hostile client; it's logged and answered STATUS_FAILED,
 * like the length checks. */
static bool refuse_before_open(const char* op, bool needs_channel)
{
    if (!g_have_device) {
        stage("%s: refused, no device open", op);
        return true;
    }
    if (needs_channel && !g_have_channel) {
        stage("%s: refused, no channel connected", op);
        return true;
    }
    return false;
}

static int handle_op_open(void)
{
    stage("OP_OPEN: calling PassThruOpen");
    unsigned long device = 0;
    long rc = g_open(nullptr, &device);
    stage("OP_OPEN: vendor status %ld, device_id=%lu", rc, device);
    if (rc == ECUIO_PT_STATUS_NOERROR) {
        g_device_id = device;
        g_have_device = true;
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
    if (rc == ECUIO_PT_STATUS_NOERROR) {
        g_channel = ch;
        g_have_channel = true;
    }
    if (out_ch != nullptr) *out_ch = static_cast<uint32_t>(ch);
    return static_cast<int>(rc);
}

static int handle_op_disconnect(unsigned long channel)
{
    long rc = g_disconnect(channel);
    stage("OP_DISCONNECT: vendor status %ld, channel=%lu", rc, channel);
    if (rc == ECUIO_PT_STATUS_NOERROR) {
        g_channel = 0;
        g_have_channel = false;
    }
    return static_cast<int>(rc);
}

static int handle_op_read(unsigned long channel, uint32_t timeout_ms,
                          ecuio_sur_response_t* resp)
{
    (void)channel;   /* child uses g_channel exclusively */
    resp->num_frames = 0;
    if (!g_have_channel || g_read_msgs == nullptr) {
        return static_cast<int>(ECUIO_PT_STATUS_NOERROR);
    }

    /* A batch that held only indications (ecuio_pt_is_indication) is
     * read again with what's left of the timeout, and at least once
     * more, so an indication never ends a read: a reply queued behind
     * it comes back in this response.
     *
     * Frames were lost (ERR_BUFFER_OVERFLOW, or a message with
     * BUFFER_OVERFLOW set, indications too): the first frame delivered
     * from there on carries the bit, the v05.00 form, whatever the
     * vendor's version. With none delivered after it, the response's
     * status says ERR_BUFFER_OVERFLOW, and the client marks the next. */
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    uint32_t wait_ms = timeout_ms;
    uint32_t skipped = 0;
    uint32_t n = 0;
    bool lost = false;
    long rc = ECUIO_PT_STATUS_NOERROR;
    for (;;) {
        unsigned long num = ECUIO_SUR_MAX_FRAMES_PER_READ;
        rc = g_read_msgs(g_channel, s_read_batch, &num, wait_ms);

        /* Trust the count only under the status allowlist
           (ecuio_trusted_count). A vendor may leave pNumMsgs untouched
           on error; the VX reports "status 7, written 1" on a failed
           write (docs/bench-sessions/2026-10-20.md). A dirty count is
           doubly dangerous here: s_read_batch is static and never
           cleared, so it would replay the PREVIOUS read's frames as
           phantoms. */
        num = ecuio_trusted_count(rc, num);
        lost = lost || rc == ECUIO_PT_STATUS_BUFFER_OVERFLOW;

        /* Frames are collected under either trusted status,
         * ERR_TIMEOUT included: per J2534-1 a read that asks for N
         * messages with a nonzero timeout returns ERR_TIMEOUT together
         * with however many arrived, so on a conforming vendor the
         * ECU's answer usually arrives with a nonzero status. The
         * status still rides home verbatim in resp->status. */
        const unsigned long lim = (num < ECUIO_SUR_MAX_FRAMES_PER_READ)
                                  ? num : ECUIO_SUR_MAX_FRAMES_PER_READ;
        for (unsigned long i = 0; i < lim; ++i) {
            const PASSTHRU_MSG& m = s_read_batch[i];
            const bool indication = ecuio_pt_is_indication(m.RxStatus) != 0;
            if (g_log_frames) {
                char hex[3 * ECUIO_SUR_MSG_MAX + 16];
                format_hex(hex, sizeof(hex), m.Data, m.DataSize);
                stage("OP_READ frame: status %ld, rx 0x%08lX, ts %lu, len %lu: %s%s",
                      rc, m.RxStatus, m.Timestamp, m.DataSize, hex,
                      indication ? " (indication, not delivered)" : "");
            }
            lost = lost || (m.RxStatus & ECUIO_PT_BUFFER_OVERFLOW) != 0;
            if (indication) {
                ++skipped;
                continue;
            }
            ecuio_pt_rx_from_msg(&m, &s_read_frames[n]);
            if (lost) {
                s_read_frames[n].rx_status |= ECUIO_PT_BUFFER_OVERFLOW;
                lost = false;
            }
            ++n;
        }
        if (n > 0 || num == 0 || skipped >= ECUIO_PT_MAX_SKIPPED_PER_READ) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        wait_ms = (now < deadline)
            ? static_cast<uint32_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - now).count())
            : 0;
    }
    resp->num_frames = n;
    if (lost) {
        rc = ECUIO_PT_STATUS_BUFFER_OVERFLOW; /* the client marks the next */
    }

    /* Trace frames and status changes only: between requests the
     * worker reads up to 100 times a second, and every empty read
     * logged would bury everything else. */
    static long last_read_rc = -1;
    if (n > 0 || rc != last_read_rc) {
        stage("OP_READ: status %ld, frames %u", rc, n);
        last_read_rc = rc;
    }
    return static_cast<int>(rc);
}

static int handle_op_write(unsigned long channel, const uint8_t* data,
                           uint32_t len, uint32_t tx_flags,
                           uint32_t timeout_ms, uint32_t* out_num)
{
    PASSTHRU_MSG msg;
    ecuio_pt_fill_msg(&msg, data, len, tx_flags);

    unsigned long num = 1;
    long rc = g_write_msgs(channel, &msg, &num, timeout_ms);
    if (out_num != nullptr) *out_num = static_cast<uint32_t>(num);

    if (g_log_frames) {
        char hex[3 * ECUIO_SUR_MSG_MAX + 16];
        format_hex(hex, sizeof(hex), msg.Data, msg.DataSize);
        stage("OP_WRITE frame: status %ld, tx 0x%08lX, len %lu: %s",
              rc, msg.TxFlags, msg.DataSize, hex);
    }

    /* Status-change logging: the request engine writes up to 50/s per
     * channel, and a per-call line would bury the interesting ones. The
     * first write and every change of (status, count) still log. */
    static long last_write_rc = -1;
    static unsigned long last_write_num = 0;
    if (rc != last_write_rc || num != last_write_num) {
        stage("OP_WRITE: status %ld, written %lu", rc, num);
        last_write_rc = rc;
        last_write_num = num;
    }

    return static_cast<int>(rc);
}

/* SCONFIG wire format (the loader seam's, ecuio_pt_sconfig_unflatten
 * in j2534_ptypes.h): a flat array of (Parameter, Value) u32 pairs,
 * 8 bytes each. The child reconstructs a real
 * SCONFIG_LIST because ConfigPtr must point into THIS process's
 * address space; the flattened form cannot be handed to a vendor
 * directly. On GET, the vendor-updated values are re-flattened into
 * the response payload. */
static int handle_op_ioctl(uint32_t channel_id, uint32_t ioctl_id,
                           const uint8_t* input, uint32_t input_len,
                           uint8_t* output, uint32_t* out_len)
{
    if (ioctl_id == ECUIO_PT_IOCTL_READ_VBATT) {
        unsigned long vbatt = 0;
        /* v04.04: READ_VBATT is device-scoped, not channel-scoped.
         * The fake pins this (device 1, channel 0x55) so a caller
         * passing the channel fails instead of colliding at 1 == 1. */
        long rc = g_ioctl(g_device_id, static_cast<long>(ioctl_id),
                          nullptr, &vbatt);
        if (rc == ECUIO_PT_STATUS_NOERROR && output != nullptr) {
            memcpy(output, &vbatt, 4);
            if (out_len != nullptr) *out_len = 4;
        }
        stage("OP_IOCTL: id 0x%X (VBATT), status %ld", ioctl_id, rc);
        return static_cast<int>(rc);
    }
    if (ioctl_id == ECUIO_PT_IOCTL_CLEAR_TX_BUFFER ||
        ioctl_id == ECUIO_PT_IOCTL_CLEAR_RX_BUFFER ||
        ioctl_id == ECUIO_PT_IOCTL_CLEAR_MSG_FILTERS) {
        long rc = g_ioctl(channel_id, static_cast<long>(ioctl_id),
                          nullptr, nullptr);
        stage("OP_IOCTL: id 0x%X (CLEAR), status %ld", ioctl_id, rc);
        return static_cast<int>(rc);
    }
    if (ioctl_id == ECUIO_PT_IOCTL_SET_CONFIG ||
        ioctl_id == ECUIO_PT_IOCTL_GET_CONFIG) {
        /* Trust boundary: the pipe is untrusted. Shape-check before
         * touching anything: a malformed length must never reach the
         * vendor, and the pair count must fit both the SCONFIG array
         * and the response payload the client sized for this request. */
        if (input == nullptr || input_len == 0 || (input_len % 8) != 0) {
            stage("OP_IOCTL: SCONFIG malformed input_len=%u", input_len);
            return static_cast<int>(ECUIO_PT_STATUS_FAILED);
        }
        const uint32_t num_params = input_len / 8;
        if (num_params > ECUIO_PT_SCONFIG_MAX_PARAMS) {
            stage("OP_IOCTL: SCONFIG too many params=%u (max %u)",
                  num_params, static_cast<unsigned>(ECUIO_PT_SCONFIG_MAX_PARAMS));
            return static_cast<int>(ECUIO_PT_STATUS_FAILED);
        }

        /* Unflatten the wire pairs into a vendor-shaped array. */
        SCONFIG cfg[ECUIO_PT_SCONFIG_MAX_PARAMS] = {};
        ecuio_pt_sconfig_unflatten(input, num_params, cfg);

        /* Reconstruct the descriptor the vendor expects. */
        SCONFIG_LIST list;
        list.NumOfParams = num_params;
        list.ConfigPtr = cfg;

        long rc = g_ioctl(channel_id, static_cast<long>(ioctl_id),
                          &list, nullptr);

        if (rc == ECUIO_PT_STATUS_NOERROR &&
            ioctl_id == ECUIO_PT_IOCTL_GET_CONFIG &&
            output != nullptr) {
            /* Re-flatten the vendor-updated values. Length is exactly
             * input_len (same pair count), so it always fits the
             * payload the client allocated for this request. */
            ecuio_pt_sconfig_flatten(cfg, num_params, output);
            if (out_len != nullptr) *out_len = num_params * 8;
        }
        stage("OP_IOCTL: id 0x%X (SCONFIG %u params), status %ld",
              ioctl_id, num_params, rc);
        return static_cast<int>(rc);
    }
    /* Out-of-contract ioctl: refuse like a strict vendor. Pointer-
     * bearing ids (FAST_INIT, FIVE_BAUD_INIT...) cannot be shaped
     * across this seam: refusing beats guessing. */
    stage("OP_IOCTL: id 0x%X refused (whitelist)", ioctl_id);
    return static_cast<int>(ECUIO_PT_STATUS_FAILED);
}

static int handle_op_start_filter(unsigned long channel_id, uint32_t filter_type,
                                  const uint8_t* mask, uint32_t mask_len,
                                  const uint8_t* pattern, uint32_t pattern_len,
                                  const uint8_t* fc, uint32_t fc_len,
                                  uint32_t tx_flags, uint32_t* out_fid)
{
    /* Built as writes are (ecuio_pt_fill_msg): zeroed, so no stale
     * byte reaches a vendor comparing struct contents. */
    PASSTHRU_MSG mask_msg, pattern_msg, fc_msg;
    ecuio_pt_fill_msg(&mask_msg, mask, mask_len, tx_flags);
    ecuio_pt_fill_msg(&pattern_msg, pattern, pattern_len, tx_flags);
    PASSTHRU_MSG* fc_ptr = nullptr;
    if (fc_len > 0) {
        ecuio_pt_fill_msg(&fc_msg, fc, fc_len, tx_flags);
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

/* Wide entry point: the client passes the DLL path as UTF-16, so a
 * non-ASCII path survives (an ANSI argv would mangle it). */
int wmain(int argc, wchar_t* argv[])
{
    /* CLI: surrogate.exe <dll_path> <pipe_name>. Exit 1 = bad args. */
    if (argc < 3) return 1;
    const wchar_t* dll_path = argv[1];
    const wchar_t* pipe_name = argv[2];
    /* UTF-8, for the log; "?" if it won't convert. */
    char dll_path8[3 * 260] = "?";
    if (WideCharToMultiByte(CP_UTF8, 0, dll_path, -1, dll_path8,
                            sizeof(dll_path8), nullptr, nullptr) <= 0) {
        strcpy_s(dll_path8, sizeof(dll_path8), "?");
    }

    for (int i = 0; i < 200; ++i) {
        if (WaitNamedPipeW(pipe_name, 50)) break;
    }
    g_pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) return 7;
    stage("pipe connected");

    char log_frames[4] = {};
    if (GetEnvironmentVariableA("ECUIO_SUR_LOG_FRAMES", log_frames,
                                sizeof(log_frames)) > 0 &&
        strcmp(log_frames, "1") == 0) {
        g_log_frames = true;
        stage("frame logging on (ECUIO_SUR_LOG_FRAMES=1)");
    }
    char test_fp[16] = {};
    if (GetEnvironmentVariableA("ECUIO_SUR_TEST_FINGERPRINT", test_fp,
                                sizeof(test_fp)) > 0) {
        g_fingerprint = static_cast<uint32_t>(strtoul(test_fp, nullptr, 0));
        stage("fingerprint %08x (ECUIO_SUR_TEST_FINGERPRINT)",
              static_cast<unsigned>(g_fingerprint));
    }
    char test_twice[16] = {};
    if (GetEnvironmentVariableA("ECUIO_SUR_TEST_REPLY_TWICE", test_twice,
                                sizeof(test_twice)) > 0) {
        g_reply_twice = static_cast<uint32_t>(strtoul(test_twice, nullptr, 0));
        if (g_reply_twice == ECUIO_SUR_OP_READ) g_reply_twice = 0;
        stage("the first reply to opcode %u goes twice "
              "(ECUIO_SUR_TEST_REPLY_TWICE)", static_cast<unsigned>(g_reply_twice));
    }

    g_module = LoadLibraryExW(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_module == nullptr) {
        stage("LoadLibraryExW failed for '%s' (err=%lu)", dll_path8,
              GetLastError());
        return 4;
    }
    stage("dll loaded");

    /* Resolve the nine mandatory exports. Ioctl falls back to the
     * legacy 'PassThruIoCtl' spelling after the spec's 'PassThruIoctl'
     * (SAE J2534-1 §7.3.19; the OBDX Pro VX exports that). */
    g_open         = reinterpret_cast<fn_pt_open>(resolve_export(g_module, "PassThruOpen", nullptr));
    g_close        = reinterpret_cast<fn_pt_close>(resolve_export(g_module, "PassThruClose", nullptr));
    g_connect      = reinterpret_cast<fn_pt_connect>(resolve_export(g_module, "PassThruConnect", nullptr));
    g_disconnect   = reinterpret_cast<fn_pt_disconnect>(resolve_export(g_module, "PassThruDisconnect", nullptr));
    g_read_msgs    = reinterpret_cast<fn_pt_read_msgs>(resolve_export(g_module, "PassThruReadMsgs", nullptr));
    g_write_msgs   = reinterpret_cast<fn_pt_write_msgs>(resolve_export(g_module, "PassThruWriteMsgs", nullptr));
    g_ioctl        = reinterpret_cast<fn_pt_ioctl>(resolve_export(g_module, "PassThruIoctl", "PassThruIoCtl"));
    g_start_filter = reinterpret_cast<fn_pt_start_filter>(resolve_export(g_module, "PassThruStartMsgFilter", nullptr));
    g_stop_filter  = reinterpret_cast<fn_pt_stop_filter>(resolve_export(g_module, "PassThruStopMsgFilter", nullptr));

    if (!g_open || !g_close || !g_connect || !g_disconnect || !g_read_msgs ||
        !g_write_msgs || !g_ioctl || !g_start_filter || !g_stop_filter) {
        /* Name every missing export, so an exit 5 can be diagnosed
         * from the log alone (x86 decoration, misspelled names). */
        stage("mandatory export missing (x86 decoration? check .def):");
        if (!g_open)         stage("  PassThruOpen");
        if (!g_close)        stage("  PassThruClose");
        if (!g_connect)     stage("  PassThruConnect");
        if (!g_disconnect)   stage("  PassThruDisconnect");
        if (!g_read_msgs)   stage("  PassThruReadMsgs");
        if (!g_write_msgs)  stage("  PassThruWriteMsgs");
        if (!g_ioctl)       stage("  PassThruIoctl (alt 'PassThruIoCtl' missing too)");
        if (!g_start_filter) stage("  PassThruStartMsgFilter");
        if (!g_stop_filter) stage("  PassThruStopMsgFilter");
        return 5;
    }
    stage("exports resolved");
    /* Optional: the text behind a refused call, and the versions in
     * the session info. */
    g_get_last_error = reinterpret_cast<fn_pt_get_last_error>(
        GetProcAddress(g_module, "PassThruGetLastError"));
    if (g_get_last_error == nullptr) {
        stage("no PassThruGetLastError export: refusals carry no vendor text");
    }
    g_read_version = reinterpret_cast<fn_pt_read_version>(
        GetProcAddress(g_module, "PassThruReadVersion"));
    if (g_read_version == nullptr) {
        stage("no PassThruReadVersion export: the session reports no versions");
    }

    while (true) {
        ecuio_sur_request_t req = {};
        DWORD rd = 0;
        if (!ReadFile(g_pipe, &req, sizeof(req), &rd, nullptr) ||
            rd != sizeof(req)) break;

        ecuio_sur_response_t resp = {};
        resp.request_id = req.request_id;
        resp.status = ECUIO_PT_STATUS_FAILED;

        /* Trust-boundary checks BEFORE calling handlers:
         * the pipe is an untrusted boundary */
        switch (req.opcode) {
        case ECUIO_SUR_OP_OPEN:
            resp.aux = g_fingerprint;
            if (req.u.open.fingerprint != g_fingerprint) {
                stage("OP_OPEN: refused, built from other shared headers "
                      "(client fingerprint %08x, surrogate %08x)",
                      static_cast<unsigned>(req.u.open.fingerprint),
                      static_cast<unsigned>(g_fingerprint));
                break;
            }
            resp.status = handle_op_open();
            break;
        case ECUIO_SUR_OP_CONNECT:
            if (refuse_before_open("OP_CONNECT", false)) {
                break;
            }
            resp.status = handle_op_connect(req.u.connect.protocol_id,
                                            req.u.connect.flags,
                                            req.u.connect.baud,
                                            &resp.handle_or_channel);
            break;
        case ECUIO_SUR_OP_DISCONNECT:
            if (refuse_before_open("OP_DISCONNECT", true)) {
                break;
            }
            resp.status = handle_op_disconnect(req.u.disconnect.channel_id);
            break;
        case ECUIO_SUR_OP_READ:
            resp.status = handle_op_read(g_channel,
                                         req.u.read.timeout_ms,
                                         &resp);
            break;
        case ECUIO_SUR_OP_WRITE:
            /* Trust boundary: validate length before memcpy */
            if (req.u.write.data_len > ECUIO_SUR_MSG_MAX ||
                refuse_before_open("OP_WRITE", true)) {
                resp.status = ECUIO_PT_STATUS_FAILED;
                break;
            }
            resp.status = handle_op_write(req.u.write.channel_id,
                                          req.u.write.data,
                                          req.u.write.data_len,
                                          req.u.write.tx_flags,
                                          req.u.write.timeout_ms,
                                          &resp.aux);
            break;
        case ECUIO_SUR_OP_IOCTL:
            /* Trust boundary: validate input length. READ_VBATT is
             * device-scoped; every other ioctl names a channel. */
            if (req.u.ioctl.input_len > ECUIO_SUR_IOCTL_DATA_MAX ||
                refuse_before_open("OP_IOCTL", req.u.ioctl.ioctl_id !=
                                                   ECUIO_PT_IOCTL_READ_VBATT)) {
                resp.status = ECUIO_PT_STATUS_FAILED;
                break;
            }
            resp.status = handle_op_ioctl(req.u.ioctl.channel_id,
                                          req.u.ioctl.ioctl_id,
                                          req.u.ioctl.input,
                                          req.u.ioctl.input_len,
                                          resp.data, &resp.data_len);
            break;
        case ECUIO_SUR_OP_START_FILTER:
            /* Trust boundary: validate filter pattern lengths */
            if (req.u.start_filter.mask_len > ECUIO_SUR_FILTER_PATTERN_MAX ||
                req.u.start_filter.pattern_len > ECUIO_SUR_FILTER_PATTERN_MAX ||
                req.u.start_filter.fc_len > ECUIO_SUR_FILTER_PATTERN_MAX ||
                refuse_before_open("OP_START_FILTER", true)) {
                resp.status = ECUIO_PT_STATUS_FAILED;
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
        case ECUIO_SUR_OP_STOP_FILTER:
            if (refuse_before_open("OP_STOP_FILTER", true)) {
                break;
            }
            resp.status = handle_op_stop_filter(
                req.u.stop_filter.channel_id,
                req.u.stop_filter.filter_id);
            break;
        case ECUIO_SUR_OP_GET_LAST_ERROR: {
            if (g_get_last_error == nullptr) {
                resp.status = ECUIO_PT_STATUS_NOT_SUPPORTED;
                break;
            }
            /* Room past the spec's 80 for a vendor that overruns it. */
            char text[256] = {};
            const long rc = g_get_last_error(text);
            copy_text(resp.text[0], text, sizeof(text));
            resp.status = static_cast<int32_t>(rc);
            stage("OP_GET_LAST_ERROR: vendor status %ld: %s", rc, resp.text[0]);
            break;
        }
        case ECUIO_SUR_OP_READ_VERSION: {
            if (g_read_version == nullptr) {
                resp.status = ECUIO_PT_STATUS_NOT_SUPPORTED;
                break;
            }
            if (refuse_before_open("OP_READ_VERSION", false)) {
                break;
            }
            char text[ECUIO_SUR_TEXT_FIELDS][256] = {};
            const long rc = g_read_version(g_device_id, text[0], text[1],
                                           text[2]);
            if (rc == ECUIO_PT_STATUS_NOERROR) {
                for (int i = 0; i < ECUIO_SUR_TEXT_FIELDS; ++i) {
                    copy_text(resp.text[i], text[i], sizeof(text[i]));
                }
            }
            resp.status = static_cast<int32_t>(rc);
            stage("OP_READ_VERSION: vendor status %ld: firmware '%s', DLL '%s', "
                  "API '%s'", rc, resp.text[0], resp.text[1], resp.text[2]);
            break;
        }
        case ECUIO_SUR_OP_INJECT: {
            typedef void (*fn_inject)(const unsigned char*, unsigned long,
                                      unsigned long, unsigned long, unsigned long);
            fn_inject inject = reinterpret_cast<fn_inject>(
                GetProcAddress(g_module, "InjectFrame"));
            if (inject != nullptr) {
                /* The DLL reads len bytes of data: never more than the
                 * request carries. */
                const uint32_t len = (req.u.inject.len > ECUIO_SUR_MSG_MAX)
                                     ? ECUIO_SUR_MSG_MAX : req.u.inject.len;
                inject(req.u.inject.data, len,
                       req.u.inject.timestamp, req.u.inject.rx_flags, 0);
                resp.status = ECUIO_PT_STATUS_NOERROR;
            }
            break;
        }
        case ECUIO_SUR_OP_SHUTDOWN: {
            /* Reply before tearing down, so the client's polite
             * shutdown completes at once instead of waiting out its
             * 1.5 s timeout. A failed write doesn't matter; we exit
             * either way. */
            resp.status = ECUIO_PT_STATUS_NOERROR;
            DWORD swr = 0;
            (void)WriteFile(g_pipe, &resp, sizeof(resp), &swr, nullptr);
            goto shutdown;
        }
        default:
            break;
        }

        /* A vendor status is data, not a reason to exit (ADR-0014):
         * the child stays alive across ERR_TIMEOUT, ERR_BUFFER_EMPTY,
         * refused ioctls, failed writes and unknown opcodes, and the
         * client maps each status. Only a broken pipe or OP_SHUTDOWN
         * ends the loop. A READ's frames follow its response. */
        DWORD wr = 0;
        if (!WriteFile(g_pipe, &resp, sizeof(resp), &wr, nullptr) ||
            wr != sizeof(resp)) {
            break;
        }
        if (g_reply_twice != 0 && req.opcode == g_reply_twice) {
            g_reply_twice = 0;
            if (!WriteFile(g_pipe, &resp, sizeof(resp), &wr, nullptr) ||
                wr != sizeof(resp)) {
                break;
            }
        }
        const DWORD frame_bytes =
            static_cast<DWORD>(resp.num_frames * sizeof(ecuio_pt_rx_t));
        if (frame_bytes > 0 &&
            (!WriteFile(g_pipe, s_read_frames, frame_bytes, &wr, nullptr) ||
             wr != frame_bytes)) {
            break;
        }
    }

shutdown:
    if (g_have_channel) g_disconnect(g_channel);
    if (g_have_device) g_close(g_device_id);
    FreeLibrary(g_module);
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    return 0;
}
