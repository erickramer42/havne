// File/layer: tests/test_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingRowsTest, RecordingTest, RecordingStatsTest,
//          RecordingStretchesTest (test_core)
//
// Each case writes into a folder of its own under TEMP/TMPDIR, removed at
// the end.

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
uint32_t column_of(uint64_t t, uint64_t t0, uint64_t t1, uint32_t n)
{
    const double c =
        static_cast<double>(t - t0) * n / static_cast<double>(t1 - t0);
    return (std::min)(static_cast<uint32_t>(c), n - 1);
}

/* The columns a scan of the samples gives: raw rows when narrower than a
 * second, each second in the column it starts in otherwise */
std::vector<ecuio_recording_column_t>
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

/* A threshold found by stepping the samples one by one */
uint64_t scan_find(const std::vector<Sample>& samples, uint32_t signal,
                   double level, double hysteresis, bool rising, uint64_t from)
{
    bool armed = false;
    for (const Sample& s : samples) {
        if (s.signal != signal || s.us < from) {
            continue;
        }
        if (armed && (rising ? s.value >= level : s.value <= level)) {
            return s.us;
        }
        if (rising ? s.value < level - hysteresis
                   : s.value > level + hysteresis) {
            armed = true;
        }
    }
    return ECUIO_RECORDING_NONE;
}

ecuio_recording_find_t threshold(uint32_t signal, double level, double h,
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

std::string open_detail(const std::string& folder, ecuio_status_t want)
{
    ecuio_recording_t* r = reinterpret_cast<ecuio_recording_t*>(&want);
    EXPECT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r), want);
    EXPECT_EQ(r, nullptr);
    ecuio_recording_close(r);
    return ecuio_last_error_detail();
}

} // namespace

// ---- The parsers ----------------------------------------------------------

TEST(RecordingRowsTest, NumbersParseWithAPointUnderAnyLocale)
{
    double v = 0;
    const char* const exact[] = {"0",
                                 "-0",
                                 "1",
                                 "12.5",
                                 "-3.25",
                                 "0.001",
                                 "1261.25",
                                 "21.9607843137",
                                 "1e-05",
                                 "1.23456789012e+15",
                                 "6.02214076e+23"};
    for (const char* s : exact) {
        ASSERT_TRUE(rows::parse_number(s, std::strlen(s), &v)) << s;
        EXPECT_EQ(v, std::strtod(s, nullptr)) << s;
    }
    ASSERT_TRUE(rows::parse_number("inf", 3, &v));
    EXPECT_TRUE(std::isinf(v) && v > 0);
    ASSERT_TRUE(rows::parse_number("-inf", 4, &v));
    EXPECT_TRUE(std::isinf(v) && v < 0);
    ASSERT_TRUE(rows::parse_number("nan", 3, &v));
    EXPECT_TRUE(std::isnan(v));
    for (const char* s : {"", "-", ".", "1,5", "1.2.3", "1e", "e5", "0x10",
                          "12 ", " 12", "ABC", "1e+"}) {
        EXPECT_FALSE(rows::parse_number(s, std::strlen(s), &v)) << s;
    }
    EXPECT_EQ(rows::decimals_shown("12.250", 6), 3u);
    EXPECT_EQ(rows::decimals_shown("1e-05", 5), 0u);
    EXPECT_EQ(rows::decimals_shown("812", 3), 0u);
}

TEST(RecordingRowsTest, FieldsAreQuotedAsTheWriterQuotesThem)
{
    rows::signal_row r;
    const char* why = nullptr;
    const std::string row = "1500,\"Name, with comma\",\"say \"\"hi\"\"\",V";
    ASSERT_TRUE(rows::parse_signal_row(row.data(), row.size(), r, &why));
    EXPECT_EQ(r.host_us, 1500u);
    EXPECT_EQ(r.name, "Name, with comma");
    EXPECT_EQ(r.value, "say \"hi\"");
    EXPECT_EQ(r.unit, "V");
    const std::string empty_unit = "7,RPM,800,";
    ASSERT_TRUE(
        rows::parse_signal_row(empty_unit.data(), empty_unit.size(), r, &why));
    EXPECT_EQ(r.unit, "");

    const char* const bad[] = {"x,RPM,1,rpm",
                               "1,RPM,1",
                               "1,RPM,1,rpm,extra",
                               "1,\"RPM,1,rpm",
                               "1,\"RPM\"x,1,rpm",
                               "1,,1,rpm",
                               "18446744073709551616,RPM,1,rpm"};
    for (const char* b : bad) {
        EXPECT_FALSE(rows::parse_signal_row(b, std::strlen(b), r, &why)) << b;
    }
    const std::string long_name = "1," + std::string(32, 'N') + ",1,rpm";
    EXPECT_FALSE(
        rows::parse_signal_row(long_name.data(), long_name.size(), r, &why));
    EXPECT_STREQ(why, "the signal's name is empty or longer than 31 bytes");
    const std::string ok_name =
        "1," + std::string(31, 'N') + ",1," + std::string(15, 'u');
    EXPECT_TRUE(
        rows::parse_signal_row(ok_name.data(), ok_name.size(), r, &why));
    const std::string nul("1,RP\0M,1,rpm", 12);
    EXPECT_FALSE(rows::parse_signal_row(nul.data(), nul.size(), r, &why));
    EXPECT_STREQ(why, "the row holds a NUL byte");
    uint64_t at = 0;
    std::string label;
    const std::string nul_label("1,la\0bel", 8);
    EXPECT_FALSE(
        rows::parse_marker_row(nul_label.data(), nul_label.size(), &at, label,
                               &why));
    /* An empty row with no buffer behind it (an empty vector's data()):
     * refused, with nothing handed to memchr, which the sanitizer job's
     * UBSan would report */
    EXPECT_FALSE(rows::parse_signal_row(nullptr, 0, r, &why));
    EXPECT_FALSE(rows::parse_marker_row(nullptr, 0, &at, label, &why));
    const std::string long_unit = "1,RPM,1," + std::string(16, 'u');
    EXPECT_FALSE(
        rows::parse_signal_row(long_unit.data(), long_unit.size(), r, &why));
    EXPECT_STREQ(why, "the unit is longer than 15 bytes");
}

TEST(RecordingRowsTest, EventsLogsLinesParse)
{
    rows::header_signal s;
    const char* why = nullptr;
    const std::string line = "\"A, B\",text,\xC2\xB0"
                             "C,2,14";
    ASSERT_TRUE(rows::parse_header_signal(line.data(), line.size(), s, &why));
    EXPECT_EQ(s.name, "A, B");
    EXPECT_TRUE(s.text);
    EXPECT_EQ(s.unit, "\xC2\xB0"
                      "C");
    EXPECT_EQ(s.decimal_places, 2);
    EXPECT_EQ(s.plot_rank, 14);
    EXPECT_EQ(s.group, 0) << "a recording from before groups";
    /* ADR-0050: the group, a sixth field */
    const std::string grouped = "RPM,number,rpm,0,1,1";
    rows::header_signal g;
    ASSERT_TRUE(
        rows::parse_header_signal(grouped.data(), grouped.size(), g, &why));
    EXPECT_EQ(g.plot_rank, 1);
    EXPECT_EQ(g.group, 1);
    for (const char* b :
         {"RPM,count,rpm,0,1", "RPM,number,rpm,4,1", "RPM,number,rpm,0,256",
          "RPM,number,rpm,0", "RPM,number,rpm,0,1,x", "RPM,number,rpm,0,1,256",
          "RPM,number,rpm,0,1,1,1", "RPM,number,rpm,0,1,"}) {
        EXPECT_FALSE(rows::parse_header_signal(b, std::strlen(b), s, &why))
            << b;
    }

    uint64_t us = 0;
    ASSERT_TRUE(rows::parse_utc("2026-11-03T04:10:27.237Z", 24, &us));
    EXPECT_EQ(us, 1793679027237000ull);
    ASSERT_TRUE(rows::parse_utc("1970-01-01T00:00:00.000Z", 24, &us));
    EXPECT_EQ(us, 0u);
    EXPECT_FALSE(rows::parse_utc("2026-13-03T04:10:27.237Z", 24, &us));
    EXPECT_FALSE(rows::parse_utc("2026-11-03 04:10:27.237Z", 24, &us));

    rows::event_line e;
    const std::string ev = "12:00:01.500  +1.500  drop  the recording "
                           "dropped 312 frames: the disk fell behind";
    ASSERT_TRUE(rows::parse_event_line(ev.data(), ev.size(), &e));
    EXPECT_EQ(e.ms, 1500u);
    const char* text = nullptr;
    size_t text_len = 0;
    size_t kind_len = 0;
    ASSERT_TRUE(
        rows::split_kind(e.rest, e.rest_len, &text, &text_len, &kind_len));
    EXPECT_EQ(std::string(e.rest, kind_len), "drop");
    uint64_t frames = 0;
    ASSERT_TRUE(rows::parse_drop(text, text_len, &frames));
    EXPECT_EQ(frames, 312u);
    EXPECT_FALSE(rows::parse_event_line("end      x", 10, &e));

    uint64_t records = 0;
    uint64_t notes = 0;
    const std::string end = "the user said: stop: 10 frames, 9 values, "
                            "3 records dropped, 1 notes dropped";
    ASSERT_TRUE(rows::parse_end(end.data(), end.size(), &records, &notes));
    EXPECT_EQ(records, 3u);
    EXPECT_EQ(notes, 1u);
    const std::string bad_end = "why: 10 frames, 9 values";
    EXPECT_FALSE(
        rows::parse_end(bad_end.data(), bad_end.size(), &records, &notes));
}

/* Format 3's label line, and the enum kind on a signal line */
TEST(RecordingRowsTest, LabelLinesParse)
{
    rows::header_signal s;
    const char* why = nullptr;
    const std::string sig = "Fuel,enum,,0,1,3";
    ASSERT_TRUE(rows::parse_header_signal(sig.data(), sig.size(), s, &why));
    EXPECT_TRUE(s.enumeration);
    EXPECT_FALSE(s.text);
    const std::string num = "RPM,number,rpm,0,1";
    ASSERT_TRUE(rows::parse_header_signal(num.data(), num.size(), s, &why));
    EXPECT_FALSE(s.enumeration);

    rows::header_label l;
    const std::string line = "\"Fuel, 1\",4294967295,\"Open loop, \"\"cold\"\"\"";
    ASSERT_TRUE(rows::parse_header_label(line.data(), line.size(), l, &why))
        << why;
    EXPECT_EQ(l.signal, "Fuel, 1");
    EXPECT_EQ(l.raw, 4294967295u);
    EXPECT_EQ(l.label, "Open loop, \"cold\"");
    const std::string longest = "Fuel,0," + std::string(27, 'x');
    ASSERT_TRUE(
        rows::parse_header_label(longest.data(), longest.size(), l, &why));
    EXPECT_EQ(l.raw, 0u);
    struct Bad {
        std::string line;
        const char* why;
    };
    const Bad bad[] = {
        {"Fuel,1", "the label line isn't signal,raw,label"},
        {"Fuel,1,a,b", "the label line isn't signal,raw,label"},
        {",1,a", "the label's signal name is empty or longer than 31 bytes"},
        {std::string(32, 'N') + ",1,a",
         "the label's signal name is empty or longer than 31 bytes"},
        {"Fuel,4294967296,a", "the label's raw value isn't 0 to 4294967295"},
        {"Fuel,-1,a", "the label's raw value isn't 0 to 4294967295"},
        {"Fuel,,a", "the label's raw value isn't 0 to 4294967295"},
        {"Fuel,1,", "the label is empty or longer than 27 bytes"},
        {"Fuel,1," + std::string(28, 'x'),
         "the label is empty or longer than 27 bytes"},
        {std::string("Fuel,1,a\0b", 10), nullptr},
    };
    for (const Bad& b : bad) {
        why = nullptr;
        EXPECT_FALSE(
            rows::parse_header_label(b.line.data(), b.line.size(), l, &why))
            << b.line;
        if (b.why != nullptr) {
            EXPECT_STREQ(why, b.why) << b.line;
        } else {
            EXPECT_NE(why, nullptr) << "a NUL is refused with words";
        }
    }
}

TEST(RecordingRowsTest, ASignalFileLineIsItsNameAndAHash)
{
    const std::string sha(64, 'a');
    std::string hex;
    const std::string ok = "signals.json," + sha;
    ASSERT_TRUE(rows::parse_signal_file(ok.data(), ok.size(), &hex));
    EXPECT_EQ(hex, sha);
    const std::string digits = "signals.json,0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef";
    ASSERT_TRUE(rows::parse_signal_file(digits.data(), digits.size(), &hex));
    EXPECT_EQ(hex, digits.substr(13));
    const std::string bad[] = {
        "signals.json," + std::string(63, 'a'),
        "signals.json," + std::string(65, 'a'),
        "signals.json," + std::string(63, 'a') + "A",
        "signals.json," + std::string(63, 'a') + "g",
        "signals.json;" + sha,
        "signals.jsoo," + sha,
        "other.json," + sha,
        "",
    };
    for (const std::string& line : bad) {
        hex = "kept";
        EXPECT_FALSE(rows::parse_signal_file(line.data(), line.size(), &hex))
            << line;
        EXPECT_EQ(hex, "kept");
    }
}

// ---- The reader -----------------------------------------------------------

