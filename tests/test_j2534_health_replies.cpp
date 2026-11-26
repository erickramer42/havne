// File/layer: tests/test_j2534_health_replies.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534HealthTest (test_j2534_health); Cross-arch

#include "test_j2534_health_support.h"

#ifdef _WIN32

namespace {

/* One request, polled at the floor's pace */
const char kPollRpm[] = "?profile=raw&filter=0x7E8&poll=0x0C";

/* The one polled request's median reply latency (write to reply, the
 * host's clock, ADR-0042) once `want` replies are timed; 0 if they
 * never were */
uint32_t median_reply_latency_us(ecuio_session_t* session, uint64_t want)
{
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    uint64_t answered = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ecuio_session_get_timing(session, &t, rows.data(),
                                     static_cast<uint32_t>(rows.size()),
                                     &count) != ECUIO_OK) {
            return 0;
        }
        answered = 0;
        for (uint32_t i = 0; i < count; ++i) {
            answered += rows[i].answered;
        }
        if (answered >= want) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return (count == 1 && answered >= want) ? rows[0].latency_p50_us : 0;
}

/* A reply later than the 10 ms slice (15 ms, as the test car's took
 * through the VX): the first read ends with the TX indication alone,
 * and the read after it must wait at least 1 ms. A VX read asked to
 * wait 0 ms holds 50 ms, which put such a reply about 50 ms late */
TEST_F(J2534HealthTest, VxAReplyPastTheSliceIsNotHeldByAZeroTimeoutRead)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "15"));
    ASSERT_NO_FATAL_FAILURE(start_session(kPollRpm));
    const uint32_t p50 = median_reply_latency_us(session, 50);
    RecordProperty("reply_p50_us", static_cast<int>(p50));
    ASSERT_GT(p50, 0u) << "replies timed";
    EXPECT_GE(p50, 15000u - kWriteReturnUs) << "the fake's reply delay";
    EXPECT_LT(p50, 25000u) << "a read waited 0 ms and the VX held it";
}

/* A read asks for one message, so the VX's read ends when the reply
 * arrives, 4 ms after its request, instead of holding the 10 ms slice
 * for a batch, and answers with it. Measured (the median is a
 * test property): 5.1 ms through either surrogate; held to
 * the slice, 11.3 ms (x64) and 13.3 ms (x86); with one more call for the
 * rest of a batch, which the fake holds 10 ms as the VX did, failing.
 * The 10 ms bound sits between */
TEST_F(J2534HealthTest, VxAReplyEndsTheReadThatWaitsForIt)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session(kPollRpm));
    const uint32_t p50 = median_reply_latency_us(session, 50);
    RecordProperty("reply_p50_us", static_cast<int>(p50));
    ASSERT_GT(p50, 0u) << "replies timed";
    EXPECT_GE(p50, 4000u - kWriteReturnUs) << "the fake's reply delay";
    EXPECT_LT(p50, 10000u) << "the read held its slice past the reply";
}

/* Each reply 94 ms after its request, inside the 100 ms wait: the fake
 * answers an empty read at once, so the last read before the deadline
 * comes about 90 ms in, before the reply. The pass reads what the
 * adapter holds before it ends the wait, so no request times out */
TEST_F(J2534HealthTest, AReplyInTheAdapterWhenItsWaitRunsOutAnswersIt)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "94"));
    ASSERT_NO_FATAL_FAILURE(start_session(kPollRpm));
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        ASSERT_EQ(ecuio_session_get_timing(session, &t, rows.data(),
                                           static_cast<uint32_t>(rows.size()),
                                           &count),
                  ECUIO_OK);
        if (count == 1 && rows[0].answered + rows[0].timeouts >= 8) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_EQ(count, 1u);
    EXPECT_GE(rows[0].answered, 8u);
    EXPECT_EQ(rows[0].timeouts, 0u) << "a reply in the adapter was lost";
}

/* PID 0x5F is one the fake's ECU doesn't support, and it answers
 * 7F 01 12, as the first vehicle did for 0x5C. The negative reply ends
 * the request like a positive one, so requests keep the floor's pace
 * instead of waiting out 100 ms each, and the ECU answering keeps the
 * link ok. */
