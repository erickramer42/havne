// File/layer: core/src/j2534_surrogate_client.cpp | C++17, Qt-free, Win32-guarded:
//             the surrogate client: ecuio_j2534_sur_open
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
using ecuio_sur_client::transact;

namespace {

/* One surrogate per DLL architecture (ADR-0025): x86, and x64, which also
 * hosts a PE for any other machine so Windows answers it as the app's own
 * process would. Each has its own relocation variable. */
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
/* The child's startup, from resuming it to its pipe connecting: fixed,
 * so a test that shrinks the open's window can't fail on a slow start */
constexpr DWORD kStartupTimeoutMs = 8000;
constexpr DWORD kOpenTimeoutMs_default = 8000;

/* The OP_OPEN reply's window: the vendor's DllMain and PassThruOpen */
DWORD open_timeout_ms()
{
    char buf[32] = {};
    const DWORD n =
        GetEnvironmentVariableA("ECUIO_SUR_OPEN_MS", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        const DWORD v = static_cast<DWORD>(strtoul(buf, nullptr, 10));
        if (v >= 100) return v;
    }
    return kOpenTimeoutMs_default;
}

/* The surrogate exe: its variable (ECUIO_SURROGATE_EXE or
 * ECUIO_SURROGATE_X64_EXE) when set, else the one next to the running exe.
 * The variable only relocates: an absolute path to an existing file of that
 * surrogate's name, so it can't start another program, and the path is
 * always logged. The file must exist either way, so a missing copy says
 * so. On failure, why says what was wrong. */
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
        /* The last separator starts the file name. */
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

/* A pipe name no other process can predict (FILE_FLAG_FIRST_PIPE_INSTANCE
 * would make a squatter fail the spawn). The pid and sequence stay in the
 * name for whoever reads a log. */
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

/* Decode the child's exit code (surrogate.md "Child exit codes") so a spawn
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
        ecuio_detail_set("the DLL path isn't valid UTF-8, or is too long");
        return ECUIO_ERR_LOAD_FAILED;
    }

    static volatile LONG counter = 0;
    const LONG seq = InterlockedIncrement(&counter);

    ecuio_j2534_sur_handle_t* h = new (std::nothrow) ecuio_j2534_sur_handle_t();
    if (h == nullptr) return ECUIO_ERR_OUT_OF_MEMORY;

    /* The log opens first, so every startup failure lands in it. Inheritable
     * only until the spawn: the child gets it as stdout/stderr, and the
     * client keeps its own copy for SUR_LOG. */
    SECURITY_ATTRIBUTES inherit_sa = { sizeof(inherit_sa), nullptr, TRUE };
    bool log_out_of_memory = false;
    h->log = ecuio_sur_log::open_session_log(
        seq, &inherit_sa, h->log_path, sizeof(h->log_path), &log_out_of_memory);
    const DWORD log_err = (h->log == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    if (log_out_of_memory) {
        ecuio_j2534_sur_close(h);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

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

    /* Unguessable, then local, user-only and first instance only
     * (ecuio_j2534_sur_create_pipe): no other process can sit on the
     * child's end. */
    char pipe_name[128];
    wchar_t wpipe[128];
    if (!make_pipe_name(seq, pipe_name, sizeof(pipe_name)) ||
        !ecuio_utf8_to_wide(pipe_name, wpipe, _countof(wpipe))) {
        const DWORD err = GetLastError();
        SUR_LOG(h, "open: pipe setup failed (err=%lu)\n", err);
        ecuio_j2534_sur_close(h);
        return spawn_failed("pipe setup", err);
    }
    void* pipe = nullptr;
    uint32_t pipe_err = 0;
    if (ecuio_j2534_sur_create_pipe(pipe_name, &pipe, &pipe_err) != ECUIO_OK) {
        const DWORD err = pipe_err;
        SUR_LOG(h, "open: CreateNamedPipe failed (err=%lu)\n", err);
        ecuio_j2534_sur_close(h);
        return spawn_failed("CreateNamedPipe", err);
    }
    h->pipe = static_cast<HANDLE>(pipe);
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

    /* Only the log reaches the child: the handle list narrows inheritance,
     * which alone would pass every inheritable handle in the app. */
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
    /* The exe is named as application and in the command line, so
     * CreateProcess never searches for it. */
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
        /* No later spawn in the app inherits this session's log. */
        SetHandleInformation(h->log, HANDLE_FLAG_INHERIT, 0);
    }
    /* Logged while the child is suspended, so it heads the session log. */
    SUR_LOG(h, "open: child pid %lu, surrogate '%s'%s%s%s, dll '%s'\n",
            pi.dwProcessId, exe8, exe_from_env ? " (" : "",
            exe_from_env ? sur.env8 : "", exe_from_env ? ")" : "", dll_path);
    if (h->log_path[0] != '\0') {
        SUR_LOG(h, "open: log '%s'\n", h->log_path);
    } else {
        SUR_LOG(h, "open: no log (err=%lu)\n", log_err);
    }

    h->process = pi.hProcess;
    /* Kill-on-close is a guarantee: without the job a wedged child outlives
     * the app, so fail here. The child is still suspended, so terminating
     * it is clean. */
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
    /* A log ending here waited on the handshake; one ending above never
     * got this far. */
    const DWORD startup_ms = kStartupTimeoutMs;
    SUR_LOG(h, "open: child resumed, waiting up to %lu ms for its pipe\n",
            startup_ms);

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
        const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, startup_ms);
        if (wr == WAIT_OBJECT_0) {
            DWORD xfer = 0;
            connected = GetOverlappedResult(h->pipe, &ov, &xfer, FALSE);
        } else if (wr == WAIT_OBJECT_0 + 1) {
            exit_code = log_child_exit(h);
        } else {
            handshake_timed_out = true;
            SUR_LOG(h,
                    "open: handshake timeout after %lu ms; child alive but "
                    "never opened the pipe (process startup stalled)\n",
                    startup_ms);
        }
    }

    /* A failed handshake can leave ConnectNamedPipe pending: cancel and drain
     * it before releasing the event and pipe, or the kernel completes it into
     * freed structures. CancelIoEx on a completed request just reports
     * NOT_FOUND. */
    if (!connected) {
        CancelIoEx(h->pipe, &ov);
        DWORD drained = 0;
        (void)GetOverlappedResult(h->pipe, &ov, &drained, TRUE);
    }
    if (!connected) {
        SUR_LOG(h, "open: handshake failed, child never connected\n");
        ecuio_j2534_sur_close(h);
        if (exit_code != STILL_ACTIVE) {
            return child_exit_failed(exit_code);
        }
        if (handshake_timed_out) {
            ecuio_detail_set("the surrogate didn't start within %lu ms",
                             startup_ms);
            return ECUIO_ERR_SURROGATE_FAILED;
        }
        return spawn_failed("the pipe handshake", cerr);
    }

    h->pipe_connected = true;

    /* OP_OPEN covers LoadLibraryEx (the vendor's DllMain), export resolution
     * and PassThruOpen: the child connects the pipe before loading. A load or
     * export failure exits the child (codes 4/5); a refusal is a non-success
     * status; no reply from a live child is a vendor blocked, typically on
     * absent hardware. It carries this build's fingerprint of the shared
     * headers; the child refuses one not its own. */
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_OPEN;
    req.u.open.fingerprint = ECUIO_SUR_FINGERPRINT;
    ecuio_sur_response_t resp = {};
    const DWORD open_ms = open_timeout_ms();
    if (!transact(h, req, &resp, open_ms)) {
        /* Classify by why the transaction failed, not by liveness: a timeout
         * with the child alive is a wedged vendor, but a broken pipe is the
         * child exiting with code 4 or 5 (the pipe closes before the process
         * handle signals), so wait for the exit and let log_child_exit name
         * the stage. */
        const bool timed_out = (h->pending_response_id == req.request_id);
        bool vendor_blocked = false;
        if (timed_out && WaitForSingleObject(h->process, 0) != WAIT_OBJECT_0) {
            SUR_LOG(h,
                    "open: no reply to OP_OPEN after %lu ms, child alive "
                    "(vendor blocked in DllMain or PassThruOpen)\n",
                    open_ms);
            vendor_blocked = true;
        } else {
            WaitForSingleObject(h->process, 1000);
        }
        const DWORD code = log_child_exit(h);
        /* Wait out the kill so close skips the OP_SHUTDOWN it could never
         * get answered. */
        TerminateProcess(h->process, 1);
        WaitForSingleObject(h->process, 1000);
        ecuio_j2534_sur_close(h);
        if (vendor_blocked) {
            ecuio_detail_set("PassThruOpen didn't return within %lu ms; the "
                             "driver may be waiting for its adapter",
                             open_ms);
            return ECUIO_ERR_VENDOR_REFUSED;
        }
        if (code != STILL_ACTIVE) {
            return child_exit_failed(code);
        }
        ecuio_detail_set("the surrogate stopped answering during PassThruOpen");
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    /* Checked before the status: a child older than the check echoes nothing
     * and opens the device anyway; the close shuts that down. */
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

} // extern "C"

#else

extern "C" {

ecuio_status_t ecuio_j2534_sur_open(const char*, uint32_t,
                                    ecuio_j2534_sur_handle_t**)
{ return ECUIO_ERR_NOT_IMPLEMENTED; }

} // extern "C"

#endif /* _WIN32 */
