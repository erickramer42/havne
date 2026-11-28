// File/layer: tests/test_j2534_health_overflow.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534HealthTest (test_j2534_health); Cross-arch

#include "test_j2534_health_support.h"

#ifdef _WIN32

namespace {

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

/* A reply longer than a CAN-FD frame arrives whole over the pipe:
 * 203 bytes, past the 64 a frame once held; ISO 15765-2's longest on
 * classic CAN, 4095; and one that fills PASSTHRU_MSG.Data, which a
 * frame holds only the first ECUIO_FRAME_DATA_MAX bytes of, marked
 * TRUNCATED. Through the surrogate each crosses the pipe whole. */
TEST_F(J2534HealthTest, LongRepliesArriveWhole)
{
    static const ecuio_channel_decl_t kChannel[] = {{0x7E8u, 0x7E0u, 0, 0, {}}};
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
        std::snprintf(defs[i].name, sizeof defs[i].name, "DID F1A%d", i);
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
    EXPECT_EQ(replies.flags[0], ECUIO_FRAME_FLAG_REASSEMBLED);
    EXPECT_TRUE(replies.filled[0]);
    EXPECT_EQ(replies.length[1], 4095u);
    EXPECT_EQ(replies.flags[1], ECUIO_FRAME_FLAG_REASSEMBLED);
    EXPECT_TRUE(replies.filled[1]);
    EXPECT_EQ(replies.length[2], ECUIO_FRAME_DATA_MAX);
    EXPECT_EQ(replies.flags[2],
              ECUIO_FRAME_FLAG_TRUNCATED | ECUIO_FRAME_FLAG_REASSEMBLED);
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
    ASSERT_TRUE(after_reads(2, 5000))
        << "the worker never read the indication and the empty read after";
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

/* A handle on the fake through the surrogate, its channel connected */
struct SeamHandle {
    ecuio_j2534_handle_t* h = nullptr;
    unsigned long ch = 0;
    ~SeamHandle() { (void)ecuio_j2534_close(h); }
};

/* At the loader seam, the fake as the VX: messages already queued come
 * back one a read, in order, each read's call returning at once. A read
 * that waited out its 500 ms, or a call held 10 ms for the rest of a
 * batch after each, takes the four past the bound */
TEST_F(J2534HealthTest, VxQueuedMessagesComeBackWithoutAHold)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    for (uint8_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(post_reply(page, i));
    }

    ecuio_pt_rx_t f = {};
    const auto from = std::chrono::steady_clock::now();
    for (uint8_t i = 1; i <= 4; ++i) {
        bool got = false;
        ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
        ASSERT_TRUE(got) << "message " << int{i};
        EXPECT_EQ(f.data[6], i);
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    EXPECT_LT(took, 30) << "a read held past its message";
}

/* How a read past its deadline called the vendor, from the fake's own
 * record (the control page), not the wall clock, which a loaded machine
 * stretches past any margin one scheduling slice wide (55 ms and
 * 70 ms against 55, with other sessions building on the machine): the stalled
 * call, then exactly one more, and neither asked to wait 0 ms, which
 * the VX holds 50 ms */
void expect_the_stalled_call_then_one_more(fake_control::Page* page)
{
    ASSERT_EQ(fake_control::reads(page), 2u)
        << "the stalled call, then one more after the deadline";
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t timeout_ms = 0;
        ASSERT_TRUE(fake_control::recorded_read_timeout(page, i, &timeout_ms));
        EXPECT_NE(timeout_ms, 0u) << "call " << i << " asked to wait 0 ms";
    }
}

/* A call that brings only an indication after the read's deadline (the
 * fake's first read stalls 30 ms, the read asks for 10) is followed by
 * one more, which waits 1 ms: the VX holds a call asked to wait 0 ms
 * for 50 ms, so the read would take about 80 */
TEST_F(J2534HealthTest, VxTheReadAfterAnIndicationPastTheDeadlineStillWaits)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_DELAY_MS", "30"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    const uint8_t tx_done[4] = {0x00, 0x00, 0x07, 0xE0};
    ASSERT_TRUE(
        fake_control::post(page, tx_done, sizeof(tx_done), 100,
                           ECUIO_PT_TX_MSG_TYPE | ECUIO_PT_TX_INDICATION));

    ecuio_pt_rx_t f = {};
    bool got = true;
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 10, &got), ECUIO_OK);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    EXPECT_FALSE(got) << "the indication is no frame";
    EXPECT_GE(took, 30) << "the fake's stall";
    expect_the_stalled_call_then_one_more(page);
}

/* A loss reported after a read's frame (an indication with
 * BUFFER_OVERFLOW behind the reply) flags the next read's frame, which
 * the call after the indication brings in the same read */
TEST_F(J2534HealthTest, VxALossAfterAReadsLastFrameFlagsTheNextRead)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    ASSERT_TRUE(post_reply(page, 1));
    ASSERT_TRUE(
        post_reply(page, 2, ECUIO_PT_TX_INDICATION | ECUIO_PT_BUFFER_OVERFLOW));

    ecuio_pt_rx_t f = {};
    bool got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 1u);
    EXPECT_EQ(f.rx_status & ECUIO_PT_BUFFER_OVERFLOW, 0u);

    ASSERT_TRUE(post_reply(page, 3));
    got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 3u);
    EXPECT_NE(f.rx_status & ECUIO_PT_BUFFER_OVERFLOW, 0u)
        << "the loss after the first read's frame was dropped";
}