TEST(RecordingTest, AFolderTheRecorderWroteOpensWithItsHeader)
{
    Folder root;
    const std::string source =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    const std::string uri = "replay://" + source + "?speed=max&profile=obd2";
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char folder[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(session, rec), ECUIO_OK);
    std::atomic<uint64_t> frames{0};
    auto count = [](const ecuio_frame_t*, void* user) {
        static_cast<std::atomic<uint64_t>*>(user)->fetch_add(1);
    };
    ASSERT_EQ(ecuio_register_frame_callback(session, count, &frames), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_EQ(ecuio_recorder_mark(rec, "lap, one"), ECUIO_OK);
    ecuio_session_info_t si{};
    ASSERT_EQ(ecuio_session_get_info(session, &si), ECUIO_OK);
    ASSERT_TRUE(
        eventually([&] { return frames.load() >= si.replay_frames; }, 20000));
    ecuio_session_destroy(session);
    ASSERT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);
    /* The rows the writer wrote, its summary's drops */
    uint64_t value_rows = 0;
    {
        std::ifstream csv(fs::u8path(folder) / "signals.csv");
        std::string line;
        while (std::getline(csv, line)) {
            ++value_rows;
        }
        --value_rows; /* the header row */
    }

    Reader r(folder);
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_info_t info = r.info();
    EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_ENUMS);
    EXPECT_EQ(info.flags, 0u) << "the summary is there";
    EXPECT_GT(info.utc_start_us, 1700000000000000ull);
    EXPECT_EQ(info.rows_torn, 0u);
    EXPECT_EQ(info.records_dropped, 0u);
    EXPECT_EQ(info.marker_count, 1u);

    /* Every def in the header's order, as Live shows them */
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(info.signal_count, set->count);
    uint64_t total = 0;
    for (uint32_t k = 0; k < set->count; ++k) {
        const ecuio_recording_signal_t s = r.signal(k);
        EXPECT_STREQ(s.name, set->defs[k].name);
        EXPECT_STREQ(s.unit, set->defs[k].unit);
        EXPECT_EQ(s.decimal_places, set->defs[k].decimal_places) << s.name;
        EXPECT_EQ(s.plot_rank, set->defs[k].plot_rank) << s.name;
        EXPECT_EQ(s.group, set->defs[k].group) << s.name;
        total += s.samples;
    }
    EXPECT_EQ(total, value_rows);
    const ecuio_recording_signal_t rpm = r.signal(r.index_of("RPM"));
    EXPECT_EQ(rpm.highest, 1261.25);
    EXPECT_LE(rpm.first_us, rpm.last_us);

    ecuio_recording_marker_t m{};
    ASSERT_EQ(ecuio_recording_get_marker(r.r, 0, &m), ECUIO_OK);
    EXPECT_STREQ(m.label_utf8, "lap, one");
    EXPECT_EQ(m.label_length, 8u);
    ecuio_recording_find_t f{};
    f.kind = ECUIO_RECORDING_FIND_MARKER;
    f.label_utf8 = "lap, one";
    EXPECT_EQ(r.find(f, 0), m.host_us);
    EXPECT_EQ(r.find(f, m.host_us + 1), ECUIO_RECORDING_NONE);
    f.label_utf8 = "lap";
    EXPECT_EQ(r.find(f, 0), ECUIO_RECORDING_NONE);
}

TEST(RecordingTest, AnHoursSignalsMatchAScanOfTheirRows)
{
    const Hour& h = fixture_hour();
    ASSERT_GT(h.values_per_pass, 5000u);
    Folder f;
    h.write(f.path);
    const auto t0 = std::chrono::steady_clock::now();
    Reader r(f.utf8());
    const double open_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    RecordProperty("open_seconds", std::to_string(open_s));
    const ecuio_recording_info_t info = r.info();
    EXPECT_EQ(info.duration_us, 3600000000ull) << "the end line's time";
    EXPECT_EQ(info.utc_start_us, 1793728800000000ull);
    ASSERT_EQ(info.signal_count, h.defs.size());
    for (uint32_t k = 0; k < info.signal_count; ++k) {
        uint64_t n = 0;
        double lo = 0;
        double hi = 0;
        uint64_t first = ECUIO_RECORDING_NONE;
        uint64_t last = ECUIO_RECORDING_NONE;
        for (const Sample& s : h.samples) {
            if (s.signal != k) {
                continue;
            }
            lo = n == 0 ? s.value : (std::min)(lo, s.value);
            hi = n == 0 ? s.value : (std::max)(hi, s.value);
            if (first == ECUIO_RECORDING_NONE) {
                first = s.us;
            }
            last = s.us;
            ++n;
        }
        const ecuio_recording_signal_t s = r.signal(k);
        EXPECT_EQ(s.samples, n) << s.name;
        EXPECT_EQ(s.lowest, lo) << s.name;
        EXPECT_EQ(s.highest, hi) << s.name;
        EXPECT_EQ(s.first_us, first) << s.name;
        EXPECT_EQ(s.last_us, last) << s.name;
        EXPECT_EQ(s.flags, 0u) << s.name;
    }
}

TEST(RecordingTest, RangeQueriesMatchAScanZoomedOutAndIn)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    struct Range {
        uint64_t t0, t1;
        uint32_t n;
    };
    const Range ranges[] = {
        {0, 3600000000ull, 1000},           /* the whole hour: summaries */
        {0, 3600000000ull, 3600},           /* a second a column */
        {12345678, 912345678, 300},         /* unaligned, 3 s a column */
        {301000000, 303000000, 800},        /* 2.5 ms a column: raw rows */
        {299500000, 300700000, 2000},       /* across a pass's seam */
        {40123456, 40123999, 7},            /* inside one second */
        {3599000000ull, 3700000000ull, 50}, /* past the end */
    };
    for (const Range& g : ranges) {
        std::vector<ecuio_recording_column_t> got(g.n);
        ASSERT_EQ(
            ecuio_recording_query(r.r, &rpm, 1, g.t0, g.t1, got.data(), g.n),
            ECUIO_OK);
        const std::vector<ecuio_recording_column_t> want =
            scan_columns(h.samples, rpm, g.t0, g.t1, g.n);
        for (uint32_t c = 0; c < g.n; ++c) {
            ASSERT_EQ(got[c].samples, want[c].samples)
                << g.t0 << ".." << g.t1 << " column " << c;
            ASSERT_EQ(got[c].lowest, want[c].lowest) << "column " << c;
            ASSERT_EQ(got[c].highest, want[c].highest) << "column " << c;
        }
    }
}

TEST(RecordingTest, AQueryOverSeveralSignalsEqualsThemOneAtATime)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    const uint32_t picks[] = {r.index_of("RPM"), r.index_of("Coolant Temp"),
                              r.index_of("Battery Voltage"), r.index_of("RPM")};
    const uint32_t n = 400;
    for (const uint64_t span : {2000000ull, 3600000000ull}) {
        std::vector<ecuio_recording_column_t> all(4 * n);
        ASSERT_EQ(ecuio_recording_query(r.r, picks, 4, 1000000, 1000000 + span,
                                        all.data(), n),
                  ECUIO_OK);
        for (uint32_t q = 0; q < 4; ++q) {
            std::vector<ecuio_recording_column_t> one(n);
            ASSERT_EQ(ecuio_recording_query(r.r, &picks[q], 1, 1000000,
                                            1000000 + span, one.data(), n),
                      ECUIO_OK);
            for (uint32_t c = 0; c < n; ++c) {
                ASSERT_EQ(all[q * n + c].samples, one[c].samples);
                ASSERT_EQ(all[q * n + c].lowest, one[c].lowest);
                ASSERT_EQ(all[q * n + c].highest, one[c].highest);
            }
        }
    }
}

TEST(RecordingTest, FindReturnsTheFirstCrossingAfterItsHysteresis)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t volts = r.index_of("Battery Voltage");
    struct Case {
        uint32_t signal;
        double level, h;
        uint32_t dir;
        uint64_t from;
    };
    const Case cases[] = {
        {rpm, 1000, 100, ECUIO_RECORDING_FIND_RISING, 0},
        {rpm, 1000, 100, ECUIO_RECORDING_FIND_RISING, 30000000},
        {rpm, 1200, 0, ECUIO_RECORDING_FIND_RISING, 0},
        {rpm, 900, 50, ECUIO_RECORDING_FIND_FALLING, 0},
        {rpm, 5000, 0, ECUIO_RECORDING_FIND_RISING, 0}, /* never */
        {volts, 10, 0.5, ECUIO_RECORDING_FIND_FALLING, 0},
        {volts, 10, 0.5, ECUIO_RECORDING_FIND_FALLING, 600000000},
    };
    for (const Case& c : cases) {
        const uint64_t want =
            scan_find(h.samples, c.signal, c.level, c.h,
                      c.dir == ECUIO_RECORDING_FIND_RISING, c.from);
        EXPECT_EQ(r.find(threshold(c.signal, c.level, c.h, c.dir), c.from),
                  want)
            << c.level << " from " << c.from;
    }
    /* Each crossing in turn, as the viewer steps through them */
    uint64_t from = 0;
    int found = 0;
    for (;;) {
        const uint64_t want = scan_find(h.samples, rpm, 1000, 100, true, from);
        const uint64_t got = r.find(
            threshold(rpm, 1000, 100, ECUIO_RECORDING_FIND_RISING), from);
        ASSERT_EQ(got, want);
        if (got == ECUIO_RECORDING_NONE) {
            break;
        }
        ++found;
        from = got + 1;
    }
    EXPECT_GE(found, 12) << "a start each pass";
}

TEST(RecordingTest, HysteresisKeepsANoisyLevelFromCountingTwice)
{
    /* rpm dithering about 1000: with 100 of hysteresis, 950 after a
     * crossing doesn't arm the next one, 880 does */
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "100000,RPM,800,rpm\n"
                               "200000,RPM,1010,rpm\n"
                               "1300000,RPM,950,rpm\n"
                               "1400000,RPM,1020,rpm\n"
                               "2500000,RPM,880,rpm\n"
                               "2600000,RPM,1030,rpm\n"
                               "3700000,RPM,1110,rpm\n"
                               "3800000,RPM,960,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const auto rising = threshold(0, 1000, 100, ECUIO_RECORDING_FIND_RISING);
    EXPECT_EQ(r.find(rising, 0), 200000u);
    EXPECT_EQ(r.find(rising, 200001), 2600000u) << "950 is within 100";
    EXPECT_EQ(
        r.find(threshold(0, 1000, 0, ECUIO_RECORDING_FIND_RISING), 200001),
        1400000u);
    const auto falling = threshold(0, 1000, 100, ECUIO_RECORDING_FIND_FALLING);
    EXPECT_EQ(r.find(falling, 0), 3800000u) << "armed only by 1110";
    EXPECT_EQ(r.find(threshold(0, 1000, 0, ECUIO_RECORDING_FIND_FALLING), 0),
              1300000u);
}

namespace {

ecuio_recording_find_t engine_start()
{
    ecuio_recording_find_t f{};
    f.kind = ECUIO_RECORDING_FIND_ENGINE_START;
    return f;
}

/* signals.csv's rows of one signal: { host_us, value } each */
std::string rows_of(const char* name, const char* unit,
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

/* The engine starting: key on at 0 rpm, the starter's 220, the catch;
 * a dip to 450 while it runs is no start, a stall to 40 and a restart
 * is the next. obd2's and the mock's unit is "RPM" */
TEST(RecordingTest, AnEngineStartIsTheRpmRisingFromAStop)
{
    Folder f;
    f.write("events.log",
            events_v2({"Coolant,number,°C,0,1", "RPM,number,RPM,0,2"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader + "\n" +
                               rows_of("Coolant", "°C", {{50000, 20}}) +
                               rows_of("RPM", "RPM",
                                       {{100000, 0},
                                        {1100000, 0},
                                        {2100000, 0},
                                        {3000000, 220},
                                        {3100000, 240},
                                        {3200000, 499.75},
                                        {3300000, 900},
                                        {4300000, 850},
                                        {5300000, 450},
                                        {5400000, 860},
                                        {6300000, 40},
                                        {7300000, 0},
                                        {8300000, 230},
                                        {8400000, 760}}));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.find(engine_start(), 0), 3300000u);
    EXPECT_EQ(r.find(engine_start(), 3300000), 8400000u)
        << "a sample before from_us arms nothing";
    EXPECT_EQ(r.find(engine_start(), 3300001), 8400000u)
        << "450 isn't a stop; 40 is";
    EXPECT_EQ(r.find(engine_start(), 6300001), 8400000u)
        << "the stop's own sample arms it";
    EXPECT_EQ(r.find(engine_start(), 8400001), ECUIO_RECORDING_NONE);
    /* It is the rising threshold at 500 with 400 of hysteresis */
    EXPECT_EQ(
        r.find(threshold(1, ECUIO_ENGINE_START_RPM,
                         ECUIO_ENGINE_START_RPM - ECUIO_ENGINE_STOPPED_RPM,
                         ECUIO_RECORDING_FIND_RISING),
               3300001),
        8400000u);
}

/* A recording that starts with the engine running finds nothing until it
 * stops and starts again; one with a stop at exactly 100 rpm, nothing:
 * the stop is below it */
TEST(RecordingTest, AnEngineAlreadyRunningIsNoStart)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader + "\n" +
                               rows_of("RPM", "rpm",
                                       {{100000, 820},
                                        {1100000, 3000},
                                        {2100000, 100},
                                        {3100000, 900},
                                        {4100000, 99.75},
                                        {5100000, 800}}));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.find(engine_start(), 0), 5100000u);
    EXPECT_EQ(r.find(engine_start(), 4100001), ECUIO_RECORDING_NONE);
}

/* No number signal in rpm: no engine start, whatever the names say. A
 * text signal in rpm isn't one, nor is a unit that only starts so */
TEST(RecordingTest, NoSignalInRpmFindsNoEngineStart)
{
    Folder f;
    f.write("events.log",
            events_v2({"RPM,number,r/min,0,1", "Note,text,rpm,0,0",
                       "Speed,number,rpms,0,2", "Shaft,number,rp,0,3"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader + "\n" +
                               rows_of("RPM", "r/min", {{100000, 0}}) +
                               rows_of("Note", "rpm", {{150000, 0}}) +
                               rows_of("Speed", "rpms", {{160000, 0}}) +
                               rows_of("Shaft", "rp", {{170000, 0}}) +
                               rows_of("RPM", "r/min", {{200000, 900}}) +
                               rows_of("Note", "rpm", {{250000, 900}}) +
                               rows_of("Speed", "rpms", {{260000, 900}}) +
                               rows_of("Shaft", "rp", {{270000, 900}}));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.find(engine_start(), 0), ECUIO_RECORDING_NONE);
}

/* Of several signals in rpm, the one ranked first among the ranked; with
 * none ranked, the first in the recording's order */
TEST(RecordingTest, TheEngineSpeedIsTheBestRankedSignalInRpm)
{
    const std::string starts = rows_of("Fan", "rpm", {{100000, 0}}) +
                               rows_of("Turbo", "RPM", {{110000, 0}}) +
                               rows_of("Engine", "Rpm", {{120000, 0}}) +
                               rows_of("Fan", "rpm", {{1000000, 900}}) +
                               rows_of("Turbo", "RPM", {{2000000, 900}}) +
                               rows_of("Engine", "Rpm", {{3000000, 900}});
    {
        Folder f;
        f.write("events.log",
                events_v2({"Fan,number,rpm,0,0", "Turbo,number,RPM,0,3",
                           "Engine,number,Rpm,0,2"}));
        f.write("signals.csv",
                std::string(kBom) + rows::kSignalsHeader + "\n" + starts);
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.find(engine_start(), 0), 3000000u) << "rank 2 over 3 and 0";
    }
    {
        Folder f;
        f.write("events.log",
                events_v2({"Fan,number,rpm,0,0", "Turbo,number,RPM,0,0",
                           "Engine,number,Rpm,0,0"}));
        f.write("signals.csv",
                std::string(kBom) + rows::kSignalsHeader + "\n" + starts);
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.find(engine_start(), 0), 1000000u)
            << "none ranked: the first";
    }
    {
        /* A text signal in rpm is none, ranked first or not */
        Folder f;
        f.write("events.log",
                events_v2({"Note,text,rpm,0,1", "Engine,number,rpm,0,0"}));
        f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader + "\n" +
                                   rows_of("Note", "rpm", {{50000, 0}}) +
                                   rows_of("Engine", "rpm", {{100000, 0}}) +
                                   rows_of("Note", "rpm", {{500000, 900}}) +
                                   rows_of("Engine", "rpm", {{1000000, 900}}));
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.find(engine_start(), 0), 1000000u) << "the number signal";
    }
}

