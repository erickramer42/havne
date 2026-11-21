// File/layer: tests/test_obd2_clear.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

/* ADR-0038: the clear's gate and steps, against the fake ECU */
class ClearTest : public DiagnosticsTest
{};

/* The fake ECU with its engine stopped: 01 0C reads 0 rpm */
Bytes stopped_engine(const Bytes& req)
{
    if (req == Bytes{0x01, 0x0C}) {
        return {0x41, 0x0C, 0x00, 0x00};
    }
    return fake_reply(req);
}

/* An engine reading only diagnostics, read to the end through the fake;
 * the report it holds */
ecuio_channel_diagnostics_t read_report(ecuio_request_engine* e, int64_t* now)
{
    ecuio_engine_init(e, 1);
    ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
    drive(e, now, fake_reply);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(e, 0, &d);
    return d;
}

std::vector<Bytes> requests_of(const std::vector<SentRequest>& sent)
{
    std::vector<Bytes> out;
    for (const SentRequest& s : sent) {
        out.push_back(s.bytes);
    }
    return out;
}

const std::vector<Bytes> kReadAfterAClear = {
    {0x01, 0x01},       {0x01, 0x41},       {0x03}, {0x07}, {0x0A},
    {0x02, 0x00, 0x00}, {0x02, 0x02, 0x00},
};

} // namespace

/* At 0 rpm the clear sends 04 and reads the report again: the fake's
 * stored and pending codes and its freeze frame gone, the MIL off, its
 * catalyst-side monitors incomplete, the permanent code kept */
TEST_F(ClearTest, AStoppedEngineIsClearedAndReadAgain)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    ASSERT_EQ(d.clear, ECUIO_CLEAR_NONE);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_CLEARING);

    const std::vector<Bytes> sent =
        requests_of(drive(&e, &now, stopped_engine));
    std::vector<Bytes> expected = {{0x01, 0x0C}, {0x04}};
    expected.insert(expected.end(), kReadAfterAClear.begin(),
                    kReadAfterAClear.end());
    EXPECT_EQ(sent, expected) << "the speed, 04, then the list again";
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);

    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.clear_nrc, 0u);
    EXPECT_EQ(d.clear_rpm, 0u);
    EXPECT_EQ(d.flags & (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH),
              ECUIO_DIAG_CLEAR_RPM)
        << "0 rpm read; no vehicle speed asked without the caller's word";
    EXPECT_EQ(d.flags & (ECUIO_DIAG_COMPLETE | ECUIO_DIAG_CLEARING |
                         ECUIO_DIAG_MIL_ON | ECUIO_DIAG_FREEZE_FRAME),
              ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.dtc_count, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].held, 0u);
    ASSERT_EQ(d.lists[ECUIO_DIAG_PERMANENT].held, 1u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].codes[0], 0x0420u);
    EXPECT_EQ(d.freeze_count, 0u);
    uint32_t incomplete = 0;
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        incomplete += (d.monitors[i].flags & ECUIO_MONITOR_COMPLETE) ? 0 : 1;
    }
    EXPECT_EQ(d.monitor_count, 8u);
    EXPECT_EQ(incomplete, 5u) << "byte C's monitors start over";
}

/* A turning engine ends the clear before 04, with the speed it read
 * (the fake's 2000 rpm); the codes stay as they were */
TEST_F(ClearTest, ARunningEngineIsNotCleared)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    const uint32_t before = d.generation;
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    EXPECT_EQ(requests_of(drive(&e, &now, fake_reply)),
              (std::vector<Bytes>{{0x01, 0x0C}}));
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);

    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 2000u);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    EXPECT_GT(d.generation, before);
    fake_diagnostics::expect_report(d);
}

/* A speed that turns at all is running: 1 count (0.25 rpm) reads 1 rpm,
 * never 0 */
TEST_F(ClearTest, TheSlowestTurningEngineIsRunning)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    const Responder crawling = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x00, 0x01}
                                        : fake_reply(req);
    };
    EXPECT_EQ(requests_of(drive(&e, &now, crawling)).size(), 1u);
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 1u);
}

