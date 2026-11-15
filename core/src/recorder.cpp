// File/layer: core/src/recorder.cpp | C++17, Qt-free; the recorder
//             (ADR-0049 sections 1 to 4), armed too (ADR-0063)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// The worker copies each frame and its values, its link transitions and
// its reply timing into a byte ring (single producer, single consumer);
// a writer thread of the recorder's own drains it into frames.csv,
// signals.csv, events.log's link and cond lines, markers.csv and
// latency.csv in ADR-0017's rows, and the caller's notes and marks, which
// come through a queue of their own, into events.log and markers.csv. A
// frame's condition events ride in its record; a frame that doesn't fit
// leaves them in a carry of 256 that goes in ahead of the next record, and
// past it they are counted and said in a drop line (ADR-0063 section 8).
// The writer never holds the session: what it writes reached it through
// the ring or the queue, or was copied at attach.

#include "recorder.h"

#include "condition_eval.h"
#include "error_detail.h"
#include "recording_lines.h" /* csv_quoted, signal_line */
#include "recording_rows.h" /* kFormat3, the first line */
#include "session.h"
#include "sha256.h"
#include "signal_file_build.h" /* a loaded file's bytes */

#include "ecuio/internal.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "win_utf8.h"
#include <io.h>
#include <share.h>
#else
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t kDefaultRingBytes = 4u * 1024u * 1024u;
constexpr uint32_t kDefaultNoteLines = 256;
constexpr size_t kLineMax = 1024;
constexpr size_t kLabelMax = 40;
constexpr auto kBusyWake = std::chrono::milliseconds(10);
constexpr auto kIdleWake = std::chrono::milliseconds(100);
constexpr auto kFlushEvery = std::chrono::milliseconds(100);
constexpr auto kSyncEvery = std::chrono::seconds(2);

/* Condition events a full ring leaves waiting; changes past them are
 * counted lost */
constexpr uint32_t kCarryEvents = 256;
/* "conditions  conditions.json,<64 hex>\n" and its NUL */
constexpr size_t kConditionsLineMax = 96;

constexpr uint64_t kTimingEveryUs = 10000000;
constexpr uint64_t kSleepLineUs = 1000000;
constexpr auto kFinalTimingWait = std::chrono::seconds(1);
constexpr auto kFinalTimingRetry = std::chrono::milliseconds(10);

/* A record in the ring: this head, then its body, which starts with its
 * host_us. A frame's is frame_body, the used bytes, then each value as
 * value_head and its double or text, then each condition event as
 * event_body; a link transition's link_body; reply timing's timing_body
 * and its rows; carried events' conditions_body and its events. */
struct record_head {
    uint32_t size; /* the whole record, head included */
    uint32_t type;
    uint64_t dropped_before; /* frames the ring had no room for */
};
constexpr uint32_t kRecordFrame = 1;
constexpr uint32_t kRecordLink = 2;
constexpr uint32_t kRecordTiming = 3;
constexpr uint32_t kRecordConditions = 4;

struct link_body {
    uint64_t host_us;
    int32_t state;
    uint32_t episodes;
};

struct timing_body {
    uint64_t host_us;
    ecuio_session_timing_t timing;
    uint32_t rows;
    uint32_t pad;
};

struct frame_body {
    uint64_t host_us;
    uint64_t timestamp_us;
    uint32_t id;
    uint32_t flags;
    uint16_t length;
    uint16_t values;
    uint32_t events;
};

/* One condition event, on the recording's clock and the session's */
struct event_body {
    uint64_t host_us;
    uint64_t t_us;
    double value;
    uint32_t condition;
    uint32_t term;
    uint8_t state;
    uint8_t flags; /* ECUIO_CONDITION_STALE, _FIRED, _HELD */
    uint16_t pad;
    uint32_t pad2;
};
static_assert(sizeof(event_body) == 40, "a ring event is 40 bytes");

/* Carried events; lost counts one run of events past the carry, the
 * first of them at lost_us */
struct conditions_body {
    uint64_t host_us;
    uint32_t events;
    uint32_t pad;
    uint64_t lost;
    uint64_t lost_us;
};

struct value_head {
    uint16_t signal_id;
    uint8_t kind;
    uint8_t pad;
    uint32_t text_len; /* text's bytes follow; else a double */
};

struct def_entry {
    uint16_t id;
    bool text;
    std::string name; /* CSV-quoted as signals.csv writes it */
    std::string unit;
};

/* A focus as a recording names it: the picks in order, its kind word,
 * and the minimum time between sweeps of a session that asks */
struct focus_named {
    std::vector<uint16_t> ids;
    bool pick = false;
    uint32_t min_sweep_ms = 0;

    bool operator==(const focus_named& o) const
    {
        return ids == o.ids && pick == o.pick && min_sweep_ms == o.min_sweep_ms;
    }
};

/* What the caller's threads hand the writer; asleep is an armed writer's
 * own, held with the notes until it knows the window it falls in */
enum class item_kind { begin, note, mark, focus, asleep };
struct item {
    item_kind kind;
    uint64_t host_us;
    std::string text;  /* focus's: its kind */
    std::string bytes; /* begin's: signals.json, empty for none; focus's:
                          its lines, a NUL between two */
    focus_named focus; /* focus's: what it names, for a window's header */
};

/* ecuio_recorder_test_set_clock's (internal.h) */
std::atomic<bool> g_test_clock_on{false};
std::atomic<uint64_t> g_test_clock_us{0};

/* The words a window's end line gives when its conditions cleared */
constexpr const char* kWindowEnded = "the window ended";

/* The pre-roll a full ring cuts: past this many of its bytes queued, an
 * armed writer lets its oldest record go */
constexpr uint64_t kPressedQuarters = 3;

/* The session's focus; the caller holds focus_mutex */
focus_named focus_now(const ecuio_session_t* session)
{
    focus_named f;
    f.ids = session->focus_ids;
    f.pick = (session->focus_flags & ECUIO_FOCUS_DEFAULT_PICK) != 0;
    /* Only a session that asks has one: the others take it and change
     * nothing */
    f.min_sweep_ms = session->focus_requests ? session->focus_min_sweep_ms : 0;
    return f;
}

bool is_text_kind(uint8_t kind)
{
    return kind == ECUIO_VALUE_TEXT || kind == ECUIO_VALUE_HEX;
}

/* recording_lines.h's, which the CSV import writes with too */
using ecuio_recording_lines::csv_quoted;

void append_u64(std::string& out, uint64_t v)
{
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%llu",
                                static_cast<unsigned long long>(v));
    out.append(buf, static_cast<size_t>(n));
}

void append_hex(std::string& out, uint32_t v)
{
    char buf[16];
    const int n = std::snprintf(buf, sizeof buf, "0x%X", v);
    out.append(buf, static_cast<size_t>(n));
}

/* Seconds since the start to the millisecond, rounded, as "+1.234" */
void append_seconds(std::string& out, uint64_t host_us)
{
    const uint64_t ms = (host_us + 500) / 1000;
    char buf[32];
    const int n = std::snprintf(buf, sizeof buf, "+%llu.%03llu",
                                static_cast<unsigned long long>(ms / 1000),
                                static_cast<unsigned long long>(ms % 1000));
    out.append(buf, static_cast<size_t>(n));
}

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

/* The most of a focus line's names: a large pick of long names takes
 * several lines, each far inside a row's bound */
constexpr size_t kFocusPartBytes = 4096;

/* The picks' names quoted as signals.csv quotes them, in pick order, in
 * parts of at most kFocusPartBytes (a name never split); named counts
 * them. name_of(id, &name) finds a pick's name; one it lacks names
 * nothing. */
template <typename NameOf>
std::vector<std::string> focus_parts_with(const std::vector<uint16_t>& ids,
                                          NameOf name_of, size_t* named)
{
    std::vector<std::string> parts;
    *named = 0;
    for (uint16_t id : ids) {
        std::string name;
        if (!name_of(id, &name)) {
            continue;
        }
        if (parts.empty() ||
            parts.back().size() + 1 + name.size() > kFocusPartBytes) {
            parts.push_back(name);
        } else {
            parts.back() += ',';
            parts.back() += name;
        }
        ++*named;
    }
    return parts;
}

