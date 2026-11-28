// File/layer: tests/test_pacing_session.cpp | test_pacing, Qt-free, C++17
// Testing: UNIT: PacingTest (test_pacing)
//
// The extended session (ADR-0060) held on many modules at once, on the
// pacing bench (tests/pacing_bench.h): uds-generic over a made-up car
// whose modules each answer 10 03 and 3E 00 (the simulated car's
// sessions=1), judged by the signals' update intervals and by each
// channel's report at the run's end (its session's state and counts).

#include "pacing_bench.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// The 2026-11-03 tap's engine ECU at idle, as test_pacing.cpp's
pacing::sim_timing car_idle()
{
    pacing::sim_timing t;
    t.p50_us = 3460;
    t.p99_us = 10930;
    t.fc_us = 4800;
    t.cf_us = 5300;
    return t;
}

// Module m's reply ID: the eight OBD pairs first, then 0x608 on, 0x10 apart
uint32_t rx_of(uint32_t m)
{
    return m < 8 ? 0x7E8 + m : 0x608 + 0x10 * (m - 8);
}

// The simulated car's modules [0, n), `rows` words each (DIDs from
// 0x1000), registered under a name while the case runs
class Car
{
public:
    Car(uint32_t n, int rows) : m_name("pacing-session-" + std::to_string(n))
    {
        uint16_t id = 1;
        for (uint32_t m = 0; m < n; ++m) {
            const uint32_t rx = rx_of(m);
            m_channels.push_back(ecuio_channel_decl_t{rx, rx - 8, 0, 0, {}});
            for (int k = 0; k < rows; ++k) {
                ecuio_signal_def_t d{};
                d.id = id++;
                d.message_id = rx;
                d.mux_length = 3;
                d.mux_key = 0x620000u | static_cast<uint32_t>(0x1000 + k);
                d.byte_order = ECUIO_BYTE_ORDER_BE;
                d.start_byte = 3;
                d.scale = 1.0;
                d.kind = ECUIO_VALUE_UINT;
                d.bit_length = 16;
                std::snprintf(d.name, sizeof d.name, "%03X %04X", rx,
                              0x1000 + k);
                m_defs.push_back(d);
            }
        }
        m_set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        m_set.count = static_cast<uint32_t>(m_defs.size());
        m_set.defs = m_defs.data();
        m_set.channel_count = static_cast<uint32_t>(m_channels.size());
        m_set.channels = m_channels.data();
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(m_name.c_str(), &m_set, &why),
                  ECUIO_OK)
            << "code " << why.code << " at " << why.index;
    }
    ~Car() { EXPECT_EQ(ecuio_profile_unregister(m_name.c_str()), ECUIO_OK); }
    Car(const Car&) = delete;
    Car& operator=(const Car&) = delete;
    const std::string& name() const { return m_name; }

private:
    std::string m_name;
    std::vector<ecuio_signal_def_t> m_defs;
    std::vector<ecuio_channel_decl_t> m_channels;
    ecuio_signal_set_t m_set{};
};

// uds-generic over the car's n modules of `rows` words, one did= key
// listing them all, through the MX+ model: the extended session with
// keepalive_ms, or none at 0; the picks module 0's rows when pick_first
pacing::bench_config config(const Car& car, uint32_t n, int rows,
                            uint32_t keepalive_ms, bool pick_first = false)
{
    pacing::bench_config cfg;
    cfg.profile = "uds-generic&did=";
    for (uint32_t m = 0; m < n; ++m) {
        for (int k = 0; k < rows; ++k) {
            char key[24];
            std::snprintf(key, sizeof key, "%s%03X:%04X:u16",
                          (m == 0 && k == 0) ? "" : ",", rx_of(m), 0x1000 + k);
            cfg.profile += key;
        }
    }
    cfg.sim_keys = "profile=" + car.name() + "&sessions=1";
    cfg.model = pacing::elm327_bluetooth();
    cfg.timing = car_idle();
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    if (keepalive_ms != 0) {
        cfg.params.session = ECUIO_SESSION_EXTENDED;
        cfg.params.keepalive_ms = keepalive_ms;
    }
    cfg.seconds = 70;
    cfg.settle_s = 15;
    for (int k = 0; pick_first && k < rows; ++k) {
        cfg.picks.push_back(
            static_cast<uint16_t>(ECUIO_UDS_GENERIC_SIG_ID_BASE + k));
    }
    return cfg;
}

// uds-generic's first count signal ids, in the did= key's order
std::vector<uint16_t> ids(uint32_t count)
{
    std::vector<uint16_t> out;
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back(static_cast<uint16_t>(ECUIO_UDS_GENERIC_SIG_ID_BASE + i));
    }
    return out;
}

