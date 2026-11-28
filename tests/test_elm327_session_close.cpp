// File/layer: tests/test_elm327_session_close.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)

#include "test_elm327_support.h"

namespace {

/* Each session-control command the adapter saw, with the header (the
 * last AT SH) it went out under */
struct SessionCommand {
    std::string command;
    std::string header;
};

std::vector<SessionCommand>
session_commands(const std::vector<std::string>& sent)
{
    std::vector<SessionCommand> out;
    std::string header;
    for (const std::string& c : sent) {
        if (c.rfind("ATSH", 0) == 0) {
            header = c;
        } else if (c == "1003" || c == "1001") {
            out.push_back({c, header});
        }
    }
    return out;
}

/* What a loaded machine may add to a close's bound */
constexpr long long kCloseMarginMs = 3000;

uint32_t interrupts()
{
    return on_fake<uint32_t>(
        [](fake_elm327::adapter& a) { return a.interrupts; });
}

long long close_ms(Session& s);

/* The close comes just after a request whose reply takes delay_ms, input
 * stopping a reply as the adapter's does. more, if given, changes knobs in
 * the same lock: a request counted before it can't take them */
void close_mid_reply(Session& s, uint32_t delay_ms, long long* ms = nullptr,
                     void (*more)(fake_elm327::knobs&) = nullptr)
{
    uint32_t asked = 0;
    fake_elm327::with([&](fake_elm327::adapter& a) {
        a.k.input_stops = true;
        a.k.reply_delay_ms = delay_ms;
        if (more != nullptr) {
            more(a.k);
        }
        asked = a.requests;
    });
    ASSERT_TRUE(wait_for(
        [&] {
            return on_fake<uint32_t>([](fake_elm327::adapter& a) {
                       return a.requests;
                   }) > asked;
        },
        10000));
    const long long took = close_ms(s);
    if (ms != nullptr) {
        *ms = took;
    }
}

long long close_ms(Session& s)
{
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

} // namespace

/* A close while a reply is under way waits for its prompt before 10 01:
 * a byte typed into the reply would stop it, and the 10 01 with it
 * (docs/bench-sessions/2026-11-11-ext-session-car.md). The reply takes
 * 600 ms, longer than any wait counted from the close's start would give
 * it; the module answers 50 01 and leaves the session */
TEST_F(Elm327SessionTest, TheCloseWaitsForTheReplyUnderWayBefore1001)
{
    SessionsReset reset;
    LogDir logs("close-waits");
    Session s;
    ASSERT_NO_FATAL_FAILURE(entered(s));
    ASSERT_NO_FATAL_FAILURE(close_mid_reply(s, 600));
    EXPECT_EQ(interrupts(), 0u) << "a byte went into a reply under way";
    const std::vector<std::string> sent = fake_commands();
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.back(), "1001");
    EXPECT_EQ(fake_count("1001"), 1u);
    EXPECT_FALSE(fake_ecu::in_extended(0x7E8)) << "left at the close";
    EXPECT_TRUE(in_order(logs.only_log(),
                         {"the close waits for ", "'s prompt",
                          "the close returns 7E8 to the default session",
                          "7E8 is back in the default session (50 01)"}));
}

/* A reply still running at its wait (ELM327_LEAVE_MS from its send) is
 * stopped with a space, which the adapter answers STOPPED, and 10 01
 * follows the prompt */
TEST_F(Elm327SessionTest, AReplyStillRunningAtTheCloseIsStoppedBefore1001)
{
    SessionsReset reset;
    LogDir logs("close-stops");
    Session s;
    ASSERT_NO_FATAL_FAILURE(entered(s));
    ASSERT_NO_FATAL_FAILURE(close_mid_reply(s, 1500));
    EXPECT_EQ(interrupts(), 1u) << "the space stops the reply, once";
    const std::vector<std::string> sent = fake_commands();
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.back(), "1001");
    EXPECT_FALSE(fake_ecu::in_extended(0x7E8)) << "left at the close";
    EXPECT_TRUE(in_order(logs.only_log(),
                         {"the close stops ", "'s reply with a space",
                          "the close returns 7E8 to the default session"}));
}

/* An adapter printing 7F 22 78 every 50 ms holds no read at the close
 * past its wait: the close ends within ELM327_CLOSE_MS (2 s), not at the
 * answer 4 s on */
