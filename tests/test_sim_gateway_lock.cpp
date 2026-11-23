// File/layer: tests/test_sim_gateway_lock.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: SimGatewayTest, SimSecurityTest (test_obd2)
//
// The simulated gateway's security access (sa=, sa_delay=, gwopt=keep-any)
// and the gated routes it opens: locked, unlocked together, locked again
// by its own session's end or the relock fault, the lockout and its
// delay, never, the fixed pairs and the named transforms, each key
// worked out in the case from the rule sim.h states, through the unbound
// sim at the case's time.

#include "sim_gateway_support.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace sim_gateway_test;

namespace {

constexpr uint32_t kGw = 0x6F0;
constexpr uint32_t kModule = 0x7E1;
constexpr int64_t kSecond = 1000000;

const std::string kBase = "mock://gw?modules=2&gateway=6F0:6F8&behind=7E9";
const Bytes kD100 = {0x22, 0xD1, 0x00};
const Bytes kD100Answer = {0x62, 0xD1, 0x00, 0x82};
const Bytes kLocked = {0x7F, 0x22, 0x33};

/* The made-up transform's key request for a seed reply (67 <level>
 * <seed>), as sim.h states it: the seed reversed, each byte XORed with a
 * letter of HAVNE */
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

Bytes wrong(Bytes key)
{
    key.back() ^= 0x01;
    return key;
}

/* The gateway's extended session entered and level 01's seed asked */
Bytes seed_of(Sim& s, uint8_t level = 0x01)
{
    EXPECT_EQ(s.one(kGw, {0x10, 0x03}).at(0), 0x50);
    return s.one(kGw, {0x27, level});
}

/* The case's clock moved to t, the gateway's session kept by tester
 * present every 4 s on the way */
void keep_until(Sim& s, int64_t t)
{
    while (s.now + 4 * kSecond < t) {
        s.now += 4 * kSecond;
        EXPECT_EQ(s.one(kGw, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    }
    s.now = t > s.now ? t : s.now;
    EXPECT_EQ(s.one(kGw, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
}

/* The gateway unlocked at level 01 by the made-up transform */
void unlock(Sim& s)
{
    const Bytes seed = seed_of(s);
    ASSERT_EQ(seed.size(), 6u);
    EXPECT_EQ(s.one(kGw, made_up_key(seed)), (Bytes{0x67, 0x02}));
}

} // namespace

// Locked, a gated class is refused 33 for the module (or the entry's
// code) and the module never hears it; an unlock opens every gated class
// at once, a gated-silent one too
TEST(SimGatewayTest, AnUnlockOpensEveryGatedClassTogether)
{
    Sim s(kBase + "&sa=key&route=reads:gated,write:gated-silent,10:gated.31@"
                  "7E9&faults=late:2@7E9");
    EXPECT_EQ(s.one(kModule, kD100), kLocked);
    EXPECT_TRUE(s.ask(kModule, {0x2E, 0xD1, 0x00, 0x01}).empty());
    EXPECT_EQ(s.one(kModule, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x31}));
    EXPECT_EQ(s.one(kModule, {0x3E, 0x00}), (Bytes{0x7E, 0x00}))
        << "tester present isn't gated: the module's first reply";
    const Bytes seed = seed_of(s);
    EXPECT_EQ(s.one(kModule, kD100), kLocked) << "a seed out unlocks nothing";
    EXPECT_EQ(s.one(kGw, wrong(made_up_key(seed))), (Bytes{0x7F, 0x27, 0x35}));
    EXPECT_EQ(s.one(kModule, kD100), kLocked) << "nor a wrong key";

    EXPECT_EQ(s.one(kGw, made_up_key(seed_of(s))), (Bytes{0x67, 0x02}));
    const std::vector<Reply> r = s.ask(kModule, kD100);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].data, kD100Answer);
    EXPECT_NE(r[0].flags & ECUIO_SIM_REPLY_LATE, 0u)
        << "the module's second reply: the withheld ones never counted";
    EXPECT_EQ(s.one(kModule, {0x2E, 0xD1, 0x00, 0x01}),
              (Bytes{0x7F, 0x2E, 0x11}))
        << "write forwards with reads, to a module that lacks it";
    EXPECT_EQ(s.one(kModule, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x12}))
        << "and the module's own 10";
}

