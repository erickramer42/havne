// File/layer: tests/test_request_quiet.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestQuietTest (a quiet module's rule,
//          request_engine_quiet.h, with the time passed in: through the
//          turns of an adapter that asks one channel at a time, and through
//          channels asked side by side as SocketCAN's and J2534's are)

#include "request_turns_support.h"

#include "ecuio/signal_sets.h"

#include <algorithm>

using namespace request_turns_test;

namespace {

/* The times channel c's requests went out, from its kth on */
std::vector<int64_t> times_of(const Adapter& a, uint32_t c, size_t k = 0)
{
    std::vector<int64_t> out;
    for (const Sent& s : a.sent) {
        if (s.c == c) {
            out.push_back(s.at_us);
        }
    }
    out.erase(out.begin(),
              out.begin() + static_cast<ptrdiff_t>(std::min(k, out.size())));
    return out;
}

/* A channel's asks past from_us, and the worst gap from an ask past it */
struct Late {
    size_t asks = 0;
    int64_t worst = 0;
};

Late late_of(const std::vector<int64_t>& t, int64_t from_us)
{
    Late l;
    for (size_t i = 1; i < t.size(); ++i) {
        l.asks += t[i] > from_us ? 1 : 0;
        l.worst =
            t[i - 1] > from_us ? std::max(l.worst, t[i] - t[i - 1]) : l.worst;
    }
    return l;
}

size_t asked_between(const Adapter& a, uint32_t c, int64_t from, int64_t to)
{
    size_t n = 0;
    for (const Sent& s : a.sent) {
        n += (s.c == c && s.at_us > from && s.at_us < to) ? 1 : 0;
    }
    return n;
}

/* The engine ECU's 4 rows on channel 0, answered in 30 ms; a module of
 * rows on channel 1 that answers only while awake */
struct Pair {
    std::unique_ptr<Adapter> a = adapter(2);
    bool awake = false;
    explicit Pair(int rows = 4)
    {
        dids(&a->e, 0, 4);
        dids(&a->e, 1, rows, 0x80);
        a->exchange_us = 30 * kMs;
        a->ecus = [this](uint32_t c, const Bytes& q) {
            return (c == 0 || awake) ? answer(q) : Bytes{};
        };
    }
};

/* Channels asked side by side (SocketCAN's and J2534's loops): each pass
 * expires the waits and asks each channel what it has due. Channel 0's 4
 * rows are answered 10 ms after each write; the others, 4 rows each,
 * answer nothing. */
struct SideBySide {
    ecuio_request_engine e;
    std::vector<std::vector<int64_t>> asked; /* each channel's writes */
    int64_t now = 0;
    int64_t answer0 = INT64_MAX;
    explicit SideBySide(uint32_t chans) : asked(chans)
    {
        EXPECT_TRUE(ecuio_engine_init(&e, chans));
        for (uint32_t c = 0; c < chans; ++c) {
            dids(&e, c, 4, static_cast<int>(0x10 * c));
        }
    }
    void answer_0()
    {
        const ecuio_request& q = e.requests[e.chans[0].outstanding];
        const Bytes r = answer(Bytes(q.bytes, q.bytes + q.len));
        (void)ecuio_engine_on_frame(&e, 0, r.data(),
                                    static_cast<uint16_t>(r.size()), now,
                                    nullptr);
        answer0 = INT64_MAX;
    }
    void until(int64_t t_us)
    {
        while (now < t_us) {
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                (void)ecuio_engine_expire(&e, c, now);
            }
            if (now >= answer0) {
                answer_0();
            }
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                const int32_t r = ecuio_engine_due(&e, c, now);
                if (r == ECUIO_ENGINE_NONE) {
                    continue;
                }
                ecuio_engine_sent(&e, c, r, true, now);
                asked[c].push_back(now);
                answer0 = c == 0 ? now + 10 * kMs : answer0;
            }
            const int64_t next = ecuio_engine_next_event_us(&e, now);
            const int64_t step = std::min(next, answer0 - now);
            ASSERT_NE(step, INT64_MAX);
            now += std::max<int64_t>(step, 1);
        }
    }
};

/* The steps as the rule gives them, written out so a mutant of the
 * engine's table is judged against them */
constexpr int64_t kSteps[] = {500 * kMs, 1000 * kMs, 2000 * kMs, 4000 * kMs,
                              5000 * kMs};

