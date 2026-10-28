// File/layer: core/src/j2534_surrogate_client.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — every J2534 suite (the x64 surrogate) and the
//          cross-arch run (the x86 one); the exe chosen by the DLL's arch

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

struct ecuio_j2534_sur_handle {
    bool    pipe_connected = false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    /* The manual-reset event every overlapped operation on the pipe
     * waits on. One per handle is enough: a handle is used by one
     * thread at a time (the connect thread, then the worker, then the
     * teardown thread), and each operation completes, or is canceled
     * and drained, before the next begins. */
    HANDLE io_event = nullptr;
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
    /* Batch cache: one OP_READ returns up to 16 messages; dispensed one
     * per read_msg call. Worker-thread-only by construction (the
     * transport serializes all reads through its worker). */
    ecuio_pt_rx_t pending[ECUIO_SUR_MAX_FRAMES_PER_READ];
    uint32_t pending_count = 0;
    uint32_t pending_next = 0;
    /* A read answered ERR_BUFFER_OVERFLOW with no message after the
     * loss: the next one dispensed carries ECUIO_PT_BUFFER_OVERFLOW */
    bool overflow_pending = false;
};

namespace {

/* One surrogate per DLL architecture (ADR-0025): the x86 exe, and the
 * x64 one that also hosts a PE for any other machine, so Windows
 * answers it as the app's own process would. Each has its own
 * relocation variable. */
struct SurrogateExe {
    const wchar_t* name;
    const char* name8;
    const wchar_t* env;
    const char* env8;
};
constexpr SurrogateExe kX86Surrogate = {
    L"ecuio-j2534-surrogate.exe", "ecuio-j2534-surrogate.exe",
    L"ECUIO_SURROGATE_EXE", "ECUIO_SURROGATE_EXE"};
constexpr SurrogateExe kX64Surrogate = {
    L"ecuio-j2534-surrogate-x64.exe", "ecuio-j2534-surrogate-x64.exe",
    L"ECUIO_SURROGATE_X64_EXE", "ECUIO_SURROGATE_X64_EXE"};

const SurrogateExe& surrogate_for(uint32_t dll_arch)
{
    return dll_arch == ECUIO_DLL_ARCH_X86 ? kX86Surrogate : kX64Surrogate;
}
constexpr DWORD kTransactTimeoutMs   = 3000;
constexpr DWORD kHandshakeTimeoutMs_default = 8000;

DWORD handshake_timeout_ms()
{
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA("ECUIO_SUR_HANDSHAKE_MS",
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
void sur_log(const ecuio_j2534_sur_handle_t* h, const char* fmt, ...)
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

/* Earlier sessions' logs kept beside the current one. A suite run
 * spawns dozens of sessions, so this bounds TEMP, not history. */
constexpr int kKeptSessionLogs = 50;

/* "ecuio-surrogate.YYYYMMDD-HHMMSS-mmm.log", the name a log is kept
 * under. Checked exactly, so pruning deletes nothing else. */
bool is_kept_log_name(const wchar_t* name)
{
    static const wchar_t kShape[] = L"ecuio-surrogate.########-######-###.log";
    size_t i = 0;
    for (; kShape[i] != L'\0'; ++i) {
        const wchar_t c = name[i];
        if (kShape[i] == L'#' ? (c < L'0' || c > L'9') : (c != kShape[i])) {
            return false;
        }
    }
    return name[i] == L'\0';
}

/* Deletes the oldest kept logs past kKeptSessionLogs. The stamp sorts
 * by time, so the oldest is the smallest name. */
void prune_kept_logs(const wchar_t* prefix)
{
    wchar_t pattern[MAX_PATH + 64];
    swprintf(pattern, _countof(pattern), L"%lsecuio-surrogate.*.log", prefix);
    for (;;) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(pattern, &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        int count = 0;
        wchar_t oldest[MAX_PATH] = {};
        do {
            if (!is_kept_log_name(fd.cFileName)) continue;
            ++count;
            if (oldest[0] == L'\0' || wcscmp(fd.cFileName, oldest) < 0) {
                wcscpy_s(oldest, fd.cFileName);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        if (count <= kKeptSessionLogs) return;
        wchar_t victim[MAX_PATH + 64];
        swprintf(victim, _countof(victim), L"%ls%ls", prefix, oldest);
        if (!DeleteFileW(victim)) return;   /* held open: try again next spawn */
    }
}

/* Keeps the previous session's log: before this session takes the
 * fixed name, the file there is renamed after its last write (local
 * time). A log another session still holds can't be renamed; the open
 * that follows fails the same way and takes a per-spawn name. */
void keep_previous_log(const wchar_t* prefix)
{
    wchar_t fixed[MAX_PATH + 64];
    swprintf(fixed, _countof(fixed), L"%lsecuio-surrogate.log", prefix);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    SYSTEMTIME utc, t;
    if (!GetFileAttributesExW(fixed, GetFileExInfoStandard, &fa) ||
        !FileTimeToSystemTime(&fa.ftLastWriteTime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &t)) {
        return;
    }
    wchar_t kept[MAX_PATH + 64];
    swprintf(kept, _countof(kept),
             L"%lsecuio-surrogate.%04u%02u%02u-%02u%02u%02u-%03u.log", prefix,
             static_cast<unsigned>(t.wYear), static_cast<unsigned>(t.wMonth),
             static_cast<unsigned>(t.wDay), static_cast<unsigned>(t.wHour),
             static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond),
             static_cast<unsigned>(t.wMilliseconds));
    if (MoveFileExW(fixed, kept, 0)) {
        prune_kept_logs(prefix);
    }
}

/* The session log's path. The fixed name keeps the documented path for
 * the usual single session, and the log it replaces is kept
 * (keep_previous_log). A second concurrent session finds it held (a
 * sharing violation) and gets a per-spawn name, so its child never
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

    keep_previous_log(prefix);
    wchar_t wpath[MAX_PATH + 64];
    swprintf(wpath, _countof(wpath), L"%lsecuio-surrogate.log", prefix);
    HANDLE f = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        swprintf(wpath, _countof(wpath), L"%lsecuio-surrogate-%lu-%ld.log",
                 prefix, GetCurrentProcessId(), seq);
        f = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (f == INVALID_HANDLE_VALUE || !ecuio_wide_to_utf8(wpath, path, cap)) {
        path[0] = '\0';
    }
    return f;
}

/* The surrogate exe: its variable (ECUIO_SURROGATE_EXE or
 * ECUIO_SURROGATE_X64_EXE) when set, else the one next to the running
 * exe. The variable relocates the surrogate and nothing more: it must
 * be an absolute path to an existing file of that surrogate's name, so
 * it can't start some other program, and the path is always logged.
 * Either way the file must exist, so a missing copy says so instead of
 * failing CreateProcess. On failure, why says what was wrong. */
bool resolve_exe_path(const SurrogateExe& sur, wchar_t* out, size_t cap,
                      bool* from_env, char* why, size_t why_cap)
{
    *from_env = false;
    const DWORD n = GetEnvironmentVariableW(sur.env, out,
                                            static_cast<DWORD>(cap));
    if (n > 0) {
        *from_env = true;
        if (n >= cap) {
            snprintf(why, why_cap, "%s is too long", sur.env8);
            return false;
        }
        if (!ecuio_is_absolute_path(out)) {
            snprintf(why, why_cap, "%s is not an absolute path", sur.env8);
            return false;
        }
        /* An absolute path has a separator; the last one starts the
         * file name. */
        const wchar_t* sep = wcsrchr(out, L'\\');
        const wchar_t* fwd = wcsrchr(out, L'/');
        if (fwd != nullptr && (sep == nullptr || fwd > sep)) sep = fwd;
        if (_wcsicmp(sep + 1, sur.name) != 0) {
            snprintf(why, why_cap, "%s does not name %s", sur.env8, sur.name8);
            return false;
        }
        const DWORD attr = GetFileAttributesW(out);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            snprintf(why, why_cap, "%s names no file", sur.env8);
            return false;
        }
        return true;
    }
    wchar_t self[MAX_PATH] = {};
    const DWORD sn = GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* slash = wcsrchr(self, L'\\');
    if (sn == 0 || sn >= MAX_PATH || slash == nullptr) {
        snprintf(why, why_cap, "the app's own path is unavailable");
        return false;
    }
    const size_t dir = static_cast<size_t>(slash - self) + 1;
    if (dir + wcslen(sur.name) + 1 > cap) {
        snprintf(why, why_cap, "the app's path is too long");
        return false;
    }
    wmemcpy(out, self, dir);
    wcscpy_s(out + dir, cap - dir, sur.name);
    const DWORD attr = GetFileAttributesW(out);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        snprintf(why, why_cap, "none next to the app");
        return false;
    }
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
    int n = snprintf(out, cap, "\\\\.\\pipe\\ecuio-j2534-%lu-%ld-",
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

bool ov_write(HANDLE pipe, HANDLE event, const void* buf, DWORD len,
              DWORD timeout_ms)
{
    OVERLAPPED ov = {};
    ov.hEvent = event;
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
    return res;
}

enum read_result { RD_OK, RD_SHORT_READ, RD_TIMEOUT, RD_OS_ERROR };

/* One overlapped ReadFile of up to len bytes. *out_got is what it
 * read, a canceled read's included. */
read_result ov_read(HANDLE pipe, HANDLE event, void* buf, DWORD len,
                    DWORD timeout_ms, DWORD* out_got, DWORD* out_err)
{
    *out_got = 0;
    *out_err = 0;
    OVERLAPPED ov = {};
    ov.hEvent = event;
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
            *out_got = done;
            res = RD_TIMEOUT;
        } else {
            *out_err = GetLastError();
            res = RD_OS_ERROR;
        }
    } else {
        *out_err = GetLastError();
        res = RD_OS_ERROR;
    }
    return res;
}

/* Read exactly len bytes within timeout_ms. A byte-mode pipe may hand
 * a long reply over in pieces, so a short read reads on with what's
 * left of the time. RD_TIMEOUT means nothing arrived, so the reply may
 * still come whole later; a reply cut off part way is RD_SHORT_READ,
 * and the pipe is out of step after it. */
read_result read_exact(ecuio_j2534_sur_handle_t* h, void* buf, DWORD len,
                       DWORD timeout_ms, DWORD* out_got, DWORD* out_err)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DWORD total = 0;
    *out_got = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = (now < deadline)
                           ? static_cast<DWORD>(deadline - now) : 0;
        DWORD got = 0;
        const read_result rr = ov_read(h->pipe, h->io_event,
                                       static_cast<char*>(buf) + total,
                                       len - total, left, &got, out_err);
        total += got;
        *out_got = total;
        if (rr == RD_OK || rr == RD_OS_ERROR) {
            return rr;
        }
        if (total == 0 && rr == RD_TIMEOUT) {
            return RD_TIMEOUT;
        }
        if (got == 0 || left == 0) {
            return RD_SHORT_READ;
        }
    }
}

