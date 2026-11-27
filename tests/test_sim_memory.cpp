// File/layer: tests/test_sim_memory.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SimMemoryTest, FakeMemoryTest (test_obd2)
//
// The simulated ECU's memory (core/src/sim_memory.h, ADR-0066 section 4):
// the header by itself (the region's bytes, a read's refusals in their
// order, the region's edges), then in the engine ECU's session
// (sim_ecu.h), through the unbound sim, and as the fakes' ECU answers it
// (tests/fake_ecus.h).

#include "sim_memory_support.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace sim_memory_test;

TEST(SimMemoryTest, TheFillIsAFixedMixOfTheAddress)
{
    /* Worked outside the header from its rule (the address times
     * 0x9E3779B1, bits 24-31 XOR bits 11-18 XOR a letter of "HAVNE" by
     * the address modulo 5), so a change to the mix shows here */
    EXPECT_EQ(ecuio_sim_memory_fill(0x00000000), 0x48);
    EXPECT_EQ(ecuio_sim_memory_fill(0x00010000), 0x18);
    EXPECT_EQ(ecuio_sim_memory_fill(0x00010001), 0x4E);
    EXPECT_EQ(ecuio_sim_memory_fill(0x00010004), 0x66);
    EXPECT_EQ(ecuio_sim_memory_fill(0x00010005), 0x1D);
    EXPECT_EQ(ecuio_sim_memory_fill(0x00018000), 0x43);
    EXPECT_EQ(ecuio_sim_memory_fill(0x0001FFFF), 0x44);
}

TEST(SimMemoryTest, AReadAnswersTheRegionsBytes)
{
    Ecu e;
    EXPECT_EQ(e.ask(read_of(kBase, 4)), (Bytes{0x63, 0x18, 0x4E, 0x06, 0xFC}))
        << "the first four, by the fill's rule";

    /* The whole region in the longest reads, a short one last */
    Bytes image;
    for (uint32_t at = 0; at < kSize;) {
        const uint32_t n = kSize - at < 4094 ? kSize - at : 4094;
        const Bytes r = e.ask(read_of(kBase + at, n));
        ASSERT_EQ(r.size(), 1u + n) << "at " << at;
        ASSERT_EQ(r[0], 0x63);
        image.insert(image.end(), r.begin() + 1, r.end());
        at += n;
    }
    ASSERT_EQ(image.size(), size_t{kSize});
    for (uint32_t i = 0; i < kSize; ++i) {
        const uint32_t a = kBase + i;
        const bool vin = a >= kVinAt && a < kVinAt + 17;
        const uint8_t want =
            vin ? static_cast<uint8_t>(ECUIO_SIM_DEFAULT_VIN[a - kVinAt])
                : ecuio_sim_memory_fill(a);
        ASSERT_EQ(image[i], want) << "offset " << i;
    }
}

TEST(SimMemoryTest, TheVinSitsAtItsPlace)
{
    Ecu e;
    const Bytes r = e.ask(read_of(kVinAt - 1, 19));
    ASSERT_EQ(r.size(), 20u);
    EXPECT_EQ(r[1], ecuio_sim_memory_fill(kVinAt - 1));
    EXPECT_EQ(std::string(r.begin() + 2, r.begin() + 19),
              ECUIO_SIM_DEFAULT_VIN);
    EXPECT_EQ(r[19], ecuio_sim_memory_fill(kVinAt + 17));
    /* Neither neighbor is a letter or a digit, so the VIN is a run of
     * exactly 17 a finder can count */
    EXPECT_FALSE(std::isalnum(r[1]));
    EXPECT_FALSE(std::isalnum(r[19]));

    const char other[] = "ZZZFAKE22N0000002";
    e.m.vin = other;
    const Bytes o = e.ask(read_of(kVinAt, 17));
    EXPECT_EQ(std::string(o.begin() + 1, o.end()), other)
        << "the node's VIN, not a fixed one";
}

