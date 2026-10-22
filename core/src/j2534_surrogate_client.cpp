// File/layer: core/src/j2534_surrogate_client.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — full stream and cross-arch suites

#include "qttune/j2534_surrogate.h"
#include "qttune/core.h"
#include "qttune/j2534.h"
#include "qttune/j2534_ptypes.h"
#include "qttune/surrogate_protocol.h"

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

struct qttune_j2534_sur_handle {
    bool    pipe_connected = false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    /* The session log: the child's stdout/stderr, and the client's own
     * lines for this session. log_path is UTF-8 (up to three bytes per
     * path character), "" when there is none. */
    HANDLE log = INVALID_HANDLE_VALUE;
    char log_path[3 * (MAX_PATH + 64)] = {};
    uint32_t next_request_id = 1;
    uint32_t pending_response_id = 0;   /* response in flight from a
                                           timed-out transaction */
    /* The child missed two deadlines in a row: a transaction's, then
     * the wait for its late reply. A slow vendor answers inside the
     * second; a wedged one doesn't. From then on every transaction
     * fails at once instead of costing another timeout each (a
     * teardown would otherwise pay one per filter, the disconnect and
     * the close), and close kills the child without the polite
     * shutdown. */
    bool unresponsive = false;
    /* Batch cache: one OP_READ returns up to 16 frames; dispensed one
     * per read_frame call. Worker-thread-only by construction (the
     * transport serializes all reads through its worker). */
    qttune_sur_frame_t pending[QTTUNE_SUR_MAX_FRAMES_PER_READ];
    uint32_t pending_count = 0;
    uint32_t pending_next = 0;
};

