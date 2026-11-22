// File/layer: tests/test_cli_elm327.cpp | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#include "test_cli_support.h"

// view --frames' choice of a set (cli/view_frames.h)
TEST(CliViewFramesTest, TheGivenProfileThenTheOwnFileThenTheRecordedOne)
{
    cli::RecordedSignals rec;
    rec.profile = "user.extras";
    rec.sha256 = std::string(64, 'a');
    cli::FramesSource s = cli::frames_source("obd2", rec, true);
    EXPECT_FALSE(s.own_file);
    EXPECT_EQ(s.profile, "obd2") << "--profile first";
    s = cli::frames_source("", rec, true);
    EXPECT_TRUE(s.own_file) << "the own file while it matches";
    EXPECT_TRUE(s.profile.empty());
    s = cli::frames_source("", rec, false);
    EXPECT_FALSE(s.own_file);
    EXPECT_EQ(s.profile, "user.extras") << "a changed copy: the recorded name";
    rec.sha256.clear();
    rec.profile = "obd2";
    s = cli::frames_source("", rec, true);
    EXPECT_FALSE(s.own_file) << "no file named, none to match";
    EXPECT_EQ(s.profile, "obd2");
}

TEST(CliViewFramesTest, ARecordingWithNoSignalsOfItsOwnSaysWhatToGive)
{
    cli::RecordedSignals rec;
    cli::FramesSource s = cli::frames_source("", rec, false);
    EXPECT_TRUE(s.profile.empty());
    EXPECT_FALSE(s.own_file);
    EXPECT_EQ(s.why,
              "its events.log names no profile: name one with --profile");
    rec.profile = "raw";
    s = cli::frames_source("", rec, false);
    EXPECT_TRUE(s.profile.empty());
    EXPECT_EQ(s.why, "it was recorded under raw, which decodes nothing: name a "
                     "profile with --profile");
    rec.profile = "uds-generic";
    s = cli::frames_source("", rec, false);
    EXPECT_TRUE(s.profile.empty());
    EXPECT_EQ(s.why, "uds-generic's signals come from its session's did= "
                     "list: give a --signals file");
    EXPECT_EQ(cli::frames_source("raw", rec, false).profile, "raw")
        << "a given one is looked up, and refused there";
}

TEST(CliViewFramesTest, AProfileWithNothingToDecodeIsRefused)
{
    EXPECT_EQ(cli::profile_refusal("obd2", true, 120), "");
    EXPECT_EQ(cli::profile_refusal("user.gone", false, 0),
              "no profile 'user.gone' is loaded: give its --signals file or "
              "--plugin");
    EXPECT_EQ(cli::profile_refusal("raw", true, 0),
              "the profile 'raw' has no signals to decode with");
}

/* ---------- The ELM327 emulator (ADR-0055) ---------- */

namespace {

/* The emulator before an unbound sim, at times the test gives */
struct Emu {
    ecuio_sim_t* sim = nullptr;
    cli::Elm327Emulator* emu = nullptr;
    int64_t t = 1000000; /* the next command's time, us */

    explicit Emu(const char* uri = "mock://emu?values=canned")
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << ecuio_last_error_detail();
        emu = new cli::Elm327Emulator(sim);
    }
    ~Emu()
    {
        delete emu;
        ecuio_sim_destroy(sim);
    }
    Emu(const Emu&) = delete;
    Emu& operator=(const Emu&) = delete;

    /* What's due by at */
    std::string heard(int64_t at)
    {
        std::string s;
        uint8_t buf[256];
        for (;;) {
            const size_t n = emu->to_host(buf, sizeof buf, at);
            if (n == 0) {
                return s;
            }
            s.append(reinterpret_cast<const char*>(buf), n);
        }
    }

