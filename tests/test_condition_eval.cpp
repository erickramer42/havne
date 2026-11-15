// File/layer: tests/test_condition_eval.cpp | C++17, Qt-free, every platform
// Testing: UNIT: ConditionEvalTest (test_signals)
//
// core/src/condition_eval.h, fed by hand: a condition file bound to
// obd2's set, then frames as a time and the values they carry. No
// session and no clock, so every time is the test's own.

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include "condition_eval.h"

#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint16_t kRpm = ECUIO_OBD2_SIG_RPM_ID;
constexpr uint16_t kCoolant = ECUIO_OBD2_SIG_COOLANT_TEMP_ID;
constexpr uint16_t kThrottle = ECUIO_OBD2_SIG_THROTTLE_ID;
constexpr uint16_t kSpeed = ECUIO_OBD2_SIG_SPEED_ID;
constexpr uint64_t kSecond = 1000000;

/* A term on engine speed, with more keys */
std::string rpm(const std::string& test)
{
    return R"({"signal":["obd2.rpm"],"unit":"RPM",)" + test + "}";
}

std::string throttle(const std::string& test)
{
    return R"({"signal":["obd2.throttle"],"unit":"%",)" + test + "}";
}

/* Condition "c<n>" around a when's JSON and what follows it */
std::string condition(int n, const std::string& when,
                      const std::string& rest = "")
{
    const std::string k = std::to_string(n);
    return R"({"key":"c)" + k + R"(","name":"C)" + k + R"(","when":)" + when +
           rest + "}";
}

struct Eval {
    std::unique_ptr<ecuio_condition_eval> e;

    /* One condition, or several separated by commas */
    explicit Eval(const std::string& conditions, uint32_t generation = 1)
    {
        const std::string json =
            R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
        ecuio_condition_set_t* set = nullptr;
        EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        if (set != nullptr) {
            e = ecuio_condition_eval::make(set, ecuio_obd2_signal_set(),
                                           nullptr, generation);
        }
        /* The evaluator keeps nothing of the set */
        ecuio_condition_set_free(set);
    }

    /* One frame at seconds, carrying these values: its events as text,
     * "<time us> c<condition> on|off [stale] [fired] t<term> v<value>",
     * one per line */
    std::string
    at(double seconds,
       std::initializer_list<std::pair<uint16_t, double>> values = {})
    {
        std::vector<ecuio_value_t> batch;
        for (const auto& v : values) {
            ecuio_value_t x{};
            x.signal_id = v.first;
            x.kind = ECUIO_VALUE_UINT;
            x.value = v.second;
            batch.push_back(x);
        }
        const uint64_t t =
            static_cast<uint64_t>(seconds * 1e6 + (seconds < 0 ? -0.5 : 0.5));
        const uint32_t n =
            e->judge(t, batch.data(), static_cast<uint32_t>(batch.size()));
        EXPECT_EQ(e->dropped, 0u);
        std::string out;
        for (uint32_t i = 0; i < n; ++i) {
            const ecuio_condition_event_t& ev = e->events[i];
            EXPECT_EQ(ev.generation, e->generation);
            EXPECT_EQ(ev.arbitration_id, 0u);
            EXPECT_EQ(ev.code, 0);
            EXPECT_EQ(ev.reserved0, 0u);
            EXPECT_EQ(ev.reserved, 0u);
            char line[128];
            std::snprintf(
                line, sizeof line, "%s%llu c%u %s%s%s t%u v%g",
                i == 0 ? "" : "\n",
                static_cast<unsigned long long>(ev.timestamp_us), ev.condition,
                ev.state == ECUIO_CONDITION_ON ? "on" : "off",
                (ev.flags & ECUIO_CONDITION_STALE) != 0 ? " stale" : "",
                (ev.flags & ECUIO_CONDITION_FIRED) != 0 ? " fired" : "",
                ev.term, ev.value);
            out += line;
        }
        return out;
    }
};

} // namespace

TEST(ConditionEvalTest, ALevelWithNoHoldFiresAtItsSample)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    /* Nothing is judged before a sample; the first judgment is an event */
    EXPECT_EQ(e.at(0.5), "");
    EXPECT_EQ(e.at(0.5, {{kSpeed, 50.0}}), "") << "another signal";
    EXPECT_EQ(e.at(1, {{kRpm, 1000.0}}), "1000000 c0 off t0 v1000");
    EXPECT_EQ(e.at(2, {{kRpm, 3000.0}}), "") << "at the level isn't above it";
    EXPECT_EQ(e.at(3, {{kRpm, 3500.0}}), "3000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(4, {{kRpm, 3600.0}}), "");
    EXPECT_EQ(e.at(5, {{kRpm, 2000.0}}), "5000000 c0 off t0 v2000");
    EXPECT_EQ(e.at(6, {{kRpm, 9000.0}}), "6000000 c0 on fired t0 v9000");
    const ecuio_condition_state_t s = e.e->state_of(0);
    EXPECT_EQ(s.state, ECUIO_CONDITION_ON);
    EXPECT_EQ(s.flags, 0u);
    EXPECT_EQ(s.since_us, 6 * kSecond);
    EXPECT_EQ(s.fired, 2u);
    EXPECT_EQ(s.value, 9000.0);
    EXPECT_EQ(s.generation, 1u);
    EXPECT_EQ(s.why, 0u);
}

