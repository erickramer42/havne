// File/layer: tests/test_scan_retry.cpp | test_obd2, Qt-free
// Testing: UNIT: ScanRetryTest (a DID scan asks its unanswered entries
//          once more after its channel's round, request_engine_scan.h,
//          with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <cstdint>
#include <initializer_list>

namespace {

constexpr int64_t kMs = 1000;

/* A scan of dids on channel 0 (and those of channel 1, when given), the
 * scan before the rotation so each case's order is fixed, unless first is
 * false and the scan takes turns with the found DIDs' rotation */
struct Scan {
    ecuio_request_engine e;
    int64_t now = 0;
    uint32_t rotation_asked = 0;

    Scan(std::initializer_list<uint16_t> dids,
         std::initializer_list<uint16_t> second = {}, bool first = true)
    {
        EXPECT_TRUE(ecuio_engine_init(&e, second.size() > 0 ? 2 : 1));
        e.scan_first = first;
        for (uint16_t d : dids) {
            EXPECT_TRUE(ecuio_engine_scan_add(&e, 0, d));
        }
        for (uint16_t d : second) {
            EXPECT_TRUE(ecuio_engine_scan_add(&e, 1, d));
        }
    }

    /* The next request on channel c is the scan's 22 <did>; it goes out */
    void ask(uint32_t c, uint16_t did)
    {
        const int32_t r = ecuio_engine_due(&e, c, now);
        ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << "DID " << std::hex << did;
        const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
        ASSERT_EQ(q.len, 3u);
        EXPECT_EQ(q.bytes[1], static_cast<uint8_t>(did >> 8));
        EXPECT_EQ(q.bytes[2], static_cast<uint8_t>(did));
        ecuio_engine_sent(&e, c, r, true, now);
    }
    /* The scan's 22 <did> taken into the one-shot, its write failed */
    void unsent(uint32_t c, uint16_t did)
    {
        const int32_t r = ecuio_engine_due(&e, c, now);
        ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << "DID " << std::hex << did;
        const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
        ASSERT_EQ(q.bytes[2], static_cast<uint8_t>(did));
        ecuio_engine_sent(&e, c, r, false, now);
    }
    /* Asks channel c until the scan's 22 <did> goes out, each rotation
     * request on the way answered positive */
    void next(uint32_t c, uint16_t did)
    {
        for (int k = 0; k < 16; ++k) {
            const int32_t r = ecuio_engine_due(&e, c, now);
            if (r == ECUIO_ENGINE_NONE) {
                now += kMs;
                continue;
            }
            const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
            if (r == ECUIO_ENGINE_ONCE) {
                ASSERT_EQ(q.bytes[1], static_cast<uint8_t>(did >> 8));
                ASSERT_EQ(q.bytes[2], static_cast<uint8_t>(did));
                ecuio_engine_sent(&e, c, r, true, now);
                return;
            }
            ecuio_engine_sent(&e, c, r, true, now);
            ++rotation_asked;
            found(c, static_cast<uint16_t>((q.bytes[1] << 8) | q.bytes[2]));
        }
        FAIL() << "the scan's " << std::hex << did << " never went out";
    }
    /* 7F 22 78: the ECU works on it */
    void pending(uint32_t c)
    {
        const uint8_t reply[] = {0x7F, 0x22, 0x78};
        (void)ecuio_engine_on_frame(&e, c, reply, sizeof reply, now + 4 * kMs,
                                    nullptr);
        now += 50 * kMs;
    }
    void found(uint32_t c, uint16_t did)
    {
        const uint8_t reply[] = {0x62, static_cast<uint8_t>(did >> 8),
                                 static_cast<uint8_t>(did), 0x01};
        (void)ecuio_engine_on_frame(&e, c, reply, sizeof reply, now + 4 * kMs,
                                    nullptr);
        now += ECUIO_ENGINE_GAP_US;
    }
    void refused(uint32_t c)
    {
        const uint8_t reply[] = {0x7F, 0x22, 0x31};
        (void)ecuio_engine_on_frame(&e, c, reply, sizeof reply, now + 4 * kMs,
                                    nullptr);
        now += ECUIO_ENGINE_GAP_US;
    }
    void lost(uint32_t c)
    {
        now += ECUIO_ENGINE_WAIT_US;
        EXPECT_TRUE(ecuio_engine_expire(&e, c, now));
    }

    uint8_t outcome(uint32_t i) const { return e.scan[i].outcome; }
    ecuio_channel_support_t support(uint32_t c) const
    {
        ecuio_channel_support_t s;
        ecuio_engine_support(&e, c, &s);
        return s;
    }
    /* Whether channel c's next request is a scanned DID's */
    bool asks_scan(uint32_t c)
    {
        return ecuio_engine_due(&e, c, now + ECUIO_ENGINE_GAP_US) ==
               ECUIO_ENGINE_ONCE;
    }
};

} // namespace

