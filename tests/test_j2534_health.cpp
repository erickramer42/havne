// File/layer: tests/test_j2534_health.cpp | GTest, HARNESS tier
// Testing: the J2534 worker's link-health machine through each backend:
//          the fake loaded in-proc, and the same fake inside the
//          surrogate child. Every case runs on both. Hard and quiet
//          reads, episode counting, recovery through the continued
//          request probe, untouched vendor counts, the write latch and
//          its priority, a write reported with nothing written,
//          unmatched frames, backoff growth, its end at a clean read
//          and its restart; and the same
//          machine against the fake's VX mode (ECUIO_FAKE_VX): a quiet
//          bus whose empty reads are ERR_BUFFER_EMPTY after their
//          timeout, answered requests, and the no-ACK bus. TX and
//          first-frame indications are skipped without ending a read,
//          so a reply comes back in its own request's cycle. Frames
//          carry session time, not the vendor's stamps.
//          The backend setup and the fake's control page are in
//          j2534_backend_fixture.h.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "j2534_backend_fixture.h"

namespace {

using j2534_backends::Backend;

void count_cb(const ecuio_frame_t* /*frame*/, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}

/* What the session delivered: each frame's payload, and how many came
 * after the fake's next write. In a request mode where every write is
 * answered, frame k belongs while the fake has taken exactly k writes;
 * one that arrives later landed a cycle late. Read the payloads only
 * after the session is closed. */
struct Delivery {
    fake_control::Page* page = nullptr;
    std::atomic<int> count{0};
    std::atomic<int> after_next_write{0};
    uint16_t lengths[16] = {};
    uint8_t payloads[16][8] = {};
    uint64_t stamps[16] = {};
};

void delivery_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* d = static_cast<Delivery*>(user_data);
    const int i = d->count.fetch_add(1);
    if (fake_control::writes(d->page) > static_cast<uint32_t>(i + 1)) {
        d->after_next_write.fetch_add(1);
    }
    if (i < 16) {
        d->lengths[i] = frame->data_length;
        memcpy(d->payloads[i], frame->data, sizeof(d->payloads[i]));
        d->stamps[i] = frame->timestamp_us;
    }
}

/* The session and its frame count are members, and TearDown closes the
 * session first, so a failed ASSERT can't leave the worker counting
 * into a destroyed local. */
class J2534HealthTest : public j2534_backends::BackendTest {
protected:
    ecuio_session_t* session = nullptr;
    std::atomic<int> frames{0};
    Delivery delivered;

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(BackendTest::SetUp());
        delivered.page = page;
    }

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        BackendTest::TearDown();
    }

    /* Creates and starts the session on the fake with the given
     * query, counting its frames, and checks the backend it runs on. */
    void start_session(const char* query)
    {
        const std::string u = uri(query);
        ASSERT_EQ(ecuio_session_create(u.c_str(), &session), ECUIO_OK) << u;
        ASSERT_NO_FATAL_FAILURE(expect_opened_on_backend());
        ASSERT_EQ(ecuio_register_frame_callback(session, count_cb, &frames),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_register_frame_callback(session, delivery_cb,
                                                &delivered),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }
};

/* A frame's time is the session's: the first takes the host's time
 * since connect, not the vendor's stamp; the next the vendor's delta,
 * across the 32-bit wrap; and a vendor clock that steps back doesn't
 * take the session's time with it. */
