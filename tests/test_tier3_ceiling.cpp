// File/layer: tests/test_tier3_ceiling.cpp | C++17, Qt-free, every platform
// Testing: UNIT: Tier3CeilingTest (test_obd2)
//
// The operation tier's read ceiling (core/src/tier3_ceiling.h, ADR-0066
// sections 1 and 4), judged over every service and sub-function against
// a list written here apart from the header's own.

#include "tier3_ceiling.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

/* The four a dump sends beside tier 2's: 23, and an upload's 35, 36, 37 */
bool listed(unsigned sid)
{
    switch (sid) {
    case 0x23:
    case 0x35:
    case 0x36:
    case 0x37:
        return true;
    default:
        return false;
    }
}

TEST(Tier3CeilingTest, EveryServiceAndSubFunctionIsAsTheListHasIt)
{
    unsigned allowed = 0;
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            const bool got = ecuio_tier3_read_allowed(
                static_cast<uint8_t>(sid), static_cast<uint8_t>(sub));
            allowed += got ? 1u : 0u;
            ASSERT_EQ(got, listed(sid)) << std::hex << sid << " " << sub;
        }
    }
    EXPECT_EQ(allowed, 4u * 256u);
}

TEST(Tier3CeilingTest, NothingDownloadsWritesResetsOrClears)
{
    /* 34 opens a download, 3D writes memory, 2E a DID, 2F drives an
     * output, 31 runs a routine, 11 resets, 14 and 04 clear, 28 and 85
     * change what the module sends and stores */
    for (const unsigned sid : {0x34u, 0x3Du, 0x2Eu, 0x2Fu, 0x31u, 0x11u, 0x14u,
                               0x04u, 0x28u, 0x85u, 0x38u, 0x87u}) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            ASSERT_FALSE(ecuio_tier3_read_allowed(static_cast<uint8_t>(sid),
                                                  static_cast<uint8_t>(sub)))
                << std::hex << sid << " " << sub;
        }
    }
}

/* The two lists stand beside each other: the session, its keep-alive,
 * the unlock and the DID reads stay tier 2's, and tier 2 reads no memory */
TEST(Tier3CeilingTest, ItSharesNoRequestWithTierTwo)
{
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            const uint8_t s = static_cast<uint8_t>(sid);
            const uint8_t b = static_cast<uint8_t>(sub);
            ASSERT_FALSE(ecuio_tier3_read_allowed(s, b) &&
                         ecuio_tier2_allowed(s, b))
                << std::hex << sid << " " << sub;
        }
    }
    EXPECT_FALSE(ecuio_tier3_read_allowed(0x10, 0x03));
    EXPECT_FALSE(ecuio_tier3_read_allowed(0x10, 0x02));
    EXPECT_FALSE(ecuio_tier3_read_allowed(0x3E, 0x00));
    EXPECT_FALSE(ecuio_tier3_read_allowed(0x27, 0x01));
    EXPECT_FALSE(ecuio_tier3_read_allowed(0x22, 0xF1));
}

/* A plugin's code never causes a memory read: its own requests are
 * tier 2's reads, whatever the operation tier adds for core's runner */
TEST(Tier3CeilingTest, APluginsOwnRequestReadsNoMemory)
{
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        for (unsigned sub = 0; sub <= 0xFF; ++sub) {
            const uint8_t s = static_cast<uint8_t>(sid);
            const uint8_t b = static_cast<uint8_t>(sub);
            if (ecuio_tier3_read_allowed(s, b)) {
                ASSERT_FALSE(ecuio_tier2_guest_may_send(s, b))
                    << std::hex << sid << " " << sub;
            }
        }
    }
}

} // namespace
