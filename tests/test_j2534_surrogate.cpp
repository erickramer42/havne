// File/layer: tests/test_j2534_surrogate.cpp | GTest, HARNESS tier
// Testing: spawns the REAL surrogate process hosting the fake vendor DLL,
//          and asserts frames cross the pipe intact: exact counts,
//          payload fidelity, RxStatus->flags translation, vendor
//          timestamp passthrough, FIFO order, no reads before CONNECT,
//          clean job-object teardown, and fast failure on a dead DLL.
//          Also: write->echo round trip, ioctl SET->GET + VBATT,
//          child-side ioctl whitelist, filter start/stop lifecycle,
//          recovery after a timed-out request, a late read reply
//          dropped and logged, a request-ID mismatch ending the
//          handle, a blocked vendor open,
//          an ERR_TIMEOUT partial batch (spec-mode fake), no
//          phantom frames from an untouched count under a hard error,
//          a child from other shared headers refused at OP_OPEN,
//          device and channel ID 0 handled as real IDs, malformed
//          requests refused by the client before sending and by the
//          child on arrival, and a surrogate exe that exits before the
//          handshake.
//
// ARCH note: in the x64 configure this is all-x64 (CI-valid IPC test);
// the true cross-arch case (x64 test process -> x86 surrogate -> x86
// fake) runs when ECUIO_SURROGATE_EXE points at Win32-tree artifacts.
// The write/ioctl/filter assertions are deliberately observable
// through RETURN VALUES and frames read back over the pipe alone (no
// fake-instance hooks) so they hold in BOTH architectures unchanged:
// an x64 test cannot reach into the x86 child's fake instance.

#include <gtest/gtest.h>

#include "ecuio/j2534_surrogate.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <cstdlib>
#include <windows.h>

#include "fake_control.h"
#include "non_ascii_copy.h"

/* Env override for the cross-arch case: an x86 surrogate can't load
 * the x64 fake baked at generate time, so the Win32 tree's fake DLL
 * takes precedence when ECUIO_FAKE_DLL is set. Mirrors the client's
 * ECUIO_SURROGATE_EXE override. */
static const char* fake_dll_path()
{
    const char* env = std::getenv("ECUIO_FAKE_DLL");
    return (env != nullptr && env[0] != '\0') ? env : FAKE_J2534_DLL_PATH;
}

/* The fake's architecture, which picks the surrogate (ADR-0025): the
 * Win32 tree's x86 one in the cross-arch run, this tree's x64 one
 * otherwise. */
static uint32_t fake_dll_arch()
{
    const char* env = std::getenv("ECUIO_FAKE_DLL");
    return (env != nullptr && env[0] != '\0') ? ECUIO_DLL_ARCH_X86
                                               : ECUIO_DLL_ARCH_X64;
}

/* That surrogate's relocation variable and exe name. */
static const char* surrogate_env()
{
    return fake_dll_arch() == ECUIO_DLL_ARCH_X86 ? "ECUIO_SURROGATE_EXE"
                                                 : "ECUIO_SURROGATE_X64_EXE";
}
static const char* surrogate_name()
{
    return fake_dll_arch() == ECUIO_DLL_ARCH_X86
               ? "ecuio-j2534-surrogate.exe"
               : "ecuio-j2534-surrogate-x64.exe";
}

namespace {

struct SurCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][64]{};
    uint32_t rx_status[8]{};
    uint32_t timestamps[8]{};
};

void sur_capture(const ecuio_pt_rx_t* msg, SurCapture* cap)
{
    const int i = cap->count.fetch_add(1);
    if (i < 8) {
        memcpy(cap->payloads[i], msg->data,
               (msg->data_size < 64) ? msg->data_size : 64);
        cap->rx_status[i] = msg->rx_status;
        cap->timestamps[i] = msg->timestamp;
    }
}

/* Poll-until-condition with deadline, no wall-clock bets: a starved
 * CI runner can take far longer than the local cadence suggests. */
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
void drain_sur_frames(ecuio_j2534_sur_handle_t* h, SurCapture* cap)
{
    for (;;) {
        ecuio_pt_rx_t f = {};
        bool got = false;
        ecuio_status_t st = ecuio_j2534_sur_read_msg(h, 0, &f, 50, &got);
        if (st != ECUIO_OK) return;
        if (!got) return;
        sur_capture(&f, cap);
    }
}

/* Flattened-SCONFIG helper: one (Parameter, Value) u32 pair, host
 * endianness (memcpy): valid because every participant in this
 * suite is x86 or x64, both little-endian, and both sides of the
 * wire use memcpy-based (un)flattening. */
void put_pair(unsigned char* buf, uint32_t param, uint32_t value)
{
    memcpy(buf + 0, &param, 4);
    memcpy(buf + 4, &value, 4);
}

uint32_t get_u32(const unsigned char* buf)
{
    uint32_t v = 0;
    memcpy(&v, buf, 4);
    return v;
}

} // namespace

