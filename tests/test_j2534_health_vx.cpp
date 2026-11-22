// File/layer: tests/test_j2534_health_vx.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534HealthTest (test_j2534_health); Cross-arch

#include "test_j2534_health_support.h"

#ifdef _WIN32

namespace {

/* ====================================================================
 * The fake's VX mode (ECUIO_FAKE_VX): reads held for their timeout, a
 * short batch with STATUS_NOERROR, an empty read with ERR_BUFFER_EMPTY,
 * a TX indication after each write and the reply 4 ms later, as the
 * OBDX Pro VX measured (docs/bench-sessions/2026-10-22.md and the
 * vehicle sheet), and a read asked to wait 0 ms held 50 ms
 * (docs/bench-sessions/2026-11-04-two-ecu-car.md).
 * ==================================================================== */

/* A quiet bus on the VX: every passive read waits out its 100 ms and
 * comes back ERR_BUFFER_EMPTY, which is "no frame", so the debounce
 * takes ten of them. The lower bound pins the hold: reads that
 * returned at once would reach quiet in about 200 ms (ten 20 ms
 * sleeps). The bench saw 1.42 s. */
TEST_F(J2534HealthTest, VxQuietBusDebouncesOnBufferEmptyReads)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));

    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_VEHICLE_TIMEOUT, 8000,
                               le, ec))
        << "quiet debounce never fired (was " << le << ")";
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(took, 950) << "ten empty reads didn't each hold 100 ms";
    EXPECT_EQ(ec, 0u);
    EXPECT_EQ(frames.load(), 0);
}

/* Requests on the VX: each write's indication is dropped, its reply
 * decodes, and the link is ok with no episode. */
TEST_F(J2534HealthTest, VxAnsweredRequestsKeepTheLinkOk)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (frames.load() < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(frames.load(), 5) << "replies didn't arrive";
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_OK);
    EXPECT_EQ(ec, 0u);
    /* Every frame is a reply: no indication reached the session. */
    EXPECT_LE(static_cast<uint32_t>(frames.load()), fake_control::writes(page));
}

/* The VX on a bus with no other node: three writes accepted, each with
 * an indication and no reply, then every write blocks about 500 ms and
 * fails. One "requests failing" episode, no frames, and no further
 * write accepted. Closing waits for at most the write in flight. */
TEST_F(J2534HealthTest, VxNoAckBusAcceptsThreeWritesThenRequestsFail)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_NO_ACK", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_REQUEST_FAILED, 5000,
                               le, ec))
        << "requests never failed (was " << le << ")";
    EXPECT_EQ(ec, 1u);
    EXPECT_EQ(fake_control::writes(page), 3u);

    /* Refused writes keep failing; none is accepted, nothing arrives. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(ec, 1u);
    EXPECT_EQ(fake_control::writes(page), 3u);
    EXPECT_EQ(frames.load(), 0);

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(took, 2000) << "close waited past the blocked write";
    ecuio_session_destroy(session);
    session = nullptr;
}

/* ====================================================================
 * Indications: TX done and START_OF_MESSAGE are skipped, and skipping
 * one doesn't end the read.
 * ==================================================================== */

/* On the VX each write's indication is queued ahead of its reply, and
 * the reply, 4 ms behind, comes back before the next request: the
 * indication is no answer, so the request stays outstanding until the
 * reply completes it. */
TEST_F(J2534HealthTest, VxReplyArrivesBeforeTheNextRequest)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (delivered.count.load() < 10 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(delivered.count.load(), 10) << "replies didn't arrive";
    EXPECT_EQ(delivered.after_next_write.load(), 0)
        << "replies landed after the next request";
}

/* Reply timing (ADR-0042) through the surrogate, the fake as the VX
 * (each reply 4 ms after its write, stamped by the fake's own clock):
 * a row per request, the answers the fake's delay after its write on
 * the host's clock (less the write's return: the send is stamped after
 * a pipe round trip a loaded machine stretches past kWriteReturnUs, so
 * the floor is judged on the median of the rows' medians, as the next
 * case does, never on one row's), the intervals on the adapter's
 * stamps, none across a fallback, and no ISO-TP counts (the adapter
 * reassembles) */
