// File/layer: tests/test_elm327_session_uri.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest, Elm327LogTest (test_elm327)

#include "test_elm327_support.h"

/* The refusals' words: what was wrong and what's wanted */
TEST_F(Elm327SessionTest, RefusalsNameTheirCause)
{
    struct Case {
        std::string uri;
        ecuio_status_t status;
        std::string detail;
    };
    const std::vector<Case> cases = {
        {"elm327://", ECUIO_ERR_BAD_URI,
         std::string("elm327:// needs a port: ") + elm327_port_name_example() +
             ", or " + elm327_tcp_example()},
        {"elm327://obdx.local:23", ECUIO_ERR_BAD_URI,
         std::string("the port must be ") + elm327_port_name_example() +
             ", or " + elm327_tcp_example()},
        {"elm327://192.168.0.10:35000?serial=38400", ECUIO_ERR_BAD_URI,
         "?serial= is a serial port's rate, and 192.168.0.10:35000 is a TCP "
         "address, which has none"},
        {uri("?baud=125000"), ECUIO_ERR_BAD_URI,
         "?baud= needs 500000 or 250000, not '125000'"},
        {uri("?serial=9"), ECUIO_ERR_BAD_URI,
         "?serial= needs a serial rate from 300 to 4000000, not '9'"},
        {uri("?flags=0"), ECUIO_ERR_BAD_URI,
         "elm327:// takes no query key 'flags', only baud, serial, filter, "
         "txid, poll, probe, identify, diagnose, pack, scan, gap, profile, "
         "did, session, keepalive_ms and packframes"},
        {uri("?gap=1001"), ECUIO_ERR_BAD_URI,
         "?gap= needs a time in ms from 0 to 1000, not '1001'"},
        {uri("?packframes=0"), ECUIO_ERR_BAD_URI,
         "?packframes= needs a frame count from 1 to 15, not '0'"},
        {uri("?pack=0&packframes=1"), ECUIO_ERR_BAD_URI,
         "?packframes= sizes Service 01 packs: it can't go with ?pack=0"},
        {uri("?probe=0&packframes=2"), ECUIO_ERR_BAD_URI,
         "?packframes= sizes Service 01 packs: it can't go with ?probe=0"},
        {uri("?poll=0x0C&packframes=1"), ECUIO_ERR_BAD_URI,
         "?packframes= sizes Service 01 packs: it can't go with ?poll="},
        {uri("?profile=uds-generic&did=7E8:1E1C&did=18DAF110:1E1C"),
         ECUIO_ERR_BAD_URI,
         "the channels mix 11-bit and 29-bit IDs, which one connection "
         "can't carry"},
        {uri("?profile=mock"), ECUIO_ERR_NOT_IMPLEMENTED,
         "elm327:// listens on its channels' IDs, and this session has "
         "none: add ?filter=, or use a profile that declares a channel"},
    };
    for (const Case& c : cases) {
        ecuio_session_t* s = nullptr;
        EXPECT_EQ(ecuio_session_create(c.uri.c_str(), &s), c.status) << c.uri;
        EXPECT_EQ(std::string(ecuio_last_error_detail()), c.detail) << c.uri;
        ecuio_session_destroy(s);
    }
}

/* A port that won't open says why, naming the port (LOAD_FAILED) */
TEST_F(Elm327SessionTest, APortThatWontOpenSaysWhy)
{
    set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &s), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string(kPort) + ": no such port");
    EXPECT_EQ(s, nullptr);
}

/* The serial rate reaches the opener: ?serial=, else 38400 */
TEST_F(Elm327SessionTest, TheSerialRateReachesThePort)
{
    for (const auto& c :
         {std::make_pair(std::string(""), 38400u),
          std::make_pair(std::string("?serial=115200"), 115200u)}) {
        const std::string u = uri(c.first);
        Session s;
        ASSERT_EQ(s.create(u), ECUIO_OK) << u;
        std::lock_guard<std::mutex> g(fake_elm327::state().lock);
        EXPECT_EQ(fake_elm327::state().last_serial, c.second) << u;
        EXPECT_STREQ(fake_elm327::state().last_name, kPort);
    }
}

/* A WiFi adapter's address is a port name on every platform: it reaches
 * the opener whole, and the session runs over it as over a serial port */
TEST_F(Elm327SessionTest, AnAddressIsAPortOnEveryPlatform)
{
    Session s;
    ASSERT_EQ(s.create("elm327://192.168.4.1:23?poll=0x0C"), ECUIO_OK)
        << ecuio_last_error_detail();
    {
        std::lock_guard<std::mutex> g(fake_elm327::state().lock);
        EXPECT_STREQ(fake_elm327::state().last_name, "192.168.4.1:23");
    }
    s.start();
    EXPECT_TRUE(
        wait_for([&] { return s.cap.count("41 0C 1F 40") >= 2; }, 5000));
}

