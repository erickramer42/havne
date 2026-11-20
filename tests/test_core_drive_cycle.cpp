// File/layer: tests/test_core_drive_cycle.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

// ---------------------------------------------------------------------------
// Mock drive cycle: stepped deterministically, no worker, no wall clock
// ---------------------------------------------------------------------------

namespace {


constexpr int kPhaseCount = static_cast<int>(Phase::Cooldown) + 1;

struct PhaseStats {
    uint32_t ticks = 0;
    float entry_rpm = 0.0f;    /* after the phase's first tick */
    float entry_kmh = 0.0f;
    float min_rpm = 1e9f;
    float peak_rpm = 0.0f;
    float peak_kmh = 0.0f;
    float peak_coolant = -100.0f;
    float peak_iat = -100.0f;
    float peak_oil = -100.0f;
    int max_gear = 0;
};

struct LoopRun {
    PhaseStats phase[kPhaseCount];
    std::vector<Phase> order;  /* phases in visit order */
    bool closed = false;       /* looped back to KeyOn */
    DriveCycle end;
};

const PhaseStats& stats(const LoopRun& r, Phase p)
{
    return r.phase[static_cast<int>(p)];
}

/* Step one loop, KeyOn through Cooldown, stopping at the tick that
 * returns to KeyOn. Bounded at 10 simulated minutes. */
LoopRun run_one_loop()
{
    LoopRun r;
    DriveCycle dc;
    PhaseStats* s = nullptr;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f); ++i) {
        dc.step();
        if (dc.loops > 0) {
            r.closed = true;
            break;
        }
        if (r.order.empty() || r.order.back() != dc.phase) {
            r.order.push_back(dc.phase);
            s = &r.phase[static_cast<int>(dc.phase)];
            s->entry_rpm = dc.rpm;
            s->entry_kmh = dc.speed_kmh;
        }
        ++s->ticks;
        s->min_rpm = std::fmin(s->min_rpm, dc.rpm);
        s->peak_rpm = std::fmax(s->peak_rpm, dc.rpm);
        s->peak_kmh = std::fmax(s->peak_kmh, dc.speed_kmh);
        s->peak_coolant = std::fmax(s->peak_coolant, dc.coolant_c);
        s->peak_iat = std::fmax(s->peak_iat, dc.iat_c);
        s->peak_oil = std::fmax(s->peak_oil, dc.oil_c);
        if (dc.gear > s->max_gear) {
            s->max_gear = dc.gear;
        }
    }
    r.end = dc;
    return r;
}

} // namespace

TEST(CoreTest, MockDriveCycleVisitsEveryPhaseInOrder)
{
    const LoopRun r = run_one_loop();
    ASSERT_TRUE(r.closed) << "the cycle never looped back to KeyOn";
    ASSERT_EQ(r.order.size(), static_cast<size_t>(kPhaseCount));
    for (int i = 0; i < kPhaseCount; ++i) {
        EXPECT_EQ(static_cast<int>(r.order[static_cast<size_t>(i)]), i)
            << "visit " << i;
    }
    EXPECT_EQ(r.end.phase, Phase::KeyOn);
}

TEST(CoreTest, MockDriveCycleSecondGearPullHoldsTheLimiter)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& pull = stats(r, Phase::Pull2nd);
    const PhaseStats& lim = stats(r, Phase::Limiter2nd);

    /* From low speed and rpm, in 2nd, to the limiter */
    EXPECT_EQ(pull.max_gear, 2);
    EXPECT_LT(pull.entry_rpm, 2000.0f);
    EXPECT_LT(pull.entry_kmh, 35.0f);
    EXPECT_LE(pull.peak_rpm, ecuio_mock::kRevLimitRpm);

    /* Exactly 5 s on the limiter: past it on first contact, before the
     * first cut lands, then sawing just under it */
    EXPECT_EQ(lim.ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(lim.min_rpm, ecuio_mock::kRevLimitRpm - 200.0f);
    EXPECT_GT(lim.peak_rpm, ecuio_mock::kRevLimitRpm);
    EXPECT_LE(lim.peak_rpm, ecuio_mock::kRevLimitRpm + 100.0f);
    EXPECT_NEAR(lim.peak_kmh, 113.0f, 5.0f);

    /* Past either end of its table the torque curve holds that end */
    EXPECT_EQ(ecuio_mock::torque_nm(ecuio_mock::kRevLimitRpm + 500.0f),
              ecuio_mock::torque_nm(ecuio_mock::kRevLimitRpm));
    EXPECT_EQ(ecuio_mock::torque_nm(-100.0f), 0.0f);
}

