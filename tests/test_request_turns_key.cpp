// File/layer: tests/test_request_turns_key.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestTurnsTest (request_turns.h and a key's window:
//          whose turn an offered key takes, and what a turn taken from
//          a walk leaves of its budget, with the time passed in)

#include "request_turns_support.h"

using namespace request_turns_test;

namespace {

/* What key_turn's ECUs answer: a level 01 seed, its key by the echo
 * bytes the one-shot holds, and every DID */
const Bytes kSeed = {0x27, 0x01};
const Bytes kKeyEcho = {0x27, 0x02};
const Bytes kKey = {0x27, 0x02, 0x11, 0x22, 0x33, 0x44};

Bytes key_ecu(uint32_t, const Bytes& q)
{
    if (q == kSeed) {
        return {0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08};
    }
    return q == kKeyEcho ? Bytes{0x67, 0x02} : answer(q);
}

/* Channel c's seed offered with a window of 300 ms, sent and answered */
void seed_on(Adapter* a, uint32_t c)
{
    ecuio_talk_request q;
    q.pdu = kSeed.data();
    q.len = 2;
    q.echo = 2;
    q.kind = ecuio_talk_kind::seed;
    q.window_us = 300 * kMs;
    ASSERT_EQ(ecuio_engine_offer_talk(&a->e, c, q, a->now),
              ecuio_talk_offer::taken);
    ecuio_talk_result settled;
    for (int i = 0;
         i < 200 && !ecuio_engine_talk_take(a->e.chans[c].talk, &settled);
         ++i) {
        a->pass();
    }
    ASSERT_EQ(settled.end, ecuio_talk_end::answered);
}

/* Three channels, the first with a turn of forty rows; channel 2's seed
 * answered, and channel 0's turn begun since */
std::unique_ptr<Adapter> key_turn()
{
    auto a = adapter(3);
    dids(&a->e, 0, 40);
    dids(&a->e, 1, 4, 0x40);
    dids(&a->e, 2, 1, 0x80);
    a->ecus = key_ecu;
    a->until(1000 * kMs);
    seed_on(a.get(), 2);
    a->pass();
    EXPECT_TRUE(a->turns.on);
    EXPECT_EQ(a->turns.chan, 0u) << "forty rows: a second of turn ahead";
    EXPECT_TRUE(ecuio_key_window_open(a->e.chans[2].key, a->now));
    return a;
}

ecuio_talk_offer offer_key(Adapter* a, uint32_t c = 2)
{
    ecuio_talk_request k;
    k.pdu = kKey.data();
    k.len = 6;
    k.echo = 2;
    k.kind = ecuio_talk_kind::key;
    return ecuio_engine_offer_talk(&a->e, c, k, a->now);
}

} // namespace

/* A key offered on one channel is the adapter's next request, whoever
 * holds the turn and whoever is next in order (rule 5): its window of
 * 300 ms is over long before a turn of forty rows is, and a key that
 * waits a turn out is never sent */
TEST(RequestTurnsTest, AnOfferedKeyIsTheNextRequestWhoeverHoldsTheTurn)
{
    auto a = key_turn();
    ASSERT_FALSE(::testing::Test::HasFailure());
    ASSERT_EQ(offer_key(a.get()), ecuio_talk_offer::taken);
    const size_t from = a->sent.size();
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 1);
    EXPECT_EQ(a->sent[from].c, 2u)
        << "not channel 0's next row, nor channel 1, the next in order";
    EXPECT_EQ(a->sent[from].bytes, kKeyEcho);
    ecuio_talk_result settled;
    ASSERT_TRUE(ecuio_engine_talk_take(a->e.chans[2].talk, &settled));
    EXPECT_EQ(settled.end, ecuio_talk_end::answered);
    /* and the turns go on from there, each channel asked */
    a->asks(from + 60);
    const std::vector<uint32_t> after = a->channels(from + 1);
    const std::set<uint32_t> asked(after.begin(), after.end());
    EXPECT_EQ(asked.size(), 3u);
}

/* Rule 5 passes over a channel the driver holds: its key ends no turn,
 * and goes once the hold is over */
