// File/layer: tests/test_sim_timing.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SimTimingTest, SimAskTest (test_obd2)
//
// The simulated ECU's reply timing (core/src/sim_timing.h): the draw's
// fixed numbers (a bench copying it pins the same), its median and 99th
// percentile, each module's own generator, the delay=, fc=, cf= and
// seed= grammar, and what the unbound ask hands back for each reply.

#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "sim_ecu.h"
#include "sim_timing.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace ecuio_sim_timing;
using Bytes = std::vector<uint8_t>;

sim_timing spread(int64_t p50, int64_t p99)
{
    sim_timing t;
    t.p50_us = p50;
    t.p99_us = p99;
    return t;
}

std::string refusal(const std::string& uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::bad)
        << uri;
    return why;
}

ecuio_sim_config parsed(const std::string& uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << uri << ": " << why;
    return c;
}

/* A profile whose modules (each rx with its tx 8 below) all hold DID
 * 1000, one byte, registered under name for the scope */
struct Profile {
    std::string name;
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    ecuio_signal_set_t set{};

    Profile(const char* n, std::vector<uint32_t> rx) : name(n)
    {
        for (const uint32_t id : rx) {
            ecuio_channel_decl_t c{};
            c.rx_id = id;
            c.tx_id = id - 8;
            channels.push_back(c);
            ecuio_signal_def_t d{};
            d.id = static_cast<uint16_t>(100 + defs.size());
            d.message_id = id;
            d.mux_length = 3;
            d.mux_key = 0x621000u;
            d.kind = ECUIO_VALUE_UINT;
            d.byte_order = ECUIO_BYTE_ORDER_BE;
            d.start_byte = 3;
            d.bit_length = 8;
            d.scale = 1.0;
            std::snprintf(d.name, sizeof d.name, "Module %X", id);
            defs.push_back(d);
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = static_cast<uint32_t>(defs.size());
        set.defs = defs.data();
        set.channel_count = static_cast<uint32_t>(channels.size());
        set.channels = channels.data();
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(name.c_str(), &set, &why), ECUIO_OK)
            << why.code << " at " << why.index;
    }
    ~Profile() { EXPECT_EQ(ecuio_profile_unregister(name.c_str()), ECUIO_OK); }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};

struct Unbound {
    ecuio_sim_t* sim = nullptr;
    explicit Unbound(const std::string& uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri.c_str(), 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    std::vector<ecuio_sim_reply_t> ask(uint32_t id, const Bytes& q)
    {
        std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(sim, id, q.data(),
                                static_cast<uint32_t>(q.size()), ECUIO_SIM_NOW,
                                out.data(), static_cast<uint32_t>(out.size()),
                                &n),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        out.resize(n);
        return out;
    }

    /* The engine ECU's delay for each of n requests of 01 0C (Single
     * Frame replies), -1 where none came */
    std::vector<int64_t> engine_delays(int n)
    {
        std::vector<int64_t> out;
        for (int i = 0; i < n; ++i) {
            int64_t d = -1;
            for (const ecuio_sim_reply_t& r : ask(0x7E0, {0x01, 0x0C})) {
                if (r.reply_id == 0x7E8) {
                    d = r.delay_us;
                }
            }
            out.push_back(d);
        }
        return out;
    }
};

} // namespace

// The draw's first numbers for seed 1 on 0x7E8, 3460:10930: pinned here
// and in the pacing bench's copy, so neither moves alone
TEST(SimTimingTest, TheDrawsFixedNumbers)
{
    std::mt19937_64 rng = sim_timing_rng(1, 0x7E8);
    const sim_timing t = spread(3460, 10930);
    const int64_t want[] = {6352, 6622, 2326, 1854, 2248};
    for (const int64_t w : want) {
        EXPECT_EQ(sim_timing_draw(t, rng), w);
    }
}

