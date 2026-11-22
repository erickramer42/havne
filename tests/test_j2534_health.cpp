// File/layer: tests/test_j2534_health.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534HealthTest (test_j2534_health); Cross-arch

#include "test_j2534_health_support.h"

#ifdef _WIN32

namespace {

/* A frame's time is the session's: the first takes the host's time
 * since connect, not the vendor's stamp; the next the vendor's delta,
 * across the 32-bit wrap; and a vendor clock that steps back doesn't
 * take the session's time with it. */
TEST_F(J2534HealthTest, VendorStampsBecomeSessionTime)
{
    const auto before = std::chrono::steady_clock::now();
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw"));
    /* Replies on the engine ECU's ID, which raw listens to */
    const uint8_t a[5] = {0x00, 0x00, 0x07, 0xE8, 0xA1};
    const uint8_t b[5] = {0x00, 0x00, 0x07, 0xE8, 0xB2};
    const uint8_t c[5] = {0x00, 0x00, 0x07, 0xE8, 0xC3};
    ASSERT_TRUE(fake_control::post(page, a, sizeof(a), 0xFFFFFF00u, 0));
    ASSERT_TRUE(fake_control::post(page, b, sizeof(b), 0x00000100u, 0));
    ASSERT_TRUE(fake_control::post(page, c, sizeof(c), 0x00000080u, 0));

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (delivered.count.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const uint64_t since_create = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - before).count());
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);   /* then read */
    ecuio_session_destroy(session);
    session = nullptr;

    ASSERT_EQ(delivered.count.load(), 3);
    ASSERT_EQ(delivered.payloads[0][0], 0xA1);   /* in posting order */
    ASSERT_EQ(delivered.payloads[2][0], 0xC3);
    EXPECT_LE(delivered.stamps[0], since_create) << "the vendor's stamp";
    EXPECT_EQ(delivered.stamps[1] - delivered.stamps[0], uint64_t{0x200})
        << "across the wrap";
    EXPECT_GE(delivered.stamps[2], delivered.stamps[1]);
    EXPECT_LT(delivered.stamps[2] - delivered.stamps[1], uint64_t{1000000})
        << "a step back counts as the host's time between the reads";
}

/* A vendor clock that never moves: two frames with one stamp, the second
 * sent past the lag after the first (the seam sets 300 ms in place of
 * J2534_CLOCK_LAG_US's 10 s), are that far apart in session time: the
 * host's clock stands in for the vendor's from the catch-up on. */
TEST_F(J2534HealthTest, AVendorClockThatStaysFrozenIsCaughtUpToTheHost)
{
    struct LagScope {
        LagScope() { ecuio_set_j2534_clock_lag_us(300000); }
        ~LagScope() { ecuio_set_j2534_clock_lag_us(0); }
    } lag;
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw"));
    const uint8_t a[5] = {0x00, 0x00, 0x07, 0xE8, 0xA1};
    const uint8_t b[5] = {0x00, 0x00, 0x07, 0xE8, 0xB2};
    ASSERT_TRUE(fake_control::post(page, a, sizeof(a), 5000u, 0));
    const auto first_by =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (delivered.count.load() < 1 &&
           std::chrono::steady_clock::now() < first_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(delivered.count.load(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(fake_control::post(page, b, sizeof(b), 5000u, 0));
    const auto second_by =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (delivered.count.load() < 2 &&
           std::chrono::steady_clock::now() < second_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    session = nullptr;
    ASSERT_EQ(delivered.count.load(), 2);
    EXPECT_GE(delivered.stamps[1] - delivered.stamps[0], uint64_t{400000})
        << "the host's time stands in for a clock that stands still";
}

/* A vendor DLL that crashes mid-session ends its surrogate, not the
 * app (ADR-0025): the session reports a hard failure, closes, and the
 * next session connects. */
TEST_F(J2534HealthTest, AVendorDllThatCrashesEndsOnlyItsChild)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw"));
    const uint8_t reply[5] = {0x00, 0x00, 0x07, 0xE8, 0xA1};
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 0, 0));
    ASSERT_TRUE(wait_for_frames(1, 5000)) << "no frame before the crash";

    const unsigned long child = fake_control::opener_pid(page);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_EXIT_ON_READ", "1"));
    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "the crash was never reported (was " << le << ")";
    EXPECT_EQ(ec, 1u);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(child));
    if (p != nullptr) {
        EXPECT_EQ(WaitForSingleObject(p, 5000), WAIT_OBJECT_0)
            << "the surrogate is still running";
        CloseHandle(p);
    }
    close_session();

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_EXIT_ON_READ", nullptr));
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw"));
    EXPECT_NE(fake_control::opener_pid(page), child) << "a new surrogate";
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 0, 0));
    EXPECT_TRUE(wait_for_frames(2, 5000)) << "the next session got no frame";
}

