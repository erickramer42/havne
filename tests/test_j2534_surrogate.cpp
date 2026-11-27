// File/layer: tests/test_j2534_surrogate.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534SurrogateTest (test_j2534_surrogate)
//
// ARCH note: in the x64 configure this is all-x64 (CI-valid IPC test);
// the true cross-arch case (x64 test process -> x86 surrogate -> x86
// fake) runs when ECUIO_SURROGATE_EXE points at Win32-tree artifacts.
// The write/ioctl/filter assertions are deliberately observable
// through RETURN VALUES and frames read back over the pipe alone (no
// fake-instance hooks) so they hold in BOTH architectures unchanged:
// an x64 test cannot reach into the x86 child's fake instance.

#include "test_j2534_surrogate_support.h"

#ifdef _WIN32

namespace {

/* Flattened-SCONFIG helper: one (Parameter, Value) u32 pair, host
 * endianness (memcpy): valid because every participant in this
 * suite is x86 or x64, both little-endian, and both sides of the
 * wire use memcpy-based (un)flattening. */
void put_pair(unsigned char* buf, uint32_t param, uint32_t value)
{
    memcpy(buf + 0, &param, 4);
    memcpy(buf + 4, &value, 4);
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
    const std::vector<unsigned char> past(ECUIO_SUR_LONG_MSG_MAX + 1);
    EXPECT_EQ(ecuio_j2534_sur_write_frame(
                  h, 0, past.data(), ECUIO_SUR_LONG_MSG_MAX + 1, 0, 0, nullptr),
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

TEST(J2534SurrogateTest, AWriteOfISOTPsLongestCrossesThePipeWhole)
{
    fake_control::Page* page = fake_control::create_for_this_process();
    ASSERT_NE(page, nullptr);
    fake_control::reset(page);
    struct PageReset {
        fake_control::Page* p;
        ~PageReset() { fake_control::reset(p); }
    } page_reset{page};
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    for (const uint32_t len :
         {uint32_t{ECUIO_SUR_MSG_MAX}, uint32_t{ECUIO_SUR_MSG_MAX + 1},
          uint32_t{ECUIO_SUR_LONG_MSG_MAX}}) {
        std::vector<unsigned char> msg(len);
        for (uint32_t i = 0; i < len; ++i) {
            msg[i] = static_cast<unsigned char>(i * 7 + len);
        }
        const uint32_t before = fake_control::writes(page);
        uint32_t written = 0;
        ASSERT_EQ(ecuio_j2534_sur_write_frame(h, channel, msg.data(), len, 0,
                                              1000, &written),
                  ECUIO_OK)
            << len;
        EXPECT_EQ(written, 1u);
        fake_control::Write w;
        ASSERT_TRUE(fake_control::recorded_write(page, before, &w)) << len;
        EXPECT_EQ(w.whole, len);
        EXPECT_EQ(w.sum, fake_control::write_sum(msg.data(), len)) << len;
    }
    const unsigned char req[3] = {0x01, 0x0C, 0x00};
    EXPECT_EQ(ecuio_j2534_sur_write_frame(h, channel, req, 3, 0, 1000, nullptr),
              ECUIO_OK)
        << "the pipe is in step after the long ones";
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

#endif // _WIN32
