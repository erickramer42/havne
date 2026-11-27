#include "engine_talk_support.h"

using namespace engine_talk_test;

namespace {

ecuio_talk_offer offer_write(ecuio_request_engine* e, const Bytes& pdu,
                             uint8_t echo, uint32_t c = 0)
{
    return ecuio_engine_offer_talk(
        e, c, request(pdu, echo, ecuio_talk_kind::write, 0), 0);
}

Bytes block(uint8_t counter, uint32_t len)
{
    Bytes b(len);
    b[0] = 0x36;
    b[1] = counter;
    for (uint32_t k = 2; k < len; ++k) {
        b[k] = static_cast<uint8_t>(k * 13 + 5);
    }
    return b;
}

void writing(ecuio_request_engine* e, uint32_t channels = 1)
{
    ASSERT_TRUE(ecuio_engine_init(e, channels));
    e->pdu_max = ECUIO_ENGINE_PDU_MAX;
    e->write_max = ECUIO_ENGINE_WRITE_MAX;
}

} // namespace

TEST(EngineTalkTest, AWriteOfISOTPsLongestGoesOutWholeAndIsMatchedOnItsEcho)
{
    static ecuio_request_engine e;
    writing(&e);
    Bytes b = block(0x01, ECUIO_ENGINE_WRITE_MAX);
    const Bytes whole = b;
    ASSERT_EQ(offer_write(&e, b, 2), ecuio_talk_offer::taken);
    b.assign(b.size(), 0x2E);
    ASSERT_EQ(ecuio_engine_due(&e, 0, 0), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, ECUIO_ENGINE_ONCE), whole)
        << "the door's copy, every byte";
    const ecuio_request& once =
        ecuio_engine_request_of(&e, 0, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(Bytes(once.bytes, once.bytes + once.len), (Bytes{0x36, 0x01}));
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, 0);
    const uint8_t other[2] = {0x76, 0x02};
    EXPECT_NE(ecuio_engine_on_frame(&e, 0, other, 2, kMs, nullptr),
              ecuio_reply::positive)
        << "another block's answer";
    const uint8_t reply[2] = {0x76, 0x01};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, reply, 2, kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(e.chans[0].talk.wide_len, 0u) << "the bytes are free again";
}

TEST(EngineTalkTest, AWriteIsBoundByWhatTheTransportDeclares)
{
    static ecuio_request_engine e;
    writing(&e);
    e.write_max = 0;
    EXPECT_EQ(offer_write(&e, block(1, 3), 2), ecuio_talk_offer::too_long)
        << "a transport that sends none (ELM327)";
    e.write_max = 1026;
    EXPECT_EQ(offer_write(&e, block(1, 1027), 2), ecuio_talk_offer::too_long);
    EXPECT_EQ(offer_write(&e, block(1, 1026), 2), ecuio_talk_offer::taken);
    EXPECT_EQ(e.chans[0].talk.wide_len, 1026u);

    ecuio_engine_talk_state t;
    ecuio_key_window w;
    static uint8_t wide[ECUIO_ENGINE_WRITE_MAX + 1];
    EXPECT_EQ(ecuio_engine_talk(t, w, ECUIO_ENGINE_PDU_MAX,
                                request(block(1, ECUIO_ENGINE_WRITE_MAX + 1), 2,
                                        ecuio_talk_kind::write, 0),
                                0, wide, ECUIO_ENGINE_WRITE_MAX + 1),
              ecuio_talk_offer::too_long)
        << "no transport's bound passes ISO-TP's";
    EXPECT_EQ(door(t, w, block(1, 3), 2, ecuio_talk_kind::write),
              ecuio_talk_offer::too_long)
        << "no buffer, no write";
    EXPECT_EQ(door(t, w, block(1, ECUIO_ENGINE_PDU_MAX + 1), 2),
              ecuio_talk_offer::refused)
        << "a plain talk never carries a 36";
}

TEST(EngineTalkTest, AWriteIsTier3sWriteCeilingAndNothingElse)
{
    static ecuio_request_engine e;
    writing(&e);
    const auto fresh = [&](const Bytes& pdu, uint8_t echo) {
        ecuio_engine_talk_state saved = e.chans[0].talk;
        const ecuio_talk_offer r = offer_write(&e, pdu, echo);
        e.chans[0].talk = saved;
        return r;
    };
    const Bytes download = {0x34, 0x00, 0x44, 0x00, 0x0F, 0x00,
                            0x00, 0x00, 0x00, 0x04, 0x00};
    EXPECT_EQ(fresh(download, 1), ecuio_talk_offer::taken);
    EXPECT_EQ(fresh({0x37}, 1), ecuio_talk_offer::taken);
    EXPECT_EQ(fresh({0x31, 0x01, 0x02, 0x01}, 3), ecuio_talk_offer::taken);
    for (const Bytes& q :
         {Bytes{0x31, 0x02, 0x02, 0x01}, Bytes{0x31, 0x03, 0x02, 0x01},
          Bytes{0x11, 0x01}, Bytes{0x2E, 0xF1, 0x90, 0x00}, Bytes{0x23, 0x14},
          Bytes{0x35, 0x00, 0x44}, Bytes{0x27, 0x01}, Bytes{0x10, 0x02},
          Bytes{0x3D, 0x14}}) {
        EXPECT_EQ(fresh(q, 1), ecuio_talk_offer::refused)
            << std::hex << int{q[0]} << " " << int{q[1]};
    }
    EXPECT_EQ(fresh({0x31, 0x01, 0x02, 0x01}, 4), ecuio_talk_offer::refused)
        << "an echo past 3";
    e.chans[0].functional = true;
    EXPECT_EQ(fresh(block(1, 3), 2), ecuio_talk_offer::refused)
        << "a functional channel's";
}

TEST(EngineTalkTest, OneChannelWritesAtATime)
{
    static ecuio_request_engine e;
    writing(&e, 2);
    ASSERT_EQ(offer_write(&e, block(1, 600), 2, 0), ecuio_talk_offer::taken);
    EXPECT_EQ(offer_write(&e, block(1, 3), 2, 1), ecuio_talk_offer::busy)
        << "the engine holds one write's bytes";
    EXPECT_EQ(offer(&e, {0x22, 0xD3, 0x01}, 3, 1), ecuio_talk_offer::taken)
        << "another channel's plain talk";
    ASSERT_EQ(ecuio_engine_due(&e, 0, 0), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, false, 0);
    EXPECT_EQ(offer_write(&e, block(1, 3), 2, 1), ecuio_talk_offer::busy)
        << "settled and not yet taken";
    ASSERT_EQ(taken(&e), ecuio_talk_end::unsent);
    ecuio_talk_result r;
    ASSERT_FALSE(ecuio_engine_talk_take(e.chans[1].talk, &r));
    ASSERT_TRUE(ecuio_talk_withdraw(e.chans[1].talk));
    EXPECT_EQ(offer_write(&e, block(1, 3), 2, 1), ecuio_talk_offer::taken);
}
