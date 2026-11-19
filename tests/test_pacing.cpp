// File/layer: tests/test_pacing.cpp | test_pacing, Qt-free, C++17
// Testing: UNIT: PacingTest (test_pacing)
//
// tests/pacing_bench.h: the request engine on a simulated clock against
// the simulated car, and tests/pacing_timing.h's draw. The figures are the
// model's, uncalibrated: the bounds pin the engine's pacing against the
// model's timing, so a change to the floor or the rotation moves them.

#include "pacing_bench.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

namespace {

// The 2026-11-03 tap's engine ECU at idle (docs/bench-sessions/2026-11-03-socketcan-car.md)
pacing::sim_timing car_idle()
{
    pacing::sim_timing t;
    t.p50_us = 3460;
    t.p99_us = 10930;
    t.fc_us = 4800;
    t.cf_us = 5300;
    return t;
}

uint16_t id_of(const char* name)
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        if (std::strcmp(set->defs[i].name, name) == 0) {
            return set->defs[i].id;
        }
    }
    ADD_FAILURE() << "no obd2 signal named " << name;
    return 0;
}

pacing::bench_result run(const pacing::bench_config& cfg)
{
    pacing::bench b(cfg);
    std::string why;
    EXPECT_TRUE(b.open(&why)) << why;
    return b.run();
}

pacing::bench_config one_pid(pacing::adapter kind)
{
    pacing::bench_config cfg;
    cfg.model.kind = kind;
    if (kind == pacing::adapter::socketcan) {
        cfg.model.deliver_us = 0;
    }
    cfg.timing = car_idle();
    cfg.picks = {id_of("RPM")};
    cfg.seconds = 30;
    return cfg;
}

} // namespace

/* The draw is the simulated car's: AS241's inverse at known points, and
 * each module's generator its own, so one module's draws don't move
 * with another's */
TEST(PacingTest, TheDrawIsTheSimulatedCarsOwn)
{
    EXPECT_DOUBLE_EQ(pacing::inverse_normal(0.5), 0.0);
    EXPECT_NEAR(pacing::inverse_normal(0.75), 0.6744897501960817, 1e-14);
    EXPECT_NEAR(pacing::inverse_normal(0.9), 1.2815515655446004, 1e-14);
    EXPECT_NEAR(pacing::inverse_normal(0.975), 1.959963984540054, 1e-14);
    EXPECT_NEAR(pacing::inverse_normal(0.99), 2.326347874040841, 1e-14);
    EXPECT_NEAR(pacing::inverse_normal(1e-10), -6.361340902404056, 1e-12);
    EXPECT_NEAR(pacing::inverse_normal(1e-300), -37.0471, 1e-3);

    const pacing::sim_timing t = car_idle();
    auto a = pacing::sim_timing_rng(t.seed, 0x7E8);
    auto b = pacing::sim_timing_rng(t.seed, 0x7E8);
    auto other = pacing::sim_timing_rng(t.seed, 0x7E9);
    const int64_t first = pacing::sim_timing_draw(t, a);
    EXPECT_EQ(first, pacing::sim_timing_draw(t, b));
    EXPECT_NE(first, pacing::sim_timing_draw(t, other));

    /* The draws' median and 99th percentile are the timing's */
    std::vector<int64_t> d;
    auto rng = pacing::sim_timing_rng(t.seed, 0x7E8);
    for (int i = 0; i < 20000; ++i) {
        d.push_back(pacing::sim_timing_draw(t, rng));
    }
    std::sort(d.begin(), d.end());
    EXPECT_NEAR(static_cast<double>(d[10000]), 3460.0, 3460.0 * 0.03);
    EXPECT_NEAR(static_cast<double>(d[19800]), 10930.0, 10930.0 * 0.06);

    pacing::sim_timing none;
    auto r = pacing::sim_timing_rng(none.seed, 0x7E8);
    EXPECT_EQ(pacing::sim_timing_draw(none, r), 0);
}

/* A run repeats exactly; another seed moves it */
TEST(PacingTest, ARunRepeatsAndASeedMovesIt)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::j2534);
    cfg.picks.clear(); /* obd2 whole, packed */
    const pacing::bench_result a = run(cfg);
    const pacing::bench_result b = run(cfg);
    ASSERT_GT(a.tally.window_values(), 0u);
    EXPECT_EQ(a.tally.window_values(), b.tally.window_values());
    EXPECT_EQ(a.tally.from_us(), b.tally.from_us());
    EXPECT_EQ(a.tally.to_us(), b.tally.to_us());
    EXPECT_EQ(a.sent_us, b.sent_us);
    cfg.timing.seed = 2;
    const pacing::bench_result c = run(cfg);
    EXPECT_NE(a.sent_us, c.sent_us);
}

