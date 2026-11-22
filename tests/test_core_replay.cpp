// File/layer: tests/test_core_replay.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

namespace {

/* The DID replies the mock answers with, kept whole: at most kMax, each
 * one a frame (4 KB) on the heap. */
struct DidCapture {
    static constexpr size_t kMax = 24;
    std::vector<ecuio_frame_t> frames;
    std::atomic<size_t> count{0};
};

void capture_did_cb(const ecuio_frame_t* frame, void* user_data)
{
    DidCapture* cap = static_cast<DidCapture*>(user_data);
    if (frame->data_length >= 3 && frame->data[0] == 0x62 &&
        cap->count.load() < DidCapture::kMax) {
        cap->frames[cap->count.load()] = *frame;
        cap->count.fetch_add(1);
    }
}

} // namespace

/* Under uds-generic the mock answers each of the session's DIDs every
 * tick, from the ECU the def names, so the app shows the profile with no
 * hardware: a number that counts the ticks and a text naming the DID. */
TEST(CoreTest, MockAnswersTheSessionsDids) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(
        "mock://dids?profile=uds-generic&did=1E1C:u16,F190:text,7E9:1E1C:u8",
        &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_EQ(set->count, 3u);
    DidCapture cap;
    cap.frames.resize(DidCapture::kMax);
    ASSERT_EQ(ecuio_register_frame_callback(session, &capture_did_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&cap]() { return cap.count.load() >= 9; }, 5000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);   /* joins the worker */

    ASSERT_GE(cap.count.load(), size_t{9}) << "three DIDs over three ticks";
    size_t seen[3] = {0, 0, 0};
    double last_word = -1.0;
    for (size_t f = 0; f < cap.count.load(); ++f) {
        const ecuio_frame_t& frame = cap.frames[f];
        EXPECT_EQ(frame.flags, 0u) << "a classic-CAN reply, as J2534 delivers";
        bool applied = false;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (!ecuio_def_applies_to_frame(&d, &frame)) {
                continue;
            }
            applied = true;
            ++seen[i];
            if (d.kind == ECUIO_VALUE_TEXT) {
                char text[64] = "";
                ASSERT_EQ(ecuio_decode_text(&d, frame.data, frame.data_length,
                                            text, sizeof(text)), ECUIO_DECODE_OK);
                EXPECT_EQ(std::string(text).rfind("MOCK F190 #", 0), size_t{0}) << text;
            } else {
                double v = 0.0;
                ASSERT_EQ(ecuio_decode_signal(&d, frame.data, frame.data_length, &v),
                          ECUIO_DECODE_OK);
                if (i == 0) {
                    EXPECT_GT(v, last_word) << "the tick count grows";
                    last_word = v;
                    EXPECT_EQ(frame.arbitration_id, 0x7E8u);
                } else {
                    EXPECT_EQ(frame.arbitration_id, 0x7E9u) << "the ECU the prefix named";
                    EXPECT_EQ(frame.data_length, 4u) << "u8: one value byte";
                }
            }
        }
        EXPECT_TRUE(applied) << "frame " << f << " answers no def";
    }
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_GE(seen[i], size_t{3}) << set->defs[i].name;
    }
    ecuio_session_destroy(session);
}

// ---- replay:// (ADR-0023) ----

namespace {

/* A scratch name carries this process's id besides the test's name, so
 * runs of one case from two checkouts at once never share a file or a
 * folder (tests/temp_name.h) */
TEST(TempNameTest, TheNameCarriesThisProcessAndTheTest)
{
#ifdef _WIN32
    const long pid = _getpid();
#else
    const long pid = static_cast<long>(getpid());
#endif
    EXPECT_EQ(temp_name("ecuio-x"), "ecuio-x-" + std::to_string(pid) +
                                        "-TheNameCarriesThisProcessAndTheTest");
}

} // namespace

/* Past start-up a worker's frames cost it no allocation, so none can
 * throw: the mock's ticks, and replay's rows and passes (the file
 * reopened each time round), each decoded for a values callback too */
TEST(CoreTest, WorkersAllocateNothingPerFrame)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(
            ecuio_register_values_callback(session, worker_news_values_cb, &w),
            ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        EXPECT_GT(w.batches, 0) << "the values path ran";
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
}

