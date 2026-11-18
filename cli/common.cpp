// File/layer: cli/common.cpp | havne-cli, what every command shares: the
//             interrupt, the lines every command writes, the file calls
// Testing: UNIT: CliGoldenTest, CliUsageTest, CliExitTest, CliPathTest,
//          CliTransferRunTest (ctest); CliPartialTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "jsonl.h"
#include "log_lines.h"
#include "partial_file.h"

#include "common.h"

#include "ecuio/core.h"

namespace cmd {

// A test knob in milliseconds: the environment's value when it is 100
// or more, else the fallback
unsigned long env_ms(const char* name, unsigned long fallback)
{
    char env[32] = {};
#ifdef _WIN32
    const DWORD n = GetEnvironmentVariableA(name, env, sizeof env);
    if (n == 0 || n >= sizeof env) {
        return fallback;
    }
#else
    const char* value = std::getenv(name);
    if (!value || std::strlen(value) >= sizeof env) {
        return fallback;
    }
    std::memcpy(env, value, std::strlen(value));
#endif
    const unsigned long v = std::strtoul(env, nullptr, 10);
    return v >= 100 ? v : fallback;
}

std::atomic<bool> g_interrupted{false};

namespace {

// Set by install_handlers(true): a second interrupt ends the process at
// once, for the commands whose stop has nothing to close (export, import,
// a recording's health report), never `log`'s recorder
std::atomic<bool> g_second_ends{false};

// What a second interrupt writes, whole and flushed before the exit
constexpr char kSecondEnd[] = "{\"type\":\"end\",\"reason\":\"interrupted\"}\n";

} // namespace

#ifdef _WIN32
std::atomic<bool> g_done{false};

namespace {

BOOL WINAPI on_console_event(DWORD event)
{
    const bool again = g_interrupted.exchange(true);
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        if (again && g_second_ends.load()) {
            DWORD wrote = 0;
            (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), kSecondEnd,
                            static_cast<DWORD>(sizeof kSecondEnd - 1), &wrote,
                            nullptr);
            TerminateProcess(GetCurrentProcess(), 1);
        }
        return TRUE;
    }
    // A closed console ends the process when this returns: give the
    // main thread its close first (Windows allows about 5 s)
    for (int i = 0; i < 40 && !g_done.load(); ++i) {
        Sleep(100);
    }
    return TRUE;
}

} // namespace
#else

namespace {

void on_signal(int)
{
    if (g_interrupted.exchange(true) && g_second_ends.load()) {
        // write and _exit are async-signal-safe; stdio's buffer is not
        // flushed, which the commands that set this keep empty
        const ssize_t wrote = write(1, kSecondEnd, sizeof kSecondEnd - 1);
        (void)wrote;
        _exit(1);
    }
}

} // namespace
#endif

// A second interrupt ends nothing from here on: called once the core call
// it could stop has returned, before any result, error or end line.
// HAVNE_CLI_EXPORT_TAIL_HOLD_MS (tests only) then waits, said on stderr
void second_interrupt_over(const char* where)
{
    g_second_ends.store(false);
    const unsigned long hold_ms = env_ms("HAVNE_CLI_EXPORT_TAIL_HOLD_MS", 0);
    if (where != nullptr && hold_ms > 0) {
        std::fprintf(stderr, "havne-cli: holding after the %s's call\n", where);
        std::fflush(stderr);
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }
}

// secondEnds: a second interrupt ends the process at once, with the end
// line and exit 1, for a command whose stop leaves nothing to close
void install_handlers(bool secondEnds)
{
    g_second_ends.store(secondEnds);
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // A closed pipe is a failed write (EPIPE), which ends the stream
    signal(SIGPIPE, SIG_IGN);
#endif
}

void write_line(Output& out, cli::Line& l)
{
    if (l.finish()) {
        out.write(l.data(), l.size());
    }
}

