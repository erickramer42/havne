// File/layer: tests/test_recorder_replay.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderTest (test_core)

#include "test_recorder_support.h"

/* The replay fixture recorded: every frame as the recording has it
 * (ID, flags, bytes), and every value obd2 decodes */
TEST(RecorderTest, ARecordedReplayHoldsEveryFrameOfItsSource)
{
    const std::string source =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    const std::vector<std::string> want =
        lines_of(read_text(fs::u8path(source) / "frames.csv"));
    ASSERT_GT(want.size(), 100u);
    Root root;
    const std::string uri = "replay://" + source + "?speed=max&profile=obd2";
    Recording rec(root, uri.c_str());
    ASSERT_NE(rec.recorder, nullptr);
    std::atomic<size_t> seen{0};
    auto count = [](const ecuio_frame_t*, void* user) {
        static_cast<std::atomic<size_t>*>(user)->fetch_add(1);
    };
    ASSERT_EQ(ecuio_register_frame_callback(rec.session, count, &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    ASSERT_TRUE(
        eventually([&] { return seen.load() >= want.size() - 1; }, 20000))
        << seen.load();
    ecuio_recorder_state_t st{};
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::vector<std::string> got = lines_of(rec.file("frames.csv"));
    ASSERT_EQ(got.size(), want.size());
    EXPECT_EQ(got[0], want[0]);
    /* id, flags and data: everything after the second comma */
    auto tail = [](const std::string& row) {
        const size_t a = row.find(',');
        return row.substr(row.find(',', a + 1));
    };
    for (size_t i = 1; i < got.size(); ++i) {
        ASSERT_EQ(tail(got[i]), tail(want[i])) << "row " << i;
    }
    EXPECT_GT(lines_of(rec.file("signals.csv")).size(), 100u);
    const std::string events = rec.file("events.log");
    EXPECT_NE(events.find("\nframe flags  from an older recording\n"),
              std::string::npos);
    (void)st;
}

namespace {

/* A ring the 1-byte frames of the case's number fill to its last byte:
 * each record is its head (16), the frame's (32), the byte and the value
 * (16), so a link record (32) can't fit either */
constexpr uint32_t kFrameRecord = 65;
constexpr uint32_t kExactRing = 7 * kFrameRecord;

/* A request's reply timing as a worker publishes it */
ecuio_request_timing_t published_row(uint8_t pid, uint32_t answered)
{
    ecuio_request_timing_t r{};
    r.length = 2;
    r.request[0] = 0x01;
    r.request[1] = pid;
    r.answered = answered;
    return r;
}

/* A latency.csv row split at its t_s: the ms it names, and the rest of
 * the row from its first comma */
struct TimedRow {
    uint64_t ms;
    std::string rest;
};

TimedRow timed_row(const std::string& line)
{
    const size_t comma = line.find(',');
    const std::string secs = line.substr(0, comma);
    const size_t dot = secs.find('.');
    EXPECT_NE(dot, std::string::npos) << line;
    if (comma == std::string::npos || dot == std::string::npos)
        return TimedRow{0, line};
    return TimedRow{std::stoull(secs.substr(0, dot)) * 1000 +
                        std::stoull(secs.substr(dot + 1)),
                    line.substr(comma)};
}

/* A row's t_s is a host_us rounded to the ms, taken at a call made
 * between from and to, from a zero set between before and attached (the
 * test's own ecuio_clock_us reads, never a fixed bound) */
void expect_between(const TimedRow& row, uint64_t before, uint64_t attached,
                    uint64_t from, uint64_t to)
{
    const uint64_t us = row.ms * 1000;
    EXPECT_LE(us, to - before + 500) << row.ms << " ms" << row.rest;
    if (from > attached + 500) {
        EXPECT_GE(us, from - attached - 500) << row.ms << " ms" << row.rest;
    }
}

} // namespace

/* Each link transition a worker publishes is a link line at its own
 * time, among the frames in order: its words, the state and the episode
 * count (ADR-0049 section 2) */
TEST(RecorderTest, ALinkTransitionIsALinkLineAtItsTime)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://link?profile=rec-test");
    ASSERT_NE(rec.session, nullptr);
    /* The test is the worker: the session never starts */
    link_health h(rec.session->last_error, rec.session->error_count,
                  rec.session->record_link.get());
    dispatch(rec.session, kNumberId, std::string("\x02", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_count_quiet(h);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    dispatch(rec.session, kNumberId, std::string("\x04", 1));
    link_on_read(h, true, false, false);
    link_on_read(h, true, false, false); /* no transition: no line */
    link_on_read(h, false, true, false);
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    const std::vector<EventLine> links = events_of(events, "link");
    ASSERT_EQ(links.size(), 3u) << events;
    EXPECT_EQ(links[0].text, "vehicle not responding (state 8, 0 episodes)");
    EXPECT_EQ(links[1].text, "adapter fault (state 9, 1 episode)");
    EXPECT_EQ(links[2].text, "recovered (state 0, 1 episode)");
    /* The timeout's line sits between the two frames' times */
    const std::vector<std::string> frames = lines_of(rec.file("frames.csv"));
    ASSERT_EQ(frames.size(), 3u);
    const uint64_t first_ms = std::stoull(frames[1]) / 1000;
    const uint64_t second_ms = std::stoull(frames[2]) / 1000;
    EXPECT_GT(links[0].ms, first_ms + 10) << events;
    EXPECT_LT(links[0].ms + 10, second_ms) << events;
}

/* A replay whose file changed after attach stops, and its recording says
 * why in a link line: replay publishes the state itself, not through
 * link_health */
TEST(RecorderTest, AReplayThatStopsEarlySaysSoInALinkLine)
{
    Root root;
    fs::create_directories(root.path);
    const fs::path csv = root.path / "source.csv";
    const auto write = [&](int rows, const char* tail) {
        std::string text = "host_us,timestamp_us,id,flags,data\n";
        for (int i = 0; i < rows; ++i) {
            const std::string t = std::to_string(5000000 + 100000 * i);
            text += t + "," + t + ",0x7E8,0x0,41 0C 1F 40\n";
        }
        text += tail;
        std::ofstream(csv, std::ios::binary) << text;
    };
    write(5, "");
    const std::string uri = "replay://" + csv.generic_u8string() + "?speed=max";
    Recording rec(root, uri.c_str());
    ASSERT_NE(rec.recorder, nullptr);
    write(3, "5300000,5300000,0x7E8,0x0,41 0"); /* cut mid-row */
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_session_get_error_state(rec.session, &last, &episodes);
            return last != ECUIO_OK;
        },
        3000));
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::vector<EventLine> links =
        events_of(rec.file("events.log"), "link");
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].text, "replay stopped: its recording changed after it "
                             "was opened (state 16, 1 episode)");
}