TEST(RequestTurnsTest, AHeldChannelsKeyEndsNoTurn)
{
    auto a = key_turn();
    ASSERT_FALSE(::testing::Test::HasFailure());
    ASSERT_EQ(offer_key(a.get()), ecuio_talk_offer::taken);
    a->held = 1u << 2;
    const size_t from = a->sent.size();
    const int64_t since = a->turns.since_us;
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 1);
    EXPECT_EQ(a->sent[from].c, 0u) << "channel 0's turn goes on";
    EXPECT_TRUE(a->turns.on);
    EXPECT_EQ(a->turns.chan, 0u);
    EXPECT_EQ(a->turns.since_us, since) << "the same turn, not a new one";
    a->held = 0;
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 2);
    EXPECT_EQ(a->sent[from + 1].c, 2u);
    EXPECT_EQ(a->sent[from + 1].bytes, kKeyEcho);
}

/* A key past its window can't go any more, so it ends no turn and is
 * settled lapsed */
TEST(RequestTurnsTest, AKeyPastItsWindowEndsNoTurn)
{
    auto a = key_turn();
    ASSERT_FALSE(::testing::Test::HasFailure());
    ASSERT_EQ(offer_key(a.get()), ecuio_talk_offer::taken);
    a->held = 1u << 2;
    const int64_t until = a->e.chans[2].key.until_us;
    const int64_t since = a->turns.since_us;
    while (a->now < until) {
        a->pass();
    }
    a->held = 0;
    ASSERT_TRUE(a->turns.on);
    ASSERT_EQ(a->turns.chan, 0u);
    ASSERT_EQ(a->turns.since_us, since) << "channel 0's turn, under a second";
    const size_t from = a->sent.size();
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 1);
    EXPECT_EQ(a->sent[from].c, 0u);
    EXPECT_EQ(a->turns.since_us, since) << "the same turn";
    ecuio_talk_result settled;
    for (int i = 0;
         i < 400 && !ecuio_engine_talk_take(a->e.chans[2].talk, &settled);
         ++i) {
        a->pass();
    }
    EXPECT_EQ(settled.end, ecuio_talk_end::lapsed);
    for (size_t i = from; i < a->sent.size(); ++i) {
        EXPECT_NE(a->sent[i].bytes, kKeyEcho) << "request " << i;
    }
}

/* A key past its window is settled at the next pass, whoever holds the
 * turn and though the driver holds its channel: its own channel may not
 * ask for a second, and the caller waits on the end */
TEST(RequestTurnsTest, AKeyPastItsWindowIsSettledAtTheNextPass)
{
    auto a = key_turn();
    ASSERT_FALSE(::testing::Test::HasFailure());
    ASSERT_EQ(offer_key(a.get()), ecuio_talk_offer::taken);
    a->held = 1u << 2;
    const int64_t until = a->e.chans[2].key.until_us;
    const int64_t since = a->turns.since_us;
    while (a->now < until) {
        a->pass();
    }
    ecuio_talk_result settled;
    EXPECT_FALSE(ecuio_engine_talk_take(a->e.chans[2].talk, &settled))
        << "inside its window it waits";
    a->pass();
    ASSERT_TRUE(ecuio_engine_talk_take(a->e.chans[2].talk, &settled))
        << "at the first pass past the window, not at channel 2's turn";
    EXPECT_EQ(settled.end, ecuio_talk_end::lapsed);
    EXPECT_EQ(a->e.chans[2].key.until_us, 0);
    EXPECT_TRUE(a->turns.on);
    EXPECT_EQ(a->turns.chan, 0u);
    EXPECT_EQ(a->turns.since_us, since) << "channel 0's turn goes on";
}

namespace {

/* The bitmaps channel 0 was asked, from the from-th request on */
std::vector<uint8_t> bitmaps(const Adapter& a, size_t from)
{
    std::vector<uint8_t> out;
    for (size_t i = from; i < a.sent.size(); ++i) {
        const Sent& x = a.sent[i];
        if (x.c == 0 && x.bytes.size() == 2 && x.bytes[0] == 0x01 &&
            x.bytes[1] % 0x20 == 0) {
            out.push_back(x.bytes[1]);
        }
    }
    return out;
}

const std::vector<uint8_t> kWholeWalk = {0x00, 0x20, 0x40, 0x60,
                                         0x80, 0xA0, 0xC0, 0xE0};

} // namespace

