// File/layer: tests/test_elm327_session_scan.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)

#include "test_elm327_support.h"

/* NO DATA to every request: the engine's waits run out, ten in a row go
 * quiet, with no episode; the answers' return is OK again */
TEST_F(Elm327SessionTest, NoDataGoesQuietWithoutAnEpisode)
{
    set_knobs([](fake_elm327::knobs& k) { k.no_data = true; });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 5000));
    uint32_t episodes = 99;
    (void)s.state(&episodes);
    EXPECT_EQ(episodes, 0u);
    set_knobs([](fake_elm327::knobs& k) { k.no_data = false; });
    EXPECT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 3000));
}

/* A scanned DID goes with the count digit, so a refusal (7F 22 31, one
 * line) ends the exchange at once; a long DID's reply, cut after its
 * First Frame, is asked again without it, found and never lost, and
 * the rotation it joins asks it without the digit too */
TEST_F(Elm327SessionTest, AScannedDidTakesTheCountDigitUntilItsReplySpansFrames)
{
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=F19E-F1A0&identify=0"
                           "&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
        },
        3000));
    EXPECT_EQ(sup.scan_found, 1u);
    EXPECT_EQ(sup.scan_refused, 2u);
    EXPECT_EQ(sup.scan_done, 3u) << "nothing unanswered";
    ASSERT_TRUE(wait_for([&] { return s.cap.count("62 F1 A0") >= 2; }, 3000))
        << "found, then polled";
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("62 F1 A0", &f));
    EXPECT_EQ(f.data.size(), 3u + fake_ecu::kLongDidLen);
    EXPECT_EQ(fake_count("22F19E1"), 1u);
    EXPECT_EQ(fake_count("22F19F1"), 1u);
    EXPECT_EQ(fake_count("22F1A01"), 1u) << "the digit once";
    EXPECT_GE(fake_count("22F1A0"), 3u) << "then none: the retry and the poll";
}

/* An adapter whose response count counts the ECU's 7F 22 78: the digit
 * ends the exchange before the answer, so the request is asked again
 * without it, and the DID is found, not left unanswered */
TEST_F(Elm327SessionTest, AScannedDidHeldPendingIsAskedAgainWithoutTheDigit)
{
    set_knobs([](fake_elm327::knobs& k) {
        k.pending = true;
        k.pending_counted = true;
    });
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=1E1C-1E1D&identify=0"
                           "&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
        },
        3000));
    EXPECT_EQ(sup.scan_found, 1u);
    EXPECT_EQ(sup.scan_refused, 1u);
    EXPECT_EQ(sup.scan_done, 2u) << "nothing unanswered";
    EXPECT_EQ(fake_count("221E1C1"), 1u);
    EXPECT_EQ(fake_count("221E1D1"), 1u);
    EXPECT_GE(fake_count("221E1C"), 2u);
    EXPECT_GE(fake_count("221E1D"), 2u);
    EXPECT_EQ(s.state(), ECUIO_OK);
}

/* A reply the digit cut goes on on the bus, and the ECU takes no request
 * until it's sent (the test car's engine ECU, 8 ms a Consecutive Frame):
 * the DID is asked again once the rest has gone, not into NO DATA, so the
 * scan completes with it found */
TEST_F(Elm327SessionTest, ACutDidIsAskedAgainOnceTheEcuHasSentTheRest)
{
    set_knobs([](fake_elm327::knobs& k) { k.cut_gap_ms = 8; });
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=F19E-F1A0&identify=0"
                           "&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
        },
        3000))
        << "scan: " << sup.scan_done << " done of " << sup.scan_total;
    EXPECT_EQ(sup.scan_found, 1u);
    EXPECT_EQ(sup.scan_refused, 2u);
    EXPECT_EQ(sup.scan_done, 3u) << "nothing unanswered";
    EXPECT_EQ(fake_count("22F1A01"), 1u) << "the digit once";
    ASSERT_TRUE(wait_for([&] { return s.cap.count("62 F1 A0") >= 1; }, 3000));
    EXPECT_EQ(s.state(), ECUIO_OK);
}

/* An ECU slower than the hold allows (15 ms a frame): the DID asked again
 * gets NO DATA, and is held again, longer, and found */