/* hold_writer waits out a loop under way: a loop that read no hold, then
 * spent its time on the notes queued ahead of the ring, takes none of
 * the frames the case puts in once hold_writer returns. The notes keep
 * that loop going past the hold longer than a fixed wait covers, as a
 * loaded machine's scheduler can */
TEST(RecorderTest, HoldWriterWaitsOutALoopUnderWay)
{
    constexpr uint32_t kNotes = 200000;
    register_profile();
    Root root;
    Recording rec(root, "mock://parked?profile=rec-test", kExactRing, nullptr,
                  {}, kNotes);
    ASSERT_NE(rec.recorder, nullptr);
    ecuio_recorder_test_hold(rec.recorder, 1);
    for (uint32_t i = 0; i < kNotes; ++i) {
        ASSERT_EQ(ecuio_recorder_note(rec.recorder, "a queued note"), ECUIO_OK);
    }
    /* The next wake's loop reads no hold and takes the notes */
    const uint64_t woke = ecuio_recorder_test_wakes(rec.recorder);
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_TRUE(eventually(
        [&] { return ecuio_recorder_test_wakes(rec.recorder) > woke; }, 2000));
    ASSERT_NO_FATAL_FAILURE(hold_writer(rec.recorder));
    for (int i = 0; i < 7; ++i) {
        dispatch(rec.session, kNumberId, std::string(1, static_cast<char>(i)));
    }
    /* Two more wakes: any loop under way has ended and drained what it
     * would */
    const uint64_t held = ecuio_recorder_test_wakes(rec.recorder);
    ASSERT_TRUE(eventually(
        [&] { return ecuio_recorder_test_wakes(rec.recorder) >= held + 2; },
        5000));
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    EXPECT_EQ(st.frame_rows, 0u) << "the held writer wrote frames";
    EXPECT_EQ(st.records_dropped, 0u);
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows == 7;
        },
        2000));
    ASSERT_EQ(rec.finish(), ECUIO_OK);
}

