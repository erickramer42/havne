// File/layer: tests/test_recording_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_recording*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "mock_drive_cycle.h" /* core/src */
#include "recording_rows.h" /* core/src */
#include "sha256.h"         /* core/src */
#include "recording_hour.h"
#include "temp_name.h"

namespace fs = std::filesystem;
namespace rows = ecuio_recording_rows;
using recording_hour::events_v2;
using recording_hour::Hour;
using recording_hour::kBom;
using recording_hour::Sample;

namespace {

template <typename Predicate> bool eventually(Predicate pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/* A folder of the case's own, removed at the end */
struct Folder {
    fs::path path;
    explicit Folder(const char* tag = "")
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               (temp_name("ecuio-recording") + tag);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    std::string utf8() const { return path.generic_u8string(); }
    void write(const char* name, const std::string& text) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << text;
    }
    void append(const char* name, const std::string& text) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::app);
        f << text;
    }
};

struct Reader {
    ecuio_recording_t* r = nullptr;
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    explicit Reader(const std::string& folder)
    {
        rc = ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r);
    }
    ~Reader() { ecuio_recording_close(r); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    ecuio_recording_info_t info() const
    {
        ecuio_recording_info_t i{};
        EXPECT_EQ(ecuio_recording_get_info(r, &i), ECUIO_OK);
        return i;
    }
    ecuio_recording_signal_t signal(uint32_t k) const
    {
        ecuio_recording_signal_t s{};
        EXPECT_EQ(ecuio_recording_get_signal(r, k, &s), ECUIO_OK);
        return s;
    }
    uint32_t index_of(const char* name) const
    {
        for (uint32_t k = 0; k < info().signal_count; ++k) {
            if (std::strcmp(signal(k).name, name) == 0) {
                return k;
            }
        }
        ADD_FAILURE() << "no signal " << name;
        return 0;
    }
    std::string text_at(uint32_t k, uint64_t t, uint64_t* at,
                        uint32_t cap = 256) const
    {
        std::vector<char> buf(cap);
        EXPECT_EQ(ecuio_recording_text_at(r, k, t, buf.data(), cap, at),
                  ECUIO_OK);
        return buf.data();
    }
    uint64_t find(const ecuio_recording_find_t& f, uint64_t from) const
    {
        uint64_t at = 0;
        EXPECT_EQ(ecuio_recording_find(r, &f, from, &at), ECUIO_OK);
        return at;
    }
};

/* The fixture's hour (recording_hour.h), made once for the binary */
const Hour& fixture_hour()
{
    static std::string why;
    static const Hour h = [] {
        Hour x;
        Hour::make(std::string(ECUIO_REPLAY_FIXTURES) +
                       "/2026-10-25-engine-start",
                   x, why);
        return x;
    }();
    EXPECT_TRUE(why.empty()) << why;
    return h;
}

/* The reader's column formula, so the scan places a sample where it does */
inline uint32_t column_of(uint64_t t, uint64_t t0, uint64_t t1, uint32_t n)
{
    const double c =
        static_cast<double>(t - t0) * n / static_cast<double>(t1 - t0);
    return (std::min)(static_cast<uint32_t>(c), n - 1);
}

/* The columns a scan of the samples gives: raw rows when narrower than a
 * second, each second in the column it starts in otherwise */
inline std::vector<ecuio_recording_column_t>
scan_columns(const std::vector<Sample>& samples, uint32_t signal, uint64_t t0,
             uint64_t t1, uint32_t n)
{
    std::vector<ecuio_recording_column_t> out(n);
    std::vector<bool> has(n, false);
    const bool wide = (t1 - t0) / n >= 1000000;
    for (const Sample& s : samples) {
        if (s.signal != signal) {
            continue;
        }
        uint32_t c = 0;
        if (wide) {
            const uint64_t start = s.us / 1000000 * 1000000;
            if (start >= t1 || start + 1000000 <= t0) {
                continue;
            }
            c = column_of((std::max)(start, t0), t0, t1, n);
        } else {
            if (s.us < t0 || s.us >= t1) {
                continue;
            }
            c = column_of(s.us, t0, t1, n);
        }
        ecuio_recording_column_t& col = out[c];
        ++col.samples;
        if (!has[c] || s.value < col.lowest) {
            col.lowest = s.value;
        }
        if (!has[c] || s.value > col.highest) {
            col.highest = s.value;
        }
        has[c] = true;
    }
    return out;
}

inline ecuio_recording_find_t threshold(uint32_t signal, double level, double h,
                                 uint32_t direction)
{
    ecuio_recording_find_t f{};
    f.kind = ECUIO_RECORDING_FIND_THRESHOLD;
    f.signal = signal;
    f.level = level;
    f.hysteresis = h;
    f.direction = direction;
    return f;
}

inline ecuio_recording_find_t engine_start()
{
    ecuio_recording_find_t f{};
    f.kind = ECUIO_RECORDING_FIND_ENGINE_START;
    return f;
}

/* signals.csv's rows of one signal: { host_us, value } each */
inline std::string rows_of(const char* name, const char* unit,
                    const std::vector<std::pair<uint64_t, double>>& samples)
{
    std::string s;
    for (const auto& p : samples) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%llu,%s,%.12g,%s\n",
                      static_cast<unsigned long long>(p.first), name, p.second,
                      unit);
        s += buf;
    }
    return s;
}

} // namespace

/* events.log in format 3 with these header lines (signal and label lines,
 * their keys included) and a summary */
inline std::string events_v3(const std::vector<std::string>& header_lines)
{
    std::string s = std::string(rows::kFormat3) +
                    "\nstarted  2026-11-05 12:00:00.000 (local time)\n"
                    "utc  2026-11-05T18:00:00.000Z\ncore  0.5.0\n"
                    "uri  mock://x\nprofile  test\nframe flags  complete\n";
    for (const std::string& l : header_lines) {
        s += l + "\n";
    }
    return s + "columns  local time, seconds since the recording started\n\n"
               "\n12:00:10.000  +10.000  end  the test ended: 0 frames, 0 "
               "values, 0 records dropped, 0 notes dropped\n";
}

namespace {

inline ecuio_recording_conditions_t conditions_of(const std::string& folder)
{
    Reader r(folder);
    EXPECT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_recording_conditions_t c{};
    EXPECT_EQ(ecuio_recording_get_conditions(r.r, &c), ECUIO_OK);
    return c;
}

/* RPM across seconds 0 to 5 (second 1 and 2 whole, 3 one sample at its
 * start, 5 holding only a sample after its middle), and VIN, a text
 * signal */
inline void write_span_folder(const Folder& f)
{
    f.write("events.log", events_v2({"RPM,number,rpm,0,1", "VIN,text,,0,0"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "500000,RPM,100,rpm\n"
                               "700000,VIN,ZZZFAKE1,\n"
                               "1000000,RPM,300,rpm\n"
                               "1400000,RPM,200,rpm\n"
                               "1900000,RPM,300,rpm\n"
                               "2000000,RPM,50,rpm\n"
                               "2500000,RPM,400,rpm\n"
                               "2600000,VIN,ZZZFAKE2,\n"
                               "3000000,RPM,400,rpm\n"
                               "3500000,RPM,10,rpm\n"
                               "4000000,RPM,20,rpm\n"
                               "5500000,RPM,7,rpm\n");
}

} // namespace
