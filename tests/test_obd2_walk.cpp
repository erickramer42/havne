// File/layer: tests/test_obd2_walk.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

/* Before its rotation a channel walks the supported-PID chain, a bitmap
 * at a time, following each one's last bit, and drops the PIDs the
 * answers mark unsupported. The first bitmap is the worked example on
 * Wikipedia's "OBD-II PIDs" page (Service 01 PID 00): BE 1F A8 13 lists
 * PIDs 01, 03 to 07, 0C to 11, 13, 15, 1C, 1F and 20, so 0x0B is clear.
 * The 0x40 bitmap's last bit is clear, so the chain ends there, and 0x61,
 * past it, is unsupported too. */
TEST(RequestEngineTest, ProbeWalksTheBitmapsBeforeTheRotation) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C, 0x42, 0x61}));
    ecuio_engine_probe(&e);

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA813u, now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(ecuio_engine_due(&e, 0, now + 4 * kMs), ECUIO_ENGINE_NONE)
        << "the walk keeps the floor";
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x20, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x20, 0x00000001u, now + 4 * kMs),
              ecuio_reply::positive);   /* only the next bitmap */
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x40, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x40, 0x40000000u, now + 4 * kMs),
              ecuio_reply::positive);   /* 0x42, and the chain ends */

    uint8_t sent[3] = {};
    for (uint8_t& pid : sent) {
        now += ECUIO_ENGINE_WAIT_US;
        pid = send_next(&e, 0, now);
        EXPECT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));
    }
    const uint8_t want[3] = {0x0C, 0x42, 0x0C};
    EXPECT_EQ(memcmp(sent, want, sizeof(want)), 0) << "0x0B and 0x61 left";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.ranges, 0x7u);
    const uint8_t listed[] = {0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C,
                              0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x15,
                              0x1C, 0x1F, 0x20};
    for (unsigned pid = 1; pid <= 0x20; ++pid) {
        bool want_set = false;
        for (uint8_t l : listed) {
            want_set = want_set || l == pid;
        }
        EXPECT_EQ(ecuio_engine_pid_bit(s.pid_bitmap, static_cast<uint8_t>(pid)),
                  want_set) << "PID " << std::hex << pid;
    }
    EXPECT_EQ(s.requests, 4u);
    ASSERT_EQ(s.dropped, 2u);
    EXPECT_EQ(s.dropped_list[0].request[1], 0x0Bu);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_BITMAP);
    EXPECT_EQ(s.dropped_list[0].nrc, 0u);
    EXPECT_EQ(s.dropped_list[1].request[0], 0x01u);
    EXPECT_EQ(s.dropped_list[1].request[1], 0x61u);
    EXPECT_EQ(s.dropped_list[1].length, 2u);
}

/* The report's list takes the refused drops before the bitmap's: a car's
 * bitmaps leave out many PIDs, and their drops would otherwise fill the
 * list before a refusal the front ends name. Ten bitmap drops, then a
 * refused DID, then one more refused: both refusals listed first, in set
 * order, then the bitmap drops in theirs; every drop counted. */
TEST(RequestEngineTest, TheReportListsRefusalsBeforeBitmapDrops)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(
        &e, {0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0A, 0x0B, 0x0E, 0x0F}));
    const uint8_t did[3] = {0x22, 0xF1, 0xFE};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    const uint8_t pid[2] = {0x01, 0x11};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(e.count, 12u);
    for (uint32_t i = 0; i < 10; ++i) {
        ecuio_engine_drop(&e, i, ECUIO_DROP_BITMAP, 0);
    }
    ecuio_engine_drop(&e, 10, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    ecuio_engine_drop(&e, 11, ECUIO_DROP_ACCESS, 0x33);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.requests, 12u);
    EXPECT_EQ(s.dropped, 12u);
    EXPECT_EQ(s.dropped_list[0].length, 3u);
    EXPECT_EQ(s.dropped_list[0].request[1], 0xF1u);
    EXPECT_EQ(s.dropped_list[0].request[2], 0xFEu);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_NOT_SUPPORTED);
    EXPECT_EQ(s.dropped_list[0].nrc, 0x31u);
    EXPECT_EQ(s.dropped_list[1].request[1], 0x11u);
    EXPECT_EQ(s.dropped_list[1].reason, ECUIO_DROP_ACCESS);
    const uint8_t bitmap[6] = {0x01, 0x03, 0x04, 0x05, 0x06, 0x07};
    for (uint32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(s.dropped_list[2 + i].request[1], bitmap[i]) << i;
        EXPECT_EQ(s.dropped_list[2 + i].reason, ECUIO_DROP_BITMAP) << i;
    }
}