/* Read and drop the count messages after a reply nobody will use: a
 * late reply's. One at a time, so no batch-sized buffer is needed. */
bool discard_frames(ecuio_j2534_sur_handle_t* h, uint32_t count,
                    DWORD timeout_ms)
{
    ecuio_pt_rx_t scratch;
    for (uint32_t i = 0; i < count; ++i) {
        DWORD got = 0, rerr = 0;
        if (read_exact(h, &scratch, sizeof(scratch), timeout_ms, &got,
                       &rerr) != RD_OK) {
            return false;
        }
    }
    return true;
}

/* Read and discard the late reply to a previous timed-out request, so
 * the next request isn't paired with it (every later call would fail
 * the ID check). If that reply never arrives, the child is hung: mark
 * the handle unresponsive and fail without sending anything new.
 *
 * If the late reply was an OP_READ's, its frames are dropped with it.
 * That takes a read the child answered more than 500 ms past its own
 * timeout, so it's rare, but the drop is logged rather than silent. */
bool drain_pending(ecuio_j2534_sur_handle_t* h, DWORD timeout_ms)
{
    ecuio_sur_response_t stale = {};
    DWORD got = 0, rerr = 0;
    const read_result rr = read_exact(h, &stale, sizeof(stale), timeout_ms,
                                      &got, &rerr);
    if (rr != RD_OK) {
        SUR_LOG(h, "drain_pending: stale response id=%u never arrived; "
                "child unresponsive, later requests fail at once\n",
                h->pending_response_id);
        h->unresponsive = true;
        return false;
    }
    if (stale.num_frames > ECUIO_SUR_MAX_FRAMES_PER_READ ||
        !discard_frames(h, stale.num_frames, timeout_ms)) {
        SUR_LOG(h, "drain_pending: the late reply's %u frame(s) didn't "
                "arrive whole; child unresponsive\n", stale.num_frames);
        h->unresponsive = true;
        return false;
    }
    if (stale.request_id != h->pending_response_id) {
        SUR_LOG(h, "drain_pending: unexpected id=%u want=%u; the pipe is "
                "out of step, child unresponsive\n",
                stale.request_id, h->pending_response_id);
        h->unresponsive = true;
        return false;
    }
    if (stale.num_frames > 0) {
        SUR_LOG(h, "drain_pending: dropped %u frame(s) from the late reply "
                "to req_id=%u\n", stale.num_frames, stale.request_id);
    }
    return true;
}