/* A read whose call brought only an indication past the deadline still
 * makes its last call, so the reply right behind the indication comes
 * back in this read */
TEST_F(J2534HealthTest,
       VxTheReadAfterAnIndicationPastTheDeadlineTakesTheReplyBehindIt)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_DELAY_MS", "30"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    const uint8_t tx_done[4] = {0x00, 0x00, 0x07, 0xE0};
    ASSERT_TRUE(
        fake_control::post(page, tx_done, sizeof(tx_done), 100,
                           ECUIO_PT_TX_MSG_TYPE | ECUIO_PT_TX_INDICATION));
    ASSERT_TRUE(post_reply(page, 1));

    ecuio_pt_rx_t f = {};
    bool got = false;
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 10, &got), ECUIO_OK);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    ASSERT_TRUE(got) << "the read stopped at its deadline before its last call";
    EXPECT_EQ(f.data[6], 1u);
    EXPECT_GE(took, 30) << "the fake's stall";
    expect_the_stalled_call_then_one_more(page);
}

/* A loss carried from the read before into a read that fails: the
 * failure is reported at once, and the loss waits for the next frame */
TEST_F(J2534HealthTest, ALossCarriedIntoAFailedReadLetsTheFailureThrough)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    ASSERT_TRUE(post_reply(page, 1));
    ASSERT_TRUE(
        post_reply(page, 2, ECUIO_PT_TX_INDICATION | ECUIO_PT_BUFFER_OVERFLOW));
    ecuio_pt_rx_t f = {};
    bool got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 1u);

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    got = false;
    EXPECT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 10, &got),
              ECUIO_ERR_TRANSPORT_HARD_FAILURE)
        << "the carried loss hid the failure as an overflow";
    EXPECT_FALSE(got);

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));
    ASSERT_TRUE(post_reply(page, 3));
    got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 3u);
    EXPECT_NE(f.rx_status & ECUIO_PT_BUFFER_OVERFLOW, 0u)
        << "the loss was dropped with the failed read";
}

/* A loss carried when a channel disconnects isn't the next channel's.
 * The vendor returns two messages to a call for one, so the second is
 * lost after the read's frame and the loss is carried */
TEST_F(J2534HealthTest, AReconnectedChannelStartsWithNoLoss)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OVER_REPORT", "1"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    ASSERT_TRUE(post_reply(page, 1));
    ASSERT_TRUE(post_reply(page, 2));
    ecuio_pt_rx_t f = {};
    bool got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 1u);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OVER_REPORT", "0"));

    ASSERT_EQ(ecuio_j2534_disconnect_channel(s.h, s.ch), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    ASSERT_TRUE(post_reply(page, 3));
    got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 3u);
    EXPECT_EQ(f.rx_status & ECUIO_PT_BUFFER_OVERFLOW, 0u)
        << "the last channel's loss flagged the new channel's first frame";
}

/* A vendor that hands a call one message more than it asked: the extra
 * one is lost, and the frame after the call's carries the loss */
TEST_F(J2534HealthTest, AVendorThatReturnsMoreThanAskedFlagsTheLoss)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OVER_REPORT", "1"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    for (uint8_t i = 1; i <= 3; ++i) {
        ASSERT_TRUE(post_reply(page, i));
    }
    ecuio_pt_rx_t f = {};
    bool got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 1u);
    EXPECT_EQ(f.rx_status & ECUIO_PT_BUFFER_OVERFLOW, 0u);
    got = false;
    ASSERT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 500, &got), ECUIO_OK);
    ASSERT_TRUE(got);
    EXPECT_EQ(f.data[6], 3u) << "the message past the first call's count";
    EXPECT_NE(f.rx_status & ECUIO_PT_BUFFER_OVERFLOW, 0u)
        << "the message the clamp dropped went unflagged";
}

/* A vendor that answers a read that filled its count with ERR_TIMEOUT
 * (J2534-1's status for a timed read): its message is delivered, and
 * only an empty batch is judged by the status */
TEST_F(J2534HealthTest, FramesUnderErrTimeoutAreDelivered)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_SPEC_READ_SEMANTICS", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_TIMED_READ_TIMEOUT", "1"));
    SeamHandle s;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &s.h), ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_connect_channel(s.h, 0, 0, &s.ch), ECUIO_OK);
    ASSERT_TRUE(post_reply(page, 1));
    ecuio_pt_rx_t f = {};
    bool got = false;
    EXPECT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 50, &got), ECUIO_OK);
    ASSERT_TRUE(got) << "a message under ERR_TIMEOUT was judged by the status";
    EXPECT_EQ(f.data[6], 1u);
    got = true;
    EXPECT_EQ(ecuio_j2534_read_msg(s.h, s.ch, &f, 20, &got),
              ECUIO_ERR_VEHICLE_TIMEOUT)
        << "an empty batch under ERR_TIMEOUT";
    EXPECT_FALSE(got);
}

/* ERR_BUFFER_OVERFLOW with nothing read: no failure, and the next
 * message arrives flagged */
TEST_F(J2534HealthTest, AnEmptyOverflowReadFlagsTheNextMessage)
{
    ASSERT_NO_FATAL_FAILURE(start_session("?profile=raw&filter=0x7E8"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "0x12"));
    ASSERT_TRUE(after_reads(2, 5000))
        << "the worker never read under the forced status";
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