TEST(CoreTest, MockDriveCycleThirdGearPullIsLongerFasterAndHotter)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& pull2 = stats(r, Phase::Pull2nd);
    const PhaseStats& lim2 = stats(r, Phase::Limiter2nd);
    const PhaseStats& pull3 = stats(r, Phase::Pull3rd);
    const PhaseStats& lim3 = stats(r, Phase::Limiter3rd);

    EXPECT_EQ(pull3.max_gear, 3);
    EXPECT_LE(pull3.peak_rpm, ecuio_mock::kRevLimitRpm);
    EXPECT_LT(pull3.entry_rpm, 2200.0f);
    EXPECT_GT(pull3.entry_kmh, pull2.entry_kmh + 10.0f);
    EXPECT_GT(pull3.ticks, pull2.ticks * 3 / 2) << "3rd should pull longer";

    EXPECT_EQ(lim3.ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(lim3.peak_kmh, lim2.peak_kmh + 40.0f);

    const float coolant2 = std::fmax(pull2.peak_coolant, lim2.peak_coolant);
    const float coolant3 = std::fmax(pull3.peak_coolant, lim3.peak_coolant);
    const float oil2 = std::fmax(pull2.peak_oil, lim2.peak_oil);
    const float oil3 = std::fmax(pull3.peak_oil, lim3.peak_oil);
    const float iat3 = std::fmax(pull3.peak_iat, lim3.peak_iat);
    /* The longer pull puts more heat into the cold engine. The intake
     * air warms only by the charge's heat the intercooler leaves, the
     * sensor lagging it, and ram air at speed keeps the bay's out */
    EXPECT_GT(coolant3, coolant2 + 4.0f);
    EXPECT_GT(oil3, oil2 + 4.0f);
    EXPECT_GT(iat3, ecuio_mock::kAmbientC + 5.0f) << "no charge heat";
    EXPECT_LE(iat3, ecuio_mock::kAmbientC +
                        ecuio_mock::charge_rise_c(ecuio_mock::kBoostMaxKpa,
                                                  ecuio_mock::kBaroKpa) +
                        5.0f);
}

TEST(CoreTest, MockDriveCycleDragRunCoversAQuarterMile)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& launch = stats(r, Phase::Launch);
    const PhaseStats& run = stats(r, Phase::DragRun);

    /* Launch control from a standstill */
    EXPECT_EQ(launch.peak_kmh, 0.0f);
    EXPECT_LE(launch.peak_rpm, ecuio_mock::kLaunchRpm);
    EXPECT_GE(launch.peak_rpm, ecuio_mock::kLaunchRpm - 150.0f);

    /* Through the gears, shifting before the limiter */
    EXPECT_EQ(run.max_gear, ecuio_mock::kTopGear);
    EXPECT_LT(run.peak_rpm, ecuio_mock::kRevLimitRpm);

    /* The run ends on the tick that crosses the line */
    EXPECT_GE(r.end.distance_m, ecuio_mock::kQuarterMileM);
    EXPECT_LT(r.end.distance_m, ecuio_mock::kQuarterMileM + 5.0f);
    /* Traction-limited off the line on street tires, so the boost shows
     * more in the trap speed than in the time; under 4th gear's limiter
     * speed, which the run never reaches */
    EXPECT_GE(r.end.drag_et_s, 11.5f);
    EXPECT_LE(r.end.drag_et_s, 13.5f);
    EXPECT_GE(r.end.drag_trap_kmh, 190.0f);
    EXPECT_LE(r.end.drag_trap_kmh,
              ecuio_mock::kRevLimitRpm /
                  ecuio_mock::rpm_per_mps(ecuio_mock::kTopGear) * 3.6f);
}

