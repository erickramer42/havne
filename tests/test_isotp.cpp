// File/layer: tests/test_isotp.cpp | ISO-TP's rules (ADR-0040 section 1)
// Testing: UNIT: IsoTpTest (test_socketcan)

#include "isotp.h"

#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

isotp_can can(uint32_t id, std::initializer_list<uint8_t> bytes)
{
    isotp_can f;
    f.id = id;
    for (uint8_t b : bytes) {
        f.data[f.len++] = b;
    }
    return f;
}

std::vector<uint8_t> counting(uint16_t n)
{
    std::vector<uint8_t> v(n);
    for (uint16_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    return v;
}

/* 32 KB each: kept off the stack, fresh per test */
isotp_rx& fresh_rx(bool timed)
{
    static isotp_rx r;
    r = isotp_rx();
    r.timed = timed;
    return r;
}

isotp_tx& fresh_tx()
{
    static isotp_tx t;
    t = isotp_tx();
    return t;
}

std::vector<uint8_t> bytes_of(const isotp_message& m)
{
    return std::vector<uint8_t>(m.data, m.data + m.len);
}

/* ---- The receiver ---------------------------------------------------- */

TEST(IsoTpTest, ASingleFrameIsAWholeMessagePaddingLeftOut)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    /* the message points into the frame, so the frame outlives it */
    const isotp_can f =
        can(0x7E8, {0x03, 0x41, 0x0D, 0x32, 0xAA, 0xAA, 0xAA, 0xAA});
    ASSERT_EQ(isotp_take(r, f, 0, &m), isotp_result::complete);
    EXPECT_EQ(m.id, 0x7E8u);
    EXPECT_EQ(bytes_of(m), (std::vector<uint8_t>{0x41, 0x0D, 0x32}));
    EXPECT_EQ(r.completed, 1u);
}

TEST(IsoTpTest, AnUnpaddedSingleFrameIsTakenWhenItHoldsItsLength)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x02, 0x41, 0x0D}), 0, &m),
              isotp_result::complete);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x03, 0x41, 0x0D}), 0, &m),
              isotp_result::malformed);
    EXPECT_EQ(r.malformed, 1u);
}

TEST(IsoTpTest, SingleFramesOfNoLengthOrPastSevenAreMalformed)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x00, 1, 2, 3, 4, 5, 6, 7}), 0, &m),
              isotp_result::malformed);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x08, 1, 2, 3, 4, 5, 6, 7}), 0, &m),
              isotp_result::malformed);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {}), 0, &m), isotp_result::malformed);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x40, 0, 0, 0, 0, 0, 0, 0}), 0, &m),
              isotp_result::malformed);
    EXPECT_EQ(r.malformed, 4u);
}

TEST(IsoTpTest, AFirstFrameAndItsConsecutiveFramesAreOneMessage)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    /* 09 02's VIN: 20 bytes, a First Frame and two Consecutive Frames */
    ASSERT_EQ(
        isotp_take(r, can(0x7E8, {0x10, 0x14, 0x49, 0x02, 0x01, 'A', 'B', 'C'}),
                   0, &m),
        isotp_result::first);
    ASSERT_EQ(isotp_take(r,
                         can(0x7E8, {0x21, 'D', 'E', 'F', 'G', 'H', 'I', 'J'}),
                         1000, &m),
              isotp_result::pending);
    ASSERT_EQ(isotp_take(r,
                         can(0x7E8, {0x22, 'K', 'L', 'M', 'N', 'O', 'P', 'Q'}),
                         2000, &m),
              isotp_result::complete);
    ASSERT_EQ(m.len, 20u);
    EXPECT_EQ(std::memcmp(m.data,
                          "\x49\x02\x01"
                          "ABCDEFGHIJKLMNOPQ",
                          20),
              0);
    EXPECT_EQ(r.first_frames, 1u);
    EXPECT_EQ(r.completed, 1u);
    EXPECT_EQ(r.dropped, 0u);
}

