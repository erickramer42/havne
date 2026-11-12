// File/layer: tests/test_recording_aids.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingAidsTest (test_core)
//
// The navigation aids' rules (core/src/recording_aids.h) over plain sample
// vectors: the pull's gate, gap, trim and rise (engine speed's climb, or
// the speed gained with no engine speed), the cruise's bands and floor,
// and the cut at a range's edge.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "recording_aids.h" /* core/src */

namespace aids = ecuio_aids;

namespace {

constexpr uint64_t kMs = 1000;
constexpr uint64_t kS = 1000000;

/* Samples every step_ms from from_ms, each value from f(ms) */
template <typename F>
aids::samples every(uint64_t from_ms, uint64_t to_ms, uint64_t step_ms, F f)
{
    aids::samples out;
    for (uint64_t ms = from_ms; ms <= to_ms; ms += step_ms) {
        out.push_back(aids::sample{ms * kMs, f(ms)});
    }
    return out;
}

const aids::pull_rules kPull{80.0, 1 * kS, 500 * kMs, 0.0, 750.0, 10.0};
const aids::cruise_rules kCruise{10 * kS, 2.0, 3.0, 56.32704};

} // namespace

TEST(RecordingAidsTest, ARunAtTheLevelKeepsADipShorterThanTheGap)
{
    /* 90 % from 1 s to 4 s, 40 % for 400 ms at 2 s, then 600 ms at 3.2 s */
    const aids::samples gate = every(0, 6000, 100, [](uint64_t ms) {
        if (ms < 1000 || ms > 5000) {
            return 10.0;
        }
        if ((ms >= 2000 && ms < 2400) || (ms >= 3200 && ms < 3800)) {
            return 40.0;
        }
        return 90.0;
    });
    const auto runs = aids::runs_at_or_above(gate, 80.0, 500 * kMs);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(gate[runs[0].first].us, 1000 * kMs);
    EXPECT_EQ(gate[runs[0].second].us, 3100 * kMs)
        << "the 400 ms dip kept, the 600 ms one ending the run";
    EXPECT_EQ(gate[runs[1].first].us, 3800 * kMs);
    EXPECT_EQ(gate[runs[1].second].us, 5000 * kMs);

    /* A dip exactly the gap long ends the run: shorter than the gap stays */
    const aids::samples edge = every(0, 3000, 100, [](uint64_t ms) {
        return ms >= 1000 && ms < 1500 ? 0.0 : 90.0;
    });
    EXPECT_EQ(aids::runs_at_or_above(edge, 80.0, 500 * kMs).size(), 2u);
    EXPECT_EQ(aids::runs_at_or_above(edge, 80.0, 501 * kMs).size(), 1u);

    /* At the level counts; NaN never does */
    const aids::samples at = {{0, 80.0}, {kS, NAN}, {2 * kS, 80.0}};
    EXPECT_EQ(aids::runs_at_or_above(at, 80.0, 500 * kMs).size(), 2u);
}

TEST(RecordingAidsTest, APullMustRiseAndLastItsHold)
{
    const aids::samples throttle = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 2000 && ms <= 6000 ? 95.0 : 5.0;
    });
    /* Speed 40 to 100 km/h over the pull */
    const aids::samples rising = every(0, 10000, 100, [](uint64_t ms) {
        return ms < 2000   ? 40.0
               : ms > 6000 ? 100.0
                           : 40.0 + (ms - 2000) * 0.015;
    });
    std::vector<aids::stretch> out;
    aids::find_pulls(throttle, rising, true, nullptr, kPull, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_EQ(out[0].from_us, 2000 * kMs);
    EXPECT_EQ(out[0].to_us, 6000 * kMs);
    EXPECT_DOUBLE_EQ(out[0].start, 40.0);
    EXPECT_DOUBLE_EQ(out[0].end, 100.0);

    /* Flat speed: no pull */
    out.clear();
    const aids::samples flat =
        every(0, 10000, 100, [](uint64_t) { return 60.0; });
    aids::find_pulls(throttle, flat, true, nullptr, kPull, 0, out);
    EXPECT_TRUE(out.empty());

    /* Shorter than the hold: no pull */
    out.clear();
    const aids::samples stab = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 2000 && ms < 2900 ? 95.0 : 5.0;
    });
    aids::find_pulls(stab, rising, true, nullptr, kPull, 0, out);
    EXPECT_TRUE(out.empty());

    /* Exactly the hold: a pull */
    const aids::samples held = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 2000 && ms <= 3000 ? 95.0 : 5.0;
    });
    aids::find_pulls(held, rising, true, nullptr, kPull, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us - out[0].from_us, kPull.hold_us);
}