TEST(CoreTest, MockDriveCycleLaterLoopsRunAtOperatingTemperature)
{
    /* Only the first loop starts cold; each later one picks up where the
     * last ended. The first loop's runs stay on a cold engine. From the
     * third the coolant holds the thermostat's range, and by the last
     * loop here it climbs past 210 F at WOT until the fan cuts in, with
     * the oil built up to 226-243 F. */
    constexpr uint32_t kLoops = 6;
    struct Range {
        float lo = 1e9f, hi = -1e9f;
        void add(float v)
        {
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
    };
    Range coolant[kLoops];
    Range oil[kLoops];
    bool restarted_cold = false;

    /* The cold first loop */
    float first_pull2_coolant = -1e9f;
    float first_end_coolant = 0.0f, first_end_oil = 0.0f;

    /* The hot last loop: coolant at cruise and at WOT (the pulls, their
     * limiters and the quarter mile), the fan, and the oil across the
     * quarter mile and the stop after it */
    Range hot_cruise_coolant, hot_wot_coolant, hot_drag_oil;
    float hot_drag_oil_entry = 0.0f;
    bool hot_fan = false;
    float hot_end_coolant = 0.0f, hot_end_oil = 0.0f;

    DriveCycle dc;
    while (dc.loops < kLoops && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        if (dc.loops >= kLoops) {
            break;
        }
        const uint32_t i = dc.loops;
        coolant[i].add(dc.coolant_c);
        oil[i].add(dc.oil_c);
        restarted_cold = restarted_cold || (i > 0 && dc.phase == Phase::WarmUp);
        if (i == 0) {
            if (dc.phase == Phase::Pull2nd) {
                first_pull2_coolant =
                    std::fmax(first_pull2_coolant, dc.coolant_c);
            }
            first_end_coolant = dc.coolant_c;
            first_end_oil = dc.oil_c;
        } else if (i == kLoops - 1) {
            const Phase p = dc.phase;
            if (p == Phase::Cruise4th) {
                hot_cruise_coolant.add(dc.coolant_c);
            }
            if (p == Phase::Pull2nd || p == Phase::Limiter2nd ||
                p == Phase::Pull3rd || p == Phase::Limiter3rd ||
                p == Phase::DragRun) {
                hot_wot_coolant.add(dc.coolant_c);
            }
            if (p == Phase::DragRun || p == Phase::Shutdown) {
                if (hot_drag_oil.hi < -1e8f) {
                    hot_drag_oil_entry = dc.oil_c;
                }
                hot_drag_oil.add(dc.oil_c);
            }
            hot_fan = hot_fan || dc.fan_on;
            hot_end_coolant = dc.coolant_c;
            hot_end_oil = dc.oil_c;
        }
    }
    ASSERT_EQ(dc.loops, kLoops) << "six loops never completed";
    EXPECT_FALSE(restarted_cold) << "a later loop ran the cold start";

    /* Cold first loop: the thermostat never opens, and the oil trails
     * the coolant as the sump's mass warms */
    EXPECT_LT(first_pull2_coolant, 40.0f);
    EXPECT_LT(coolant[0].hi, ecuio_mock::kThermostatOpenC);
    EXPECT_LT(first_end_oil, first_end_coolant);

    /* From the third loop on, the coolant never falls below the
     * thermostat's range */
    for (uint32_t i = 2; i < kLoops; ++i) {
        EXPECT_GE(coolant[i].lo, 85.0f) << "loop " << i + 1;
    }

    /* Hot: the thermostat holds the cruise, the radiator can't keep up
     * at WOT, and the fan cuts in */
    EXPECT_GE(hot_cruise_coolant.lo, 85.0f);
    EXPECT_LE(hot_cruise_coolant.hi, 97.0f);
    EXPECT_GE(hot_wot_coolant.hi, 100.0f) << "the radiator kept up at WOT";
    EXPECT_LE(hot_wot_coolant.hi, 105.0f);
    EXPECT_TRUE(hot_fan) << "the fan never cut in";
    EXPECT_GE(oil[kLoops - 1].lo, 100.0f);
    EXPECT_GE(oil[kLoops - 1].hi, 112.0f);
    EXPECT_LE(oil[kLoops - 1].hi, 125.0f);
    /* The sump's mass: a quarter mile moves the oil several degrees, not
     * the coolant's swing */
    EXPECT_GE(hot_drag_oil.hi - hot_drag_oil_entry, 2.0f);
    EXPECT_LE(hot_drag_oil.hi - hot_drag_oil_entry, 9.0f);
    EXPECT_GT(hot_end_oil, hot_end_coolant)
        << "the oil cooled below the coolant";
}

TEST(CoreTest, MockDriveCycleEveryTickRoundTripsThroughTheMockSet)
{
    /* Three loops: the cold first one, the one that warms through the
     * thermostat, and a hot one. Every tick goes through the real
     * encoder and decoder. A value the wire can't carry would clamp in
     * the encoder and show up as a large error. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);

    /* Per-signal accumulators, indexed like set->defs. */
    constexpr uint32_t kMaxDefs = 16;
    ASSERT_LE(set->count, kMaxDefs);
    double worst_err[kMaxDefs] = {};
    double lo[kMaxDefs];
    double hi[kMaxDefs];
    for (uint32_t i = 0; i < kMaxDefs; ++i) {
        lo[i] = 1e9;
        hi[i] = -1e9;
    }

    DriveCycle dc;
    uint32_t steps = 0;
    double boost_lo = 1e9, boost_hi = -1e9;
    while (dc.loops < 3 && steps < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ++steps;
        uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN];
        ecuio_mock::encode_payload(dc, payload);

        double map = 0.0, baro = 0.0;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
                continue; /* boost: from the two below */
            }
            double val = 0.0;
            ASSERT_EQ(ecuio_decode_signal(&def, payload, ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &val), 0)
                << def.name << " at tick " << steps;
            double phys = 0.0;
            switch (def.id) {
            case ECUIO_MOCK_SIG_RPM_ID:       phys = dc.rpm; break;
            case ECUIO_MOCK_SIG_COOLANT_ID:   phys = dc.coolant_c; break;
            case ECUIO_MOCK_SIG_SPEED_ID:     phys = dc.speed_kmh; break;
            case ECUIO_MOCK_SIG_THROTTLE_ID: phys = dc.throttle_pct; break;
            case ECUIO_MOCK_SIG_IAT_ID:       phys = dc.iat_c; break;
            case ECUIO_MOCK_SIG_LOAD_ID:      phys = dc.load_pct; break;
            case ECUIO_MOCK_SIG_MAF_ID:       phys = dc.maf_gs; break;
            case ECUIO_MOCK_SIG_OIL_ID:       phys = dc.oil_c; break;
            case ECUIO_MOCK_SIG_LAMBDA_ID:    phys = dc.lambda; break;
            case ECUIO_MOCK_SIG_MAP_ID:
                phys = dc.map_kpa;
                map = val;
                break;
            case ECUIO_MOCK_SIG_BARO_ID:
                phys = dc.baro_kpa;
                baro = val;
                break;
            case ECUIO_MOCK_SIG_BATTERY_V_ID:
                phys = dc.battery_v;
                break;
            default: FAIL() << "unknown signal id in mock set: " << def.id;
            }
            worst_err[i] = std::fmax(worst_err[i], std::fabs(val - phys));
            lo[i] = std::fmin(lo[i], val);
            hi[i] = std::fmax(hi[i], val);
        }
        /* Boost as the wire's two whole kPa give it */
        boost_lo = std::fmin(boost_lo, map - baro);
        boost_hi = std::fmax(boost_hi, map - baro);
    }
    ASSERT_EQ(dc.loops, 3u) << "three loops never completed";
    EXPECT_GE(boost_lo, kMockBoostMin);
    EXPECT_LE(boost_hi, kMockBoostMax);

    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
            continue;
        }
        /* Half an LSB of each signal's scale, plus float slack */
        const double tol = 0.5 * std::fabs(def.scale) + 0.01;
        EXPECT_LE(worst_err[i], tol) << def.name << " lost precision or clamped";
        /* Every value the cycle sends inside the def's range, which a
         * gauge's default scale starts from (ADR-0057) */
        double range_lo = 0.0;
        double range_hi = 0.0;
        ASSERT_EQ(ecuio_def_range(&def, &range_lo, &range_hi), ECUIO_DECODE_OK)
            << def.name;
        EXPECT_GE(lo[i], range_lo) << def.name;
        EXPECT_LE(hi[i], range_hi) << def.name;

        switch (def.id) {
        case ECUIO_MOCK_SIG_RPM_ID:
            EXPECT_GE(lo[i], kMockRpmMin);     EXPECT_LE(hi[i], kMockRpmMax); break;
        case ECUIO_MOCK_SIG_COOLANT_ID:
            EXPECT_GE(lo[i], kMockCoolantMin); EXPECT_LE(hi[i], kMockCoolantMax); break;
        case ECUIO_MOCK_SIG_SPEED_ID:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], kMockSpeedMax); break;
        case ECUIO_MOCK_SIG_IAT_ID:
            EXPECT_GE(lo[i], kMockIatMin);     EXPECT_LE(hi[i], kMockIatMax); break;
        case ECUIO_MOCK_SIG_MAF_ID:
            EXPECT_GE(lo[i], 0.0f);    EXPECT_LE(hi[i], kMockMafMax); break;
        case ECUIO_MOCK_SIG_OIL_ID:
            EXPECT_GE(lo[i], kMockOilMin); EXPECT_LE(hi[i], kMockOilMax); break;
        case ECUIO_MOCK_SIG_LAMBDA_ID:
            EXPECT_GE(lo[i], kMockLambdaMin); EXPECT_LE(hi[i], kMockLambdaMax); break;
        case ECUIO_MOCK_SIG_MAP_ID:
            EXPECT_GE(lo[i], kMockMapMin);
            EXPECT_LE(hi[i], kMockMapMax);
            break;
        case ECUIO_MOCK_SIG_BARO_ID:
            EXPECT_GE(lo[i], kMockBaroMin);
            EXPECT_LE(hi[i], kMockBaroMax);
            break;
        case ECUIO_MOCK_SIG_BATTERY_V_ID:
            EXPECT_GE(lo[i], kMockBatteryMin);
            EXPECT_LE(hi[i], kMockBatteryMax);
            break;
        default:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], 100.0f); break;
        }
    }
}

