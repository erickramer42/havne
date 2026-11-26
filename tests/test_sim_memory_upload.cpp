// File/layer: tests/test_sim_memory_upload.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: SimMemoryTest, FakeMemoryTest (test_obd2)
//
// The simulated ECU's upload (core/src/sim_memory.h: services 35, 36 and
// 37): a region handed over block by block, the counter's wrap, each
// refusal in its order, and the session that ends one.

#include "sim_memory_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace sim_memory_test;

const Bytes kOpened = {0x75, 0x20, 0x04, 0x02};

/* Every block of the upload open on e, their data run together; each
 * must carry the next counter, from 01 and FF wrapping to 00 */
Bytes take_blocks(Ecu& e, uint32_t* blocks = nullptr)
{
    Bytes image;
    uint8_t counter = 1;
    uint32_t n = 0;
    while (e.m.left > 0) {
        const Bytes r = e.ask({0x36, counter});
        if (r.size() < 3 || r[0] != 0x76 || r[1] != counter) {
            ADD_FAILURE() << "block " << n << " under counter "
                          << static_cast<int>(counter);
            return image;
        }
        image.insert(image.end(), r.begin() + 2, r.end());
        ++counter;
        ++n;
    }
    if (blocks != nullptr) {
        *blocks = n;
    }
    return image;
}

/* The region from address on as a read answers it, without the 63 */
Bytes bytes_of(uint32_t address, uint32_t size, const char* vin)
{
    const Bytes r = data_of(address, size, vin);
    return Bytes(r.begin() + 1, r.end());
}

TEST(SimMemoryTest, AnUploadHandsOverTheRegionBlockByBlock)
{
    Ecu e;
    EXPECT_EQ(e.ask(upload_of(kBase, kSize)), kOpened)
        << "75, a two-byte length, 0x0402: 1,024 bytes of data a block";
    uint32_t blocks = 0;
    const Bytes image = take_blocks(e, &blocks);
    EXPECT_EQ(blocks, 64u);
    EXPECT_EQ(image, bytes_of(kBase, kSize, e.m.vin))
        << "what a read answers, the VIN at its place";
    EXPECT_EQ(e.ask({0x36, 0x41}), nrc(0x36, 0x24)) << "all handed over";
    EXPECT_EQ(e.ask({0x37}), (Bytes{0x77}));
    EXPECT_EQ(e.ask({0x37}), nrc(0x37, 0x24)) << "none open";
    EXPECT_EQ(e.ask({0x36, 0x01}), nrc(0x36, 0x24));
}

TEST(SimMemoryTest, TheCountersWrapIsFromFFToZero)
{
    Ecu e;
    e.m.faults.block = 0x82; /* 128 bytes of data: 512 blocks, two wraps */
    EXPECT_EQ(e.ask(upload_of(kBase, kSize)), (Bytes{0x75, 0x20, 0x00, 0x82}));
    for (int i = 1; i <= 0xFF; ++i) {
        ASSERT_EQ(e.ask({0x36, static_cast<uint8_t>(i)}).size(), 130u);
    }
    EXPECT_EQ(e.ask({0x36, 0x01}), nrc(0x36, 0x73)) << "FF is followed by 00";
    const Bytes zero = e.ask({0x36, 0x00});
    ASSERT_EQ(zero.size(), 130u);
    EXPECT_EQ(zero[1], 0x00);
    EXPECT_EQ(Bytes(zero.begin() + 2, zero.end()),
              bytes_of(kBase + 255 * 128, 128, e.m.vin));

    Ecu whole;
    whole.m.faults.block = 0x82;
    whole.ask(upload_of(kBase, kSize));
    uint32_t blocks = 0;
    EXPECT_EQ(take_blocks(whole, &blocks), bytes_of(kBase, kSize, whole.m.vin));
    EXPECT_EQ(blocks, 512u);
    EXPECT_EQ(whole.ask({0x37}), (Bytes{0x77}));
}

TEST(SimMemoryTest, ABlocksDataIsItsLengthLessTwoAndTheLastIsShort)
{
    Ecu e;
    EXPECT_EQ(e.ask(upload_of(kBase + 7, 1500)), kOpened);
    const Bytes first = e.ask({0x36, 0x01});
    ASSERT_EQ(first.size(), 0x402u) << "the block length counts 76 and the "
                                       "counter";
    EXPECT_EQ(Bytes(first.begin() + 2, first.end()),
              bytes_of(kBase + 7, 1024, e.m.vin));
    const Bytes last = e.ask({0x36, 0x02});
    ASSERT_EQ(last.size(), 2u + 476u);
    EXPECT_EQ(Bytes(last.begin() + 2, last.end()),
              bytes_of(kBase + 7 + 1024, 476, e.m.vin));
    EXPECT_EQ(e.ask({0x37}), (Bytes{0x77}));

    EXPECT_EQ(e.ask(upload_of(kBase + kSize - 1, 1)), kOpened);
    EXPECT_EQ(e.ask({0x36, 0x01}),
              (Bytes{0x76, 0x01, ecuio_sim_memory_fill(kBase + kSize - 1)}));
    EXPECT_EQ(e.ask({0x37}), (Bytes{0x77}));
}