/* No speed, refused or unanswered, sends nothing more */
TEST_F(ClearTest, NoEngineSpeedSendsNothing)
{
    const std::pair<Bytes, uint8_t> kAnswers[] = {
        {Bytes{}, 0x00},                 /* silence */
        {Bytes{0x7F, 0x01, 0x12}, 0x12}, /* refused */
        {Bytes{0x41, 0x0C, 0x00}, 0x00}, /* cut short */
    };
    for (const auto& a : kAnswers) {
        SCOPED_TRACE(a.first.size());
        static ecuio_request_engine e;
        int64_t now = 0;
        ecuio_channel_diagnostics_t d = read_report(&e, &now);
        ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
        const Bytes answer = a.first;
        const Responder speed = [answer](const Bytes& req) {
            return req == Bytes{0x01, 0x0C} ? answer : fake_reply(req);
        };
        EXPECT_EQ(requests_of(drive(&e, &now, speed)),
                  (std::vector<Bytes>{{0x01, 0x0C}}));
        ecuio_engine_diagnostics(&e, 0, &d);
        EXPECT_EQ(d.clear, ECUIO_CLEAR_NO_ENGINE_SPEED);
        EXPECT_EQ(d.clear_nrc, a.second);
        EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    }
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
}

/* The caller's word on a running engine (ADR-0038's appendix of
 * 2026-11-01): the fake's 2000 rpm is recorded, not a gate; its 0 km/h
 * sends 04, and the report reads again as at a stopped engine */
TEST_F(ClearTest, ARunningEngineClearsWithTheCallersWord)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(
        ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, fake_reply));
    std::vector<Bytes> expected = {{0x01, 0x0C}, {0x01, 0x0D}, {0x04}};
    expected.insert(expected.end(), kReadAfterAClear.begin(),
                    kReadAfterAClear.end());
    EXPECT_EQ(sent, expected) << "both speeds, 04, then the list again";
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);

    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.clear_rpm, 2000u);
    EXPECT_EQ(d.clear_kmh, 0u);
    EXPECT_EQ(d.flags & (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH),
              ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH)
        << "both read, kept across the read after";
    fake_diagnostics::expect_cleared_report(d);
}

/* Moving, the caller's word on the engine isn't enough: the clear ends
 * at 01 0D with the speed it read, nothing sent; the word on a moving
 * vehicle, at the report's new generation, then clears */
TEST_F(ClearTest, AMovingVehicleNeedsTheCallersWordOnItToo)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    const Responder moving = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0D} ? Bytes{0x41, 0x0D, 0x30}
                                        : fake_reply(req);
    };
    ASSERT_TRUE(
        ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
    EXPECT_EQ(requests_of(drive(&e, &now, moving)),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_MOVING);
    EXPECT_EQ(d.clear_kmh, 48u);
    EXPECT_EQ(d.clear_rpm, 2000u);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
    fake_diagnostics::expect_report(d);

    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation,
                                   ECUIO_CLEAR_ALLOW_RUNNING |
                                       ECUIO_CLEAR_ALLOW_MOVING));
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, moving));
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(std::vector<Bytes>(sent.begin(), sent.begin() + 3),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}, {0x04}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.clear_kmh, 48u) << "the speed it cleared at, recorded";
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
}

/* No vehicle speed, refused, unanswered or cut short, is no word that the
 * vehicle stands: nothing is sent, unless the caller allowed it moving */
TEST_F(ClearTest, NoVehicleSpeedSendsNothingWithoutTheCallersWord)
{
    const std::pair<Bytes, uint8_t> kAnswers[] = {
        {Bytes{}, 0x00},                 /* silence */
        {Bytes{0x7F, 0x01, 0x12}, 0x12}, /* refused */
        {Bytes{0x41, 0x0D}, 0x00},       /* cut short */
    };
    for (const auto& a : kAnswers) {
        SCOPED_TRACE(a.first.size());
        static ecuio_request_engine e;
        int64_t now = 0;
        ecuio_channel_diagnostics_t d = read_report(&e, &now);
        const Bytes answer = a.first;
        const Responder speed = [answer](const Bytes& req) {
            return req == Bytes{0x01, 0x0D} ? answer : fake_reply(req);
        };
        ASSERT_TRUE(
            ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
        EXPECT_EQ(requests_of(drive(&e, &now, speed)),
                  (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}}));
        ecuio_engine_diagnostics(&e, 0, &d);
        EXPECT_EQ(d.clear, ECUIO_CLEAR_NO_VEHICLE_SPEED);
        EXPECT_EQ(d.clear_nrc, a.second);
        EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEAR_KMH);
        EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);

        ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation,
                                       ECUIO_CLEAR_ALLOW_RUNNING |
                                           ECUIO_CLEAR_ALLOW_MOVING));
        drive(&e, &now, speed);
        ecuio_engine_diagnostics(&e, 0, &d);
        EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
        fake_ecu::reset_diagnostics();
    }
}

/* Under the caller's word on the engine, an ECU that gives no engine
 * speed (a transmission's, say) is asked the vehicle speed and cleared;
 * the report says no engine speed was read */