TEST(SimMemoryTest, EachRefusalComesInItsOrder)
{
    Ecu e;
    e.s.unlocked = 0;
    e.extended = false;
    EXPECT_EQ(e.ask({0x23}), nrc(0x23, 0x7F))
        << "the session is judged before the length";
    EXPECT_EQ(e.ask(read_of(kBase, 16)), nrc(0x23, 0x7F));

    e.extended = true;
    EXPECT_EQ(e.ask({0x23}), nrc(0x23, 0x13));
    EXPECT_EQ(e.ask({0x23, 0x24}), nrc(0x23, 0x13)) << "under 3 bytes";
    Bytes shorter = read_of(kBase, 16);
    shorter.pop_back();
    EXPECT_EQ(e.ask(shorter), nrc(0x23, 0x13)) << "not the identifier's";
    Bytes longer = read_of(kBase, 16);
    longer.push_back(0x00);
    EXPECT_EQ(e.ask(longer), nrc(0x23, 0x13));
    /* A length judged before the identifier's own range: 5 address
     * bytes with one missing is 13, with all five 31 */
    EXPECT_EQ(e.ask({0x23, 0x15, 0x00, 0x00, 0x01, 0x00, 0x10}),
              nrc(0x23, 0x13));

    /* Locked, each range refusal still comes first */
    EXPECT_EQ(e.ask({0x23, 0x15, 0x00, 0x00, 0x01, 0x00, 0x00, 0x10}),
              nrc(0x23, 0x31))
        << "an address of 5 bytes";
    EXPECT_EQ(e.ask({0x23, 0x10, 0x10}), nrc(0x23, 0x31))
        << "an address of no bytes";
    EXPECT_EQ(e.ask({0x23, 0x04, 0x00, 0x01, 0x00, 0x00}), nrc(0x23, 0x31))
        << "a size of no bytes";
    EXPECT_EQ(e.ask({0x23, 0x54, 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0x10}),
              nrc(0x23, 0x31))
        << "a size of 5 bytes";
    EXPECT_EQ(e.ask(read_of(kBase, 0)), nrc(0x23, 0x31)) << "a zero size";
    EXPECT_EQ(e.ask(read_of(kBase - 1, 16)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(kBase, 16)), nrc(0x23, 0x33)) << "locked";
    EXPECT_EQ(e.ask(read_of(kBase, 4095)), nrc(0x23, 0x33))
        << "locked is judged before the reply's length";

    e.s.unlocked = 0x01;
    EXPECT_EQ(e.ask(read_of(kBase, 4095)), nrc(0x23, 0x14));
    EXPECT_EQ(e.ask(read_of(kBase, 4094)).size(), 4095u);
    e.s.unlocked = 0x03;
    EXPECT_EQ(e.ask(read_of(kBase, 16)), data_of(kBase, 16, e.m.vin))
        << "either level unlocks it";
}

TEST(SimMemoryTest, AReadEndsAtTheRegionsEndAndNotOneBytePast)
{
    Ecu e;
    const uint32_t end = kBase + kSize;
    EXPECT_EQ(e.ask(read_of(end - 1, 1)),
              (Bytes{0x63, ecuio_sim_memory_fill(end - 1)}));
    EXPECT_EQ(e.ask(read_of(end - 16, 16)), data_of(end - 16, 16, e.m.vin));
    EXPECT_EQ(e.ask(read_of(end - 16, 17)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(end - 1, 2)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(end, 1)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(kBase, 1)),
              (Bytes{0x63, ecuio_sim_memory_fill(kBase)}));
    EXPECT_EQ(e.ask(read_of(kBase - 1, 1)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(kBase - 1, 2)), nrc(0x23, 0x31))
        << "a read that starts outside";

    /* The whole region is inside it, and only too long to answer */
    EXPECT_EQ(e.ask(read_of(kBase, kSize, 4, 3)), nrc(0x23, 0x14));
    EXPECT_EQ(e.ask(read_of(kBase, kSize + 1, 4, 3)), nrc(0x23, 0x31));
    /* Sums that would wrap 32 bits stay outside */
    EXPECT_EQ(e.ask(read_of(0xFFFFFFFFu, 0xFFFFFFFFu, 4, 4)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(kBase + 1, 0xFFFFFFFFu, 4, 4)), nrc(0x23, 0x31));
    EXPECT_EQ(e.ask(read_of(0xFFFFFFF0u, 0x00010020u, 4, 4)), nrc(0x23, 0x31));
}

