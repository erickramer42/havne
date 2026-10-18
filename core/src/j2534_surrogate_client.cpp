// File/layer: core/src/j2534_surrogate_client.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — driven end-to-end by test_j2534_surrogate against the
//          x86-built fake DLL through the real x86 surrogate process.
// CI: non-Windows gets linking stubs, identical to the loader pattern.
//
// PIPE MODEL: the handle is FILE_FLAG_OVERLAPPED, so ALL pipe I/O goes
// through ov_write/ov_read (pseudo-sync with bounded timeout). This is
// load-bearing, not style: ConnectNamedPipe on a synchronous handle
// ignores OVERLAPPED and blocks forever — the DeadDll hang (2026-10-14).
// Overlapped also gives transact() timeouts, bounding the wedged-child
// case in close() that previously could block WriteFile indefinitely.

#include "qttune/j2534_surrogate.h"
#include "qttune/core.h"
#include "qttune/j2534_ptypes.h"
#include "qttune/surrogate_protocol.h"

#include <cstring>
#include <cstdint>
#include <new>
#include <cstdlib>

#ifdef _WIN32

#include <windows.h>
#include <cstdio>

struct qttune_j2534_sur_handle {
    bool    pipe_connected = false;  /* true only after OP_OPEN handshake */
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    qttune_sur_ring_t* ring = nullptr;
    HANDLE data_event = nullptr;   /* auto-reset, set by surrogate */
    HANDLE job = nullptr;          /* KILL_ON_JOB_CLOSE owns surrogate */
    HANDLE process = nullptr;
    uint32_t next_request_id = 1;
    uint32_t pending_response_id = 0;  /* response still in flight from
                                          a timed-out transaction */
};

namespace {

constexpr const char* kSurrogateExeName = "qttune-j2534-surrogate.exe";
constexpr DWORD kTransactTimeoutMs   = 3000;
constexpr DWORD kHandshakeTimeoutMs_default = 8000;

/* Test knob: shrink the handshake window so a blocked-vendor-open test
 * fails in milliseconds instead of eight seconds. Env, not a code
 * change, precisely because the child inherits the environment — the
 * knob arms the child-side fake from an x64 test process without any
 * protocol surface (the OP_INJECT lesson generalized). */
DWORD handshake_timeout_ms()
{
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA("QTTUNE_SUR_HANDSHAKE_MS",
                                             buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        const DWORD v = static_cast<DWORD>(strtoul(buf, nullptr, 10));
        if (v >= 100) return v;   /* sanity floor */
    }
    return kHandshakeTimeoutMs_default;
}

/* TEMP cross-arch diagnostics — stderr surfaces in ctest
 * --output-on-failure output. Remove once diagnosed. */
#define SUR_LOG(...) fprintf(stderr, "[sur] " __VA_ARGS__)

/* Surrogate exe lives NEXT TO the running process's image (same
 * discipline as the baked fake-DLL path). Env override FIRST: lets an
 * x64 app/test drive an x86-built surrogate (Win32-configure tree)
 * without code changes. */
bool build_exe_path(char* out, size_t cap)
{
    const DWORD n = GetEnvironmentVariableA("QTTUNE_SURROGATE_EXE",
                                             out, static_cast<DWORD>(cap));
    if (n > 0 && n < cap) {
        return true;
    }
    char self[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, self, MAX_PATH) == 0) return false;
    const char* slash = strrchr(self, '\\');
    if (slash == nullptr) return false;
    const size_t dir = static_cast<size_t>(slash - self) + 1;
    if (dir + strlen(kSurrogateExeName) + 1 > cap) return false;
    memcpy(out, self, dir);
    strcpy_s(out + dir, cap - dir, kSurrogateExeName);
    return true;
}

/* Pseudo-sync write on the overlapped pipe: bounded, cancellable. */
bool ov_write(HANDLE pipe, const void* buf, DWORD len, DWORD timeout_ms)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, buf, len, &written, &ov);
    bool res = false;
    if (ok) {
        res = (written == len);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout_ms) == WAIT_OBJECT_0) {
            DWORD done = 0;
            if (GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                res = (done == len);
            }
        } else {
            CancelIoEx(pipe, &ov);
            DWORD done = 0;
            GetOverlappedResult(pipe, &ov, &done, TRUE);   /* drain */
        }
    }
    CloseHandle(ov.hEvent);
    return res;
}