/* Channel 1's rows from the third: one at each step, each gap its step
 * and at most a pass more (the other channel's exchange, a wait) */
void expect_steps(const std::vector<int64_t>& t)
{
    for (size_t i = 1; i < t.size(); ++i) {
        const int64_t step = kSteps[std::min<size_t>(i, 4)];
        EXPECT_GE(t[i] - t[i - 1], step) << "gap " << i;
        EXPECT_LE(t[i] - t[i - 1], step + ECUIO_ENGINE_WAIT_US) << "gap " << i;
    }
}

} // namespace

/* A module never heard beside one that answers: two waits, then one row
 * 0.5 s after the second ran out, then 1, 2, 4 and every 5 s, never a
 * pack; the other channel's rows go on meanwhile */
TEST(RequestQuietTest, ANeverHeardChannelAsksOnItsStepsBesideALiveOne)
{
    Pair p;
    p.a->until(40000 * kMs);
    const std::vector<int64_t> all = times_of(*p.a, 1);
    ASSERT_GE(all.size(), 10u);
    const int64_t quiet = all[1] + ECUIO_ENGINE_WAIT_US;
    EXPECT_GE(all[2] - quiet, kSteps[0]);
    EXPECT_LE(all[2] - quiet, kSteps[0] + ECUIO_ENGINE_WAIT_US);
    expect_steps(times_of(*p.a, 1, 2));
    /* quiet near 0.3 s; 0.5 + 1 + 2 + 4 s to the fourth row, then one
     * each 5 s */
    EXPECT_EQ(all.size(), 2u + 4u + (40000 - 300 - 7500) / 5000);
    EXPECT_GT(times_of(*p.a, 0).size(), 1000u);
    for (const Sent& s : p.a->sent) {
        EXPECT_EQ(s.bytes.size(), 3u) << "one row, never a pack";
    }
}

/* A frame from a quiet channel ends its quiet state at once: its next
 * step is answered and from then every row of it is asked each lap */
TEST(RequestQuietTest, AQuietChannelsFrameEndsItAtOnce)
{
    Pair p;
    p.a->until(20000 * kMs);
    const size_t before = times_of(*p.a, 1).size();
    p.awake = true;
    p.a->until(25100 * kMs); /* its next step is at most 5 s away */
    const std::vector<int64_t> woke = times_of(*p.a, 1, before);
    ASSERT_FALSE(woke.empty()) << "a waking module is read at its step";
    const int64_t first = woke.front();
    p.a->until(first + 2000 * kMs);
    /* a lap is 4 x 30 ms on each channel: 8 laps in 2 s, 6 at least */
    EXPECT_GE(asked_between(*p.a, 1, first, first + 2000 * kMs), 24u);
    std::set<uint8_t> rows;
    for (const Sent& s : p.a->sent) {
        if (s.c == 1 && s.at_us > first) {
            rows.insert(s.bytes[2]);
        }
    }
    EXPECT_EQ(rows.size(), 4u);
}

/* A module heard and then silent (the ignition off) goes on being asked
 * each lap for 2 s after its last frame and its whole rotation, then on
 * its steps. One whose rows answer, for 2 s, and its replies stop, is not
 * judged on its run alone. */
TEST(RequestQuietTest, AHeardChannelGoesQuietAfterTwoSecondsAndItsRotation)
{
    Pair p;
    p.awake = true;
    p.a->until(5000 * kMs);
    p.awake = false;
    int64_t last = 0;
    for (const Sent& s : p.a->sent) {
        last = s.c == 1 ? s.at_us : last;
    }
    p.a->until(30000 * kMs);
    /* each lap is 4 x 30 ms and 4 waits: 1.9 s holds three laps */
    EXPECT_GE(asked_between(*p.a, 1, last, last + 1900 * kMs), 12u);
    EXPECT_LE(asked_between(*p.a, 1, last + 3000 * kMs, last + 13000 * kMs),
              4u);
    EXPECT_GE(asked_between(*p.a, 1, last + 3000 * kMs, last + 13000 * kMs),
              2u);
}

/* Rows a module never answers among rows it does are no quiet: every
 * answer ends the run, so all eight go on each lap */