/* Hard failure: one episode per outage, not per read, and recovery
 * through the continued request probe. */
TEST_F(J2534HealthTest, HardFailureSetsCanonicalErrorAndRecovers)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    /* Dead adapter: STATUS_FAILED (0x07) on every read */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "hard failure never reported (was " << le << ")";
    EXPECT_EQ(ec, 1u);

    /* Still exactly one episode while the outage persists: episodes
     * count transitions; the backoff may be doubling under us but must
     * not re-arm the counter. */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(ec, 1u);

    /* Clear the knob; scheduler writes continue as the recovery probe,
     * so a good frame follows and the link heals. The backoff may have
     * grown toward the cap, so budget generously. */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 10000, le, ec))
        << "recovery never reported (was " << le << ")";
    EXPECT_EQ(ec, 1u);   /* recovery adds no episode */
}

TEST_F(J2534HealthTest, QuietBusDebouncesToVehicleTimeoutWithoutEpisode)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    /* Quiet vehicle: ERR_TIMEOUT (0x09) on every read, nothing else */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "9"));

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_VEHICLE_TIMEOUT, 8000,
                               le, ec))   /* debounce ~1 s, plus CI */
        << "quiet debounce never fired (was " << le << ")";
    EXPECT_EQ(ec, 0u);   /* vehicle silence is not an episode */

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 5000, le, ec));
}

/* A uds-generic scan of two DIDs the fake's ECU refuses (7F 22 31),
 * with identification off, leaves nothing to ask: no request can time
 * out, so the session reads as a passive one does, and its empty reads
 * go quiet about a second after the scan ends, with no episode. A frame
 * from the ECU then shows it's there again. Counting only timed-out
 * requests would keep the link ok for good. */
TEST_F(J2534HealthTest, NothingLeftToAskGoesQuiet)
{
    ASSERT_NO_FATAL_FAILURE(
        start_session("?profile=uds-generic&did=1E00-1E01&identify=0"));
    ASSERT_TRUE(wait_for_frames(2, 2000)) << "the scan's refusals didn't arrive";

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_VEHICLE_TIMEOUT, 5000,
                               le, ec))
        << "the link never went quiet (was " << le << ")";
    EXPECT_EQ(ec, 0u);
    EXPECT_EQ(writes_during(page, 300), 0u) << "something was still asked";

    const uint8_t msg[7] = {0x00, 0x00, 0x07, 0xE8, 0x7F, 0x22, 0x31};
    ASSERT_TRUE(fake_control::post(page, msg, sizeof(msg), 1000u, 0));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 2000, le, ec))
        << "the ECU's frame didn't end the quiet (was " << le << ")";
}

/* Count-trust rule: a dirty count under a hard error must NOT read as
 * health. In-proc the loader asks for one message and the knob leaves
 * num at 1 over a zeroed PASSTHRU_MSG; in the child it leaves 16 over
 * the static read batch. Without ecuio_trusted_count either would
 * dispatch phantom frames and report the link healthy. Bare URI: the
 * scheduler keeps writing, so recovery to OK is reachable. */
TEST_F(J2534HealthTest, UntouchedCountUnderHardErrorReportsUnhealthy)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", "7"));

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "dead adapter reported healthy (was " << le << ")";
    EXPECT_GE(ec, 1u);
    /* Phantom check, taken BEFORE clearing the knob: scheduler writes
     * have parked replies in the fake's queue, but the poisoned reads
     * must dispatch nothing. Frames may have arrived before the knob
     * landed, so compare against a count taken while it holds. */
    const int before = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(frames.load(), before) << "frames dispatched under a hard error";

    /* Recovery: writes continued as the probe, and replies flush once
     * reads return honest counts. */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 10000, le, ec))
        << "recovery never reported (was " << le << ")";
}

