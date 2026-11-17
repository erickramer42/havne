// File/layer: tests/test_sim_security.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SimSecurityTest, FakeSecurityTest (test_obd2)
//
// The simulated ECU's security access (core/src/sim_security.h, ADR-0066
// section 2.7): the header by itself (the seed, the key, each refusal in
// its order, the lockout and its delay on a clock the case steps by
// hand), then in the engine ECU's session (sim_ecu.h), through the
// unbound sim, and as the fakes' ECU answers it (tests/fake_ecus.h).

#include "ecuio/sim.h"
#include "fake_ecus.h"
#include "sim_ecu.h"
#include "sim_security.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

constexpr int64_t kDelay = ECUIO_SIM_SECURITY_DELAY_US;

/* One ECU in the extended session, its clock the case's */
struct Ecu {
    ecuio_sim_security s;
    bool extended = true;
    int64_t now = 1000000;

    Bytes ask(const Bytes& req)
    {
        uint8_t out[32] = {};
        const uint32_t n =
            ecuio_sim_security_answer(s, extended, kDelay, now, req.data(),
                                      static_cast<uint32_t>(req.size()), out);
        return Bytes(out, out + n);
    }

    /* The right key for level's seed, as the header's transform has it */
    static Bytes key_for(uint8_t level)
    {
        const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
        Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
        req.resize(2u + l->bytes);
        ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
        return req;
    }

    static Bytes wrong_key_for(uint8_t level)
    {
        Bytes req = key_for(level);
        req.back() ^= 0x01;
        return req;
    }

    Bytes did()
    {
        uint8_t out[8] = {};
        const uint32_t n = ecuio_sim_security_did(s, extended, out);
        return Bytes(out, out + n);
    }
};

const Bytes kNrc12 = {0x7F, 0x27, 0x12};
const Bytes kNrc13 = {0x7F, 0x27, 0x13};
const Bytes kNrc24 = {0x7F, 0x27, 0x24};
const Bytes kNrc35 = {0x7F, 0x27, 0x35};
const Bytes kNrc36 = {0x7F, 0x27, 0x36};
const Bytes kNrc37 = {0x7F, 0x27, 0x37};
const Bytes kNrc7F = {0x7F, 0x27, 0x7F};

TEST(SimSecurityTest, TheKeyIsTheSeedReversedAndXoredWithHavne)
{
    /* Worked by hand from the header's rule, so a change to the
     * transform or to level 01's seed shows here */
    const uint8_t seed[4] = {0x5A, 0x17, 0xC3, 0x08};
    uint8_t key[4] = {};
    ecuio_sim_security_key(seed, 4, key);
    EXPECT_EQ(key[0], 0x08 ^ 'H');
    EXPECT_EQ(key[1], 0xC3 ^ 'A');
    EXPECT_EQ(key[2], 0x17 ^ 'V');
    EXPECT_EQ(key[3], 0x5A ^ 'N');
    const ecuio_sim_security_level* one = ecuio_sim_security_find(0x01);
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(Bytes(one->seed, one->seed + one->bytes), Bytes(seed, seed + 4));
    /* the letters go round past the fifth byte */
    const uint8_t six[6] = {1, 2, 3, 4, 5, 6};
    uint8_t out[6] = {};
    ecuio_sim_security_key(six, 6, out);
    EXPECT_EQ(out[4], 2 ^ 'E');
    EXPECT_EQ(out[5], 1 ^ 'H');
}

