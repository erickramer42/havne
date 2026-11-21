// File/layer: tests/test_j2534_hold_absent.cpp | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest (core/src/j2534_hold.h: what an absent or
//          sleeping module costs the others under the hold, on the hold's
//          test bus with the time passed in)

#include "j2534_hold_support.h"

using namespace j2534_hold_test;

namespace {

Bytes did_reply(const ecuio_request& q, unsigned data)
{
    Bytes out = {0x62, q.bytes[1], q.bytes[2]};
    out.insert(out.end(), data, uint8_t{0x2A});
    return out;
}

bool identity_item(const ecuio_request& q)
{
    return q.bytes[0] == 0x09 || (q.bytes[0] == 0x22 && q.bytes[1] == 0xF1);
}

/* Modules of rows DIDs each, identification on every one: the present
 * ones (the first) refuse Service 09, answer each F1xx DID in 20 bytes and
 * each row in one frame, the long ones' rows in three. The others answer
 * nothing until wake_us (never when 0). */
struct Shape {
    uint32_t present;
    uint32_t absent;
    uint32_t rows;
    bool absent_long = false; /* the others' rows read long, and answer so */
    bool first_long = false;  /* module 0's first row too */
    int64_t wake_us = 0;
};

struct Count {
    size_t first_3s = 0; /* the present modules' rows answered */
    size_t lost = 0;
    size_t lost_after_wake = 0;
    size_t lost_settled = 0; /* from a second after the wake */
};

void add_rows(Bus& b, const Shape& s)
{
    const uint32_t chans = s.present + s.absent;
    for (uint32_t c = 0; c < chans; ++c) {
        for (uint32_t k = 0; k < s.rows; ++k) {
            const uint8_t did[3] = {0x22, static_cast<uint8_t>(0x10 + c),
                                    static_cast<uint8_t>(k)};
            ASSERT_EQ(ecuio_engine_add(&b.e, c, did, 3),
                      ecuio_engine_add_result::added);
            const bool long_row = (c >= s.present && s.absent_long) ||
                                  (c == 0 && k == 0 && s.first_long);
            b.e.requests[b.e.count - 1].long_defs = long_row ? 1 : 0;
        }
    }
}

Count run(const Shape& s, int64_t until_us = 3000 * kMs)
{
    Count out;
    Bus b;
    EXPECT_TRUE(ecuio_engine_init(&b.e, s.present + s.absent));
    b.e.gap_us = 5 * kMs;
    add_rows(b, s);
    ecuio_engine_identify(&b.e);
    b.lose = true;
    b.reply_of = [&b, s](uint32_t c, const ecuio_request& q) {
        const bool other = c >= s.present;
        if (other && (s.wake_us == 0 || b.now < s.wake_us)) {
            return Bytes{};
        }
        if (identity_item(q)) {
            return q.bytes[0] == 0x09 ? Bytes{0x7F, 0x09, 0x11}
                                      : did_reply(q, 17);
        }
        const bool long_row = (other && s.absent_long) ||
                              (c == 0 && q.bytes[2] == 0x00 && s.first_long);
        return did_reply(q, long_row ? 17u : 2u);
    };
    size_t at_wake = 0;
    size_t at_settled = 0;
    while (b.now < until_us) {
        if (s.wake_us != 0 && b.now < s.wake_us) {
            at_wake = b.lost;
        }
        if (s.wake_us != 0 && b.now < s.wake_us + 1000 * kMs) {
            at_settled = b.lost;
        }
        b.pass();
    }
    for (const Exchange& x : b.exchanges) {
        if (x.c < s.present && x.answered && x.sent_us < 3000 * kMs &&
            x.bytes[0] == 0x22 && x.bytes[1] != 0xF1) {
            ++out.first_3s;
        }
    }
    out.lost = b.lost;
    out.lost_after_wake = s.wake_us != 0 ? b.lost - at_wake : 0;
    out.lost_settled = s.wake_us != 0 ? b.lost - at_settled : 0;
    return out;
}

} // namespace

/* An absent module holds no one once a wait of its own has run out: four
 * present modules beside 14 absent ones of one-frame rows answer in the
 * first 3 s about what they answer alone (92% here; held to 85%), and
 * nothing is lost. An absent module whose first ask reads long still
 * holds the bus for that one wait (40% here; held above 30%, where it
 * read 1% before) */
TEST(J2534HoldTest, AbsentModulesHoldNoOne)
{
    const Count alone = run({4, 0, 20});
    const Count absent = run({4, 14, 20});
    const Count absent_long = run({4, 14, 20, true});
    ASSERT_GT(alone.first_3s, 1000u);
    EXPECT_GE(absent.first_3s, alone.first_3s * 85 / 100);
    EXPECT_GE(absent_long.first_3s, alone.first_3s * 30 / 100);
    EXPECT_EQ(absent.lost + absent_long.lost, 0u);
}

/* Each module's first ask is judged by its defs, unheard or not: a module
 * whose first row reads long asks it alone at connect, so 18 present
 * modules lose no reply */
TEST(J2534HoldTest, AFirstAskThatReadsLongCostsNoReplyAtConnect)
{
    Shape s{18, 0, 20};
    s.first_long = true;
    const Count c = run(s);
    EXPECT_EQ(c.lost, 0u);
}

/* A module of long rows asleep at connect, every row's wait run out, then
 * awake at 5 s: its first frame after so long a silence takes back its
 * rows' waits run out, so each row's first answer is judged by its defs
 * again and goes alone. The waking reply itself, judged one frame, can
 * cost each other module one Single Frame, once (3 here; 24 with the
 * waits run out kept), and nothing after */