TEST(RecordingAidsTest,
     TheLargestClimbIsTheMostALaterSampleStandsAboveAnEarlier)
{
    /* A pull through an upshift: up 1000, down 1900, up 600 */
    const aids::samples shift = {{0, 5500.0},
                                 {1 * kS, 6000.0},
                                 {2 * kS, 6500.0},
                                 {3 * kS, 4600.0},
                                 {4 * kS, 5200.0}};
    EXPECT_DOUBLE_EQ(aids::largest_climb(shift, 0, 4 * kS), 1000.0);
    EXPECT_DOUBLE_EQ(aids::largest_climb(shift, 3 * kS, 4 * kS), 600.0);
    EXPECT_DOUBLE_EQ(aids::largest_climb(shift, 2 * kS, 3 * kS), 0.0)
        << "falling alone is no climb";
    EXPECT_DOUBLE_EQ(aids::largest_climb(shift, 0, 0), 0.0);
    /* A dip and then a climb: from the dip's foot, not from the start */
    const aids::samples dip = {{0, 5000.0}, {1 * kS, 3000.0}, {2 * kS, 4500.0}};
    EXPECT_DOUBLE_EQ(aids::largest_climb(dip, 0, 2 * kS), 1500.0);

    /* The value in force at the start counts, and nothing before it or
     * after the end does */
    const aids::samples around = {{0, 1000.0},
                                  {1 * kS, 3000.0},
                                  {2 * kS + 500 * kMs, 3400.0},
                                  {4 * kS, 9000.0}};
    EXPECT_DOUBLE_EQ(aids::largest_climb(around, 2 * kS, 3 * kS), 400.0);
    EXPECT_DOUBLE_EQ(aids::largest_climb(around, 2 * kS, 4 * kS), 6000.0);

    /* A NaN is passed by, in force at the start too; no sample, no climb */
    const aids::samples holed = {
        {0, NAN}, {1 * kS, 2000.0}, {2 * kS, NAN}, {3 * kS, 2900.0}};
    EXPECT_DOUBLE_EQ(aids::largest_climb(holed, 0, 3 * kS), 900.0);
    EXPECT_DOUBLE_EQ(aids::largest_climb(aids::samples{}, 0, 3 * kS), 0.0);
}

