// File/layer: tests/test_core_replay_faults.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

namespace {

/* A faulted replay of n rows at max speed, run to its end: the frames
 * delivered and the counts */
struct FaultedRun {
    std::vector<ecuio_frame_t> frames;
    ecuio_replay_faults_t counts{};
    uint32_t info_flags = 0;
};

FaultedRun run_faulted(const TempRecording& rec, const std::string& query,
                       uint64_t rows)
{
    FaultedRun r;
    ReplayCapture capture;
    ecuio_session_t* session = start_replay(rec.uri(query.c_str()), &capture);
    if (session == nullptr) {
        return r;
    }
    EXPECT_TRUE(wait_for(
        [&] {
            ecuio_replay_faults_t c{};
            ecuio_session_get_replay_faults(session, &c);
            return c.frames == rows &&
                   capture.count() == rows - c.dropped - c.overflowed;
        },
        10000))
        << query;
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_get_replay_faults(session, &r.counts), ECUIO_OK);
    ecuio_session_info_t info{};
    EXPECT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    r.info_flags = info.flags;
    ecuio_session_destroy(session);
    std::lock_guard<std::mutex> lock(capture.mutex);
    r.frames = capture.frames;
    return r;
}

/* Within six standard deviations of n * p: a bound no seed should miss */
void expect_rate(uint64_t got, uint64_t n, double p, const char* what)
{
    const double mean = n * p;
    const double sd = std::sqrt(n * p * (1 - p));
    EXPECT_GE(static_cast<double>(got), mean - 6 * sd) << what;
    EXPECT_LE(static_cast<double>(got), mean + 6 * sd) << what;
}

int bits_apart(const ecuio_frame_t& f, const uint8_t* want, uint16_t len)
{
    int n = 0;
    for (uint16_t i = 0; i < len; ++i) {
        uint8_t x = static_cast<uint8_t>(f.data[i] ^ want[i]);
        for (; x != 0; x &= static_cast<uint8_t>(x - 1)) {
            ++n;
        }
    }
    return n;
}

} // namespace

/* ?faults= and ?seed=: each kind once, a percent over 0 and up to 100
 * with three decimals at most, a stall's length 1 to 10000 ms, a seed
 * from 1; anything else refused, through the session too */
TEST(CoreTest, ReplayFaultsReadTheirQuery)
{
    const auto parses = [](const char* q, replay_faults* f = nullptr) {
        replay_faults scratch;
        replay_faults* out = f != nullptr ? f : &scratch;
        return replay_parse_faults(q, std::strlen(q), out);
    };
    replay_faults f;
    ASSERT_TRUE(parses("drop:2,overflow:0.5,corrupt:100,truncate:12.125,"
                       "stall:1:250",
                       &f));
    EXPECT_TRUE(f.any);
    EXPECT_EQ(f.rate[REPLAY_FAULT_DROP], 2000u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_OVERFLOW], 500u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_CORRUPT], 100000u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_TRUNCATE], 12125u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_STALL], 1000u);
    EXPECT_EQ(f.stall_ms, 250u);
    EXPECT_TRUE(parses("stall:0.001:10000"));
    for (const char* bad :
         {"",           "drop",         "drop:",         "drop:0",
          "drop:0.000", "drop:100.001", "drop:101",      "drop:1.2345",
          "drop:-1",    "drop:1e2",     "drop: 1",       "drop:2,drop:3",
          "stall:5",    "stall:5:0",    "stall:5:10001", "drop:5:10",
          "nope:5",     "drop:5,",      ",drop:5",       "Drop:5"}) {
        EXPECT_FALSE(parses(bad)) << bad;
    }
    uint32_t seed = 0;
    EXPECT_TRUE(replay_parse_seed("4294967295", 10, &seed));
    EXPECT_EQ(seed, 4294967295u);
    for (const char* bad : {"0", "4294967296", "", "x", "-1", "1 "}) {
        EXPECT_FALSE(replay_parse_seed(bad, std::strlen(bad), &seed)) << bad;
    }

    TempRecording rec(paced_rows(2));
    for (const char* q :
         {"?faults=drop:0", "?faults=", "?seed=0", "?faults=drop:5&seed=x"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
    }
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?faults=drop:5").c_str(), &session),
              ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS, 0u);
    ecuio_session_destroy(session);
}

