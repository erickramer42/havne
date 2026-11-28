// File/layer: tests/test_pacing_stall.cpp | test_pacing, Qt-free, C++17
// Testing: UNIT: PacingTest (test_pacing)
//
// v0.8.0's Dashboard stall as a whole scenario on the pacing bench
// (tests/pacing_bench.h): several modules through an adapter's model,
// judged by what the user saw, each picked signal's update interval
// (cli/summary_lines.h's tally, its P50 read at an eighth octave's top).
// docs/bench-sessions/2026-11-18-stall-fix-car.md is the car's run.

#include "pacing_bench.h"
#include "uds_generic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
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

// The test car's six bitmaps, 41 00 to 41 A0 (A1a's wire log)
std::vector<uint8_t> car_chain()
{
    const uint8_t bitmaps[6][4] = {
        {0xFE, 0x7F, 0xA8, 0x13}, {0xA0, 0x17, 0xF0, 0x11},
        {0xFE, 0xD0, 0x8C, 0x81}, {0x63, 0x08, 0x00, 0x01},
        {0x00, 0x04, 0x00, 0x0D}, {0x04, 0x00, 0x00, 0x00}};
    std::vector<uint8_t> pids;
    for (uint32_t b = 0; b < 6; ++b) {
        for (uint32_t k = 0; k < 32; ++k) {
            if (bitmaps[b][k / 8] & (0x80u >> (k % 8))) {
                pids.push_back(static_cast<uint8_t>(b * 0x20 + k + 1));
            }
        }
    }
    return pids;
}

// The Dashboard's 19 Service 01 PIDs: the 17 the car packs into three
// requests a lap, and 0x22 and 0x5C, which its bitmaps leave clear
constexpr uint8_t kDashPids[] = {0x04, 0x05, 0x0C, 0x0D, 0x0F, 0x11, 0x42,
                                 0x10, 0x06, 0x07, 0x0B, 0x0E, 0x33, 0x44,
                                 0x46, 0x49, 0x68, 0x22, 0x5C};

ecuio_signal_def_t did_word(uint16_t id, uint32_t rx, uint16_t did,
                            const char* name)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.bit_length = 16;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "%s", name);
    return d;
}

// A 10-byte text DID, whose reply takes two frames
ecuio_signal_def_t did_text(uint16_t id, uint32_t rx, uint16_t did,
                            const char* name)
{
    ecuio_signal_def_t d = did_word(id, rx, did, name);
    d.kind = ECUIO_VALUE_TEXT;
    d.bit_length = 0;
    d.byte_length = 10;
    return d;
}

// A set registered under a name while the case runs; the registry and
// the sessions keep the pointers
class Registered
{
public:
    Registered(const char* name, std::vector<ecuio_signal_def_t> defs,
               std::vector<ecuio_channel_decl_t> channels)
        : m_name(name), m_defs(std::move(defs)), m_channels(std::move(channels))
    {
        m_set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        m_set.count = static_cast<uint32_t>(m_defs.size());
        m_set.defs = m_defs.data();
        m_set.channel_count = static_cast<uint32_t>(m_channels.size());
        m_set.channels = m_channels.data();
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(m_name, &m_set, &why), ECUIO_OK)
            << m_name << ": code " << why.code << " at " << why.index;
    }
    ~Registered() { EXPECT_EQ(ecuio_profile_unregister(m_name), ECUIO_OK); }
    Registered(const Registered&) = delete;
    Registered& operator=(const Registered&) = delete;

private:
    const char* m_name;
    std::vector<ecuio_signal_def_t> m_defs;
    std::vector<ecuio_channel_decl_t> m_channels;
    ecuio_signal_set_t m_set{};
};

constexpr uint16_t kTransmissionId = 30001;
constexpr uint16_t kSilentId = 30002;
constexpr uint16_t kRefusingId = 30003;