namespace {

constexpr const wchar_t* kSurrogateExeName = L"qttune-j2534-surrogate.exe";
constexpr DWORD kTransactTimeoutMs   = 3000;
constexpr DWORD kHandshakeTimeoutMs_default = 8000;

DWORD handshake_timeout_ms()
{
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA("QTTUNE_SUR_HANDSHAKE_MS",
                                             buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        const DWORD v = static_cast<DWORD>(strtoul(buf, nullptr, 10));
        if (v >= 100) return v;
    }
    return kHandshakeTimeoutMs_default;
}

/* Client trace line: to stderr, and to the session log when there is
 * one, so a GUI launch (no stderr) keeps the client's half next to the
 * child's. Stamped with local wall-clock
 * time, like the child's lines. One WriteFile per line, as on the
 * child's side, so the two never splice mid-line. h may be null. */
void sur_log(const qttune_j2534_sur_handle_t* h, const char* fmt, ...)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[512];
    const int n = snprintf(line, sizeof(line) - 2, "[sur %02u:%02u:%02u.%03u] ",
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
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
    line[len] = '\0';
    fprintf(stderr, "%s\n", line);
    if (h != nullptr && h->log != INVALID_HANDLE_VALUE) {
        line[len++] = '\r';
        line[len++] = '\n';
        DWORD written = 0;
        (void)WriteFile(h->log, line, static_cast<DWORD>(len), &written, nullptr);
    }
}

#define SUR_LOG(h, ...) sur_log((h), __VA_ARGS__)

/* The session log's path. The fixed name keeps the documented path for
 * the usual single session. A second concurrent session finds it held
 * (a sharing violation) and gets a per-spawn name, so its child never
 * runs without a log. path receives the name as UTF-8 (TEMP can hold a
 * non-ASCII user name). Returns INVALID_HANDLE_VALUE, with path empty,
 * if neither name opens. */
HANDLE open_session_log(LONG seq, SECURITY_ATTRIBUTES* sa,
                        char* path, size_t cap)
{
    wchar_t dir[MAX_PATH] = {};
    const DWORD tn = GetTempPathW(MAX_PATH, dir);
    /* No usable TEMP: a dotfile in the current directory. */
    const wchar_t* prefix = (tn == 0 || tn >= MAX_PATH) ? L"." : dir;

    wchar_t wpath[MAX_PATH + 64];
    swprintf(wpath, _countof(wpath), L"%lsqttune-surrogate.log", prefix);
    HANDLE f = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        swprintf(wpath, _countof(wpath), L"%lsqttune-surrogate-%lu-%ld.log",
                 prefix, GetCurrentProcessId(), seq);
        f = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (f == INVALID_HANDLE_VALUE || !qttune_wide_to_utf8(wpath, path, cap)) {
        path[0] = '\0';
    }
    return f;
}

/* True for a drive-absolute ("C:\", "C:/") or UNC ("\\") path. */
bool is_absolute_path(const wchar_t* p)
{
    const bool drive = ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
                       p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
    const bool unc = (p[0] == L'\\' && p[1] == L'\\');
    return drive || unc;
}

/* The surrogate exe: QTTUNE_SURROGATE_EXE when set, else the one next
 * to the running exe. The variable relocates the surrogate and nothing
 * more: it must be an absolute path to an existing file named
 * qttune-j2534-surrogate.exe, so it can't start some other program, and
 * the path is always logged. On failure, *why says what was wrong. */
bool resolve_exe_path(wchar_t* out, size_t cap, bool* from_env, const char** why)
{
    *from_env = false;
    const DWORD n = GetEnvironmentVariableW(L"QTTUNE_SURROGATE_EXE",
                                             out, static_cast<DWORD>(cap));
    if (n > 0) {
        *from_env = true;
        if (n >= cap) {
            *why = "QTTUNE_SURROGATE_EXE is too long";
            return false;
        }
        if (!is_absolute_path(out)) {
            *why = "QTTUNE_SURROGATE_EXE is not an absolute path";
            return false;
        }
        /* An absolute path has a separator; the last one starts the
         * file name. */
        const wchar_t* sep = wcsrchr(out, L'\\');
        const wchar_t* fwd = wcsrchr(out, L'/');
        if (fwd != nullptr && (sep == nullptr || fwd > sep)) sep = fwd;
        if (_wcsicmp(sep + 1, kSurrogateExeName) != 0) {
            *why = "QTTUNE_SURROGATE_EXE does not name qttune-j2534-surrogate.exe";
            return false;
        }
        const DWORD attr = GetFileAttributesW(out);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            *why = "QTTUNE_SURROGATE_EXE names no file";
            return false;
        }
        return true;
    }
    wchar_t self[MAX_PATH] = {};
    const DWORD sn = GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* slash = wcsrchr(self, L'\\');
    if (sn == 0 || sn >= MAX_PATH || slash == nullptr) {
        *why = "the app's own path is unavailable";
        return false;
    }
    const size_t dir = static_cast<size_t>(slash - self) + 1;
    if (dir + wcslen(kSurrogateExeName) + 1 > cap) {
        *why = "the app's path is too long";
        return false;
    }
    wmemcpy(out, self, dir);
    wcscpy_s(out + dir, cap - dir, kSurrogateExeName);
    return true;
}

/* A pipe name no other process can predict. The pipe is created with
 * FILE_FLAG_FIRST_PIPE_INSTANCE, so a process that created a guessed
 * name first would make the spawn fail; the random part rules that out.
 * The pid and sequence stay in the name for whoever reads a log. */
bool make_pipe_name(LONG seq, char* out, size_t cap)
{
    uint8_t rnd[16];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, rnd, sizeof(rnd),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return false;
    }
    int n = snprintf(out, cap, "\\\\.\\pipe\\qttune-j2534-%lu-%ld-",
                     GetCurrentProcessId(), seq);
    for (uint8_t b : rnd) {
        if (n < 0 || static_cast<size_t>(n) + 3 > cap) return false;
        n += snprintf(out + n, cap - static_cast<size_t>(n), "%02x", b);
    }
    return n > 0;
}

/* The pipe's security: full access for this process's user, no one
 * else. The default DACL also lets Everyone open a pipe for reading,
 * which is enough to connect before the child and take the only
 * instance. The child runs under the same token, so it still connects.
 * Lives on the caller's stack until CreateNamedPipe returns. */