/* One request and its reply. frames receives a READ's messages and
 * has room for ECUIO_SUR_MAX_FRAMES_PER_READ; every other opcode
 * passes none, so frames on its reply mean a skewed child. A reply
 * that arrives only in part, or claims more frames than fit, leaves
 * the pipe out of step, so the handle turns unresponsive. */
bool transact(ecuio_j2534_sur_handle_t* h, const ecuio_sur_request_t& req,
              ecuio_sur_response_t* out_resp, DWORD timeout_ms,
              ecuio_pt_rx_t* frames = nullptr)
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
    if (!ov_write(h->pipe, h->io_event, &req, sizeof(req), timeout_ms)) {
        SUR_LOG(h, "transact: ov_write failed (req_id=%u op=%u err=%lu)\n",
                req.request_id, req.opcode, GetLastError());
        return false;
    }
    DWORD got = 0, rerr = 0;
    const read_result rr = read_exact(h, out_resp, sizeof(*out_resp),
                                      timeout_ms, &got, &rerr);
    if (rr == RD_TIMEOUT) h->pending_response_id = req.request_id;
    if (rr == RD_SHORT_READ) h->unresponsive = true;
    if (rr != RD_OK) {
        const char* kind =
            (rr == RD_SHORT_READ) ? "SHORT-READ"
          : (rr == RD_TIMEOUT)    ? "TIMEOUT"
          :                         "OS-ERROR";
        SUR_LOG(h, "transact: ov_read failed (req_id=%u op=%u %s got=%lu/%zu err=%lu)\n",
                req.request_id, req.opcode, kind, got, sizeof(*out_resp), rerr);
        return false;
    }
    if (out_resp->num_frames > 0) {
        /* Trust boundary: a skewed stale child can send garbage. */
        if (frames == nullptr ||
            out_resp->num_frames > ECUIO_SUR_MAX_FRAMES_PER_READ) {
            SUR_LOG(h, "transact: %u frame(s) on the reply to op=%u "
                    "(skew?); child unresponsive\n",
                    out_resp->num_frames, req.opcode);
            h->unresponsive = true;
            return false;
        }
        const DWORD want = static_cast<DWORD>(
            out_resp->num_frames * sizeof(ecuio_pt_rx_t));
        if (read_exact(h, frames, want, timeout_ms, &got, &rerr) != RD_OK) {
            SUR_LOG(h, "transact: the reply's frames didn't arrive whole "
                    "(req_id=%u got=%lu/%lu err=%lu); child unresponsive\n",
                    req.request_id, got, want, rerr);
            h->unresponsive = true;
            return false;
        }
    }
    if (out_resp->request_id != req.request_id) {
        /* The child answers one reply per request, so this request's is
         * still in the pipe and every later one would read the reply
         * before its own: out of step for good, like a short read. */
        SUR_LOG(h, "transact: DESYNC — got req_id=%u want=%u; child "
                "unresponsive\n", out_resp->request_id, req.request_id);
        h->unresponsive = true;
        return false;
    }
    return true;
}