TEST_F(ClearTest, NoEngineSpeedClearsWithTheCallersWord)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    const Responder no_rpm = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0C} ? Bytes{} : fake_reply(req);
    };
    ASSERT_TRUE(
        ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, no_rpm));
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(std::vector<Bytes>(sent.begin(), sent.begin() + 3),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}, {0x04}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEAR_RPM);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_CLEAR_KMH);
}

/* A refusal (7F 04 22: the engine's conditions) changed nothing, so
 * nothing is read again */
TEST_F(ClearTest, ARefusedClearReadsNothingAgain)
{
    fake_ecu::diagnostics().clear_nrc = 0x22;
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    EXPECT_EQ(requests_of(drive(&e, &now, stopped_engine)),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x04}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_REFUSED);
    EXPECT_EQ(d.clear_nrc, 0x22u);
    fake_diagnostics::expect_report(d);
}

/* 04 unanswered may still have cleared: the report is read again, which
 * here shows the codes kept */
TEST_F(ClearTest, AnUnansweredClearReadsTheReportAgain)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    const Responder deaf = [](const Bytes& req) {
        return req == Bytes{0x04} ? Bytes{} : stopped_engine(req);
    };
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, deaf));
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(sent[1], (Bytes{0x04}));
    EXPECT_EQ(sent[2], (Bytes{0x01, 0x01})) << "the read again";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_UNANSWERED);
    fake_diagnostics::expect_report(d);
}

/* Only the report the caller saw is cleared: one never read, one mid-
 * read, or another generation records REPORT_CHANGED and sends nothing;
 * a channel past the count, or one already clearing, is left as it is */
TEST_F(ClearTest, OnlyTheReportTheCallerSawIsCleared)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_engine_init(&e, 1);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, 0)) << "no diagnostics read";
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_NONE);
    EXPECT_EQ(d.generation, 0u);

    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, 0)) << "mid-read";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_REPORT_CHANGED);
    for (const Bytes& b : requests_of(drive(&e, &now, stopped_engine))) {
        EXPECT_NE(b, (Bytes{0x04}));
        EXPECT_NE(b, (Bytes{0x01, 0x0C}));
    }

    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, d.generation - 1)) << "older";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_REPORT_CHANGED);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, d.generation + 1)) << "newer";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_FALSE(ecuio_engine_clear(&e, 1, d.generation)) << "no channel 1";
    ecuio_channel_diagnostics_t same;
    ecuio_engine_diagnostics(&e, 0, &same);
    EXPECT_EQ(same.generation, d.generation) << "channel 0 untouched";
    EXPECT_TRUE(requests_of(drive(&e, &now, stopped_engine)).empty());
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);

    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, d.generation)) << "one at a time";
    ecuio_channel_diagnostics_t after;
    ecuio_engine_diagnostics(&e, 0, &after);
    EXPECT_EQ(after.generation, d.generation) << "the running one untouched";
    EXPECT_EQ(after.clear, ECUIO_CLEAR_NONE);
    drive(&e, &now, stopped_engine);
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
}

/* A Read again waits for a clear under way, and a later one keeps the
 * last clear's outcome while the report starts over */
TEST_F(ClearTest, ReadAgainKeepsTheLastClear)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_diagnose_again(&e);
    ecuio_engine_rescan(&e);
    EXPECT_EQ(requests_of(drive(&e, &now, fake_reply)),
              (std::vector<Bytes>{{0x01, 0x0C}}))
        << "the clear alone, ended by the running engine";

    ecuio_engine_diagnose_again(&e);
    std::vector<Bytes> list = requests_of(drive(&e, &now, fake_reply));
    ASSERT_FALSE(list.empty());
    EXPECT_EQ(list.front(), (Bytes{0x01, 0x01}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 2000u);
    fake_diagnostics::expect_report(d);
}

/* A rescan (the link back to ok) leaves a clearing channel's report as
 * the caller saved it: an item left unanswered isn't asked again until
 * the clear ends */
TEST_F(ClearTest, ARescanWaitsForTheClear)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    const Responder no_pending = [](const Bytes& req) {
        return req == Bytes{0x07} ? Bytes{} : fake_reply(req);
    };
    drive(&e, &now, no_pending);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    ASSERT_EQ(d.lists[ECUIO_DIAG_PENDING].outcome, ECUIO_DIAG_UNANSWERED);

    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_rescan(&e);
    EXPECT_EQ(requests_of(drive(&e, &now, fake_reply)),
              (std::vector<Bytes>{{0x01, 0x0C}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].outcome, ECUIO_DIAG_UNANSWERED);
}