/* Read outcomes are three different diagnoses, not one bool plus a
 * stale GetLastError(): SHORT_READ = struct-layout skew between trees;
 * TIMEOUT = child wedged or vendor blocked; OS_ERROR = broken pipe,
 * the child died. The first real-VX failure report was misattributed
 * exactly because these were collapsed. */
enum read_result { RD_OK, RD_SHORT_READ, RD_TIMEOUT, RD_OS_ERROR };

read_result ov_read(HANDLE pipe, void* buf, DWORD len, DWORD timeout_ms,
                    DWORD* out_got, DWORD* out_err)
{
    *out_got = 0;
    *out_err = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        *out_err = GetLastError();
        return RD_OS_ERROR;
    }

    DWORD nread = 0;
    const BOOL ok = ReadFile(pipe, buf, len, &nread, &ov);
    read_result res = RD_OK;
    if (ok) {
        *out_got = nread;
        res = (nread == len) ? RD_OK : RD_SHORT_READ;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        const DWORD wr = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wr == WAIT_OBJECT_0) {
            DWORD done = 0;
            if (GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                *out_got = done;   /* byte-mode pipe: ANY bytes complete the read */
                res = (done == len) ? RD_OK : RD_SHORT_READ;
            } else {
                *out_err = GetLastError();   /* captured BEFORE CloseHandle */
                res = RD_OS_ERROR;
            }
        } else if (wr == WAIT_TIMEOUT) {
            CancelIoEx(pipe, &ov);
            DWORD done = 0;
            GetOverlappedResult(pipe, &ov, &done, TRUE);   /* drain */
            res = RD_TIMEOUT;
        } else {
            *out_err = GetLastError();
            res = RD_OS_ERROR;
        }
    } else {
        *out_err = GetLastError();
        res = RD_OS_ERROR;
    }
    CloseHandle(ov.hEvent);
    return res;
}

/* Consume-and-discard the response to a PREVIOUS timed-out
 * transaction (review finding 1). Without this, the next request's
 * read mispairs with the stale response and every subsequent op
 * fails the ID check — the desync. If the stale response never
 * arrives, the child never answered at all: session unusable, fail
 * without writing anything new. */
bool drain_pending(qttune_j2534_sur_handle_t* h, DWORD timeout_ms)
{
    qttune_sur_response_t stale = {};
    DWORD got = 0, rerr = 0;
    const read_result rr = ov_read(h->pipe, &stale, sizeof(stale),
                                   timeout_ms, &got, &rerr);
    if (rr != RD_OK) {
        SUR_LOG("drain_pending: stale response id=%u never arrived\n",
                h->pending_response_id);
        return false;
    }
    if (stale.request_id != h->pending_response_id) {
        SUR_LOG("drain_pending: unexpected id=%u want=%u\n",
                stale.request_id, h->pending_response_id);
        return false;
    }
    return true;
}

/* One request/response round-trip with a bounded timeout.
 * REVIEW FINDING 1 (2026-10-17), two-part fix:
 *  - FlushFileBuffers REMOVED (confirmed live: a synchronous flush on
 *    the overlapped pipe handle blocked the parent indefinitely while
 *    the child sat healthy and idle). Byte-mode pipe, one fixed-size
 *    struct in flight: no flush needed, ov_write's pseudo-sync already
 *    guarantees completion or cancellation.
 *  - Stale-response drain: after a timeout, the response is still in
 *    flight. Consume it before the next request so the read can't
 *    mispair against it. */
