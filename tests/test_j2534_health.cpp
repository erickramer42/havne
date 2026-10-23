// File/layer: tests/test_j2534_health.cpp | GTest, HARNESS tier
// Testing: the J2534 worker's link-health machine through each backend:
//          the fake loaded in-proc, and the same fake inside the
//          surrogate child. Every case runs on both. Hard and quiet
//          reads, episode counting, recovery through the continued
//          request probe, untouched vendor counts, the write latch and
//          its priority, unmatched frames, backoff growth.
//          The fake is driven through its control page
//          (fake_control.h), the one channel that reaches the child's
//          instance mid-session; the page also shows which process
//          opened the fake, so each case checks its backend.
//
// ARCH note: in this tree the surrogate case runs the x64 surrogate
// hosting the x64 fake, with the loader's routing forced
// (ecuio_j2534_force_surrogate_for_testing). With ECUIO_FAKE_DLL set
// (the cross-arch run), it runs the Win32 tree's fake in the x86
// surrogate that ECUIO_SURROGATE_EXE names.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "fake_control.h"

namespace {

enum class Backend { InProc, Surrogate };

std::string backend_label(const ::testing::TestParamInfo<Backend>& info)
{
    return info.param == Backend::InProc ? "InProc" : "Surrogate";
}

/* Replaces a variable for one scope and puts the previous value back:
 * the cross-arch run sets ECUIO_SURROGATE_EXE itself. */
struct EnvSwap {
    const char* name;
    bool had = false;
    char old[1024] = {};
    EnvSwap(const char* n, const char* v) : name(n)
    {
        const DWORD k = GetEnvironmentVariableA(n, old, sizeof(old));
        had = k > 0 && k < sizeof(old);
        SetEnvironmentVariableA(n, v);
    }
    ~EnvSwap() { SetEnvironmentVariableA(name, had ? old : nullptr); }
    EnvSwap(const EnvSwap&) = delete;
    EnvSwap& operator=(const EnvSwap&) = delete;
};

void count_cb(const ecuio_frame_t* /*frame*/, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}

/* The session and its frame count are members, and TearDown closes the
 * session first, so a failed ASSERT can't leave the worker counting
 * into a destroyed local. */
class J2534HealthTest : public ::testing::TestWithParam<Backend> {
protected:
    fake_control::Page* page = nullptr;
    std::string dll;
    std::unique_ptr<EnvSwap> exe_swap;
    ecuio_session_t* session = nullptr;
    std::atomic<int> frames{0};

    void SetUp() override
    {
        page = fake_control::create_for_this_process();
        ASSERT_NE(page, nullptr);
        fake_control::reset(page);

        if (GetParam() == Backend::InProc) {
            dll = FAKE_J2534_DLL_PATH;
            return;
        }
        const char* cross = std::getenv("ECUIO_FAKE_DLL");
        if (cross != nullptr && cross[0] != '\0') {
            dll = cross;   /* x86: its surrogate is already named */
        } else {
            dll = FAKE_J2534_DLL_PATH;
            exe_swap = std::make_unique<EnvSwap>("ECUIO_SURROGATE_EXE",
                                                 SURROGATE_EXE_PATH);
        }
        ecuio_j2534_force_surrogate_for_testing(true);
    }

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        ecuio_j2534_force_surrogate_for_testing(false);
        exe_swap.reset();
        if (page != nullptr) {
            fake_control::reset(page);   /* no knob reaches the next case */
        }
    }

    void knob(const char* name, const char* value)
    {
        ASSERT_TRUE(fake_control::set(page, name, value)) << name;
    }

    /* Creates and starts the session on the fake with the given
     * query, counting its frames, and checks the backend it runs on:
     * the fake publishes the process that opened it. */
    void start_session(const char* query)
    {
        char uri[1200];
        snprintf(uri, sizeof(uri), "j2534://%s%s", dll.c_str(), query);
        ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK) << uri;
        const unsigned long opener = fake_control::opener_pid(page);
        if (GetParam() == Backend::InProc) {
            ASSERT_EQ(opener, GetCurrentProcessId())
                << "the fake didn't open in this process";
        } else {
            ASSERT_NE(opener, 0ul) << "the fake never opened";
            ASSERT_NE(opener, GetCurrentProcessId())
                << "the fake opened in-proc, not in the surrogate";
        }
        ASSERT_EQ(ecuio_register_frame_callback(session, count_cb, &frames),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }
};

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

INSTANTIATE_TEST_SUITE_P(Backend, J2534HealthTest,
                         ::testing::Values(Backend::InProc,
                                           Backend::Surrogate),
                         backend_label);

} // namespace

#endif // _WIN32
