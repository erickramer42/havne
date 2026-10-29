// File/layer: core/src/elm327_log.cpp | C++17, Qt-free, every platform
// Testing: HARNESS — Elm327SessionTest.TheWireLog* (test_elm327) against
//          the fake ELM327, in a directory of the test's own
//          (ecuio_elm327_set_log_dir); UNIT — Elm327LogTest for the kept
//          name and the text's escapes. The POSIX file calls first
//          compile in CI
//
// The wire log's file (elm327_log.h): its name, its lines, and the
// pruning, on Windows' wide-character calls and POSIX alike.

#include "elm327_log.h"

#include "ecuio/internal.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "win_utf8.h"
#else
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace {

/* ecuio_elm327_set_log_dir's directory; "" is the temporary one */
std::mutex g_dir_lock;
std::string g_dir;

constexpr char kPrefix[] = "ecuio-elm327.";

#ifdef _WIN32
constexpr char kSep = '\\';
constexpr char kNewline[] = "\r\n";
#else
constexpr char kSep = '/';
constexpr char kNewline[] = "\n";
#endif

struct local_time {
    unsigned year, month, day, hour, minute, second, ms;
};

local_time now_local()
{
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    return {t.wYear,   t.wMonth,  t.wDay,         t.wHour,
            t.wMinute, t.wSecond, t.wMilliseconds};
#else
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm lt{};
    localtime_r(&ts.tv_sec, &lt);
    return {static_cast<unsigned>(lt.tm_year + 1900),
            static_cast<unsigned>(lt.tm_mon + 1),
            static_cast<unsigned>(lt.tm_mday),
            static_cast<unsigned>(lt.tm_hour),
            static_cast<unsigned>(lt.tm_min),
            static_cast<unsigned>(lt.tm_sec),
            static_cast<unsigned>(ts.tv_nsec / 1000000)};
#endif
}

/* The log's directory with its separator, UTF-8; "" for none */
std::string log_dir()
{
    std::string dir;
    {
        std::lock_guard<std::mutex> g(g_dir_lock);
        dir = g_dir;
    }
    if (dir.empty()) {
#ifdef _WIN32
        wchar_t w[MAX_PATH] = {};
        const DWORD n = GetTempPathW(MAX_PATH, w);
        char u[MAX_PATH * 3];
        if (n == 0 || n >= MAX_PATH || !ecuio_wide_to_utf8(w, u, sizeof(u))) {
            return "";
        }
        dir = u;
#else
        const char* t = getenv("TMPDIR");
        dir = (t != nullptr && t[0] != '\0') ? t : "/tmp";
#endif
    }
    if (dir.back() != '/' && dir.back() != kSep) {
        dir += kSep;
    }
    return dir;
}

#ifdef _WIN32

bool wide(const std::string& path, std::wstring& out)
{
    out.assign(path.size() + 1, L'\0');
    if (!ecuio_utf8_to_wide(path.c_str(), &out[0], out.size())) {
        return false;
    }
    out.resize(wcslen(out.c_str()));
    return true;
}

/* A new file at path, or none (*exists when one was there) */
elm327_log_file create_new(const std::string& path, bool* exists)
{
    *exists = false;
    std::wstring w;
    if (!wide(path, w)) {
        return ELM327_LOG_NONE;
    }
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *exists = GetLastError() == ERROR_FILE_EXISTS;
        return ELM327_LOG_NONE;
    }
    return h;
}

void write_all(elm327_log_file f, const char* p, size_t n)
{
    DWORD written = 0;
    (void)WriteFile(f, p, static_cast<DWORD>(n), &written, nullptr);
}

void close_file(elm327_log_file f)
{
    CloseHandle(f);
}