TEST(IsoTpTest, TheSequenceNumberWrapsFromFifteenToZero)
{
    isotp_rx& r = fresh_rx(false);
    const std::vector<uint8_t> msg = counting(6 + 7 * 17);
    isotp_tx& t = fresh_tx();
    isotp_can f;
    isotp_message m;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(),
                               static_cast<uint16_t>(msg.size()), 0, &f));
    ASSERT_EQ(isotp_take(r, f, 0, &m), isotp_result::first);
    ASSERT_EQ(isotp_tx_flow(t, isotp_flow_control(0x7E0), 0),
              isotp_flow_result::go);
    std::vector<uint8_t> seqs;
    isotp_result last = isotp_result::pending;
    while (isotp_tx_next(t, 0, &f) == isotp_tx_step::frame) {
        seqs.push_back(f.data[0] & 0x0F);
        last = isotp_take(r, f, 0, &m);
    }
    ASSERT_EQ(last, isotp_result::complete);
    ASSERT_EQ(seqs.size(), 17u);
    EXPECT_EQ(seqs[14], 15u);
    EXPECT_EQ(seqs[15], 0u);
    EXPECT_EQ(seqs[16], 1u);
    EXPECT_EQ(bytes_of(m), msg);
}

TEST(IsoTpTest, RepliesFromSeveralEcusInterleaveFrameByFrame)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x09, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    ASSERT_EQ(
        isotp_take(r, can(0x7E9, {0x10, 0x09, 11, 12, 13, 14, 15, 16}), 0, &m),
        isotp_result::first);
    ASSERT_EQ(isotp_take(r, can(0x7E9, {0x21, 17, 18, 19}), 0, &m),
              isotp_result::complete);
    EXPECT_EQ(m.id, 0x7E9u);
    EXPECT_EQ(bytes_of(m),
              (std::vector<uint8_t>{11, 12, 13, 14, 15, 16, 17, 18, 19}));
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x21, 7, 8, 9}), 0, &m),
              isotp_result::complete);
    EXPECT_EQ(m.id, 0x7E8u);
    EXPECT_EQ(bytes_of(m), (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(IsoTpTest, AFirstFrameMustFillItsFrameAndNeedASegmentedLength)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x09, 1, 2, 3, 4, 5}), 0, &m),
              isotp_result::malformed);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x07, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::malformed);
    /* the escape to a 32-bit length */
    EXPECT_EQ(
        isotp_take(r, can(0x7E8, {0x10, 0x00, 0, 0, 0x10, 0, 1, 2}), 0, &m),
        isotp_result::malformed);
    EXPECT_EQ(r.first_frames, 0u);
    EXPECT_EQ(r.malformed, 3u);
}

TEST(IsoTpTest, ALongestMessageFillsItsSlot)
{
    isotp_rx& r = fresh_rx(false);
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(ECUIO_ISOTP_MAX);
    isotp_can f;
    isotp_message m;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), ECUIO_ISOTP_MAX, 0, &f));
    EXPECT_EQ(f.data[0], 0x1F);
    EXPECT_EQ(f.data[1], 0xFF);
    ASSERT_EQ(isotp_take(r, f, 0, &m), isotp_result::first);
    ASSERT_EQ(isotp_tx_flow(t, isotp_flow_control(0x7E0), 0),
              isotp_flow_result::go);
    isotp_result last = isotp_result::pending;
    while (isotp_tx_next(t, 0, &f) == isotp_tx_step::frame) {
        last = isotp_take(r, f, 0, &m);
    }
    ASSERT_EQ(last, isotp_result::complete);
    EXPECT_EQ(bytes_of(m), msg);
}

TEST(IsoTpTest, AGapInTheSequenceDropsTheMessage)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x22, 7, 8, 9, 10, 11, 12, 13}), 0, &m),
              isotp_result::dropped);
    /* what follows has no message to join */
    EXPECT_EQ(
        isotp_take(r, can(0x7E8, {0x23, 14, 15, 16, 17, 18, 19, 20}), 0, &m),
        isotp_result::malformed);
    EXPECT_EQ(r.dropped, 1u);
    EXPECT_EQ(r.malformed, 1u);
    EXPECT_EQ(r.completed, 0u);
}

TEST(IsoTpTest, AConsecutiveFrameShortOfItsShareDropsTheMessage)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x21, 7, 8, 9}), 0, &m),
              isotp_result::dropped);
    EXPECT_EQ(r.dropped, 1u);
}

TEST(IsoTpTest, ANewFirstFrameOrSingleFrameOnABusyIdDropsTheOldMessage)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x09, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    EXPECT_EQ(r.dropped, 1u);
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x02, 0x41, 0x00}), 0, &m),
              isotp_result::complete);
    EXPECT_EQ(r.dropped, 2u);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x21, 7, 8, 9}), 0, &m),
              isotp_result::malformed);
}

