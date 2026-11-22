// File/layer: core/src/recorder_text.cpp | C++17, Qt-free; the
//             recorder's text helpers and its folder's naming (ADR-0049)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)

#include "recorder_text.h"

#include "ecuio/signals.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include "win_utf8.h"
#include <io.h>
#include <share.h>
#else
#include <climits>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ecuio_rec {

constexpr size_t kLineMax = 1024;
constexpr size_t kLabelMax = 40;

std::tm local_tm(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::tm utc_tm(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

/* focus_parts_with over a recorder's defs, sorted by id: each pick's key */
std::vector<std::string> focus_parts(const std::vector<def_entry>& defs,
                                     const std::vector<uint16_t>& ids,
                                     size_t* named)
{
    return focus_parts_with(
        ids,
        [&defs](uint16_t id, std::string* name) {
            const auto it = std::lower_bound(
                defs.begin(), defs.end(), id,
                [](const def_entry& d, uint16_t v) { return d.id < v; });
            if (it == defs.end() || it->id != id) {
                return false;
            }
            *name = it->key;
            return true;
        },
        named);
}

/* A header's focus lines ("focus" or "pick", as many as the names take)
 * and its minimum time between sweeps, none for no pick */
std::string focus_header(const std::vector<std::string>& parts, size_t named,
                         const focus_named& f)
{
    std::string lines;
    for (const std::string& part : parts) {
        /* A front end's default pick isn't a log's choice */
        lines += f.pick ? "pick  " : "focus  ";
        lines += part + "\n";
    }
    if (named > 0 && f.min_sweep_ms != 0) {
        lines += "min sweep  " + std::to_string(f.min_sweep_ms) + " ms\n";
    }
    return lines;
}

/* A line safe for events.log or markers.csv: carriage returns and
 * newlines as spaces, cut at 1 KB on a character's start */
std::string one_line(const char* text)
{
    std::string s(text, strnlen(text, kLineMax + 4));
    if (s.size() > kLineMax) {
        size_t cut = kLineMax;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        s.resize(cut);
    }
    for (char& c : s) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return s;
}

/*
 * An enum def's label lines for events.log's header: "label
 * <key>,<raw>,<label>" per entry, raw values ascending as the set holds
 * them. The label is CSV-quoted, so a comma or a quote in it reads
 * back, and a line break is a space, keeping the label within its 27
 * bytes. A label can't start a formula: the set's validator refuses one
 * that would (ADR-0049 section 8), so none reaches a file. key is the
 * signal's key.
 */
std::string label_lines(const ecuio_signal_set_t* set,
                        const ecuio_signal_def_t& d, const std::string& key)
{
    if (d.kind != ECUIO_VALUE_ENUM || set->enums == nullptr ||
        uint32_t{d.table_first} + d.table_count > set->enum_count) {
        return {};
    }
    std::string lines;
    for (uint32_t k = 0; k < d.table_count; ++k) {
        const ecuio_enum_entry_t& e = set->enums[d.table_first + k];
        const std::string label(e.label, strnlen(e.label, sizeof e.label));
        lines += "label  " + key + "," + std::to_string(e.raw) + "," +
                 csv_quoted(label.data(), label.size()) + "\n";
    }
    return lines;
}

/* ADR-0027 section 2's rule, in core: ASCII letters and digits, space,
 * '-', '_', '.', and every character past ASCII kept whole; runs of
 * spaces made one; at most 40 characters; no dot or space at the end */
std::string reduce_label(const char* label)
{
    std::string out;
    size_t chars = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(label);
    while (*p != 0 && chars < kLabelMax) {
        const unsigned char c = *p;
        if (c < 0x80) {
            const bool keep = (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                              c == '.' || c == ' ';
            if (keep && !(c == ' ' && (out.empty() || out.back() == ' '))) {
                out += static_cast<char>(c);
                ++chars;
            }
            ++p;
            continue;
        }
        /* A character past ASCII: its lead byte and continuation bytes,
         * kept only when whole */
        size_t n = (c & 0xE0) == 0xC0   ? 2
                   : (c & 0xF0) == 0xE0 ? 3
                   : (c & 0xF8) == 0xF0 ? 4
                                        : 0;
        bool whole = n != 0;
        for (size_t i = 1; whole && i < n; ++i) {
            whole = (p[i] & 0xC0) == 0x80;
        }
        if (whole) {
            out.append(reinterpret_cast<const char*>(p), n);
            ++chars;
            p += n;
        } else {
            ++p;
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

/* The C library's words for errno */
std::string errno_text(int err)
{
#ifdef _WIN32
    char buf[128];
    return strerror_s(buf, sizeof buf, err) == 0 ? std::string(buf)
                                                 : "the write failed";
#else
    const char* s = std::strerror(err);
    return s != nullptr ? std::string(s) : "the write failed";
#endif
}

/* The OS's words for a failed write, never the path */
std::string write_reason(int err)
{
    switch (err) {
    case ENOSPC:
        return "the disk is full";
    case EFBIG:
        return "the file reached the file system's size limit";
    case EIO:
        return "the storage stopped answering";
    default:
        return errno_text(err);
    }
}

#ifdef _WIN32
std::wstring wide(const std::string& utf8)
{
    std::wstring w(utf8.size() + 1, L'\0');
    if (!ecuio_utf8_to_wide(utf8.c_str(), &w[0], w.size())) {
        return std::wstring();
    }
    w.resize(wcslen(w.c_str()));
    return w;
}

std::string os_reason(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::string out;
    if (n > 0 && text != nullptr) {
        char buf[512];
        if (ecuio_wide_to_utf8(text, buf, sizeof buf)) {
            out = buf;
        }
    }
    if (text != nullptr) {
        LocalFree(text);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = "Windows error " + std::to_string(code);
    }
    return out;
}

FILE* open_file(const std::string& path)
{
    const std::wstring w = wide(path);
    /* Text mode, as the app writes them (CRLF on Windows), and shared
     * for reading, so the reader can open a recording being written */
    return w.empty() ? nullptr : _wfsopen(w.c_str(), L"w", _SH_DENYWR);
}
#else
FILE* open_file(const std::string& path)
{
    return std::fopen(path.c_str(), "w");
}
#endif

/* Writes bytes to path as they are (binary, so its hash holds on every
 * platform); false with errno set on a failure */
bool write_bytes(const std::string& path, const std::string& bytes)
{
#ifdef _WIN32
    const std::wstring w = wide(path);
    FILE* f = w.empty() ? nullptr : _wfsopen(w.c_str(), L"wb", _SH_DENYWR);
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (f == nullptr) {
        return false;
    }
    const bool wrote =
        std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const int e = errno;
    const bool closed = std::fclose(f) == 0;
    if (!wrote) {
        errno = e;
    }
    return wrote && closed;
}

/* Creates every missing folder of root; false with the OS's reason */
bool make_root(const std::string& root, std::string* why)
{
    for (size_t i = 1; i <= root.size(); ++i) {
        if (i < root.size() && root[i] != '/' && root[i] != '\\') {
            continue;
        }
        const std::string prefix = root.substr(0, i);
        if (prefix.empty() || prefix.back() == ':') {
            continue;
        }
#ifdef _WIN32
        const std::wstring w = wide(prefix);
        if (w.empty()) {
            *why = "the folder's name isn't valid UTF-8";
            return false;
        }
        if (!CreateDirectoryW(w.c_str(), nullptr)) {
            const DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS && e != ERROR_ACCESS_DENIED) {
                *why = os_reason(e);
                return false;
            }
        }
#else
        if (mkdir(prefix.c_str(), 0777) != 0 && errno != EEXIST) {
            *why = errno_text(errno);
            return false;
        }
#endif
    }
    return true;
}

/* Creates the leaf: true when this call made it, false with gone set
 * when the name is taken, false with why when it can't be made */
bool make_leaf(const std::string& path, bool* taken, std::string* why)
{
    *taken = false;
#ifdef _WIN32
    const std::wstring w = wide(path);
    if (w.empty()) {
        *why = "the folder's name isn't valid UTF-8";
        return false;
    }
    if (CreateDirectoryW(w.c_str(), nullptr)) {
        return true;
    }
    const DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS) {
        *taken = true;
    } else {
        *why = os_reason(e);
    }
#else
    if (mkdir(path.c_str(), 0777) == 0)
        return true;
    if (errno == EEXIST) {
        *taken = true;
    } else {
        *why = errno_text(errno);
    }
#endif
    return false;
}

std::string absolute(const std::string& path)
{
#ifdef _WIN32
    const std::wstring w = wide(path);
    wchar_t buf[4096];
    const DWORD n = GetFullPathNameW(w.c_str(), 4096, buf, nullptr);
    if (n == 0 || n >= 4096) {
        return path;
    }
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] == L'\\') {
            buf[i] = L'/';
        }
    }
    char out[4096 * 3];
    return ecuio_wide_to_utf8(buf, out, sizeof out) ? std::string(out) : path;
#else
    /* As the caller spelled it, links kept: realpath would turn macOS's
     * /var into /private/var, or an iOS container's into another spelling
     * than the caller shows relative to. A relative path joins the working
     * directory; ".", ".." and repeated separators go */
    std::string full = path;
    if (full.empty() || full[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd) == nullptr) {
            return path;
        }
        full = std::string(cwd) + "/" + full;
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= full.size()) {
        size_t end = full.find('/', start);
        if (end == std::string::npos) {
            end = full.size();
        }
        const std::string part = full.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out;
    for (const std::string& part : parts) {
        out += '/';
        out += part;
    }
    return out.empty() ? std::string("/") : out;
#endif
}

/* A link state in the words the reader and a person both take:
 * "adapter fault (state 9, 3 episodes)" */
std::string link_text(int32_t state, uint32_t episodes)
{
    std::string s;
    switch (state) {
    case ECUIO_OK:
        s = "recovered";
        break;
    case ECUIO_ERR_VEHICLE_TIMEOUT:
        s = "vehicle not responding";
        break;
    case ECUIO_ERR_REQUEST_FAILED:
        s = "requests failing";
        break;
    case ECUIO_ERR_TRANSPORT_HARD_FAILURE:
        s = "adapter fault";
        break;
    case ECUIO_ERR_RECORDING_CHANGED:
        s = "replay stopped: its recording changed after it was opened";
        break;
    default:
        s = ecuio_status_string(static_cast<ecuio_status_t>(state));
        break;
    }
    return s + " (state " + std::to_string(state) + ", " +
           std::to_string(episodes) +
           (episodes == 1 ? " episode)" : " episodes)");
}

/* A header's link line: OK with no episode is healthy, where the event
 * lines' "recovered" would say a fault had passed */
std::string header_link(int32_t state, uint32_t episodes)
{
    return "link  " +
           (state == ECUIO_OK && episodes == 0 ? "healthy (state 0, 0 episodes)"
                                               : link_text(state, episodes)) +
           "\n";
}

/* The header's started and utc lines for a recording's zero at wall */
std::string started_lines(std::chrono::system_clock::time_point wall)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(wall);
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       wall.time_since_epoch())
                       .count() %
                   1000;
    if (ms < 0) {
        ms += 1000;
    }
    const std::tm lt = local_tm(t);
    const std::tm ut = utc_tm(t);
    char buf[96];
    std::string lines;
    std::snprintf(
        buf, sizeof buf,
        "started  %04d-%02d-%02d %02d:%02d:%02d.%03lld (local time)\n",
        lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
        lt.tm_sec, ms);
    lines += buf;
    std::snprintf(buf, sizeof buf,
                  "utc  %04d-%02d-%02dT%02d:%02d:%02d.%03lldZ\n",
                  ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday, ut.tm_hour,
                  ut.tm_min, ut.tm_sec, ms);
    lines += buf;
    return lines;
}