    /* cmd and its CR at t, and everything it answers within 10 s; t
     * moves past it */
    std::string say(const std::string& cmd)
    {
        const std::string line = cmd + "\r";
        emu->from_host(reinterpret_cast<const uint8_t*>(line.data()),
                       line.size(), t);
        const std::string got = heard(t + 10000000);
        t += 20000000;
        return got;
    }

    /* cmd at t, without reading it: heard() then takes it at its times */
    void send(const std::string& cmd)
    {
        const std::string line = cmd + "\r";
        emu->from_host(reinterpret_cast<const uint8_t*>(line.data()),
                       line.size(), t);
    }

    /* The power-on echo off, line feeds off */
    void quiet() { (void)say("ATE0"); }
};

/* AT ST's default wait, 0x32 x 4.096 ms */
constexpr int64_t kWait = 0x32 * 4096;

} // namespace

TEST(CliElm327Test, TheResetAndTheSettings)
{
    Emu e;
    EXPECT_EQ(e.say("ATZ"), "ATZ\r\r\rELM327 v1.4b\r\r>") << "echo on";
    EXPECT_EQ(e.say("at e0"), "at e0\rOK\r\r>") << "lower case, spaces";
    EXPECT_EQ(e.say("ATI"), "ELM327 v1.4b\r\r>");
    EXPECT_EQ(e.say(""), "ELM327 v1.4b\r\r>") << "a CR alone repeats it";
    EXPECT_EQ(e.say("AT@1"), "OBDII to RS232 Interpreter\r\r>");
    EXPECT_EQ(e.say("ATRV"), "13.8V\r\r>");
    EXPECT_EQ(e.say("ATL1"), "OK\r\n\r\n>");
    EXPECT_EQ(e.say("ATL0"), "OK\r\r>");
    EXPECT_EQ(e.say("ATXYZ"), "?\r\r>");
    EXPECT_EQ(e.say("STI"), "?\r\r>") << "a plain ELM327";
    EXPECT_EQ(e.say("ATBRD10"), "?\r\r>") << "no baud change over TCP";
    EXPECT_EQ(e.say("0G"), "?\r\r>") << "not hex";
    EXPECT_EQ(e.say("0102030405060708"), "?\r\r>")
        << "8 bytes: past a Single Frame";
    EXPECT_EQ(e.say("ATD"), "OK\r\r>");
    EXPECT_EQ(e.say("ATI"), "ATI\rELM327 v1.4b\r\r>")
        << "the defaults: echo on";
    e.emu->power_on();
    EXPECT_EQ(e.say("ATI"), "ATI\rELM327 v1.4b\r\r>");
    EXPECT_EQ(e.emu->commands, 16u);
}

