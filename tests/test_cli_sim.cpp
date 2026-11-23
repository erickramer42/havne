// File/layer: tests/test_cli_sim.cpp | C++17, Qt-free, every desktop
//             platform
// Testing: UNIT: CliSimTest (test_cli)
//
// The URI the sim line and the emulator's elm327 line show
// (cli/sim_line.h): the gateway's sa= seeds, keys and masks hidden,
// every other key as given.

#include "elm327_lines.h"
#include "sim_line.h"

#include <gtest/gtest.h>

#include <string>

TEST(CliSimTest, TheSimLineHidesTheGatewaysSeedsKeysAndMasks)
{
    const auto shown = [](const char* uri) { return cli::sim_shown_uri(uri); };
    const std::string g = "mock://sim?modules=2&gateway=1&behind=7E9&sa=";
    EXPECT_EQ(shown((g + "key:01020304:0A0B0C0D,11121314:1A1B1C1D").c_str()),
              g + "key:*");
    EXPECT_EQ(shown((g + "xor:5AA5&sa_delay=200").c_str()),
              g + "xor:*&sa_delay=200");
    EXPECT_EQ(shown((g + "add:01&route=reads:gated").c_str()),
              g + "add:*&route=reads:gated");
    EXPECT_EQ(shown((g + "key").c_str()), g + "key");
    EXPECT_EQ(shown((g + "never").c_str()), g + "never");
    EXPECT_EQ(shown((g + "never:250").c_str()), g + "never:250");
    EXPECT_EQ(shown((g + "never:2A").c_str()), g + "never:*");
    EXPECT_EQ(shown((g + "5AA5").c_str()), g + "*");
    EXPECT_EQ(
        shown("mock://sim?sa=xor:01&values=canned&sa=key:01020304:0A0B0C0D"),
        "mock://sim?sa=xor:*&values=canned&sa=key:*");
    for (const char* kept :
         {"mock://sim", "mock://sim?values=canned&vin=ZZZFAKE11M0000001",
          "mock://sim?gateway=1&sa_delay=200&visa=01",
          "replay://C:/runs/sa=key:01"}) {
        EXPECT_EQ(shown(kept), kept);
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::sim_line(l, "mock://sim?gateway=1&sa=xor:5AA5", "vcan0", false);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"sim\",\"uri\":\"mock://sim?gateway=1&sa=xor:*\","
              "\"on\":\"vcan0\",\"desk_bus\":false}");
    cli::Line e(buf, sizeof buf);
    cli::elm327_line(e, "mock://emu?gateway=1&sa=key:01020304:0A0B0C0D",
                     "127.0.0.1:1");
    EXPECT_EQ(std::string(e.data(), e.size()),
              "{\"type\":\"elm327\",\"uri\":\"mock://emu?gateway=1&sa=key:*\","
              "\"listen\":\"127.0.0.1:1\"}");
}