/* An engine start takes no signal, direction, level or hysteresis */
TEST(RecordingTest, AnEngineStartRefusesWhatItDoesntTake)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}));
    f.write("signals.csv",
            std::string(kBom) + rows::kSignalsHeader + "\n" +
                rows_of("RPM", "rpm", {{100000, 0}, {200000, 900}}));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    uint64_t at = 0;
    const auto refused = [&](ecuio_recording_find_t what) {
        return ecuio_recording_find(r.r, &what, 0, &at) ==
               ECUIO_ERR_INVALID_ARGUMENT;
    };
    ecuio_recording_find_t bad = engine_start();
    bad.signal = 1;
    EXPECT_TRUE(refused(bad)) << "a signal";
    bad = engine_start();
    bad.direction = ECUIO_RECORDING_FIND_RISING;
    EXPECT_TRUE(refused(bad)) << "a direction";
    bad = engine_start();
    bad.level = ECUIO_ENGINE_START_RPM;
    EXPECT_TRUE(refused(bad)) << "a level";
    bad.level = NAN;
    EXPECT_TRUE(refused(bad)) << "a NaN level";
    bad = engine_start();
    bad.hysteresis = 400;
    EXPECT_TRUE(refused(bad)) << "a hysteresis";
    bad = engine_start();
    bad.flags = 1;
    EXPECT_TRUE(refused(bad)) << "a flag";
    bad = engine_start();
    bad.reserved[0] = 1;
    EXPECT_TRUE(refused(bad)) << "a reserved word";
    bad = engine_start();
    bad.kind = ECUIO_RECORDING_FIND_ENGINE_START + 1;
    EXPECT_TRUE(refused(bad)) << "the kind after it";
    const ecuio_recording_find_t ok = engine_start();
    EXPECT_EQ(ecuio_recording_find(r.r, &ok, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_find(nullptr, &ok, 0, &at),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.find(ok, 0), 200000u);
}

/* The mock's drive cycle as recorded: 15 s of key on at 0 rpm, then the
 * start (the starter at about 220 rpm, the catch), found where its RPM
 * first reaches 500 */
TEST(RecordingTest, AMockRecordingsEngineStartIsAfterItsKeyOn)
{
    Folder root;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &session), ECUIO_OK);
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char folder[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(session, rec), ECUIO_OK);
    /* The RPM seen, so the wait ends once the engine runs */
    std::atomic<double> rpm{0};
    auto on_values = [](const ecuio_value_batch_t* b, void* user) {
        for (uint32_t i = 0; i < b->count; ++i) {
            if (b->values[i].signal_id == ECUIO_MOCK_SIG_RPM_ID) {
                static_cast<std::atomic<double>*>(user)->store(
                    b->values[i].value);
            }
        }
    };
    ASSERT_EQ(ecuio_register_values_callback(session, on_values, &rpm),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    const bool ran = eventually([&] { return rpm.load() >= 700; }, 40000);
    ecuio_session_destroy(session);
    ASSERT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);
    ASSERT_TRUE(ran) << "the mock's engine starts after its key on";

    Reader r(folder);
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t k = r.index_of("RPM");
    EXPECT_STREQ(r.signal(k).unit, "RPM");
    const uint64_t at = r.find(engine_start(), 0);
    ASSERT_NE(at, ECUIO_RECORDING_NONE);
    EXPECT_GE(at, 15000000u) << "after the key on";
    uint64_t sample = 0;
    EXPECT_GE(std::stod(r.text_at(k, at, &sample)), 500.0);
    EXPECT_EQ(sample, at);
    EXPECT_LT(std::stod(r.text_at(k, at - 1, &sample)), 500.0)
        << "the sample before it hadn't reached 500";
    EXPECT_EQ(r.find(threshold(k, 500, 400, ECUIO_RECORDING_FIND_RISING), 0),
              at);
    EXPECT_EQ(r.find(engine_start(), at + 1), ECUIO_RECORDING_NONE);
}

TEST(RecordingTest, ConcurrentQueriesOnOneHandleAgree)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    const uint32_t rpm = r.index_of("RPM");
    /* Pans across the hour, narrow and wide, worked out once alone */
    struct Q {
        uint64_t t0, t1;
        std::vector<ecuio_recording_column_t> want;
    };
    std::vector<Q> qs;
    for (uint64_t k = 0; k < 40; ++k) {
        const uint64_t t0 = k * 89000000;
        const uint64_t span = (k % 2 == 0) ? 3000000 : 900000000;
        qs.push_back(
            Q{t0, t0 + span, scan_columns(h.samples, rpm, t0, t0 + span, 300)});
    }
    ecuio_recording_test_set_cache_limit(r.r, 64 << 10); /* evicts often */
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::vector<ecuio_recording_column_t> got(300);
            for (int round = 0; round < 3; ++round) {
                for (size_t i = 0; i < qs.size(); ++i) {
                    const Q& q =
                        qs[(i * 7 + static_cast<size_t>(t)) % qs.size()];
                    if (ecuio_recording_query(r.r, &rpm, 1, q.t0, q.t1,
                                              got.data(), 300) != ECUIO_OK) {
                        ++bad;
                        continue;
                    }
                    for (uint32_t c = 0; c < 300; ++c) {
                        if (got[c].samples != q.want[c].samples ||
                            got[c].highest != q.want[c].highest) {
                            ++bad;
                            break;
                        }
                    }
                    uint64_t at = 0;
                    char text[64];
                    if (ecuio_recording_text_at(r.r, rpm, q.t0 + 500000, text,
                                                sizeof text, &at) != ECUIO_OK) {
                        ++bad;
                    }
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(bad.load(), 0);
    EXPECT_LE(ecuio_recording_test_cache_bytes(r.r), 64u << 10);
}

TEST(RecordingTest, MemoryStaysWithinTheBound)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    /* 40 bytes per number signal per second it has samples in, 16 per
     * text signal's, 16 per second for the index */
    std::set<std::pair<uint32_t, uint64_t>> signal_seconds;
    std::set<uint64_t> seconds;
    for (const Sample& s : h.samples) {
        signal_seconds.emplace(s.signal, s.us / 1000000);
        seconds.insert(s.us / 1000000);
    }
    const uint64_t bound = 40 * signal_seconds.size() + 16 * seconds.size();
    EXPECT_LE(ecuio_recording_test_table_bytes(r.r), bound);
    EXPECT_GT(ecuio_recording_test_table_bytes(r.r), bound / 2);

    /* Every second read raw, two seconds a query: the cache stays within
     * its 8 MiB, and evicts past a lower limit */
    const uint32_t rpm = r.index_of("RPM");
    std::vector<ecuio_recording_column_t> cols(100);
    for (uint64_t t = 0; t < 3600000000ull; t += 2000000) {
        ASSERT_EQ(ecuio_recording_query(r.r, &rpm, 1, t, t + 2000000,
                                        cols.data(), 100),
                  ECUIO_OK);
    }
    EXPECT_GT(ecuio_recording_test_cache_bytes(r.r), 0u);
    EXPECT_LE(ecuio_recording_test_cache_bytes(r.r), 8u << 20);
    ecuio_recording_test_set_cache_limit(r.r, 32 << 10);
    ASSERT_EQ(ecuio_recording_query(r.r, &rpm, 1, 0, 2000000, cols.data(), 100),
              ECUIO_OK);
    EXPECT_LE(ecuio_recording_test_cache_bytes(r.r), 32u << 10);
}

TEST(RecordingTest, TextAtGivesTheValueInForce)
{
    Folder f;
    f.write("events.log", events_v2({"VIN,text,,0,0", "Volts,number,V,1,1"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "500000,VIN,ZZZFAKE1,\n"
                               "1200000,Volts,12.5,V\n"
                               "2700000,VIN,\"B, two\",\n"
                               "2900000,VIN,ABC,\n"
                               "5100000,Volts,13.25,V\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t vin = r.index_of("VIN");
    const uint32_t volts = r.index_of("Volts");
    EXPECT_EQ(r.signal(vin).flags, ECUIO_RECORDING_SIGNAL_TEXT);
    EXPECT_EQ(r.signal(vin).samples, 3u);
    uint64_t at = 0;
    EXPECT_EQ(r.text_at(vin, 400000, &at), "");
    EXPECT_EQ(at, ECUIO_RECORDING_NONE);
    EXPECT_EQ(r.text_at(vin, 500000, &at), "ZZZFAKE1");
    EXPECT_EQ(at, 500000u);
    EXPECT_EQ(r.text_at(vin, 2000000, &at), "ZZZFAKE1");
    EXPECT_EQ(r.text_at(vin, 2500000, &at), "ZZZFAKE1")
        << "t's second holds only later samples";
    EXPECT_EQ(r.text_at(vin, 2800000, &at), "B, two");
    EXPECT_EQ(at, 2700000u);
    EXPECT_EQ(r.text_at(vin, 99000000, &at), "ABC");
    EXPECT_EQ(at, 2900000u);
    EXPECT_EQ(r.text_at(vin, 99000000, &at, 2), "A") << "cut to capacity";
    EXPECT_EQ(r.text_at(volts, 3000000, &at), "12.5");
    EXPECT_EQ(at, 1200000u);
    EXPECT_EQ(r.text_at(volts, 5100000, &at), "13.25");

    /* A text signal's columns carry counts alone */
    ecuio_recording_column_t cols[3];
    ASSERT_EQ(ecuio_recording_query(r.r, &vin, 1, 0, 3000000, cols, 3),
              ECUIO_OK);
    EXPECT_EQ(cols[0].samples, 1u);
    EXPECT_EQ(cols[1].samples, 0u);
    EXPECT_EQ(cols[2].samples, 2u);
    EXPECT_EQ(cols[2].highest, 0.0);
    ecuio_recording_find_t fd =
        threshold(vin, 1, 0, ECUIO_RECORDING_FIND_RISING);
    uint64_t got = 0;
    EXPECT_EQ(ecuio_recording_find(r.r, &fd, 0, &got),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST(RecordingTest, AnOlderRecordingOpensWithItsKindsInferred)
{
    for (const char* product : {"Havne", "QtTune"}) {
        Folder f(product);
        f.write("events.log",
                std::string(product) +
                    " session recording (ADR-0017)\n"
                    "started  2026-10-25 10:00:00.000 (local time)\n"
                    "core     0.3.1\nuri      j2534://x\n"
                    "signals  3: RPM, VIN, Load\n"
                    "surrogate frame logging  off\n"
                    "columns  local time, seconds since the recording "
                    "started (host_us / 1e6 in the CSV files), message\n\n"
                    "10:00:00.100  +0.100  Connected: j2534://x\n"
                    "10:00:07.250  +7.250  Disconnected\n\n"
                    "end      the user disconnected: 4 frames, 5 values, 0 "
                    "frames dropped by the bridge\n");
        f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                                   "\n"
                                   "100000,RPM,812.25,rpm\n"
                                   "200000,VIN,1FAKE,\n"
                                   "300000,RPM,900.5,rpm\n"
                                   "400000,Load,1.123456,%\n"
                                   "500000,Load,7,%\n");
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_recording_info_t info = r.info();
        EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_OLDER) << product;
        EXPECT_EQ(info.flags, 0u) << "its summary is there";
        EXPECT_EQ(info.utc_start_us, 0u);
        EXPECT_EQ(info.duration_us, 7250000u);
        ASSERT_EQ(info.signal_count, 3u);
        const ecuio_recording_signal_t rpm = r.signal(0);
        EXPECT_STREQ(rpm.name, "RPM");
        EXPECT_STREQ(rpm.unit, "rpm");
        EXPECT_EQ(rpm.flags, 0u);
        EXPECT_EQ(rpm.decimal_places, 2);
        EXPECT_EQ(rpm.plot_rank, 0);
        EXPECT_EQ(rpm.lowest, 812.25);
        EXPECT_EQ(rpm.highest, 900.5);
        EXPECT_EQ(r.signal(1).flags, ECUIO_RECORDING_SIGNAL_TEXT);
        EXPECT_EQ(r.signal(2).decimal_places, 3) << "at most 3";
        uint64_t at = 0;
        EXPECT_EQ(r.text_at(1, 1000000, &at), "1FAKE");
        EXPECT_EQ(ecuio_recording_test_table_bytes(r.r), 2 * 16u + 2 * 40u)
            << "one second: the index, VIN's text, RPM's and Load's numbers";
    }
}

TEST(RecordingTest, AHeaderItDoesntKnowOrNoEventsLogStillOpens)
{
    Folder f;
    f.write("signals.csv", std::string(rows::kSignalsHeader) +
                               "\r\n10,RPM,800,rpm\r\n20,RPM,810,rpm\r\n");
    {
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.info().format, ECUIO_RECORDING_FORMAT_UNKNOWN);
        EXPECT_EQ(r.info().flags, ECUIO_RECORDING_INFO_NO_SUMMARY);
        EXPECT_EQ(r.signal(0).samples, 2u) << "CRLF rows";
    }
    f.write("events.log", "ecuio session recording, format 3 (ADR-0099)\n"
                          "signal  Ghost,number,x,0,1\n\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    EXPECT_EQ(r.info().format, ECUIO_RECORDING_FORMAT_UNKNOWN);
    EXPECT_EQ(r.info().signal_count, 1u) << "a newer header's lines unread";
}

TEST(RecordingTest, NoSummarySetsTheFlagAndCountsItsDropLines)
{
    Folder f;
    f.write("events.log",
            events_v2({"RPM,number,rpm,0,1"}, nullptr) +
                "12:00:01.000  +1.000  note  Connected: mock://x\n"
                "12:00:02.000  +2.000  drop  the recording dropped 312 "
                "frames: the disk fell behind\n"
                "12:00:03.000  +3.000  drop  the recording dropped 1 frame: "
                "the disk fell behind\n"
                "12:00:04.000  +4.000  note  a cut-off li");
    f.write("signals.csv",
            std::string(kBom) + rows::kSignalsHeader + "\n10,RPM,800,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_info_t info = r.info();
    EXPECT_EQ(info.flags, ECUIO_RECORDING_INFO_NO_SUMMARY);
    EXPECT_EQ(info.records_dropped, 313u);
    EXPECT_EQ(info.duration_us, 3000000u) << "the torn line isn't read";
}

TEST(RecordingTest, ATornLastRowIsDroppedAndCounted)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}, nullptr));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n10,RPM,800,rpm\n2000000,RPM,8");
    f.write("markers.csv", "host_us,label\n500,a\n90,\"b, c\"\n700,d");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_info_t info = r.info();
    EXPECT_EQ(info.rows_torn, 2u);
    EXPECT_EQ(r.signal(0).samples, 1u);
    EXPECT_EQ(r.signal(0).last_us, 10u);
    ASSERT_EQ(info.marker_count, 2u);
    ecuio_recording_marker_t m{};
    ASSERT_EQ(ecuio_recording_get_marker(r.r, 0, &m), ECUIO_OK);
    EXPECT_EQ(m.host_us, 90u) << "markers in time order";
    EXPECT_STREQ(m.label_utf8, "b, c");
}

