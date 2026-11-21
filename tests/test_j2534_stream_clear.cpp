// File/layer: tests/test_j2534_stream_clear.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534AdoptionTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

#include "new_counter.h"   /* tests: operator new counted per thread */

namespace {

/* ADR-0038 through the surrogate: with the fake's engine stopped, a clear
 * of the report the test saw sends 04, and the report read again holds
 * the fake's cleared state; the same generation again is refused */
TEST_F(J2534AdoptionTest, AClearAtAStoppedEngineEmptiesTheReport)
{
    KnobGuard off("ECUIO_FAKE_ENGINE_OFF", "1");
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&probe=0&identify=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000));
    fake_diagnostics::expect_report(d);
    const uint32_t saved = d.generation;

    ASSERT_EQ(ecuio_session_clear_diagnostics(session, 0, saved), ECUIO_OK);
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return r.clear == ECUIO_CLEAR_CLEARED &&
                   (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000))
        << "clear " << int{d.clear} << ", generation " << d.generation;
    fake_diagnostics::expect_cleared_report(d);
    EXPECT_EQ(ecuio_session_clear_diagnostics(session, 0, saved),
              ECUIO_ERR_REPORT_CHANGED)
        << "the saved report is gone";
    ecuio_session_destroy(session);
}

/* The fake's engine turns (2000 rpm): the clear ends unsent, saying so,
 * and the codes stay */
TEST_F(J2534AdoptionTest, AClearWithTheEngineRunningSendsNo04)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&probe=0&identify=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000));
    ASSERT_EQ(ecuio_session_clear_diagnostics(session, 0, d.generation),
              ECUIO_OK);
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return r.clear != ECUIO_CLEAR_NONE;
        },
        5000));
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 2000u);
    fake_diagnostics::expect_report(d);
    ecuio_session_destroy(session);
}

/* The caller's word through the surrogate (ADR-0038's appendix of
 * 2026-11-01): the fake's running engine, its vehicle at 48 km/h
 * (ECUIO_FAKE_SPEED_KMH): the word on the engine stops at the speed,
 * nothing sent; the word on a moving vehicle too clears it */
TEST_F(J2534AdoptionTest, AClearOfAMovingCarNeedsTheCallersWordOnIt)
{
    KnobGuard moving("ECUIO_FAKE_SPEED_KMH", "48");
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&probe=0&identify=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000));
    ASSERT_EQ(ecuio_session_clear_diagnostics_with(session, 0, d.generation,
                                                   ECUIO_CLEAR_ALLOW_RUNNING),
              ECUIO_OK);
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return r.clear != ECUIO_CLEAR_NONE &&
                   (r.flags & ECUIO_DIAG_CLEARING) == 0;
        },
        5000));
    EXPECT_EQ(d.clear, ECUIO_CLEAR_MOVING);
    EXPECT_EQ(d.clear_kmh, 48u);
    EXPECT_EQ(d.clear_rpm, 2000u);
    fake_diagnostics::expect_report(d);

    ASSERT_EQ(ecuio_session_clear_diagnostics_with(
                  session, 0, d.generation,
                  ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING),
              ECUIO_OK);
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return r.clear == ECUIO_CLEAR_CLEARED &&
                   (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000))
        << "clear " << int{d.clear} << ", generation " << d.generation;
    fake_diagnostics::expect_cleared_report(d);
    ecuio_session_destroy(session);
}

/* The worker's whole engine past start-up (the probe's walk, the
 * identification one-shots, the rotation) and the decode for a values
 * callback cost it no allocation per frame, so none can throw. Counts
 * core's operator new only: the fake DLL's own don't reach the
 * counter */
TEST_F(J2534AdoptionTest, TheWorkerAllocatesNothingPerFrame)
{
    struct News {
        std::atomic<int> frames{0};
        std::atomic<unsigned long> at_from{0};
        std::atomic<unsigned long> at_to{0};
        std::atomic<int> batches{0};
    };
    static constexpr int kFrom = 3;
    static constexpr int kTo = 40;
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    News news;
    ASSERT_EQ(ecuio_register_frame_callback(
                  session,
                  [](const ecuio_frame_t*, void* user_data) {
                      auto* n = static_cast<News*>(user_data);
                      const int i = n->frames.fetch_add(1) + 1;
                      if (i == kFrom)
                          n->at_from.store(new_counter::on_this_thread());
                      if (i == kTo)
                          n->at_to.store(new_counter::on_this_thread());
                  },
                  &news),
              ECUIO_OK);
    ASSERT_EQ(ecuio_register_values_callback(
                  session,
                  [](const ecuio_value_batch_t*, void* user_data) {
                      ++static_cast<News*>(user_data)->batches;
                  },
                  &news),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (news.frames.load() < kTo &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    ASSERT_GE(news.frames.load(), kTo);
    EXPECT_GT(news.batches.load(), 0) << "the values path ran";
    EXPECT_EQ(news.at_to.load() - news.at_from.load(), 0u)
        << "operator new on the worker between frames " << kFrom << " and "
        << kTo;
}

/* A connect while the ECU answers nothing (reads bring no frame): the
 * walk's first bitmap times out, nothing is judged, and every PID stays
 * while the bus is quiet. Once replies flow the link is ok again, and
 * the walk resumes and drops what the bitmaps mark unsupported. The ECU
 * stays silent for an unsupported PID, so only the walk can drop 0x12. */
TEST_F(J2534AdoptionTest, ProbeResumesWhenTheLinkRecovers)
{
    static const uint8_t kPids[] = {0x12, 0x0C};
    static ecuio_signal_def_t defs[2];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(register_pid_profile("test-probe-recovery", kPids,
                                                 2, defs, &set));
    KnobGuard silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");
    std::unique_ptr<KnobGuard> quiet =
        std::make_unique<KnobGuard>("ECUIO_FAKE_READ_STATUS", "9");

    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=test-probe-recovery&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    int32_t le = 0;
    uint32_t ec = 0;
    for (int i = 0; i < 500 && le != ECUIO_ERR_VEHICLE_TIMEOUT; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)ecuio_session_get_error_state(session, &le, &ec);
    }
    ASSERT_EQ(le, ECUIO_ERR_VEHICLE_TIMEOUT) << "the bus never read as quiet";
    expect_write(0, 0x01, 0x00);
    for (unsigned long i = 1; i < 5; ++i) {
        expect_write(i, 0x01, kPids[(i - 1) % 2]);
    }
    ecuio_channel_support_t s = {};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
    EXPECT_EQ(s.ranges, 0u);
    EXPECT_EQ(s.dropped, 0u) << "silence dropped a request";

    quiet.reset();
    EXPECT_TRUE(wait_for_support(session, s, [](const ecuio_channel_support_t& r) {
        return (r.flags & ECUIO_SUPPORT_COMPLETE) != 0;
    }, 3000)) << "the walk didn't resume (flags " << s.flags << ")";
    EXPECT_EQ(s.ranges, 0x3Fu);
    ASSERT_EQ(s.dropped, 1u);
    EXPECT_EQ(s.dropped_list[0].request[1], 0x12u);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_BITMAP);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

} // namespace

#endif // _WIN32
