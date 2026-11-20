// File/layer: tests/test_elm327_session.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)

#include "test_elm327_support.h"

namespace {

class LogRootEnvironment : public ::testing::Environment
{
public:
    void SetUp() override
    {
        g_log_root =
            fs::temp_directory_path() /
            ("havne-elm327-logs-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(g_log_root);
        ecuio_elm327_set_log_dir(g_log_root.u8string().c_str());
    }
    void TearDown() override
    {
        ecuio_elm327_set_log_dir(nullptr);
        std::error_code ec;
        fs::remove_all(g_log_root, ec);
    }
};

[[maybe_unused]] ::testing::Environment* const kLogRoot =
    ::testing::AddGlobalTestEnvironment(new LogRootEnvironment);

} // namespace

/* The setup's contract commands in order, then the readings, then the
 * requests: the obd2 profile's probe first, each Service 01 request to
 * the engine ECU with the count digit (ADR-0026 section 3) */
TEST_F(Elm327SessionTest, SetupRunsTheContractThenTheReadings)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<std::string> setup = {
        "ATZ",      "ATE0",   "ATL0",  "ATS0",  "ATH1",   "ATD0",
        "ATCAF1",   "ATCFC1", "ATSP6", "ATAT1", "ATST19", "ATSH7E0",
        "ATCRA7E8", "ATI",    "STI",   "STVR",  "ATPPS"};
    EXPECT_EQ(fake_commands(), setup);

    s.start();
    ASSERT_TRUE(wait_for([&] { return fake_count("01") >= 3; }, 10000));
    const std::vector<std::string> sent = fake_commands();
    ASSERT_GT(sent.size(), setup.size());
    EXPECT_EQ(sent[setup.size()], "01001") << "the probe's first bitmap";
    EXPECT_EQ(
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.bare_crs; }),
        0u)
        << "a bare CR repeats the last command";
}

/* What the session runs on (ADR-0026 section 7): the backend, ISO 15765 at the
 * CAN rate, the channel, an STN's two version strings and STVR's
 * voltage */
TEST_F(Elm327SessionTest, TheSessionInfoSaysWhatItRunsOn)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_ELM327);
    EXPECT_EQ(info.dll_arch, ECUIO_DLL_ARCH_UNKNOWN);
    EXPECT_EQ(info.protocol_id, 6u) << "ISO 15765";
    EXPECT_EQ(info.baud, 500000u);
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    EXPECT_STREQ(info.firmware_version, "ELM327 v1.4b, STN2255 v9.9.9");
    EXPECT_STREQ(info.dll_version, "");
    EXPECT_STREQ(info.api_version, "");
    EXPECT_EQ(info.vbatt_mv, 13800u);
    EXPECT_EQ(info.flags, ECUIO_SESSION_INFO_VBATT |
                              ECUIO_SESSION_INFO_VERSIONS |
                              ECUIO_SESSION_INFO_REQUESTS);
    EXPECT_STREQ(info.profile, "obd2") << "elm327://'s default";
}

/* A plain ELM327 refuses the ST commands: not an STN, which is no
 * failure. Its voltage comes from AT RV, and AT PPS isn't asked */
TEST_F(Elm327SessionTest, APlainElm327ConnectsWithoutTheStnReadings)
{
    set_knobs([](fake_elm327::knobs& k) { k.plain = true; });
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_STREQ(info.firmware_version, "ELM327 v1.4b");
    EXPECT_EQ(info.vbatt_mv, 13800u) << "AT RV's 13.8V";
    EXPECT_EQ(fake_count("STVR"), 0u);
    EXPECT_EQ(fake_count("ATRV"), 1u);
    EXPECT_EQ(fake_count("ATPPS"), 0u);
}

/* An adapter whose reset answers OK alone (the OBDX Pro VX, ADR-0026's
 * appendix of 2026-10-27) names its version to AT I, asked straight
 * after; the reset isn't sent again. A dropped link's setup does the
 * same */
