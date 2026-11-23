// File/layer: tests/test_sim_gateway_view.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: SimGatewayTest (test_obd2)
//
// The simulated gateway's view (ecuio_sim_get_gateway and
// ecuio_sim_get_gateway_override): the contracts, the routes as
// configured and as they stand by the lock, the flags and each count,
// copied as soon as the request that changes them is answered.

#include "sim_gateway_support.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace sim_gateway_test;

namespace {

constexpr uint32_t kGw = 0x6F0;
constexpr uint32_t kModule = 0x7E1;

const std::string kBase = "mock://gw?modules=2&gateway=6F0:6F8&behind=7E9";
const Bytes kD100 = {0x22, 0xD1, 0x00};

ecuio_sim_gateway_t view(const Sim& s)
{
    ecuio_sim_gateway_t v;
    std::memset(&v, 0xA5, sizeof v);
    EXPECT_EQ(ecuio_sim_get_gateway(s.sim, &v), ECUIO_OK)
        << ecuio_last_error_detail();
    return v;
}

ecuio_sim_gateway_override_t override_at(const Sim& s, uint32_t i)
{
    ecuio_sim_gateway_override_t o;
    std::memset(&o, 0xA5, sizeof o);
    EXPECT_EQ(ecuio_sim_get_gateway_override(s.sim, i, &o), ECUIO_OK)
        << ecuio_last_error_detail();
    return o;
}

Bytes made_up_key(const Bytes& seed_reply)
{
    const size_t n = seed_reply.size() - 2;
    Bytes key = {0x27, static_cast<uint8_t>(seed_reply[1] + 1)};
    const char word[] = "HAVNE";
    for (size_t i = 0; i < n; ++i) {
        key.push_back(
            static_cast<uint8_t>(seed_reply[2 + n - 1 - i] ^ word[i % 5]));
    }
    return key;
}

/* Every zero field of a view is zero */
void expect_zeroes(const ecuio_sim_gateway_t& v)
{
    for (uint32_t c = ECUIO_SIM_CLASS_COUNT; c < 8; ++c) {
        EXPECT_EQ(v.route[c], 0);
        EXPECT_EQ(v.route_nrc[c], 0);
        EXPECT_EQ(v.now[c], 0);
    }
    EXPECT_EQ(v.reserved0[0], 0);
    EXPECT_EQ(v.reserved0[1], 0);
    for (const uint32_t w : v.reserved) {
        EXPECT_EQ(w, 0u);
    }
}

} // namespace

