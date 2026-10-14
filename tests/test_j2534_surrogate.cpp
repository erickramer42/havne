// File/layer: tests/test_j2534_surrogate.cpp | GTest, HARNESS tier
// Testing: spawns the REAL surrogate process hosting the fake vendor DLL,
//          and asserts frames cross the pipe+ring boundary intact: exact
//          counts, payload fidelity, RxStatus->flags translation, vendor
//          timestamp passthrough, FIFO order, pump gating on CONNECT,
//          clean job-object teardown, and fast failure on a dead DLL.
//
// ARCH note: in the x64 configure this is all-x64 (CI-valid IPC test);
// the true cross-arch case (x64 test process -> x86 surrogate -> x86
// fake) runs when QTTUNE_SURROGATE_EXE points at Win32-tree artifacts.

#include <gtest/gtest.h>

#include "qttune/j2534_surrogate.h"
#include "qttune/core.h"
#include "qttune/j2534_ptypes.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

struct SurCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][64]{};
    unsigned int frame_flags[8]{};
    uint64_t timestamps[8]{};
};

void sur_capture_cb(const qttune_frame_t* frame, void* user_data)
{
    SurCapture* cap = static_cast<SurCapture*>(user_data);
    const int i = cap->count.fetch_add(1);
    if (i < 8) {
        memcpy(cap->payloads[i], frame->data, 64);
        cap->frame_flags[i] = static_cast<unsigned int>(frame->flags);
        cap->timestamps[i] = frame->timestamp_us;
    }
}

/* Poll-until-condition with deadline — no wall-clock bets (the
 * 2026-10-14 CI-flake lesson, applied from birth this time). */
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms, int step_ms = 10)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

/* Drain the surrogate ring via the client read API into a capture. */
void drain_sur_frames(qttune_j2534_sur_handle_t* h, SurCapture* cap)
{
    for (;;) {
        qttune_frame_t f = {};
        bool got = false;
        qttune_status_t st = qttune_j2534_sur_read_frame(h, 0, &f, 50, &got);
        if (st != QT_OK) return;
        if (!got) return;
        sur_capture_cb(&f, cap);
    }
}

} // namespace

TEST(J2534SurrogateTest, OpenConnectInjectReadCloseCycle)
{
    qttune_j2534_sur_handle_t* h = nullptr;

    /* Spawn: creates mapping/event/pipe, spawns under the kill-on-close
     * job, surrogate loads the fake DLL, calls PassThruOpen, handshakes. */
    ASSERT_EQ(qttune_j2534_sur_open(FAKE_J2534_DLL_PATH, &h), QT_OK);
    ASSERT_NE(h, nullptr);

    /* Connect with default sentinel baud — resolves to ISO 15765 500k
     * client-side per the loader contract; the surrogate relays the
     * resolved values verbatim to the fake's PassThruConnect. */
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* Inject THROUGH the surrogate (control pipe -> InjectFrame in the
     * surrogate's module instance -> fake FIFO). Varied flags exercise
     * the RxStatus -> QTTUNE_FRAME_FLAG_EXTENDED translation both ways;
     * timestamps are ours and must pass through untranslated. */
    const unsigned char f0[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};
    const unsigned char f1[8] = {0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0};
    const unsigned char f2[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(
                  h, f0, 8, 1000, 0), QT_OK);
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(
                  h, f1, 8, 2000, QTTUNE_PT_CAN_29BIT_ID), QT_OK);
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(
                  h, f2, 8, 3000, 0), QT_OK);

    SurCapture cap;
    /* The predicate MUST drain — cap only grows when the ring is
     * polled, so waiting on count without draining inside the loop
     * burns the full timeout every run. Sub-second now if IPC works. */
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 3; },
        10000, 50);
    ASSERT_TRUE(delivered);
    drain_sur_frames(h, &cap);

    /* EXACT count: the fake fabricates nothing we did not inject,
     * and the ring drops nothing we did. (2026-10-13 lesson 7.) */
    ASSERT_EQ(cap.count.load(), 3);

    ASSERT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[1], f1, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[2], f2, 8), 0);

    /* Flag translation through the whole IPC hop */
    EXPECT_EQ(cap.frame_flags[0], 0u);
    EXPECT_EQ(cap.frame_flags[1], QTTUNE_FRAME_FLAG_EXTENDED);
    EXPECT_EQ(cap.frame_flags[2], 0u);

    /* Vendor timestamp passthrough (documented v0.3.0 contract) */
    EXPECT_EQ(cap.timestamps[0], uint64_t{1000});
    EXPECT_EQ(cap.timestamps[1], uint64_t{2000});
    EXPECT_EQ(cap.timestamps[2], uint64_t{3000});

    /* Teardown: SHUTDOWN, bounded wait, job-object kill. */
    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, ReadPumpIsGatedOnConnect)
{
    /* Frame injected PRE-connect parks in the fake's FIFO; the pump
     * gate (g_channel == 0) must hold it there. It arrives only after
     * CONNECT — the fake's InjectFrame and ReadMsgs share one queue,
     * so parking is real, not an artifact of separate containers. */
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(FAKE_J2534_DLL_PATH, &h), QT_OK);

    SurCapture cap;
    const unsigned char f0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(h, f0, 8, 500, 0), QT_OK);

    /* Pre-connect: nothing may arrive. Predicate drains as it waits. */
    const bool early = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() > 0; },
        1500, 50);
    EXPECT_FALSE(early) << "frame flowed before CONNECT — pump gating broken";
    EXPECT_EQ(cap.count.load(), 0);

    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* Post-connect: the parked frame arrives. */
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        10000, 50);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(cap.count.load(), 1);
    EXPECT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    EXPECT_EQ(cap.timestamps[0], uint64_t{500});

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, DeadDllFailsFastAtOpen)
{
    /* A surrogate spawned with an unloadable DLL must die before (or
     * during) the handshake; the client must notice via the process
     * handle wait, not by burning the 8s deadline. QT_ERR_LOAD_FAILED
     * is the contract. Wall-clock guard: 5s budget proves fail-fast. */
    const auto t0 = std::chrono::steady_clock::now();
    qttune_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(qttune_j2534_sur_open("C:\\does\\not\\exist.dll", &h),
              QT_ERR_LOAD_FAILED);
    EXPECT_EQ(h, nullptr);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5000);
}

TEST(J2534SurrogateTest, NullArgumentsRejected)
{
    qttune_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(qttune_j2534_sur_open(nullptr, &h), QT_ERR_NULL_ARGUMENT);
    EXPECT_EQ(qttune_j2534_sur_open(FAKE_J2534_DLL_PATH, nullptr),
              QT_ERR_NULL_ARGUMENT);
    EXPECT_EQ(qttune_j2534_sur_close(nullptr), QT_OK);   /* free(nullptr) */
}

#endif /* _WIN32 */