/* A transition that finds the ring full is carried: a later one takes
 * its place, and the latest goes in ahead of the next record that fits,
 * so a drop loses frames but never the link's state */
TEST(RecorderTest, ALinkTransitionThatFindsTheRingFullIsCarried)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://carry?profile=rec-test", kExactRing);
    ASSERT_NE(rec.recorder, nullptr);
    link_health h(rec.session->last_error, rec.session->error_count,
                  rec.session->record_link.get());
    ASSERT_NO_FATAL_FAILURE(hold_writer(rec.recorder));
    for (int i = 0; i < 100; ++i) {
        dispatch(rec.session, kNumberId, std::string(1, static_cast<char>(i)));
    }
    link_on_write(h, false);             /* requests failing, episode 1 */
    link_on_read(h, true, false, false); /* an adapter fault, episode 2 */
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    const uint64_t dropped = st.records_dropped;
    ASSERT_EQ(dropped, 93u) << "the ring full to its last byte";
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows == 100 - dropped;
        },
        2000));
    dispatch(rec.session, kNumberId, std::string("\x01", 1));
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    const std::vector<EventLine> links = events_of(events, "link");
    ASSERT_EQ(links.size(), 1u) << events;
    EXPECT_EQ(links[0].text, "adapter fault (state 9, 2 episodes)");
    const size_t link_at = events.find("  link  ");
    const size_t drop_at = events.find("  drop  the recording dropped " +
                                       std::to_string(dropped) + " frames");
    ASSERT_NE(drop_at, std::string::npos) << events;
    EXPECT_LT(link_at, drop_at) << "the carried state goes in first";
    EXPECT_EQ(lines_of(rec.file("frames.csv")).size(), 1 + 101 - dropped);
}

/* A state still carried when the worker lets go, and drops no record
 * came after, are written at the close, ahead of the summary */
TEST(RecorderTest, AStateStillCarriedAtTheEndIsWritten)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://carried?profile=rec-test", kExactRing);
    ASSERT_NE(rec.recorder, nullptr);
    link_health h(rec.session->last_error, rec.session->error_count,
                  rec.session->record_link.get());
    ASSERT_NO_FATAL_FAILURE(hold_writer(rec.recorder));
    for (int i = 0; i < 100; ++i) {
        dispatch(rec.session, kNumberId, std::string(1, static_cast<char>(i)));
    }
    link_on_write(h, false);
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    const uint64_t dropped = st.records_dropped;
    ASSERT_EQ(dropped, 93u) << "the ring full to its last byte";
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    const std::vector<EventLine> links = events_of(events, "link");
    ASSERT_EQ(links.size(), 1u) << events;
    EXPECT_EQ(links[0].text, "requests failing (state 10, 1 episode)");
    const size_t drop_at = events.find("  drop  the recording dropped " +
                                       std::to_string(dropped) + " frames");
    ASSERT_NE(drop_at, std::string::npos) << events;
    EXPECT_LT(drop_at, events.find("  end  ")) << events;
}

/* Reply timing reaches latency.csv every 10 s of session time and at the
 * end, each request's row only when its counts changed (ADR-0017's
 * appendix of 2026-10-31), under its channel's ECU (the mock's is
 * 0x7E8); the file is made with its first row. Each row's t_s is the
 * recording's time of the call that published it, not the snapshot's
 * session time (a recorder attached after start counts from its attach),
 * so it's bounded by the test's clock reads */
