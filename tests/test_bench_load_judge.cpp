// File/layer: tests/test_bench_load_judge.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: BenchLoadJudgeTest (test_core)

#include <gtest/gtest.h>

#include "bench_load_judge.h"

using bench_judge::best;
using bench_judge::kRecordedLoopShareLinux;
using bench_judge::kRecordedLoopShareOther;
using bench_judge::recordedShare;
using bench_judge::recordedShareHolds;

namespace {

// A typical Linux runner: about 330,000 frames a second unrecorded, the
// recorded run at the 0.86 median of CI's linux-build runs
const std::vector<double> kUnrecorded = {330000.0, 322000.0, 328000.0};

std::vector<double> recordedAt(double share)
{
    return {330000.0 * share, 322000.0 * share * 0.97, 328000.0 * share * 0.98};
}

}  // namespace

TEST(BenchLoadJudgeTest, TheFloorsAreTheOnesTheirCommentDerives)
{
    EXPECT_DOUBLE_EQ(kRecordedLoopShareLinux, 0.66);
    EXPECT_DOUBLE_EQ(kRecordedLoopShareOther, 0.6);
#if defined(__linux__) && !defined(__ANDROID__)
    EXPECT_DOUBLE_EQ(bench_judge::minRecordedLoopShare(), kRecordedLoopShareLinux);
#else
    EXPECT_DOUBLE_EQ(bench_judge::minRecordedLoopShare(), kRecordedLoopShareOther);
#endif
}

TEST(BenchLoadJudgeTest, ATypicalAndALoadedLinuxRunnerPass)
{
    EXPECT_TRUE(recordedShareHolds(recordedShare(recordedAt(0.86), kUnrecorded),
                                   kRecordedLoopShareLinux));
    // The lowest single run CI has read, 0.69, and a loaded runner's 0.67
    // before its best of three
    EXPECT_TRUE(recordedShareHolds(recordedShare(recordedAt(0.69), kUnrecorded),
                                   kRecordedLoopShareLinux));
    EXPECT_TRUE(recordedShareHolds(recordedShare({0.67 * 330000.0, 0.69 * 330000.0, 0.68 * 330000.0},
                                                 kUnrecorded),
                                   kRecordedLoopShareLinux));
}

TEST(BenchLoadJudgeTest, ALossOfAQuarterFromTheMedianTripsLinuxButNotTheOtherFloor)
{
    // 0.86 less a quarter is 0.645
    const double share = recordedShare(recordedAt(0.86 * 0.75), kUnrecorded);
    EXPECT_FALSE(recordedShareHolds(share, kRecordedLoopShareLinux));
    EXPECT_TRUE(recordedShareHolds(share, kRecordedLoopShareOther));
}

TEST(BenchLoadJudgeTest, ALossOfAFifthFromTheMedianPasses)
{
    EXPECT_TRUE(recordedShareHolds(recordedShare(recordedAt(0.86 * 0.85), kUnrecorded),
                                   kRecordedLoopShareLinux));
}

TEST(BenchLoadJudgeTest, EachSideTakesItsBestRun)
{
    EXPECT_DOUBLE_EQ(best({100.0, 300.0, 200.0}), 300.0);
    // One slow recorded run among three doesn't fail the check, and one
    // slow unrecorded run doesn't lift it
    EXPECT_TRUE(recordedShareHolds(recordedShare({150000.0, 290000.0, 140000.0}, kUnrecorded),
                                   kRecordedLoopShareLinux));
    EXPECT_FALSE(recordedShareHolds(recordedShare({150000.0, 160000.0, 140000.0},
                                                  {330000.0, 150000.0, 150000.0}),
                                    kRecordedLoopShareLinux));
}

TEST(BenchLoadJudgeTest, TheShareAtTheFloorHoldsAndNoUnrecordedRateFails)
{
    EXPECT_TRUE(recordedShareHolds(0.66, 0.66));
    EXPECT_FALSE(recordedShareHolds(0.6599, 0.66));
    EXPECT_DOUBLE_EQ(recordedShare({1.0}, {}), 0.0);
    EXPECT_DOUBLE_EQ(recordedShare({1.0}, {0.0}), 0.0);
    EXPECT_FALSE(recordedShareHolds(recordedShare({1.0}, {}), kRecordedLoopShareOther));
}