bool transact(qttune_j2534_sur_handle_t* h, const qttune_sur_request_t& req,
              qttune_sur_response_t* out_resp, DWORD timeout_ms)
{
    if (h->pending_response_id != 0) {
        if (!drain_pending(h, timeout_ms)) {
            return false;
        }
        h->pending_response_id = 0;
    }
    if (!ov_write(h->pipe, &req, sizeof(req), timeout_ms)) {
        SUR_LOG("transact: ov_write failed (req_id=%u op=%u err=%lu)\n",
                req.request_id, req.opcode, GetLastError());
        return false;
    }
    DWORD got = 0, rerr = 0;
    const read_result rr = ov_read(h->pipe, out_resp, sizeof(*out_resp),
                                   timeout_ms, &got, &rerr);
    if (rr == RD_TIMEOUT) {
        /* Remember the orphan: its response is still coming. */
        h->pending_response_id = req.request_id;
    }
    if (rr != RD_OK) {
        const char* kind =
            (rr == RD_SHORT_READ) ? "SHORT-READ (struct layout skew between trees?)"
          : (rr == RD_TIMEOUT)    ? "TIMEOUT (child wedged or vendor blocked)"
          :                         "OS-ERROR (broken pipe — child died?)";
        SUR_LOG("transact: ov_read failed (req_id=%u op=%u %s got=%lu/%zu err=%lu)\n",
                req.request_id, req.opcode, kind, got, sizeof(*out_resp), rerr);
        return false;
    }
    if (out_resp->request_id != req.request_id) {
        SUR_LOG("transact: DESYNC — got req_id=%u want=%u "
                "(struct layout mismatch?)\n",
                out_resp->request_id, req.request_id);
        return false;
    }
    return true;
}

} // namespace

