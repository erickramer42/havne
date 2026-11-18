// File/layer: tests/test_socketcan_send.cpp | a request on its way out of a
//             requesting socketcan:// session (ADR-0066 section 2.4)
// Testing: UNIT: SocketCanSendTest (test_socketcan)

#include "socketcan_send.h"

#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr uint32_t kTx = 0x7E0; /* the request's ID */
constexpr uint32_t kRx = 0x7E8; /* the ECU's */

isotp_can can(uint32_t id, std::initializer_list<uint8_t> bytes)
{
    isotp_can f;
    f.id = id;
    for (uint8_t b : bytes) {
        f.data[f.len++] = b;
    }
    return f;
}

Bytes counting(uint32_t n)
{
    Bytes v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    return v;
}

Bytes bytes_of(const isotp_can& f)
{
    return Bytes(f.data, f.data + f.len);
}

/* 4 KB and 32 KB: kept off the stack, fresh for each case */
isotp_tx& fresh_tx()
{
    static isotp_tx t;
    t = isotp_tx();
    return t;
}

isotp_rx& fresh_rx()
{
    static isotp_rx r;
    r = isotp_rx();
    return r;
}

/* A send and the ECU's receiver across from it: what the ECU took whole */
struct Wire {
    socketcan_send s;
    isotp_tx& tx = fresh_tx();
    isotp_rx& ecu = fresh_rx();
    Bytes got;
    int frames = 0;

    socketcan_send_step start(const Bytes& pdu, int64_t now)
    {
        isotp_can f;
        const socketcan_send_step step =
            socketcan_send_start(s, tx, 2, 5, kTx, pdu.data(),
                                 static_cast<uint32_t>(pdu.size()), now, &f);
        take(step, f, now);
        return step;
    }

    socketcan_send_step next(bool echo_pending, int64_t now)
    {
        isotp_can f;
        const socketcan_send_step step =
            socketcan_send_next(s, tx, echo_pending, now, &f);
        take(step, f, now);
        return step;
    }

    /* The ECU's Flow Control, as the worker hands it over */
    socketcan_send_step flow(std::initializer_list<uint8_t> bytes, int64_t now)
    {
        socketcan_send_step step = socketcan_send_step::none;
        EXPECT_TRUE(socketcan_send_flow(s, tx, can(kRx, bytes), now, &step));
        return step;
    }

    void take(socketcan_send_step step, const isotp_can& f, int64_t now)
    {
        if (step != socketcan_send_step::frame &&
            step != socketcan_send_step::last) {
            return;
        }
        ++frames;
        EXPECT_EQ(f.id, kTx);
        EXPECT_EQ(f.len, 8) << "every frame is padded to 8 bytes";
        isotp_message m;
        if (isotp_take(ecu, f, now, &m) == isotp_result::complete) {
            got.assign(m.data, m.data + m.len);
        }
    }
};

const socketcan_send_step kNone = socketcan_send_step::none;
const socketcan_send_step kFrame = socketcan_send_step::frame;
const socketcan_send_step kLast = socketcan_send_step::last;
const socketcan_send_step kFailed = socketcan_send_step::failed;

} // namespace

TEST(SocketCanSendTest, ARequestOfUpToSevenBytesIsOneFrame)
{
    for (uint32_t n = 1; n <= 7; ++n) {
        Wire w;
        const Bytes pdu = counting(n);
        EXPECT_EQ(w.start(pdu, 0), kLast) << n;
        EXPECT_FALSE(w.s.active);
        EXPECT_EQ(w.got, pdu);
        EXPECT_EQ(w.next(false, 1000), kNone) << "nothing more is owed";
        EXPECT_EQ(socketcan_send_due_us(w.s, w.tx), INT64_MAX);
    }
    Wire w;
    isotp_can f;
    const uint8_t seed[2] = {0x27, 0x01};
    ASSERT_EQ(socketcan_send_start(w.s, w.tx, 0, -2, kTx, seed, 2, 0, &f),
              kLast);
    EXPECT_EQ(bytes_of(f), (Bytes{0x02, 0x27, 0x01, 0, 0, 0, 0, 0}));
}