/* Decode the child's exit code (j2534.md "Child exit codes") so a spawn
 * failure names its stage in the session log. Returns the code, or
 * STILL_ACTIVE while the child runs (it then logs nothing). */
DWORD log_child_exit(const ecuio_j2534_sur_handle_t* h)
{
    DWORD code = STILL_ACTIVE;
    if (h->process == nullptr || !GetExitCodeProcess(h->process, &code)) {
        return STILL_ACTIVE;
    }
    if (code == STILL_ACTIVE) return code;
    const char* stage =
        (code == 1) ? "bad argument count"
      : (code == 4) ? "LoadLibraryEx of the vendor DLL failed"
      : (code == 5) ? "a mandatory export is missing (named in the child log)"
      : (code == 7) ? "could not connect to the app's pipe"
      :               "unknown";
    SUR_LOG(h, "open: child exited with code %lu (%s)\n", code, stage);
    return code;
}

/* A child that exited during startup: a DLL that won't load is the
 * DLL's fault (LOAD_FAILED); anything else is the surrogate's. */
ecuio_status_t child_exit_failed(DWORD code)
{
    switch (code) {
    case 4:
        ecuio_detail_set("the surrogate couldn't load the DLL (the Windows "
                         "error is in its session log, in %%TEMP%%)");
        return ECUIO_ERR_LOAD_FAILED;
    case 5:
        ecuio_detail_set("the DLL lacks a mandatory J2534 export (the "
                         "surrogate's session log, in %%TEMP%%, names it)");
        return ECUIO_ERR_LOAD_FAILED;
    case 7:
        ecuio_detail_set("the surrogate couldn't connect to the app's pipe");
        return ECUIO_ERR_SURROGATE_FAILED;
    default:
        ecuio_detail_set("the surrogate exited during startup with code %lu",
                         code);
        return ECUIO_ERR_SURROGATE_FAILED;
    }
}

