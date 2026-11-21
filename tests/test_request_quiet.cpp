// File/layer: tests/test_request_quiet.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestQuietTest (a quiet module's rule,
//          request_engine_quiet.h, with the time passed in: through the
//          turns of an adapter that asks one channel at a time, and through
//          channels asked side by side as SocketCAN's and J2534's are)

#include "request_turns_support.h"

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
