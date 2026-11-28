// File/layer: tests/bench_pacing.cpp | bench_pacing, Qt-free, C++17
// Testing: BENCH (not in ctest): the figures for docs/performance.md
//
// The pacing bench's tables (tests/pacing_bench.h): each pick through
// each adapter model at each setting, with log's summary figures, on the
// simulated car's timing; then the car's runs of
// docs/bench-sessions/2026-11-18-pacing-car.md, each against its model.
//
//   build/tests/Release/bench_pacing.exe [seconds, default 70]

#include "pacing_bench.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
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

pacing::adapter_model vx_j2534()
{
    return pacing::adapter_model{};
}

pacing::adapter_model socketcan()
{
    pacing::adapter_model m;
    m.kind = pacing::adapter::socketcan;
    m.deliver_us = 0;
    return m;
}

// A constructor, not an aggregate: a row may leave the last two out, which
// Clang's -Wmissing-field-initializers refuses in an aggregate's braces
struct row {
    row(const char* l, const pacing::adapter_model& m, int64_t gap, bool p,
        std::vector<uint16_t> pk, uint8_t frames = 0,
        std::vector<uint8_t> sup = {})
        : label(l), model(m), gap_ms(gap), pack(p), picks(std::move(pk)),
          pack_frames(frames), supported(std::move(sup))
    {}
    const char* label;
    pacing::adapter_model model;
    int64_t gap_ms; /* -1: the transport's */
    bool pack;
    std::vector<uint16_t> picks;
    uint8_t pack_frames;            /* ?packframes=, 0 none */
    std::vector<uint8_t> supported; /* none: the simulated car's own */
};

pacing::bench_config config_of(const row& r, int64_t seconds)
{
    pacing::bench_config cfg;
    cfg.model = r.model;
    cfg.params.gap_us = r.gap_ms < 0 ? -1 : r.gap_ms * 1000;
    cfg.params.pack = r.pack;
    cfg.params.pack_frames = r.pack_frames;
    cfg.timing = car_idle();
    cfg.picks = r.picks;
    cfg.supported = r.supported;
    cfg.seconds = seconds;
    return cfg;
}

void print(const row& r, int64_t seconds)
{
    pacing::bench b(config_of(r, seconds));
    std::string why;
    if (!b.open(&why)) {
        std::printf("%-32s refused: %s\n", r.label, why.c_str());
        return;
    }
    const pacing::bench_result res = b.run();
    const cli::SummaryFigures f = cli::summary_figures(res.tally, r.picks);
    std::printf("%-32s %8.1f %8.1f %8.2f %4u %9.1f %9.1f %6llu\n", r.label,
                res.requests_per_s(), res.values_per_s(),
                f.signals > 0 ? res.values_per_s() / f.signals : 0.0, f.signals,
                f.median_p50_us / 1000.0, f.worst_p50_us / 1000.0,
                static_cast<unsigned long long>(res.timeouts));
}

