// File/layer: tests/test_pacing.cpp | test_pacing, Qt-free, C++17
// Testing: UNIT: PacingTest (test_pacing)
//
// tests/pacing_bench.h: the request engine on a simulated clock against
// the simulated car, and the draw it takes from core/src/sim_timing.h.
// The figures are the model's, uncalibrated: the bounds pin the engine's
// pacing against the model's timing, so a change to the floor or the
// rotation moves them.

#include "pacing_bench.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
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

    /* A P99 far above its P50 reaches the 60 s cap (z past about 2.6,
     * about 1 draw in 200) and never passes it, as the simulated car's */
    pacing::sim_timing wide;
    wide.p50_us = 1;
    wide.p99_us = 10000000;
    auto w = pacing::sim_timing_rng(wide.seed, 0x7E8);
    int64_t most = 0;
    for (int i = 0; i < 20000; ++i) {
        most = std::max(most, pacing::sim_timing_draw(wide, w));
    }
    EXPECT_EQ(most, 60000000);
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

pacing::bench_config one_pid_elm(const pacing::adapter_model& m)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::elm327);
    cfg.model = m;
    return cfg;
}

/* An ELM327 asks one command at a time and the engine's 20 ms floor
 * counts from the command's write: over USB the exchange (about 8 ms)
 * is under the floor, so ?gap=0 more than doubles the pace; over
 * Bluetooth the link's round trip makes the exchange longer than the
 * floor, so ?gap=0 gains little (the line sets the MX+'s pace) */
TEST(PacingTest, AnElm327RunsAtTheFloorOrItsExchange)
{
    pacing::bench_config usb = one_pid_elm(pacing::vx_elm_mode());
    const pacing::bench_result floor = run(usb);
    EXPECT_GT(floor.requests_per_s(), 45.0);
    EXPECT_LT(floor.requests_per_s(), 50.5);
    usb.params.gap_us = 0;
    const pacing::bench_result free = run(usb);
    EXPECT_GT(free.requests_per_s(), 2.0 * floor.requests_per_s());
    EXPECT_EQ(free.timeouts, 0u);
    /* A tenth of the character time (a raised UART) takes about 2 ms of
     * the command and the reply's line off the exchange */
    usb.model.char_us /= 10;
    const pacing::bench_result fast_line = run(usb);
    EXPECT_GT(fast_line.requests_per_s(), 1.2 * free.requests_per_s());

    pacing::bench_config bt = one_pid_elm(pacing::elm327_bluetooth());
    const pacing::bench_result line = run(bt);
    bt.params.gap_us = 0;
    const pacing::bench_result bt_free = run(bt);
    EXPECT_LT(line.requests_per_s(), 50.5);
    EXPECT_NEAR(bt_free.requests_per_s(), line.requests_per_s(),
                0.1 * line.requests_per_s());
    EXPECT_EQ(line.headers, 1u); /* one module: AT SH once */
}

/* With the count digit the prompt follows the reply's last line, so the
 * adapter's adaptive wait never runs: a longer one moves nothing */
TEST(PacingTest, TheCountDigitSparesTheAdaptiveWait)
{
    pacing::bench_config cfg = one_pid_elm(pacing::vx_elm_mode());
    cfg.params.gap_us = 0;
    cfg.params.identify = false; /* Service 09 and 03 go with no digit */
    cfg.params.diagnose = false;
    const pacing::bench_result a = run(cfg);
    cfg.model.wait_us *= 4;
    const pacing::bench_result b = run(cfg);
    ASSERT_GT(a.sent_us.size(), 1000u);
    EXPECT_EQ(a.sent_us, b.sent_us);

    /* A pack's digit is its frames: the same through six short PIDs'
     * two-frame pack (the whole set has PIDs asked alone with no digit) */
    cfg.picks.clear();
    for (const char* name :
         {"RPM", "Vehicle Speed", "Throttle Position", "Engine Load",
          "Intake Air Temp", "Intake Manifold Pressure"}) {
        cfg.picks.push_back(id_of(name));
    }
    cfg.model.late_prompt_us = 0;
    const pacing::bench_result c = run(cfg);
    cfg.model.wait_us /= 4;
    const pacing::bench_result d = run(cfg);
    EXPECT_EQ(c.sent_us, d.sent_us);
    EXPECT_EQ(c.cuts, 0u);
}