struct user_only_security {
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    alignas(DWORD) BYTE acl[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    alignas(void*) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
};

bool init_user_only(user_only_security* s)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD len = 0;
    const BOOL got = GetTokenInformation(token, TokenUser, s->user,
                                         sizeof(s->user), &len);
    CloseHandle(token);
    if (!got) {
        return false;
    }
    PSID sid = reinterpret_cast<TOKEN_USER*>(s->user)->User.Sid;
    ACL* acl = reinterpret_cast<ACL*>(s->acl);
    if (!InitializeAcl(acl, sizeof(s->acl), ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, sid) ||
        !InitializeSecurityDescriptor(&s->sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&s->sd, TRUE, acl, FALSE)) {
        return false;
    }
    s->sa.nLength = sizeof(s->sa);
    s->sa.lpSecurityDescriptor = &s->sd;
    s->sa.bInheritHandle = FALSE;
    return true;
}

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
            GetOverlappedResult(pipe, &ov, &done, TRUE);
        }
    }
    CloseHandle(ov.hEvent);
    return res;
}

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
                *out_got = done;
                res = (done == len) ? RD_OK : RD_SHORT_READ;
            } else {
                *out_err = GetLastError();
                res = RD_OS_ERROR;
            }
        } else if (wr == WAIT_TIMEOUT) {
            CancelIoEx(pipe, &ov);
            DWORD done = 0;
            GetOverlappedResult(pipe, &ov, &done, TRUE);
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

/* Read and discard the late reply to a previous timed-out request, so
 * the next request isn't paired with it (every later call would fail
 * the ID check). If that reply never arrives, the child is hung: mark
 * the handle unresponsive and fail without sending anything new.
 *
 * If the late reply was an OP_READ's, its frames are dropped with it.
 * That takes a read the child answered more than 500 ms past its own
 * timeout, so it's rare, but the drop is logged rather than silent. */
bool drain_pending(qttune_j2534_sur_handle_t* h, DWORD timeout_ms)
{
    qttune_sur_response_t stale = {};
    DWORD got = 0, rerr = 0;
    const read_result rr = ov_read(h->pipe, &stale, sizeof(stale),
                                   timeout_ms, &got, &rerr);
    if (rr != RD_OK) {
        SUR_LOG(h, "drain_pending: stale response id=%u never arrived; "
                "child unresponsive, later requests fail at once\n",
                h->pending_response_id);
        h->unresponsive = true;
        return false;
    }
    if (stale.request_id != h->pending_response_id) {
        SUR_LOG(h, "drain_pending: unexpected id=%u want=%u\n",
                stale.request_id, h->pending_response_id);
        return false;
    }
    if (stale.num_frames > 0) {
        SUR_LOG(h, "drain_pending: dropped %u frame(s) from the late reply "
                "to req_id=%u\n", stale.num_frames, stale.request_id);
    }
    return true;
}

bool transact(qttune_j2534_sur_handle_t* h, const qttune_sur_request_t& req,
              qttune_sur_response_t* out_resp, DWORD timeout_ms)
{
    if (h->unresponsive) {
        return false;
    }
    if (h->pending_response_id != 0) {
        if (!drain_pending(h, timeout_ms)) {
            return false;
        }
        h->pending_response_id = 0;
    }
    if (!ov_write(h->pipe, &req, sizeof(req), timeout_ms)) {
        SUR_LOG(h, "transact: ov_write failed (req_id=%u op=%u err=%lu)\n",
                req.request_id, req.opcode, GetLastError());
        return false;
    }
    DWORD got = 0, rerr = 0;
    const read_result rr = ov_read(h->pipe, out_resp, sizeof(*out_resp),
                                   timeout_ms, &got, &rerr);
    if (rr == RD_TIMEOUT) h->pending_response_id = req.request_id;
    if (rr != RD_OK) {
        const char* kind =
            (rr == RD_SHORT_READ) ? "SHORT-READ"
          : (rr == RD_TIMEOUT)    ? "TIMEOUT"
          :                         "OS-ERROR";
        SUR_LOG(h, "transact: ov_read failed (req_id=%u op=%u %s got=%lu/%zu err=%lu)\n",
                req.request_id, req.opcode, kind, got, sizeof(*out_resp), rerr);
        return false;
    }
    if (out_resp->request_id != req.request_id) {
        SUR_LOG(h, "transact: DESYNC — got req_id=%u want=%u\n",
                out_resp->request_id, req.request_id);
        return false;
    }
    return true;
}

/* Decode the child's exit code (j2534.md "Child exit codes") so a spawn
 * failure names its stage instead of surfacing as a bare LOAD_FAILED.
 * Safe to call when the child is still running — it then does nothing. */
void log_child_exit(const qttune_j2534_sur_handle_t* h)
{
    DWORD code = 0;
    if (h->process == nullptr || !GetExitCodeProcess(h->process, &code)) return;
    if (code == STILL_ACTIVE) return;
    const char* stage =
        (code == 1) ? "bad argument count"
      : (code == 4) ? "LoadLibraryEx of the vendor DLL failed"
      : (code == 5) ? "a mandatory export is missing (named in the child log)"
      : (code == 7) ? "could not connect to the app's pipe"
      :               "unknown";
    SUR_LOG(h, "open: child exited with code %lu (%s)\n", code, stage);
}

} // namespace

