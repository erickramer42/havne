// File/layer: tests/test_security_access.cpp | test_obd2, Qt-free
// Testing: UNIT: SecurityAccessTest (security_access.h: one module's
//          attempts, what each end counts and the delay after a failure,
//          with the time passed in)

#include "security_access.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace ecuio_security;

namespace {

constexpr int64_t kMs = 1000;
constexpr int64_t kS = 1000000;
/* Made-up levels: no module's own */
constexpr uint8_t kLevel = 0x01;
constexpr uint8_t kOther = 0x03;
constexpr uint32_t kDelayMs = 10000;
constexpr uint32_t kWindowMs = 500;
constexpr uint32_t kBound = 60; /* a transport that takes a whole request */
const uint8_t kSeedBytes[] = {0x12, 0x34, 0x56, 0x78};

module_access two_levels()
{
    module_access a;
    EXPECT_TRUE(add_level(a, kLevel, 4, 4, kDelayMs, kWindowMs));
    EXPECT_TRUE(add_level(a, kOther, 2, 6, 2 * kDelayMs, kWindowMs));
    return a;
}

/* An attempt on level up to its key offered, at now */
void to_key_sent(module_access& a, uint8_t level, int64_t now)
{
    ASSERT_EQ(ask(a, level, kBound, now).answer, ask_answer::go);
    const level_state& l = a.levels[a.in_flight];
    const std::vector<uint8_t> s(l.seed_bytes, 0x5A);
    ASSERT_EQ(seed(a, ecuio_talk_end::answered, 0, s.data(), l.seed_bytes, now),
              seed_answer::key);
    ASSERT_TRUE(key(a, l.key_bytes));
    key_offered(a);
    ASSERT_EQ(a.at, phase::key_sent);
}

} // namespace

TEST(SecurityAccessTest, ALevelIsAddedOnceAndOnlyWhileThereIsRoom)
{
    module_access a;
    EXPECT_TRUE(add_level(a, kLevel, 4, 4, kDelayMs, kWindowMs));
    EXPECT_FALSE(add_level(a, kLevel, 2, 2, kDelayMs, kWindowMs))
        << "a level twice";
    EXPECT_EQ(a.count, 1u);
    EXPECT_EQ(a.levels[0].seed_bytes, 4) << "the first stands";
    EXPECT_EQ(a.levels[0].delay_us, kDelayMs * kMs);
    EXPECT_EQ(a.levels[0].window_us, kWindowMs * kMs);
    for (uint32_t k = 1; k < kMaxLevels; ++k) {
        ASSERT_TRUE(add_level(a, static_cast<uint8_t>(2 * k + 1), 4, 4,
                              kDelayMs, kWindowMs));
    }
    EXPECT_EQ(a.count, kMaxLevels);
    EXPECT_FALSE(add_level(a, 0x7D, 4, 4, kDelayMs, kWindowMs)) << "full";
    EXPECT_EQ(a.count, kMaxLevels);
    EXPECT_EQ(find_level(a, kLevel), 0u);
    EXPECT_EQ(find_level(a, 0x05), 2u);
    EXPECT_EQ(find_level(a, 0x7D), a.count) << "none such";
    /* A seed of no bytes would read as all zeroes, and a key of none is
     * no key */
    module_access b;
    EXPECT_FALSE(add_level(b, kLevel, 0, 4, kDelayMs, kWindowMs));
    EXPECT_FALSE(add_level(b, kLevel, 4, 0, kDelayMs, kWindowMs));
    EXPECT_EQ(b.count, 0u);
    EXPECT_TRUE(add_level(b, kLevel, 1, 1, kDelayMs, kWindowMs));
}

/* A right key: go, the seed to the module's code, the key, unlocked */
TEST(SecurityAccessTest, ARightKeyUnlocksTheLevel)
{
    module_access a = two_levels();
    const asked r = ask(a, kLevel, kBound, 5 * kS);
    ASSERT_EQ(r.answer, ask_answer::go);
    EXPECT_EQ(r.index, 0u);
    EXPECT_EQ(a.at, phase::seed_asked);
    EXPECT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, 5 * kS),
              seed_answer::key);
    EXPECT_EQ(a.at, phase::key_wanted);
    EXPECT_TRUE(key(a, 4));
    EXPECT_EQ(a.at, phase::key_wanted) << "not offered yet";
    key_offered(a);
    EXPECT_EQ(a.at, phase::key_sent);
    EXPECT_EQ(key_end(a, ecuio_talk_end::answered, 5 * kS),
              key_answer::unlocked);
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_TRUE(a.levels[0].unlocked);
    EXPECT_FALSE(a.levels[1].unlocked) << "the level asked alone";
    EXPECT_EQ(a.levels[0].failed, 0u);
}

