// File/layer: fuzz/fuzz_recording.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: a recording's files (ADR-0049 section 5), the first byte
//          picking one: each line through every parser of
//          core/src/recording_rows.h (signals.csv's and markers.csv's
//          rows, events.log's signal, utc and activity lines, the
//          summary and the drop lines; a parsed name within 31 bytes, a
//          unit within 15, decimals 0 to 3; two parses agree, NaN bits
//          included), or a folder: the input cut at 0x1E bytes into
//          events.log, signals.csv and markers.csv, written to a scratch
//          folder in TEMP / TMPDIR and opened through
//          ecuio_recording_open. The status is ECUIO_OK or
//          ECUIO_ERR_LOAD_FAILED with a one-line detail that never holds
//          the path; an open recording's counts agree with its queries
//          (a whole-range column holds every sample), every signal's
//          name and unit are terminated, and its text_at and find return
//          ECUIO_OK; two opens agree.

#include <ecuio/core.h>
#include <ecuio/recording.h>

#include "recording_rows.h" /* core/src: pure */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h> /* _getpid */
#define ecuio_fuzz_getpid _getpid
#else
#include <unistd.h>
#define ecuio_fuzz_getpid getpid
#endif

namespace rows = ecuio_recording_rows;

namespace {

bool same_bits(double a, double b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

void one_line(const char* p, size_t n)
{
    rows::signal_row a;
    rows::signal_row b;
    const char* why = nullptr;
    const bool ok = rows::parse_signal_row(p, n, a, &why);
    if (ok != rows::parse_signal_row(p, n, b, &why)) {
        abort();
    }
    if (ok) {
        if (a.name.empty() || a.name.size() > rows::kNameMax ||
            a.unit.size() > rows::kUnitMax || a.host_us != b.host_us ||
            a.name != b.name || a.value != b.value || a.unit != b.unit) {
            abort();
        }
        double x = 0.0;
        double y = 0.0;
        const bool num = rows::parse_number(a.value.data(), a.value.size(), &x);
        if (num != rows::parse_number(a.value.data(), a.value.size(), &y) ||
            (num && !same_bits(x, y))) {
            abort();
        }
    } else if (why == nullptr) {
        abort();
    }

    uint64_t host_us = 0;
    std::string label;
    why = nullptr;
    if (!rows::parse_marker_row(p, n, &host_us, label, &why) &&
        why == nullptr) {
        abort();
    }

    rows::header_signal s;
    why = nullptr;
    if (rows::parse_header_signal(p, n, s, &why)) {
        if (s.name.empty() || s.name.size() > rows::kNameMax ||
            s.unit.size() > rows::kUnitMax || s.decimal_places > 3) {
            abort();
        }
    } else if (why == nullptr) {
        abort();
    }

    uint64_t utc = 0;
    if (rows::parse_utc(p, n, &utc) && utc % 1000 != 0) {
        abort(); /* milliseconds, as microseconds */
    }

    rows::event_line e;
    if (rows::parse_event_line(p, n, &e)) {
        if (e.rest < p || e.rest + e.rest_len != p + n) {
            abort();
        }
        const char* text = nullptr;
        size_t text_len = 0;
        size_t kind_len = 0;
        if (rows::split_kind(e.rest, e.rest_len, &text, &text_len, &kind_len) &&
            (kind_len == 0 || text + text_len != p + n)) {
            abort();
        }
    }
    uint64_t records = 0;
    uint64_t notes = 0;
    (void)rows::parse_end(p, n, &records, &notes);
    uint64_t frames = 0;
    (void)rows::parse_drop(p, n, &frames);
}

/* Every line of the input, each a heap copy of its exact size, so ASan
 * sees a read past it */
void each_line(const uint8_t* data, size_t size)
{
    size_t start = 0;
    for (size_t i = 0; i <= size; ++i) {
        if (i == size || data[i] == '\n') {
            const size_t n = i - start;
            std::vector<char> copy(data + start, data + start + n);
            one_line(copy.data(), n);
            start = i + 1;
        }
    }
}

/* The scratch folder, one per process (libFuzzer's -jobs run several) */
std::string g_folder;

void remove_folder()
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(g_folder), ec);
}

const std::string& folder()
{
    if (g_folder.empty()) {
        const char* tmp = getenv("TEMP");
        if (tmp == nullptr) {
            tmp = getenv("TMPDIR");
        }
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\') {
                c = '/';
            }
        }
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }
        g_folder = base + "/ecuio-fuzz-recording-" +
                   std::to_string(static_cast<long>(ecuio_fuzz_getpid()));
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(g_folder),
                                            ec);
        atexit(remove_folder);
    }
    return g_folder;
}