extern "C" {

qttune_status_t qttune_j2534_sur_open(const char* dll_path,
                                      qttune_j2534_sur_handle_t** out_handle)
{
    if (dll_path == nullptr || out_handle == nullptr || dll_path[0] == '\0') {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    *out_handle = nullptr;

    /* The DLL path reaches the child as UTF-16 on its command line. */
    wchar_t wdll[QTTUNE_J2534_DLL_PATH_LEN];
    if (!qttune_utf8_to_wide(dll_path, wdll, _countof(wdll))) {
        SUR_LOG(nullptr, "open: dll path is not UTF-8 or too long\n");
        return QTTUNE_ERR_LOAD_FAILED;
    }

    static volatile LONG counter = 0;
    const LONG seq = InterlockedIncrement(&counter);

    qttune_j2534_sur_handle_t* h = new (std::nothrow) qttune_j2534_sur_handle_t();
    if (h == nullptr) return QTTUNE_ERR_OUT_OF_MEMORY;

    /* The log opens first, so every startup failure below lands in it.
     * Inheritable only until the spawn: the child gets it as its
     * stdout/stderr, and the client keeps its own copy for SUR_LOG. */
    SECURITY_ATTRIBUTES inherit_sa = { sizeof(inherit_sa), nullptr, TRUE };
    h->log = open_session_log(seq, &inherit_sa, h->log_path, sizeof(h->log_path));
    const DWORD log_err = (h->log == INVALID_HANDLE_VALUE) ? GetLastError() : 0;

    wchar_t exe[MAX_PATH] = {};
    bool exe_from_env = false;
    const char* exe_why = "";
    const bool exe_ok = resolve_exe_path(exe, _countof(exe), &exe_from_env, &exe_why);
    char exe8[3 * MAX_PATH];
    (void)qttune_wide_to_utf8(exe, exe8, sizeof(exe8));
    if (!exe_ok) {
        SUR_LOG(h, "open: no surrogate exe: %s ('%s')\n", exe_why, exe8);
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }

    /* Local, user-only, unguessable, and refused if the name already
     * exists, so no other process can sit on the child's end. */
    char pipe_name[128];
    wchar_t wpipe[128];
    user_only_security sec = {};
    if (!make_pipe_name(seq, pipe_name, sizeof(pipe_name)) ||
        !qttune_utf8_to_wide(pipe_name, wpipe, _countof(wpipe)) ||
        !init_user_only(&sec)) {
        SUR_LOG(h, "open: pipe setup failed (err=%lu)\n", GetLastError());
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }
    h->pipe = CreateNamedPipeW(wpipe,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                   FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                   PIPE_REJECT_REMOTE_CLIENTS,
                               1, sizeof(qttune_sur_request_t),
                               sizeof(qttune_sur_response_t), 0, &sec.sa);
    if (h->pipe == INVALID_HANDLE_VALUE) {
        SUR_LOG(h, "open: CreateNamedPipe failed (err=%lu)\n", GetLastError());
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }

    h->job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji = {};
    ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (h->job == nullptr ||
        !SetInformationJobObject(h->job, JobObjectExtendedLimitInformation,
                                 &ji, sizeof(ji))) {
        SUR_LOG(h, "open: job object setup failed (err=%lu)\n", GetLastError());
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }

    wchar_t cmdline[MAX_PATH * 3 + 256];
    if (swprintf(cmdline, _countof(cmdline), L"\"%ls\" \"%ls\" \"%ls\"",
                 exe, wdll, wpipe) < 0) {
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }

    /* Only the log reaches the child. Inheritance on its own would hand
     * it every inheritable handle in the app; the handle list narrows it
     * to the one named. */
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    alignas(void*) BYTE attr_buf[128];
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    if (h->log != INVALID_HANDLE_VALUE) {
        SIZE_T attr_size = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf);
        if (attr_size > sizeof(attr_buf) ||
            !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
            SUR_LOG(h, "open: attribute list failed (size %zu)\n",
                    static_cast<size_t>(attr_size));
            qttune_j2534_sur_close(h);
            return QTTUNE_ERR_LOAD_FAILED;
        }
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       &h->log, sizeof(h->log), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attrs);
            SUR_LOG(h, "open: handle list failed (err=%lu)\n", GetLastError());
            qttune_j2534_sur_close(h);
            return QTTUNE_ERR_LOAD_FAILED;
        }
        si.lpAttributeList = attrs;
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = h->log;
        si.StartupInfo.hStdError  = h->log;
    }
    PROCESS_INFORMATION pi = {};
    /* The exe is named twice, as the application and in the command
     * line, so CreateProcess never searches for it. */
    const BOOL spawned = CreateProcessW(
        exe, cmdline, nullptr, nullptr, attrs != nullptr,
        CREATE_SUSPENDED | CREATE_NO_WINDOW |
            (attrs != nullptr ? EXTENDED_STARTUPINFO_PRESENT : 0),
        nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD spawn_err = GetLastError();
    if (attrs != nullptr) {
        DeleteProcThreadAttributeList(attrs);
    }
    if (!spawned) {
        SUR_LOG(h, "open: CreateProcess failed (err=%lu) exe='%s'\n", spawn_err, exe8);
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }
    if (h->log != INVALID_HANDLE_VALUE) {
        /* No later spawn, by this client or anyone in the app, inherits
         * this session's log. */
        SetHandleInformation(h->log, HANDLE_FLAG_INHERIT, 0);
    }
    /* Logged while the child is still suspended, so it heads the
     * session log instead of racing the child's first line. */
    SUR_LOG(h, "open: child pid %lu, surrogate '%s'%s, dll '%s'\n",
            pi.dwProcessId, exe8,
            exe_from_env ? " (QTTUNE_SURROGATE_EXE)" : "", dll_path);
    if (h->log_path[0] != '\0') {
        SUR_LOG(h, "open: log '%s'\n", h->log_path);
    } else {
        SUR_LOG(h, "open: no log (err=%lu)\n", log_err);
    }

    h->process = pi.hProcess;
    /* Kill-on-close is a safety guarantee, not an optimization:
     * without the assignment a wedged child outlives the app. Fail
     * fast instead of silently losing the guarantee. The child is
     * still CREATE_SUSPENDED and no vendor call has run, so a hard
     * termination here is clean. */
    if (!AssignProcessToJobObject(h->job, pi.hProcess)) {
        SUR_LOG(h, "open: AssignProcessToJobObject failed (err=%lu)\n",
                GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
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
            log_child_exit(h);                   /* replaces the inline decode */
        } else {
            SUR_LOG(h, "open: handshake timeout after %lu ms — child alive but "
                    "never opened the pipe (process startup stalled)\n",
                    handshake_timeout_ms());
        }
    }

    /* A failed handshake can leave ConnectNamedPipe pending. Cancel and
     * drain it BEFORE releasing the event and the pipe, or the kernel
     * completes that I/O into freed structures. CancelIoEx on an
     * already-completed request is harmless — it reports NOT_FOUND. */
    if (!connected) {
        CancelIoEx(h->pipe, &ov);
        DWORD drained = 0;
        (void)GetOverlappedResult(h->pipe, &ov, &drained, TRUE);
    }
    CloseHandle(ov.hEvent);
    if (!connected) {
        SUR_LOG(h, "open: handshake failed — child never connected\n");
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }

    h->pipe_connected = true;

    /* OP_OPEN: the child connects the pipe before it loads the vendor
     * DLL, so this transaction covers LoadLibraryEx (the vendor's
     * DllMain), export resolution and PassThruOpen. A load or export
     * failure exits the child (codes 4/5, decoded below); a refusal is
     * a non-success status; no reply with a live child is a vendor
     * blocked in DllMain or PassThruOpen, typically on absent
     * hardware. */
    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_OPEN;
    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, handshake_timeout_ms())) {
        /* Classify by why the transaction failed, not by a liveness
         * snapshot. A reply that TIMED OUT with the child still alive
         * is a vendor wedged in DllMain or PassThruOpen. A broken pipe
         * means the child is exiting with code 4 or 5: its pipe closes
         * a moment before its process handle signals, so a liveness
         * check at that instant would call a failed DLL load
         * "blocked". Wait for that exit and let log_child_exit name
         * the stage instead. */
        const bool timed_out = (h->pending_response_id == req.request_id);
        if (timed_out && WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0) {
            SUR_LOG(h, "open: no reply to OP_OPEN after %lu ms, child alive "
                    "(vendor blocked in DllMain or PassThruOpen)\n",
                    handshake_timeout_ms());
        } else {
            WaitForSingleObject(h->process, 1000);
        }
        log_child_exit(h);
        /* Wait out the kill as well, so close sees a dead child and
         * skips the polite OP_SHUTDOWN it could never answer. */
        TerminateProcess(h->process, 1);
        WaitForSingleObject(h->process, 1000);
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        SUR_LOG(h, "open: PassThruOpen refused (vendor status %d)\n", resp.status);
        qttune_j2534_sur_close(h);
        return QTTUNE_ERR_LOAD_FAILED;
    }
    *out_handle = h;
    return QTTUNE_OK;
}

