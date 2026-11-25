// File/layer: tests/test_elm327_session_diag.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)

#include "test_elm327_support.h"

/* Diagnostics (ADR-0037) through the adapter: the code lists as
 * one-byte requests with no count digit, the stored list's 8 bytes as a
 * First Frame and a Consecutive Frame, reassembled, and the whole
 * report filled from the fake ECU. Read again asks each item once more
 * and fills it again. */
TEST_F(Elm327SessionTest, DiagnosticsFillTheReportAndReadAgain)
{
    fake_ecu::reset_diagnostics();
    Session s;
    ASSERT_EQ(s.create(uri("?probe=0&identify=0")), ECUIO_OK);
    s.start();
    ecuio_channel_diagnostics_t d{};
    const auto read_to = [&](uint32_t generation) {
        return wait_for(
            [&] {
                return ecuio_session_get_channel_diagnostics(s.s, 0, &d) ==
                           ECUIO_OK &&
                       d.generation == generation &&
                       (d.flags & ECUIO_DIAG_COMPLETE) != 0;
            },
            8000);
    };
    ASSERT_TRUE(read_to(fake_diagnostics::kItems))
        << "the first read didn't complete; generation " << d.generation;
    EXPECT_EQ(d.rx_id, 0x7E8u);
    fake_diagnostics::expect_report(d);
    for (const char* q : {"03", "07", "0A", "020000"}) {
        EXPECT_EQ(fake_count(q), 1u) << q;
    }

    ASSERT_EQ(ecuio_session_read_diagnostics(s.s), ECUIO_OK);
    ASSERT_TRUE(read_to(2 * fake_diagnostics::kItems + 1))
        << "the second read didn't complete; generation " << d.generation;
    fake_diagnostics::expect_report(d);
    for (const char* q : {"03", "07", "0A", "020000"}) {
        EXPECT_EQ(fake_count(q), 2u) << q;
    }
}

/* The caller's word over ELM327 (ADR-0038's appendix of 2026-11-01): at
 * the fake's running engine and 0 km/h, 01 0D goes after 01 0C and 04
 * clears; moving, nothing goes until the word on a moving vehicle too */
TEST_F(Elm327SessionTest, AClearWithTheCallersWordSendsAtARunningEngine)
{
    for (const int kmh : {-1, 48}) {
        SCOPED_TRACE(kmh < 0 ? "standing" : "moving");
        fake_ecu::reset_diagnostics();
        fake_ecu::diagnostics().speed_kmh = kmh;
        Session s;
        ASSERT_EQ(s.create(uri("?probe=0&identify=0")), ECUIO_OK);
        s.start();
        ecuio_channel_diagnostics_t d{};
        const auto settled = [&] {
            return ecuio_session_get_channel_diagnostics(s.s, 0, &d) ==
                       ECUIO_OK &&
                   d.clear != ECUIO_CLEAR_NONE &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0 &&
                   (d.flags & ECUIO_DIAG_CLEARING) == 0;
        };
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_get_channel_diagnostics(s.s, 0, &d) ==
                           ECUIO_OK &&
                       (d.flags & ECUIO_DIAG_COMPLETE) != 0;
            },
            8000));
        ASSERT_EQ(ecuio_session_clear_diagnostics_with(
                      s.s, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING),
                  ECUIO_OK);
        ASSERT_TRUE(wait_for(settled, 8000));
        EXPECT_EQ(d.clear_rpm, 2000u);
        if (kmh < 0) {
            EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
            EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
            continue;
        }
        EXPECT_EQ(d.clear, ECUIO_CLEAR_MOVING);
        EXPECT_EQ(d.clear_kmh, 48u);
        EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
        ASSERT_EQ(ecuio_session_clear_diagnostics_with(
                      s.s, 0, d.generation,
                      ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING),
                  ECUIO_OK);
        ASSERT_TRUE(wait_for(
            [&] { return settled() && d.clear == ECUIO_CLEAR_CLEARED; }, 8000))
            << "clear " << int{d.clear};
        /* The ECU's own count: the fake's command record keeps only the
         * session's first commands */
        EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
        EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 0u);
    }
}

/* ADR-0038 over ELM327: with the engine stopped, a clear sends 04 once,
 * with no count digit, after the engine speed, and the report read again
 * is the fake's cleared one; with it running, no 04 goes out */