TEST_F(J2534HealthTest, NegativeReplyCompletesItsRequest)
{
    ASSERT_NO_FATAL_FAILURE(
        start_session("?profile=raw&filter=0x7E8&poll=0x5F"));
    ASSERT_TRUE(wait_for_frames(3, 2000)) << "replies didn't arrive";

    EXPECT_GE(writes_during(page, 1000), 20u)
        << "each request waited out its wait";
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_OK);
    EXPECT_EQ(ec, 0u);

    ASSERT_NO_FATAL_FAILURE(close_session());
    ASSERT_EQ(delivered.lengths[0], 3u);
    const uint8_t want[3] = {0x7F, 0x01, 0x12};
    EXPECT_EQ(memcmp(delivered.payloads[0], want, sizeof(want)), 0);
}

/* Replies 400 ms late: each request waits its 100 ms and times out, so
 * about ten go out a second, and each late reply is still delivered,
 * which keeps the link ok. Waiting for the replies instead would send
 * about two and a half. */
TEST_F(J2534HealthTest, UnansweredRequestWaitsOutItsWait)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "400"));
    ASSERT_NO_FATAL_FAILURE(start_session(""));
    ASSERT_TRUE(wait_for_frames(3, 3000)) << "late replies weren't delivered";

    const uint32_t n = writes_during(page, 1000);
    EXPECT_GE(n, 6u) << "requests waited for their late replies";
    EXPECT_LE(n, 12u) << "requests didn't wait their 100 ms";
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_OK);
}

/* A scanned DID whose long reply comes after its wait (the test car's
 * 263-byte DA9E through the VX came 215 ms after the reply before it)
 * is still found: the
 * late reply answers it, it's polled, and the scan completes. F19E and
 * F19F are refused at once; F1A0's 200 bytes come 200 ms late */
TEST_F(J2534HealthTest, AScannedDidsLateLongReplyIsFound)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LONG_REPLY_DELAY_MS", "200"));
    ASSERT_NO_FATAL_FAILURE(start_session(
        "?profile=uds-generic&did=F19E-F1A0&identify=0&diagnose=0"));
    ecuio_channel_support_t s{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           !(ecuio_session_get_channel_support(session, 0, &s) == ECUIO_OK &&
             (s.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_SCAN_COMPLETE)
        << "the scan didn't end (done " << s.scan_done << ")";
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 2u);
    EXPECT_EQ(s.scan_polled, 1u);
}

/* The fake's ECU answers each request "response pending" (7F 01 78) at
 * once and the reply 300 ms later. Pending extends the request's wait,
 * so the next request follows the reply, about one every 300 ms, and
 * each reply arrives while its own request is the latest. Ending the
 * wait at 100 ms instead would send three times as many, each reply
 * landing after the next request. */
TEST_F(J2534HealthTest, PendingReplyHoldsTheRequestForItsAnswer)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_PENDING", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "300"));
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8&poll=0x0C"));
    ASSERT_TRUE(wait_for_frames(2, 3000)) << "nothing arrived";

    const uint32_t n = writes_during(page, 1500);
    EXPECT_GE(n, 2u) << "requests stopped";
    EXPECT_LE(n, 7u) << "pending didn't extend the wait";
    ASSERT_NO_FATAL_FAILURE(close_session());

    const int got = (delivered.count.load() < 16) ? delivered.count.load() : 16;
    uint32_t replies = 0;
    for (int i = 0; i < got; ++i) {
        if (delivered.payloads[i][0] == 0x7F) {
            EXPECT_EQ(delivered.payloads[i][2], 0x78u) << "frame " << i;
            continue;
        }
        ASSERT_EQ(delivered.payloads[i][0], 0x41u) << "frame " << i;
        ++replies;
        EXPECT_EQ(delivered.writes_at[i], replies)
            << "reply " << replies << " came after the next request";
    }
    EXPECT_GE(replies, 2u);
}