/* A walk's second holds its channel's own exchanges only. A key on
 * another channel takes the turn two bitmaps in (rule 5), and that
 * channel then asks its forty rows for a second: the walk goes on from
 * its third bitmap with the rest of its second, and walks the table. */
TEST(RequestTurnsTest, AWalkInterruptedByAKeyStillWalksItsTable)
{
    auto a = adapter(2);
    add(&a->e, 0, {0x01, 0x0C});
    dids(&a->e, 1, 40);
    a->ecus = key_ecu;
    a->until(500 * kMs);
    seed_on(a.get(), 1);
    ASSERT_FALSE(::testing::Test::HasFailure());
    ecuio_engine_probe(&a->e);
    ASSERT_TRUE(a->e.chans[0].walking);
    const size_t from = a->sent.size();
    a->pass();
    a->pass();
    ASSERT_EQ(bitmaps(*a, from), (std::vector<uint8_t>{0x00, 0x20}));
    ASSERT_EQ(offer_key(a.get(), 1), ecuio_talk_offer::taken);
    a->pass();
    ASSERT_EQ(a->sent.back().c, 1u);
    ASSERT_EQ(a->sent.back().bytes, kKeyEcho);
    const size_t keyed = a->sent.size();
    a->until(a->now + 3000 * kMs);
    size_t rows = 0;
    while (keyed + rows < a->sent.size() && a->sent[keyed + rows].c == 1) {
        ++rows;
    }
    EXPECT_GE(static_cast<int64_t>(rows) * a->exchange_us,
              ECUIO_ENGINE_PROBE_BUDGET_US - 2 * a->exchange_us)
        << "channel 1 kept the turn past the rest of the walk's second";
    EXPECT_EQ(bitmaps(*a, from), kWholeWalk);
    EXPECT_FALSE(a->e.chans[0].walking);
}

/* The same beside a channel in its session: channel 1's keep-alive falls
 * due while channel 0 walks, and its first request after is a row,
 * standing in for the keep-alive; the walk walks its table */
TEST(RequestTurnsTest, AWalkBesideASessionStillWalksItsTable)
{
    auto a = adapter(2);
    add(&a->e, 0, {0x01, 0x0C});
    dids(&a->e, 1, 40);
    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MIN_US));
    a->ecus = [](uint32_t, const Bytes& q) { return in_session(q); };
    a->until(2000 * kMs);
    /* channel 0 takes the turn from channel 1, whose last request is then
     * an exchange old: its keep-alive comes due inside the walk */
    for (int i = 0; i < 400; ++i) {
        const size_t n = a->sent.size();
        a->pass();
        if (a->sent.size() > n && n > 0 && a->sent[n].c == 0 &&
            a->sent[n - 1].c == 1) {
            break;
        }
    }
    ASSERT_EQ(a->sent.back().c, 0u);
    ecuio_engine_probe(&a->e);
    ASSERT_TRUE(a->e.chans[0].walking);
    const size_t from = a->sent.size();
    a->until(a->now + 4000 * kMs);
    size_t first_other = 0;
    for (size_t i = from; i < a->sent.size(); ++i) {
        const Sent& x = a->sent[i];
        if (x.c == 1 && first_other == 0) {
            first_other = i;
            EXPECT_EQ(x.bytes[0], 0x22) << "a row stands in for 3E 00";
        }
    }
    ASSERT_NE(first_other, 0u);
    EXPECT_EQ(bitmaps(*a, from), kWholeWalk);
    EXPECT_EQ(a->e.chans[1].session_lost, 0u);
}

/* A walk that outlasts another channel's urgent time: channel 1's last
 * request is S3 less 1.1 s old as channel 0's walk starts, so its
 * session falls urgent a tenth of a second in (rule 4). Its next request
 * goes within two exchanges of that, and the walk, its second standing
 * still meanwhile, still walks its table */