/* One PID a request through the VX: a request takes the longer of the
 * floor (5 ms from the write's start) and the write, the bus and the
 * reply, about 6.2 ms at the car's idle on this model, so about 161 a
 * second (a write of no time would let it reach the floor's 190s);
 * ?gap=20 holds it under 50 */
TEST(PacingTest, OnePidARequestRunsAtTheFloorOrTheReply)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::j2534);
    const pacing::bench_result r = run(cfg);
    EXPECT_GT(r.requests_per_s(), 155.0);
    EXPECT_LT(r.requests_per_s(), 170.0);
    EXPECT_EQ(r.timeouts, 0u);

    cfg.params.gap_us = 20000;
    const pacing::bench_result slow = run(cfg);
    EXPECT_GT(slow.requests_per_s(), 40.0);
    EXPECT_LT(slow.requests_per_s(), 50.5);
}

/* SocketCAN has no write to wait for and the engine's 20 ms floor unless
 * ?gap= says otherwise; with ?gap=0 the reply alone sets the pace */
TEST(PacingTest, SocketCanAtNoGapRunsAtTheReplysPace)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::socketcan);
    const pacing::bench_result floor = run(cfg);
    EXPECT_GT(floor.requests_per_s(), 45.0);
    EXPECT_LT(floor.requests_per_s(), 50.5);

    cfg.params.gap_us = 0;
    const pacing::bench_result free = run(cfg);
    /* the request's and reply's airtime and the drawn delay, about 4.5 ms */
    EXPECT_GT(free.requests_per_s(), 180.0);
    EXPECT_LT(free.requests_per_s(), 260.0);
}

/* obd2 whole through the VX: packing beats one PID a request, though by
 * less than the 5 ms floor's absence once made it (a six-PID pack's
 * three frames take about 14 ms against one PID's 5.5) */
TEST(PacingTest, PackingBeatsOnePidARequestOnTheWholeSet)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::j2534);
    cfg.picks.clear();
    const pacing::bench_result packed = run(cfg);
    cfg.params.pack = false;
    const pacing::bench_result single = run(cfg);
    ASSERT_GT(single.values_per_s(), 0.0);
    EXPECT_GT(packed.values_per_s(), 1.5 * single.values_per_s())
        << packed.values_per_s() << " packed, " << single.values_per_s()
        << " one PID a request";
    EXPECT_EQ(packed.timeouts, 0u);
}

/* A support set rewrites the simulated car's bitmaps: the walk ends with
 * only those PIDs in the rotation, so only their signals answer */
TEST(PacingTest, TheSupportSetLimitsWhatTheWalkKeeps)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::j2534);
    cfg.picks.clear();
    cfg.supported = {0x0C, 0x0D, 0x11}; /* RPM, speed, throttle: all fast */
    const pacing::bench_result r = run(cfg);
    const cli::SummaryFigures f = cli::summary_figures(r.tally, {});
    EXPECT_EQ(f.signals, 3u);
    for (const char* name : {"RPM", "Vehicle Speed", "Throttle Position"}) {
        const cli::SignalTally* t = r.tally.tally_of(id_of(name));
        ASSERT_NE(t, nullptr) << name;
        EXPECT_TRUE(cli::in_window(*t, r.tally.epoch())) << name;
    }
    /* one three-PID pack a request, a Single Frame: three values each */
    EXPECT_NEAR(r.values_per_s() / r.requests_per_s(), 3.0, 0.1);

    std::vector<uint8_t> bitmap = {0x41, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    pacing::filter_bitmap(bitmap.data(), 6, {0x01, 0x21});
    EXPECT_EQ(bitmap,
              (std::vector<uint8_t>{0x41, 0x00, 0x80, 0x00, 0x00, 0x01}));
}

/* The draw's fixed vectors, pinned on both sides (SimTimingTest pins the
 * same five in the simulated car): seed 1, the module on 0x7E8, P50
 * 3,460 us and P99 10,930 us */
TEST(PacingTest, TheDrawsFixedVectors)
{
    const pacing::sim_timing t = car_idle();
    auto rng = pacing::sim_timing_rng(1, 0x7E8);
    std::vector<int64_t> got;
    for (int i = 0; i < 5; ++i) {
        got.push_back(pacing::sim_timing_draw(t, rng));
    }
    EXPECT_EQ(got, (std::vector<int64_t>{6352, 6622, 2326, 1854, 2248}));
}
