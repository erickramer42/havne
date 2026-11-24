// File/layer: tests/test_cli_unlock.cpp | test_cli, Qt-free
// Testing: UNIT: CliUnlockLineTest: stream --unlock's form and the
//          conversation and unlock lines (cli/conversation_lines.h)

#include "conversation_lines.h"

#include <gtest/gtest.h>

#include <string>

namespace {

std::string line_of(const ecuio_channel_conversation_t& r)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::conversation_line(l, r);
    return std::string(l.data(), l.size());
}

} // namespace

TEST(CliUnlockLineTest, TheUnlocksFormIsAnEcuAndAnHexLevel)
{
    std::string ecu;
    uint8_t level = 0;
    ASSERT_TRUE(cli::parse_unlock("pcm:01", &ecu, &level));
    EXPECT_EQ(ecu, "pcm");
    EXPECT_EQ(level, 0x01);
    ASSERT_TRUE(cli::parse_unlock("tcm:3f", &ecu, &level));
    EXPECT_EQ(level, 0x3F);
    for (const char* bad :
         {"pcm01", ":01", "pcm:", "pcm:001", "pcm:0x", "pcm:00", "pcm:g1",
          "an-ecu-id-longer-than-31-bytes-x:01"}) {
        EXPECT_FALSE(cli::parse_unlock(bad, &ecu, &level)) << bad;
    }
}

TEST(CliUnlockLineTest, EveryCodeHasAWordOfItsOwn)
{
    for (uint8_t s = ECUIO_CONVERSATION_NONE; s <= ECUIO_CONVERSATION_ENDED;
         ++s) {
        EXPECT_STRNE(cli::conversation_state_word(s), "unknown") << int(s);
    }
    for (uint8_t u = ECUIO_UNLOCK_NONE; u <= ECUIO_UNLOCK_REFUSED; ++u) {
        EXPECT_STRNE(cli::unlock_word(u), "unknown") << int(u);
    }
    for (uint8_t f = ECUIO_CONVERSATION_FAULT_NONE;
         f <= ECUIO_CONVERSATION_FAULT_CHANNEL; ++f) {
        EXPECT_STRNE(cli::conversation_fault_word(f), "unknown") << int(f);
    }
    EXPECT_STREQ(cli::unlock_word(ECUIO_UNLOCK_REFUSED + 1), "unknown");
}

TEST(CliUnlockLineTest, AReportsLineNamesItsModuleAndWhereTheWordStands)
{
    ecuio_channel_conversation_t r{};
    r.rx_id = 0x7E8;
    r.state = ECUIO_CONVERSATION_TALKING;
    EXPECT_EQ(line_of(r),
              R"({"type":"conversation","ecu":"0x7E8","state":"talking",)"
              R"("level":null,"unlock":"none","nrc":null,"failed":0,)"
              R"("delay_left_ms":0,"kept":true})");
    r.flags = ECUIO_CONVERSATION_NOT_KEPT;
    r.state = ECUIO_CONVERSATION_FAULTED;
    r.fault = ECUIO_CONVERSATION_FAULT_WINDOW;
    r.level = 0x01;
    r.unlock = ECUIO_UNLOCK_FAILED;
    r.nrc = 0x35;
    r.failed = 2;
    r.delay_left_ms = 9500;
    EXPECT_EQ(line_of(r),
              R"({"type":"conversation","ecu":"0x7E8","state":"faulted",)"
              R"("fault":"window","level":"01","unlock":"failed",)"
              R"("nrc":"35","failed":2,"delay_left_ms":9500,"kept":false})");
}

TEST(CliUnlockLineTest, TheUnlockLineSaysAskedDelayedOrRefused)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::unlock_line(l, 0x7E8, 0x01, ECUIO_OK, 0, nullptr);
    EXPECT_EQ(std::string(l.data(), l.size()),
              R"({"type":"unlock","ecu":"0x7E8","level":"01",)"
              R"("outcome":"asked"})");
    cli::Line d(buf, sizeof buf);
    cli::unlock_line(d, 0x7E8, 0x01, ECUIO_ERR_TOO_SOON, 41000,
                     "the delay runs");
    EXPECT_EQ(std::string(d.data(), d.size()),
              R"({"type":"unlock","ecu":"0x7E8","level":"01",)"
              R"("outcome":"delayed","wait_ms":41000,"code":)" +
                  std::to_string(ECUIO_ERR_TOO_SOON) +
                  R"(,"detail":"the delay runs"})");
    cli::Line x(buf, sizeof buf);
    cli::unlock_line(x, 0x7E8, 0x01, ECUIO_ERR_INVALID_ARGUMENT, 0,
                     "an unlock is under way");
    EXPECT_NE(
        std::string(x.data(), x.size()).find(R"("outcome":"refused","code":)"),
        std::string::npos);
}