TEST(ConditionEvalTest, TheFirstJudgmentMayBeTheFiring)
{
    Eval e(condition(0, rpm(R"("above":3000)")), 7);
    ecuio_condition_state_t s = e.e->state_of(0);
    EXPECT_EQ(s.state, ECUIO_CONDITION_OFF);
    EXPECT_EQ(s.flags, ECUIO_CONDITION_STALE) << "OFF and stale at the attach";
    EXPECT_EQ(s.since_us, 0u);
    EXPECT_EQ(s.generation, 7u);
    EXPECT_EQ(e.at(1, {{kRpm, 4000.0}}), "1000000 c0 on fired t0 v4000");
}

TEST(ConditionEvalTest, HysteresisHoldsBetweenTheLevelAndItsClear)
{
    Eval above(condition(0, rpm(R"("above":100,"clear":90)")));
    EXPECT_EQ(above.at(1, {{kRpm, 95.0}}), "1000000 c0 off t0 v95")
        << "inside the band from nothing is false";
    EXPECT_EQ(above.at(2, {{kRpm, 101.0}}), "2000000 c0 on fired t0 v101");
    EXPECT_EQ(above.at(3, {{kRpm, 95.0}}), "") << "kept inside the band";
    EXPECT_EQ(above.at(4, {{kRpm, 90.5}}), "");
    EXPECT_EQ(above.at(5, {{kRpm, 90.0}}), "5000000 c0 off t0 v90")
        << "false at its clear";
    EXPECT_EQ(above.at(6, {{kRpm, 100.0}}), "")
        << "and not true again at the level";

    Eval below(condition(0, rpm(R"("below":10,"clear":12)")));
    EXPECT_EQ(below.at(1, {{kRpm, 9.0}}), "1000000 c0 on fired t0 v9");
    EXPECT_EQ(below.at(2, {{kRpm, 11.0}}), "");
    EXPECT_EQ(below.at(3, {{kRpm, 12.0}}), "3000000 c0 off t0 v12");
    EXPECT_EQ(below.at(4, {{kRpm, 10.0}}), "") << "at the level isn't below it";

    /* With no clear the level alone decides */
    Eval bare(condition(0, rpm(R"("below":10)")));
    EXPECT_EQ(bare.at(1, {{kRpm, 9.0}}), "1000000 c0 on fired t0 v9");
    EXPECT_EQ(bare.at(2, {{kRpm, 10.0}}), "2000000 c0 off t0 v10");
}

TEST(ConditionEvalTest, EqualsAndDiffersCompareTheNumber)
{
    Eval e(condition(0, rpm(R"("equals":2)")) + "," +
           condition(1, rpm(R"("differs":2)")));
    EXPECT_EQ(e.at(1, {{kRpm, 2.0}}),
              "1000000 c0 on fired t0 v2\n1000000 c1 off t0 v2");
    EXPECT_EQ(e.at(2, {{kRpm, 2.5}}),
              "2000000 c0 off t0 v2.5\n2000000 c1 on fired t0 v2.5");
}

TEST(ConditionEvalTest, AHoldEndsAtItsOwnTimeSettledByALaterFrame)
{
    Eval e(condition(0, rpm(R"("above":3000)"), R"(,"for":2,"clear_for":1.5)"));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500")
        << "judged, and the hold begins";
    EXPECT_EQ(e.at(2, {{kRpm, 3600.0}}), "");
    /* Any frame past the hold's end settles it, at the end's own time,
     * with the sample the term held then */
    EXPECT_EQ(e.at(3.5, {{kSpeed, 1.0}}), "3000000 c0 on fired t0 v3600");
    EXPECT_EQ(e.at(4, {{kRpm, 100.0}}), "") << "the clear hold begins";
    EXPECT_EQ(e.at(5, {{kRpm, 200.0}}), "");
    EXPECT_EQ(e.at(8, {{kRpm, 300.0}}), "5500000 c0 off t0 v200");
    EXPECT_EQ(e.e->state_of(0).since_us, 5500000u);
    EXPECT_EQ(e.e->state_of(0).fired, 1u) << "a clear hold's end fires nothing";
}