/* Nor do the pull queues (ecuio/queue.h): their slots are made at open,
 * and each frame and value is copied into one, the oldest dropped when
 * full; the values queue alone makes the worker decode */
TEST(CoreTest, WorkersAllocateNothingPerFrameIntoQueues)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_queue_t* frames = nullptr;
        ecuio_queue_t* values = nullptr;
        ASSERT_EQ(ecuio_queue_open(session, ECUIO_QUEUE_FRAMES, 4, &frames),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_queue_open(session, ECUIO_QUEUE_VALUES, 4, &values),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        ecuio_queue_value_t v[4];
        uint32_t count = 0;
        uint64_t dropped = 0;
        EXPECT_EQ(ecuio_queue_read_values(values, v, 4, &count, &dropped, 0),
                  ECUIO_OK);
        EXPECT_GT(count, 0u) << "the values path ran";
        EXPECT_GT(dropped, 0u) << "and dropped";
        ecuio_queue_close(frames);
        ecuio_queue_close(values);
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
}

/* Recording costs the worker no allocation either: the ring is made at
 * open, and each frame and its values (decoded for the recording, with
 * no values callback registered) are copied into it (ADR-0049 section 1) */
TEST(CoreTest, WorkersAllocateNothingPerFrameWhileRecording)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr)
        tmp = std::getenv("TMPDIR");
    const std::filesystem::path root =
        std::filesystem::path(tmp != nullptr ? tmp : "/tmp") /
        temp_name("ecuio-core-recording-news");
    const std::string root8 = root.generic_u8string();
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_record_options_t o{};
        o.root_utf8 = root8.c_str();
        ecuio_recorder_t* recorder = nullptr;
        ASSERT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_session_record(session, recorder), ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        /* The writer counts the rows it writes, within its next wakes */
        ecuio_recorder_state_t st{};
        wait_for(
            [&] {
                ecuio_recorder_get_state(recorder, &st);
                return st.value_rows > 0;
            },
            2000);
        EXPECT_EQ(ecuio_recorder_close(recorder, nullptr), ECUIO_OK);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        EXPECT_GT(st.value_rows, 0u)
            << "the values were decoded for the recording";
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

/* Each row goes out as recorded (ID, flags, bytes, an empty payload
 * included), stamped with the recording's session time rebased to the
 * first row; the session info says what's playing */
TEST(CoreTest, ReplaySendsTheRecordedFramesAtTheirRebasedTimes)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "10,1000000,0x7E8,0x0,41 0C 1F 40\n"
                      "600,1000500,0x18DAF110,0x1,\n"
                      "2100,1002000,0x7E8,0x40,62 F1 90 41 42\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));

    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_REPLAY);
    EXPECT_EQ(info.replay_frames, 3u);
    EXPECT_EQ(info.replay_span_ms, 2u);
    EXPECT_EQ(info.replay_speed_milli, 0u) << "max";
    EXPECT_EQ(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP, 0u);
    EXPECT_EQ(info.channel_count, 0u) << "passive";
    EXPECT_STREQ(info.profile, "raw") << "replay's default";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int32_t last = -1;
    uint32_t episodes = 99;
    ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
              ECUIO_OK);
    EXPECT_EQ(last, ECUIO_OK) << "the end of the file isn't an error";
    EXPECT_EQ(episodes, 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);

    ASSERT_EQ(cap.frames.size(), 3u)
        << "the end of the file sends nothing more";
    const ecuio_frame_t& a = cap.frames[0];
    EXPECT_EQ(a.arbitration_id, 0x7E8u);
    EXPECT_EQ(a.flags, 0u);
    EXPECT_EQ(a.timestamp_us, 0u);
    ASSERT_EQ(a.data_length, 4u);
    EXPECT_EQ(a.data[0], 0x41);
    EXPECT_EQ(a.data[3], 0x40);
    const ecuio_frame_t& b = cap.frames[1];
    EXPECT_EQ(b.arbitration_id, 0x18DAF110u);
    EXPECT_EQ(b.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_EQ(b.data_length, 0u);
    EXPECT_EQ(b.timestamp_us, 500u);
    const ecuio_frame_t& c = cap.frames[2];
    EXPECT_EQ(c.flags, 0x40u) << "flags as recorded, TRUNCATED among them";
    EXPECT_EQ(c.data_length, 5u);
    EXPECT_EQ(c.timestamp_us, 2000u);
    ecuio_session_destroy(session);
}