// A command's last word: its output flushed, then its exit, or 1 when a
// write failed (a closed pipe stays quiet, as ok() says). Every return
// after the output began goes through it, so a write that fails only at
// the flush is never a quiet 0
int ended(Output& out, int code)
{
    out.flush();
    return out.ok() ? code : kExitFailed;
}

namespace {

// export's cancel (ecuio_export_with), its column check's, import's
// (ecuio_import_with), a recording's health report's
// (ecuio_health_export_with) and view's searches': Ctrl+C or SIGTERM
// stops it, and core removes what it had begun. HAVNE_CLI_EXPORT_HOLD_MS
// (tests only) holds the run's first ask that long, naming whose it is on
// stderr, so a test's interrupt lands there; with HAVNE_CLI_EXPORT_HOLD_DEAF
// set the hold ignores the interrupt, so a second one can arrive while the
// run waits
int interrupted_at(bool* asked, const char* where)
{
    if (!*asked) {
        *asked = true;
        const uint64_t hold_ms = env_ms("HAVNE_CLI_EXPORT_HOLD_MS", 0);
        if (hold_ms > 0) {
            std::fprintf(stderr, "havne-cli: holding the %s's first ask\n",
                         where);
            std::fflush(stderr);
        }
        const uint64_t until = ecuio_clock_us() + hold_ms * 1000ULL;
        const bool deaf = env_ms("HAVNE_CLI_EXPORT_HOLD_DEAF", 0) > 0;
        while ((deaf || !g_interrupted.load()) && ecuio_clock_us() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return g_interrupted.load() ? 1 : 0;
}

} // namespace

int export_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "export");
}

int import_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "import");
}

int health_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "health report");
}

// The sweep's sizing before an export at an interval asks as the export
// does, its first ask held under its own name
int interval_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "interval check");
}

// The column check's open (ecuio_open_progress_t) asks as the export does
int open_interrupted(uint64_t, uint64_t, void* user)
{
    return interrupted_at(static_cast<bool*>(user), "column check");
}

// view's searches (ecuio_recording_find_with, _stretches_with) ask as the
// export does
int view_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "view");
}

// The second call of a search's pair (the stretches a count asked for):
// its first ask held under its own name, so a test's interrupt lands there
int view_second_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "view's second search");
}

// view's opens stop at an interrupt, with no hold
int view_open_interrupted(uint64_t, uint64_t, void*)
{
    return g_interrupted.load() ? 1 : 0;
}

// An export, its sizing, an import or a recording's health report stopped
// by an interrupt: the end line the other commands write on one, and
// exit 1, as clear's, since nothing was written. `where` names the caller
// for HAVNE_CLI_EXPORT_TAIL_HOLD_MS's hold (the column check)
int ended_interrupted(Output& out, const char* where)
{
    second_interrupt_over(where); // one end line, whatever interrupt follows
    char buf[64];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string("interrupted", SIZE_MAX);
    l.ch('}');
    write_line(out, l);
    return ended(out, kExitFailed);
}

void write_start(Output& out)
{
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"start\"");
    l.key("schema");
    l.u64(kSchema);
    l.key("core");
    l.string(ecuio_version_string(), SIZE_MAX);
    l.ch('}');
    write_line(out, l);
}

void write_error(Output& out, ecuio_status_t status, const char* detail)
{
    if (detail == nullptr) {
        detail = ecuio_last_error_detail();
    }
    BigLine b(1024 + std::strlen(detail) * 6);
    cli::Line& l = b.line;
    l.raw("{\"type\":\"error\"");
    l.key("code");
    l.u64(static_cast<uint32_t>(status));
    l.key("message");
    l.string(ecuio_status_string(status), SIZE_MAX);
    l.key("detail");
    l.string(detail, SIZE_MAX);
    l.ch('}');
    write_line(out, l);
    std::fprintf(stderr, "havne-cli: %s%s%s\n", ecuio_status_string(status),
                 *detail ? ": " : "", detail);
}

