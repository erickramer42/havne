// File/layer: tests/test_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingRowsTest: recording_rows.h's parsers; HARNESS:
//          RecordingTest: the reader (ADR-0049 section 5) on a folder the
//          recorder wrote (the replay fixture, with a marker), on an
//          hour of the fixture's own values at their own times, and on
//          folders written by hand (an older recording, torn and refused
//          rows, no summary); every query checked against a scan of the
//          rows the test wrote, from several threads too, and the memory
//          bound and the cache through the seams in internal.h
//
// Each case writes into a folder of its own under TEMP/TMPDIR, removed at
// the end.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <vector>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "recording_rows.h" /* core/src */
#include "recording_hour.h"

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
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") /
            (std::string("ecuio-recording-") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name() +
             tag);
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
    for (const char* b :
         {"RPM,count,rpm,0,1", "RPM,number,rpm,4,1", "RPM,number,rpm,0,256",
          "RPM,number,rpm,0", "RPM,number,rpm,0,1,x"}) {
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
    EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_CORE);
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
    /* 32 bytes per number signal per second it has samples in, 16 per
     * text signal's, 16 per second for the index */
    std::set<std::pair<uint32_t, uint64_t>> signal_seconds;
    std::set<uint64_t> seconds;
    for (const Sample& s : h.samples) {
        signal_seconds.emplace(s.signal, s.us / 1000000);
        seconds.insert(s.us / 1000000);
    }
    const uint64_t bound = 32 * signal_seconds.size() + 16 * seconds.size();
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
        EXPECT_EQ(ecuio_recording_test_table_bytes(r.r), 2 * 16u + 2 * 32u)
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
         "events.log line 9: the signal's kind isn't number or text"},
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
