// File/layer: tests/test_recorder_attach.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderTest (test_core)

#include "test_recorder_support.h"

#include "open_handles.h"

/* Out of memory at each of an attach's allocations in turn, on a
 * session of six modules: the attach answers OK or OUT_OF_MEMORY and
 * throws nothing, and a refused recorder attaches at the next try with
 * the header a recorder never refused writes */
TEST(RecorderTest, OutOfMemoryAnywhereInAnAttachIsAnsweredNotThrown)
{
    Root root;
    const std::string keep = root.utf8();
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(
                  "mock://x?profile=uds-generic&did=7E8:D100:u8,7E9:D101:u8,"
                  "7EA:D102:u8,7EB:D103:u8,7EC:D104:u8,7ED:D105:u8",
                  &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    /* Caller's lines long enough together that joining the header to
     * them under the attach's lock has to allocate */
    const std::string long_line(100, 'x');
    const char* caller[16];
    for (const char*& line : caller) {
        line = long_line.c_str();
    }
    ecuio_record_options_t o{};
    o.root_utf8 = keep.c_str();
    o.header_lines = caller;
    o.header_line_count = 16;
    const auto opened = [&] {
        ecuio_recorder_t* rec = nullptr;
        EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
            << ecuio_last_error_detail();
        return rec;
    };
    ecuio_recorder_t* rec = opened();
    ASSERT_NE(rec, nullptr);
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK)
        << ecuio_last_error_detail();
    const unsigned long news = news_on_this_thread() - before;
    EXPECT_EQ(ecuio_recorder_close(rec, "counted"), ECUIO_OK);
    ASSERT_GT(news, 3u);
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        rec = opened();
        ASSERT_NE(rec, nullptr);
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_session_record(s, rec);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "the attach threw at allocation " << n;
        if (rc != ECUIO_OK) {
            ++refused;
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(ecuio_session_record(s, rec), ECUIO_OK)
                << "at " << n << ": " << ecuio_last_error_detail();
        }
        char folder[1024] = {};
        uint32_t len = 0;
        ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_recorder_close(rec, "the test's turn"), ECUIO_OK);
        /* The header is written once, whatever was refused before it */
        size_t profiles = 0;
        size_t columns = 0;
        size_t signals = 0;
        size_t callers = 0;
        for (const std::string& line :
             lines_of(read_text(fs::u8path(folder) / "events.log"))) {
            callers += line == "caller  " + long_line;
            profiles += line.rfind("profile  ", 0) == 0;
            columns += line.rfind("columns  ", 0) == 0;
            signals += line.rfind("signal  ", 0) == 0;
        }
        EXPECT_EQ(profiles, 1u) << "at " << n;
        EXPECT_EQ(columns, 1u) << "at " << n;
        EXPECT_EQ(signals, 6u) << "at " << n;
        EXPECT_EQ(callers, 16u) << "at " << n;
    }
    EXPECT_GT(refused, 0u);
    ecuio_session_destroy(s);
}

/* Out of memory at each of an open's allocations in turn, a plain and an
 * armed recorder, each try under a root of its own: the open answers OK
 * with a whole recorder or OUT_OF_MEMORY with none, throws nothing, and
 * leaves no file open */