TEST_F(Elm327SessionTest, AResetAnsweredOkTakesTheVersionFromAtI)
{
    set_knobs([](fake_elm327::knobs& k) {
        k.reset_ok = true;
        k.plain = true;
        k.version = "ELM327 V2.1";
    });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK)
        << ecuio_last_error_detail();
    const std::vector<std::string> sent = fake_commands();
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(sent[0], "ATZ");
    EXPECT_EQ(sent[1], "ATI");
    EXPECT_EQ(sent[2], "ATE0");
    EXPECT_EQ(fake_count("ATZ"), 1u);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_STREQ(info.firmware_version, "ELM327 V2.1");

    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 10000));
    set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 10000));
    const size_t before = s.cap.count("41 0C");
    set_knobs([](fake_elm327::knobs& k) { k.dropped = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    EXPECT_TRUE(wait_for([&] { return s.cap.count("41 0C") > before; }, 10000));
    EXPECT_EQ(fake_count("ATZ"), 2u) << "the setup ran again";
}

/* A link that opens with a reset's banner already on it (the MX+ over a
 * new Bluetooth link, docs/bench-sessions/2026-10-29-android.md): AT Z
 * takes that banner for its answer, its own answer comes late, for
 * AT E0, and AT E0 is sent once more; the setup goes through */
TEST_F(Elm327SessionTest, ASetupCommandAnsweredByALateResetIsSentAgain)
{
    set_knobs([](fake_elm327::knobs& k) { k.stale_banner = true; });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(fake_count("ATZ"), 1u);
    EXPECT_EQ(fake_count("ATE0"), 2u) << "AT E0 again after the late reset";
    s.start();
    EXPECT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 10000));
}

/* Parameter 2A on with bit 2 clear: the adapter won't wait past a
 * 7F xx 78, and the session info says so. Off, or on with the bit set,
 * it does */
TEST_F(Elm327SessionTest, AnStnSetNotToWaitOnPendingRepliesIsReported)
{
    struct Case {
        uint8_t value;
        bool on;
        bool reported;
    };
    for (const Case c : {Case{0x00, true, true}, Case{0x00, false, false},
                         Case{0x04, true, false}, Case{0xFB, true, true}}) {
        SCOPED_TRACE(testing::Message()
                     << "2A " << int{c.value} << " " << (c.on ? 'N' : 'F'));
        set_knobs([&](fake_elm327::knobs& k) {
            k.pp2a = c.value;
            k.pp2a_on = c.on;
        });
        Session s;
        ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
        EXPECT_EQ((info.flags & ECUIO_SESSION_INFO_PENDING_OFF) != 0,
                  c.reported);
    }
}

/* The obd2 profile's values arrive from the engine ECU as the J2534
 * fake's do: the same ECU answers both (tests/fake_ecu.h), and the
 * session stays OK */
TEST_F(Elm327SessionTest, Obd2ValuesArriveFromTheEngineEcu)
{
    Session s;
    /* One PID per request, so each engine speed is a reply of its own */
    ASSERT_EQ(s.create(uri("?pack=0")), ECUIO_OK) << ecuio_last_error_detail();
    s.start();
    /* The second engine speed comes after the probe, identification,
     * diagnostics and a lap of the whole obd2 set: seconds on a slow
     * runner */
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 2; }, 10000));
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("41 0C", &f));
    EXPECT_EQ(f.id, 0x7E8u);
    EXPECT_EQ(f.flags, uint32_t{ECUIO_FRAME_FLAG_REASSEMBLED})
        << "a message's payload, not a frame as it crossed the bus";
    EXPECT_EQ(hex_of(f.data.data(), f.data.size()), "41 0C 1F 40")
        << "the PCI byte and the padding are gone";
    EXPECT_GE(s.cap.count("41 05"), 1u);
    uint32_t episodes = 99;
    EXPECT_EQ(s.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u);
}

