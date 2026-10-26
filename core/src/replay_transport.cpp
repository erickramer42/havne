// File/layer: core/src/replay_transport.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT — CoreTest.Replay* (the reader's refusals with their line
//          numbers, either line ending, a whole-message payload, pacing at
//          speed 1 and 10, stamps rebased and never decreasing, the
//          backward-step fallback, loop and end of file, the query);
//          BridgeTest.ReplayOfAMockRecordingDecodesWhatWasRecorded (the
//          round trip through SessionRecorder);
//          CoreTest.ReplayOfTheVehicleFixtureDecodesTheSheetsValues
//
// replay:// (ADR-0023): a recording's frames.csv (ADR-0017) played back
// through the frame API. Attach reads the whole file once and refuses a
// bad row with its line number; the worker reads it again a row at a time
// and sends each frame when the recording's own session time, divided by
// the speed, has passed. Passive: no requests, no link health.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "error_detail.h"
#include "session.h"
#include "uri_query.h"

#ifdef _WIN32
#include "win_utf8.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

/* frames.csv's header row is its format version (ADR-0017) */
constexpr const char* kHeader = "host_us,timestamp_us,id,flags,data";
/* A whole-message row: two 20-digit times, an id and flags, and
 * ECUIO_FRAME_DATA_MAX bytes as spaced hex (3 characters a byte) */
constexpr size_t kLineMax = 64 + 3 * ECUIO_FRAME_DATA_MAX;
/* speed is kept in thousandths: 0.1 to 100 */
constexpr uint32_t kSpeedMin = 100;
constexpr uint32_t kSpeedMax = 100000;
constexpr uint32_t kSpeedDefault = 1000;

/* A row as the reader parsed it */
struct row {
    uint64_t host_us = 0;
    uint64_t timestamp_us = 0;
};

/* The step from one row to the next in session time: the recording's
 * timestamp_us, or its host_us when timestamp_us steps backwards (a raw
 * adapter clock wrapping, as j2534_clock.h stands the host in) */
uint64_t step_us(const row& prev, const row& cur)
{
    if (cur.timestamp_us >= prev.timestamp_us) {
        return cur.timestamp_us - prev.timestamp_us;
    }
    return cur.host_us >= prev.host_us ? cur.host_us - prev.host_us : 0;
}