TEST(SimMemoryTest, EveryIdentifierFormReadsTheSameBytes)
{
    Ecu e;
    const Bytes want = data_of(kBase + 0x1234, 32, e.m.vin);
    for (int a = 1; a <= 4; ++a) {
        for (int s = 1; s <= 4; ++s) {
            const Bytes req = read_of(kBase + 0x1234, 32, a, s);
            ASSERT_EQ(req.size(), size_t(2 + a + s));
            /* An address of one or two bytes can't reach the region */
            EXPECT_EQ(e.ask(req), a >= 3 ? want : nrc(0x23, 0x31))
                << a << " address bytes, " << s << " size bytes";
        }
    }
}

/* ---------- in the engine ECU's session (sim_ecu.h) ---------- */

TEST(SimMemoryTest, TheSessionAnswersAReadOnlyOnceUnlocked)
{
    Node n;
    const Bytes read = read_of(kBase + 8, 24);
    EXPECT_EQ(n.ask(read), nrc(0x23, 0x7F)) << "the default session";
    EXPECT_TRUE(n.handled);
    EXPECT_EQ(n.ask({0x10, 0x03}), kEntered);
    EXPECT_EQ(n.ask(read), nrc(0x23, 0x33));
    n.unlock();
    EXPECT_EQ(n.ask(read), data_of(kBase + 8, 24, n.ecu.config.vin));
    EXPECT_TRUE(n.handled);

    EXPECT_EQ(n.ask({0x10, 0x03}), kEntered);
    EXPECT_EQ(n.ask(read), nrc(0x23, 0x33)) << "the session entered anew";
    n.unlock();
    n.now += ECUIO_SIM_S3_US;
    EXPECT_EQ(n.ask(read), nrc(0x23, 0x7F)) << "S3 lapsed it";
    EXPECT_EQ(n.ask({0x10, 0x03}), kEntered);
    n.unlock();
    n.now += ECUIO_SIM_S3_US - 1;
    EXPECT_EQ(n.ask(read).size(), 25u) << "a read keeps the session";
    n.now += ECUIO_SIM_S3_US - 1;
    EXPECT_EQ(n.ask(read).size(), 25u);
}

TEST(SimMemoryTest, AFunctionalReadIsUnansweredAndChangesNothing)
{
    Node n;
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_TRUE(n.ask(read_of(kBase, 16), true).empty());
    EXPECT_NE(n.s.security.unlocked, 0);
    EXPECT_EQ(n.ask(read_of(kBase, 16)), data_of(kBase, 16, n.ecu.config.vin));
}

TEST(SimMemoryTest, ANodeWithNoMemoryLeavesAReadToItsCaller)
{
    Node n(false);
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_EQ(n.ask(read_of(kBase, 16)), nrc(0x23, 0x11));
    EXPECT_FALSE(n.handled);
    EXPECT_EQ(n.ask(kHardware), kNoDid);
    EXPECT_EQ(n.ask(kSoftware), kNoDid);
}

TEST(SimMemoryTest, AReadComesBetweenAStrictOnesSeedAndItsKey)
{
    Node n;
    n.s.security.strict = true;
    n.ask({0x10, 0x03});
    n.ask({0x27, 0x01});
    EXPECT_EQ(n.ask(read_of(kBase, 16)), nrc(0x23, 0x33));
    EXPECT_EQ(n.ask(key_for(0x01)), nrc(0x27, 0x24));
}

