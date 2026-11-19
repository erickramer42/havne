// File/layer: tests/test_cli_emulator.cpp | C++17, Qt-free, every desktop
//             platform
// Testing: UNIT: CliElm327Test (test_cli)
//
// The ELM327 emulator (cli/elm327_emulator.h) before an unbound sim
// whose modules keep a car's pace (fc=, cf=): a segmented reply's lines
// come at the module's times, not the adapter's own STmin 0 pace.

#include "elm327_emulator.h"

#include "ecuio/sim.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

/* The emulator before an unbound sim, at times the case gives */
struct PacedEmu {
    ecuio_sim_t* sim = nullptr;
    cli::Elm327Emulator* emu = nullptr;

    explicit PacedEmu(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << ecuio_last_error_detail();
        emu = new cli::Elm327Emulator(sim);
    }
    ~PacedEmu()
    {
        delete emu;
        ecuio_sim_destroy(sim);
    }
    PacedEmu(const PacedEmu&) = delete;
    PacedEmu& operator=(const PacedEmu&) = delete;

    void send(const std::string& cmd, int64_t at)
    {
        const std::string line = cmd + "\r";
        emu->from_host(reinterpret_cast<const uint8_t*>(line.data()),
                       line.size(), at);
    }

    /* What's due by at */
    std::string heard(int64_t at)
    {
        std::string s;
        uint8_t buf[256];
        for (size_t n; (n = emu->to_host(buf, sizeof buf, at)) != 0;) {
            s.append(reinterpret_cast<const char*>(buf), n);
        }
        return s;
    }

    /* cmd at at, everything it answers within a second */
    std::string say(const std::string& cmd, int64_t at)
    {
        send(cmd, at);
        return heard(at + 1000000);
    }
};

} // namespace

// fc=20000 and cf=30000: the VIN's First Frame at once, its first
// Consecutive Frame 20 ms on and the second 30 ms after that
TEST(CliElm327Test, AModulesFramesComeAtItsPace)
{
    PacedEmu e("mock://emu?values=canned&fc=20000&cf=30000");
    int64_t t = 1000000;
    for (const char* cmd : {"ATE0", "ATSP6", "ATH1"}) {
        (void)e.say(cmd, t);
        t += 2000000;
    }
    e.send("0902", t);
    EXPECT_EQ(e.heard(t), "7E8 10 14 49 02 01 5A 5A 5A\r");
    EXPECT_EQ(e.heard(t + 19999), "");
    EXPECT_EQ(e.heard(t + 20000), "7E8 21 46 41 4B 45 31 31 4D\r");
    EXPECT_EQ(e.heard(t + 49999), "");
    EXPECT_EQ(e.heard(t + 50000), "7E8 22 30 30 30 30 30 30 31\r");
}
