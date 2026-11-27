#include "awake_seam.h"
#include "session_operation_support.h"

#include "../core/src/host_power.h"
#include "../core/src/operation_checks.h"

#include <ecuio/internal.h>

#include <gtest/gtest.h>

#include <atomic>
#include <string>

namespace {

using engine_talk_test::Bytes;
using session_operation::Bench;
using session_operation::request;

constexpr uint32_t kMay =
    ECUIO_OPERATION_MAY_UNLOCK | ECUIO_OPERATION_BATTERY_SUPPORT;

/* Every start judged write-class while it lives */
struct WriteClass {
    WriteClass() { ecuio_test_operation_write_class(1); }
    ~WriteClass() { ecuio_test_operation_write_class(0); }
    WriteClass(const WriteClass&) = delete;
    WriteClass& operator=(const WriteClass&) = delete;
};

/* The host's power as a case sets it */
struct HostPower {
    static inline std::atomic<int> state{1};
    static inline std::atomic<int> percent{-1};
    HostPower(int s, int p)
    {
        state = s;
        percent = p;
        ecuio_test_set_host_power(&read);
    }
    ~HostPower() { ecuio_test_set_host_power(nullptr); }
    HostPower(const HostPower&) = delete;
    HostPower& operator=(const HostPower&) = delete;
    static int read(int* out)
    {
        *out = percent;
        return state;
    }
};

ecuio_status_t start_write(Bench& b, uint32_t flags = kMay)
{
    const ecuio_operation_request_t q = request(flags);
    return ecuio_session_operation_start(b.s, &q, b.folder.str().c_str());
}

TEST(HostPowerTest, TheSuppliesSayMainsOrTheBatterysPercent)
{
    using host_power::supply;
    const supply laptop_off[] = {{"Mains", 0, -1}, {"Battery", -1, 22}};
    host_power::reading r = host_power::judge_supplies(laptop_off, 2);
    EXPECT_TRUE(r.known);
    EXPECT_FALSE(r.mains);
    EXPECT_EQ(r.percent, 22);
    const supply laptop_on[] = {{"Battery", -1, 22}, {"Mains", 1, -1}};
    r = host_power::judge_supplies(laptop_on, 2);
    EXPECT_TRUE(r.known && r.mains);
    const supply usb_c[] = {{"USB", 1, -1}, {"Battery", -1, 50}};
    EXPECT_TRUE(host_power::judge_supplies(usb_c, 2).mains);
    const supply desktop[] = {{"Mains", 0, -1}};
    r = host_power::judge_supplies(desktop, 1);
    EXPECT_TRUE(r.known && r.mains) << "no battery: on mains";
    EXPECT_FALSE(host_power::judge_supplies(nullptr, 0).known);
    const supply two[] = {{"Battery", -1, 80}, {"Battery", -1, 10}};
    EXPECT_EQ(host_power::judge_supplies(two, 2).percent, 10)
        << "the emptier battery";
}

TEST(HostPowerTest, AWriteStopsOffMainsUnderItsStopPercent)
{
    host_power::reading r;
    r.known = true;
    r.mains = false;
    r.percent = host_power::kWriteStopPercent - 1;
    EXPECT_TRUE(host_power::write_stops(r));
    r.percent = host_power::kWriteStopPercent;
    EXPECT_FALSE(host_power::write_stops(r));
    EXPECT_TRUE(host_power::off_mains(r));
    r.percent = -1;
    EXPECT_FALSE(host_power::write_stops(r)) << "no percentage: warn only";
    r.mains = true;
    r.percent = 5;
    EXPECT_FALSE(host_power::write_stops(r));
    EXPECT_FALSE(host_power::off_mains(r));
    r.known = false;
    r.mains = false;
    EXPECT_FALSE(host_power::write_stops(r));
    EXPECT_FALSE(host_power::off_mains(r));
}

TEST(SessionOperationTest, AWriteNeedsBatterySupportAndAVoltageSource)
{
    const WriteClass write;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    EXPECT_EQ(start_write(b, ECUIO_OPERATION_MAY_UNLOCK),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "connect battery support (a charger in its power-supply "
                 "mode) and confirm it first");
    b.s->battery.source = false;
    EXPECT_EQ(start_write(b), ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "this adapter reads no battery voltage: a write needs one");
    EXPECT_EQ(b.state().generation, 0u);
    EXPECT_TRUE(b.files().empty());
}

TEST(SessionOperationTest, ADumpNeedsNoBatterySupport)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.s->battery.source = false;
    b.battery_source = false;
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_SAVED);
}

/* A write's start: under 11.5 V refuses, to 12.0 V warns, above 16.0 V
 * refuses; tests name the floor's constants, never its numbers */