/* Each refusal sends nothing and starts no attempt, in this order: no
 * such level, a key past the transport's bound, an attempt under way,
 * the delay, the level unlocked */
TEST(SecurityAccessTest, AskRefusesInItsOrder)
{
    module_access a = two_levels();
    EXPECT_EQ(ask(a, 0x05, kBound, 0).answer, ask_answer::no_level);
    EXPECT_EQ(ask(a, 0x02, kBound, 0).answer, ask_answer::no_level)
        << "a key's sub-function is no level";
    /* 27, the sub-function and the key: 6 for kLevel, 8 for kOther */
    EXPECT_EQ(ask(a, kOther, 7, 0).answer, ask_answer::key_too_long);
    EXPECT_EQ(ask(a, kLevel, 5, 0).answer, ask_answer::key_too_long);
    EXPECT_EQ(a.at, phase::idle);
    ASSERT_EQ(ask(a, kLevel, 6, 0).answer, ask_answer::go)
        << "a key that just fits";
    EXPECT_EQ(ask(a, kOther, 7, 0).answer, ask_answer::key_too_long)
        << "the bound before the attempt under way";
    EXPECT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::busy);
    EXPECT_EQ(ask(a, kOther, kBound, 0).answer, ask_answer::busy)
        << "one attempt for the module, whatever the level";
    EXPECT_EQ(a.in_flight, 0u) << "the attempt under way is untouched";
    EXPECT_EQ(a.at, phase::seed_asked);
}

TEST(SecurityAccessTest, AnUnlockedLevelIsNotAskedAgain)
{
    module_access a = two_levels();
    to_key_sent(a, kLevel, 0);
    ASSERT_EQ(key_end(a, ecuio_talk_end::answered, 0), key_answer::unlocked);
    EXPECT_EQ(ask(a, kLevel, kBound, kS).answer, ask_answer::unlocked);
    EXPECT_EQ(a.at, phase::idle) << "no attempt";
    EXPECT_EQ(ask(a, kOther, kBound, kS).answer, ask_answer::go)
        << "another level is its own";
}

/* Every end of an offered key but 67 is one failed attempt and starts the
 * delay: the three codes ISO 14229 gives a wrong key, any other code, no
 * answer, and a write that failed */
TEST(SecurityAccessTest, EveryEndOfAKeyButItsPositiveReplyCountsOne)
{
    struct row {
        ecuio_talk_end end;
        const char* name;
    };
    const row rows[] = {
        {ecuio_talk_end::refused, "refused (35, 36, 37 or any code)"},
        {ecuio_talk_end::unanswered, "no answer"},
        {ecuio_talk_end::unsent, "its write failed"},
    };
    module_access a = two_levels();
    int64_t now = 0;
    uint32_t want = 0;
    for (const row& r : rows) {
        to_key_sent(a, kLevel, now);
        ASSERT_FALSE(::testing::Test::HasFailure()) << r.name;
        EXPECT_EQ(key_end(a, r.end, now + kMs), key_answer::failed) << r.name;
        EXPECT_EQ(a.levels[0].failed, ++want) << r.name;
        EXPECT_EQ(a.levels[0].wait_until_us, now + kMs + kDelayMs * kMs)
            << r.name;
        EXPECT_FALSE(a.levels[0].unlocked) << r.name;
        EXPECT_EQ(a.at, phase::idle) << r.name;
        now += kMs + kDelayMs * kMs;
    }
    EXPECT_EQ(a.levels[1].failed, 0u) << "the other level's count is its own";
}

/* A key whose window ended before its turn never touched the bus */
TEST(SecurityAccessTest, ALapsedKeyCountsNothing)
{
    module_access a = two_levels();
    to_key_sent(a, kLevel, 0);
    EXPECT_EQ(key_end(a, ecuio_talk_end::lapsed, kS), key_answer::not_counted);
    EXPECT_EQ(a.levels[0].failed, 0u);
    EXPECT_EQ(a.levels[0].wait_until_us, 0) << "no delay";
    EXPECT_FALSE(a.levels[0].unlocked);
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_EQ(ask(a, kLevel, kBound, kS).answer, ask_answer::go)
        << "the next ask goes at once";
}