TEST(SimMemoryTest, EachUploadRefusalComesInItsOrder)
{
    Ecu e;
    e.s.unlocked = 0;
    e.extended = false;
    EXPECT_EQ(e.ask({0x35}), nrc(0x35, 0x7F)) << "the session comes first";
    EXPECT_EQ(e.ask({0x36}), nrc(0x36, 0x7F));
    EXPECT_EQ(e.ask({0x37, 0x00}), nrc(0x37, 0x7F));

    e.extended = true;
    EXPECT_EQ(e.ask({0x35, 0x00, 0x44}), nrc(0x35, 0x13)) << "under 4 bytes";
    Bytes shorter = upload_of(kBase, 16);
    shorter.pop_back();
    EXPECT_EQ(e.ask(shorter), nrc(0x35, 0x13));
    Bytes other = upload_of(kBase, 16);
    other[1] = 0x01;
    EXPECT_EQ(e.ask(other), nrc(0x35, 0x31))
        << "a compressed or encrypted upload isn't the module's bytes";
    EXPECT_EQ(e.ask(upload_of(kBase + kSize - 1, 2)), nrc(0x35, 0x31))
        << "locked, the range still comes first";
    EXPECT_EQ(e.ask(upload_of(kBase, 0)), nrc(0x35, 0x31));
    EXPECT_EQ(e.ask(upload_of(kBase, 16)), nrc(0x35, 0x33));
    EXPECT_FALSE(e.m.uploading);
    EXPECT_EQ(e.ask({0x36, 0x01}), nrc(0x36, 0x24)) << "none open";
    EXPECT_EQ(e.ask({0x37}), nrc(0x37, 0x24));

    e.s.unlocked = 0x01;
    EXPECT_EQ(e.ask(upload_of(kBase, 3000)), kOpened);
    EXPECT_EQ(e.ask(upload_of(kBase, 16)), nrc(0x35, 0x70)) << "one is open";
    EXPECT_EQ(e.ask(upload_of(kBase + kSize, 16)), nrc(0x35, 0x31))
        << "the range is judged before the one open";
    EXPECT_EQ(e.ask({0x36}), nrc(0x36, 0x13));
    EXPECT_EQ(e.ask({0x36, 0x01, 0x00}), nrc(0x36, 0x13)) << "extra bytes";
    EXPECT_EQ(e.ask({0x36, 0x02}), nrc(0x36, 0x73));
    EXPECT_EQ(e.ask({0x36, 0x00}), nrc(0x36, 0x73));
    EXPECT_EQ(e.ask({0x37, 0x00}), nrc(0x37, 0x13));
    EXPECT_EQ(e.ask({0x37}), nrc(0x37, 0x24)) << "bytes are still left";
    EXPECT_EQ(e.ask({0x36, 0x01}).size(), 0x402u)
        << "none of those moved the upload";
    EXPECT_EQ(e.ask({0x36, 0x01}), nrc(0x36, 0x73))
        << "a block is handed over once";
    e.s.unlocked = 0;
    EXPECT_EQ(e.ask({0x36, 0x02}), nrc(0x36, 0x33));
    EXPECT_EQ(e.ask({0x36, 0x03}), nrc(0x36, 0x33))
        << "locked is judged before the counter";
    e.s.unlocked = 0x01;
    EXPECT_EQ(e.ask({0x36, 0x02}).size(), 0x402u);
    EXPECT_EQ(e.ask({0x36, 0x03}).size(), 2u + 3000u - 2048u);
    EXPECT_EQ(e.ask({0x37}), (Bytes{0x77}));
}

TEST(SimMemoryTest, ABlockLengthUnderThreeHandsOverNothing)
{
    Ecu e;
    e.m.faults.block = 2;
    EXPECT_EQ(e.ask(upload_of(kBase, 16)), (Bytes{0x75, 0x20, 0x00, 0x02}));
    EXPECT_EQ(e.ask({0x36, 0x01}), nrc(0x36, 0x24));
    e.m.faults.block = 3;
    EXPECT_EQ(e.ask({0x36, 0x01}),
              (Bytes{0x76, 0x01, ecuio_sim_memory_fill(kBase)}));
}

TEST(SimMemoryTest, AReadWhileAnUploadIsOpenMovesNothing)
{
    Ecu e;
    e.ask(upload_of(kBase, 2000));
    EXPECT_EQ(e.ask(read_of(kBase + 100, 8)), data_of(kBase + 100, 8, e.m.vin));
    EXPECT_EQ(take_blocks(e), bytes_of(kBase, 2000, e.m.vin));
}

/* ---------- in the engine ECU's session (sim_ecu.h) ---------- */