TEST_F(Elm327SessionTest, ACutDidAnsweredNoDataIsHeldAgainLonger)
{
    set_knobs([](fake_elm327::knobs& k) { k.cut_gap_ms = 15; });
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=F19F-F1A0&identify=0"
                           "&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
        },
        4000))
        << "scan: " << sup.scan_done << " done of " << sup.scan_total;
    EXPECT_EQ(sup.scan_found, 1u) << "found after the second hold";
    EXPECT_EQ(sup.scan_done, 2u) << "nothing unanswered";
    EXPECT_EQ(fake_count("22F1A01"), 1u) << "the digit once";
    EXPECT_GE(fake_count("22F1A0"), 2u) << "NO DATA once, then the answer";
    EXPECT_EQ(s.state(), ECUIO_OK);
}

/* ?scan=first: every scanned DID is asked before the listed one's
 * second poll, where the turns would alternate them */
TEST_F(Elm327SessionTest, ScanFirstAsksTheRangeBeforeTheRotation)
{
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=1E1C&did=1E00-1E07"
                           "&scan=first&identify=0&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
        },
        3000));
    ASSERT_TRUE(wait_for([&] { return fake_count("221E1C") >= 2; }, 3000));
    const std::vector<std::string> sent = fake_commands();
    size_t last_scan = 0;
    std::vector<size_t> polls;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (sent[i].rfind("221E0", 0) == 0) {
            last_scan = i;
        } else if (sent[i].rfind("221E1C", 0) == 0) {
            polls.push_back(i);
        }
    }
    ASSERT_GE(polls.size(), 2u);
    EXPECT_LT(last_scan, polls[0]) << "the rotation waits for the scan";
    EXPECT_EQ(sup.scan_done, 8u);
}

/* ?scan= takes turns or first, and first only where a scan can run */
TEST_F(Elm327SessionTest, ScanFirstBelongsToUdsGeneric)
{
    Session s;
    EXPECT_EQ(s.create(uri("?scan=first")), ECUIO_ERR_BAD_URI);
    EXPECT_NE(strstr(ecuio_last_error_detail(), "uds-generic"), nullptr)
        << ecuio_last_error_detail();
    EXPECT_EQ(s.create(uri("?profile=uds-generic&did=1E1C&scan=fast")),
              ECUIO_ERR_BAD_URI);
    EXPECT_NE(strstr(ecuio_last_error_detail(), "turns or first"), nullptr)
        << ecuio_last_error_detail();
}

/* A scan whose DIDs are all refused leaves nothing to ask: the worker
 * listens, and ten quiet reads (about a second) go quiet, as J2534's
 * passive reads do */
TEST_F(Elm327SessionTest, NothingLeftToAskGoesQuiet)
{
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=1E00-1E03&identify=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
        },
        3000));
    EXPECT_EQ(sup.scan_refused, 4u);
    const auto from = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 4000));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    EXPECT_GE(took, 700) << "ten idle reads of 100 ms";
    const uint32_t requests =
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; }),
        requests)
        << "nothing more is asked";
}

/* CAN ERROR (no node acknowledges): requests failing, one episode; the
 * bus's return clears it */
TEST_F(Elm327SessionTest, CanErrorIsRequestsFailingUntilTheBusAnswers)
{
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    set_knobs([](fake_elm327::knobs& k) { k.can_error = true; });
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_REQUEST_FAILED; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    uint32_t episodes = 0;
    EXPECT_EQ(s.state(&episodes), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes, 1u) << "one episode, however many requests fail";
    set_knobs([](fake_elm327::knobs& k) { k.can_error = false; });
    EXPECT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 8000));
    (void)s.state(&episodes);
    EXPECT_EQ(episodes, 1u);
}

/* The link drops (a Bluetooth adapter out of range): a hard failure, one
 * episode, the port reopened on the backoff's schedule; its return runs
 * the setup again and the values resume, with no reconnect */