TEST(RecorderTest, ReplyTimingGoesToLatencyCsvEveryTenSeconds)
{
    Root root;
    const uint64_t before = ecuio_clock_us();
    Recording rec(root, "mock://timing");
    const uint64_t attached = ecuio_clock_us();
    ASSERT_NE(rec.recorder, nullptr);
    ecuio_record_link* link = rec.session->record_link.get();
    ecuio_session_timing_t t{};
    t.interval_clock = ECUIO_TIMING_CLOCK_HOST;
    ecuio_request_timing_t rows[2] = {published_row(0x0C, 5),
                                      published_row(0x0D, 0)};
    rows[0].timeouts = 1;
    rows[0].flags = ECUIO_TIMING_LATENCY_P99;
    rows[0].latency_p50_us = 12000;
    rows[0].latency_p99_us = 14000;
    rows[0].latency_max_us = 15000;
    rows[0].intervals = 4;
    rows[0].interval_p50_us = 100000;
    rows[0].interval_max_us = 120000;

    /* Each call bracketed by the clock, for its rows' t_s */
    uint64_t from[5], to[5];
    const auto publish = [&](int k, bool final) {
        from[k] = ecuio_clock_us();
        ecuio_record_timing(link, t, rows, 2, final);
        to[k] = ecuio_clock_us();
    };
    t.snapshot_us = 5000000; /* before the first 10 s: nothing */
    publish(0, false);
    /* Past the writer's idle wake, so a file made would be there */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_FALSE(fs::exists(fs::u8path(rec.folder) / "latency.csv"));
    t.snapshot_us = 10000000;
    publish(1, false);
    rows[0].answered = 6;
    t.snapshot_us = 12000000; /* not due */
    publish(2, false);
    rows[1].refused = 1;
    t.snapshot_us = 20400000;
    publish(3, false);
    rows[1].refused = 2;
    t.snapshot_us = 21000000; /* the final one, whenever it comes */
    publish(4, true);
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::vector<std::string> got = lines_of(rec.file("latency.csv"));
    ASSERT_EQ(got.size(), 5u) << rec.file("latency.csv");
    EXPECT_EQ(got[0], ECUIO_TIMING_COLUMNS);
    struct Want {
        int call;
        const char* rest;
    };
    const Want want[4] = {
        {1, ",0x7E8,01 0C,5,0,1,0,12000,14000,,15000,4,0,100000,,,120000,host"},
        {3, ",0x7E8,01 0C,6,0,1,0,12000,14000,,15000,4,0,100000,,,120000,host"},
        {3, ",0x7E8,01 0D,0,1,0,0,0,,,0,0,0,,,,,host"},
        {4, ",0x7E8,01 0D,0,2,0,0,0,,,0,0,0,,,,,host"},
    };
    uint64_t last_ms = 0;
    for (int i = 0; i < 4; ++i) {
        const TimedRow row = timed_row(got[i + 1]);
        EXPECT_EQ(row.rest, want[i].rest);
        expect_between(row, before, attached, from[want[i].call],
                       to[want[i].call]);
        EXPECT_GE(row.ms, last_ms) << got[i + 1];
        last_ms = row.ms;
    }
}

/* The final snapshot, the worker's last record, waits up to 1 s for the
 * writer to make room; with none, it's dropped and counted */