/* The VX in ELM mode gives a multi-frame reply's prompt one AT ST after
 * it: obd2's six-PID packs (three frames) run near 8 a second, where an
 * adapter keeping the digit runs them at over four times that */
TEST(PacingTest, TheVxInElmModeHoldsAMultiFramePackToItsLatePrompt)
{
    pacing::bench_config cfg = one_pid_elm(pacing::vx_elm_mode());
    cfg.picks.clear();
    const pacing::bench_result late = run(cfg);
    EXPECT_GT(late.requests_per_s(), 6.0);
    EXPECT_LT(late.requests_per_s(), 10.0);
    cfg.model.late_prompt_us = 0;
    const pacing::bench_result kept = run(cfg);
    EXPECT_GT(kept.requests_per_s(), 4.0 * late.requests_per_s());
}

/* ?packframes=1 on the VX in ELM mode, the test car's PIDs: each pack a
 * Single Frame, so the count digit's prompt ends it and the late prompt
 * never waits. A pack of about 2.6 values every 21 ms (the floor) against
 * about 6.9 every 118 ms: about 1.9x the values, 5.7x the requests */
TEST(PacingTest, OneFramePacksSpareTheVxInElmModeItsLatePrompt)
{
    pacing::bench_config cfg = one_pid_elm(pacing::vx_elm_mode());
    cfg.picks.clear();
    cfg.supported = pacing::test_car_pids();
    const pacing::bench_result whole = run(cfg);
    cfg.params.pack_frames = 1;
    const pacing::bench_result one = run(cfg);
    ASSERT_GT(whole.values_per_s(), 0.0);
    EXPECT_GT(one.values_per_s(), 1.7 * whole.values_per_s())
        << one.values_per_s() << " one-frame packs, " << whole.values_per_s()
        << " unlimited";
    EXPECT_GT(one.requests_per_s(), 4.0 * whole.requests_per_s());
    EXPECT_EQ(one.cuts, 0u);
    EXPECT_EQ(one.timeouts, 0u);
}

/* ?packframes= on J2534, the test car's PIDs: a two-frame pack (about 5.2
 * values in 11.4 ms) loses nothing to the six-PID pack's three frames
 * (about 6.9 in 16.6 ms); a one-frame pack loses about a tenth, since the
 * 5 ms floor and the write hold it to about 6.2 ms for about 2.3 values */
TEST(PacingTest, TwoFramePacksLoseNothingOnJ2534)
{
    pacing::bench_config cfg = one_pid(pacing::adapter::j2534);
    cfg.picks.clear();
    cfg.supported = pacing::test_car_pids();
    const pacing::bench_result whole = run(cfg);
    cfg.params.pack_frames = 2;
    const pacing::bench_result two = run(cfg);
    cfg.params.pack_frames = 1;
    const pacing::bench_result one = run(cfg);
    ASSERT_GT(whole.values_per_s(), 0.0);
    EXPECT_GE(two.values_per_s(), whole.values_per_s())
        << two.values_per_s() << " two-frame packs, " << whole.values_per_s()
        << " unlimited";
    EXPECT_GT(one.values_per_s(), 0.8 * whole.values_per_s());
    EXPECT_LT(one.values_per_s(), 0.95 * whole.values_per_s());
    EXPECT_EQ(two.timeouts + one.timeouts, 0u);
}