TEST(ConditionEvalTest, AHoldUnderWayKeepsItsEndAndTheTermThatBeganIt)
{
    Eval e(condition(0,
                     R"({"any":[)" + rpm(R"("above":3000)") + "," +
                         throttle(R"("above":80)") + "]}",
                     R"(,"for":2)"));
    EXPECT_EQ(e.at(1, {{kRpm, 1000.0}, {kThrottle, 10.0}}),
              "1000000 c0 off t1 v10");
    EXPECT_EQ(e.at(2, {{kThrottle, 90.0}}), "") << "the hold begins on term 1";
    /* The other term turning true changes nothing the hold waits on */
    EXPECT_EQ(e.at(3, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(4.5, {{kSpeed, 1.0}}), "4000000 c0 on fired t1 v90")
        << "two seconds from the hold's start, with the term that began it";
}

TEST(ConditionEvalTest, AHoldBrokenStartsOver)
{
    Eval e(condition(0, rpm(R"("above":3000)"), R"(,"for":2,"clear_for":2)"));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(e.at(2.5, {{kRpm, 100.0}}), "") << "broken half a second short";
    EXPECT_EQ(e.at(2.75, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(4.5, {{kRpm, 3500.0}}), "")
        << "two seconds from the first hold";
    EXPECT_EQ(e.at(5, {{kRpm, 3500.0}}), "4750000 c0 on fired t0 v3500");
    /* And the clear hold the same */
    EXPECT_EQ(e.at(6, {{kRpm, 100.0}}), "");
    EXPECT_EQ(e.at(7, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(10, {{kRpm, 3500.0}}), "") << "the condition stays ON";
    EXPECT_EQ(e.e->state_of(0).state, ECUIO_CONDITION_ON);
}

TEST(ConditionEvalTest, AHoldEndingAtAFramesTimeIsSettledAfterItsValues)
{
    /* The frame at the hold's end still holds: ON at that time */
    Eval kept(condition(0, rpm(R"("above":3000)"), R"(,"for":2)"));
    EXPECT_EQ(kept.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(kept.at(3, {{kRpm, 3700.0}}), "3000000 c0 on fired t0 v3700");
    /* The frame at the hold's end breaks it: never ON */
    Eval broken(condition(0, rpm(R"("above":3000)"), R"(,"for":2)"));
    EXPECT_EQ(broken.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(broken.at(3, {{kRpm, 100.0}}), "");
    EXPECT_EQ(broken.at(6, {{kRpm, 100.0}}), "");
    /* A frame with no value for it at exactly the end settles it too */
    Eval quiet(
        condition(0, rpm(R"("above":3000,"max_age":10)"), R"(,"for":2)"));
    EXPECT_EQ(quiet.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(quiet.at(3), "3000000 c0 on fired t0 v3500");
    /* An earlier check still owed at that frame settles nothing of the
     * hold before the frame's values: the age bound watched at 2 s moved
     * on with the samples, and the check stayed behind */
    Eval owed(condition(0, rpm(R"("above":3000,"max_age":1)"), R"(,"for":2)"));
    EXPECT_EQ(owed.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(owed.at(1.5, {{kRpm, 3600.0}}), "");
    EXPECT_EQ(owed.at(2.4, {{kRpm, 3600.0}}), "");
    EXPECT_LT(owed.e->next_check_us, 3 * kSecond);
    EXPECT_EQ(owed.at(3, {{kRpm, 100.0}}), "") << "broken at its end: never ON";
    EXPECT_EQ(owed.e->state_of(0).fired, 0u);
}

TEST(ConditionEvalTest, AllAndAnyJoinThreeWays)
{
    const std::string two =
        rpm(R"("above":3000)") + "," + throttle(R"("above":80)");
    /* all: false once one term is false, unjudged while none is false
     * and one can't be judged */
    Eval all(condition(0, R"({"all":[)" + two + "]}"));
    EXPECT_EQ(all.at(1, {{kRpm, 3500.0}}), "")
        << "true and unjudged: not judged";
    EXPECT_EQ(all.e->state_of(0).flags, ECUIO_CONDITION_STALE);
    EXPECT_EQ(all.at(2, {{kThrottle, 90.0}}), "2000000 c0 on fired t1 v90");
    EXPECT_EQ(all.at(3, {{kRpm, 1000.0}}), "3000000 c0 off t0 v1000");
    Eval all_false(condition(0, R"({"all":[)" + two + "]}"));
    EXPECT_EQ(all_false.at(1, {{kRpm, 1000.0}}), "1000000 c0 off t0 v1000")
        << "one false term settles all";

    /* any: true once one term is true, unjudged while none is true and
     * one can't be judged */
    Eval any(condition(0, R"({"any":[)" + two + "]}"));
    EXPECT_EQ(any.at(1, {{kRpm, 1000.0}}), "")
        << "false and unjudged: not judged";
    EXPECT_EQ(any.at(2, {{kThrottle, 90.0}}), "2000000 c0 on fired t1 v90");
    EXPECT_EQ(any.at(3, {{kThrottle, 10.0}}), "3000000 c0 off t1 v10");
    Eval any_true(condition(0, R"({"any":[)" + two + "]}"));
    EXPECT_EQ(any_true.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500")
        << "one true term settles any";

    /* Both of a frame's values are in before the join: one judgment */
    Eval both(condition(0, R"({"all":[)" + two + "]}"));
    EXPECT_EQ(both.at(1, {{kRpm, 3500.0}, {kThrottle, 90.0}}),
              "1000000 c0 on fired t1 v90");
}

TEST(ConditionEvalTest, AnOldInputKeepsTheStateAndIsStale)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    /* At its bound (3 s by default) the sample is still fresh */
    EXPECT_EQ(e.at(4, {{kSpeed, 1.0}}), "");
    /* Past it, stale at the bound's own time, the state and sample kept */
    EXPECT_EQ(e.at(4.000001, {{kSpeed, 1.0}}), "4000000 c0 on stale t0 v3500");
    EXPECT_EQ(e.at(60, {{kSpeed, 1.0}}), "") << "never OFF by age alone";
    ecuio_condition_state_t s = e.e->state_of(0);
    EXPECT_EQ(s.state, ECUIO_CONDITION_ON);
    EXPECT_EQ(s.flags, ECUIO_CONDITION_STALE);
    EXPECT_EQ(s.since_us, 4 * kSecond);
    /* Back to the state it had: an event, and no firing */
    EXPECT_EQ(e.at(61, {{kRpm, 3600.0}}), "61000000 c0 on t0 v3600");
    EXPECT_EQ(e.e->state_of(0).fired, 1u);
    /* Stale again, then back below the level: OFF with the stale gone.
     * After a 60 s gap the bound is its most, 180 s: stale at 241 s */
    EXPECT_EQ(e.at(240, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(e.at(250, {{kRpm, 100.0}}),
              "241000000 c0 on stale t0 v3600\n250000000 c0 off t0 v100");
}

TEST(ConditionEvalTest, ATermsOwnAgeBoundIsItsLimit)
{
    Eval e(condition(0, rpm(R"("above":3000,"max_age":0.5)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(1.5), "");
    EXPECT_EQ(e.at(1.6), "1500000 c0 on stale t0 v3500");
}

/* With no max_age a term's bound is its signal's pace: three times the
 * larger of the last gap and the gaps' running average (each a quarter,
 * each gap counted up to 60 s), from 3 s to 180 s. A sweep every 4 s
 * holds; when it stops, stale 12 s after its last sample. */
TEST(ConditionEvalTest, ASlowSweepHoldsBetweenItsSamplesAndStopsStale)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    /* Before a second sample the bound is 3 s */
    EXPECT_EQ(e.at(5, {{kRpm, 3500.0}}),
              "4000000 c0 on stale t0 v3500\n5000000 c0 on t0 v3500");
    for (int t = 9; t <= 41; t += 4) {
        EXPECT_EQ(e.at(t, {{kRpm, 3500.0}}), "") << "no flap at " << t;
    }
    EXPECT_EQ(e.at(53, {{kSpeed, 1.0}}), "") << "fresh at its bound";
    EXPECT_EQ(e.at(53.5, {{kSpeed, 1.0}}), "53000000 c0 on stale t0 v3500");
}

TEST(ConditionEvalTest, AFastSignalThatStopsIsStaleAtTheFloor)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    for (int i = 11; i <= 30; ++i) {
        EXPECT_EQ(e.at(i / 10.0, {{kRpm, 3500.0}}), "") << i;
    }
    EXPECT_EQ(e.at(6, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(e.at(6.05, {{kSpeed, 1.0}}), "6000000 c0 on stale t0 v3500")
        << "3 s after its last sample, not three of its 100 ms gaps";
}

/* A gap of 100 s counts as 60 s: the bound is three times that */
TEST(ConditionEvalTest, AGapCountsUpToAMinuteAndTheBoundToThreeMinutes)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(101, {{kRpm, 3500.0}}),
              "4000000 c0 on stale t0 v3500\n101000000 c0 on t0 v3500");
    EXPECT_EQ(e.at(281, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(e.at(282, {{kSpeed, 1.0}}), "281000000 c0 on stale t0 v3500");
}

/* Sweeps 60.005 s apart, past the longest minimum a focus takes: held
 * between samples (the bound 180 s), so a hold of 90 s completes, from
 * the judgment after the first sample's stale stretch */
TEST(ConditionEvalTest, ASweepOfAMinuteHoldsAndAHoldPastAMinuteCompletes)
{
    Eval e(condition(0, rpm(R"("above":3000)"), R"(,"for":90)"));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(e.at(61.005, {{kRpm, 3500.0}}),
              "4000000 c0 off stale t0 v3500\n61005000 c0 off t0 v3500");
    EXPECT_EQ(e.at(121.01, {{kRpm, 3500.0}}), "") << "no flap";
    EXPECT_EQ(e.at(181.015, {{kRpm, 3500.0}}),
              "151005000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(241.02, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(301.025, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(481.025, {{kSpeed, 1.0}}), "") << "fresh at its bound";
    EXPECT_EQ(e.at(481.03, {{kSpeed, 1.0}}), "481025000 c0 on stale t0 v3500");
}

/* A signal at 100 ms (1 s to 3 s), a 70 s outage, then back. The outage
 * counts as 60 s: at the first sample back the bound is 180 s; from the
 * next it is three times the average, which loses about a quarter with
 * each sample, here 19.25 s after three more (averages 11.33125 s,
 * 8.523437 s, 6.417577 s) */
TEST(ConditionEvalTest, AfterAnOutageItsBoundHoldsAndFallsWithEachSample)
{
    const auto before_outage = [](Eval& e) {
        EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
        for (int i = 11; i <= 30; ++i) {
            EXPECT_EQ(e.at(i / 10.0, {{kRpm, 3500.0}}), "") << i;
        }
        EXPECT_EQ(e.at(73, {{kRpm, 3500.0}}),
                  "6000000 c0 on stale t0 v3500\n73000000 c0 on t0 v3500");
    };
    Eval once(condition(0, rpm(R"("above":3000)")));
    before_outage(once);
    EXPECT_EQ(once.at(253, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(once.at(254, {{kSpeed, 1.0}}), "253000000 c0 on stale t0 v3500");

    Eval more(condition(0, rpm(R"("above":3000)")));
    before_outage(more);
    for (const double t : {73.1, 73.2, 73.3}) {
        EXPECT_EQ(more.at(t, {{kRpm, 3500.0}}), "") << t;
    }
    EXPECT_EQ(more.at(92.552731, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(more.at(93, {{kSpeed, 1.0}}), "92552731 c0 on stale t0 v3500");
}

/* A stated limit: a signal in bursts (three samples 50 ms apart every
 * 5 s) goes STALE every period, since each short gap takes a quarter off
 * the average. A term's own max_age holds it. */
TEST(ConditionEvalTest, ASignalInBurstsIsStaleEachPeriodUnlessItsTermSetsAnAge)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    Eval aged(condition(0, rpm(R"("above":3000,"max_age":6)")));
    const char* const stale[] = {"4100000", "9100000", "14159286", "19523444",
                                 "24677074"};
    for (int p = 0; p < 6; ++p) {
        const double start = 1 + 5 * p;
        const std::string first =
            p == 0 ? "1000000 c0 on fired t0 v3500"
                   : std::string(stale[p - 1]) + " c0 on stale t0 v3500\n" +
                         std::to_string(static_cast<int>(start) * kSecond) +
                         " c0 on t0 v3500";
        EXPECT_EQ(e.at(start, {{kRpm, 3500.0}}), first) << p;
        EXPECT_EQ(aged.at(start, {{kRpm, 3500.0}}),
                  p == 0 ? "1000000 c0 on fired t0 v3500" : "")
            << p;
        for (const double t : {start + 0.05, start + 0.1}) {
            EXPECT_EQ(e.at(t, {{kRpm, 3500.0}}), "") << t;
            EXPECT_EQ(aged.at(t, {{kRpm, 3500.0}}), "") << t;
        }
    }
}

/* Gaps of 0.5 s and 5 s in turn: the last gap alone would bound the long
 * one by 3 s after each short one; the average holds it once it has
 * learned the pace (two turns, since the first gap is a short one) */
TEST(ConditionEvalTest, GapsThatAlternateAreHeldByTheirAverage)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(1.5, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(6.5, {{kRpm, 3500.0}}),
              "4500000 c0 on stale t0 v3500\n6500000 c0 on t0 v3500");
    EXPECT_EQ(e.at(7, {{kRpm, 3500.0}}), "");
    /* The average 1.34375 s: bound 4.03125 s from 7 s */
    EXPECT_EQ(e.at(12, {{kRpm, 3500.0}}),
              "11031250 c0 on stale t0 v3500\n12000000 c0 on t0 v3500");
    for (double t = 12; t < 50; t += 5.5) {
        EXPECT_EQ(e.at(t + 0.5, {{kRpm, 3500.0}}), "") << t + 0.5;
        EXPECT_EQ(e.at(t + 5.5, {{kRpm, 3500.0}}), "") << t + 5.5;
    }
}

/* From 100 ms to 4 s: stale once at the first long gap, which nothing
 * foretold, then the last gap holds the next; the average alone would
 * still be learning */
TEST(ConditionEvalTest, ASignalThatSlowsIsStaleOnceThenFollows)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    for (int i = 10; i <= 30; ++i) {
        EXPECT_EQ(e.at(i / 10.0, {{kRpm, 3500.0}}),
                  i == 10 ? "1000000 c0 on fired t0 v3500" : "")
            << i;
    }
    EXPECT_EQ(e.at(7, {{kRpm, 3500.0}}),
              "6000000 c0 on stale t0 v3500\n7000000 c0 on t0 v3500");
    EXPECT_EQ(e.at(11, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(15, {{kRpm, 3500.0}}), "");
}

/* A pace that shortens moves the deadline earlier: 4 s sweeps, then three
 * samples 0.5 s apart and none after. The averages 3.125 s, 2.46875 s and
 * 1.976562 s give a bound of 5.929686 s from 10.5 s. Each condition
 * reading the signal moves. */
TEST(ConditionEvalTest, AShorterPaceMovesTheDeadlineEarlier)
{
    Eval e(condition(0, rpm(R"("above":3000)")) + "," +
           condition(1, rpm(R"("below":9000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}),
              "1000000 c0 on fired t0 v3500\n1000000 c1 on fired t0 v3500");
    EXPECT_EQ(e.at(5, {{kRpm, 3500.0}}),
              "4000000 c0 on stale t0 v3500\n4000000 c1 on stale t0 v3500\n"
              "5000000 c0 on t0 v3500\n5000000 c1 on t0 v3500");
    for (const double t : {9.0, 9.5, 10.0, 10.5}) {
        EXPECT_EQ(e.at(t, {{kRpm, 3500.0}}), "") << t;
    }
    EXPECT_EQ(e.at(16.4, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(e.at(16.5, {{kSpeed, 1.0}}),
              "16429686 c0 on stale t0 v3500\n16429686 c1 on stale t0 v3500")
        << "before the 17 s its sweep had set";
}

/* A term's own max_age is its bound whatever its signal's pace */
TEST(ConditionEvalTest, ATermsOwnAgeBoundOverridesThePace)
{
    Eval e(condition(0, rpm(R"("above":3000,"max_age":1)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    for (int t = 5; t <= 13; t += 4) {
        EXPECT_EQ(e.at(t, {{kRpm, 3500.0}}), std::to_string((t - 3) * kSecond) +
                                                 " c0 on stale t0 v3500\n" +
                                                 std::to_string(t * kSecond) +
                                                 " c0 on t0 v3500")
            << t;
    }
}

TEST(ConditionEvalTest, AHoldStartsOverAcrossAStaleStretch)
{
    Eval e(condition(0, rpm(R"("above":3000)"), R"(,"for":5)"));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    /* The sample ages out at 4 s, a second before the hold would end */
    EXPECT_EQ(e.at(7, {{kRpm, 3500.0}}),
              "4000000 c0 off stale t0 v3500\n7000000 c0 off t0 v3500");
    EXPECT_EQ(e.at(9, {{kRpm, 3500.0}}), "") << "no ON at 6 s";
    EXPECT_EQ(e.at(11.5, {{kRpm, 3500.0}}), "");
    EXPECT_EQ(e.at(12.5, {{kRpm, 3500.0}}), "12000000 c0 on fired t0 v3500")
        << "five seconds from the judgment after the stale stretch";
}

TEST(ConditionEvalTest, ATermKeepsItsTruthAcrossAStaleStretch)
{
    Eval e(condition(0, rpm(R"("above":100,"clear":90)")));
    EXPECT_EQ(e.at(1, {{kRpm, 101.0}}), "1000000 c0 on fired t0 v101");
    /* Stale, then a sample inside the band: ON as before, no firing */
    EXPECT_EQ(e.at(10, {{kRpm, 95.0}}),
              "4000000 c0 on stale t0 v101\n10000000 c0 on t0 v95");
    EXPECT_EQ(e.e->state_of(0).fired, 1u);
}

TEST(ConditionEvalTest, OneFrameGivesAConditionThreeEventsAtMostInTimeOrder)
{
    Eval e(condition(0, rpm(R"("above":3000,"max_age":2)"), R"(,"for":1)"));
    EXPECT_EQ(e.at(10, {{kRpm, 3500.0}}), "10000000 c0 off t0 v3500");
    /* The hold's end, the age bound, then the frame's own judgment */
    EXPECT_EQ(e.at(20, {{kRpm, 100.0}}), "11000000 c0 on fired t0 v3500\n"
                                         "12000000 c0 on stale t0 v3500\n"
                                         "20000000 c0 off t0 v100");
    EXPECT_EQ(e.e->event_count, ecuio_condition_eval::kEventsPerCondition);
    EXPECT_EQ(e.e->events.size(), size_t{3})
        << "the buffer is exactly the bound";
    EXPECT_EQ(e.e->dropped, 0u);
}

TEST(ConditionEvalTest, ChangesAtOneTimeAreOneEvent)
{
    /* The hold's end and the age bound fall at 4 s: the hold first */
    Eval e(condition(0, rpm(R"("above":3000)"), R"(,"for":3)"));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 off t0 v3500");
    EXPECT_EQ(e.at(30), "4000000 c0 on stale fired t0 v3500");
    EXPECT_EQ(e.e->state_of(0).fired, 1u);
}

TEST(ConditionEvalTest, EventsOfSeveralConditionsComeByTimeThenByIndex)
{
    Eval e(condition(0, rpm(R"("above":3000)"), R"(,"for":2)") + "," +
           condition(1, rpm(R"("above":3000)"), R"(,"for":1)") + "," +
           condition(2, rpm(R"("below":5000)")) + "," +
           condition(3, throttle(R"("above":50)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}),
              "1000000 c0 off t0 v3500\n1000000 c1 off t0 v3500\n"
              "1000000 c2 on fired t0 v3500");
    EXPECT_EQ(e.at(3.5, {{kThrottle, 60.0}, {kRpm, 6000.0}}),
              "2000000 c1 on fired t0 v3500\n3000000 c0 on fired t0 v3500\n"
              "3500000 c2 off t0 v6000\n3500000 c3 on fired t0 v60");
}

TEST(ConditionEvalTest, AConditionThatCantBindIsNeverJudged)
{
    Eval e(condition(0, rpm(R"("above":3000)")) + "," +
           condition(1, R"({"signal":["obd2.rpm"],"unit":"krpm","above":3})") +
           "," +
           condition(2, R"({"any":[)" + rpm(R"("above":1)") +
                            R"(,{"link":"not_ok"}]})") +
           "," + condition(3, R"({"check":"gap"})") + "," +
           condition(4, rpm(R"("above":1)"), R"(,"enabled":false)") + "," +
           condition(5, R"({"signal":["key:none"],"equals":1})"));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(60, {{kRpm, 3500.0}}),
              "4000000 c0 on stale t0 v3500\n60000000 c0 on t0 v3500");
    const uint32_t whys[] = {
        ECUIO_CONDITION_WHY_UNIT, ECUIO_CONDITION_WHY_NO_LINK,
        ECUIO_CONDITION_WHY_NO_CHECK, ECUIO_CONDITION_WHY_DISABLED,
        ECUIO_CONDITION_WHY_NO_SIGNAL};
    for (uint32_t i = 1; i < 6; ++i) {
        const ecuio_condition_state_t s = e.e->state_of(i);
        EXPECT_EQ(s.flags, ECUIO_CONDITION_UNAVAILABLE) << i;
        EXPECT_EQ(s.why, whys[i - 1]) << i;
        EXPECT_EQ(s.state, ECUIO_CONDITION_OFF) << i;
        EXPECT_EQ(s.fired, 0u) << i;
    }
    EXPECT_EQ(e.e->events.size(), size_t{3}) << "room for the one that binds";
}

TEST(ConditionEvalTest, TextAndUnwatchedValuesAreSkipped)
{
    Eval e(condition(0, rpm(R"("above":3000)")));
    ecuio_value_t batch[3] = {};
    batch[0].signal_id = kRpm;
    batch[0].kind = ECUIO_VALUE_TEXT;
    batch[0].value = 9000.0;
    batch[1].signal_id = kRpm;
    batch[1].kind = ECUIO_VALUE_HEX;
    batch[1].value = 9000.0;
    batch[2].signal_id = kCoolant;
    batch[2].kind = ECUIO_VALUE_UINT;
    batch[2].value = 9000.0;
    EXPECT_EQ(e.e->judge(kSecond, batch, 3), 0u);
    EXPECT_EQ(e.e->state_of(0).flags, ECUIO_CONDITION_STALE);
    /* An enumeration's value is its raw number */
    batch[0].kind = ECUIO_VALUE_ENUM;
    EXPECT_EQ(e.e->judge(2 * kSecond, batch, 1), 1u);
    EXPECT_EQ(e.e->events[0].state, ECUIO_CONDITION_ON);
}

TEST(ConditionEvalTest, TwoTermsOnOneSignalAreBothFed)
{
    Eval e(condition(0, R"({"any":[)" + rpm(R"("below":500)") + "," +
                            rpm(R"("above":6500)") + "]}") +
           "," + condition(1, rpm(R"("above":6000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3000.0}}),
              "1000000 c0 off t0 v3000\n1000000 c1 off t0 v3000");
    EXPECT_EQ(e.at(2, {{kRpm, 7000.0}}),
              "2000000 c0 on fired t1 v7000\n2000000 c1 on fired t0 v7000");
    EXPECT_EQ(e.at(3, {{kRpm, 100.0}}), "3000000 c1 off t0 v100")
        << "c0 goes from one true term to the other: no change";
    EXPECT_EQ(e.e->state_of(0).state, ECUIO_CONDITION_ON);
}

TEST(ConditionEvalTest, TheSameFramesGiveTheSameEventsHoweverTheyAreSplit)
{
    /* Deadlines are settled by whichever frame comes next, at their own
     * times: frames in between that carry nothing for the condition
     * change nothing */
    const std::string set = condition(0, rpm(R"("above":3000,"max_age":2)"),
                                      R"(,"for":1,"clear_for":1)");
    Eval sparse(set);
    Eval dense(set);
    std::string a;
    std::string b;
    const auto step = [](Eval& e, std::string& into, double t,
                         std::initializer_list<std::pair<uint16_t, double>> v) {
        const std::string got = e.at(t, v);
        if (!got.empty()) {
            into += got + "\n";
        }
    };
    for (const double t : {1.0, 9.0, 10.0, 20.0}) {
        step(sparse, a, t, {{kRpm, t < 15 ? 3500.0 : 100.0}});
    }
    for (int i = 10; i <= 200; ++i) {
        const double t = i / 10.0;
        if (t == 1.0 || t == 9.0 || t == 10.0 || t == 20.0) {
            step(dense, b, t, {{kRpm, t < 15 ? 3500.0 : 100.0}});
        } else {
            step(dense, b, t, {{kSpeed, t}});
        }
    }
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, "1000000 c0 off t0 v3500\n2000000 c0 on fired t0 v3500\n"
                 "3000000 c0 on stale t0 v3500\n9000000 c0 on t0 v3500\n"
                 "12000000 c0 on stale t0 v3500\n20000000 c0 on t0 v100\n");
}

TEST(ConditionEvalTest, TheLargestSetJudgesWithinItsBuffer)
{
    std::string all;
    for (int i = 0; i < static_cast<int>(ECUIO_CONDITION_MAX_CONDITIONS); ++i) {
        const std::string four =
            rpm(R"("above":)" + std::to_string(1000 + i)) + "," +
            throttle(R"("above":)" + std::to_string(i)) + "," +
            rpm(R"("below":9000)") + "," + throttle(R"("differs":255)");
        all += std::string(i == 0 ? "" : ",") +
               condition(i, R"({"all":[)" + four + "]}", R"(,"for":1)");
    }
    Eval e(all);
    ASSERT_EQ(e.e->conditions.size(), size_t{ECUIO_CONDITION_MAX_CONDITIONS});
    EXPECT_EQ(e.e->terms.size(), size_t{256});
    EXPECT_EQ(e.e->events.size(), size_t{192});
    /* What a session holds for the largest set */
    EXPECT_LE(e.e->bytes(), size_t{48} * 1024);
    uint32_t most = 0;
    for (int f = 0; f < 4000; ++f) {
        const double t = 1.0 + f * 0.37;
        const double r = (f * 977) % 9000;
        const double th = (f * 31) % 100;
        if (f % 7 == 3) {
            e.at(t);
        } else if (f % 2 == 0) {
            e.at(t, {{kRpm, r}, {kThrottle, th}});
        } else {
            e.at(t, {{kRpm, r}});
        }
        most = e.e->event_count > most ? e.e->event_count : most;
    }
    EXPECT_GT(most, 64u) << "the run crosses many conditions in one frame";
    EXPECT_EQ(e.e->dropped, 0u);
}

TEST(ConditionEvalTest, AValueThatIsNoNumberIsNoSample)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    Eval e(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(e.at(1, {{kRpm, 3500.0}}), "1000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(2, {{kRpm, nan}}), "");
    /* The age runs on from the number before it */
    EXPECT_EQ(e.at(4.5, {{kRpm, nan}}), "4000000 c0 on stale t0 v3500");
    /* And the next number is judged against the level as any other */
    EXPECT_EQ(e.at(5, {{kRpm, 100.0}}), "5000000 c0 off t0 v100");

    Eval never(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(never.at(1, {{kRpm, nan}}), "");
    EXPECT_EQ(never.e->state_of(0).flags, ECUIO_CONDITION_STALE)
        << "nothing was judged";
    EXPECT_EQ(never.at(2, {{kRpm, 3500.0}}), "2000000 c0 on fired t0 v3500");

    /* An infinity is a number on one side of every level */
    Eval endless(condition(0, rpm(R"("above":3000)")));
    EXPECT_EQ(endless.at(1, {{kRpm, inf}}), "1000000 c0 on fired t0 vinf");
    EXPECT_EQ(endless.at(2, {{kRpm, -inf}}), "2000000 c0 off t0 v-inf");
    EXPECT_EQ(endless.at(3, {{kRpm, 3500.0}}), "3000000 c0 on fired t0 v3500");
}

/* Two channels' frames can arrive a little out of order */
TEST(ConditionEvalTest, AFrameStampedEarlierIsJudgedAtTheNewestTime)
{
    Eval e(condition(0, rpm(R"("above":3000,"max_age":1)")) + "," +
           condition(1, throttle(R"("above":80)"), R"(,"for":1)"));
    EXPECT_EQ(e.at(2, {{kRpm, 3500.0}}), "2000000 c0 on fired t0 v3500");
    EXPECT_EQ(e.at(1.5, {{kRpm, 100.0}}), "2000000 c0 off t0 v100")
        << "judged, at the time already reached";
    EXPECT_EQ(e.e->state_of(0).since_us, 2 * kSecond);
    /* Its sample is as new as that time: stale a second after it */
    EXPECT_EQ(e.at(2.9, {{kSpeed, 1.0}}), "");
    EXPECT_EQ(e.at(3.1, {{kSpeed, 1.0}}), "3000000 c0 off stale t0 v100");
    /* A hold begun by an early frame runs from the time reached */
    EXPECT_EQ(e.at(1, {{kThrottle, 90.0}}), "3100000 c1 off t0 v90");
    EXPECT_EQ(e.at(4, {{kThrottle, 90.0}}), "");
    EXPECT_EQ(e.at(4.2, {{kThrottle, 90.0}}), "4100000 c1 on fired t0 v90");
}