TEST(SessionOperationTest, AWritesBatteryIsJudgedOnceAtItsStart)
{
    using ecuio_operation::kWriteRefuseHighMv;
    using ecuio_operation::kWriteStartFloorMv;
    using ecuio_operation::kWriteWarnMv;
    const WriteClass write;
    const struct {
        uint32_t mv;
        uint32_t end;
        uint32_t warning;
    } cases[] = {
        {kWriteStartFloorMv - 1, ECUIO_OPERATION_END_VOLTAGE_LOW, 0},
        {kWriteStartFloorMv, ECUIO_OPERATION_END_SAVED,
         ECUIO_OPERATION_CHECK_VOLTS_LOW},
        {kWriteWarnMv - 1, ECUIO_OPERATION_END_SAVED,
         ECUIO_OPERATION_CHECK_VOLTS_LOW},
        {kWriteWarnMv, ECUIO_OPERATION_END_SAVED, 0},
        {kWriteRefuseHighMv, ECUIO_OPERATION_END_SAVED,
         ECUIO_OPERATION_CHECK_VOLTS_HIGH},
        {kWriteRefuseHighMv + 1, ECUIO_OPERATION_END_VOLTAGE_HIGH, 0},
    };
    for (const auto& c : cases) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        const uint32_t mv = c.mv;
        b.battery = [mv](uint32_t) { return mv; };
        const size_t from = b.sent.size();
        ASSERT_EQ(start_write(b), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, c.end) << c.mv << ": " << v.words;
        if (c.end == ECUIO_OPERATION_END_SAVED) {
            EXPECT_EQ(v.checks & (ECUIO_OPERATION_CHECK_VOLTS_LOW |
                                  ECUIO_OPERATION_CHECK_VOLTS_HIGH),
                      c.warning)
                << c.mv;
        } else {
            EXPECT_EQ(b.count(0x27, from) + b.count(0x23, from), 0u);
            EXPECT_TRUE(b.files().empty());
        }
    }
}

TEST(SessionOperationTest, AWritesStartRefusalsSayWhy)
{
    const WriteClass write;
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.battery = [](uint32_t) { return 11300u; };
        ASSERT_EQ(start_write(b), ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_STREQ(b.until_ended().words,
                     "the battery reads 11.3 V at the adapter, under the "
                     "11.5 V a write needs: connect a charger; nothing was "
                     "read");
    }
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.battery = [](uint32_t) { return 16400u; };
        ASSERT_EQ(start_write(b), ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_STREQ(b.until_ended().words,
                     "the battery reads 16.4 V at the adapter: set the "
                     "charger to its power-supply mode (24 V systems aren't "
                     "supported); nothing was read");
    }
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery = [](uint32_t) { return 0u; };
    const size_t from = b.sent.size();
    ASSERT_EQ(start_write(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_VOLTAGE_LOW);
    EXPECT_STREQ(v.words, "no battery reading at the adapter: a write needs "
                          "one; nothing was read");
    for (size_t i = from; i < b.sent.size(); ++i) {
        EXPECT_NE(b.sent[i], (Bytes{0x01, 0x40}))
            << "a write takes no module's reading";
    }
}

/* Through a write nothing stops on voltage */
TEST(SessionOperationTest, AWritesSagThroughTheRunGoesOn)
{
    const WriteClass write;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery = [](uint32_t nth) { return nth < 3 ? 12600u : 9800u; };
    ASSERT_EQ(start_write(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.volts_min_mv, 9800u);
}

TEST(SessionOperationTest, TheHostsPowerStopsAWriteAndWarnsADump)
{
    {
        const HostPower power(0, 9);
        const WriteClass write;
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        EXPECT_EQ(start_write(b), ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "plug the computer in: it runs on its battery at 9%");
        EXPECT_EQ(b.state().generation, 0u);
    }
    {
        const HostPower power(0, 10);
        const WriteClass write;
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        ASSERT_EQ(start_write(b), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_ON_BATTERY, 0u);
    }
    for (const int state : {0, -1, 1}) {
        const HostPower power(state, 5);
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_EQ((v.checks & ECUIO_OPERATION_CHECK_ON_BATTERY) != 0,
                  state == 0)
            << state;
    }
}

TEST(SessionOperationTest, ARunHoldsTheHostAwakeUntilItsEnd)
{
    const AwakeSeam seam;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.s->info.backend = ECUIO_BACKEND_J2534_SURROGATE;
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::last_level.load(), 1u) << "the sleep level";
    EXPECT_EQ(AwakeSeam::releases.load(), 0);
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.checks & ECUIO_OPERATION_CHECK_NOT_HELD_AWAKE, 0u);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
}

TEST(SessionOperationTest, AHoldRefusedWarnsADumpAndStopsAWrite)
{
    const AwakeSeam seam;
    AwakeSeam::refuse_with("the host can't be held awake here");
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.s->info.backend = ECUIO_BACKEND_J2534_SURROGATE;
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_STREQ(ecuio_last_error_detail(), "");
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_NOT_HELD_AWAKE, 0u);
    }
    const WriteClass write;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.s->info.backend = ECUIO_BACKEND_J2534_SURROGATE;
    EXPECT_NE(start_write(b), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the host can't be held awake here");
    EXPECT_EQ(b.state().generation, 0u);
}

/* A refusal after the hold was taken gives it back */
TEST(SessionOperationTest, ARefusalAfterTheHoldLetsItGo)
{
    const AwakeSeam seam;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.s->info.backend = ECUIO_BACKEND_J2534_SURROGATE;
    const ecuio_operation_request_t q = request(ECUIO_OPERATION_MAY_UNLOCK);
    EXPECT_EQ(
        ecuio_session_operation_start(b.s, &q, b.folder.at("none").c_str()),
        ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
}

} // namespace