TEST_F(Elm327SessionTest, AClearSends04OnlyWithTheEngineStopped)
{
    for (const bool off : {false, true}) {
        SCOPED_TRACE(off ? "engine off" : "engine running");
        fake_ecu::reset_diagnostics();
        fake_ecu::diagnostics().engine_off = off;
        Session s;
        ASSERT_EQ(s.create(uri("?probe=0&identify=0")), ECUIO_OK);
        s.start();
        ecuio_channel_diagnostics_t d{};
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_get_channel_diagnostics(s.s, 0, &d) ==
                           ECUIO_OK &&
                       (d.flags & ECUIO_DIAG_COMPLETE) != 0;
            },
            8000));
        ASSERT_EQ(ecuio_session_clear_diagnostics(s.s, 0, d.generation),
                  ECUIO_OK);
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_get_channel_diagnostics(s.s, 0, &d) ==
                           ECUIO_OK &&
                       d.clear != ECUIO_CLEAR_NONE &&
                       (d.flags & ECUIO_DIAG_COMPLETE) != 0;
            },
            8000));
        if (off) {
            EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
            EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 0u);
            EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].held, 1u);
            /* The fake's record keeps only the first commands, so a slow
             * run's 04 can fall past it: these counters don't */
            EXPECT_EQ(on_fake<uint32_t>([](fake_elm327::adapter& a) {
                          return a.service04;
                      }),
                      1u);
            EXPECT_EQ(on_fake<uint32_t>(
                          [](fake_elm327::adapter& a) { return a.exact04; }),
                      1u)
                << "04 alone, no count digit";
            /* The speed went before it; the clear's 01 0C carries the count
             * digit as the rotation's does, so the engine's own tests pin
             * which one it was (ClearTest) */
            EXPECT_GT(on_fake<uint32_t>([](fake_elm327::adapter& a) {
                          return a.speed_before_04;
                      }),
                      0u);
        } else {
            EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
            EXPECT_EQ(on_fake<uint32_t>([](fake_elm327::adapter& a) {
                          return a.service04;
                      }),
                      0u);
            fake_diagnostics::expect_report(d);
        }
    }
    fake_ecu::reset_diagnostics();
}

/* ADR-0065: a clear asked of a module past the fourth reaches the
 * worker: the module answers no OBD service, so its gate ends the clear
 * there with nothing cleared */
TEST_F(Elm327SessionTest, AClearReachesAModulePastTheFourth)
{
    SixModules modules;
    fake_ecu::reset_diagnostics();
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=1E1C,7E9:D801,7EA:D802,"
                           "7EB:D803,7EC:D804,7ED:D805&identify=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s.s, 5, &d) ==
                       ECUIO_OK &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        15000));
    EXPECT_EQ(d.rx_id, 0x7EDu);
    ASSERT_EQ(ecuio_session_clear_diagnostics(s.s, 5, d.generation), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s.s, 5, &d) ==
                       ECUIO_OK &&
                   d.clear != ECUIO_CLEAR_NONE &&
                   (d.flags & ECUIO_DIAG_CLEARING) == 0;
        },
        8000));
    EXPECT_EQ(d.clear, ECUIO_CLEAR_NOT_EMISSIONS)
        << "the module refuses every OBD service: nothing sent";
    EXPECT_EQ(fake_ecu::diagnostics().clears, 0u);
    fake_ecu::reset_diagnostics();
}

/* ?diagnose=0: no diagnostics request goes out, Read again included,
 * and the report says none is asked. Diagnostics would take every other
 * turn, so a count of Service 01 requests shows it, whatever the set's
 * size (a lap of the obd2 set is too long a wait). */
TEST_F(Elm327SessionTest, DiagnoseOffAsksNoDiagnostics)
{
    Session s;
    ASSERT_EQ(s.create(uri("?probe=0&identify=0&diagnose=0")), ECUIO_OK);
    s.start();
    EXPECT_EQ(ecuio_session_read_diagnostics(s.s), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return fake_count("01") >= 8; }, 5000));
    for (const char* q : {"03", "07", "0A", "02"}) {
        EXPECT_EQ(fake_count(q), 0u) << q;
    }
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s.s, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.flags, 0u);
    EXPECT_EQ(d.generation, 0u);
}

/* ISO-TP's longest message, 4095 bytes in 586 frame lines, arrives as
 * one frame, every byte in place */
TEST_F(Elm327SessionTest, TheLongestMessageArrivesWhole)
{
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=F1A1&identify=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("62 F1 A1") >= 1; }, 5000));
    Capture::Frame f{};
    ASSERT_TRUE(s.cap.find("62 F1 A1", &f));
    ASSERT_EQ(f.data.size(), 4095u);
    EXPECT_EQ(f.flags & ECUIO_FRAME_FLAG_TRUNCATED, 0u);
    bool in_place = true;
    for (size_t i = 3; i < f.data.size(); ++i) {
        in_place =
            in_place && f.data[i] == static_cast<uint8_t>((i - 3) & 0xFF);
    }
    EXPECT_TRUE(in_place);
}