/* The first vehicle's own bitmaps (docs/bench-sessions/2026-10-22-vehicle.md),
 * read off the bytes by J1979's layout there: of the obd2 set's first
 * ten, every PID is supported but 0x5C, so the walk drops only that one
 * of them, and of the rest exactly those the bitmaps leave out. The 0x40
 * bitmap's last bit says a fourth exists; it's left unanswered here, so
 * PIDs past 0x60 stay. */
TEST(RequestEngineTest, TheFirstVehiclesBitmapsDropOnly0x5C) {
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1,
                                    nullptr));
    ecuio_engine_probe(&e);
    const struct {
        uint8_t pid;
        uint32_t bits;
    } kCar[] = {{0x00, 0xFE7FA813u}, {0x20, 0xA017F011u}, {0x40, 0xFED08C81u}};
    int64_t now = 0;
    for (const auto& b : kCar) {
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, b.pid, now));
        ASSERT_EQ(answer_bitmap(&e, 0, b.pid, b.bits, now + 4 * kMs),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
    }
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x60, now));
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));

    /* Dropped: a PID its answered bitmap marks clear (bit 0x80 >>
     * ((k - 1) % 8) of byte (k - 1) / 8 for PID base + k). The PIDs past
     * 0x60, whose bitmap went unanswered, stay; of the ten, only 0x5C
     * goes, as on the car. */
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const uint8_t pid = e.requests[i].bytes[1];
        bool clear = false;
        for (const auto& b : kCar) {
            const unsigned k = pid - b.pid;
            if (pid > b.pid && k <= 32) {
                clear = ((b.bits >> (32 - k)) & 1u) == 0;
            }
        }
        EXPECT_EQ(e.requests[i].dropped, clear ? ECUIO_DROP_BITMAP : 0u)
            << "PID " << std::hex << int{pid};
        if (i < 10) {
            EXPECT_EQ(clear, pid == 0x5C) << "PID " << std::hex << int{pid};
        }
        dropped += clear ? 1u : 0u;
    }
    EXPECT_GT(dropped, 1u);
    EXPECT_EQ(e.chans[0].requests, e.count - dropped);
}

/* ADR-0039 section 3: the fuel rail's two forms read one value on the test car
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md), so when a car's
 * bitmaps list both, 59 goes slow and 23 stays fast. A car that lists
 * only 59 keeps it fast, and with no walk nothing is judged. */
TEST(RequestEngineTest, ADuplicateFuelRailPressureGoesSlow)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar));
    EXPECT_EQ(class_of(e, 0x23), 0);
    EXPECT_EQ(class_of(e, 0x59), 1) << "both listed: the duplicate is slow";
    EXPECT_EQ(class_of(e, 0x0A), 0) << "the low-pressure side stays fast";

    std::vector<std::pair<uint8_t, uint32_t>> no23 = kTestCar;
    no23[1].second &= ~(0x80000000u >> (0x23 - 0x21));
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, no23));
    EXPECT_EQ(class_of(e, 0x23), -1) << "dropped on its bitmap";
    EXPECT_EQ(class_of(e, 0x59), 0) << "the only rail pressure stays fast";

    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar, false));
    EXPECT_EQ(class_of(e, 0x23), 0);
    EXPECT_EQ(class_of(e, 0x59), 0) << "no walk, nothing judged";
}

/* A bitmap nobody answers ends the walk with nothing judged: every
 * request stays, and the answer coming late changes nothing. */
