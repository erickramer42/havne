#include "session_operation_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using engine_talk_test::Bytes;
using session_operation::Answer;
using session_operation::Bench;

constexpr uint32_t kVolts = ECUIO_OPERATION_CHECK_VOLTS;
constexpr uint32_t kSpeeds =
    ECUIO_OPERATION_CHECK_RPM | ECUIO_OPERATION_CHECK_KMH;

TEST(SessionOperationTest, TheBatteryIsReadAtTheStartAndThroughTheRun)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery = [](uint32_t nth) { return nth == 1 ? 12600u : 12350u; };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.checks, kSpeeds | kVolts);
    EXPECT_EQ(v.volts_mv, 12350u);
    EXPECT_EQ(v.volts_min_mv, 12350u);
    EXPECT_GE(b.battery_reads, 3u) << "the start and each step's start";
    for (const Bytes& q : b.sent) {
        EXPECT_FALSE(q == (Bytes{0x01, 0x40}) || q == (Bytes{0x01, 0x42}))
            << "the adapter read it: the module isn't asked";
    }
}

TEST(SessionOperationTest, ALowOrHighBatteryAtTheStartWarnsAndTheRunGoesOn)
{
    const struct {
        uint32_t mv;
        uint32_t warning;
    } cases[] = {
        {11799, ECUIO_OPERATION_CHECK_VOLTS_LOW},
        {11800, 0},
        {15000, 0},
        {15001, ECUIO_OPERATION_CHECK_VOLTS_HIGH},
        {16400, ECUIO_OPERATION_CHECK_VOLTS_HIGH},
    };
    for (const auto& c : cases) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        const uint32_t mv = c.mv;
        b.battery = [mv](uint32_t) { return mv; };
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << c.mv << ": " << v.words;
        EXPECT_EQ(v.checks, kSpeeds | kVolts | c.warning) << c.mv;
        EXPECT_EQ(v.volts_mv, c.mv);
    }
}

/* Under 11.0 V through a dump warns and the read goes on; 11.0 V itself
 * warns nothing */
TEST(SessionOperationTest, ADropUnderElevenVoltsWarnsAndTheReadGoesOn)
{
    for (const uint32_t low : {10999u, 11000u}) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.battery = [low](uint32_t nth) { return nth < 3 ? 12600u : low; };
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << low << ": " << v.words;
        EXPECT_EQ(v.checks & ECUIO_OPERATION_CHECK_VOLTS_SAG,
                  low < 11000u ? ECUIO_OPERATION_CHECK_VOLTS_SAG : 0u)
            << low;
        EXPECT_EQ(v.checks & ECUIO_OPERATION_CHECK_VOLTS_LOW, 0u)
            << "the start's warning is its own";
        EXPECT_EQ(v.volts_min_mv, low);
        EXPECT_EQ(b.files().size(), 1u);
    }
}

/* The start's reading warns at any voltage: the run's own readings judge */
TEST(SessionOperationTest, AStartUnderTheFloorOnlyWarns)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery = [](uint32_t nth) { return nth == 1 ? 10900u : 12600u; };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_VOLTS_LOW, 0u);
    EXPECT_EQ(v.volts_min_mv, 10900u);
}

TEST(SessionOperationTest, NoAdapterReadingAsksTheEngineModulesOwn)
{
    for (const bool source : {true, false}) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.battery_source = source;
        b.battery = [](uint32_t) { return 0u; };
        const size_t from = b.sent.size();
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_EQ(v.checks,
                  kSpeeds | kVolts | ECUIO_OPERATION_CHECK_VOLTS_FROM_ECU);
        EXPECT_EQ(v.volts_mv, 12600u) << "the parked sim's 01 42";
        std::vector<Bytes> checks;
        for (size_t i = from; i < b.sent.size(); ++i) {
            if (b.sent[i][0] == 0x01 && b.sent[i].size() == 2) {
                checks.push_back(b.sent[i]);
            }
        }
        EXPECT_EQ(checks,
                  (std::vector<Bytes>{
                      {0x01, 0x0C}, {0x01, 0x0D}, {0x01, 0x40}, {0x01, 0x42}}));
    }
}

/* The checks wait for the adapter's answer before they judge whether
 * the module is asked its own */
TEST(SessionOperationTest, TheChecksWaitForTheAdaptersAnswer)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery = [](uint32_t) { return 0u; };
    b.battery_held = 12;
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_VOLTS_FROM_ECU, 0u);
}

/* A module that doesn't list 42 isn't asked it; the run goes on with no
 * voltage at all */
TEST(SessionOperationTest, NoReadingAnywhereLetsTheRunGoOn)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery_source = false;
    b.script = [](const Bytes& q, size_t, Answer* a) {
        if (q != Bytes{0x01, 0x40}) {
            return false;
        }
        a->frames = {Bytes{0x41, 0x40, 0x80, 0x00, 0x00, 0x00}};
        return true;
    };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.checks, kSpeeds);
    EXPECT_EQ(v.volts_mv, 0u);
    for (const Bytes& q : b.sent) {
        EXPECT_NE(q, (Bytes{0x01, 0x42}));
    }
}

} // namespace