namespace {

/* Steps a fresh cycle until `phase` starts (in the first loop), then
 * calls `visit` on each of its ticks. */
template <typename F>
void for_each_tick_of(Phase phase, F visit)
{
    DriveCycle dc;
    bool in = false;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f) && dc.loops == 0; ++i) {
        dc.step();
        if (dc.phase == phase) {
            in = true;
            visit(dc);
        } else if (in) {
            return;
        }
    }
}

} // namespace

TEST(CoreTest, MockBatteryDipsUnderTheStarterAndChargesOnceRunning)
{
    /* Key on, the battery alone near 12.4 V; the starter pulls it under
     * 10 V and it settles under 10.5 V; the alternator then holds it
     * between 13.7 and 14.8 V on every running tick from the warm-up to
     * the end of the first loop */
    float key_on_lo = 1e9f, key_on_hi = -1e9f;
    for_each_tick_of(Phase::KeyOn, [&](const DriveCycle& dc) {
        key_on_lo = std::fmin(key_on_lo, dc.battery_v);
        key_on_hi = std::fmax(key_on_hi, dc.battery_v);
    });
    EXPECT_GE(key_on_lo, 12.3f);
    EXPECT_LE(key_on_hi, 12.6f);

    float crank_lo = 1e9f, crank_last = 0.0f;
    for_each_tick_of(Phase::Start, [&](const DriveCycle& dc) {
        if (!dc.running) {
            crank_lo = std::fmin(crank_lo, dc.battery_v);
            crank_last = dc.battery_v;
        }
    });
    EXPECT_LT(crank_lo, 10.0f) << "the dip as the starter engages";
    EXPECT_GE(crank_last, 9.5f);
    EXPECT_LT(crank_last, 10.5f) << "still turning over, settled under 10.5 V";

    /* The rest of the first loop, the pulls and the cruise included, from
     * the warm-up on: the smoothing after the start is over by then */
    float run_lo = 1e9f, run_hi = -1e9f;
    uint32_t running_ticks = 0;
    DriveCycle dc;
    bool warm = false;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f) && dc.loops == 0;
         ++i) {
        dc.step();
        warm = warm || dc.phase == Phase::WarmUp;
        if (warm && dc.running) {
            run_lo = std::fmin(run_lo, dc.battery_v);
            run_hi = std::fmax(run_hi, dc.battery_v);
            ++running_ticks;
        }
    }
    EXPECT_GT(running_ticks, ecuio_mock::ticks_for(60.0f)) << "the loop ran";
    EXPECT_GE(run_lo, 13.7f);
    EXPECT_LE(run_hi, 14.8f);
}