TEST(SimMemoryTest, ASessionEnteredLeftOrLapsedEndsAnUpload)
{
    Node n;
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_EQ(n.ask(upload_of(kBase, 3000)), kOpened);
    EXPECT_TRUE(n.handled);
    EXPECT_EQ(n.ask({0x36, 0x01}).size(), 0x402u);

    EXPECT_EQ(n.ask({0x10, 0x03}), kEntered);
    n.unlock();
    EXPECT_EQ(n.ask({0x36, 0x02}), nrc(0x36, 0x24)) << "entered anew";
    EXPECT_EQ(n.ask(upload_of(kBase, 3000)), kOpened);

    n.ask({0x10, 0x01});
    EXPECT_EQ(n.ask({0x36, 0x01}), nrc(0x36, 0x7F));
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_EQ(n.ask({0x37}), nrc(0x37, 0x24)) << "left";
    EXPECT_EQ(n.ask(upload_of(kBase, 3000)), kOpened);

    n.now += ECUIO_SIM_S3_US;
    EXPECT_EQ(n.ask({0x36, 0x01}), nrc(0x36, 0x7F));
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_EQ(n.ask({0x36, 0x01}), nrc(0x36, 0x24)) << "lapsed";

    /* 34 and 31 are no memory's: the engine ECU lacks them */
    EXPECT_EQ(n.ask({0x34, 0x00, 0x44, 0, 1, 0, 0, 0, 0, 0, 16}),
              nrc(0x34, 0x11));
    EXPECT_FALSE(n.handled);
    EXPECT_EQ(n.ask({0x31, 0x01, 0xFF, 0x00}), nrc(0x31, 0x11));
}

/* ---------- through the unbound sim ---------- */

TEST(SimMemoryTest, AnUnboundSimUploadsItsMemory)
{
    Unbound u("mock://ask?values=canned&modules=2");
    ASSERT_NE(u.sim, nullptr);
    EXPECT_EQ(u.ask(0x7E0, upload_of(kBase, 1500)), nrc(0x35, 0x7F));
    u.unlock();
    EXPECT_TRUE(u.ask(0x7DF, upload_of(kBase, 1500)).empty())
        << "a functional 35 is unanswered";
    EXPECT_EQ(u.ask(0x7E0, {0x36, 0x01}), nrc(0x36, 0x24))
        << "and opened nothing";
    EXPECT_EQ(u.ask(0x7E0, upload_of(kBase, 1500)), kOpened);
    EXPECT_TRUE(u.ask(0x7DF, {0x36, 0x01}).empty());
    EXPECT_TRUE(u.ask(0x7DF, {0x37}).empty());
    const Bytes first = u.ask(0x7E0, {0x36, 0x01});
    ASSERT_EQ(first.size(), 0x402u) << "the functional 36 moved nothing";
    EXPECT_EQ(Bytes(first.begin() + 2, first.end()),
              bytes_of(kBase, 1024, ECUIO_SIM_DEFAULT_VIN));
    EXPECT_EQ(u.ask(0x7E0, {0x36, 0x02}).size(), 2u + 476u);
    EXPECT_EQ(u.ask(0x7E0, {0x37}), (Bytes{0x77}));

    /* modules=2's module has no memory */
    EXPECT_EQ(u.ask(0x7E1, upload_of(kBase, 16)), nrc(0x35, 0x11));
    EXPECT_EQ(u.ask(0x7E1, {0x36, 0x01}), nrc(0x36, 0x11));
    EXPECT_EQ(u.ask(0x7E1, {0x37}), nrc(0x37, 0x11));
}

/* ---------- the fakes' ECU (tests/fake_ecus.h) ---------- */

TEST_F(FakeMemoryTest, TheEngineEcuUploadsItsMemoryByItsReplyId)
{
    const auto unlock = [](uint32_t rx) {
        ask(rx, {0x10, 0x03});
        ask(rx, {0x27, 0x01});
        EXPECT_EQ(ask(rx, key_for(0x01)), (Bytes{0x67, 0x02}));
    };
    unlock(0x7E8);
    unlock(0x7E9);
    EXPECT_EQ(ask(0x7E8, upload_of(kBase, 1500)), kOpened);
    EXPECT_EQ(ask(0x7E9, {0x36, 0x01}), nrc(0x36, 0x24))
        << "the upload is the ECU's own";
    const Bytes first = ask(0x7E8, {0x36, 0x01});
    ASSERT_EQ(first.size(), 0x402u);
    EXPECT_EQ(Bytes(first.begin() + 2, first.end()),
              bytes_of(kBase, 1024, fake_ecu::kVin));

    ask(0x7E8, {0x10, 0x03});
    unlock(0x7E8);
    EXPECT_EQ(ask(0x7E8, {0x36, 0x02}), nrc(0x36, 0x24))
        << "the session entered anew ended it";

    ASSERT_TRUE(fake_ecu::set_module_dids("7EA:D801=00 12"));
    EXPECT_EQ(ask(0x7EA, upload_of(kBase, 16)), nrc(0x35, 0x11));
}

} // namespace