TEST(J2534HoldTest, AModuleAwakeAfterASilenceIsJudgedByItsDefsAgain)
{
    Shape s{3, 1, 20, true};
    s.wake_us = 5000 * kMs;
    const Count c = run(s, 10000 * kMs);
    EXPECT_LE(c.lost_after_wake, size_t{s.present}) << "one each, at the wake";
    EXPECT_EQ(c.lost_settled, 0u);
    EXPECT_EQ(c.lost, c.lost_after_wake) << "nothing while it slept";
}

/* A channel whose rows all ran out wakes at its first frame after a run
 * of J2534_HOLD_ASLEEP_MISSES waits that also covers its whole rotation:
 * a rotation of 3 rows not at 3 or 7, at 8 (a small module losing a few
 * replies in a row keeps its rows' one frame); one of 20 not at 19, at
 * 20 */
TEST(J2534HoldTest, ASmallRotationWakesNoSoonerThanEightMisses)
{
    const auto woken_after = [](uint8_t rows, uint32_t misses) {
        ecuio_request_engine e;
        EXPECT_TRUE(ecuio_engine_init(&e, 1));
        for (uint8_t k = 0; k < rows; ++k) {
            const uint8_t did[3] = {0x22, 0x1E, k};
            EXPECT_EQ(ecuio_engine_add(&e, 0, did, 3),
                      ecuio_engine_add_result::added);
            e.requests[e.count - 1].reply_frames = 1;
            e.requests[e.count - 1].ran_out = 1;
        }
        EXPECT_EQ(e.chans[0].requests, uint32_t{rows});
        j2534_hold h;
        h.misses[0] = misses;
        j2534_hold_answered(h, &e, 0);
        return e.requests[0].ran_out == 0 && e.requests[0].reply_frames == 0;
    };
    for (const uint32_t m : {3u, 7u}) {
        EXPECT_FALSE(woken_after(3, m)) << m << " waits in a row";
    }
    EXPECT_TRUE(woken_after(3, J2534_HOLD_ASLEEP_MISSES));
    EXPECT_FALSE(woken_after(20, 19));
    EXPECT_TRUE(woken_after(20, 20));
}

/* The hold's judgment of a long row, by its module's state: a first ask
 * by its defs; unheard after a wait run out, one frame; heard, by its
 * defs through one lost reply; past its first waits (silent, or its
 * probe), one frame */
TEST(J2534HoldTest, AModuleIsJudgedByItsDefsUntilItsSilenceSaysAbsent)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 2));
    const uint8_t did[3] = {0x22, 0x1E, 0x00};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    e.requests[0].long_defs = 1;
    j2534_hold h;
    EXPECT_TRUE(j2534_hold_long(h, &e, 0, 0)) << "a first ask";
    h.misses[0] = 1;
    EXPECT_FALSE(j2534_hold_long(h, &e, 0, 0)) << "unheard, a wait run out";
    j2534_hold_answered(h, &e, 0);
    h.misses[0] = 1;
    EXPECT_TRUE(j2534_hold_long(h, &e, 0, 0)) << "heard, one reply lost";
    for (const uint32_t m :
         {J2534_HOLD_SILENT_MISSES, J2534_HOLD_PROBE_EVERY}) {
        h.misses[0] = m;
        EXPECT_FALSE(j2534_hold_long(h, &e, 0, 0)) << m << " waits in a row";
    }
}

/* A module that answers every row but a run it never answers keeps those
 * rows' waits run out: a silence shorter than its whole rotation is the
 * rows', not the module asleep, so each holds the bus once, not on every
 * lap, however long the run (a run of 8 or more passes
 * J2534_HOLD_ASLEEP_MISSES). The other module answers about as often as
 * with those rows one frame long. */
TEST(J2534HoldTest, ARowAModuleNeverAnswersHoldsTheBusOnce)
{
    const auto answered = [](unsigned run, bool unanswered_long) {
        const auto silent = [run](uint32_t c, unsigned k) {
            return c == 0 && k >= 5 && k < 5 + run;
        };
        Bus b;
        EXPECT_TRUE(ecuio_engine_init(&b.e, 2));
        b.e.gap_us = 5 * kMs;
        for (uint32_t c = 0; c < 2; ++c) {
            for (uint8_t k = 0; k < 20; ++k) {
                const uint8_t q[3] = {0x22, static_cast<uint8_t>(0x10 + c), k};
                EXPECT_EQ(ecuio_engine_add(&b.e, c, q, 3),
                          ecuio_engine_add_result::added);
                b.e.requests[b.e.count - 1].long_defs =
                    silent(c, k) && unanswered_long ? 1 : 0;
            }
        }
        b.reply_of = [silent](uint32_t c, const ecuio_request& q) {
            return silent(c, q.bytes[2]) ? Bytes{} : did_reply(q, 2);
        };
        b.until(10000 * kMs);
        size_t n = 0;
        for (const Exchange& x : b.exchanges) {
            n += x.c == 1 && x.answered ? 1u : 0u;
        }
        return n;
    };
    for (const unsigned run : {1u, 8u, 12u}) {
        SCOPED_TRACE(run);
        const size_t plain = answered(run, false);
        const size_t held = answered(run, true);
        ASSERT_GT(plain, 100u);
        EXPECT_GE(held, plain * 95 / 100) << held << " against " << plain;
    }
}