TEST(RequestQuietTest, RowsAModuleNeverAnswersAmongItsAnswersAreNoQuiet)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 8, 0x80);
    a->exchange_us = 30 * kMs;
    a->ecus = [](uint32_t c, const Bytes& q) {
        return (c == 0 || q[2] % 2 == 0) ? answer(q) : Bytes{};
    };
    a->until(30000 * kMs);
    /* a lap: 4 x 30 ms, 4 x 30 ms and 4 waits, 760 ms */
    EXPECT_GE(times_of(*a, 1).size(), 8u * 30u);
}

/* A run of rows a module never answers that lasts past 2 s is no quiet
 * either, while it is shorter than the rotation: 40 rows, the first 10
 * answered, 30 waits in a row each lap (3 s) */
TEST(RequestQuietTest, ALongRunOfUnansweredRowsShorterThanTheRotationIsNoQuiet)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 40, 0x80);
    a->exchange_us = 30 * kMs;
    a->ecus = [](uint32_t c, const Bytes& q) {
        return (c == 0 || q[2] < 0x80 + 10) ? answer(q) : Bytes{};
    };
    a->until(30000 * kMs);
    /* a lap: 10 x 30 ms and 30 waits, channel 0's 4 x 30 ms at each turn's
     * second: about 3.7 s */
    EXPECT_GE(times_of(*a, 1).size(), 40u * 7u);
}

/* With no other channel live, a quiet channel is asked as before: a
 * lone module, and two that both stop answering (the ignition off), go
 * on asking each lap and time out, so the link is judged as it is */
TEST(RequestQuietTest, WithNoLiveChannelEveryChannelAsksEachLap)
{
    auto lone = adapter(1);
    dids(&lone->e, 0, 4);
    lone->ecus = [](uint32_t, const Bytes&) { return Bytes{}; };
    lone->until(10000 * kMs);
    EXPECT_GE(times_of(*lone, 0).size(), 90u); /* one wait each */

    Pair p;
    p.a->ecus = [](uint32_t, const Bytes&) { return Bytes{}; };
    p.a->until(10000 * kMs);
    EXPECT_GE(times_of(*p.a, 0).size(), 40u); /* 4 waits a turn each */
    EXPECT_GE(times_of(*p.a, 1).size(), 40u);
}

/* Fourteen absent modules beside one that answers, asked side by side so
 * they go quiet together: no two of their rows go within 250 ms, so
 * their waits are spread and never one stall */
TEST(RequestQuietTest, QuietChannelsAskAQuarterSecondApart)
{
    SideBySide b(15);
    ASSERT_NO_FATAL_FAILURE(b.until(40000 * kMs));
    std::vector<int64_t> quiet;
    for (uint32_t c = 1; c < 15; ++c) {
        std::vector<int64_t> t = b.asked[c];
        ASSERT_GE(t.size(), 6u) << "channel " << c;
        quiet.insert(quiet.end(), t.begin() + 2, t.end());
    }
    std::sort(quiet.begin(), quiet.end());
    for (size_t i = 1; i < quiet.size(); ++i) {
        EXPECT_GE(quiet[i] - quiet[i - 1], 250 * kMs) << "ask " << i;
    }
}

/* A module silent from the connect, identification and diagnostics on:
 * its lists settle unanswered after its first two waits, unasked, so its
 * reports complete; it costs no more than its steps. When it answers,
 * the items settled so are asked again, every one. */
TEST(RequestQuietTest, AQuietModulesListsSettleUnaskedAndAreAskedAtItsAnswer)
{
    Pair p;
    ecuio_engine_identify(&p.a->e);
    ecuio_engine_diagnose(&p.a->e, nullptr);
    p.a->until(60000 * kMs);
    ecuio_channel_identity_t id{};
    ecuio_engine_identity(&p.a->e, 1, &id);
    EXPECT_NE(id.flags & ECUIO_IDENTITY_COMPLETE, 0u);
    for (uint32_t i = 0; i < id.items; ++i) {
        EXPECT_EQ(id.list[i].outcome, ECUIO_IDENTITY_UNANSWERED) << i;
    }
    ecuio_channel_diagnostics_t d{};
    ecuio_engine_diagnostics(&p.a->e, 1, &d);
    EXPECT_NE(d.flags & ECUIO_DIAG_COMPLETE, 0u);
    /* two waits, then one row a step: 0.5, 1, 2, 4 s, then each 5 s */
    const size_t asked = times_of(*p.a, 1).size();
    EXPECT_EQ(asked, 2u + 4u + (60000 - 400 - 7500) / 5000);

    p.awake = true;
    p.a->until(80000 * kMs);
    ecuio_engine_identity(&p.a->e, 1, &id);
    EXPECT_NE(id.flags & ECUIO_IDENTITY_COMPLETE, 0u);
    for (uint32_t i = 0; i < id.items; ++i) {
        EXPECT_NE(id.list[i].outcome, ECUIO_IDENTITY_UNANSWERED) << i;
    }
    ecuio_engine_diagnostics(&p.a->e, 1, &d);
    EXPECT_NE(d.flags & ECUIO_DIAG_COMPLETE, 0u);
    for (uint32_t i = 0; i < p.a->e.chans[1].diag_count; ++i) {
        EXPECT_NE(p.a->e.chans[1].diag_items[i].outcome, ECUIO_DIAG_UNANSWERED)
            << i;
    }
}

