// File/layer: tests/test_j2534_hold_key.cpp | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest (core/src/j2534_hold.h and a key's window:
//          a key goes ahead of the other channels' next requests, never
//          beside a segmented reply and never past its window, against
//          the hold's bus with the time passed in)

#include "j2534_hold_support.h"

using namespace j2534_hold_test;

namespace {

/* A made-up level 01: its seed's request, its key's by the echo bytes the
 * one-shot holds, and the key as offered */
const Bytes kSeed = {0x27, 0x01};
const Bytes kKeyEcho = {0x27, 0x02};
const Bytes kKey = {0x27, 0x02, 0x11, 0x22, 0x33, 0x44};

/* two_modules with security access answered in Single Frames on every
 * channel, and a DID's reply three frames on each channel with a bit in
 * segmented */
void key_modules(Bus& b, uint32_t chans, uint32_t segmented)
{
    ASSERT_NO_FATAL_FAILURE(two_modules(b, chans));
    b.reply_of = [segmented](uint32_t c, const ecuio_request& q) {
        const Bytes asked(q.bytes, q.bytes + q.len);
        if (asked == kSeed) {
            return Bytes{0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08};
        }
        if (asked == kKeyEcho) {
            return Bytes{0x67, 0x02};
        }
        Bytes out = {0x62, q.bytes[1], q.bytes[2]};
        out.insert(out.end(), ((segmented >> c) & 1u) != 0 ? 17u : 2u,
                   uint8_t{0x2A});
        return out;
    };
}

/* Channel c's seed offered with a window of window_ms and written */
void seed_out(Bus& b, uint32_t c, int64_t window_ms)
{
    ecuio_talk_request q;
    q.pdu = kSeed.data();
    q.len = 2;
    q.echo = 2;
    q.kind = ecuio_talk_kind::seed;
    q.window_us = window_ms * kMs;
    ASSERT_EQ(ecuio_engine_offer_talk(&b.e, c, q, b.now),
              ecuio_talk_offer::taken);
    for (int i = 0; i < 4000 && !b.e.chans[c].talk.out; ++i) {
        b.pass();
    }
    ASSERT_TRUE(b.e.chans[c].talk.out);
}

/* Passes until channel c's talk has ended, its end into *out. It stops
 * with the reply delivered and the pass at that time not yet made, where
 * a caller's next offer goes. */
void talk_ended(Bus& b, uint32_t c, ecuio_talk_result* out)
{
    for (int i = 0; i < 4000; ++i) {
        b.deliver();
        if (ecuio_engine_talk_take(b.e.chans[c].talk, out)) {
            return;
        }
        b.pass();
    }
    FAIL() << "channel " << c << "'s talk never ended";
}

void seed_answered(Bus& b, uint32_t c, int64_t window_ms)
{
    ASSERT_NO_FATAL_FAILURE(seed_out(b, c, window_ms));
    ecuio_talk_result r;
    ASSERT_NO_FATAL_FAILURE(talk_ended(b, c, &r));
    ASSERT_EQ(r.end, ecuio_talk_end::answered);
}

ecuio_talk_offer offer_key(Bus& b, uint32_t c)
{
    ecuio_talk_request q;
    q.pdu = kKey.data();
    q.len = static_cast<uint32_t>(kKey.size());
    q.echo = 2;
    q.kind = ecuio_talk_kind::key;
    return ecuio_engine_offer_talk(&b.e, c, q, b.now);
}

/* Passes until a request is written; false when none is in 100 ms */
bool next_write(Bus& b)
{
    const size_t from = b.exchanges.size();
    for (int i = 0; i < 400 && b.exchanges.size() == from; ++i) {
        b.pass();
    }
    return b.exchanges.size() > from;
}

/* The exchange still out on channel c: its index, or the count */
size_t out_on(const Bus& b, uint32_t c)
{
    for (size_t i = b.exchanges.size(); i-- > 0;) {
        if (b.exchanges[i].c == c && b.exchanges[i].ended_us == INT64_MAX) {
            return i;
        }
    }
    return b.exchanges.size();
}

} // namespace

/* A key offered while another channel's chosen request waits goes first,
 * whichever of the two channels the pass reaches first: the key isn't
 * held for a request that hasn't started (its channel first), and the
 * chosen request waits on for the key's reply (its channel first) */