// Four modules shaped like the throughput milestone's P3 (the car's four
// with the most DIDs; the car's counts wait on its sweeps): 40 words on
// 0x7E8 (Single Frames), 20 10-byte texts on 0x7E9 (two frames), 8
// 16-byte texts on 0x7EA (three frames), 4 bytes on 0x7EB; with mixed,
// 0x7E9's first 10 are words. Registered for the session and the
// simulated car alike, which answers each module from the set
class four_modules
{
public:
    explicit four_modules(bool mixed)
    {
        const uint32_t rows[4] = {40, 20, 8, 4};
        const uint16_t text[4] = {0, 10, 16, 0};
        for (uint32_t m = 0; m < 4; ++m) {
            const uint32_t rx = 0x7E8 + m;
            m_channels[m] = ecuio_channel_decl_t{rx, rx - 8, 0, 0, {}};
            for (uint32_t k = 0; k < rows[m]; ++k) {
                const bool word = text[m] == 0 || (mixed && m == 1 && k < 10);
                m_defs.push_back(did_def(rx, k, word ? 0 : text[m], m == 3));
            }
        }
        m_set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        m_set.count = static_cast<uint32_t>(m_defs.size());
        m_set.defs = m_defs.data();
        m_set.channel_count = 4;
        m_set.channels = m_channels;
        m_ok = ecuio_profile_register(kName, &m_set, nullptr) == ECUIO_OK;
    }
    ~four_modules() { (void)ecuio_profile_unregister(kName); }
    four_modules(const four_modules&) = delete;
    four_modules& operator=(const four_modules&) = delete;
    bool ok() const { return m_ok; }
    static constexpr const char* kName = "pacing-four";

private:
    // DID D2<m><k>'s def on the module at rx: a word, a byte, or text
    ecuio_signal_def_t did_def(uint32_t rx, uint32_t k, uint16_t text,
                               bool byte)
    {
        ecuio_signal_def_t d{};
        d.id = static_cast<uint16_t>(m_defs.size() + 1);
        d.message_id = rx;
        d.mux_length = 3;
        d.mux_key = 0x62D200u | ((rx - 0x7E8) << 6) | k;
        d.kind = ECUIO_VALUE_UINT;
        if (text != 0) {
            d.kind = ECUIO_VALUE_TEXT;
        }
        d.byte_order = ECUIO_BYTE_ORDER_BE;
        d.start_byte = 3;
        d.bit_length = text != 0 ? 0 : byte ? 8 : 16;
        d.byte_length = text;
        d.scale = 1.0;
        std::snprintf(d.name, sizeof d.name, "M%X D%02u", rx - 0x7E8, k);
        return d;
    }

    std::vector<ecuio_signal_def_t> m_defs;
    ecuio_channel_decl_t m_channels[4] = {};
    ecuio_signal_set_t m_set{};
    bool m_ok = false;
};

// The VX through four_modules, Known Issue #42's loss on (lost replies
// time out), identification and diagnostics off, at seed
pacing::bench_config four_config(int64_t seconds, uint64_t seed)
{
    pacing::bench_config cfg;
    cfg.profile = four_modules::kName;
    cfg.model = vx_j2534();
    cfg.model.lost_under_segments = true;
    cfg.timing = car_idle();
    cfg.timing.seed = seed;
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    cfg.seconds = seconds;
    return cfg;
}

void print_four(const char* label, bool mixed, int64_t seconds)
{
    const four_modules set(mixed);
    pacing::bench b(four_config(seconds, 1));
    std::string why;
    if (!set.ok() || !b.open(&why)) {
        std::printf("%-32s refused: %s\n", label, why.c_str());
        return;
    }
    const pacing::bench_result res = b.run();
    const cli::SummaryFigures f = cli::summary_figures(res.tally, {});
    std::printf("%-32s %8.1f %8.1f %4u %9.1f %9.1f %9.1f %5llu %5llu\n",
                label, res.requests_per_s(), res.values_per_s(), f.signals,
                f.median_p50_us / 1000.0, f.worst_p50_us / 1000.0,
                f.worst_p99_us / 1000.0,
                static_cast<unsigned long long>(res.lost),
                static_cast<unsigned long long>(res.timeouts));
}

// A car run's shape and its replies a second on the car
struct car_row {
    row shape;
    double car;
};

void print_car(const car_row& c)
{
    pacing::bench b(config_of(c.shape, 60));
    std::string why;
    if (!b.open(&why)) {
        std::printf("%-32s refused: %s\n", c.shape.label, why.c_str());
        return;
    }
    const double model = b.run().requests_per_s();
    std::printf("%-32s %8.1f %8.1f %+7.1f%%\n", c.shape.label, model, c.car,
                100.0 * (model - c.car) / c.car);
}

} // namespace