TEST_F(J2534HealthTest, VxTimingReportsEachRequestOnTheAdaptersClock)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session("?identify=0&diagnose=0"));
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    uint32_t intervals = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        ASSERT_EQ(ecuio_session_get_timing(session, &t, rows.data(),
                                           static_cast<uint32_t>(rows.size()),
                                           &count),
                  ECUIO_OK);
        intervals = 0;
        uint32_t deep = 0;
        for (uint32_t i = 0; i < count; ++i) {
            intervals += rows[i].intervals;
            deep += rows[i].answered >= 5 ? 1u : 0u;
        }
        if (intervals >= 40 && deep >= 3) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_GE(intervals, 40u) << "intervals published";
    EXPECT_EQ(t.interval_clock, ECUIO_TIMING_CLOCK_ADAPTER);
    EXPECT_EQ(t.flags & ECUIO_TIMING_ISOTP, 0u);
    std::vector<uint32_t> medians;
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_request_timing_t& r = rows[i];
        if (r.answered == 0) {
            continue;
        }
        SCOPED_TRACE("PID " + std::to_string(r.request[1]));
        if (r.answered >= 5) {
            medians.push_back(r.latency_p50_us);
        }
        EXPECT_EQ(r.intervals_mixed, 0u);
    }
    ASSERT_GE(medians.size(), 3u) << "requests with five replies or more";
    std::sort(medians.begin(), medians.end());
    EXPECT_GE(medians[medians.size() / 2], 4000u - kWriteReturnUs)
        << "the fake's reply delay";
}

/* The engine reads the adapter's stamp of the reply it is handed, so
 * the stamp is set before the frame reaches it. No ECU answers; the
 * test posts the two replies to the one DID's requests, 50 ms apart on
 * the adapter's clock, as the requests go out: the interval is the
 * posted stamps' difference, nothing the host's clock or the load
 * moves. A stamp set after the frame leaves the first answer to the
 * host's time and the second to the first's stamp, an interval that
 * counts as mixed or comes out near 0 */
TEST_F(J2534HealthTest, AnIntervalIsTheDifferenceOfTheRepliesOwnStamps)
{
    ASSERT_TRUE(fake_control::set(page, "ECUIO_FAKE_ABSENT_ECU", "FFFFFFFF"));
    ASSERT_NO_FATAL_FAILURE(start_session(
        "?profile=uds-generic&did=7E8:F190:u8&probe=0&identify=0&diagnose=0"));
    const uint8_t reply[8] = {0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x90, 0x05};
    const uint32_t kBase = 2000000u;
    const uint32_t kStepUs = 50000u;
    for (uint32_t k = 0; k < 2; ++k) {
        const auto by =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (fake_control::writes(page) <= k &&
               std::chrono::steady_clock::now() < by) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_GT(fake_control::writes(page), k) << "request " << k;
        ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply),
                                       kBase + k * kStepUs, 0));
        ASSERT_TRUE(wait_for_frames(static_cast<int>(k) + 1, 5000))
            << "reply " << k;
    }
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_timing(session, &t, rows.data(),
                                       static_cast<uint32_t>(rows.size()),
                                       &count),
              ECUIO_OK);
    /* published once a second: wait for the row to show both answers */
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < by) {
        ASSERT_EQ(ecuio_session_get_timing(session, &t, rows.data(),
                                           static_cast<uint32_t>(rows.size()),
                                           &count),
                  ECUIO_OK);
        if (count > 0 && rows[0].answered >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(rows[0].answered, 2u);
    EXPECT_EQ(rows[0].intervals_mixed, 0u);
    EXPECT_EQ(rows[0].intervals, 1u);
    EXPECT_GE(rows[0].interval_max_us, kStepUs - 1000u);
    EXPECT_LE(rows[0].interval_max_us, kStepUs + 1000u);
}

/* The surrogate holds a 1 ms timer: the fake as the VX waits for its
 * message inside the surrogate's process, in the timer's steps, so the
 * default timer sees a 4 ms reply at 15.6 ms. With Windows' default 15.6 ms timer
 * every request's median read 15.4 to 16.4 ms (bench_timing); with
 * 1 ms, 11.3 ms. 14 ms sits between, for the median
 * of the requests' medians: a slow-class PID has few replies in the
 * run, and on a loaded CI runner one median alone read 14.2 ms. */
TEST_F(J2534HealthTest, VxRepliesAreSeenWhenTheSliceEndsNotTheTimersStep)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session("?identify=0&diagnose=0"));
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    uint64_t answered = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        ASSERT_EQ(ecuio_session_get_timing(session, &t, rows.data(),
                                           static_cast<uint32_t>(rows.size()),
                                           &count),
                  ECUIO_OK);
        answered = 0;
        for (uint32_t i = 0; i < count; ++i) {
            answered += rows[i].answered;
        }
        if (answered >= 200) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_GE(answered, 200u) << "replies timed";
    std::vector<uint32_t> medians;
    for (uint32_t i = 0; i < count; ++i) {
        if (rows[i].answered >= 5) {
            medians.push_back(rows[i].latency_p50_us);
        }
    }
    ASSERT_GE(medians.size(), 3u) << "requests with five replies or more";
    std::sort(medians.begin(), medians.end());
    EXPECT_LT(medians[medians.size() / 2], 14000u)
        << "reads held to the timer's 15.6 ms step";
}