/* The same seed plays the same faults, frame for frame; another seed
 * others. A replay without faults counts its frames and nothing else. */
TEST(CoreTest, ReplayFaultsAreTheSameFromTheSameSeed)
{
    TempRecording rec(paced_rows(300, 1000));
    const std::string q =
        "?speed=max&faults=drop:10,overflow:5,corrupt:10,truncate:10";
    const FaultedRun a = run_faulted(rec, q + "&seed=7", 300);
    const FaultedRun b = run_faulted(rec, q + "&seed=7", 300);
    const FaultedRun c = run_faulted(rec, q + "&seed=8", 300);
    ASSERT_EQ(a.frames.size(), b.frames.size());
    for (size_t i = 0; i < a.frames.size(); ++i) {
        ASSERT_EQ(a.frames[i].data_length, b.frames[i].data_length) << i;
        EXPECT_EQ(std::memcmp(a.frames[i].data, b.frames[i].data,
                              a.frames[i].data_length),
                  0)
            << i;
        EXPECT_EQ(a.frames[i].flags, b.frames[i].flags) << i;
        EXPECT_EQ(a.frames[i].timestamp_us, b.frames[i].timestamp_us) << i;
    }
    EXPECT_EQ(std::memcmp(&a.counts, &b.counts, sizeof a.counts), 0);
    EXPECT_NE(std::memcmp(&a.counts, &c.counts, sizeof a.counts), 0)
        << "seed 8 drew the same counts as seed 7";

    const FaultedRun clean = run_faulted(rec, "?speed=max", 300);
    EXPECT_EQ(clean.frames.size(), 300u);
    EXPECT_EQ(clean.counts.frames, 300u);
    EXPECT_EQ(clean.counts.dropped + clean.counts.overflowed +
                  clean.counts.corrupted + clean.counts.truncated +
                  clean.counts.stalled,
              0u);
    EXPECT_EQ(clean.info_flags & ECUIO_SESSION_INFO_REPLAY_FAULTS, 0u);
}

/* Each kind does what it counts: a drop removes the frame, an overflow
 * too and flags the next one delivered, a corruption flips one bit, a
 * truncation shortens the data, a stall pushes every later stamp back;
 * each near its rate */
TEST(CoreTest, ReplayFaultsDoWhatTheyCount)
{
    constexpr uint64_t kRows = 1000;
    TempRecording rec(paced_rows(static_cast<int>(kRows), 1000));
    const uint8_t clean[4] = {0x41, 0x0C, 0x1F, 0x40};

    FaultedRun r = run_faulted(rec, "?speed=max&faults=drop:20", kRows);
    EXPECT_EQ(r.frames.size() + r.counts.dropped, kRows);
    expect_rate(r.counts.dropped, kRows, 0.2, "drop");
    for (const ecuio_frame_t& f : r.frames) {
        EXPECT_EQ(f.flags & ECUIO_FRAME_FLAG_OVERFLOW, 0u)
            << "a drop says nothing";
    }

    r = run_faulted(rec, "?speed=max&faults=overflow:20", kRows);
    EXPECT_EQ(r.frames.size() + r.counts.overflowed, kRows);
    expect_rate(r.counts.overflowed, kRows, 0.2, "overflow");
    uint64_t flagged = 0;
    for (const ecuio_frame_t& f : r.frames) {
        flagged += (f.flags & ECUIO_FRAME_FLAG_OVERFLOW) != 0 ? 1 : 0;
    }
    EXPECT_GT(flagged, 0u);
    EXPECT_LE(flagged, r.counts.overflowed) << "losses in a row flag one frame";
    /* a flagged frame's stamp is past a missing row's: rows are 1 ms apart */
    for (size_t i = 1; i < r.frames.size(); ++i) {
        if (r.frames[i].flags & ECUIO_FRAME_FLAG_OVERFLOW) {
            EXPECT_GE(r.frames[i].timestamp_us - r.frames[i - 1].timestamp_us,
                      2000u)
                << i;
        }
    }

    r = run_faulted(rec, "?speed=max&faults=corrupt:100", kRows);
    ASSERT_EQ(r.frames.size(), kRows);
    EXPECT_EQ(r.counts.corrupted, kRows);
    for (const ecuio_frame_t& f : r.frames) {
        ASSERT_EQ(f.data_length, 4u);
        EXPECT_EQ(bits_apart(f, clean, 4), 1);
    }

    r = run_faulted(rec, "?speed=max&faults=truncate:30", kRows);
    ASSERT_EQ(r.frames.size(), kRows);
    expect_rate(r.counts.truncated, kRows, 0.3, "truncate");
    uint64_t shorter = 0;
    for (const ecuio_frame_t& f : r.frames) {
        ASSERT_LE(f.data_length, 4u);
        if (f.data_length < 4) {
            ++shorter;
        }
        EXPECT_EQ(std::memcmp(f.data, clean, f.data_length), 0)
            << "a truncation keeps what's left as it was";
    }
    EXPECT_EQ(shorter, r.counts.truncated);

    r = run_faulted(rec, "?speed=max&faults=stall:5:40", kRows);
    ASSERT_EQ(r.frames.size(), kRows);
    expect_rate(r.counts.stalled, kRows, 0.05, "stall");
    /* the recording's own steps are 1 ms; each stall adds 40 */
    uint64_t pushed = 0;
    for (size_t i = 1; i < r.frames.size(); ++i) {
        const uint64_t step =
            r.frames[i].timestamp_us - r.frames[i - 1].timestamp_us;
        ASSERT_TRUE(step == 1000u || step == 41000u) << i << ": " << step;
        pushed += step == 41000u ? 1 : 0;
    }
    const bool first_stalled = r.frames[0].timestamp_us == 40000u;
    EXPECT_EQ(pushed + (first_stalled ? 1 : 0), r.counts.stalled);
}