std::vector<ecuio_signal_def_t> dashboard_pids()
{
    std::vector<ecuio_signal_def_t> defs;
    const ecuio_signal_set_t* obd2 = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    for (uint32_t i = 0; obd2 != nullptr && i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        const auto pid = static_cast<uint8_t>(d.mux_key & 0xFF);
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 && d.mux_length == 2 &&
            (d.mux_key >> 8) == 0x41u &&
            std::find(std::begin(kDashPids), std::end(kDashPids), pid) !=
                std::end(kDashPids)) {
            defs.push_back(d);
        }
    }
    return defs;
}

std::vector<ecuio_signal_def_t>
with(std::vector<ecuio_signal_def_t> defs,
     std::initializer_list<ecuio_signal_def_t> more)
{
    defs.insert(defs.end(), more.begin(), more.end());
    return defs;
}

// The car's four modules as the stall's Dashboard asked them: the engine
// ECU (0x7E8) with the Dashboard's PIDs, a transmission module (0x7E9)
// with one DID, a module that answers nothing (0x769: its identification
// and diagnostics reads time out at the connect; its DID isn't picked)
// and one that refuses its DID (0x72E, 7F 22 31). The simulated car has
// no node on 0x769 and answers another DID on 0x72E
struct FourModules {
    FourModules()
        : session(
              kName,
              with(dashboard_pids(),
                   {did_word(kTransmissionId, 0x7E9, 0xD200, "Transmission"),
                    did_word(kSilentId, 0x769, 0xD300, "Silent"),
                    did_word(kRefusingId, 0x72E, 0xD400, "Refusing")}),
              {{0x7E8, 0x7E0, 0, 0, {}},
               {0x7E9, 0x7E1, 0, 0, {}},
               {0x769, 0x761, 0, 0, {}},
               {0x72E, 0x726, 0, 0, {}}}),
          car(kCar,
              {did_word(kTransmissionId, 0x7E9, 0xD200, "Transmission"),
               did_word(kRefusingId, 0x72E, 0xD401, "Other")},
              {{0x7E8, 0x7E0, 0, 0, {}},
               {0x7E9, 0x7E1, 0, 0, {}},
               {0x72E, 0x726, 0, 0, {}}})
    {}
    static constexpr const char* kName = "pacing-stall";
    static constexpr const char* kCar = "pacing-stall-car";
    Registered session;
    Registered car;
};

// The Dashboard's picks: the engine ECU's PIDs, the transmission's DID
// and the refusing module's
std::vector<uint16_t> dashboard_picks(bool transmission)
{
    std::vector<uint16_t> picks;
    for (const ecuio_signal_def_t& d : dashboard_pids()) {
        picks.push_back(d.id);
    }
    if (transmission) {
        picks.push_back(kTransmissionId);
    }
    picks.push_back(kRefusingId);
    return picks;
}

pacing::bench_result run(const pacing::bench_config& cfg)
{
    pacing::bench b(cfg);
    std::string why;
    EXPECT_TRUE(b.open(&why)) << why;
    return b.run();
}

// What the user saw over the picks that were answered: how many, the
// lowest and highest P50 and the worst interval, in ms, judged from the
// end of the connect (bench_config::settle_s)
struct seen {
    uint32_t answered = 0;
    double low_p50 = 1e12;
    double high_p50 = 0.0;
    double worst = 0.0;
};

seen seen_of(const pacing::bench_result& r, const std::vector<uint16_t>& picks)
{
    seen s;
    for (uint16_t id : picks) {
        const cli::SignalTally* t = r.tally.tally_of(id);
        if (t == nullptr || t->intervals == 0) {
            continue;
        }
        const double p50 =
            static_cast<double>(cli::interval_at(*t, 0.5)) / 1000.0;
        ++s.answered;
        s.low_p50 = std::min(s.low_p50, p50);
        s.high_p50 = std::max(s.high_p50, p50);
        s.worst = std::max(s.worst, static_cast<double>(t->max_us) / 1000.0);
    }
    return s;
}

double p50_of(const pacing::bench_result& r, uint16_t id)
{
    const cli::SignalTally* t = r.tally.tally_of(id);
    return t == nullptr || t->intervals == 0
               ? 0.0
               : static_cast<double>(cli::interval_at(*t, 0.5)) / 1000.0;
}