/* A TX indication, then the START_OF_MESSAGE indication a vendor queues
 * as a segmented reply starts arriving (the CAN ID alone), then the
 * reply: exactly one frame reaches the session, the reply's. Delivered,
 * the first-frame indication would arrive as an empty payload on the
 * reply's ID. */
TEST_F(J2534HealthTest, IndicationsAheadOfAReplyAreSkipped)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));

    const uint8_t tx_done[4] = {0x00, 0x00, 0x07, 0xE0};
    const uint8_t first_frame[4] = {0x00, 0x00, 0x07, 0xE8};
    const uint8_t reply[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    ASSERT_TRUE(fake_control::post(page, tx_done, sizeof(tx_done), 100,
                                   ECUIO_PT_TX_MSG_TYPE |
                                       ECUIO_PT_TX_INDICATION));
    ASSERT_TRUE(fake_control::post(page, first_frame, sizeof(first_frame),
                                   101, ECUIO_PT_START_OF_MESSAGE));
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 102, 0));

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (delivered.count.load() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    session = nullptr;

    ASSERT_EQ(delivered.count.load(), 1);
    EXPECT_EQ(delivered.lengths[0], 4u);
    const uint8_t want[4] = {0x41, 0x0C, 0x1F, 0x40};
    EXPECT_EQ(memcmp(delivered.payloads[0], want, sizeof(want)), 0);
}

/* Beneath the worker, at the loader seam: a read that meets an
 * indication reads on, so the reply queued behind it comes back from the
 * same read (the child calls the vendor again). A read that stopped at the
 * indication would cost the reply a read slice, which no session-level
 * case can see. */
TEST_F(J2534HealthTest, ReadPastAnIndicationReturnsTheReplyBehindIt)
{
    struct HandleGuard {
        ecuio_j2534_handle_t* h = nullptr;
        ~HandleGuard() { (void)ecuio_j2534_close(h); }
    } guard;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &guard.h), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    unsigned long ch = 0;
    ASSERT_EQ(ecuio_j2534_connect_channel(guard.h, 0, 0, &ch), ECUIO_OK);

    const uint8_t tx_done[4] = {0x00, 0x00, 0x07, 0xE0};
    const uint8_t reply[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    ASSERT_TRUE(fake_control::post(page, tx_done, sizeof(tx_done), 100,
                                   ECUIO_PT_TX_MSG_TYPE |
                                       ECUIO_PT_TX_INDICATION));
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 101, 0));

    ecuio_pt_rx_t f = {};
    bool got = false;
    EXPECT_EQ(ecuio_j2534_read_msg(guard.h, ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got) << "the read stopped at the indication";
    ASSERT_EQ(f.data_size, sizeof(reply));
    EXPECT_EQ(memcmp(f.data, reply, sizeof(reply)), 0);

    /* A read skips at most ECUIO_PT_MAX_SKIPPED_PER_READ, then is
     * judged like an empty one; the reply behind them waits for the
     * next read. */
    for (uint32_t i = 0; i < ECUIO_PT_MAX_SKIPPED_PER_READ; ++i) {
        ASSERT_TRUE(fake_control::post(page, tx_done, sizeof(tx_done), 200 + i,
                                       ECUIO_PT_TX_INDICATION));
    }
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 300, 0));
    got = true;
    EXPECT_EQ(ecuio_j2534_read_msg(guard.h, ch, &f, 500, &got), ECUIO_OK);
    EXPECT_FALSE(got) << "read past the cap";
    EXPECT_EQ(ecuio_j2534_read_msg(guard.h, ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got) << "the reply behind the indications was lost";
    EXPECT_EQ(memcmp(f.data, reply, sizeof(reply)), 0);
    EXPECT_EQ(ecuio_j2534_disconnect_channel(guard.h, ch), ECUIO_OK);
}