/* Held, a channel's next event is its step, so a driver waiting on it
 * neither spins nor counts a quiet read; an ELM327 turn passes it by */
TEST(RequestQuietTest, AHeldChannelsNextEventIsItsStep)
{
    Pair p;
    p.a->until(3000 * kMs);
    const ecuio_engine_channel& ch = p.a->e.chans[1];
    ASSERT_TRUE(ecuio_engine_quiet_held(&p.a->e, ch));
    const int64_t now = p.a->now;
    const int64_t due = ecuio_engine_quiet_due_us(&p.a->e, ch);
    ASSERT_GT(due, now);
    EXPECT_EQ(ecuio_engine_next_event_us(&p.a->e, now, 1u), due - now);
    EXPECT_EQ(ecuio_engine_turn_next_us(&p.a->e, ch), due);
    EXPECT_EQ(ecuio_engine_due(&p.a->e, 1, now), ECUIO_ENGINE_NONE);
}

/* A recovery's rescan ends the quiet state on every channel: the module
 * is asked each lap again until its own two waits */
TEST(RequestQuietTest, ARescanEndsTheQuietState)
{
    Pair p;
    p.a->until(10000 * kMs);
    ASSERT_TRUE(p.a->e.chans[1].quiet);
    ecuio_engine_rescan(&p.a->e);
    EXPECT_FALSE(p.a->e.chans[1].quiet);
    EXPECT_FALSE(p.a->e.chans[1].heard);
    const int64_t from = p.a->now;
    p.a->until(from + 400 * kMs);
    EXPECT_EQ(asked_between(*p.a, 1, from - 1, from + 400 * kMs), 2u);
}

/* The absent module asked side by side asks on the same steps, so a
 * waking one is read up to 5 s later */
TEST(RequestQuietTest, SideBySideAQuietChannelAsksOnItsSteps)
{
    SideBySide b(2);
    ASSERT_NO_FATAL_FAILURE(b.until(40000 * kMs));
    std::vector<int64_t> asked1 = b.asked[1];
    ASSERT_GE(asked1.size(), 3u);
    asked1.erase(asked1.begin(), asked1.begin() + 2);
    expect_steps(asked1);
    EXPECT_EQ(asked1.size(), 4u + (40000 - 200 - 7500) / 5000);
}

namespace {

/* Channel c's identification items whose outcome is o */
size_t items_of(const ecuio_request_engine& e, uint32_t c, uint8_t o)
{
    ecuio_channel_identity_t id{};
    ecuio_engine_identity(&e, c, &id);
    size_t n = 0;
    for (uint32_t i = 0; i < id.items; ++i) {
        n += id.list[i].outcome == o ? 1 : 0;
    }
    return n;
}

/* The engine ECU's 4 rows on channel 0, answered in 30 ms; on channel 1
 * a module with no rows (nothing picked of it), identification on, that
 * answers as ecu1 says */
std::unique_ptr<Adapter> rowless(const Ecus& ecu1)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    a->exchange_us = 30 * kMs;
    a->ecus = [ecu1](uint32_t c, const Bytes& q) {
        return c == 0 ? answer(q) : ecu1(c, q);
    };
    ecuio_engine_identify(&a->e);
    return a;
}

/* Side by side, passes until channel 1 has a request out */
void to_request_out(SideBySide& b)
{
    for (int i = 0; i < 100000 && b.e.chans[1].outstanding == ECUIO_ENGINE_NONE;
         ++i) {
        b.until(b.now + 1);
    }
}

} // namespace