TEST(IsoTpTest, ANinthMessageEvictsTheOldest)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    for (uint32_t i = 0; i < ECUIO_ISOTP_SLOTS + 1; ++i) {
        ASSERT_EQ(isotp_take(r, can(0x700 + i, {0x10, 0x09, 1, 2, 3, 4, 5, 6}),
                             0, &m),
                  isotp_result::first);
    }
    EXPECT_EQ(r.dropped, 1u);
    EXPECT_EQ(isotp_take(r, can(0x700, {0x21, 7, 8, 9}), 0, &m),
              isotp_result::malformed);
    EXPECT_EQ(isotp_take(r, can(0x701, {0x21, 7, 8, 9}), 0, &m),
              isotp_result::complete);
    EXPECT_EQ(isotp_take(r, can(0x708, {0x21, 7, 8, 9}), 0, &m),
              isotp_result::complete);
}

TEST(IsoTpTest, AConsecutiveFrameLaterThanNcrDropsTheMessage)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x21, 7, 8, 9, 10, 11, 12, 13}),
                         ECUIO_ISOTP_N_CR_US, &m),
              isotp_result::pending);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x22, 14, 15, 16, 17, 18, 19, 20}),
                         2 * ECUIO_ISOTP_N_CR_US + 1, &m),
              isotp_result::dropped);
    EXPECT_EQ(r.dropped, 1u);
}

TEST(IsoTpTest, ExpireEndsOnlyMessagesPastNcr)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    ASSERT_EQ(
        isotp_take(r, can(0x7E9, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 100000, &m),
        isotp_result::first);
    EXPECT_EQ(isotp_expire(r, ECUIO_ISOTP_N_CR_US), 0u);
    EXPECT_EQ(isotp_expire(r, ECUIO_ISOTP_N_CR_US + 1), 1u);
    EXPECT_EQ(r.dropped, 1u);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x21, 7, 8, 9, 10, 11, 12, 13}),
                         ECUIO_ISOTP_N_CR_US + 1, &m),
              isotp_result::malformed);
    EXPECT_EQ(isotp_take(r, can(0x7E9, {0x21, 7, 8, 9, 10, 11, 12, 13}),
                         ECUIO_ISOTP_N_CR_US + 1, &m),
              isotp_result::pending);
}

TEST(IsoTpTest, AnUntimedReceiverWaitsForTheEndOfAReply)
{
    isotp_rx& r = fresh_rx(false);
    isotp_message m;
    ASSERT_EQ(isotp_take(r, can(0x7E8, {0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0, &m),
              isotp_result::first);
    EXPECT_EQ(isotp_expire(r, 10 * ECUIO_ISOTP_N_CR_US), 0u);
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x21, 7, 8, 9, 10, 11, 12, 13}),
                         10 * ECUIO_ISOTP_N_CR_US, &m),
              isotp_result::pending);
    EXPECT_EQ(isotp_end_all(r), 1u);
    EXPECT_EQ(r.dropped, 1u);
}

TEST(IsoTpTest, AFlowControlFrameIsTheSendersNotTheReceivers)
{
    isotp_rx& r = fresh_rx(true);
    isotp_message m;
    EXPECT_EQ(isotp_take(r, can(0x7E8, {0x30, 0, 0}), 0, &m),
              isotp_result::flow);
    EXPECT_EQ(r.malformed, 0u);
}

/* A functional request's Flow Control goes on the physical request ID
 * paired with the replying ECU's (ISO 15765-4 section 6.3.2.1); any other
 * request's on the request ID itself */
TEST(IsoTpTest, AFunctionalRequestsFlowControlGoesToThePhysicalId)
{
    EXPECT_EQ(isotp_flow_control_id(0x7E8, 0x7DF), 0x7E0u);
    EXPECT_EQ(isotp_flow_control_id(0x7E9, 0x7DF), 0x7E1u);
    EXPECT_EQ(isotp_flow_control_id(0x7EF, 0x7DF), 0x7E7u);
    EXPECT_EQ(isotp_flow_control_id(0x18DAF110u, 0x18DB33F1u), 0x18DA10F1u);
    EXPECT_EQ(isotp_flow_control_id(0x7E8, 0x7E0), 0x7E0u);
    EXPECT_EQ(isotp_flow_control_id(0x7E9, 0x7E1), 0x7E1u);
    EXPECT_EQ(isotp_flow_control_id(0x641, 0x241), 0x241u) << "not OBD's IDs";
    EXPECT_EQ(isotp_flow_control_id(0x18DAF110u, 0x18DA10F1u), 0x18DA10F1u);
    EXPECT_EQ(isotp_flow_control_id(0x5, 0x7DF), 0x7DFu)
        << "no physical request ID below 9";
}