TEST(J2534SurrogateTest, OpenConnectInjectReadCloseCycle)
{
    ecuio_j2534_sur_handle_t* h = nullptr;

    /* Spawn: creates mapping/event/pipe, spawns under the kill-on-close
     * job, surrogate loads the fake DLL, calls PassThruOpen, handshakes. */
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    ASSERT_NE(h, nullptr);

    /* Connect with default sentinel baud: resolves to ISO 15765 500k
     * client-side per the loader contract; the surrogate relays the
     * resolved values verbatim to the fake's PassThruConnect. */
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    /* Inject THROUGH the surrogate (control pipe -> InjectFrame in the
     * surrogate's module instance -> fake FIFO). RxStatus and the
     * timestamps are ours and must pass through untranslated; the
     * transport maps CAN_29BIT_ID to ECUIO_FRAME_FLAG_EXTENDED. */
    const unsigned char f0[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};
    const unsigned char f1[8] = {0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0};
    const unsigned char f2[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(
                  h, f0, 8, 1000, 0), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(
                  h, f1, 8, 2000, ECUIO_PT_CAN_29BIT_ID), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(
                  h, f2, 8, 3000, 0), ECUIO_OK);

    SurCapture cap;
    /* The predicate MUST drain: cap only grows when reads are
     * issued over the pipe, so waiting on count without draining
     * inside the loop burns the full timeout every run. Sub-second
     * when IPC works. */
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 3; },
        10000, 50);
    ASSERT_TRUE(delivered);
    drain_sur_frames(h, &cap);

    /* EXACT count: the fake fabricates nothing we did not inject,
     * and the pipe drops nothing we did. */
    ASSERT_EQ(cap.count.load(), 3);

    ASSERT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[1], f1, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[2], f2, 8), 0);

    /* RxStatus through the whole IPC hop */
    EXPECT_EQ(cap.rx_status[0], 0u);
    EXPECT_EQ(cap.rx_status[1], ECUIO_PT_CAN_29BIT_ID);
    EXPECT_EQ(cap.rx_status[2], 0u);

    /* The client passes the vendor's stamp through; the transport
     * makes it session time (j2534_clock.h). */
    EXPECT_EQ(cap.timestamps[0], 1000u);
    EXPECT_EQ(cap.timestamps[1], 2000u);
    EXPECT_EQ(cap.timestamps[2], 3000u);

    /* Teardown: SHUTDOWN, bounded wait, job-object kill. */
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

TEST(J2534SurrogateTest, ReadPumpIsGatedOnConnect)
{
    /* Frame injected PRE-connect parks in the fake's FIFO; the child's
     * read gate (no channel connected yet) must hold it there. It
     * arrives only after CONNECT: the fake's InjectFrame and ReadMsgs
     * share one queue,
     * so parking is real, not an artifact of separate containers. */
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);

    SurCapture cap;
    const unsigned char f0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, f0, 8, 500, 0), ECUIO_OK);

    /* Pre-connect: nothing may arrive. Predicate drains as it waits. */
    const bool early = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() > 0; },
        1500, 50);
    EXPECT_FALSE(early) << "frame flowed before CONNECT: pump gating broken";
    EXPECT_EQ(cap.count.load(), 0);

    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    /* Post-connect: the parked frame arrives. */
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        10000, 50);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(cap.count.load(), 1);
    EXPECT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    EXPECT_EQ(cap.timestamps[0], 500u);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* J2534 doesn't reserve 0 as a device or channel ID, so the child
 * tracks both with presence flags. With a fake that hands out 0 for
 * each, a frame still flows once connected, and a close with no
 * disconnect first still has the child disconnect channel 0 and close
 * device 0 as it shuts down. */
TEST(J2534SurrogateTest, DeviceAndChannelIdZeroAreRealIds)
{
    fake_control::Page* page = fake_control::create_for_this_process();
    ASSERT_NE(page, nullptr);
    fake_control::reset(page);
    struct PageReset {
        fake_control::Page* p;
        ~PageReset() { fake_control::reset(p); }
    } page_reset{page};
    ASSERT_TRUE(fake_control::set(page, "ECUIO_FAKE_ZERO_IDS", "1"));

    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0x55;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    EXPECT_EQ(channel, 0ul);

    const unsigned char f0[8] = {0, 0, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, f0, 8, 700, 0), ECUIO_OK);
    SurCapture cap;
    ASSERT_TRUE(wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        10000, 50)) << "channel 0 read as no channel";
    EXPECT_EQ(memcmp(cap.payloads[0], f0, 8), 0);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    EXPECT_EQ(fake_control::disconnects(page), 1u) << "channel 0 left connected";
    EXPECT_EQ(fake_control::closes(page), 1u) << "device 0 left open";
}

TEST(J2534SurrogateTest, DeadDllFailsFastAtOpen)
{
    /* A surrogate spawned with an unloadable DLL must die before (or
     * during) the handshake; the client must notice via the process
     * handle wait, not by burning the 8s deadline. ECUIO_ERR_LOAD_FAILED
     * is the contract. Wall-clock guard: 5s budget proves fail-fast. */
    const auto t0 = std::chrono::steady_clock::now();
    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open("C:\\does\\not\\exist.dll", fake_dll_arch(), &h),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate couldn't load the DLL (the Windows error is "
                 "in its session log, in %TEMP%)");
    EXPECT_EQ(h, nullptr);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5000);
}