TEST(RequestEngineTest, SilenceEndsTheWalkAndDropsNothing) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_WAIT_US), 0x0Bu);
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA813u,
                            ECUIO_ENGINE_WAIT_US + 4 * kMs),
              ecuio_reply::none) << "a late bitmap answers nothing";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
    EXPECT_EQ(s.ranges, 0u);
    EXPECT_EQ(s.dropped, 0u);
    EXPECT_GT(s.generation, 0u) << "the walk's end is news";
    EXPECT_EQ(e.chans[0].requests, 2u);
}

/* A stray single frame answers the first bitmap's ask and its wait runs
 * out: the retry is due, so the support report says nothing was asked
 * yet (a front end reads PROBED with no range as "no answer"). Answered
 * on the retry, it reports the chain. */
TEST(RequestEngineTest, ARetryDueIsNotAMissedSupportQuery) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    const uint8_t stray[3] = {0x41, 0x05, 0x46};
    (void)ecuio_engine_on_frame(&e, 0, stray, sizeof stray, 4 * kMs, nullptr);
    ASSERT_TRUE(e.chans[0].walk_stray);
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    ASSERT_TRUE(e.chans[0].walking) << "the retry is due";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags & ECUIO_SUPPORT_PROBED, 0u);

    const int64_t now = ECUIO_ENGINE_WAIT_US + ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00100000u, now + 4 * kMs),
              ecuio_reply::positive);
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.ranges, 0x1u);
}

/* The retry is used up when a stray comes again: the walk gives up and
 * the report says the query was asked and not answered. */
TEST(RequestEngineTest, AWalkThatGivesUpAfterItsRetryIsReportedUnanswered) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    const uint8_t stray[3] = {0x41, 0x05, 0x46};
    (void)ecuio_engine_on_frame(&e, 0, stray, sizeof stray, 4 * kMs, nullptr);
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    int64_t now = ECUIO_ENGINE_WAIT_US + ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    (void)ecuio_engine_on_frame(&e, 0, stray, sizeof stray, now + 4 * kMs,
                                nullptr);
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));
    ASSERT_FALSE(e.chans[0].walking);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
    EXPECT_EQ(s.ranges, 0u);
}

/* A bitmap whose wait ends after a reply naming another PID is asked
 * once more: on the car, 01 00 drew 41 1F 00 01, the run time's answer
 * (docs/bench-sessions/2026-10-29-android.md). Asked again, it's
 * answered and the walk goes on; another PID's answer the second time
 * ends the walk as silence does. */
TEST(RequestEngineTest, AnotherPidsAnswerAsksTheBitmapAgainOnce)
{
    const uint8_t run_time[4] = {0x41, 0x1F, 0x00, 0x01};
    {
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
        ecuio_engine_probe(&e);
        int64_t now = 0;
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
        EXPECT_EQ(ecuio_engine_on_frame(&e, 0, run_time, sizeof(run_time),
                                        now + 4 * kMs, nullptr),
                  ecuio_reply::none);
        now += ECUIO_ENGINE_WAIT_US;
        EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now))
            << "the same bitmap again";
        EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA812u, now + 4 * kMs),
                  ecuio_reply::positive);
        EXPECT_EQ(send_next(&e, 0, now + ECUIO_ENGINE_WAIT_US), 0x0Cu)
            << "the walk ended with the chain, and dropped 0x0B";
        ecuio_channel_support_t s;
        ecuio_engine_support(&e, 0, &s);
        EXPECT_EQ(s.ranges, 0x1u);
    }
    {
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
        ecuio_engine_probe(&e);
        int64_t now = 0;
        for (int ask = 0; ask < 2; ++ask) {
            ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
            (void)ecuio_engine_on_frame(&e, 0, run_time, sizeof(run_time),
                                        now + 4 * kMs, nullptr);
            now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        }
        EXPECT_EQ(send_next(&e, 0, now), 0x0Bu) << "the walk ended, once";
        ecuio_channel_support_t s;
        ecuio_engine_support(&e, 0, &s);
        EXPECT_EQ(s.ranges, 0u);
        EXPECT_EQ(s.dropped, 0u);
    }
}