TEST_F(Elm327SessionTest, AChattyReplyCantStretchTheClose)
{
    SessionsReset reset;
    Session s;
    ASSERT_NO_FATAL_FAILURE(entered(s));
    long long ms = 0;
    ASSERT_NO_FATAL_FAILURE(
        close_mid_reply(s, 4000, &ms, [](fake_elm327::knobs& k) {
            k.pending = true;
            k.pending_count = 80;
            k.pending_gap_ms = 50;
        }));
    EXPECT_LT(ms, 2000 + kCloseMarginMs) << "the lines kept the close waiting";
    EXPECT_EQ(fake_commands().back(), "1001");
}

/* 10 01 answered STOPPED is sent once more, and the second leaves the
 * session */
TEST_F(Elm327SessionTest, A1001AnsweredStoppedIsSentOnceMore)
{
    SessionsReset reset;
    Session s;
    ASSERT_NO_FATAL_FAILURE(entered(s));
    set_knobs([](fake_elm327::knobs& k) { k.stopped_once = "1001"; });
    (void)close_ms(s);
    EXPECT_EQ(fake_count("1001"), 2u);
    EXPECT_FALSE(fake_ecu::in_extended(0x7E8)) << "left at the close";
}

/* An adapter that stops answering holds the close no longer than
 * ELM327_CLOSE_MS (2 s), and one whose port fails closes at once */
TEST_F(Elm327SessionTest, ACloseOnASilentOrDeadAdapterEndsWithinItsWaits)
{
    SessionsReset reset;
    {
        Session s;
        ASSERT_NO_FATAL_FAILURE(entered(s));
        set_knobs([](fake_elm327::knobs& k) { k.silent = true; });
        EXPECT_LT(close_ms(s), 2000 + kCloseMarginMs)
            << "a silent adapter held the close";
    }
    set_knobs([](fake_elm327::knobs& k) { k.silent = false; });
    {
        Session s;
        ASSERT_NO_FATAL_FAILURE(entered(s));
        set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
        EXPECT_LT(close_ms(s), kCloseMarginMs) << "a dead port held the close";
    }
}

/* Without ?session=03 the engine ECU stays in the default session: D200
 * is refused (7F 22 31), never answered, while 1E1C reads */
TEST_F(Elm327SessionTest, WithoutASessionD200IsRefused)
{
    SessionsReset reset;
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=1E1C,D200&identify=0"
                           "&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    const std::vector<uint8_t> engine = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int a = 0;
            for (const Capture::Frame& f : s.cap.frames) {
                a += f.id == 0x7E8u && f.data == engine;
            }
            return a >= 3;
        },
        5000));
    EXPECT_EQ(fake_count("1003"), 0u);
    std::lock_guard<std::mutex> g(s.cap.m);
    for (const Capture::Frame& f : s.cap.frames) {
        EXPECT_FALSE(f.data.size() >= 3 && f.data[0] == 0x62 &&
                     f.data[1] == 0xD2)
            << "D200 answered in the default session";
    }
}

/* A module that refuses 10 03 goes on reading in the default session,
 * and the close sends 10 01 to the engine ECU alone */
TEST_F(Elm327SessionTest, AModuleThatRefusesTheSessionIsReadInTheDefaultOne)
{
    SessionsReset reset;
    ASSERT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12"));
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=D200,7E9:D801"
                           "&session=03&identify=0&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    const std::vector<uint8_t> d200 = {0x62, 0xD2, 0x00, 0x1A, 0x2C};
    const std::vector<uint8_t> mod = {0x62, 0xD8, 0x01, 0x00, 0x12};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            bool a = false;
            bool b = false;
            for (const Capture::Frame& f : s.cap.frames) {
                a = a || (f.id == 0x7E8u && f.data == d200);
                b = b || (f.id == 0x7E9u && f.data == mod);
            }
            return a && b;
        },
        5000))
        << "the engine ECU's D200 and the module's D801 both read";
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    const std::vector<SessionCommand> cmds = session_commands(fake_commands());
    int entered_engine = 0;
    int entered_module = 0;
    int left = 0;
    for (const SessionCommand& c : cmds) {
        if (c.command == "1003") {
            entered_engine += c.header == "ATSH7E0";
            entered_module += c.header == "ATSH7E1";
        } else {
            ++left;
            EXPECT_EQ(c.header, "ATSH7E0")
                << "10 01 goes to the ECU in the session alone";
        }
    }
    EXPECT_EQ(entered_engine, 1);
    EXPECT_GE(entered_module, 1) << "each channel tries the session";
    EXPECT_EQ(left, 1);
}

/* ?session= and ?keepalive_ms= are refused where the engine can't use
 * them, each with its reason */