/* A raw adapter clock that wraps steps timestamp_us backwards: that row
 * is timed by its host_us step instead, so session time never decreases */
TEST(CoreTest, ReplayTimesABackwardStampByTheHostClock)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,4294967000,0x7E8,0x0,41 0D 00\n"
                      "200,4294967200,0x7E8,0x0,41 0D 01\n"
                      "600,100,0x7E8,0x0,41 0D 02\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(cap.frames[0].timestamp_us, 0u);
    EXPECT_EQ(cap.frames[1].timestamp_us, 200u);
    EXPECT_EQ(cap.frames[2].timestamp_us, 600u) << "the host's 400 us step";
    ecuio_session_destroy(session);
}

/* loop=1 plays the file again, its stamps carried on from the last pass */
TEST(CoreTest, ReplayLoopsWithStampsThatNeverDecrease)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,0,0x7E8,0x0,41 0D 00\n"
                      "1000,1000,0x7E8,0x0,41 0D 01\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max&loop=1"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 6; }, 2000));
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP, 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    for (size_t i = 1; i < cap.frames.size(); ++i) {
        ASSERT_GE(cap.frames[i].timestamp_us, cap.frames[i - 1].timestamp_us)
            << "frame " << i;
    }
    EXPECT_EQ(cap.frames[2].timestamp_us, 1000u)
        << "the second pass starts where the first ended";
    EXPECT_EQ(cap.frames[3].timestamp_us, 2000u);
    EXPECT_EQ(cap.frames[2].data[2], 0x00) << "from the top";
    ecuio_session_destroy(session);
}

/* A recording whose frames all share one time (one frame, or several
 * stamped alike) has no span to pace a pass by: ?loop=1 is refused at
 * create, where the worker would spin; played once, it plays */
TEST(CoreTest, ReplayRefusesToLoopARecordingWithNoSpan)
{
    struct Case {
        const char* rows;
        size_t frames;
    };
    for (const Case& c : {Case{"5,7,0x7E8,0x0,41 0D 00\n", 1},
                          Case{"5,7,0x7E8,0x0,41 0D 00\n"
                               "6,7,0x7E8,0x0,41 0D 01\n", 2}}) {
        TempRecording rec(std::string(kFramesHeader) + c.rows);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri("?loop=1").c_str(), &session),
                  ECUIO_ERR_BAD_URI);
        EXPECT_EQ(session, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "?loop=1 needs a recording that spans time; this one's "
                     "frames all share one time");

        ReplayCapture cap;
        session = start_replay(rec.uri("?speed=max"), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= c.frames; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        EXPECT_EQ(cap.frames.size(), c.frames) << "played once";
        ecuio_session_destroy(session);
    }
}

/* Six rows a second apart by host_us, their adapter stamps 7 s ahead and
 * 0.25 s apart, so a stretch chosen by host_us and timed by timestamp_us
 * shows which clock does which; row i carries 41 0D i */
std::string stretch_rows()
{
    std::string text = kFramesHeader;
    for (int i = 0; i < 6; ++i) {
        char row[64];
        std::snprintf(row, sizeof row, "%d,%d,0x7E8,0x0,41 0D %02X\n",
                      i * 1000000, 7000000 + i * 250000, i);
        text += row;
    }
    return text;
}

/* ?from= and ?to= (ADR-0023's appendix "a stretch replayed") play the
 * rows from the first at or after from to the last at or before to, by
 * host_us, the viewer's and havne-cli view's seconds; the first is
 * stamped 0 and the rest by the recording's session time; the info counts
 * the stretch's frames and span */