TEST(J2534SurrogateTest, NullArgumentsRejected)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open(nullptr, fake_dll_arch(), &h), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_open(FAKE_J2534_DLL_PATH, fake_dll_arch(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_close(nullptr), ECUIO_OK);     /* free(nullptr) */

    const unsigned char req[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    const unsigned char id[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter = 0;
    ecuio_pt_rx_t rx = {};
    bool got = false;
    char fw[ECUIO_SUR_TEXT_MAX], dll[ECUIO_SUR_TEXT_MAX],
        api[ECUIO_SUR_TEXT_MAX];
    EXPECT_EQ(ecuio_j2534_sur_write_frame(nullptr, 0, req, 6, 0, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_ioctl(nullptr, 0, ECUIO_PT_IOCTL_CLEAR_RX_BUFFER,
                                    nullptr, 0, nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_read_version(nullptr, fw, dll, api),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(nullptr, 0, ECUIO_PT_PASS_FILTER,
                                               id, 4, id, 4, nullptr, 0, 0,
                                               &filter),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_read_msg(nullptr, 0, &rx, 0, &got),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_inject_test_frame(nullptr, req, 6, 0, 0),
              ECUIO_ERR_NULL_ARGUMENT);
}

/* The pipe is a trust boundary both ways. The client refuses a request
 * of the wrong shape before sending it, and the child refuses what a
 * broken client might still send: a channel op before OP_CONNECT, and
 * SCONFIG input that isn't whole pairs. Called on the client directly,
 * past the loader seam's own checks. */
TEST(J2534SurrogateTest, MalformedRequestsAreRefusedAtBothEnds)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    struct Closer {
        ecuio_j2534_sur_handle_t* h;
        ~Closer() { (void)ecuio_j2534_sur_close(h); }
    } closer{h};

    const unsigned char req[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    const unsigned char big[65] = {};
    const unsigned char mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char id[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter = 0;

    /* The child's: no channel is connected yet. */
    EXPECT_EQ(ecuio_j2534_sur_write_frame(h, 0, req, 6, 0, 0, nullptr),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, 0, ECUIO_PT_IOCTL_CLEAR_RX_BUFFER,
                                    nullptr, 0, nullptr, 0, nullptr),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(h, 0, ECUIO_PT_PASS_FILTER, mask,
                                               4, id, 4, nullptr, 0, 0,
                                               &filter),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(ecuio_j2534_sur_stop_msg_filter(h, 0, 0),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(ecuio_j2534_sur_disconnect_channel(h, 0), ECUIO_OK)
        << "the child refuses it too, but a disconnect's status is ignored";

    /* The client's: nothing crosses the pipe. */
    EXPECT_EQ(ecuio_j2534_sur_write_frame(h, 0, req, 0, 0, 0, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_write_frame(h, 0, big, 65, 0, 0, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, 0, ECUIO_PT_IOCTL_SET_CONFIG, big, 65,
                                    nullptr, 0, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, 0, ECUIO_PT_IOCTL_SET_CONFIG, nullptr, 8,
                                    nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, 0, ECUIO_PT_FLOW_CONTROL_FILTER, mask, 4, id, 4, nullptr,
                  4, 0, &filter),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(h, 0, ECUIO_PT_PASS_FILTER, mask,
                                               4, id, 4, big, 13, 0, &filter),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "flow control past 12 bytes";
    EXPECT_EQ(
        ecuio_j2534_sur_start_msg_filter(h, 0, ECUIO_PT_FLOW_CONTROL_FILTER,
                                         mask, 4, id, 4, id, 3, 0, &filter),
        ECUIO_ERR_INVALID_ARGUMENT)
        << "flow control a byte short";
    EXPECT_EQ(ecuio_j2534_sur_inject_test_frame(h, req, 0, 0, 0),
              ECUIO_ERR_INVALID_ARGUMENT);

    /* The child's, connected: 7 bytes are no whole SCONFIG pair. */
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_SET_CONFIG, big,
                                    7, nullptr, 0, nullptr),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_CLEAR_RX_BUFFER,
                                    nullptr, 0, nullptr, 0, nullptr),
              ECUIO_OK);
}

/* ================= WRITE / IOCTL / FILTER relay ================= */

TEST(J2534SurrogateTest, WriteRoundTripProducesEchoResponse)
{
    /* The full vendor request path minus a vehicle: WRITE crosses the
     * pipe, the surrogate materializes a PASSTHRU_MSG and calls
     * PassThruWriteMsgs, the fake records it and enqueues its
     * deterministic echo (byte 0 + 0x40), and our next OP_READ brings
     * it back over the pipe. Observable from EITHER architecture: no
     * fake-instance hooks involved. */
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    /* A realistic Mode-01 request shape (service 0x01, PID 0x0C RPM). */
    const unsigned char req[3] = {0x01, 0x0C, 0x00};
    const unsigned char expect[3] = {0x41, 0x0C, 0x00};   /* +0x40 */
    uint32_t num_written = 0;
    ASSERT_EQ(ecuio_j2534_sur_write_frame(h, channel, req, 3, 0, 1000,
                                            &num_written),
              ECUIO_OK);
    EXPECT_EQ(num_written, 1u);

    SurCapture cap;
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        10000, 50);
    ASSERT_TRUE(delivered);
    ASSERT_EQ(cap.count.load(), 1);
    EXPECT_EQ(cap.payloads[0][0], expect[0]);
    EXPECT_EQ(memcmp(cap.payloads[0], expect, 3), 0);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

TEST(J2534SurrogateTest, IoctlSetGetConfigRoundTrip)
{
    /* SET_CONFIG then GET_CONFIG across the flattened-SCONFIG wire:
     * the child materializes a real SCONFIG_LIST (pointer valid only
     * in ITS address space), stores, then GET must report the stored
     * value back through the re-flattened response payload. */
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    constexpr uint32_t kParam = 0x10;        /* arbitrary parameter id */
    constexpr uint32_t kValue = 0xBEEF;

    unsigned char in[8];
    put_pair(in, kParam, kValue);
    ASSERT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_SET_CONFIG,
                                    in, 8, nullptr, 0, nullptr),
              ECUIO_OK);

    unsigned char q[8], out[64];
    put_pair(q, kParam, 0);
    uint32_t out_len = 0;
    ASSERT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_GET_CONFIG,
                                    q, 8, out, sizeof(out), &out_len),
              ECUIO_OK);
    ASSERT_EQ(out_len, 8u);
    EXPECT_EQ(get_u32(out), kParam);
    EXPECT_EQ(get_u32(out + 4), kValue);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

TEST(J2534SurrogateTest, IoctlReadVbattReturnsFixedVoltage)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    unsigned char out[8] = {};
    uint32_t out_len = 0;
    ASSERT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    ASSERT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);   /* 13.8 V in mV, per the fake */

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* The caller's capacity bounds the copy, and out_len still reports
 * the whole result, so a cut shows: the loader passes callers' buffers
 * straight through (FakeFixture's twin, through the loader). */
TEST(J2534SurrogateTest, IoctlCopiesAtMostItsCapacity)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    unsigned char out[24];
    memset(out, 0xAA, sizeof(out));
    uint32_t out_len = 0;
    ASSERT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, 2, &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 4u);
    const uint32_t mv = 13800;
    EXPECT_EQ(memcmp(out, &mv, 2), 0);
    EXPECT_EQ(out[2], 0xAA) << "wrote past the capacity";

    unsigned char q[16];
    put_pair(q, 0x10, 0);
    put_pair(q + 8, 0x11, 0);
    memset(out, 0xAA, sizeof(out));
    ASSERT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_GET_CONFIG,
                                    q, 16, out, 12, &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 16u);
    EXPECT_EQ(get_u32(out), 0x10u);
    EXPECT_EQ(get_u32(out + 8), 0x11u);
    EXPECT_EQ(out[12], 0xAA) << "wrote past the capacity";

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* PassThruReadVersion through the child (OP_READ_VERSION): the three
 * strings in their own reply fields. The fake answers only for the
 * device it opened, so the child must pass that, not the channel. */