/* A contract command answered wrongly refuses the connect, naming the
 * command and its answer (VENDOR_REFUSED); so does a reset that isn't an
 * ELM327's, after a second try */
TEST_F(Elm327SessionTest, AWrongSetupAnswerRefusesTheConnect)
{
    set_knobs([](fake_elm327::knobs& k) { k.refuse = "ATH1"; });
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &s),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string("the adapter on ") + kPort +
                  " answered ATH1 with '?', not OK");
    EXPECT_EQ(fake_count("ATS0"), 1u);
    EXPECT_EQ(fake_count("ATD0"), 0u) << "nothing after the refusal";

    fake_elm327::with([](fake_elm327::adapter& a) {
        a.clear();
        a.k.version = "OBDII to RS232 Interpreter";
    });
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &s),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string("the adapter on ") + kPort +
                  " answered ATZ with 'OBDII to RS232 Interpreter', not an "
                  "ELM327 version");
    EXPECT_EQ(fake_count("ATZ"), 2u);
    EXPECT_EQ(s, nullptr);

    /* A reset answered OK, then AT I with no version */
    fake_elm327::with([](fake_elm327::adapter& a) {
        a.clear();
        a.k.reset_ok = true;
        a.k.version = "OBDII to RS232 Interpreter";
    });
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &s),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string("the adapter on ") + kPort +
                  " answered ATI with 'OBDII to RS232 Interpreter', not an "
                  "ELM327 version");
    EXPECT_EQ(fake_count("ATZ"), 1u);
    EXPECT_EQ(fake_count("ATE0"), 0u) << "nothing after the refusal";
    EXPECT_EQ(s, nullptr);
}

/* ---- The wire log (core/src/elm327_log.h) ---------------------------- */

/* Without frame logging: the header, the setup and the readings, each
 * command with what came back and its prompt; the requests stay out */
TEST_F(Elm327SessionTest, TheWireLogHoldsTheSetupAndTheReadings)
{
    const EnvSet off("ECUIO_SUR_LOG_FRAMES", nullptr);
    const LogDir logs("setup");
    {
        Session s;
        ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
        s.start();
        ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 3; }, 3000));
    }
    const std::string log = logs.only_log();
    EXPECT_TRUE(in_order(
        log, {"] Havne ELM327 wire log (ADR-0026), core ",
              std::string("] uri ") + uri("?poll=0x0C"),
              std::string("] ") + kPort +
                  " at 38400 bit/s; CAN at 500000 bit/s, protocol 6; the setup",
              "] > ATZ", "] < ELM327 v1.4b", "] prompt ", " ms after ATZ",
              "] > ATE0", "] < OK", "] > ATST19", "] > ATI", "] > STI",
              "] < STN2255 v9.9.9", "] > STVR", "] < 13.80", "] > ATPPS",
              "] the session closes", std::string("] ") + kPort + " closed"}));
    EXPECT_EQ(log.find("] > 010C"), std::string::npos) << log;
    EXPECT_EQ(log.find("] < 7E8"), std::string::npos) << log;
}

/* With frame logging, as a recording sets it: every request, every line
 * of its reply, and its prompt's time after the request */
TEST_F(Elm327SessionTest, TheWireLogTakesEveryRequestWhenRecording)
{
    const EnvSet on("ECUIO_SUR_LOG_FRAMES", "1");
    const LogDir logs("requests");
    {
        Session s;
        ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
        s.start();
        ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 3; }, 3000));
    }
    const std::string log = logs.only_log();
    EXPECT_TRUE(
        in_order(log, {"every request and reply (ECUIO_SUR_LOG_FRAMES=1)",
                       "] > ATPPS", "] > 010C1", "] < 7E8", " ms after 010C1",
                       "] > 010C1", "] < 7E8"}));
}

/* A refused setup says why, and a dropped link shows its failure, the
 * reopens that fail and the one that brings the setup back */