/* A reply lost while the round asked: once the round has asked every
 * entry, the lost one is asked again, and its answer completes the scan */
TEST(ScanRetryTest, ALostReplyIsAskedOnceMoreAndCompletesTheScan)
{
    Scan s({0x1E20, 0x1E21, 0x1E22});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNANSWERED)
        << "not yet: 1E22 is unasked";
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E22));
    s.refused(0);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNASKED) << "the round is over";
    EXPECT_EQ(s.e.chans[0].scan_pending, 1u);
    EXPECT_EQ(s.support(0).flags & ECUIO_SUPPORT_SCAN_COMPLETE, 0u);

    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.found(0, 0x1E21);
    const ecuio_channel_support_t r = s.support(0);
    EXPECT_EQ(r.flags & ECUIO_SUPPORT_SCAN_COMPLETE,
              ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(r.scan_done, 3u);
    EXPECT_EQ(r.scan_found, 2u);
    EXPECT_EQ(s.e.count, 2u) << "both found DIDs polled";
}

/* Lost on the second ask too: unanswered stands, nothing more is asked
 * until a recovery's rescan, and the scan isn't complete */
TEST(ScanRetryTest, LostTwiceStandsAndNothingMoreIsAsked)
{
    Scan s({0x1E20, 0x1E21});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_EQ(s.outcome(1), ECUIO_SCAN_UNASKED);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNANSWERED);
    EXPECT_EQ(s.e.chans[0].scan_pending, 0u);
    for (int k = 0; k < 10; ++k) {
        EXPECT_FALSE(s.asks_scan(0)) << "pass " << k;
        s.now += ECUIO_ENGINE_GAP_US;
    }
    const ecuio_channel_support_t r = s.support(0);
    EXPECT_EQ(r.flags & ECUIO_SUPPORT_SCAN_COMPLETE, 0u);
    EXPECT_EQ(r.scan_done, 1u);
}

/* A module that answered no entry is silent, not lossy: its scan ends at
 * the round, with nothing asked again and no event left */
TEST(ScanRetryTest, AChannelThatAnsweredNothingIsNotAskedAgain)
{
    Scan s({0x1E20, 0x1E21});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    EXPECT_EQ(s.outcome(0), ECUIO_SCAN_UNANSWERED);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNANSWERED);
    EXPECT_EQ(s.e.chans[0].scan_pending, 0u);
    EXPECT_EQ(ecuio_engine_due(&s.e, 0, s.now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&s.e, s.now), INT64_MAX);
}

/* Each channel judges its own answers: one module answering doesn't make
 * another, silent one's entries go again, and the answering one's lost
 * entry goes again at its own round's end */
TEST(ScanRetryTest, EachChannelsRoundIsItsOwn)
{
    Scan s({0x1E20, 0x1E21}, {0xD100, 0xD101});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(1, 0xD100));
    s.found(1, 0xD100);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    EXPECT_EQ(s.e.chans[0].scan_pending, 0u) << "channel 0 answered nothing";
    EXPECT_EQ(s.outcome(0), ECUIO_SCAN_UNANSWERED);
    ASSERT_NO_FATAL_FAILURE(s.ask(1, 0xD101));
    s.lost(1);
    EXPECT_EQ(s.outcome(3), ECUIO_SCAN_UNASKED) << "channel 1 answered D100";
    EXPECT_EQ(s.e.chans[1].scan_pending, 1u);
    EXPECT_FALSE(s.asks_scan(0));
}

/* A recovery's rescan asks the unanswered again, and its round gets its
 * own one more ask */
TEST(ScanRetryTest, ARescanStartsARoundWithItsOwnRetry)
{
    Scan s({0x1E20, 0x1E21});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_EQ(s.outcome(1), ECUIO_SCAN_UNANSWERED);

    ecuio_engine_rescan(&s.e);
    EXPECT_EQ(s.e.scan[1].retried, 0u);
    EXPECT_EQ(s.e.chans[0].scan_pending, 1u);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNASKED) << "this round's one more ask";
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.found(0, 0x1E21);
    EXPECT_EQ(s.support(0).flags & ECUIO_SUPPORT_SCAN_COMPLETE,
              ECUIO_SUPPORT_SCAN_COMPLETE);
}

/* The first ask's late reply settles an entry waiting to be asked again:
 * found and polled, and it isn't asked */