/* ====================================================================
 * The request engine (ADR-0015): one request outstanding per channel,
 * the next sent when the reply is in, or when the 100 ms wait runs out,
 * and never sooner than the floor after the last write's start (5 ms
 * through J2534, or ?gap='s). The passive drain is here too.
 * ==================================================================== */

/* The fake answers at once, so a second of obd2 at a 20 ms floor
 * (?gap=20) brings about 50 requests, one per floor, against the 10 a
 * fixed 100 ms slot sent. The floor caps it: without one, an instant
 * answer would bring the next request at once. */
TEST_F(J2534HealthTest, RequestsFollowRepliesDownToTheFloor)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?gap=20"));
    ASSERT_TRUE(wait_for_frames(3, 2000)) << "replies didn't arrive";

    const uint32_t n = writes_during(page, 1000);
    EXPECT_GE(n, 20u) << "requests didn't follow their replies";
    EXPECT_LE(n, 55u) << "requests came faster than the floor";
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_OK);
    EXPECT_EQ(ec, 0u);
}

/* Two ECUs, each asked at a 20 ms floor (?gap=20) send to send, not at
 * two of Windows' 15.6 ms timer steps: the fake answers at once and its
 * empty reads return at once, so the worker's waits set the pace, which
 * end on time only on its high-resolution timer (precise_wait.h). On a
 * plain sleep each channel's requests came 31 ms apart, about 63 a
 * second for the two; on the timer 192 to 194 in 2 s. The
 * floor's 100 a second bounds it from above. */
TEST_F(J2534HealthTest, ATwoEcuSessionAsksAtTheEnginesFloor)
{
    ASSERT_NO_FATAL_FAILURE(start_session(
        "?profile=uds-generic&did=1E1C,7E9:1E1C&identify=0&diagnose=0"
        "&gap=20"));
    ASSERT_TRUE(wait_for_frames(6, 2000)) << "replies didn't arrive";

    /* Judged by the fake's stamps on its recorded writes, not a count in
     * a wall-clock window that load thins: the two channels take turns,
     * so write i to write i + 2 is one channel's period, and its median
     * is the floor's (about 21 ms) whatever a few stalls add; on a
     * timer's 15.6 ms step it is 31 */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fake_control::writes(page) < fake_control::kWriteDepth &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(fake_control::writes(page), fake_control::kWriteDepth);
    std::vector<uint32_t> stamps;
    for (uint32_t i = 0; i < fake_control::kWriteDepth; ++i) {
        fake_control::Write w{};
        ASSERT_TRUE(fake_control::recorded_write(page, i, &w));
        stamps.push_back(w.us);
    }
    std::vector<uint32_t> periods;
    for (size_t i = 0; i + 2 < stamps.size(); ++i) {
        periods.push_back(stamps[i + 2] - stamps[i]);
    }
    std::sort(periods.begin(), periods.end());
    const uint32_t median = periods[periods.size() / 2];
    RecordProperty("period_median_us", static_cast<int>(median));
    EXPECT_GE(median, 19000u) << "requests came faster than the floor";
    EXPECT_LE(median, 26000u)
        << "requests waited out the timer's step, not the floor";
}

/* On the VX a call holds its whole timeout unless its batch fills.
 * Reads capped at the 10 ms slice keep requests near the floor's pace
 * whatever the vendor holds; a read held for the 100 ms wait would leave
 * about ten a second. */