TEST(RecorderTest, OutOfMemoryAnywhereInAnOpenIsAnsweredNotThrown)
{
    Root root;
    const std::string base = root.utf8();
    const char* caller[2] = {"one line", "another"};
    for (const uint32_t flags : {0u, uint32_t{ECUIO_RECORD_ARMED}}) {
        ecuio_record_options_t o{};
        o.flags = flags;
        o.label_utf8 = "a label";
        o.header_lines = caller;
        o.header_line_count = 2;
        const auto root_of = [&](unsigned long n) {
            return base + "/" + std::to_string(flags) + "-" + std::to_string(n);
        };
        ecuio_recorder_t* rec = nullptr;
        const unsigned long handles = open_handles();
        std::string dir = root_of(0);
        o.root_utf8 = dir.c_str();
        const unsigned long before = news_on_this_thread();
        ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
            << ecuio_last_error_detail();
        const unsigned long news = news_on_this_thread() - before;
        EXPECT_EQ(ecuio_recorder_close(rec, "counted"), ECUIO_OK);
        ASSERT_GT(news, 5u);
        unsigned long refused = 0;
        for (unsigned long n = 1; n <= news; ++n) {
            rec = nullptr;
            dir = root_of(n);
            o.root_utf8 = dir.c_str();
            ecuio_status_t rc = ECUIO_OK;
            bool threw = false;
            fail_new_on_this_thread(n);
            try {
                rc = ecuio_recorder_open(&o, &rec);
            } catch (...) {
                threw = true;
            }
            fail_new_on_this_thread(0);
            ASSERT_FALSE(threw) << "the open threw at allocation " << n;
            if (rc != ECUIO_OK) {
                ++refused;
                EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
                EXPECT_EQ(rec, nullptr) << "at " << n;
                /* The root is the caller's; nothing the open made in it
                 * stays, so no list shows a recording that won't open */
                std::error_code ec;
                const fs::path made = fs::u8path(dir);
                EXPECT_TRUE(!fs::exists(made, ec) || fs::is_empty(made, ec))
                    << "a refused open left its folder at " << n;
                continue;
            }
            ASSERT_NE(rec, nullptr);
            if (flags == 0) {
                char folder[1024] = {};
                uint32_t len = 0;
                ASSERT_EQ(
                    ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
                    ECUIO_OK);
                for (const char* file :
                     {"frames.csv", "signals.csv", "events.log"}) {
                    EXPECT_TRUE(fs::exists(fs::u8path(folder) / file))
                        << file << " at " << n;
                }
            }
            EXPECT_EQ(ecuio_recorder_close(rec, "the test's turn"), ECUIO_OK);
        }
        EXPECT_GT(refused, 0u);
        EXPECT_EQ(open_handles(), handles) << "every file closed";
    }
}

/* Recorders attach and close one after another while the session
 * streams, each attach and close answering OK: the lifetime under the
 * worker's pins, for a sanitizer to judge */
TEST(RecorderTest, RecordersComeAndGoWhileTheSessionStreams)
{
    Root root;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://churn", &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    for (int i = 0; i < 12; ++i) {
        ecuio_recorder_t* rec = nullptr;
        ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK) << i;
        ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK)
            << i << ": " << ecuio_last_error_detail();
        if (i % 3 == 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(ecuio_recorder_close(rec, "churn"), ECUIO_OK) << i;
    }
    ecuio_session_destroy(s);
}

/* A recorder's marks and notes count from its own zero, not from a later
 * recorder's, whose attach moves the link's */
TEST(RecorderTest, AMarkCountsFromItsOwnRecordersZeroAfterASuccessorAttaches)
{
    Root root;
    Recording first(root, "mock://zero");
    ASSERT_NE(first.recorder, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ecuio_record_detach(*first.session->record_link);

    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* second = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &second), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(first.session, second), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_recorder_mark(first.recorder, "after the successor"),
              ECUIO_OK);
    ASSERT_EQ(ecuio_recorder_note(first.recorder, "a note after it"), ECUIO_OK);
    ecuio_recorder_test_add_slept(first.recorder, 2000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ecuio_session_destroy(first.session);
    first.session = nullptr;
    EXPECT_EQ(ecuio_recorder_close(second, "the second"), ECUIO_OK);
    const std::string folder = first.folder;
    EXPECT_EQ(ecuio_recorder_close(first.recorder, "the first"), ECUIO_OK);
    first.recorder = nullptr;

    const std::vector<std::string> marks =
        lines_of(read_text(fs::u8path(folder) / "markers.csv"));
    ASSERT_EQ(marks.size(), 2u);
    EXPECT_GE(std::stoull(marks[1]), 350000u) << marks[1];
    const std::vector<EventLine> notes =
        events_of(read_text(fs::u8path(folder) / "events.log"), "note");
    bool found = false;
    for (const EventLine& n : notes) {
        if (n.text.find("a note after it") != std::string::npos) {
            found = true;
            EXPECT_GE(n.ms, 350u) << n.text;
        }
    }
    EXPECT_TRUE(found);
    const std::vector<EventLine> sleeps =
        events_of(read_text(fs::u8path(folder) / "events.log"), "asleep");
    ASSERT_EQ(sleeps.size(), 1u);
    EXPECT_GE(sleeps[0].ms, 350u) << "the asleep line, from the first's zero";
}

/* A recorder's close lets go of the link only while the link is its
 * own: one detached earlier (the session's side) and closed after a
 * successor attached leaves the successor taking frames */
