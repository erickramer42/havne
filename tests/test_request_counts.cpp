// File/layer: tests/test_request_counts.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestCountsTest (each module's lost, busy, pending and
//          late replies and the engine's overflow and refused-write counts,
//          ecuio_engine_counts in request_engine_state.h, counted in
//          request_engine_reply.h; ADR-0065 section 11); RequestEngineTest
//          (a wait run out that no read covered, ecuio_engine_wait_unread)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

/* An engine of one channel asking 01 0C, nothing else */
struct OneRow {
    ecuio_request_engine e;
    OneRow()
    {
        EXPECT_TRUE(ecuio_engine_init(&e, 1));
        const uint8_t q[2] = {0x01, 0x0C};
        EXPECT_EQ(ecuio_engine_add(&e, 0, q, 2),
                  ecuio_engine_add_result::added);
    }
    /* The due request, sent at now */
    int32_t send(int64_t now)
    {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        EXPECT_NE(r, ECUIO_ENGINE_NONE);
        ecuio_engine_sent(&e, 0, r, true, now);
        return r;
    }
    ecuio_reply take(const Bytes& b, int64_t now)
    {
        return ecuio_engine_on_frame(&e, 0, b.data(),
                                     static_cast<uint16_t>(b.size()), now,
                                     nullptr);
    }
    const ecuio_engine_counts& counts() const { return e.chans[0].counts; }
};

const Bytes kRpm = {0x41, 0x0C, 0x1A, 0xF8};

/* A wait run out whose deadline no read covered (ecuio_engine_wait_unread):
 * the worker reads before it ends the wait. A read that covered the
 * deadline, a wait not yet run out, or one ended, leaves nothing unread */
TEST(RequestEngineTest, AWaitRunOutThatNoReadCoveredIsUnread)
{
    OneRow t;
    constexpr int64_t d = ECUIO_ENGINE_WAIT_US;
    EXPECT_FALSE(ecuio_engine_wait_unread(&t.e, 0, d)) << "nothing out";
    t.send(0);
    EXPECT_FALSE(ecuio_engine_wait_unread(&t.e, 0, d - 1)) << "still waiting";
    EXPECT_TRUE(ecuio_engine_wait_unread(&t.e, d - 1, d));
    EXPECT_TRUE(ecuio_engine_wait_unread(&t.e, 0, d + 50 * kMs));
    EXPECT_FALSE(ecuio_engine_wait_unread(&t.e, d, d)) << "read to it";
    EXPECT_FALSE(ecuio_engine_wait_unread(&t.e, d + 1, d + 2));
    EXPECT_TRUE(ecuio_engine_expire(&t.e, 0, d));
    EXPECT_FALSE(ecuio_engine_wait_unread(&t.e, 0, d)) << "ended";
}

/* An empty read that waited out its timeout covers to its end; one that
 * returned early, or brought a frame, only to its start */
TEST(RequestEngineTest, AReadCoversToItsEndOnlyEmptyAndWaitedOut)
{
    EXPECT_EQ(ecuio_engine_read_covers(90, 100, 10, false), 100);
    EXPECT_EQ(ecuio_engine_read_covers(90, 101, 10, false), 101);
    EXPECT_EQ(ecuio_engine_read_covers(90, 99, 10, false), 90) << "early";
    EXPECT_EQ(ecuio_engine_read_covers(90, 100, 10, true), 90) << "a frame";
}

TEST(RequestCountsTest, AWaitRunOutWithNothingIsALostReply)
{
    OneRow t;
    t.send(0);
    EXPECT_FALSE(ecuio_engine_expire(&t.e, 0, ECUIO_ENGINE_WAIT_US - 1));
    EXPECT_EQ(t.counts().lost, 0u);
    EXPECT_TRUE(ecuio_engine_expire(&t.e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(t.counts().lost, 1u);
    t.send(200 * kMs);
    EXPECT_EQ(t.take(kRpm, 204 * kMs), ecuio_reply::positive);
    EXPECT_EQ(t.counts().lost, 1u) << "an answered request loses nothing";
}

TEST(RequestCountsTest, AnUnansweredItemIsNoLostReply)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0xF190));
    ecuio_engine_identify(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, ECUIO_SESSION_EXTENDED,
                                      ECUIO_ENGINE_KEEPALIVE_US));
    int64_t now = 0;
    uint32_t asked = 0;
    /* the session's 10 03, the identification items and the scanned DID,
     * each let run out */
    for (; asked < 40; ++asked) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            break;
        }
        ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
        ecuio_engine_sent(&e, 0, r, true, now);
        now += ECUIO_ENGINE_WAIT_US;
        ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
        now += ECUIO_ENGINE_GAP_US;
    }
    EXPECT_GT(asked, 2u) << "the session, the items and the DID were asked";
    EXPECT_EQ(e.chans[0].counts.lost, 0u);
}

TEST(RequestCountsTest, ABusyAnswerIsCounted)
{
    OneRow t;
    t.send(0);
    EXPECT_EQ(t.take({0x7F, 0x01, 0x21}, 4 * kMs), ecuio_reply::negative);
    EXPECT_EQ(t.counts().busy, 1u);
    EXPECT_EQ(t.counts().pending, 0u);
    t.send(100 * kMs);
    EXPECT_EQ(t.take({0x7F, 0x01, 0x31}, 104 * kMs), ecuio_reply::negative);
    EXPECT_EQ(t.counts().busy, 1u) << "another refusal is no busy answer";
}

TEST(RequestCountsTest, EachPendingAnswerIsCounted)
{
    OneRow t;
    t.send(0);
    const Bytes pending = {0x7F, 0x01, 0x78};
    EXPECT_EQ(t.take(pending, 4 * kMs), ecuio_reply::pending);
    EXPECT_EQ(t.take(pending, 1000 * kMs), ecuio_reply::pending);
    EXPECT_EQ(t.take(kRpm, 1500 * kMs), ecuio_reply::positive);
    EXPECT_EQ(t.counts().pending, 2u);
    EXPECT_EQ(t.counts().busy, 0u);
}

TEST(RequestCountsTest, AReplyPastP2IsLate)
{
    OneRow t;
    t.send(0);
    EXPECT_EQ(t.take(kRpm, ECUIO_ENGINE_P2_US), ecuio_reply::positive);
    EXPECT_EQ(t.counts().late, 0u) << "at P2 a reply is on time";
    const int64_t at = 200 * kMs;
    t.send(at);
    EXPECT_EQ(t.take(kRpm, at + ECUIO_ENGINE_P2_US + 1),
              ecuio_reply::positive);
    EXPECT_EQ(t.counts().late, 1u);
    t.send(400 * kMs);
    EXPECT_EQ(t.take({0x7F, 0x01, 0x31}, 400 * kMs + 60 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(t.counts().late, 2u) << "a late refusal is late too";
    EXPECT_EQ(t.take(kRpm, 470 * kMs), ecuio_reply::none);
    EXPECT_EQ(t.counts().late, 2u) << "a reply to nothing out completes none";
}

TEST(RequestCountsTest, TheEnginesCountsStopAtTheirMost)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ecuio_engine_overflowed(&e);
    ecuio_engine_write_refused(&e);
    EXPECT_EQ(e.overflowed, 1u);
    EXPECT_EQ(e.writes_refused, 1u);
    e.overflowed = UINT32_MAX;
    ecuio_engine_overflowed(&e);
    EXPECT_EQ(e.overflowed, UINT32_MAX);
}

} // namespace