// The uniform stays inside (0, 1) at a generator's two extremes, where
// the top 53 bits would round to 1 and the quantile to NaN
TEST(SimTimingTest, TheUniformNeverReachesEitherEnd)
{
    EXPECT_LT(sim_timing_uniform(UINT64_MAX), 1.0);
    EXPECT_GT(sim_timing_uniform(0), 0.0);
    EXPECT_TRUE(std::isfinite(inverse_normal(sim_timing_uniform(UINT64_MAX))));
    EXPECT_TRUE(std::isfinite(inverse_normal(sim_timing_uniform(0))));
    EXPECT_GT(inverse_normal(sim_timing_uniform(UINT64_MAX)), 8.0);
}

// AS241's quantiles at points in its central region and its tails
TEST(SimTimingTest, TheInverseNormalHoldsAS241sPoints)
{
    EXPECT_NEAR(inverse_normal(0.75), 0.6744897501960817, 1e-14);
    EXPECT_NEAR(inverse_normal(0.9), 1.2815515655446004, 1e-14);
    EXPECT_NEAR(inverse_normal(0.975), 1.959963984540054, 1e-14);
    EXPECT_NEAR(inverse_normal(0.99), 2.326347874040841, 1e-14);
    EXPECT_NEAR(inverse_normal(1e-10), -6.361340902404056, 1e-12);
    EXPECT_NEAR(inverse_normal(0.25), -0.6744897501960817, 1e-14);
}

// 100,000 draws: their median and 99th percentile are the two points
TEST(SimTimingTest, TheDrawsPassThroughItsTwoPoints)
{
    std::mt19937_64 rng = sim_timing_rng(7, 0x7E9);
    const sim_timing t = spread(3500, 11000);
    std::vector<int64_t> d(100000);
    for (int64_t& x : d) {
        x = sim_timing_draw(t, rng);
    }
    std::sort(d.begin(), d.end());
    EXPECT_NEAR(static_cast<double>(d[d.size() / 2]), 3500.0, 3500.0 * 0.02);
    EXPECT_NEAR(static_cast<double>(d[d.size() * 99 / 100]), 11000.0,
                11000.0 * 0.02);
}

// p50 equal to p99 is a constant; no delay draws nothing, so the
// generator stands; a far tail meets the ceiling, never wraps
TEST(SimTimingTest, AConstantSpreadNoDelayAndTheCeiling)
{
    std::mt19937_64 rng = sim_timing_rng(1, 0x7E8);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(sim_timing_draw(spread(5000, 5000), rng), 5000);
    }
    std::mt19937_64 a = sim_timing_rng(1, 0x7E8);
    std::mt19937_64 b = sim_timing_rng(1, 0x7E8);
    EXPECT_EQ(sim_timing_draw(sim_timing{}, a), 0);
    EXPECT_EQ(a(), b()) << "no delay drew nothing";
    std::mt19937_64 wide = sim_timing_rng(3, 0x7E8);
    for (int i = 0; i < 100000; ++i) {
        const int64_t x = sim_timing_draw(spread(1, DELAY_MAX_US), wide);
        ASSERT_GE(x, 0);
        ASSERT_LE(x, static_cast<int64_t>(DRAW_MAX_US));
    }
}

// The grammar's forms, each aimed item in place of the key's for its
// module alone
TEST(SimTimingTest, TheKeysSetEachModulesTiming)
{
    const ecuio_sim_config c =
        parsed("mock://sim?modules=2&delay=3000:9000,900:30000@7E9"
               "&fc=4000&cf=5000,800@7e9&seed=18446744073709551615");
    const sim_timing engine = for_node(c.timing, 0x7E8);
    const sim_timing module = for_node(c.timing, 0x7E9);
    EXPECT_EQ(engine.p50_us, 3000);
    EXPECT_EQ(engine.p99_us, 9000);
    EXPECT_EQ(engine.fc_us, 4000u);
    EXPECT_EQ(engine.cf_us, 5000u);
    EXPECT_EQ(module.p50_us, 900);
    EXPECT_EQ(module.p99_us, 30000);
    EXPECT_EQ(module.fc_us, 4000u);
    EXPECT_EQ(module.cf_us, 800u);
    EXPECT_EQ(engine.seed, UINT64_MAX);
    const sim_timing none = for_node(parsed("mock://sim").timing, 0x7E8);
    EXPECT_EQ(none.p50_us, 0);
    EXPECT_EQ(none.fc_us, 0u);
    EXPECT_EQ(none.cf_us, 0u);
    EXPECT_EQ(none.seed, 1u);
}