TEST(RecorderTest, AClosedRecorderNeverDetachesItsSuccessor)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://successor?profile=rec-test");
    ASSERT_NE(rec.recorder, nullptr);
    ecuio_record_link& link = *rec.session->record_link;
    ecuio_record_detach(link);
    EXPECT_EQ(link.active.load(), nullptr);

    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* second = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &second), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(rec.session, second), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_close(rec.recorder, "the first"), ECUIO_OK);
    rec.recorder = nullptr;
    EXPECT_EQ(link.active.load(), second) << "the first's close left it";

    dispatch(rec.session, kNumberId, std::string("\x02", 1));
    ecuio_recorder_state_t st{};
    EXPECT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(second, &st);
            return st.frame_rows == 1;
        },
        2000))
        << st.frame_rows << " frame rows";
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(second, buf, sizeof buf, &len),
              ECUIO_OK);
    ecuio_session_destroy(rec.session);
    rec.session = nullptr;
    EXPECT_EQ(ecuio_recorder_close(second, "the second"), ECUIO_OK);
    EXPECT_EQ(lines_of(read_text(fs::u8path(buf) / "frames.csv")).size(), 2u);
    EXPECT_NE(rec.file("events.log").find("  end  the first: 0 frames, "),
              std::string::npos)
        << rec.file("events.log");
}

/* A recording begun mid-session opens in the reader as any other: its
 * times run from the attach (the duration fits the test's clock reads
 * around it), its signals carry samples */
TEST(RecorderTest, ARecordingBegunMidSessionOpensInTheReader)
{
    Root root;
    Recording rec(root, "mock://mid", 0, nullptr, {}, 0, false);
    ASSERT_NE(rec.recorder, nullptr);
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const uint64_t before = ecuio_clock_us();
    rec.attach();
    ecuio_recorder_state_t st{};
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.value_rows >= 2;
        },
        5000))
        << st.value_rows << " value rows";
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const uint64_t after = ecuio_clock_us();

    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(rec.folder.c_str(), nullptr, nullptr, &r),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_info_t info{};
    ASSERT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_EQ(info.flags, 0u) << "the summary is there";
    /* events.log's times are rounded to the nearest millisecond, so the
     * duration reads up to 500 us past the span the clock saw */
    EXPECT_LE(info.duration_us, after - before + 500)
        << "the recording's clock starts at the attach";
    ASSERT_GE(info.signal_count, 1u);
    std::vector<uint32_t> ids(info.signal_count);
    std::vector<ecuio_recording_column_t> cols(info.signal_count);
    for (uint32_t k = 0; k < info.signal_count; ++k)
        ids[k] = k;
    ASSERT_EQ(ecuio_recording_query(r, ids.data(), info.signal_count, 0,
                                    info.duration_us + 1, cols.data(), 1),
              ECUIO_OK);
    uint64_t samples = 0;
    for (const ecuio_recording_column_t& c : cols)
        samples += c.samples;
    EXPECT_GE(samples, 2u);
    ecuio_recording_close(r);
}

/* A replay's recording writes its source's frame flags line, so a
 * recording core made stays complete through any number of replays;
 * anything else is an older recording's (ADR-0049 section 2) */
TEST(RecorderTest, AReplayPassesOnItsSourcesFrameFlagsLine)
{
    register_profile();
    Root root;
    std::string source;
    {
        Recording rec(root, "mock://source?profile=rec-test");
        ASSERT_NE(rec.recorder, nullptr);
        dispatch(rec.session, kNumberId, std::string("\x02", 1),
                 ECUIO_FRAME_FLAG_REASSEMBLED);
        dispatch(rec.session, kNumberId, std::string("\x04", 1));
        source = rec.folder;
        ASSERT_EQ(rec.finish(), ECUIO_OK);
    }
    const auto replayed = [&](const std::string& path) {
        const std::string uri = "replay://" + path + "?speed=max";
        Recording rec(root, uri.c_str());
        EXPECT_NE(rec.recorder, nullptr);
        if (rec.recorder == nullptr)
            return std::string();
        ecuio_session_start(rec.session);
        ecuio_recorder_state_t st{};
        EXPECT_TRUE(eventually(
            [&] {
                ecuio_recorder_get_state(rec.recorder, &st);
                return st.frame_rows == 2;
            },
            3000));
        const std::string folder = rec.folder;
        EXPECT_EQ(rec.finish(), ECUIO_OK);
        return folder;
    };
    const auto flags_line = [](const std::string& folder) {
        for (const std::string& line :
             lines_of(read_text(fs::u8path(folder) / "events.log"))) {
            if (line.rfind("frame flags  ", 0) == 0)
                return line;
        }
        return std::string();
    };

    const std::string first = replayed(source);
    EXPECT_EQ(flags_line(first), "frame flags  complete");
    const std::vector<std::string> rows =
        lines_of(read_text(fs::u8path(first) / "frames.csv"));
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NE(rows[1].find(",0x100,0x100,02"), std::string::npos) << rows[1];
    EXPECT_NE(rows[2].find(",0x100,0x0,04"), std::string::npos) << rows[2];
    EXPECT_EQ(flags_line(replayed(first + "/frames.csv")),
              "frame flags  complete")
        << "passed on again";

    /* A word core doesn't write, an older header, a .csv of another name */
    const fs::path odd = root.path / "odd";
    fs::create_directories(odd);
    fs::copy_file(fs::u8path(source) / "frames.csv", odd / "frames.csv");
    std::ofstream(odd / "events.log", std::ios::binary)
        << "ecuio session recording, format 2 (ADR-0049)\n"
           "frame flags  everything\n\n";
    EXPECT_EQ(flags_line(replayed(odd.generic_u8string())),
              "frame flags  from an older recording");
    std::ofstream(odd / "events.log", std::ios::binary)
        << "Havne session recording\nframe flags  complete\n\n";
    EXPECT_EQ(flags_line(replayed(odd.generic_u8string())),
              "frame flags  from an older recording");
    fs::copy_file(fs::u8path(source) / "frames.csv", odd / "other.csv");
    fs::copy_file(fs::u8path(source) / "events.log", odd / "events.log",
                  fs::copy_options::overwrite_existing);
    EXPECT_EQ(flags_line(replayed((odd / "other.csv").generic_u8string())),
              "frame flags  from an older recording");
    EXPECT_EQ(flags_line(replayed(odd.generic_u8string())),
              "frame flags  complete")
        << "the same events.log beside its own frames.csv";
}