/* With the probe on, a negative reply drops its request when its code
 * can't change within the session: 0x12 and 0x31 (not supported), 0x33
 * and 0x7F (security access, another session: the engine asks for
 * neither). Any other code leaves it in the rotation: 0x22 (conditions
 * not correct), 0x21 (busy), 0x10, 0x13, 0x14. */
TEST(RequestEngineTest, NegativeRepliesDropOnAnswersThatLast) {
    const struct {
        uint8_t nrc;
        uint8_t reason;
    } kCases[] = {{0x12, ECUIO_DROP_NOT_SUPPORTED},
                  {0x31, ECUIO_DROP_NOT_SUPPORTED},
                  {0x33, ECUIO_DROP_ACCESS},
                  {0x7F, ECUIO_DROP_ACCESS},
                  {0x22, 0}, {0x21, 0}, {0x10, 0}, {0x13, 0}, {0x14, 0}};
    for (const auto& k : kCases) {
        ecuio_request_engine e;
        ecuio_engine_init(&e, 1);
        const uint8_t unknown[3] = {0x22, 0x1E, 0x1D};
        const uint8_t known[3] = {0x22, 0x1E, 0x1C};
        ASSERT_EQ(ecuio_engine_add(&e, 0, unknown, 3),
                  ecuio_engine_add_result::added);
        ASSERT_EQ(ecuio_engine_add(&e, 0, known, 3),
                  ecuio_engine_add_result::added);
        ecuio_engine_probe(&e);   /* no Service 01, so no walk */
        ASSERT_EQ(ecuio_engine_due(&e, 0, 0), 0) << std::hex << int{k.nrc};
        ecuio_engine_sent(&e, 0, 0, true, 0);
        EXPECT_EQ(answer_negative(&e, 0, 0x22, k.nrc, 4 * kMs),
                  ecuio_reply::negative);
        EXPECT_EQ(e.requests[0].dropped, k.reason) << std::hex << int{k.nrc};
        EXPECT_EQ(e.requests[0].nrc, (k.reason != 0) ? k.nrc : 0)
            << std::hex << int{k.nrc};
        EXPECT_EQ(e.requests[1].dropped, 0u);
        EXPECT_EQ(e.chans[0].requests, (k.reason != 0) ? 1u : 2u);
    }
}

/* 0x11 says the ECU has no such service: every request of that service
 * on that channel leaves; its other services, and the other channel's
 * requests, stay. */
TEST(RequestEngineTest, ServiceNotSupportedDropsTheServiceOnItsChannel) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t a[3] = {0x22, 0x1E, 0x1C}, b[3] = {0x22, 0x1E, 0x1D},
                  vin[2] = {0x09, 0x02};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, b, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, vin, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, a, 3), ecuio_engine_add_result::added);
    ecuio_engine_probe(&e);
    ASSERT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x11, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(e.requests[1].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(e.requests[1].nrc, 0x11u);
    EXPECT_EQ(e.requests[2].dropped, 0u) << "Service 09 stays";
    EXPECT_EQ(e.requests[3].dropped, 0u) << "the other ECU's stays";
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_EQ(e.chans[1].requests, 1u);
}

/* With the probe off (?probe=0, and ?poll=), no channel walks and every
 * request stays, whatever the ECU answers. */
TEST(RequestEngineTest, WithoutTheProbeNothingIsDropped) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    EXPECT_EQ(send_next(&e, 0, 0), 0x0Bu) << "no walk first";
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x0Cu);
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x11, ECUIO_ENGINE_GAP_US + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[0].dropped, 0u);
    EXPECT_EQ(e.requests[1].dropped, 0u);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(e.chans[0].generation, 0u);
}

/* A negative reply names the service, not the PID, so one that comes
 * after its request timed out would answer the next request: it drops
 * nothing until a positive reply shows the ECU answers the request
 * that's out. Here 0x0B's refusal comes 100 ms late, while 0x0C is
 * out. */