TEST(SimMemoryTest, TheIdentificationDidsAnswerInTheSessionWithNoUnlock)
{
    Node n;
    EXPECT_EQ(n.ask(kHardware), kNoDid) << "the default session";
    EXPECT_EQ(n.ask(kSoftware), kNoDid);
    n.ask({0x10, 0x03});
    EXPECT_EQ(n.ask(kHardware), ident_of(0xD220, "HAVNESIM-HW-0001"));
    EXPECT_EQ(n.ask(kSoftware), ident_of(0xD221, "HAVNESIM-SW-0001"));
    EXPECT_EQ(n.s.security.unlocked, 0);
    EXPECT_EQ(n.ask({0x22, 0xD2, 0x22}), kNoDid);
    EXPECT_EQ(n.ask({0x22, 0xD2, 0x00}), (Bytes{0x62, 0xD2, 0x00, 0x1A, 0x2C}))
        << "the session's other DIDs are untouched";
    EXPECT_EQ(n.ask(kHardware, true).size(), 19u)
        << "a functional read of a DID is answered as any DID";
}

/* ---------- through the unbound sim ---------- */

TEST(SimMemoryTest, AnUnboundSimReadsItsMemoryOnceUnlocked)
{
    Unbound u("mock://ask?values=canned&modules=2&vin=ZZZFAKE22N0000002");
    ASSERT_NE(u.sim, nullptr);
    const Bytes read = read_of(kVinAt - 4, 32);
    EXPECT_EQ(u.ask(0x7E0, read), nrc(0x23, 0x7F));
    EXPECT_EQ(u.ask(0x7E0, {0x10, 0x03}), kEntered);
    EXPECT_EQ(u.ask(0x7E0, kHardware), ident_of(0xD220, "HAVNESIM-HW-0001"));
    EXPECT_EQ(u.ask(0x7E0, read), nrc(0x23, 0x33));
    u.unlock();
    EXPECT_EQ(u.ask(0x7E0, read), data_of(kVinAt - 4, 32, "ZZZFAKE22N0000002"))
        << "the URI's VIN is the one in the region";
    EXPECT_EQ(u.ask(0x7E0, read_of(kBase, 4094)).size(), 4095u)
        << "the longest read fits a reply";

    EXPECT_TRUE(u.ask(0x7DF, read).empty()) << "a functional read";
    EXPECT_EQ(u.ask(0x7E0, read).size(), 33u) << "which locked nothing";

    /* modules=2's module has no memory */
    EXPECT_EQ(u.ask(0x7E1, read), nrc(0x23, 0x11));
    EXPECT_EQ(u.ask(0x7E1, kHardware), kNoDid);
}

TEST(SimMemoryTest, AMemoryReplyIsCountedAsAnyReplyOfItsModule)
{
    Unbound u("mock://ask?values=canned&faults=silent:2");
    ASSERT_NE(u.sim, nullptr);
    u.unlock();
    /* Segmented replies have a count of their own: the second is lost */
    const Bytes read = read_of(kBase, 64);
    EXPECT_EQ(u.ask(0x7E0, read).size(), 65u);
    EXPECT_TRUE(u.ask(0x7E0, read).empty());
    EXPECT_EQ(u.ask(0x7E0, read).size(), 65u);
}

TEST(SimMemoryTest, ARecordingsEcuHasNoMemory)
{
    const std::string uri = std::string("replay://") + ECUIO_REPLAY_FIXTURES +
                            "/2026-10-25-engine-start";
    Unbound u(uri.c_str());
    ASSERT_NE(u.sim, nullptr);
    u.ask(0x7E0, {0x10, 0x03});
    EXPECT_EQ(u.ask(0x7E0, read_of(kBase, 16)), nrc(0x23, 0x11))
        << "a recording answers only what the car answered";
    EXPECT_EQ(u.ask(0x7E0, kHardware), kNoDid);
}

/* ---------- the fakes' ECU (tests/fake_ecus.h) ---------- */