/* The writer writes nothing before a session attaches, however long the
 * caller takes: an attach past its wakes still puts the header first,
 * and a recorder closed unattached leaves its files empty */
TEST(RecorderTest, NothingIsWrittenBeforeASessionAttaches)
{
    register_profile();
    Root root;
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    const char* caller[] = {"app  Havne test"};
    o.header_lines = caller;
    o.header_line_count = 1;
    for (const bool attach : {false, true}) {
        SCOPED_TRACE(attach ? "attached late" : "never attached");
        ecuio_recorder_t* rec = nullptr;
        ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
        char buf[1024];
        uint32_t len = 0;
        ecuio_recorder_get_folder(rec, buf, sizeof buf, &len);
        const fs::path folder = fs::u8path(buf);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ecuio_session_t* s = nullptr;
        if (attach) {
            ASSERT_EQ(ecuio_session_create("mock://late?profile=rec-test", &s),
                      ECUIO_OK);
            ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK);
            dispatch(s, kNumberId, std::string("\x02", 1));
            ecuio_session_destroy(s);
        }
        EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
        const std::string events = read_text(folder / "events.log");
        if (!attach) {
            for (const char* f : {"frames.csv", "signals.csv", "events.log"}) {
                EXPECT_EQ(fs::file_size(folder / f), 0u) << f;
            }
            continue;
        }
        EXPECT_EQ(events.rfind("ecuio session recording, format 4", 0), 0u)
            << events;
        EXPECT_NE(events.find("\ncaller  app  Havne test\n"), std::string::npos)
            << events;
        const std::vector<std::string> frames =
            lines_of(read_text(folder / "frames.csv"));
        ASSERT_EQ(frames.size(), 2u);
        EXPECT_EQ(frames[0], "host_us,timestamp_us,id,flags,data");
    }
}

/* A sleep of the machine while recording is an asleep line, stamped as
 * the writer woke, with its length; a rise under a second, or a sleep
 * before the session attached, has none */