TEST(IsoTpTest, TheReceiversFlowControlFrameIsClearToSendAtOnce)
{
    const isotp_can f = isotp_flow_control(0x7E0);
    EXPECT_EQ(f.id, 0x7E0u);
    ASSERT_EQ(f.len, 8u);
    const uint8_t want[8] = {0x30,
                             0x00,
                             0x00,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD};
    EXPECT_EQ(std::memcmp(f.data, want, 8), 0);
}

/* ---- The sender ------------------------------------------------------ */

TEST(IsoTpTest, AShortMessageIsOnePaddedSingleFrame)
{
    isotp_tx& t = fresh_tx();
    const uint8_t req[] = {0x01, 0x0C, 0x0D, 0x05, 0x04, 0x0B, 0x10};
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E0, req, 7, 0, &f));
    EXPECT_EQ(f.id, 0x7E0u);
    ASSERT_EQ(f.len, 8u);
    EXPECT_EQ(f.data[0], 0x07);
    EXPECT_EQ(std::memcmp(f.data + 1, req, 7), 0);
    EXPECT_EQ(t.state, isotp_tx_state::idle);

    ASSERT_TRUE(isotp_tx_start(t, 0x7E0, req, 2, 0, &f));
    const uint8_t want[8] = {0x02,
                             0x01,
                             0x0C,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD,
                             ECUIO_ISOTP_PAD};
    EXPECT_EQ(std::memcmp(f.data, want, 8), 0);
    EXPECT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::idle);
}

TEST(IsoTpTest, ASendRefusesNoBytesTooManyAndOneUnderWay)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(ECUIO_ISOTP_MAX + 1);
    isotp_can f;
    EXPECT_FALSE(isotp_tx_start(t, 0x7E0, msg.data(), 0, 0, &f));
    EXPECT_FALSE(
        isotp_tx_start(t, 0x7E0, msg.data(), ECUIO_ISOTP_MAX + 1, 0, &f));
    ASSERT_TRUE(isotp_tx_start(t, 0x7E0, msg.data(), 20, 0, &f));
    EXPECT_FALSE(isotp_tx_start(t, 0x7E0, msg.data(), 2, 0, &f));
}

TEST(IsoTpTest, ASegmentedSendWaitsForFlowControlWithinNbs)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(20);
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), 20, 0, &f));
    EXPECT_EQ(f.data[0], 0x10);
    EXPECT_EQ(f.data[1], 20);
    EXPECT_EQ(std::memcmp(f.data + 2, msg.data(), 6), 0);
    EXPECT_EQ(isotp_tx_next(t, ECUIO_ISOTP_N_BS_US, &f), isotp_tx_step::later);
    EXPECT_EQ(isotp_tx_next(t, ECUIO_ISOTP_N_BS_US + 1, &f),
              isotp_tx_step::timed_out);
    EXPECT_EQ(t.state, isotp_tx_state::idle);
    EXPECT_EQ(
        isotp_tx_flow(t, isotp_flow_control(0x7E0), ECUIO_ISOTP_N_BS_US + 2),
        isotp_flow_result::ignored);
}

TEST(IsoTpTest, TheSenderKeepsTheReceiversSeparationTime)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(20);
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), 20, 0, &f));
    ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x30, 0x00, 0x0A}), 1000),
              isotp_flow_result::go);
    ASSERT_EQ(isotp_tx_next(t, 1000, &f), isotp_tx_step::frame);
    EXPECT_EQ(f.data[0], 0x21);
    EXPECT_EQ(isotp_tx_next(t, 10999, &f), isotp_tx_step::later);
    ASSERT_EQ(isotp_tx_next(t, 11000, &f), isotp_tx_step::frame);
    EXPECT_EQ(f.data[0], 0x22);
    ASSERT_EQ(f.len, 8u);
    EXPECT_EQ(std::memcmp(f.data + 1, msg.data() + 13, 7), 0);
    EXPECT_EQ(t.state, isotp_tx_state::idle);
}