TEST(J2534HoldTest, AKeyGoesAheadOfAnothersChosenRequest)
{
    for (const uint32_t key : {0u, 1u}) {
        const uint32_t other = 1 - key;
        Bus b;
        ASSERT_NO_FATAL_FAILURE(key_modules(b, 2, 1u << other));
        b.until(200 * kMs);
        /* The key's module answers after the other's floor, so the other's
         * next request falls due with the seed out and is kept chosen */
        b.first_of_us[key] = 8 * kMs;
        ASSERT_NO_FATAL_FAILURE(seed_out(b, key, 300));
        for (int i = 0; i < 400 && b.hold.chosen[other] == ECUIO_ENGINE_NONE;
             ++i) {
            b.pass();
        }
        ASSERT_NE(b.hold.chosen[other], ECUIO_ENGINE_NONE) << key;
        ASSERT_TRUE(b.e.chans[key].talk.out) << "chosen while the seed is out";
        ecuio_talk_result r;
        ASSERT_NO_FATAL_FAILURE(talk_ended(b, key, &r));
        ASSERT_EQ(r.end, ecuio_talk_end::answered) << key;
        ASSERT_NE(b.hold.chosen[other], ECUIO_ENGINE_NONE) << "still waiting";
        ASSERT_EQ(offer_key(b, key), ecuio_talk_offer::taken) << key;
        const size_t from = b.exchanges.size();
        ASSERT_TRUE(next_write(b)) << key;
        EXPECT_EQ(b.exchanges[from].c, key)
            << "the key, ahead of the chosen one";
        EXPECT_EQ(b.exchanges[from].bytes, kKeyEcho) << key;
        ASSERT_NO_FATAL_FAILURE(talk_ended(b, key, &r));
        EXPECT_EQ(r.end, ecuio_talk_end::answered) << key;
        ASSERT_TRUE(next_write(b)) << key;
        const Exchange& chosen = b.exchanges[from + 1];
        EXPECT_EQ(chosen.c, other) << key;
        EXPECT_TRUE(chosen.released) << "the request that waited, given back";
        EXPECT_GE(chosen.sent_us, b.exchanges[from].ended_us)
            << "after the key's reply, never beside it";
    }
}

/* With three modules a key offered while channel 2's segmented exchange is
 * out waits for that exchange alone: it is the next write, at the
 * exchange's end, though channel 0, ahead of it in the pass's order, has
 * a segmented request ready, which follows the key's reply */
TEST(J2534HoldTest, AKeyWaitsOnlyForTheExchangeInFlight)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(key_modules(b, 3, (1u << 0) | (1u << 2)));
    b.until(300 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_answered(b, 1, 300));
    for (int i = 0; i < 800 && !j2534_hold_on_bus(&b.e, 2); ++i) {
        b.pass();
    }
    ASSERT_TRUE(j2534_hold_on_bus(&b.e, 2));
    const size_t flight = out_on(b, 2);
    ASSERT_LT(flight, b.exchanges.size());
    ASSERT_TRUE(b.exchanges[flight].segmented);
    ASSERT_EQ(offer_key(b, 1), ecuio_talk_offer::taken);
    const size_t from = b.exchanges.size();
    ASSERT_TRUE(next_write(b));
    const Exchange key = b.exchanges[from];
    EXPECT_EQ(key.c, 1u) << "the key, not channel 0's request";
    EXPECT_EQ(key.bytes, kKeyEcho);
    EXPECT_GE(key.sent_us, b.exchanges[flight].ended_us)
        << "never beside a segmented reply";
    EXPECT_LE(key.sent_us - b.exchanges[flight].ended_us, kStepUs)
        << "and at that exchange's end";
    ecuio_talk_result r;
    ASSERT_NO_FATAL_FAILURE(talk_ended(b, 1, &r));
    EXPECT_EQ(r.end, ecuio_talk_end::answered);
    ASSERT_TRUE(next_write(b));
    EXPECT_NE(b.exchanges[from + 1].c, 1u);
    EXPECT_GE(b.exchanges[from + 1].sent_us, b.exchanges[from].ended_us)
        << "the others ask on after the key's reply";
}

/* While a key waits no other channel starts a request: one due in the
 * same pass, ahead of the key's channel in the pass's order, would put a
 * segmented reply on the bus for the key to wait out */
