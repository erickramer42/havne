// File/layer: tests/test_tier2_ceiling.cpp | C++17, Qt-free, every platform
// Testing: UNIT: Tier2CeilingTest (test_obd2)
//
// The conversation tier's service ceiling (core/src/tier2_ceiling.h,
// ADR-0066 section 1), judged over every service and sub-function
// against a list written here apart from the header's own.

#include "tier2_ceiling.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <utility>

namespace {

using Pair = std::pair<unsigned, unsigned>;

/* ADR-0066 section 1's list, a pair for each request tier 2 may cause */
std::set<Pair> ceiling()
{
    std::set<Pair> all;
    all.insert({0x10, 0x03});
    all.insert({0x3E, 0x00});
    all.insert({0x2C, 0x01});
    all.insert({0x2C, 0x03});
    for (unsigned sub = 0x01; sub <= 0x7E; ++sub) {
        if (sub != 0x5F && sub != 0x60) {
            all.insert({0x27, sub});
        }
    }
    for (const unsigned sid : {0x19u, 0x22u, 0x2Au}) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            all.insert({sid, sub});
        }
    }
    return all;
}

TEST(Tier2CeilingTest, EveryServiceAndSubFunctionIsAsTheListHasIt)
{
    const std::set<Pair> all = ceiling();
    unsigned allowed = 0;
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            const bool want = all.count({sid, sub}) != 0;
            allowed += want ? 1u : 0u;
            ASSERT_EQ(ecuio_tier2_allowed(static_cast<uint8_t>(sid),
                                          static_cast<uint8_t>(sub)),
                      want)
                << std::hex << sid << " " << sub;
        }
    }
    EXPECT_EQ(allowed, 1u + 1u + 2u + 124u + 3u * 256u);
}

TEST(Tier2CeilingTest, ASubFunctionSplitsReadFromWrite)
{
    /* the extended session, never the programming one or the default
     * (the close's 10 01 is the engine's own) */
    EXPECT_TRUE(ecuio_tier2_allowed(0x10, 0x03));
    EXPECT_FALSE(ecuio_tier2_allowed(0x10, 0x02));
    EXPECT_FALSE(ecuio_tier2_allowed(0x10, 0x01));
    EXPECT_FALSE(ecuio_tier2_allowed(0x10, 0x83));
    /* define by identifier and clear, never define by memory address */
    EXPECT_TRUE(ecuio_tier2_allowed(0x2C, 0x01));
    EXPECT_FALSE(ecuio_tier2_allowed(0x2C, 0x02));
    EXPECT_TRUE(ecuio_tier2_allowed(0x2C, 0x03));
    /* tester present answers */
    EXPECT_TRUE(ecuio_tier2_allowed(0x3E, 0x00));
    EXPECT_FALSE(ecuio_tier2_allowed(0x3E, 0x80));
}

TEST(Tier2CeilingTest, SecurityAccessKeepsToItsLevels)
{
    EXPECT_FALSE(ecuio_tier2_allowed(0x27, 0x00));
    EXPECT_TRUE(ecuio_tier2_allowed(0x27, 0x01));
    EXPECT_TRUE(ecuio_tier2_allowed(0x27, 0x5E));
    EXPECT_FALSE(ecuio_tier2_allowed(0x27, 0x5F)); /* ISO 26021's pair */
    EXPECT_FALSE(ecuio_tier2_allowed(0x27, 0x60));
    EXPECT_TRUE(ecuio_tier2_allowed(0x27, 0x61));
    EXPECT_TRUE(ecuio_tier2_allowed(0x27, 0x7E));
    EXPECT_FALSE(ecuio_tier2_allowed(0x27, 0x7F));
    /* bit 7 suppresses the reply an attempt is counted by */
    for (unsigned sub = 0x80; sub <= 0xFF; ++sub) {
        EXPECT_FALSE(ecuio_tier2_allowed(0x27, static_cast<uint8_t>(sub)))
            << std::hex << sub;
    }
}

TEST(Tier2CeilingTest, NothingWritesTransfersResetsOrClears)
{
    for (const unsigned sid :
         {0x04u, 0x11u, 0x14u, 0x23u, 0x28u, 0x2Eu, 0x2Fu, 0x31u, 0x34u, 0x35u,
          0x36u, 0x37u, 0x38u, 0x3Du, 0x85u}) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            ASSERT_FALSE(ecuio_tier2_allowed(static_cast<uint8_t>(sid),
                                             static_cast<uint8_t>(sub)))
                << std::hex << sid << " " << sub;
        }
    }
}

TEST(Tier2CeilingTest, APluginsOwnRequestIsAReadAndNeverADoor)
{
    const std::set<Pair> all = ceiling();
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            const bool want = (sid == 0x19 || sid == 0x22 || sid == 0x2C) &&
                              all.count({sid, sub}) != 0;
            ASSERT_EQ(ecuio_tier2_guest_may_send(static_cast<uint8_t>(sid),
                                                 static_cast<uint8_t>(sub)),
                      want)
                << std::hex << sid << " " << sub;
        }
    }
    /* core builds 27, enters and keeps the session, and nothing takes a
     * periodic reply */
    EXPECT_FALSE(ecuio_tier2_guest_may_send(0x27, 0x01));
    EXPECT_FALSE(ecuio_tier2_guest_may_send(0x27, 0x02));
    EXPECT_FALSE(ecuio_tier2_guest_may_send(0x10, 0x03));
    EXPECT_FALSE(ecuio_tier2_guest_may_send(0x3E, 0x00));
    EXPECT_FALSE(ecuio_tier2_guest_may_send(0x2A, 0x01));
    EXPECT_FALSE(ecuio_tier2_guest_may_send(0x2C, 0x02));
}

TEST(Tier2CeilingTest, OnlyFourServicesSplitBySubFunction)
{
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        EXPECT_EQ(ecuio_tier2_splits(static_cast<uint8_t>(sid)),
                  sid == 0x10 || sid == 0x27 || sid == 0x2C || sid == 0x3E)
            << std::hex << sid;
    }
}

} // namespace