TEST(SecurityAccessTest, ARightKeyZeroesTheCount)
{
    module_access a = two_levels();
    to_key_sent(a, kLevel, 0);
    ASSERT_EQ(key_end(a, ecuio_talk_end::refused, 0), key_answer::failed);
    to_key_sent(a, kLevel, kDelayMs * kMs);
    ASSERT_EQ(key_end(a, ecuio_talk_end::unanswered, kDelayMs * kMs),
              key_answer::failed);
    ASSERT_EQ(a.levels[0].failed, 2u);
    to_key_sent(a, kLevel, 2 * kDelayMs * kMs);
    EXPECT_EQ(key_end(a, ecuio_talk_end::answered, 2 * kDelayMs * kMs),
              key_answer::unlocked);
    EXPECT_EQ(a.levels[0].failed, 0u);
    EXPECT_EQ(a.levels[0].wait_until_us, 0);
}

/* The count stops at its range's end rather than wrap to zero */
TEST(SecurityAccessTest, TheCountNeverWraps)
{
    module_access a = two_levels();
    a.levels[0].failed = UINT32_MAX;
    to_key_sent(a, kLevel, 0);
    EXPECT_EQ(key_end(a, ecuio_talk_end::refused, 0), key_answer::failed);
    EXPECT_EQ(a.levels[0].failed, UINT32_MAX);
}

/* The delay inside a session, on the session's clock: refused one tick
 * short with the time left, taken on time */
TEST(SecurityAccessTest, TheDelayRunsFromTheFailureToItsLastTick)
{
    module_access a = two_levels();
    const int64_t failed_at = 3 * kS;
    to_key_sent(a, kLevel, 0);
    ASSERT_EQ(key_end(a, ecuio_talk_end::refused, failed_at),
              key_answer::failed);
    const int64_t until = failed_at + kDelayMs * kMs;
    asked r = ask(a, kLevel, kBound, failed_at);
    EXPECT_EQ(r.answer, ask_answer::delayed);
    EXPECT_EQ(r.left_us, kDelayMs * kMs) << "the whole delay at the failure";
    r = ask(a, kLevel, kBound, until - 1);
    EXPECT_EQ(r.answer, ask_answer::delayed);
    EXPECT_EQ(r.left_us, 1) << "one tick short";
    EXPECT_EQ(a.at, phase::idle) << "no attempt while it runs";
    EXPECT_EQ(ask(a, kOther, kBound, until - 1).answer, ask_answer::go)
        << "a level's delay is its own";
    EXPECT_EQ(ask(a, kLevel, kBound, until - 1).answer, ask_answer::busy)
        << "the attempt under way before the delay";
    ASSERT_TRUE(drop(a));
    EXPECT_EQ(ask(a, kLevel, kBound, until).answer, ask_answer::go)
        << "on time";
    EXPECT_EQ(a.levels[0].wait_until_us, 0);
}

/* Each level waits its own manifest's delay */
TEST(SecurityAccessTest, ALevelWaitsItsOwnDelay)
{
    module_access a = two_levels();
    to_key_sent(a, kOther, 0);
    ASSERT_EQ(key_end(a, ecuio_talk_end::refused, 0), key_answer::failed);
    EXPECT_EQ(ask(a, kOther, kBound, kDelayMs * kMs).answer,
              ask_answer::delayed);
    EXPECT_EQ(ask(a, kOther, kBound, 2 * kDelayMs * kMs).answer,
              ask_answer::go);
}

/* A seed's ends count nothing; 7F 27 36 and 7F 27 37, the module saying
 * its own delay runs, start the delay */