TEST(J2534HoldTest, WhileAKeyWaitsNoOtherChannelStartsARequest)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(key_modules(b, 2, 1u << 0));
    /* A floor longer than an exchange: channel 0 falls due with the bus
     * idle, so the pass before held no one and this one starts with it */
    b.e.gap_us = 20 * kMs;
    b.until(300 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_answered(b, 1, 300));
    /* The pass where channel 0, idle, has its next request due */
    bool due = false;
    for (int i = 0; i < 800 && !due; ++i) {
        b.deliver();
        const ecuio_engine_channel& ch = b.e.chans[0];
        due = ch.outstanding == ECUIO_ENGINE_NONE &&
              b.hold.chosen[0] == ECUIO_ENGINE_NONE && b.now >= ch.next_send_us;
        if (!due) {
            b.pass();
        }
    }
    ASSERT_TRUE(due);
    ASSERT_EQ(b.hold.held, 0u) << "channel 0 is first in this pass";
    ASSERT_EQ(offer_key(b, 1), ecuio_talk_offer::taken);
    const size_t from = b.exchanges.size();
    ASSERT_TRUE(next_write(b));
    EXPECT_EQ(b.exchanges[from].c, 1u) << "the key first";
    EXPECT_EQ(b.exchanges[from].bytes, kKeyEcho);
    ecuio_talk_result r;
    ASSERT_NO_FATAL_FAILURE(talk_ended(b, 1, &r));
    EXPECT_EQ(r.end, ecuio_talk_end::answered);
    ASSERT_TRUE(next_write(b));
    EXPECT_EQ(b.exchanges[from + 1].c, 0u);
    EXPECT_GE(b.exchanges[from + 1].sent_us, b.exchanges[from].ended_us);
}

/* A key past its window keeps no channel waiting, in the very pass that
 * will settle it: a channel ahead of the key's in the pass's order asks
 * as if there were none */
TEST(J2534HoldTest, AKeyPastItsWindowHoldsNoOne)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(key_modules(b, 2, 0));
    b.until(100 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_answered(b, 1, 300));
    /* Channel 0's reply in, its next request not yet asked */
    bool idle = false;
    for (int i = 0; i < 400 && !idle; ++i) {
        b.deliver();
        idle = b.e.chans[0].outstanding == ECUIO_ENGINE_NONE;
        if (!idle) {
            b.pass();
        }
    }
    ASSERT_TRUE(idle);
    ASSERT_EQ(offer_key(b, 1), ecuio_talk_offer::taken);
    const int64_t until = b.e.chans[1].key.until_us;
    uint32_t held = 0;
    EXPECT_EQ(j2534_hold_next(b.hold, &b.e, 0, until - 1, &held),
              ECUIO_ENGINE_NONE)
        << "inside the window channel 0 waits for the key";
    EXPECT_EQ(held, 1u);
    held = 0;
    EXPECT_NE(j2534_hold_next(b.hold, &b.e, 0, until, &held), ECUIO_ENGINE_NONE)
        << "past it channel 0 asks";
    EXPECT_EQ(held, 0u);
}

/* A key whose channel's floor (?gap=) falls past its window can never
 * go: it holds no one, so channel 0 asks at its own floor inside the
 * window, and the window's end is the next event, where the key is
 * settled unsent, ahead of the key's floor */
TEST(J2534HoldTest, AKeyThatCantGoInItsWindowHoldsNoOne)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(key_modules(b, 2, 1u << 0));
    /* Channel 0's 13 ms segmented exchanges hold channel 1 that long, so
     * channel 1 asks 13 ms behind it, each every 100 ms */
    b.e.gap_us = 100 * kMs;
    b.until(500 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_answered(b, 1, 90));
    const int64_t until = b.e.chans[1].key.until_us;
    ASSERT_GE(b.e.chans[1].next_send_us, until) << "the key's floor past it";
    ASSERT_GT(b.e.chans[0].next_send_us, b.now);
    ASSERT_LT(b.e.chans[0].next_send_us, until) << "channel 0 due inside it";
    const int64_t floor0 = b.e.chans[0].next_send_us;
    ASSERT_EQ(offer_key(b, 1), ecuio_talk_offer::taken);
    const size_t from = b.exchanges.size();
    ASSERT_TRUE(next_write(b));
    EXPECT_EQ(b.exchanges[from].c, 0u);
    EXPECT_LE(b.exchanges[from].sent_us - floor0, kStepUs)
        << "at channel 0's floor, not held for the key";
    EXPECT_EQ(j2534_hold_next_event_us(b.hold, &b.e, b.now), until - b.now)
        << "the window's end, ahead of the key's floor";
    while (b.now < until) {
        b.pass();
    }
    ecuio_talk_result r;
    EXPECT_FALSE(ecuio_engine_talk_take(b.e.chans[1].talk, &r));
    b.pass();
    ASSERT_TRUE(ecuio_engine_talk_take(b.e.chans[1].talk, &r))
        << "settled at the window's end";
    EXPECT_EQ(r.end, ecuio_talk_end::unsent);
    b.until(b.now + 300 * kMs);
    for (size_t i = from; i < b.exchanges.size(); ++i) {
        EXPECT_NE(b.exchanges[i].bytes, kKeyEcho) << "request " << i;
    }
}

