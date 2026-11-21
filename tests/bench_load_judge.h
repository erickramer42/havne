// File/layer: tests/bench_load_judge.h | header-only, standard library only, C++17
// Testing: UNIT: BenchLoadJudgeTest (test_core, tests/test_bench_load_judge.cpp)
//
// The recorded-loop check of bench_bridge --load (tests/bench_bridge_load.h):
// the share of the looping run's rate that survives core's recorder, judged
// on the best of three recorded runs against the best of three unrecorded
// ones, and the floor it must reach.

#pragma once

#include <algorithm>
#include <vector>

namespace bench_judge {

// CI's linux-build read 0.69 to 0.97 over 50 runs (median 0.86, tenth
// percentile 0.76), each the faster of two runs a side, and a loaded runner
// has read 0.67. The 0.66 floor sits 3 points under the lowest of those
// runs, so a loaded runner doesn't fail it, and it trips when the best of
// three falls about 23% under the median: about 1 us a frame at the
// 330,000 frames a second an unrecorded run reaches there.
constexpr double kRecordedLoopShareLinux = 0.66;
// Windows' CI runners read 0.68 to 0.95 (median 0.80) over 63 runs: a
// 25% loss from a typical figure is about 2 us a frame there (220,000
// frames a second unrecorded), 1 us on its fastest runner
constexpr double kRecordedLoopShareOther = 0.6;

constexpr double minRecordedLoopShare()
{
#if defined(__linux__) && !defined(__ANDROID__)
    return kRecordedLoopShareLinux;
#else
    return kRecordedLoopShareOther;
#endif
}

// A runner's noise only slows a run, so each side's best run is its figure
inline double best(const std::vector<double>& rates)
{
    return rates.empty() ? 0.0 : *std::max_element(rates.begin(), rates.end());
}

inline double recordedShare(const std::vector<double>& recorded,
                            const std::vector<double>& unrecorded)
{
    const double base = best(unrecorded);
    return base > 0.0 ? best(recorded) / base : 0.0;
}

inline bool recordedShareHolds(double share, double floor)
{
    return share >= floor;
}

}  // namespace bench_judge