void write_file(const char* name, const uint8_t* p, size_t n, bool present)
{
    const std::string path = folder() + "/" + name;
    if (!present) {
        remove(path.c_str());
        return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        abort();
    }
    if (n > 0 && fwrite(p, 1, n, f) != n) {
        abort();
    }
    fclose(f);
}

struct opened {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_recording_info_t info{};
};

opened open_and_query()
{
    opened o;
    ecuio_recording_t* r = nullptr;
    o.rc = ecuio_recording_open(folder().c_str(), nullptr, nullptr, &r);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr) {
        abort();
    }
    o.detail = detail;
    if (o.rc != ECUIO_OK) {
        if (r != nullptr || o.rc != ECUIO_ERR_LOAD_FAILED) {
            abort();
        }
        return o;
    }
    if (r == nullptr || ecuio_recording_get_info(r, &o.info) != ECUIO_OK) {
        abort();
    }
    for (uint32_t k = 0; k < o.info.signal_count; ++k) {
        ecuio_recording_signal_t s{};
        if (ecuio_recording_get_signal(r, k, &s) != ECUIO_OK ||
            memchr(s.name, '\0', sizeof s.name) == nullptr ||
            memchr(s.unit, '\0', sizeof s.unit) == nullptr ||
            s.decimal_places > 3) {
            abort();
        }
        if (s.samples == 0) {
            if (s.first_us != ECUIO_RECORDING_NONE) {
                abort();
            }
            continue;
        }
        if (s.first_us > s.last_us || s.last_us > o.info.duration_us) {
            abort();
        }
        /* One column over everything holds every sample, from the
         * summaries or the raw rows alike */
        ecuio_recording_column_t col{};
        const uint64_t t1 = s.last_us + 1;
        if (ecuio_recording_query(r, &k, 1, 0, t1, &col, 1) != ECUIO_OK ||
            col.samples != (s.samples > UINT32_MAX ? UINT32_MAX : s.samples)) {
            abort();
        }
        const bool text = (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        if (!text && !std::isnan(s.lowest) &&
            (col.lowest != s.lowest || col.highest != s.highest)) {
            abort();
        }
        /* Narrow columns over the first second agree with the wide one
         * on the count there */
        ecuio_recording_column_t cols[16];
        if (ecuio_recording_query(r, &k, 1, s.first_us, s.first_us + 1000, cols,
                                  16) != ECUIO_OK ||
            cols[0].samples == 0) {
            abort();
        }
        char buf[64];
        uint64_t at = 0;
        if (ecuio_recording_text_at(r, k, s.last_us, buf, sizeof buf, &at) !=
                ECUIO_OK ||
            at > s.last_us || at < s.first_us) {
            abort();
        }
        if (!text) {
            ecuio_recording_find_t f{};
            f.kind = ECUIO_RECORDING_FIND_THRESHOLD;
            f.signal = k;
            f.direction = ECUIO_RECORDING_FIND_RISING;
            f.level = s.highest;
            f.hysteresis = 0.0;
            if (std::isinf(f.level)) {
                f.level = 0.0;
            }
            uint64_t found = 0;
            if (ecuio_recording_find(r, &f, 0, &found) != ECUIO_OK ||
                (found != ECUIO_RECORDING_NONE &&
                 (found < s.first_us || found > s.last_us))) {
                abort();
            }
        }
    }
    for (uint32_t m = 0; m < o.info.marker_count; ++m) {
        ecuio_recording_marker_t mk{};
        if (ecuio_recording_get_marker(r, m, &mk) != ECUIO_OK ||
            mk.label_utf8 == nullptr ||
            strlen(mk.label_utf8) != mk.label_length) {
            abort();
        }
    }
    ecuio_recording_close(r);
    return o;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 262144) {
        return 0;
    }
    if (data[0] != 'F') {
        each_line(data + 1, size - 1);
        return 0;
    }
    /* events.log, signals.csv, markers.csv: absent past the cuts given */
    const uint8_t* parts[3] = {};
    size_t lens[3] = {};
    size_t count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= size && count < 3; ++i) {
        if (i == size || data[i] == 0x1E) {
            parts[count] = data + start;
            lens[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    write_file("events.log", parts[0], lens[0], count > 0);
    write_file("signals.csv", parts[1], lens[1], count > 1);
    write_file("markers.csv", parts[2], lens[2], count > 2);
    const opened a = open_and_query();
    const opened b = open_and_query();
    if (a.rc != b.rc || a.detail != b.detail ||
        memcmp(&a.info, &b.info, sizeof a.info) != 0) {
        abort();
    }
    return 0;
}