// The unlock lasts the gateway's own session: 10 01, 10 03 anew and S3
// with no request on its own ID lock every gated class again; requests
// forwarded to a module keep no session, tester present to the gateway
// does
TEST(SimGatewayTest, TheUnlockLastsTheGatewaysOwnSession)
{
    Sim s(kBase + "&sa=key&route=reads:gated");
    unlock(s);
    EXPECT_EQ(s.one(kGw, {0x10, 0x01}).at(0), 0x50);
    EXPECT_EQ(s.one(kModule, kD100), kLocked) << "10 01 locks";

    unlock(s);
    EXPECT_EQ(s.one(kGw, {0x10, 0x03}).at(0), 0x50);
    EXPECT_EQ(s.one(kModule, kD100), kLocked) << "10 03 anew locks";

    unlock(s);
    const int64_t at = s.now;
    for (int64_t t = 1; t <= 4; ++t) {
        s.now = at + t * kSecond;
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer) << t << " s";
    }
    s.now = at + ECUIO_SIM_S3_US + 100000;
    EXPECT_EQ(s.one(kModule, kD100), kLocked)
        << "forwarded requests kept no session: S3 locked it";

    unlock(s);
    const int64_t kept = s.now;
    for (int64_t t = 1; t <= 12; ++t) {
        s.now = kept + t * kSecond;
        EXPECT_EQ(s.one(kGw, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer) << t << " s";
    }
    s.now += 2 * kSecond;
    EXPECT_TRUE(s.ask(kGw, {0x3E, 0x80}).empty());
    s.now += 4 * kSecond;
    EXPECT_EQ(s.one(kModule, kD100), kD100Answer)
        << "3E 80, silent, keeps it too";
}

// gwopt=keep-any: a request the gateway lets through keeps its session;
// one it withholds does not
TEST(SimGatewayTest, KeepAnyKeepsTheSessionByWhatGoesThrough)
{
    Sim s(kBase + "&sa=key&route=reads:gated,19:refuse&gwopt=keep-any");
    unlock(s);
    const int64_t at = s.now;
    for (int64_t t = 1; t <= 12; ++t) {
        s.now = at + t * kSecond;
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer) << t << " s";
    }
    const int64_t last = s.now;
    for (int64_t t = 1; t <= 4; ++t) {
        s.now = last + t * kSecond;
        EXPECT_EQ(s.one(kModule, {0x19, 0x02, 0xFF}),
                  (Bytes{0x7F, 0x19, 0x22}));
    }
    s.now = last + ECUIO_SIM_S3_US + 100000;
    EXPECT_EQ(s.one(kModule, kD100), kLocked)
        << "withheld requests kept no session";
}

// Three wrong keys answer 35, 35, 36; inside the delay every 27 is 37;
// after it one more wrong key locks again, a right one unlocks. The
// delay is sa_delay's, else the 10 s
TEST(SimGatewayTest, TheLockoutRunsTheGatewaysDelay)
{
    for (const auto& c : std::vector<std::pair<std::string, int64_t>>{
             {"&sa=key&sa_delay=200", 200000},
             {"&sa=key", ECUIO_SIM_SECURITY_DELAY_US},
             {"&sa_delay=1&sa=key", 1000}}) {
        Sim s(kBase + c.first + "&route=reads:gated");
        const Bytes key = made_up_key(seed_of(s));
        EXPECT_EQ(s.one(kGw, wrong(key)), (Bytes{0x7F, 0x27, 0x35}));
        s.one(kGw, {0x27, 0x01});
        EXPECT_EQ(s.one(kGw, wrong(key)), (Bytes{0x7F, 0x27, 0x35}));
        s.one(kGw, {0x27, 0x01});
        EXPECT_EQ(s.one(kGw, wrong(key)), (Bytes{0x7F, 0x27, 0x36})) << c.first;
        const int64_t locked = s.now;
        /* the ECU's clock ticks in 50 ms, the request's time within one */
        keep_until(s, locked + c.second - ECUIO_SIM_TICK_US - 1);
        EXPECT_EQ(s.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x37}))
            << c.first;
        keep_until(s, locked + c.second + ECUIO_SIM_TICK_US);
        s.one(kGw, {0x27, 0x01});
        EXPECT_EQ(s.one(kGw, wrong(key)), (Bytes{0x7F, 0x27, 0x36}))
            << "one more wrong key locks again: " << c.first;
        keep_until(s, s.now + c.second + 2 * ECUIO_SIM_TICK_US);
        s.one(kGw, {0x27, 0x01});
        EXPECT_EQ(s.one(kGw, key), (Bytes{0x67, 0x02})) << c.first;
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer);
    }
}