TEST_F(Elm327SessionTest, TheSessionKeysAreRefusedWhereTheyCannotApply)
{
    struct Case {
        const char* query;
        const char* detail;
    };
    const Case cases[] = {
        {"?profile=obd2&session=03",
         "?session=03 belongs to profile=uds-generic, the profile that reads "
         "DIDs"},
        {"?profile=uds-generic&did=1E1C&keepalive_ms=1000",
         "?keepalive_ms= keeps the extended session alive: it needs "
         "?session=03"},
        {"?profile=uds-generic&did=1E1C&session=02",
         "?session= needs 01 (the default session) or 03 (the extended one), "
         "not '02'"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.query);
        Session s;
        EXPECT_EQ(s.create(uri(c.query)), ECUIO_ERR_BAD_URI);
        EXPECT_STREQ(ecuio_last_error_detail(), c.detail);
    }
}

/* Two ECUs asked through one adapter, each at the engine's floor (20 ms
 * send to send on each channel): the fake answers at once, so the
 * worker's wait while no command is out sets the pace; never faster than
 * the floor, and both channels kept asked */
TEST_F(Elm327SessionTest, TwoEcusAreAskedAtTheEnginesFloor)
{
    Session s;
    ASSERT_EQ(
        s.create(uri(
            "?profile=uds-generic&did=1E1C,7E9:1E1C&identify=0&diagnose=0")),
        ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    const auto requests = [] {
        return on_fake<uint32_t>(
            [](fake_elm327::adapter& a) { return a.requests; });
    };
    /* Judged by the fake's stamps on its first requests, not a count in
     * a wall-clock window that load thins: the two channels take turns,
     * so request i to request i + 2 is one channel's period, and its
     * median is the floor's (about 21 ms) whatever a few stalls add. The
     * floor is judged; the ceiling is loose, 4x the floor, since a loaded
     * machine's median reaches about 34 ms under a full verify.sh and
     * macOS runners end a plain sleep up to 18 ms late: it fails a pace
     * that drifted far slow, not load. The high-resolution timer's own
     * pace is CoreTest.APreciseWait*'s */
    static constexpr uint32_t want = 40; /* no capture: MSVC and Clang agree */
    ASSERT_TRUE(wait_for([&] { return requests() >= want; }, 5000))
        << "no requests";
    const std::vector<int64_t> stamps =
        on_fake<std::vector<int64_t>>([](fake_elm327::adapter& a) {
            return std::vector<int64_t>(a.request_us, a.request_us + want);
        });
    std::vector<int64_t> periods;
    for (size_t i = 0; i + 2 < stamps.size(); ++i) {
        periods.push_back(stamps[i + 2] - stamps[i]);
    }
    std::sort(periods.begin(), periods.end());
    const int64_t median = periods[periods.size() / 2];
    RecordProperty("period_median_us", static_cast<int>(median));
    EXPECT_GE(median, 19000) << "requests came faster than the floor";
    EXPECT_LE(median, 4 * 20000) << "requests came far slower than the floor";
    /* one channel alone at the floor passes both bounds: both answer */
    EXPECT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int a = 0;
            int b = 0;
            for (const Capture::Frame& f : s.cap.frames) {
                a += f.id == 0x7E8u;
                b += f.id == 0x7E9u;
            }
            return a >= 5 && b >= 5;
        },
        5000))
        << "one ECU left unasked";
}

/* 29-bit channels: protocol 7, the priority and header in two commands,
 * the 29-bit receive filter, and frames flagged EXTENDED */