TEST(SocketCanSendTest, AnEighteenByteRequestIsAFirstFrameAndTwoMore)
{
    Wire w;
    const Bytes key = counting(18);
    isotp_can f;
    ASSERT_EQ(socketcan_send_start(w.s, w.tx, 2, 5, kTx, key.data(), 18, 0, &f),
              kFrame);
    EXPECT_EQ(bytes_of(f), (Bytes{0x10, 0x12, key[0], key[1], key[2], key[3],
                                  key[4], key[5]}));
    w.take(kFrame, f, 0);
    EXPECT_TRUE(w.s.active);
    EXPECT_EQ(w.s.c, 2u);
    EXPECT_EQ(w.s.r, 5);
    /* Nothing more until the First Frame is echoed and the ECU's Flow
     * Control has come */
    EXPECT_EQ(w.next(true, 100), kNone) << "its echo is awaited";
    EXPECT_EQ(w.next(false, 200), kNone) << "the Flow Control is owed";
    EXPECT_EQ(w.flow({0x30, 0x00, 0x00}, 300), kNone);
    EXPECT_EQ(w.next(true, 300), kNone) << "still no echo";
    EXPECT_EQ(w.next(false, 300), kFrame);
    EXPECT_TRUE(w.s.active);
    EXPECT_EQ(w.next(true, 300), kNone)
        << "each frame waits for the echo of the one before";
    EXPECT_EQ(w.next(false, 400), kLast);
    EXPECT_FALSE(w.s.active);
    EXPECT_EQ(w.frames, 3);
    EXPECT_EQ(w.got, key) << "the ECU has the request whole";
    EXPECT_EQ(w.next(false, 500), kNone);
}

TEST(SocketCanSendTest, TheLongestTalkGoesWhole)
{
    Wire w;
    const Bytes pdu = counting(60);
    ASSERT_EQ(w.start(pdu, 0), kFrame);
    ASSERT_EQ(w.flow({0x30, 0x00, 0x00}, 0), kNone);
    socketcan_send_step step = kFrame;
    while (step == kFrame) {
        step = w.next(false, 0);
    }
    EXPECT_EQ(step, kLast);
    EXPECT_EQ(w.frames, 9) << "a First Frame's 6 bytes, then 54 in 8 frames";
    EXPECT_EQ(w.got, pdu);
    /* a length past ISO-TP's is refused, not cut to its low 16 bits */
    Wire far;
    isotp_can f;
    EXPECT_EQ(socketcan_send_start(far.s, far.tx, 0, 0, kTx, pdu.data(),
                                   65536u + 18u, 0, &f),
              kFailed);
    EXPECT_FALSE(far.s.active);
}

TEST(SocketCanSendTest, TheFlowControlsBlockSizeAndStminSetThePace)
{
    Wire w;
    const Bytes pdu = counting(60);
    ASSERT_EQ(w.start(pdu, 0), kFrame);
    /* two frames a block, 10 ms apart */
    ASSERT_EQ(w.flow({0x30, 0x02, 0x0A}, 1000), kNone);
    EXPECT_EQ(socketcan_send_due_us(w.s, w.tx), 1000) << "the first at once";
    EXPECT_EQ(w.next(false, 1000), kFrame);
    EXPECT_EQ(socketcan_send_due_us(w.s, w.tx), 11000);
    EXPECT_EQ(w.next(false, 10999), kNone) << "STmin has not passed";
    EXPECT_EQ(w.next(false, 11000), kFrame);
    /* the block is out: Flow Control again, within N_Bs */
    EXPECT_EQ(w.next(false, 30000), kNone);
    EXPECT_EQ(socketcan_send_due_us(w.s, w.tx),
              11000 + ECUIO_ISOTP_N_BS_US + 1);
    ASSERT_EQ(w.flow({0x30, 0x00, 0x00}, 30000), kNone)
        << "no block and no STmin from here";
    socketcan_send_step step = kFrame;
    int more = 0;
    while (step == kFrame) {
        step = w.next(false, 30000);
        ++more;
    }
    EXPECT_EQ(step, kLast);
    EXPECT_EQ(more, 6);
    EXPECT_EQ(w.got, pdu);
}

TEST(SocketCanSendTest, NoFlowControlWithinNBsFailsIt)
{
    Wire w;
    const Bytes pdu = counting(18);
    ASSERT_EQ(w.start(pdu, 5000), kFrame);
    EXPECT_EQ(socketcan_send_due_us(w.s, w.tx), 5000 + ECUIO_ISOTP_N_BS_US + 1);
    EXPECT_EQ(w.next(false, 5000 + ECUIO_ISOTP_N_BS_US), kNone)
        << "N_Bs to the microsecond";
    EXPECT_EQ(w.next(false, 5000 + ECUIO_ISOTP_N_BS_US + 1), kFailed);
    EXPECT_FALSE(w.s.active);
    EXPECT_EQ(w.frames, 1) << "only the First Frame went";
    /* a wait frame starts N_Bs again; the tenth in a row abandons it */
    Wire again;
    int64_t now = 0;
    ASSERT_EQ(again.start(pdu, now), kFrame);
    for (uint32_t i = 1; i < ECUIO_ISOTP_WAIT_MAX; ++i) {
        now += ECUIO_ISOTP_N_BS_US;
        ASSERT_EQ(again.flow({0x31, 0x00, 0x00}, now), kNone) << i;
        EXPECT_EQ(again.next(false, now + ECUIO_ISOTP_N_BS_US), kNone);
    }
    EXPECT_EQ(again.flow({0x31, 0x00, 0x00}, now), kFailed);
    EXPECT_FALSE(again.s.active);
}