TEST(RecordingTest, ABadRowIsRefusedWithItsLineNumber)
{
    struct Case {
        std::string events;
        std::string signals;
        const char* detail;
    };
    const std::string head = std::string(kBom) + rows::kSignalsHeader + "\n";
    const std::string ev = events_v2({"RPM,number,rpm,0,1", "VIN,text,,0,0"});
    const Case cases[] = {
        {ev, head + "10,RPM,800,rpm\n5,RPM,801,rpm\n",
         "signals.csv line 3: host_us is earlier than the row before it"},
        {ev, head + "10," + std::string(32, 'N') + ",1,x\n",
         "signals.csv line 2: the signal's name is empty or longer than 31 "
         "bytes"},
        {ev, head + "10,RPM,1," + std::string(16, 'u') + "\n",
         "signals.csv line 2: the unit is longer than 15 bytes"},
        {ev, head + "10,RPM,fast,rpm\n",
         "signals.csv line 2: the value isn't a number"},
        {ev, head + "10,RPM,800\n",
         "signals.csv line 2: the row isn't host_us,signal,value,unit"},
        {ev, head + "10,RPM," + std::string(40000, '9') + ",rpm\n",
         "signals.csv line 2: the row is longer than a recording's rows can "
         "be"},
        {ev, std::string(kBom) + "host_us,value\n",
         "signals.csv's header row isn't host_us,signal,value,unit"},
        {ev, "", "signals.csv has no header row"},
        {events_v2({"RPM,number,rpm,0,1", "Bad,count,x,0,0"}), head,
         "events.log line 9: the signal's kind isn't number, text or enum"},
    };
    for (const Case& c : cases) {
        Folder f;
        f.write("events.log", c.events);
        f.write("signals.csv", c.signals);
        EXPECT_EQ(open_detail(f.utf8(), ECUIO_ERR_LOAD_FAILED), c.detail);
    }
    Folder none;
    EXPECT_EQ(open_detail(none.utf8(), ECUIO_ERR_LOAD_FAILED),
              "the folder has no signals.csv");
    Folder markers;
    markers.write("signals.csv", head);
    markers.write("markers.csv", "host_us,label\nlater,a\n");
    EXPECT_EQ(open_detail(markers.utf8(), ECUIO_ERR_LOAD_FAILED),
              "markers.csv line 2: host_us isn't a decimal count");
}

/* events.log in format 3 with these header lines (signal and label lines,
 * their keys included) and a summary */
std::string events_v3(const std::vector<std::string>& header_lines)
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

/* Format 3 (ADR-0049's appendix of 2026-11-05): an enum reads as a number
 * with its labels beside it; a format 2 recording still reads as one */