TEST(J2534SurrogateTest, ReadVersionRelaysTheThreeStrings)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    char fw[80], dll[80], api[80];
    ASSERT_EQ(ecuio_j2534_sur_read_version(h, fw, dll, api), ECUIO_OK);
    EXPECT_STREQ(fw, "fake-fw 1.0");
    EXPECT_STREQ(dll, "fake_j2534 0.3");
    EXPECT_STREQ(api, "04.04");

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

TEST(J2534SurrogateTest, IoctlWhitelistRefusesUnsupportedIds)
{
    /* Both ends must refuse an out-of-contract ioctl: the loader seam
     * returns NOT_IMPLEMENTED before the pipe; the CHILD whitelist
     * (patched into the OP_IOCTL handler) must independently fail it
     * if a request somehow arrives: it answers STATUS_FAILED, as a
     * strict vendor would, which crosses the pipe as
     * ECUIO_ERR_VENDOR_REFUSED. Driving the client directly (this
     * suite) exercises the child side. */
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    /* 0x05 = FIVE_BAUD_INIT: pointer-bearing, never shapable. */
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, 0x05,
                                    nullptr, 0, nullptr, 0, nullptr),
              ECUIO_ERR_VENDOR_REFUSED);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

TEST(J2534SurrogateTest, FilterStartStopLifecycle)
{
    /* Register a PASS filter for a 29-bit-style 4-byte pattern, stop
     * it, then stop it AGAIN: the strict-vendor stop-twice refusal
     * must surface as an error, proving vendor status isn't being
     * flattened into silent success anywhere on the relay. */
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    const unsigned char mask[4]    = {0x00, 0x00, 0x07, 0xFF};
    const unsigned char pattern[4]  = {0x00, 0x00, 0x07, 0xE8};

    unsigned long filter_id = 0;
    ASSERT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER,
                  mask, 4, pattern, 4,
                  nullptr, 0, 0, &filter_id),
              ECUIO_OK);
    EXPECT_GE(filter_id, 1ul);

    /* Malformed shapes are refused client-side before the pipe. */
    unsigned long junk = 0;
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER,
                  mask, 4, pattern, 3,     /* length mismatch */
                  nullptr, 0, 0, &junk),
              ECUIO_ERR_INVALID_ARGUMENT);
    /* Flow control on a non-FC filter is also refused. */
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER,
                  mask, 4, pattern, 4,
                  pattern, 4, 0, &junk),
              ECUIO_ERR_INVALID_ARGUMENT);

    EXPECT_EQ(ecuio_j2534_sur_stop_msg_filter(h, channel, filter_id), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_stop_msg_filter(h, channel, filter_id),
              ECUIO_ERR_VENDOR_REFUSED);    /* already stopped: strict vendor */

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* A mid-session vendor call that outlives the client's 3s transact
 * timeout leaves its response in flight. The NEXT request must
 * recover (consume the stale response), not desync against it.
 * Without that, this second, valid ioctl fails on a request-ID
 * mismatch. Knob armed BEFORE open (the child snapshots the environment at
 * spawn; SetEnvironmentVariableA afterwards does not propagate) and
 * restored before any later test spawns. */
TEST(J2534SurrogateTest, TimedOutOpDoesNotDesyncNextRequest)
{
    SetEnvironmentVariableA("ECUIO_FAKE_IOCTL_DELAY_MS", "3500");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    SetEnvironmentVariableA("ECUIO_FAKE_IOCTL_DELAY_MS", nullptr);

    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    /* Vendor ioctl sleeps 3.5s: 0.5s past the 3s transact timeout:
     * parent gives up at ~3s, response lands at ~3.5s. */
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED);

    /* The differentiator: with the desync present this FAILS (reads
     * the stale response, ID mismatch); with recovery it succeeds. */
    out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* A vendor PassThruOpen that blocks (adapter absent) must
 * fail at the handshake timeout with the child ALIVE, not wedge and
 * not require process-death to detect. The child inherits the parent's
 * environment, so arming the fake-side sleep from here works in the
 * cross-arch configuration too (x64 test -> x86 child). */
TEST(J2534SurrogateTest, BlockedVendorOpenFailsAtHandshakeTimeout)
{
    SetEnvironmentVariableA("ECUIO_FAKE_OPEN_DELAY_MS", "60000");
    SetEnvironmentVariableA("ECUIO_SUR_HANDSHAKE_MS", "500");
    const auto t0 = std::chrono::steady_clock::now();

    ecuio_j2534_sur_handle_t* h = nullptr;
    const ecuio_status_t rc = ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h);
    const std::string detail = ecuio_last_error_detail();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    /* Restore BEFORE assertions: a failing EXPECT would otherwise leak
     * the knobs into later tests in this process (sequential runs). */
    SetEnvironmentVariableA("ECUIO_FAKE_OPEN_DELAY_MS", nullptr);
    SetEnvironmentVariableA("ECUIO_SUR_HANDSHAKE_MS", nullptr);

    /* The vendor's call never returned: the driver's doing, not the
     * surrogate's. */
    EXPECT_EQ(rc, ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(detail, "PassThruOpen didn't return within 500 ms; the "
                      "driver may be waiting for its adapter");
    EXPECT_EQ(h, nullptr);
    /* Failed at the SHRUNK window (~0.5s), nowhere near the 8s default:
     * if this approaches 8s, an env knob stopped being honored. */
    EXPECT_LT(elapsed, 3000);
}