TEST(CliElm327Test, TheSearchAndTheProtocols)
{
    Emu e;
    e.quiet();
    EXPECT_EQ(e.say("ATDPN"), "0\r\r>");
    EXPECT_EQ(e.say("ATDP"), "AUTO\r\r>");
    /* the reply prints as it comes, the prompt after the adapter waited
     * for more */
    const int64_t at = e.t;
    e.send("0100");
    EXPECT_EQ(e.heard(at), "SEARCHING...\r41 00 9F FF 9F E3\r");
    EXPECT_EQ(e.heard(at + kWait - 1), "");
    EXPECT_EQ(e.heard(at + kWait), "\r>");
    e.t += 20000000;
    EXPECT_EQ(e.say("0100"), "41 00 9F FF 9F E3\r\r>") << "searched once";
    EXPECT_EQ(e.say("ATDPN"), "A6\r\r>");
    EXPECT_EQ(e.say("ATDP"), "AUTO, ISO 15765-4 (CAN 11/500)\r\r>");
    EXPECT_EQ(e.say("ATSP6"), "OK\r\r>");
    EXPECT_EQ(e.say("ATDPN"), "6\r\r>");
    EXPECT_EQ(e.say("010C"), "41 0C 1F 40\r\r>") << "the canned 2000 rpm";
    EXPECT_EQ(e.say("ATSP7"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "NO DATA\r\r>") << "no 29-bit ECU";
    EXPECT_EQ(e.say("ATSP8"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "CAN ERROR\r\r>")
        << "250 kbaud on a 500 kbaud bus";
    EXPECT_EQ(e.say("ATTP3"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "UNABLE TO CONNECT\r\r>");
    EXPECT_EQ(e.say("ATSPA6"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "SEARCHING...\r41 00 9F FF 9F E3\r\r>");
    EXPECT_EQ(e.emu->requests, 7u);
}

TEST(CliElm327Test, HeadersSpacesAndTheDlc)
{
    Emu e;
    e.quiet();
    (void)e.say("ATSP6");
    EXPECT_EQ(e.say("ATH1"), "OK\r\r>");
    EXPECT_EQ(e.say("010C"), "7E8 04 41 0C 1F 40 00 00 00\r\r>")
        << "padded with zeros, as the vcan sim does";
    (void)e.say("ATD1");
    EXPECT_EQ(e.say("010C"), "7E8 8 04 41 0C 1F 40 00 00 00\r\r>");
    (void)e.say("ATS0");
    EXPECT_EQ(e.say("010C"), "7E8804410C1F40000000\r\r>");
    (void)e.say("ATD0");
    (void)e.say("ATS1");
    EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                             "7E8 21 46 41 4B 45 31 31 4D\r"
                             "7E8 22 30 30 30 30 30 30 31\r\r>");
    (void)e.say("ATH0");
    EXPECT_EQ(e.say("0902"),
              "014\r0: 49 02 01 5A 5A 5A\r1: 46 41 4B 45 31 31 4D\r"
              "2: 30 30 30 30 30 30 31\r\r>");
    (void)e.say("ATS0");
    EXPECT_EQ(e.say("03"), "008\r0:430303010420\r1:C100\r\r>")
        << "a long message's last line holds its own bytes alone";
}

TEST(CliElm327Test, TheCountDigitEndsAtItsLine)
{
    Emu e;
    e.quiet();
    (void)e.say("ATSP6");
    (void)e.say("ATH1");
    const int64_t at = e.t;
    e.send("09021");
    EXPECT_EQ(e.heard(at), "7E8 10 14 49 02 01 5A 5A 5A\r\r>")
        << "the first line, the prompt at once";
    e.t += 20000000;
    EXPECT_EQ(e.say("09022"),
              "7E8 10 14 49 02 01 5A 5A 5A\r7E8 21 46 41 4B 45 31 31 4D\r\r>");
    EXPECT_EQ(e.say("0902G"), "?\r\r>");
}

TEST(CliElm327Test, TheHeaderAndTheReceiveFilterPickTheEcu)
{
    Emu e("mock://emu?modules=2");
    e.quiet();
    (void)e.say("ATSP6");
    (void)e.say("ATH1");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>")
        << "functional: the engine ECU's refusal is never sent";
    EXPECT_EQ(e.say("ATSH7E0"), "OK\r\r>");
    EXPECT_EQ(e.say("22D101"), "7E8 03 7F 22 31 00 00 00 00\r\r>");
    EXPECT_EQ(e.say("ATSH7E1"), "OK\r\r>");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>");
    (void)e.say("ATSH7DF");
    EXPECT_EQ(e.say("ATCRA7E8"), "OK\r\r>");
    EXPECT_EQ(e.say("22D101"), "NO DATA\r\r>") << "the filter keeps 0x7E9 out";
    (void)e.say("ATCRA");
    (void)e.say("ATCF7E0");
    (void)e.say("ATCM7F0");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>");
    (void)e.say("ATCM7FF");
    EXPECT_EQ(e.say("22D101"), "NO DATA\r\r>");
    (void)e.say("ATAR");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>");
}

TEST(CliElm327Test, FaultsAsAnAdapterSeesThem)
{
    {
        /* 7F 01 78 at once; the adapter waits P2* for the reply 300 ms on */
        Emu e("mock://emu?values=canned&faults=pending:1");
        e.quiet();
        (void)e.say("ATSP6");
        const int64_t at = e.t;
        e.send("010C");
        EXPECT_EQ(e.heard(at), "7F 01 78\r");
        EXPECT_EQ(e.heard(at + 299999), "");
        EXPECT_EQ(e.heard(at + 300000), "41 0C 1F 40\r");
        EXPECT_EQ(e.heard(at + 300000 + kWait), "\r>");
    }
    {
        /* 130 ms late: inside the default wait, past AT ST 19's */
        Emu e("mock://emu?values=canned&faults=late:1");
        e.quiet();
        (void)e.say("ATSP6");
        const int64_t at = e.t;
        e.send("010C");
        EXPECT_EQ(e.heard(at + 129999), "");
        EXPECT_EQ(e.heard(at + 130000), "41 0C 1F 40\r");
        EXPECT_EQ(e.heard(at + 130000 + kWait), "\r>");
        e.t += 20000000;
        EXPECT_EQ(e.say("ATST19"), "OK\r\r>");
        const int64_t at2 = e.t;
        e.send("010C");
        EXPECT_EQ(e.heard(at2 + 0x19 * 4096), "NO DATA\r\r>") << "lost";
    }
    {
        /* the last Consecutive Frame 160 ms after the one before */
        Emu e("mock://emu?values=canned&faults=stalled:1");
        e.quiet();
        (void)e.say("ATSP6");
        (void)e.say("ATH1");
        EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                                 "7E8 21 46 41 4B 45 31 31 4D\r"
                                 "7E8 22 30 30 30 30 30 30 31\r\r>");
        (void)e.say("ATST19");
        EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                                 "7E8 21 46 41 4B 45 31 31 4D\r\r>")
            << "past the shorter wait, the last frame is lost";
    }
    {
        Emu e("mock://emu?values=canned&faults=bad_sequence:1");
        e.quiet();
        (void)e.say("ATSP6");
        (void)e.say("ATH1");
        EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                                 "7E8 21 46 41 4B 45 31 31 4D\r"
                                 "7E8 23 30 30 30 30 30 30 31\r\r>")
            << "the last frame's number skips one";
    }
    {
        Emu e("mock://emu?values=canned&faults=silent:1");
        e.quiet();
        (void)e.say("ATSP6");
        EXPECT_EQ(e.say("010C"), "NO DATA\r\r>");
    }
}

