// File/layer: core/src/frames_csv.h | internal, C++17, Qt-free, header-only;
//             ALL platforms
// Testing: UNIT: CoreTest.Replay* (replay_transport.cpp's reader: the
//          refusals with their line numbers, either line ending, a
//          whole-message payload, a torn last row dropped);
//          SimRecordingTest (test_obd2: the simulated ECU's recording,
//          sim_recording.h, a torn last row dropped); FUZZ:
//          fuzz_replay_file
//
// Reading a recording's frames.csv (ADR-0017): its header row, one row at
// a time into a frame, and the recording's path as fopen takes it. Shared
// by replay:// (replay_transport.cpp) and the simulated ECU's recording
// (sim_recording.h), so both read a file the same way.

#ifndef ECUIO_FRAMES_CSV_H
#define ECUIO_FRAMES_CSV_H

#include "ecuio/core.h"
#include "error_detail.h"

#ifdef _WIN32
#include "win_utf8.h"
#include <share.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ecuio_frames_csv {

/* frames.csv's header row is its format version (ADR-0017) */
constexpr const char* kHeader = "host_us,timestamp_us,id,flags,data";
/* A whole-message row: two 20-digit times, an id and flags, and
 * ECUIO_FRAME_DATA_MAX bytes as spaced hex (3 characters a byte) */
constexpr size_t kLineMax = 64 + 3 * ECUIO_FRAME_DATA_MAX;

/* A row as the reader parsed it */
struct row {
    uint64_t host_us = 0;
    uint64_t timestamp_us = 0;
};

/* The step from one row to the next in session time: the recording's
 * timestamp_us, or its host_us when timestamp_us steps backwards (a raw
 * adapter clock wrapping, as j2534_clock.h stands the host in) */
inline uint64_t step_us(const row& prev, const row& cur)
{
    if (cur.timestamp_us >= prev.timestamp_us) {
        return cur.timestamp_us - prev.timestamp_us;
    }
    return cur.host_us >= prev.host_us ? cur.host_us - prev.host_us : 0;
}

/* The recording's path in the form fopen takes, converted once in attach,
 * so the worker opens it again without allocating */
struct native_path {
#ifdef _WIN32
    std::vector<wchar_t> wide;
#else
    std::string narrow;
#endif
    /* False when the UTF-8 doesn't convert; may throw bad_alloc */
    bool set(const std::string& utf8)
    {
#ifdef _WIN32
        wide.assign(utf8.size() + 1, L'\0');
        return ecuio_utf8_to_wide(utf8.c_str(), wide.data(), wide.size());
#else
        narrow = utf8;
        return true;
#endif
    }
    /* Shared for reading and writing, as fopen is elsewhere: playing a
     * recording never locks it against the app or the user (_wfopen_s
     * would), and a file changed under it is ECUIO_ERR_RECORDING_CHANGED */
    FILE* open() const
    {
#ifdef _WIN32
        return _wfsopen(wide.data(), L"rb", _SH_DENYNO);
#else
        return std::fopen(narrow.c_str(), "rb");
#endif
    }
};

/* One line without its ending (LF or CRLF). False at the end of the file;
 * *too_long when the line doesn't fit kLineMax; *torn, when given, for the
 * file's last bytes with no newline after them: a write cut short (a
 * crash, a pulled cable, a power cut), which ADR-0049 section 7 drops and
 * counts rather than refuses. */
inline bool read_line(FILE* f, std::vector<char>& buf, bool* too_long,
                      bool* torn = nullptr)
{
    *too_long = false;
    if (torn != nullptr) {
        *torn = false;
    }
    /* No lock is held here: the worker's wait block ends before the next
     * read. clang-tidy 22's analyzer loses the unique_lock's unlock. */
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
    if (std::fgets(buf.data(), static_cast<int>(buf.size()), f) == nullptr) {
        return false;
    }
    size_t n = std::strlen(buf.data());
    if (n > 0 && buf[n - 1] == '\n') {
        buf[--n] = '\0';
    } else if (!std::feof(f)) {
        *too_long = true;
        return true;
    } else if (torn != nullptr) {
        *torn = true;
    }
    if (n > 0 && buf[n - 1] == '\r') {
        buf[--n] = '\0';
    }
    return true;
}

inline bool parse_decimal(const char*& p, uint64_t* out)
{
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        const uint64_t d = static_cast<uint64_t>(*p - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

inline int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

inline bool parse_hex32(const char*& p, uint32_t* out)
{
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) {
        return false;
    }
    p += 2;
    uint32_t v = 0;
    int digits = 0;
    for (int d; (d = hex_digit(*p)) >= 0; ++p) {
        if (++digits > 8) {
            return false;
        }
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    *out = v;
    return digits > 0;
}

/* One data row into *r and *frame. On failure, *why says which field. */
inline bool parse_row(const char* line, row* r, ecuio_frame_t* frame, const char** why)
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
inline bool read_header(FILE* f, std::vector<char>& buf)
{
    bool too_long = false;
    if (!read_line(f, buf, &too_long) || too_long) {
        ecuio_detail_set("the recording's frames.csv is empty");
        return false;
    }
    const char* line = buf.data();
    if (static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line += 3; /* a byte-order mark, should an editor add one */
    }
    if (std::strcmp(line, kHeader) != 0) {
        ecuio_detail_set("not a Havne frames.csv, or a newer format: the "
                         "header row isn't %s",
                         kHeader);
        return false;
    }
    return true;
}

inline bool is_absolute(const std::string& p)
{
#ifdef _WIN32
    return ecuio_is_absolute_path(p.c_str());
#else
    return !p.empty() && p[0] == '/';
#endif
}

/* A recording's path to its frames.csv: a path ending in .csv is the
 * file, anything else the folder holding it */
inline void to_file(std::string& path)
{
    const bool csv = path.size() >= 4 &&
                     (path.compare(path.size() - 4, 4, ".csv") == 0 ||
                      path.compare(path.size() - 4, 4, ".CSV") == 0);
    if (!csv) {
        while (path.size() > 1 &&
               (path.back() == '/' || path.back() == '\\')) {
            path.pop_back();
        }
        path += "/frames.csv";
    }
}

} // namespace ecuio_frames_csv

#endif /* ECUIO_FRAMES_CSV_H */