TEST(SimTimingTest, TheKeysBadFormsAreRefused)
{
    for (const char* q :
         {"delay=0:10", "delay=10:5", "delay=5", "delay=1:10000001",
          "delay=a:b", "delay=10:20@800", "delay=10:20@7E8x", "delay=10:20@",
          "delay=", "delay=10:20,", "fc=1000001", "fc=-1", "cf=", "cf=1@",
          "seed=abc", "seed=18446744073709551616", "seed=-1"}) {
        const std::string why = refusal(std::string("mock://sim?") + q);
        const std::string key(q, std::strchr(q, '=') - q);
        EXPECT_EQ(why.rfind(key + ":", 0), 0u) << q << ": " << why;
    }
    EXPECT_EQ(refusal("mock://sim?delay=10:5"),
              "delay: '10:5' isn't <p50 us>:<p99 us>[@<reply ID>], p50 from "
              "1 to p99 and p99 at most 10000000, the reply ID 11-bit hex");
    EXPECT_EQ(refusal("mock://sim?fc=1000001"),
              "fc: '1000001' isn't <us>[@<reply ID>], from 0 to 1000000, the "
              "reply ID 11-bit hex");
}

// At most 32 aims for each key, and the three keys' together however
// often a key comes
TEST(SimTimingTest, TheAimsAreBounded)
{
    std::string items;
    for (int i = 0; i < 32; ++i) {
        items += (i == 0 ? "" : ",") + std::to_string(i) + "@7E8";
    }
    parsed("mock://sim?fc=" + items);
    EXPECT_EQ(refusal("mock://sim?fc=" + items + ",1@7E8"),
              "fc: more than 32 items aimed at a module");
    const std::string four = "mock://sim?fc=" + items + "&cf=" + items +
                             "&fc=" + items + "&cf=0@7E8";
    EXPECT_EQ(refusal(four), "cf: more than 32 items aimed at a module");
}

// The unbound ask's replies carry each module's timing: the draw, fc and
// cf; with no key every field is 0, as before the keys
TEST(SimAskTest, ARepliesTimingIsItsModules)
{
    Unbound none("mock://sim?modules=2");
    for (const ecuio_sim_reply_t& r : none.ask(0x7E0, {0x01, 0x0C})) {
        EXPECT_EQ(r.delay_us, 0);
        EXPECT_EQ(r.fc_us, 0u);
        EXPECT_EQ(r.cf_us, 0u);
        EXPECT_EQ(r.reserved[0] | r.reserved[1] | r.reserved[2], 0u);
    }
    Unbound s("mock://sim?modules=2&delay=2000:2000&fc=4000"
              "&cf=5000,800@7E9");
    const std::vector<ecuio_sim_reply_t> engine = s.ask(0x7E0, {0x01, 0x0C});
    ASSERT_EQ(engine.size(), 1u);
    EXPECT_EQ(engine[0].delay_us, 2000);
    EXPECT_EQ(engine[0].fc_us, 4000u);
    EXPECT_EQ(engine[0].cf_us, 5000u);
    const std::vector<ecuio_sim_reply_t> module =
        s.ask(0x7E1, {0x22, 0xD1, 0x00});
    ASSERT_EQ(module.size(), 1u);
    EXPECT_EQ(module[0].reply_id, 0x7E9u);
    EXPECT_EQ(module[0].delay_us, 2000);
    EXPECT_EQ(module[0].fc_us, 4000u);
    EXPECT_EQ(module[0].cf_us, 800u);
    EXPECT_EQ(module[0].reserved[0] | module[0].reserved[1] |
                  module[0].reserved[2],
              0u);
}