int main(int argc, char** argv)
{
    const int64_t seconds = argc > 1 ? std::atoll(argv[1]) : 70;
    const std::vector<uint16_t> rpm = {id_of("RPM")};
    const std::vector<uint8_t> car = pacing::test_car_pids();
    const pacing::adapter_model elm = pacing::vx_elm_mode();
    const pacing::adapter_model mx = pacing::elm327_bluetooth();
    const std::vector<row> rows = {
        {"VX J2534, obd2 whole", vx_j2534(), -1, true, {}},
        {"VX J2534, obd2 whole, pack=0", vx_j2534(), -1, false, {}},
        {"VX J2534, obd2 whole, gap=0", vx_j2534(), 0, true, {}},
        {"VX J2534, obd2 whole, gap=20", vx_j2534(), 20, true, {}},
        {"VX J2534, RPM, pack=0", vx_j2534(), -1, false, rpm},
        {"VX J2534, obd2, packframes=1", vx_j2534(), -1, true, {}, 1},
        {"VX J2534, obd2, packframes=2", vx_j2534(), -1, true, {}, 2},
        {"VX J2534, the car", vx_j2534(), -1, true, {}, 0, car},
        {"VX J2534, the car, packframes=1", vx_j2534(), -1, true, {}, 1, car},
        {"VX J2534, the car, packframes=2", vx_j2534(), -1, true, {}, 2, car},
        {"SocketCAN, obd2 whole", socketcan(), -1, true, {}},
        {"SocketCAN, obd2, gap=0", socketcan(), 0, true, {}},
        {"MX+ ELM327, obd2 whole", pacing::elm327_bluetooth(), -1, true, {}},
        {"MX+ ELM327, obd2, pack=0", pacing::elm327_bluetooth(), -1, false, {}},
        {"MX+ ELM327, RPM, pack=0", pacing::elm327_bluetooth(), -1, false, rpm},
        {"VX ELM, obd2 whole", pacing::vx_elm_mode(), -1, true, {}},
        {"VX ELM, obd2 whole, pack=0", pacing::vx_elm_mode(), -1, false, {}},
        {"VX ELM, RPM, pack=0", pacing::vx_elm_mode(), -1, false, rpm},
        {"VX ELM, obd2, packframes=1", elm, -1, true, {}, 1},
        {"VX ELM, the car", elm, -1, true, {}, 0, car},
        {"VX ELM, the car, packframes=1", elm, -1, true, {}, 1, car},
        {"MX+ ELM327, the car", mx, -1, true, {}, 0, car},
        {"MX+ ELM327, obd2, packframes=1", mx, -1, true, {}, 1},
    };
    std::printf("%-32s %8s %8s %8s %4s %9s %9s %6s\n", "run", "req/s",
                "values/s", "sweeps/s", "sigs", "P50 ms", "worst ms", "t/out");
    for (const row& r : rows) {
        print(r, seconds);
    }

    std::printf("\n%-32s %8s %8s %4s %9s %9s %9s %5s %5s\n", "unequal modules",
                "req/s", "values/s", "sigs", "P50 ms", "worst ms", "P99 ms",
                "lost", "t/out");
    print_four("VX J2534, four modules, unequal", false, seconds);
    print_four("VX J2534, four modules, mixed", true, seconds);

    /* D9-D11 ask one DID a module, registered sets: PacingTest's pins */
    const std::vector<uint16_t> eight = {id_of("RPM"),
                                         id_of("Engine Load"),
                                         id_of("Coolant Temp"),
                                         id_of("Vehicle Speed"),
                                         id_of("Intake Air Temp"),
                                         id_of("Throttle Position"),
                                         id_of("MAF"),
                                         id_of("Timing Advance")};
    const std::vector<car_row> cars = {
        {{"D1 VX, obd2", vx_j2534(), -1, true, {}, 0, car}, 58.7},
        {{"D2 VX, obd2, pack=0", vx_j2534(), -1, false, {}, 0, car}, 140.8},
        {{"D4 VX, obd2, gap=0", vx_j2534(), 0, true, {}, 0, car}, 52.7},
        {{"D5 VX, obd2, gap=20", vx_j2534(), 20, true, {}, 0, car}, 48.2},
        {{"D6 VX, RPM, pack=0", vx_j2534(), -1, false, rpm, 0, car}, 140.3},
        {{"D7 VX, 8 signals, pack=0", vx_j2534(), -1, false, eight, 0, car},
         138.4},
        {{"D3 MX+, obd2", mx, -1, true, {}, 0, car}, 40.9},
        {{"D8 MX+, obd2, gap=0", mx, 0, true, {}, 0, car}, 41.9},
    };
    std::printf("\n%-32s %8s %8s %8s\n", "the car's run", "model", "car",
                "off");
    for (const car_row& c : cars) {
        print_car(c);
    }
    return 0;
}