// never: every seed answered, every key refused as a wrong one, the
// right one too, the lockout and its delay as for any; nothing gated ever
// forwards
TEST(SimGatewayTest, NeverUnlocksForAnyKey)
{
    for (const std::string& form : {std::string("&sa=never&sa_delay=200"),
                                    std::string("&sa=never:200")}) {
        Sim s(kBase + form + "&route=reads:gated");
        EXPECT_EQ(s.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x7F}))
            << "outside the extended session";
        EXPECT_EQ(s.one(kGw, {0x10, 0x03}).at(0), 0x50);
        for (int k = 0; k < 3; ++k) {
            const Bytes seed = s.one(kGw, {0x27, 0x01});
            ASSERT_EQ(seed.size(), 6u) << form;
            EXPECT_EQ(
                s.one(kGw, made_up_key(seed)),
                (Bytes{0x7F, 0x27, static_cast<uint8_t>(k < 2 ? 0x35 : 0x36)}))
                << form << ", the right key " << k;
            EXPECT_EQ(s.one(kModule, kD100), kLocked);
        }
        keep_until(s, s.now + 200000 - ECUIO_SIM_TICK_US - 1);
        EXPECT_EQ(s.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x37})) << form;
        keep_until(s, s.now + 2 * ECUIO_SIM_TICK_US);
        const Bytes seed = s.one(kGw, {0x27, 0x01});
        ASSERT_EQ(seed.size(), 6u);
        EXPECT_EQ(s.one(kGw, made_up_key(seed)), (Bytes{0x7F, 0x27, 0x36}))
            << form << ": after the delay one more locks again";
        EXPECT_EQ(s.one(kModule, kD100), kLocked);
    }
}

// Fixed pairs: each level's seeds handed out in turn, each with its own
// key, a level without a pair refused 12; xor and add over the level's
// seed as sim.h states; each form takes sa_delay
TEST(SimGatewayTest, FixedPairsAndNamedTransformsUnlockAsNamed)
{
    const std::string pairs =
        "&sa=key:01020304:A1A2A3A4,11121314:B1B2B3B4&sa_delay=300";
    Sim s(kBase + pairs + "&route=reads:gated");
    EXPECT_EQ(seed_of(s), (Bytes{0x67, 0x01, 0x01, 0x02, 0x03, 0x04}));
    EXPECT_EQ(s.one(kGw, {0x27, 0x02, 0xB1, 0xB2, 0xB3, 0xB4}),
              (Bytes{0x7F, 0x27, 0x35}))
        << "the other pair's key";
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}),
              (Bytes{0x67, 0x01, 0x11, 0x12, 0x13, 0x14}))
        << "the next pair in turn";
    EXPECT_EQ(s.one(kGw, {0x27, 0x02, 0xB1, 0xB2, 0xB3, 0xB4}),
              (Bytes{0x67, 0x02}));
    EXPECT_EQ(s.one(kModule, kD100), kD100Answer);
    EXPECT_EQ(s.one(kGw, {0x27, 0x03}), (Bytes{0x7F, 0x27, 0x12}))
        << "no 16-byte pair: no level 03";
    EXPECT_EQ(s.one(kGw, {0x10, 0x03}).at(0), 0x50);
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}),
              (Bytes{0x67, 0x01, 0x01, 0x02, 0x03, 0x04}))
        << "round again";
    s.one(kGw, {0x27, 0x02, 0, 0, 0, 0});
    s.one(kGw, {0x27, 0x01});
    s.one(kGw, {0x27, 0x02, 0, 0, 0, 0});
    s.one(kGw, {0x27, 0x01});
    EXPECT_EQ(s.one(kGw, {0x27, 0x02, 0, 0, 0, 0}), (Bytes{0x7F, 0x27, 0x36}));
    s.now += 250000;
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x37}))
        << "sa_delay's 300 ms on pairs";
    s.now += 100000;
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}).at(0), 0x67);

    const Bytes seed16 = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    Sim sixteen(kBase + "&sa=key:00112233445566778899AABBCCDDEEFF:"
                        "FFEEDDCCBBAA99887766554433221100&route=reads:gated");
    const Bytes got = seed_of(sixteen, 0x03);
    EXPECT_EQ(Bytes(got.begin() + 2, got.end()), seed16);
    EXPECT_EQ(sixteen.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x12}));
    sixteen.one(kGw, {0x27, 0x03});
    Bytes key = {0x27, 0x04};
    for (size_t i = 0; i < 16; ++i) {
        key.push_back(static_cast<uint8_t>(0xFF - 0x11 * i));
    }
    EXPECT_EQ(sixteen.one(kGw, key), (Bytes{0x67, 0x04}));
    EXPECT_EQ(sixteen.one(kModule, kD100), kD100Answer);

    for (const bool add : {false, true}) {
        const std::string form =
            std::string(add ? "&sa=add:" : "&sa=xor:") + "0F01&sa_delay=300";
        Sim t(kBase + form + "&route=reads:gated");
        const Bytes seed = seed_of(t);
        ASSERT_EQ(seed.size(), 6u);
        Bytes k = {0x27, 0x02};
        const uint8_t mask[2] = {0x0F, 0x01};
        for (size_t i = 0; i < 4; ++i) {
            k.push_back(static_cast<uint8_t>(add ? seed[2 + i] + mask[i % 2]
                                                 : seed[2 + i] ^ mask[i % 2]));
        }
        EXPECT_EQ(t.one(kGw, made_up_key(seed)), (Bytes{0x7F, 0x27, 0x35}))
            << "the made-up transform's key isn't " << form << "'s";
        t.one(kGw, {0x27, 0x01});
        EXPECT_EQ(t.one(kGw, wrong(k)), (Bytes{0x7F, 0x27, 0x35}));
        t.one(kGw, {0x27, 0x01});
        EXPECT_EQ(t.one(kGw, wrong(k)), (Bytes{0x7F, 0x27, 0x36}));
        t.now += 250000;
        EXPECT_EQ(t.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x37}))
            << "sa_delay on " << form;
        t.now += 100000;
        t.one(kGw, {0x3E, 0x00});
        t.one(kGw, {0x27, 0x01});
        EXPECT_EQ(t.one(kGw, k), (Bytes{0x67, 0x02})) << form;
        EXPECT_EQ(t.one(kModule, kD100), kD100Answer) << form;
    }
}