// Each module draws from its own generator: the engine ECU's draws are
// the same whether a second module answers beside it or not (the fixed
// numbers), and the module's, seeded with its own reply ID, are others
TEST(SimAskTest, AModulesDrawsAreItsOwn)
{
    Profile alone("sim-timing-alone", {0x7E8});
    Profile beside("sim-timing-beside", {0x7E8, 0x7E9});
    Unbound a("mock://sim?profile=sim-timing-alone&delay=3460:10930");
    Unbound b("mock://sim?profile=sim-timing-beside&delay=3460:10930");
    std::vector<int64_t> engine_a, engine_b, module_b;
    for (int i = 0; i < 5; ++i) {
        const auto ra = a.ask(0x7DF, {0x22, 0x10, 0x00});
        const auto rb = b.ask(0x7DF, {0x22, 0x10, 0x00});
        ASSERT_EQ(ra.size(), 1u);
        ASSERT_EQ(rb.size(), 2u) << "both modules answer each";
        engine_a.push_back(ra[0].delay_us);
        for (const ecuio_sim_reply_t& r : rb) {
            (r.reply_id == 0x7E8 ? engine_b : module_b).push_back(r.delay_us);
        }
    }
    EXPECT_EQ(engine_a, (std::vector<int64_t>{6352, 6622, 2326, 1854, 2248}));
    EXPECT_EQ(engine_a, engine_b);
    /* The module's own: seeded with its reply ID, not the engine's */
    EXPECT_EQ(module_b, (std::vector<int64_t>{7344, 1638, 2645, 4478, 2293}));
}

// seed= seeds every module's generator: another seed, other draws
TEST(SimAskTest, ASeedChangesTheDraws)
{
    Unbound s("mock://sim?delay=3460:10930&seed=7");
    EXPECT_EQ(s.engine_delays(5),
              (std::vector<int64_t>{2689, 6248, 4034, 5173, 2489}));
}

// A fault never moves the draws: a silent reply draws as any other, and
// a late one adds its time to its draw
TEST(SimAskTest, AFaultMovesNoDraw)
{
    Unbound silent("mock://sim?delay=3460:10930&faults=silent:2");
    EXPECT_EQ(silent.engine_delays(5),
              (std::vector<int64_t>{6352, -1, 2326, -1, 2248}));
    Unbound late("mock://sim?delay=3460:10930&faults=late:2");
    const std::vector<int64_t> d = late.engine_delays(3);
    EXPECT_EQ(d, (std::vector<int64_t>{6352, 6622 + ECUIO_SIM_LATE_US, 2326}));
}

// A pending reply keeps its own timing: its 7F xx 78 at once, the reply
// ECUIO_SIM_PENDING_US later, whatever the draw
TEST(SimAskTest, APendingReplyKeepsItsOwnTiming)
{
    Unbound s("mock://sim?delay=3460:10930&faults=pending:1");
    const std::vector<ecuio_sim_reply_t> r = s.ask(0x7E0, {0x01, 0x0C});
    ASSERT_EQ(r.size(), 2u);
    EXPECT_NE(r[0].flags & ECUIO_SIM_REPLY_PENDING, 0u);
    EXPECT_EQ(r[0].delay_us, 0);
    EXPECT_EQ(r[1].delay_us, ECUIO_SIM_PENDING_US);
}

// A timing item aimed at a module the car hasn't is refused at create
// as a fault's is, naming the modules it has
TEST(SimTest, ATimingsAimNeedsItsModule)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound("mock://sim?cf=800@7E9", 0, &sim),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(sim, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "cf: an item @7E9 aims at a module the simulated car "
                 "hasn't; it answers on 7E8");
    EXPECT_EQ(
        ecuio_sim_create_unbound("mock://sim?modules=2&cf=800@7E9", 0, &sim),
        ECUIO_OK);
    ecuio_sim_destroy(sim);
}
