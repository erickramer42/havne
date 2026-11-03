// File/layer: core/src/recorder.cpp | C++17, Qt-free; the recorder
//             (ADR-0049 sections 1 to 4)
// Testing: HARNESS: RecorderTest (test_core): the mock and the replay
//          fixture recorded, the CSV files against the app's
//          (BridgeTest's round trip), the seams in internal.h for drops,
//          failed writes, a held writer and syncs, the statuses; link
//          transitions and reply timing with the test as the worker, and
//          through the fake ELM327 (Elm327SessionTest.ARecordingHolds*)
//
// The worker copies each frame and its values, its link transitions and
// its reply timing into a byte ring (single producer, single consumer);
// a writer thread of the recorder's own drains it into frames.csv,
// signals.csv, events.log's link lines and latency.csv in ADR-0017's
// rows, and the caller's notes and marks, which come through a queue of
// their own, into events.log and markers.csv. The writer
// never holds the session: what it writes reached it through the ring
// or the queue, or was copied at attach.

#include "recorder.h"

#include "error_detail.h"
#include "session.h"

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

constexpr uint64_t kTimingEveryUs = 10000000;
constexpr auto kFinalTimingWait = std::chrono::seconds(1);
constexpr auto kFinalTimingRetry = std::chrono::milliseconds(10);

/* A record in the ring: this head, then its body, which starts with its
 * host_us. A frame's body is frame_body, the frame's used bytes, then
 * each value as value_head followed by its double or its text; a link
 * transition's is link_body; reply timing's is timing_body, then its
 * rows. */
struct record_head {
    uint32_t size; /* the whole record, head included */
    uint32_t type;
    uint64_t dropped_before; /* frames the ring had no room for */
};
constexpr uint32_t kRecordFrame = 1;
constexpr uint32_t kRecordLink = 2;
constexpr uint32_t kRecordTiming = 3;

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
    uint32_t pad;
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

/* What the caller's threads hand the writer */
enum class item_kind { begin, note, mark };
struct item {
    item_kind kind;
    uint64_t host_us;
    std::string text;
};

bool is_text_kind(uint8_t kind)
{
    return kind == ECUIO_VALUE_TEXT || kind == ECUIO_VALUE_HEX;
}

/* A CSV field, quoted only when it has to be, as the app's recorder
 * quotes it */
std::string csv_quoted(const char* text, size_t len)
{
    std::string f(text, len);
    if (f.find_first_of(",\"\n") == std::string::npos) {
        return f;
    }
    std::string q = "\"";
    for (char c : f) {
        if (c == '"')
            q += '"';
        q += c;
    }
    q += '"';
    return q;
}

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

/* The value to 12 significant digits (the app's 'g', 12), with a '.'
 * whatever the process's locale: a Qt program sets LC_NUMERIC from the
 * environment, where the decimal point may be a comma */
void append_value(std::string& out, double v)
{
    char buf[40];
    const int n = std::snprintf(buf, sizeof buf, "%.12g", v);
    if (n <= 0)
        return;
    const char* point = std::localeconv()->decimal_point;
    const char p = (point != nullptr && point[0] != '\0') ? point[0] : '.';
    for (int i = 0; i < n; ++i) {
        out += (buf[i] == p) ? '.' : buf[i];
    }
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
        if (c == '\r' || c == '\n')
            c = ' ';
    }
    return s;
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
        if (ecuio_wide_to_utf8(text, buf, sizeof buf))
            out = buf;
    }
    if (text != nullptr)
        LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty())
        out = "Windows error " + std::to_string(code);
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

/* Creates every missing folder of root; false with the OS's reason */
bool make_root(const std::string& root, std::string* why)
{
    for (size_t i = 1; i <= root.size(); ++i) {
        if (i < root.size() && root[i] != '/' && root[i] != '\\')
            continue;
        const std::string prefix = root.substr(0, i);
        if (prefix.empty() || prefix.back() == ':')
            continue;
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
    if (CreateDirectoryW(w.c_str(), nullptr))
        return true;
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
    if (n == 0 || n >= 4096)
        return path;
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] == L'\\')
            buf[i] = L'/';
    }
    char out[4096 * 3];
    return ecuio_wide_to_utf8(buf, out, sizeof out) ? std::string(out) : path;
#else
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) != nullptr ? std::string(buf) : path;
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
    if (has)
        append_u64(out, us);
}