TEST_F(Elm327SessionTest, TheWireLogNamesFailuresAndRecoveries)
{
    const EnvSet off("ECUIO_SUR_LOG_FRAMES", nullptr);
    {
        const LogDir logs("refused");
        set_knobs([](fake_elm327::knobs& k) { k.refuse = "ATH1"; });
        ecuio_session_t* s = nullptr;
        EXPECT_EQ(ecuio_session_create(uri().c_str(), &s),
                  ECUIO_ERR_VENDOR_REFUSED);
        EXPECT_TRUE(
            in_order(logs.only_log(),
                     {"] > ATH1", "] < ?",
                      std::string("] the setup failed: the adapter on ") +
                          kPort + " answered ATH1 with '?', not OK",
                      std::string("] ") + kPort + " closed"}));
        set_knobs([](fake_elm327::knobs& k) { k.refuse = nullptr; });
    }
    const LogDir logs("dropped");
    {
        Session s;
        ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
        s.start();
        ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
        set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
        ASSERT_TRUE(wait_for(
            [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; },
            3000));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        set_knobs([](fake_elm327::knobs& k) { k.dropped = false; });
        ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    }
    EXPECT_TRUE(in_order(
        logs.only_log(),
        {"] the port failed (", std::string("] ") + kPort + " closed",
         std::string("] reopening ") + kPort + " failed: no such port",
         std::string("] ") + kPort + " reopened", "] > ATZ", "] > ATST19"}));

    const LogDir reset_logs("reset");
    {
        Session s;
        ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
        s.start();
        ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
        const uint32_t resets = fake_count("ATE0");
        fake_elm327::with([](fake_elm327::adapter& a) {
            a.spontaneous_reset(fake_elm327::steady_us());
        });
        ASSERT_TRUE(
            wait_for([&] { return fake_count("ATE0") > resets; }, 3000));
    }
    EXPECT_TRUE(in_order(reset_logs.only_log(),
                         {"] > ATPPS", "] unasked: ELM327 v1.4b",
                          "] the adapter reset: the setup runs again",
                          "] > ATZ", "] > ATE0"}));
}

/* Each connect starts a log; the oldest past the newest 50 go, and a
 * name of another shape is never touched */
TEST_F(Elm327SessionTest, TheWireLogKeepsTheNewest50)
{
    const LogDir logs("kept");
    for (int i = 0; i < 55; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "ecuio-elm327.20200101-000000-%03d.log",
                 i);
        std::ofstream(logs.path() / name) << "earlier\n";
    }
    std::ofstream(logs.path() / "ecuio-elm327.notes.log") << "mine\n";
    std::ofstream(logs.path() / "other.log") << "mine\n";
    {
        Session s;
        ASSERT_EQ(s.create(uri()), ECUIO_OK);
    }
    const std::vector<std::string> names = logs.names();
    const auto kept =
        std::count_if(names.begin(), names.end(), [](const std::string& n) {
            return elm327_log_kept_name(n.c_str());
        });
    EXPECT_EQ(kept, ELM327_KEPT_LOGS);
    const auto has = [&](const char* n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    EXPECT_FALSE(has("ecuio-elm327.20200101-000000-005.log"))
        << "the oldest go";
    EXPECT_TRUE(has("ecuio-elm327.20200101-000000-006.log"));
    EXPECT_TRUE(has("ecuio-elm327.notes.log"));
    EXPECT_TRUE(has("other.log"));
    EXPECT_FALSE(has("ecuio-elm327.20200101-000000-000.log"));
}

/* The names the log keeps, and how it shows a byte that isn't text */
TEST(Elm327LogTest, KeptNamesAndTextHaveTheirShape)
{
    EXPECT_TRUE(elm327_log_kept_name("ecuio-elm327.20261027-172533-123.log"));
    EXPECT_TRUE(elm327_log_kept_name("ecuio-elm327.20261027-172533-123-2.log"));
    EXPECT_TRUE(elm327_log_kept_name("ecuio-elm327.20261027-172533-123-9.log"));
    for (const char* other : {"ecuio-elm327.log", "ecuio-elm327.notes.log",
                              "ecuio-elm327.20261027-172533-123-1.log",
                              "ecuio-elm327.20261027-172533-123-10.log",
                              "ecuio-elm327.2026102-172533-123.log",
                              "ecuio-elm327.20261027-172533-123.log.bak",
                              "ecuio-surrogate.20261027-172533-123.log"}) {
        EXPECT_FALSE(elm327_log_kept_name(other)) << other;
    }

    char out[32];
    elm327_log_text("AB\x01\\>\x8F", 6, out, sizeof(out));
    EXPECT_STREQ(out, "AB\\x01\\x5C>\\x8F");
    elm327_log_text("ABCDEF", 6, out, 4);
    EXPECT_STREQ(out, "ABC") << "cut to fit, terminated";
    elm327_log_text("A\x01", 2, out, 5);
    EXPECT_STREQ(out, "A") << "an escape is never cut part way";
}