TEST(SocketCanSendTest, AnOverflowOrAFlowControlItCantFollowFailsIt)
{
    const Bytes pdu = counting(18);
    {
        Wire w;
        ASSERT_EQ(w.start(pdu, 0), kFrame);
        EXPECT_EQ(w.flow({0x32, 0x00, 0x00}, 100), kFailed) << "overflow";
        EXPECT_FALSE(w.s.active);
        EXPECT_EQ(w.next(false, 200), kNone) << "and nothing more goes";
        EXPECT_EQ(w.frames, 1);
    }
    {
        Wire w;
        ASSERT_EQ(w.start(pdu, 0), kFrame);
        EXPECT_EQ(w.flow({0x30, 0x00}, 100), kFailed) << "two bytes";
        EXPECT_FALSE(w.s.active);
    }
    {
        Wire w;
        ASSERT_EQ(w.start(pdu, 0), kFrame);
        EXPECT_EQ(w.flow({0x3F, 0x00, 0x00}, 100), kFailed)
            << "a flow status ISO 15765-2 reserves";
        EXPECT_FALSE(w.s.active);
    }
}

TEST(SocketCanSendTest, OnlyAFlowControlDuringASendIsTheSenders)
{
    Wire w;
    socketcan_send_step step = kFrame;
    EXPECT_FALSE(
        socketcan_send_flow(w.s, w.tx, can(kRx, {0x30, 0x00, 0x00}), 0, &step))
        << "no send under way: the frame is the receiver's to judge";
    EXPECT_EQ(step, kNone);
    const Bytes pdu = counting(18);
    ASSERT_EQ(w.start(pdu, 0), kFrame);
    EXPECT_FALSE(socketcan_send_flow(
        w.s, w.tx, can(kRx, {0x03, 0x7F, 0x27, 0x78}), 0, &step))
        << "a reply's frame is a reply's";
    EXPECT_FALSE(socketcan_send_flow(
        w.s, w.tx, can(kRx, {0x10, 0x14, 0x62, 0xF1, 0x90}), 0, &step));
    EXPECT_TRUE(w.s.active);
    /* a second Flow Control with none due is taken and changes nothing */
    ASSERT_EQ(w.flow({0x30, 0x00, 0x00}, 100), kNone);
    EXPECT_EQ(w.flow({0x32, 0x00, 0x00}, 100), kNone);
    EXPECT_TRUE(w.s.active);
    EXPECT_EQ(w.next(false, 100), kFrame);
    EXPECT_EQ(w.next(false, 100), kLast);
    EXPECT_EQ(w.got, pdu);
}

TEST(SocketCanSendTest, OneSendAtATimeAndAnEndFreesTheSender)
{
    Wire w;
    const Bytes first = counting(18);
    Bytes second = counting(30);
    second[0] = 0x55;
    ASSERT_EQ(w.start(first, 0), kFrame);
    isotp_can f;
    EXPECT_EQ(
        socketcan_send_start(w.s, w.tx, 7, 9, kTx, second.data(), 30, 10, &f),
        kFailed)
        << "a send is under way";
    EXPECT_EQ(
        socketcan_send_start(w.s, w.tx, 7, 9, kTx, second.data(), 3, 10, &f),
        kFailed)
        << "even one frame waits";
    EXPECT_EQ(w.s.c, 2u) << "the send under way is untouched";
    EXPECT_EQ(w.s.r, 5);
    /* the frame before was never echoed: the worker ends the send */
    socketcan_send_end(w.s, w.tx);
    EXPECT_FALSE(w.s.active);
    EXPECT_EQ(socketcan_send_due_us(w.s, w.tx), INT64_MAX);
    EXPECT_EQ(w.next(false, 20), kNone);
    w.ecu = isotp_rx(); /* the ECU's own N_Cr gave the first one up */
    w.got.clear();
    ASSERT_EQ(w.start(second, 30), kFrame);
    ASSERT_EQ(w.flow({0x30, 0x00, 0x00}, 30), kNone);
    socketcan_send_step step = kFrame;
    while (step == kFrame) {
        step = w.next(false, 30);
    }
    EXPECT_EQ(step, kLast);
    EXPECT_EQ(w.got, second);
    /* a length ISO-TP can't carry never starts */
    EXPECT_EQ(
        socketcan_send_start(w.s, w.tx, 0, 0, kTx, second.data(), 0, 40, &f),
        kFailed);
    EXPECT_EQ(socketcan_send_start(w.s, w.tx, 0, 0, kTx, second.data(),
                                   ECUIO_ISOTP_MAX + 1u, 40, &f),
              kFailed);
    EXPECT_FALSE(w.s.active);
}
