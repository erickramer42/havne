// File/layer: tests/test_recording_aids.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingAidsTest (test_core)
//
// The navigation aids' rules (core/src/recording_aids.h) over plain sample
// vectors: the pull's gate, gap, rising test and trim, the cruise's bands
// and floor, and the cut at a range's edge.

#include <gtest/gtest.h>

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

const aids::pull_rules kPull{80.0, 1 * kS, 500 * kMs, 0.0};
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
    aids::find_pulls(throttle, rising, true, kPull, 0, out);
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
    aids::find_pulls(throttle, flat, true, kPull, 0, out);
    EXPECT_TRUE(out.empty());

    /* Shorter than the hold: no pull */
    out.clear();
    const aids::samples stab = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 2000 && ms < 2900 ? 95.0 : 5.0;
    });
    aids::find_pulls(stab, rising, true, kPull, 0, out);
    EXPECT_TRUE(out.empty());

    /* Exactly the hold: a pull */
    const aids::samples held = every(0, 10000, 100, [](uint64_t ms) {
        return ms >= 2000 && ms <= 3000 ? 95.0 : 5.0;
    });
    aids::find_pulls(held, rising, true, kPull, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us - out[0].from_us, kPull.hold_us);
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
    aids::find_pulls(throttle, speed, true, kPull, 0, out);
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
    aids::find_pulls(throttle, rpm, false, kPull,
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
    aids::find_pulls(throttle, creep, true, kPull, 0, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].to_us, 9000 * kMs) << "no tolerance: the creep's top";
    aids::pull_rules tolerant = kPull;
    tolerant.tolerance = 0.5;
    out.clear();
    aids::find_pulls(throttle, creep, true, tolerant, 0, out);
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
    aids::cut_to(found, 5000 * kMs, 25000 * kMs, kS, 10 * kS, speed, speed);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].from_us, 5000 * kMs);
    EXPECT_EQ(found[0].to_us, 25000 * kMs);
    EXPECT_EQ(found[0].flags, ECUIO_RECORDING_STRETCH_CUT_START |
                                  ECUIO_RECORDING_STRETCH_CUT_END);

    /* What is left shorter than the hold goes */
    found = {{ECUIO_RECORDING_STRETCH_CRUISE, 0, 30000 * kMs, 100.0, 100.0, 0}};
    aids::cut_to(found, 25000 * kMs, 40000 * kMs, kS, 10 * kS, speed, speed);
    EXPECT_TRUE(found.empty());

    /* A pull cut is measured again at its new ends */
    const aids::samples rising =
        every(0, 10000, 100, [](uint64_t ms) { return 40.0 + ms * 0.01; });
    found = {{ECUIO_RECORDING_STRETCH_PULL, 0, 10000 * kMs, 40.0, 140.0, 0}};
    aids::cut_to(found, 2000 * kMs, 20000 * kMs, kS, 10 * kS, rising, speed);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_DOUBLE_EQ(found[0].start, 60.0);
    EXPECT_DOUBLE_EQ(found[0].end, 140.0);
    EXPECT_EQ(found[0].flags, ECUIO_RECORDING_STRETCH_CUT_START);
}