/* A key that can't go in its window, with nothing on the bus to hold its
 * channel: the window's end is still the next event, ahead of both
 * channels' floors, so the worker wakes to settle it there */
TEST(J2534HoldTest, AKeyThatCantGoIsSettledAtItsWindowsEnd)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(key_modules(b, 2, 0));
    b.e.gap_us = 100 * kMs; /* both channels ask together, every 100 ms */
    b.until(500 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_answered(b, 1, 90));
    const int64_t until = b.e.chans[1].key.until_us;
    ASSERT_GE(b.e.chans[1].next_send_us, until) << "the key's floor past it";
    ASSERT_GE(b.e.chans[0].next_send_us, until) << "channel 0's too";
    ASSERT_EQ(offer_key(b, 1), ecuio_talk_offer::taken);
    b.pass();
    ASSERT_EQ(b.hold.held, 0u) << "nothing on the bus holds the key's channel";
    EXPECT_EQ(j2534_hold_next_event_us(b.hold, &b.e, b.now), until - b.now);
    while (b.now < until) {
        b.pass();
    }
    b.pass();
    ecuio_talk_result r;
    ASSERT_TRUE(ecuio_engine_talk_take(b.e.chans[1].talk, &r));
    EXPECT_EQ(r.end, ecuio_talk_end::unsent);
}

/* A key held behind a segmented exchange past its window is settled
 * unsent at the window's end, which is the hold's next event, and is
 * never written: late is worse than not at all, for a seed the module
 * has dropped */
TEST(J2534HoldTest, AKeyHeldPastItsWindowIsSettledUnsentAndNeverWritten)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(key_modules(b, 2, 1u << 1));
    b.first_of_us[1] = 80 * kMs; /* an exchange longer than the window */
    b.until(500 * kMs);
    ASSERT_NO_FATAL_FAILURE(seed_answered(b, 0, 50));
    for (int i = 0; i < 400 && !j2534_hold_on_bus(&b.e, 1); ++i) {
        b.pass();
    }
    ASSERT_TRUE(j2534_hold_on_bus(&b.e, 1));
    const size_t flight = out_on(b, 1);
    ASSERT_LT(flight, b.exchanges.size());
    const int64_t until = b.e.chans[0].key.until_us;
    ASSERT_EQ(offer_key(b, 0), ecuio_talk_offer::taken)
        << "inside the window still";
    const size_t from = b.exchanges.size();
    b.pass();
    EXPECT_NE(b.hold.held & 1u, 0u) << "held behind the segmented reply";
    EXPECT_EQ(j2534_hold_next_event_us(b.hold, &b.e, b.now), until - b.now)
        << "the window's end is the next event, ahead of the other's wait";
    while (b.now < until) {
        b.pass();
    }
    ecuio_talk_result r;
    EXPECT_FALSE(ecuio_engine_talk_take(b.e.chans[0].talk, &r))
        << "inside its window it waits";
    b.pass();
    ASSERT_TRUE(ecuio_engine_talk_take(b.e.chans[0].talk, &r))
        << "settled at the window's end, not at the hold's";
    EXPECT_EQ(r.end, ecuio_talk_end::unsent);
    EXPECT_EQ(b.exchanges[flight].ended_us, INT64_MAX)
        << "the exchange it waited for is still out";
    EXPECT_EQ(b.exchanges.size(), from) << "nothing was written meanwhile";
    b.until(b.now + 400 * kMs);
    for (size_t i = from; i < b.exchanges.size(); ++i) {
        EXPECT_NE(b.exchanges[i].bytes, kKeyEcho) << "request " << i;
    }
    EXPECT_GT(b.exchanges.size(), from) << "and the channels ask on";
}