/* A module with no rows whose first two items go unanswered (a slow wake,
 * two replies lost) goes quiet; held, it asks one of its settled items at
 * each step in a row's place, and at its answer every item is asked again */
TEST(RequestQuietTest, AModuleWithNoRowsAsksAnItemAtEachStep)
{
    int lost = 0;
    auto a = rowless([&lost](uint32_t, const Bytes& q) {
        if (lost < 2) {
            ++lost;
            return Bytes{};
        }
        return answer(q);
    });
    a->until(60000 * kMs);
    EXPECT_FALSE(a->e.chans[1].quiet);
    EXPECT_EQ(items_of(a->e, 1, ECUIO_IDENTITY_UNANSWERED), 0u);
    EXPECT_EQ(items_of(a->e, 1, ECUIO_IDENTITY_UNASKED), 0u);
}

/* A module with no rows that never answers Service 09 but answers its
 * DIDs: its next step asks another service than the last step's, so its
 * second step's 22 item finds it, and only Service 09's four stay
 * unanswered */
TEST(RequestQuietTest, AModuleWithNoRowsAsksAnotherServiceAtItsNextStep)
{
    auto a = rowless([](uint32_t, const Bytes& q) {
        return q[0] == 0x09 ? Bytes{} : answer(q);
    });
    a->until(10000 * kMs);
    EXPECT_FALSE(a->e.chans[1].quiet);
    EXPECT_EQ(items_of(a->e, 1, ECUIO_IDENTITY_UNANSWERED), 4u);
    EXPECT_EQ(items_of(a->e, 1, ECUIO_IDENTITY_ANSWERED),
              size_t{ECUIO_IDENT_COUNT} - 4u);
}

/* An absent module with no rows costs its first four steps alone: its
 * report completes unanswered at its second wait, then one item at 0.5,
 * 1, 2 and 4 s, and nothing more (never heard, it is absent, and nothing
 * on screen waits on it) */
TEST(RequestQuietTest, AnAbsentModuleWithNoRowsCostsItsFirstFourSteps)
{
    auto a = rowless([](uint32_t, const Bytes&) { return Bytes{}; });
    a->until(60000 * kMs);
    EXPECT_EQ(items_of(a->e, 1, ECUIO_IDENTITY_UNANSWERED),
              size_t{ECUIO_IDENT_COUNT});
    const std::vector<int64_t> all = times_of(*a, 1);
    ASSERT_EQ(all.size(), 2u + 4u);
    const int64_t quiet = all[1] + ECUIO_ENGINE_WAIT_US;
    /* the first step can wait out a turn of the engine ECU's under way:
     * its 4 rows taking turns with its own 4 items */
    EXPECT_GE(all[2] - quiet, kSteps[0]);
    EXPECT_LE(all[2] - quiet, kSteps[0] + 8 * 30 * kMs);
    expect_steps(times_of(*a, 1, 2));
}

/* A module with no rows heard before (its identification answered, its
 * scan's DIDs not) goes quiet and asks a settled entry at every step, the
 * 5 s ones too, since it can wake; at its answer the scan is asked again
 * and every DID found */
TEST(RequestQuietTest, AHeardModuleWithNoRowsAsksAtEveryStep)
{
    bool awake = false;
    auto a = rowless([&awake](uint32_t, const Bytes& q) {
        return (q[0] == 0x22 && q[1] == 0x40 && !awake) ? Bytes{} : answer(q);
    });
    for (uint16_t k = 0; k < 60; ++k) {
        ASSERT_TRUE(ecuio_engine_scan_add(&a->e, 1, 0x4000 + k));
    }
    a->until(30000 * kMs);
    ASSERT_TRUE(a->e.chans[1].quiet);
    EXPECT_GE(asked_between(*a, 1, 20000 * kMs, 30000 * kMs), 2u)
        << "its 5 s steps";
    awake = true;
    a->until(36000 * kMs);
    EXPECT_FALSE(a->e.chans[1].quiet);
    size_t found = 0;
    for (uint32_t i = 0; i < a->e.scan_count; ++i) {
        found += a->e.scan[i].outcome == ECUIO_SCAN_FOUND ? 1 : 0;
    }
    EXPECT_EQ(found, 60u);
}

/* An item a quiet module's step asks and that goes unanswered again moves
 * no report's generation, so havne-cli prints no identity or diagnostics
 * line again: an absent module's first four steps (identification and
 * diagnostics), then a heard module's steps through its scan */