namespace {

/* Channel 0's support flags once done says so, or 0 after budget_ms */
uint32_t support_flags_when(ecuio_session_t* s, uint32_t want, int budget_ms)
{
    ecuio_channel_support_t sup{};
    (void)wait_for(
        [&] {
            return ecuio_session_get_channel_support(s, 0, &sup) == ECUIO_OK &&
                   (sup.flags & want) != 0;
        },
        budget_ms);
    return sup.flags;
}

/* The fakes' ECU answering a pack for its first PID alone, for a scope */
struct FirstPidOnly {
    FirstPidOnly() { fake_ecu::pack_first_only() = true; }
    ~FirstPidOnly() { fake_ecu::pack_first_only() = false; }
};

} // namespace

/* ADR-0039 Appendix A over ELM327: once its PIDs have answered alone,
 * a request carries several, with the count digit its reply's frames
 * take (a reply past one Single Frame among them), and the report says
 * packs were answered; ?pack=0 asks one PID per request */
TEST_F(Elm327SessionTest, APackedRequestCarriesItsRepliesFrames)
{
    {
        Session s;
        ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
        s.start();
        const uint32_t flags =
            support_flags_when(s.s, ECUIO_SUPPORT_PACKED, 10000);
        EXPECT_TRUE(flags & ECUIO_SUPPORT_PACKED);
        EXPECT_FALSE(flags & ECUIO_SUPPORT_UNPACKED);
    }
    size_t packs = 0;
    bool past_one_frame = false;
    for (const std::string& cmd : fake_commands()) {
        if (cmd.size() < 7 || cmd.size() % 2 == 0 || cmd.compare(0, 2, "01") != 0) {
            continue; /* a pack is 01, two or more PIDs and its digit */
        }
        ++packs;
        unsigned char req[8] = {};
        const size_t len = (cmd.size() - 1) / 2;
        for (size_t i = 0; i < len; ++i) {
            req[i] = static_cast<unsigned char>(
                std::stoul(cmd.substr(2 * i, 2), nullptr, 16));
        }
        unsigned char reply[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        ASSERT_TRUE(fake_ecu::answer(req, len, -1, reply, &n)) << cmd;
        const uint8_t digit =
            static_cast<uint8_t>(std::stoul(cmd.substr(cmd.size() - 1), nullptr, 16));
        EXPECT_EQ(digit, elm327_pack_count(static_cast<uint32_t>(n), 0x7E0)) << cmd;
        past_one_frame = past_one_frame || n > 7;
    }
    EXPECT_GT(packs, 0u);
    EXPECT_TRUE(past_one_frame) << "a reply of a First Frame and more";

    Session off;
    ASSERT_EQ(off.create(uri("?identify=0&diagnose=0&pack=0")), ECUIO_OK);
    off.start();
    const auto requests = [] {
        return on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; });
    };
    const uint32_t before = requests();
    ASSERT_TRUE(wait_for([&] { return requests() >= before + 150; }, 10000));
    const uint32_t flags = support_flags_when(off.s, ECUIO_SUPPORT_PACKED, 0);
    EXPECT_FALSE(flags & (ECUIO_SUPPORT_PACKED | ECUIO_SUPPORT_UNPACKED));
}

/* An adapter's ECU that answers a pack for its first PID alone, three
 * packs in a row, is asked one PID per request from then on */
TEST_F(Elm327SessionTest, AnEcuAnsweringOnePidEndsPacking)
{
    FirstPidOnly first;
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
    s.start();
    const uint32_t flags = support_flags_when(s.s, ECUIO_SUPPORT_UNPACKED, 10000);
    EXPECT_TRUE(flags & ECUIO_SUPPORT_UNPACKED);
    EXPECT_FALSE(flags & ECUIO_SUPPORT_PACKED);
    EXPECT_EQ(s.state(nullptr), ECUIO_OK) << "a fallback, not a fault";
}