TEST_F(Elm327SessionTest, TwentyNineBitChannelsUseTheirOwnProtocol)
{
    Session s;
    ASSERT_EQ(s.create(uri("?filter=0x18DAF110&txid=0x18DA10F1&poll=0x0C")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const std::vector<std::string> setup = fake_commands();
    for (const char* cmd : {"ATSP7", "ATCP18", "ATSHDA10F1", "ATCRA18DAF110"}) {
        EXPECT_NE(std::find(setup.begin(), setup.end(), cmd), setup.end())
            << cmd;
    }
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("41 0C", &f));
    EXPECT_EQ(f.id, 0x18DAF110u);
    EXPECT_EQ(f.flags, uint32_t{ECUIO_FRAME_FLAG_EXTENDED |
                                ECUIO_FRAME_FLAG_REASSEMBLED});
}

/* ?baud=250000 picks protocol 8, and the session info says so */
TEST_F(Elm327SessionTest, TheCanRatePicksTheProtocol)
{
    Session s;
    ASSERT_EQ(s.create(uri("?baud=250000&poll=0x0C")), ECUIO_OK);
    EXPECT_EQ(fake_count("ATSP8"), 1u);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_EQ(info.baud, 250000u);
}

/* raw has the engine ECU's channel (ADR-0013's appendix), so ?poll=
 * asks it; a lone ?poll= sends nothing else, and with no def to size the
 * reply, no count digit */
TEST_F(Elm327SessionTest, RawWithPollAsksTheEngineEcu)
{
    Session s;
    ASSERT_EQ(s.create(uri("?profile=raw&poll=0x0C")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 3; }, 3000));
    /* All under one lock: the worker keeps polling between two reads */
    const std::vector<uint32_t> counts =
        on_fake<std::vector<uint32_t>>([](fake_elm327::adapter& a) {
            return std::vector<uint32_t>{a.count("010C"), a.requests,
                                         a.count("010C1"), a.count("0100")};
        });
    EXPECT_EQ(counts[3], 1u) << "the start's probe";
    EXPECT_EQ(counts[0] + counts[3], counts[1]) << "only ?poll='s request";
    EXPECT_EQ(counts[2], 0u);
}

namespace {

/* A set reading PID 0x7F (engine run time, 13 data bytes: a First Frame
 * and two Consecutive Frames) from the engine ECU, one def at
 * start_byte, 4 bytes, registered as name */
void register_run_time(const char* name, ecuio_signal_def_t* def,
                       ecuio_signal_set_t* set, uint16_t start_byte)
{
    static const ecuio_channel_decl_t kEngine[] = {{0x7E8u, 0x7E0u, 0, 0, {}}};
    *def = ecuio_signal_def_t{};
    def->id = 1;
    def->message_id = 0x7E8u;
    def->mux_length = 2;
    def->mux_key = 0x417Fu;
    def->kind = ECUIO_VALUE_UINT;
    def->byte_order = ECUIO_BYTE_ORDER_BE;
    def->start_byte = start_byte;
    def->bit_length = 32;
    def->scale = 1.0;
    *set = ecuio_signal_set_t{};
    set->layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set->count = 1;
    set->defs = def;
    set->channel_count = 1;
    set->channels = kEngine;
    ASSERT_EQ(ecuio_profile_register(name, set, nullptr), ECUIO_OK);
}

} // namespace

/* A def ending past a Single Frame (0x7F's idle time, bytes 7 to 10)
 * says the reply spans frames: the PID goes without the count digit,
 * which would end the reply after its First Frame */
TEST_F(Elm327SessionTest, ALongServiceOnePidGoesWithoutTheCountDigit)
{
    static ecuio_signal_def_t def;
    static ecuio_signal_set_t set;
    register_run_time("test-elm-idle-time", &def, &set, 7);
    Session s;
    ASSERT_EQ(s.create(uri("?profile=test-elm-idle-time&probe=0&identify=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 7F") >= 2; }, 3000));
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("41 7F", &f));
    EXPECT_EQ(hex_of(f.data.data(), f.data.size()),
              "41 7F 07 00 00 0E 10 00 00 03 84 00 00 00 00");
    EXPECT_EQ(fake_count("017F1"), 0u);
}

/* A def that fits a Single Frame (0x7F's total, bytes 3 to 6) can't say
 * the reply is longer, but J1979's 13 bytes for the PID do: no request
 * carries the digit, and every reply arrives whole, the first too */
TEST_F(Elm327SessionTest, APidJ1979SaysSpansFramesNeverTakesTheDigit)
{
    static ecuio_signal_def_t def;
    static ecuio_signal_set_t set;
    register_run_time("test-elm-run-time", &def, &set, 3);
    Session s;
    ASSERT_EQ(s.create(uri("?profile=test-elm-run-time&probe=0&identify=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 7F") >= 2; }, 3000));
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("41 7F", &f));
    EXPECT_EQ(f.data.size(), 15u);
    const std::vector<uint32_t> counts =
        on_fake<std::vector<uint32_t>>([](fake_elm327::adapter& a) {
            return std::vector<uint32_t>{a.count("017F1"), a.count("017F")};
        });
    EXPECT_EQ(counts[0], 0u) << "no digit, the first request too";
    EXPECT_GE(counts[1], 2u);
}

/* An ECU answering a PID longer than J1979 gives it (0x0C, 2 bytes, here
 * 13): the first request goes with the digit, the adapter stops after
 * the First Frame, and from then on the PID goes without it and its
 * replies arrive whole */
