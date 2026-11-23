// File/layer: tests/test_request_slots.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestSlotsTest (the cap on requests out at once and the
//          free slot going to the module due longest: ecuio_engine_slots,
//          ecuio_engine_out_count and ecuio_engine_slots_full in the engine,
//          ecuio_engine_next_slot in request_turns.h; ADR-0065 section 11),
//          J2534HoldTest's cap cases (j2534_hold_pass at the cap)

#include <gtest/gtest.h>

#include "../core/src/request_turns.h"
#include "j2534_hold_support.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

/* Modules answering a SocketCAN worker's sends, the time passed in: each
 * free slot's request written (ecuio_engine_next_slot), each module
 * answering after its own delay (pending: 7F xx 78 first, its answer
 * never coming) */
struct Slots {
    ecuio_request_engine e;
    std::vector<int64_t> delay_us;
    std::vector<int64_t> reply_at;
    std::vector<bool> pending;
    std::vector<std::vector<int64_t>> sent_at;
    uint32_t peak = 0;
    int64_t now = 0;

    /* n modules, rows rows each, answering after delay */
    void make(uint32_t n, int64_t delay, uint32_t rows = 1)
    {
        ASSERT_TRUE(ecuio_engine_init(&e, n));
        delay_us.assign(n, delay);
        reply_at.assign(n, INT64_MAX);
        pending.assign(n, false);
        sent_at.assign(n, {});
        for (uint32_t c = 0; c < n; ++c) {
            for (uint32_t k = 0; k < rows; ++k) {
                const uint8_t q[2] = {0x01, static_cast<uint8_t>(0x0C + k)};
                ASSERT_EQ(ecuio_engine_add(&e, c, q, 2),
                          ecuio_engine_add_result::added);
            }
        }
    }

    Bytes reply_to(uint32_t c) const
    {
        const ecuio_engine_channel& ch = e.chans[c];
        const ecuio_request& q = ecuio_engine_request_of(&e, c, ch.outstanding);
        if (pending[c]) {
            return {0x7F, q.bytes[0], 0x78};
        }
        Bytes b = {static_cast<uint8_t>(q.bytes[0] + 0x40)};
        b.insert(b.end(), q.bytes + 1, q.bytes + q.len);
        b.insert(b.end(), {0x11, 0x22});
        return b;
    }

    void send_free()
    {
        uint32_t c = 0;
        for (int32_t r = ecuio_engine_next_slot(&e, now, 0, &c);
             r != ECUIO_ENGINE_NONE;
             r = ecuio_engine_next_slot(&e, now, 0, &c)) {
            ecuio_engine_sent(&e, c, r, true, now);
            reply_at[c] = now + delay_us[c];
            sent_at[c].push_back(now);
            peak = std::max(peak, ecuio_engine_out_count(&e));
        }
    }

    void step()
    {
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (reply_at[c] <= now &&
                e.chans[c].outstanding != ECUIO_ENGINE_NONE) {
                const Bytes b = reply_to(c);
                (void)ecuio_engine_on_frame(&e, c, b.data(),
                                            static_cast<uint16_t>(b.size()),
                                            now, nullptr);
                reply_at[c] = INT64_MAX;
            }
            (void)ecuio_engine_expire(&e, c, now);
        }
        send_free();
        now += kMs;
    }

    void until(int64_t t_us)
    {
        while (now < t_us) {
            step();
        }
    }

    /* Channel c's longest time unasked from from_us to now: between two
     * sends, or from the last one (or from_us) to now */
    int64_t longest_gap(uint32_t c, int64_t from_us) const
    {
        int64_t gap = 0;
        int64_t last = from_us;
        for (const int64_t t : sent_at[c]) {
            if (t >= from_us) {
                gap = std::max(gap, t - last);
                last = t;
            }
        }
        return std::max(gap, now - last);
    }
};

TEST(RequestSlotsTest, NeverMoreThanTheCapOutstanding)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(18, 25 * kMs));
    s.until(10000 * kMs);
    EXPECT_EQ(s.peak, ECUIO_ENGINE_INFLIGHT_DEFAULT) << "reached, never passed";
    for (uint32_t c = 0; c < 18; ++c) {
        EXPECT_GT(s.sent_at[c].size(), 10u) << "module " << c;
    }
}

TEST(RequestSlotsTest, TheCapIsTheOpenModulesWhenFewer)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(3, 25 * kMs));
    s.until(1000 * kMs);
    EXPECT_EQ(s.peak, 3u) << "three modules ask at once";
}

TEST(RequestSlotsTest, TheCeilingBoundsTheCap)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(6, 25 * kMs));
    ecuio_engine_slots(&s.e, ECUIO_ENGINE_INFLIGHT_DEFAULT, 2);
    s.until(1000 * kMs);
    EXPECT_EQ(s.peak, 2u);
}

