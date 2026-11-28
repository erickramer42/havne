// File/layer: tests/test_pacing_quiet.cpp | test_pacing, Qt-free, C++17
// Testing: UNIT: PacingTest (test_pacing)
//
// Modules that answer nothing (off, asleep, not fitted) beside modules
// that answer, on the pacing bench (tests/pacing_bench.h), judged by the
// answering signals' update intervals (cli/summary_lines.h's tally, its
// P50 read at an eighth octave's top). Each pin is a bound the engine's
// quiet module rule (core/src/request_engine_quiet.h) meets: without it,
// through the MX+ model each silent row costs every lap its NO DATA, and
// under the J2534 hold an absent module's probes hold the segmented rows.

#include "pacing_bench.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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

// The J2534 model with the hold on and the VX's loss of a Single Frame
// that ends under another module's segmented reply (Known Issue #42)
pacing::adapter_model the_vx()
{
    pacing::adapter_model m;
    m.lost_under_segments = true;
    return m;
}

// One module of a made-up car: words (one Single Frame each, DIDs from
// 0x1000) and texts (16 bytes, three frames, DIDs from 0x1400); pids puts
// the obd2 set's Service 01 defs on it as well
struct module {
    uint32_t rx = 0x7E8;
    int words = 0;
    int texts = 0;
    bool pids = false;
};

module words(uint32_t rx, int n)
{
    module m;
    m.rx = rx;
    m.words = n;
    return m;
}

// n absent modules of 4 words each, reply IDs from first, 0x10 apart
std::vector<module> absent(uint32_t first, int n)
{
    std::vector<module> mods;
    for (int i = 0; i < n; ++i) {
        mods.push_back(words(first + 0x10u * static_cast<uint32_t>(i), 4));
    }
    return mods;
}

std::vector<module> joined(std::vector<module> a, const std::vector<module>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did, int text)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.scale = 1.0;
    if (text > 0) {
        d.kind = ECUIO_VALUE_TEXT;
        d.byte_length = static_cast<uint16_t>(text);
    } else {
        d.kind = ECUIO_VALUE_UINT;
        d.bit_length = 16;
    }
    std::snprintf(d.name, sizeof d.name, "%03X %04X", rx, did);
    return d;
}

// The modules' defs, ids from 1 in module order: a module keeps its ids
// in any set that lists the modules before it alike
std::vector<ecuio_signal_def_t> defs_of(const std::vector<module>& mods)
{
    std::vector<ecuio_signal_def_t> defs;
    uint16_t id = 1;
    const ecuio_signal_set_t* obd2 = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    for (const module& m : mods) {
        for (uint32_t i = 0; m.pids && obd2 != nullptr && i < obd2->count;
             ++i) {
            const ecuio_signal_def_t& p = obd2->defs[i];
            if ((p.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 && p.mux_length == 2 &&
                (p.mux_key >> 8) == 0x41u) {
                defs.push_back(p);
                defs.back().id = id++;
                defs.back().message_id = m.rx;
            }
        }
        for (int k = 0; k < m.words; ++k) {
            defs.push_back(
                did_def(id++, m.rx, static_cast<uint16_t>(0x1000 + k), 0));
        }
        for (int k = 0; k < m.texts; ++k) {
            defs.push_back(
                did_def(id++, m.rx, static_cast<uint16_t>(0x1400 + k), 16));
        }
    }
    return defs;
}

// A set registered under a name while the case runs; the registry and
// the sessions keep the pointers
class Registered
{
public:
    Registered(std::string name, const std::vector<module>& mods)
        : m_name(std::move(name)), m_defs(defs_of(mods))
    {
        for (const module& m : mods) {
            m_channels.push_back(ecuio_channel_decl_t{m.rx, m.rx - 8, 0, 0, {}});
        }
        m_set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        m_set.count = static_cast<uint32_t>(m_defs.size());
        m_set.defs = m_defs.data();
        m_set.channel_count = static_cast<uint32_t>(m_channels.size());
        m_set.channels = m_channels.data();
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(m_name.c_str(), &m_set, &why),
                  ECUIO_OK)
            << m_name << ": code " << why.code << " at " << why.index;
    }
    ~Registered()
    {
        EXPECT_EQ(ecuio_profile_unregister(m_name.c_str()), ECUIO_OK);
    }
    Registered(const Registered&) = delete;
    Registered& operator=(const Registered&) = delete;
    const std::string& name() const { return m_name; }
    const std::vector<ecuio_signal_def_t>& defs() const { return m_defs; }

private:
    std::string m_name;
    std::vector<ecuio_signal_def_t> m_defs;
    std::vector<ecuio_channel_decl_t> m_channels;
    ecuio_signal_set_t m_set{};
};

