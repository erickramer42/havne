// File/layer: tests/test_recording_hour.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingTest, RecordingRowsTest (test_core)

#include "test_recording_support.h"

namespace {

/* events_v3's events.log with these activity lines before its end line */
std::string events_with(const std::vector<std::string>& header_lines,
                        const std::string& activity)
{
    std::string s = events_v3(header_lines);
    s.insert(s.find("12:00:10.000"), activity);
    return s;
}

std::string cond_at(unsigned second, const std::string& text)
{
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "12:00:%02u.000  +%u.000  ", second,
                  second);
    return stamp + std::string("cond  ") + text + "\n";
}

} // namespace

/* Each cond line in the file's order with its seven fields; a flag word
 * the reader doesn't know and fields after the seventh passed by; a line
 * that doesn't parse skipped and counted, the rest read as before */
TEST(RecordingTest, CondLinesAreReadInOrderAndABadOneIsSkippedAndCounted)
{
    const std::string good =
        cond_at(1, "coolant_high,on,fired,112,0,1000000,999000") +
        cond_at(1, "coolant_high,off,stale,nan,1,1500000,1499000,x,y") +
        cond_at(2, "pull_2,on,fired held blink,-0.5,3,2000000,"
                   "18446744073709551615") +
        cond_at(2, "low,on,,1e300,4294967295,2500000,2499000") +
        cond_at(3, "coolant_high,on,fired,113,0,3000000,2999000");
    const std::vector<std::string> bad = {
        "Upper,on,,1,0,1,1", std::string(32, 'k') + ",on,,1,0,1,1",
        ",on,,1,0,1,1",      "a,maybe,,1,0,1,1",
        "a,on,,x,0,1,1",     "a,on,,1,4294967296,1,1",
        "a,on,,1,-1,1,1",    "a,on,,1,0,1",
        "\"a\",on,,1,0,1,1", "a,on,,1,0,1,18446744073709551616",
    };
    std::string activity = good;
    for (const std::string& b : bad) {
        activity += cond_at(4, b);
    }
    const std::string rows = std::string(rows::kSignalsHeader) +
                             "\n1000000,RPM,800,rpm\n2000000,RPM,900,rpm\n";
    Folder f;
    f.write("events.log", events_with({"signal  RPM,number,rpm,0,1"},
                                      cond_at(0, "a,on,,1,0,0,0") + activity));
    f.write("signals.csv", rows);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_info_t info = r.info();
    EXPECT_EQ(info.condition_events, 6u);
    EXPECT_EQ(info.flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT, 0u);
    ecuio_recording_conditions_t c{};
    ASSERT_EQ(ecuio_recording_get_conditions(r.r, &c), ECUIO_OK);
    EXPECT_EQ(c.lines_skipped, bad.size());
    EXPECT_EQ(c.flags, 0u) << "no line names a copy";

    struct Want {
        const char* key;
        uint32_t state, flags;
        double value;
        uint32_t term;
        uint64_t host_us, t_us;
    };
    const Want want[] = {
        {"a", ECUIO_CONDITION_ON, 0, 1, 0, 0, 0},
        {"coolant_high", ECUIO_CONDITION_ON, ECUIO_CONDITION_FIRED, 112, 0,
         1000000, 999000},
        {"coolant_high", ECUIO_CONDITION_OFF, ECUIO_CONDITION_STALE, NAN, 1,
         1500000, 1499000},
        {"pull_2", ECUIO_CONDITION_ON,
         ECUIO_CONDITION_FIRED | ECUIO_CONDITION_HELD, -0.5, 3, 2000000,
         UINT64_MAX},
        {"low", ECUIO_CONDITION_ON, 0, 1e300, UINT32_MAX, 2500000, 2499000},
        {"coolant_high", ECUIO_CONDITION_ON, ECUIO_CONDITION_FIRED, 113, 0,
         3000000, 2999000},
    };
    for (size_t i = 0; i < 6; ++i) {
        ecuio_recording_condition_event_t e;
        std::memset(&e, 0xA5, sizeof e);
        ASSERT_EQ(ecuio_recording_get_condition_event(r.r, i, &e), ECUIO_OK);
        EXPECT_STREQ(e.key, want[i].key) << i;
        EXPECT_EQ(e.state, want[i].state) << i;
        EXPECT_EQ(e.flags, want[i].flags) << i;
        if (std::isnan(want[i].value)) {
            EXPECT_TRUE(std::isnan(e.value)) << i;
        } else {
            EXPECT_EQ(e.value, want[i].value) << i;
        }
        EXPECT_EQ(e.term, want[i].term) << i;
        EXPECT_EQ(e.host_us, want[i].host_us) << i;
        EXPECT_EQ(e.timestamp_us, want[i].t_us) << i;
        EXPECT_EQ(e.reserved0, 0u) << i;
        EXPECT_EQ(e.reserved[0] | e.reserved[1], 0u) << i;
        const void* end = std::memchr(e.key, '\0', sizeof e.key);
        ASSERT_NE(end, nullptr) << i;
        for (const char* k = static_cast<const char*>(end); k < e.key + 32;
             ++k) {
            EXPECT_EQ(*k, '\0') << "the key's room past it is zero, " << i;
        }
    }
    ecuio_recording_condition_event_t past;
    std::memset(&past, 0x5A, sizeof past);
    const ecuio_recording_condition_event_t kept = past;
    EXPECT_EQ(ecuio_recording_get_condition_event(r.r, 6, &past),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&past, &kept, sizeof past), 0) << "out untouched";
    EXPECT_EQ(ecuio_recording_get_condition_event(r.r, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_get_condition_event(nullptr, 0, &past),
              ECUIO_ERR_NULL_ARGUMENT);

    /* The same recording with no cond lines reads the same otherwise */
    Folder plain("-plain");
    plain.write("events.log", events_v3({"signal  RPM,number,rpm,0,1"}));
    plain.write("signals.csv", rows);
    Reader p(plain.utf8());
    ASSERT_EQ(p.rc, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_recording_info_t without = p.info();
    EXPECT_EQ(without.condition_events, 0u);
    ecuio_recording_conditions_t none{};
    ASSERT_EQ(ecuio_recording_get_conditions(p.r, &none), ECUIO_OK);
    EXPECT_EQ(none.lines_skipped, 0u);
    EXPECT_EQ(ecuio_recording_get_condition_event(p.r, 0, &past),
              ECUIO_ERR_INVALID_ARGUMENT);
    without.condition_events = info.condition_events;
    EXPECT_EQ(std::memcmp(&without, &info, sizeof info), 0);
    const ecuio_recording_signal_t a = r.signal(0);
    const ecuio_recording_signal_t b = p.signal(0);
    EXPECT_EQ(std::memcmp(&a, &b, sizeof a), 0);
}

/* Past ECUIO_RECORDING_MAX_CONDITION_EVENTS the first that many are kept
 * and every line is counted */
TEST(RecordingTest, CondLinesPastTheCapAreCountedAndTheFirstKept)
{
    const uint64_t cap = ECUIO_RECORDING_MAX_CONDITION_EVENTS;
    std::string activity;
    activity.reserve((cap + 2) * 48);
    for (uint64_t i = 0; i < cap + 2; ++i) {
        activity += "12:00:00.000  +0.000  cond  ";
        activity += (i % 2 == 0) ? "a,on,," : "b,off,,";
        activity += std::to_string(i) + ",0,0," + std::to_string(i) + "\n";
    }
    Folder f;
    f.write("events.log", events_with({}, activity));
    f.write("signals.csv", std::string(rows::kSignalsHeader) + "\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_info_t info = r.info();
    EXPECT_EQ(info.condition_events, cap + 2);
    EXPECT_NE(info.flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT, 0u);
    ecuio_recording_condition_event_t e{};
    ASSERT_EQ(ecuio_recording_get_condition_event(r.r, cap - 1, &e), ECUIO_OK);
    EXPECT_EQ(e.timestamp_us, cap - 1);
    EXPECT_STREQ(e.key, "b");
    EXPECT_EQ(ecuio_recording_get_condition_event(r.r, cap, &e),
              ECUIO_ERR_INVALID_ARGUMENT);

    /* At the cap exactly, nothing is cut */
    for (int k = 0; k < 2; ++k) {
        activity.resize(activity.rfind("12:00:00.000"));
    }
    Folder g("-at-cap");
    g.write("events.log", events_with({}, activity));
    g.write("signals.csv", std::string(rows::kSignalsHeader) + "\n");
    Reader s(g.utf8());
    ASSERT_EQ(s.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(s.info().condition_events, cap);
    EXPECT_EQ(s.info().flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT, 0u);
}

/* A set holds at most ECUIO_CONDITION_MAX_CONDITIONS keys: a line naming
 * one more is none a writer makes, skipped and counted, and the keys
 * already held go on being read */
TEST(RecordingTest, CondLinesPastASetsKeysAreSkippedAndCounted)
{
    const uint32_t most = ECUIO_CONDITION_MAX_CONDITIONS;
    std::string activity;
    for (uint32_t k = 0; k <= most; ++k) {
        activity += cond_at(1, "k" + std::to_string(k) + ",on,,1,0,1,1");
    }
    activity += cond_at(2, "k0,off,,2,0,2,2");
    activity += cond_at(2, "k" + std::to_string(most) + ",off,,2,0,2,2");
    activity += cond_at(2, "k" + std::to_string(most - 1) + ",off,,2,0,2,2");
    Folder f;
    f.write("events.log", events_with({}, activity));
    f.write("signals.csv", std::string(rows::kSignalsHeader) + "\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().condition_events, most + 2u);
    ecuio_recording_conditions_t c{};
    ASSERT_EQ(ecuio_recording_get_conditions(r.r, &c), ECUIO_OK);
    EXPECT_EQ(c.lines_skipped, 2u) << "the key past a set's most, twice";
    const std::string want[] = {"k0", "k" + std::to_string(most - 1)};
    for (uint32_t i = 0; i < 2; ++i) {
        ecuio_recording_condition_event_t e{};
        ASSERT_EQ(ecuio_recording_get_condition_event(r.r, most + i, &e),
                  ECUIO_OK);
        EXPECT_EQ(std::string(e.key), want[i]);
        EXPECT_EQ(e.state, ECUIO_CONDITION_OFF);
    }
}

/* A crash's torn last line and a line past the longest row are no lines a
 * reader takes: a cond line among them is counted as one that doesn't
 * parse, and a line of another kind isn't */
TEST(RecordingTest, ATornOrOverLongCondLineIsSkippedAndCounted)
{
    const std::string head = std::string(rows::kSignalsHeader) + "\n";
    const std::string long_tail(rows::kRowMax, 'x');
    Folder f;
    f.write("events.log",
            events_with({}, cond_at(1, "a,on,,1,0,1,1") +
                                cond_at(2, "a,off,,1,0,2,2," + long_tail) +
                                "12:00:03.000  +3.000  note  " + long_tail +
                                "\n") +
                "12:00:11.000  +11.000  cond  a,on,fired,3,0,11000000,"
                "11000000");
    f.write("signals.csv", head);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().condition_events, 1u);
    ecuio_recording_conditions_t c{};
    ASSERT_EQ(ecuio_recording_get_conditions(r.r, &c), ECUIO_OK);
    EXPECT_EQ(c.lines_skipped, 2u) << "the over-long and the torn cond line";

    Folder note("-note");
    note.write("events.log", events_v3({}) + "12:00:11.000  +11.000  note  a");
    note.write("signals.csv", head);
    EXPECT_EQ(conditions_of(note.utf8()).lines_skipped, 0u);
}

/* Only format 3 holds cond lines: in an older or unknown events.log a line
 * of that kind is neither read nor counted */
TEST(RecordingTest, CondLinesAreReadInFormat3Alone)
{
    const std::string head = std::string(rows::kSignalsHeader) + "\n";
    const std::string lines =
        cond_at(1, "a,on,,1,0,1,1") + cond_at(2, "Bad,on,,1,0,1,1");
    std::string v2 = events_v2({});
    v2.insert(v2.find("12:00:10.000"), lines);
    const std::string unknown = "a log of some other tool\n\n" + lines;
    for (const std::string& events : {v2, unknown}) {
        Folder f;
        f.write("events.log", events);
        f.write("signals.csv", head);
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.info().condition_events, 0u) << events.substr(0, 40);
        EXPECT_EQ(conditions_of(f.utf8()).lines_skipped, 0u)
            << events.substr(0, 40);
    }
    Folder v3("-v3");
    v3.write("events.log", events_with({}, lines));
    v3.write("signals.csv", head);
    Reader r(v3.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().condition_events, 1u) << "the same lines in format 3";
    EXPECT_EQ(conditions_of(v3.utf8()).lines_skipped, 1u);
}

/* The condition events a recorder lost are the sum of its drop lines that
 * count them, in format 3 alone; a frames' drop line is none of them, and
 * a sum past a uint64 stays at its most */
TEST(RecordingTest, LostConditionEventsAreTheSumOfTheirDropLines)
{
    const std::string head = std::string(rows::kSignalsHeader) + "\n";
    const auto drop_at = [](unsigned second, const std::string& text) {
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "12:00:%02u.000  +%u.000  ", second,
                      second);
        return stamp + std::string("drop  ") + text + "\n";
    };
    const std::string lines =
        cond_at(1, "a,on,fired,1,0,1,1") +
        drop_at(2, "the recording lost 3 condition events: the disk fell "
                   "behind") +
        drop_at(3, "the recording dropped 40 frames: the disk fell behind") +
        drop_at(4, "the recording lost 1 condition event: the disk fell "
                   "behind") +
        drop_at(5, "the recording lost 9 notes: the disk fell behind");
    Folder v3;
    v3.write("events.log", events_with({}, lines));
    v3.write("signals.csv", head);
    ecuio_recording_conditions_t c = conditions_of(v3.utf8());
    EXPECT_EQ(c.events_lost, 4u);
    EXPECT_EQ(c.lines_skipped, 0u) << "a drop line is no cond line";
    {
        Reader r(v3.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.info().condition_events, 1u);
        EXPECT_EQ(r.info().records_dropped, 0u) << "the end line's count";
    }

    /* With no end line the frames' drop lines are the records dropped, and
     * the lost events are none of them */
    std::string cut = events_with({}, lines);
    cut.erase(cut.find("12:00:10.000"));
    Folder torn("-no-end");
    torn.write("events.log", cut);
    torn.write("signals.csv", head);
    {
        Reader r(torn.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(r.info().flags, ECUIO_RECORDING_INFO_NO_SUMMARY);
        EXPECT_EQ(r.info().records_dropped, 40u)
            << "the frames' drop line alone";
    }
    EXPECT_EQ(conditions_of(torn.utf8()).events_lost, 4u);

    Folder none("-none");
    none.write("events.log", events_with({}, cond_at(1, "a,on,,1,0,1,1")));
    none.write("signals.csv", head);
    EXPECT_EQ(conditions_of(none.utf8()).events_lost, 0u);

    std::string v2 = events_v2({});
    v2.insert(v2.find("12:00:10.000"), lines);
    Folder older("-v2");
    older.write("events.log", v2);
    older.write("signals.csv", head);
    EXPECT_EQ(conditions_of(older.utf8()).events_lost, 0u)
        << "format 2 holds no conditions";

    Folder most("-most");
    most.write("events.log",
               events_with({}, drop_at(1, "the recording lost "
                                          "18446744073709551615 condition "
                                          "events: x") +
                                   drop_at(2, "the recording lost 2 condition "
                                              "events: x")));
    most.write("signals.csv", head);
    EXPECT_EQ(conditions_of(most.utf8()).events_lost, UINT64_MAX);
}

/* A copy no loader takes (past ECUIO_CONDITION_FILE_MAX_BYTES) is present
 * and never MATCH, its own hash named or not; one at the bound matches */
TEST(RecordingTest, AConditionsCopyPastTheLoadersBoundNeverMatches)
{
    const std::string head = std::string(rows::kSignalsHeader) + "\n";
    for (const size_t size : {static_cast<size_t>(ECUIO_CONDITION_FILE_MAX_BYTES),
                              ECUIO_CONDITION_FILE_MAX_BYTES + size_t{1}}) {
        const std::string json(size, ' ');
        const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());
        Folder f;
        f.write("events.log",
                events_v3({"conditions  conditions.json," + sha}));
        f.write("signals.csv", head);
        f.write("conditions.json", json);
        const ecuio_recording_conditions_t c = conditions_of(f.utf8());
        const bool loads = size <= ECUIO_CONDITION_FILE_MAX_BYTES;
        EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_NAMED |
                               ECUIO_RECORDING_CONDITIONS_PRESENT |
                               (loads ? ECUIO_RECORDING_CONDITIONS_MATCH : 0u))
            << size;
        EXPECT_EQ(c.length, size);
        EXPECT_EQ(std::string(c.sha256), sha);
    }
}