/* An ECU that goes quiet while packing (the ignition off: NO DATA to
 * every request) unpacks the channel; once it answers again the link
 * recovers and packs go out again */
TEST_F(Elm327SessionTest, PackingResumesAfterTheEcuWentQuiet)
{
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
    s.start();
    ASSERT_TRUE(support_flags_when(s.s, ECUIO_SUPPORT_PACKED, 10000) &
                ECUIO_SUPPORT_PACKED);
    set_knobs([](fake_elm327::knobs& k) { k.no_data = true; });
    ASSERT_TRUE(support_flags_when(s.s, ECUIO_SUPPORT_UNPACKED, 10000) &
                ECUIO_SUPPORT_UNPACKED);
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 5000));
    set_knobs([](fake_elm327::knobs& k) { k.no_data = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    const auto packs = [] {
        return on_fake<uint32_t>(
            [](fake_elm327::adapter& a) { return a.packs; });
    };
    const uint32_t before = packs();
    EXPECT_TRUE(wait_for([&] { return packs() >= before + 5; }, 10000))
        << "packs go out again";
    ecuio_channel_support_t sup{};
    ASSERT_EQ(ecuio_session_get_channel_support(s.s, 0, &sup), ECUIO_OK);
    EXPECT_FALSE(sup.flags & ECUIO_SUPPORT_UNPACKED);
}

/* The probe walks the supported-PID chain (01 00, 20, 40), and the
 * support report says what it learned */
TEST_F(Elm327SessionTest, TheProbeWalksTheChainAndReportsIt)
{
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0")), ECUIO_OK);
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000));
    EXPECT_EQ(sup.rx_id, 0x7E8u);
    EXPECT_EQ(sup.ranges, 0x3Fu) << "the bitmaps at 0x00 to 0xA0";
    EXPECT_EQ(sup.dropped, 0u) << "the fake supports every obd2 PID";
    EXPECT_EQ(fake_count("01001"), 1u);
    EXPECT_EQ(fake_count("01201"), 1u);
    EXPECT_EQ(fake_count("01401"), 1u);
    EXPECT_EQ(fake_count("01601"), 1u);
    EXPECT_EQ(fake_count("01801"), 1u);
    EXPECT_EQ(fake_count("01A01"), 1u);
    EXPECT_EQ(fake_count("01C01"), 0u) << "past A0's last bit";
}

/* ADR-0039 section 6 end to end: the probe reads bitmaps that leave out
 * the PIDs the ECU withholds, and core lists exactly their signals
 * unsupported, in the set's order */
TEST_F(Elm327SessionTest, TheSignalsOfAWithheldPidAreUnsupported)
{
    Withheld w({0x0D, 0x46});
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000));
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> expected;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const unsigned pid = d.mux_key & 0xFFu;
        if (d.mux_length == 2 && (d.mux_key >> 8) == 0x41 &&
            (pid == 0x0D || pid == 0x46)) {
            expected.push_back(d.id);
        }
    }
    ASSERT_EQ(expected.size(), 2u) << "a def each for 0D and 46";
    std::vector<uint16_t> ids(set->count);
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_unsupported_signals(
                  s.s, ids.data(), static_cast<uint32_t>(ids.size()), &count),
              ECUIO_OK);
    ids.resize(count);
    EXPECT_EQ(ids, expected);
}

/* ADR-0056 section 2: a focus set before start has the adapter asked for
 * the picked PIDs alone from its first request, and a count of 0 has it
 * ask the rest again */