/* A failed write latches REQUEST_FAILED with ONE episode; a later
 * successful write clears it at once, without a matched read (pinned
 * by holding reads quiet during recovery: only the write can produce
 * OK). The knob leaves the write count untouched, the VX's "status 7,
 * written 1". */
TEST_F(J2534HealthTest, WriteFailureLatchesRequestFailedAndClears)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", "7"));   /* reads clean */

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_REQUEST_FAILED, 5000,
                               le, ec))
        << "failed write never latched (was " << le << ")";
    EXPECT_EQ(ec, 1u) << "write failure flapped episodes";

    /* Latch discipline: past the ~1.2 s quiet debounce, clean but empty
     * reads must NOT have moved the state to VEHICLE_TIMEOUT; only a
     * write result moves it. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_REQUEST_FAILED)
        << "quiet reads overrode the latched write failure";
    EXPECT_EQ(ec, 1u);

    /* Recovery with reads held QUIET (0x09): no frame can be delivered,
     * so OK can only come from the write succeeding. The backoff may
     * sit near its 5 s cap; budget 12 s. */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "9"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 12000, le, ec))
        << "successful write never cleared (was " << le << ")";
    EXPECT_EQ(ec, 1u) << "recovery added an episode";
}

/* A write the vendor calls a success but reports no message written for
 * sent nothing, so it's a failed write: one "requests failing" episode,
 * held while the knob holds, and cleared by the first write that really
 * goes out. Nothing answers the unsent writes, so without the count the
 * session would read as a quiet bus instead. */
TEST_F(J2534HealthTest, ZeroWrittenCountIsAFailedWrite)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_ZERO_COUNT", "1"));

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_REQUEST_FAILED, 5000,
                               le, ec))
        << "an unsent write passed as sent (was " << le << ")";
    EXPECT_EQ(ec, 1u);

    /* Past the ~1.2 s quiet debounce, still failing, no new episode,
     * and none of those writes reached the bus. */
    const uint32_t wc0 = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(ec, 1u);
    EXPECT_EQ(fake_control::writes(page), wc0);

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_ZERO_COUNT", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 12000, le, ec))
        << "a sent write never cleared it (was " << le << ")";
    EXPECT_EQ(ec, 1u) << "recovery added an episode";
}

/* With writes AND reads failing, the hard read failure outranks
 * requests-failing. Both transitions happen in the first cycle (write,
 * then drain read), so exactly two episodes: one REQUEST_FAILED, one
 * HARD. While both persist, the write path must not downgrade back. */
TEST_F(J2534HealthTest, HardReadFailureOutranksRequestFailed)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", "7"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "hard read failure never reported (was " << le << ")";
    EXPECT_EQ(ec, 2u) << "expected write-then-read episodes, got " << ec;

    /* Hold: the continued failing writes must not downgrade the hard
     * failure or add episodes. */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_TRANSPORT_HARD_FAILURE)
        << "write path downgraded a hard read failure";
    EXPECT_EQ(ec, 2u);
}

/* With writes AND reads failing, then reads recovering while writes
 * still fail: the state steps down from adapter fault to requests
 * failing, with no new episode, because the adapter reads cleanly again
 * and what fails is the requests. The write that goes out at last
 * returns it to ok. */
TEST_F(J2534HealthTest, CleanReadsWhileWritesFailStepDownToRequestsFailing)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", "7"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "hard read failure never reported (was " << le << ")";
    ASSERT_EQ(ec, 2u);

    /* The backoff may be sleeping toward its 5 s cap; budget 10 s. */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_REQUEST_FAILED, 10000,
                               le, ec))
        << "adapter fault stuck while reads were clean (was " << le << ")";
    EXPECT_EQ(ec, 2u) << "the step down added an episode";

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", nullptr));
    EXPECT_TRUE(wait_for_state(session, ECUIO_OK, 12000, le, ec))
        << "a sent write never cleared it (was " << le << ")";
    EXPECT_EQ(ec, 2u);
}