TEST_F(Elm327SessionTest, AReplyLongerThanJ1979SaysTakesThePidOffTheCountDigit)
{
    set_knobs([](fake_elm327::knobs& k) { k.longer_pid = 0x0C; });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C&identify=0")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 2; }, 3000));
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("41 0C", &f));
    EXPECT_EQ(f.data.size(), 15u);
    const std::vector<uint32_t> counts =
        on_fake<std::vector<uint32_t>>([](fake_elm327::adapter& a) {
            return std::vector<uint32_t>{a.count("010C1"), a.count("010C")};
        });
    EXPECT_EQ(counts[0], 1u) << "the digit once, on the first request";
    EXPECT_GE(counts[1] - counts[0], 2u) << "then none";
}

/* An adapter that prints an 11-bit ID as two bytes (07E8) reads the
 * same */
TEST_F(Elm327SessionTest, FourDigitElevenBitIdsReadTheSame)
{
    set_knobs([](fake_elm327::knobs& k) { k.four_digit_ids = true; });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.cap.count("41 0C 1F 40") >= 2; }, 3000));
}

/* A pending reply (7F 01 78) and its answer in the same read: the
 * engine waits on, the answer completes the request, and the rotation
 * keeps its pace */
TEST_F(Elm327SessionTest, APendingReplyIsFollowedByItsAnswer)
{
    set_knobs([](fake_elm327::knobs& k) { k.pending = true; });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 5; }, 2000))
        << "each answer completes its request, no 5 s pending wait";
    EXPECT_GE(s.cap.count("7F 01 78"), 5u);
    EXPECT_EQ(s.state(), ECUIO_OK);
}

/* The waits as shipped; the cases below shorten them */
TEST_F(Elm327SessionTest, TheWorkersWaitsAreFiveSeconds)
{
    uint32_t prompt = 0;
    uint32_t check = 0;
    ecuio_elm327_get_timeouts_ms(&prompt, &check);
    EXPECT_EQ(prompt, 5000u);
    EXPECT_EQ(check, 5000u);
    {
        ShortTimeouts timeouts(2000, 1500);
        ecuio_elm327_get_timeouts_ms(&prompt, &check);
        EXPECT_EQ(prompt, 2000u);
        EXPECT_EQ(check, 1500u);
    }
    ecuio_elm327_get_timeouts_ms(&prompt, &check);
    EXPECT_EQ(prompt, 5000u);
    EXPECT_EQ(check, 5000u);
}

/* An ECU that keeps answering 7F 01 78 past the worker's limit: the adapter
 * listens on, and so does the worker, since its limit counts from the last
 * line, not the request; the answer past it completes the request, with no
 * failure on the way */
TEST_F(Elm327SessionTest, RepeatedPendingRepliesHoldTheReadPastThePromptLimit)
{
    ShortTimeouts timeouts(2000, 0);
    set_knobs([](fake_elm327::knobs& k) {
        k.pending = true;
        k.pending_count = 2;
        k.pending_gap_ms = 1200;
        k.reply_delay_ms = 2200;
    });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    bool failed = false;
    ASSERT_TRUE(wait_for(
        [&] {
            failed = failed || s.state() != ECUIO_OK;
            return s.cap.count("41 0C") >= 1;
        },
        9000));
    EXPECT_FALSE(failed);
    uint32_t episodes = 99;
    EXPECT_EQ(s.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u);
    EXPECT_GE(s.cap.count("7F 01 78"), 2u)
        << "both pending replies, and maybe the next request's first";
}

/* Read-only (ADR-0026 section 5): nothing written to the adapter's memory, no
 * serial number asked, no monitor, no OBDX binary mode, across a session
 * that probes, identifies and polls */
TEST_F(Elm327SessionTest, NothingIsWrittenOrAskedThatTheAdrRulesOut)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK);
    s.start();
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.s, 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        8000));
    for (const std::string& c : fake_commands()) {
        for (const char* banned :
             {"ATPP", "STWBR", "STRSTNVM", "STSATI", "STSDI", "STS@1", "STBTDN",
              "STSN", "DX", "ATMA", "STMA", "ATBRD", "STSBR"}) {
            if (c.rfind(banned, 0) == 0 && c != "ATPPS") {
                ADD_FAILURE() << "sent " << c;
            }
        }
        if (c.rfind("AT", 0) != 0 && c.rfind("ST", 0) != 0) {
            EXPECT_TRUE(c.rfind("01", 0) == 0 || c.rfind("09", 0) == 0 ||
                        c.rfind("22", 0) == 0)
                << "request " << c << ": only Services 01, 09 and 22";
        }
    }
}