TEST(CliElm327Test, ResponsesOffAndTheMonitor)
{
    Emu e;
    e.quiet();
    (void)e.say("ATSP6");
    EXPECT_EQ(e.say("ATR0"), "OK\r\r>");
    EXPECT_EQ(e.say("04"), ">") << "sent, nothing printed";
    ecuio_sim_stats_t s{};
    ASSERT_EQ(ecuio_sim_get_stats(e.sim, &s), ECUIO_OK);
    EXPECT_EQ(s.clears, 1u);
    (void)e.say("ATR1");
    EXPECT_EQ(e.say("ATMA"), "") << "nothing else on the bus";
    EXPECT_TRUE(e.emu->monitoring());
    const uint8_t space = ' ';
    e.emu->from_host(&space, 1, e.t);
    EXPECT_EQ(e.heard(e.t), "STOPPED\r\r>") << "any byte stops it";
    EXPECT_FALSE(e.emu->monitoring());
    EXPECT_EQ(e.say("ATMA"), "");
    EXPECT_EQ(e.say("x"), "STOPPED\r\r>");
    EXPECT_TRUE(e.emu->monitoring())
        << "the CR after the stop is a CR alone: it repeats AT MA";
}

TEST(CliElm327Test, AddressesAndLines)
{
    cli::Elm327Address a;
    ASSERT_TRUE(cli::parse_elm327_address("tcp:35000", &a));
    EXPECT_EQ(a.ip, 0x7F000001u);
    EXPECT_EQ(a.port, 35000);
    ASSERT_TRUE(cli::parse_elm327_address("tcp:0", &a));
    EXPECT_EQ(a.port, 0);
    ASSERT_TRUE(cli::parse_elm327_address("tcp:192.168.4.1:65535", &a));
    EXPECT_EQ(a.ip, 0xC0A80401u);
    EXPECT_EQ(a.port, 65535);
    ASSERT_TRUE(cli::parse_elm327_address("tcp:0.0.0.0:23", &a));
    EXPECT_EQ(a.ip, 0u);
    for (const char* bad :
         {"35000", "udp:35000", "tcp:", "tcp:65536", "tcp:035000", "tcp:-1",
          "tcp:1.2.3:5", "tcp:1.2.3.4.5:5", "tcp:256.0.0.1:5", "tcp:01.2.3.4:5",
          "tcp:localhost:5", "tcp:1.2.3.4:", "tcp::5", "tcp:1..3.4:5",
          "tcp:[::1]:5"}) {
        EXPECT_FALSE(cli::parse_elm327_address(bad, &a)) << bad;
    }
    EXPECT_EQ(cli::address_text(0x7F000001u, 35000), "127.0.0.1:35000");

    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::elm327_line(l, "mock://emu?modules=2", "127.0.0.1:35000");
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"elm327\",\"uri\":\"mock://emu?modules=2\","
              "\"listen\":\"127.0.0.1:35000\"}");
    cli::Line c(buf, sizeof buf);
    cli::client_line(c, "refused", "127.0.0.1:50123");
    EXPECT_EQ(std::string(c.data(), c.size()),
              "{\"type\":\"client\",\"event\":\"refused\","
              "\"peer\":\"127.0.0.1:50123\"}");
}