TEST(SimSecurityTest, ASeedThenItsKeyUnlocksTheLevel)
{
    Ecu e;
    EXPECT_EQ(e.did(), (Bytes{0x7F, 0x22, 0x33}));
    EXPECT_EQ(e.ask({0x27, 0x01}), (Bytes{0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08}));
    EXPECT_EQ(e.s.seed_out, 0x01);
    EXPECT_EQ(e.ask(Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
    EXPECT_EQ(e.s.unlocked, 0x01);
    EXPECT_EQ(e.s.seed_out, 0);
    EXPECT_EQ(e.did(), (Bytes{0x62, 0xD2, 0x10, 0x4B, 0x2D}));
}

TEST(SimSecurityTest, TheSixteenByteLevelUnlocksToo)
{
    Ecu e;
    const Bytes seed = e.ask({0x27, 0x03});
    ASSERT_EQ(seed.size(), 18u);
    EXPECT_EQ(seed[0], 0x67);
    EXPECT_EQ(seed[1], 0x03);
    const Bytes key = Ecu::key_for(0x03);
    ASSERT_EQ(key.size(), 18u); /* longer than a CAN frame */
    EXPECT_EQ(e.ask(key), (Bytes{0x67, 0x04}));
    EXPECT_EQ(e.s.unlocked, 0x03);
    EXPECT_EQ(e.did().size(), 5u);
}

TEST(SimSecurityTest, ASeedForALevelAlreadyUnlockedIsAllZeroes)
{
    Ecu e;
    e.ask({0x27, 0x01});
    e.ask(Ecu::key_for(0x01));
    EXPECT_EQ(e.ask({0x27, 0x01}), (Bytes{0x67, 0x01, 0, 0, 0, 0}));
    EXPECT_EQ(e.s.seed_out, 0) << "no key is owed for a zero seed";
    /* another level's seed is its own */
    EXPECT_EQ(e.ask({0x27, 0x03})[2], 0x11);
    /* and only the seed asked for last is out: a zero seed ends the wait
     * for that level's key */
    EXPECT_EQ(e.s.seed_out, 0x03);
    EXPECT_EQ(e.ask({0x27, 0x01}), (Bytes{0x67, 0x01, 0, 0, 0, 0}));
    EXPECT_EQ(e.ask(Ecu::key_for(0x03)), kNrc24);
    EXPECT_EQ(e.s.unlocked, 0x01);
    EXPECT_EQ(e.s.wrong, 0);
}

TEST(SimSecurityTest, EachRefusalComesInItsOrder)
{
    Ecu e;
    EXPECT_EQ(e.ask({0x27}), kNrc13) << "no sub-function";
    e.extended = false;
    EXPECT_EQ(e.ask({0x27}), kNrc7F) << "the session is judged first";
    EXPECT_EQ(e.ask({0x27, 0x01}), kNrc7F);
    EXPECT_EQ(e.ask({0x27, 0x05}), kNrc7F) << "the session before the level";
    EXPECT_EQ(e.did(), Bytes{})
        << "outside the session the DID is the caller's";
    e.extended = true;
    for (const unsigned sub : {0x00u, 0x05u, 0x06u, 0x7Fu, 0x81u, 0xFFu}) {
        EXPECT_EQ(e.ask({0x27, static_cast<uint8_t>(sub)}), kNrc12) << sub;
    }
    EXPECT_EQ(e.ask(Ecu::key_for(0x01)), kNrc24) << "a key with no seed out";
    EXPECT_EQ(e.ask({0x27, 0x02}), kNrc24)
        << "the sequence is judged before the key's length";
    EXPECT_EQ(e.ask({0x27, 0x01, 0x00}), kNrc13) << "a seed request too long";
    EXPECT_EQ(e.s.seed_out, 0);
    e.ask({0x27, 0x01});
    EXPECT_EQ(e.ask(Ecu::key_for(0x03)), kNrc24)
        << "another level's seed is out";
    Bytes shorter = Ecu::key_for(0x01);
    shorter.pop_back();
    EXPECT_EQ(e.ask(shorter), kNrc13);
    Bytes longer = Ecu::key_for(0x01);
    longer.push_back(0);
    EXPECT_EQ(e.ask(longer), kNrc13);
    EXPECT_EQ(e.s.seed_out, 0x01) << "a key of the wrong length spends no seed";
    EXPECT_EQ(e.s.wrong, 0);
    EXPECT_EQ(e.ask(Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
}

TEST(SimSecurityTest, AKeyIsAskedForOnce)
{
    Ecu e;
    e.ask({0x27, 0x01});
    EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x01)), kNrc35);
    EXPECT_EQ(e.ask(Ecu::key_for(0x01)), kNrc24)
        << "the right key after a wrong one needs a new seed";
    EXPECT_EQ(e.s.unlocked, 0);
    EXPECT_EQ(e.s.wrong, 1);
}

TEST(SimSecurityTest, AKeyWrongInAnyOneByteIsRefused)
{
    for (const uint8_t level : {uint8_t{0x01}, uint8_t{0x03}}) {
        const Bytes right = Ecu::key_for(level);
        for (size_t i = 2; i < right.size(); ++i) {
            Ecu e;
            e.ask({0x27, level});
            Bytes key = right;
            key[i] ^= 0x80;
            EXPECT_EQ(e.ask(key), kNrc35) << int{level} << ", byte " << i;
            EXPECT_EQ(e.s.unlocked, 0);
        }
    }
}

TEST(SimSecurityTest, TheThirdWrongKeyLocksAndTheDelayRefusesEverything)
{
    Ecu e;
    for (int i = 0; i < 2; ++i) {
        e.ask({0x27, 0x01});
        EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x01)), kNrc35) << i;
    }
    e.ask({0x27, 0x01});
    EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x01)), kNrc36);
    EXPECT_EQ(e.s.delay_until_us, e.now + kDelay);
    /* In the delay: a seed, a key, the other level, the right key */
    EXPECT_EQ(e.ask({0x27, 0x01}), kNrc37);
    EXPECT_EQ(e.ask(Ecu::key_for(0x01)), kNrc37);
    EXPECT_EQ(e.ask({0x27, 0x03}), kNrc37);
    e.now += kDelay - 1;
    EXPECT_EQ(e.ask({0x27, 0x01}), kNrc37) << "a microsecond short";
    /* a level that doesn't exist is still that, not the delay */
    EXPECT_EQ(e.ask({0x27, 0x05}), kNrc12);
    e.now += 1;
    EXPECT_EQ(e.ask({0x27, 0x01}).size(), 6u) << "the delay has passed";
    EXPECT_EQ(e.ask(Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
    EXPECT_EQ(e.s.wrong, 0) << "a right key zeroes the count";
}

TEST(SimSecurityTest, AfterTheDelayOneMoreWrongKeyLocksAgain)
{
    Ecu e;
    for (int i = 0; i < 3; ++i) {
        e.ask({0x27, 0x01});
        e.ask(Ecu::wrong_key_for(0x01));
    }
    EXPECT_EQ(e.s.wrong, ECUIO_SIM_SECURITY_WRONG_MAX - 1);
    e.now += kDelay;
    e.ask({0x27, 0x01});
    EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x01)), kNrc36)
        << "the count stayed one short of the lockout";
    EXPECT_EQ(e.ask({0x27, 0x01}), kNrc37);
    EXPECT_EQ(e.s.delay_until_us, e.now + kDelay) << "a new delay, from now";
}

