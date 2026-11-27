#include "sim_memory_support.h"

#include "../core/src/sim_session.h"

#include <gtest/gtest.h>

namespace {

using sim_memory_test::Bytes;
using sim_memory_test::data_of;
using sim_memory_test::Node;
using sim_memory_test::nrc;
using sim_memory_test::read_of;
using sim_memory_test::upload_of;

constexpr uint32_t kCode = ECUIO_SIM_MEMORY_CODE_ADDRESS;
const Bytes kProgramming = {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4};

/* The code region's bytes as the node answers them */
Bytes code_of(const Node& n, uint32_t address, uint32_t size)
{
    Bytes want = {0x63};
    for (uint32_t i = 0; i < size; ++i) {
        want.push_back(ecuio_sim_memory_byte(n.s.memory, address + i));
    }
    return want;
}

TEST(SimProgrammingTest, TenZeroTwoEntersFromTheDefaultOrTheExtendedSession)
{
    for (const bool extended_first : {false, true}) {
        Node n;
        if (extended_first) {
            EXPECT_EQ(n.ask({0x10, 0x03}), sim_memory_test::kEntered);
        }
        EXPECT_TRUE(ecuio_sim_session_in(n.s, extended_first ? 0x03 : 0x01));
        EXPECT_EQ(n.ask({0x10, 0x02}), kProgramming);
        EXPECT_TRUE(ecuio_sim_session_in(n.s, ECUIO_STEP_SESSION_PROGRAMMING));
        EXPECT_FALSE(n.s.extended) << "never both";
        EXPECT_TRUE(ecuio_sim_memory_code_readable(n.s));
        EXPECT_EQ(n.ask({0x3E, 0x00}), (Bytes{0x7E, 0x00}));
        EXPECT_TRUE(n.s.programming) << "a keep-alive holds it";
    }
}

TEST(SimProgrammingTest, TheCodeRegionIsReadOnlyInTheProgrammingSession)
{
    Node n;
    n.ask({0x10, 0x03});
    n.unlock();
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x31))
        << "a range outside the extended session's regions";
    EXPECT_EQ(n.ask(upload_of(kCode, 16)), nrc(0x35, 0x31));
    EXPECT_EQ(n.ask({0x10, 0x02}), kProgramming);
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x33))
        << "the session change locked it";
    n.unlock();
    EXPECT_EQ(n.ask(read_of(kCode, 16)), code_of(n, kCode, 16));
    EXPECT_EQ(n.ask(read_of(kCode + ECUIO_SIM_MEMORY_CODE_BYTES - 8, 8)),
              code_of(n, kCode + ECUIO_SIM_MEMORY_CODE_BYTES - 8, 8));
    EXPECT_EQ(n.ask(read_of(kCode + ECUIO_SIM_MEMORY_CODE_BYTES - 8, 9)),
              nrc(0x23, 0x31))
        << "a byte past its end";
    EXPECT_EQ(n.ask(upload_of(kCode, 16)).at(0), 0x75);
    const Bytes calibration =
        data_of(ECUIO_SIM_MEMORY_ADDRESS, 16, ECUIO_SIM_DEFAULT_VIN);
    EXPECT_EQ(n.ask({0x37}), (Bytes{0x7F, 0x37, 0x24}));
    EXPECT_EQ(n.ask(read_of(ECUIO_SIM_MEMORY_ADDRESS, 16)), calibration)
        << "region A answers in the programming session too";
}

TEST(SimProgrammingTest, EachWayOutLeavesItLocked)
{
    const struct {
        Bytes request;
        Bytes reply;
        uint8_t session;
    } ways[] = {
        {{0x10, 0x01}, {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}, 0x01},
        {{0x10, 0x03}, sim_memory_test::kEntered, 0x03},
        {{0x11, 0x01}, {0x51, 0x01}, 0x01},
    };
    for (const auto& w : ways) {
        Node n;
        n.ask({0x10, 0x02});
        n.unlock();
        EXPECT_EQ(n.ask(w.request), w.reply);
        EXPECT_TRUE(ecuio_sim_session_in(n.s, w.session));
        EXPECT_FALSE(n.s.programming);
        EXPECT_FALSE(ecuio_sim_memory_code_readable(n.s));
        EXPECT_EQ(n.s.security.unlocked, 0u) << int{w.request[1]};
    }
    Node n;
    n.ask({0x10, 0x02});
    n.now += ECUIO_SIM_S3_US;
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x7F))
        << "S3 of silence: the default session";
    EXPECT_TRUE(ecuio_sim_session_in(n.s, 0x01));
    EXPECT_EQ(n.ask({0x11, 0x03}), nrc(0x11, 0x12));
    EXPECT_EQ(n.ask({0x11}), nrc(0x11, 0x13));
}

TEST(SimProgrammingTest, TheIdentifiersAnswerInTheProgrammingSession)
{
    Node n;
    n.ask({0x10, 0x02});
    EXPECT_EQ(n.ask(sim_memory_test::kHardware),
              sim_memory_test::ident_of(ECUIO_SIM_HARDWARE_DID,
                                        ECUIO_SIM_HARDWARE_TEXT));
}

/* Only a node with memory has something to program */
TEST(SimProgrammingTest, ANodeWithoutMemoryRefusesIt)
{
    Node n(false);
    EXPECT_EQ(n.ask({0x10, 0x02}), nrc(0x10, 0x12));
    EXPECT_FALSE(n.s.programming);
    EXPECT_EQ(n.ask({0x10, 0x03}), sim_memory_test::kEntered);
}

} // namespace