TEST(ScanRetryTest, ALateReplySettlesAnEntryWaitingToBeAskedAgain)
{
    Scan s({0x1E20, 0x1E21});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_EQ(s.outcome(1), ECUIO_SCAN_UNASKED);
    ASSERT_EQ(s.e.chans[0].scan_pending, 1u);
    s.found(0, 0x1E21); /* the first ask's reply, after its wait */
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_FOUND);
    EXPECT_EQ(s.e.scan[1].polled, 1u);
    EXPECT_EQ(s.e.chans[0].scan_pending, 0u);
    EXPECT_FALSE(s.asks_scan(0));
    EXPECT_EQ(s.support(0).flags & ECUIO_SUPPORT_SCAN_COMPLETE,
              ECUIO_SUPPORT_SCAN_COMPLETE);
}

/* A retried entry the one-shot holds after its write failed is left to
 * its own ask: the first ask's late reply doesn't settle it, so it is
 * settled once, and the next entry waiting to be asked is still asked */
TEST(ScanRetryTest, AHeldEntryIsLeftToItsOwnAsk)
{
    Scan s({0x1E20, 0x1E21, 0x1E22});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E22));
    s.lost(0);
    ASSERT_EQ(s.e.chans[0].scan_pending, 2u);

    ASSERT_NO_FATAL_FAILURE(s.unsent(0, 0x1E21));
    s.found(0, 0x1E21); /* the first ask's reply, late, while it's held */
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNASKED);
    EXPECT_EQ(s.e.chans[0].scan_pending, 2u);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21)); /* sent again */
    s.found(0, 0x1E21);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_FOUND);
    EXPECT_EQ(s.e.chans[0].scan_pending, 1u);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E22));
    s.found(0, 0x1E22);
    const ecuio_channel_support_t r = s.support(0);
    EXPECT_EQ(r.flags & ECUIO_SUPPORT_SCAN_COMPLETE,
              ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(r.scan_found, 3u);
    EXPECT_EQ(s.e.count, 3u);
}

/* The first ask's late reply while the retry's ask is out answers that
 * ask: settled once, one rotation row, and the retry's own reply after it
 * changes nothing */
TEST(ScanRetryTest, ALateReplyWhileTheRetryIsOutSettlesOnce)
{
    Scan s({0x1E20, 0x1E21});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.found(0, 0x1E21); /* the first ask's reply, late */
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_FOUND);
    s.found(0, 0x1E21); /* the retry's own, after */
    const ecuio_channel_support_t r = s.support(0);
    EXPECT_EQ(r.scan_found, 2u);
    EXPECT_EQ(r.scan_polled, 2u);
    EXPECT_EQ(s.e.count, 2u);
}

/* The retry's ask settles as any ask: refused, or found after the ECU's
 * pending reply */
TEST(ScanRetryTest, TheRetrysAskSettlesOnARefusalOrAfterPending)
{
    Scan s({0x1E20, 0x1E21, 0x1E22});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E22));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.refused(0);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_REFUSED);
    EXPECT_EQ(s.e.scan[1].nrc, 0x31u);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E22));
    s.pending(0);
    s.found(0, 0x1E22);
    EXPECT_EQ(s.outcome(2), ECUIO_SCAN_FOUND);
    const ecuio_channel_support_t r = s.support(0);
    EXPECT_EQ(r.flags & ECUIO_SUPPORT_SCAN_COMPLETE,
              ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(r.scan_refused, 1u);
    EXPECT_EQ(r.scan_found, 2u);
}

/* Taking turns with the found DIDs' rotation (no scan first), the round's
 * end still asks its lost entry once more, between the rotation's turns */
TEST(ScanRetryTest, TheRetryTakesTurnsWithTheRotation)
{
    Scan s({0x1E20, 0x1E21, 0x1E22}, {}, false);
    ASSERT_NO_FATAL_FAILURE(s.next(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.next(0, 0x1E21));
    s.lost(0);
    ASSERT_NO_FATAL_FAILURE(s.next(0, 0x1E22));
    s.refused(0);
    EXPECT_EQ(s.outcome(1), ECUIO_SCAN_UNASKED);
    ASSERT_NO_FATAL_FAILURE(s.next(0, 0x1E21));
    s.found(0, 0x1E21);
    EXPECT_GT(s.rotation_asked, 0u) << "the scan took turns with the rotation";
    EXPECT_EQ(s.support(0).flags & ECUIO_SUPPORT_SCAN_COMPLETE,
              ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.e.count, 2u);
}

/* A late reply that settles the round's last waiting entry ends the round
 * as a settle would, and leaves nothing pending */
TEST(ScanRetryTest, ALateReplyEndingTheRoundLeavesNothingPending)
{
    Scan s({0x1E20, 0x1E21});
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E20));
    s.found(0, 0x1E20);
    ASSERT_NO_FATAL_FAILURE(s.ask(0, 0x1E21));
    s.lost(0);
    ASSERT_EQ(s.e.chans[0].scan_pending, 1u);
    s.found(0, 0x1E21); /* late: settles the waiting entry */
    EXPECT_EQ(s.e.chans[0].scan_pending, 0u);
    EXPECT_FALSE(s.asks_scan(0));
}
