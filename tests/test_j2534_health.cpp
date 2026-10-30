// File/layer: tests/test_j2534_health.cpp | GTest, HARNESS tier
// Testing: the J2534 worker against the fake inside the surrogate
//          child (this tree's x64 one, or the x86 one in the cross-arch
//          run). The link-health machine: hard and quiet
//          reads, episode counting, recovery through the continued
//          request probe, untouched vendor counts, the write latch, its
//          priority and its step-down when reads recover, a write
//          reported with nothing written, unmatched frames, backoff
//          growth, its end at a clean read and its restart; and the
//          same machine against the fake's VX mode (ECUIO_FAKE_VX): a
//          quiet bus whose empty reads are ERR_BUFFER_EMPTY after their
//          timeout, answered requests, and the no-ACK bus. TX and
//          first-frame indications are skipped without ending a read,
//          so a reply comes back before the next request. The request
//          engine's pacing (ADR-0015): the floor, the wait, negative
//          and pending replies, two channels at once; and the drain in
//          passive mode. Frames carry session time, not the vendor's
//          stamps. With nothing left to ask, empty reads go quiet as
//          in passive mode. A vendor DLL that crashes ends only its
//          child. Replies longer than a CAN-FD frame arrive
//          whole, up to ISO 15765-2's longest; a longer one is cut and
//          marked. Frames lost (ERR_BUFFER_OVERFLOW with or without a
//          message, v05.00's BUFFER_OVERFLOW bit) flag the next frame
//          dispatched with the link unchanged; an ERROR_INDICATION is
//          no frame. Diagnostics (ADR-0037) fill the fake ECU's report
//          through either surrogate.
//          The backend setup and the fake's control page are in
//          j2534_backend_fixture.h.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/profile.h"

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "fake_diagnostics.h"
#include "j2534_backend_fixture.h"

namespace {


void count_cb(const ecuio_frame_t* /*frame*/, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}

/* What the session delivered: each frame's payload and ID, how many
 * writes the fake had taken when it arrived, and how many came after
 * the fake's next write. In a request mode where every write is
 * answered, frame k belongs while the fake has taken exactly k writes;
 * one that arrives later came back after the next request. Read the
 * payloads only after the session is closed. */
struct Delivery {
    fake_control::Page* page = nullptr;
    std::atomic<int> count{0};
    std::atomic<int> after_next_write{0};
    uint16_t lengths[16] = {};
    uint8_t payloads[16][8] = {};
    uint64_t stamps[16] = {};
    uint32_t ids[16] = {};
    uint32_t flags[16] = {};
    uint32_t writes_at[16] = {};
};

void delivery_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* d = static_cast<Delivery*>(user_data);
    const int i = d->count.fetch_add(1);
    const uint32_t writes = fake_control::writes(d->page);
    if (writes > static_cast<uint32_t>(i + 1)) {
        d->after_next_write.fetch_add(1);
    }
    if (i < 16) {
        d->lengths[i] = frame->data_length;
        memcpy(d->payloads[i], frame->data, sizeof(d->payloads[i]));
        d->stamps[i] = frame->timestamp_us;
        d->ids[i] = frame->arbitration_id;
        d->flags[i] = frame->flags;
        d->writes_at[i] = writes;
    }
}

/* The fake's long DID replies (0xF1A0, 0xF1A1, 0xF1A2) as the session
 * delivered them: each one's length and flags, and whether every byte
 * after the 62 <DID> echo holds its index (& 0xFF), the fake's fill.
 * Read them only after the session is closed. */
struct LongReplies {
    std::atomic<int> count{0};
    uint16_t length[3] = {};
    uint32_t flags[3] = {};
    bool filled[3] = {};
};