pacing::bench_result run(const pacing::bench_config& cfg)
{
    pacing::bench b(cfg);
    std::string why;
    EXPECT_TRUE(b.open(&why)) << why;
    return b.run();
}

// The signals' P50s, the highest of them and the worst interval of any,
// in ms, and how many were answered
struct seen {
    uint32_t answered = 0;
    double high_p50 = 0.0;
    double worst = 0.0;
};

seen seen_of(const pacing::bench_result& r, const std::vector<uint16_t>& of)
{
    seen s;
    for (uint16_t id : of) {
        const cli::SignalTally* t = r.tally.tally_of(id);
        if (t == nullptr || t->intervals == 0) {
            continue;
        }
        ++s.answered;
        s.high_p50 =
            std::max(s.high_p50,
                     static_cast<double>(cli::interval_at(*t, 0.5)) / 1000.0);
        s.worst = std::max(s.worst, static_cast<double>(t->max_us) / 1000.0);
    }
    return s;
}

// The 3E 00 sent in the tally's window
uint64_t keepalives(const pacing::bench_result& r)
{
    const int64_t from = static_cast<int64_t>(r.tally.from_us());
    uint64_t n = 0;
    for (size_t k = 0; k < r.sent_us.size(); ++k) {
        n += r.sent_us[k] >= from && r.sent_sid[k] == 0x3E ? 1 : 0;
    }
    return n;
}

// Every channel's session entered and never lost, by its report
void expect_every_session_kept(const pacing::bench_result& r, uint32_t n)
{
    ASSERT_EQ(r.support.size(), n);
    for (uint32_t c = 0; c < n; ++c) {
        EXPECT_EQ(r.support[c].session_state, ECUIO_SESSION_STATE_ENTERED)
            << "module " << c;
        EXPECT_GE(r.support[c].session_entries, 1u) << "module " << c;
        EXPECT_EQ(r.support[c].session_lost, 0u) << "module " << c;
    }
}

} // namespace

/* uds-generic over six modules of four words with ?session=03 at the
 * default keep-alive, through the MX+ model: the simulated car's
 * sessions=1 has every module answer 10 03, so each enters its session
 * and keeps it, and every row is read */
TEST(PacingTest, ASessionIsEnteredOnEachOfSixModules)
{
    const Car car(6, 4);
    const pacing::bench_result r = run(config(
        car, 6, 4, static_cast<uint32_t>(ECUIO_ENGINE_KEEPALIVE_US / 1000)));
    EXPECT_EQ(seen_of(r, ids(24)).answered, 24u);
    expect_every_session_kept(r, 6);
}

/* 14 modules of four words in the extended session, the keep-alive at
 * its least, through the MX+ model: each lap (about 1.2 s) outlasts the
 * keep-alive, so the rows stand in for it. Every row keeps the pace it
 * has with no session at all and no 3E 00 goes */
TEST(PacingTest, FourteenModulesInTheSessionKeepTheirLapThroughTheMxPlus)
{
    const Car car(14, 4);
    const seen alone = seen_of(run(config(car, 14, 4, 0)), ids(56));
    const pacing::bench_result r = run(config(car, 14, 4, 500));
    const seen s = seen_of(r, ids(56));
    EXPECT_EQ(s.answered, 56u);
    EXPECT_LT(s.high_p50, 1.1 * alone.high_p50)
        << s.high_p50 << " ms against " << alone.high_p50 << " alone";
    EXPECT_LT(s.worst, 1.25 * alone.worst)
        << s.worst << " ms against " << alone.worst << " alone";
    EXPECT_EQ(keepalives(r), 0u);
    expect_every_session_kept(r, 14);
}

/* The same 14 modules, module 0's four rows picked: the other 13 have
 * nothing to ask but their keep-alive. Each gives way to a due row until
 * its session is urgent, so the picks keep their pace and every session
 * is kept */
TEST(PacingTest,
     APickBesideThirteenModulesInTheSessionKeepsItsPaceThroughTheMxPlus)
{
    const Car car(14, 4);
    const seen alone = seen_of(run(config(car, 14, 4, 0, true)), ids(4));
    const pacing::bench_result r = run(config(car, 14, 4, 500, true));
    const seen s = seen_of(r, ids(4));
    EXPECT_EQ(s.answered, 4u);
    EXPECT_LT(s.high_p50, 1.25 * alone.high_p50)
        << s.high_p50 << " ms against " << alone.high_p50 << " alone";
    expect_every_session_kept(r, 14);
}