TEST(CoreTest, MockDriveCycleSignalsStayPhysical)
{
    /* Three loops (cold, warming, hot), every tick, against the rules a
     * real ECU's log keeps:
     * rpm locked to wheel speed in gear, no speed at idle rpm with the
     * clutch in, airflow consistent with rpm and load, lambda lean
     * exactly when fuel is cut, and temperatures that move like masses
     * of metal and coolant. */
    DriveCycle dc;
    DriveCycle prev = dc;
    uint32_t cut_run = 0;
    uint32_t fueled_run = 0;
    uint32_t fueled_at_limit = 0;
    int checked_locked = 0;
    while (dc.loops < 3 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        const float k = dc.gear > 0 ? ecuio_mock::rpm_per_mps(dc.gear) : 0.0f;
        const bool limiter = dc.phase == Phase::Limiter2nd || dc.phase == Phase::Limiter3rd;
        const bool locked = dc.gear > 0 && !dc.clutch_slip && !dc.shifting() && !dc.spinning;

        if (locked && !limiter) {
            /* Within the crank reading's firing-pulse jitter */
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 10.0f) << "tick " << dc.tick;
            ++checked_locked;
        } else if (locked) {
            /* The saw is crank speed against the driveline's compliance */
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 200.0f) << "tick " << dc.tick;
        }
        if (dc.gear == 0 && !dc.shifting()) {
            EXPECT_LE(dc.speed_kmh, 50.0f) << "clutch in at speed, tick " << dc.tick;
        }
        if (dc.gear > 0 && !dc.clutch_slip && !dc.shifting()) {
            EXPECT_GE(dc.rpm, 1000.0f) << "lugging in gear " << dc.gear
                                       << ", tick " << dc.tick;
        }
        if (dc.spinning) {
            EXPECT_GE(dc.wheel_mps, dc.v_mps) << "tick " << dc.tick;
            EXPECT_LE(dc.slip(), 0.5f) << "tick " << dc.tick;
        }

        /* Below full load the load is the fill, so MAF is WOT's airflow
         * times it; at full load (boost reads 100 %) MAF carries the rest,
         * at least WOT's airflow and at most what the boost's charge adds */
        const float wot = ecuio_mock::maf_wot_gs(dc.rpm);
        if (dc.load_pct < 99.0f) {
            const float estimate = wot * dc.load_pct / 100.0f;
            EXPECT_NEAR(dc.maf_gs, estimate, 0.1f * estimate + 0.5f)
                << "tick " << dc.tick;
        } else {
            EXPECT_GE(dc.maf_gs, 0.9f * wot) << "tick " << dc.tick;
            EXPECT_LE(dc.maf_gs,
                      1.1f * wot *
                          ecuio_mock::charge_fill(dc.boost_kpa, dc.baro_kpa))
                << "tick " << dc.tick;
        }
        if (dc.running && dc.rpm >= 600.0f) {
            EXPECT_GT(dc.maf_gs, 5.0f) << "engine running, tick " << dc.tick;
        }

        /* The manifold: never above the outside air plus the most boost
         * the wastegate holds, and above the outside air only on boost,
         * which needs full load above the spool rpm; the outside air
         * itself with the engine stopped; with no boost, once it pumps a
         * full vacuum, the residual plus load's share of the outside air,
         * less the intake's loss; near the outside air at WOT, a deep
         * vacuum at idle */
        EXPECT_LE(dc.map_kpa, dc.baro_kpa + ecuio_mock::kBoostMaxKpa)
            << "tick " << dc.tick;
        if (dc.map_kpa > dc.baro_kpa) {
            EXPECT_GE(dc.load_pct, 95.0f)
                << "boost without load, tick " << dc.tick;
            EXPECT_GE(dc.rpm, ecuio_mock::kSpoolRpm)
                << "boost below the spool, tick " << dc.tick;
        }
        if (dc.rpm == 0.0f) {
            EXPECT_FLOAT_EQ(dc.map_kpa, dc.baro_kpa)
                << "stopped, tick " << dc.tick;
        }
        if (dc.rpm >= ecuio_mock::kMapPumpRpm && dc.boost_kpa == 0.0f) {
            const float fill = ecuio_mock::kMapResidualKpa +
                               (dc.baro_kpa - ecuio_mock::kMapResidualKpa) *
                                   dc.load_pct / 100.0f;
            EXPECT_LE(dc.map_kpa, fill + 0.01f) << "tick " << dc.tick;
            EXPECT_GE(dc.map_kpa, fill - 1.2f * ecuio_mock::kIntakeLossKpa)
                << "tick " << dc.tick;
            EXPECT_GE(dc.map_kpa, kMockMapMin) << "tick " << dc.tick;
        }
        if (dc.load_pct >= 95.0f) {
            EXPECT_GE(dc.map_kpa, 0.9f * dc.baro_kpa)
                << "WOT, tick " << dc.tick;
        }
        if (dc.phase == Phase::Stage) {
            EXPECT_GE(dc.map_kpa, 25.0f) << "idle, tick " << dc.tick;
            EXPECT_LE(dc.map_kpa, 50.0f) << "idle, tick " << dc.tick;
        }
        EXPECT_GE(dc.throttle_pct, ecuio_mock::kTpClosedPct - 0.01f);
        EXPECT_LE(dc.throttle_pct, ecuio_mock::kTpWotPct + 0.01f);

        cut_run = dc.fuel_cut ? cut_run + 1 : 0;
        fueled_run = dc.fuel_cut ? 0 : fueled_run + 1;
        /* The rev limit cuts fuel the tick after rpm meets it, whatever
         * the phase: a lift off the limiter doesn't sit there fueled */
        fueled_at_limit = (!limiter && !dc.fuel_cut && dc.rpm >= ecuio_mock::kLimiterOnRpm)
            ? fueled_at_limit + 1 : 0;
        EXPECT_LE(fueled_at_limit, 1u) << "fueled at the rev limit, tick " << dc.tick;
        if (cut_run >= 2) {
            EXPECT_GT(dc.lambda, 1.3f) << "fuel cut, tick " << dc.tick;
        }
        if (fueled_run >= 3) {
            EXPECT_GE(dc.lambda, 0.8f) << "tick " << dc.tick;
            EXPECT_LE(dc.lambda, 1.1f) << "tick " << dc.tick;
        }

        EXPECT_LE(std::fabs(dc.coolant_c - prev.coolant_c), 0.5f) << "tick " << dc.tick;
        EXPECT_LE(std::fabs(dc.oil_c - prev.oil_c), 0.5f) << "tick " << dc.tick;
        EXPECT_LE(std::fabs(dc.iat_c - prev.iat_c), 0.5f) << "tick " << dc.tick;
        prev = dc;
    }
    ASSERT_EQ(dc.loops, 3u) << "three loops never completed";
    EXPECT_GT(checked_locked, 1000) << "too few in-gear ticks checked";
}