/* signals.json takes its loader's bound as conditions.json does, a
 * set's, since the copy is a session's whole set: one at
 * ECUIO_SIGNAL_SET_MAX_BYTES matches, one byte more never does */
TEST(RecordingTest, ASignalFilePastTheLoadersBoundNeverMatches)
{
    const std::string head = std::string(rows::kSignalsHeader) + "\n";
    for (const size_t size :
         {static_cast<size_t>(ECUIO_SIGNAL_FILE_MAX_BYTES) + 1,
          static_cast<size_t>(ECUIO_SIGNAL_SET_MAX_BYTES),
          ECUIO_SIGNAL_SET_MAX_BYTES + size_t{1}}) {
        const std::string json(size, ' ');
        const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());
        Folder f;
        f.write("events.log", events_v3({"signal file  signals.json," + sha}));
        f.write("signals.csv", head);
        f.write("signals.json", json);
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        ecuio_recording_signals_t s{};
        ASSERT_EQ(ecuio_recording_get_signals(r.r, &s), ECUIO_OK);
        const bool loads = size <= ECUIO_SIGNAL_SET_MAX_BYTES;
        EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_NAMED |
                               ECUIO_RECORDING_SIGNALS_PRESENT |
                               (loads ? ECUIO_RECORDING_SIGNALS_MATCH : 0u))
            << size;
        EXPECT_EQ(s.length, size);
        EXPECT_EQ(std::string(s.sha256), sha);
    }
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