TEST(SecurityAccessTest, ASeedsEndsCountNothing)
{
    struct row {
        ecuio_talk_end end;
        uint8_t nrc;
        bool delay;
        const char* name;
    };
    const row rows[] = {
        {ecuio_talk_end::refused, 0x12, false, "refused, 12"},
        {ecuio_talk_end::refused, 0x35, false, "refused, 35"},
        {ecuio_talk_end::refused, 0x36, true, "refused, 36: the delay runs"},
        {ecuio_talk_end::refused, 0x37, true, "refused, 37: the delay runs"},
        {ecuio_talk_end::refused, 0x38, false, "refused, 38"},
        {ecuio_talk_end::unanswered, 0, false, "no answer"},
        {ecuio_talk_end::unanswered, 0x37, false, "no answer, a stale code"},
        {ecuio_talk_end::unsent, 0x36, false, "unsent, a stale code"},
        {ecuio_talk_end::unsent, 0, false, "its write failed"},
        {ecuio_talk_end::lapsed, 0, false, "an end a seed never has"},
    };
    for (const row& r : rows) {
        module_access a = two_levels();
        ASSERT_EQ(ask(a, kLevel, kBound, kS).answer, ask_answer::go) << r.name;
        EXPECT_EQ(seed(a, r.end, r.nrc, nullptr, 0, 2 * kS), seed_answer::ended)
            << r.name;
        EXPECT_EQ(a.at, phase::idle) << r.name;
        EXPECT_EQ(a.levels[0].failed, 0u) << r.name;
        EXPECT_EQ(a.levels[0].wait_until_us,
                  r.delay ? 2 * kS + kDelayMs * kMs : 0)
            << r.name;
        EXPECT_FALSE(a.levels[0].unlocked) << r.name;
    }
}

/* A seed of all zeroes at the declared length: unlocked already, no key */
TEST(SecurityAccessTest, AZeroSeedIsALevelAlreadyUnlocked)
{
    module_access a = two_levels();
    const uint8_t zero[4] = {};
    ASSERT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::go);
    EXPECT_EQ(seed(a, ecuio_talk_end::answered, 0, zero, 4, 0),
              seed_answer::unlocked);
    EXPECT_TRUE(a.levels[0].unlocked);
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_FALSE(key(a, 4)) << "no key is wanted";
    /* One byte that isn't zero is a seed */
    module_access b = two_levels();
    const uint8_t last[4] = {0, 0, 0, 1};
    ASSERT_EQ(ask(b, kLevel, kBound, 0).answer, ask_answer::go);
    EXPECT_EQ(seed(b, ecuio_talk_end::answered, 0, last, 4, 0),
              seed_answer::key);
    EXPECT_FALSE(b.levels[0].unlocked);
}

/* A seed of another length than the manifest declares ends the attempt:
 * the module's code is never handed bytes it wasn't written for */
TEST(SecurityAccessTest, ASeedOfAnotherLengthEndsTheAttempt)
{
    const uint8_t zero[5] = {};
    const uint8_t five[5] = {1, 2, 3, 4, 5};
    struct row {
        const uint8_t* bytes;
        uint32_t len;
        const char* name;
    };
    const row rows[] = {
        {kSeedBytes, 3, "one short"},  {five, 5, "one long"},
        {kSeedBytes, 0, "none"},       {zero, 3, "zeroes, one short"},
        {zero, 5, "zeroes, one long"}, {nullptr, 4, "no bytes"},
    };
    for (const row& r : rows) {
        module_access a = two_levels();
        ASSERT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::go) << r.name;
        EXPECT_EQ(seed(a, ecuio_talk_end::answered, 0, r.bytes, r.len, 0),
                  seed_answer::ended)
            << r.name;
        EXPECT_EQ(a.at, phase::idle) << r.name;
        EXPECT_FALSE(a.levels[0].unlocked) << r.name;
        EXPECT_EQ(a.levels[0].failed, 0u) << r.name;
        EXPECT_EQ(a.levels[0].wait_until_us, 0) << r.name;
    }
}

/* A key of another length than declared is never sent, and counts
 * nothing */
TEST(SecurityAccessTest, AKeyOfAnotherLengthIsNotSent)
{
    for (const uint32_t len : {0u, 3u, 5u, 58u}) {
        module_access a = two_levels();
        ASSERT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::go);
        ASSERT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, 0),
                  seed_answer::key);
        EXPECT_FALSE(key(a, len)) << len;
        EXPECT_EQ(a.at, phase::idle) << len;
        EXPECT_EQ(a.levels[0].failed, 0u) << len;
        EXPECT_EQ(a.levels[0].wait_until_us, 0) << len;
        key_offered(a);
        EXPECT_EQ(a.at, phase::idle) << "nothing to offer";
    }
}