TEST(CoreTest, MockDriveCycleReadingsNeverHoldDeadStill)
{
    /* A running engine's rpm, as the wire carries it, never holds one
     * value for long: the idle hunts, the driver's speed wanders, the
     * crank reading jitters. A stopped one reads 0 (KeyOn). And away
     * from the moments that are quick on a real car (a shift, the
     * limiter, the launch and the drag run), rpm never steps between
     * ticks by more than an engine can change, phase changes, a stop and
     * a start included. */
    DriveCycle dc;
    uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN] = {};
    uint16_t last_raw = 0;
    uint32_t same = 0, longest = 0;
    float prev_rpm = dc.rpm;
    float worst_step = 0.0f;
    uint64_t worst_tick = 0;
    while (dc.loops < 2 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ecuio_mock::encode_payload(dc, payload);
        const uint16_t raw = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
        same = (dc.tick > 1 && raw == last_raw && dc.phase != Phase::KeyOn)
                   ? same + 1
                   : 0;
        longest = std::max(longest, same);
        last_raw = raw;

        const bool quick = dc.shifting() || dc.phase == Phase::Limiter2nd ||
                           dc.phase == Phase::Limiter3rd || dc.phase == Phase::Launch ||
                           dc.phase == Phase::DragRun;
        if (!quick && std::fabs(dc.rpm - prev_rpm) > worst_step) {
            worst_step = std::fabs(dc.rpm - prev_rpm);
            worst_tick = dc.tick;
        }
        prev_rpm = dc.rpm;
    }
    ASSERT_EQ(dc.loops, 2u) << "two loops never completed";
    EXPECT_LE(longest, 10u) << "rpm read the same for over half a second";
    EXPECT_LE(worst_step, 150.0f) << "rpm stepped at tick " << worst_tick;
}

TEST(CoreTest, MockDriveCycleEachLoopStartsWithTheEngineStopped)
{
    /* Every loop starts with 15 s of the key on and the engine stopped,
     * where a clear can run (ADR-0038): from the first tick on the
     * first loop, and once the engine has run down from idle on later
     * ones, it reads 0 rpm with no air, no speed and the sensor lean,
     * and with the pump stopped the warm coolant soaks heat from the
     * block rather than losing it to the radiator. Then the starter
     * cranks it, and it catches and runs at its idle by the end of the
     * start. */
    constexpr uint32_t kLoops = 3;
    uint32_t key_on[kLoops] = {}, stopped[kLoops] = {};
    float crank_hi[kLoops] = {}, start_end_rpm[kLoops] = {};
    float soak_entry[kLoops] = {}, soak_lo[kLoops] = {};
    bool started[kLoops] = {};
    bool ran_with_key_on = false;
    DriveCycle dc;
    EXPECT_EQ(dc.phase, Phase::KeyOn);
    EXPECT_EQ(dc.rpm, 0.0f);
    while (dc.loops < kLoops && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        if (dc.loops >= kLoops) {
            break;
        }
        const uint32_t i = dc.loops;
        if (dc.phase == Phase::KeyOn) {
            if (key_on[i] == 0) {
                soak_entry[i] = soak_lo[i] = dc.coolant_c;
            }
            soak_lo[i] = std::fmin(soak_lo[i], dc.coolant_c);
            ++key_on[i];
            ran_with_key_on = ran_with_key_on || dc.running;
            if (dc.rpm == 0.0f) {
                ++stopped[i];
                EXPECT_EQ(dc.maf_gs, 0.0f) << "tick " << dc.tick;
                EXPECT_EQ(dc.load_pct, 0.0f) << "tick " << dc.tick;
                EXPECT_EQ(dc.speed_kmh, 0.0f) << "tick " << dc.tick;
                EXPECT_GT(dc.lambda, 1.3f) << "tick " << dc.tick;
            }
        } else if (dc.phase == Phase::Start) {
            if (!dc.running) {
                crank_hi[i] = std::fmax(crank_hi[i], dc.rpm);
            }
            start_end_rpm[i] = dc.rpm;
            started[i] = dc.running;
        }
    }
    ASSERT_EQ(dc.loops, kLoops) << "three loops never completed";
    EXPECT_FALSE(ran_with_key_on);
    ASSERT_GT(soak_entry[kLoops - 1], ecuio_mock::kThermostatOpenC);
    EXPECT_GE(soak_lo[kLoops - 1], soak_entry[kLoops - 1] - 0.1f)
        << "the radiator cooled a stopped engine";
    for (uint32_t i = 0; i < kLoops; ++i) {
        SCOPED_TRACE(i + 1);
        EXPECT_EQ(key_on[i], ecuio_mock::kKeyOnTicks);
        EXPECT_GE(stopped[i], ecuio_mock::ticks_for(14.0f))
            << "it ran down within a second";
        EXPECT_GE(crank_hi[i], 180.0f);
        EXPECT_LE(crank_hi[i], 260.0f);
        EXPECT_TRUE(started[i]);
        EXPECT_GE(start_end_rpm[i], 700.0f);
        EXPECT_LE(start_end_rpm[i], 1500.0f);
    }
}