TEST(IsoTpTest, SeparationTimesInMicrosecondsAndReservedValues)
{
    EXPECT_EQ(isotp_stmin_us(0x00), 0);
    EXPECT_EQ(isotp_stmin_us(0x7F), 127000);
    EXPECT_EQ(isotp_stmin_us(0xF1), 100);
    EXPECT_EQ(isotp_stmin_us(0xF9), 900);
    for (int b : {0x80, 0xF0, 0xFA, 0xFF}) {
        EXPECT_EQ(isotp_stmin_us(static_cast<uint8_t>(b)), 127000) << b;
    }
}

TEST(IsoTpTest, ABlockSizeAsksForFlowControlAgainAfterEachBlock)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(6 + 7 * 5);
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(),
                               static_cast<uint16_t>(msg.size()), 0, &f));
    ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x30, 0x02, 0x00}), 0),
              isotp_flow_result::go);
    ASSERT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::frame);
    ASSERT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::frame);
    EXPECT_EQ(t.state, isotp_tx_state::awaiting_flow);
    EXPECT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::later);
    ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x30, 0x02, 0x00}), 0),
              isotp_flow_result::go);
    ASSERT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::frame);
    ASSERT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::frame);
    ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x30, 0x02, 0x00}), 0),
              isotp_flow_result::go);
    ASSERT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::frame);
    EXPECT_EQ(f.data[0], 0x25);
    EXPECT_EQ(t.state, isotp_tx_state::idle);
}

TEST(IsoTpTest, WaitFramesRestartNbsUpToCoresBound)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(20);
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), 20, 0, &f));
    int64_t now = 0;
    for (uint32_t i = 1; i < ECUIO_ISOTP_WAIT_MAX; ++i) {
        now += ECUIO_ISOTP_N_BS_US;
        ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x31, 0, 0}), now),
                  isotp_flow_result::wait)
            << i;
        EXPECT_EQ(isotp_tx_next(t, now + ECUIO_ISOTP_N_BS_US, &f),
                  isotp_tx_step::later);
    }
    EXPECT_EQ(isotp_tx_flow(t, can(0x7E0, {0x31, 0, 0}), now),
              isotp_flow_result::abandoned);
    EXPECT_EQ(t.state, isotp_tx_state::idle);
}

TEST(IsoTpTest, AClearToSendResetsTheWaitCount)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(6 + 7 * 4);
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(),
                               static_cast<uint16_t>(msg.size()), 0, &f));
    for (uint32_t i = 1; i < ECUIO_ISOTP_WAIT_MAX; ++i) {
        ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x31, 0, 0}), 0),
                  isotp_flow_result::wait);
    }
    ASSERT_EQ(isotp_tx_flow(t, can(0x7E0, {0x30, 0x01, 0}), 0),
              isotp_flow_result::go);
    ASSERT_EQ(isotp_tx_next(t, 0, &f), isotp_tx_step::frame);
    EXPECT_EQ(isotp_tx_flow(t, can(0x7E0, {0x31, 0, 0}), 0),
              isotp_flow_result::wait);
}

TEST(IsoTpTest, AnOverflowOrABadFlowStatusEndsTheSend)
{
    isotp_tx& t = fresh_tx();
    const std::vector<uint8_t> msg = counting(20);
    isotp_can f;
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), 20, 0, &f));
    EXPECT_EQ(isotp_tx_flow(t, can(0x7E0, {0x32, 0, 0}), 0),
              isotp_flow_result::overflow);
    EXPECT_EQ(t.state, isotp_tx_state::idle);

    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), 20, 0, &f));
    EXPECT_EQ(isotp_tx_flow(t, can(0x7E0, {0x33, 0, 0}), 0),
              isotp_flow_result::abandoned);
    ASSERT_TRUE(isotp_tx_start(t, 0x7E8, msg.data(), 20, 0, &f));
    EXPECT_EQ(isotp_tx_flow(t, can(0x7E0, {0x30, 0}), 0),
              isotp_flow_result::abandoned);
    EXPECT_EQ(t.state, isotp_tx_state::idle);
}

TEST(IsoTpTest, FrameKindsComeFromThePciTypeNibble)
{
    EXPECT_EQ(isotp_kind(can(1, {0x07})), isotp_pci::single);
    EXPECT_EQ(isotp_kind(can(1, {0x1F})), isotp_pci::first);
    EXPECT_EQ(isotp_kind(can(1, {0x2A})), isotp_pci::consecutive);
    EXPECT_EQ(isotp_kind(can(1, {0x30})), isotp_pci::flow);
    EXPECT_EQ(isotp_kind(can(1, {0x40})), isotp_pci::other);
    EXPECT_EQ(isotp_kind(can(1, {})), isotp_pci::other);
}

} // namespace