extern "C" {

qttune_status_t qttune_j2534_sur_open(const char* dll_path,
                                      qttune_j2534_sur_handle_t** out_handle)
{
    if (dll_path == nullptr || out_handle == nullptr || dll_path[0] == '\0') {
        return QT_ERR_NULL_ARGUMENT;
    }
    *out_handle = nullptr;

    char exe[MAX_PATH] = {};
    if (!build_exe_path(exe, sizeof(exe))) {
        return QT_ERR_LOAD_FAILED;
    }

    static volatile LONG counter = 0;
    const LONG seq = InterlockedIncrement(&counter);
    char pipe_name[96], map_name[96], evt_name[96];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\qttune-j2534-%lu-%ld",
             GetCurrentProcessId(), seq);
    snprintf(map_name, sizeof(map_name), "Local\\qttune-j2534-ring-%lu-%ld",
             GetCurrentProcessId(), seq);
    snprintf(evt_name, sizeof(evt_name), "Local\\qttune-j2534-evt-%lu-%ld",
             GetCurrentProcessId(), seq);

    qttune_j2534_sur_handle_t* h = new (std::nothrow) qttune_j2534_sur_handle_t();
    if (h == nullptr) return QT_ERR_OUT_OF_MEMORY;

    h->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                    PAGE_READWRITE, 0,
                                    sizeof(qttune_sur_ring_t), map_name);
    if (h->mapping == nullptr) { delete h; return QT_ERR_OUT_OF_MEMORY; }
    h->ring = static_cast<qttune_sur_ring_t*>(
        MapViewOfFile(h->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (h->ring == nullptr) {
        CloseHandle(h->mapping); delete h; return QT_ERR_OUT_OF_MEMORY;
    }
    memset(h->ring, 0, sizeof(qttune_sur_ring_t));

    h->data_event = CreateEventA(nullptr, FALSE, FALSE, evt_name);
    /* FILE_FLAG_OVERLAPPED is LOAD-BEARING: without it ConnectNamedPipe
     * ignores OVERLAPPED and blocks until a client connects — forever
     * on the dead-DLL path. All subsequent I/O uses ov_write/ov_read. */
    h->pipe = CreateNamedPipeA(pipe_name,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               1, sizeof(qttune_sur_request_t),
                               sizeof(qttune_sur_response_t), 0, nullptr);
    if (h->data_event == nullptr || h->pipe == INVALID_HANDLE_VALUE) {
        SUR_LOG("open: pipe/evt create failed (pipe_err=%lu)\n",
                GetLastError());
        qttune_j2534_sur_close(h);
        return QT_ERR_OUT_OF_MEMORY;
    }

    h->job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji = {};
    ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (h->job == nullptr ||
        !SetInformationJobObject(h->job, JobObjectExtendedLimitInformation,
                                 &ji, sizeof(ji))) {
        SUR_LOG("open: job setup failed (err=%lu)\n", GetLastError());
        qttune_j2534_sur_close(h);
        return QT_ERR_OUT_OF_MEMORY;
    }

    char cmdline[MAX_PATH * 3 + 256];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\" \"%s\" \"%s\" \"%s\"",
             exe, dll_path, pipe_name, map_name, evt_name);

    /* Child stderr redirect: the surrogate's [surrogate] stage lines are
     * the load-bearing diagnostics for real-vendor failures, but a GUI
     * app has no console — they go to %TEMP% instead. bInheritHandles is
     * safe here because every other handle this client creates passes
     * nullptr security attributes (non-inheritable), so ONLY this file
     * crosses into the child. Note: CREATE_ALWAYS means the last spawn
     * wins; fine for diagnostics, each child rewrites from scratch. */
    char log_path[MAX_PATH] = {};
    {
        const DWORD tn = GetTempPathA(sizeof(log_path), log_path);
        if (tn == 0 || tn >= sizeof(log_path)) {
            strcpy_s(log_path, sizeof(log_path), ".qttune-surrogate.log");
        } else {
            strcat_s(log_path, sizeof(log_path), "qttune-surrogate.log");
        }
    }
    SECURITY_ATTRIBUTES inherit_sa = { sizeof(inherit_sa), nullptr, TRUE };
    HANDLE child_log = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ,
                                   &inherit_sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si = { sizeof(si) };
    if (child_log != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = child_log;
        si.hStdError  = child_log;
    }
    PROCESS_INFORMATION pi = {};
    /* CREATE_NO_WINDOW: a GUI app must never flash consoles; the
     * redirected log file replaces the flash as evidence. */
    if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr,
                        (child_log != INVALID_HANDLE_VALUE),
                        CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        SUR_LOG("open: CreateProcessA failed (err=%lu)\n  exe='%s'\n"
                "  dll='%s'\n", GetLastError(), exe, dll_path);
        if (child_log != INVALID_HANDLE_VALUE) CloseHandle(child_log);
        qttune_j2534_sur_close(h);
        return QT_ERR_LOAD_FAILED;
    }
    if (child_log != INVALID_HANDLE_VALUE) {
        CloseHandle(child_log);   /* the child holds its own inherited copy */
    }
    h->process = pi.hProcess;
    AssignProcessToJobObject(h->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    /* Truly-overlapped connect: wait on connect-completion OR
     * surrogate-death — a child that exited (bad DLL, failed
     * PassThruOpen) fails FAST instead of wedging. */
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        qttune_j2534_sur_close(h);
        return QT_ERR_LOAD_FAILED;
    }
    BOOL conn = ConnectNamedPipe(h->pipe, &ov);
    DWORD cerr = GetLastError();
    bool connected = false;
    if (conn || cerr == ERROR_PIPE_CONNECTED) {
        connected = true;
    } else if (cerr == ERROR_IO_PENDING) {
        const HANDLE waits[2] = { ov.hEvent, h->process };
        const DWORD wr = WaitForMultipleObjects(2, waits, FALSE,
                                                 handshake_timeout_ms());
        if (wr == WAIT_OBJECT_0) {
            DWORD xfer = 0;
            connected = GetOverlappedResult(h->pipe, &ov, &xfer, FALSE);
        } else if (wr == WAIT_OBJECT_0 + 1) {
            DWORD ecx = 0;
            if (GetExitCodeProcess(h->process, &ecx)) {
                SUR_LOG("open: child died pre-connect, exit=%lu\n", ecx);
            }
        } else {
            SUR_LOG("open: connect TIMEOUT after %lums\n",
                handshake_timeout_ms());
        }
    } else {
        SUR_LOG("open: ConnectNamedPipe immediate failure (err=%lu)\n",
                cerr);
    }
    if (!connected) {
        /* Cancel and DRAIN the outstanding operation before teardown:
         * closing a handle with pending I/O is undefined. */
        CancelIoEx(h->pipe, &ov);
        DWORD junk = 0;
        GetOverlappedResult(h->pipe, &ov, &junk, TRUE);
    }
    CloseHandle(ov.hEvent);
    if (!connected) {
        qttune_j2534_sur_close(h);
        return QT_ERR_LOAD_FAILED;   /* child died pre-connect, or timeout */
    }

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_OPEN;
    qttune_sur_response_t resp = {};
    const ULONGLONG t0 = GetTickCount64();
    if (!transact(h, req, &resp, handshake_timeout_ms())) {
        /* Measured facts, not theories. A crashed/exited child can remain
         * "alive" on the process handle for a moment AFTER the pipe
         * breaks, so give it a short grace period before ruling — 250 ms
         * is bounded and keeps the blocked-open test fast. */
        DWORD ecx = 0;
        if (WaitForSingleObject(h->process, 250) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(h->process, &ecx)) {
            SUR_LOG("open: OP_OPEN failed after %llums — child EXITED, "
                    "exit=%lu (1=args,2=mapping,3=view/event,4=LoadLibrary,"
                    "5=export,7=pipe)\n", GetTickCount64() - t0, ecx);
        } else {
            SUR_LOG("open: OP_OPEN unanswered after %llums, child ALIVE — "
                    "child stages in %s\n", GetTickCount64() - t0, log_path);
        }
        qttune_j2534_sur_close(h);
        return QT_ERR_LOAD_FAILED;
    }
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        SUR_LOG("open: vendor PassThruOpen refused (status=%d)\n",
                resp.status);
        qttune_j2534_sur_close(h);
        return QT_ERR_LOAD_FAILED;
    }

    h->pipe_connected = true;   /* only NOW may close() transact */
    *out_handle = h;
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_connect_channel(qttune_j2534_sur_handle_t* h,
                                                 unsigned long baud,
                                                 unsigned long flags,
                                                 unsigned long* out_channel)
{
    if (h == nullptr || out_channel == nullptr) return QT_ERR_NULL_ARGUMENT;
    if (baud == 0) baud = QTTUNE_PT_BAUD_ISO15765_500K;   /* loader contract */

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_CONNECT;
    req.u.connect.protocol_id = QTTUNE_PT_PROTOCOL_ISO15765;
    req.u.connect.flags = flags;
    req.u.connect.baud = baud;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
        return QT_ERR_NO_TRANSPORT;
    }
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QT_ERR_NO_TRANSPORT;
    }
    *out_channel = resp.handle_or_channel;
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_disconnect_channel(qttune_j2534_sur_handle_t* h,
                                                    unsigned long channel)
{
    if (h == nullptr) return QT_ERR_NULL_ARGUMENT;
    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_DISCONNECT;
    req.u.disconnect.channel_id = channel;
    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs)) return QT_ERR_NO_TRANSPORT;
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_write_frame(qttune_j2534_sur_handle_t* h,
                                             unsigned long channel,
                                             const uint8_t* data,
                                             uint32_t len,
                                             uint32_t tx_flags,
                                             uint32_t timeout_ms,
                                             uint32_t* out_num_written)
{
    if (h == nullptr || data == nullptr || len == 0 ||
        len > QTTUNE_SUR_MSG_MAX) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (out_num_written != nullptr) {
        *out_num_written = 0;
    }

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_WRITE;
    req.u.write.channel_id = channel;
    memcpy(req.u.write.data, data, len);
    req.u.write.data_len = len;
    req.u.write.tx_flags = tx_flags;
    req.u.write.timeout_ms = timeout_ms;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
        return QT_ERR_NO_TRANSPORT;   /* IPC failure — pipe/child/skew */
    }
    /* Nonzero vendor status maps like connect_channel does; a PARTIAL
     * write (vendor NOERROR but count < requested) surfaces via aux —
     * the caller sees num_written < len and decides. We do not hide
     * the count, and we do not conflate IPC failure with vendor
     * refusal — different failure, different path home. */
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QT_ERR_NO_TRANSPORT;
    }
    if (out_num_written != nullptr) {
        *out_num_written = resp.aux;
    }
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_ioctl(qttune_j2534_sur_handle_t* h,
                                       unsigned long channel,
                                       uint32_t ioctl_id,
                                       const uint8_t* input,
                                       uint32_t input_len,
                                       uint8_t* output,
                                       uint32_t* out_len)
{
    if (h == nullptr || input_len > QTTUNE_SUR_IOCTL_DATA_MAX) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (input == nullptr && input_len != 0) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (out_len != nullptr) {
        *out_len = 0;
    }

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_IOCTL;
    req.u.ioctl.channel_id = channel;
    req.u.ioctl.ioctl_id = ioctl_id;
    if (input_len > 0) {
        memcpy(req.u.ioctl.input, input, input_len);
    }
    req.u.ioctl.input_len = input_len;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QT_ERR_NO_TRANSPORT;
    }

    /* GET_CONFIG writes results back through pInput (the flattened
     * array); READ_VBATT puts its ULONG in the output slot. Both come
     * home in resp.data — copy out, honoring a caller-provided cap.
     * A NULL output with non-NULL out_len is legal (length probe). */
    if (output != nullptr && resp.data_len > 0) {
        memcpy(output, resp.data, resp.data_len);
    }
    if (out_len != nullptr) {
        *out_len = resp.data_len;
    }
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_start_msg_filter(qttune_j2534_sur_handle_t* h,
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
        return QT_ERR_NULL_ARGUMENT;
    }
    /* Spec: pattern 1..12 bytes; mask/pattern lengths must MATCH (the
     * vendor compares them bitwise). Flow control rides only with
     * FLOW_CONTROL_FILTER. Enforcing here keeps malformed filter
     * setups from wasting a pipe round trip and, more importantly,
     * from reaching a real device in garbage form. */
    if (mask_len < 1 || mask_len > QTTUNE_SUR_FILTER_PATTERN_MAX ||
        mask_len != pattern_len) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (fc_len > QTTUNE_SUR_FILTER_PATTERN_MAX) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (filter_type == QTTUNE_PT_FLOW_CONTROL_FILTER && fc_len != mask_len) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (filter_type != QTTUNE_PT_FLOW_CONTROL_FILTER && fc_len != 0) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (fc_len > 0 && flow_control == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    *out_filter_id = 0;

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_START_FILTER;
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

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QT_ERR_NO_TRANSPORT;
    }
    *out_filter_id = resp.handle_or_channel;
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_stop_msg_filter(qttune_j2534_sur_handle_t* h,
                                                  unsigned long channel,
                                                  unsigned long filter_id)
{
    if (h == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_STOP_FILTER;
    req.u.stop_filter.channel_id = channel;
    req.u.stop_filter.filter_id = filter_id;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QT_ERR_NO_TRANSPORT;
    }
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_read_frame(qttune_j2534_sur_handle_t* h,
                                            unsigned long /*channel*/,
                                            qttune_frame_t* out_frame,
                                            uint32_t timeout_ms,
                                            bool* got_frame)
{
    if (h == nullptr || out_frame == nullptr || got_frame == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    *got_frame = false;

    uint32_t tail = h->ring->tail;          /* consumer-owned */
    if (tail == h->ring->head) {            /* empty */
        const DWORD wait = (timeout_ms < 50) ? timeout_ms : 50;
        WaitForSingleObject(h->data_event, wait);
        tail = h->ring->tail;
        if (tail == h->ring->head) return QT_OK;
    }

    const qttune_sur_ring_slot_t& s =
        h->ring->slots[tail & (QTTUNE_SURROGATE_RING_SIZE - 1)];

    *out_frame = qttune_frame_t{};
    out_frame->arbitration_id = 0;          /* documented inertness (F2) */
    out_frame->channel_id = 0;
    out_frame->flags = (s.rx_status & QTTUNE_PT_CAN_29BIT_ID)
                        ? QTTUNE_FRAME_FLAG_EXTENDED : 0u;
    out_frame->timestamp_us = s.timestamp;  /* vendor units, passthrough */
    unsigned long n = s.data_len;
    if (n > sizeof(out_frame->data)) n = sizeof(out_frame->data);
    out_frame->data_length = static_cast<uint16_t>(n);
    memcpy(out_frame->data, s.data, n);

    h->ring->tail = tail + 1;               /* release slot AFTER copy */
    *got_frame = true;
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_inject_test_frame(qttune_j2534_sur_handle_t* h,
                                                   const uint8_t* data,
                                                   uint32_t len,
                                                   uint32_t timestamp,
                                                   uint32_t rx_flags)
{
    if (h == nullptr || data == nullptr || len == 0 || len > 64) {
        return QT_ERR_NULL_ARGUMENT;
    }
    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_INJECT;
    memcpy(req.u.inject.data, data, len);
    req.u.inject.len = len;
    req.u.inject.timestamp = timestamp;
    req.u.inject.rx_flags = rx_flags;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QT_ERR_NO_TRANSPORT;
    }
    return QT_OK;
}

qttune_status_t qttune_j2534_sur_close(qttune_j2534_sur_handle_t* h)
{
    if (h == nullptr) return QT_OK;

    /* Polite shutdown ONLY if the handshake completed AND the child is
     * alive: the dead-DLL child never connected, and a wedged child is
     * handled by the transact timeout + job kill. */
    if (h->pipe_connected && h->pipe != INVALID_HANDLE_VALUE &&
        h->process != nullptr &&
        WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0) {
        qttune_sur_request_t req = {};
        req.request_id = h->next_request_id++;
        req.opcode = QTTUNE_SUR_OP_SHUTDOWN;
        qttune_sur_response_t resp = {};
        (void)transact(h, req, &resp, 1500);   /* bounded best-effort */
    }
    if (h->process != nullptr) {
        WaitForSingleObject(h->process, 1500);
    }
    if (h->ring != nullptr) UnmapViewOfFile(h->ring);
    if (h->pipe != INVALID_HANDLE_VALUE) CloseHandle(h->pipe);
    if (h->data_event != nullptr) CloseHandle(h->data_event);
    if (h->mapping != nullptr) CloseHandle(h->mapping);
    if (h->process != nullptr) CloseHandle(h->process);
    if (h->job != nullptr) CloseHandle(h->job);   /* kills if still alive */
    delete h;
    return QT_OK;
}

} // extern "C"

#else

extern "C" {

qttune_status_t qttune_j2534_sur_open(const char*, qttune_j2534_sur_handle_t** out)
{ if (out) *out = nullptr; return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_connect_channel(qttune_j2534_sur_handle_t*, unsigned long,
                                                 unsigned long, unsigned long*)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_disconnect_channel(qttune_j2534_sur_handle_t*, unsigned long)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_write_frame(qttune_j2534_sur_handle_t*, unsigned long,
                                             const uint8_t*, uint32_t, uint32_t,
                                             uint32_t, uint32_t*)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_ioctl(qttune_j2534_sur_handle_t*, unsigned long,
                                       uint32_t, const uint8_t*, uint32_t,
                                       uint8_t*, uint32_t*)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_start_msg_filter(qttune_j2534_sur_handle_t*, unsigned long,
                                                  uint32_t, const uint8_t*, uint32_t,
                                                  const uint8_t*, uint32_t,
                                                  const uint8_t*, uint32_t, uint32_t,
                                                  unsigned long*)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_stop_msg_filter(qttune_j2534_sur_handle_t*, unsigned long,
                                                  unsigned long)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_read_frame(qttune_j2534_sur_handle_t*, unsigned long,
                                            qttune_frame_t*, uint32_t, bool* got)
{ if (got) *got = false; return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_inject_test_frame(qttune_j2534_sur_handle_t*, const uint8_t*,
                                                  uint32_t, uint32_t, uint32_t)
{ return QT_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_close(qttune_j2534_sur_handle_t*)
{ return QT_ERR_NOT_IMPLEMENTED; }

} // extern "C"

#endif /* _WIN32 */