TEST(RequestTurnsTest, AnUrgentSessionEndsAWalk)
{
    auto a = adapter(2);
    add(&a->e, 0, {0x01, 0x0C});
    dids(&a->e, 1, 1);
    ASSERT_TRUE(ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    a->ecus = [](uint32_t, const Bytes& q) { return in_session(q); };
    a->until(2000 * kMs);
    ASSERT_EQ(a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    a->e.chans[1].last_sent_us =
        a->now - (ECUIO_ENGINE_S3_US - ECUIO_ENGINE_S3_MARGIN_US) + 100 * kMs;
    const int64_t urgent = ecuio_engine_session_urgent_us(&a->e, a->e.chans[1]);
    ecuio_engine_probe(&a->e);
    a->turns.on = false;
    a->turns.chan = 0;
    const size_t from = a->sent.size();
    a->until(a->now + 3000 * kMs);
    const Sent* first = nullptr;
    size_t first_at = 0;
    size_t last_bitmap = 0;
    for (size_t i = from; i < a->sent.size(); ++i) {
        if (a->sent[i].c == 1 && first == nullptr) {
            first = &a->sent[i];
            first_at = i;
        }
        if (a->sent[i].c == 0 && a->sent[i].bytes == Bytes{0x01, 0xE0}) {
            last_bitmap = i;
        }
    }
    ASSERT_NE(first, nullptr);
    EXPECT_GE(first->at_us, urgent);
    EXPECT_LE(first->at_us, urgent + 2 * a->exchange_us)
        << "channel 1 waited for the walk";
    EXPECT_LT(first_at, last_bitmap) << "the walk was interrupted";
    EXPECT_EQ(bitmaps(*a, from), kWholeWalk);
    EXPECT_EQ(a->e.chans[1].session_lost, 0u);
}

/* The budget's bookkeeping by itself: a walk is away from its turn's end
 * to its channel's next request, each stretch counted once, and a channel
 * that isn't walking is never away */
TEST(RequestTurnsTest, AWalksSecondStandsStillWhileItsChannelIsAway)
{
    auto a = adapter(2);
    add(&a->e, 0, {0x01, 0x0C});
    dids(&a->e, 1, 1);
    ecuio_engine_probe(&a->e);
    ecuio_engine_turns t;
    ecuio_engine_channel& ch = a->e.chans[0];
    int64_t now = 100 * kMs;
    ASSERT_EQ(ecuio_engine_turn_due(&a->e, t, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&a->e, 0, ECUIO_ENGINE_ONCE, true, now);
    const int64_t until = ch.walk_until_us;
    ASSERT_EQ(until, now + ECUIO_ENGINE_PROBE_BUDGET_US);
    const Bytes r = answer({0x01, 0x00});
    now += 90 * kMs;
    ASSERT_EQ(ecuio_engine_on_frame(&a->e, 0, r.data(),
                                    static_cast<uint16_t>(r.size()), now,
                                    nullptr),
              ecuio_reply::positive);
    ASSERT_TRUE(ch.walking);
    EXPECT_EQ(t.walk_away, 0u);

    ecuio_engine_turn_left(&a->e, t, 1, now);
    EXPECT_EQ(t.walk_away, 0u) << "channel 1 isn't walking";
    ecuio_engine_turn_left(&a->e, t, 0, now);
    EXPECT_EQ(t.walk_away, 1u);
    /* asked twice before its floor has passed: nothing, and away still */
    ch.next_send_us = now + 400 * kMs;
    EXPECT_EQ(ecuio_engine_turn_due(&a->e, t, 0, now + 100 * kMs),
              ECUIO_ENGINE_NONE);
    EXPECT_EQ(ch.walk_until_us, until + 100 * kMs);
    EXPECT_EQ(ecuio_engine_turn_due(&a->e, t, 0, now + 300 * kMs),
              ECUIO_ENGINE_NONE);
    EXPECT_EQ(ch.walk_until_us, until + 300 * kMs) << "each stretch once";
    EXPECT_EQ(t.walk_away, 1u);
    ASSERT_EQ(ecuio_engine_turn_due(&a->e, t, 0, now + 400 * kMs),
              ECUIO_ENGINE_ONCE);
    EXPECT_EQ(ch.once.bytes[1], 0x20u);
    EXPECT_EQ(ch.walk_until_us, until + 400 * kMs);
    EXPECT_EQ(t.walk_away, 0u) << "it asks: away no more";
    ecuio_engine_sent(&a->e, 0, ECUIO_ENGINE_ONCE, true, now + 400 * kMs);
    EXPECT_EQ(ecuio_engine_turn_due(&a->e, t, 0, now + 450 * kMs),
              ECUIO_ENGINE_NONE);
    EXPECT_EQ(ch.walk_until_us, until + 400 * kMs)
        << "its own exchange is its second's";

    /* a walk the driver's recovery started again takes a new second */
    ecuio_engine_turn_left(&a->e, t, 0, now + 450 * kMs);
    ecuio_engine_probe(&a->e);
    ASSERT_FALSE(ch.walk_timed);
    const int64_t before = ch.walk_until_us;
    (void)ecuio_engine_turn_due(&a->e, t, 0, now + 900 * kMs);
    EXPECT_EQ(ch.walk_until_us, before)
        << "nothing added to a second not begun";
}

/* Rule 5 is another channel's key alone: a talk that is no key waits
 * for its channel's turn, and a key on the channel that holds the turn
 * goes in that turn */
TEST(RequestTurnsTest, OnlyAnotherChannelsKeyEndsATurn)
{
    auto a = adapter(2);
    dids(&a->e, 0, 40);
    dids(&a->e, 1, 1, 0x80);
    a->ecus = key_ecu;
    a->until(1000 * kMs);
    for (int i = 0;
         i < 200 && !(a->turns.on && a->turns.chan == 0 && a->turns.asked == 1);
         ++i) {
        a->pass();
    }
    ASSERT_EQ(a->turns.asked, 1u) << "channel 0's turn, just begun";
    const int64_t since = a->turns.since_us;
    const Bytes read = {0x22, 0x1E, 0x90};
    ecuio_talk_request q;
    q.pdu = read.data();
    q.len = 3;
    q.echo = 3;
    ASSERT_EQ(ecuio_engine_offer_talk(&a->e, 1, q, a->now),
              ecuio_talk_offer::taken);
    size_t from = a->sent.size();
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 1);
    EXPECT_EQ(a->sent[from].c, 0u) << "a plain talk ends no turn";
    EXPECT_EQ(a->turns.since_us, since);

    seed_on(a.get(), 0);
    ASSERT_FALSE(::testing::Test::HasFailure());
    ASSERT_TRUE(a->turns.on);
    ASSERT_EQ(a->turns.chan, 0u);
    ASSERT_EQ(a->turns.since_us, since) << "the seed went in the turn";
    ASSERT_EQ(offer_key(a.get(), 0), ecuio_talk_offer::taken);
    from = a->sent.size();
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 1);
    EXPECT_EQ(a->sent[from].c, 0u);
    EXPECT_EQ(a->sent[from].bytes, kKeyEcho);
    EXPECT_EQ(a->turns.since_us, since) << "its own key ends no turn";
}

/* A talk that is no key ends no turn inside its channel's window either:
 * nothing but the key goes there */
TEST(RequestTurnsTest, APlainTalkInsideAWindowEndsNoTurn)
{
    auto a = key_turn();
    ASSERT_FALSE(::testing::Test::HasFailure());
    const Bytes read = {0x22, 0x1E, 0x90};
    ecuio_talk_request q;
    q.pdu = read.data();
    q.len = 3;
    q.echo = 3;
    ASSERT_EQ(ecuio_engine_offer_talk(&a->e, 2, q, a->now),
              ecuio_talk_offer::taken);
    const int64_t since = a->turns.since_us;
    const size_t from = a->sent.size();
    a->pass();
    ASSERT_EQ(a->sent.size(), from + 1);
    EXPECT_EQ(a->sent[from].c, 0u);
    EXPECT_EQ(a->turns.since_us, since) << "channel 0's turn goes on";
}

/* A held channel past bit 15 is its own for a key too: channel 16's key
 * offered, held, is not the next request */
TEST(RequestTurnsTest, AHeldChannelsKeyPastBitFifteenIsNotNext)
{
    constexpr uint32_t n = 17;
    auto a = adapter(n);
    dids(&a->e, 0, 4);
    dids(&a->e, 16, 1, 0x80);
    a->ecus = key_ecu;
    a->until(1000 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_on(a.get(), 16));
    ASSERT_EQ(offer_key(a.get(), 16), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_turn_key(&a->e, 0), 16u);
    EXPECT_EQ(ecuio_engine_turn_key(&a->e, 1u << 16), n) << "held";
}