pacing::bench_config the_dashboard(const std::vector<uint32_t>& order)
{
    pacing::bench_config cfg;
    cfg.profile = FourModules::kName;
    cfg.sim_keys = std::string("profile=") + FourModules::kCar;
    cfg.model = pacing::elm327_bluetooth();
    cfg.timing = car_idle();
    cfg.supported = car_chain();
    cfg.channel_order = order;
    cfg.seconds = 70;
    cfg.settle_s = 10;
    return cfg;
}

} // namespace

/* The stall's Dashboard through the MX+ model, in the plugin's order and
 * with the engine ECU listed last. Each module's rows go once a lap and
 * the walk reaches the chain's end, so the 17 PIDs go in three packs and
 * every value answered comes once a lap: 107 ms each, the transmission's
 * too, judged from 10 s on as the car's sheet is. The transmission's DID
 * costs the engine's values a lap's header switches and one exchange:
 * RPM 107 ms against 74 without it. Turns by channel give the DID every
 * other exchange (53 ms against the PIDs' 164); one walk deadline from
 * the connect, with the silent module's reads ahead of the engine's
 * walk, ends the walk before its packs (459 ms). The car with the fix
 * read every value's median at 0.30 s, on a lap with more of the engine's
 * DIDs in it. */
TEST(PacingTest, TheStallsDashboardKeepsEveryValueOnTimeThroughTheMxPlus)
{
    const FourModules profile;
    const std::vector<std::vector<uint32_t>> orders = {
        {}, {0x769, 0x72E, 0x7E9, 0x7E8}};
    for (const std::vector<uint32_t>& order : orders) {
        SCOPED_TRACE(order.empty() ? "the plugin's order"
                                   : "the engine ECU last");
        pacing::bench_config cfg = the_dashboard(order);
        cfg.picks = dashboard_picks(true);
        const pacing::bench_result r = run(cfg);
        const seen s = seen_of(r, cfg.picks);
        EXPECT_EQ(s.answered, 19u)
            << "the 17 PIDs' 18 values and the transmission's DID";
        EXPECT_LT(s.high_p50, 150.0) << "the slowest value's P50, ms";
        EXPECT_LT(s.worst, 200.0) << "the worst interval, ms";
        EXPECT_LT(s.high_p50, 1.25 * s.low_p50)
            << s.low_p50 << " to " << s.high_p50
            << " ms: no value gets more turns";

        cfg.picks = dashboard_picks(false);
        const pacing::bench_result alone = run(cfg);
        const double rpm = p50_of(r, ECUIO_OBD2_SIG_RPM_ID);
        const double rpm_alone = p50_of(alone, ECUIO_OBD2_SIG_RPM_ID);
        ASSERT_GT(rpm_alone, 0.0);
        EXPECT_LT(rpm, 1.7 * rpm_alone)
            << "RPM's P50 " << rpm << " ms with the transmission's DID, "
            << rpm_alone << " without";
    }
}

/* ADR-0060's extended session through the MX+ model: the engine ECU's
 * one DID in its session (kept with 3E 00) beside the second module's
 * two, which refuses 10 03 and reads in default. Each row goes once a
 * lap, 61 ms. A keep-alive counted due ahead of its time takes every
 * turn's hand-over, so the session's DID goes after each of the other
 * module's rows: 41 ms against their 82 */
TEST(PacingTest, AKeepAliveTakesNoTurnBeforeItsTimeThroughTheMxPlus)
{
    pacing::bench_config cfg;
    cfg.profile =
        "uds-generic&did=7E8:D200:u16&did=7E9:D100:u8&did=7E9:D101:u8";
    cfg.sim_keys = "modules=2";
    cfg.model = pacing::elm327_bluetooth();
    cfg.timing = car_idle();
    cfg.params.session = 3;
    cfg.seconds = 40;
    cfg.settle_s = 5;
    const pacing::bench_result r = run(cfg);
    std::vector<uint16_t> ids;
    for (uint16_t i = 0; i < 3; ++i) {
        ids.push_back(static_cast<uint16_t>(ECUIO_UDS_GENERIC_SIG_ID_BASE + i));
    }
    const seen s = seen_of(r, ids);
    EXPECT_EQ(s.answered, 3u) << "the session's DID and the module's two";
    EXPECT_LT(s.high_p50, 1.25 * s.low_p50)
        << s.low_p50 << " to " << s.high_p50 << " ms: no row gets more turns";
    EXPECT_EQ(r.timeouts, 0u);
}