// A key whose reply a fault puts off (late, pending) or drops (silent):
// the gateway's state changed when it judged the key, so a tester that
// asks again finds it already unlocked
TEST(SimGatewayTest, AKeysLateReplyComesAfterTheUnlock)
{
    for (const char* fault : {"late:3", "pending:3", "silent:3"}) {
        Sim s(kBase + "&sa=key&route=reads:gated&faults=" + fault + "@6F8");
        const Bytes seed = seed_of(s);
        const std::vector<Reply> r = s.ask(kGw, made_up_key(seed));
        /* its third reply: 50 03, the seed, then the key's */
        if (std::string(fault) == "silent:3") {
            EXPECT_TRUE(r.empty());
        } else {
            ASSERT_FALSE(r.empty()) << fault;
            EXPECT_EQ(r.back().data, (Bytes{0x67, 0x02})) << fault;
            EXPECT_GE(r.back().delay_us, ECUIO_SIM_LATE_US) << fault;
        }
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer)
            << fault << ": already unlocked";
        EXPECT_EQ(s.one(kGw, {0x27, 0x01}),
                  (Bytes{0x67, 0x01, 0x00, 0x00, 0x00, 0x00}))
            << fault << ": a seed of zeroes";
    }
}

// sa= and sa_delay='s grammar, and what needs what
TEST(SimGatewayTest, TheSecurityAccessKeys)
{
    const ecuio_sim_config pairs =
        parsed(kBase + "&sa=key:0A0B0C0D:01020304,"
                       "00112233445566778899AABBCCDDEEFF:"
                       "00112233445566778899AABBCCDDEEFF");
    EXPECT_EQ(pairs.gateway.keying.kind, ecuio_sim_keying_kind::pairs);
    EXPECT_EQ(pairs.gateway.keying.pair_count, 2u);
    EXPECT_EQ(pairs.gateway.keying.pairs[1].bytes, 16);
    const ecuio_sim_config masked = parsed(kBase + "&sa=xor:FF");
    EXPECT_EQ(masked.gateway.keying.mask_len, 1);
    EXPECT_TRUE(parsed(kBase + "&sa=never:600000").gateway.never);
    EXPECT_EQ(parsed(kBase + "&sa=never:600000").gateway.delay_us,
              600000LL * 1000);
    EXPECT_EQ(parsed(kBase + "&sa=key&sa=never").gateway.never, true)
        << "a later sa= replaces the earlier";
    EXPECT_EQ(parsed(kBase + "&sa=never&sa=key").gateway.never, false);
    // never:<ms>'s delay is that sa='s own; sa_delay='s outlives a later sa=
    const ecuio_sim_config again = parsed(kBase + "&sa=never:5000&sa=key");
    EXPECT_FALSE(again.gateway.delay_given);
    EXPECT_EQ(again.gateway.delay_us, 0);
    const ecuio_sim_config kept =
        parsed(kBase + "&sa=never:5000&sa_delay=7&sa=key");
    EXPECT_TRUE(kept.gateway.delay_given);
    EXPECT_EQ(kept.gateway.delay_us, 7LL * 1000);

    std::string nine = kBase + "&sa=key:";
    for (int i = 0; i < 9; ++i) {
        nine += std::string(i == 0 ? "" : ",") + "01020304:01020304";
    }
    for (const std::string& bad :
         {std::string("keys"), std::string("key:"), std::string("key:0102"),
          std::string("key:01020304"), std::string("key:01020304:0102"),
          std::string("key:010203:010203"),
          std::string("key:01020304:01020304,"), std::string("xor:"),
          std::string("xor:F"), std::string("add:zz"),
          std::string("xor:00112233445566778899AABBCCDDEEFF00"),
          std::string("never:"), std::string("never:0"),
          std::string("never:600001"), std::string("nevermore")}) {
        EXPECT_NE(refusal(kBase + "&sa=" + bad).find("sa: key (the simulated"),
                  std::string::npos)
            << bad;
    }
    EXPECT_NE(refusal(nine).find("sa: key"), std::string::npos) << "nine pairs";
    for (const char* bad : {"0", "600001", "1.5", "-1", "", "99999999999"}) {
        EXPECT_EQ(refusal(kBase + "&sa=key&sa_delay=" + bad),
                  "sa_delay: the lockout's delay in ms, 1 to 600000")
            << bad;
    }
    EXPECT_EQ(refusal(kBase + "&sa_delay=200"),
              "sa_delay is the lockout's delay of the gateway's security "
              "access: it needs sa=");
    for (const char* gated : {"reads:gated", "22:gated-silent@7E9"}) {
        EXPECT_EQ(refusal(kBase + "&route=" + gated),
                  "route: a gated class needs the gateway's security access, "
                  "sa=")
            << gated;
    }
    EXPECT_TRUE(refusal(kBase + "&route=reads:gated.7F&sa=key").empty());
    EXPECT_NE(refusal(kBase + "&route=reads:gated-silent.33&sa=key")
                  .find("only refuse and gated take"),
              std::string::npos);
    for (const char* alone : {"mock://gw?sa=key", "mock://gw?sa_delay=5"}) {
        EXPECT_NE(refusal(alone).find("they need gateway="), std::string::npos)
            << alone;
    }
    EXPECT_TRUE(parsed(kBase + "&gwopt=keep-any").gateway.keep_any);

    Sim plain("mock://gw?gateway=6F0:6F8&sa=key");
    EXPECT_EQ(plain.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x7F}))
        << "security access is the extended session's";
}