TEST_P(J2534HealthTest, VendorStampsBecomeSessionTime)
{
    const auto before = std::chrono::steady_clock::now();
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw"));
    const uint8_t a[4] = {0xA1}, b[4] = {0xB2}, c[4] = {0xC3};
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

/* Polls the error state until it reads want or budget_ms passes; le and
 * ec keep the last reading. */
bool wait_for_state(ecuio_session_t* s, int32_t want, int budget_ms,
                    int32_t& le, uint32_t& ec)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budget_ms);
    for (;;) {
        (void)ecuio_session_get_error_state(s, &le, &ec);
        if (le == want) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/* Hard failure: one episode per outage, not per read, and recovery
 * through the continued request probe. */
TEST_P(J2534HealthTest, HardFailureSetsCanonicalErrorAndRecovers)
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

TEST_P(J2534HealthTest, QuietBusDebouncesToVehicleTimeoutWithoutEpisode)
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

/* Count-trust rule: a dirty count under a hard error must NOT read as
 * health. In-proc the loader asks for one message and the knob leaves
 * num at 1 over a zeroed PASSTHRU_MSG; in the child it leaves 16 over
 * the static read batch. Without ecuio_trusted_count either would
 * dispatch phantom frames and report the link healthy. Bare URI: the
 * scheduler keeps writing, so recovery to OK is reachable. */
TEST_P(J2534HealthTest, UntouchedCountUnderHardErrorReportsUnhealthy)
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
TEST_P(J2534HealthTest, WriteFailureLatchesRequestFailedAndClears)
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
TEST_P(J2534HealthTest, ZeroWrittenCountIsAFailedWrite)
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
TEST_P(J2534HealthTest, HardReadFailureOutranksRequestFailed)
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

/* A delivered-but-unmatched frame (header matches no channel) clears
 * hard failure and the backoff, since the adapter answered, but moves
 * to VEHICLE_TIMEOUT, not OK, and adds no episode.
 *
 * The 900 ms budget is the discriminator, so it is NOT generous by
 * accident: code that fully clears reaches OK (never TIMEOUT, fails);
 * code that treats the frame as plain quiet needs the ~1.2 s debounce
 * and misses the window. The passive URI keeps writes out of it. */
TEST_P(J2534HealthTest, UnmatchedDeliveredFrameGoesQuietNotHealthy)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));

    /* Force hard failure, then deliver something the adapter DID read */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));

    int32_t le = 0;
    uint32_t ec = 0;
    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                               5000, le, ec));
    EXPECT_EQ(ec, 1u);

    /* Header 0x7D0: no channel's rx ID (the filter registered 0x7E8).
     * Gating is off, so the fake delivers it; the transport decides. */
    const uint8_t unmatched[12] = {
        0x00, 0x00, 0x07, 0xD0,
        0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0
    };
    ASSERT_TRUE(fake_control::post(page, unmatched, sizeof(unmatched),
                                   1000, 0));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));

    EXPECT_TRUE(wait_for_state(session, ECUIO_ERR_VEHICLE_TIMEOUT, 900,
                               le, ec))   /* < the debounce */
        << "unmatched frame did not degrade to quiet (was " << le << ")";
    EXPECT_EQ(ec, 1u) << "unmatched frame added an episode";
}

/* The backoff GROWS under sustained hard failure. Writes continue as
 * the recovery probe, but the doubling backoff spaces them: about 5
 * probes in 1.5 s (100 + 200 + 400 + 800 ms), against about 15 if it
 * reset to its base on every failure. Wide margins on purpose: the two
 * behaviours differ by 3x, so CI timing noise can't confuse them. */
TEST_P(J2534HealthTest, BackoffGrowsUnderSustainedHardFailure)
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
TEST_P(J2534HealthTest, HardFailureEndingInSilenceGoesQuietAtFullCadence)
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
TEST_P(J2534HealthTest, BackoffRestartsWhenHardReadsResume)
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

/* ====================================================================
 * The fake's VX mode (ECUIO_FAKE_VX): reads held for their timeout, a
 * short batch with STATUS_NOERROR, an empty read with ERR_BUFFER_EMPTY,
 * a TX indication after each write and the reply 4 ms later, as the
 * OBDX Pro VX measured (docs/bench-sessions/2026-10-22.md and the
 * vehicle sheet).
 * ==================================================================== */

/* A quiet bus on the VX: every passive read waits out its 100 ms and
 * comes back ERR_BUFFER_EMPTY, which is "no frame", so the debounce
 * takes ten of them. The lower bound pins the hold: reads that
 * returned at once would reach quiet in about 200 ms (ten 20 ms
 * sleeps). The bench saw 1.42 s. */
TEST_P(J2534HealthTest, VxQuietBusDebouncesOnBufferEmptyReads)
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
TEST_P(J2534HealthTest, VxAnsweredRequestsKeepTheLinkOk)
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
TEST_P(J2534HealthTest, VxNoAckBusAcceptsThreeWritesThenRequestsFail)
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
 * the reply, 4 ms behind, comes back in the same cycle's drain, before
 * the next write. In-proc the loader reads a message at a time; had the
 * skipped indication ended the read, the drain would stop there and
 * every reply would land a cycle late. */
TEST_P(J2534HealthTest, VxReplyArrivesInItsOwnRequestsCycle)
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

/* A TX indication, then the START_OF_MESSAGE indication a vendor queues
 * as a segmented reply starts arriving (the CAN ID alone), then the
 * reply: exactly one frame reaches the session, the reply's. Delivered,
 * the first-frame indication would arrive as an empty payload on the
 * reply's ID. */
TEST_P(J2534HealthTest, IndicationsAheadOfAReplyAreSkipped)
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

INSTANTIATE_TEST_SUITE_P(Backend, J2534HealthTest,
                         ::testing::Values(Backend::InProc,
                                           Backend::Surrogate),
                         j2534_backends::label);

} // namespace

#endif // _WIN32