void long_reply_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* r = static_cast<LongReplies*>(user_data);
    if (frame->data_length < 3 || frame->data[0] != 0x62 ||
        frame->data[1] != 0xF1 || frame->data[2] < 0xA0 ||
        frame->data[2] > 0xA2) {
        return;
    }
    const int k = frame->data[2] - 0xA0;
    bool filled = true;
    for (uint32_t i = 3; i < frame->data_length; ++i) {
        filled = filled && frame->data[i] == static_cast<uint8_t>(i - 3);
    }
    r->length[k] = frame->data_length;
    r->flags[k] = frame->flags;
    r->filled[k] = filled;
    r->count.fetch_add(1);
}

/* The fake's writes over the next ms milliseconds. */
uint32_t writes_during(fake_control::Page* page, int ms)
{
    const uint32_t before = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return fake_control::writes(page) - before;
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
        ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
        ASSERT_EQ(ecuio_register_frame_callback(session, count_cb, &frames),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_register_frame_callback(session, delivery_cb,
                                                &delivered),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }

    /* Waits up to budget_ms for at least n frames. */
    bool wait_for_frames(int n, int budget_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        while (frames.load() < n &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return frames.load() >= n;
    }

    /* Closes the session, so what it delivered can be read. */
    void close_session()
    {
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        session = nullptr;
    }
};

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
 * same call (the child reads a batch at a time). A read that stopped at the
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
 * and never sooner than the 20 ms floor after the last. The passive
 * drain is here too.
 * ==================================================================== */

/* The fake answers at once, so a second of obd2 brings about 50
 * requests, one per floor, against the 10 a fixed 100 ms slot sent. The
 * floor caps it: without one, an instant answer would bring the next
 * request at once. */
TEST_F(J2534HealthTest, RequestsFollowRepliesDownToTheFloor)
{
    ASSERT_NO_FATAL_FAILURE(start_session(""));
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

/* On the VX a read holds its whole timeout unless the batch fills, and
 * the surrogate asks for 16 messages, so a reply 4 ms after its request
 * is seen only when the read ends. Reads capped at the 10 ms slice keep
 * requests near the floor's pace; a read held for the 100 ms wait
 * would leave about ten a second. */
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

/* PID 0x5D is one the fake's ECU doesn't support, and it answers
 * 7F 01 12, as the first vehicle did for 0x5C. The negative reply ends
 * the request like a positive one, so requests keep the floor's pace
 * instead of waiting out 100 ms each, and the ECU answering keeps the
 * link ok. */
TEST_F(J2534HealthTest, NegativeReplyCompletesItsRequest)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8&poll=0x5D"));
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

/* Two ECUs, each on its channel, answer 60 ms after a request. Each
 * channel waits only for its own reply, so both requests are out before
 * either reply arrives, and both ECUs' replies come back. Test-only
 * profile, static storage (the registry keeps the pointer). */
TEST_F(J2534HealthTest, ChannelsWaitOnlyForTheirOwnReplies)
{
    static const ecuio_channel_decl_t kChannels[] = {{0x7E8u, 0x7E0u},
                                                     {0x7E9u, 0x7E1u}};
    static ecuio_signal_def_t defs[2] = {};
    for (int i = 0; i < 2; ++i) {
        defs[i].id = static_cast<uint16_t>(1 + i);
        defs[i].kind = ECUIO_VALUE_UINT;
        defs[i].start_byte = 2;
        defs[i].bit_length = 8;
        defs[i].scale = 1.0;
        defs[i].message_id = kChannels[i].rx_id;
        defs[i].mux_length = 2;
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
 * delivered together rather than one per pass with a wait between. */
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
    EXPECT_LT(took, 250) << "the burst was read one frame per pass";
    EXPECT_EQ(frames.load(), 16);
}

/* A reply longer than a CAN-FD frame arrives whole over the pipe:
 * 203 bytes, past the 64 a frame once held; ISO 15765-2's longest on
 * classic CAN, 4095; and one that fills PASSTHRU_MSG.Data, which a
 * frame holds only the first ECUIO_FRAME_DATA_MAX bytes of, marked
 * TRUNCATED. Through the surrogate each crosses the pipe whole. */
TEST_F(J2534HealthTest, LongRepliesArriveWhole)
{
    static const ecuio_channel_decl_t kChannel[] = {{0x7E8u, 0x7E0u}};
    static ecuio_signal_def_t defs[3] = {};
    for (int i = 0; i < 3; ++i) {
        defs[i].id = static_cast<uint16_t>(1 + i);
        defs[i].kind = ECUIO_VALUE_UINT;
        defs[i].start_byte = 3;
        defs[i].bit_length = 8;
        defs[i].scale = 1.0;
        defs[i].message_id = 0x7E8u;
        defs[i].mux_length = 3;
        defs[i].mux_key = 0x62F1A0u + static_cast<uint32_t>(i);
    }
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    set.channel_count = 1;
    set.channels = kChannel;
    ASSERT_EQ(ecuio_profile_register("test-long-dids", &set, nullptr),
              ECUIO_OK);

    LongReplies replies;
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=test-long-dids"));
    /* Nothing between here and close_session returns early, so the
     * worker never writes into replies after it's gone. */
    EXPECT_EQ(ecuio_register_frame_callback(session, long_reply_cb, &replies),
              ECUIO_OK);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (replies.count.load() < 6 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_NO_FATAL_FAILURE(close_session());

    ASSERT_GE(replies.count.load(), 6) << "the long replies didn't arrive";
    EXPECT_EQ(replies.length[0], 203u);
    EXPECT_EQ(replies.flags[0], 0u);
    EXPECT_TRUE(replies.filled[0]);
    EXPECT_EQ(replies.length[1], 4095u);
    EXPECT_EQ(replies.flags[1], 0u);
    EXPECT_TRUE(replies.filled[1]);
    EXPECT_EQ(replies.length[2], ECUIO_FRAME_DATA_MAX);
    EXPECT_EQ(replies.flags[2], ECUIO_FRAME_FLAG_TRUNCATED);
    EXPECT_TRUE(replies.filled[2]);
}

/* Diagnostics (ADR-0037) through the surrogate of either architecture:
 * the one-byte requests framed, the stored list's First Frame
 * reassembled, and the whole report filled from the fake ECU */
TEST_F(J2534HealthTest, DiagnosticsFillTheReportThroughTheSurrogate)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=obd2&probe=0&identify=0"));
    ecuio_channel_diagnostics_t d{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((ecuio_session_get_channel_diagnostics(session, 0, &d) != ECUIO_OK ||
            (d.flags & ECUIO_DIAG_COMPLETE) == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(d.flags & ECUIO_DIAG_COMPLETE, 0u)
        << "diagnostics didn't complete";
    EXPECT_EQ(d.rx_id, 0x7E8u);
    EXPECT_EQ(d.generation, fake_diagnostics::kItems);
    fake_diagnostics::expect_report(d);
}

/* ---- Frames lost (ERR_BUFFER_OVERFLOW, the BUFFER_OVERFLOW bit) ------ */

/* A message on the engine ECU's channel, 41 0C <n> */
bool post_reply(fake_control::Page* page, uint8_t n, uint32_t rx_status = 0)
{
    const uint8_t msg[7] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, n};
    return fake_control::post(page, msg, sizeof(msg), 1000u + n, rx_status);
}

/* A v04.04 vendor's ERR_BUFFER_OVERFLOW with the message it read: the
 * message arrives flagged, the next one doesn't, and the link is as it
 * was: no hard failure, no episode */
TEST_F(J2534HealthTest, AnOverflowReadDeliversItsMessageFlagged)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_TRUE(post_reply(page, 1));
    ASSERT_TRUE(wait_for_frames(1, 2000));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_OVERFLOW", "1"));
    ASSERT_TRUE(post_reply(page, 2));
    ASSERT_TRUE(wait_for_frames(2, 2000)) << "the overflow read's message";
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_OVERFLOW", nullptr));
    ASSERT_TRUE(post_reply(page, 3));
    ASSERT_TRUE(wait_for_frames(3, 2000));
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_OK);
    EXPECT_EQ(ec, 0u);
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_EQ(delivered.payloads[1][2], 2u);
    EXPECT_EQ(delivered.flags[0] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_NE(delivered.flags[1] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_EQ(delivered.flags[2] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
}

/* v05.00's BUFFER_OVERFLOW bit on a message flags it; one on a message
 * for no channel of ours carries to the next one dispatched */
TEST_F(J2534HealthTest, TheOverflowBitFlagsTheNextFrameDispatched)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_TRUE(post_reply(page, 1, ECUIO_PT_BUFFER_OVERFLOW));
    ASSERT_TRUE(post_reply(page, 2));
    ASSERT_TRUE(wait_for_frames(2, 2000));
    const uint8_t stray[7] = {0x00, 0x00, 0x07, 0xD0, 0x41, 0x0C, 0x09};
    ASSERT_TRUE(fake_control::post(page, stray, sizeof(stray), 1050,
                                   ECUIO_PT_BUFFER_OVERFLOW));
    ASSERT_TRUE(post_reply(page, 3));
    ASSERT_TRUE(wait_for_frames(3, 2000));
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_NE(delivered.flags[0] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_EQ(delivered.flags[1] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_EQ(delivered.payloads[2][2], 3u);
    EXPECT_NE(delivered.flags[2] & ECUIO_FRAME_FLAG_OVERFLOW, 0u)
        << "the stray's loss went with it";
}

/* The bit on an indication, which the child skips, reaches the next
 * message it delivers */
TEST_F(J2534HealthTest, TheOverflowBitOnAnIndicationFlagsTheNextMessage)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_TRUE(
        post_reply(page, 1, ECUIO_PT_TX_INDICATION | ECUIO_PT_BUFFER_OVERFLOW));
    ASSERT_TRUE(post_reply(page, 2));
    ASSERT_TRUE(post_reply(page, 3));
    ASSERT_TRUE(wait_for_frames(2, 2000));
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_EQ(frames.load(), 2) << "the indication is no frame";
    EXPECT_EQ(delivered.payloads[0][2], 2u);
    EXPECT_NE(delivered.flags[0] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_EQ(delivered.flags[1] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
}

/* The same with nothing behind the indication in its read: the child
 * answers the status, so the loss waits for the next message */
TEST_F(J2534HealthTest, AnOverflowBitWithNothingAfterItWaitsForTheNextMessage)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_TRUE(
        post_reply(page, 1, ECUIO_PT_TX_INDICATION | ECUIO_PT_BUFFER_OVERFLOW));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(post_reply(page, 2));
    ASSERT_TRUE(wait_for_frames(1, 2000));
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_NE(le, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(ec, 0u);
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_EQ(delivered.payloads[0][2], 2u);
    EXPECT_NE(delivered.flags[0] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
}

/* ERR_BUFFER_OVERFLOW with nothing read: no failure, and the next
 * message arrives flagged */
TEST_F(J2534HealthTest, AnEmptyOverflowReadFlagsTheNextMessage)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "0x12"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int32_t le = 0;
    uint32_t ec = 0;
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_NE(le, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(ec, 0u);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));
    ASSERT_TRUE(post_reply(page, 1));
    ASSERT_TRUE(post_reply(page, 2));
    ASSERT_TRUE(wait_for_frames(2, 2000));
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_NE(delivered.flags[0] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_EQ(delivered.flags[1] & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
}

/* v05.00's ERROR_INDICATION is an indication, not a frame, even on a
 * channel's ID */
TEST_F(J2534HealthTest, AnErrorIndicationIsNoFrame)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_TRUE(post_reply(page, 1, ECUIO_PT_ERROR_INDICATION));
    ASSERT_TRUE(post_reply(page, 2));
    ASSERT_TRUE(wait_for_frames(1, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(frames.load(), 1);
    ASSERT_NO_FATAL_FAILURE(close_session());
    EXPECT_EQ(delivered.payloads[0][2], 2u);
}

} // namespace

#endif // _WIN32