TEST_F(FakeMemoryTest, TheEngineEcuReadsItsMemoryOnceUnlockedByItsReplyId)
{
    const Bytes read = read_of(kVinAt - 4, 32);
    EXPECT_EQ(ask(0x7E8, read), nrc(0x23, 0x7F));
    EXPECT_EQ(ask(0x7E8, kHardware), kNoDid);
    ask(0x7E8, {0x10, 0x03});
    ask(0x7E9, {0x10, 0x03});
    EXPECT_EQ(ask(0x7E8, kHardware), ident_of(0xD220, "HAVNESIM-HW-0001"));
    EXPECT_EQ(ask(0x7E8, kSoftware), ident_of(0xD221, "HAVNESIM-SW-0001"));
    EXPECT_EQ(ask(0x7E8, read), nrc(0x23, 0x33));
    ask(0x7E8, {0x27, 0x01});
    EXPECT_EQ(ask(0x7E8, key_for(0x01)), (Bytes{0x67, 0x02}));
    EXPECT_EQ(ask(0x7E8, read), data_of(kVinAt - 4, 32, fake_ecu::kVin))
        << "the fakes' VIN is the one in the region";
    EXPECT_EQ(ask(0x7E9, read), nrc(0x23, 0x33)) << "the unlock is the ECU's";
    EXPECT_EQ(ask(0x7E8, read_of(kBase, 4094)).size(), 4095u);
    EXPECT_EQ(ask(0x7E8, read_of(kBase, 4095)), nrc(0x23, 0x14));

    ask(0x7E8, {0x10, 0x03});
    EXPECT_EQ(ask(0x7E8, read), nrc(0x23, 0x33)) << "the session entered anew";
    ask(0x7E8, {0x10, 0x01});
    EXPECT_EQ(ask(0x7E8, read), nrc(0x23, 0x7F));
}

TEST_F(FakeMemoryTest, TheEngineEcuReadsItsCodeOnlyInItsProgrammingSession)
{
    const Bytes code = read_of(ECUIO_SIM_MEMORY_CODE_ADDRESS, 32);
    ask(0x7E8, {0x10, 0x03});
    ask(0x7E8, {0x27, 0x01});
    ASSERT_EQ(ask(0x7E8, key_for(0x01)), (Bytes{0x67, 0x02}));
    EXPECT_EQ(ask(0x7E8, code), nrc(0x23, 0x31)) << "region B is programming's";
    EXPECT_EQ(ask(0x7E8, {0x10, 0x02}),
              (Bytes{0x50, 0x02, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_FALSE(fake_ecu::in_extended(0x7E8));
    EXPECT_EQ(ask(0x7E8, code), nrc(0x23, 0x33)) << "the session locks it";
    ask(0x7E8, {0x27, 0x01});
    ASSERT_EQ(ask(0x7E8, key_for(0x01)), (Bytes{0x67, 0x02}));
    const Bytes reply = ask(0x7E8, code);
    ASSERT_EQ(reply.size(), 33u);
    EXPECT_EQ(reply[0], 0x63);
    EXPECT_EQ(ask(0x7E8, {0x11, 0x01}), (Bytes{0x51, 0x01}));
    EXPECT_FALSE(fake_ecu::in_programming(0x7E8));
    EXPECT_EQ(ask(0x7E8, code), nrc(0x23, 0x7F)) << "a reset is the default";
    ASSERT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12"));
    EXPECT_EQ(ask(0x7E9, {0x10, 0x02}), nrc(0x10, 0x12))
        << "a module has no programming session";
}

TEST_F(FakeMemoryTest, AModuleHasNoMemory)
{
    ASSERT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12"));
    ask(0x7E9, {0x10, 0x03});
    EXPECT_EQ(ask(0x7E9, read_of(kBase, 16)), nrc(0x23, 0x11));
    EXPECT_EQ(ask(0x7E9, kHardware), kNoDid);
}

} // namespace