/* The hold's sharing by signal through the VX (ADR-0065's appendix): an
 * engine ECU of six one-frame DIDs, a transmission module of one, a
 * module whose two DIDs answer in segments, and one that answers nothing,
 * Known Issue #42's loss on. No row's interval passes twice its share of
 * what the bus carried (the engine's rows 82 ms against a share of 66),
 * none starves, and no reply is lost. With ?hold=0 the engine's rows run
 * 360-655 ms, about a thousand replies lost under the segments */
TEST(PacingTest, TheHoldSharesTheBusBySignalAcrossFourModules)
{
    std::vector<ecuio_signal_def_t> defs;
    for (uint16_t i = 0; i < 6; ++i) {
        defs.push_back(did_word(static_cast<uint16_t>(1 + i), 0x7E8,
                                static_cast<uint16_t>(0x1E10 + i), "Engine"));
        std::snprintf(defs.back().name, sizeof defs.back().name, "Engine %u",
                      static_cast<unsigned>(i));
    }
    defs.push_back(did_word(7, 0x7E9, 0xD200, "Transmission"));
    defs.push_back(did_text(8, 0x7EA, 0xD300, "Segmented A"));
    defs.push_back(did_text(9, 0x7EA, 0xD301, "Segmented B"));
    std::vector<ecuio_signal_def_t> car = defs;
    defs.push_back(did_word(10, 0x769, 0xD400, "Silent"));
    const Registered session("pacing-hold-four", defs,
                             {{0x7E8, 0x7E0, 0, 0, {}},
                              {0x7E9, 0x7E1, 0, 0, {}},
                              {0x7EA, 0x7E2, 0, 0, {}},
                              {0x769, 0x761, 0, 0, {}}});
    const Registered sim("pacing-hold-four-car", car,
                         {{0x7E8, 0x7E0, 0, 0, {}},
                          {0x7E9, 0x7E1, 0, 0, {}},
                          {0x7EA, 0x7E2, 0, 0, {}}});
    pacing::bench_config cfg;
    cfg.profile = "pacing-hold-four";
    cfg.sim_keys = "profile=pacing-hold-four-car";
    cfg.timing = car_idle();
    cfg.model.lost_under_segments = true;
    cfg.params.identify = false;
    cfg.params.diagnose = false;
    cfg.seconds = 60;
    cfg.settle_s = 5;
    const pacing::bench_result r = run(cfg);
    std::vector<double> p50s;
    double worst = 0.0;
    uint64_t values = 0;
    for (uint16_t id = 1; id <= 9; ++id) {
        const cli::SignalTally* t = r.tally.tally_of(id);
        ASSERT_TRUE(t != nullptr && t->intervals > 0) << "signal " << id;
        p50s.push_back(static_cast<double>(cli::interval_at(*t, 0.5)) / 1000.0);
        worst = std::max(worst, static_cast<double>(t->max_us) / 1000.0);
        values += t->values;
    }
    /* A row's share: its interval were the values carried shared evenly */
    const double span_ms =
        static_cast<double>(r.tally.to_us() - r.tally.from_us()) / 1000.0;
    ASSERT_GT(values, 0u);
    const double share = span_ms * 9.0 / static_cast<double>(values);
    for (size_t i = 0; i < p50s.size(); ++i) {
        EXPECT_LT(p50s[i], 2.0 * share) << "signal " << i + 1 << ": " << p50s[i]
                                        << " ms, a share " << share;
    }
    EXPECT_LT(worst, 5.0 * share) << "the worst interval, ms";
    EXPECT_EQ(r.lost, 0u);
}