/* The simulated car's second module over an ELM327 that keeps the count
 * digit (the VX in ELM mode ignores it after a multi-frame reply, so
 * never cuts one): D102's reply spans frames, so the digit cuts it once;
 * the DID is taken back, asked again with no digit once the module is
 * done sending, and answers from then on, beside D101 in one line. AT SH
 * goes once: nothing is asked of the engine ECU */
TEST(PacingTest, AnElm327TakesBackALongDid)
{
    pacing::bench_config cfg = one_pid_elm(pacing::vx_elm_mode());
    cfg.model.late_prompt_us = 0;
    cfg.profile = "uds-generic&did=7E9:D101:u8,7E9:D102:text";
    cfg.sim_keys = "modules=2";
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    cfg.picks.clear();
    const pacing::bench_result r = run(cfg);
    EXPECT_EQ(r.cuts, 1u);
    EXPECT_EQ(r.headers, 1u);
    EXPECT_EQ(r.timeouts, 0u);
    const cli::SummaryFigures f = cli::summary_figures(r.tally, {});
    EXPECT_EQ(f.signals, 2u);
    /* D101 at the 20 ms floor; D102, with no digit, its three frames
     * and the adaptive wait, about 37 ms: two requests in about 58 ms */
    EXPECT_GT(r.requests_per_s(), 31.0);
    EXPECT_LT(r.requests_per_s(), 38.0);
}

/* A module that never answers: each request ends in NO DATA after AT ST's
 * 100 ms, and the engine's wait times it out */
TEST(PacingTest, AnElm327WaitsOutASilentModuleToItsNoData)
{
    pacing::bench_config cfg = one_pid_elm(pacing::vx_elm_mode());
    cfg.profile = "uds-generic&did=7E9:D101:u8"; /* no modules=2 */
    cfg.sim_keys = "profile=obd2";               /* the engine ECU alone */
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    cfg.picks.clear();
    const pacing::bench_result r = run(cfg);
    const double asked = static_cast<double>(r.sent_us.size()) / cfg.seconds;
    EXPECT_GT(asked, 9.0) << asked; /* 100 ms and the command, about 103 */
    EXPECT_LT(asked, 10.0) << asked;
    EXPECT_GT(r.timeouts, 0u);
    EXPECT_EQ(r.tally.window_values(), 0u);
}

namespace {

ecuio_signal_def_t did_word(uint16_t id, uint32_t rx, uint16_t did,
                            const char* name)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.bit_length = 16;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "%s", name);
    return d;
}

// Two modules' DIDs, each a word in one Single Frame, registered for the
// session and the simulated car alike (which answers each module from
// the set); the registry keeps the pointers, so static storage
struct TwoModules {
    TwoModules()
    {
        static const ecuio_signal_def_t defs[] = {
            did_word(1, 0x7E8, 0x1E1C, "Engine A"),
            did_word(2, 0x7E8, 0x1E1D, "Engine B"),
            did_word(3, 0x7EA, 0xD200, "Module A"),
            did_word(4, 0x7EA, 0xD201, "Module B"),
        };
        static const ecuio_channel_decl_t channels[] = {{0x7E8, 0x7E0, 0, 0},
                                                        {0x7EA, 0x7E2, 0, 0}};
        static ecuio_signal_set_t set{};
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 4;
        set.defs = defs;
        set.channel_count = 2;
        set.channels = channels;
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(kName, &set, &why), ECUIO_OK)
            << "code " << why.code << " at " << why.index;
    }
    ~TwoModules() { EXPECT_EQ(ecuio_profile_unregister(kName), ECUIO_OK); }
    TwoModules(const TwoModules&) = delete;
    TwoModules& operator=(const TwoModules&) = delete;
    static constexpr const char* kName = "pacing-two";
};