// A computed input has no age bound of its own (ADR-0062): its ages are the
// number signals' under it, so its line says so instead of the stale age
TEST(CliSignalFileTest, AComputedInputIsNamedComputedWithNoAge)
{
    const std::string json =
        "{\"havne_signals\":2,\"name\":\"Chain\","
        "\"source\":{\"name\":\"test\"},"
        "\"ecus\":[{\"id\":\"tcm\",\"rx\":\"7E9\"}],\"signals\":["
        "{\"key\":\"temp\",\"name\":\"Temp\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},"
        "\"field\":{\"byte\":0}},"
        "{\"key\":\"half\",\"name\":\"Half\",\"kind\":\"computed\","
        "\"expr\":\"temp / 2\",\"max_age\":{\"temp\":1.5}},"
        "{\"key\":\"sum\",\"name\":\"Sum\",\"kind\":\"computed\","
        "\"expr\":\"half + temp\"}]}";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    ASSERT_EQ(set.count, 3u);
    char buf[4096];
    const auto line = [&](uint32_t i) {
        cli::Line l(buf, sizeof buf);
        cli::def_line(l, set, set.defs[i], nullptr,
                      ecuio_signal_file_expr(file, &set.defs[i]));
        return std::string(l.data(), l.size());
    };
    EXPECT_EQ(ecuio_signal_file_format(file), 2u);
    const std::string half = line(1);
    EXPECT_NE(half.find("\"expr\":\"temp / 2\",\"inputs\""),
              std::string::npos)
        << half;
    EXPECT_NE(half.find("\"inputs\":[{\"id\":30000,\"max_age_us\":1500000}]"),
              std::string::npos)
        << half;
    const std::string sum = line(2);
    EXPECT_NE(sum.find("\"expr\":\"half + temp\",\"inputs\":[{\"id\":30001,\"max_age_us\":null,"
                       "\"computed\":true},{\"id\":30000,"
                       "\"max_age_us\":3000000}]"),
              std::string::npos)
        << sum;
    EXPECT_LE(sum.size(), cli::def_line_bytes(set, set.defs[2]));
    ecuio_signal_file_free(file);
}
