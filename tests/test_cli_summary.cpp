// File/layer: tests/test_cli_summary.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliSummaryTest (test_cli)
//
// cli/summary_lines.h: log's tally of values and intervals, its window,
// the interval buckets and the interval and summary lines.

#include "summary_lines.h"

#include "new_counter.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A session's worker feeding a tally, as log's callbacks do, with the
 * worker's own operator new count read at two frames past its start-up */
struct WorkerRun {
    static constexpr int kFrom = 3;
    static constexpr int kTo = 12;
    cli::SummaryTally* tally = nullptr;
    std::atomic<int> frames{0};
    std::atomic<unsigned long> at_from{0};
    std::atomic<unsigned long> at_to{0};
};

void worker_frame(const ecuio_frame_t*, void* user)
{
    auto* w = static_cast<WorkerRun*>(user);
    w->tally->on_frame();
    const int n = w->frames.fetch_add(1) + 1;
    if (n == WorkerRun::kFrom) {
        w->at_from.store(new_counter::on_this_thread());
    }
    if (n == WorkerRun::kTo) {
        w->at_to.store(new_counter::on_this_thread());
    }
}

void worker_values(const ecuio_value_batch_t* batch, void* user)
{
    static_cast<WorkerRun*>(user)->tally->on_batch(*batch);
}