/* The names in dir that start with the prefix */
void list_logs(const std::string& dir, std::vector<std::string>& names)
{
    std::wstring pattern;
    if (!wide(dir + kPrefix + "*", pattern)) {
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char name[MAX_PATH * 3];
        if (ecuio_wide_to_utf8(fd.cFileName, name, sizeof(name))) {
            names.emplace_back(name);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

void remove_file(const std::string& path)
{
    std::wstring w;
    if (wide(path, w)) {
        (void)DeleteFileW(w.c_str());
    }
}

#else

elm327_log_file create_new(const std::string& path, bool* exists)
{
    const int fd =
        open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    *exists = fd < 0 && errno == EEXIST;
    return fd < 0 ? ELM327_LOG_NONE : fd;
}

void write_all(elm327_log_file f, const char* p, size_t n)
{
    while (n > 0) {
        const ssize_t w = write(f, p, n);
        if (w <= 0) {
            return;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

void close_file(elm327_log_file f)
{
    close(f);
}

void list_logs(const std::string& dir, std::vector<std::string>& names)
{
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }
    while (const dirent* e = readdir(d)) {
        if (strncmp(e->d_name, kPrefix, sizeof(kPrefix) - 1) == 0) {
            names.emplace_back(e->d_name);
        }
    }
    closedir(d);
}

void remove_file(const std::string& path)
{
    (void)unlink(path.c_str());
}

#endif

/* Deletes the oldest kept logs past ELM327_KEPT_LOGS. The stamp sorts by
 * time, so the oldest is the smallest name. */
void prune(const std::string& dir)
{
    std::vector<std::string> names;
    list_logs(dir, names);
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& n) {
                                   return !elm327_log_kept_name(n.c_str());
                               }),
                names.end());
    if (names.size() <= static_cast<size_t>(ELM327_KEPT_LOGS)) {
        return;
    }
    std::sort(names.begin(), names.end());
    const size_t extra = names.size() - static_cast<size_t>(ELM327_KEPT_LOGS);
    for (size_t i = 0; i < extra; ++i) {
        remove_file(dir + names[i]);
    }
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

} // namespace

bool elm327_log_kept_name(const char* name)
{
    static const char kShape[] = "ecuio-elm327.########-######-###";
    size_t i = 0;
    for (; kShape[i] != '\0'; ++i) {
        const char c = name[i];
        if (kShape[i] == '#' ? !is_digit(c) : c != kShape[i]) {
            return false;
        }
    }
    if (name[i] == '-' && name[i + 1] >= '2' && name[i + 1] <= '9') {
        i += 2;
    }
    return strcmp(name + i, ".log") == 0;
}

void elm327_log_open(elm327_log& log)
{
    log = elm327_log{};
    try {
        const std::string dir = log_dir();
        if (dir.empty()) {
            return;
        }
        const local_time t = now_local();
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u-%03u", t.year,
                 t.month, t.day, t.hour, t.minute, t.second, t.ms);
        for (int n = 1; n <= 9 && !elm327_log_is_open(log); ++n) {
            char suffix[8] = "";
            if (n > 1) {
                snprintf(suffix, sizeof(suffix), "-%d", n);
            }
            bool exists = false;
            log.file =
                create_new(dir + kPrefix + stamp + suffix + ".log", &exists);
            if (!exists) {
                break;
            }
        }
        if (!elm327_log_is_open(log)) {
            return;
        }
        prune(dir);
    } catch (...) {
        /* no memory for a path: no log */
        if (elm327_log_is_open(log)) {
            close_file(log.file);
        }
        log = elm327_log{};
        return;
    }
#ifdef _WIN32
    char v[4] = {};
    const DWORD n =
        GetEnvironmentVariableA("ECUIO_SUR_LOG_FRAMES", v, sizeof(v));
    log.requests = n == 1 && v[0] == '1';
#else
    const char* v = getenv("ECUIO_SUR_LOG_FRAMES");
    log.requests = v != nullptr && strcmp(v, "1") == 0;
#endif
    log.opened = host_clock::now();
}

void elm327_log_write(elm327_log& log, const char* fmt, ...)
{
    if (!elm327_log_is_open(log)) {
        return;
    }
    const local_time t = now_local();
    const long long us = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            host_clock::now() - log.opened)
            .count());
    char line[1024];
    const size_t room = sizeof(line) - sizeof(kNewline);
    int n =
        snprintf(line, room, "[elm327 %02u:%02u:%02u.%03u +%lld.%06lld] ",
                 t.hour, t.minute, t.second, t.ms, us / 1000000, us % 1000000);
    if (n < 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line + n, room - static_cast<size_t>(n), fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    memcpy(line + len, kNewline, sizeof(kNewline));
    len += sizeof(kNewline) - 1;
    write_all(log.file, line, len);
}

void elm327_log_text(const char* in, size_t n, char* out, size_t cap)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            if (o + 1 >= cap) {
                break;
            }
            out[o++] = static_cast<char>(c);
        } else {
            if (o + 4 >= cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = digits[c >> 4];
            out[o++] = digits[c & 0x0F];
        }
    }
    if (cap > 0) {
        out[o] = '\0';
    }
}

void elm327_log_close(elm327_log& log)
{
    if (elm327_log_is_open(log)) {
        close_file(log.file);
    }
    log = elm327_log{};
}

extern "C" void ecuio_elm327_set_log_dir(const char* dir)
{
    std::lock_guard<std::mutex> g(g_dir_lock);
    g_dir = (dir != nullptr) ? dir : "";
}
