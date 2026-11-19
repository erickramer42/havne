// File/layer: tests/test_j2534_hold.cpp | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest (core/src/j2534_hold.h: the J2534 worker's
//          hold for Known Issue #42, against a bus of modules answering
//          with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/j2534_hold.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;
constexpr int64_t kStepUs = 250;

/* One request on the bus: its channel, when it was written and when it
 * ended (answered, or its wait ran out; INT64_MAX while it is out),
 * whether it was answered and whether its reply took more than one
 * frame */
struct Exchange {
    uint32_t c;
    int64_t sent_us;
    int64_t ended_us;
    bool answered;
    bool segmented;
};

/* Modules answering a J2534 worker's pass: each request's first frame
 * after first_us, each frame more 5 ms later; a module's reply_of gives
 * its answer (empty: silence). The pass is the worker's: expire, then
 * each channel in the hold's order through j2534_hold_next, or due when
 * the hold is left out (plain). */
struct Bus {
    ecuio_request_engine e;
    j2534_hold hold;
    bool plain = false;
    int64_t now = 0;
    int64_t first_us = 3 * kMs;
    std::function<Bytes(uint32_t, const ecuio_request&)> reply_of;
    std::vector<Exchange> exchanges;
    struct Due {
        uint32_t c;
        int64_t at;
        Bytes bytes;
        size_t exchange;
    };
    std::vector<Due> due;

    void pass()
    {
        for (size_t i = 0; i < due.size();) {
            if (due[i].at > now) {
                ++i;
                continue;
            }
            (void)ecuio_engine_on_frame(
                &e, due[i].c, due[i].bytes.data(),
                static_cast<uint16_t>(due[i].bytes.size()), now, nullptr);
            exchanges[due[i].exchange].ended_us = now;
            exchanges[due[i].exchange].answered = true;
            due.erase(due.begin() + static_cast<std::ptrdiff_t>(i));
        }
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (ecuio_engine_expire(&e, c, now)) {
                for (Exchange& x : exchanges) {
                    if (x.c == c && x.ended_us == INT64_MAX) {
                        x.ended_us = now;
                    }
                }
            }
        }
        uint32_t order[ECUIO_MAX_CHANNELS];
        j2534_hold_order(hold, &e, order);
        uint32_t held_now = 0;
        for (uint32_t k = 0; k < e.chan_count; ++k) {
            const uint32_t c = order[k];
            const int32_t r =
                plain ? ecuio_engine_due(&e, c, now)
                      : j2534_hold_next(hold, &e, c, now, &held_now);
            if (r != ECUIO_ENGINE_NONE) {
                write(c, r);
            }
        }
        j2534_hold_pass_end(hold, held_now);
        now += kStepUs;
    }

    void write(uint32_t c, int32_t r)
    {
        const Bytes reply = reply_of(c, ecuio_engine_request_of(&e, c, r));
        ecuio_engine_sent(&e, c, r, true, now);
        const int64_t frames =
            reply.size() <= 7 ? 1 : 1 + (int64_t(reply.size()) - 6 + 6) / 7;
        exchanges.push_back({c, now, INT64_MAX, false, frames > 1});
        if (!reply.empty()) {
            due.push_back({c, now + first_us + (frames - 1) * 5 * kMs, reply,
                           exchanges.size() - 1});
        }
    }

    void until(int64_t t_us)
    {
        while (now < t_us) {
            pass();
        }
    }

    std::vector<uint32_t> channels(size_t from = 0) const
    {
        std::vector<uint32_t> out;
        for (size_t i = from; i < exchanges.size(); ++i) {
            out.push_back(exchanges[i].c);
        }
        return out;
    }
};

/* Each channel asks one DID: channel seg's reply takes three frames, the
 * others' a Single Frame; the J2534 floor (5 ms) */
void two_modules(Bus& b, uint32_t chans = 2, uint32_t seg = 0)
{
    ASSERT_TRUE(ecuio_engine_init(&b.e, chans));
    b.e.gap_us = 5 * kMs;
    for (uint32_t c = 0; c < chans; ++c) {
        const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(0x80 * c)};
        ASSERT_EQ(ecuio_engine_add(&b.e, c, did, 3),
                  ecuio_engine_add_result::added);
    }
    b.reply_of = [seg](uint32_t c, const ecuio_request& q) {
        Bytes out = {0x62, q.bytes[1], q.bytes[2]};
        out.insert(out.end(), c == seg ? 17u : 2u, uint8_t{0x2A});
        return out;
    };
}

/* Whether exchange a, a segmented one, overlaps any other channel's
 * exchange on the bus */
bool overlaps_another(const std::vector<Exchange>& xs, size_t a)
{
    for (size_t i = 0; i < xs.size(); ++i) {
        if (i != a && xs[i].c != xs[a].c && xs[i].sent_us < xs[a].ended_us &&
            xs[a].sent_us < xs[i].ended_us) {
            return true;
        }
    }
    return false;
}

} // namespace

/* Once channel 0's reply is known to take three frames, each of its
 * exchanges is alone on the bus: no other channel's request is out while
 * it is (the first, not yet learned, goes as any) */
TEST(J2534HoldTest, ASegmentedExchangeIsAloneOnTheBus)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(2000 * kMs);
    size_t segmented = 0;
    bool learned = false;
    for (size_t i = 0; i < b.exchanges.size(); ++i) {
        if (b.exchanges[i].c != 0) {
            continue;
        }
        if (learned) {
            ++segmented;
            EXPECT_FALSE(overlaps_another(b.exchanges, i)) << "exchange " << i;
        }
        learned = true;
    }
    EXPECT_GT(segmented, 50u);
    for (const Exchange& x : b.exchanges) {
        EXPECT_TRUE(x.answered || x.ended_us == INT64_MAX)
            << "every request answered but the last ones out";
    }
}