/* Shares read access with the session's writers. "" if it can't open.
 * The path is UTF-8 (ecuio_j2534_sur_log_path). */
static std::string read_session_log(const std::string& path)
{
    std::string out;
    wchar_t wpath[MAX_PATH + 64] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath,
                            static_cast<int>(MAX_PATH + 64)) <= 0) {
        return out;
    }
    HANDLE f = CreateFileW(wpath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(f);
    return out;
}

/* The client's own lines go into the session log beside the child's, so a GUI launch (no stderr) keeps
 * both halves in one file. Each side writes whole lines, so every line
 * starts "[sur" ("[sur ..." from the client, "[surrogate ..." from the
 * child). */
TEST(J2534SurrogateTest, ClientAndChildLinesShareTheSessionLog)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);
    /* Close first: the child exits, so the file is complete. */
    ASSERT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    ASSERT_FALSE(path.empty()) << "the session ran without a log";

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("[sur "), std::string::npos) << log;
    EXPECT_NE(log.find("open: child pid"), std::string::npos) << log;
    EXPECT_NE(log.find("surrogate '"), std::string::npos)
        << "the log must name the surrogate exe it spawned:\n" << log;
    EXPECT_NE(log.find("[surrogate "), std::string::npos) << log;
    EXPECT_NE(log.find("pipe connected"), std::string::npos) << log;

    int lines = 0;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t eol = log.find('\n', pos);
        if (eol == std::string::npos) eol = log.size();
        std::string line = log.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            ++lines;
            EXPECT_EQ(line.rfind("[sur", 0), 0u) << "spliced line: " << line;
        }
        pos = eol + 1;
    }
    EXPECT_GE(lines, 4);   /* client spawn line + child startup stages */
}

namespace {
/* Sets an environment variable for one scope. The child snapshots its
 * environment at spawn, so arm before open; the destructor clears the
 * variable even when an ASSERT returns early. */
struct EnvArm {
    const char* name;
    EnvArm(const char* n, const char* v) : name(n) { SetEnvironmentVariableA(n, v); }
    ~EnvArm() { SetEnvironmentVariableA(name, nullptr); }
    EnvArm(const EnvArm&) = delete;
    EnvArm& operator=(const EnvArm&) = delete;
};

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
}  // namespace

/* The surrogate's variable (ECUIO_SURROGATE_EXE, or _X64_EXE)
 * relocates the surrogate and nothing more: a
 * relative path, another program, or a missing file is refused before
 * anything spawns. */
TEST(J2534SurrogateTest, SurrogateExeOverrideIsRestricted)
{
    char self[MAX_PATH] = {};
    ASSERT_GT(GetModuleFileNameA(nullptr, self, MAX_PATH), 0u);
    const std::string too_long =
        "C:/" + std::string(MAX_PATH, 'a') + "/" + surrogate_name();
    const std::string missing =
        std::string("C:/ecuio-definitely-missing/") + surrogate_name();
    for (const char* exe :
         {surrogate_name(), static_cast<const char*>(self), missing.c_str(),
          too_long.c_str()}) {
        EnvSwap swap(surrogate_env(), exe);
        ecuio_j2534_sur_handle_t* h = nullptr;
        EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_ERR_SURROGATE_FAILED) << exe;
        EXPECT_EQ(h, nullptr) << exe;
    }
}

/* A surrogate that exits before it opens the pipe: the client sees the
 * process end during the handshake and names its exit code. Not ours:
 * whoami.exe under the surrogate's name, which refuses the surrogate's
 * two arguments and exits with 1. */
TEST(J2534SurrogateTest, SurrogateExitingAtStartupNamesItsExitCode)
{
    char sys[MAX_PATH] = {};
    char tmp[MAX_PATH] = {};
    ASSERT_GT(GetSystemDirectoryA(sys, MAX_PATH), 0u);
    ASSERT_GT(GetTempPathA(MAX_PATH, tmp), 0u);
    struct Cleanup {
        std::string dir, exe;
        ~Cleanup()
        {
            DeleteFileA(exe.c_str());
            RemoveDirectoryA(dir.c_str());
        }
    } cleanup;
    cleanup.dir = std::string(tmp) + "ecuio-foreign-surrogate-" +
                  std::to_string(GetCurrentProcessId());
    cleanup.exe = cleanup.dir + "\\" + surrogate_name();
    ASSERT_TRUE(CreateDirectoryA(cleanup.dir.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS);
    ASSERT_TRUE(CopyFileA((std::string(sys) + "\\whoami.exe").c_str(),
                          cleanup.exe.c_str(), FALSE));

    EnvSwap swap(surrogate_env(), cleanup.exe.c_str());
    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate exited during startup with code 1");
    EXPECT_EQ(h, nullptr);
}

/* The client and the child each carry a fingerprint of the shared
 * headers, so trees built from different headers refuse each other at
 * OP_OPEN. ECUIO_SUR_TEST_FINGERPRINT stands in for a child from
 * another build. The refusal comes before the vendor is called: the
 * fake's open is armed to block for 60 s, so a child that called
 * PassThruOpen would hold the open until the 8 s handshake deadline. */
TEST(J2534SurrogateTest, MismatchedFingerprintRefusesOpenBeforeVendor)
{
    EnvArm fingerprint("ECUIO_SUR_TEST_FINGERPRINT", "1");
    EnvArm blocked_open("ECUIO_FAKE_OPEN_DELAY_MS", "60000");
    const auto t0 = std::chrono::steady_clock::now();

    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(h, nullptr);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5000) << "the child called PassThruOpen";
}

/* The DLL path reaches the child as UTF-16 (wmain), so a path no ANSI
 * code page can spell loads there too. */