#ifdef _WIN32
// A UTF-8 path (the arguments arrive as UTF-16 and are converted, core's
// paths are UTF-8) as Windows' own form, for the CLI's own file calls;
// empty when it isn't UTF-8, which then names no file
std::wstring wide(const char* utf8)
{
    return cli::utf8_to_wide(utf8);
}
#endif

// A file's bytes, read whole up to one byte past max, so the reader they
// go to refuses a larger file in its own words. false with why, in words
// of its own and never the path ("the <what> can't be read: ..."):
// strerror isn't thread-safe, and its words differ by platform
bool read_whole(const char* path, size_t max, const char* what,
                std::string* bytes, std::string* why)
{
    bytes->clear();
    FILE* f = nullptr;
#ifdef _WIN32
    int e = _wfopen_s(&f, wide(path).c_str(), L"rb");
#else
    f = std::fopen(path, "rb");
    int e = errno;
#endif
    bool failed = f == nullptr;
    if (f != nullptr) {
        char chunk[64 * 1024];
        size_t n = 0;
        while (bytes->size() <= max &&
               (n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
            bytes->append(chunk, n);
        }
        failed = std::ferror(f) != 0;
        e = errno;
        std::fclose(f);
        if (bytes->size() > max + 1) {
            bytes->resize(max + 1);
        }
    }
    if (failed) {
        *why = std::string("the ") + what + " can't be read: " +
               (e == ENOENT   ? std::string("there's no such file")
                : e == EACCES ? std::string("permission denied")
                              : "error " + std::to_string(e));
        return false;
    }
    return true;
}

// Writes a file whole, or not at all: into a partial of the call's own
// beside it (cli/partial_file.h), then renamed over path, so a failed
// write never leaves half a file at the user's path, and never over a
// folder. false with why (what can't be written), never the path
bool write_whole(const char* path, const std::string& bytes, std::string* why,
                 const char* what)
{
    if (is_folder(path)) {
        *why = std::string(what) + " can't be written: it's a folder";
        return false;
    }
    std::string partial;
    std::string words; /* a Windows error errno doesn't name */
    int e = 0;
    FILE* f = cli::create_partial(path, cli::process_id(), &partial, &e);
    if (f == nullptr) {
        /* nothing made, nothing to remove: a name there is another's */
        *why = std::string(what) + " can't be written: " +
               (e == EEXIST   ? std::string("every partial name beside it "
                                            "is taken")
                : e == ENOENT ? std::string("there's no such folder")
                : e == EACCES ? std::string("permission denied")
                              : "error " + std::to_string(e));
        return false;
    }
    bool failed =
        std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size() ||
        std::fflush(f) != 0;
    e = errno;
    failed = std::fclose(f) != 0 || failed;
    if (!failed) {
#ifdef _WIN32
        failed = MoveFileExW(wide(partial.c_str()).c_str(), wide(path).c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH) == 0;
        if (failed) {
            const DWORD w = GetLastError();
            if (w == ERROR_ACCESS_DENIED) {
                e = EACCES;
            } else {
                words = "Windows error " + std::to_string(w);
            }
        }
#else
        failed = std::rename(partial.c_str(), path) != 0;
        e = errno;
#endif
    }
    if (failed) {
        cli::remove_file(partial);
        *why = std::string(what) + " can't be written: " +
               (!words.empty() ? words
                : e == ENOENT  ? std::string("there's no such folder")
                : e == EACCES  ? std::string("permission denied")
                               : "error " + std::to_string(e));
        return false;
    }
    return true;
}

// Whether path names a folder (a plugin's); otherwise it's read as a file
bool is_folder(const char* path)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st{};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// Whether path names a file, not a folder
bool is_file(const char* path)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st{};
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool parse_count(const char* text, uint64_t& out)
{
    if (!text || !*text) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno || *end || v == 0 || *text == '-' || *text == '+') {
        return false;
    }
    out = v;
    return true;
}

bool parse_run_seconds(const char* text, uint64_t& out)
{
    return parse_count(text, out) && out <= cli::kMaxRunSeconds;
}

} // namespace cmd