// What the session asks (the present modules first, then the absent) and
// what the simulated car answers (the present alone): an absent module's
// requests go unanswered and its signals never start the tally's window
struct Car {
    Car(const std::vector<module>& present, const std::vector<module>& gone)
        : session("pacing-quiet", joined(present, gone)),
          car("pacing-quiet-car", present)
    {}
    pacing::bench_config config(const pacing::adapter_model& model) const
    {
        pacing::bench_config cfg;
        cfg.profile = session.name();
        cfg.sim_keys = "profile=" + car.name();
        cfg.model = model;
        cfg.timing = car_idle();
        cfg.params.identify = false;
        cfg.params.diagnose = false;
        cfg.seconds = 70;
        cfg.settle_s = 15;
        return cfg;
    }
    Registered session;
    Registered car;
};

pacing::bench_result run(const pacing::bench_config& cfg)
{
    pacing::bench b(cfg);
    std::string why;
    EXPECT_TRUE(b.open(&why)) << why;
    return b.run();
}

// The answering signals of the modules in rx, in ms: the median of their
// P50s and the worst interval of any, and how many were answered
struct pace {
    size_t signals = 0;
    size_t answered = 0;
    double p50 = 0.0;
    double worst = 0.0;
};

pace pace_of(const pacing::bench_result& r, const Registered& set,
             const std::vector<uint32_t>& rx, bool texts_only = false)
{
    pace p;
    std::vector<double> p50s;
    for (const ecuio_signal_def_t& d : set.defs()) {
        if (std::find(rx.begin(), rx.end(), d.message_id) == rx.end() ||
            (texts_only && d.kind != ECUIO_VALUE_TEXT)) {
            continue;
        }
        ++p.signals;
        const cli::SignalTally* t = r.tally.tally_of(d.id);
        if (t == nullptr || t->intervals == 0) {
            continue;
        }
        ++p.answered;
        p50s.push_back(static_cast<double>(cli::interval_at(*t, 0.5)) / 1000.0);
        p.worst = std::max(p.worst, static_cast<double>(t->max_us) / 1000.0);
    }
    if (!p50s.empty()) {
        std::sort(p50s.begin(), p50s.end());
        p.p50 = p50s[p50s.size() / 2];
    }
    return p;
}

std::string text_of(const pace& p)
{
    char line[96];
    std::snprintf(line, sizeof line, "P50 %.1f ms, worst %.1f ms, %zu of %zu",
                  p.p50, p.worst, p.answered, p.signals);
    return line;
}

// The engine ECU's 8 words through the MX+ model with the modules beside
// it: alone, or beside silent ones the car hasn't
pace engine_beside(const std::vector<module>& silent)
{
    const Car car({words(0x7E8, 8)}, silent);
    return pace_of(run(car.config(pacing::elm327_bluetooth())), car.session,
                   {0x7E8});
}

// Bounds against the same run with nothing silent (the tally's steps are
// an eighth octave, 9%): within a quarter beside one silent module,
// twice beside many, a tenth for the hold's segmented rows; no value
// stale (Live's mark, 3 s); a connect's last first value within 5 s
constexpr double kNearAlone = 1.25;
constexpr double kBesideMany = 2.0;
constexpr double kNearHold = 1.1;
constexpr double kStaleMs = 3000.0;
constexpr double kConnectLaterS = 5.0;

} // namespace

/* A silent module of 1, 4 or 8 rows beside the engine ECU's 8 through the
 * MX+ model: the engine's values keep their pace alone (164 ms; 180 here,
 * the tally's step). Without the quiet rule each silent row's NO DATA
 * goes in every lap: 279, 602 and 1,033 ms. */
TEST(PacingTest, AnElm327sSilentRowsCostTheOthersNothing)
{
    const pace alone = engine_beside({});
    ASSERT_EQ(alone.answered, 8u) << text_of(alone);
    for (int rows : {1, 4, 8}) {
        const pace beside = engine_beside({words(0x7E9, rows)});
        EXPECT_EQ(beside.answered, 8u) << rows << " silent rows";
        EXPECT_LE(beside.p50, kNearAlone * alone.p50)
            << rows << " silent rows: " << text_of(beside) << " against "
            << text_of(alone) << " alone";
    }
}

/* Six silent modules of 4 rows beside the engine ECU's 8 through the MX+
 * model: the engine's values within twice their pace alone (180 ms, as
 * alone). Without the rule they come every 2.8 s, 24 NO DATA a lap. */
TEST(PacingTest, AnElm327sSilentModulesCostTheOthersNothing)
{
    const pace alone = engine_beside({});
    const pace beside = engine_beside(absent(0x608, 6));
    EXPECT_EQ(beside.answered, 8u);
    EXPECT_LE(beside.p50, kBesideMany * alone.p50)
        << text_of(beside) << " against " << text_of(alone) << " alone";
}

/* Four modules that answer (8, 4, 4 and 4 rows) beside 14 absent ones of
 * 4 rows, the whole-car shape, through the MX+ model: every answering
 * signal within twice its pace alone, none stale (524 ms against 426).
 * Without the rule every one comes every 6.5 s, dimmed as stale. */
