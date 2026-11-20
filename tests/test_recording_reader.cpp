// File/layer: tests/test_recording_reader.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingTest (test_core)

#include "test_recording_support.h"

namespace {

std::string open_detail(const std::string& folder, ecuio_status_t want)
{
    ecuio_recording_t* r = reinterpret_cast<ecuio_recording_t*>(&want);
    EXPECT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r), want);
    EXPECT_EQ(r, nullptr);
    ecuio_recording_close(r);
    return ecuio_last_error_detail();
}

} // namespace

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

/* conditions.json beside a recording reads as signals.json does, each
 * copy by its own line and file (ADR-0063 section 8); a conditions line
 * that isn't the copy's names none and the open goes on */
TEST(RecordingTest, AConditionsLineAndItsCopyAreReadAsTheSignalFiles)
{
    const std::string head = std::string(kBom) + rows::kSignalsHeader + "\n";
    const std::string json = "{\"havne_conditions\":1,\"conditions\":[]}\n";
    const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());
    const std::string line = "conditions  conditions.json," + sha;
    const uint32_t all = ECUIO_RECORDING_CONDITIONS_NAMED |
                         ECUIO_RECORDING_CONDITIONS_PRESENT |
                         ECUIO_RECORDING_CONDITIONS_MATCH;

    /* Both copies, each matching its own line */
    const std::string signals = "{\"havne_signals\": 1}\n";
    const std::string signals_sha =
        ecuio_sha256::hex_of(signals.data(), signals.size());
    Folder both;
    both.write("events.log",
               events_v3({"signal file  signals.json," + signals_sha, line}));
    both.write("signals.csv", head);
    both.write("signals.json", signals);
    both.write("conditions.json", json);
    {
        Reader r(both.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        ecuio_recording_conditions_t c{};
        ecuio_recording_signals_t s{};
        ASSERT_EQ(ecuio_recording_get_conditions(r.r, &c), ECUIO_OK);
        ASSERT_EQ(ecuio_recording_get_signals(r.r, &s), ECUIO_OK);
        EXPECT_EQ(c.flags, all);
        EXPECT_EQ(c.length, json.size());
        EXPECT_EQ(std::string(c.sha256), sha);
        EXPECT_EQ(c.lines_skipped, 0u);
        EXPECT_EQ(s.flags, all) << "the signal file's copy is its own";
        EXPECT_EQ(s.length, signals.size());
        EXPECT_EQ(std::string(s.sha256), signals_sha);
        EXPECT_EQ(ecuio_recording_get_conditions(r.r, nullptr),
                  ECUIO_ERR_NULL_ARGUMENT);
        EXPECT_EQ(ecuio_recording_get_conditions(nullptr, &c),
                  ECUIO_ERR_NULL_ARGUMENT);
    }

    /* Another file's hash: present, not the bytes named */
    Folder other("-other");
    other.write("events.log", events_v3({"conditions  conditions.json," +
                                         std::string(64, '0')}));
    other.write("signals.csv", head);
    other.write("conditions.json", json);
    ecuio_recording_conditions_t c = conditions_of(other.utf8());
    EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_NAMED |
                           ECUIO_RECORDING_CONDITIONS_PRESENT);

    /* No line: a file there is present, named by nothing */
    Folder unnamed("-unnamed");
    unnamed.write("events.log", events_v3({}));
    unnamed.write("signals.csv", head);
    unnamed.write("conditions.json", json);
    c = conditions_of(unnamed.utf8());
    EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_PRESENT);
    EXPECT_STREQ(c.sha256, "");

    /* A line and no file */
    Folder absent("-absent");
    absent.write("events.log", events_v3({line}));
    absent.write("signals.csv", head);
    c = conditions_of(absent.utf8());
    EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_NAMED);
    EXPECT_EQ(c.length, 0u);
    EXPECT_EQ(std::string(c.sha256), sha);

    /* A line that isn't the copy's names nothing */
    for (const std::string& bad :
         {std::string("conditions  conditions.json,ABC"),
          "conditions  signals.json," + sha,
          "conditions  conditions.json," + sha.substr(1)}) {
        Folder f("-bad");
        f.write("events.log", events_v3({bad}));
        f.write("signals.csv", head);
        f.write("conditions.json", json);
        c = conditions_of(f.utf8());
        EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_PRESENT) << bad;
        EXPECT_STREQ(c.sha256, "") << bad;
    }

    /* Format 2 has no such line */
    std::string v2 = events_v2({"RPM,number,rpm,0,0"});
    v2.insert(v2.find("columns  "), line + "\n");
    Folder older("-v2");
    older.write("events.log", v2);
    older.write("signals.csv", head);
    c = conditions_of(older.utf8());
    EXPECT_EQ(c.flags, 0u);
    EXPECT_STREQ(c.sha256, "");
}