TEST(RequestSlotsTest, TheLongestDueModuleTakesAFreeSlot)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(6, 25 * kMs));
    ecuio_engine_slots(&s.e, 1, ECUIO_MAX_CHANNELS);
    for (uint32_t c = 0; c < 6; ++c) {
        s.e.chans[c].next_send_us = 20 * kMs;
    }
    s.e.chans[5].next_send_us = 10 * kMs;
    uint32_t c = 99;
    EXPECT_NE(ecuio_engine_next_slot(&s.e, 30 * kMs, 0, &c),
              ECUIO_ENGINE_NONE);
    EXPECT_EQ(c, 5u) << "due since 10 ms, the others since 20 ms";
}

TEST(RequestSlotsTest, EqualWaitsGoFromPastTheLastChosen)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(6, 25 * kMs));
    ecuio_engine_slots(&s.e, 1, ECUIO_MAX_CHANNELS);
    s.e.slot_from = 3;
    uint32_t c = 99;
    const int32_t r = ecuio_engine_next_slot(&s.e, 0, 0, &c);
    ASSERT_NE(r, ECUIO_ENGINE_NONE);
    EXPECT_EQ(c, 3u);
    EXPECT_EQ(s.e.slot_from, 4u);
    ecuio_engine_sent(&s.e, c, r, true, 0);
    EXPECT_EQ(ecuio_engine_next_slot(&s.e, 0, 0, &c), ECUIO_ENGINE_NONE)
        << "the one slot is taken";
}

TEST(RequestSlotsTest, NoModuleStarvesAtTheCap)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(18, 40 * kMs));
    s.delay_us[7] = 5 * kMs;
    s.until(10000 * kMs);
    /* 18 modules over 4 slots at 40 ms an exchange, half as long again */
    const int64_t most = 3 * 18 * 40 * kMs / (2 * 4);
    for (uint32_t c = 0; c < 18; ++c) {
        EXPECT_LE(s.longest_gap(c, 1000 * kMs), most) << "module " << c;
    }
}

TEST(RequestSlotsTest, AOneShotTakesASlot)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(6, 25 * kMs, 0));
    ecuio_engine_identify(&s.e);
    s.until(200 * kMs);
    EXPECT_EQ(s.peak, ECUIO_ENGINE_INFLIGHT_DEFAULT);
    for (uint32_t c = 0; c < 6; ++c) {
        EXPECT_FALSE(s.sent_at[c].empty()) << "module " << c;
    }
}

TEST(RequestSlotsTest, APendingWaitHoldsItsSlot)
{
    Slots s;
    ASSERT_NO_FATAL_FAILURE(s.make(6, 25 * kMs));
    for (uint32_t c = 0; c < 4; ++c) {
        s.pending[c] = true;
    }
    s.until(1000 * kMs);
    EXPECT_EQ(s.peak, ECUIO_ENGINE_INFLIGHT_DEFAULT);
    EXPECT_TRUE(s.sent_at[4].empty() && s.sent_at[5].empty())
        << "four requests in a pending wait hold every slot";
}

/* j2534_hold_pass, held and plain, over six Single Frame modules */
void six_single_frames(j2534_hold_test::Bus& b)
{
    ASSERT_TRUE(ecuio_engine_init(&b.e, 6));
    b.e.gap_us = 5 * kMs;
    for (uint32_t c = 0; c < 6; ++c) {
        const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(c)};
        ASSERT_EQ(ecuio_engine_add(&b.e, c, did, 3),
                  ecuio_engine_add_result::added);
    }
    b.first_us = 20 * kMs;
    b.reply_of = [](uint32_t, const ecuio_request& q) {
        return Bytes{0x62, q.bytes[1], q.bytes[2], 0x2A};
    };
}

/* The most requests out at once over b's exchanges */
uint32_t peak_out(const j2534_hold_test::Bus& b)
{
    uint32_t peak = 0;
    for (const j2534_hold_test::Exchange& x : b.exchanges) {
        uint32_t out = 0;
        for (const j2534_hold_test::Exchange& y : b.exchanges) {
            out += (y.sent_us <= x.sent_us && x.sent_us < y.ended_us) ? 1u : 0u;
        }
        peak = std::max(peak, out);
    }
    return peak;
}

TEST(J2534HoldTest, TheHoldWritesNoMoreThanTheCap)
{
    j2534_hold_test::Bus b;
    ASSERT_NO_FATAL_FAILURE(six_single_frames(b));
    b.until(2000 * kMs);
    EXPECT_EQ(peak_out(b), ECUIO_ENGINE_INFLIGHT_DEFAULT);
    for (uint32_t c = 0; c < 6; ++c) {
        EXPECT_GT(b.written(c).size(), 10u) << "module " << c;
    }
}

TEST(J2534HoldTest, ThePlainPassWritesNoMoreThanTheCap)
{
    j2534_hold_test::Bus b;
    ASSERT_NO_FATAL_FAILURE(six_single_frames(b));
    b.plain = true;
    b.until(2000 * kMs);
    EXPECT_EQ(peak_out(b), ECUIO_ENGINE_INFLIGHT_DEFAULT);
    for (uint32_t c = 0; c < 6; ++c) {
        EXPECT_GT(b.written(c).size(), 10u) << "module " << c;
    }
}

} // namespace