/* Three number signals at sparse ids, as a file's own and a base's are */
struct Set {
    ecuio_signal_def_t defs[3];
    ecuio_signal_set_t set;
    Set()
    {
        std::memset(defs, 0, sizeof defs);
        std::memset(&set, 0, sizeof set);
        const char* names[] = {"RPM", "Vehicle Speed", "Boost"};
        const uint16_t ids[] = {12, 40001, 7};
        for (size_t i = 0; i < 3; ++i) {
            defs[i].id = ids[i];
            std::strncpy(defs[i].name, names[i], sizeof defs[i].name - 1);
            defs[i].kind = ECUIO_VALUE_UINT;
            defs[i].message_id = 0x7E8;
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.defs = defs;
        set.count = 3;
    }
};

constexpr uint16_t kRpm = 12;
constexpr uint16_t kSpeed = 40001;
constexpr uint16_t kBoost = 7;

/* One batch at ts_us holding the values of ids */
void feed(cli::SummaryTally& s, uint64_t ts_us, std::vector<uint16_t> ids)
{
    std::vector<ecuio_value_t> v(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        std::memset(&v[i], 0, sizeof v[i]);
        v[i].signal_id = ids[i];
        v[i].kind = ECUIO_VALUE_UINT;
    }
    ecuio_value_batch_t b;
    std::memset(&b, 0, sizeof b);
    b.timestamp_us = ts_us;
    b.count = static_cast<uint32_t>(v.size());
    b.values = v.data();
    s.on_frame();
    s.on_batch(b);
}

constexpr uint64_t ms(uint64_t n)
{
    return n * 1000;
}

template <typename F> std::string text(F fill)
{
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    fill(l);
    EXPECT_TRUE(l.finish());
    return std::string(l.data(), l.size() - 1);
}

bool has(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

} // namespace

/* Eighth octaves from 2^10 us: everything below in the first, from 2^26
 * us in the last; a top is the next bucket's first interval */
TEST(CliSummaryTest, EachOctaveSplitsInEighthsFromAMillisecond)
{
    EXPECT_EQ(cli::interval_bucket(0), 0u);
    EXPECT_EQ(cli::interval_bucket(1023), 0u);
    EXPECT_EQ(cli::interval_bucket(1151), 0u);
    EXPECT_EQ(cli::interval_bucket(1152), 1u);
    EXPECT_EQ(cli::interval_bucket(2047), 7u);
    EXPECT_EQ(cli::interval_bucket(2048), 8u);
    EXPECT_EQ(cli::interval_bucket((uint64_t{1} << 26) - 1), 127u);
    EXPECT_EQ(cli::interval_bucket(uint64_t{1} << 40), 127u);
    EXPECT_EQ(cli::interval_bucket_top(0), 1152u);
    EXPECT_EQ(cli::interval_bucket_top(7), 2048u);
    EXPECT_EQ(cli::interval_bucket_top(8), 2304u);
    for (uint32_t i = 0; i + 1 < cli::kIntervalBuckets; ++i) {
        EXPECT_EQ(cli::interval_bucket(cli::interval_bucket_top(i)), i + 1);
        EXPECT_EQ(cli::interval_bucket(cli::interval_bucket_top(i) - 1), i);
    }
}

/* A percentile is its rank's bucket top, at most an eighth above the
 * interval, and never past the exact maximum */
TEST(CliSummaryTest, APercentileReadsAtItsBucketsTopUnderTheMaximum)
{
    Set s;
    cli::SummaryTally t(&s.set);
    uint64_t ts = ms(1000);
    feed(t, ts, {kRpm});
    for (int i = 0; i < 99; ++i) {
        ts += ms(100);
        feed(t, ts, {kRpm});
    }
    ts += ms(250);
    feed(t, ts, {kRpm});
    const cli::SignalTally& r = *t.tally_of(kRpm);
    EXPECT_EQ(r.values, 101u);
    EXPECT_EQ(r.intervals, 100u);
    EXPECT_EQ(r.min_us, ms(100));
    EXPECT_EQ(r.max_us, ms(250));
    /* 100 ms is in [98,304, 106,496): read at 106,496 us */
    EXPECT_EQ(cli::interval_at(r, 0.5), 106496u);
    EXPECT_EQ(cli::interval_at(r, 0.99), 106496u);
    /* The 100th of 100 is 250 ms, in a bucket whose top is past it */
    EXPECT_EQ(cli::interval_at(r, 1.0), ms(250));
}

/* The window starts at the last first value: what came before it is set
 * aside, and an interval spanning its start is not counted */
TEST(CliSummaryTest, TheWindowStartsAtTheLastFirstValue)
{
    Set s;
    cli::SummaryTally t(&s.set);
    feed(t, ms(0), {kRpm});
    feed(t, ms(100), {kRpm});
    feed(t, ms(200), {kRpm});
    feed(t, ms(250), {kSpeed});
    feed(t, ms(300), {kRpm});
    feed(t, ms(350), {kSpeed});
    feed(t, ms(400), {kRpm});
    feed(t, ms(450), {kSpeed});
    EXPECT_EQ(t.first_us(), ms(0));
    EXPECT_EQ(t.from_us(), ms(250));
    EXPECT_EQ(t.to_us(), ms(450));
    EXPECT_EQ(t.window_values(), 5u);
    /* The frame that brought the first value is in the window */
    EXPECT_EQ(t.window_frames(), 5u);
    const cli::SignalTally& rpm = *t.tally_of(kRpm);
    EXPECT_EQ(rpm.values, 2u);
    EXPECT_EQ(rpm.intervals, 1u);
    EXPECT_EQ(rpm.min_us, ms(100));
    const cli::SignalTally& speed = *t.tally_of(kSpeed);
    EXPECT_EQ(speed.values, 3u);
    EXPECT_EQ(speed.intervals, 2u);
    EXPECT_FALSE(cli::in_window(*t.tally_of(kBoost), t.epoch()));
}

/* A signal that answered before the window and not since is out of it,
 * and its counts make no part of the summary */
TEST(CliSummaryTest, ASignalQuietSinceTheWindowsStartIsLeftOut)
{
    Set s;
    cli::SummaryTally t(&s.set);
    feed(t, ms(0), {kRpm});
    feed(t, ms(100), {kRpm});
    feed(t, ms(200), {kSpeed});
    feed(t, ms(300), {kSpeed});
    EXPECT_FALSE(cli::in_window(*t.tally_of(kRpm), t.epoch()));
    EXPECT_EQ(t.window_values(), 2u);
    const std::vector<ecuio_signal_key_t> keys;
    const std::string line =
        text([&](cli::Line& l) { cli::summary_line(l, &s.set, keys, {}, t); });
    EXPECT_TRUE(has(line, "\"signals\":1,")) << line;
    EXPECT_TRUE(has(line, "\"values\":2,")) << line;
}

/* With picks only they are counted: a signal decoded beside them (the
 * mock's, a passive bus's) neither counts nor moves the window */
TEST(CliSummaryTest, WithPicksOnlyThePicksAreCounted)
{
    Set s;
    cli::SummaryTally t(&s.set, {kRpm});
    feed(t, ms(0), {kRpm, kSpeed});
    feed(t, ms(100), {kRpm, kSpeed});
    feed(t, ms(150), {kBoost});
    feed(t, ms(200), {kRpm});
    EXPECT_EQ(t.tally_of(kSpeed), nullptr);
    EXPECT_EQ(t.from_us(), ms(0));
    EXPECT_EQ(t.window_values(), 3u);
    EXPECT_EQ(t.tally_of(kRpm)->intervals, 2u);
}

/* silent counts the picks that never answered, and is said only with
 * picks; single the signals of one value in the window */
TEST(CliSummaryTest, SilentCountsOnlyThePicksThatNeverAnswered)
{
    Set s;
    cli::SummaryTally t(&s.set);
    feed(t, ms(0), {kRpm});
    feed(t, ms(100), {kRpm, kSpeed});
    feed(t, ms(200), {kRpm});
    const std::vector<ecuio_signal_key_t> keys;
    const std::string picked = text([&](cli::Line& l) {
        cli::summary_line(l, &s.set, keys, {kRpm, kSpeed, kBoost}, t);
    });
    EXPECT_TRUE(has(picked, "\"silent\":1,")) << picked;
    EXPECT_TRUE(has(picked, "\"single\":1,")) << picked;
    EXPECT_TRUE(has(picked, "\"signals\":2,")) << picked;
    /* The single value's signal is out of the interval figures: the
     * median is RPM's alone */
    EXPECT_TRUE(has(picked, "\"p50_ms\":100,")) << picked;
    const std::string whole =
        text([&](cli::Line& l) { cli::summary_line(l, &s.set, keys, {}, t); });
    EXPECT_FALSE(has(whole, "silent")) << whole;
}

/* The summary's figures: the rates over the window's span, the worst
 * signal by its P50, short_window only under half the span */
TEST(CliSummaryTest, TheSummaryNamesTheWorstSignalAndAShortWindow)
{
    Set s;
    cli::SummaryTally t(&s.set);
    for (uint64_t i = 0; i <= 10; ++i) {
        feed(t, ms(100 * i), {kRpm});
        if (i == 6 || i == 10) {
            feed(t, ms(100 * i), {kSpeed}); /* the window: 600 to 1,000 ms */
        }
    }
    std::vector<ecuio_signal_key_t> keys(3);
    std::memset(keys.data(), 0, keys.size() * sizeof keys[0]);
    std::strncpy(keys[1].text, "car.speed", sizeof keys[1].text - 1);
    const std::string line =
        text([&](cli::Line& l) { cli::summary_line(l, &s.set, keys, {}, t); });
    EXPECT_TRUE(has(line, "\"from_s\":0.6,\"span_s\":0.4,")) << line;
    EXPECT_TRUE(has(line, "\"from_us\":600000,\"to_us\":1000000,")) << line;
    EXPECT_TRUE(has(line, "\"short_window\":true")) << line;
    /* 6 values in 0.4 s (Speed's two, RPM's from 700 ms), 2 signals;
     * RPM's 600 ms came before Speed's first */
    EXPECT_TRUE(has(line, "\"signals\":2,")) << line;
    EXPECT_TRUE(has(line, "\"values\":6,\"values_per_s\":15,"
                          "\"sweeps_per_s\":7.5,\"frames_per_s\":15,"))
        << line;
    /* The median of RPM's 100 and Speed's 400 is the lower; each bucket's
     * top (106.496, 425.984) is held to its signal's maximum */
    EXPECT_TRUE(has(line, "\"p50_ms\":100,\"worst_p50_ms\":400,"
                          "\"worst\":{\"signal\":\"Vehicle Speed\","
                          "\"key\":\"car.speed\"},\"worst_p99_ms\":400}"))
        << line;

    cli::SummaryTally half(&s.set);
    feed(half, ms(0), {kRpm});
    feed(half, ms(500), {kSpeed});
    feed(half, ms(1000), {kSpeed, kRpm});
    const std::string even = text(
        [&](cli::Line& l) { cli::summary_line(l, &s.set, keys, {}, half); });
    EXPECT_FALSE(has(even, "short_window")) << even;
}

/* Nothing answered: no window, the rates and times null */
TEST(CliSummaryTest, ALogWithNoValueHasANullSummary)
{
    Set s;
    cli::SummaryTally t(&s.set);
    t.on_frame();
    const std::vector<ecuio_signal_key_t> keys;
    const std::string line = text(
        [&](cli::Line& l) { cli::summary_line(l, &s.set, keys, {kRpm}, t); });
    EXPECT_TRUE(has(line, "\"signals\":0,\"single\":0,\"silent\":1,"
                          "\"values\":0,\"values_per_s\":null,"))
        << line;
    EXPECT_TRUE(has(line, "\"worst\":null")) << line;
    EXPECT_FALSE(has(line, "short_window")) << line;
}

/* An interval line: its figures, and null times for one value; two
 * first values in one frame both start the window */
TEST(CliSummaryTest, AnIntervalLineGivesItsSignalsFigures)
{
    Set s;
    cli::SummaryTally t(&s.set);
    feed(t, ms(0), {kRpm, kSpeed});
    feed(t, ms(20), {kRpm});
    feed(t, ms(50), {kRpm});
    const std::string two = text([&](cli::Line& l) {
        cli::interval_line(l, s.defs[0], "obd2.rpm", 32, *t.tally_of(kRpm));
    });
    EXPECT_EQ(two, "{\"type\":\"interval\",\"signal\":\"RPM\",\"key\":"
                   "\"obd2.rpm\",\"values\":3,\"p50_ms\":20.48,\"p99_ms\":30,"
                   "\"min_ms\":20,\"max_ms\":30}");
    const std::string one = text([&](cli::Line& l) {
        cli::interval_line(l, s.defs[1], nullptr, 0, *t.tally_of(kSpeed));
    });
    EXPECT_EQ(one, "{\"type\":\"interval\",\"signal\":\"Vehicle Speed\","
                   "\"key\":null,\"values\":1,\"p50_ms\":null,"
                   "\"p99_ms\":null,\"min_ms\":null,\"max_ms\":null}");
}

/* A name of the most escapes fits both lines' room */
TEST(CliSummaryTest, TheLinesFitANameOfControlCharacters)
{
    Set s;
    std::memset(s.defs[0].name, 0x01, sizeof s.defs[0].name - 1);
    cli::SummaryTally t(&s.set);
    feed(t, ms(0), {kRpm});
    feed(t, ms(10), {kRpm});
    std::vector<ecuio_signal_key_t> keys(3);
    std::memset(keys.data(), 'k', keys.size() * sizeof keys[0]);
    for (ecuio_signal_key_t& k : keys) {
        k.text[sizeof k.text - 1] = '\0';
    }
    std::vector<char> a(cli::interval_line_bytes());
    cli::Line il(a.data(), a.size());
    cli::interval_line(il, s.defs[0], keys[0].text, sizeof keys[0].text,
                       *t.tally_of(kRpm));
    EXPECT_TRUE(il.finish());
    std::vector<char> b(cli::summary_line_bytes());
    cli::Line sl(b.data(), b.size());
    cli::summary_line(sl, &s.set, keys, {kRpm, kSpeed, kBoost}, t);
    EXPECT_TRUE(sl.finish());
}

/* The worker's calls allocate nothing: the table and tallies are made
 * before the start, so a lookup or a restart takes no memory */
TEST(CliSummaryTest, TheWorkersCallsAllocateNothing)
{
    Set s;
    cli::SummaryTally t(&s.set);
    ecuio_value_t v[3];
    std::memset(v, 0, sizeof v);
    v[0].signal_id = kRpm;
    v[1].signal_id = kSpeed;
    v[2].signal_id = 999; /* not in the set */
    ecuio_value_batch_t b;
    std::memset(&b, 0, sizeof b);
    b.count = 3;
    b.values = v;
    const unsigned long before = new_counter::on_this_thread();
    for (uint64_t i = 0; i < 100; ++i) {
        b.timestamp_us = ms(10 * i);
        b.count = i == 50 ? 3 : 1; /* Speed's first value: a restart */
        t.on_frame();
        t.on_batch(b);
    }
    EXPECT_EQ(new_counter::on_this_thread(), before);
    EXPECT_EQ(t.epoch(), 2u);
}

/* On a session's own worker, as log runs it: the mock's values through
 * the tally cost the worker no allocation past its start-up, so the
 * callbacks can't throw there */
TEST(CliSummaryTest, ARunsValuesCostTheWorkerNoAllocation)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &session), ECUIO_OK)
        << ecuio_last_error_detail();
    cli::SummaryTally tally(ecuio_session_signal_set(session));
    WorkerRun w;
    w.tally = &tally;
    EXPECT_EQ(ecuio_register_frame_callback(session, worker_frame, &w),
              ECUIO_OK);
    EXPECT_EQ(ecuio_register_values_callback(session, worker_values, &w),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (w.frames.load() < WorkerRun::kTo &&
           std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    ASSERT_GE(w.frames.load(), WorkerRun::kTo);
    EXPECT_GT(tally.window_values(), 0u) << "the tally took values";
    EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
        << "operator new on the worker between frames " << WorkerRun::kFrom
        << " and " << WorkerRun::kTo;
}