TEST(CoreTest, MockDriveCycleLimiterCutsFuelNotAir)
{
    /* On the limiter the blade stays open, airflow stays at WOT's or
     * above (the cut starves the turbine, so boost sags but holds) and the
     * fuel is cut most of each cycle: lambda reads lean, load pegged.
     * Road speed holds while rpm saws. */
    const float wot_maf =
        ecuio_mock::maf_wot_gs(ecuio_mock::kRevLimitRpm - 200.0f);
    float min_load = 100.0f, min_maf = 1e9f, max_lambda = 0.0f;
    float min_kmh = 1e9f, max_kmh = 0.0f;
    uint32_t cut = 0, ticks = 0;
    for_each_tick_of(Phase::Limiter2nd, [&](const DriveCycle& dc) {
        EXPECT_FLOAT_EQ(dc.throttle_pct, ecuio_mock::kTpWotPct);
        min_load = std::fmin(min_load, dc.load_pct);
        min_maf = std::fmin(min_maf, dc.maf_gs);
        max_lambda = std::fmax(max_lambda, dc.lambda);
        if (dc.phase_ticks >= ecuio_mock::ticks_for(1.0f)) {
            min_kmh = std::fmin(min_kmh, dc.speed_kmh);
            max_kmh = std::fmax(max_kmh, dc.speed_kmh);
        }
        cut += dc.fuel_cut ? 1u : 0u;
        ++ticks;
    });
    ASSERT_EQ(ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(min_load, 94.0f) << "near WOT air, less the MAF noise";
    EXPECT_GE(min_maf, 0.9f * wot_maf);
    EXPECT_GT(max_lambda, 1.6f);
    EXPECT_GT(cut, ticks / 2) << "the limiter should spend most of its time in cut";
    EXPECT_LE(max_kmh - min_kmh, 1.0f) << "speed should hold on the limiter";
}

TEST(CoreTest, MockDriveCycleHighwayCruiseHoldsSpeed)
{
    /* A light, steady cruise in top gear: closed-loop fueling, the low
     * load and airflow of road-load power. */
    float lo_kmh = 1e9f, hi_kmh = 0.0f, lo_rpm = 1e9f, hi_rpm = 0.0f;
    float hi_load = 0.0f, hi_maf = 0.0f, lo_lambda = 9.0f, hi_lambda = 0.0f;
    uint32_t ticks = 0;
    for_each_tick_of(Phase::Cruise4th, [&](const DriveCycle& dc) {
        EXPECT_EQ(dc.gear, ecuio_mock::kTopGear);
        if (dc.phase_ticks < ecuio_mock::ticks_for(5.0f)) {
            return;   /* settling from the lift */
        }
        lo_kmh = std::fmin(lo_kmh, dc.speed_kmh);
        hi_kmh = std::fmax(hi_kmh, dc.speed_kmh);
        lo_rpm = std::fmin(lo_rpm, dc.rpm);
        hi_rpm = std::fmax(hi_rpm, dc.rpm);
        hi_load = std::fmax(hi_load, dc.load_pct);
        hi_maf = std::fmax(hi_maf, dc.maf_gs);
        lo_lambda = std::fmin(lo_lambda, dc.lambda);
        hi_lambda = std::fmax(hi_lambda, dc.lambda);
        ++ticks;
    });
    ASSERT_GE(ticks, ecuio_mock::ticks_for(9.0f));
    EXPECT_GE(lo_kmh, 98.0f);
    EXPECT_LE(hi_kmh, 102.0f);
    EXPECT_GE(lo_rpm, 2850.0f);
    EXPECT_LE(hi_rpm, 3100.0f);
    EXPECT_LE(hi_load, 35.0f);
    EXPECT_LE(hi_maf, 37.0f);
    EXPECT_GE(lo_lambda, 0.97f);
    EXPECT_LE(hi_lambda, 1.03f);
}

TEST(CoreTest, MockDriveCycleColdStartIdlesFastThenSettles)
{
    /* The cold-start strategy as the first vehicle showed it: a fast
     * idle near 1430 rpm on extra air for ~20 s, open-loop fueling, then
     * an 850-900 rpm warm-up idle. */
    float early_lo = 1e9f, early_hi = 0.0f, late_rpm = 0.0f, fast_load = 0.0f;
    float fast_tp = 0.0f, lambda_lo = 9.0f, lambda_hi = 0.0f;
    for_each_tick_of(Phase::WarmUp, [&](const DriveCycle& dc) {
        if (dc.phase_ticks < ecuio_mock::ticks_for(15.0f)) {
            early_lo = std::fmin(early_lo, dc.rpm);
            early_hi = std::fmax(early_hi, dc.rpm);
            fast_load = std::fmax(fast_load, dc.load_pct);
            fast_tp = std::fmax(fast_tp, dc.throttle_pct);
        }
        late_rpm = dc.rpm;
        lambda_lo = std::fmin(lambda_lo, dc.lambda);
        lambda_hi = std::fmax(lambda_hi, dc.lambda);
    });
    EXPECT_GE(early_lo, 1400.0f);
    EXPECT_LE(early_hi, 1460.0f);
    EXPECT_GE(fast_load, 40.0f) << "catalyst heating runs on extra air";
    EXPECT_GT(fast_tp, 30.0f);
    EXPECT_GE(late_rpm, 850.0f);
    EXPECT_LE(late_rpm, 900.0f);
    EXPECT_NEAR(lambda_lo, ecuio_mock::kLambdaColdOpenLoop, 0.01f);
    EXPECT_NEAR(lambda_hi, ecuio_mock::kLambdaColdOpenLoop, 0.01f);
}

TEST(CoreTest, MockDriveCycleLaunchSpinsTheTiresAndHooksUp)
{
    /* Off the line the clutch slips while the tires sit near their peak
     * slip; once it locks, traction control holds the drive to the grip,
     * so the tires come back to road speed in 1st and never spin again,
     * and the engine is locked to the wheels from there on */
    float slip_hi = 0.0f;
    float clutch_rpm_lo = 1e9f;
    bool hooked = false;
    bool spun_after = false;
    for_each_tick_of(Phase::DragRun, [&](const DriveCycle& dc) {
        if (dc.clutch_slip) {
            clutch_rpm_lo = std::fmin(clutch_rpm_lo, dc.rpm);
            slip_hi = std::fmax(slip_hi, dc.slip());
        } else if (!dc.spinning) {
            hooked = true;
        }
        if (hooked && dc.spinning) {
            spun_after = true;
        }
    });
    EXPECT_GE(clutch_rpm_lo, 3000.0f) << "the engine bogged off the line";
    EXPECT_GT(slip_hi, 0.05f)
        << "the clutch never took the tires to their slip";
    EXPECT_LT(slip_hi, 0.2f);
    EXPECT_TRUE(hooked) << "the tires never hooked up";
    EXPECT_FALSE(spun_after) << "traction control let the tires spin";
}

TEST(CoreTest, MockDriveCycleEachWotPullBoostsAndALiftVentsIt)
{
    /* Three loops. In each WOT pull (2nd, 3rd, the quarter mile) boost,
     * MAP over the outside air as a gauge reads it, passes 12 psi above
     * the spool rpm, trailing the throttle (none in a road pull's first
     * ticks); and a lift from WOT (off the limiters, past the line, each
     * of the strip's upshifts) vents it through the blow-off valve: MAP
     * below the outside air within three ticks of the blade starting to
     * close. At 12 psi the charge's density puts MAF well past WOT's and
     * the fueling runs rich, lambda 0.80-0.85 */
    constexpr float k12Psi = 82.7f;
    constexpr uint32_t kVentTicks = 3;
    float peak[3][3] = {};
    float first_ticks_hi = -1e9f;
    uint32_t vents = 0;
    uint32_t on_boost = 0;
    DriveCycle dc;
    float prev_blade = 0.0f;
    uint32_t closing = 0;
    while (dc.loops < 3 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        if (dc.loops >= 3) {
            break;
        }
        const float boost = dc.map_kpa - dc.baro_kpa;
        const int pull = dc.phase == Phase::Pull2nd   ? 0
                         : dc.phase == Phase::Pull3rd ? 1
                         : dc.phase == Phase::DragRun ? 2
                                                      : -1;
        if (pull >= 0 && dc.rpm >= ecuio_mock::kSpoolRpm) {
            float& p = peak[dc.loops][pull];
            p = std::fmax(p, boost);
        }
        if (pull >= 0 && pull < 2 && dc.phase_ticks <= 2) {
            first_ticks_hi = std::fmax(first_ticks_hi, boost);
        }
        if (pull >= 0 && boost >= k12Psi && !dc.fuel_cut) {
            EXPECT_GE(dc.maf_gs, 1.4f * ecuio_mock::maf_wot_gs(dc.rpm))
                << "tick " << dc.tick;
            EXPECT_GE(dc.lambda, 0.80f) << "tick " << dc.tick;
            EXPECT_LE(dc.lambda, 0.85f) << "tick " << dc.tick;
            ++on_boost;
        }
        /* A blade closing from WOT and still closing (a lift, not the
         * strip's feathering, which eases it and holds) */
        if (dc.blade < prev_blade && prev_blade >= 0.99f) {
            closing = 1;
        } else if (closing > 0 && dc.blade < prev_blade) {
            ++closing;
        } else {
            closing = 0;
        }
        if (closing == kVentTicks) {
            EXPECT_LT(dc.map_kpa, dc.baro_kpa)
                << "the boost wasn't vented, tick " << dc.tick;
            closing = 0;
            ++vents;
        }
        prev_blade = dc.blade;
    }
    ASSERT_EQ(dc.loops, 3u);
    for (uint32_t loop = 0; loop < 3; ++loop) {
        for (int pull = 0; pull < 3; ++pull) {
            EXPECT_GE(peak[loop][pull], k12Psi)
                << "loop " << loop + 1 << ", pull " << pull;
        }
    }
    EXPECT_LT(first_ticks_hi, 5.0f) << "boost came with the throttle, no lag";
    EXPECT_GE(vents, 3u * 6u) << "too few lifts seen";
    EXPECT_GE(on_boost, ecuio_mock::ticks_for(30.0f))
        << "too little boost seen";
}