// TwoModules with the engine ECU's second DID a 16-byte text, whose reply
// takes three frames
struct TwoModulesOneLong {
    TwoModulesOneLong()
    {
        static ecuio_signal_def_t defs[] = {
            did_word(1, 0x7E8, 0x1E1C, "Engine A"),
            did_word(2, 0x7E8, 0x1E1D, "Engine name"),
            did_word(3, 0x7EA, 0xD200, "Module A"),
            did_word(4, 0x7EA, 0xD201, "Module B"),
        };
        defs[1].kind = ECUIO_VALUE_TEXT;
        defs[1].bit_length = 0;
        defs[1].byte_length = 16;
        static const ecuio_channel_decl_t channels[] = {{0x7E8, 0x7E0, 0, 0},
                                                        {0x7EA, 0x7E2, 0, 0}};
        static ecuio_signal_set_t set{};
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 4;
        set.defs = defs;
        set.channel_count = 2;
        set.channels = channels;
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(kName, &set, &why), ECUIO_OK)
            << "code " << why.code << " at " << why.index;
    }
    ~TwoModulesOneLong()
    {
        EXPECT_EQ(ecuio_profile_unregister(kName), ECUIO_OK);
    }
    TwoModulesOneLong(const TwoModulesOneLong&) = delete;
    TwoModulesOneLong& operator=(const TwoModulesOneLong&) = delete;
    static constexpr const char* kName = "pacing-two-long";
};

} // namespace

/* The J2534 model's pass is the worker's (j2534_hold_pass), the hold on by
 * default: while the engine ECU's three-frame reply is out the other
 * module isn't asked, so two modules ask fewer requests a second than
 * with ?hold=0, and nothing times out either way */
TEST(PacingTest, J2534TakesTurnsAroundALongReplyByDefault)
{
    const TwoModulesOneLong profile;
    pacing::bench_config cfg = one_pid(pacing::adapter::j2534);
    cfg.profile = TwoModulesOneLong::kName;
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    cfg.picks.clear();
    const pacing::bench_result held = run(cfg);
    cfg.model.hold = false;
    const pacing::bench_result plain = run(cfg);
    const cli::SummaryFigures f = cli::summary_figures(held.tally, {});
    EXPECT_EQ(f.signals, 4u);
    EXPECT_EQ(held.timeouts + plain.timeouts, 0u);
    ASSERT_GT(plain.requests_per_s(), 0.0);
    EXPECT_LT(held.requests_per_s(), 0.75 * plain.requests_per_s())
        << held.requests_per_s() << " held, " << plain.requests_per_s()
        << " with ?hold=0";
}

/* Two modules over an ELM327: a module keeps the turn until it has asked
 * each of its DIDs once (request_turns.h), so AT SH goes once for each
 * turn of two requests, not before every request */
TEST(PacingTest, AnElm327SwitchesHeaderOnceATurn)
{
    const TwoModules profile;
    pacing::bench_config cfg = one_pid_elm(pacing::vx_elm_mode());
    cfg.profile = TwoModules::kName;
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    cfg.picks.clear();
    const pacing::bench_result r = run(cfg);
    const cli::SummaryFigures f = cli::summary_figures(r.tally, {});
    EXPECT_EQ(f.signals, 4u);
    EXPECT_EQ(r.timeouts, 0u);
    const double asked = static_cast<double>(r.sent_us.size());
    ASSERT_GT(asked, 1000.0);
    EXPECT_NEAR(static_cast<double>(r.headers) / asked, 0.5, 0.05)
        << r.headers << " AT SH for " << asked << " requests";
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

    /* another module's generator, and another seed */
    const auto five = [&t](uint64_t seed, uint32_t reply_id) {
        auto g = pacing::sim_timing_rng(seed, reply_id);
        std::vector<int64_t> v;
        for (int i = 0; i < 5; ++i) {
            v.push_back(pacing::sim_timing_draw(t, g));
        }
        return v;
    };
    EXPECT_EQ(five(1, 0x7E9),
              (std::vector<int64_t>{7344, 1638, 2645, 4478, 2293}));
    EXPECT_EQ(five(7, 0x7E8),
              (std::vector<int64_t>{2689, 6248, 4034, 5173, 2489}));
}