/* Two ECUs: the receive filter passes both (AT CF/CM), each request goes
 * out under its channel's header, and AT SH is sent only on a switch */
TEST_F(Elm327SessionTest, EachChannelIsAskedUnderItsOwnHeader)
{
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=7E8:1E1C&did=7E9:1E1C"
                           "&identify=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const std::vector<std::string> setup = fake_commands();
    EXPECT_NE(std::find(setup.begin(), setup.end(), "ATCF7E8"), setup.end());
    EXPECT_NE(std::find(setup.begin(), setup.end(), "ATCM7FE"), setup.end());
    s.start();
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int a = 0;
            int b = 0;
            for (const Capture::Frame& f : s.cap.frames) {
                a += f.id == 0x7E8u;
                b += f.id == 0x7E9u;
            }
            return a >= 3 && b >= 3;
        },
        5000));
    const std::vector<std::string> sent = fake_commands();
    std::string header;
    for (size_t i = setup.size(); i < sent.size(); ++i) {
        if (sent[i].rfind("ATSH", 0) == 0) {
            EXPECT_NE(sent[i], header) << "AT SH repeated at " << i;
            header = sent[i];
        } else if (sent[i].rfind("221E1C", 0) == 0) {
            EXPECT_TRUE(header == "ATSH7E0" || header == "ATSH7E1" ||
                        (header.empty() && i == setup.size()))
                << sent[i] << " at " << i;
        }
    }
}

/* Two ECUs in one session answer each its own: the engine ECU's 1E1C
 * on 0x7E8, a module's (fake_ecu's table) D801 on 0x7E9, through one
 * adapter taking turns */
TEST_F(Elm327SessionTest, TwoEcusInOneSessionAnswerEachItsOwn)
{
    struct Module {
        Module() { EXPECT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12")); }
        ~Module() { fake_ecu::set_module_dids(nullptr); }
    } module;
    Session s;
    ASSERT_EQ(
        s.create(uri(
            "?profile=uds-generic&did=1E1C,7E9:D801&identify=0&diagnose=0")),
        ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    const std::vector<uint8_t> engine = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    const std::vector<uint8_t> mod = {0x62, 0xD8, 0x01, 0x00, 0x12};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int a = 0;
            int b = 0;
            for (const Capture::Frame& f : s.cap.frames) {
                a += f.id == 0x7E8u && f.data == engine;
                b += f.id == 0x7E9u && f.data == mod;
            }
            return a >= 3 && b >= 3;
        },
        5000))
        << "three of each ECU's own replies";
    std::lock_guard<std::mutex> g(s.cap.m);
    for (const Capture::Frame& f : s.cap.frames) {
        EXPECT_TRUE((f.id == 0x7E8u && f.data == engine) ||
                    (f.id == 0x7E9u && f.data == mod))
            << "a reply on 0x" << std::hex << f.id << " of " << std::dec
            << f.data.size() << " bytes";
    }
}

/* ADR-0065: a session of more modules than the info lists itself names
 * each through ecuio_session_get_module, keeps a report for each, reads
 * each, and a focus finds a signal on a module past the fourth */