TEST_F(Elm327SessionTest, ADroppedLinkRecoversWithoutAReconnect)
{
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint32_t episodes = 0;
    EXPECT_EQ(s.state(&episodes), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes, 1u);
    const size_t before = s.cap.count("41 0C");
    set_knobs([](fake_elm327::knobs& k) { k.dropped = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    EXPECT_TRUE(wait_for([&] { return s.cap.count("41 0C") > before; }, 3000));
    EXPECT_EQ(fake_count("ATZ"), 2u) << "the setup ran again";
    const uint32_t opens = [] {
        std::lock_guard<std::mutex> g(fake_elm327::state().lock);
        return fake_elm327::state().opens;
    }();
    EXPECT_GE(opens, 2u);
    (void)s.state(&episodes);
    EXPECT_EQ(episodes, 1u);
}

/* The adapter resets on its own (a supply dip) and prints its banner:
 * the setup runs again on the same port, with no failure */
TEST_F(Elm327SessionTest, AnAdapterResetGetsItsSetupAgain)
{
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    fake_elm327::with([](fake_elm327::adapter& a) {
        a.spontaneous_reset(fake_elm327::steady_us());
    });
    ASSERT_TRUE(wait_for([&] { return fake_count("ATZ") >= 2; }, 3000));
    const size_t before = s.cap.count("41 0C");
    EXPECT_TRUE(
        wait_for([&] { return s.cap.count("41 0C") > before + 2; }, 3000));
    uint32_t episodes = 99;
    EXPECT_EQ(s.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u);
    EXPECT_EQ(fake_count("ATE0"), 2u);
}

/* A reset whose setup then fails (the adapter refuses a contract
 * command): a hard failure, one episode, until the setup goes through
 * again */
TEST_F(Elm327SessionTest, AResetWhoseSetupFailsIsAHardFailure)
{
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    fake_elm327::with([](fake_elm327::adapter& a) {
        a.k.refuse = "ATH1";
        a.spontaneous_reset(fake_elm327::steady_us());
    });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 3000));
    set_knobs([](fake_elm327::knobs& k) { k.refuse = nullptr; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    uint32_t episodes = 0;
    (void)s.state(&episodes);
    EXPECT_EQ(episodes, 1u);
}

/* An adapter that stops answering, prompt and all: a hard failure once
 * the worker's limit passes with nothing, and the log names the limit
 * in force */
TEST_F(Elm327SessionTest, NoPromptWithinThePromptLimitIsAHardFailure)
{
    const LogDir logs("silent");
    {
        Session s;
        ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
        s.start();
        ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
        const ShortTimeouts timeouts(500, 0);
        const auto from = std::chrono::steady_clock::now();
        set_knobs([](fake_elm327::knobs& k) { k.silent = true; });
        ASSERT_TRUE(wait_for(
            [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; },
            4000));
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - from)
                              .count();
        EXPECT_GE(took, 400);
    }
    const std::string log = logs.only_log();
    EXPECT_NE(log.find("] no line and no prompt for 500 ms ("),
              std::string::npos)
        << log;
}

/* A session that asks nothing runs the setup, then AT MA and nothing
 * else; the channel's messages arrive whole, multi-frame too, flagged
 * reassembled, and a line the monitor marks or another ID's frame is no
 * frame of ours */
TEST_F(Elm327SessionTest, ListeningDeliversTheChannelsMessages)
{
    Session s;
    listen_on(s);
    on_bus(0x7E8, {0x41, 0x0C, 0x1A, 0xF8});
    on_bus(0x7E9, {0x41, 0x0D, 0x20}); /* the receive filter's to stop */
    on_bus(0x7E8, kVinReply);
    fake_elm327::with([](fake_elm327::adapter& a) {
        const int64_t now = fake_elm327::steady_us();
        a.monitor_line("7E8 FC: 30 00 00", now);
        a.monitor_line("7E8 03 41 0D 20 AA AA AA AA <DATA ERROR", now);
        a.monitor_line("7E9 03 41 0D 20 AA AA AA AA", now);
    });
    on_bus(0x7E8, {0x41, 0x05, 0x7B});
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 05") == 1; }, 3000));
    EXPECT_EQ(s.cap.count("41 0C 1A F8"), 1u);
    Capture::Frame vin{};
    ASSERT_TRUE(s.cap.find("49 02 01", &vin));
    EXPECT_EQ(vin.data, kVinReply);
    EXPECT_EQ(vin.id, 0x7E8u);
    EXPECT_EQ(vin.flags & ECUIO_FRAME_FLAG_OVERFLOW, 0u);
    EXPECT_NE(vin.flags & ECUIO_FRAME_FLAG_REASSEMBLED, 0u)
        << "core put the monitor's lines together as it does a reply's";
    EXPECT_EQ(s.cap.count("41 0D"), 0u);
    std::vector<std::string> commands = fake_commands();
    ASSERT_GE(commands.size(), 2u);
    EXPECT_EQ(commands[commands.size() - 2], "ATPPS");
    EXPECT_EQ(commands.back(), "ATMA");
    EXPECT_EQ(
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; }),
        0u)
        << "nothing is asked";
    EXPECT_EQ(s.state(), ECUIO_OK);
}