constexpr char kExtendedSession[] =
    "?profile=uds-generic&did=1E1C&session=03&identify=0&diagnose=0";

/* ADR-0060's close: 10 01's answer, 14 ms after it, is read before the
 * filter stops (the session dispatches nothing once its close begins, so
 * the fake counts what it still held then), so the module's reply finds
 * the channel there and is sent once
 * (docs/bench-sessions/2026-11-11-ext-session-car.md: after the
 * disconnect it came again for 13.5 s) */
TEST_F(J2534HealthTest, TheClosesAnswerIsReadBeforeTheFilterStops)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "14"));
    ASSERT_NO_FATAL_FAILURE(start_session(kExtendedSession));
    ASSERT_TRUE(wait_for_frames(3, 3000)) << "nothing arrived";
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_NE(fake_control::last_stopped(page), 0u) << "no filter stopped";
    EXPECT_EQ(fake_control::unread_at_stop(page), 0u)
        << "a reply was left unread when the filter stopped";
}

/* The fake's steady clock (its write stamps' microseconds, low 32 bits),
 * which this process shares */
uint32_t fake_now_us()
{
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/* Waits for the fake's next write after `after` writes, up to 3 s */
bool wait_for_write(fake_control::Page* page, uint32_t after)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (fake_control::writes(page) <= after) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/* A request out when the close comes is answered before 10 01 goes: the
 * close follows a write at once, its reply comes 80 ms after it, so 10 01
 * follows that write by 80 ms at least */
TEST_F(J2534HealthTest, TheCloseWaitsForTheRequestInFlight)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "80"));
    ASSERT_NO_FATAL_FAILURE(start_session(kExtendedSession));
    ASSERT_TRUE(wait_for_frames(2, 3000)) << "nothing arrived";
    ASSERT_TRUE(wait_for_write(page, fake_control::writes(page)));
    const uint32_t closed_at = fake_now_us();
    ASSERT_NO_FATAL_FAILURE(close_session());

    const uint32_t n = fake_control::writes(page);
    ASSERT_GE(n, 3u);
    ASSERT_LE(n, fake_control::kWriteDepth) << "the page keeps the first 16";
    fake_control::Write last{}, before{};
    ASSERT_TRUE(fake_control::recorded_write(page, n - 1, &last));
    ASSERT_TRUE(fake_control::recorded_write(page, n - 2, &before));
    const uint8_t leave[6] = {0x00, 0x00, 0x07, 0xE0, 0x10, 0x01};
    ASSERT_EQ(last.size, sizeof(leave));
    EXPECT_EQ(memcmp(last.data, leave, sizeof(leave)), 0) << "the last write";
    ASSERT_LT(closed_at - before.us, 80000u)
        << "the close came after the request's reply: nothing was in flight";
    EXPECT_GE(last.us - before.us, 80000u)
        << "10 01 went out before the reply to the request in flight";
}

/* A module that answers 7F 22 78 and then nothing for 3 s holds the close
 * no longer than its one bound (J2534_CLOSE_US, 200 ms), however the 78
 * extends the request's own wait, and 10 01 still goes out */
TEST_F(J2534HealthTest, APendingAnswerHoldsTheCloseNoLongerThanItsBound)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_PENDING", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "3000"));
    ASSERT_NO_FATAL_FAILURE(start_session(kExtendedSession));
    /* 10 03, its 50 03 after 3 s, then the read whose 78 comes at once */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (fake_control::writes(page) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(fake_control::writes(page), 2u) << "the read never went out";
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_FATAL_FAILURE(close_session());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 1500) << "a 78 held the close past its bound";
    ASSERT_EQ(fake_control::writes(page), 3u);
    fake_control::Write last{};
    ASSERT_TRUE(fake_control::recorded_write(page, 2, &last));
    const uint8_t leave[6] = {0x00, 0x00, 0x07, 0xE0, 0x10, 0x01};
    ASSERT_EQ(last.size, sizeof(leave));
    EXPECT_EQ(memcmp(last.data, leave, sizeof(leave)), 0) << "10 01";
}