TEST(SimSecurityTest, TheWrongKeysOfBothLevelsCountTogether)
{
    Ecu e;
    e.ask({0x27, 0x01});
    EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x01)), kNrc35);
    e.ask({0x27, 0x03});
    EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x03)), kNrc35);
    e.ask({0x27, 0x01});
    EXPECT_EQ(e.ask(Ecu::wrong_key_for(0x01)), kNrc36);
}

TEST(SimSecurityTest, ALockTakesTheUnlockAndTheSeedAndKeepsTheCount)
{
    Ecu e;
    e.ask({0x27, 0x01});
    e.ask(Ecu::wrong_key_for(0x01));
    e.ask({0x27, 0x01});
    e.ask(Ecu::key_for(0x01));
    e.ask({0x27, 0x03});
    e.ask(Ecu::wrong_key_for(0x03));
    e.ask({0x27, 0x03});
    ASSERT_EQ(e.s.unlocked, 0x01);
    ASSERT_EQ(e.s.seed_out, 0x03);
    ASSERT_EQ(e.s.wrong, 1);
    ecuio_sim_security_lock(e.s); /* the session changed */
    EXPECT_EQ(e.s.unlocked, 0);
    EXPECT_EQ(e.s.seed_out, 0);
    EXPECT_EQ(e.s.wrong, 1);
    EXPECT_EQ(e.did(), (Bytes{0x7F, 0x22, 0x33}));
    EXPECT_EQ(e.ask(Ecu::key_for(0x03)), kNrc24);
}