/* A surrogate that couldn't be started; the session log has the same
 * stage and error. */
ecuio_status_t spawn_failed(const char* stage, DWORD err)
{
    if (err != 0) {
        ecuio_detail_set("couldn't start the surrogate: %s (Windows error %lu)",
                         stage, err);
    } else {
        ecuio_detail_set("couldn't start the surrogate: %s", stage);
    }
    return ECUIO_ERR_SURROGATE_FAILED;
}

/* The vendor's description of its last failed call, through
 * OP_GET_LAST_ERROR, into out (ECUIO_PT_TEXT_LEN + 1 bytes); "" when
 * the DLL exports no PassThruGetLastError or the child can't answer. */
void fetch_vendor_text(ecuio_j2534_sur_handle_t* h, char* out)
{
    out[0] = '\0';
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_GET_LAST_ERROR;
    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs) ||
        resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return;
    }
    resp.text[0][ECUIO_SUR_TEXT_MAX - 1] = '\0';   /* untrusted pipe */
    memcpy(out, resp.text[0], ECUIO_SUR_TEXT_MAX);
    out[ECUIO_PT_TEXT_LEN] = '\0';
}

} // namespace

extern "C" {

ecuio_status_t ecuio_j2534_sur_open(const char* dll_path, uint32_t dll_arch,
                                    ecuio_j2534_sur_handle_t** out_handle)
{
    if (dll_path == nullptr || out_handle == nullptr || dll_path[0] == '\0') {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_handle = nullptr;

    /* The DLL path reaches the child as UTF-16 on its command line. */
    wchar_t wdll[ECUIO_J2534_DLL_PATH_LEN];
    if (!ecuio_utf8_to_wide(dll_path, wdll, _countof(wdll))) {
        SUR_LOG(nullptr, "open: dll path is not UTF-8 or too long\n");
        ecuio_detail_set("the DLL path isn't valid UTF-8, or is too long");
        return ECUIO_ERR_LOAD_FAILED;
    }

    static volatile LONG counter = 0;
    const LONG seq = InterlockedIncrement(&counter);

    ecuio_j2534_sur_handle_t* h = new (std::nothrow) ecuio_j2534_sur_handle_t();
    if (h == nullptr) return ECUIO_ERR_OUT_OF_MEMORY;

    /* The log opens first, so every startup failure below lands in it.
     * Inheritable only until the spawn: the child gets it as its
     * stdout/stderr, and the client keeps its own copy for SUR_LOG. */
    SECURITY_ATTRIBUTES inherit_sa = { sizeof(inherit_sa), nullptr, TRUE };
    h->log = open_session_log(seq, &inherit_sa, h->log_path, sizeof(h->log_path));
    const DWORD log_err = (h->log == INVALID_HANDLE_VALUE) ? GetLastError() : 0;

    const SurrogateExe& sur = surrogate_for(dll_arch);
    wchar_t exe[MAX_PATH] = {};
    bool exe_from_env = false;
    char exe_why[160] = {};
    const bool exe_ok = resolve_exe_path(sur, exe, _countof(exe), &exe_from_env,
                                         exe_why, sizeof(exe_why));
    char exe8[3 * MAX_PATH];
    (void)ecuio_wide_to_utf8(exe, exe8, sizeof(exe8));
    if (!exe_ok) {
        SUR_LOG(h, "open: no surrogate exe: %s ('%s')\n", exe_why, exe8);
        ecuio_j2534_sur_close(h);
        ecuio_detail_set("no surrogate (%s): %s", sur.name8, exe_why);
        return ECUIO_ERR_SURROGATE_FAILED;
    }

    /* Local, user-only, unguessable, and refused if the name already
     * exists, so no other process can sit on the child's end. */
    char pipe_name[128];
    wchar_t wpipe[128];
    user_only_security sec = {};
    if (!make_pipe_name(seq, pipe_name, sizeof(pipe_name)) ||
        !ecuio_utf8_to_wide(pipe_name, wpipe, _countof(wpipe)) ||
        !init_user_only(&sec)) {
        const DWORD err = GetLastError();
        SUR_LOG(h, "open: pipe setup failed (err=%lu)\n", err);
        ecuio_j2534_sur_close(h);
        return spawn_failed("pipe setup", err);
    }
    h->pipe = CreateNamedPipeW(wpipe,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                   FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                   PIPE_REJECT_REMOTE_CLIENTS,
                               1, sizeof(ecuio_sur_request_t),
                               sizeof(ecuio_sur_response_t) +
                                   ECUIO_SUR_MAX_FRAMES_PER_READ *
                                       sizeof(ecuio_pt_rx_t),
                               0, &sec.sa);
    if (h->pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        SUR_LOG(h, "open: CreateNamedPipe failed (err=%lu)\n", err);
        ecuio_j2534_sur_close(h);
        return spawn_failed("CreateNamedPipe", err);
    }
    h->io_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (h->io_event == nullptr) {
        const DWORD err = GetLastError();
        ecuio_j2534_sur_close(h);
        return spawn_failed("CreateEvent", err);
    }

    h->job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji = {};
    ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (h->job == nullptr ||
        !SetInformationJobObject(h->job, JobObjectExtendedLimitInformation,
                                 &ji, sizeof(ji))) {
        const DWORD err = GetLastError();
        SUR_LOG(h, "open: job object setup failed (err=%lu)\n", err);
        ecuio_j2534_sur_close(h);
        return spawn_failed("job object setup", err);
    }

    wchar_t cmdline[MAX_PATH * 3 + 256];
    if (swprintf(cmdline, _countof(cmdline), L"\"%ls\" \"%ls\" \"%ls\"",
                 exe, wdll, wpipe) < 0) {
        ecuio_j2534_sur_close(h);
        return spawn_failed("the command line is too long", 0);
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
            ecuio_j2534_sur_close(h);
            return spawn_failed("attribute list", 0);
        }
        if (!UpdateProcThreadAttribute(attrs, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       static_cast<void*>(&h->log),
                                       sizeof(h->log), nullptr, nullptr)) {
            const DWORD err = GetLastError();
            DeleteProcThreadAttributeList(attrs);
            SUR_LOG(h, "open: handle list failed (err=%lu)\n", err);
            ecuio_j2534_sur_close(h);
            return spawn_failed("handle list", err);
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
        ecuio_j2534_sur_close(h);
        return spawn_failed("CreateProcess", spawn_err);
    }
    if (h->log != INVALID_HANDLE_VALUE) {
        /* No later spawn, by this client or anyone in the app, inherits
         * this session's log. */
        SetHandleInformation(h->log, HANDLE_FLAG_INHERIT, 0);
    }
    /* Logged while the child is still suspended, so it heads the
     * session log instead of racing the child's first line. */
    SUR_LOG(h, "open: child pid %lu, surrogate '%s'%s%s%s, dll '%s'\n",
            pi.dwProcessId, exe8, exe_from_env ? " (" : "",
            exe_from_env ? sur.env8 : "", exe_from_env ? ")" : "", dll_path);
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
        const DWORD err = GetLastError();
        SUR_LOG(h, "open: AssignProcessToJobObject failed (err=%lu)\n", err);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        ecuio_j2534_sur_close(h);
        return spawn_failed("AssignProcessToJobObject", err);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    OVERLAPPED ov = {};
    ov.hEvent = h->io_event;
    BOOL conn = ConnectNamedPipe(h->pipe, &ov);
    DWORD cerr = GetLastError();
    bool connected = false;
    DWORD exit_code = STILL_ACTIVE;
    bool handshake_timed_out = false;
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
            exit_code = log_child_exit(h);
        } else {
            handshake_timed_out = true;
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
    if (!connected) {
        SUR_LOG(h, "open: handshake failed — child never connected\n");
        ecuio_j2534_sur_close(h);
        if (exit_code != STILL_ACTIVE) {
            return child_exit_failed(exit_code);
        }
        if (handshake_timed_out) {
            ecuio_detail_set("the surrogate didn't start within %lu ms",
                             handshake_timeout_ms());
            return ECUIO_ERR_SURROGATE_FAILED;
        }
        return spawn_failed("the pipe handshake", cerr);
    }

    h->pipe_connected = true;

    /* OP_OPEN: the child connects the pipe before it loads the vendor
     * DLL, so this transaction covers LoadLibraryEx (the vendor's
     * DllMain), export resolution and PassThruOpen. A load or export
     * failure exits the child (codes 4/5, decoded below); a refusal is
     * a non-success status; no reply with a live child is a vendor
     * blocked in DllMain or PassThruOpen, typically on absent
     * hardware. It carries this build's fingerprint of the shared
     * headers; the child refuses one not its own. */
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_OPEN;
    req.u.open.fingerprint = ECUIO_SUR_FINGERPRINT;
    ecuio_sur_response_t resp = {};
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
        bool vendor_blocked = false;
        if (timed_out && WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0) {
            SUR_LOG(h, "open: no reply to OP_OPEN after %lu ms, child alive "
                    "(vendor blocked in DllMain or PassThruOpen)\n",
                    handshake_timeout_ms());
            vendor_blocked = true;
        } else {
            WaitForSingleObject(h->process, 1000);
        }
        const DWORD code = log_child_exit(h);
        /* Wait out the kill as well, so close sees a dead child and
         * skips the polite OP_SHUTDOWN it could never answer. */
        TerminateProcess(h->process, 1);
        WaitForSingleObject(h->process, 1000);
        ecuio_j2534_sur_close(h);
        if (vendor_blocked) {
            ecuio_detail_set("PassThruOpen didn't return within %lu ms; the "
                             "driver may be waiting for its adapter",
                             handshake_timeout_ms());
            return ECUIO_ERR_VENDOR_REFUSED;
        }
        if (code != STILL_ACTIVE) {
            return child_exit_failed(code);
        }
        ecuio_detail_set("the surrogate stopped answering during PassThruOpen");
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    /* Checked here too, before the status: a child older than the
     * check echoes nothing and opens the device anyway, and the close
     * below shuts that down. */
    if (resp.aux != ECUIO_SUR_FINGERPRINT) {
        SUR_LOG(h, "open: the surrogate was built from other shared headers "
                "(surrogate %08x, client %08x): rebuild build-win32, then build\n",
                static_cast<unsigned>(resp.aux),
                static_cast<unsigned>(ECUIO_SUR_FINGERPRINT));
        ecuio_j2534_sur_close(h);
        ecuio_detail_set("the surrogate was built from other shared headers: "
                         "rebuild build-win32, then build");
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        char text[ECUIO_PT_TEXT_LEN + 1];
        fetch_vendor_text(h, text);
        SUR_LOG(h, "open: PassThruOpen refused (vendor status %d)%s%s\n",
                resp.status, text[0] != '\0' ? ": " : "", text);
        ecuio_detail_set_vendor("PassThruOpen", resp.status, text);
        ecuio_j2534_sur_close(h);
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    *out_handle = h;
    return ECUIO_OK;
}

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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) return ECUIO_ERR_SURROGATE_FAILED;
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
    if (len == 0 || len > ECUIO_SUR_MSG_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (out_num_written != nullptr) *out_num_written = 0;

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_WRITE;
    req.u.write.channel_id = channel;
    memcpy(req.u.write.data, data, len);
    req.u.write.data_len = len;
    req.u.write.tx_flags = tx_flags;
    req.u.write.timeout_ms = timeout_ms;

    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    /* Trust boundary, as for read's num_frames: the pipe is untrusted,
     * and a reply could claim more than resp.data holds. */
    if (resp.data_len > ECUIO_SUR_IOCTL_DATA_MAX) {
        SUR_LOG(h, "ioctl: data_len=%u exceeds the wire limit (skew?)\n",
                resp.data_len);
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    /* The caller's capacity bounds the copy; out_len keeps the full
     * length, so the caller spots a cut. */
    if (output != nullptr && resp.data_len > 0) {
        memcpy(output, resp.data,
               (resp.data_len < output_cap) ? resp.data_len : output_cap);
    }
    if (out_len != nullptr) *out_len = resp.data_len;
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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (resp.status == static_cast<int32_t>(ECUIO_PT_STATUS_NOT_SUPPORTED)) {
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    for (int i = 0; i < ECUIO_SUR_TEXT_FIELDS; ++i) {
        resp.text[i][ECUIO_SUR_TEXT_MAX - 1] = '\0';   /* untrusted pipe */
        memcpy(out[i], resp.text[i], ECUIO_SUR_TEXT_MAX);
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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
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
    *got_msg = false;

    /* Dispense from the cache first — no pipe traffic while a batch
     * is partially consumed. */
    if (h->pending_next < h->pending_count) {
        *out_msg = h->pending[h->pending_next++];
        if (h->overflow_pending) {
            out_msg->rx_status |= ECUIO_PT_BUFFER_OVERFLOW;
            h->overflow_pending = false;
        }
        *got_msg = true;
        return ECUIO_OK;
    }

    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_READ;
    req.u.read.timeout_ms = timeout_ms;

    ecuio_sur_response_t resp = {};
    /* Margin over the vendor timeout: the child spends up to
     * timeout_ms inside ReadMsgs before answering. Transact timing
     * out before the child does means the child is hung, not busy. */
    const DWORD tmo = timeout_ms + 500;
    if (!transact(h, req, &resp, tmo, h->pending)) {
        return ECUIO_ERR_TRANSPORT_HARD_FAILURE;    /* child dead/wedged/skewed */
    }

    /* Deliver first, judge the status second. Per J2534-1 a read
     * asking for N messages with a nonzero timeout returns ERR_TIMEOUT
     * TOGETHER WITH however many arrived, so a conforming vendor reports
     * nearly every response that way. Dropping such a message would
     * hide the ECU's answer and leave a
     * healthy session showing nothing at all. The child has already
     * applied the count-trust rule (ecuio_trusted_count), so a
     * non-empty batch only arrives under NOERROR, ERR_TIMEOUT or
     * ERR_BUFFER_OVERFLOW, whose loss the child marked on a frame. */
    if (resp.num_frames > 0) {
        h->pending_count = resp.num_frames;
        h->pending_next = 0;
        /* Recurse once into the cache-dispense path above. */
        return ecuio_j2534_sur_read_msg(h, channel, out_msg, timeout_ms,
                                        got_msg);
    }

    /* Empty batch: the status decides. ERR_BUFFER_OVERFLOW with nothing
     * after it: frames were lost and the adapter reads on, so the next
     * message delivered carries the bit, and the link is as it was. */
    if (resp.status == static_cast<int32_t>(ECUIO_PT_STATUS_BUFFER_OVERFLOW)) {
        h->overflow_pending = true;
        return ECUIO_OK;
    }
    if (resp.status == static_cast<int32_t>(ECUIO_PT_STATUS_TIMEOUT)) {
        return ECUIO_ERR_VEHICLE_TIMEOUT;
    }
    if (resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        if (resp.status == static_cast<int32_t>(ECUIO_PT_STATUS_BUFFER_EMPTY)) {
            return ECUIO_OK;    /* queue drained — quiet, not failed */
        }
        return ECUIO_ERR_TRANSPORT_HARD_FAILURE;
    }
    return ECUIO_OK;    /* NOERROR, nothing available */
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
    if (!transact(h, req, &resp, kTransactTimeoutMs)) {
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
    if (h->pipe_connected && h->pipe != INVALID_HANDLE_VALUE &&
        h->process != nullptr &&
        WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0) {
        ecuio_sur_request_t req = {};
        req.request_id = h->next_request_id++;
        req.opcode = ECUIO_SUR_OP_SHUTDOWN;
        ecuio_sur_response_t resp = {};
        (void)transact(h, req, &resp, 1500);   /* fails at once if unresponsive */
    }
    /* An unresponsive child won't exit on its own: closing the job
     * below kills it, so don't wait for it first. */
    if (h->process != nullptr && !h->unresponsive) {
        WaitForSingleObject(h->process, 1500);
    }
    if (h->pipe != INVALID_HANDLE_VALUE) CloseHandle(h->pipe);
    if (h->io_event != nullptr) CloseHandle(h->io_event);
    if (h->process != nullptr) CloseHandle(h->process);
    if (h->job != nullptr) CloseHandle(h->job);
    if (h->log != INVALID_HANDLE_VALUE) CloseHandle(h->log);
    delete h;
    return ECUIO_OK;
}

const char* ecuio_j2534_sur_log_path(const ecuio_j2534_sur_handle_t* h)
{
    return (h != nullptr) ? h->log_path : "";
}

} // extern "C"

#else

extern "C" {

ecuio_status_t ecuio_j2534_sur_open(const char*, uint32_t,
                                    ecuio_j2534_sur_handle_t**)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }
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

} // extern "C"

#endif /* _WIN32 */
