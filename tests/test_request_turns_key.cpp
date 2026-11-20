// File/layer: tests/test_request_turns_key.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestTurnsTest (request_turns.h and a key's window:
//          whose turn an offered key takes, with the time passed in)

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
