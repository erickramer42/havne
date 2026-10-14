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
};

namespace {

constexpr const char* kSurrogateExeName = "qttune-j2534-surrogate.exe";
constexpr DWORD kHandshakeTimeoutMs = 8000;
constexpr DWORD kTransactTimeoutMs   = 3000;

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

/* Pseudo-sync read on the overlapped pipe: bounded, cancellable. */
bool ov_read(HANDLE pipe, void* buf, DWORD len, DWORD timeout_ms)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    DWORD nread = 0;
    const BOOL ok = ReadFile(pipe, buf, len, &nread, &ov);
    bool res = false;
    if (ok) {
        res = (nread == len);
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

/* One request/response round-trip with a bounded timeout. */
bool transact(qttune_j2534_sur_handle_t* h, const qttune_sur_request_t& req,
              qttune_sur_response_t* out_resp, DWORD timeout_ms)
{
    if (!ov_write(h->pipe, &req, sizeof(req), timeout_ms)) {
        SUR_LOG("transact: ov_write failed (req_id=%u op=%u err=%lu)\n",
                req.request_id, req.opcode, GetLastError());
        return false;
    }
    FlushFileBuffers(h->pipe);
    if (!ov_read(h->pipe, out_resp, sizeof(*out_resp), timeout_ms)) {
        SUR_LOG("transact: ov_read failed (req_id=%u op=%u err=%lu)\n",
                req.request_id, req.opcode, GetLastError());
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

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        SUR_LOG("open: CreateProcessA failed (err=%lu)\n  exe='%s'\n"
                "  dll='%s'\n", GetLastError(), exe, dll_path);
        qttune_j2534_sur_close(h);
        return QT_ERR_LOAD_FAILED;
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
                                                 kHandshakeTimeoutMs);
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
                    kHandshakeTimeoutMs);
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
    if (!transact(h, req, &resp, kHandshakeTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        DWORD ecx = 0;
        GetExitCodeProcess(h->process, &ecx);
        SUR_LOG("open: OP_OPEN handshake failed (child_exit=%lu "
                "status=%d)\n", ecx, resp.status);
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