TEST(SimSecurityTest, ALockLeavesADelayRunning)
{
    Ecu e;
    for (int i = 0; i < 3; ++i) {
        e.ask({0x27, 0x01});
        e.ask(Ecu::wrong_key_for(0x01));
    }
    ecuio_sim_security_lock(e.s);
    EXPECT_EQ(e.ask({0x27, 0x01}), kNrc37);
}

/* ---------- in the engine ECU's session (sim_ecu.h) ---------- */

/* A node's session and the engine ECU behind it, the clock the case's */
struct Node {
    ecuio_sim_ecu ecu;
    ecuio_sim_session s;
    bool has_extended = true;
    int64_t now = 0;
    bool handled = false;

    Node()
    {
        std::memcpy(ecu.config.vin, ECUIO_SIM_DEFAULT_VIN,
                    sizeof ECUIO_SIM_DEFAULT_VIN);
    }

    /* As sim.cpp's node answers: the session's services first, then the
     * engine ECU's */
    Bytes ask(const Bytes& req)
    {
        uint8_t out[ECUIO_SIM_REPLY_MAX] = {};
        uint32_t n = ecuio_sim_session_answer(s, has_extended, now, req.data(),
                                              static_cast<uint32_t>(req.size()),
                                              out, &handled);
        if (!handled) {
            n = ecuio_sim_answer(ecu, s, req.data(),
                                 static_cast<uint32_t>(req.size()), false, out);
        }
        return Bytes(out, out + n);
    }