TEST(RequestEngineTest, ALateNegativeReplyDropsNothing) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    /* PIDs 01 to 1F supported, and the chain ends */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0xFFFFFFFEu, 4 * kMs),
              ecuio_reply::positive);

    int64_t now = ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(send_next(&e, 0, now), 0x0Bu);
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_EQ(send_next(&e, 0, now), 0x0Cu);
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[1].dropped, 0u) << "0x0B's answer dropped 0x0C";

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(send_next(&e, 0, now), 0x0Bu);
    const uint8_t reply[3] = {0x41, 0x0B, 0x2A};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply, 3, now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(send_next(&e, 0, now), 0x0Cu);
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[1].dropped, ECUIO_DROP_NOT_SUPPORTED)
        << "back in step, the refusal is 0x0C's";
}

/* A walk that stopped short resumes, at the driver's next probe (on a
 * recovery), at the first bitmap it hasn't had; a channel whose chain
 * was answered to its end isn't walked again. */
TEST(RequestEngineTest, TheWalkResumesWhereItStopped) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x2F}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    /* 0x0C (bit 0x10 of the second byte), and the next bitmap */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00100001u, 4 * kMs),
              ecuio_reply::positive);
    int64_t now = ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x20, now));
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.ranges, 0x1u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);

    ASSERT_EQ(send_next(&e, 0, now), 0x0Cu);
    const uint8_t reply[3] = {0x41, 0x0C, 0x1F};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply, 3, now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    ecuio_engine_probe(&e);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x20, now));
    /* 0x2F (bit 0x02 of the second byte), and the chain ends */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x20, 0x00020000u, now + 4 * kMs),
              ecuio_reply::positive);
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.ranges, 0x3u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.dropped, 0u);

    ecuio_engine_probe(&e);
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_NE(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE)
        << "an answered chain was walked again";
}

/* No bitmap is asked after the budget: a walk that pending replies hold
 * past it ends there, the answers it had applied, and the PIDs it didn't
 * reach stay. */
TEST(RequestEngineTest, TheBudgetEndsTheWalk) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C, 0x2E}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(answer_negative(&e, 0, 0x01, ECUIO_NRC_RESPONSE_PENDING, 10 * kMs),
              ecuio_reply::pending);
    const int64_t late = ECUIO_ENGINE_PROBE_BUDGET_US - 10 * kMs;
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA813u, late),
              ecuio_reply::positive);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_PROBE_BUDGET_US), 0x0Cu)
        << "the walk asked past its budget";
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_BITMAP) << "0x0B's bit is clear";
    EXPECT_EQ(e.requests[2].dropped, 0u) << "0x2E wasn't reached";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.ranges, 0x1u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
}

/* A refused bitmap ends the walk. 0x12 there says nothing of the PIDs,
 * which stay; 0x11 says the ECU has no Service 01, so every PID leaves.
 * A positive reply too short to be a bitmap ends it unjudged. */
TEST(RequestEngineTest, ARefusedBitmapEndsTheWalk) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x0Cu);
    EXPECT_EQ(e.requests[0].dropped, 0u);

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x11, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(e.requests[1].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x1Eu) << "the DID stays";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B}));
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    const uint8_t short_bitmap[3] = {0x41, 0x00, 0xBE};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, short_bitmap, 3, 4 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x0Bu);
    EXPECT_EQ(e.chans[0].ranges, 0u);
}

/* Only a channel with Service 01 requests walks; the other's rotation
 * starts at once. A channel whose every request was dropped sends
 * nothing more and has no next event. */
TEST(RequestEngineTest, OnlyChannelsAskingForPidsWalk) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t pid[2] = {0x01, 0x0B}, did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(send_next(&e, 1, 0), 0x1Eu);

    /* Nothing supported, and the chain ends: 0x0B leaves. */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00000000u, 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].requests, 0u);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, ECUIO_ENGINE_GAP_US),
              ECUIO_ENGINE_WAIT_US - ECUIO_ENGINE_GAP_US)
        << "only channel 1's wait";
}