TEST_F(Elm327SessionTest, AFocusAsksOnlyThePickedPids)
{
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0&probe=0")), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            (d.mux_key == 0x410Cu || d.mux_key == 0x410Du)) {
            ids.push_back(d.id);
        }
    }
    ASSERT_EQ(ids.size(), 2u);
    ASSERT_EQ(ecuio_session_set_focus(s.s, ids.data(), 2), ECUIO_OK);
    s.start();
    /* The Service 01 requests sent, each as its first four characters */
    const auto requests = [] {
        std::vector<std::string> out;
        for (const std::string& cmd : fake_commands()) {
            if (cmd.size() >= 4 && cmd.compare(0, 2, "01") == 0) {
                out.push_back(cmd.substr(0, 4));
            }
        }
        return out;
    };
    ASSERT_TRUE(wait_for([&] { return requests().size() >= 8; }, 5000));
    const std::vector<std::string> first = requests();
    for (size_t k = 0; k < first.size(); ++k) {
        EXPECT_TRUE(first[k] == "010C" || first[k] == "010D") << first[k];
    }
    ASSERT_EQ(ecuio_session_set_focus(s.s, nullptr, 0), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            const std::vector<std::string> sent = requests();
            for (size_t k = first.size(); k < sent.size(); ++k) {
                if (sent[k] != "010C" && sent[k] != "010D") {
                    return true;
                }
            }
            return false;
        },
        5000))
        << "the whole set is asked again";
}

/* ADR-0056 section 4: two picks with a minimum time of 200 ms, judged by
 * the fake's own stamps on its requests: from one sweep's first request
 * to the next's, the median is at least the minimum (a stall only adds)
 * and each sweep's second request follows at the engine's floor, not
 * spread over the gap */
TEST_F(Elm327SessionTest, AMinimumTimeSpacesTheFocusSweeps)
{
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0&probe=0")), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            (d.mux_key == 0x410Cu || d.mux_key == 0x410Du)) {
            ids.push_back(d.id);
        }
    }
    ASSERT_EQ(ids.size(), 2u);
    ASSERT_EQ(ecuio_session_set_focus_with(s.s, ids.data(), 2, 200, 0),
              ECUIO_OK);
    s.start();
    static constexpr uint32_t want = 12; /* no capture: MSVC and Clang agree */
    ASSERT_TRUE(wait_for(
        [&] {
            return on_fake<uint32_t>([](fake_elm327::adapter& a) {
                       return a.requests;
                   }) >= want;
        },
        5000));
    const std::vector<int64_t> stamps =
        on_fake<std::vector<int64_t>>([](fake_elm327::adapter& a) {
            return std::vector<int64_t>(a.request_us, a.request_us + want);
        });
    std::vector<int64_t> sweeps;
    std::vector<int64_t> within;
    for (size_t i = 0; i + 2 < stamps.size(); i += 2) {
        sweeps.push_back(stamps[i + 2] - stamps[i]);
        within.push_back(stamps[i + 1] - stamps[i]);
    }
    std::sort(sweeps.begin(), sweeps.end());
    std::sort(within.begin(), within.end());
    const int64_t sweep = sweeps[sweeps.size() / 2];
    const int64_t inner = within[within.size() / 2];
    RecordProperty("sweep_median_us", static_cast<int>(sweep));
    EXPECT_GE(sweep, 190000) << "sweeps came faster than the minimum";
    EXPECT_LT(inner, 100000) << "a sweep's requests were spread over the gap";
}

/* A session that asks says so in its info, and a recording of it names
 * the focus's minimum time, after the focus line and before the signals */
TEST_F(Elm327SessionTest, ARecordingNamesTheMinimumTimeOfASessionThatAsks)
{
    const fs::path root =
        fs::temp_directory_path() / temp_name("ecuio-elm327-floor");
    std::error_code ec;
    fs::remove_all(root, ec);
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0&probe=0")), ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    uint16_t rpm = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 && d.mux_key == 0x410Cu) {
            rpm = d.id;
        }
    }
    ASSERT_EQ(ecuio_session_set_focus_with(s.s, &rpm, 1, 250, 0), ECUIO_OK);
    const std::string root8 = root.generic_u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len), ECUIO_OK);
    const fs::path folder = fs::u8path(buf);
    ASSERT_EQ(ecuio_session_record(s.s, rec), ECUIO_OK);
    ecuio_session_destroy(s.s);
    s.s = nullptr;
    EXPECT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);
    std::ifstream f(folder / "events.log", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string events;
    for (char c : ss.str()) {
        if (c != '\r') {
            events += c;
        }
    }
    const size_t focus = events.find("\nfocus  ");
    const size_t floor = events.find("\nmin sweep  250 ms\n");
    const size_t signal = events.find("\nsignal  ");
    ASSERT_NE(focus, std::string::npos) << events;
    ASSERT_NE(floor, std::string::npos) << events;
    EXPECT_TRUE(focus < floor && floor < signal) << events;
    fs::remove_all(root, ec);
}