    void unlock()
    {
        ask({0x27, 0x01});
        EXPECT_EQ(ask(Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
    }
};

const Bytes kD210 = {0x22, 0xD2, 0x10};
const Bytes kD210Data = {0x62, 0xD2, 0x10, 0x4B, 0x2D};
const Bytes kDenied = {0x7F, 0x22, 0x33};
const Bytes kNoDid = {0x7F, 0x22, 0x31};
const Bytes kEntered = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};

TEST(SimSecurityTest, TheSessionAnswers27WhereANodeHasAnExtendedSession)
{
    Node n;
    EXPECT_EQ(n.ask({0x27, 0x01}), kNrc7F) << "the default session";
    EXPECT_TRUE(n.handled);
    EXPECT_EQ(n.ask({0x10, 0x03}), kEntered);
    EXPECT_EQ(n.ask({0x27, 0x01}).size(), 6u);
    EXPECT_TRUE(n.handled);
    EXPECT_EQ(n.ask(Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));

    /* A node with no extended session leaves 27 to its caller, which
     * answers it as any service it lacks */
    Node none;
    none.has_extended = false;
    EXPECT_EQ(none.ask({0x27, 0x01}), (Bytes{0x7F, 0x27, 0x11}));
    EXPECT_FALSE(none.handled);
    EXPECT_EQ(none.s.security.seed_out, 0);
}

TEST(SimSecurityTest, TheGatedDidFollowsTheSessionAndTheUnlock)
{
    Node n;
    EXPECT_EQ(n.ask(kD210), kNoDid) << "the default session, as D200";
    n.ask({0x10, 0x03});
    EXPECT_EQ(n.ask(kD210), kDenied);
    EXPECT_EQ(n.ask({0x22, 0xD2, 0x00}), (Bytes{0x62, 0xD2, 0x00, 0x1A, 0x2C}))
        << "the session's own DIDs need no unlock";
    n.unlock();
    EXPECT_EQ(n.ask(kD210), kD210Data);
    EXPECT_EQ(n.ask({0x22, 0xD2, 0x10, 0x00}), kNoDid) << "a longer request";

    /* An engine ECU asked with no node's security (the mock's) lacks it */
    uint8_t out[ECUIO_SIM_REPLY_MAX] = {};
    const uint32_t len =
        ecuio_sim_answer(n.ecu, kD210.data(), 3, false, out, true);
    EXPECT_EQ(Bytes(out, out + len), kNoDid);
}

TEST(SimSecurityTest, EverySessionEnteredOrLeftLocksIt)
{
    Node n;
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_EQ(n.ask({0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    EXPECT_EQ(n.ask({0x10, 0x02}), (Bytes{0x7F, 0x10, 0x12}));
    EXPECT_EQ(n.ask(kD210), kD210Data) << "a keep-alive and a refused session";

    EXPECT_EQ(n.ask({0x10, 0x03}), kEntered);
    EXPECT_EQ(n.ask(kD210), kDenied) << "the session entered anew";
    n.unlock();
    n.ask({0x10, 0x01});
    n.ask({0x10, 0x03});
    EXPECT_EQ(n.ask(kD210), kDenied) << "left and entered";

    n.unlock();
    n.now += ECUIO_SIM_S3_US - 1;
    EXPECT_EQ(n.ask(kD210), kD210Data) << "a microsecond inside S3";
    n.now += ECUIO_SIM_S3_US;
    EXPECT_EQ(n.ask(kD210), kNoDid) << "S3 of silence";
    EXPECT_EQ(n.s.security.unlocked, 0);
    n.ask({0x10, 0x03});
    EXPECT_EQ(n.ask(kD210), kDenied);
}

TEST(SimSecurityTest, A27KeepsTheSessionAsAnyRequestDoes)
{
    Node n;
    n.ask({0x10, 0x03});
    n.now += ECUIO_SIM_S3_US - 1;
    EXPECT_EQ(n.ask({0x27, 0x01}).size(), 6u);
    n.now += ECUIO_SIM_S3_US - 1;
    EXPECT_EQ(n.ask(Ecu::key_for(0x01)), (Bytes{0x67, 0x02}))
        << "S3 runs from the seed's request";
}

TEST(SimSecurityTest, ASessionLeftKeepsTheCountAndTheDelay)
{
    Node n;
    n.ask({0x10, 0x03});
    for (int i = 0; i < 3; ++i) {
        n.ask({0x27, 0x01});
        n.ask(Ecu::wrong_key_for(0x01));
    }
    EXPECT_EQ(n.ask({0x27, 0x01}), kNrc37);
    n.ask({0x10, 0x01});
    n.ask({0x10, 0x03});
    EXPECT_EQ(n.ask({0x27, 0x01}), kNrc37) << "a new session ends no delay";
    n.now += ECUIO_SIM_SECURITY_DELAY_US; /* past S3 too: back in default */
    EXPECT_EQ(n.ask({0x27, 0x01}), kNrc7F);
    n.ask({0x10, 0x03});
    n.ask({0x27, 0x01});
    EXPECT_EQ(n.ask(Ecu::wrong_key_for(0x01)), kNrc36)
        << "the count is the ECU's, not the session's";
}

/* ---------- through the unbound sim ---------- */

struct Unbound {
    ecuio_sim_t* sim = nullptr;
    int64_t t = 0;

    explicit Unbound(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    /* The one reply to req on id at the case's time, empty for none */
    Bytes ask(uint32_t id, const Bytes& req)
    {
        std::vector<ecuio_sim_reply_t> r(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(sim, id, req.data(),
                                static_cast<uint32_t>(req.size()), t, r.data(),
                                ECUIO_SIM_MAX_REPLIES, &n),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_LE(n, 1u);
        return n == 0 ? Bytes{} : Bytes(r[0].data, r[0].data + r[0].length);
    }
};

TEST(SimSecurityTest, AnUnboundSimUnlocksAndReadsTheGatedDid)
{
    Unbound u("mock://ask?values=canned&modules=2");
    ASSERT_NE(u.sim, nullptr);
    EXPECT_EQ(u.ask(0x7E0, kD210), kNoDid);
    EXPECT_EQ(u.ask(0x7E0, {0x27, 0x01}), kNrc7F);
    EXPECT_EQ(u.ask(0x7E0, {0x10, 0x03}), kEntered);
    EXPECT_EQ(u.ask(0x7E0, kD210), kDenied);
    EXPECT_EQ(u.ask(0x7E0, {0x27, 0x01}),
              (Bytes{0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08}));
    EXPECT_TRUE(u.ask(0x7DF, {0x27, 0x02, 0, 0, 0, 0}).empty())
        << "a functional 27 is unanswered";
    EXPECT_EQ(u.ask(0x7E0, Ecu::key_for(0x01)), (Bytes{0x67, 0x02}))
        << "and spent no seed";
    EXPECT_EQ(u.ask(0x7E0, kD210), kD210Data);

    /* The 16-byte level: a request and a reply longer than a frame */
    const Bytes seed = u.ask(0x7E0, {0x27, 0x03});
    ASSERT_EQ(seed.size(), 18u);
    EXPECT_EQ(u.ask(0x7E0, Ecu::key_for(0x03)), (Bytes{0x67, 0x04}));

    /* modules=2's module has no extended session and no security access */
    EXPECT_EQ(u.ask(0x7E1, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x11}));
    EXPECT_EQ(u.ask(0x7E1, kD210), kNoDid);

    EXPECT_EQ(u.ask(0x7E0, {0x10, 0x01}),
              (Bytes{0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_EQ(u.ask(0x7E0, {0x10, 0x03}), kEntered);
    EXPECT_EQ(u.ask(0x7E0, kD210), kDenied)
        << "the unlock went with the session";
}

TEST(SimSecurityTest, AnUnboundSimsLockoutRunsOnTheCallersClock)
{
    Unbound u("mock://ask?values=canned");
    ASSERT_NE(u.sim, nullptr);
    u.ask(0x7E0, {0x10, 0x03});
    for (int i = 0; i < 2; ++i) {
        u.ask(0x7E0, {0x27, 0x01});
        EXPECT_EQ(u.ask(0x7E0, Ecu::wrong_key_for(0x01)), kNrc35);
    }
    u.ask(0x7E0, {0x27, 0x01});
    EXPECT_EQ(u.ask(0x7E0, Ecu::wrong_key_for(0x01)), kNrc36);
    const int64_t locked = u.t;
    const int64_t ten_s = 10000000; /* the delay as it ships */
    /* The delay, the session kept by a keep-alive every 2 s; the ECU's
     * clock goes by ticks, so the case stays a tick clear of the edge */
    while (u.t + 2000000 < locked + ten_s) {
        u.t += 2000000;
        EXPECT_EQ(u.ask(0x7E0, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
        EXPECT_EQ(u.ask(0x7E0, {0x27, 0x01}), kNrc37) << u.t;
    }
    u.t = locked + ten_s - ECUIO_SIM_TICK_US;
    EXPECT_EQ(u.ask(0x7E0, {0x27, 0x01}), kNrc37) << "a tick short";
    u.t = locked + ten_s + ECUIO_SIM_TICK_US;
    EXPECT_EQ(u.ask(0x7E0, {0x27, 0x01}).size(), 6u);
    EXPECT_EQ(u.ask(0x7E0, Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
    EXPECT_EQ(u.ask(0x7E0, kD210), kD210Data);
}

/* ---------- the fakes' ECU (tests/fake_ecus.h) ---------- */

/* The fakes' state is the process's: each case starts and ends clean */
struct FakeSecurityTest : ::testing::Test {
    void SetUp() override { clean(); }
    void TearDown() override { clean(); }

    static void clean()
    {
        fake_ecu::set_module_dids(nullptr);
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_delay_ms() = 0;
    }

    static Bytes ask(uint32_t rx, const Bytes& req)
    {
        uint8_t out[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        if (!fake_ecu::answer_for(rx, req.data(), req.size(), -1, out, &n)) {
            return Bytes{};
        }
        return Bytes(out, out + n);
    }
};

TEST_F(FakeSecurityTest, TheEngineEcuUnlocksInItsSessionByItsReplyId)
{
    EXPECT_EQ(ask(0x7E8, {0x27, 0x01}), kNrc7F);
    EXPECT_EQ(ask(0x7E8, kD210), kNoDid)
        << "outside the session, a DID as any it lacks";
    ask(0x7E8, {0x10, 0x03});
    ask(0x7E9, {0x10, 0x03});
    EXPECT_EQ(ask(0x7E8, kD210), kDenied);
    EXPECT_EQ(ask(0x7E8, {0x27, 0x01}),
              (Bytes{0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08}));
    EXPECT_EQ(ask(0x7E9, Ecu::key_for(0x01)), kNrc24)
        << "the seed is out on the other ECU";
    EXPECT_EQ(ask(0x7E8, Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
    EXPECT_EQ(ask(0x7E8, kD210), kD210Data);
    EXPECT_EQ(ask(0x7E9, kD210), kDenied) << "the unlock is the ECU's own";
    EXPECT_EQ(ask(0x7E8, {0x22, 0xD2, 0x00}),
              (Bytes{0x62, 0xD2, 0x00, 0x1A, 0x2C}));

    /* The 16-byte level, its key's request 18 bytes */
    ASSERT_EQ(ask(0x7E8, {0x27, 0x03}).size(), 18u);
    EXPECT_EQ(ask(0x7E8, Ecu::key_for(0x03)), (Bytes{0x67, 0x04}));

    EXPECT_EQ(ask(0x7E8, {0x10, 0x03}), kEntered);
    EXPECT_EQ(ask(0x7E8, kD210), kDenied) << "the session entered anew";
    ask(0x7E8, {0x27, 0x01});
    ask(0x7E8, Ecu::key_for(0x01));
    ask(0x7E8, {0x10, 0x01});
    ask(0x7E8, {0x10, 0x03});
    EXPECT_EQ(ask(0x7E8, kD210), kDenied) << "left and entered";
    ask(0x7E8, {0x27, 0x01});
    ask(0x7E8, Ecu::key_for(0x01));
    fake_ecu::reset_security();
    EXPECT_EQ(ask(0x7E8, kD210), kDenied) << "reset_security locks every ECU";
}

TEST_F(FakeSecurityTest, AModuleHasNoSecurityAccess)
{
    ASSERT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12"));
    EXPECT_EQ(ask(0x7E9, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x11}));
    EXPECT_EQ(ask(0x7E9, kD210), kNoDid);
    EXPECT_EQ(ask(0x7E9, {0x10, 0x01}),
              (Bytes{0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_EQ(ask(0x7E9, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x11}));
}

TEST_F(FakeSecurityTest, TheLockoutsDelayIsTheKnobs)
{
    using clock = std::chrono::steady_clock;
    fake_ecu::security_delay_ms() = 300;
    ask(0x7E8, {0x10, 0x03});
    for (int i = 0; i < 2; ++i) {
        ask(0x7E8, {0x27, 0x01});
        EXPECT_EQ(ask(0x7E8, Ecu::wrong_key_for(0x01)), kNrc35);
    }
    ask(0x7E8, {0x27, 0x01});
    const clock::time_point before = clock::now();
    EXPECT_EQ(ask(0x7E8, Ecu::wrong_key_for(0x01)), kNrc36);
    /* 37 until the knob's time has passed since the 36, then a seed;
     * never the simulated ECU's own 10 s. The first ask is judged only
     * when it came inside the knob's time: a stalled runner can pass it. */
    Bytes got = ask(0x7E8, {0x27, 0x01});
    if (clock::now() - before < std::chrono::milliseconds(300)) {
        EXPECT_EQ(got, kNrc37) << "inside the delay";
    }
    const clock::time_point give_up = before + std::chrono::seconds(5);
    while (got == kNrc37 && clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        got = ask(0x7E8, {0x27, 0x01});
    }
    ASSERT_EQ(got.size(), 6u) << "the delay ended inside the case's bound";
    EXPECT_GE(clock::now() - before, std::chrono::milliseconds(300));
    EXPECT_EQ(ask(0x7E8, Ecu::key_for(0x01)), (Bytes{0x67, 0x02}));
}

} // namespace