TEST(RequestQuietTest, AnItemUnansweredAgainMovesNoReport)
{
    auto a = rowless([](uint32_t, const Bytes&) { return Bytes{}; });
    ecuio_engine_diagnose(&a->e, ecuio_obd2_signal_set());
    for (int i = 0; i < 100000 && times_of(*a, 1).size() < 3; ++i) {
        a->pass();
    }
    ASSERT_EQ(times_of(*a, 1).size(), 3u) << "the first step's item out";
    const uint32_t ident = a->e.chans[1].ident_generation;
    const uint32_t diag = a->e.chans[1].diag.generation;
    EXPECT_GT(ident, 0u) << "a first unanswered settle moves it";
    EXPECT_GT(diag, 0u) << "a first unanswered settle moves it";
    a->until(60000 * kMs);
    EXPECT_EQ(times_of(*a, 1).size(), 2u + 4u);
    EXPECT_EQ(a->e.chans[1].ident_generation, ident);
    EXPECT_EQ(a->e.chans[1].diag.generation, diag);

    auto b = rowless([](uint32_t, const Bytes& q) {
        return (q[0] == 0x22 && q[1] == 0x40) ? Bytes{} : answer(q);
    });
    for (uint16_t k = 0; k < 60; ++k) {
        ASSERT_TRUE(ecuio_engine_scan_add(&b->e, 1, 0x4000 + k));
    }
    for (int i = 0; i < 100000 && !b->e.chans[1].quiet; ++i) {
        b->pass();
    }
    size_t scanned = 0;
    for (const Sent& s : b->sent) {
        scanned +=
            (s.c == 1 && s.bytes[0] == 0x22 && s.bytes[1] == 0x40) ? 1 : 0;
    }
    ASSERT_GT(scanned, 0u);
    EXPECT_GE(b->e.chans[1].generation, scanned)
        << "each first unanswered settle moves it";
    b->until(15000 * kMs);
    ASSERT_TRUE(b->e.chans[1].quiet);
    const uint32_t scan = b->e.chans[1].generation;
    b->until(40000 * kMs);
    EXPECT_GE(asked_between(*b, 1, 15000 * kMs, 40000 * kMs), 4u);
    EXPECT_EQ(b->e.chans[1].generation, scan);
}

/* Past twenty quiet channels side by side (a whole-car profile in
 * accessory) the 250 ms slots fill and the oldest step goes first, so
 * each takes its turn: 29 channels go around in 7.25 s and none waits
 * longer. In channel order alone the last nine were never asked again. */
TEST(RequestQuietTest, ManyQuietChannelsTakeTheirStepsInTurn)
{
    SideBySide b(30);
    ASSERT_NO_FATAL_FAILURE(b.until(120000 * kMs));
    for (uint32_t c = 1; c < 30; ++c) {
        const Late l = late_of(b.asked[c], 30000 * kMs);
        EXPECT_GE(l.asks, 12u) << "channel " << c;
        EXPECT_LE(l.worst, 29 * 250 * kMs + 10 * kMs) << "channel " << c;
    }
}

/* Any frame from a quiet module ends its quiet state, not only a positive
 * answer: a refusal of its step's row, a 7F xx 78, or a positive reply
 * handed over after the row's wait ran out */
TEST(RequestQuietTest, ARefusalAPendingReplyOrALateAnswerEndsTheQuietState)
{
    for (int shape = 0; shape < 3; ++shape) {
        SideBySide b(2);
        ASSERT_NO_FATAL_FAILURE(b.until(3000 * kMs));
        ASSERT_TRUE(b.e.chans[1].quiet);
        to_request_out(b);
        ASSERT_NE(b.e.chans[1].outstanding, ECUIO_ENGINE_NONE);
        const ecuio_request q = b.e.requests[b.e.chans[1].outstanding];
        Bytes r = {0x7F, 0x22, static_cast<uint8_t>(shape == 0 ? 0x31 : 0x78)};
        if (shape == 2) {
            ASSERT_NO_FATAL_FAILURE(b.until(b.e.chans[1].deadline_us + 1));
            ASSERT_EQ(b.e.chans[1].outstanding, ECUIO_ENGINE_NONE);
            r = {0x62, q.bytes[1], q.bytes[2], 0x12, 0x34};
        }
        (void)ecuio_engine_on_frame(
            &b.e, 1, r.data(), static_cast<uint16_t>(r.size()), b.now, nullptr);
        EXPECT_FALSE(b.e.chans[1].quiet) << "shape " << shape;
    }
}