// never as a field of the engine's own state: the right key counted as
// a wrong one, while the field's default leaves every other case alone
TEST(SimSecurityTest, NeverRefusesTheRightKey)
{
    ecuio_sim_security s;
    EXPECT_FALSE(s.never);
    EXPECT_EQ(s.delay_us, ECUIO_SIM_SECURITY_DELAY_US);
    EXPECT_EQ(s.keying, nullptr);
    s.never = true;
    uint8_t out[32] = {};
    const uint8_t seed_req[2] = {0x27, 0x01};
    ASSERT_EQ(ecuio_sim_security_answer(s, true, 1000, 0, seed_req, 2, out),
              6u);
    uint8_t key[6] = {0x27, 0x02};
    ecuio_sim_security_key(out + 2, 4, key + 2);
    for (int k = 0; k < 3; ++k) {
        ecuio_sim_security_answer(s, true, 1000, 0, seed_req, 2, out);
        ASSERT_EQ(ecuio_sim_security_answer(s, true, 1000, 0, key, 6, out), 3u);
        EXPECT_EQ(out[2], k < 2 ? 0x35 : 0x36);
    }
    EXPECT_EQ(s.unlocked, 0);
    EXPECT_EQ(s.delay_until_us, 1000);
}

// faults=relock:<n>: every nth request let through on a gated route, on
// the gateway's own count, locks it again first and is withheld as the
// route is locked; other routes' requests count nothing
TEST(SimGatewayTest, ARelockLocksTheGatewayAgain)
{
    for (const char* aim : {"", "@6F8"}) {
        Sim s(kBase +
              "&sa=key&route=reads:gated,write:gated-silent&faults="
              "relock:3" +
              aim);
        unlock(s);
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer) << aim;
        EXPECT_EQ(s.one(kModule, {0x3E, 0x00}), (Bytes{0x7E, 0x00}))
            << "not gated: no count";
        EXPECT_EQ(s.one(kModule, {0x2E, 0xD1, 0x00, 0x01}),
                  (Bytes{0x7F, 0x2E, 0x11}));
        EXPECT_EQ(s.one(kModule, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
        EXPECT_EQ(s.one(kModule, kD100), kLocked)
            << aim << ": the third gated request locks it first";
        EXPECT_EQ(s.one(kModule, kD100), kLocked) << "and it stays locked";
        EXPECT_TRUE(s.ask(kModule, {0x2E, 0xD1, 0x00, 0x01}).empty());
        ecuio_sim_gateway_t v{};
        ASSERT_EQ(ecuio_sim_get_gateway(s.sim, &v), ECUIO_OK);
        EXPECT_EQ(v.relocks, 1u);
        EXPECT_EQ(v.unlocked_level, 0);
        EXPECT_NE(v.flags & ECUIO_SIM_GATEWAY_EXTENDED, 0u)
            << "the session stays; the unlock is lost";

        EXPECT_EQ(s.one(kGw, made_up_key(s.one(kGw, {0x27, 0x01}))),
                  (Bytes{0x67, 0x02}));
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer);
        EXPECT_EQ(s.one(kModule, kD100), kD100Answer);
        EXPECT_TRUE(s.ask(kModule, {0x2E, 0xD1, 0x00, 0x01}).empty())
            << aim << ": the sixth, gated silent, locks it again";
        ASSERT_EQ(ecuio_sim_get_gateway(s.sim, &v), ECUIO_OK);
        EXPECT_EQ(v.relocks, 2u);
    }
    Sim module_faults(kBase + "&sa=key&route=reads:gated&faults=relock:1@6F8,"
                              "late:1@7E9");
    unlock(module_faults);
    EXPECT_EQ(module_faults.one(kModule, kD100), kLocked)
        << "relock:1: the first gated request locks it";
}