/* A recording folder's name under root: the local time to the second,
 * "-n" past the first that's taken, then the label; false with why when
 * none can be made. With from (an armed recorder's windows), n starts
 * there, is six digits so names sort as made, and *from is the next. */
bool make_recording_folder(const std::string& root, std::time_t at,
                           const std::string& tail, std::string* leaf,
                           std::string* why, int* from)
{
    const std::tm tm = local_tm(at);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    const int most = from != nullptr ? 999999 : 1000;
    for (int n = from != nullptr ? *from : 1;; ++n) {
        std::string path = root;
        path += '/';
        path += stamp;
        if (n > 1) {
            char number[16];
            std::snprintf(number, sizeof number,
                          from != nullptr ? "-%06d" : "-%d", n);
            path += number;
        }
        path += tail;
        bool taken = false;
        if (make_leaf(path, &taken, why)) {
            leaf->swap(path); /* a copy could throw, the folder made */
            if (from != nullptr) {
                *from = n + 1;
            }
            return true;
        }
        if (!taken || n >= most) {
            if (taken) {
                *why = "every name for this second is taken";
            }
            return false;
        }
    }
}

bool sync_file(FILE* f)
{
    if (f == nullptr) {
        return true;
    }
    if (std::fflush(f) != 0) {
        return false;
    }
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

} // namespace ecuio_rec