TEST(RecordingAidsTest, APullClimbsItsRiseOrWithNoEngineSpeedGainsItsSpeed)
{
    const aids::samples throttle = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 2000 && ms <= 6000 ? 95.0 : 5.0;
    });
    /* Speed gaining gain km/h and engine speed rise rpm over the pull */
    const auto ramp = [](double from, double gain) {
        return every(0, 10000, 100, [=](uint64_t ms) {
            return ms < 2000   ? from
                   : ms > 6000 ? from + gain
                               : from + gain * (ms - 2000) / 4000.0;
        });
    };
    const auto pulls = [&](const aids::samples& speed,
                           const aids::samples* rpm) {
        std::vector<aids::stretch> out;
        aids::find_pulls(throttle, speed, true, rpm, kPull, 0, out);
        return out.size();
    };

    /* A pull in top gear gains little speed and climbs a long way; a stab
     * at the throttle gains nearly as much and climbs next to nothing */
    const aids::samples top_gear_rpm = ramp(4300.0, 2000.0);
    const aids::samples stab_rpm = ramp(2500.0, 180.0);
    EXPECT_EQ(pulls(ramp(150.0, 15.0), &top_gear_rpm), 1u);
    EXPECT_EQ(pulls(ramp(60.0, 8.0), &stab_rpm), 0u);

    /* The rise at the floor counts; a hair under doesn't */
    const aids::samples at_floor = ramp(3000.0, 750.0);
    const aids::samples under = ramp(3000.0, 749.0);
    EXPECT_EQ(pulls(ramp(60.0, 30.0), &at_floor), 1u);
    EXPECT_EQ(pulls(ramp(60.0, 30.0), &under), 0u);

    /* With engine speed the gain is no floor: 2 km/h with a real climb is
     * a pull, 30 km/h with none isn't (speed must still rise at all) */
    EXPECT_EQ(pulls(ramp(200.0, 2.0), &top_gear_rpm), 1u);
    EXPECT_EQ(pulls(ramp(60.0, 30.0), &stab_rpm), 0u);
    EXPECT_EQ(pulls(ramp(60.0, 0.0), &top_gear_rpm), 0u);
    /* Speed read 50 ms before the throttle, as a poll loop reads them
     * one after the other: still falling as the throttle opens, then
     * rising under where it was. No rise over the pull, whatever engine
     * speed climbs. */
    const aids::samples dipped = every(50, 10000, 100, [](uint64_t ms) {
        return ms < 2000 ? 100.0
                         : 95.0 + (std::min<uint64_t>)(ms - 2000, 4000) * 0.001;
    });
    EXPECT_EQ(pulls(dipped, &top_gear_rpm), 0u);

    /* With none, the gain is: at the floor counts, under it doesn't */
    EXPECT_EQ(pulls(ramp(150.0, 15.0), nullptr), 1u);
    EXPECT_EQ(pulls(ramp(60.0, 10.0), nullptr), 1u);
    EXPECT_EQ(pulls(ramp(60.0, 8.0), nullptr), 0u);

    /* An engine-speed measure: the climb alone, so a pull through an
     * upshift that ends under where it began is one */
    const aids::samples shifted = every(0, 10000, 100, [](uint64_t ms) {
        if (ms < 2000) {
            return 5500.0;
        }
        if (ms < 4000) {
            return 5500.0 + (ms - 2000) * 0.5; /* to 6500 */
        }
        return ms < 6000 ? 4600.0 + (ms - 4000) * 0.3 : 5200.0;
    });
    std::vector<aids::stretch> out;
    aids::find_pulls(throttle, shifted, false, &shifted, kPull,
                     ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0].start, 5500.0);
    EXPECT_LT(out[0].end, out[0].start);
    out.clear();
    aids::find_pulls(throttle, stab_rpm, false, &stab_rpm, kPull,
                     ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED, out);
    EXPECT_TRUE(out.empty());

    /* A measure with no value at an end is no pull */
    const aids::samples late =
        every(3000, 10000, 100, [](uint64_t ms) { return 3000.0 + ms; });
    out.clear();
    aids::find_pulls(throttle, late, false, &late, kPull, 0, out);
    EXPECT_TRUE(out.empty());
}