FILE* open_utf8(const std::string& path)
{
#ifdef _WIN32
    std::vector<wchar_t> wide(path.size() + 1);
    if (!ecuio_utf8_to_wide(path.c_str(), wide.data(), wide.size())) {
        return nullptr;
    }
    FILE* f = nullptr;
    return _wfopen_s(&f, wide.data(), L"rb") == 0 ? f : nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

/* One line without its ending (LF or CRLF). False at the end of the file;
 * *too_long when the line doesn't fit kLineMax. */
bool read_line(FILE* f, std::vector<char>& buf, bool* too_long)
{
    *too_long = false;
    if (std::fgets(buf.data(), static_cast<int>(buf.size()), f) == nullptr) {
        return false;
    }
    size_t n = std::strlen(buf.data());
    if (n > 0 && buf[n - 1] == '\n') {
        buf[--n] = '\0';
    } else if (!std::feof(f)) {
        *too_long = true;
        return true;
    }
    if (n > 0 && buf[n - 1] == '\r') {
        buf[--n] = '\0';
    }
    return true;
}

bool parse_decimal(const char*& p, uint64_t* out)
{
    if (*p < '0' || *p > '9')
        return false;
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        const uint64_t d = static_cast<uint64_t>(*p - '0');
        if (v > (UINT64_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool parse_hex32(const char*& p, uint32_t* out)
{
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X'))
        return false;
    p += 2;
    uint32_t v = 0;
    int digits = 0;
    for (int d; (d = hex_digit(*p)) >= 0; ++p) {
        if (++digits > 8)
            return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    *out = v;
    return digits > 0;
}

/* One data row into *r and *frame. On failure, *why says which field. */
bool parse_row(const char* line, row* r, ecuio_frame_t* frame, const char** why)
{
    const char* p = line;
    if (!parse_decimal(p, &r->host_us) || *p++ != ',') {
        *why = "host_us isn't a decimal count";
        return false;
    }
    if (!parse_decimal(p, &r->timestamp_us) || *p++ != ',') {
        *why = "timestamp_us isn't a decimal count";
        return false;
    }
    if (!parse_hex32(p, &frame->arbitration_id) || *p++ != ',') {
        *why = "id isn't 0x-prefixed hex";
        return false;
    }
    if (!parse_hex32(p, &frame->flags) || *p++ != ',') {
        *why = "flags isn't 0x-prefixed hex";
        return false;
    }
    uint16_t n = 0;
    while (*p != '\0') {
        const int hi = hex_digit(p[0]);
        const int lo = hi >= 0 ? hex_digit(p[1]) : -1;
        if (lo < 0 || (p[2] != ' ' && p[2] != '\0') ||
            (p[2] == ' ' && p[3] == '\0')) {
            *why = "data isn't hex byte pairs separated by spaces";
            return false;
        }
        if (n == ECUIO_FRAME_DATA_MAX) {
            *why = "data is longer than a frame holds";
            return false;
        }
        frame->data[n++] = static_cast<uint8_t>((hi << 4) | lo);
        p += (p[2] == ' ') ? 3 : 2;
    }
    frame->data_length = n;
    frame->timestamp_us = r->timestamp_us;
    return true;
}

/* Reads the header row; false (with the detail set) when it isn't
 * frames.csv's */
bool read_header(FILE* f, std::vector<char>& buf)
{
    bool too_long = false;
    if (!read_line(f, buf, &too_long) || too_long) {
        ecuio_detail_set("the recording's frames.csv is empty");
        return false;
    }
    const char* line = buf.data();
    if (static_cast<unsigned char>(line[0]) == 0xEF) {
        line += 3; /* a byte-order mark, should an editor add one */
    }
    if (std::strcmp(line, kHeader) != 0) {
        ecuio_detail_set("not a QtTune frames.csv, or a newer format: the "
                         "header row isn't %s",
                         kHeader);
        return false;
    }
    return true;
}

/* The whole file, checked: every row parses. Gives the row count and the
 * session time of the last row. */
ecuio_status_t check_file(const std::string& path, uint64_t* rows,
                          uint64_t* span_us)
{
    FILE* f = open_utf8(path);
    if (f == nullptr) {
        ecuio_detail_set("couldn't open the recording's frames.csv");
        return ECUIO_ERR_LOAD_FAILED;
    }
    std::vector<char> buf(kLineMax + 2);
    if (!read_header(f, buf)) {
        std::fclose(f);
        return ECUIO_ERR_LOAD_FAILED;
    }
    std::unique_ptr<ecuio_frame_t> frame(new ecuio_frame_t());
    row prev, cur;
    uint64_t n = 0, span = 0, line_no = 1;
    bool too_long = false;
    while (read_line(f, buf, &too_long)) {
        ++line_no;
        const char* why = nullptr;
        if (too_long) {
            why = "the line is longer than any frame's row";
        } else if (!parse_row(buf.data(), &cur, frame.get(), &why)) {
            /* why is set */
        }
        if (why != nullptr) {
            ecuio_detail_set("frames.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            std::fclose(f);
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (n > 0)
            span += step_us(prev, cur);
        prev = cur;
        ++n;
    }
    std::fclose(f);
    if (n == 0) {
        ecuio_detail_set("the recording's frames.csv holds no frames");
        return ECUIO_ERR_LOAD_FAILED;
    }
    *rows = n;
    *span_us = span;
    return ECUIO_OK;
}

bool is_absolute(const std::string& p)
{
#ifdef _WIN32
    return ecuio_is_absolute_path(p.c_str());
#else
    return !p.empty() && p[0] == '/';
#endif
}

/* "0.1" to "100", or "max" (0): at most three decimals, no locale */
bool parse_speed(const ecuio_uri::param& p, uint32_t* milli)
{
    if (p.val_len == 3 && std::strncmp(p.val, "max", 3) == 0) {
        *milli = 0;
        return true;
    }
    uint64_t whole = 0, frac = 0;
    size_t i = 0, int_digits = 0, frac_digits = 0;
    for (; i < p.val_len && p.val[i] >= '0' && p.val[i] <= '9';
         ++i, ++int_digits) {
        whole = whole * 10 + static_cast<uint64_t>(p.val[i] - '0');
        if (whole > kSpeedMax)
            return false;
    }
    if (i < p.val_len && p.val[i] == '.') {
        for (++i; i < p.val_len && p.val[i] >= '0' && p.val[i] <= '9'; ++i) {
            if (++frac_digits > 3)
                return false;
            frac = frac * 10 + static_cast<uint64_t>(p.val[i] - '0');
        }
        if (frac_digits == 0)
            return false;
    }
    if (i != p.val_len || int_digits == 0)
        return false;
    for (size_t k = frac_digits; k < 3; ++k)
        frac *= 10;
    const uint64_t v = whole * 1000 + frac;
    if (v < kSpeedMin || v > kSpeedMax)
        return false;
    *milli = static_cast<uint32_t>(v);
    return true;
}

} // namespace

struct ecuio_replay_transport {
    ecuio_session_t* session = nullptr;
    std::string path;                     /* the frames.csv, UTF-8 */
    uint32_t speed_milli = kSpeedDefault; /* 0 = max */
    bool loop = false;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false; /* under mutex */
};

namespace {

/* Sends the file's frames, paced by their session time over the speed;
 * on loop=1 again from the top, its stamps carried on past the last */
void replay_worker(ecuio_replay_transport* t)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<char> buf(kLineMax + 2);
    std::unique_ptr<ecuio_frame_t> frame(new ecuio_frame_t());
    uint64_t base_us = 0; /* session time at the start of this pass */
    for (;;) {
        FILE* f = open_utf8(t->path);
        if (f == nullptr || !read_header(f, buf)) {
            if (f != nullptr)
                std::fclose(f);
            return; /* checked at attach; changed since */
        }
        row prev, cur;
        uint64_t pass_us = 0;
        bool first = true, too_long = false;
        while (read_line(f, buf, &too_long)) {
            const char* why = nullptr;
            *frame = ecuio_frame_t{};
            if (too_long || !parse_row(buf.data(), &cur, frame.get(), &why)) {
                std::fclose(f);
                return; /* checked at attach; changed since */
            }
            if (!first)
                pass_us += step_us(prev, cur);
            first = false;
            prev = cur;
            const uint64_t now_us = base_us + pass_us;
            frame->timestamp_us = now_us;
            {
                std::unique_lock<std::mutex> lock(t->mutex);
                if (t->speed_milli != 0) {
                    const auto due =
                        start + std::chrono::microseconds(now_us * 1000 /
                                                          t->speed_milli);
                    t->wake.wait_until(lock, due, [t] { return t->stop; });
                }
                if (t->stop) {
                    std::fclose(f);
                    return;
                }
            }
            ecuio_session_dispatch_callbacks(t->session, frame.get());
        }
        std::fclose(f);
        if (!t->loop) {
            return; /* the end of the file: the session stays open, silent */
        }
        base_us += pass_us;
    }
}

ecuio_status_t replay_start(ecuio_session_t* session, void* impl)
{
    (void)session;
    ecuio_replay_transport* t = static_cast<ecuio_replay_transport*>(impl);
    if (t == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return ECUIO_OK; /* already running: idempotent */
    }
    try {
        t->worker = std::thread(replay_worker, t);
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void replay_detach(ecuio_session_t* session, void* impl)
{
    (void)session;
    ecuio_replay_transport* t = static_cast<ecuio_replay_transport*>(impl);
    if (t == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(t->mutex);
        t->stop = true;
    }
    t->wake.notify_all();
    if (t->worker.joinable()) {
        t->worker.join();
    }
    delete t;
}

const ecuio_transport_ops_t kReplayTransportOps = {replay_start, replay_detach};

} // namespace

extern "C" {

const char* const ecuio_replay_query_keys[] = {"profile", "did", "speed",
                                               "loop", nullptr};

ecuio_status_t ecuio_replay_transport_attach(ecuio_session_t* session,
                                             const char* uri,
                                             const ecuio_uri_query* query)
{
    if (session->transport != nullptr) {
        return ECUIO_OK; /* idempotent */
    }
    uint32_t speed = kSpeedDefault;
    bool loop = false;
    for (size_t i = 0; i < query->count; ++i) { /* duplicates: last wins */
        const ecuio_uri::param& p = query->params[i];
        if (p.is("speed") && !parse_speed(p, &speed)) {
            ecuio_detail_set("?speed= takes a factor from 0.1 to 100, at most "
                             "three decimals, or max");
            return ECUIO_ERR_BAD_URI;
        }
        if (p.is("loop")) {
            if (p.val_len != 1 || (p.val[0] != '0' && p.val[0] != '1')) {
                ecuio_detail_set("?loop= takes 0 or 1");
                return ECUIO_ERR_BAD_URI;
            }
            loop = p.val[0] == '1';
        }
    }

    const char* body = uri + std::strlen("replay://");
    const char* q = std::strchr(body, '?');
    std::string path(body, q != nullptr ? static_cast<size_t>(q - body)
                                        : std::strlen(body));
    if (path.empty()) {
        ecuio_detail_set(
            "replay:// needs a recording's folder or its frames.csv");
        return ECUIO_ERR_BAD_URI;
    }
    if (!is_absolute(path)) {
        ecuio_detail_set("the recording's path must be absolute, like "
#ifdef _WIN32
                         "C:\\path\\to\\recording"
#else
                         "/path/to/recording"
#endif
        );
        return ECUIO_ERR_BAD_URI;
    }
    /* A path ending in .csv is the file; anything else is the folder */
    const bool csv =
        path.size() >= 4 && (path.compare(path.size() - 4, 4, ".csv") == 0 ||
                             path.compare(path.size() - 4, 4, ".CSV") == 0);
    if (!csv) {
        while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
            path.pop_back();
        }
        path += "/frames.csv";
    }

    uint64_t rows = 0, span_us = 0;
    const ecuio_status_t checked = check_file(path, &rows, &span_us);
    if (checked != ECUIO_OK) {
        return checked;
    }

    try {
        std::unique_ptr<ecuio_replay_transport> impl(
            new ecuio_replay_transport());
        impl->session = session;
        impl->path = std::move(path);
        impl->speed_milli = speed;
        impl->loop = loop;
        session->transport =
            new ecuio_transport_t{&kReplayTransportOps, impl.get()};
        impl.release();
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    session->info.backend = ECUIO_BACKEND_REPLAY;
    session->info.replay_frames =
        rows > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rows);
    const uint64_t span_ms = span_us / 1000;
    session->info.replay_span_ms =
        span_ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(span_ms);
    session->info.replay_speed_milli = speed;
    if (loop) {
        session->info.flags |= ECUIO_SESSION_INFO_REPLAY_LOOP;
    }
    return ECUIO_OK;
}

} // extern "C"