TEST(PacingTest, FourModulesBesideFourteenAbsentKeepTheirLap)
{
    const std::vector<module> present = {words(0x7E8, 8), words(0x7E9, 4),
                                         words(0x7EA, 4), words(0x7EB, 4)};
    const std::vector<uint32_t> rx = {0x7E8, 0x7E9, 0x7EA, 0x7EB};
    pace alone;
    {
        const Car car(present, {});
        alone = pace_of(run(car.config(pacing::elm327_bluetooth())),
                        car.session, rx);
    }
    const Car car(present, absent(0x608, 14));
    const pace beside =
        pace_of(run(car.config(pacing::elm327_bluetooth())), car.session, rx);
    EXPECT_EQ(beside.answered, 20u) << text_of(beside);
    EXPECT_LE(beside.p50, kBesideMany * alone.p50)
        << text_of(beside) << " against " << text_of(alone) << " alone";
    EXPECT_LT(beside.worst, kStaleMs) << text_of(beside);
}

/* The connect as shipped (identification and diagnostics on, the engine
 * ECU's PIDs walked and packed) of the same four modules beside 14
 * absent: every answering signal has its first value within 5 s of the
 * same connect with none absent (2.0 s; 5.2 s beside them), each absent
 * module costing a couple of waits. Without the rule the last first
 * value comes at 12.7 s, every absent module's lists waited out item by
 * item. */
TEST(PacingTest, AConnectBesideFourteenAbsentReadsThePresentSoon)
{
    module engine;
    engine.rx = 0x7E8;
    engine.pids = true;
    const std::vector<module> present = {engine, words(0x7E9, 4),
                                         words(0x7EA, 4), words(0x7EB, 4)};
    const auto connect = [&](const std::vector<module>& gone) {
        const Car car(present, gone);
        pacing::bench_config cfg = car.config(pacing::elm327_bluetooth());
        cfg.params.identify = true;
        cfg.params.diagnose = true;
        cfg.supported = pacing::test_car_pids();
        cfg.seconds = 30;
        cfg.settle_s = 0;
        /* the window starts at the last first value of a signal answered */
        return static_cast<double>(run(cfg).tally.from_us()) / 1e6 - 1.0;
    };
    const double none = connect({});
    const double beside = connect(absent(0x608, 14));
    EXPECT_GT(none, 0.0);
    EXPECT_LE(beside, none + kConnectLaterS)
        << "the last first value at " << beside << " s against " << none
        << " s with none absent";
}

/* The same connect, read from 5 s on: beside the 14 absent the answering
 * signals keep twice their pace with none absent, since an absent
 * module's lists settle unasked once it is quiet. Asked one item a lap
 * each, their waits would hold every lap for about half a minute. */
TEST(PacingTest, TheLapsAfterAConnectBesideFourteenAbsentKeepTheirPace)
{
    module engine;
    engine.rx = 0x7E8;
    engine.pids = true;
    const std::vector<module> present = {engine, words(0x7E9, 4),
                                         words(0x7EA, 4), words(0x7EB, 4)};
    const std::vector<uint32_t> rx = {0x7E9, 0x7EA, 0x7EB};
    const auto laps = [&](const std::vector<module>& gone) {
        const Car car(present, gone);
        pacing::bench_config cfg = car.config(pacing::elm327_bluetooth());
        cfg.params.identify = true;
        cfg.params.diagnose = true;
        cfg.supported = pacing::test_car_pids();
        cfg.seconds = 30;
        cfg.settle_s = 5;
        return pace_of(run(cfg), car.session, rx);
    };
    const pace none = laps({});
    const pace beside = laps(absent(0x608, 14));
    ASSERT_EQ(none.answered, 12u) << text_of(none);
    EXPECT_EQ(beside.answered, 12u) << text_of(beside);
    EXPECT_LE(beside.p50, kBesideMany * none.p50)
        << text_of(beside) << " against " << text_of(none) << " with none";
}

/* The J2534 hold, the VX model: the engine ECU's 20 text rows (three
 * frames each) beside a module of 4 words, and 1, 4 or 14 absent modules
 * of 4 rows. The text rows keep their pace with none absent (490 ms)
 * within an eighth octave's step (491, 491 and 524 ms). Without the rule
 * an absent module's probe (one ask in 8 under the hold) holds a chosen
 * segmented row for its wait: 572, 793 and 2,663 ms. */
TEST(PacingTest, TheHoldsSegmentedRowsKeepTheirPaceBesideAbsentOnes)
{
    module engine;
    engine.rx = 0x7E8;
    engine.texts = 20;
    const std::vector<module> present = {engine, words(0x7E9, 4)};
    const auto texts = [&](int gone) {
        const Car car(present, absent(0x508, gone));
        pacing::bench_config cfg = car.config(the_vx());
        cfg.settle_s = 30;
        return pace_of(run(cfg), car.session, {0x7E8}, true);
    };
    const pace none = texts(0);
    ASSERT_EQ(none.answered, 20u) << text_of(none);
    for (int gone : {1, 4, 14}) {
        const pace beside = texts(gone);
        EXPECT_EQ(beside.answered, 20u) << gone << " absent";
        EXPECT_LE(beside.p50, kNearHold * none.p50)
            << gone << " absent: " << text_of(beside) << " against "
            << text_of(none) << " with none";
    }
}