TEST(J2534SurrogateTest, NonAsciiDllPathLoadsInTheChild)
{
    NonAsciiCopy copy(fake_dll_path());
    ASSERT_TRUE(copy.ok) << "couldn't stage the fake under a non-ASCII path";
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(copy.utf8, fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    EXPECT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);     /* the child exits, so the copy can go */
}

/* ECUIO_SUR_LOG_FRAMES=1 puts every message written and read into the
 * session log verbatim: status, flags, the vendor timestamp and the
 * bytes. Without it the log keeps its counts and status changes only. */
TEST(J2534SurrogateTest, FrameLoggingRecordsPayloadsVerbatimWhenEnabled)
{
    const uint8_t req[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    const uint8_t rsp[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    std::string logs[2];   /* [0] disabled, [1] enabled */

    for (int enabled = 1; enabled >= 0; --enabled) {
        EnvArm arm("ECUIO_SUR_LOG_FRAMES", enabled ? "1" : nullptr);
        ecuio_j2534_sur_handle_t* h = nullptr;
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
        unsigned long channel = 0;
        ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

        uint32_t written = 0;
        ASSERT_EQ(ecuio_j2534_sur_write_frame(h, channel, req, sizeof(req),
                                              ECUIO_PT_TX_ISO15765_FRAME_PAD,
                                              0, &written), ECUIO_OK);
        ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, rsp, sizeof(rsp),
                                                    4242, 0), ECUIO_OK);
        /* The fake returns one message per read: the write's echo, then
         * the injected frame. Extra reads come back empty. */
        for (int i = 0; i < 4; ++i) {
            ecuio_pt_rx_t f = {};
            bool got = false;
            (void)ecuio_j2534_sur_read_msg(h, channel, &f, 20, &got);
        }

        const std::string path = ecuio_j2534_sur_log_path(h);
        ASSERT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);     /* child exits: log complete */
        ASSERT_FALSE(path.empty());
        logs[enabled] = read_session_log(path);
    }

    const std::string& on = logs[1];
    EXPECT_NE(on.find("frame logging on (ECUIO_SUR_LOG_FRAMES=1)"), std::string::npos) << on;
    EXPECT_NE(on.find("OP_WRITE frame: status 0, tx 0x00000040, len 6: 00 00 07 E0 01 0C"),
              std::string::npos) << on;
    EXPECT_NE(on.find("ts 4242, len 8: 00 00 07 E8 41 0C 1F 40"), std::string::npos) << on;

    const std::string& off = logs[0];
    EXPECT_EQ(off.find(" frame: "), std::string::npos) << "frame lines without the knob:\n" << off;
    EXPECT_NE(off.find("OP_WRITE: status 0"), std::string::npos) << off;
}

/* A child that misses two deadlines in a row (a transaction's, then the
 * wait for its late reply) is wedged, not slow. Later transactions fail
 * at once instead of each costing another 3 s, which a teardown would
 * otherwise pay per filter, for the disconnect and for the close, and
 * close kills the child without waiting for a polite exit. A slow vendor
 * that answers inside the second wait still recovers
 * (TimedOutOpDoesNotDesyncNextRequest). The fake's one-shot ioctl stall
 * wedges the single-threaded child for 20 s. */
TEST(J2534SurrogateTest, UnresponsiveChildFailsFastAfterTwoMissedDeadlines)
{
    using clock = std::chrono::steady_clock;
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        EnvArm arm("ECUIO_FAKE_IOCTL_DELAY_MS", "20000");
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    /* First deadline: the ioctl outlives the 3 s transaction timeout. */
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED);

    /* Second: its late reply doesn't come within another timeout. */
    EXPECT_EQ(ecuio_j2534_sur_disconnect_channel(h, channel),
              ECUIO_ERR_SURROGATE_FAILED);

    /* From here, every transaction fails at once. */
    auto t0 = clock::now();
    EXPECT_EQ(ecuio_j2534_sur_stop_msg_filter(h, channel, 1),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_LT(clock::now() - t0, std::chrono::milliseconds(500));

    /* Close kills the child instead of waiting 1.5 s twice. */
    t0 = clock::now();
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    EXPECT_LT(clock::now() - t0, std::chrono::milliseconds(1000));

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("child unresponsive"), std::string::npos) << log;
}

/* A reply the client never asked for (the child answers OP_CONNECT
 * twice, ECUIO_SUR_TEST_REPLY_TWICE) leaves the pipe one reply behind:
 * the next transaction reads the reply before its own. That's the pipe
 * out of step for good, so the handle turns unresponsive: later calls
 * fail at once without reaching the child, and close kills it. Treated
 * as a one-off, every later call would still be sent and fail on the
 * reply before its own. */
TEST(J2534SurrogateTest, RequestIdMismatchMakesTheChildUnresponsive)
{
    using clock = std::chrono::steady_clock;
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        char twice[8];
        snprintf(twice, sizeof(twice), "%u",
                 static_cast<unsigned>(ECUIO_SUR_OP_CONNECT));
        EnvArm arm("ECUIO_SUR_TEST_REPLY_TWICE", twice);
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED) << "read the connect's second reply";

    const auto t0 = clock::now();
    const uint8_t msg[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    uint32_t written = 0;
    EXPECT_EQ(ecuio_j2534_sur_write_frame(h, channel, msg, sizeof(msg), 0, 0,
                                          &written),
              ECUIO_ERR_SURROGATE_FAILED);

    /* So does every other call, the connect-path ones with a detail. */
    unsigned long again = 0;
    EXPECT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &again),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate stopped answering during PassThruConnect");
    const unsigned char mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char id[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter = 0;
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(h, channel, ECUIO_PT_PASS_FILTER,
                                               mask, 4, id, 4, nullptr, 0, 0,
                                               &filter),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate stopped answering during "
                 "PassThruStartMsgFilter");
    char fw[ECUIO_SUR_TEXT_MAX], dll[ECUIO_SUR_TEXT_MAX],
        api[ECUIO_SUR_TEXT_MAX];
    EXPECT_EQ(ecuio_j2534_sur_read_version(h, fw, dll, api),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(ecuio_j2534_sur_inject_test_frame(h, msg, sizeof(msg), 0, 0),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_LT(clock::now() - t0, std::chrono::milliseconds(500));
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("DESYNC"), std::string::npos) << log;
    EXPECT_NE(log.find("child unresponsive"), std::string::npos) << log;
    EXPECT_EQ(log.find("OP_WRITE:"), std::string::npos)
        << "a write reached the child after the mismatch\n" << log;
}