qttune_status_t qttune_j2534_sur_connect_channel(qttune_j2534_sur_handle_t* h,
                                                 unsigned long baud,
                                                 unsigned long flags,
                                                 unsigned long* out_channel)
{
    if (h == nullptr || out_channel == nullptr) return QTTUNE_ERR_NULL_ARGUMENT;
    if (baud == 0) baud = QTTUNE_PT_BAUD_ISO15765_500K;

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_CONNECT;
    req.u.connect.protocol_id = QTTUNE_PT_PROTOCOL_ISO15765;
    req.u.connect.flags = flags;
    req.u.connect.baud = baud;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        SUR_LOG(h, "connect: PassThruConnect refused (vendor status %d)\n",
                resp.status);
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    *out_channel = resp.handle_or_channel;
    return QTTUNE_OK;
}

qttune_status_t qttune_j2534_sur_disconnect_channel(qttune_j2534_sur_handle_t* h,
                                                    unsigned long channel)
{
    if (h == nullptr) return QTTUNE_ERR_NULL_ARGUMENT;
    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_DISCONNECT;
    req.u.disconnect.channel_id = channel;
    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs)) return QTTUNE_ERR_NO_TRANSPORT;
    return QTTUNE_OK;
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
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (out_num_written != nullptr) *out_num_written = 0;

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
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    if (out_num_written != nullptr) *out_num_written = resp.aux;
    return QTTUNE_OK;
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
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (input == nullptr && input_len != 0) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (out_len != nullptr) *out_len = 0;

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_IOCTL;
    req.u.ioctl.channel_id = channel;
    req.u.ioctl.ioctl_id = ioctl_id;
    if (input_len > 0) memcpy(req.u.ioctl.input, input, input_len);
    req.u.ioctl.input_len = input_len;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    /* Trust boundary, as for read's num_frames: a mismatched child (a
     * stale build-win32 tree, #19) could claim more than the wire
     * carries. Callers size output for the wire limit, so copying a
     * larger claim would overrun both resp.data and their buffer. */
    if (resp.data_len > QTTUNE_SUR_IOCTL_DATA_MAX) {
        SUR_LOG(h, "ioctl: data_len=%u exceeds the wire limit (skew?)\n",
                resp.data_len);
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    if (output != nullptr && resp.data_len > 0) {
        memcpy(output, resp.data, resp.data_len);
    }
    if (out_len != nullptr) *out_len = resp.data_len;
    return QTTUNE_OK;
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
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (mask_len < 1 || mask_len > QTTUNE_SUR_FILTER_PATTERN_MAX ||
        mask_len != pattern_len) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (fc_len > QTTUNE_SUR_FILTER_PATTERN_MAX) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (filter_type == QTTUNE_PT_FLOW_CONTROL_FILTER && fc_len != mask_len) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (filter_type != QTTUNE_PT_FLOW_CONTROL_FILTER && fc_len != 0) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    if (fc_len > 0 && flow_control == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
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
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    *out_filter_id = resp.handle_or_channel;
    return QTTUNE_OK;
}

qttune_status_t qttune_j2534_sur_stop_msg_filter(qttune_j2534_sur_handle_t* h,
                                                  unsigned long channel,
                                                  unsigned long filter_id)
{
    if (h == nullptr) return QTTUNE_ERR_NULL_ARGUMENT;
    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_STOP_FILTER;
    req.u.stop_filter.channel_id = channel;
    req.u.stop_filter.filter_id = filter_id;

    qttune_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    return QTTUNE_OK;
}

qttune_status_t qttune_j2534_sur_read_frame(qttune_j2534_sur_handle_t* h,
                                            unsigned long channel,
                                            qttune_frame_t* out_frame,
                                            uint32_t timeout_ms,
                                            bool* got_frame)
{
    if (h == nullptr || out_frame == nullptr || got_frame == nullptr) {
        return QTTUNE_ERR_NULL_ARGUMENT;
    }
    *got_frame = false;

    /* Dispense from the cache first — no pipe traffic while a batch
     * is partially consumed. */
    if (h->pending_next < h->pending_count) {
        const qttune_sur_frame_t& s = h->pending[h->pending_next++];
        *out_frame = qttune_frame_t{};
        /* channel_id is the frame ABI's bus index (core.h), not the
         * vendor's J2534 ChannelID handle — 0, matching in-proc. */
        out_frame->channel_id = 0;
        out_frame->flags = (s.rx_flags & QTTUNE_PT_CAN_29BIT_ID)
                            ? QTTUNE_FRAME_FLAG_EXTENDED : 0u;
        out_frame->timestamp_us = s.timestamp;
        uint32_t n = s.data_len;
        if (n > sizeof(out_frame->data)) n = sizeof(out_frame->data);
        out_frame->data_length = static_cast<uint16_t>(n);
        memcpy(out_frame->data, s.data, n);
        *got_frame = true;
        return QTTUNE_OK;
    }

    qttune_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = QTTUNE_SUR_OP_READ;
    req.u.read.timeout_ms = timeout_ms;

    qttune_sur_response_t resp = {};
    /* Margin over the vendor timeout: the child spends up to
     * timeout_ms inside ReadMsgs before answering. Transact timing
     * out before the child does means the child is hung, not busy. */
    const DWORD tmo = timeout_ms + 500;
    if (!transact(h, req, &resp, tmo)) {
        return QTTUNE_ERR_TRANSPORT_HARD_FAILURE;   /* child dead/wedged/skewed */
    }
    /* Trust boundary: a skewed stale child can send garbage. */
    if (resp.num_frames > QTTUNE_SUR_MAX_FRAMES_PER_READ) {
        SUR_LOG(h, "read: num_frames=%u exceeds capacity (skew?)\n",
                resp.num_frames);
        return QTTUNE_ERR_TRANSPORT_HARD_FAILURE;
    }

    /* Deliver first, judge the status second. Per J2534-1 a read
     * asking for N messages with a nonzero timeout returns ERR_TIMEOUT
     * TOGETHER WITH however many arrived, so a conforming vendor reports
     * nearly every response that way. The in-proc path delivers such a
     * message too; dropping it would hide the ECU's answer and leave a
     * healthy session showing nothing at all. The child has already
     * applied the count-trust rule (qttune_trusted_count), so a
     * non-empty batch only arrives under NOERROR or ERR_TIMEOUT. */
    if (resp.num_frames > 0) {
        memcpy(h->pending, resp.frames,
               sizeof(qttune_sur_frame_t) * resp.num_frames);
        h->pending_count = resp.num_frames;
        h->pending_next = 0;
        /* Recurse once into the cache-dispense path above. */
        return qttune_j2534_sur_read_frame(h, channel, out_frame,
                                           timeout_ms, got_frame);
    }

    /* Empty batch: the status decides. */
    if (resp.status == static_cast<int32_t>(QTTUNE_PT_STATUS_TIMEOUT)) {
        return QTTUNE_ERR_VEHICLE_TIMEOUT;
    }
    if (resp.status != static_cast<int32_t>(QTTUNE_PT_STATUS_NOERROR)) {
        if (resp.status == static_cast<int32_t>(QTTUNE_PT_STATUS_BUFFER_EMPTY)) {
            return QTTUNE_OK;   /* queue drained — quiet, not failed */
        }
        return QTTUNE_ERR_TRANSPORT_HARD_FAILURE;
    }
    return QTTUNE_OK;   /* NOERROR, nothing available */
}

qttune_status_t qttune_j2534_sur_inject_test_frame(qttune_j2534_sur_handle_t* h,
                                                   const uint8_t* data,
                                                   uint32_t len,
                                                   uint32_t timestamp,
                                                   uint32_t rx_flags)
{
    if (h == nullptr || data == nullptr || len == 0 || len > 64) {
        return QTTUNE_ERR_NULL_ARGUMENT;
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
        return QTTUNE_ERR_NO_TRANSPORT;
    }
    return QTTUNE_OK;
}

qttune_status_t qttune_j2534_sur_close(qttune_j2534_sur_handle_t* h)
{
    if (h == nullptr) return QTTUNE_OK;
    if (h->pipe_connected && h->pipe != INVALID_HANDLE_VALUE &&
        h->process != nullptr &&
        WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0) {
        qttune_sur_request_t req = {};
        req.request_id = h->next_request_id++;
        req.opcode = QTTUNE_SUR_OP_SHUTDOWN;
        qttune_sur_response_t resp = {};
        (void)transact(h, req, &resp, 1500);   /* fails at once if unresponsive */
    }
    /* An unresponsive child won't exit on its own: closing the job
     * below kills it, so don't wait for it first. */
    if (h->process != nullptr && !h->unresponsive) {
        WaitForSingleObject(h->process, 1500);
    }
    if (h->pipe != INVALID_HANDLE_VALUE) CloseHandle(h->pipe);
    if (h->process != nullptr) CloseHandle(h->process);
    if (h->job != nullptr) CloseHandle(h->job);
    if (h->log != INVALID_HANDLE_VALUE) CloseHandle(h->log);
    delete h;
    return QTTUNE_OK;
}

const char* qttune_j2534_sur_log_path(const qttune_j2534_sur_handle_t* h)
{
    return (h != nullptr) ? h->log_path : "";
}

} // extern "C"