TEST(CoreTest, ReplayStretchPlaysItsRowsAlone)
{
    TempRecording rec(stretch_rows());
    struct Case {
        const char* query;
        std::vector<int> rows;
    };
    const Case cases[] = {
        {"?speed=max&from=1.5&to=4", {2, 3, 4}},
        {"?speed=max&from=4", {4, 5}},
        {"?speed=max&to=1", {0, 1}},
        {"?speed=max&from=0.5&to=0.999&from=3&to=3.5", {3}}, /* last wins */
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.query);
        ReplayCapture cap;
        ecuio_session_t* session = start_replay(rec.uri(c.query), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(
            wait_for([&] { return cap.count() >= c.rows.size(); }, 2000));
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
        EXPECT_EQ(info.replay_frames, c.rows.size());
        EXPECT_EQ(info.replay_span_ms, (c.rows.size() - 1) * 250u);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int32_t last = -1;
        uint32_t episodes = 99;
        ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
                  ECUIO_OK);
        EXPECT_EQ(last, ECUIO_OK) << "the stretch's end isn't an early stop";
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        ASSERT_EQ(cap.frames.size(), c.rows.size()) << "nothing past to";
        for (size_t i = 0; i < c.rows.size(); ++i) {
            EXPECT_EQ(int{cap.frames[i].data[2]}, c.rows[i]) << "frame " << i;
            EXPECT_EQ(cap.frames[i].timestamp_us, i * 250000u)
                << "frame " << i << ": 0 first, then session time";
        }
        ecuio_session_destroy(session);
    }
}

/* The rows before a stretch are read but never waited for: at speed 1
 * its first frame goes out at once and the next 0.3 s of session time
 * later (the rows' host_us steps are 1 s); a loop plays the stretch
 * again, its stamps carried on. Both frames are timed from before the
 * session starts, and frames are due at set times from the worker's
 * start: a wrong wait puts them past each bound below on any machine,
 * so a loaded one has until those bounds to run a right one */
TEST(CoreTest, ReplayStretchStartsAtOnceAndLoopsItself)
{
    std::string text = kFramesHeader;
    for (int i = 0; i < 5; ++i) {
        char row[64];
        std::snprintf(row, sizeof row, "%d,%d,0x7E8,0x0,41 0D %02X\n",
                      i * 1000000, i * 300000, i);
        text += row;
    }
    TempRecording rec(text);
    ReplayCapture cap;
    const auto started = std::chrono::steady_clock::now();
    ecuio_session_t* session = start_replay(rec.uri("?from=3&to=4"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 3000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    const auto ms = [&](size_t i) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   cap.arrived[i] - started)
            .count();
    };
    EXPECT_LT(ms(0), 900) << "not after the 0.9 s of rows before it";
    EXPECT_GE(ms(1), 300) << "the stretch's own 0.3 s";
    EXPECT_LT(ms(1), 1000) << "timestamp_us's 0.3 s, not host_us's 1 s";
    ecuio_session_destroy(session);

    ReplayCapture looped;
    session = start_replay(rec.uri("?speed=max&loop=1&from=3&to=4"), &looped);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return looped.count() >= 6; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(int{looped.frames[i].data[2]}, static_cast<int>(3 + i % 2))
            << "frame " << i;
        EXPECT_EQ(looped.frames[i].timestamp_us, (i + 1) / 2 * 300000u)
            << "frame " << i;
    }
    ecuio_session_destroy(session);
}

/* What a stretch refuses, at create: a value that isn't seconds or to
 * not after from (BAD_URI), a stretch with no frames, saying where the
 * frames run (LOAD_FAILED), and a loop of one instant (BAD_URI) */
TEST(CoreTest, ReplayStretchRefusals)
{
    TempRecording rec(stretch_rows());
    const char* const bad_values[] = {
        "?from=-1",       "?from=1.2345", "?from=abc", "?to=",   "?from=1e3",
        "?to=1000000001", "?from=.5",     "?from=5.",  "?to=1,5"};
    for (const char* q : bad_values) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "?from= and ?to= take the recording's seconds, as the "
                     "viewer and havne-cli view show them, with at most three "
                     "decimals")
            << q;
    }
    for (const char* q : {"?from=2&to=2", "?from=3&to=1"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_STREQ(ecuio_last_error_detail(), "?to= must come after ?from=")
            << q;
    }
    struct Empty {
        const char* query;
        const char* says;
    };
    const Empty empties[] = {
        {"?from=10", "the recording holds no frames from 10 s on: its frames "
                     "run from 0 s to 5 s"},
        {"?from=1.2&to=1.8", "the recording holds no frames from 1.2 s to "
                             "1.8 s: its frames run from 0 s to 5 s"},
    };
    for (const Empty& e : empties) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(e.query).c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED)
            << e.query;
        EXPECT_STREQ(ecuio_last_error_detail(), e.says);
    }
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(rec.uri("?loop=1&from=2&to=2.5").c_str(),
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?loop=1 needs a stretch that spans time; this one's frames "
                 "all share one time");
}