/* A frame for no channel of ours is one more quiet read, not quiet at
 * once: on a talking bus with strays among the replies, health would
 * otherwise flip between ok and quiet frame by frame. A reply sets ok,
 * then a stray follows (header 0x7D0; the filter registered 0x7E8), and
 * 300 ms on the link still reads ok, since the stray and the empty reads
 * after it are a few of the ten the debounce needs. It goes quiet when
 * they run out, with no episode. The passive URI keeps requests out of
 * it. */
TEST_F(J2534HealthTest, UnmatchedFrameCountsAsAQuietRead)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));

    const uint8_t reply[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 1000, 0));
    ASSERT_TRUE(wait_for_frames(1, 2000)) << "the reply never arrived";
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    ASSERT_EQ(le, ECUIO_OK);

    const uint8_t stray[12] = {
        0x00, 0x00, 0x07, 0xD0,
        0x62, 0x1E, 0x1C, 0x2A, 0, 0, 0, 0
    };
    ASSERT_TRUE(fake_control::post(page, stray, sizeof(stray), 1100, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_OK) << "one stray frame read as a quiet bus";
    EXPECT_EQ(frames.load(), 1) << "the stray was dispatched";

    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_VEHICLE_TIMEOUT, 5000,
                               le, ec))
        << "the quiet run never ended (was " << le << ")";
    EXPECT_EQ(ec, 0u) << "going quiet added an episode";
}

/* The backoff GROWS under sustained hard failure. Writes continue as
 * the recovery probe, but the doubling backoff spaces them: about 5
 * probes in 1.5 s (100 + 200 + 400 + 800 ms), against about 15 if it
 * reset to its base on every failure. Wide margins on purpose: the two
 * behaviors differ by 3x, so CI timing noise can't confuse them. */
TEST_F(J2534HealthTest, BackoffGrowsUnderSustainedHardFailure)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));   /* writes clean */

    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "hard failure never reported";

    const uint32_t wc0 = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const uint32_t wc1 = fake_control::writes(page);

    EXPECT_GE(wc1 - wc0, 1u) << "probe writes stopped entirely";
    EXPECT_LE(wc1 - wc0, 8u) << "backoff never grew (reset-to-base shape)";
}

/* A hard failure that ends on a silent bus: the adapter answers again,
 * but nothing replies. Its first clean read ends the backoff, so the
 * quiet debounce counts reads at the request cadence and fires in about
 * a second, with no new episode, and the probe runs at about 10 writes
 * a second. Counted one read per backoff interval, the debounce would
 * take about 26 s. */
TEST_F(J2534HealthTest, HardFailureEndingInSilenceGoesQuietAtFullCadence)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "hard failure never reported";
    ASSERT_EQ(ec, 1u);

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "9"));
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_VEHICLE_TIMEOUT, 5000,
                               le, ec))
        << "silence after a hard failure never read as quiet (was " << le
        << ")";
    EXPECT_EQ(ec, 1u) << "going quiet added an episode";

    const uint32_t wc0 = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const uint32_t wc1 = fake_control::writes(page);
    EXPECT_GE(wc1 - wc0, 6u) << "the probe stayed backed off";
}

/* A clean read in the middle of a hard failure ends the backoff, but
 * when the hard reads resume the backoff starts again: about 5 probes in
 * 1.5 s, against about 15 if a dead adapter were probed at the request
 * cadence. */
TEST_F(J2534HealthTest, BackoffRestartsWhenHardReadsResume)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    int32_t le = 0;
    uint32_t ec = 0;
    ASSERT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec))
        << "hard failure never reported";
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    /* A few clean reads, short of the quiet debounce, then dead again.
     * Each cycle writes, then reads, so three writes after the switch
     * mean at least two clean reads have run. */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "9"));
    const uint32_t at_switch = fake_control::writes(page);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(6);
    while (fake_control::writes(page) < at_switch + 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(fake_control::writes(page), at_switch + 3)
        << "no cycle ran with clean reads";
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const uint32_t wc0 = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const uint32_t wc1 = fake_control::writes(page);
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(ec, 1u) << "one outage, one episode";
    EXPECT_GE(wc1 - wc0, 1u) << "probe writes stopped entirely";
    EXPECT_LE(wc1 - wc0, 8u) << "a dead adapter was probed at full cadence";
}

} // namespace

#endif // _WIN32