/* On a session that asks, a focus line carries its minimum time between
 * sweeps, and a new minimum on the same picks is a line of its own; the
 * same focus again writes none */
TEST_F(Elm327SessionTest, AFocusLineCarriesItsMinimumAndComesOnlyOnAChange)
{
    const fs::path root =
        fs::temp_directory_path() / temp_name("ecuio-elm327-floor-line");
    std::error_code ec;
    fs::remove_all(root, ec);
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0&probe=0")), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    uint16_t rpm = 0;
    std::string key;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 && d.mux_key == 0x410Cu) {
            rpm = d.id;
            ecuio_signal_key_t k{};
            ASSERT_EQ(ecuio_signal_address_key(&d, &k), ECUIO_OK);
            key = k.text;
        }
    }
    ASSERT_EQ(key, "7E8:410C:2.0:16");
    ASSERT_EQ(ecuio_session_set_focus_with(s.s, &rpm, 1, 250, 0), ECUIO_OK);
    const std::string root8 = root.generic_u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len), ECUIO_OK);
    const fs::path folder = fs::u8path(buf);
    ASSERT_EQ(ecuio_session_record(s.s, rec), ECUIO_OK);
    ASSERT_EQ(ecuio_session_set_focus_with(s.s, &rpm, 1, 250, 0), ECUIO_OK);
    ASSERT_EQ(ecuio_session_set_focus_with(s.s, &rpm, 1, 500, 0), ECUIO_OK);
    ASSERT_EQ(ecuio_session_set_focus_with(s.s, &rpm, 1, 0, 0), ECUIO_OK);
    ASSERT_EQ(ecuio_session_set_focus(s.s, &rpm, 1), ECUIO_OK);
    ecuio_session_destroy(s.s);
    s.s = nullptr;
    EXPECT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);
    std::ifstream f(folder / "events.log", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t at = line.find("  focus  ");
        if (at != std::string::npos && at > 0) {
            lines.push_back(line.substr(at + 9));
        }
    }
    EXPECT_EQ(lines, (std::vector<std::string>{
                         "1 signal, at least 500 ms between sweeps: " + key,
                         "1 signal: " + key}))
        << ss.str();
    fs::remove_all(root, ec);
}

/* A focus is judged by what the session can still ask: once the walk
 * has dropped a PID the ECU withholds, a focus on it alone is refused
 * with the reason in the detail, and one that adds an askable pick is
 * taken, the session then asking that pick alone */
TEST_F(Elm327SessionTest, AFocusOnADroppedPidAloneIsRefused)
{
    Withheld w({0x0D});
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    uint16_t rpm = 0;
    uint16_t speed = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 && d.mux_key == 0x410Cu) {
            rpm = d.id;
        }
        if (d.mux_offset == 0 && d.mux_length == 2 && d.mux_key == 0x410Du) {
            speed = d.id;
        }
    }
    ASSERT_NE(rpm, speed);
    s.start();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000));
    EXPECT_EQ(ecuio_session_set_focus(s.s, &speed, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the focus leaves nothing to request: none of the picked "
                 "signals is asked on this session");
    const uint16_t both[2] = {rpm, speed};
    ASSERT_EQ(ecuio_session_set_focus(s.s, both, 2), ECUIO_OK);
    const auto requests = [] {
        std::vector<std::string> out;
        for (const std::string& cmd : fake_commands()) {
            if (cmd.size() >= 4 && cmd.compare(0, 2, "01") == 0) {
                out.push_back(cmd);
            }
        }
        return out;
    };
    /* One request may be out when the call lands; the rest are the pick */
    const size_t from = requests().size() + 1;
    ASSERT_TRUE(wait_for([&] { return requests().size() >= from + 6; }, 5000));
    const std::vector<std::string> sent = requests();
    for (size_t k = from; k < sent.size(); ++k) {
        EXPECT_EQ(sent[k].substr(0, 4), "010C") << sent[k];
    }
}