/* One second of recording takes about a second at speed 1 and a tenth of
 * that at 10; the frames keep the recording's times either way */
TEST(CoreTest, ReplayPacesByTheRecordingsTimeOverTheSpeed)
{
    TempRecording rec(paced_rows(11));
    for (const char* q : {"", "?speed=10"}) {
        SCOPED_TRACE(q);
        ReplayCapture cap;
        ecuio_session_t* session = start_replay(rec.uri(q), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= 11; }, 5000));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            cap.arrived.back() - cap.arrived.front())
                            .count();
        if (q[0] == '\0') {
            EXPECT_GE(ms, 900) << "real time";
            EXPECT_LE(ms, 1600);
        } else {
            EXPECT_GE(ms, 80) << "ten times faster";
            EXPECT_LE(ms, 400);
        }
        EXPECT_EQ(cap.frames.back().timestamp_us, 1000000u)
            << "the recording's time";
        ecuio_session_destroy(session);
    }
}

/* Each frame is due at its time from the start of playback, not a step
 * after the last send: a frame held up 400 ms is followed at once by the
 * ones it delayed, and a second of recording still takes about a second.
 * Waiting each step from the last send would add the stall, and on
 * Windows every short wait's rounding, so 200 steps of 5 ms (speed 1) or
 * 50 ms at 10x (5 ms waits) show it either way. */
TEST(CoreTest, ReplayCatchesUpAfterALateFrameAndNeverDrifts)
{
    struct Case {
        const char* query;
        int step_us;
    };
    for (const Case& c : {Case{"", 5000}, Case{"?speed=10", 50000}}) {
        SCOPED_TRACE(c.query);
        TempRecording rec(paced_rows(201, c.step_us));
        ReplayCapture cap;
        cap.stall_at = 20;
        cap.stall_ms = 400;
        ecuio_session_t* session = start_replay(rec.uri(c.query), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= 201; }, 5000));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            cap.arrived.back() - cap.arrived.front())
                            .count();
        EXPECT_GE(ms, 950) << "the last frame is due a second in";
        EXPECT_LE(ms, 1250) << "late frames catch up; the stall isn't added";
        EXPECT_EQ(cap.frames.back().timestamp_us,
                  200u * static_cast<uint64_t>(c.step_us))
            << "the recording's time";
        ecuio_session_destroy(session);
    }
}

/* Attach checked the file, but it can change before the worker reads it
 * again: playback sends the rows up to the first bad one, then stops, and
 * the session still closes cleanly */
TEST(CoreTest, ReplayStopsAtARowThatTurnedBadAfterAttach)
{
    TempRecording rec(paced_rows(5));
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    {
        const std::string rows = paced_rows(3);
        const std::string cut =
            rows + "5300000,5300000,0x7E8,0x0,41 0"; // cut mid-row
        FILE* f = std::fopen(rec.csv.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(cut.data(), 1, cut.size(), f);
        std::fclose(f);
    }
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_session_get_error_state(session, &last, &episodes);
            return last != ECUIO_OK;
        },
        2000));
    EXPECT_EQ(last, ECUIO_ERR_RECORDING_CHANGED) << "says why it stopped";
    EXPECT_EQ(episodes, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cap.count(), 3u) << "nothing past the bad row";
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A recording cut short after attach plays what's left, then says why it
 * stopped: a reader counting to attach's rows would wait forever. One
 * that grew plays attach's rows and ends as at the end of its file */