/* focus_parts_with over a set's defs */
std::vector<std::string> focus_parts(const ecuio_signal_set_t* set,
                                     const std::vector<uint16_t>& ids,
                                     size_t* named)
{
    return focus_parts_with(
        ids,
        [set](uint16_t id, std::string* name) {
            for (uint32_t i = 0;
                 set != nullptr && set->defs != nullptr && i < set->count;
                 ++i) {
                const ecuio_signal_def_t& d = set->defs[i];
                if (d.id == id) {
                    *name = csv_quoted(d.name, strnlen(d.name, sizeof d.name));
                    return true;
                }
            }
            return false;
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
 * An enum def's label lines for events.log's header (format 3): "label
 * <name>,<raw>,<label>" per entry, raw values ascending as the set holds
 * them. The name and the label are CSV-quoted, so a comma or a quote in
 * either reads back, and a line break is a space, keeping the label
 * within its 27 bytes. A label can't start a formula: the set's
 * validator refuses one that would (ADR-0049 section 8), so none reaches
 * a file. name is the CSV-quoted name.
 */
std::string label_lines(const ecuio_signal_set_t* set,
                        const ecuio_signal_def_t& d, const std::string& name)
{
    if (d.kind != ECUIO_VALUE_ENUM || set->enums == nullptr ||
        uint32_t{d.table_first} + d.table_count > set->enum_count) {
        return {};
    }
    std::string lines;
    for (uint32_t k = 0; k < d.table_count; ++k) {
        const ecuio_enum_entry_t& e = set->enums[d.table_first + k];
        const std::string label(e.label, strnlen(e.label, sizeof e.label));
        lines += "label  " + name + "," + std::to_string(e.raw) + "," +
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
                           std::string* why, int* from = nullptr)
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
            *leaf = path;
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

/* The interval's clock as havne-cli's timing lines name it */
const char* clock_name(uint32_t clock)
{
    switch (clock) {
    case ECUIO_TIMING_CLOCK_HOST:
        return "host";
    case ECUIO_TIMING_CLOCK_ADAPTER:
        return "adapter";
    case ECUIO_TIMING_CLOCK_KERNEL:
        return "kernel";
    default:
        return "none";
    }
}

/* A quantile, or nothing when it has too few samples */
void append_quantile(std::string& out, uint32_t us, bool has)
{
    if (has) {
        append_u64(out, us);
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

} // namespace

/* One per recording, its members grouped by who uses them: the padding
 * the analyzer would save is some 40 bytes */
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ecuio_recorder {
    std::string folder; /* UTF-8, absolute, '/' separated */
    FILE* frames = nullptr;
    FILE* signals = nullptr;
    FILE* events = nullptr;
    FILE* markers = nullptr;
    FILE* latency = nullptr;

    /* The ring: the worker advances head, the writer tail; both only
     * grow, so head - tail is what's queued */
    std::unique_ptr<uint8_t[]> ring;
    uint32_t cap = 0;
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> records_dropped{0};

    /* The worker's alone: frames dropped since the last record, a link
     * transition still to go in, the next timing snapshot's session time */
    uint64_t pending_dropped = 0;
    bool link_pending = false;
    link_body pending_link{};
    uint64_t timing_next_us = kTimingEveryUs;

    /* The recording's set of conditions (NULL: none) and its attach, fixed
     * at attach; held states and the carry are the worker's, then close's */
    std::shared_ptr<const ecuio_condition_record> conditions;
    uint32_t conditions_generation = 0;
    bool held_taken = false;
    uint32_t held_count = 0;
    event_body held[ECUIO_CONDITION_MAX_CONDITIONS] = {};
    event_body carry[kCarryEvents] = {};
    uint32_t carry_start = 0;
    uint32_t carry_count = 0;
    uint64_t carry_lost = 0;
    uint64_t carry_lost_us = 0;
    std::atomic<uint64_t> conditions_lost{0};
    /* The worker's: the host_us of the last frame put into the ring, no
     * event of a later frame placed before it */
    uint64_t put_host_us = 0;

    /* The signal file decoding the session to come, set before attach
     * (ecuio_recorder_signal_file): the set it must be, its bytes and
     * their SHA-256 */
    const ecuio_signal_set_t* signals_set = nullptr;
    std::string signals_json;
    std::string signals_sha;
    std::string signals_plugin; /* "<id>,<version>,<variant>" or "" */

    /* Fixed at attach; zero_us is this recording's, which the link's
     * stops being once a later recorder attaches */
    std::shared_ptr<ecuio_record_link> link;
    uint64_t zero_us = 0;
    std::vector<def_entry> defs; /* sorted by id */
    uint32_t rx_ids[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    uint32_t channels = 0;
    std::chrono::system_clock::time_point start_wall{};
    std::atomic<bool> attached{false};
    std::atomic<bool> detached{false};
    /* Detached, and the writer has taken every record put (DRAINED) */
    std::atomic<bool> drained{false};

    std::mutex note_m;
    std::deque<item> items;
    uint32_t note_cap = kDefaultNoteLines;
    std::atomic<uint64_t> notes_dropped{0};

    /* The focus the header or the last focus line named: set at attach,
     * then read and written under the session's focus_mutex */
    focus_named named_focus;

    std::thread writer;
    std::mutex wake_m;
    std::condition_variable wake_cv;
    bool stop = false; /* under wake_m */

    std::atomic<uint32_t> state{ECUIO_RECORDING_ACTIVE};
    mutable std::mutex reason_m;
    std::string reason;
    std::atomic<uint64_t> frame_rows{0};
    std::atomic<uint64_t> value_rows{0};

    /* The test seams (internal.h) */
    std::atomic<bool> hold{false};
    std::atomic<bool> fail_next{false};
    std::atomic<uint64_t> syncs{0};
    std::atomic<uint64_t> wakes{0};
    std::atomic<uint64_t> slept_added{0};

    /* The machine's slept count at attach, written before attached; the
     * writer's own after that */
    uint64_t slept_at_attach = 0;
    bool sleep_armed = false;
    uint64_t slept_seen = 0;

    /* The writer's own */
    std::vector<uint8_t> scratch;
    std::string frame_out, value_out, event_out, marker_out, latency_out;
    std::string cond_text;
    /* Each request's answers, refusals and timeouts at its last row, so
     * latency.csv gets only the rows that changed */
    uint64_t timing_totals[ECUIO_PROFILE_MAX_REQUESTS] = {};
    /* The files' times count from base_us (a window's first record; 0 for
     * a recording of every frame), the wall clock's from wall_base */
    uint64_t base_us = 0;
    std::chrono::system_clock::time_point wall_base{};

    /* get_folder's copy, under folder_m: an armed writer moves it */
    mutable std::mutex folder_m;
    std::string published;

    /* An armed recorder (ADR-0063 section 7). armed, root and label_tail
     * are set at open; the pieces of a window's header, the copies and
     * the states at attach, before attached is stored; the rest is the
     * writer's. */
    bool armed = false;
    std::string root;
    std::string label_tail;    /* " <label>" or "" */
    std::string armed_core;    /* core to frame flags, conditions among them */
    std::string armed_signals; /* the signal and label lines */
    std::string armed_caller;  /* the caller's lines */
    std::string window_json;   /* signals.json, "" for none */
    uint64_t longest_before = 0;
    /* The second the last window's name took, and the next n in it */
    std::time_t name_second = 0;
    int name_next = 1;
    int32_t win_link_state = ECUIO_OK; /* at the tail */
    uint32_t win_link_episodes = 0;
    focus_named win_focus; /* at the tail */

    /* A window as the scan has seen it, in session time: from start_us to
     * end_us once no record condition is ON (open_end false) */
    struct window {
        uint64_t start_us = 0;
        uint64_t end_us = 0;
        bool open_end = true;
        bool opened = false;
        uint32_t trigger = 0;
    };
    std::deque<window> windows_q;
    /* The scan reads every record ahead of the tail for its events: scan
     * the ring position read to, newest_frame_us the newest session time
     * of a frame it read. A record's place is that newest time where it
     * stands (tail_pos the tail's), as the evaluator judges a frame
     * stamped earlier than the one before at that one's time. */
    uint64_t scan = 0;
    uint64_t newest_frame_us = 0;
    bool scanned_frame = false;
    uint64_t tail_pos = 0;
    /* The open window's last record written, on the recording's clock */
    uint64_t win_last_host = 0;
    std::vector<uint8_t> scan_on; /* a record condition ON and fresh */
    uint32_t scan_active = 0;
    /* Each condition as the tail has passed it, for a window's held lines */
    struct tail_state {
        bool on = false;
        bool stale = false;
        double value = 0;
        uint32_t term = 0;
    };
    std::vector<tail_state> tail_cond;
    /* The newest record a full ring let go before its time */
    uint64_t early_upto_us = 0;
    bool early_any = false;
    /* Notes, marks, focus and asleep lines, held until the tail reaches
     * their time */
    std::deque<item> pending;
    bool in_window = false;
    std::atomic<bool> window_open{false};
    uint64_t win_frames0 = 0, win_values0 = 0, win_notes0 = 0;
    uint64_t win_dropped = 0;
    std::atomic<uint32_t> windows_opened{0};
    std::atomic<uint32_t> prerolls_short{0};
    std::atomic<uint64_t> frames_passed{0};

    uint64_t rel(uint64_t host_us) const
    {
        return host_us > base_us ? host_us - base_us : 0;
    }

    void fail_text(const std::string& why)
    {
        {
            std::lock_guard<std::mutex> g(reason_m);
            if (failed()) {
                return;
            }
            reason = why;
        }
        state.store(ECUIO_RECORDING_FAILED, std::memory_order_release);
    }

    bool failed() const
    {
        return state.load(std::memory_order_acquire) == ECUIO_RECORDING_FAILED;
    }

    void fail(int err)
    {
        {
            std::lock_guard<std::mutex> g(reason_m);
            if (failed()) {
                return;
            }
            reason = write_reason(err);
        }
        state.store(ECUIO_RECORDING_FAILED, std::memory_order_release);
    }

    /* Writes and clears out; on a failed write the recording stops
     * writing, and what follows is dropped */
    void write(FILE* f, std::string& out)
    {
        if (out.empty() || f == nullptr) {
            out.clear();
            return;
        }
        if (!failed()) {
            if (fail_next.exchange(false)) {
                fail(ENOSPC);
            } else if (std::fwrite(out.data(), 1, out.size(), f) !=
                       out.size()) {
                fail(errno != 0 ? errno : EIO);
            }
        }
        out.clear();
    }

    void copy_out(uint64_t pos, void* dst, size_t n) const
    {
        const size_t at = static_cast<size_t>(pos % cap);
        const size_t first = (std::min)(n, cap - at);
        std::memcpy(dst, ring.get() + at, first);
        if (n > first) {
            std::memcpy(static_cast<uint8_t*>(dst) + first, ring.get(),
                        n - first);
        }
    }

    void copy_in(uint64_t pos, const void* src, size_t n)
    {
        const size_t at = static_cast<size_t>(pos % cap);
        const size_t first = (std::min)(n, cap - at);
        std::memcpy(ring.get() + at, src, first);
        if (n > first) {
            std::memcpy(ring.get(), static_cast<const uint8_t*>(src) + first,
                        n - first);
        }
    }

    const def_entry* def(uint16_t id) const
    {
        auto it = std::lower_bound(
            defs.begin(), defs.end(), id,
            [](const def_entry& d, uint16_t v) { return d.id < v; });
        return (it != defs.end() && it->id == id) ? &*it : nullptr;
    }

    /* "HH:mm:ss.zzz  +S.sss  <kind>  <text>" at host_us */
    void event_line(uint64_t host_us, const char* kind, const std::string& text)
    {
        host_us = rel(host_us);
        const auto at = wall_base + std::chrono::microseconds(host_us);
        const std::time_t t = std::chrono::system_clock::to_time_t(at);
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                at.time_since_epoch())
                .count() %
            1000;
        const std::tm tm = local_tm(t);
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03lld", tm.tm_hour,
                      tm.tm_min, tm.tm_sec, ms < 0 ? ms + 1000 : ms);
        event_out += stamp;
        event_out += "  ";
        append_seconds(event_out, host_us);
        event_out += "  ";
        event_out += kind;
        event_out += "  ";
        event_out += text;
        event_out += '\n';
    }

    void drop_line(uint64_t host_us, uint64_t dropped)
    {
        event_line(host_us, "drop",
                   "the recording dropped " + std::to_string(dropped) +
                       (dropped == 1 ? " frame" : " frames") +
                       ": the disk fell behind");
    }

    void add_marker(uint64_t host_us, const std::string& label)
    {
        if (markers == nullptr && !failed()) {
            markers = open_file(folder + "/markers.csv");
            if (markers == nullptr) {
                fail(errno != 0 ? errno : EIO);
                return;
            }
            marker_out += "host_us,label\n";
        }
        append_u64(marker_out, rel(host_us));
        marker_out += ',';
        marker_out += csv_quoted(label.data(), label.size());
        marker_out += '\n';
    }

    /* "cond  <key>,<on|off>,<flags>,<value>,<term>,<host_us>,<t_us>", and
     * a marker named for the condition at a firing of one with mark */
    void take_event(const event_body& e)
    {
        const ecuio_condition_record* set = conditions.get();
        if (set == nullptr || e.condition >= set->conditions.size()) {
            return;
        }
        const ecuio_condition_record::entry& c = set->conditions[e.condition];
        ecuio_recording_rows::cond_line line;
        const size_t key_len =
            c.key.size() < sizeof line.key ? c.key.size() : sizeof line.key - 1;
        std::memcpy(line.key, c.key.data(), key_len);
        line.on = e.state == ECUIO_CONDITION_ON;
        line.flags = e.flags;
        line.value = e.value;
        line.term = e.term;
        line.host_us = rel(e.host_us);
        line.t_us = e.t_us;
        cond_text.clear();
        ecuio_recording_rows::append_cond_text(cond_text, line);
        event_line(e.host_us, "cond", cond_text);
        if ((e.flags & ECUIO_CONDITION_FIRED) != 0 && c.mark) {
            add_marker(e.host_us, c.name);
        }
    }

    void lost_line(uint64_t host_us, uint64_t lost)
    {
        event_line(host_us, "drop",
                   "the recording lost " + std::to_string(lost) +
                       (lost == 1 ? " condition event" : " condition events") +
                       ": the disk fell behind");
    }

    void take_conditions(const uint8_t* body, size_t size)
    {
        if (size < sizeof(conditions_body)) {
            return;
        }
        conditions_body cb;
        std::memcpy(&cb, body, sizeof cb);
        size_t at = sizeof cb;
        for (uint32_t k = 0; k < cb.events && at + sizeof(event_body) <= size;
             ++k) {
            event_body e;
            std::memcpy(&e, body + at, sizeof e);
            at += sizeof e;
            take_event(e);
        }
        if (cb.lost > 0) {
            lost_line(cb.lost_us, cb.lost);
        }
    }

    void take_link(const uint8_t* body, size_t size)
    {
        if (size < sizeof(link_body)) {
            return;
        }
        link_body lb;
        std::memcpy(&lb, body, sizeof lb);
        event_line(lb.host_us, "link", link_text(lb.state, lb.episodes));
    }

    /* latency.csv's rows whose counts changed since their last row
     * (ADR-0017's appendix of 2026-10-31), the file made with the first */
    void take_timing(const uint8_t* body, size_t size)
    {
        if (size < sizeof(timing_body)) {
            return;
        }
        timing_body tb;
        std::memcpy(&tb, body, sizeof tb);
        /* t_s on the recording's clock, as every other file's time: the
         * snapshot's session time is the session's, older than the
         * recording when the recorder attached after start */
        const uint64_t ms = (rel(tb.host_us) + 500) / 1000;
        char at[32];
        std::snprintf(at, sizeof at, "%llu.%03llu",
                      static_cast<unsigned long long>(ms / 1000),
                      static_cast<unsigned long long>(ms % 1000));
        static const char kHex[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < tb.rows && i < ECUIO_PROFILE_MAX_REQUESTS;
             ++i) {
            const size_t off = sizeof tb + i * sizeof(ecuio_request_timing_t);
            if (off + sizeof(ecuio_request_timing_t) > size) {
                break;
            }
            ecuio_request_timing_t r;
            std::memcpy(&r, body + off, sizeof r);
            const uint64_t total =
                uint64_t{r.answered} + r.refused + r.timeouts;
            if (total == timing_totals[i]) {
                continue;
            }
            timing_totals[i] = total;
            const bool latency_has = r.answered + r.refused > 0;
            const bool interval_has = r.intervals > 0;
            latency_out += at;
            latency_out += ',';
            append_hex(latency_out,
                       r.channel < channels ? rx_ids[r.channel] : 0);
            latency_out += ',';
            for (uint8_t k = 0; k < r.length && k < 3; ++k) {
                if (k > 0) {
                    latency_out += ' ';
                }
                latency_out += kHex[r.request[k] >> 4];
                latency_out += kHex[r.request[k] & 0x0F];
            }
            for (uint32_t n : {r.answered, r.refused, r.timeouts, r.packed}) {
                latency_out += ',';
                append_u64(latency_out, n);
            }
            latency_out += ',';
            append_quantile(latency_out, r.latency_p50_us, latency_has);
            latency_out += ',';
            append_quantile(latency_out, r.latency_p99_us,
                            (r.flags & ECUIO_TIMING_LATENCY_P99) != 0);
            latency_out += ',';
            append_quantile(latency_out, r.latency_p999_us,
                            (r.flags & ECUIO_TIMING_LATENCY_P999) != 0);
            latency_out += ',';
            append_quantile(latency_out, r.latency_max_us, latency_has);
            latency_out += ',';
            append_u64(latency_out, r.intervals);
            latency_out += ',';
            append_u64(latency_out, r.intervals_mixed);
            latency_out += ',';
            append_quantile(latency_out, r.interval_p50_us, interval_has);
            latency_out += ',';
            append_quantile(latency_out, r.interval_p99_us,
                            (r.flags & ECUIO_TIMING_INTERVAL_P99) != 0);
            latency_out += ',';
            append_quantile(latency_out, r.interval_p999_us,
                            (r.flags & ECUIO_TIMING_INTERVAL_P999) != 0);
            latency_out += ',';
            append_quantile(latency_out, r.interval_max_us, interval_has);
            latency_out += ',';
            latency_out += clock_name(tb.timing.interval_clock);
            latency_out += '\n';
        }
        if (!latency_out.empty() && latency == nullptr && !failed()) {
            latency = open_file(folder + "/latency.csv");
            if (latency == nullptr) {
                fail(errno != 0 ? errno : EIO);
                return;
            }
            latency_out.insert(0, ECUIO_TIMING_COLUMNS "\n");
        }
    }

    void take_frame(const uint8_t* body, size_t size)
    {
        if (size < sizeof(frame_body)) {
            return;
        }
        frame_body fb;
        std::memcpy(&fb, body, sizeof fb);
        size_t at = sizeof fb;
        if (at + fb.length > size) {
            return;
        }
        const uint64_t host_us = rel(fb.host_us);
        append_u64(frame_out, host_us);
        frame_out += ',';
        append_u64(frame_out, fb.timestamp_us);
        frame_out += ',';
        append_hex(frame_out, fb.id);
        frame_out += ',';
        append_hex(frame_out, fb.flags);
        frame_out += ',';
        static const char kHex[] = "0123456789ABCDEF";
        for (uint16_t i = 0; i < fb.length; ++i) {
            if (i > 0) {
                frame_out += ' ';
            }
            const uint8_t b = body[at + i];
            frame_out += kHex[b >> 4];
            frame_out += kHex[b & 0x0F];
        }
        frame_out += '\n';
        at += fb.length;
        frame_rows.fetch_add(1, std::memory_order_relaxed);

        for (uint16_t v = 0; v < fb.values; ++v) {
            if (at + sizeof(value_head) > size) {
                return;
            }
            value_head vh;
            std::memcpy(&vh, body + at, sizeof vh);
            at += sizeof vh;
            const def_entry* d = def(vh.signal_id);
            const bool text = is_text_kind(vh.kind);
            const size_t len = text ? vh.text_len : sizeof(double);
            if (at + len > size) {
                return;
            }
            if (d != nullptr) {
                append_u64(value_out, host_us);
                value_out += ',';
                value_out += d->name;
                value_out += ',';
                if (text) {
                    value_out += csv_quoted(
                        reinterpret_cast<const char*>(body + at), len);
                } else {
                    double x;
                    std::memcpy(&x, body + at, sizeof x);
                    ecuio_recording_rows::append_value(value_out, x);
                }
                value_out += ',';
                value_out += d->unit;
                value_out += '\n';
                value_rows.fetch_add(1, std::memory_order_relaxed);
            }
            at += len;
        }
        for (uint32_t k = 0; k < fb.events; ++k) {
            if (at + sizeof(event_body) > size) {
                return;
            }
            event_body e;
            std::memcpy(&e, body + at, sizeof e);
            at += sizeof e;
            take_event(e);
        }
    }

    /* Everything the worker has queued; true when there was any */
    bool drain_ring()
    {
        bool any = false;
        uint64_t t = tail.load(std::memory_order_relaxed);
        const uint64_t h = head.load(std::memory_order_acquire);
        while (h - t >= sizeof(record_head)) {
            record_head rh;
            copy_out(t, &rh, sizeof rh);
            if (rh.size < sizeof rh || rh.size > h - t) {
                break;
            }
            const size_t body = rh.size - sizeof rh;
            if (scratch.size() < body) {
                scratch.resize(body);
            }
            copy_out(t + sizeof rh, scratch.data(), body);
            if (rh.dropped_before > 0 && body >= sizeof(uint64_t)) {
                uint64_t host_us;
                std::memcpy(&host_us, scratch.data(), sizeof host_us);
                drop_line(host_us, rh.dropped_before);
            }
            switch (rh.type) {
            case kRecordFrame:
                take_frame(scratch.data(), body);
                break;
            case kRecordLink:
                take_link(scratch.data(), body);
                break;
            case kRecordTiming:
                take_timing(scratch.data(), body);
                break;
            case kRecordConditions:
                take_conditions(scratch.data(), body);
                break;
            default:
                break;
            }
            t += rh.size;
            any = true;
        }
        tail.store(t, std::memory_order_release);
        write(frames, frame_out);
        write(signals, value_out);
        write(events, event_out);
        write(markers, marker_out);
        write(latency, latency_out);
        return any;
    }

    bool drain_items()
    {
        std::deque<item> taken;
        {
            std::lock_guard<std::mutex> g(note_m);
            taken.swap(items);
        }
        for (const item& it : taken) {
            switch (it.kind) {
            case item_kind::begin:
                /* The file before the header line naming its hash */
                if (!it.bytes.empty() && !failed()) {
                    errno = 0;
                    if (fail_next.exchange(false)) {
                        fail(ENOSPC);
                    } else if (!write_bytes(
                                   folder + "/" +
                                       ecuio_recording_rows::kSignalFileName,
                                   it.bytes)) {
                        fail(errno != 0 ? errno : EIO);
                    }
                }
                if (conditions != nullptr && !failed()) {
                    errno = 0;
                    if (!write_bytes(folder + "/" +
                                         ecuio_recording_rows::kConditionsName,
                                     conditions->bytes)) {
                        fail(errno != 0 ? errno : EIO);
                    }
                }
                event_out += it.text;
                begin_rows();
                break;
            default:
                write_item(it);
                break;
            }
        }
        write(frames, frame_out);
        write(signals, value_out);
        write(events, event_out);
        write(markers, marker_out);
        return !taken.empty();
    }

    /* frames.csv's and signals.csv's header rows */
    void begin_rows()
    {
        frame_out += "host_us,timestamp_us,id,flags,data\n";
        /* A byte-order mark, so a spreadsheet reads the units' UTF-8 as
         * UTF-8 (ADR-0017) */
        value_out += "\xEF\xBB\xBFhost_us,signal,value,unit\n";
    }

    /* A note, a mark, a focus's lines or an asleep line */
    void write_item(const item& it)
    {
        switch (it.kind) {
        case item_kind::focus: {
            size_t at = 0;
            while (at <= it.bytes.size()) {
                const size_t end =
                    (std::min)(it.bytes.find('\0', at), it.bytes.size());
                event_line(it.host_us, it.text.c_str(),
                           it.bytes.substr(at, end - at));
                at = end + 1;
            }
            break;
        }
        case item_kind::note:
            event_line(it.host_us, "note", it.text);
            break;
        case item_kind::asleep:
            event_line(it.host_us, "asleep", it.text);
            break;
        case item_kind::mark:
            add_marker(it.host_us, it.text);
            break;
        case item_kind::begin:
            break;
        }
    }

    void write_outs()
    {
        write(frames, frame_out);
        write(signals, value_out);
        write(events, event_out);
        write(markers, marker_out);
        write(latency, latency_out);
    }

    /* ---- The armed writer (ADR-0063 section 7) ----
     * The scan reads each record ahead of the tail and keeps the windows
     * its events make; the tail then writes a record inside a window, and
     * lets one go only once no event yet unread can reach back to it:
     * older than the longest before behind the newest frame read, or the
     * ring three quarters full. Windows and release are in session time,
     * as every condition rule is, so a replay at any speed gives the same
     * windows; release is by time, never by count, since a frame's events
     * can lie back in time (a deadline settled at a gap's end), never
     * before the frame judged before it. The files keep the recording's
     * clock, and notes and marks go by it. */

    /* The caller's items, held with their times; the begin item is the
     * attach's header, which a window builds itself */
    void take_items()
    {
        std::deque<item> taken;
        {
            std::lock_guard<std::mutex> g(note_m);
            taken.swap(items);
        }
        for (item& it : taken) {
            if (it.kind == item_kind::begin) {
                continue;
            }
            if (pending.size() >= note_cap) {
                notes_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            pending.push_back(std::move(it));
        }
    }

    /* Each condition event in a record's body */
    template <typename F>
    static void each_event(uint32_t type, const uint8_t* body, size_t size, F f)
    {
        size_t at = 0;
        uint32_t count = 0;
        if (type == kRecordFrame) {
            if (size < sizeof(frame_body)) {
                return;
            }
            frame_body fb;
            std::memcpy(&fb, body, sizeof fb);
            at = sizeof fb + fb.length;
            for (uint16_t v = 0; v < fb.values; ++v) {
                if (at + sizeof(value_head) > size) {
                    return;
                }
                value_head vh;
                std::memcpy(&vh, body + at, sizeof vh);
                at += sizeof vh +
                      (is_text_kind(vh.kind) ? vh.text_len : sizeof(double));
            }
            count = fb.events;
        } else if (type == kRecordConditions) {
            if (size < sizeof(conditions_body)) {
                return;
            }
            conditions_body cb;
            std::memcpy(&cb, body, sizeof cb);
            at = sizeof cb;
            count = cb.events;
        }
        for (uint32_t k = 0; k < count && at + sizeof(event_body) <= size;
             ++k) {
            event_body e;
            std::memcpy(&e, body + at, sizeof e);
            at += sizeof e;
            f(e);
        }
    }

    /* A record condition's change at the scan, at its session time: a rise
     * opens a window or carries the last one on; the last fall gives the
     * window its end, after past it or the record that settled it,
     * whichever is later */
    void scan_event(const event_body& e, uint64_t record_us)
    {
        const ecuio_condition_record* set = conditions.get();
        if (set == nullptr || e.condition >= scan_on.size() ||
            e.condition >= set->conditions.size() ||
            !set->conditions[e.condition].record) {
            return;
        }
        const ecuio_condition_record::entry& c = set->conditions[e.condition];
        const bool on = e.state == ECUIO_CONDITION_ON &&
                        (e.flags & ECUIO_CONDITION_STALE) == 0;
        if (on == (scan_on[e.condition] != 0)) {
            return;
        }
        scan_on[e.condition] = on ? 1 : 0;
        if (on) {
            ++scan_active;
            const uint64_t start =
                e.t_us > c.before_us ? e.t_us - c.before_us : 0;
            if (!windows_q.empty()) {
                window& w = windows_q.back();
                if (w.open_end || start <= w.end_us) {
                    w.open_end = true;
                    if (!w.opened && start < w.start_us) {
                        w.start_us = start;
                    }
                    return;
                }
            }
            window w;
            w.start_us = start;
            w.trigger = e.condition;
            windows_q.push_back(w);
            return;
        }
        --scan_active;
        if (windows_q.empty()) {
            return;
        }
        window& w = windows_q.back();
        w.end_us =
            (std::max)(w.end_us, (std::max)(e.t_us + c.after_us, record_us));
        if (scan_active == 0) {
            w.open_end = false;
        }
    }

    /* What the tail passes, written or not: the conditions' states and the
     * link's, for the next window's header and held lines */
    void follow(const record_head& rh, const uint8_t* body, size_t size)
    {
        if (rh.type == kRecordLink && size >= sizeof(link_body)) {
            link_body lb;
            std::memcpy(&lb, body, sizeof lb);
            win_link_state = lb.state;
            win_link_episodes = lb.episodes;
            return;
        }
        each_event(rh.type, body, size, [this](const event_body& e) {
            if (e.condition < tail_cond.size()) {
                tail_state& s = tail_cond[e.condition];
                s.on = e.state == ECUIO_CONDITION_ON;
                s.stale = (e.flags & ECUIO_CONDITION_STALE) != 0;
                s.value = e.value;
                s.term = e.term;
            }
        });
    }

    /* A record into the open window */
    void write_record(const record_head& rh, const uint8_t* body, size_t size)
    {
        if (rh.dropped_before > 0 && size >= sizeof(uint64_t)) {
            uint64_t host_us;
            std::memcpy(&host_us, body, sizeof host_us);
            drop_line(host_us, rh.dropped_before);
            win_dropped += rh.dropped_before;
        }
        switch (rh.type) {
        case kRecordFrame:
            take_frame(body, size);
            break;
        case kRecordLink:
            take_link(body, size);
            break;
        case kRecordTiming:
            take_timing(body, size);
            break;
        case kRecordConditions:
            take_conditions(body, size);
            break;
        default:
            break;
        }
    }

    /* The session time of the first frame from ring position at on */
    uint64_t first_frame_t_us(uint64_t at) const
    {
        while (scan - at >= sizeof(record_head)) {
            record_head rh;
            copy_out(at, &rh, sizeof rh);
            if (rh.size < sizeof rh || rh.size > scan - at) {
                break;
            }
            if (rh.type == kRecordFrame &&
                rh.size >= sizeof rh + sizeof(frame_body)) {
                frame_body fb;
                copy_out(at + sizeof rh, &fb, sizeof fb);
                return fb.timestamp_us;
            }
            at += rh.size;
        }
        return 0;
    }

    /* A window's header: a recording begun mid-session's, at its start */
    std::string window_header(const window& w)
    {
        std::string h = std::string(ecuio_recording_rows::kFormat3) + "\n";
        h += started_lines(wall_base);
        h += armed_core;
        const ecuio_condition_record::entry& c =
            conditions->conditions[w.trigger];
        h += "trigger  " + c.key + "," +
             std::to_string((uint64_t{c.before_us} + 500) / 1000) + " ms\n";
        size_t named = 0;
        const std::vector<std::string> parts = focus_parts_with(
            win_focus.ids,
            [this](uint16_t id, std::string* name) {
                const def_entry* d = def(id);
                if (d != nullptr) {
                    *name = d->name;
                }
                return d != nullptr;
            },
            &named);
        h += focus_header(parts, named, win_focus);
        h += header_link(win_link_state, win_link_episodes);
        h += armed_signals;
        h += armed_caller;
        h += "columns  local time, seconds since the recording started "
             "(host_us / 1e6 in the CSV files), kind, text\n\n";
        return h;
    }

    /* The front window's folder, made at its first record (at, host_us):
     * the copies and the header written and flushed before the folder is
     * given to get_folder, so a front end never reads a half-made one */
    void open_window(window& w, uint64_t at, uint64_t host_us)
    {
        w.opened = true;
        base_us = host_us;
        wall_base = start_wall + std::chrono::microseconds(host_us);
        std::string leaf;
        std::string why;
        const std::time_t second =
            std::chrono::system_clock::to_time_t(wall_base);
        if (second != name_second) {
            name_second = second;
            name_next = 1;
        }
        if (failed() || !make_recording_folder(root, second, label_tail, &leaf,
                                               &why, &name_next)) {
            if (!why.empty()) {
                fail_text(why);
            }
            return;
        }
        folder = absolute(leaf);
        errno = 0;
        if ((!window_json.empty() &&
             !write_bytes(folder + "/" + ecuio_recording_rows::kSignalFileName,
                          window_json)) ||
            !write_bytes(folder + "/" + ecuio_recording_rows::kConditionsName,
                         conditions->bytes)) {
            fail(errno != 0 ? errno : EIO);
            return;
        }
        frames = open_file(folder + "/frames.csv");
        signals = open_file(folder + "/signals.csv");
        events = open_file(folder + "/events.log");
        if (frames == nullptr || signals == nullptr || events == nullptr) {
            fail(errno != 0 ? errno : EIO);
            return;
        }
        std::memset(timing_totals, 0, sizeof timing_totals);
        win_frames0 = frame_rows.load(std::memory_order_relaxed);
        win_values0 = value_rows.load(std::memory_order_relaxed);
        win_notes0 = notes_dropped.load(std::memory_order_relaxed);
        win_dropped = 0;
        win_last_host = host_us;
        event_out += window_header(w);
        begin_rows();
        /* Each condition standing ON at the window's start */
        const uint64_t t_us = first_frame_t_us(at);
        for (uint32_t i = 0; i < tail_cond.size(); ++i) {
            const tail_state& s = tail_cond[i];
            if (!s.on) {
                continue;
            }
            event_body e{};
            e.host_us = host_us;
            e.t_us = t_us;
            e.value = s.value;
            e.condition = i;
            e.term = s.term;
            e.state = ECUIO_CONDITION_ON;
            e.flags = static_cast<uint8_t>(
                ECUIO_CONDITION_HELD | (s.stale ? ECUIO_CONDITION_STALE : 0u));
            take_event(e);
        }
        write_outs();
        flush_all();
        if (early_any && w.start_us <= early_upto_us) {
            prerolls_short.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> g(folder_m);
            published = folder;
        }
        windows_opened.fetch_add(1, std::memory_order_release);
        in_window = true;
        window_open.store(true, std::memory_order_release);
    }

    /* The open window's end line at end_us and its files closed */
    void close_window(uint64_t end_us, const std::string& why)
    {
        if (!in_window) {
            return;
        }
        event_out += '\n';
        event_line(
            end_us, "end",
            why + ": " + std::to_string(frame_rows.load() - win_frames0) +
                " frames, " + std::to_string(value_rows.load() - win_values0) +
                " values, " + std::to_string(win_dropped) +
                " records dropped, " +
                std::to_string(notes_dropped.load() - win_notes0) +
                " notes dropped");
        write_outs();
        sync_all();
        close_files();
        in_window = false;
        window_open.store(false, std::memory_order_release);
    }

    /* The held items at or before limit, the recording's time of the record
     * the tail is at: into the window when that record is written in it,
     * else gone; a focus moves the next header's either way */
    void flush_items(uint64_t limit)
    {
        while (!pending.empty() && pending.front().host_us <= limit) {
            const item it = std::move(pending.front());
            pending.pop_front();
            if (it.kind == item_kind::focus) {
                win_focus = it.focus;
            }
            if (in_window) {
                write_item(it);
            }
        }
    }

    /* A record's place in session time: a frame's own, or the newest
     * frame's before it, from pos */
    static uint64_t place_of(const record_head& rh, const uint8_t* body,
                             size_t size, uint64_t pos)
    {
        if (rh.type == kRecordFrame && size >= sizeof(frame_body)) {
            frame_body fb;
            std::memcpy(&fb, body, sizeof fb);
            return (std::max)(pos, fb.timestamp_us);
        }
        return pos;
    }

    bool pressed() const
    {
        const uint64_t queued = head.load(std::memory_order_acquire) -
                                tail.load(std::memory_order_relaxed);
        return queued > cap / 4 * kPressedQuarters;
    }

    /* The tail as far as the windows allow; final (the recorder closing,
     * or its session gone) holds nothing back */
    bool advance_tail(bool final)
    {
        bool any = false;
        for (;;) {
            const uint64_t t = tail.load(std::memory_order_relaxed);
            if (scan - t < sizeof(record_head)) {
                break;
            }
            record_head rh;
            copy_out(t, &rh, sizeof rh);
            const size_t body = rh.size - sizeof rh;
            if (scratch.size() < body) {
                scratch.resize(body);
            }
            copy_out(t + sizeof rh, scratch.data(), body);
            uint64_t host_us = 0;
            if (body >= sizeof host_us) {
                std::memcpy(&host_us, scratch.data(), sizeof host_us);
            }
            const uint64_t pos = place_of(rh, scratch.data(), body, tail_pos);
            window* w = windows_q.empty() ? nullptr : &windows_q.front();
            if (w != nullptr && pos >= w->start_us &&
                (w->open_end || pos <= w->end_us)) {
                if (!w->opened) {
                    open_window(*w, t, host_us);
                }
                flush_items(host_us);
                if (in_window) { /* a failed open writes nothing */
                    write_record(rh, scratch.data(), body);
                    win_last_host = (std::max)(win_last_host, host_us);
                }
            } else if (w != nullptr && !w->open_end && pos > w->end_us) {
                /* Past the window's end: a firing whose before reaches
                 * back into it would carry it on, until the scan is that
                 * far past its end */
                if (!final && !pressed() &&
                    !(scanned_frame &&
                      newest_frame_us > w->end_us + longest_before)) {
                    break;
                }
                if (!w->opened) { /* a full ring let all of it go */
                    prerolls_short.fetch_add(1, std::memory_order_relaxed);
                }
                close_window(win_last_host, kWindowEnded);
                windows_q.pop_front();
                continue;
            } else {
                const bool old =
                    scanned_frame && pos + longest_before < newest_frame_us;
                if (!final && !old) {
                    if (!pressed()) {
                        break;
                    }
                    early_any = true;
                    early_upto_us = (std::max)(early_upto_us, pos);
                }
                flush_items(host_us);
                if (rh.type == kRecordFrame) {
                    frames_passed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            follow(rh, scratch.data(), body);
            tail_pos = pos;
            tail.store(t + rh.size, std::memory_order_release);
            any = true;
        }
        return any;
    }

    /* One wake of an armed writer: the scan to the head, the tail after
     * it; final leaves the open window for close to end */
    bool armed_pass(bool final)
    {
        bool any = false;
        const uint64_t h = head.load(std::memory_order_acquire);
        while (h - scan >= sizeof(record_head)) {
            record_head rh;
            copy_out(scan, &rh, sizeof rh);
            if (rh.size < sizeof rh || rh.size > h - scan) {
                break;
            }
            const size_t body = rh.size - sizeof rh;
            if (scratch.size() < body) {
                scratch.resize(body);
            }
            copy_out(scan + sizeof rh, scratch.data(), body);
            const uint64_t pos =
                place_of(rh, scratch.data(), body, newest_frame_us);
            if (rh.type == kRecordFrame) {
                newest_frame_us = pos;
                scanned_frame = true;
            }
            each_event(
                rh.type, scratch.data(), body,
                [this, pos](const event_body& e) { scan_event(e, pos); });
            scan += rh.size;
            any = true;
        }
        any = advance_tail(final) || any;
        if (final) {
            /* The tail is at the scan: a window still unopened had all of
             * its records let go by a full ring */
            while (!windows_q.empty() && !windows_q.front().opened) {
                prerolls_short.fetch_add(1, std::memory_order_relaxed);
                windows_q.pop_front();
            }
            /* What came after the last record: the open window's while a
             * record condition is ON, else past its end and in no window */
            const bool ended =
                in_window && !windows_q.empty() && !windows_q.front().open_end;
            flush_items(ended ? win_last_host : UINT64_MAX);
        }
        write_outs();
        return any;
    }

    void flush_all()
    {
        for (FILE* f : {frames, signals, events, markers, latency}) {
            if (f != nullptr && !failed() && std::fflush(f) != 0) {
                fail(errno != 0 ? errno : EIO);
            }
        }
    }

    void sync_all()
    {
        for (FILE* f : {frames, signals, events, markers, latency}) {
            if (f != nullptr && !failed() && !sync_file(f)) {
                fail(errno != 0 ? errno : EIO);
            }
        }
        syncs.fetch_add(1, std::memory_order_relaxed);
    }

    /* An asleep line when the machine's slept count rose a second or more
     * since the last wake (ADR-0049 section 2): stamped as the writer
     * woke, its N s the N s before. Smaller rises are taken in without a
     * line, as the app's tick takes them. */
    void check_sleep()
    {
        if (!attached.load(std::memory_order_acquire)) {
            return;
        }
        const uint64_t now = ecuio_clock_slept_us() +
                             slept_added.load(std::memory_order_relaxed);
        if (!sleep_armed) {
            sleep_armed = true;
            slept_seen = slept_at_attach;
        }
        const uint64_t rise = now > slept_seen ? now - slept_seen : 0;
        slept_seen = (std::max)(now, slept_seen);
        if (rise < kSleepLineUs) {
            return;
        }
        const uint64_t tenths = (rise + 50000) / 100000;
        const uint64_t host_us = host_us_of(ecuio_record_clock_us());
        std::string text = "Asleep for " + std::to_string(tenths / 10) + "." +
                           std::to_string(tenths % 10) +
                           " s: nothing was read meanwhile";
        if (armed) {
            pending.push_back(
                item{item_kind::asleep, host_us, std::move(text), {}, {}});
        } else {
            event_line(host_us, "asleep", text);
        }
    }

    uint64_t host_us_of(uint64_t clock_us) const
    {
        return clock_us > zero_us ? clock_us - zero_us : 0;
    }

    void run()
    {
        auto last_flush = std::chrono::steady_clock::now();
        auto last_sync = last_flush;
        bool busy = false;
        for (;;) {
            bool stopping;
            {
                std::unique_lock<std::mutex> lk(wake_m);
                wake_cv.wait_for(lk, busy ? kBusyWake : kIdleWake,
                                 [this] { return stop; });
                stopping = stop;
            }
            wakes.fetch_add(1, std::memory_order_relaxed);
            if (hold.load(std::memory_order_acquire) && !stopping) {
                continue;
            }
            /* Nothing until a session attaches: it completes the header
             * item open queued; one closed unattached leaves its files empty */
            if (!attached.load(std::memory_order_acquire)) {
                if (stopping) {
                    return;
                }
                continue;
            }
            /* Read before the pass: a detach after it can still put */
            const bool gone = detached.load(std::memory_order_acquire);
            if (armed) {
                /* Its session gone, no record follows: nothing held back */
                const bool final = stopping || gone;
                take_items();
                check_sleep();
                busy = armed_pass(final);
            } else {
                /* The items first: the header comes ahead of any row */
                const bool notes = drain_items();
                check_sleep();
                const bool frames_taken = drain_ring();
                busy = notes || frames_taken;
            }
            if (gone && tail.load(std::memory_order_relaxed) ==
                            head.load(std::memory_order_acquire)) {
                drained.store(true, std::memory_order_release);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= kFlushEvery) {
                flush_all();
                last_flush = now;
            }
            if (now - last_sync >= kSyncEvery) {
                sync_all();
                last_sync = now;
            }
            if (stopping) {
                return;
            }
        }
    }

    void close_files()
    {
        for (FILE** f : {&frames, &signals, &events, &markers, &latency}) {
            if (*f != nullptr) {
                if (std::fclose(*f) != 0 && !failed()) {
                    fail(errno != 0 ? errno : EIO);
                }
                *f = nullptr;
            }
        }
    }

    /* Files still open only when open failed part way, which reported
     * its own failure: closed, nothing more recorded */
    ~ecuio_recorder()
    {
        for (FILE* f : {frames, signals, events, markers, latency}) {
            if (f != nullptr) {
                (void)std::fclose(f);
            }
        }
    }
};

uint64_t ecuio_record_clock_us()
{
    return g_test_clock_on.load(std::memory_order_acquire)
               ? g_test_clock_us.load(std::memory_order_acquire)
               : ecuio_clock_us();
}

void ecuio_record_detach(ecuio_record_link& link, ecuio_recorder_t* only)
{
    std::lock_guard<std::mutex> g(link.m);
    if (only != nullptr) {
        ecuio_recorder_t* now = link.active.load(std::memory_order_seq_cst);
        if (now != nullptr && now != only) {
            return;
        }
    }
    ecuio_recorder_t* r =
        link.active.exchange(nullptr, std::memory_order_seq_cst);
    while (link.pins.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    if (r != nullptr) {
        r->detached.store(true, std::memory_order_release);
    }
}

namespace {

/* The worker's: whether size bytes fit in the ring */
bool ring_has_room(const ecuio_recorder_t* r, uint64_t size)
{
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    const uint64_t t = r->tail.load(std::memory_order_acquire);
    return size <= r->cap - (h - t);
}

/* The worker's: a carried link transition into the ring ahead of
 * anything else; false while it still doesn't fit. It carries no drop
 * count, so the drop line keeps the time of the record after the drops. */
bool put_pending_link(ecuio_recorder_t* r)
{
    if (!r->link_pending) {
        return true;
    }
    const uint64_t size = sizeof(record_head) + sizeof(link_body);
    if (!ring_has_room(r, size)) {
        return false;
    }
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    const record_head rh{static_cast<uint32_t>(size), kRecordLink, 0};
    r->copy_in(h, &rh, sizeof rh);
    r->copy_in(h + sizeof rh, &r->pending_link, sizeof r->pending_link);
    r->head.store(h + size, std::memory_order_release);
    r->link_pending = false;
    return true;
}

/* The worker's: carried events into the ring ahead of anything else, as
 * many as fit; false while some still wait. A run's lost count goes once. */
bool put_pending_conditions(ecuio_recorder_t* r)
{
    constexpr uint64_t kHead = sizeof(record_head) + sizeof(conditions_body);
    while (r->carry_count > 0) {
        const uint64_t h = r->head.load(std::memory_order_relaxed);
        const uint64_t t = r->tail.load(std::memory_order_acquire);
        const uint64_t room = r->cap - (h - t);
        if (room < kHead + sizeof(event_body)) {
            return false;
        }
        const uint32_t n = static_cast<uint32_t>(
            (std::min)(uint64_t{r->carry_count},
                       (room - kHead) / sizeof(event_body)));
        const uint64_t size = kHead + uint64_t{n} * sizeof(event_body);
        const event_body* first = r->carry + r->carry_start;
        const record_head rh{static_cast<uint32_t>(size), kRecordConditions, 0};
        const conditions_body cb{first->host_us, n, 0, r->carry_lost,
                                 r->carry_lost_us};
        r->copy_in(h, &rh, sizeof rh);
        r->copy_in(h + sizeof rh, &cb, sizeof cb);
        r->copy_in(h + kHead, first, n * sizeof(event_body));
        r->head.store(h + size, std::memory_order_release);
        r->carry_lost = 0; /* a loss after this is a run of its own */
        r->carry_start += n;
        r->carry_count -= n;
        if (r->carry_count == 0) {
            r->carry_start = 0;
        }
    }
    return true;
}

/* The worker's: an event whose frame found no room waits in the carry;
 * past it, the event is counted lost */
void carry_event(ecuio_recorder_t* r, const event_body& e)
{
    if (r->carry_count == kCarryEvents) {
        if (r->carry_lost == 0) {
            r->carry_lost_us = e.host_us;
        }
        ++r->carry_lost;
        r->conditions_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (r->carry_start + r->carry_count == kCarryEvents) {
        std::memmove(r->carry, r->carry + r->carry_start,
                     r->carry_count * sizeof(event_body));
        r->carry_start = 0;
    }
    r->carry[r->carry_start + r->carry_count++] = e;
}

/* ecuio_session_test_before_record_attach's (internal.h) */
std::atomic<void (*)(void*)> g_before_record_attach{nullptr};
std::atomic<void*> g_before_record_attach_user{nullptr};

uint64_t host_us_now(const ecuio_record_link& link)
{
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t zero = link.zero_us.load(std::memory_order_relaxed);
    return now > zero ? now - zero : 0;
}

} // namespace

void ecuio_record_link_state(ecuio_record_link* link, int32_t state,
                             uint32_t episodes)
{
    if (link == nullptr) {
        return;
    }
    ecuio_recorder_t* r = ecuio_record_pin(*link);
    if (r == nullptr) {
        return;
    }
    /* A transition still carried gives way to this one: the latest state
     * and count are what a reader needs */
    r->pending_link = link_body{host_us_now(*link), state, episodes};
    r->link_pending = true;
    if (put_pending_conditions(r)) {
        put_pending_link(r);
    }
    ecuio_record_unpin(*link);
}

void ecuio_record_timing(ecuio_record_link* link,
                         const ecuio_session_timing_t& timing,
                         const ecuio_request_timing_t* rows, uint32_t count,
                         bool final)
{
    if (link == nullptr) {
        return;
    }
    const uint32_t n = (std::min)(count, uint32_t{ECUIO_PROFILE_MAX_REQUESTS});
    const uint64_t size = sizeof(record_head) + sizeof(timing_body) +
                          uint64_t{n} * sizeof(ecuio_request_timing_t);
    const auto give_up = std::chrono::steady_clock::now() + kFinalTimingWait;
    /* The record's time is the call's, read once a recorder is found: a
     * final snapshot that waits for room keeps the time of its figures */
    uint64_t at = 0;
    bool stamped = false;
    for (;;) {
        ecuio_recorder_t* r = ecuio_record_pin(*link);
        if (r == nullptr) {
            return;
        }
        if (!final && timing.snapshot_us < r->timing_next_us) {
            ecuio_record_unpin(*link);
            return;
        }
        if (!stamped) {
            at = host_us_now(*link);
            stamped = true;
        }
        if (put_pending_conditions(r) && put_pending_link(r) &&
            ring_has_room(r, size)) {
            const uint64_t h = r->head.load(std::memory_order_relaxed);
            const record_head rh{static_cast<uint32_t>(size), kRecordTiming,
                                 r->pending_dropped};
            r->pending_dropped = 0;
            const timing_body tb{at, timing, n, 0};
            r->copy_in(h, &rh, sizeof rh);
            r->copy_in(h + sizeof rh, &tb, sizeof tb);
            r->copy_in(h + sizeof rh + sizeof tb, rows,
                       n * sizeof(ecuio_request_timing_t));
            r->head.store(h + size, std::memory_order_release);
            r->timing_next_us = timing.snapshot_us + kTimingEveryUs;
            ecuio_record_unpin(*link);
            return;
        }
        /* No room: a periodic snapshot is dropped and counted (the next's
         * totals cover it); the final one waits unpinned, so a detach never
         * waits on it */
        if (!final || size > r->cap ||
            std::chrono::steady_clock::now() >= give_up) {
            r->records_dropped.fetch_add(1, std::memory_order_relaxed);
            r->timing_next_us = timing.snapshot_us + kTimingEveryUs;
            ecuio_record_unpin(*link);
            return;
        }
        ecuio_record_unpin(*link);
        std::this_thread::sleep_for(kFinalTimingRetry);
    }
}

void ecuio_recorder_hold_states(ecuio_recorder_t* r,
                                const ecuio_condition_eval& conditions)
{
    if (r->held_taken || r->conditions == nullptr ||
        conditions.generation != r->conditions_generation) {
        return;
    }
    r->held_taken = true;
    uint32_t n = 0;
    for (uint32_t i = 0;
         i < conditions.conditions.size() && n < ECUIO_CONDITION_MAX_CONDITIONS;
         ++i) {
        const ecuio_condition_eval::condition& c = conditions.conditions[i];
        if (c.state != ECUIO_CONDITION_ON) {
            continue;
        }
        event_body& e = r->held[n++];
        e = event_body{};
        e.value = c.value;
        e.condition = i;
        e.term = c.changed_term;
        e.state = ECUIO_CONDITION_ON;
        e.flags = static_cast<uint8_t>(
            ECUIO_CONDITION_HELD | (c.stale != 0 ? ECUIO_CONDITION_STALE : 0u));
    }
    r->held_count = n;
}

void ecuio_recorder_put_frame(ecuio_recorder_t* r, uint64_t host_us,
                              const ecuio_frame_t* frame,
                              const ecuio_value_t* values, uint32_t count,
                              const ecuio_condition_event_t* events,
                              uint32_t event_count, uint32_t generation)
{
    const uint16_t length = frame->data_length <= ECUIO_FRAME_DATA_MAX
                                ? frame->data_length
                                : static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX);
    const uint16_t nvalues =
        count > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(count);
    /* The recording's own set's events alone, its held states first (an
     * older set's frame under way at the attach gives none) */
    const bool own = r->conditions != nullptr &&
                     generation == r->conditions_generation &&
                     (events != nullptr || event_count == 0);
    const uint32_t held = own ? r->held_count : 0;
    const uint32_t settled = own ? event_count : 0;
    r->held_count = 0;
    const uint32_t nevents = held + settled;
    /* A held state stands before anything this frame settles */
    const uint64_t held_us =
        settled > 0 && events[0].timestamp_us < frame->timestamp_us
            ? events[0].timestamp_us
            : frame->timestamp_us;
    /* An event's recording time: the frame's, less how far its own time
     * lies before the frame's, never before the frame put before it */
    const uint64_t floor_us = (std::min)(r->put_host_us, host_us);
    const auto host_of = [&](uint64_t t_us) {
        if (t_us >= frame->timestamp_us) {
            return host_us;
        }
        const uint64_t back = frame->timestamp_us - t_us;
        return back < host_us - floor_us ? host_us - back : floor_us;
    };
    const auto event_at = [&](uint32_t k) {
        event_body e{};
        if (k < held) {
            e = r->held[k];
            e.t_us = held_us;
        } else {
            const ecuio_condition_event_t& c = events[k - held];
            e.t_us = c.timestamp_us;
            e.value = c.value;
            e.condition = c.condition;
            e.term = c.term;
            e.state = static_cast<uint8_t>(c.state);
            e.flags = static_cast<uint8_t>(c.flags);
        }
        e.host_us = host_of(e.t_us);
        return e;
    };

    uint64_t size = sizeof(record_head) + sizeof(frame_body) + length +
                    uint64_t{nevents} * sizeof(event_body);
    for (uint16_t i = 0; i < nvalues; ++i) {
        size += sizeof(value_head) + (is_text_kind(values[i].kind)
                                          ? values[i].text_len
                                          : sizeof(double));
    }
    /* What was carried goes first, or the frame waits behind it, dropped;
     * its events are carried, never dropped with it */
    if (!put_pending_conditions(r) || !put_pending_link(r) ||
        !ring_has_room(r, size)) {
        ++r->pending_dropped;
        r->records_dropped.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t k = 0; k < nevents; ++k) {
            carry_event(r, event_at(k));
        }
        return;
    }
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    record_head rh{static_cast<uint32_t>(size), kRecordFrame,
                   r->pending_dropped};
    r->pending_dropped = 0;
    frame_body fb{host_us,
                  frame->timestamp_us,
                  frame->arbitration_id,
                  frame->flags,
                  length,
                  nvalues,
                  nevents};
    uint64_t at = h;
    r->copy_in(at, &rh, sizeof rh);
    at += sizeof rh;
    r->copy_in(at, &fb, sizeof fb);
    at += sizeof fb;
    r->copy_in(at, frame->data, length);
    at += length;
    for (uint16_t i = 0; i < nvalues; ++i) {
        const ecuio_value_t& v = values[i];
        const bool text = is_text_kind(v.kind);
        value_head vh{v.signal_id, v.kind, 0, text ? v.text_len : 0};
        r->copy_in(at, &vh, sizeof vh);
        at += sizeof vh;
        if (text) {
            r->copy_in(at, v.text, v.text_len);
            at += v.text_len;
        } else {
            r->copy_in(at, &v.value, sizeof v.value);
            at += sizeof v.value;
        }
    }
    for (uint32_t k = 0; k < nevents; ++k) {
        const event_body e = event_at(k);
        r->copy_in(at, &e, sizeof e);
        at += sizeof e;
    }
    r->head.store(h + size, std::memory_order_release);
    r->put_host_us = host_us;
}

ecuio_status_t ecuio_recorder_take_signals(ecuio_recorder_t* recorder,
                                           const ecuio_signal_set_t* set,
                                           const char* bytes, size_t length,
                                           const char* plugin)
{
    if (recorder == nullptr || set == nullptr || plugin == nullptr ||
        (bytes == nullptr && length > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (recorder->attached.load(std::memory_order_acquire) ||
        recorder->signals_set != nullptr ||
        length > ECUIO_SIGNAL_FILE_MAX_BYTES) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::string json(bytes != nullptr ? bytes : "", length);
        recorder->signals_sha = ecuio_sha256::hex_of(json.data(), json.size());
        recorder->signals_plugin = one_line(plugin);
        recorder->signals_json = std::move(json);
    } catch (...) {
        recorder->signals_sha.clear();
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    recorder->signals_set = set;
    return ECUIO_OK;
}

extern "C" {

ecuio_status_t ecuio_recorder_open(const ecuio_record_options_t* options,
                                   ecuio_recorder_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (options == nullptr || options->root_utf8 == nullptr ||
        (options->header_line_count > 0 && options->header_lines == nullptr)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if ((options->flags & ~ECUIO_RECORD_ARMED) != 0 ||
        options->reserved[0] != 0 || options->reserved[1] != 0 ||
        options->reserved[2] != 0 || options->reserved[3] != 0 ||
        options->root_utf8[0] == '\0') {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    std::unique_ptr<ecuio_recorder> r;
    try {
        r = std::make_unique<ecuio_recorder>();
        r->cap =
            options->ring_bytes != 0 ? options->ring_bytes : kDefaultRingBytes;
        r->ring.reset(new uint8_t[r->cap]);
        r->note_cap =
            options->note_lines != 0 ? options->note_lines : kDefaultNoteLines;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    std::string root = options->root_utf8;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) {
        root.pop_back();
    }
    std::string why;
    if (!make_root(root, &why)) {
        ecuio_detail_set("can't create the recordings' folder: %s",
                         why.c_str());
        return ECUIO_ERR_RECORDING_FAILED;
    }

    const std::string label =
        options->label_utf8 != nullptr ? reduce_label(options->label_utf8) : "";
    const std::string tail = label.empty() ? "" : " " + label;
    if ((options->flags & ECUIO_RECORD_ARMED) != 0) {
        /* Each window makes its own folder, under the same name's rule */
        r->armed = true;
        r->root = root;
        r->label_tail = tail;
    } else {
        std::string leaf;
        if (!make_recording_folder(root, std::time(nullptr), tail, &leaf,
                                   &why)) {
            ecuio_detail_set("can't create the recording's folder: %s",
                             why.c_str());
            return ECUIO_ERR_RECORDING_FAILED;
        }
        r->folder = absolute(leaf);
        r->published = r->folder;

        r->frames = open_file(leaf + "/frames.csv");
        r->signals = open_file(leaf + "/signals.csv");
        r->events = open_file(leaf + "/events.log");
        if (r->frames == nullptr || r->signals == nullptr ||
            r->events == nullptr) {
            const int e = errno;
            ecuio_detail_set("can't create the recording's files: %s",
                             e != 0 ? errno_text(e).c_str()
                                    : "the folder refused them");
            return ECUIO_ERR_RECORDING_FAILED;
        }
    }

    /* The caller's header lines, under one key, written at attach */
    std::string caller;
    for (uint32_t i = 0; i < options->header_line_count; ++i) {
        if (options->header_lines[i] != nullptr) {
            caller += "caller  " + one_line(options->header_lines[i]) + "\n";
        }
    }
    r->items.push_back(item{item_kind::begin, 0, caller, {}, {}});
    if (r->armed) {
        r->armed_caller = caller;
    }

    try {
        r->writer = std::thread([rec = r.get()] { rec->run(); });
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = r.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_signal_file(ecuio_recorder_t* recorder,
                                          const ecuio_signal_file_t* file)
{
    if (recorder == nullptr || file == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const std::string& bytes = ecuio_sf::bytes_of(*file);
    return ecuio_recorder_take_signals(recorder, &file->set, bytes.data(),
                                       bytes.size(), file->plugin.c_str());
}

ecuio_status_t ecuio_session_record(ecuio_session_t* session,
                                    ecuio_recorder_t* recorder)
{
    if (session == nullptr || recorder == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    /* A recorder has one session ever; a session one recorder at a time
     * (ADR-0056 section 3), judged under the link's mutex below */
    if (recorder->attached.load()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->signals_set != nullptr &&
        recorder->signals_set != session->active_signal_set) {
        ecuio_detail_set("the signal file isn't what decodes the session "
                         "(its profile is %s)",
                         session->info.profile);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    /* Built in locals first: the recorder changes only once the link is
     * taken, so a refused attach leaves it ready for the next try */
    const std::shared_ptr<ecuio_record_link> link = session->record_link;
    std::vector<def_entry> defs;
    std::string header;
    size_t conditions_at = 0;
    int32_t link_state = ECUIO_OK;
    uint32_t link_episodes = 0;
    /* An armed recorder's: a window's header lines from core to frame
     * flags (the conditions line's place in them) and the signal lines,
     * and room for a state per condition */
    std::string armed_core;
    size_t armed_conditions_at = 0;
    std::string armed_signals;
    std::vector<uint8_t> armed_cond;
    std::vector<ecuio_recorder::tail_state> armed_tail;
    focus_named armed_focus;
    uint64_t zero_us = 0;
    uint64_t slept_at_attach = 0;
    focus_named named_focus;
    std::chrono::system_clock::time_point start_wall{};
    try {
        const ecuio_signal_set_t* set = session->active_signal_set;
        header = std::string(ecuio_recording_rows::kFormat3) + "\n";
        for (uint32_t i = 0;
             set != nullptr && set->defs != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const size_t nlen = strnlen(d.name, sizeof d.name);
            const size_t ulen = strnlen(d.unit, sizeof d.unit);
            const bool text = is_text_kind(d.kind);
            defs.push_back(def_entry{d.id, text, csv_quoted(d.name, nlen),
                                     csv_quoted(d.unit, ulen)});
        }
        std::sort(
            defs.begin(), defs.end(),
            [](const def_entry& a, const def_entry& b) { return a.id < b.id; });

        /* The zero and the wall clock read back to back */
        zero_us = ecuio_record_clock_us();
        slept_at_attach = ecuio_clock_slept_us() + recorder->slept_added.load();
        start_wall = std::chrono::system_clock::now();

        header += started_lines(start_wall);
        const size_t core_at = header.size();
        header += std::string("core  ") + ecuio_version_string() + "\n";
        header += "uri  " + one_line(session->uri.c_str()) + "\n";
        header += std::string("profile  ") + session->info.profile + "\n";
        if (!recorder->signals_json.empty()) {
            header += std::string(ecuio_recording_rows::kSignalFileKey) +
                      ecuio_recording_rows::kSignalFileName + "," +
                      recorder->signals_sha + "\n";
        }
        if (!recorder->signals_plugin.empty()) {
            header += std::string(ecuio_recording_rows::kPluginKey) +
                      recorder->signals_plugin + "\n";
        }
        /* The conditions line goes here once the set is read under the
         * locks, into room kept now */
        conditions_at = header.size();
        /* A replay's frames carry its source's flags, so it writes its
         * source's line, which a recording from before ADR-0049 lacks */
        header += "frame flags  ";
        header += session->info.backend != ECUIO_BACKEND_REPLAY ? "complete"
                  : session->source_frame_flags.empty()
                      ? "from an older recording"
                      : session->source_frame_flags.c_str();
        header += "\n";
        /* A window's header has these lines from the copy, the conditions
         * line put in where the full header takes it */
        if (recorder->armed) {
            armed_core = header.substr(core_at);
            armed_core.reserve(armed_core.size() + kConditionsLineMax);
            armed_conditions_at = conditions_at - core_at;
        }
        /* A session attached to after start (ADR-0056 section 3) says what
         * it had come to: its focus in pick order (none: no line) and, once
         * started, the link's state and episodes in the link lines' words,
         * so a reader needn't wait for a transition */
        {
            std::lock_guard<std::mutex> f(session->focus_mutex);
            named_focus = focus_now(session);
        }
        size_t named = 0;
        /* As many lines as the names take; a reader appends them */
        const std::vector<std::string> parts =
            focus_parts(set, named_focus.ids, &named);
        header += focus_header(parts, named, named_focus);
        if (recorder->armed) {
            armed_focus = named_focus;
        }
        if (session->started) {
            ecuio_session_get_error_state(session, &link_state, &link_episodes);
            header += header_link(link_state, link_episodes);
        }
        const size_t signals_at = header.size();
        for (uint32_t i = 0;
             set != nullptr && set->defs != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const bool enumeration = d.kind == ECUIO_VALUE_ENUM;
            const std::string name =
                csv_quoted(d.name, strnlen(d.name, sizeof d.name));
            header += ecuio_recording_lines::signal_line(
                name,
                is_text_kind(d.kind) ? "text"
                : enumeration        ? "enum"
                                     : "number",
                csv_quoted(d.unit, strnlen(d.unit, sizeof d.unit)),
                d.decimal_places, d.plot_rank, d.group);
            header += label_lines(set, d, name);
        }
        header.reserve(header.size() + kConditionsLineMax);
        if (recorder->armed) {
            armed_signals = header.substr(signals_at);
            armed_cond.reserve(ECUIO_CONDITION_MAX_CONDITIONS);
            armed_tail.reserve(ECUIO_CONDITION_MAX_CONDITIONS);
        }
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    /* The set the session judges by, read under the link's mutex and
     * callback_mutex, so it is the one the recording keeps */
    std::shared_ptr<const ecuio_condition_record> conditions;
    uint32_t generation = 0;

    /* The recorder takes what was made, under the link's mutex */
    const auto commit = [&] {
        if (conditions != nullptr) {
            char line[kConditionsLineMax];
            const int n = std::snprintf(line, sizeof line, "%s%s,%s\n",
                                        ecuio_recording_rows::kConditionsKey,
                                        ecuio_recording_rows::kConditionsName,
                                        conditions->sha256.c_str());
            const size_t len = n < 0 ? 0
                               : static_cast<size_t>(n) < sizeof line
                                   ? static_cast<size_t>(n)
                                   : sizeof line - 1;
            header.insert(conditions_at, line, len);
            if (recorder->armed) {
                armed_core.insert(armed_conditions_at, line, len);
            }
        }
        if (recorder->armed) {
            /* Room for each state was kept before the lock, so these sizes
             * take no allocation; conditions is not NULL (the check) */
            const size_t n = conditions->conditions.size();
            armed_cond.resize(n);
            armed_tail.resize(n);
            for (const ecuio_condition_record::entry& c :
                 conditions->conditions) {
                if (c.record) {
                    recorder->longest_before =
                        (std::max)(recorder->longest_before,
                                   uint64_t{c.before_us});
                }
            }
            recorder->scan_on.swap(armed_cond);
            recorder->tail_cond.swap(armed_tail);
            recorder->armed_core.swap(armed_core);
            recorder->armed_signals.swap(armed_signals);
            recorder->window_json.swap(recorder->signals_json);
            recorder->win_link_state = link_state;
            recorder->win_link_episodes = link_episodes;
            recorder->win_focus.ids.swap(armed_focus.ids);
            recorder->win_focus.pick = armed_focus.pick;
            recorder->win_focus.min_sweep_ms = armed_focus.min_sweep_ms;
        }
        recorder->conditions = std::move(conditions);
        recorder->conditions_generation = generation;
        if (!recorder->armed) {
            std::lock_guard<std::mutex> n(recorder->note_m);
            /* The caller's lines open() queued, then the columns */
            item& begin = recorder->items.front();
            begin.bytes = std::move(recorder->signals_json);
            begin.text =
                header + begin.text +
                "columns  local time, seconds since the recording started "
                "(host_us / 1e6 in the CSV files), kind, text\n\n";
        }
        recorder->defs = std::move(defs);
        recorder->channels =
            (std::min)(session->info.channel_count,
                       uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS});
        for (uint32_t i = 0; i < recorder->channels; ++i) {
            recorder->rx_ids[i] = session->info.rx_ids[i];
        }
        recorder->link = link;
        recorder->zero_us = zero_us;
        recorder->slept_at_attach = slept_at_attach;
        recorder->start_wall = start_wall;
        recorder->wall_base = start_wall;
        recorder->named_focus = std::move(named_focus);
    };
    {
        /* Under the mutex that orders detaches, so a closing recorder or a
         * second attach sees one state of the link, and the refusal is judged
         * here alone. The zero goes in before active, whose release publishes
         * it to the worker (dispatch pins the recorder, then reads the
         * zero). */
        std::lock_guard<std::mutex> g(link->m);
        if (link->active.load(std::memory_order_seq_cst) != nullptr) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> c(session->callback_mutex);
            conditions = session->conditions_record;
            generation = session->conditions_generation;
        }
        /* An armed recorder that nothing could open a window for is
         * refused, so the caller learns it now (ADR-0063 section 7) */
        if (recorder->armed) {
            bool records = false;
            for (size_t i = 0; conditions != nullptr &&
                               i < conditions->conditions.size() && !records;
                 ++i) {
                records = conditions->conditions[i].record;
            }
            if (!records) {
                ecuio_detail_set(
                    conditions == nullptr
                        ? "no condition records: the session has no set of "
                          "conditions attached"
                        : "no condition records: no enabled condition in the "
                          "set has record");
                return ECUIO_ERR_INVALID_ARGUMENT;
            }
        }
        if (const auto hook = g_before_record_attach.load()) {
            hook(g_before_record_attach_user.load());
        }
        commit();
        link->zero_us.store(zero_us, std::memory_order_relaxed);
        link->active.store(recorder, std::memory_order_release);
    }
    recorder->attached.store(true, std::memory_order_release);
    /* A focus set since the header's read may have found no recorder to
     * pin; the compare keeps it from being written twice */
    ecuio_record_focus(session);
    return ECUIO_OK;
}

} // extern "C"

namespace {

/* The focus's activity lines into the pinned recorder's note queue: the
 * count, the minimum when there is one, then the names */
void push_focus(ecuio_recorder_t* r, const ecuio_signal_set_t* set,
                const focus_named& f)
{
    size_t named = 0;
    const std::vector<std::string> parts = focus_parts(set, f.ids, &named);
    std::string lines;
    if (parts.empty()) {
        lines = "none: every signal asked";
    }
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k == 0) {
            lines +=
                std::to_string(named) + (named == 1 ? " signal" : " signals");
            if (f.min_sweep_ms != 0) {
                lines += ", at least " + std::to_string(f.min_sweep_ms) +
                         " ms between sweeps";
            }
            lines += ": ";
        } else {
            lines += std::string(1, '\0') + "more: ";
        }
        lines += parts[k];
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
    std::lock_guard<std::mutex> g(r->note_m);
    if (r->items.size() >= r->note_cap) {
        r->notes_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        r->items.push_back(item{item_kind::focus, host_us,
                                f.pick ? "pick" : "focus", std::move(lines),
                                f});
    }
}

} // namespace

void ecuio_record_focus(ecuio_session_t* session)
{
    ecuio_record_link& link = *session->record_link;
    ecuio_recorder_t* r = nullptr;
    {
        std::lock_guard<std::mutex> g(link.m);
        r = ecuio_record_pin(link);
    }
    if (r == nullptr) {
        return;
    }
    try {
        focus_named f;
        bool changed = false;
        {
            std::lock_guard<std::mutex> g(session->focus_mutex);
            f = focus_now(session);
            changed = !(f == r->named_focus);
            if (changed) {
                r->named_focus = f;
            }
        }
        if (changed) {
            push_focus(r, session->active_signal_set, f);
        }
    } catch (...) {
        /* The focus stands; its line is lost, counted as a note */
        r->notes_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ecuio_record_unpin(link);
}

extern "C" {

ecuio_status_t ecuio_recorder_note(ecuio_recorder_t* recorder,
                                   const char* utf8_line)
{
    if (recorder == nullptr || utf8_line == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!recorder->attached.load(std::memory_order_acquire)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->failed()) {
        return ECUIO_ERR_RECORDING_FAILED;
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us =
        now > recorder->zero_us ? now - recorder->zero_us : 0;
    try {
        std::string line = one_line(utf8_line);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::note, host_us, std::move(line), {}, {}});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_mark(ecuio_recorder_t* recorder,
                                   const char* utf8_label)
{
    if (recorder == nullptr || utf8_label == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!recorder->attached.load(std::memory_order_acquire)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->failed()) {
        return ECUIO_ERR_RECORDING_FAILED;
    }
    const uint64_t now = ecuio_record_clock_us();
    const uint64_t host_us =
        now > recorder->zero_us ? now - recorder->zero_us : 0;
    try {
        std::string label = one_line(utf8_label);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::mark, host_us, std::move(label), {}, {}});
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_get_state(const ecuio_recorder_t* recorder,
                                        ecuio_recorder_state_t* out)
{
    if (recorder == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::memset(out, 0, sizeof *out);
    if (recorder->drained.load(std::memory_order_acquire)) {
        out->flags |= ECUIO_RECORDER_DRAINED;
    }
    const uint32_t state = recorder->state.load(std::memory_order_acquire);
    out->state = state == ECUIO_RECORDING_FAILED ? ECUIO_RECORDING_FAILED
                 : recorder->detached.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_DETACHED
                 : recorder->armed &&
                         !recorder->window_open.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_ARMED
                     : ECUIO_RECORDING_ACTIVE;
    out->frame_rows = recorder->frame_rows.load(std::memory_order_relaxed);
    out->value_rows = recorder->value_rows.load(std::memory_order_relaxed);
    out->records_dropped =
        recorder->records_dropped.load(std::memory_order_relaxed);
    out->notes_dropped =
        recorder->notes_dropped.load(std::memory_order_relaxed);
    out->conditions_lost =
        recorder->conditions_lost.load(std::memory_order_relaxed);
    out->windows = recorder->windows_opened.load(std::memory_order_acquire);
    out->preroll_short =
        recorder->prerolls_short.load(std::memory_order_relaxed);
    out->frames_passed =
        recorder->frames_passed.load(std::memory_order_relaxed);
    if (state == ECUIO_RECORDING_FAILED) {
        std::lock_guard<std::mutex> g(recorder->reason_m);
        const size_t n =
            (std::min)(recorder->reason.size(), sizeof out->reason - 1);
        std::memcpy(out->reason, recorder->reason.data(), n);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_get_folder(const ecuio_recorder_t* recorder,
                                         char* utf8, uint32_t capacity,
                                         uint32_t* out_len)
{
    if (recorder == nullptr || out_len == nullptr ||
        (utf8 == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> g(recorder->folder_m);
    const std::string& f = recorder->published;
    *out_len = static_cast<uint32_t>(f.size());
    if (capacity > 0) {
        const size_t n =
            (std::min)(f.size(), static_cast<size_t>(capacity) - 1);
        std::memcpy(utf8, f.data(), n);
        utf8[n] = '\0';
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_close(ecuio_recorder_t* recorder,
                                    const char* utf8_why)
{
    if (recorder == nullptr) {
        return ECUIO_OK;
    }
    std::unique_ptr<ecuio_recorder> r(recorder);
    if (r->link != nullptr) {
        ecuio_record_detach(*r->link, r.get());
    }
    {
        std::lock_guard<std::mutex> g(r->wake_m);
        r->stop = true;
    }
    r->wake_cv.notify_all();
    if (r->writer.joinable()) {
        r->writer.join();
    }
    /* The writer has drained everything; the summary is the last line */
    if (r->attached.load() && r->armed) {
        /* The window still open, if one is: what the worker carried, then
         * its end, at the close while a record condition is ON */
        if (r->in_window && !r->windows_q.empty()) {
            const ecuio_recorder::window w = r->windows_q.front();
            const uint64_t now = ecuio_record_clock_us();
            const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
            /* A window whose conditions cleared ends at its last record,
             * one still ON at the close; the carry goes in up to its end */
            uint64_t end_host = w.open_end ? host_us : r->win_last_host;
            for (uint32_t k = 0; k < r->carry_count; ++k) {
                const event_body& e = r->carry[r->carry_start + k];
                if (w.open_end || e.t_us <= w.end_us) {
                    r->take_event(e);
                    end_host = (std::max)(end_host, e.host_us);
                }
            }
            if (w.open_end) {
                if (r->carry_lost > 0) {
                    r->lost_line(r->carry_lost_us, r->carry_lost);
                }
                if (r->link_pending) {
                    r->event_line(r->pending_link.host_us, "link",
                                  link_text(r->pending_link.state,
                                            r->pending_link.episodes));
                }
                if (r->pending_dropped > 0) {
                    r->drop_line(end_host, r->pending_dropped);
                    r->win_dropped += r->pending_dropped;
                }
            }
            r->close_window(
                end_host,
                w.open_end ? one_line(utf8_why != nullptr ? utf8_why : "closed")
                           : std::string(kWindowEnded));
        }
    } else if (r->attached.load()) {
        std::string why = one_line(utf8_why != nullptr ? utf8_why : "closed");
        const uint64_t now = ecuio_record_clock_us();
        const uint64_t host_us = now > r->zero_us ? now - r->zero_us : 0;
        /* What the worker couldn't queue before it let go: the detach
         * waited for it, so its fields are this thread's now */
        for (uint32_t k = 0; k < r->carry_count; ++k) {
            r->take_event(r->carry[r->carry_start + k]);
        }
        if (r->carry_lost > 0) {
            r->lost_line(r->carry_lost_us, r->carry_lost);
        }
        if (r->link_pending) {
            r->event_line(
                r->pending_link.host_us, "link",
                link_text(r->pending_link.state, r->pending_link.episodes));
        }
        if (r->pending_dropped > 0) {
            r->drop_line(host_us, r->pending_dropped);
        }
        r->event_out += '\n';
        r->event_line(
            host_us, "end",
            why + ": " + std::to_string(r->frame_rows.load()) + " frames, " +
                std::to_string(r->value_rows.load()) + " values, " +
                std::to_string(r->records_dropped.load()) +
                " records dropped, " + std::to_string(r->notes_dropped.load()) +
                " notes dropped");
        r->write(r->events, r->event_out);
        r->write(r->markers, r->marker_out);
    }
    r->sync_all();
    r->close_files();
    return r->failed() ? ECUIO_ERR_RECORDING_FAILED : ECUIO_OK;
}

void ecuio_session_test_before_record_attach(void (*hook)(void* user),
                                             void* user)
{
    /* The user before the hook, and a clear leaves the user: a reader
     * that took the old hook still finds its own */
    if (hook != nullptr) {
        g_before_record_attach_user.store(user);
    }
    g_before_record_attach.store(hook);
}

void ecuio_recorder_test_hold(ecuio_recorder_t* recorder, int hold)
{
    if (recorder != nullptr) {
        recorder->hold.store(hold != 0, std::memory_order_release);
    }
}

void ecuio_recorder_test_fail_writes(ecuio_recorder_t* recorder)
{
    if (recorder != nullptr) {
        recorder->fail_next.store(true, std::memory_order_release);
    }
}

void ecuio_recorder_test_add_slept(ecuio_recorder_t* recorder, uint64_t us)
{
    if (recorder != nullptr) {
        recorder->slept_added.fetch_add(us, std::memory_order_relaxed);
    }
}

void ecuio_recorder_test_set_clock(int on, uint64_t now_us)
{
    g_test_clock_us.store(now_us, std::memory_order_release);
    g_test_clock_on.store(on != 0, std::memory_order_release);
}

uint64_t ecuio_recorder_test_wakes(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->wakes.load(std::memory_order_relaxed)
                               : 0;
}

uint64_t ecuio_recorder_test_syncs(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->syncs.load(std::memory_order_relaxed)
                               : 0;
}

} // extern "C"