TEST(RecordingTest, AFormat3EnumHasItsLabelsAndFormat2StillReads)
{
    Folder f;
    f.write("events.log",
            events_v3({"signal  Fuel,enum,,0,1,3",
                       "label  Fuel,1,\"Open loop, cold\"",
                       "label  Fuel,2,Closed loop", "label  Fuel,4,Fault",
                       "signal  RPM,number,rpm,0,2,1",
                       "signal  Mode,enum,,0,0,0", "signal  VIN,text,,0,0,0"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n10,Fuel,1,\n20,Fuel,2,\n30,Fuel,4,\n"
                               "40,Fuel,3,\n50,RPM,800,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().format, ECUIO_RECORDING_FORMAT_ENUMS);
    const uint32_t fuel = r.index_of("Fuel");
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t mode = r.index_of("Mode");
    const uint32_t vin = r.index_of("VIN");
    const ecuio_recording_signal_t s = r.signal(fuel);
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNAL_ENUM);
    EXPECT_EQ(s.samples, 4u);
    EXPECT_EQ(s.lowest, 1.0);
    EXPECT_EQ(s.highest, 4.0);
    EXPECT_EQ(s.plot_rank, 1);
    EXPECT_EQ(s.group, 3);
    EXPECT_EQ(r.signal(mode).flags, ECUIO_RECORDING_SIGNAL_ENUM);
    EXPECT_EQ(r.signal(rpm).flags, 0u);
    EXPECT_EQ(r.signal(vin).flags, ECUIO_RECORDING_SIGNAL_TEXT);

    /* The serial ports' contract: all counted, the first capacity copied */
    uint32_t count = 99;
    ASSERT_EQ(ecuio_recording_get_labels(r.r, fuel, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 3u);
    ecuio_enum_entry_t two[3] = {};
    two[2].raw = 77;
    ASSERT_EQ(ecuio_recording_get_labels(r.r, fuel, two, 2, &count), ECUIO_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(two[0].raw, 1u);
    EXPECT_STREQ(two[0].label, "Open loop, cold");
    EXPECT_EQ(two[1].raw, 2u);
    EXPECT_STREQ(two[1].label, "Closed loop");
    EXPECT_EQ(two[2].raw, 77u) << "past the capacity, untouched";
    for (uint32_t other : {rpm, mode, vin}) {
        count = 99;
        EXPECT_EQ(ecuio_recording_get_labels(r.r, other, two, 3, &count),
                  ECUIO_OK);
        EXPECT_EQ(count, 0u) << other;
    }
    EXPECT_EQ(ecuio_recording_get_labels(r.r, fuel, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_labels(r.r, fuel, two, 3, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_labels(nullptr, fuel, two, 3, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_labels(r.r, 4, two, 3, &count),
              ECUIO_ERR_INVALID_ARGUMENT);

    EXPECT_STREQ(ecuio_recording_enum_label(r.r, fuel, 1), "Open loop, cold");
    EXPECT_STREQ(ecuio_recording_enum_label(r.r, fuel, 4), "Fault");
    EXPECT_EQ(ecuio_recording_enum_label(r.r, fuel, 3), nullptr)
        << "a number with no label: a UI shows the number";
    EXPECT_EQ(ecuio_recording_enum_label(r.r, fuel, 0), nullptr);
    EXPECT_EQ(ecuio_recording_enum_label(r.r, fuel, 5), nullptr);
    EXPECT_EQ(ecuio_recording_enum_label(r.r, rpm, 1), nullptr);
    EXPECT_EQ(ecuio_recording_enum_label(r.r, 4, 1), nullptr);
    EXPECT_EQ(ecuio_recording_enum_label(nullptr, fuel, 1), nullptr);

    /* The same recording in format 2, with no enum: its kinds as before */
    Folder older("-v2");
    older.write("events.log", events_v2({"Fuel,number,,0,1,3",
                                         "RPM,number,rpm,0,2,1",
                                         "VIN,text,,0,0,0"}));
    older.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                                   "\n10,Fuel,1,\n50,RPM,800,rpm\n");
    Reader two_r(older.utf8());
    ASSERT_EQ(two_r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(two_r.info().format, ECUIO_RECORDING_FORMAT_CORE);
    EXPECT_EQ(two_r.signal(two_r.index_of("Fuel")).flags, 0u);
    EXPECT_EQ(two_r.signal(two_r.index_of("VIN")).flags,
              ECUIO_RECORDING_SIGNAL_TEXT);
    EXPECT_EQ(ecuio_recording_get_labels(two_r.r, 0, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
}

/* A label line a writer never makes is refused with its line, as a bad
 * signal line is; format 2 has neither enum signals nor label lines */
TEST(RecordingTest, ALabelLineIsRefusedWithItsLineNumber)
{
    const std::string head = std::string(kBom) + rows::kSignalsHeader + "\n";
    const std::string fuel = "signal  Fuel,enum,,0,0,0";
    std::vector<std::string> many = {fuel};
    for (int i = 0; i <= 256; ++i) {
        many.push_back("label  Fuel," + std::to_string(i) + ",L" +
                       std::to_string(i));
    }
    struct Case {
        std::vector<std::string> lines;
        const char* detail;
    };
    const Case cases[] = {
        {{fuel, "label  Gone,1,a"},
         "events.log line 9: the label names no enum signal above it"},
        {{"label  Fuel,1,a", fuel},
         "events.log line 8: the label names no enum signal above it"},
        {{"signal  RPM,number,rpm,0,0", "label  RPM,1,a"},
         "events.log line 9: the label names no enum signal above it"},
        {{fuel, "label  Fuel,2,a", "label  Fuel,1,b"},
         "events.log line 10: the signal's labels aren't in ascending raw "
         "order"},
        {{fuel, "label  Fuel,2,a", "label  Fuel,2,b"},
         "events.log line 10: the signal's labels aren't in ascending raw "
         "order"},
        {{fuel, "label  Fuel,1," + std::string(28, 'x')},
         "events.log line 9: the label is empty or longer than 27 bytes"},
        {many, "events.log line 265: the signal has more labels than an "
               "enum holds"},
    };
    for (const Case& c : cases) {
        Folder f;
        f.write("events.log", events_v3(c.lines));
        f.write("signals.csv", head);
        EXPECT_EQ(open_detail(f.utf8(), ECUIO_ERR_LOAD_FAILED), c.detail);
    }
    /* 256 labels, a def's most, open */
    many.pop_back();
    Folder full;
    full.write("events.log", events_v3(many));
    full.write("signals.csv", head);
    Reader r(full.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    uint32_t count = 0;
    ASSERT_EQ(ecuio_recording_get_labels(r.r, 0, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 256u);

    Folder v2("-v2");
    v2.write("events.log", events_v2({"Fuel,enum,,0,0,0"}));
    v2.write("signals.csv", head);
    EXPECT_EQ(open_detail(v2.utf8(), ECUIO_ERR_LOAD_FAILED),
              "events.log line 8: format 2 has no enum signals");
    /* A label line in format 2 is a header line it doesn't know: read past */
    std::string unknown = events_v2({"Fuel,number,,0,0,0"});
    const std::string cols = "columns  ";
    unknown.insert(unknown.find(cols), "label  Fuel,1,a\n");
    Folder ignored("-ignored");
    ignored.write("events.log", unknown);
    ignored.write("signals.csv", head);
    Reader ri(ignored.utf8());
    ASSERT_EQ(ri.rc, ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_recording_get_labels(ri.r, 0, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
}

/* Format 3's signal file line names signals.json's hash; one that isn't
 * that is refused with its line, and format 2 reads past it as a line it
 * doesn't know. signals.json is looked at whatever the header says. */
TEST(RecordingTest, ASignalFileLineIsReadInFormat3Only)
{
    const std::string head = std::string(kBom) + rows::kSignalsHeader + "\n";
    const std::string json = "{\"havne_signals\": 1}\n";
    const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());
    auto signals = [](const std::string& folder) {
        Reader r(folder);
        EXPECT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        ecuio_recording_signals_t s{};
        EXPECT_EQ(ecuio_recording_get_signals(r.r, &s), ECUIO_OK);
        return s;
    };

    Folder named;
    named.write("events.log", events_v3({"signal file  signals.json," + sha}));
    named.write("signals.csv", head);
    named.write("signals.json", json);
    ecuio_recording_signals_t s = signals(named.utf8());
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_NAMED |
                           ECUIO_RECORDING_SIGNALS_PRESENT |
                           ECUIO_RECORDING_SIGNALS_MATCH);
    EXPECT_EQ(s.length, json.size());
    EXPECT_EQ(std::string(s.sha256), sha);

    /* Another file's hash: present, not the bytes named */
    Folder other("-other");
    other.write("events.log", events_v3({"signal file  signals.json," +
                                         std::string(64, '0')}));
    other.write("signals.csv", head);
    other.write("signals.json", json);
    s = signals(other.utf8());
    EXPECT_EQ(s.flags,
              ECUIO_RECORDING_SIGNALS_NAMED | ECUIO_RECORDING_SIGNALS_PRESENT);

    /* No line: a file there is present, named by nothing */
    Folder unnamed("-unnamed");
    unnamed.write("events.log", events_v3({}));
    unnamed.write("signals.csv", head);
    unnamed.write("signals.json", json);
    s = signals(unnamed.utf8());
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_PRESENT);
    EXPECT_STREQ(s.sha256, "");

    Folder bad("-bad");
    bad.write("events.log", events_v3({"signal file  signals.json,ABC"}));
    bad.write("signals.csv", head);
    EXPECT_EQ(open_detail(bad.utf8(), ECUIO_ERR_LOAD_FAILED),
              "events.log line 8: the signal file line isn't signals.json "
              "and its SHA-256");

    std::string v2 = events_v2({"RPM,number,rpm,0,0"});
    v2.insert(v2.find("columns  "), "signal file  signals.json," + sha + "\n");
    Folder older("-v2");
    older.write("events.log", v2);
    older.write("signals.csv", head);
    s = signals(older.utf8());
    EXPECT_EQ(s.flags, 0u);
    EXPECT_STREQ(s.sha256, "");
}

TEST(RecordingTest, ProgressIsCalledAndCancelsTheOpen)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    struct Seen {
        int calls = 0;
        uint64_t last = 0;
        uint64_t total = 0;
        int cancel_at = -1;
    };
    auto progress = [](uint64_t read, uint64_t total, void* user) {
        auto* s = static_cast<Seen*>(user);
        EXPECT_GE(read, s->last);
        s->last = read;
        s->total = total;
        return s->calls++ == s->cancel_at ? 1 : 0;
    };
    Seen whole;
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(f.utf8().c_str(), progress, &whole, &r),
              ECUIO_OK);
    ecuio_recording_close(r);
    EXPECT_GE(whole.calls, 2);
    EXPECT_EQ(whole.last, whole.total);
    for (int at : {0, 1}) {
        Seen s;
        s.cancel_at = at;
        r = reinterpret_cast<ecuio_recording_t*>(&s);
        EXPECT_EQ(ecuio_recording_open(f.utf8().c_str(), progress, &s, &r),
                  ECUIO_ERR_CANCELED);
        EXPECT_EQ(r, nullptr);
    }
}

TEST(RecordingTest, RowsWrittenAfterTheOpenArentSeenAndAChangeIsSaid)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}, nullptr));
    const std::string head = std::string(kBom) + rows::kSignalsHeader + "\n";
    f.write("signals.csv", head + "10,RPM,800,rpm\n20,RPM,810,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    f.append("signals.csv", "30,RPM,9000,rpm\n");
    const uint32_t rpm = 0;
    ecuio_recording_column_t col{};
    ASSERT_EQ(ecuio_recording_query(r.r, &rpm, 1, 0, 1000, &col, 1), ECUIO_OK);
    EXPECT_EQ(col.samples, 2u);
    EXPECT_EQ(col.highest, 810.0);

    Reader r2(f.utf8());
    ASSERT_EQ(r2.rc, ECUIO_OK);
    f.write("signals.csv", head + "10,RPM,800,rpm\n20,RPM\n");
    ASSERT_EQ(ecuio_recording_query(r2.r, &rpm, 1, 0, 1000, &col, 1),
              ECUIO_ERR_RECORDING_CHANGED);
    uint64_t at = 0;
    char text[16];
    EXPECT_EQ(ecuio_recording_text_at(r2.r, rpm, 25, text, sizeof text, &at),
              ECUIO_ERR_RECORDING_CHANGED);
}

TEST(RecordingTest, TheReaderRefusesWhatItMust)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}));
    f.write("signals.csv",
            std::string(rows::kSignalsHeader) + "\n10,RPM,800,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    ecuio_recording_t* out = nullptr;
    EXPECT_EQ(ecuio_recording_open(nullptr, nullptr, nullptr, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_open(f.utf8().c_str(), nullptr, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_recording_info_t info{};
    ecuio_recording_signal_t sig{};
    ecuio_recording_marker_t mk{};
    EXPECT_EQ(ecuio_recording_get_info(nullptr, &info),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_info(r.r, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_signal(r.r, 1, &sig),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_marker(r.r, 0, &mk),
              ECUIO_ERR_INVALID_ARGUMENT);
    const uint32_t ok = 0;
    const uint32_t past = 1;
    ecuio_recording_column_t col{};
    EXPECT_EQ(ecuio_recording_query(r.r, &past, 1, 0, 10, &col, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_query(r.r, &ok, 1, 10, 10, &col, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_query(r.r, &ok, 0, 0, 10, &col, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_query(r.r, &ok, 1, 0, 10, &col, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_query(r.r, nullptr, 1, 0, 10, &col, 1),
              ECUIO_ERR_NULL_ARGUMENT);
    char text[8];
    uint64_t at = 0;
    EXPECT_EQ(ecuio_recording_text_at(r.r, 1, 0, text, sizeof text, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_text_at(r.r, 0, 0, text, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_text_at(r.r, 0, 0, nullptr, 8, &at),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_recording_find_t fd =
        threshold(0, 900, 0, ECUIO_RECORDING_FIND_RISING);
    EXPECT_EQ(ecuio_recording_find(r.r, &fd, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_recording_find_t bad = fd;
    bad.direction = 3;
    EXPECT_EQ(ecuio_recording_find(r.r, &bad, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    bad = fd;
    bad.hysteresis = -1;
    EXPECT_EQ(ecuio_recording_find(r.r, &bad, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    bad.hysteresis = NAN;
    EXPECT_EQ(ecuio_recording_find(r.r, &bad, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    bad = fd;
    bad.kind = 9;
    EXPECT_EQ(ecuio_recording_find(r.r, &bad, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    bad = fd;
    bad.reserved[3] = 1;
    EXPECT_EQ(ecuio_recording_find(r.r, &bad, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    bad = fd;
    bad.kind = ECUIO_RECORDING_FIND_MARKER;
    EXPECT_EQ(ecuio_recording_find(r.r, &bad, 0, &at), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.find(fd, 0), ECUIO_RECORDING_NONE) << "found nothing is OK";
    ecuio_recording_close(nullptr);
}

// A number is the double nearest it, as strtod gives: a 17-digit text
// (a tool's shortest for its double, or "%.17g") lands on that double,
// not a unit off as a second rounding would, and so does every one of
// many random doubles; a fraction's leading zeros count however many,
// and a German locale's comma changes nothing
TEST(RecordingRowsTest, ANumberIsTheDoubleNearestIt)
{
    double v = 0;
    const char* tool = "1.0212934580499022e-07";
    ASSERT_TRUE(rows::parse_number(tool, std::strlen(tool), &v));
    EXPECT_EQ(v, std::strtod(tool, nullptr));

    uint64_t bits = 0x9E3779B97F4A7C15u;
    int misses = 0;
    for (int i = 0; i < 100000; ++i) {
        bits ^= bits << 13; /* xorshift64 */
        bits ^= bits >> 7;
        bits ^= bits << 17;
        double x = 0;
        std::memcpy(&x, &bits, sizeof x);
        if (!std::isfinite(x)) {
            continue;
        }
        char text[40];
        const int n = std::snprintf(text, sizeof text, "%.17g", x);
        ASSERT_GT(n, 0);
        double back = 0;
        ASSERT_TRUE(rows::parse_number(text, static_cast<size_t>(n), &back))
            << text;
        if (back != x && misses++ < 5) {
            ADD_FAILURE() << text << " read back as another double";
        }
    }
    EXPECT_EQ(misses, 0);

    std::string zeros = "0." + std::string(999999, '0') + "1e1000000";
    ASSERT_TRUE(rows::parse_number(zeros.data(), zeros.size(), &v));
    EXPECT_EQ(v, 1.0) << "the fraction's zeros counted";
    zeros = "-0." + std::string(400, '0') + "5e400";
    ASSERT_TRUE(rows::parse_number(zeros.data(), zeros.size(), &v));
    EXPECT_EQ(v, -0.5);

    const char* before = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = before != nullptr ? before : "C";
    if (std::setlocale(LC_NUMERIC, "de-DE") == nullptr &&
        std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr) {
        GTEST_SKIP() << "no German locale here";
    }
    double comma = 0;
    const bool read = rows::parse_number("0.30000000000000004", 19, &comma);
    std::setlocale(LC_NUMERIC, saved.c_str());
    ASSERT_TRUE(read);
    EXPECT_EQ(comma, 0.1 + 0.2) << "the slow path reads in the C locale";
}

// Past a double's range or under half its least, a value is infinite or
// 0 whatever its digits: the edges either side still read as strtod's
TEST(RecordingRowsTest, AnExponentFarPastTheRangeIsZeroOrInfinite)
{
    double v = 1;
    ASSERT_TRUE(rows::parse_number("1e-999999", 9, &v));
    EXPECT_EQ(v, 0.0);
    EXPECT_FALSE(std::signbit(v));
    ASSERT_TRUE(rows::parse_number("-1e-999999", 10, &v));
    EXPECT_TRUE(v == 0.0 && std::signbit(v));
    ASSERT_TRUE(rows::parse_number("1e999999", 8, &v));
    EXPECT_TRUE(std::isinf(v) && v > 0);
    for (const char* s : {"4.9e-324", "9999999999999999999e-342", "1e308",
                          "1.7976931348623157e308"}) {
        ASSERT_TRUE(rows::parse_number(s, std::strlen(s), &v)) << s;
        EXPECT_GT(v, 0.0) << s;
        EXPECT_FALSE(std::isinf(v)) << s;
    }

    /* Each far exponent at once, never a loop of a step a power of ten:
     * about what an ordinary number costs */
    const auto seconds = [](const char* s) {
        double x = 0;
        const size_t n = std::strlen(s);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; ++i) {
            EXPECT_TRUE(rows::parse_number(s, n, &x));
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             start)
            .count();
    };
    const double plain = seconds("1.2345e+9");
    for (const char* far : {"1e999999", "1e-999999"}) {
        const double t = seconds(far);
        EXPECT_LT(t, 10 * plain + 0.05)
            << far << ": " << t << " s against " << plain;
    }
}

/* A value that isn't finite is written as nan, inf or -inf on every C
 * library (MSVC's %g writes a negative NaN as -nan(ind), which no reader
 * takes), and each reads back */
TEST(RecordingRowsTest, ANonFiniteValueIsWrittenAsAWordEveryReaderTakes)
{
    uint32_t bits = 0xFFC00000u; /* a negative quiet NaN */
    float negative_nan = 0;
    std::memcpy(&negative_nan, &bits, sizeof bits);
    const struct {
        double v;
        const char* text;
    } cases[] = {
        {static_cast<double>(negative_nan), "nan"},
        {std::nan(""), "nan"},
        {HUGE_VAL, "inf"},
        {-HUGE_VAL, "-inf"},
    };
    for (const auto& c : cases) {
        std::string out = "x,";
        rows::append_value(out, c.v);
        EXPECT_EQ(out, std::string("x,") + c.text);
        double back = 0;
        ASSERT_TRUE(rows::parse_number(out.data() + 2, out.size() - 2, &back))
            << out;
        EXPECT_EQ(std::isnan(back), std::isnan(c.v)) << out;
        if (!std::isnan(c.v)) {
            EXPECT_EQ(back, c.v) << out;
        }
    }
    std::string finite;
    rows::append_value(finite, 1726.25);
    EXPECT_EQ(finite, "1726.25");
}

// ---- The span's statistics and the step (ADR-0049 section 5) --------------

namespace {

constexpr uint64_t kNone = ECUIO_RECORDING_NONE;

/* RPM across seconds 0 to 5 (second 1 and 2 whole, 3 one sample at its
 * start, 5 holding only a sample after its middle), and VIN, a text
 * signal */
void write_span_folder(const Folder& f)
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

ecuio_recording_stats_t stats_of(const Reader& r, uint32_t signal, uint64_t t0,
                                 uint64_t t1)
{
    ecuio_recording_stats_t st{};
    EXPECT_EQ(ecuio_recording_stats(r.r, &signal, 1, t0, t1, &st), ECUIO_OK);
    return st;
}

uint64_t step_of(const Reader& r, uint32_t signal, uint64_t t,
                 uint32_t direction)
{
    uint64_t at = 0;
    EXPECT_EQ(ecuio_recording_step(r.r, signal, t, direction, &at), ECUIO_OK);
    return at;
}

/* A raw pass over the hour's samples: what ecuio_recording_stats owes */
struct RawStats {
    uint64_t samples = 0;
    uint64_t valued = 0;
    double sum = 0.0;
    double magnitude = 0.0; /* the sum of |x|, for the mean's bound */
    double lowest = NAN;
    double highest = NAN;
    uint64_t lowest_us = kNone;
    uint64_t highest_us = kNone;
    double first = NAN;
    double last = NAN;
};

RawStats raw_stats(const std::vector<Sample>& samples, uint32_t signal,
                   uint64_t t0, uint64_t t1)
{
    RawStats s;
    for (const Sample& x : samples) {
        if (x.signal != signal) {
            continue;
        }
        if (x.us <= t0) {
            s.first = x.value;
        }
        if (x.us <= t1) {
            s.last = x.value;
        }
        if (x.us < t0 || x.us > t1) {
            continue;
        }
        ++s.samples;
        if (std::isnan(x.value)) {
            continue;
        }
        ++s.valued;
        s.sum += x.value;
        s.magnitude += std::fabs(x.value);
        if (std::isnan(s.lowest) || x.value < s.lowest) {
            s.lowest = x.value;
            s.lowest_us = x.us;
        }
        if (std::isnan(s.highest) || x.value > s.highest) {
            s.highest = x.value;
            s.highest_us = x.us;
        }
    }
    return s;
}

} // namespace

TEST(RecordingStatsTest, ASpanTakesWholeSecondsAndItsPartialEnds)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");

    /* Seconds 1 and 2 whole, 3's sample at the span's end */
    const ecuio_recording_stats_t st = stats_of(r, rpm, 1000000, 3000000);
    EXPECT_EQ(st.samples, 6u);
    EXPECT_EQ(st.first, 300.0);
    EXPECT_EQ(st.last, 400.0);
    EXPECT_EQ(st.lowest, 50.0);
    EXPECT_EQ(st.lowest_us, 2000000u) << "a summary's extreme, its time read";
    EXPECT_EQ(st.highest, 400.0);
    EXPECT_EQ(st.highest_us, 2500000u) << "the first sample at the extreme";
    EXPECT_EQ(st.mean, 1650.0 / 6);
    EXPECT_EQ(st.flags, 0u);
    EXPECT_EQ(st.reserved0, 0u);

    /* Inside one second, its value in force from before the span */
    const ecuio_recording_stats_t in = stats_of(r, rpm, 1200000, 1950000);
    EXPECT_EQ(in.samples, 2u);
    EXPECT_EQ(in.first, 300.0);
    EXPECT_EQ(in.last, 300.0);
    EXPECT_EQ(in.lowest_us, 1400000u);
    EXPECT_EQ(in.highest_us, 1900000u);
    EXPECT_EQ(in.mean, 250.0);

    /* t0's second holds only a later sample: the second before's last */
    EXPECT_EQ(stats_of(r, rpm, 5200000, 5600000).first, 20.0);

    /* The whole recording */
    const ecuio_recording_stats_t all = stats_of(r, rpm, 0, 6000000);
    EXPECT_EQ(all.samples, 10u);
    EXPECT_EQ(all.lowest, 7.0);
    EXPECT_EQ(all.lowest_us, 5500000u);
    EXPECT_EQ(all.mean, 1787.0 / 10);

    /* Before the first sample: nothing in force, nothing in the span */
    const ecuio_recording_stats_t none = stats_of(r, rpm, 0, 400000);
    EXPECT_EQ(none.samples, 0u);
    EXPECT_TRUE(std::isnan(none.first));
    EXPECT_TRUE(std::isnan(none.last));
    EXPECT_TRUE(std::isnan(none.lowest));
    EXPECT_TRUE(std::isnan(none.highest));
    EXPECT_TRUE(std::isnan(none.mean));
    EXPECT_EQ(none.lowest_us, kNone);
    EXPECT_EQ(none.highest_us, kNone);
}

TEST(RecordingStatsTest, StatsCloseTheSpanWhereAQueryLeavesItsEndOut)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    /* A sample exactly at t1: the statistics' closed span counts it */
    EXPECT_EQ(stats_of(r, rpm, 2500000, 3000000).samples, 2u);
    EXPECT_EQ(stats_of(r, rpm, 2500000, 2999999).samples, 1u);
    ecuio_recording_column_t col{};
    ASSERT_EQ(ecuio_recording_query(r.r, &rpm, 1, 2500000, 3000000, &col, 1),
              ECUIO_OK);
    EXPECT_EQ(col.samples, 1u) << "the query's half-open span leaves it out";
}

TEST(RecordingStatsTest, TheMeanLeavesANaNOutAndTakesAnInf)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "1100000,RPM,10,rpm\n"
                               "1200000,RPM,nan,rpm\n"
                               "1300000,RPM,30,rpm\n"
                               "2100000,RPM,inf,rpm\n"
                               "2200000,RPM,-inf,rpm\n"
                               "3100000,RPM,5,rpm\n"
                               "3200000,RPM,inf,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");

    /* A whole second holding a NaN: its rows read again for the mean */
    const ecuio_recording_stats_t nan_second =
        stats_of(r, rpm, 1000000, 1999999);
    EXPECT_EQ(nan_second.samples, 3u) << "the NaN is a sample";
    EXPECT_EQ(nan_second.mean, 20.0) << "and not in the mean";
    EXPECT_EQ(nan_second.lowest, 10.0);
    EXPECT_EQ(nan_second.highest, 30.0);
    EXPECT_EQ(nan_second.highest_us, 1300000u);

    /* An inf and a -inf, no NaN: the second's sum is NaN, so it's read
     * again, and the mean is NaN */
    const ecuio_recording_stats_t both = stats_of(r, rpm, 2000000, 2999999);
    EXPECT_EQ(both.samples, 2u);
    EXPECT_TRUE(std::isnan(both.mean));
    EXPECT_EQ(both.lowest, -HUGE_VAL);
    EXPECT_EQ(both.highest, HUGE_VAL);

    /* An inf alone: the summary's sum is inf, and so is the mean */
    const ecuio_recording_stats_t inf = stats_of(r, rpm, 3000000, 3999999);
    EXPECT_EQ(inf.samples, 2u);
    EXPECT_EQ(inf.mean, HUGE_VAL);
    EXPECT_EQ(inf.highest, HUGE_VAL);
    EXPECT_EQ(inf.highest_us, 3200000u);
    EXPECT_EQ(inf.lowest, 5.0);
}

TEST(RecordingStatsTest, ATextSignalCountsItsSamplesAlone)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t vin = r.index_of("VIN");
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t both[] = {vin, rpm};
    ecuio_recording_stats_t st[2];
    std::memset(st, 0xAB, sizeof st);
    ASSERT_EQ(ecuio_recording_stats(r.r, both, 2, 0, 6000000, st), ECUIO_OK);
    EXPECT_EQ(st[0].samples, 2u);
    EXPECT_TRUE(std::isnan(st[0].first));
    EXPECT_TRUE(std::isnan(st[0].last));
    EXPECT_TRUE(std::isnan(st[0].lowest));
    EXPECT_TRUE(std::isnan(st[0].highest));
    EXPECT_TRUE(std::isnan(st[0].mean));
    EXPECT_EQ(st[0].lowest_us, kNone);
    EXPECT_EQ(st[0].highest_us, kNone);
    EXPECT_EQ(st[0].flags, 0u);
    EXPECT_EQ(st[0].reserved[2], 0u);
    EXPECT_EQ(st[1].samples, 10u) << "each entry its own signal's";
    EXPECT_EQ(stats_of(r, vin, 600000, 2500000).samples, 1u)
        << "a partial second's text rows read raw";
}

TEST(RecordingStatsTest, StatsRefuseWhatTheyCantAnswer)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t past = r.info().signal_count;
    ecuio_recording_stats_t st{};
    EXPECT_EQ(ecuio_recording_stats(nullptr, &rpm, 1, 0, 1, &st),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, nullptr, 1, 0, 1, &st),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 1, 0, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 0, 0, 1, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 1, 5, 5, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 1, 6, 5, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &past, 1, 0, 1, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST(RecordingStatsTest, TheSummariesGiveWhatARawPassGivesOverTheHour)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    const uint64_t end = r.info().duration_us;
    const std::pair<uint64_t, uint64_t> spans[] = {
        {0, end},
        {123456789, 2345678901},
        {1000000, 1000000000},
        {59999999, 61000001},
        {1800000000, 1800400000},
    };
    for (uint32_t d = 0; d < h.defs.size(); ++d) {
        if (h.defs[d].text) {
            continue;
        }
        const uint32_t k = r.index_of(h.defs[d].name.c_str());
        for (const auto& span : spans) {
            const RawStats want =
                raw_stats(h.samples, d, span.first, span.second);
            const ecuio_recording_stats_t got =
                stats_of(r, k, span.first, span.second);
            SCOPED_TRACE(h.defs[d].name + " " + std::to_string(span.first) +
                         " to " + std::to_string(span.second));
            EXPECT_EQ(got.samples, want.samples);
            if (want.valued == 0) {
                EXPECT_TRUE(std::isnan(got.mean));
                continue;
            }
            EXPECT_EQ(got.lowest, want.lowest);
            EXPECT_EQ(got.highest, want.highest);
            EXPECT_EQ(got.lowest_us, want.lowest_us);
            EXPECT_EQ(got.highest_us, want.highest_us);
            /* NaN when nothing is in force yet, which no == matches */
            EXPECT_TRUE(std::isnan(want.first) ? std::isnan(got.first)
                                               : got.first == want.first)
                << got.first << " for " << want.first;
            EXPECT_TRUE(std::isnan(want.last) ? std::isnan(got.last)
                                              : got.last == want.last)
                << got.last << " for " << want.last;
            /* The summaries add each second's sum, a raw pass each
             * sample: they agree to a rounding of the magnitudes summed,
             * which a mean near zero after cancellation still meets */
            const double magnitude_bound =
                1e-12 * want.magnitude / static_cast<double>(want.valued);
            EXPECT_NEAR(got.mean, want.sum / static_cast<double>(want.valued),
                        magnitude_bound);
        }
    }
}

TEST(RecordingStatsTest, AStepFindsTheNextOrTheLastSample)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t vin = r.index_of("VIN");
    constexpr uint32_t fwd = ECUIO_RECORDING_STEP_FORWARD;
    constexpr uint32_t back = ECUIO_RECORDING_STEP_BACKWARD;
    EXPECT_EQ(step_of(r, rpm, 0, fwd), 500000u);
    EXPECT_EQ(step_of(r, rpm, 500000, fwd), 1000000u) << "strictly after";
    EXPECT_EQ(step_of(r, rpm, 1450000, fwd), 1900000u);
    EXPECT_EQ(step_of(r, rpm, 1900000, fwd), 2000000u)
        << "into the next second";
    EXPECT_EQ(step_of(r, rpm, 4000000, fwd), 5500000u)
        << "past an empty second";
    EXPECT_EQ(step_of(r, rpm, 5500000, fwd), kNone);
    EXPECT_EQ(step_of(r, rpm, 5500000, back), 4000000u);
    EXPECT_EQ(step_of(r, rpm, 5200000, back), 4000000u)
        << "t's second holds only a later sample";
    EXPECT_EQ(step_of(r, rpm, 1000000, back), 500000u) << "strictly before";
    EXPECT_EQ(step_of(r, rpm, 1400000, back), 1000000u);
    EXPECT_EQ(step_of(r, rpm, 500000, back), kNone);
    EXPECT_EQ(step_of(r, rpm, 99000000, back), 5500000u);
    EXPECT_EQ(step_of(r, vin, 700000, fwd), 2600000u) << "a text signal's";
    EXPECT_EQ(step_of(r, vin, 2600000, back), 700000u);

    uint64_t at = 0;
    EXPECT_EQ(ecuio_recording_step(nullptr, rpm, 0, fwd, &at),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, rpm, 0, fwd, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, rpm, 0, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, rpm, 0, 3, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, r.info().signal_count, 0, fwd, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
}

// ---- The navigation aids (ADR-0056's appendix "the navigation aids") -----

namespace {

/* A signal of a written drive: its name, unit, rank and value by ms */
struct Track {
    const char* name;
    const char* unit;
    unsigned rank;
    double (*at)(uint64_t ms);
};

/* Every track sampled each 100 ms from 0 to end_ms */
void write_drive(const Folder& f, const std::vector<Track>& tracks,
                 uint64_t end_ms)
{
    std::vector<std::string> lines;
    for (const Track& t : tracks) {
        lines.push_back(std::string(t.name) + ",number," + t.unit + ",1," +
                        std::to_string(t.rank));
    }
    f.write("events.log", events_v2(lines));
    std::string csv = std::string(kBom) + rows::kSignalsHeader + "\n";
    for (uint64_t ms = 0; ms <= end_ms; ms += 100) {
        for (const Track& t : tracks) {
            csv += std::to_string(ms * 1000) + "," + t.name + ",";
            rows::append_value(csv, t.at(ms));
            csv += std::string(",") + t.unit + "\n";
        }
    }
    f.write("signals.csv", csv);
}

/* A pull from 5 s (40 to 110 km/h by 10 s, then the limiter's flat speed
 * to 12 s) and a cruise from 20 s to 45 s at 100 km/h, 30 km/h between */
double drive_speed(uint64_t ms)
{
    if (ms < 5000) {
        return 40.0;
    }
    if (ms < 10000) {
        return 40.0 + static_cast<double>(ms - 5000) * 0.014;
    }
    if (ms < 12000) {
        return 110.0;
    }
    if (ms >= 20000 && ms < 45000) {
        return (ms / 100) % 2 == 0 ? 99.6 : 100.4;
    }
    return 30.0;
}

double drive_throttle(uint64_t ms)
{
    if (ms >= 5000 && ms < 12000) {
        return 95.0;
    }
    if (ms >= 20000 && ms < 45000) {
        return (ms / 100) % 2 == 0 ? 19.0 : 21.0;
    }
    return 10.0;
}

double drive_rpm(uint64_t ms)
{
    return 1000.0 + drive_speed(ms) * 40.0;
}

ecuio_stretch_rules_t both_kinds()
{
    ecuio_stretch_rules_t r{};
    r.kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    r.throttle = UINT32_MAX;
    r.engine_load = UINT32_MAX;
    r.vehicle_speed = UINT32_MAX;
    r.engine_speed = UINT32_MAX;
    return r;
}

std::vector<ecuio_recording_stretch_t>
stretches_of(const Reader& r, const ecuio_stretch_rules_t& rules, uint64_t from,
             uint64_t to)
{
    uint32_t n = 99;
    EXPECT_EQ(ecuio_recording_stretches(r.r, &rules, from, to, nullptr, 0, &n),
              ECUIO_OK);
    std::vector<ecuio_recording_stretch_t> out(n);
    uint32_t again = 0;
    EXPECT_EQ(
        ecuio_recording_stretches(r.r, &rules, from, to, out.data(), n, &again),
        ECUIO_OK);
    EXPECT_EQ(again, n);
    return out;
}

uint32_t role_of(const Reader& r, uint32_t role)
{
    uint32_t at = 7;
    EXPECT_EQ(ecuio_recording_role_signal(r.r, role, &at), ECUIO_OK);
    return at;
}

/* A road log's shape: every signal read in one poll loop at 3 to 4 Hz,
 * its steps uneven. The instants the cases measure from are sampled
 * exactly, the loop's own sample within 100 ms of one left out. */
const uint64_t kLogInstants[] = {12000, 13300, 20000, 24000, 24925, 28000,
                                 34000, 37000, 38400, 41000, 43200, 47000};

std::vector<uint64_t> log_times(uint64_t end_ms)
{
    static const uint64_t kSteps[] = {250, 330, 290, 310, 270};
    std::vector<uint64_t> out(std::begin(kLogInstants), std::end(kLogInstants));
    size_t n = 0;
    for (uint64_t ms = 0; ms <= end_ms; ms += kSteps[n++ % 5]) {
        bool near = false;
        for (uint64_t at : kLogInstants) {
            near = near || (ms > at ? ms - at : at - ms) < 100;
        }
        if (!near) {
            out.push_back(ms);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void write_log(const Folder& f, const std::vector<Track>& tracks,
               uint64_t end_ms)
{
    std::vector<std::string> lines;
    for (const Track& t : tracks) {
        lines.push_back(std::string(t.name) + ",number," + t.unit + ",1," +
                        std::to_string(t.rank));
    }
    f.write("events.log", events_v2(lines));
    std::string csv = std::string(kBom) + rows::kSignalsHeader + "\n";
    for (uint64_t ms : log_times(end_ms)) {
        for (const Track& t : tracks) {
            csv += std::to_string(ms * 1000) + "," + t.name + ",";
            rows::append_value(csv, t.at(ms));
            csv += std::string(",") + t.unit + "\n";
        }
    }
    f.write("signals.csv", csv);
}

/* The value at ms on the line through knots, held past its ends */
double along(const std::vector<std::pair<uint64_t, double>>& knots, uint64_t ms)
{
    if (ms <= knots.front().first) {
        return knots.front().second;
    }
    for (size_t i = 1; i < knots.size(); ++i) {
        if (ms <= knots[i].first) {
            const double span =
                static_cast<double>(knots[i].first - knots[i - 1].first);
            return knots[i - 1].second +
                   (knots[i].second - knots[i - 1].second) *
                       static_cast<double>(ms - knots[i - 1].first) / span;
        }
    }
    return knots.back().second;
}

/* The log's drive. Part throttle through 3000 rpm from 4 s to 9 s; a
 * stab at the throttle from 12 s to 13.3 s (+8 km/h, +180 rpm); a pull
 * through an upshift from 20 s to 28 s, the lift between its gears 925 ms
 * from its first sample to the next at the level, engine speed ending
 * under where it began; a pull from 34 s whose lift is 1.4 s; and 2.2 s
 * after that one's throttle closes, a pull in top gear from 43.2 s to
 * 47 s (+15 km/h, +2000 rpm). */
double log_throttle(uint64_t ms)
{
    if (ms >= 12000 && ms <= 13300) {
        return 90.0;
    }
    if ((ms >= 20000 && ms < 24000) || (ms >= 24925 && ms <= 28000) ||
        (ms >= 34000 && ms < 37000) || (ms >= 38400 && ms < 41000) ||
        (ms >= 43200 && ms <= 47000)) {
        return 95.0;
    }
    if (ms >= 4000 && ms < 9000) {
        return 45.0;
    }
    return (ms >= 24000 && ms < 24925) || (ms >= 37000 && ms < 38400) ? 20.0
                                                                      : 12.0;
}

double log_speed(uint64_t ms)
{
    return along({{4000, 50.0},
                  {9000, 65.0},
                  {12000, 65.0},
                  {13300, 73.0},
                  {20000, 90.0},
                  {24000, 112.0},
                  {24925, 114.0},
                  {28000, 130.0},
                  {34000, 80.0},
                  {37000, 100.0},
                  {38400, 101.0},
                  {41000, 118.0},
                  {43200, 120.0},
                  {47000, 135.0}},
                 ms);
}

double log_rpm(uint64_t ms)
{
    return along({{4000, 2400.0},
                  {9000, 3300.0},
                  {12000, 2500.0},
                  {13300, 2680.0},
                  {19000, 2700.0},
                  {20000, 5500.0},
                  {24000, 6500.0},
                  {24925, 4600.0},
                  {28000, 5200.0},
                  {34000, 3000.0},
                  {37000, 5800.0},
                  {38400, 4000.0},
                  {41000, 5200.0},
                  {43200, 4300.0},
                  {47000, 6300.0}},
                 ms);
}

const uint64_t kLogEndMs = 52000;

ecuio_stretch_rules_t pulls_only()
{
    ecuio_stretch_rules_t r = both_kinds();
    r.kinds = ECUIO_STRETCH_PULLS;
    return r;
}

} // namespace

TEST(RecordingStretchesTest, EachRoleIsPickedByItsUnitAndName)
{
    Folder f;
    f.write(
        "events.log",
        events_v2({"Commanded Throttle,number,%,1,0",
                   "Relative Throttle Position,number,%,1,0",
                   "Throttle Position,number,%,1,0",
                   "Accelerator Pedal D,number,%,1,3",
                   "Engine Load,number,%,1,0", "Wheel Speed,number,MPH,1,0",
                   "Engine Speed,number,RPM,0,0", "Speed Note,text,km/h,0,0"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "1000,Commanded Throttle,1,%\n"
                               "1000,Relative Throttle Position,1,%\n"
                               "1000,Throttle Position,1,%\n"
                               "1000,Accelerator Pedal D,1,%\n"
                               "1000,Engine Load,1,%\n"
                               "1000,Wheel Speed,1,MPH\n"
                               "1000,Engine Speed,800,RPM\n"
                               "1000,Speed Note,x,km/h\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_THROTTLE),
              r.index_of("Accelerator Pedal D"))
        << "the ranked one, neither relative nor commanded";
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_ENGINE_LOAD),
              r.index_of("Engine Load"));
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_VEHICLE_SPEED),
              r.index_of("Wheel Speed"))
        << "mph in any case, never a text signal";
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_ENGINE_SPEED),
              r.index_of("Engine Speed"));

    uint32_t at = 0;
    EXPECT_EQ(ecuio_recording_role_signal(r.r, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_role_signal(r.r, 5, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_role_signal(nullptr, 1, &at),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_role_signal(r.r, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    Folder none;
    write_span_folder(none);
    Reader n(none.utf8());
    ASSERT_EQ(n.rc, ECUIO_OK);
    EXPECT_EQ(role_of(n, ECUIO_RECORDING_ROLE_THROTTLE), UINT32_MAX);
    EXPECT_EQ(role_of(n, ECUIO_RECORDING_ROLE_VEHICLE_SPEED), UINT32_MAX);
}

TEST(RecordingStretchesTest, ADrivesPullAndCruiseAreFoundInTimeOrder)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t speed = r.index_of("Vehicle Speed");
    const std::vector<ecuio_recording_stretch_t> all =
        stretches_of(r, both_kinds(), 0, 60000000);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_EQ(all[0].signal, speed);
    EXPECT_EQ(all[0].from_us, 5000000u);
    EXPECT_EQ(all[0].to_us, 10000000u) << "the limiter's flat speed ends it";
    EXPECT_EQ(all[0].start, 40.0);
    EXPECT_EQ(all[0].end, 110.0);
    EXPECT_EQ(all[0].flags, 0u);
    EXPECT_EQ(all[1].kind, ECUIO_RECORDING_STRETCH_CRUISE);
    EXPECT_EQ(all[1].signal, speed);
    EXPECT_EQ(all[1].from_us, 20000000u);
    EXPECT_EQ(all[1].to_us, 44900000u);
    EXPECT_NEAR(all[1].start, 100.0, 1e-9);
    EXPECT_EQ(all[1].start, all[1].end);
    EXPECT_EQ(all[1].flags, 0u);
    EXPECT_EQ(all[1].reserved0, 0u);
    EXPECT_EQ(all[1].reserved[1], 0u);

    /* One kind at a time */
    ecuio_stretch_rules_t pulls = both_kinds();
    pulls.kinds = ECUIO_STRETCH_PULLS;
    EXPECT_EQ(stretches_of(r, pulls, 0, 60000000).size(), 1u);

    /* A range cuts each stretch reaching past it and measures it again */
    const std::vector<ecuio_recording_stretch_t> cut =
        stretches_of(r, both_kinds(), 7000000, 30000000);
    ASSERT_EQ(cut.size(), 2u);
    EXPECT_EQ(cut[0].from_us, 7000000u);
    EXPECT_EQ(cut[0].flags, ECUIO_RECORDING_STRETCH_CUT_START);
    EXPECT_EQ(cut[0].start, 68.0);
    EXPECT_EQ(cut[1].to_us, 30000000u);
    EXPECT_EQ(cut[1].flags, ECUIO_RECORDING_STRETCH_CUT_END);
    EXPECT_NEAR(cut[1].start, 100.0, 0.01);

    /* The capacity copies the first; the count is all */
    ecuio_recording_stretch_t one{};
    uint32_t n = 0;
    ecuio_stretch_rules_t rules = both_kinds();
    ASSERT_EQ(ecuio_recording_stretches(r.r, &rules, 0, 60000000, &one, 1, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(one.kind, ECUIO_RECORDING_STRETCH_PULL);

    /* A level the user sets: 96 % finds no pull */
    rules.pull_level = 96.0;
    EXPECT_EQ(stretches_of(r, rules, 0, 60000000).size(), 1u);
}

namespace {

/* A cancel that stops at its nth ask (0: never), counting its asks */
struct StopAt {
    int at = 0;
    int asked = 0;
    static int ask(void* user)
    {
        auto* s = static_cast<StopAt*>(user);
        return ++s->asked == s->at ? 1 : 0;
    }
};

} // namespace

/* stretches_with asks its cancel once the arguments pass and before each
 * second's rows: never stopping gives the plain call's stretches, and a
 * stop at each ask in turn ends the call there, CANCELED with a count of
 * 0 and the detail's words, asking no more */
TEST(RecordingStretchesTest, ACancelStopsTheSearchAtItsNextSecond)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_stretch_rules_t rules = both_kinds();
    const std::vector<ecuio_recording_stretch_t> plain =
        stretches_of(r, rules, 0, 60000000);
    ASSERT_EQ(plain.size(), 2u);

    StopAt never;
    ecuio_recording_stretch_t got[4] = {};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_recording_stretches_with(r.r, &rules, 0, 60000000,
                                             StopAt::ask, &never, got, 4, &n),
              ECUIO_OK);
    ASSERT_EQ(n, 2u);
    for (uint32_t k = 0; k < n; ++k) {
        EXPECT_EQ(got[k].from_us, plain[k].from_us);
        EXPECT_EQ(got[k].to_us, plain[k].to_us);
    }
    EXPECT_EQ(never.asked, 62) << "once, then each of the 61 seconds";

    for (int at = 1; at <= never.asked; ++at) {
        StopAt stop{at};
        n = 99;
        ASSERT_EQ(ecuio_recording_stretches_with(
                      r.r, &rules, 0, 60000000, StopAt::ask, &stop, got, 4, &n),
                  ECUIO_ERR_CANCELED)
            << "at " << at;
        EXPECT_EQ(n, 0u);
        EXPECT_EQ(stop.asked, at) << "asked again after its stop";
        EXPECT_STREQ(ecuio_last_error_detail(), "the search was canceled");
    }

    /* A refusal comes before any ask; a NULL cancel is the plain call */
    StopAt refused;
    ecuio_stretch_rules_t none = rules;
    none.kinds = 0;
    EXPECT_EQ(ecuio_recording_stretches_with(r.r, &none, 0, 60000000,
                                             StopAt::ask, &refused, got, 4, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(refused.asked, 0);
    ASSERT_EQ(ecuio_recording_stretches_with(r.r, &rules, 0, 60000000, nullptr,
                                             nullptr, got, 4, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 2u);
}

/* find_with asks its cancel once the arguments pass and before each
 * second whose rows it reads, never one its summary passes by: a stop at
 * each ask ends the search CANCELED with nothing found */
TEST(RecordingStretchesTest, ACancelStopsAFindAtItsNextSecond)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_find_t rising = threshold(
        r.index_of("Vehicle Speed"), 100, 0, ECUIO_RECORDING_FIND_RISING);
    const uint64_t plain = r.find(rising, 0);
    ASSERT_EQ(plain, 9300000u);

    StopAt never;
    uint64_t at = 0;
    ASSERT_EQ(
        ecuio_recording_find_with(r.r, &rising, 0, StopAt::ask, &never, &at),
        ECUIO_OK);
    EXPECT_EQ(at, plain);
    EXPECT_EQ(never.asked, 3)
        << "once, then second 0 that arms it and second 9 that fires";
    for (int k = 1; k <= never.asked; ++k) {
        StopAt stop{k};
        at = 0;
        EXPECT_EQ(
            ecuio_recording_find_with(r.r, &rising, 0, StopAt::ask, &stop, &at),
            ECUIO_ERR_CANCELED);
        EXPECT_EQ(at, ECUIO_RECORDING_NONE);
        EXPECT_EQ(stop.asked, k);
        EXPECT_STREQ(ecuio_last_error_detail(), "the search was canceled");
    }

    /* An engine start and a marker ask once before they look */
    ecuio_recording_find_t start{};
    start.kind = ECUIO_RECORDING_FIND_ENGINE_START;
    ecuio_recording_find_t marker{};
    marker.kind = ECUIO_RECORDING_FIND_MARKER;
    marker.label_utf8 = "lap";
    for (const ecuio_recording_find_t* what : {&start, &marker}) {
        StopAt once;
        EXPECT_EQ(
            ecuio_recording_find_with(r.r, what, 0, StopAt::ask, &once, &at),
            ECUIO_OK);
        EXPECT_GE(once.asked, 1);
        StopAt first{1};
        EXPECT_EQ(
            ecuio_recording_find_with(r.r, what, 0, StopAt::ask, &first, &at),
            ECUIO_ERR_CANCELED);
        EXPECT_EQ(at, ECUIO_RECORDING_NONE);
        EXPECT_EQ(first.asked, 1);
    }

    /* A refusal comes before any ask; a NULL cancel is the plain call */
    StopAt refused;
    ecuio_recording_find_t bad = rising;
    bad.flags = 1;
    EXPECT_EQ(
        ecuio_recording_find_with(r.r, &bad, 0, StopAt::ask, &refused, &at),
        ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(refused.asked, 0);
    ASSERT_EQ(ecuio_recording_find_with(r.r, &rising, 0, nullptr, nullptr, &at),
              ECUIO_OK);
    EXPECT_EQ(at, plain);
}

TEST(RecordingStretchesTest, TheFallbacksAreFlagged)
{
    Folder load;
    write_drive(load,
                {{"Engine Load", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed}},
                60000);
    Reader l(load.utf8());
    ASSERT_EQ(l.rc, ECUIO_OK);
    const std::vector<ecuio_recording_stretch_t> by_load =
        stretches_of(l, both_kinds(), 0, 60000000);
    ASSERT_EQ(by_load.size(), 2u);
    EXPECT_EQ(by_load[0].flags, ECUIO_RECORDING_STRETCH_BY_LOAD);
    EXPECT_EQ(by_load[1].flags, ECUIO_RECORDING_STRETCH_NO_THROTTLE);

    Folder rpm;
    write_drive(rpm,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader e(rpm.utf8());
    ASSERT_EQ(e.rc, ECUIO_OK);
    const std::vector<ecuio_recording_stretch_t> by_rpm =
        stretches_of(e, both_kinds(), 0, 60000000);
    ASSERT_EQ(by_rpm.size(), 1u) << "no speed: no cruise";
    EXPECT_EQ(by_rpm[0].flags, ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
    EXPECT_EQ(by_rpm[0].signal, e.index_of("RPM"));
    EXPECT_EQ(by_rpm[0].to_us, 11900000u) << "engine speed keeps the whole run";
}

TEST(RecordingStretchesTest, TheFloorIsThirtyFiveMphInEitherUnit)
{
    /* 1 mph is 1.609344 km/h exactly, so the default is 35 mph to the bit */
    ecuio_display_value_t shown{};
    ASSERT_EQ(ecuio_display_convert("km/h", 2,
                                    ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR,
                                    ECUIO_UNITS_IMPERIAL, &shown),
              ECUIO_DISPLAY_OK);
    EXPECT_NEAR(shown.value, 35.0, 1e-12);
    EXPECT_STREQ(shown.unit, "mph");

    struct Case {
        const char* unit;
        double (*speed)(uint64_t);
        bool found;
    };
    const Case cases[] = {
        {"km/h", [](uint64_t) { return 50.0; }, false},
        {"km/h", [](uint64_t) { return 60.0; }, true},
        {"mph", [](uint64_t) { return 30.0; }, false},
        {"mph", [](uint64_t) { return 40.0; }, true},
    };
    for (const Case& c : cases) {
        Folder f;
        write_drive(f, {{"Vehicle Speed", c.unit, 0, c.speed}}, 15000);
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK);
        EXPECT_EQ(stretches_of(r, both_kinds(), 0, 15000000).size(),
                  c.found ? 1u : 0u)
            << c.speed(0) << " " << c.unit;
    }
}

TEST(RecordingStretchesTest, StretchesRefuseRulesTheyCantRun)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    uint32_t n = 0;
    ecuio_recording_stretch_t one{};
    const ecuio_stretch_rules_t good = both_kinds();
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 0, 1, nullptr, 0, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 0u) << "no throttle, load or speed: nothing found";
    EXPECT_EQ(ecuio_recording_stretches(nullptr, &good, 0, 1, nullptr, 0, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, nullptr, 0, 1, nullptr, 0, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 0, 1, nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 0, 1, nullptr, 1, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 5, 5, &one, 1, &n),
              ECUIO_ERR_INVALID_ARGUMENT);

    const auto refused = [&](void (*spoil)(ecuio_stretch_rules_t&),
                             const char* why) {
        ecuio_stretch_rules_t bad = both_kinds();
        spoil(bad);
        EXPECT_EQ(ecuio_recording_stretches(r.r, &bad, 0, 1, &one, 1, &n),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << why;
    };
    refused([](ecuio_stretch_rules_t& b) { b.kinds = 0; }, "no kind");
    refused([](ecuio_stretch_rules_t& b) { b.kinds = 0x4; }, "unknown kind");
    refused([](ecuio_stretch_rules_t& b) { b.flags = 1; }, "a flag");
    refused([](ecuio_stretch_rules_t& b) { b.reserved[0] = 1; }, "reserved");
    refused([](ecuio_stretch_rules_t& b) { b.reserved[1] = 1; }, "reserved");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_rise = -1; },
            "a negative rise");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_rise = NAN; },
            "a NaN rise");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_gain = -0.5; },
            "a negative gain");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_gain = NAN; },
            "a NaN gain");
    refused([](ecuio_stretch_rules_t& b) { b.pull_level = -1; }, "negative");
    refused([](ecuio_stretch_rules_t& b) { b.cruise_speed_band = NAN; }, "NaN");
    refused([](ecuio_stretch_rules_t& b) { b.cruise_floor = -0.5; },
            "negative floor");
    refused([](ecuio_stretch_rules_t& b) { b.vehicle_speed = 99; },
            "an index past the signals");
    ecuio_stretch_rules_t text = both_kinds();
    text.throttle = r.index_of("VIN");
    EXPECT_EQ(ecuio_recording_stretches(r.r, &text, 0, 1, &one, 1, &n),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a text signal in a role";
}

TEST(RecordingStretchesTest, ARoadLogsPullsAreFoundAndItsStabIsnt)
{
    Folder f;
    write_log(f,
              {{"Throttle Position", "%", 0, log_throttle},
               {"Vehicle Speed", "km/h", 0, log_speed},
               {"RPM", "rpm", 0, log_rpm}},
              kLogEndMs);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint64_t end = kLogEndMs * 1000 + 1;
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, pulls_only(), 0, end);
    ASSERT_EQ(found.size(), 3u)
        << "neither the part throttle nor the stab is a pull";

    /* The 925 ms lift between two gears stays inside its pull */
    EXPECT_EQ(found[0].from_us, 20000000u);
    EXPECT_EQ(found[0].to_us, 28000000u);
    EXPECT_EQ(found[0].start, 90.0);
    EXPECT_EQ(found[0].end, 130.0);
    EXPECT_EQ(found[0].flags, 0u);

    /* So does a 1.4 s one; the pull ends at the last sample before its
     * throttle closes at 41 s */
    EXPECT_EQ(found[1].from_us, 34000000u);
    EXPECT_GE(found[1].to_us, 40600000u);
    EXPECT_LT(found[1].to_us, 41000000u);

    /* 2.2 s on, the next pull is its own: in top gear, 15 km/h gained */
    EXPECT_EQ(found[2].from_us, 43200000u);
    EXPECT_EQ(found[2].to_us, 47000000u);
    EXPECT_EQ(found[2].start, 120.0);
    EXPECT_EQ(found[2].end, 135.0);

    /* Where the gap's default sits: a second splits the 1.4 s lift, and
     * 2.5 s joins the two pulls 2.2 s apart */
    ecuio_stretch_rules_t rules = pulls_only();
    rules.pull_gap_us = 1000000;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 4u);
    rules.pull_gap_us = 2500000;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 2u);
    rules.pull_gap_us = ECUIO_STRETCH_DEFAULT_PULL_GAP_US;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* The rise the user sets: 150 rpm lets the stab in (+180), 2500 keeps
     * only the pull that climbs 2800 */
    rules = pulls_only();
    rules.pull_min_rise = 150.0;
    const std::vector<ecuio_recording_stretch_t> with_stab =
        stretches_of(r, rules, 0, end);
    ASSERT_EQ(with_stab.size(), 4u);
    EXPECT_EQ(with_stab[0].from_us, 12000000u);
    EXPECT_EQ(with_stab[0].to_us, 13300000u);
    EXPECT_EQ(with_stab[0].end - with_stab[0].start, 8.0);
    rules.pull_min_rise = 2500.0;
    const std::vector<ecuio_recording_stretch_t> steep =
        stretches_of(r, rules, 0, end);
    ASSERT_EQ(steep.size(), 1u);
    EXPECT_EQ(steep[0].from_us, 34000000u);
    rules.pull_min_rise = ECUIO_STRETCH_DEFAULT_PULL_RISE;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* With engine speed in the log, the gain is no floor at all: one past
     * the top-gear pull's 15 km/h drops nothing */
    rules = pulls_only();
    rules.pull_min_gain = 16.0;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* A range cutting the top-gear pull at 45.95 s leaves over its hold
     * and under 730 rpm to climb, counting from the sample in force there:
     * what is left is no pull; from 45.2 s, over 900 rpm, it is */
    EXPECT_EQ(stretches_of(r, pulls_only(), 45950000, end).size(), 0u);
    const std::vector<ecuio_recording_stretch_t> cut =
        stretches_of(r, pulls_only(), 45200000, end);
    ASSERT_EQ(cut.size(), 1u);
    EXPECT_EQ(cut[0].flags, ECUIO_RECORDING_STRETCH_CUT_START);
}

TEST(RecordingStretchesTest, WithNoEngineSpeedAPullGainsItsSpeed)
{
    Folder f;
    write_log(f,
              {{"Throttle Position", "%", 0, log_throttle},
               {"Vehicle Speed", "km/h", 0, log_speed}},
              kLogEndMs);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint64_t end = kLogEndMs * 1000 + 1;
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, pulls_only(), 0, end);
    ASSERT_EQ(found.size(), 3u) << "the stab's 8 km/h is under the gain";
    EXPECT_EQ(found[0].from_us, 20000000u);
    EXPECT_EQ(found[2].from_us, 43200000u);

    /* The gain the user sets: 5 km/h lets the stab in, 16 drops the pull
     * in top gear, which is why engine speed's climb is the floor
     * wherever the log has it */
    ecuio_stretch_rules_t rules = pulls_only();
    rules.pull_min_gain = 5.0;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 4u);
    rules.pull_min_gain = 16.0;
    const std::vector<ecuio_recording_stretch_t> fast =
        stretches_of(r, rules, 0, end);
    ASSERT_EQ(fast.size(), 2u);
    EXPECT_EQ(fast[1].from_us, 34000000u);
    rules.pull_min_gain = ECUIO_STRETCH_DEFAULT_PULL_GAIN;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* The rise is engine speed's: with none it changes nothing */
    rules = pulls_only();
    rules.pull_min_rise = 1e6;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* The gain is in km/h: the same numbers in mph make the stab's 8 mph
     * 12.9 km/h, past it */
    Folder m;
    write_log(m,
              {{"Throttle Position", "%", 0, log_throttle},
               {"Vehicle Speed", "mph", 0, log_speed}},
              kLogEndMs);
    Reader mph(m.utf8());
    ASSERT_EQ(mph.rc, ECUIO_OK);
    EXPECT_EQ(stretches_of(mph, pulls_only(), 0, end).size(), 4u);
    rules = pulls_only();
    rules.pull_min_gain = 13.0;
    EXPECT_EQ(stretches_of(mph, rules, 0, end).size(), 3u);
}

TEST(RecordingStretchesTest, APullThroughAnUpshiftCountsByEngineSpeedAlone)
{
    Folder f;
    write_log(f,
              {{"Throttle Position", "%", 0, log_throttle},
               {"RPM", "rpm", 0, log_rpm}},
              kLogEndMs);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, pulls_only(), 0, kLogEndMs * 1000 + 1);
    ASSERT_EQ(found.size(), 3u) << "the stab climbs 180 rpm";
    for (const ecuio_recording_stretch_t& s : found) {
        EXPECT_EQ(s.flags, ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
    }
    EXPECT_EQ(found[0].from_us, 20000000u);
    EXPECT_EQ(found[0].to_us, 28000000u);
    EXPECT_EQ(found[0].start, 5500.0);
    EXPECT_EQ(found[0].end, 5200.0)
        << "it ends under where it began, having climbed 1000 rpm";
}

TEST(RecordingStretchesTest, TheMockDriveCyclesPullsAndCruiseAreFound)
{
    /* One loop of the mock's drive cycle, each tick's frame decoded as a
     * recording of the mock holds it (whole km/h, the throttle's bytes) */
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &set), ECUIO_OK);
    const uint16_t wanted[] = {ECUIO_MOCK_SIG_RPM_ID, ECUIO_MOCK_SIG_SPEED_ID,
                               ECUIO_MOCK_SIG_THROTTLE_ID,
                               ECUIO_MOCK_SIG_LOAD_ID};
    std::vector<const ecuio_signal_def_t*> defs;
    std::vector<std::string> lines;
    for (uint16_t id : wanted) {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].id == id) {
                defs.push_back(&set->defs[i]);
                lines.push_back(std::string(set->defs[i].name) + ",number," +
                                set->defs[i].unit + ",1,0");
            }
        }
    }
    ASSERT_EQ(defs.size(), 4u);
    Folder f;
    f.write("events.log", events_v2(lines));
    std::string csv = std::string(kBom) + rows::kSignalsHeader + "\n";
    std::map<int, std::pair<uint64_t, uint64_t>> phases;
    ecuio_mock::DriveCycle dc;
    while (dc.loops < 1) {
        dc.step();
        const uint64_t us = dc.tick * 50000;
        auto& span = phases[static_cast<int>(dc.phase)];
        if (span.first == 0) {
            span.first = us;
        }
        span.second = us;
        uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN];
        ecuio_mock::encode_payload(dc, payload);
        for (const ecuio_signal_def_t* d : defs) {
            double v = 0;
            ASSERT_EQ(ecuio_decode_signal(d, payload,
                                          ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &v),
                      0);
            csv += std::to_string(us) + "," + d->name + ",";
            rows::append_value(csv, v);
            csv += std::string(",") + d->unit + "\n";
        }
    }
    f.write("signals.csv", csv);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, both_kinds(), 0, r.info().duration_us + 1);
    using ecuio_mock::Phase;
    const auto span = [&](Phase p) { return phases[static_cast<int>(p)]; };
    const auto within = [](const ecuio_recording_stretch_t& s, uint64_t lo,
                           uint64_t hi) {
        return s.from_us >= lo && s.to_us <= hi;
    };
    ASSERT_EQ(found.size(), 4u);
    for (const ecuio_recording_stretch_t& s : found) {
        EXPECT_EQ(s.flags, 0u);
    }

    /* Each gear's pull ends where the limiter's flat speed begins: in
     * whole km/h, up to a tick before rpm meets the limit */
    const uint64_t tick = 50000;
    EXPECT_EQ(found[0].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(found[0], span(Phase::Pull2nd).first,
                       span(Phase::Limiter2nd).first));
    EXPECT_GE(found[0].to_us + tick, span(Phase::Pull2nd).second);
    EXPECT_EQ(found[1].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(found[1], span(Phase::Pull3rd).first,
                       span(Phase::Limiter3rd).first));
    EXPECT_GE(found[1].to_us + tick, span(Phase::Pull3rd).second);
    EXPECT_GT(found[1].end, found[0].end) << "third gear runs faster";

    /* The cruise in fourth, at its 100 km/h */
    EXPECT_EQ(found[2].kind, ECUIO_RECORDING_STRETCH_CRUISE);
    EXPECT_TRUE(within(found[2], span(Phase::Cruise4th).first,
                       span(Phase::Cruise4th).second + 100000));
    EXPECT_GE(found[2].to_us - found[2].from_us,
              ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US);
    EXPECT_NEAR(found[2].start, 100.0, 2.0);

    /* The quarter mile is one pull once the clutch locks (the throttle is
     * feathered under the level while it slips): every upshift's lift
     * (about 0.55 s under the level) inside the gap. It ends at its top
     * speed, which can be read a tick past the line, the pedal still down */
    EXPECT_EQ(found[3].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(found[3], span(Phase::DragRun).first,
                       span(Phase::DragRun).second + tick));
    EXPECT_GT(found[3].to_us - found[3].from_us, 10000000u);

    /* The two-step's launch at a standstill is no pull: a full-throttle
     * run of its own, 1.7 s before the quarter mile's, creeping 2 km/h
     * while engine speed climbs some 540 rpm onto the two-step. A rise of
     * 400 rpm lets it in. */
    ecuio_stretch_rules_t low_rise = both_kinds();
    low_rise.pull_min_rise = 400.0;
    const std::vector<ecuio_recording_stretch_t> with_launch =
        stretches_of(r, low_rise, 0, r.info().duration_us + 1);
    ASSERT_EQ(with_launch.size(), 5u);
    EXPECT_EQ(with_launch[3].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(with_launch[3], span(Phase::Launch).first,
                       span(Phase::DragRun).first));
    EXPECT_LT(with_launch[3].end - with_launch[3].start, 5.0);

    /* A gap of 500 ms splits the quarter mile at each of its three
     * upshifts */
    ecuio_stretch_rules_t short_gap = both_kinds();
    short_gap.pull_gap_us = 500000;
    EXPECT_EQ(stretches_of(r, short_gap, 0, r.info().duration_us + 1).size(),
              7u);
}