/* Reply timing (ADR-0042) through an ELM327: a row per request in the
 * rotation, each answered one at least the fake's reply delay after its
 * request, the intervals on the host's clock (an ELM327's lines are
 * stamped as they're read), ISO-TP's counts from core's reassembly; the
 * rows published while the session runs and kept after close */
TEST_F(Elm327SessionTest, TimingReportsEachRequestOnTheHostsClock)
{
    set_knobs([](fake_elm327::knobs& k) { k.reply_delay_ms = 10; });
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
    s.start();
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    const auto answered = [&] {
        uint32_t n = 0;
        for (uint32_t i = 0; i < count && i < rows.size(); ++i) {
            n += rows[i].answered;
        }
        return n;
    };
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_timing(s.s, &t, rows.data(),
                                            static_cast<uint32_t>(rows.size()),
                                            &count) == ECUIO_OK &&
                   answered() >= 40;
        },
        10000))
        << answered() << " answers published";
    EXPECT_EQ(t.interval_clock, ECUIO_TIMING_CLOCK_HOST);
    EXPECT_NE(t.flags & ECUIO_TIMING_ISOTP, 0u);
    EXPECT_GT(t.isotp_completed, 0u);
    EXPECT_GT(t.snapshot_us, 0u);
    EXPECT_GT(count, 10u);
    EXPECT_EQ(t.rows, count);
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_request_timing_t& r = rows[i];
        EXPECT_EQ(r.channel, 0u);
        EXPECT_EQ(r.length, 2u);
        EXPECT_EQ(r.request[0], 0x01);
        if (r.answered > 0) {
            SCOPED_TRACE("PID " + std::to_string(r.request[1]));
            EXPECT_GE(r.latency_p50_us, 10000u) << "the fake's delay";
            EXPECT_GE(r.latency_max_us, r.latency_p50_us);
        }
    }

    /* Right after a publish, half a second, then close: only the final
     * publish, as the worker stops, can carry that half second's answers */
    const uint64_t seen = t.snapshot_us;
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_timing(s.s, &t, rows.data(),
                                            static_cast<uint32_t>(rows.size()),
                                            &count) == ECUIO_OK &&
                   t.snapshot_us != seen;
        },
        10000));
    const uint32_t before = answered();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ecuio_session_close(s.s);
    ecuio_session_timing_t after{};
    uint32_t after_count = 0;
    ASSERT_EQ(ecuio_session_get_timing(s.s, &after, rows.data(),
                                       static_cast<uint32_t>(rows.size()),
                                       &after_count),
              ECUIO_OK);
    EXPECT_EQ(after_count, count) << "readable after close";
    EXPECT_GT(after.snapshot_us, t.snapshot_us) << "the final rows";
    EXPECT_GT(answered(), before) << "the last half second's answers";
}

/* A recorded ELM327 session (ADR-0049 section 1): the worker's link
 * transitions are events.log's link lines, and its final reply timing,
 * its last record, is latency.csv's rows under the channel's ECU */