/* An ECU that stops answering (each reply 2 s late) holds the close no
 * longer than the request in flight's wait and 10 01's, 100 ms each */
TEST_F(J2534HealthTest, ACloseWithNoAnswerEndsWithinItsWaits)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kExtendedSession));
    ASSERT_TRUE(wait_for_frames(3, 3000)) << "nothing arrived";
    const uint32_t before = fake_control::writes(page);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "2000"));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (fake_control::writes(page) < before + 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(fake_control::writes(page), before + 2) << "no write went out";
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_FATAL_FAILURE(close_session());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 1000) << "the close waited for a reply past its waits";
}

/* A dead adapter (every read STATUS_FAILED) still closes at once */
TEST_F(J2534HealthTest, ACloseOnADeadLinkEndsPromptly)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kExtendedSession));
    ASSERT_TRUE(wait_for_frames(3, 3000)) << "nothing arrived";
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE, 5000,
                               le, ec));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_FATAL_FAILURE(close_session());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 1500) << "a dead link held the close";
}

/* Two ECUs, each on its channel, answer 60 ms after a request. Each
 * channel waits only for its own reply, so both requests are out before
 * either reply arrives, and both ECUs' replies come back. Test-only
 * profile, static storage (the registry keeps the pointer). */
TEST_F(J2534HealthTest, ChannelsWaitOnlyForTheirOwnReplies)
{
    static const ecuio_channel_decl_t kChannels[] = {{0x7E8u, 0x7E0u, 0, 0},
                                                     {0x7E9u, 0x7E1u, 0, 0}};
    static ecuio_signal_def_t defs[2] = {};
    for (int i = 0; i < 2; ++i) {
        defs[i].id = static_cast<uint16_t>(1 + i);
        defs[i].kind = ECUIO_VALUE_UINT;
        defs[i].start_byte = 2;
        defs[i].bit_length = 8;
        defs[i].scale = 1.0;
        defs[i].message_id = kChannels[i].rx_id;
        defs[i].mux_length = 2;
        std::snprintf(defs[i].name, sizeof defs[i].name, "Signal %d", i);
    }
    defs[0].mux_key = 0x410Cu;
    defs[1].mux_key = 0x410Du;
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    set.channel_count = 2;
    set.channels = kChannels;
    ASSERT_EQ(ecuio_profile_register("test-two-ecus", &set, nullptr), ECUIO_OK);

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "60"));
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=test-two-ecus"));
    ASSERT_TRUE(wait_for_frames(4, 3000)) << "replies didn't arrive";
    ASSERT_NO_FATAL_FAILURE(close_session());

    EXPECT_EQ(delivered.writes_at[0], 2u)
        << "the second channel waited for the first channel's reply";
    bool from[2] = {};
    for (int i = 0; i < 4; ++i) {
        from[0] = from[0] || delivered.ids[i] == 0x7E8u;
        from[1] = from[1] || delivered.ids[i] == 0x7E9u;
    }
    EXPECT_TRUE(from[0]) << "no reply from 0x7E8";
    EXPECT_TRUE(from[1]) << "no reply from 0x7E9";
}

/* Passive mode reads again at once after a frame, so a burst of 16 is
 * delivered together rather than one per pass with a wait between. One
 * idle wait per frame would take 16 x 100 ms, so the ceiling is half of
 * that: loose for a loaded machine, still under the wait's total. */
TEST_F(J2534HealthTest, PassiveModeDrainsABurstWithoutWaiting)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));

    const auto t0 = std::chrono::steady_clock::now();
    for (uint8_t i = 0; i < 16; ++i) {
        const uint8_t msg[6] = {0x00, 0x00, 0x07, 0xE8, 0x41, i};
        ASSERT_TRUE(fake_control::post(page, msg, sizeof(msg), 1000u + i, 0))
            << "message " << int{i};
    }
    ASSERT_TRUE(wait_for_frames(16, 3000));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(took, 800) << "the burst was read one frame per pass";
    EXPECT_EQ(frames.load(), 16);
}

} // namespace

#endif // _WIN32