TEST(RecordingAidsTest, ASpeedMeasureEndsAPullAtItsHighestAndEngineSpeedDoesnt)
{
    /* Full throttle 1 s to 9 s; speed rises to 5 s, then holds flat (the
     * limiter); engine speed rises, drops a third at 4 s (a shift), rises */
    const aids::samples throttle = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 1000 && ms <= 9000 ? 99.0 : 3.0;
    });
    const aids::samples speed = every(0, 10000, 100, [](uint64_t ms) {
        return ms < 1000 ? 30.0 : ms < 5000 ? 30.0 + (ms - 1000) * 0.02 : 110.0;
    });
    std::vector<aids::stretch> out;
    aids::find_pulls(throttle, speed, true, nullptr, kPull, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us, 5000 * kMs) << "the flat speed ends it";
    EXPECT_DOUBLE_EQ(out[0].end, 110.0);

    const aids::samples rpm = every(0, 10000, 100, [](uint64_t ms) {
        if (ms < 1000) {
            return 2000.0;
        }
        return ms < 4000 ? 2000.0 + (ms - 1000) : 4000.0 + (ms - 4000) * 0.5;
    });
    out.clear();
    aids::find_pulls(throttle, rpm, false, &rpm, kPull,
                     ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us, 9000 * kMs) << "the shift's drop keeps it whole";
    EXPECT_EQ(out[0].flags, ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);

    /* A tolerance takes a speed creeping up on the limiter as at its
     * highest from where the creep starts */
    const aids::samples creep = every(0, 10000, 100, [](uint64_t ms) {
        if (ms < 1000) {
            return 30.0;
        }
        return ms < 5000 ? 30.0 + (ms - 1000) * 0.02
                         : 110.0 + (ms - 5000) * 0.0001;
    });
    out.clear();
    aids::find_pulls(throttle, creep, true, nullptr, kPull, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us, 9000 * kMs) << "no tolerance: the creep's top";
    aids::pull_rules tolerant = kPull;
    tolerant.tolerance = 0.5;
    out.clear();
    aids::find_pulls(throttle, creep, true, nullptr, tolerant, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us, 5000 * kMs);
}

TEST(RecordingAidsTest, ACruiseHoldsBothBandsAboveTheFloor)
{
    /* 80 km/h within 1 km/h for 12 s, a 3 km/h step, 12 s at 83 */
    const aids::samples speed = every(0, 24000, 200, [](uint64_t ms) {
        const double wobble = (ms / 200) % 2 == 0 ? 0.0 : 0.8;
        return (ms < 12000 ? 80.0 : 83.0) + wobble;
    });
    std::vector<aids::stretch> out;
    aids::find_cruises(speed, nullptr, kCruise,
                       ECUIO_RECORDING_STRETCH_NO_THROTTLE, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].kind, ECUIO_RECORDING_STRETCH_CRUISE);
    EXPECT_EQ(out[0].from_us, 0u);
    EXPECT_EQ(out[0].to_us, 11800 * kMs);
    EXPECT_EQ(out[1].from_us, 12000 * kMs) << "the step starts the next";
    EXPECT_EQ(out[1].to_us, 24000 * kMs);
    EXPECT_NEAR(out[0].start, 80.4, 0.05);
    EXPECT_EQ(out[0].start, out[0].end);
    EXPECT_EQ(out[0].flags, ECUIO_RECORDING_STRETCH_NO_THROTTLE);

    /* Throttle moving 5 % at 6 s breaks the first; the next starts at the
     * next speed sample and runs out the second's hold */
    const aids::samples throttle = every(0, 24000, 200, [](uint64_t ms) {
        return ms >= 6000 && ms < 6400 ? 25.0 : 20.0;
    });
    out.clear();
    aids::find_cruises(speed, &throttle, kCruise, 0, out);
    ASSERT_EQ(out.size(), 1u) << "6 s is under the hold; so is 6 to 12 s";
    EXPECT_EQ(out[0].from_us, 12000 * kMs);

    /* Under the floor: none, however steady */
    const aids::samples slow =
        every(0, 30000, 200, [](uint64_t) { return 50.0; });
    out.clear();
    aids::find_cruises(slow, nullptr, kCruise, 0, out);
    EXPECT_TRUE(out.empty());
    const aids::samples fast =
        every(0, 30000, 200, [](uint64_t) { return 60.0; });
    aids::find_cruises(fast, nullptr, kCruise, 0, out);
    EXPECT_EQ(out.size(), 1u);
    const aids::samples at_floor =
        every(0, 30000, 200, [](uint64_t) { return kCruise.floor; });
    out.clear();
    aids::find_cruises(at_floor, nullptr, kCruise, 0, out);
    EXPECT_EQ(out.size(), 1u) << "a speed at the floor clears it";
    const aids::samples just_held =
        every(0, 10000, 200, [](uint64_t) { return 100.0; });
    out.clear();
    aids::find_cruises(just_held, nullptr, kCruise, 0, out);
    ASSERT_EQ(out.size(), 1u) << "a cruise exactly its hold long is one";
    EXPECT_EQ(out[0].to_us - out[0].from_us, kCruise.hold_us);
}