/* The channels take turns: a held channel goes first when the hold ends,
 * so from channel 0's first reply (its frames learned) the writes
 * alternate 0, 1, 0, 1 */
TEST(J2534HoldTest, TheHeldChannelGoesNext)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(500 * kMs);
    ASSERT_EQ(b.exchanges[0].c, 0u);
    size_t from = 0;
    while (from < b.exchanges.size() &&
           b.exchanges[from].sent_us < b.exchanges[0].ended_us) {
        ++from;
    }
    const std::vector<uint32_t> cs = b.channels(from);
    ASSERT_GT(cs.size(), 20u);
    for (size_t i = 1; i < cs.size(); ++i) {
        EXPECT_NE(cs[i], cs[i - 1]) << "write " << from + i;
    }
}

/* The hold costs the segmented channel the other's exchange and nothing
 * more, whichever channel it is: with modules answering in 6 ms (past
 * the 5 ms floor), the two are asked equally often, once for each 16 ms
 * exchange and 6 ms one. With the segmented channel second in the pass,
 * its chosen request is what keeps the first from starting another. */
TEST(J2534HoldTest, EachChannelKeepsAPace)
{
    for (const uint32_t seg : {0u, 1u}) {
        Bus b;
        ASSERT_NO_FATAL_FAILURE(two_modules(b, 2, seg));
        b.first_us = 6 * kMs;
        b.until(1000 * kMs);
        size_t n[2] = {};
        for (const Exchange& x : b.exchanges) {
            ++n[x.c];
        }
        /* about 1000 / 22 each */
        EXPECT_GT(n[seg], 38u) << "segmented channel " << seg;
        EXPECT_LT(n[seg], 52u) << "segmented channel " << seg;
        EXPECT_LE(n[0] > n[1] ? n[0] - n[1] : n[1] - n[0], 1u)
            << "segmented channel " << seg;
    }
}

/* A chosen request waits only for the replies out: a silent module's
 * request holds it for that request's wait, and it is written then */
TEST(J2534HoldTest, AChosenRequestGoesWhenTheOthersWaitRunsOut)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(200 * kMs); /* channel 0's reply learned */
    b.reply_of = [](uint32_t c, const ecuio_request& q) {
        if (c == 1) {
            return Bytes{}; /* channel 1 falls silent */
        }
        Bytes out = {0x62, q.bytes[1], q.bytes[2]};
        out.insert(out.end(), 17u, uint8_t{0x2A});
        return out;
    };
    const size_t from = b.exchanges.size();
    b.until(1200 * kMs);
    for (size_t i = from; i < b.exchanges.size(); ++i) {
        const Exchange& x = b.exchanges[i];
        if (x.c == 0) {
            EXPECT_FALSE(overlaps_another(b.exchanges, i)) << "exchange " << i;
        }
    }
    size_t zero = 0;
    for (size_t i = from; i < b.exchanges.size(); ++i) {
        zero += b.exchanges[i].c == 0 ? 1u : 0u;
    }
    /* each lap: channel 1's 100 ms wait, then channel 0's 13 ms */
    EXPECT_GE(zero, 8u);
    EXPECT_LE(zero, 10u);
}

/* Off (?hold=0) or with one channel, the hold is due itself: the same
 * writes at the same times as a worker with no hold */
TEST(J2534HoldTest, OffOrWithOneChannelTheHoldIsDue)
{
    for (const uint32_t chans : {2u, 1u}) {
        Bus plain;
        ASSERT_NO_FATAL_FAILURE(two_modules(plain, chans));
        plain.plain = true;
        plain.until(300 * kMs);
        Bus held;
        ASSERT_NO_FATAL_FAILURE(two_modules(held, chans));
        held.hold.on = chans == 1; /* one channel: on, and no hold */
        held.until(300 * kMs);
        ASSERT_EQ(held.exchanges.size(), plain.exchanges.size()) << chans;
        for (size_t i = 0; i < plain.exchanges.size(); ++i) {
            EXPECT_EQ(held.exchanges[i].c, plain.exchanges[i].c);
            EXPECT_EQ(held.exchanges[i].sent_us, plain.exchanges[i].sent_us);
        }
        if (chans == 2) {
            bool any = false;
            for (size_t i = 2; i < plain.exchanges.size(); ++i) {
                any = any || (plain.exchanges[i].c == 0 &&
                              overlaps_another(plain.exchanges, i));
            }
            EXPECT_TRUE(any) << "with no hold the exchanges overlap";
        }
    }
}

/* Three channels, one segmented: the two Single Frame channels may share
 * the bus with each other, never with the segmented one */
TEST(J2534HoldTest, SingleFramesShareTheBusButNotWithASegmentedReply)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b, 3));
    b.until(1000 * kMs);
    bool shared = false;
    bool learned = false;
    for (size_t i = 0; i < b.exchanges.size(); ++i) {
        const Exchange& x = b.exchanges[i];
        if (x.c == 0) {
            if (learned) {
                EXPECT_FALSE(overlaps_another(b.exchanges, i)) << i;
            }
            learned = true;
            continue;
        }
        for (const Exchange& y : b.exchanges) {
            shared =
                shared || (y.c != x.c && y.c != 0 && y.sent_us < x.ended_us &&
                           x.sent_us < y.ended_us);
        }
    }
    EXPECT_TRUE(shared) << "channels 1 and 2 overlap";
}