TEST(SimGatewayTest, TheGettersContracts)
{
    ecuio_sim_gateway_t v;
    ecuio_sim_gateway_override_t o;
    EXPECT_EQ(ecuio_sim_get_gateway(nullptr, &v), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_get_gateway_override(nullptr, 0, &o),
              ECUIO_ERR_NULL_ARGUMENT);
    Sim none("mock://gw?modules=2");
    EXPECT_EQ(ecuio_sim_get_gateway(none.sim, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_get_gateway_override(none.sim, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    std::memset(&v, 0xA5, sizeof v);
    EXPECT_EQ(ecuio_sim_get_gateway(none.sim, &v), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the simulated ECU has no gateway (no gateway= in its URI)");
    EXPECT_EQ(v.request_id, 0xA5A5A5A5u) << "untouched";
    EXPECT_EQ(ecuio_sim_get_gateway_override(none.sim, 0, &o),
              ECUIO_ERR_INVALID_ARGUMENT);

    Sim s(kBase + "&route=22:refuse@7E9");
    ASSERT_EQ(ecuio_sim_get_gateway(s.sim, &v), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(v.overrides, 1u);
    std::memset(&o, 0xA5, sizeof o);
    EXPECT_EQ(ecuio_sim_get_gateway_override(s.sim, 1, &o),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the gateway has 1 route= entries for a service or a module");
    EXPECT_EQ(o.reply_id, 0xA5A5A5A5u) << "untouched";
}

// route[] and route_nrc[] as configured for the whole gateway, each entry
// for a service or a module by index, in the URI's order
TEST(SimGatewayTest, TheViewHoldsTheRoutesAsConfigured)
{
    Sim s(kBase + "&sa=key&route=reads:refuse.31,session:silent,write:gated,"
                  "prog:gated-silent,22:forward,19:gated.7F@7E9,"
                  "write:refuse@7E9&gwopt=aloud,unsuppressed,keep-any");
    const ecuio_sim_gateway_t v = view(s);
    EXPECT_EQ(v.request_id, 0x6F0u);
    EXPECT_EQ(v.reply_id, 0x6F8u);
    const uint8_t routes[5] = {
        ECUIO_SIM_ROUTE_REFUSE, ECUIO_SIM_ROUTE_SILENT, ECUIO_SIM_ROUTE_GATED,
        ECUIO_SIM_ROUTE_GATED_SILENT, ECUIO_SIM_ROUTE_FORWARD};
    for (uint32_t c = 0; c < ECUIO_SIM_CLASS_COUNT; ++c) {
        EXPECT_EQ(v.route[c], routes[c]) << c;
    }
    EXPECT_EQ(v.route_nrc[ECUIO_SIM_CLASS_READS], 0x31);
    EXPECT_EQ(v.route_nrc[ECUIO_SIM_CLASS_WRITE], 0);
    EXPECT_EQ(v.flags, ECUIO_SIM_GATEWAY_SECURITY | ECUIO_SIM_GATEWAY_KEEP_ANY |
                           ECUIO_SIM_GATEWAY_ALOUD |
                           ECUIO_SIM_GATEWAY_UNSUPPRESSED);
    expect_zeroes(v);
    ASSERT_EQ(v.overrides, 3u);

    const ecuio_sim_gateway_override_t a = override_at(s, 0);
    EXPECT_EQ(a.reply_id, 0u);
    EXPECT_EQ(a.service, 1);
    EXPECT_EQ(a.key, 0x22);
    EXPECT_EQ(a.route, ECUIO_SIM_ROUTE_FORWARD);
    EXPECT_EQ(a.nrc, 0);
    const ecuio_sim_gateway_override_t b = override_at(s, 1);
    EXPECT_EQ(b.reply_id, 0x7E9u);
    EXPECT_EQ(b.service, 1);
    EXPECT_EQ(b.key, 0x19);
    EXPECT_EQ(b.route, ECUIO_SIM_ROUTE_GATED);
    EXPECT_EQ(b.nrc, 0x7F);
    const ecuio_sim_gateway_override_t c = override_at(s, 2);
    EXPECT_EQ(c.reply_id, 0x7E9u);
    EXPECT_EQ(c.service, 0);
    EXPECT_EQ(c.key, ECUIO_SIM_CLASS_WRITE);
    EXPECT_EQ(c.route, ECUIO_SIM_ROUTE_REFUSE);
    for (const ecuio_sim_gateway_override_t& o : {a, b, c}) {
        EXPECT_EQ(o.reserved0[0] | o.reserved0[1] | o.reserved0[2], 0);
        EXPECT_EQ(o.reserved[0] | o.reserved[1] | o.reserved[2], 0u);
    }
    Sim plain("mock://gw?gateway=1&sa=never:5");
    EXPECT_EQ(view(plain).flags,
              ECUIO_SIM_GATEWAY_SECURITY | ECUIO_SIM_GATEWAY_NEVER);
    EXPECT_EQ(view(plain).overrides, 0u);
}

// now[] and each override's now follow the lock: a gated route refuses
// (or drops) while locked and forwards once unlocked, the rest stand;
// each relock (10 01, 10 03, S3) is counted
TEST(SimGatewayTest, NowFollowsTheLock)
{
    Sim s(kBase + "&sa=key&route=reads:gated,write:gated-silent,"
                  "prog:refuse,19:gated@7E9,session:gated-silent@7E9");
    const auto expect_now = [&](bool open, const char* when) {
        const ecuio_sim_gateway_t v = view(s);
        const uint8_t gated =
            open ? ECUIO_SIM_ROUTE_FORWARD : ECUIO_SIM_ROUTE_REFUSE;
        const uint8_t silent =
            open ? ECUIO_SIM_ROUTE_FORWARD : ECUIO_SIM_ROUTE_SILENT;
        EXPECT_EQ(v.now[ECUIO_SIM_CLASS_READS], gated) << when;
        EXPECT_EQ(v.now[ECUIO_SIM_CLASS_WRITE], silent) << when;
        EXPECT_EQ(v.now[ECUIO_SIM_CLASS_PROG], ECUIO_SIM_ROUTE_REFUSE) << when;
        EXPECT_EQ(v.now[ECUIO_SIM_CLASS_SESSION], ECUIO_SIM_ROUTE_FORWARD)
            << when;
        EXPECT_EQ(v.route[ECUIO_SIM_CLASS_READS], ECUIO_SIM_ROUTE_GATED)
            << "as configured, whatever the lock: " << when;
        EXPECT_EQ(override_at(s, 0).now, gated) << when;
        EXPECT_EQ(override_at(s, 1).now, silent) << when;
        EXPECT_EQ((v.flags & ECUIO_SIM_GATEWAY_UNLOCKED) != 0, open) << when;
        EXPECT_EQ(v.unlocked_level, open ? 1 : 0) << when;
    };
    expect_now(false, "at create");
    EXPECT_EQ(s.one(kGw, {0x10, 0x03}).at(0), 0x50);
    EXPECT_NE(view(s).flags & ECUIO_SIM_GATEWAY_EXTENDED, 0u);
    EXPECT_EQ(s.one(kGw, made_up_key(s.one(kGw, {0x27, 0x01}))),
              (Bytes{0x67, 0x02}));
    expect_now(true, "unlocked");
    EXPECT_EQ(view(s).relocks, 0u);
    EXPECT_EQ(s.one(kGw, {0x10, 0x01}).at(0), 0x50);
    expect_now(false, "after 10 01");
    EXPECT_EQ(view(s).flags & ECUIO_SIM_GATEWAY_EXTENDED, 0u);
    EXPECT_EQ(view(s).relocks, 1u);

    s.one(kGw, {0x10, 0x03});
    s.one(kGw, made_up_key(s.one(kGw, {0x27, 0x01})));
    s.one(kGw, {0x10, 0x03});
    expect_now(false, "after 10 03 anew");
    EXPECT_EQ(view(s).relocks, 2u);

    s.one(kGw, made_up_key(s.one(kGw, {0x27, 0x01})));
    expect_now(true, "unlocked again");
    s.now += ECUIO_SIM_S3_US + 100000;
    EXPECT_EQ(s.one(kModule, kD100), (Bytes{0x7F, 0x22, 0x33}));
    expect_now(false, "after S3");
    EXPECT_EQ(view(s).relocks, 3u) << "S3 found by a request routed";
}

// Each count, copied as soon as the request that moves it is answered
TEST(SimGatewayTest, TheViewCountsEachRequestAsItGoes)
{
    Sim s(kBase + "&sa=key&sa_delay=200&route=reads:gated,19:refuse,"
                  "write:silent");
    EXPECT_EQ(s.one(kModule, kD100), (Bytes{0x7F, 0x22, 0x33}));
    EXPECT_EQ(view(s).refused, 1u);
    EXPECT_EQ(s.one(kModule, {0x19, 0x02, 0xFF}), (Bytes{0x7F, 0x19, 0x22}));
    EXPECT_EQ(view(s).refused, 2u);
    EXPECT_TRUE(s.ask(kModule, {0x2E, 0xD1, 0x00, 0x01}).empty());
    EXPECT_EQ(view(s).dropped, 1u);
    s.one(kModule, {0x3E, 0x00});
    EXPECT_EQ(view(s).forwarded, 1u);
    EXPECT_TRUE(s.ask(0x7DF, kD100).empty());
    EXPECT_EQ(view(s).dropped, 2u) << "a functional request withheld";

    s.one(kGw, {0x10, 0x03});
    const Bytes seed = s.one(kGw, {0x27, 0x01});
    EXPECT_EQ(view(s).seeds, 1u);
    Bytes bad = made_up_key(seed);
    bad.back() ^= 1;
    EXPECT_EQ(s.one(kGw, bad), (Bytes{0x7F, 0x27, 0x35}));
    ecuio_sim_gateway_t v = view(s);
    EXPECT_EQ(v.keys, 1u);
    EXPECT_EQ(v.wrong_keys, 1u);
    EXPECT_EQ(v.wrong, 1);
    s.one(kGw, {0x27, 0x01});
    s.one(kGw, bad);
    s.one(kGw, {0x27, 0x01});
    EXPECT_EQ(s.one(kGw, bad), (Bytes{0x7F, 0x27, 0x36}));
    v = view(s);
    EXPECT_EQ(v.seeds, 3u);
    EXPECT_EQ(v.keys, 3u);
    EXPECT_EQ(v.wrong_keys, 3u);
    EXPECT_EQ(v.lockouts, 1u);
    EXPECT_NE(v.flags & ECUIO_SIM_GATEWAY_DELAYED, 0u);
    EXPECT_EQ(v.delayed_until_us, 200000) << "the ECU's time, from 0";
    EXPECT_EQ(v.too_early, 0u);
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x37}));
    EXPECT_EQ(s.one(kGw, {0x27, 0x02}), (Bytes{0x7F, 0x27, 0x37}));
    EXPECT_EQ(view(s).too_early, 2u) << "a 27 inside the delay, either kind";
    EXPECT_EQ(view(s).keys, 3u) << "a key refused 37 isn't judged";

    s.now = 300000;
    s.one(kGw, {0x3E, 0x00});
    EXPECT_EQ(view(s).flags & ECUIO_SIM_GATEWAY_DELAYED, 0u)
        << "the delay over by the ECU's time";
    s.one(kGw, {0x27, 0x01});
    EXPECT_EQ(view(s).flags & ECUIO_SIM_GATEWAY_DELAYED, 0u);
    EXPECT_EQ(view(s).delayed_until_us, 0) << "a 27 past it ended the delay";
    EXPECT_EQ(s.one(kGw, made_up_key(seed)), (Bytes{0x67, 0x02}));
    v = view(s);
    EXPECT_EQ(v.unlocks, 1u);
    EXPECT_EQ(v.keys, 4u);
    EXPECT_EQ(v.wrong, 0);
    EXPECT_EQ(v.seeds, 4u);
    EXPECT_EQ(s.one(kModule, kD100).at(0), 0x62);
    EXPECT_EQ(view(s).forwarded, 2u);
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}),
              (Bytes{0x67, 0x01, 0x00, 0x00, 0x00, 0x00}));
    EXPECT_EQ(view(s).seeds, 5u) << "a zero seed is handed out too";
    expect_zeroes(view(s));
}