TEST_F(Elm327SessionTest, ARecordingHoldsTheLinksTransitionsAndReplyTiming)
{
    const fs::path root =
        fs::temp_directory_path() / temp_name("ecuio-elm327-recording");
    std::error_code ec;
    fs::remove_all(root, ec);
    set_knobs([](fake_elm327::knobs& k) { k.no_data = true; });
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C")), ECUIO_OK);
    const std::string root8 = root.generic_u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len), ECUIO_OK);
    const fs::path folder = fs::u8path(buf);
    ASSERT_EQ(ecuio_session_record(s.s, rec), ECUIO_OK);
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 5000));
    set_knobs([](fake_elm327::knobs& k) { k.no_data = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 5; }, 10000));
    ecuio_session_destroy(s.s);
    s.s = nullptr;
    EXPECT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);

    const auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string out;
        for (char c : ss.str()) {
            if (c != '\r')
                out += c;
        }
        return out;
    };
    const std::string events = read(folder / "events.log");
    const size_t quiet =
        events.find("  link  vehicle not responding (state 8, 0 episodes)\n");
    ASSERT_NE(quiet, std::string::npos) << events;
    EXPECT_NE(events.find("  link  recovered (state 0, 0 episodes)\n", quiet),
              std::string::npos)
        << events;
    const std::string latency = read(folder / "latency.csv");
    EXPECT_EQ(latency.rfind(ECUIO_TIMING_COLUMNS "\n", 0), 0u) << latency;
    EXPECT_NE(latency.find(",0x7E8,01 0C,"), std::string::npos) << latency;
    EXPECT_NE(latency.find(",host\n"), std::string::npos) << latency;
    fs::remove_all(root, ec);
}

/* ADR-0065: a recording's reply timing names the module a row is for,
 * one past the four the session info lists too */
TEST_F(Elm327SessionTest, ARecordingsReplyTimingNamesAModulePastTheFourth)
{
    SixModules modules;
    const fs::path root =
        fs::temp_directory_path() / temp_name("ecuio-elm327-six-modules");
    std::error_code ec;
    fs::remove_all(root, ec);
    Session s;
    ASSERT_EQ(s.create(uri(kSixModulesQuery)), ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string root8 = root.generic_u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len), ECUIO_OK);
    const fs::path folder = fs::u8path(buf);
    ASSERT_EQ(ecuio_session_record(s.s, rec), ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int sixth = 0;
            for (const Capture::Frame& f : s.cap.frames) {
                sixth += f.id == 0x7EDu;
            }
            return sixth >= 3;
        },
        10000))
        << "three replies from the sixth module";
    ecuio_session_destroy(s.s);
    s.s = nullptr;
    EXPECT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);

    std::ifstream f(folder / "latency.csv", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string latency = ss.str();
    EXPECT_NE(latency.find(",0x7E8,22 1E 1C,"), std::string::npos) << latency;
    EXPECT_NE(latency.find(",0x7EC,22 D8 04,"), std::string::npos) << latency;
    EXPECT_NE(latency.find(",0x7ED,22 D8 05,"), std::string::npos) << latency;
    fs::remove_all(root, ec);
}

/* Identification's replies span frames (the VIN's 20 bytes: a First
 * Frame and two Consecutive Frames), are reassembled and reach the
 * identity report; Service 09 carries no count digit, and a 0x22 item's,
 * cut after the First Frame, is asked again without it */
TEST_F(Elm327SessionTest, IdentificationReassemblesItsLongReplies)
{
    Session s;
    ASSERT_EQ(s.create(uri("?probe=0")), ECUIO_OK);
    s.start();
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.s, 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        8000));
    ASSERT_GT(id.items, 0u);
    EXPECT_EQ(id.list[0].request[0], 0x09);
    EXPECT_EQ(id.list[0].request[1], 0x02);
    EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(id.list[0].text, "FAKEJ2534VIN00001");
    EXPECT_EQ(fake_count("0902"), 1u);
    EXPECT_EQ(fake_count("09021"), 0u);
    EXPECT_EQ(fake_count("22F1901"), 1u);
    EXPECT_EQ(fake_count("22F190"), 2u) << "22F1901, then 22F190";
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("49 02", &f));
    EXPECT_EQ(f.data.size(), 20u);
}