bool sync_file(FILE* f)
{
    if (f == nullptr)
        return true;
    if (std::fflush(f) != 0)
        return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

} // namespace

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

    /* Fixed at attach */
    std::shared_ptr<ecuio_record_link> link;
    std::vector<def_entry> defs; /* sorted by id */
    uint32_t rx_ids[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    uint32_t channels = 0;
    std::chrono::system_clock::time_point start_wall{};
    std::atomic<bool> attached{false};
    std::atomic<bool> detached{false};

    std::mutex note_m;
    std::deque<item> items;
    uint32_t note_cap = kDefaultNoteLines;
    std::atomic<uint64_t> notes_dropped{0};

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

    /* The writer's own */
    std::vector<uint8_t> scratch;
    std::string frame_out, value_out, event_out, marker_out, latency_out;
    /* Each request's answers, refusals and timeouts at its last row, so
     * latency.csv gets only the rows that changed */
    uint64_t timing_totals[ECUIO_PROFILE_MAX_REQUESTS] = {};

    bool failed() const
    {
        return state.load(std::memory_order_acquire) == ECUIO_RECORDING_FAILED;
    }

    void fail(int err)
    {
        {
            std::lock_guard<std::mutex> g(reason_m);
            if (failed())
                return;
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
        const auto at = start_wall + std::chrono::microseconds(host_us);
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

    void take_link(const uint8_t* body, size_t size)
    {
        if (size < sizeof(link_body))
            return;
        link_body lb;
        std::memcpy(&lb, body, sizeof lb);
        event_line(lb.host_us, "link", link_text(lb.state, lb.episodes));
    }

    /* latency.csv's rows whose counts changed since their last row
     * (ADR-0017's appendix of 2026-10-31), the file made with the first */
    void take_timing(const uint8_t* body, size_t size)
    {
        if (size < sizeof(timing_body))
            return;
        timing_body tb;
        std::memcpy(&tb, body, sizeof tb);
        const ecuio_session_timing_t& t = tb.timing;
        const uint64_t ms = (t.snapshot_us + 500) / 1000;
        char at[32];
        std::snprintf(at, sizeof at, "%llu.%03llu",
                      static_cast<unsigned long long>(ms / 1000),
                      static_cast<unsigned long long>(ms % 1000));
        static const char kHex[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < tb.rows && i < ECUIO_PROFILE_MAX_REQUESTS;
             ++i) {
            const size_t off = sizeof tb + i * sizeof(ecuio_request_timing_t);
            if (off + sizeof(ecuio_request_timing_t) > size)
                break;
            ecuio_request_timing_t r;
            std::memcpy(&r, body + off, sizeof r);
            const uint64_t total =
                uint64_t{r.answered} + r.refused + r.timeouts;
            if (total == timing_totals[i])
                continue;
            timing_totals[i] = total;
            const bool latency_has = r.answered + r.refused > 0;
            const bool interval_has = r.intervals > 0;
            latency_out += at;
            latency_out += ',';
            append_hex(latency_out,
                       r.channel < channels ? rx_ids[r.channel] : 0);
            latency_out += ',';
            for (uint8_t k = 0; k < r.length && k < 3; ++k) {
                if (k > 0)
                    latency_out += ' ';
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
            latency_out += clock_name(t.interval_clock);
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
        if (size < sizeof(frame_body))
            return;
        frame_body fb;
        std::memcpy(&fb, body, sizeof fb);
        size_t at = sizeof fb;
        if (at + fb.length > size)
            return;
        append_u64(frame_out, fb.host_us);
        frame_out += ',';
        append_u64(frame_out, fb.timestamp_us);
        frame_out += ',';
        append_hex(frame_out, fb.id);
        frame_out += ',';
        append_hex(frame_out, fb.flags);
        frame_out += ',';
        static const char kHex[] = "0123456789ABCDEF";
        for (uint16_t i = 0; i < fb.length; ++i) {
            if (i > 0)
                frame_out += ' ';
            const uint8_t b = body[at + i];
            frame_out += kHex[b >> 4];
            frame_out += kHex[b & 0x0F];
        }
        frame_out += '\n';
        at += fb.length;
        frame_rows.fetch_add(1, std::memory_order_relaxed);

        for (uint16_t v = 0; v < fb.values; ++v) {
            if (at + sizeof(value_head) > size)
                return;
            value_head vh;
            std::memcpy(&vh, body + at, sizeof vh);
            at += sizeof vh;
            const def_entry* d = def(vh.signal_id);
            const bool text = is_text_kind(vh.kind);
            const size_t len = text ? vh.text_len : sizeof(double);
            if (at + len > size)
                return;
            if (d != nullptr) {
                append_u64(value_out, fb.host_us);
                value_out += ',';
                value_out += d->name;
                value_out += ',';
                if (text) {
                    value_out += csv_quoted(
                        reinterpret_cast<const char*>(body + at), len);
                } else {
                    double x;
                    std::memcpy(&x, body + at, sizeof x);
                    append_value(value_out, x);
                }
                value_out += ',';
                value_out += d->unit;
                value_out += '\n';
                value_rows.fetch_add(1, std::memory_order_relaxed);
            }
            at += len;
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
            if (rh.size < sizeof rh || rh.size > h - t)
                break;
            const size_t body = rh.size - sizeof rh;
            if (scratch.size() < body)
                scratch.resize(body);
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
                event_out += it.text;
                frame_out += "host_us,timestamp_us,id,flags,data\n";
                /* A byte-order mark, so a spreadsheet reads the units'
                 * UTF-8 as UTF-8 (ADR-0017) */
                value_out += "\xEF\xBB\xBFhost_us,signal,value,unit\n";
                break;
            case item_kind::note:
                event_line(it.host_us, "note", it.text);
                break;
            case item_kind::mark:
                if (markers == nullptr && !failed()) {
                    markers = open_file(folder + "/markers.csv");
                    if (markers == nullptr) {
                        fail(errno != 0 ? errno : EIO);
                        break;
                    }
                    marker_out += "host_us,label\n";
                }
                append_u64(marker_out, it.host_us);
                marker_out += ',';
                marker_out += csv_quoted(it.text.data(), it.text.size());
                marker_out += '\n';
                break;
            }
        }
        write(frames, frame_out);
        write(signals, value_out);
        write(events, event_out);
        write(markers, marker_out);
        return !taken.empty();
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
            if (hold.load(std::memory_order_acquire) && !stopping) {
                continue;
            }
            /* The items first: the header comes ahead of any row */
            const bool notes = drain_items();
            const bool frames_taken = drain_ring();
            busy = notes || frames_taken;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= kFlushEvery) {
                flush_all();
                last_flush = now;
            }
            if (now - last_sync >= kSyncEvery) {
                sync_all();
                last_sync = now;
            }
            if (stopping)
                return;
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

    ~ecuio_recorder() { close_files(); }
};

void ecuio_record_detach(ecuio_record_link& link)
{
    std::lock_guard<std::mutex> g(link.m);
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
    if (!r->link_pending)
        return true;
    const uint64_t size = sizeof(record_head) + sizeof(link_body);
    if (!ring_has_room(r, size))
        return false;
    const uint64_t h = r->head.load(std::memory_order_relaxed);
    const record_head rh{static_cast<uint32_t>(size), kRecordLink, 0};
    r->copy_in(h, &rh, sizeof rh);
    r->copy_in(h + sizeof rh, &r->pending_link, sizeof r->pending_link);
    r->head.store(h + size, std::memory_order_release);
    r->link_pending = false;
    return true;
}

uint64_t host_us_now(const ecuio_record_link& link)
{
    const uint64_t now = ecuio_clock_us();
    return now > link.zero_us ? now - link.zero_us : 0;
}

} // namespace

void ecuio_record_link_state(ecuio_record_link* link, int32_t state,
                             uint32_t episodes)
{
    if (link == nullptr)
        return;
    ecuio_recorder_t* r = ecuio_record_pin(*link);
    if (r == nullptr)
        return;
    /* A transition still carried gives way to this one: the latest state
     * and count are what a reader needs */
    r->pending_link = link_body{host_us_now(*link), state, episodes};
    r->link_pending = true;
    put_pending_link(r);
    ecuio_record_unpin(*link);
}

void ecuio_record_timing(ecuio_record_link* link,
                         const ecuio_session_timing_t& timing,
                         const ecuio_request_timing_t* rows, uint32_t count,
                         bool final)
{
    if (link == nullptr)
        return;
    const uint32_t n = (std::min)(count, uint32_t{ECUIO_PROFILE_MAX_REQUESTS});
    const uint64_t size = sizeof(record_head) + sizeof(timing_body) +
                          uint64_t{n} * sizeof(ecuio_request_timing_t);
    const auto give_up = std::chrono::steady_clock::now() + kFinalTimingWait;
    for (;;) {
        ecuio_recorder_t* r = ecuio_record_pin(*link);
        if (r == nullptr)
            return;
        if (!final && timing.snapshot_us < r->timing_next_us) {
            ecuio_record_unpin(*link);
            return;
        }
        if (put_pending_link(r) && ring_has_room(r, size)) {
            const uint64_t h = r->head.load(std::memory_order_relaxed);
            const record_head rh{static_cast<uint32_t>(size), kRecordTiming,
                                 r->pending_dropped};
            r->pending_dropped = 0;
            const timing_body tb{host_us_now(*link), timing, n, 0};
            r->copy_in(h, &rh, sizeof rh);
            r->copy_in(h + sizeof rh, &tb, sizeof tb);
            r->copy_in(h + sizeof rh + sizeof tb, rows,
                       n * sizeof(ecuio_request_timing_t));
            r->head.store(h + size, std::memory_order_release);
            r->timing_next_us = timing.snapshot_us + kTimingEveryUs;
            ecuio_record_unpin(*link);
            return;
        }
        /* No room: a snapshot every 10 s is dropped and counted once, and
         * the next one's totals cover it; the final one waits for the
         * writer, unpinned, so a detach meanwhile never waits on it */
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

void ecuio_recorder_put_frame(ecuio_recorder_t* r, uint64_t host_us,
                              const ecuio_frame_t* frame,
                              const ecuio_value_t* values, uint32_t count)
{
    const uint16_t length = frame->data_length <= ECUIO_FRAME_DATA_MAX
                                ? frame->data_length
                                : static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX);
    const uint16_t nvalues =
        count > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(count);
    uint64_t size = sizeof(record_head) + sizeof(frame_body) + length;
    for (uint16_t i = 0; i < nvalues; ++i) {
        size += sizeof(value_head) + (is_text_kind(values[i].kind)
                                          ? values[i].text_len
                                          : sizeof(double));
    }
    /* A carried link transition goes first, or the frame waits behind
     * it, dropped */
    if (!put_pending_link(r) || !ring_has_room(r, size)) {
        ++r->pending_dropped;
        r->records_dropped.fetch_add(1, std::memory_order_relaxed);
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
                  0};
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
    r->head.store(h + size, std::memory_order_release);
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
    if (options->flags != 0 || options->reserved[0] != 0 ||
        options->reserved[1] != 0 || options->reserved[2] != 0 ||
        options->reserved[3] != 0 || options->root_utf8[0] == '\0') {
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

    const std::time_t now = std::time(nullptr);
    const std::tm tm = local_tm(now);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    const std::string label =
        options->label_utf8 != nullptr ? reduce_label(options->label_utf8) : "";
    const std::string tail = label.empty() ? "" : " " + label;
    std::string leaf;
    for (int n = 1;; ++n) {
        const std::string name =
            std::string(stamp) + (n > 1 ? "-" + std::to_string(n) : "") + tail;
        bool taken = false;
        if (make_leaf(root + "/" + name, &taken, &why)) {
            leaf = root + "/" + name;
            break;
        }
        if (!taken || n >= 1000) {
            ecuio_detail_set("can't create the recording's folder: %s",
                             taken ? "every name for this second is taken"
                                   : why.c_str());
            return ECUIO_ERR_RECORDING_FAILED;
        }
    }
    r->folder = absolute(leaf);

    r->frames = open_file(leaf + "/frames.csv");
    r->signals = open_file(leaf + "/signals.csv");
    r->events = open_file(leaf + "/events.log");
    if (r->frames == nullptr || r->signals == nullptr || r->events == nullptr) {
        const int e = errno;
        ecuio_detail_set("can't create the recording's files: %s",
                         e != 0 ? errno_text(e).c_str()
                                : "the folder refused them");
        return ECUIO_ERR_RECORDING_FAILED;
    }

    /* The caller's header lines, under one key, written at attach */
    std::string caller;
    for (uint32_t i = 0; i < options->header_line_count; ++i) {
        if (options->header_lines[i] != nullptr) {
            caller += "caller  " + one_line(options->header_lines[i]) + "\n";
        }
    }
    r->items.push_back(item{item_kind::begin, 0, caller});

    try {
        r->writer = std::thread([rec = r.get()] { rec->run(); });
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = r.release();
    return ECUIO_OK;
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
    if (session->started || session->record_link != nullptr ||
        recorder->attached.load()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    std::shared_ptr<ecuio_record_link> link;
    std::vector<def_entry> defs;
    std::string header;
    try {
        link = std::make_shared<ecuio_record_link>();
        const ecuio_signal_set_t* set = session->active_signal_set;
        header = "ecuio session recording, format 2 (ADR-0049)\n";
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
        link->zero_us = ecuio_clock_us();
        recorder->start_wall = std::chrono::system_clock::now();

        const std::time_t t =
            std::chrono::system_clock::to_time_t(recorder->start_wall);
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                recorder->start_wall.time_since_epoch())
                .count() %
            1000;
        const std::tm lt = local_tm(t);
        const std::tm ut = utc_tm(t);
        char buf[96];
        std::snprintf(
            buf, sizeof buf,
            "started  %04d-%02d-%02d %02d:%02d:%02d.%03lld (local time)\n",
            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
            lt.tm_sec, ms);
        header += buf;
        std::snprintf(buf, sizeof buf,
                      "utc  %04d-%02d-%02dT%02d:%02d:%02d.%03lldZ\n",
                      ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday, ut.tm_hour,
                      ut.tm_min, ut.tm_sec, ms);
        header += buf;
        header += std::string("core  ") + ecuio_version_string() + "\n";
        header += "uri  " + one_line(session->uri.c_str()) + "\n";
        header += std::string("profile  ") + session->info.profile + "\n";
        /* A replay's frames carry its source's flags, which a recording
         * from before ADR-0049 never marked reassembled */
        header += session->info.backend == ECUIO_BACKEND_REPLAY
                      ? "frame flags  from an older recording\n"
                      : "frame flags  complete\n";
        for (uint32_t i = 0;
             set != nullptr && set->defs != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            header += "signal  " +
                      csv_quoted(d.name, strnlen(d.name, sizeof d.name)) +
                      (is_text_kind(d.kind) ? ",text," : ",number,") +
                      csv_quoted(d.unit, strnlen(d.unit, sizeof d.unit)) + "," +
                      std::to_string(d.decimal_places) + "," +
                      std::to_string(d.plot_rank) + "\n";
        }
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    {
        std::lock_guard<std::mutex> g(recorder->note_m);
        /* The caller's lines open() queued, then the columns */
        item& begin = recorder->items.front();
        begin.text = header + begin.text +
                     "columns  local time, seconds since the recording started "
                     "(host_us / 1e6 in the CSV files), kind, text\n\n";
    }
    recorder->defs = std::move(defs);
    recorder->channels = (std::min)(session->info.channel_count,
                                    uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS});
    for (uint32_t i = 0; i < recorder->channels; ++i) {
        recorder->rx_ids[i] = session->info.rx_ids[i];
    }
    recorder->link = link;
    session->record_link = link;
    link->active.store(recorder, std::memory_order_release);
    recorder->attached.store(true, std::memory_order_release);
    return ECUIO_OK;
}

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
    const uint64_t now = ecuio_clock_us();
    const uint64_t host_us =
        now > recorder->link->zero_us ? now - recorder->link->zero_us : 0;
    try {
        std::string line = one_line(utf8_line);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::note, host_us, std::move(line)});
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
    const uint64_t now = ecuio_clock_us();
    const uint64_t host_us =
        now > recorder->link->zero_us ? now - recorder->link->zero_us : 0;
    try {
        std::string label = one_line(utf8_label);
        std::lock_guard<std::mutex> g(recorder->note_m);
        if (recorder->items.size() >= recorder->note_cap) {
            recorder->notes_dropped.fetch_add(1, std::memory_order_relaxed);
            return ECUIO_OK;
        }
        recorder->items.push_back(
            item{item_kind::mark, host_us, std::move(label)});
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
    const uint32_t state = recorder->state.load(std::memory_order_acquire);
    out->state = state == ECUIO_RECORDING_FAILED ? ECUIO_RECORDING_FAILED
                 : recorder->detached.load(std::memory_order_acquire)
                     ? ECUIO_RECORDING_DETACHED
                     : ECUIO_RECORDING_ACTIVE;
    out->frame_rows = recorder->frame_rows.load(std::memory_order_relaxed);
    out->value_rows = recorder->value_rows.load(std::memory_order_relaxed);
    out->records_dropped =
        recorder->records_dropped.load(std::memory_order_relaxed);
    out->notes_dropped =
        recorder->notes_dropped.load(std::memory_order_relaxed);
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
    const std::string& f = recorder->folder;
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
        ecuio_record_detach(*r->link);
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
    if (r->attached.load()) {
        std::string why = one_line(utf8_why != nullptr ? utf8_why : "closed");
        const uint64_t now = ecuio_clock_us();
        const uint64_t host_us =
            now > r->link->zero_us ? now - r->link->zero_us : 0;
        /* What the worker couldn't queue before it let go: the detach
         * waited for it, so its fields are this thread's now */
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
    }
    r->sync_all();
    r->close_files();
    return r->failed() ? ECUIO_ERR_RECORDING_FAILED : ECUIO_OK;
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

uint64_t ecuio_recorder_test_syncs(const ecuio_recorder_t* recorder)
{
    return recorder != nullptr ? recorder->syncs.load(std::memory_order_relaxed)
                               : 0;
}

} // extern "C"
