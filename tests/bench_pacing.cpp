// File/layer: tests/bench_pacing.cpp | bench_pacing, Qt-free, C++17
// Testing: BENCH (not in ctest): the figures for docs/performance.md
//
// The pacing bench's table (tests/pacing_bench.h): each pick through each
// adapter model at each setting, with log's summary figures, on the
// simulated car's timing. Uncalibrated until the constants are set from a
// car run.
//
//   build/tests/Release/bench_pacing.exe [seconds, default 70]

#include "pacing_bench.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

pacing::sim_timing car_idle()
{
    pacing::sim_timing t;
    t.p50_us = 3460; /* the 2026-11-03 tap, the engine ECU at idle */
    t.p99_us = 10930;
    t.fc_us = 4800;
    t.cf_us = 5300;
    return t;
}

uint16_t id_of(const char* name)
{
    const ecuio_signal_set_t* set = nullptr;
    if (ecuio_profile_lookup("obd2", &set) != ECUIO_OK) {
        return 0;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        if (std::strcmp(set->defs[i].name, name) == 0) {
            return set->defs[i].id;
        }
    }
    return 0;
}

struct row {
    const char* label;
    pacing::adapter kind;
    int64_t gap_ms; /* -1: the transport's */
    bool pack;
    std::vector<uint16_t> picks;
};

void print(const row& r, int64_t seconds)
{
    pacing::bench_config cfg;
    cfg.model.kind = r.kind;
    if (r.kind == pacing::adapter::socketcan) {
        cfg.model.deliver_us = 0;
    }
    cfg.params.gap_us = r.gap_ms < 0 ? -1 : r.gap_ms * 1000;
    cfg.params.pack = r.pack;
    cfg.timing = car_idle();
    cfg.picks = r.picks;
    cfg.seconds = seconds;
    pacing::bench b(cfg);
    std::string why;
    if (!b.open(&why)) {
        std::printf("%-30s refused: %s\n", r.label, why.c_str());
        return;
    }
    const pacing::bench_result res = b.run();
    const cli::SummaryFigures f = cli::summary_figures(res.tally, r.picks);
    std::printf("%-30s %8.1f %8.1f %8.2f %4u %9.1f %9.1f %6llu\n", r.label,
                res.requests_per_s(), res.values_per_s(),
                f.signals > 0 ? res.values_per_s() / f.signals : 0.0, f.signals,
                f.median_p50_us / 1000.0, f.worst_p50_us / 1000.0,
                static_cast<unsigned long long>(res.timeouts));
}

} // namespace

int main(int argc, char** argv)
{
    const int64_t seconds = argc > 1 ? std::atoll(argv[1]) : 70;
    const std::vector<uint16_t> rpm = {id_of("RPM")};
    const std::vector<row> rows = {
        {"VX J2534, obd2 whole", pacing::adapter::j2534, -1, true, {}},
        {"VX J2534, obd2 whole, pack=0", pacing::adapter::j2534, -1, false, {}},
        {"VX J2534, obd2 whole, gap=0", pacing::adapter::j2534, 0, true, {}},
        {"VX J2534, obd2 whole, gap=20", pacing::adapter::j2534, 20, true, {}},
        {"VX J2534, RPM, pack=0", pacing::adapter::j2534, -1, false, rpm},
        {"SocketCAN, obd2 whole", pacing::adapter::socketcan, -1, true, {}},
        {"SocketCAN, obd2, gap=0", pacing::adapter::socketcan, 0, true, {}},
    };
    std::printf("%-30s %8s %8s %8s %4s %9s %9s %6s\n", "run", "req/s",
                "values/s", "sweeps/s", "sigs", "P50 ms", "worst ms", "t/out");
    for (const row& r : rows) {
        print(r, seconds);
    }
    return 0;
}
