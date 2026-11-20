// File/layer: tests/test_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingRowsTest, RecordingTest (test_core)
//
// Each case writes into a folder of its own under TEMP/TMPDIR, removed at
// the end.

#include "test_recording_support.h"

namespace {

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
    /* Each of the clock's places must be a digit */
    for (const size_t d : {0, 1, 3, 4, 6, 7, 9, 10, 11}) {
        std::string bad = ev;
        bad[d] = 'x';
        EXPECT_FALSE(rows::parse_event_line(bad.data(), bad.size(), &e)) << d;
    }

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

/* A cond line's text is written fired, stale, held in that order, and
 * each text the writer gives parses back to its fields */
TEST(RecordingRowsTest, ACondLinesTextIsWrittenInOneOrderAndReadsBack)
{
    rows::cond_line c;
    std::memcpy(c.key, "boost_low", 9);
    c.on = true;
    c.value = -3.5;
    c.term = 2;
    c.host_us = 1500000;
    c.t_us = 1450000;
    const std::pair<uint32_t, const char*> cases[] = {
        {0, ""},
        {rows::kCondFired, "fired"},
        {rows::kCondStale | rows::kCondHeld, "stale held"},
        {rows::kCondFired | rows::kCondStale | rows::kCondHeld,
         "fired stale held"},
    };
    for (const auto& k : cases) {
        c.flags = k.first;
        std::string text;
        rows::append_cond_text(text, c);
        EXPECT_EQ(text, std::string("boost_low,on,") + k.second +
                            ",-3.5,2,1500000,1450000");
        rows::cond_line back;
        ASSERT_TRUE(rows::parse_cond_line(text.data(), text.size(), &back))
            << text;
        EXPECT_STREQ(back.key, c.key);
        EXPECT_EQ(back.flags, c.flags) << text;
        EXPECT_EQ(back.value, c.value);
        EXPECT_EQ(back.term, c.term);
        EXPECT_EQ(back.host_us, c.host_us);
        EXPECT_EQ(back.t_us, c.t_us);
    }
}

/* The recorder's line for lost condition events reads back as its count,
 * and no other drop line reads as one */
TEST(RecordingRowsTest, ALostEventsLinesTextReadsBackAsItsCount)
{
    const std::pair<uint64_t, const char*> cases[] = {
        {1, "the recording lost 1 condition event: the disk fell behind"},
        {312, "the recording lost 312 condition events: the disk fell behind"},
        {UINT64_MAX, "the recording lost 18446744073709551615 condition "
                     "events: the disk fell behind"},
    };
    for (const auto& k : cases) {
        std::string text;
        rows::append_lost_text(text, k.first);
        EXPECT_EQ(text, k.second);
        uint64_t back = 0;
        ASSERT_TRUE(rows::parse_lost(text.data(), text.size(), &back)) << text;
        EXPECT_EQ(back, k.first);
    }
    for (const std::string text :
         {"the recording dropped 3 frames: the disk fell behind",
          "the recording lost 3 frames: the disk fell behind",
          "the recording lost  condition events: the disk fell behind",
          "the recording lost x condition events: the disk fell behind",
          "the recording lost 3condition events: the disk fell behind",
          "the recording lost 3 condition", "the recording lost 3",
          "the recording lost ", "",
          "the recording lost 18446744073709551616 condition events: x"}) {
        uint64_t count = 7;
        EXPECT_FALSE(rows::parse_lost(text.data(), text.size(), &count))
            << text;
        EXPECT_EQ(count, 7u) << text;
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
    EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_KEYS);
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