// relock is the gateway's: with no gateway, or aimed at another node, it
// is refused after the profile, beside the aims
TEST(SimEcuTest, RelockIsTheGatewaysFault)
{
    const ecuio_sim_config c =
        parsed("mock://sim?faults=relock:4,relock:2@6F8,silent:3");
    EXPECT_EQ(c.faults.relock, 4u);
    ASSERT_EQ(c.aimed.count, 1u);
    EXPECT_EQ(c.aimed.items[0].kind, ecuio_sim_fault_kind::relock);
    EXPECT_STREQ(ecuio_sim_fault_name(ecuio_sim_fault_kind::relock), "relock");
    EXPECT_EQ(ecuio_sim_faults_for(c.faults, c.aimed, 0x6F8).relock, 2u);

    std::string why;
    EXPECT_EQ(create_status("mock://sim?faults=relock:2", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "faults: relock locks a gateway again: it needs gateway=");
    EXPECT_EQ(create_status("mock://sim?modules=2&faults=relock:2@7E9", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "faults: relock locks a gateway again: it needs gateway=");
    EXPECT_EQ(create_status(kBase + "&faults=relock:2@7E9", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "faults: relock:2@7E9 aims at a module: relock is the "
                   "gateway's alone (6F8)");
    EXPECT_EQ(create_status(kBase + "&faults=relock:2@6F8", &why), ECUIO_OK)
        << why;
    EXPECT_EQ(create_status(kBase + "&faults=relock:2", &why), ECUIO_OK) << why;
    EXPECT_EQ(create_status(kBase + "&faults=relock:2@7EA", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_NE(why.find("aims at a module the simulated car hasn't"),
              std::string::npos)
        << "an aim landing nowhere is the aims' refusal first: " << why;
}