/* A long events.log is asked through too: every 4 MiB of it, bytes_read
 * 0 and bytes_total the data file's, so an open stops there, through
 * signals.csv and through frames.csv alike */
TEST(RecordingTest, ALongEventsLogIsAskedThroughAndStopsTheOpen)
{
    Folder f;
    std::string events = events_v2({"RPM,number,rpm,0,1"}, nullptr);
    const std::string note =
        "12:00:01.000  +1.000  note  " + std::string(100, 'x') + "\n";
    while (events.size() < (9u << 20)) {
        events += note;
    }
    f.write("events.log", events);
    const std::string values =
        std::string(kBom) + rows::kSignalsHeader + "\n10,RPM,800,rpm\n";
    f.write("signals.csv", values);
    const std::string frames = "host_us,timestamp_us,id,flags,data\n";
    f.write("frames.csv", frames);
    struct Seen {
        std::vector<std::pair<uint64_t, uint64_t>> asks;
        bool stop = false;
    };
    auto progress = [](uint64_t read, uint64_t total, void* user) {
        auto* s = static_cast<Seen*>(user);
        s->asks.emplace_back(read, total);
        return s->stop ? 1 : 0;
    };
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    const auto open = [&](bool through_frames, Seen* s) {
        ecuio_recording_t* r = nullptr;
        const ecuio_status_t rc =
            through_frames
                ? ecuio_recording_open_frames(f.utf8().c_str(), mock, progress,
                                              s, &r)
                : ecuio_recording_open(f.utf8().c_str(), progress, s, &r);
        ecuio_recording_close(r);
        return rc;
    };
    for (bool through_frames : {false, true}) {
        SCOPED_TRACE(through_frames ? "frames.csv" : "signals.csv");
        const uint64_t data = through_frames ? frames.size() : values.size();
        Seen whole;
        EXPECT_NE(open(through_frames, &whole), ECUIO_ERR_CANCELED);
        ASSERT_GE(whole.asks.size(), 3u);
        EXPECT_EQ(whole.asks[0], std::make_pair(uint64_t{0}, data));
        EXPECT_EQ(whole.asks[1], std::make_pair(uint64_t{0}, data));
        EXPECT_EQ(whole.asks[2], std::make_pair(uint64_t{0}, data))
            << "the data file's first ask";

        Seen stopped;
        stopped.stop = true;
        EXPECT_EQ(open(through_frames, &stopped), ECUIO_ERR_CANCELED);
        EXPECT_EQ(stopped.asks.size(), 1u) << "the first ask stops it";
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