/* A step told out of its turn changes nothing */
TEST(SecurityAccessTest, AStepOutOfItsTurnChangesNothing)
{
    module_access a = two_levels();
    EXPECT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, 0),
              seed_answer::no_seed);
    EXPECT_FALSE(key(a, 4));
    key_offered(a);
    EXPECT_EQ(key_end(a, ecuio_talk_end::refused, 0), key_answer::no_key);
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_EQ(a.levels[0].failed, 0u);
    EXPECT_EQ(a.levels[0].wait_until_us, 0);
    ASSERT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::go);
    EXPECT_FALSE(key(a, 4)) << "no seed yet";
    key_offered(a);
    EXPECT_EQ(key_end(a, ecuio_talk_end::answered, 0), key_answer::no_key);
    EXPECT_EQ(a.at, phase::seed_asked);
    EXPECT_FALSE(a.levels[0].unlocked) << "no key went: nothing unlocks";
    ASSERT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, 0),
              seed_answer::key);
    EXPECT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, 0),
              seed_answer::no_seed)
        << "a seed twice";
    EXPECT_EQ(key_end(a, ecuio_talk_end::refused, 0), key_answer::no_key)
        << "a key not offered has no end";
    EXPECT_EQ(a.at, phase::key_wanted);
    ASSERT_TRUE(key(a, 4));
    key_offered(a);
    EXPECT_EQ(key_end(a, ecuio_talk_end::none, 0), key_answer::no_key)
        << "not settled yet";
    EXPECT_EQ(a.at, phase::key_sent);
    EXPECT_EQ(a.levels[0].failed, 0u);
}

/* An attempt dropped before its key is offered counts nothing; once the
 * key is offered only its end closes the attempt */
TEST(SecurityAccessTest, AnAttemptIsDroppedOnlyBeforeItsKeyIsOffered)
{
    module_access a = two_levels();
    EXPECT_TRUE(drop(a)) << "nothing under way";
    ASSERT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::go);
    EXPECT_TRUE(drop(a));
    EXPECT_EQ(a.at, phase::idle);
    ASSERT_EQ(ask(a, kLevel, kBound, 0).answer, ask_answer::go);
    ASSERT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, 0),
              seed_answer::key);
    EXPECT_TRUE(drop(a));
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_EQ(a.levels[0].failed, 0u);
    EXPECT_EQ(a.levels[0].wait_until_us, 0);
    to_key_sent(a, kLevel, 0);
    EXPECT_FALSE(drop(a)) << "the key may be on the bus";
    EXPECT_EQ(a.at, phase::key_sent);
    EXPECT_EQ(key_end(a, ecuio_talk_end::unanswered, 0), key_answer::failed);
    EXPECT_EQ(a.levels[0].failed, 1u);
}

/* A session entered again locks every level and ends the attempt; with a
 * key on its way whose end wasn't told, that key counts */
TEST(SecurityAccessTest, ALostSessionLocksEveryLevel)
{
    module_access a = two_levels();
    to_key_sent(a, kLevel, 0);
    ASSERT_EQ(key_end(a, ecuio_talk_end::answered, 0), key_answer::unlocked);
    to_key_sent(a, kOther, 0);
    ASSERT_EQ(key_end(a, ecuio_talk_end::answered, 0), key_answer::unlocked);
    EXPECT_FALSE(session_lost(a, kS)) << "idle: nothing counted";
    EXPECT_FALSE(a.levels[0].unlocked);
    EXPECT_FALSE(a.levels[1].unlocked);
    EXPECT_EQ(ask(a, kLevel, kBound, kS).answer, ask_answer::go)
        << "locked again, it may be asked";
    EXPECT_FALSE(session_lost(a, kS)) << "a seed asked: nothing counted";
    EXPECT_EQ(a.at, phase::idle);
    ASSERT_EQ(ask(a, kLevel, kBound, kS).answer, ask_answer::go);
    ASSERT_EQ(seed(a, ecuio_talk_end::answered, 0, kSeedBytes, 4, kS),
              seed_answer::key);
    EXPECT_FALSE(session_lost(a, kS)) << "a key wanted: nothing counted";
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_EQ(a.levels[0].failed, 0u);
    EXPECT_EQ(a.levels[0].wait_until_us, 0);
    to_key_sent(a, kLevel, kS);
    EXPECT_TRUE(session_lost(a, 2 * kS)) << "a key on its way counts";
    EXPECT_EQ(a.at, phase::idle);
    EXPECT_EQ(a.levels[0].failed, 1u);
    EXPECT_EQ(a.levels[0].wait_until_us, 2 * kS + kDelayMs * kMs);
    EXPECT_EQ(a.levels[1].failed, 0u);
    EXPECT_EQ(key_end(a, ecuio_talk_end::refused, 3 * kS), key_answer::no_key)
        << "its end told afterwards counts no second time";
    EXPECT_EQ(a.levels[0].failed, 1u);
}