/* A pack's wait counts each of its rows toward a heard module's run: 29
 * Service 01 PIDs in packs of six run out one rotation in a lap of five
 * packs, so the module is quiet at 2 s with no frame, not after 29 packs'
 * waits (about 3.6 s) */
TEST(RequestQuietTest, APacksWaitCountsEachOfItsRows)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    for (uint8_t pid = 0x03; pid < 0x20; ++pid) {
        add(&a->e, 1, {0x01, pid});
    }
    a->e.pack = true;
    ecuio_engine_probe(&a->e);
    a->exchange_us = 30 * kMs;
    bool awake = true;
    a->ecus = [&awake](uint32_t c, const Bytes& q) {
        return (c == 0 || awake) ? answer(q) : Bytes{};
    };
    a->until(5000 * kMs);
    awake = false;
    int64_t last = 0;
    size_t packs = 0;
    for (const Sent& s : a->sent) {
        last = s.c == 1 ? s.at_us : last;
        packs += (s.c == 1 && s.bytes.size() > 2) ? 1 : 0;
    }
    ASSERT_GT(packs, 10u);
    a->until(last + 6000 * kMs);
    /* quiet at 2 s, its steps 0.5 and 1 s later */
    EXPECT_LE(asked_between(*a, 1, last + 2300 * kMs, last + 4000 * kMs), 2u);
}

/* Held with no rows, a channel's next event and its turn's are its step
 * too, so a driver waits for it and an ELM327 turn reaches it */
TEST(RequestQuietTest, AHeldChannelWithNoRowsNextEventIsItsStep)
{
    auto a = rowless([](uint32_t, const Bytes&) { return Bytes{}; });
    const ecuio_engine_channel& ch = a->e.chans[1];
    for (int i = 0;
         i < 100000 && !(ch.quiet && ch.outstanding == ECUIO_ENGINE_NONE &&
                         ecuio_engine_quiet_due_us(&a->e, ch) > a->now);
         ++i) {
        a->pass();
    }
    ASSERT_TRUE(ecuio_engine_quiet_held(&a->e, ch));
    const int64_t now = a->now;
    const int64_t due = ecuio_engine_quiet_due_us(&a->e, ch);
    ASSERT_GT(due, now);
    EXPECT_EQ(ecuio_engine_next_event_us(&a->e, now, 1u), due - now);
    EXPECT_EQ(ecuio_engine_turn_next_us(&a->e, ch), due);
    EXPECT_EQ(ecuio_engine_due(&a->e, 1, now), ECUIO_ENGINE_NONE);
}

/* An absent module with no rows past its four steps asks no more and
 * holds back no other quiet module, though its last step is the oldest:
 * an absent one with rows before it goes on asking at its 5 s steps */
TEST(RequestQuietTest, ARowlessModulePastItsStepsHoldsNoOneBack)
{
    auto a = adapter(3);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 4, 0x80);
    a->exchange_us = 30 * kMs;
    a->ecus = [](uint32_t c, const Bytes& q) {
        return c == 0 ? answer(q) : Bytes{};
    };
    ecuio_engine_identify(&a->e);
    a->until(60000 * kMs);
    EXPECT_EQ(times_of(*a, 2).size(), 2u + 4u);
    EXPECT_GE(asked_between(*a, 1, 30000 * kMs, 60000 * kMs), 5u);
}

/* Through an ELM327's turns the held channels come together at a lap's
 * end, so the spacing leaves one ask a lap among them: 14 quiet modules
 * beside 4 that answer (a lap of about 0.58 s) are asked in turn, the
 * oldest step first, each about every 14 laps. In channel order alone the
 * last five were never asked again. */
TEST(RequestQuietTest, AnElm327sTurnsAskTheQuietModulesInTurn)
{
    auto a = adapter(18);
    for (uint32_t c = 0; c < 18; ++c) {
        dids(&a->e, c, 4);
    }
    a->exchange_us = 30 * kMs;
    a->ecus = [](uint32_t c, const Bytes& q) {
        return c < 4 ? answer(q) : Bytes{};
    };
    a->until(70000 * kMs);
    for (uint32_t c = 4; c < 18; ++c) {
        const Late l = late_of(times_of(*a, c), 15000 * kMs);
        EXPECT_GE(l.asks, 5u) << "channel " << c;
        EXPECT_LE(l.worst, 9500 * kMs) << "channel " << c;
    }
}