TEST_F(J2534HealthTest, VxRepliesAreSeenWithinASlice)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(start_session(""));
    ASSERT_TRUE(wait_for_frames(3, 2000)) << "replies didn't arrive";

    EXPECT_GE(writes_during(page, 1000), 20u)
        << "replies waited for the read to end";
    EXPECT_EQ(delivered.after_next_write.load(), 0)
        << "replies landed after the next request";
}

/* The median time between the starts of the fake's recorded writes, by
 * its own stamps, once it holds kWriteDepth of them; 0 if it never does */
uint32_t median_write_period_us(fake_control::Page* page)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fake_control::writes(page) < fake_control::kWriteDepth &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (fake_control::writes(page) < fake_control::kWriteDepth) {
        return 0;
    }
    std::vector<uint32_t> periods;
    uint32_t last = 0;
    for (uint32_t i = 0; i < fake_control::kWriteDepth; ++i) {
        fake_control::Write w{};
        if (!fake_control::recorded_write(page, i, &w)) {
            return 0;
        }
        if (i > 0) {
            periods.push_back(w.us - last);
        }
        last = w.us;
    }
    std::sort(periods.begin(), periods.end());
    return periods[periods.size() / 2];
}

/* Without ?gap=, J2534's floor is 5 ms: the fake answers at once, so the
 * floor sets the pace. 20 ms, the engine's own default, reads about
 * 21 ms; the ceiling is loose, four times the floor. */
TEST_F(J2534HealthTest, TheFloorIsFiveMillisecondsWithoutAGap)
{
    ASSERT_NO_FATAL_FAILURE(
        start_session("?profile=raw&filter=0x7E8&poll=0x0C"));
    const uint32_t median = median_write_period_us(page);
    RecordProperty("period_median_us", static_cast<int>(median));
    ASSERT_GT(median, 0u) << "the writes weren't recorded";
    EXPECT_GE(median, 4500u) << "requests came faster than the floor";
    EXPECT_LE(median, 20000u) << "the floor isn't 5 ms";
}

/* ?gap= sets the floor, judged from below; a 40 ms floor timed from the
 * write's return would read about the same, so that rule is
 * RequestEngineTest.TheFloorRunsFromTheWritesStart's. */
TEST_F(J2534HealthTest, TheFloorIsTheGapKeys)
{
    ASSERT_NO_FATAL_FAILURE(
        start_session("?profile=raw&filter=0x7E8&poll=0x0C&gap=40"));
    const uint32_t median = median_write_period_us(page);
    RecordProperty("period_median_us", static_cast<int>(median));
    ASSERT_GT(median, 0u) << "the writes weren't recorded";
    EXPECT_GE(median, 38500u) << "requests came faster than ?gap=";
    EXPECT_LE(median, 160000u);
}

/* Through the VX a read asked to wait under 10 ms holds 10 ms, so a
 * worker reading out a floor's wait paced every request by the hold. With
 * every reply in, the worker waits on its own timer and asks the vendor
 * for nothing: at a 100 ms floor each request takes its write's
 * indication and its reply, two or three reads, where reading out the
 * floor took about ten more. Counted by the fake, so load can only lower
 * the reads. */
TEST_F(J2534HealthTest, VxTheFloorsWaitReadsNothing)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(
        start_session("?profile=raw&filter=0x7E8&poll=0x0C&gap=100"));
    ASSERT_TRUE(wait_for_frames(2, 2000)) << "replies didn't arrive";

    const uint32_t writes_before = fake_control::writes(page);
    const uint32_t reads_before = fake_control::reads(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const uint32_t writes = fake_control::writes(page) - writes_before;
    const uint32_t reads = fake_control::reads(page) - reads_before;
    RecordProperty("writes", static_cast<int>(writes));
    RecordProperty("reads", static_cast<int>(reads));
    ASSERT_GE(writes, 5u) << "requests stopped";
    EXPECT_LE(writes, 11u) << "requests came faster than ?gap=";
    EXPECT_LE(reads, 4 * writes) << "the worker read out the floor's wait";
}

} // namespace

#endif // _WIN32