/* The delay across sessions, by the store's UTC second: never under 0,
 * never over one whole delay, and a second long since both seconds are
 * cut down */
TEST(SecurityAccessTest, AStoredDelayIsRebuiltFromTheUtcSecond)
{
    const int64_t d = kDelayMs * kMs; /* 10 s */
    const int64_t last = 1795132800;
    EXPECT_EQ(stored_delay_left_us(d, 0, last), 0) << "no failure kept";
    EXPECT_EQ(stored_delay_left_us(d, 0, 3), 0)
        << "none kept, whatever the clock says";
    EXPECT_EQ(stored_delay_left_us(d, last, last), d)
        << "the same second: the whole delay, never more";
    EXPECT_EQ(stored_delay_left_us(d, last, last + 1), d)
        << "a second on may be an instant on";
    EXPECT_EQ(stored_delay_left_us(d, last, last + 2), d - kS);
    EXPECT_EQ(stored_delay_left_us(d, last, last + 10), kS)
        << "the delay's own length on: a second may be left";
    EXPECT_EQ(stored_delay_left_us(d, last, last + 11), 0) << "on time";
    EXPECT_EQ(stored_delay_left_us(d, last, last + 12), 0);
    EXPECT_EQ(stored_delay_left_us(d, last, last - 1), d)
        << "the clock set back a second";
    EXPECT_EQ(stored_delay_left_us(d, last, last - 86400), d)
        << "set back a day costs one delay, not a day";
    EXPECT_EQ(stored_delay_left_us(d, last, last + 86400), 0)
        << "set forward ends it";
    EXPECT_EQ(stored_delay_left_us(d, 1, INT64_MAX), 0) << "no overflow";
    EXPECT_EQ(stored_delay_left_us(d, INT64_MAX, 1), d);
    /* A delay that isn't whole seconds */
    EXPECT_EQ(stored_delay_left_us(1500 * kMs, last, last + 2), 500 * kMs);
    EXPECT_EQ(stored_delay_left_us(1500 * kMs, last, last + 3), 0);
}

TEST(SecurityAccessTest, ARestoredLevelWaitsWhatIsLeftOfItsDelay)
{
    module_access a = two_levels();
    const int64_t last = 1795132800;
    const int64_t now = 7 * kS;
    EXPECT_TRUE(restore_level(a, 0, 3, last, last + 4, now));
    EXPECT_EQ(a.levels[0].failed, 3u);
    EXPECT_EQ(a.levels[0].wait_until_us, now + 7 * kS)
        << "10 s less the 4 gone, a second long";
    asked r = ask(a, kLevel, kBound, now + 7 * kS - 1);
    EXPECT_EQ(r.answer, ask_answer::delayed);
    EXPECT_EQ(r.left_us, 1);
    EXPECT_EQ(ask(a, kLevel, kBound, now + 7 * kS).answer, ask_answer::go);
    /* A failure long past leaves the count and no wait */
    EXPECT_TRUE(restore_level(a, 1, 2, last, last + 3600, now));
    EXPECT_EQ(a.levels[1].failed, 2u);
    EXPECT_EQ(a.levels[1].wait_until_us, 0);
    /* find_level's answer for no such level restores nothing */
    ASSERT_EQ(find_level(a, 0x7D), a.count);
    EXPECT_FALSE(restore_level(a, a.count, 9, last, last, now));
    EXPECT_EQ(a.levels[a.count].failed, 0u);
    EXPECT_EQ(a.levels[a.count].wait_until_us, 0);
    /* A session's first instant is time 0: a wait still stands */
    module_access b = two_levels();
    ASSERT_TRUE(restore_level(b, 0, 1, last, last, 0));
    EXPECT_EQ(b.levels[0].wait_until_us, kDelayMs * kMs);
    EXPECT_EQ(ask(b, kLevel, kBound, 0).answer, ask_answer::delayed);
}