TEST_F(Elm327SessionTest, ASessionOfSixModulesNamesReadsAndFocusesEachOne)
{
    SixModules modules;
    Session s;
    ASSERT_EQ(s.create(uri(kSixModulesQuery)), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 6u);
    EXPECT_EQ(info.channel_count, uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS});
    for (uint32_t c = 0; c < 6; ++c) {
        ecuio_channel_decl_t m{};
        m.flags = 7;
        m.reserved = 7;
        ASSERT_EQ(ecuio_session_get_module(s.s, c, &m), ECUIO_OK) << c;
        EXPECT_EQ(m.rx_id, 0x7E8u + c);
        EXPECT_EQ(m.tx_id, 0x7E0u + c);
        EXPECT_EQ(m.flags, 0u);
        EXPECT_EQ(m.reserved, 0u);
        if (c < ECUIO_SESSION_INFO_MAX_CHANNELS) {
            EXPECT_EQ(info.rx_ids[c], m.rx_id);
            EXPECT_EQ(info.tx_ids[c], m.tx_id);
        }
        ecuio_channel_support_t support{};
        ecuio_channel_identity_t identity{};
        ecuio_channel_diagnostics_t diagnostics{};
        ASSERT_EQ(ecuio_session_get_channel_support(s.s, c, &support),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_get_channel_identity(s.s, c, &identity),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_get_channel_diagnostics(s.s, c, &diagnostics),
                  ECUIO_OK);
        EXPECT_EQ(support.rx_id, m.rx_id);
        EXPECT_EQ(identity.rx_id, m.rx_id);
        EXPECT_EQ(diagnostics.rx_id, m.rx_id);
    }
    ecuio_channel_decl_t past{};
    past.rx_id = 0x123u;
    EXPECT_EQ(ecuio_session_get_module(s.s, 6, &past),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(past.rx_id, 0x123u) << "out untouched";
    EXPECT_EQ(ecuio_session_get_module(nullptr, 0, &past),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_module(s.s, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_channel_support_t none{};
    EXPECT_EQ(ecuio_session_get_channel_support(s.s, 6, &none),
              ECUIO_ERR_INVALID_ARGUMENT);
    /* A clear names a module by the same index: the sixth's report isn't
     * read under diagnose=0, a seventh isn't there */
    EXPECT_EQ(ecuio_session_clear_diagnostics(s.s, 5, 0),
              ECUIO_ERR_REPORT_CHANGED);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s.s, 6, 0),
              ECUIO_ERR_INVALID_ARGUMENT);

    /* A focus on the sixth module's signal asks its DID alone */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.s);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->count, 6u);
    ASSERT_EQ(set->defs[5].message_id, 0x7EDu);
    const uint16_t sixth = set->defs[5].id;
    ASSERT_EQ(ecuio_session_set_focus(s.s, &sixth, 1), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    const auto dids = [] {
        std::vector<std::string> out;
        for (const std::string& cmd : fake_commands()) {
            if (cmd.size() >= 6 && cmd.compare(0, 2, "22") == 0) {
                out.push_back(cmd.substr(0, 6));
            }
        }
        return out;
    };
    ASSERT_TRUE(wait_for([&] { return dids().size() >= 6; }, 5000));
    const std::vector<std::string> focused = dids();
    /* From the second: the first may go out before the worker's first take */
    for (size_t k = 1; k < focused.size(); ++k) {
        EXPECT_EQ(focused[k], "22D805") << k;
    }

    /* With the focus ended every module is read again, each its own DID */
    size_t from = 0;
    {
        std::lock_guard<std::mutex> g(s.cap.m);
        from = s.cap.frames.size();
    }
    ASSERT_EQ(ecuio_session_set_focus(s.s, nullptr, 0), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int seen[6] = {};
            for (size_t k = from; k < s.cap.frames.size(); ++k) {
                const Capture::Frame& f = s.cap.frames[k];
                if (f.id >= 0x7E8u && f.id <= 0x7EDu) {
                    ++seen[f.id - 0x7E8u];
                }
            }
            for (int n : seen) {
                if (n < 2) {
                    return false;
                }
            }
            return true;
        },
        10000))
        << "two replies from each of the six modules";
    std::lock_guard<std::mutex> g(s.cap.m);
    for (size_t k = from; k < s.cap.frames.size(); ++k) {
        const Capture::Frame& f = s.cap.frames[k];
        if (f.id == 0x7E8u) {
            EXPECT_EQ(f.data,
                      (std::vector<uint8_t>{0x62, 0x1E, 0x1C, 0x12, 0x34}));
            continue;
        }
        ASSERT_TRUE(f.id >= 0x7E9u && f.id <= 0x7EDu) << std::hex << f.id;
        const uint8_t n = static_cast<uint8_t>(f.id - 0x7E8u);
        EXPECT_EQ(f.data,
                  (std::vector<uint8_t>{0x62, 0xD8, n, 0x00,
                                        static_cast<uint8_t>(0x10 + n)}))
            << std::hex << f.id;
    }
}

/* ADR-0060: ?session=03 sends 10 03 before any request, the engine ECU
 * answers D200 only in the session it entered, and the close's last
 * command is 10 01 */
TEST_F(Elm327SessionTest, AnExtendedSessionIsEnteredFirstAndLeftAtClose)
{
    SessionsReset reset;
    Session s;
    ASSERT_EQ(s.create(uri("?profile=uds-generic&did=1E1C,D200&session=03"
                           "&identify=0&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    const std::vector<uint8_t> d200 = {0x62, 0xD2, 0x00, 0x1A, 0x2C};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            for (const Capture::Frame& f : s.cap.frames) {
                if (f.id == 0x7E8u && f.data == d200) {
                    return true;
                }
            }
            return false;
        },
        5000))
        << "D200 answers only in the extended session";
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    const std::vector<std::string> sent = fake_commands();
    std::string first;
    for (const std::string& c : sent) {
        if (c.rfind("AT", 0) != 0 && c.rfind("ST", 0) != 0 &&
            c != kProbeCommand) {
            first = c;
            break;
        }
    }
    EXPECT_EQ(first, "1003") << "the first request";
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.back(), "1001") << "the close's last command";
    EXPECT_EQ(fake_count("1003"), 1u);
    EXPECT_FALSE(fake_ecu::in_extended(0x7E8)) << "left at the close";
}