TEST(RecorderTest, TheFinalTimingWaitsASecondForRoom)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://final?profile=rec-test", 512, nullptr, {}, 0,
                  false);
    ASSERT_NE(rec.recorder, nullptr);
    const uint64_t attach_from = ecuio_clock_us();
    rec.attach();
    const uint64_t attach_to = ecuio_clock_us();
    ecuio_record_link* link = rec.session->record_link.get();
    ecuio_session_timing_t t{};
    t.snapshot_us = 3000000;
    const ecuio_request_timing_t row = published_row(0x0C, 3);
    const auto fill = [&] {
        for (int i = 0; i < 100; ++i) {
            dispatch(rec.session, kNumberId,
                     std::string(1, static_cast<char>(i)));
        }
    };

    ASSERT_NO_FATAL_FAILURE(hold_writer(rec.recorder));
    fill();
    ecuio_recorder_state_t st{};
    ecuio_recorder_get_state(rec.recorder, &st);
    const uint64_t frames_dropped = st.records_dropped;
    uint64_t released_at = 0;
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        released_at = ecuio_clock_us();
        ecuio_recorder_test_hold(rec.recorder, 0);
    });
    auto begun = std::chrono::steady_clock::now();
    const uint64_t called = ecuio_clock_us();
    ecuio_record_timing(link, t, &row, 1, true);
    const uint64_t returned = ecuio_clock_us();
    const auto waited = std::chrono::steady_clock::now() - begun;
    release.join();
    EXPECT_GE(waited, std::chrono::milliseconds(150));
    EXPECT_LT(waited, std::chrono::milliseconds(1000));
    ecuio_recorder_get_state(rec.recorder, &st);
    EXPECT_EQ(st.records_dropped, frames_dropped) << "the snapshot went in";

    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows == 100 - frames_dropped;
        },
        2000));
    ASSERT_NO_FATAL_FAILURE(hold_writer(rec.recorder));
    fill();
    ecuio_recorder_get_state(rec.recorder, &st);
    const uint64_t before = st.records_dropped;
    begun = std::chrono::steady_clock::now();
    ecuio_record_timing(link, t, &row, 1, true);
    EXPECT_GE(std::chrono::steady_clock::now() - begun,
              std::chrono::milliseconds(950));
    ecuio_recorder_get_state(rec.recorder, &st);
    EXPECT_EQ(st.records_dropped, before + 1) << "no room came";
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::vector<std::string> got = lines_of(rec.file("latency.csv"));
    ASSERT_EQ(got.size(), 2u);
    const TimedRow timed = timed_row(got[1]);
    EXPECT_EQ(timed.rest.rfind(",0x7E8,01 0C,3,", 0), 0u) << got[1];
    /* Its t_s is the call's time, kept through the wait for room: before
     * the release that made the room, not after it */
    expect_between(timed, attach_from, attach_to, called, returned);
    EXPECT_LT(timed.ms * 1000 + 500, released_at - attach_to) << got[1];
}

/* A recorder attached after start (ADR-0056 section 3) counts its
 * host_us from the attach, not the session's start: the first frame's
 * is small against the session's age (its timestamp_us, which the mock
 * counts from its start), every host_us fits between the test's clock
 * reads around the recording, and the header says what the link was */
TEST(RecorderTest, ARecorderAttachedAfterStartCountsFromItsAttach)
{
    Root root;
    Recording rec(root, "mock://running", 0, nullptr, {}, 0, false);
    ASSERT_NE(rec.recorder, nullptr);
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint64_t before = ecuio_clock_us();
    rec.attach();
    ecuio_recorder_state_t st{};
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows >= 2;
        },
        5000))
        << st.frame_rows << " frame rows";
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const uint64_t after = ecuio_clock_us();

    const std::vector<std::string> frames = lines_of(rec.file("frames.csv"));
    ASSERT_GE(frames.size(), 3u);
    uint64_t last = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        const uint64_t host_us = std::stoull(frames[i]);
        const uint64_t timestamp_us =
            std::stoull(frames[i].substr(frames[i].find(',') + 1));
        EXPECT_LE(host_us, after - before) << frames[i];
        EXPECT_GE(host_us, last) << frames[i];
        EXPECT_GE(timestamp_us, host_us + 400000)
            << frames[i] << ": the session was half a second old at the attach";
        last = host_us;
    }
    const std::string events = rec.file("events.log");
    EXPECT_NE(events.find("\nlink  healthy (state 0, 0 episodes)\n"),
              std::string::npos)
        << events;
    EXPECT_EQ(events.find("\nfocus  "), std::string::npos)
        << "no focus, no line: " << events;
    const std::vector<EventLine> ends = events_of(events, "end");
    ASSERT_EQ(ends.size(), 1u) << events;
    EXPECT_LE(ends[0].ms * 1000, after - before + 500) << events;
}