/* A read the child answers past its deadline: the late reply, which
 * carries a frame, is drained and dropped before the next request, the
 * drop is logged with its count, and the next read is paired with its
 * own reply. The fake's one-shot read stall holds the first ReadMsgs
 * 1.5 s, past the 50 ms read's 550 ms deadline and well inside the
 * next transaction's 3 s. */
TEST(J2534SurrogateTest, LateReadReplyIsDroppedAndLogged)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        EnvArm arm("ECUIO_FAKE_READ_DELAY_MS", "1500");
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    const unsigned char late[8] = {0xA1, 1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, late, 8, 100, 0), ECUIO_OK);
    ecuio_pt_rx_t f = {};
    bool got = false;
    EXPECT_EQ(ecuio_j2534_sur_read_msg(h, 0, &f, 50, &got),
              ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_FALSE(got);

    /* This transaction drains the late reply first. */
    const unsigned char next[8] = {0xB2, 1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, next, 8, 200, 0), ECUIO_OK);
    SurCapture cap;
    ASSERT_TRUE(wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        5000, 50));
    EXPECT_EQ(cap.count.load(), 1);
    EXPECT_EQ(memcmp(cap.payloads[0], next, 8), 0)
        << "the late reply's frame was delivered";

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("drain_pending: dropped 1 frame(s) from the late reply"),
              std::string::npos) << log;
}

/* The session log is opened CREATE_ALWAYS with FILE_SHARE_READ only,
 * so while one session's child holds it, a second concurrent
 * session's open fails. That session must get a per-spawn name, not
 * run its child with no log at all. Holds
 * whether or not something else (a running app) already has the fixed
 * name: then both sessions get per-spawn names. */
TEST(J2534SurrogateTest, SecondConcurrentSessionGetsItsOwnLog)
{
    ecuio_j2534_sur_handle_t* a = nullptr;
    ecuio_j2534_sur_handle_t* b = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &a), ECUIO_OK);
    const ecuio_status_t rb = ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &b);
    const std::string path_a = ecuio_j2534_sur_log_path(a);
    const std::string path_b = ecuio_j2534_sur_log_path(b);
    ecuio_j2534_sur_close(b);
    ecuio_j2534_sur_close(a);

    ASSERT_EQ(rb, ECUIO_OK);
    EXPECT_FALSE(path_a.empty());
    EXPECT_FALSE(path_b.empty()) << "the second session ran without a log";
    EXPECT_NE(path_a, path_b);
    EXPECT_NE(read_session_log(path_b).find("pipe connected"),
              std::string::npos);

    /* Per-spawn names are this test's litter; the fixed name stays. */
    for (const std::string* p : { &path_a, &path_b }) {
        if (p->find("ecuio-surrogate-") != std::string::npos) {
            DeleteFileA(p->c_str());
        }
    }
}

namespace {
std::wstring to_wide(const std::string& utf8)
{
    wchar_t buf[MAX_PATH + 64] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, MAX_PATH + 64);
    return buf;
}

std::string to_utf8(const std::wstring& wide)
{
    char buf[3 * (MAX_PATH + 64)] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buf, sizeof(buf),
                        nullptr, nullptr);
    return buf;
}
}  // namespace

/* Each session takes the fixed log name, and the log it replaces is
 * kept as ecuio-surrogate.<last write>.log, so a run of connects keeps
 * every session's log. Skips when something else (a running app, a
 * parallel test) holds the fixed name: sessions then get per-spawn
 * names and nothing is replaced. */
TEST(J2534SurrogateTest, EarlierSessionLogIsKept)
{
    const std::string fixed_name = "ecuio-surrogate.log";
    ecuio_j2534_sur_handle_t* a = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &a), ECUIO_OK);
    const std::string path_a = ecuio_j2534_sur_log_path(a);
    ASSERT_EQ(ecuio_j2534_sur_close(a), ECUIO_OK);
    if (path_a.size() < fixed_name.size() ||
        path_a.compare(path_a.size() - fixed_name.size(), fixed_name.size(),
                       fixed_name) != 0) {
        DeleteFileW(to_wide(path_a).c_str());   /* a per-spawn name: litter */
        GTEST_SKIP() << "the fixed log name is held elsewhere";
    }
    const std::string log_a = read_session_log(path_a);
    ASSERT_NE(log_a.find("open: child pid"), std::string::npos) << log_a;

    ecuio_j2534_sur_handle_t* b = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &b), ECUIO_OK);
    const std::string path_b = ecuio_j2534_sur_log_path(b);
    ASSERT_EQ(ecuio_j2534_sur_close(b), ECUIO_OK);
    EXPECT_EQ(path_b, path_a) << "the second session should take the fixed name";
    EXPECT_NE(read_session_log(path_b), log_a);

    /* The first session's log, byte for byte, under a kept name. */
    const std::wstring dir = to_wide(path_a.substr(0, path_a.size() - fixed_name.size()));
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"ecuio-surrogate.*.log").c_str(), &fd);
    ASSERT_NE(find, INVALID_HANDLE_VALUE) << "no kept log";
    std::wstring kept;
    do {
        if (read_session_log(to_utf8(dir + fd.cFileName)) == log_a) {
            kept = fd.cFileName;
        }
    } while (kept.empty() && FindNextFileW(find, &fd));
    FindClose(find);
    ASSERT_FALSE(kept.empty()) << "the first session's log wasn't kept";
    /* "ecuio-surrogate." + "YYYYMMDD-HHMMSS-mmm" + ".log" */
    EXPECT_EQ(kept.size(), 16u + 19u + 4u) << to_utf8(kept);
}