TEST(CoreTest, ReplayOfARecordingWithOtherRowsPlaysAttachsAtMost)
{
    struct Case {
        int rows_now;
        ecuio_status_t state;
    };
    for (const Case& c :
         {Case{3, ECUIO_ERR_RECORDING_CHANGED}, Case{7, ECUIO_OK}}) {
        SCOPED_TRACE(c.rows_now);
        TempRecording rec(paced_rows(5));
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        {
            const std::string rows = paced_rows(c.rows_now);
            FILE* f = std::fopen(rec.csv.c_str(), "wb");
            ASSERT_NE(f, nullptr);
            std::fwrite(rows.data(), 1, rows.size(), f);
            std::fclose(f);
        }
        ReplayCapture cap;
        ASSERT_EQ(
            ecuio_register_frame_callback(session, replay_capture_cb, &cap),
            ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
        const size_t played =
            static_cast<size_t>(c.rows_now < 5 ? c.rows_now : 5);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= played; }, 2000));
        int32_t last = ECUIO_OK;
        uint32_t episodes = 0;
        if (c.state != ECUIO_OK) {
            ASSERT_TRUE(wait_for(
                [&] {
                    ecuio_session_get_error_state(session, &last, &episodes);
                    return last != ECUIO_OK;
                },
                2000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ecuio_session_get_error_state(session, &last, &episodes);
        EXPECT_EQ(last, c.state);
        EXPECT_EQ(episodes, c.state == ECUIO_OK ? 0u : 1u);
        EXPECT_EQ(cap.count(), played) << "attach's rows at most";
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
    }
}

/* A recording deleted between attach and start: nothing to play, and the
 * error state says so */
TEST(CoreTest, ReplayOfARecordingThatWentAwaySaysSo)
{
    TempRecording rec(paced_rows(3));
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(std::remove(rec.csv.c_str()), 0);
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_session_get_error_state(session, &last, &episodes);
            return last != ECUIO_OK;
        },
        2000));
    EXPECT_EQ(last, ECUIO_ERR_RECORDING_CHANGED);
    EXPECT_EQ(episodes, 1u);
    EXPECT_EQ(cap.count(), 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A long gap in the recording doesn't hold up a close */
TEST(CoreTest, ReplayClosesAtOnceDuringAGap)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,0,0x7E8,0x0,41 0D 00\n"
                      "3600000000,3600000000,0x7E8,0x0,41 0D 01\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri(), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 1; }, 2000));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    EXPECT_EQ(cap.count(), 1u) << "the hour-later frame never went out";
    ecuio_session_destroy(session);
}

/* Attach reads the whole file: anything but a frames.csv, or a bad row,
 * fails create with its line number. Either line ending reads, and a
 * whole-message payload arrives whole. */
/* ADR-0049 section 7: a last row with no newline is a write cut short,
 * left out and counted; a recording of it alone holds no frames */
TEST(CoreTest, ReplayDropsATornLastRowAndCountsIt)
{
    TempRecording rec(std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0C 1F 40\n"
                                                   "2,2,0x7E8,0x0,41 0D 20\n"
                                                   "3,3,0x7E8,0x0,41 0C 1F");
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_frames, 2u);
    EXPECT_EQ(info.replay_rows_torn, 1u);
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cap.count(), 2u) << "the torn row isn't played";
    int32_t last = -1;
    uint32_t episodes = 99;
    ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
              ECUIO_OK);
    EXPECT_EQ(last, ECUIO_OK) << "a whole pass, not a changed file";
    EXPECT_EQ(episodes, 0u);
    ecuio_session_destroy(session);

    TempRecording whole(std::string(kFramesHeader) +
                        "1,1,0x7E8,0x0,41 0C 1F 40\n");
    ASSERT_EQ(ecuio_session_create(whole.uri().c_str(), &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_rows_torn, 0u);
    ecuio_session_destroy(session);

    TempRecording only(std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0C");
    EXPECT_EQ(ecuio_session_create(only.uri().c_str(), &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the recording's frames.csv holds no frames");
}

/* A row attach read whole that has lost its newline when the worker
 * reaches it is a file changed since attach, not a torn row to skip */
TEST(CoreTest, ReplayOfARowThatLostItsNewlineSaysTheFileChanged)
{
    const std::string rows = std::string(kFramesHeader) +
                             "1,1,0x7E8,0x0,41 0C 1F 40\n"
                             "2,2,0x7E8,0x0,41 0D 20\n";
    TempRecording rec(rows);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK);
    {
        FILE* f = std::fopen(rec.csv.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(rows.data(), 1, rows.size() - 1, f); /* the last '\n' */
        std::fclose(f);
    }
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    int32_t last = -1;
    uint32_t episodes = 0;
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_error_state(session, &last, &episodes) ==
                       ECUIO_OK &&
                   last == ECUIO_ERR_RECORDING_CHANGED;
        },
        5000));
    EXPECT_EQ(cap.count(), 1u);
    ecuio_session_destroy(session);
}