TEST(RecordingAidsTest, AStretchPastAnEdgeIsCutThereAndSaysSo)
{
    const aids::samples speed =
        every(0, 30000, 200, [](uint64_t) { return 100.0; });
    std::vector<aids::stretch> found = {
        {ECUIO_RECORDING_STRETCH_CRUISE, 0, 30000 * kMs, 100.0, 100.0, 0},
    };
    aids::cut_to(found, 5000 * kMs, 25000 * kMs, kPull, true, nullptr, 10 * kS,
                 speed, speed);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].from_us, 5000 * kMs);
    EXPECT_EQ(found[0].to_us, 25000 * kMs);
    EXPECT_EQ(found[0].flags, ECUIO_RECORDING_STRETCH_CUT_START |
                                  ECUIO_RECORDING_STRETCH_CUT_END);

    /* What is left shorter than the hold goes */
    found = {{ECUIO_RECORDING_STRETCH_CRUISE, 0, 30000 * kMs, 100.0, 100.0, 0}};
    aids::cut_to(found, 25000 * kMs, 40000 * kMs, kPull, true, nullptr, 10 * kS,
                 speed, speed);
    EXPECT_TRUE(found.empty());

    /* A pull cut is measured again at its new ends */
    const aids::samples rising =
        every(0, 10000, 100, [](uint64_t ms) { return 40.0 + ms * 0.01; });
    found = {{ECUIO_RECORDING_STRETCH_PULL, 0, 10000 * kMs, 40.0, 140.0, 0}};
    aids::cut_to(found, 2000 * kMs, 20000 * kMs, kPull, true, nullptr, 10 * kS,
                 rising, speed);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_DOUBLE_EQ(found[0].start, 60.0);
    EXPECT_DOUBLE_EQ(found[0].end, 140.0);
    EXPECT_EQ(found[0].flags, ECUIO_RECORDING_STRETCH_CUT_START);

    /* What is left must still rise as a pull must: 8 km/h gained over
     * four seconds goes, 12 over six stays, and with engine speed its
     * climb over what is left decides */
    const aids::samples slow =
        every(0, 10000, 100, [](uint64_t ms) { return 40.0 + ms * 0.002; });
    found = {{ECUIO_RECORDING_STRETCH_PULL, 0, 10000 * kMs, 40.0, 60.0, 0}};
    aids::cut_to(found, 6000 * kMs, 20000 * kMs, kPull, true, nullptr, 10 * kS,
                 slow, speed);
    EXPECT_TRUE(found.empty());
    found = {{ECUIO_RECORDING_STRETCH_PULL, 0, 10000 * kMs, 40.0, 60.0, 0}};
    aids::cut_to(found, 4000 * kMs, 20000 * kMs, kPull, true, nullptr, 10 * kS,
                 slow, speed);
    EXPECT_EQ(found.size(), 1u);
    const aids::samples rpm =
        every(0, 10000, 100, [](uint64_t ms) { return 2000.0 + ms * 0.4; });
    found = {{ECUIO_RECORDING_STRETCH_PULL, 0, 10000 * kMs, 40.0, 140.0, 0}};
    aids::cut_to(found, 8000 * kMs, 20000 * kMs, kPull, true, &rpm, 10 * kS,
                 rising, speed);
    ASSERT_EQ(found.size(), 1u) << "800 rpm left to climb";
    found = {{ECUIO_RECORDING_STRETCH_PULL, 0, 10000 * kMs, 40.0, 140.0, 0}};
    aids::cut_to(found, 8200 * kMs, 20000 * kMs, kPull, true, &rpm, 10 * kS,
                 rising, speed);
    EXPECT_TRUE(found.empty()) << "720 rpm left";
}