/* Per J2534-1, a read asking for more messages than arrived, with a
 * nonzero timeout, returns ERR_TIMEOUT TOGETHER WITH those messages.
 * The spec-mode fake emulates exactly that; the client must deliver
 * the batch and only judge the status when the batch is empty. A
 * client that judged the status first would discard the batch and
 * render a talking bus invisible. Knob armed BEFORE
 * open: the child snapshots the environment at spawn (same
 * discipline as ECUIO_FAKE_IOCTL_DELAY_MS). */
TEST(J2534SurrogateTest, SpecTimeoutReadDeliversPartialBatch)
{
    SetEnvironmentVariableA("ECUIO_FAKE_SPEC_READ_SEMANTICS", "1");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    const unsigned char f0[8] = {0x41, 0x0C, 0x1F, 0x40, 0, 0, 0, 0};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, f0, 8, 4242, 0),
              ECUIO_OK);

    /* A conforming vendor holds a short read for its timeout. */
    const auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t).count();
    };
    ecuio_pt_rx_t f1 = {};
    bool got1 = false;
    auto t = std::chrono::steady_clock::now();
    const ecuio_status_t st1 =
        ecuio_j2534_sur_read_msg(h, 0, &f1, 50, &got1);
    const auto held1 = ms_since(t);

    ecuio_pt_rx_t f2 = {};
    bool got2 = false;
    t = std::chrono::steady_clock::now();
    const ecuio_status_t st2 =
        ecuio_j2534_sur_read_msg(h, 0, &f2, 50, &got2);
    const auto held2 = ms_since(t);

    /* Restore BEFORE assertions (knob-leak discipline) */
    SetEnvironmentVariableA("ECUIO_FAKE_SPEC_READ_SEMANTICS", nullptr);

    /* Child asks for 16 with a 50 ms timeout; one queued -> spec mode
     * returns 1 message WITH ERR_TIMEOUT. The client must deliver it. */
    EXPECT_EQ(st1, ECUIO_OK);
    EXPECT_TRUE(got1);
    EXPECT_EQ(memcmp(f1.data, f0, 8), 0);
    EXPECT_EQ(f1.data_size, 8u);
    EXPECT_EQ(f1.timestamp, 4242u);
    EXPECT_GE(held1, 45) << "the short batch came back before its timeout";

    /* Empty queue, nonzero timeout: quiet, not delivered, not hard. */
    EXPECT_EQ(st2, ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_FALSE(got2);
    EXPECT_GE(held2, 45) << "the empty read came back before its timeout";

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* A read's reply carries its messages whole, however long: sixteen of
 * the fake's 0xF1A1 replies, each ISO 15765-2's longest message with
 * its CAN ID in front (4099 bytes), come back from one OP_READ (the
 * fake's spec mode batches; by default it returns one per read), about
 * 66 KB across the pipe, every byte as the vendor gave it. */
TEST(J2534SurrogateTest, ABatchOfLongMessagesCrossesThePipeWhole)
{
    EnvArm spec("ECUIO_FAKE_SPEC_READ_SEMANTICS", "1");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const unsigned char mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char pattern[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter_id = 0;
    ASSERT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER, mask, 4, pattern, 4,
                  nullptr, 0, 0, &filter_id),
              ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    const unsigned char req[7] = {0x00, 0x00, 0x07, 0xE0, 0x22, 0xF1, 0xA1};
    for (int i = 0; i < ECUIO_SUR_MAX_FRAMES_PER_READ; ++i) {
        uint32_t written = 0;
        ASSERT_EQ(ecuio_j2534_sur_write_frame(h, channel, req, sizeof(req),
                                              ECUIO_PT_TX_ISO15765_FRAME_PAD,
                                              0, &written), ECUIO_OK) << i;
    }
    int whole = 0;
    for (int i = 0; i < ECUIO_SUR_MAX_FRAMES_PER_READ; ++i) {
        ecuio_pt_rx_t m = {};
        bool got = false;
        ASSERT_EQ(ecuio_j2534_sur_read_msg(h, channel, &m, 50, &got),
                  ECUIO_OK) << i;
        ASSERT_TRUE(got) << i;
        bool same = m.data_size == 4099u && m.data[3] == 0xE8 &&
                    m.data[4] == 0x62 && m.data[6] == 0xA1;
        for (uint32_t b = 7; same && b < m.data_size; ++b) {
            same = m.data[b] == static_cast<uint8_t>(b - 7);
        }
        whole += same ? 1 : 0;
    }
    EXPECT_EQ(whole, ECUIO_SUR_MAX_FRAMES_PER_READ);
    ASSERT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);   /* child exits: log complete */
    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("OP_READ: status 0, frames 16"), std::string::npos)
        << "the sixteen didn't come back in one read:\n" << log;
}

/* Count-trust rule, surrogate half: a vendor returning a hard error
 * without touching pNumMsgs leaves the caller's count dirty. Without
 * ecuio_trusted_count the child's static read batch would deliver up to
 * 16 stale/zero frames and the client reports the batch as healthy.
 * The knob is sticky in the child (env inherited at spawn; the
 * parent's clear never propagates), so recovery is not observable
 * here: instead the follow-up ioctl proves request correlation
 * survived the poisoned request. */
TEST(J2534SurrogateTest, HardErrorUntouchedCountDeliversNoFrames)
{
    SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", "7");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", nullptr);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    ecuio_pt_rx_t f = {};
    bool got = false;
    const ecuio_status_t st =
        ecuio_j2534_sur_read_msg(h, 0, &f, 50, &got);

    EXPECT_EQ(st, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_FALSE(got);

    /* No desync and a live channel after the poisoned request. */
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

#endif /* _WIN32 */