#else

extern "C" {

qttune_status_t qttune_j2534_sur_open(const char*, qttune_j2534_sur_handle_t**)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_connect_channel(qttune_j2534_sur_handle_t*,
                                                 unsigned long, unsigned long,
                                                 unsigned long*)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_disconnect_channel(qttune_j2534_sur_handle_t*, unsigned long)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_write_frame(qttune_j2534_sur_handle_t*, unsigned long,
                                             const uint8_t*, uint32_t, uint32_t,
                                             uint32_t, uint32_t*)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_ioctl(qttune_j2534_sur_handle_t*, unsigned long,
                                       uint32_t, const uint8_t*, uint32_t,
                                       uint8_t*, uint32_t*)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_start_msg_filter(qttune_j2534_sur_handle_t*, unsigned long,
                                                  uint32_t, const uint8_t*, uint32_t,
                                                  const uint8_t*, uint32_t,
                                                  const uint8_t*, uint32_t, uint32_t,
                                                  unsigned long*)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_stop_msg_filter(qttune_j2534_sur_handle_t*, unsigned long,
                                                  unsigned long)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_read_frame(qttune_j2534_sur_handle_t*, unsigned long,
                                            qttune_frame_t*, uint32_t, bool*)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_inject_test_frame(qttune_j2534_sur_handle_t*, const uint8_t*,
                                                  uint32_t, uint32_t, uint32_t)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
qttune_status_t qttune_j2534_sur_close(qttune_j2534_sur_handle_t*)
{ return QTTUNE_ERR_NOT_IMPLEMENTED; }
const char* qttune_j2534_sur_log_path(const qttune_j2534_sur_handle_t*)
{ return ""; }

} // extern "C"

#endif /* _WIN32 */