/* A stall is paced: at speed 1 the frames after it come that much later */
TEST(CoreTest, ReplayFaultsStallThePace)
{
    TempRecording rec(paced_rows(3, 10000));
    ReplayCapture capture;
    const auto t0 = std::chrono::steady_clock::now();
    ecuio_session_t* session =
        start_replay(rec.uri("?faults=stall:100:150"), &capture);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return capture.count() == 3; }, 5000));
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           capture.arrived[2] - t0)
                           .count();
    EXPECT_GE(spent, 3 * 150 + 20) << "three stalls and the recording's 20 ms";
    EXPECT_EQ(capture.frames[2].timestamp_us, 3u * 150000u + 20000u);
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, ReplayJudgesItsPathAndQuery)
{
    TempRecording folder(paced_rows(2), /*as_folder=*/true);
    ASSERT_TRUE(folder.made_dir);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(folder.uri().c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_destroy(session);
    session = nullptr;
    ASSERT_EQ(
        ecuio_session_create((folder.uri() + "/?speed=0.1").c_str(), &session),
        ECUIO_OK)
        << "a trailing separator";
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_speed_milli, 100u);
    ecuio_session_destroy(session);

    for (const char* bad : {"replay://", "replay://?speed=1",
                            "replay://recordings/x", "replay://frames.csv"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(bad, &session), ECUIO_ERR_BAD_URI)
            << bad;
        EXPECT_EQ(session, nullptr);
    }
    for (const char* q :
         {"?speed=0", "?speed=0.05", "?speed=101", "?speed=1.2345", "?speed=1.",
          "?speed=.5", "?speed=fast", "?speed=", "?loop=2", "?loop=yes"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(folder.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
    }
    for (const char* q : {"?speed=100", "?speed=2.5", "?speed=max", "?loop=0",
                          "?loop=1", "?speed=1&speed=3"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(folder.uri(q).c_str(), &session),
                  ECUIO_OK)
            << q;
        ecuio_session_destroy(session);
    }
    session = nullptr;
    EXPECT_EQ(
        ecuio_session_create(folder.uri("?baud=500000").c_str(), &session),
        ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "replay:// takes no query key 'baud', only profile, did, "
                 "speed, loop, faults, seed, from and to");

    TempRecording empty_dir_marker("", /*as_folder=*/true);
    std::remove(empty_dir_marker.csv.c_str());
    session = nullptr;
    EXPECT_EQ(ecuio_session_create(empty_dir_marker.uri().c_str(), &session),
              ECUIO_ERR_LOAD_FAILED)
        << "a folder with no frames.csv";
}

/* Recorded replies decode under the profile the URI names: the defs
 * match by ID and mux key, with no request behind them */
TEST(CoreTest, ReplayDecodesUnderTheProfileItNames)
{
    TempRecording rec(paced_rows(2));
    ReplayCapture cap;
    ecuio_session_t* session =
        start_replay(rec.uri("?profile=obd2&speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    bool decoded = false;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (!ecuio_def_applies_to_frame(&d, &cap.frames[0]))
            continue;
        double v = 0.0;
        ASSERT_EQ(ecuio_decode_signal(&d, cap.frames[0].data,
                                      cap.frames[0].data_length, &v),
                  ECUIO_DECODE_OK);
        EXPECT_DOUBLE_EQ(v, 2000.0) << d.name << ": 0x1F40 / 4";
        decoded = true;
    }
    EXPECT_TRUE(decoded) << "41 0C is obd2's RPM";
    ecuio_session_destroy(session);
}

/* The second vehicle session's engine start (docs/bench-sessions/
 * 2026-10-25-vehicle.md, Phase 4), replayed under obd2: the values the
 * sheet reports come back out of the recorded replies. The fixture is
 * the recording scrubbed of its identification replies (ADR-0023), and
 * this test holds it to that. */
TEST(CoreTest, ReplayOfTheVehicleFixtureDecodesTheSheetsValues)
{
    const std::string dir =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    ReplayCapture cap;
    ecuio_session_t* session =
        start_replay("replay://" + dir + "?profile=obd2&speed=max", &cap);
    ASSERT_NE(session, nullptr);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    const size_t n = info.replay_frames;
    EXPECT_EQ(n, 7074u);
    EXPECT_EQ(info.replay_span_ms, 297446u);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= n; }, 10000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ASSERT_EQ(cap.frames.size(), n);

    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    /* The obd2 def reading a Service 01 PID */
    const auto def_for = [set](uint8_t pid) -> const ecuio_signal_def_t* {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].mux_length == 2 &&
                set->defs[i].mux_key == (0x4100u | pid)) {
                return &set->defs[i];
            }
        }
        return nullptr;
    };
    const ecuio_signal_def_t* rpm = def_for(0x0C);
    const ecuio_signal_def_t* coolant = def_for(0x05);
    const ecuio_signal_def_t* volts = def_for(0x42);
    ASSERT_TRUE(rpm && coolant && volts);

    double rpm_max = 0, volts_min = 1e9, coolant_min = 1e9, coolant_max = -1e9;
    double first_rpm_s = -1;
    size_t identification = 0;
    for (const ecuio_frame_t& f : cap.frames) {
        identification +=
            (f.data_length >= 1 && f.data[0] == 0x49) ||
            (f.data_length >= 2 && f.data[0] == 0x62 && f.data[1] == 0xF1);
        double v = 0;
        if (ecuio_def_applies_to_frame(rpm, &f) &&
            ecuio_decode_signal(rpm, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            rpm_max = std::max(rpm_max, v);
            if (v > 0 && first_rpm_s < 0)
                first_rpm_s = f.timestamp_us / 1e6;
        }
        if (ecuio_def_applies_to_frame(volts, &f) &&
            ecuio_decode_signal(volts, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            volts_min = std::min(volts_min, v);
        }
        if (ecuio_def_applies_to_frame(coolant, &f) &&
            ecuio_decode_signal(coolant, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            coolant_min = std::min(coolant_min, v);
            coolant_max = std::max(coolant_max, v);
        }
    }
    EXPECT_EQ(identification, 0u)
        << "the fixture carries no identification reply";
    EXPECT_NEAR(rpm_max, 1261.25, 0.01) << "the sheet's 1,261 peak";
    EXPECT_NEAR(volts_min, 9.564, 0.0005)
        << "the sheet's 9.56 V dip at the starter";
    EXPECT_EQ(coolant_min, 60.0) << "the sheet's 60 to 86 degrees C";
    EXPECT_EQ(coolant_max, 86.0);
    EXPECT_NEAR(first_rpm_s, 3.45, 0.01)
        << "the start, 3.45 s into the recording";
    EXPECT_EQ(cap.frames.back().timestamp_us, 297446075u)
        << "the recording's span, rebased";
    ecuio_session_destroy(session);
}

/* ---- Decoded values (ADR-0024) ------------------------------------- */