TEST(CoreTest, ReplayRefusesABadRecordingWithItsLineNumber)
{
    const std::string ok_row = "1,1,0x7E8,0x0,41 0C 1F 40\n";
    struct Case {
        std::string text;
        const char* detail;
    };
    const std::vector<Case> cases = {
        {"", "the recording's frames.csv is empty"},
        {"host_us,signal,value,unit\n1,RPM,800,RPM\n",
         "not a Havne frames.csv, or a newer format"},
        {kFramesHeader, "the recording's frames.csv holds no frames"},
        {std::string(kFramesHeader) + ok_row + "2,2,7E8,0x0,41\n",
         "frames.csv line 3: id isn't 0x-prefixed hex"},
        {std::string(kFramesHeader) + "x,1,0x7E8,0x0,41\n",
         "frames.csv line 2: host_us isn't"},
        {std::string(kFramesHeader) + "1,-1,0x7E8,0x0,41\n",
         "frames.csv line 2: timestamp_us isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,1,41\n",
         "frames.csv line 2: flags isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0\n",
         "frames.csv line 2: data isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 \n",
         "frames.csv line 2: data isn't"},
        {std::string(kFramesHeader) + "1,1,0x123456789,0x0,41\n",
         "frames.csv line 2: id isn't"},
        /* 0xEF and two more bytes that aren't a byte-order mark's */
        {std::string("\xEF" "XY") + kFramesHeader + ok_row,
         "not a Havne frames.csv, or a newer format"},
        {std::string("\xEF\n") + ok_row,
         "not a Havne frames.csv, or a newer format"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        TempRecording rec(c.text);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(session, nullptr);
        EXPECT_EQ(std::string(ecuio_last_error_detail()).rfind(c.detail, 0),
                  size_t{0})
            << ecuio_last_error_detail();
    }

    {
        /* A byte-order mark ahead of the header, as an editor may add */
        TempRecording rec(std::string("\xEF\xBB\xBF") + kFramesHeader + ok_row);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_session_destroy(session);
    }

    std::string long_data, too_long;
    for (unsigned i = 0; i < ECUIO_FRAME_DATA_MAX; ++i)
        long_data += (i ? " " : "") + std::string("A5");
    too_long = long_data + " A5";
    {
        TempRecording rec(std::string(kFramesHeader) + "1,1,0x7E8,0x0," +
                          too_long + "\n");
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "frames.csv line 2: data is longer than a frame holds");
    }

    TempRecording crlf(std::string("host_us,timestamp_us,id,flags,data\r\n") +
                       "1,1,0x7E8,0x0," + long_data +
                       "\r\n2,2,0x7E8,0x0,41 0C 1F 40\r\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(crlf.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(cap.frames[0].data_length,
              static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX));
    EXPECT_EQ(cap.frames[0].data[ECUIO_FRAME_DATA_MAX - 1], 0xA5);
    EXPECT_EQ(cap.frames[1].data_length, 4u)
        << "CRLF read as an ending, not data";
    ecuio_session_destroy(session);

    ecuio_session_t* missing = nullptr;
#ifdef _WIN32
    const char* gone = "replay://C:/ecuio-definitely-missing/frames.csv";
#else
    const char* gone = "replay:///ecuio-definitely-missing/frames.csv";
#endif
    EXPECT_EQ(ecuio_session_create(gone, &missing), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "couldn't open the recording's frames.csv");
}

/* The path must be absolute; a folder means its frames.csv; speed, loop
 * and unknown keys are judged before the file is read */
/* ---- Fault injection on a replay (ADR-0041) ---- */