TEST(RecorderTest, ASleepIsAnAsleepLineWithItsLength)
{
    Root root;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://sleep", &s), ECUIO_OK);
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char buf[1024];
    uint32_t len = 0;
    ecuio_recorder_get_folder(rec, buf, sizeof buf, &len);
    const fs::path folder = fs::u8path(buf);
    ecuio_recorder_test_add_slept(rec, 5000000); /* before attach */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    /* The recording's zero is read inside the attach */
    const uint64_t attach_from = ecuio_clock_us();
    ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK);
    const uint64_t attach_to = ecuio_clock_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    /* The sleep goes in just after a wake has taken its look, so the next
     * look is most of a wake away: a writer that stamped a wake late
     * couldn't pass. The writer counts a wake before it looks, so the
     * test lets 5 ms more go by, spinning: a sleep_for's own lateness
     * (15.6 ms on Windows) would eat into the wake */
    const uint64_t woke = ecuio_recorder_test_wakes(rec);
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ecuio_recorder_test_wakes(rec) == woke &&
           std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    const auto looked =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    while (std::chrono::steady_clock::now() < looked) {
        std::this_thread::yield();
    }
    const uint64_t added_from = ecuio_clock_us();
    ecuio_recorder_test_add_slept(rec, 2340000);
    const uint64_t added_to = ecuio_clock_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ecuio_recorder_test_add_slept(rec, 600000); /* under a second */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);

    const std::string events = read_text(folder / "events.log");
    EXPECT_EQ(events.rfind("ecuio session recording, format 4", 0), 0u)
        << "the header first, though attach came past the writer's wake";
    const std::vector<EventLine> sleeps = events_of(events, "asleep");
    ASSERT_EQ(sleeps.size(), 1u) << events;
    EXPECT_EQ(sleeps[0].text, "Asleep for 2.3 s: nothing was read meanwhile");
    /* Stamped at the wake that saw it, on the test's own clock: no
     * earlier than the sleep went in, no later than one idle wake after,
     * and a late wake's margin, kept under a wake so that a stamp a wake
     * late fails. macOS's runner fires timers 100-200 ms late (a stamp
     * 22 ms past this bound there): its margin is wider, and the "a wake
     * late fails" check is Windows' and Linux's. Both from the
     * recording's zero, somewhere in the attach; the stamp is whole ms */
    constexpr uint64_t kWakeUs = 100000;  /* the writer's idle wake */
#ifdef __APPLE__
    constexpr uint64_t kMarginUs = 400000; /* macOS's late timers */
#else
    constexpr uint64_t kMarginUs = 90000; /* a timer firing late */
#endif
    const uint64_t earliest_ms = (added_from - attach_to) / 1000;
    const uint64_t latest_ms =
        (added_to + kWakeUs + kMarginUs - attach_from) / 1000 + 1;
    ::testing::Test::RecordProperty("asleep_ms", std::to_string(sleeps[0].ms));
    ::testing::Test::RecordProperty("asleep_window_ms",
                                    std::to_string(earliest_ms) + "-" +
                                        std::to_string(latest_ms));
    EXPECT_GE(sleeps[0].ms, earliest_ms) << events;
    EXPECT_LE(sleeps[0].ms, latest_ms) << events;
}

/* The writer wakes every 100 ms while idle, so an idle recording costs a
 * phone's background session ten wakes a second, and every 10 ms while
 * the ring has records (ADR-0049 section 1) */
TEST(RecorderTest, AnIdleWriterWakesTenTimesASecond)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://wakes?profile=rec-test");
    ASSERT_NE(rec.recorder, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t idle_from = ecuio_recorder_test_wakes(rec.recorder);
    /* Two seconds: a writer that waits a second wakes 3 times at most in
     * them, and the macOS runner, whose timers fire late, counted 4 a
     * second */
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const uint64_t idle_2s =
        ecuio_recorder_test_wakes(rec.recorder) - idle_from;
    ::testing::Test::RecordProperty("idle_wakes_2s", std::to_string(idle_2s));
    EXPECT_GE(idle_2s, 5u);
    EXPECT_LE(idle_2s, 40u) << "an idle writer wakes every 100 ms";
    const uint64_t idle = idle_2s / 2;

    /* One frame waiting at every wake: the next goes in as soon as the
     * writer has written the last, so no wake finds the ring empty and
     * none spends its time on a full one */
    uint64_t sent = 0;
    const auto busy_second = [&] {
        const uint64_t from = ecuio_recorder_test_wakes(rec.recorder);
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end) {
            ecuio_recorder_state_t st{};
            ecuio_recorder_get_state(rec.recorder, &st);
            if (st.frame_rows == sent) {
                dispatch(rec.session, kNumberId, std::string("\x02", 1));
                ++sent;
            }
            std::this_thread::yield();
        }
        return ecuio_recorder_test_wakes(rec.recorder) - from;
    };
    /* Against the idle second just measured, not a count: a desktop
     * wakes about 57 times (Windows' 15.6 ms timer), the macOS runner 18
     * (its timers fire late), and a busy wake no shorter than the idle
     * one wakes as often as idle. Up to three seconds, the best kept: a
     * runner busy with other tests has left the writer no turn for a
     * whole second (0 wakes on CI's Windows), while a writer waking at
     * the idle pace falls short in every one */
    uint64_t busy = 0;
    for (int attempt = 0; attempt < 3 && busy < idle + 5; ++attempt) {
        busy = std::max(busy, busy_second());
    }
    ::testing::Test::RecordProperty("busy_wakes", std::to_string(busy));
    EXPECT_GE(busy, idle + 5) << "a busy writer wakes every 10 ms";
}