/* The oldest step goes first whatever channel a lap starts at: an ELM327's
 * turn starts past the last one's, so with the modules that answer at 4-7
 * every lap reaches 8-17 before 0-3. Waiting only for a later channel left
 * 0-3 unasked from 15 s. */
TEST(RequestQuietTest, QuietModulesBelowTheLiveOnesAreAskedInTurn)
{
    auto a = adapter(18);
    for (uint32_t c = 0; c < 18; ++c) {
        dids(&a->e, c, 4);
    }
    a->exchange_us = 30 * kMs;
    a->ecus = [](uint32_t c, const Bytes& q) {
        return (c >= 4 && c < 8) ? answer(q) : Bytes{};
    };
    a->until(70000 * kMs);
    for (uint32_t c = 0; c < 18; ++c) {
        if (c >= 4 && c < 8) {
            continue;
        }
        const Late l = late_of(times_of(*a, c), 15000 * kMs);
        EXPECT_GE(l.asks, 5u) << "channel " << c;
        EXPECT_LE(l.worst, 9500 * kMs) << "channel " << c;
    }
}

namespace {

/* SocketCAN's pass: one request at a time, the first due from the channel
 * after the last asked. Channel 1's 4 rows are answered 10 ms after each
 * write; the others, 4 rows each, answer nothing. */
struct Rotating {
    ecuio_request_engine e;
    std::vector<std::vector<int64_t>> asked; /* each channel's writes */
    int64_t now = 0;
    int64_t answer1 = INT64_MAX;
    uint32_t next_chan = 0;
    explicit Rotating(uint32_t chans) : asked(chans)
    {
        EXPECT_TRUE(ecuio_engine_init(&e, chans));
        for (uint32_t c = 0; c < chans; ++c) {
            dids(&e, c, 4, static_cast<int>(0x10 * c));
        }
    }
    void answer_1()
    {
        const ecuio_request& q = e.requests[e.chans[1].outstanding];
        const Bytes r = answer(Bytes(q.bytes, q.bytes + q.len));
        (void)ecuio_engine_on_frame(
            &e, 1, r.data(), static_cast<uint16_t>(r.size()), now, nullptr);
        answer1 = INT64_MAX;
    }
    /* The first request due from next_chan on, sent; false with none */
    bool send_due()
    {
        for (uint32_t k = 0; k < e.chan_count; ++k) {
            const uint32_t c = (next_chan + k) % e.chan_count;
            const int32_t r = ecuio_engine_due(&e, c, now);
            if (r != ECUIO_ENGINE_NONE) {
                ecuio_engine_sent(&e, c, r, true, now);
                asked[c].push_back(now);
                answer1 = c == 1 ? now + 10 * kMs : answer1;
                next_chan = (c + 1) % e.chan_count;
                return true;
            }
        }
        return false;
    }
    void until(int64_t t_us)
    {
        while (now < t_us) {
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                (void)ecuio_engine_expire(&e, c, now);
            }
            if (now >= answer1) {
                answer_1();
            }
            if (send_due()) {
                now += kMs; /* the echo */
                continue;
            }
            const int64_t next = ecuio_engine_next_event_us(&e, now);
            const int64_t step = std::min(next, answer1 - now);
            ASSERT_NE(step, INT64_MAX);
            now += std::max<int64_t>(step, 1);
        }
    }
};

} // namespace

/* SocketCAN's pass starts past the last channel asked, nearly always the
 * one that answers, so channel 0 below it is reached last: the oldest step
 * going first, it is asked in turn with the 30 above it. Waiting only for
 * a later channel left it unasked from 30 s. */
TEST(RequestQuietTest, SocketCansPassAsksAQuietModuleBelowTheLiveOne)
{
    Rotating b(32);
    ASSERT_NO_FATAL_FAILURE(b.until(120000 * kMs));
    for (uint32_t c = 0; c < 32; ++c) {
        if (c == 1) {
            continue;
        }
        const Late l = late_of(b.asked[c], 30000 * kMs);
        EXPECT_GE(l.asks, 10u) << "channel " << c;
        EXPECT_LE(l.worst, 31 * 250 * kMs + 10 * kMs) << "channel " << c;
    }
}
