// File/layer: tests/test_core.cpp | GTest, UNIT tier, headless CI job

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h> /* _mkdir, _rmdir */
#else
#include <sys/stat.h>
#include <time.h> /* clock_gettime: the platform's clock, for ecuio_clock_us */
#include <unistd.h>
#endif

extern "C" {
#include "ecuio/version.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "ecuio/profile.h"
#include "ecuio/vin.h"
#include "fake_diagnostics.h" /* tests: the fake ECU's report, the mock's too */
}

/* C++ headers (namespaced classes and inline functions): must stay
 * outside the extern "C" block above. */
#include "error_detail.h" /* core/src: a refused vendor call's words */
#include "link_health.h"  /* core/src: the workers' link-health rules */
#include "replay_faults.h" /* core/src: ?faults= and its draws */
#include "mock_drive_cycle.h"
#include "mock_ecu.h"      /* core/src: the simulated ECU's answers */
#include "def_index.h"     /* core/src: the defs a frame can match */
#include "precise_wait.h"  /* core/src: a requesting worker's waits */
#include "request_setup.h" /* core/src: the requests and the focus's map */
#include "session.h"       /* core/src: the session's focus fields */
#include "uds_generic.h"   /* core/src: the template set's identity */
#include "surrogate_log_names.h" /* core/src: the surrogate logs pruning keeps */
#include "new_counter.h"   /* tests: operator new counted per thread */
#include "temp_name.h"     /* tests: a scratch name of this process's own */

/* new_counter's count for the binary's other files, which can't include
 * the header that replaces operator new (test_recording_check.cpp) */
unsigned long news_on_this_thread()
{
    return new_counter::on_this_thread();
}

/* The same for new_counter::fail_new (test_export.cpp) */
void fail_new_on_this_thread(unsigned long n)
{
    new_counter::fail_new(n);
}

namespace {

std::atomic<int> g_callback_count{0};

void counting_callback(const ecuio_frame_t* frame, void* user_data)
{
    (void)frame;
    int* counter = static_cast<int*>(user_data);
    if (counter) {
        (*counter)++;
    }
    g_callback_count++;
}

/* Helper to create test session */
ecuio_session_t* make_test_session(void)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);
    return session;
}

/* Poll-until-predicate with deadline. The mock's 50ms cadence is a
 * LOCAL expectation, not a CI-runner guarantee: a starved macOS VM
 * can deliver 2 frames in 200+ ms of wall clock. Polling keeps the
 * asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). Deadline is long
 * enough to absorb pathological runner load, short enough that a
 * genuinely dead transport still fails the test promptly. */
template <typename Predicate>
bool wait_for(Predicate pred, int timeout_ms, int poll_ms = 10)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return pred();   // final check past the deadline, then give up
}

} // anonymous namespace

/* Each create failure says why: a status, and words in
 * ecuio_last_error_detail. A success, or a null argument, leaves "". */
TEST(CoreTest, CreateFailuresNameTheirCause) {
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create("can://bus0", &s), ECUIO_ERR_NO_TRANSPORT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "unsupported URI scheme: expected mock://, replay://, "
                 "j2534://, elm327:// or socketcan://");
    EXPECT_EQ(ecuio_session_create("mock://x?interval=10", &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "mock:// takes no query key 'interval', only profile, did and "
                 "diagnose");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=mock&&profile=raw", &s),
              ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "malformed query: expected key=value pairs joined by '&'");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=", &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?profile= needs a profile name of 1 to 63 characters");
    std::string long_query = "mock://x?profile=mock";
    for (int i = 0; i < 16; ++i) {
        long_query += "&profile=mock";
    }
    EXPECT_EQ(ecuio_session_create(long_query.c_str(), &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the query has more than 16 key=value pairs");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_STREQ(ecuio_last_error_detail(), "no profile is named 'bogus'");

    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_session_destroy(s);

    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(ecuio_session_create(nullptr, &s), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
}

/* A refused vendor call's detail is one line: the vendor's text loses
 * its line breaks and trailing blanks, and text that's only blanks is
 * left out, as is no text at all. */
TEST(CoreTest, VendorDetailIsOneLine)
{
    ecuio_detail_set_vendor("PassThruOpen", 8, "no adapter\r\nattached  \n");
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "PassThruOpen returned status 8 (0x8, ERR_DEVICE_NOT_CONNECTED): "
        "no adapter  attached");
    ecuio_detail_set_vendor("PassThruOpen", 8, " \r\n");
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "PassThruOpen returned status 8 (0x8, ERR_DEVICE_NOT_CONNECTED)");
    ecuio_detail_set_vendor("PassThruConnect", 34, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruConnect returned status 34 (0x22, vendor-specific)");
    ecuio_detail_clear();
}

/* An open refused with the vendor's own status and no text asks after
 * the two usual causes; the vendor's text, a status J2534 names, or
 * another call keeps the plain form. */
TEST(CoreTest, AnOpenRefusedWithoutTextAsksAfterTheAdapter)
{
    ecuio_detail_set_vendor("PassThruOpen", 34, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruOpen returned status 34 (0x22, vendor-specific): "
                 "is the adapter attached, and not open in another program?");
    ecuio_detail_set_vendor("PassThruOpen", 34, " \r\n");
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruOpen returned status 34 (0x22, vendor-specific): "
                 "is the adapter attached, and not open in another program?");
    ecuio_detail_set_vendor("PassThruOpen", 34, "in use");
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruOpen returned status 34 (0x22, vendor-specific): "
                 "in use");
    ecuio_detail_set_vendor("PassThruOpen", 8, nullptr);
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "PassThruOpen returned status 8 (0x8, ERR_DEVICE_NOT_CONNECTED)");
    ecuio_detail_clear();
}

/* Per thread, like dlerror(): a failure on one thread leaves another
 * thread's detail as it was. */
TEST(CoreTest, LastErrorDetailIsPerThread) {
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=bogus", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    std::string other;
    std::thread t([&other]() {
        ecuio_session_t* s2 = nullptr;
        (void)ecuio_session_create("mock://x?interval=1", &s2);
        other = ecuio_last_error_detail();
    });
    t.join();
    EXPECT_STREQ(ecuio_last_error_detail(), "no profile is named 'bogus'");
    EXPECT_EQ(other, "mock:// takes no query key 'interval', only profile, did "
                     "and diagnose");
}

/* A mock session says it's one, names its profile and its one channel
 * (the simulated engine ECU), and reports nothing else; the info
 * outlives close. */
TEST(CoreTest, MockSessionInfoNamesTheBackend) {
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_session_info_t info;
    memset(&info, 0xEE, sizeof(info));
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_MOCK);
    EXPECT_EQ(info.dll_arch, ECUIO_DLL_ARCH_UNKNOWN);
    EXPECT_EQ(info.flags, 0u);
    EXPECT_EQ(info.protocol_id, 0u);
    EXPECT_EQ(info.baud, 0u);
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    EXPECT_STREQ(info.firmware_version, "");
    EXPECT_STREQ(info.profile, "mock");
    for (uint32_t r : info.reserved) {
        EXPECT_EQ(r, 0u);
    }

    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    memset(&info, 0, sizeof(info));
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_MOCK);

    EXPECT_EQ(ecuio_session_get_info(s, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_info(nullptr, &info), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(s);

    ASSERT_EQ(ecuio_session_create("mock://demo?profile=raw", &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_STREQ(info.profile, "raw");
    ecuio_session_destroy(s);
}

/* A mock session has no channels, so no ECU to report on: every index
 * is out of range, and the report is left as it was. */
/* The mock has one channel, the simulated engine ECU (0x7E8, requests
 * to 0x7E0), whose support and identity reports stay empty: it answers
 * no bitmap or identification */
TEST(CoreTest, MockSessionHasOneQuietChannel)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    ecuio_channel_support_t support;
    memset(&support, 0xEE, sizeof(support));
    ASSERT_EQ(ecuio_session_get_channel_support(s, 0, &support), ECUIO_OK);
    EXPECT_EQ(support.rx_id, 0x7E8u);
    EXPECT_EQ(support.flags, 0u);
    EXPECT_EQ(support.generation, 0u);
    ecuio_channel_identity_t identity;
    memset(&identity, 0xEE, sizeof(identity));
    ASSERT_EQ(ecuio_session_get_channel_identity(s, 0, &identity), ECUIO_OK);
    EXPECT_EQ(identity.rx_id, 0x7E8u);
    EXPECT_EQ(identity.items, 0u);
    EXPECT_EQ(identity.flags, 0u);
    memset(&support, 0xEE, sizeof(support));
    EXPECT_EQ(ecuio_session_get_channel_support(s, 1, &support),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(support.flags, 0xEEEEEEEEu);
    EXPECT_EQ(ecuio_session_get_channel_support(s, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_channel_support(nullptr, 0, &support),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(s);
}

/* The simulated ECU's diagnostics (ADR-0037) are the fakes' report,
 * read at create, before start, whatever the profile */
TEST(CoreTest, MockReportsTheFakeEcusDiagnostics)
{
    for (const char* uri :
         {"mock://demo", "mock://demo?profile=raw", "mock://demo?profile=obd2",
          "mock://demo?profile=uds-generic&did=1E1C"}) {
        SCOPED_TRACE(uri);
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_channel_diagnostics_t d{};
        ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
        EXPECT_EQ(d.rx_id, 0x7E8u);
        EXPECT_EQ(d.generation, fake_diagnostics::kItems);
        fake_diagnostics::expect_report(d);
        EXPECT_EQ(ecuio_session_get_channel_diagnostics(s, 1, &d),
                  ECUIO_ERR_INVALID_ARGUMENT);
        ecuio_session_destroy(s);
    }
}

/* Read again on the mock reads the list once more on the worker's next
 * tick: the generation moves past a second read, the report the same */
TEST(CoreTest, MockReadAgainReadsTheSameReport)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(s), ECUIO_OK)
        << "before start: taken on the first tick";
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    const uint32_t again = 2 * fake_diagnostics::kItems + 1;
    ecuio_channel_diagnostics_t d{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((ecuio_session_get_channel_diagnostics(s, 0, &d) != ECUIO_OK ||
            d.generation != again) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(d.generation, again);
    fake_diagnostics::expect_report(d);

    /* Two more, one read: the worker takes what has come since it last
     * looked */
    EXPECT_EQ(ecuio_session_read_diagnostics(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(s), ECUIO_OK);
    const uint32_t third = again + 1 + fake_diagnostics::kItems;
    const auto deadline2 =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((ecuio_session_get_channel_diagnostics(s, 0, &d) != ECUIO_OK ||
            d.generation < third) &&
           std::chrono::steady_clock::now() < deadline2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.generation, third);
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(s), ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
}

/* ?diagnose=0 on the mock: the channel stays, its report empty and not
 * asked, Read again or not; a bad value is refused at create */
TEST(CoreTest, MockDiagnoseOffReportsNone)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo?diagnose=0", &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(s), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.rx_id, 0x7E8u);
    EXPECT_EQ(d.flags, 0u);
    EXPECT_EQ(d.generation, 0u);
    ecuio_session_destroy(s);

    EXPECT_EQ(ecuio_session_create("mock://demo?diagnose=2", &s),
              ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(), "?diagnose= needs 0 or 1, not '2'");
}

/* ADR-0038 on the simulated ECU, no worker: at an engine speed the
 * drive cycle could give it, a clear asks 01 0C and stops there while
 * the engine turns; with it stopped, 04 clears the ECU and the list read
 * again is the fake's cleared report */
TEST(CoreTest, MockEcuClearsOnlyWithTheEngineStopped)
{
    auto e = std::make_unique<ecuio_request_engine>();
    ecuio_engine_init(e.get(), 1);
    ecuio_engine_diagnose(e.get(), ecuio_obd2_signal_set());
    const ecuio_channel_diagnostics_t& d = e->chans[0].diag;
    ecuio_mock::DriveCycle car;
    ecuio_mock_ecu ecu;
    ecu.engine = &car;
    int64_t now = 0;
    ecuio_mock_ecu_read(e.get(), &ecu, &now);
    fake_diagnostics::expect_report(d);

    car.rpm = 812.25f;
    ASSERT_TRUE(ecuio_engine_clear(e.get(), 0, d.generation));
    ecuio_mock_ecu_read(e.get(), &ecu, &now);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 813u) << "rounded up";
    EXPECT_FALSE(ecu.cleared);
    fake_diagnostics::expect_report(d);

    car.rpm = 0.0f;
    ASSERT_TRUE(ecuio_engine_clear(e.get(), 0, d.generation));
    ecuio_mock_ecu_read(e.get(), &ecu, &now);
    EXPECT_TRUE(ecu.cleared);
    fake_diagnostics::expect_cleared_report(d);
}

/* The caller's word (ecuio_session_clear_diagnostics_with's flags) on
 * the simulated ECU, no worker: moving, the word on the engine stops at
 * 01 0D with the drive cycle's speed; the word on a moving vehicle too
 * clears it, which the mock never refuses */
TEST(CoreTest, MockEcuClearsAMovingCarOnlyWithTheCallersWord)
{
    auto e = std::make_unique<ecuio_request_engine>();
    ecuio_engine_init(e.get(), 1);
    ecuio_engine_diagnose(e.get(), ecuio_obd2_signal_set());
    const ecuio_channel_diagnostics_t& d = e->chans[0].diag;
    ecuio_mock::DriveCycle car;
    ecuio_mock_ecu ecu;
    ecu.engine = &car;
    int64_t now = 0;
    ecuio_mock_ecu_read(e.get(), &ecu, &now);

    car.rpm = 2400.0f;
    car.speed_kmh = 47.6f;
    ASSERT_TRUE(ecuio_engine_clear(e.get(), 0, d.generation,
                                   ECUIO_CLEAR_ALLOW_RUNNING));
    ecuio_mock_ecu_read(e.get(), &ecu, &now);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_MOVING);
    EXPECT_EQ(d.clear_kmh, 48u) << "rounded";
    EXPECT_EQ(d.clear_rpm, 2400u);
    EXPECT_FALSE(ecu.cleared);

    ASSERT_TRUE(ecuio_engine_clear(e.get(), 0, d.generation,
                                   ECUIO_CLEAR_ALLOW_RUNNING |
                                       ECUIO_CLEAR_ALLOW_MOVING));
    ecuio_mock_ecu_read(e.get(), &ecu, &now);
    EXPECT_TRUE(ecu.cleared);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    fake_diagnostics::expect_cleared_report(d);
}

/* Through the worker: the flags reach the mock's engine. At its start
 * the engine is stopped and the car stands, so the word on the engine
 * clears it, both speeds read */
TEST(CoreTest, TheCallersWordReachesTheWorker)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    EXPECT_EQ(ecuio_session_clear_diagnostics_with(s, 0, d.generation,
                                                   ECUIO_CLEAR_ALLOW_RUNNING),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s, 0, &d) ==
                       ECUIO_OK &&
                   d.clear != ECUIO_CLEAR_NONE &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        2000));
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_CLEAR_KMH)
        << "01 0D asked: only the word on the engine asks it";
    ecuio_session_destroy(s);
}

/* Through the worker: a session starts with the key on and the engine
 * stopped, so a clear of the report saved at create empties it on an
 * early tick, and that generation is gone; a new connect starts
 * uncleared */
TEST(CoreTest, MockClearsTheReportLikeTheFakeEcu)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    const uint32_t saved = d.generation;
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 0, saved), ECUIO_OK)
        << "before start: taken on the first tick";
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s, 0, &d) ==
                       ECUIO_OK &&
                   d.clear != ECUIO_CLEAR_NONE &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        2000));
    fake_diagnostics::expect_cleared_report(d);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 0, saved),
              ECUIO_ERR_REPORT_CHANGED);
    ecuio_session_destroy(s);

    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    fake_diagnostics::expect_report(d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_NONE);
    ecuio_session_destroy(s);
}

/* ADR-0038: a clear names the report the caller saved, so anything else
 * is refused before a request could go: a stale generation, a channel
 * past the count, a session reading no diagnostics, a closed session */
TEST(CoreTest, AClearNeedsTheReportTheCallerSaved)
{
    EXPECT_EQ(ecuio_session_clear_diagnostics(nullptr, 0, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 0, d.generation - 1),
              ECUIO_ERR_REPORT_CHANGED);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 0, d.generation + 1),
              ECUIO_ERR_REPORT_CHANGED);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 1, d.generation),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_clear_diagnostics_with(s, 0, d.generation, 0x4u),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a flag core doesn't know";
    ASSERT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 0, d.generation),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);

    ASSERT_EQ(ecuio_session_create("mock://demo?diagnose=0", &s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_clear_diagnostics(s, 0, 0),
              ECUIO_ERR_REPORT_CHANGED)
        << "nothing read, so nothing saved";
    ecuio_session_destroy(s);
}

TEST(CoreTest, StatusStringValidValues) {
    EXPECT_STREQ(ecuio_status_string(ECUIO_OK), "success");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_NULL_ARGUMENT), "null argument");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_NOT_IMPLEMENTED), "not implemented yet");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_NO_TRANSPORT), "no transport available");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_OUT_OF_MEMORY), "out of memory");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_SESSION_CLOSED), "session already closed");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_ALREADY_REGISTERED), "already registered");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_LOAD_FAILED), "couldn't open what was named");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_INVALID_ARGUMENT), "invalid argument");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_BAD_URI), "malformed transport URI");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_UNKNOWN_PROFILE), "unknown profile");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_VENDOR_REFUSED), "the adapter's driver refused");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_SURROGATE_FAILED), "surrogate process failed");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_RECORDING_CHANGED),
                 "the recording changed after it was opened");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_REPORT_CHANGED),
                 "the diagnostics report changed since it was saved");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_VEHICLE_TIMEOUT),
                 "vehicle not responding (timeout)");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_RECORDING_FAILED),
                 "the recording couldn't be made or written");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_CANCELED), "canceled");

    /* Explicit cast required by MSVC for enum conversion */
    EXPECT_STRNE(ecuio_status_string(static_cast<ecuio_status_t>(999)), nullptr);
}

TEST(CoreTest, VersionStringContainsNumbers) {
    const char* version = ecuio_version_string();
    EXPECT_TRUE(version != nullptr);
    EXPECT_GT(strlen(version), 0);
    EXPECT_NE(strstr(version, "."), nullptr);
}

/* ecuio_version (ADR-0053): version.h's numbers, the string's too, and
 * a NULL refused with nothing written */
TEST(CoreTest, VersionNumbersAreTheStrings) {
    uint32_t major = 99, minor = 99, patch = 99;
    ASSERT_EQ(ecuio_version(&major, &minor, &patch), ECUIO_OK);
    EXPECT_EQ(major, static_cast<uint32_t>(ECUIO_VERSION_MAJOR));
    EXPECT_EQ(minor, static_cast<uint32_t>(ECUIO_VERSION_MINOR));
    EXPECT_EQ(patch, static_cast<uint32_t>(ECUIO_VERSION_PATCH));
    EXPECT_EQ(std::to_string(major) + "." + std::to_string(minor) + "." +
                  std::to_string(patch),
              ecuio_version_string());

    uint32_t a = 7, b = 7, c = 7;
    EXPECT_EQ(ecuio_version(nullptr, &b, &c), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_version(&a, nullptr, &c), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_version(&a, &b, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(a, 7u);
    EXPECT_EQ(b, 7u);
    EXPECT_EQ(c, 7u);
}

TEST(CoreTest, InitShutdownIdempotent) {
    EXPECT_EQ(ecuio_init(), ECUIO_OK);
    EXPECT_EQ(ecuio_init(), ECUIO_OK);
    
    ecuio_shutdown();
    
    EXPECT_EQ(ecuio_init(), ECUIO_OK);
    ecuio_shutdown();
}

/* core.h's promise: init and shutdown are optional. A session needs
 * neither, and one that is open when shutdown runs keeps delivering. */
TEST(CoreTest, ShutdownLeavesAnOpenSessionWorking) {
    ecuio_shutdown();   /* with no init before it */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://no-init", &session), ECUIO_OK);
    std::atomic<int> frames{0};
    ASSERT_EQ(ecuio_register_frame_callback(
                  session,
                  [](const ecuio_frame_t*, void* user_data) {
                      static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
                  },
                  &frames),
              ECUIO_OK);
    ecuio_shutdown();
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_TRUE(wait_for([&frames]() { return frames.load() >= 2; }, 5000))
        << "only " << frames.load() << " frames after shutdown";
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

TEST(CoreTest, SessionCreateRequiresNonNullPointer) {
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);
    
    EXPECT_EQ(ecuio_session_create("mock://test", nullptr), ECUIO_ERR_NULL_ARGUMENT);
    
    if (session) {
        ecuio_session_close(session);
        ecuio_session_destroy(session);
    }
}

TEST(CoreTest, RegisterCallbackSuccess) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    ecuio_status_t status = ecuio_register_frame_callback(
        session, counting_callback, &counter
    );
    
    EXPECT_EQ(status, ECUIO_OK);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, RegisterDuplicateCallbackIdempotent) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    ecuio_status_t status1 = ecuio_register_frame_callback(
        session, counting_callback, &counter
    );
    ecuio_status_t status2 = ecuio_register_frame_callback(
        session, counting_callback, &counter
    );
    
    EXPECT_EQ(status1, ECUIO_OK);
    EXPECT_EQ(status2, ECUIO_OK);   /* Silently deduplicated, not an error */
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, SameCallbackDifferentUserdataAllowed) {
    ecuio_session_t* session = make_test_session();
    int counter1 = 0;
    int counter2 = 0;
    
    /* Same function, different user_data = distinct registrations */
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter1), ECUIO_OK);
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter2), ECUIO_OK);

    /* ...so unregistering one pair leaves the other */
    EXPECT_EQ(
        ecuio_unregister_frame_callback(session, counting_callback, &counter2),
        ECUIO_OK);
    ecuio_frame_t frame{};
    ecuio_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter1, 1);
    EXPECT_EQ(counter2, 0);

    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, UnregisterCallback) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    
    EXPECT_EQ(ecuio_unregister_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    
    /* Second unregister is also success (idempotent) */
    EXPECT_EQ(ecuio_unregister_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);

    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, UnregisterNonexistentCallbackDoesntFail) {
    ecuio_session_t* session = make_test_session();
    
    /* Never registered, but should not crash */
    EXPECT_EQ(ecuio_unregister_frame_callback(
        session, counting_callback, nullptr), ECUIO_OK);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, NullPointerHandling) {
    EXPECT_EQ(ecuio_register_frame_callback(
        nullptr, counting_callback, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    
    ecuio_session_t* session = make_test_session();
    EXPECT_EQ(ecuio_register_frame_callback(
        session, nullptr, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

/* Threading contract test - validates snapshot pattern compiles */
TEST(CoreTest, CallbackSnapshotPatternCompiles) {
    /* This test documents the threading discipline:
     * 1. Mock transport will call ecuio_session_dispatch_callbacks()
     * 2. That function copies the callback list under lock
     * 3. Then invokes callbacks OUTSIDE the lock
     *
     * This prevents deadlock if callback calls unregister().
     *
     * Full threading validation requires TSAN job or live transport test.
     */
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    
    /* Null frame is a documented no-op: proves precondition guard */
    ecuio_session_dispatch_callbacks(session, nullptr);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, CallbackDispatchInvokesRegisteredCallback) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    ecuio_frame_t frame{};
    frame.data_length = 8;

    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);

    ecuio_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter, 1);

    /* Duplicate registration is deduplicated: one invocation per dispatch */
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    ecuio_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter, 2);  // Still 1 per dispatch, dedup worked

    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, MockTransportDeliversFrames) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);

    std::atomic<int> frame_count{0};
    /* plain function to keep extern "C" signature compatibility */
    static std::atomic<int>* counter_ptr = &frame_count;
    EXPECT_EQ(ecuio_register_frame_callback(
        session,
        [](const ecuio_frame_t*, void*) { (*counter_ptr)++; },
        nullptr), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* CONTRACT: at least 3 frames eventually arrive. The 50ms tick
     * implies ~4 in 200ms LOCALLY, but CI runners under load deliver
     * fewer (2 in 356 ms has been seen on macOS): poll to the
     * condition instead of betting on a fixed sleep. */
    const bool delivered = wait_for(
        [&frame_count]() { return frame_count.load() >= 3; },
        5000);
    EXPECT_TRUE(delivered) << "mock delivered only "
                           << frame_count.load() << " frames in 5s";
    EXPECT_GE(frame_count.load(), 3);

    /* Close must join the worker without hanging */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Send isn't built yet. A frame whose data_length claims more than
 * data[] holds is the caller's error, INVALID_ARGUMENT; any frame that
 * fits says the call isn't implemented. */
TEST(CoreTest, SessionSendRefusesALengthPastTheFramesData) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &session), ECUIO_OK);
    ecuio_frame_t frame = {};
    frame.data_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX + 1);
    EXPECT_EQ(ecuio_session_send(session, &frame), ECUIO_ERR_INVALID_ARGUMENT);
    frame.data_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX);
    EXPECT_EQ(ecuio_session_send(session, &frame), ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_EQ(ecuio_session_send(session, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(session);
}

TEST(CoreTest, UnknownTransportUriRejected) {
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("j2534://dev0", &session),
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(ecuio_session_create(nullptr, &session),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(CoreTest, CallbackExecutesOnTransportWorkerThread) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://thread-test", &session), ECUIO_OK);

    /* std::atomic<std::thread::id> is lock-free on all 3 CI platforms;
     * mutex alternative also fine. */
    std::atomic<std::thread::id> seen_id{};
    struct Ctx { std::atomic<std::thread::id>* out; };
    static Ctx ctx{&seen_id};  /* static: captureless lambda can't capture */

    EXPECT_EQ(ecuio_register_frame_callback(
        session,
        [](const ecuio_frame_t*, void* ud) {
            static_cast<Ctx*>(ud)->out->store(std::this_thread::get_id());
        },
        &ctx), ECUIO_OK);

    const auto main_id = std::this_thread::get_id();
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Already a poll loop: widened deadline only (400ms -> 2s) to
     * cover starved runners; the shape was right. */
    const bool seen = wait_for(
        [&seen_id]() { return seen_id.load() != std::thread::id{}; },
        2000);
    EXPECT_TRUE(seen);

    EXPECT_NE(seen_id.load(), std::thread::id{});
    EXPECT_NE(seen_id.load(), main_id);  /* Marshaling in bridge is MANDATORY */

    /* A second start leaves the one worker running. */
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

namespace {

struct ReentryCtx {
    ecuio_session_t* session = nullptr;
    std::atomic<int> self_count{0};
    std::atomic<int> other_count{0};
};

/* NAMED function: required so the callback can unregister ITSELF.
 * A captureless lambda cannot refer to its own function pointer. */
void self_unregistering_cb(const ecuio_frame_t*, void* user_data)
{
    ReentryCtx* c = static_cast<ReentryCtx*>(user_data);
    c->self_count.fetch_add(1, std::memory_order_relaxed);
    /* Runs on worker thread while snapshot dispatch is mid-iteration.
     * Touches the callback registry (same mutex being snapshotted).
     * Deadlocks here if the snapshot pattern regresses. */
    ecuio_unregister_frame_callback(c->session, &self_unregistering_cb, user_data);
}

void sibling_counter_cb(const ecuio_frame_t*, void* user_data)
{
    static_cast<ReentryCtx*>(user_data)->other_count
        .fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST(CoreTest, SelfUnregisterDuringLiveDispatch) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://reentry", &session), ECUIO_OK);

    ReentryCtx ctx;   // stack-local is safe: session_close joins the worker
    ctx.session = session;

    EXPECT_EQ(ecuio_register_frame_callback(
        session, &self_unregistering_cb, &ctx), ECUIO_OK);
    EXPECT_EQ(ecuio_register_frame_callback(
        session, &sibling_counter_cb, &ctx), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    /* Poll until the sibling has seen >= 3 frames: that implies the
     * self-callback fired (it dispatched first) and the stream is
     * flowing. Polls to the CONDITION instead of a fixed 250ms. */
    wait_for([&ctx]() { return ctx.other_count.load() >= 3; }, 5000);

    /* Exactly one self-invocation: snapshot allowed THIS dispatch to
     * complete, and the removal took effect for every frame after. */
    EXPECT_EQ(ctx.self_count.load(), 1);
    /* Sibling survived the concurrent removal untouched. */
    EXPECT_GE(ctx.other_count.load(), 3);

    /* Quiesce: close joins worker; must not hang under reentry. */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

namespace {

/* One counter per callback, so a dispatch shows which it reached */
struct Tally {
    int hits[8] = {};
};

void tally_cb(const ecuio_frame_t*, void* user_data)
{
    ++*static_cast<int*>(user_data);
}

/* The first callback unregisters the second and registers a third, on
 * the first frame only */
struct SwapCtx {
    ecuio_session_t* session = nullptr;
    int removed = 0;
    int added = 0;
    bool swapped = false;
};

void removed_cb(const ecuio_frame_t*, void* user_data)
{
    ++static_cast<SwapCtx*>(user_data)->removed;
}

void added_cb(const ecuio_frame_t*, void* user_data)
{
    ++static_cast<SwapCtx*>(user_data)->added;
}

void swapping_cb(const ecuio_frame_t*, void* user_data)
{
    auto* c = static_cast<SwapCtx*>(user_data);
    if (!c->swapped) {
        c->swapped = true;
        ecuio_unregister_frame_callback(c->session, removed_cb, c);
        ecuio_register_frame_callback(c->session, added_cb, c);
    }
}

} // namespace

/* Every callback runs once a frame at any list length, either side of
 * the snapshot's room on the stack */
TEST(CoreTest, DispatchReachesEveryCallbackOnceAtAnyListLength)
{
    for (int n = 1; n <= 8; ++n) {
        SCOPED_TRACE(n);
        ecuio_session_t* session = make_test_session();
        Tally t;
        for (int i = 0; i < n; ++i) {
            EXPECT_EQ(
                ecuio_register_frame_callback(session, tally_cb, &t.hits[i]),
                ECUIO_OK);
        }
        ecuio_frame_t frame{};
        ecuio_session_dispatch_callbacks(session, &frame);
        ecuio_session_dispatch_callbacks(session, &frame);
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(t.hits[i], i < n ? 2 : 0) << "callback " << i;
        }
        ecuio_session_destroy(session);
    }
}

/* A callback that unregisters one and registers another mid-dispatch:
 * that frame still reaches the one removed and not the one added, the
 * next frame the reverse (ADR-0002), on a list that fits the stack and
 * one that doesn't */
TEST(CoreTest, DispatchReachesTheListAsItStoodWhenTheFrameArrived)
{
    for (int fillers : {0, 4}) {
        SCOPED_TRACE(fillers);
        ecuio_session_t* session = make_test_session();
        SwapCtx c;
        c.session = session;
        Tally t;
        EXPECT_EQ(ecuio_register_frame_callback(session, swapping_cb, &c),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(session, removed_cb, &c),
                  ECUIO_OK);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(
                ecuio_register_frame_callback(session, tally_cb, &t.hits[i]),
                ECUIO_OK);
        }
        ecuio_frame_t frame{};
        ecuio_session_dispatch_callbacks(session, &frame);
        EXPECT_EQ(c.removed, 1) << "in the snapshot";
        EXPECT_EQ(c.added, 0) << "not in it";
        ecuio_session_dispatch_callbacks(session, &frame);
        EXPECT_EQ(c.removed, 1);
        EXPECT_EQ(c.added, 1);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(t.hits[i], 2) << "filler " << i;
        }
        ecuio_session_destroy(session);
    }
}

/* A dispatch to up to four callbacks (the app registers one) allocates
 * nothing; a fifth puts the snapshot on the heap, which also shows the
 * counter sees core's allocations */
TEST(CoreTest, DispatchToAShortListAllocatesNothing)
{
    ecuio_session_t* session = make_test_session();
    Tally t;
    ecuio_frame_t frame{};
    for (int n = 1; n <= 5; ++n) {
        EXPECT_EQ(
            ecuio_register_frame_callback(session, tally_cb, &t.hits[n - 1]),
            ECUIO_OK);
        const unsigned long before = new_counter::on_this_thread();
        ecuio_session_dispatch_callbacks(session, &frame);
        const unsigned long news = new_counter::on_this_thread() - before;
        if (n <= 4) {
            EXPECT_EQ(news, 0u) << n << " callbacks";
        } else {
            EXPECT_GT(news, 0u) << "the heap copy past four";
        }
    }
    ecuio_session_destroy(session);
}

namespace {

/* The mock's physical envelope across a whole drive cycle. Shared by
 * the worker test below, which sees only the first second of the
 * cycle, and the deterministic cycle tests, which see all of it. */
constexpr float kMockRpmMin = 0.0f; /* key on, the engine stopped */
constexpr float kMockRpmMax = 7100.0f;
constexpr float kMockCoolantMin = 15.0f;
constexpr float kMockCoolantMax = 110.0f;
constexpr float kMockSpeedMax = 200.0f;
constexpr float kMockIatMin = 15.0f;
constexpr float kMockIatMax = 80.0f;
constexpr float kMockMafMax = 300.0f;   /* WOT near the limiter peaks ~255 */
constexpr float kMockOilMin = 15.0f;    /* cold start reads 20 */
constexpr float kMockOilMax = 140.0f;
constexpr float kMockLambdaMin = 0.8f;  /* WOT enrichment reads ~0.86 */
constexpr float kMockLambdaMax = 2.0f;  /* fuel cut pegs the sensor lean */
constexpr float kMockMapMin = 15.0f;    /* a closed throttle's most vacuum */
constexpr float kMockBaroMin = 95.0f;   /* the outside air near sea level */
constexpr float kMockBaroMax = 105.0f;
/* Boost, MAP minus baro: an NA engine's vacuum, never above the outside
 * air, never under a closed throttle's most vacuum */
constexpr float kMockBoostMin = kMockMapMin - kMockBaroMax;
constexpr float kMockBoostMax = 0.0f;

struct MockCapture {
    static constexpr size_t kMaxFrames = 20;
    uint8_t payloads[kMaxFrames][ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN]{};
    size_t count = 0;
};

/* Named free function: payload capture needs self-reference-free
 * simplicity; user_data points at a stack struct, safe because
 * session_close joins the worker before the stack frame unwinds. */
void capture_payload_cb(const ecuio_frame_t* frame, void* user_data)
{
    MockCapture* cap = static_cast<MockCapture*>(user_data);
    if (cap->count < MockCapture::kMaxFrames) {
        std::memcpy(cap->payloads[cap->count], frame->data, ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN);
        ++cap->count;
    }
}

} // namespace

TEST(CoreTest, MockFramesDecodeAgainstMockSignalSet) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://signals", &session), ECUIO_OK);

    MockCapture cap;
    EXPECT_EQ(ecuio_register_frame_callback(
        session, &capture_payload_cb, &cap), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Collect up to kMaxFrames, polling to the condition. Was a fixed
     * 1400ms sleep; under starvation fewer than the nominal 28 frames
     * would arrive and the >=5 floor got uncomfortably close. Now we
     * simply leave as soon as the buffer is full. */
    wait_for([&cap]() { return cap.count >= MockCapture::kMaxFrames; },
             10000);

    /* Close joins the worker; capture struct outlives dispatch safely.
     * Destroy here, before the ASSERTs below can return early. */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count, size_t{5}) << "expected several frames from mock";

    /* Encoder/decoder round-trip contract: every frame decodes under
     * every mock definition with physically plausible results. A
     * byte-order or scale inversion passes structural checks but
     * produces absurd physics: the range assertions catch that. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);

    for (size_t f = 0; f < cap.count; ++f) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
                continue; /* no field: MockBoostIsMapMinusBaro */
            }
            double val = 0.0;
            ASSERT_EQ(ecuio_decode_signal(&def, cap.payloads[f], ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &val), 0)
                << "frame " << f << ", signal " << def.name;

            switch (def.id) {
            case ECUIO_MOCK_SIG_RPM_ID:
                EXPECT_GE(val, kMockRpmMin);  EXPECT_LE(val, kMockRpmMax); break;
            case ECUIO_MOCK_SIG_COOLANT_ID:
                EXPECT_GE(val, kMockCoolantMin);
                EXPECT_LE(val, kMockCoolantMax); break;
            case ECUIO_MOCK_SIG_SPEED_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, kMockSpeedMax); break;
            case ECUIO_MOCK_SIG_THROTTLE_ID:
            case ECUIO_MOCK_SIG_LOAD_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, 100.0f); break;
            case ECUIO_MOCK_SIG_IAT_ID:
                EXPECT_GE(val, kMockIatMin);  EXPECT_LE(val, kMockIatMax); break;
            case ECUIO_MOCK_SIG_MAF_ID:
                EXPECT_GE(val, 0.0f);          EXPECT_LE(val, kMockMafMax); break;
            case ECUIO_MOCK_SIG_OIL_ID:
                EXPECT_GE(val, kMockOilMin);    EXPECT_LE(val, kMockOilMax); break;
            case ECUIO_MOCK_SIG_LAMBDA_ID:
                EXPECT_GE(val, kMockLambdaMin); EXPECT_LE(val, kMockLambdaMax); break;
            case ECUIO_MOCK_SIG_MAP_ID:
                EXPECT_GE(val, kMockMapMin);
                EXPECT_LE(val, kMockBaroMax);
                break;
            case ECUIO_MOCK_SIG_BARO_ID:
                EXPECT_GE(val, kMockBaroMin);
                EXPECT_LE(val, kMockBaroMax);
                break;
            default:
                FAIL() << "unknown signal id in mock set: " << def.id;
            }
        }
    }
}

namespace {

/* Each batch's MAP, baro and boost, for the first kMax batches that
 * carry boost */
struct BoostCapture {
    static constexpr size_t kMax = 20;
    double map[kMax]{};
    double baro[kMax]{};
    double boost[kMax]{};
    size_t count = 0;
};

void capture_boost_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* cap = static_cast<BoostCapture*>(user_data);
    if (cap->count >= BoostCapture::kMax) {
        return;
    }
    double map = -1.0, baro = -1.0, boost = 0.0;
    bool has_boost = false;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (v.signal_id == ECUIO_MOCK_SIG_MAP_ID)
            map = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BARO_ID)
            baro = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BOOST_ID) {
            boost = v.value;
            has_boost = true;
        }
    }
    if (has_boost) {
        cap->map[cap->count] = map;
        cap->baro[cap->count] = baro;
        cap->boost[cap->count] = boost;
        ++cap->count;
    }
}

/* Every batch's boost against its MAP and baro, counted */
struct BoostTally {
    size_t batches = 0;
    size_t wrong = 0; /* boost isn't MAP minus baro */
    double lowest = 1e9;
    double highest = -1e9;
};

void tally_boost_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* t = static_cast<BoostTally*>(user_data);
    double map = -1.0, baro = -1.0, boost = 0.0;
    bool has_boost = false;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (v.signal_id == ECUIO_MOCK_SIG_MAP_ID)
            map = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BARO_ID)
            baro = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BOOST_ID) {
            boost = v.value;
            has_boost = true;
        }
    }
    if (!has_boost) {
        return;
    }
    ++t->batches;
    if (map < 0.0 || baro < 0.0 || boost != map - baro) {
        ++t->wrong;
    }
    t->lowest = std::fmin(t->lowest, boost);
    t->highest = std::fmax(t->highest, boost);
}

} // namespace

/* The mock's computed signal (ADR-0051 section 5) through the worker:
 * every frame carries MAP and baro, so every batch carries boost, the
 * one minus the other, within the model's bounds */
TEST(CoreTest, MockBoostIsMapMinusBaro)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://boost", &session), ECUIO_OK);
    BoostCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, &capture_boost_cb, &cap),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&cap]() { return cap.count >= BoostCapture::kMax; }, 10000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count, size_t{5}) << "expected boost in several batches";
    for (size_t i = 0; i < cap.count; ++i) {
        ASSERT_GE(cap.map[i], 0.0) << "a batch with boost but no MAP, " << i;
        ASSERT_GE(cap.baro[i], 0.0) << "a batch with boost but no baro, " << i;
        EXPECT_DOUBLE_EQ(cap.boost[i], cap.map[i] - cap.baro[i])
            << "batch " << i;
        EXPECT_GE(cap.boost[i], kMockBoostMin) << "batch " << i;
        EXPECT_LE(cap.boost[i], kMockBoostMax) << "batch " << i;
    }

    /* The worker's first frames are all key on, MAP and baro one value,
     * so a whole loop, the engine running, goes through a session's own
     * dispatch, every tick a frame */
    ecuio_session_t* fed = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://boost-fed", &fed), ECUIO_OK);
    BoostTally tally;
    EXPECT_EQ(ecuio_register_values_callback(fed, &tally_boost_cb, &tally),
              ECUIO_OK);
    auto frame = std::make_unique<ecuio_frame_t>(); /* 4 KB: not on the stack */
    frame->arbitration_id = ECUIO_MOCK_TELEMETRY_ARB_ID;
    frame->flags = ECUIO_FRAME_FLAG_FD;
    frame->data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    ecuio_mock::DriveCycle dc;
    size_t ticks = 0;
    while (dc.loops < 1 && ticks < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ++ticks;
        frame->timestamp_us = ticks * 50000u;
        ecuio_mock::encode_payload(dc, frame->data);
        ecuio_session_dispatch_callbacks(fed, frame.get());
    }
    ecuio_session_destroy(fed);
    ASSERT_EQ(dc.loops, 1u) << "the loop never completed";
    EXPECT_EQ(tally.batches, ticks) << "a frame without boost";
    EXPECT_EQ(tally.wrong, 0u) << "boost that isn't MAP minus baro";
    EXPECT_LT(tally.lowest, -50.0) << "no running engine's vacuum";
    EXPECT_GE(tally.lowest, kMockBoostMin);
    EXPECT_LE(tally.highest, kMockBoostMax);
}

// ---------------------------------------------------------------------------
// Mock drive cycle: stepped deterministically, no worker, no wall clock
// ---------------------------------------------------------------------------

namespace {

using ecuio_mock::DriveCycle;
using ecuio_mock::Phase;

constexpr int kPhaseCount = static_cast<int>(Phase::Cooldown) + 1;

struct PhaseStats {
    uint32_t ticks = 0;
    float entry_rpm = 0.0f;    /* after the phase's first tick */
    float entry_kmh = 0.0f;
    float min_rpm = 1e9f;
    float peak_rpm = 0.0f;
    float peak_kmh = 0.0f;
    float peak_coolant = -100.0f;
    float peak_iat = -100.0f;
    float peak_oil = -100.0f;
    int max_gear = 0;
};

struct LoopRun {
    PhaseStats phase[kPhaseCount];
    std::vector<Phase> order;  /* phases in visit order */
    bool closed = false;       /* looped back to KeyOn */
    DriveCycle end;
};

const PhaseStats& stats(const LoopRun& r, Phase p)
{
    return r.phase[static_cast<int>(p)];
}

/* Step one loop, KeyOn through Cooldown, stopping at the tick that
 * returns to KeyOn. Bounded at 10 simulated minutes. */
LoopRun run_one_loop()
{
    LoopRun r;
    DriveCycle dc;
    PhaseStats* s = nullptr;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f); ++i) {
        dc.step();
        if (dc.loops > 0) {
            r.closed = true;
            break;
        }
        if (r.order.empty() || r.order.back() != dc.phase) {
            r.order.push_back(dc.phase);
            s = &r.phase[static_cast<int>(dc.phase)];
            s->entry_rpm = dc.rpm;
            s->entry_kmh = dc.speed_kmh;
        }
        ++s->ticks;
        s->min_rpm = std::fmin(s->min_rpm, dc.rpm);
        s->peak_rpm = std::fmax(s->peak_rpm, dc.rpm);
        s->peak_kmh = std::fmax(s->peak_kmh, dc.speed_kmh);
        s->peak_coolant = std::fmax(s->peak_coolant, dc.coolant_c);
        s->peak_iat = std::fmax(s->peak_iat, dc.iat_c);
        s->peak_oil = std::fmax(s->peak_oil, dc.oil_c);
        if (dc.gear > s->max_gear) {
            s->max_gear = dc.gear;
        }
    }
    r.end = dc;
    return r;
}

} // namespace

TEST(CoreTest, MockDriveCycleVisitsEveryPhaseInOrder)
{
    const LoopRun r = run_one_loop();
    ASSERT_TRUE(r.closed) << "the cycle never looped back to KeyOn";
    ASSERT_EQ(r.order.size(), static_cast<size_t>(kPhaseCount));
    for (int i = 0; i < kPhaseCount; ++i) {
        EXPECT_EQ(static_cast<int>(r.order[static_cast<size_t>(i)]), i)
            << "visit " << i;
    }
    EXPECT_EQ(r.end.phase, Phase::KeyOn);
}

TEST(CoreTest, MockDriveCycleSecondGearPullHoldsTheLimiter)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& pull = stats(r, Phase::Pull2nd);
    const PhaseStats& lim = stats(r, Phase::Limiter2nd);

    /* From low speed and rpm, in 2nd, to the limiter */
    EXPECT_EQ(pull.max_gear, 2);
    EXPECT_LT(pull.entry_rpm, 2000.0f);
    EXPECT_LT(pull.entry_kmh, 35.0f);
    EXPECT_LE(pull.peak_rpm, ecuio_mock::kRevLimitRpm);

    /* Exactly 5 s on the limiter: past it on first contact, before the
     * first cut lands, then sawing just under it */
    EXPECT_EQ(lim.ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(lim.min_rpm, ecuio_mock::kRevLimitRpm - 200.0f);
    EXPECT_GT(lim.peak_rpm, ecuio_mock::kRevLimitRpm);
    EXPECT_LE(lim.peak_rpm, ecuio_mock::kRevLimitRpm + 100.0f);
    EXPECT_NEAR(lim.peak_kmh, 113.0f, 5.0f);

    /* Past either end of its table the torque curve holds that end */
    EXPECT_EQ(ecuio_mock::torque_nm(ecuio_mock::kRevLimitRpm + 500.0f),
              ecuio_mock::torque_nm(ecuio_mock::kRevLimitRpm));
    EXPECT_EQ(ecuio_mock::torque_nm(-100.0f), 0.0f);
}

TEST(CoreTest, MockDriveCycleThirdGearPullIsLongerFasterAndHotter)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& pull2 = stats(r, Phase::Pull2nd);
    const PhaseStats& lim2 = stats(r, Phase::Limiter2nd);
    const PhaseStats& pull3 = stats(r, Phase::Pull3rd);
    const PhaseStats& lim3 = stats(r, Phase::Limiter3rd);

    EXPECT_EQ(pull3.max_gear, 3);
    EXPECT_LE(pull3.peak_rpm, ecuio_mock::kRevLimitRpm);
    EXPECT_LT(pull3.entry_rpm, 2200.0f);
    EXPECT_GT(pull3.entry_kmh, pull2.entry_kmh + 10.0f);
    EXPECT_GT(pull3.ticks, pull2.ticks * 3 / 2) << "3rd should pull longer";

    EXPECT_EQ(lim3.ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(lim3.peak_kmh, lim2.peak_kmh + 40.0f);

    const float coolant2 = std::fmax(pull2.peak_coolant, lim2.peak_coolant);
    const float coolant3 = std::fmax(pull3.peak_coolant, lim3.peak_coolant);
    const float oil2 = std::fmax(pull2.peak_oil, lim2.peak_oil);
    const float oil3 = std::fmax(pull3.peak_oil, lim3.peak_oil);
    const float iat3 = std::fmax(pull3.peak_iat, lim3.peak_iat);
    /* The longer pull puts more heat into the cold engine. The intake
     * air doesn't follow: with no turbo, and ram air at speed, it stays
     * near ambient. */
    EXPECT_GT(coolant3, coolant2 + 4.0f);
    EXPECT_GT(oil3, oil2 + 4.0f);
    EXPECT_LE(iat3, ecuio_mock::kAmbientC + 10.0f);
}

TEST(CoreTest, MockDriveCycleDragRunCoversAQuarterMile)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& launch = stats(r, Phase::Launch);
    const PhaseStats& run = stats(r, Phase::DragRun);

    /* Launch control from a standstill */
    EXPECT_EQ(launch.peak_kmh, 0.0f);
    EXPECT_LE(launch.peak_rpm, ecuio_mock::kLaunchRpm);
    EXPECT_GE(launch.peak_rpm, ecuio_mock::kLaunchRpm - 150.0f);

    /* Through the gears, shifting before the limiter */
    EXPECT_EQ(run.max_gear, ecuio_mock::kTopGear);
    EXPECT_LT(run.peak_rpm, ecuio_mock::kRevLimitRpm);

    /* The run ends on the tick that crosses the line */
    EXPECT_GE(r.end.distance_m, ecuio_mock::kQuarterMileM);
    EXPECT_LT(r.end.distance_m, ecuio_mock::kQuarterMileM + 5.0f);
    EXPECT_GE(r.end.drag_et_s, 11.0f);
    EXPECT_LE(r.end.drag_et_s, 15.0f);
    EXPECT_GE(r.end.drag_trap_kmh, 150.0f);
    EXPECT_LE(r.end.drag_trap_kmh, 200.0f);
}

TEST(CoreTest, MockDriveCycleLaterLoopsRunAtOperatingTemperature)
{
    /* Only the first loop starts cold; each later one picks up where the
     * last ended. The first loop's runs stay on a cold engine. From the
     * third the coolant holds the thermostat's range, and by the last
     * loop here it climbs past 210 F at WOT until the fan cuts in, with
     * the oil built up to 226-243 F. */
    constexpr uint32_t kLoops = 6;
    struct Range {
        float lo = 1e9f, hi = -1e9f;
        void add(float v)
        {
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
    };
    Range coolant[kLoops];
    Range oil[kLoops];
    bool restarted_cold = false;

    /* The cold first loop */
    float first_pull2_coolant = -1e9f;
    float first_end_coolant = 0.0f, first_end_oil = 0.0f;

    /* The hot last loop: coolant at cruise and at WOT (the pulls, their
     * limiters and the quarter mile), the fan, and the oil across the
     * quarter mile and the stop after it */
    Range hot_cruise_coolant, hot_wot_coolant, hot_drag_oil;
    float hot_drag_oil_entry = 0.0f;
    bool hot_fan = false;
    float hot_end_coolant = 0.0f, hot_end_oil = 0.0f;

    DriveCycle dc;
    while (dc.loops < kLoops && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        if (dc.loops >= kLoops) {
            break;
        }
        const uint32_t i = dc.loops;
        coolant[i].add(dc.coolant_c);
        oil[i].add(dc.oil_c);
        restarted_cold = restarted_cold || (i > 0 && dc.phase == Phase::WarmUp);
        if (i == 0) {
            if (dc.phase == Phase::Pull2nd) {
                first_pull2_coolant =
                    std::fmax(first_pull2_coolant, dc.coolant_c);
            }
            first_end_coolant = dc.coolant_c;
            first_end_oil = dc.oil_c;
        } else if (i == kLoops - 1) {
            const Phase p = dc.phase;
            if (p == Phase::Cruise4th) {
                hot_cruise_coolant.add(dc.coolant_c);
            }
            if (p == Phase::Pull2nd || p == Phase::Limiter2nd ||
                p == Phase::Pull3rd || p == Phase::Limiter3rd ||
                p == Phase::DragRun) {
                hot_wot_coolant.add(dc.coolant_c);
            }
            if (p == Phase::DragRun || p == Phase::Shutdown) {
                if (hot_drag_oil.hi < -1e8f) {
                    hot_drag_oil_entry = dc.oil_c;
                }
                hot_drag_oil.add(dc.oil_c);
            }
            hot_fan = hot_fan || dc.fan_on;
            hot_end_coolant = dc.coolant_c;
            hot_end_oil = dc.oil_c;
        }
    }
    ASSERT_EQ(dc.loops, kLoops) << "six loops never completed";
    EXPECT_FALSE(restarted_cold) << "a later loop ran the cold start";

    /* Cold first loop: the thermostat never opens, and the oil trails
     * the coolant as the sump's mass warms */
    EXPECT_LT(first_pull2_coolant, 40.0f);
    EXPECT_LT(coolant[0].hi, ecuio_mock::kThermostatOpenC);
    EXPECT_LT(first_end_oil, first_end_coolant);

    /* From the third loop on, the coolant never falls below the
     * thermostat's range */
    for (uint32_t i = 2; i < kLoops; ++i) {
        EXPECT_GE(coolant[i].lo, 85.0f) << "loop " << i + 1;
    }

    /* Hot: the thermostat holds the cruise, the radiator can't keep up
     * at WOT, and the fan cuts in */
    EXPECT_GE(hot_cruise_coolant.lo, 85.0f);
    EXPECT_LE(hot_cruise_coolant.hi, 97.0f);
    EXPECT_GE(hot_wot_coolant.hi, 100.0f) << "the radiator kept up at WOT";
    EXPECT_LE(hot_wot_coolant.hi, 105.0f);
    EXPECT_TRUE(hot_fan) << "the fan never cut in";
    EXPECT_GE(oil[kLoops - 1].lo, 100.0f);
    EXPECT_GE(oil[kLoops - 1].hi, 112.0f);
    EXPECT_LE(oil[kLoops - 1].hi, 125.0f);
    /* The sump's mass: a quarter mile moves the oil several degrees, not
     * the coolant's swing */
    EXPECT_GE(hot_drag_oil.hi - hot_drag_oil_entry, 2.0f);
    EXPECT_LE(hot_drag_oil.hi - hot_drag_oil_entry, 9.0f);
    EXPECT_GT(hot_end_oil, hot_end_coolant)
        << "the oil cooled below the coolant";
}

TEST(CoreTest, MockDriveCycleEveryTickRoundTripsThroughTheMockSet)
{
    /* Three loops: the cold first one, the one that warms through the
     * thermostat, and a hot one. Every tick goes through the real
     * encoder and decoder. A value the wire can't carry would clamp in
     * the encoder and show up as a large error. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);

    /* Per-signal accumulators, indexed like set->defs. */
    constexpr uint32_t kMaxDefs = 16;
    ASSERT_LE(set->count, kMaxDefs);
    double worst_err[kMaxDefs] = {};
    double lo[kMaxDefs];
    double hi[kMaxDefs];
    for (uint32_t i = 0; i < kMaxDefs; ++i) {
        lo[i] = 1e9;
        hi[i] = -1e9;
    }

    DriveCycle dc;
    uint32_t steps = 0;
    double boost_lo = 1e9, boost_hi = -1e9;
    uint32_t padded = 0;
    while (dc.loops < 3 && steps < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ++steps;
        uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN];
        ecuio_mock::encode_payload(dc, payload);

        double map = 0.0, baro = 0.0;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
                continue; /* boost: from the two below */
            }
            double val = 0.0;
            ASSERT_EQ(ecuio_decode_signal(&def, payload, ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &val), 0)
                << def.name << " at tick " << steps;
            double phys = 0.0;
            switch (def.id) {
            case ECUIO_MOCK_SIG_RPM_ID:       phys = dc.rpm; break;
            case ECUIO_MOCK_SIG_COOLANT_ID:   phys = dc.coolant_c; break;
            case ECUIO_MOCK_SIG_SPEED_ID:     phys = dc.speed_kmh; break;
            case ECUIO_MOCK_SIG_THROTTLE_ID: phys = dc.throttle_pct; break;
            case ECUIO_MOCK_SIG_IAT_ID:       phys = dc.iat_c; break;
            case ECUIO_MOCK_SIG_LOAD_ID:      phys = dc.load_pct; break;
            case ECUIO_MOCK_SIG_MAF_ID:       phys = dc.maf_gs; break;
            case ECUIO_MOCK_SIG_OIL_ID:       phys = dc.oil_c; break;
            case ECUIO_MOCK_SIG_LAMBDA_ID:    phys = dc.lambda; break;
            case ECUIO_MOCK_SIG_MAP_ID:
                phys = dc.map_kpa;
                map = val;
                break;
            case ECUIO_MOCK_SIG_BARO_ID:
                phys = dc.baro_kpa;
                baro = val;
                break;
            default: FAIL() << "unknown signal id in mock set: " << def.id;
            }
            worst_err[i] = std::fmax(worst_err[i], std::fabs(val - phys));
            lo[i] = std::fmin(lo[i], val);
            hi[i] = std::fmax(hi[i], val);
        }
        /* Boost as the wire's two whole kPa give it */
        boost_lo = std::fmin(boost_lo, map - baro);
        boost_hi = std::fmax(boost_hi, map - baro);
        /* The padding to CAN FD's length, zero */
        padded += (payload[14] != 0 || payload[15] != 0) ? 1u : 0u;
    }
    EXPECT_EQ(padded, 0u) << "ticks whose padding wasn't zero";
    ASSERT_EQ(dc.loops, 3u) << "three loops never completed";
    EXPECT_GE(boost_lo, kMockBoostMin);
    EXPECT_LE(boost_hi, kMockBoostMax);

    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
            continue;
        }
        /* Half an LSB of each signal's scale, plus float slack */
        const double tol = 0.5 * std::fabs(def.scale) + 0.01;
        EXPECT_LE(worst_err[i], tol) << def.name << " lost precision or clamped";
        /* Every value the cycle sends inside the def's range, which a
         * gauge's default scale starts from (ADR-0057) */
        double range_lo = 0.0;
        double range_hi = 0.0;
        ASSERT_EQ(ecuio_def_range(&def, &range_lo, &range_hi), ECUIO_DECODE_OK)
            << def.name;
        EXPECT_GE(lo[i], range_lo) << def.name;
        EXPECT_LE(hi[i], range_hi) << def.name;

        switch (def.id) {
        case ECUIO_MOCK_SIG_RPM_ID:
            EXPECT_GE(lo[i], kMockRpmMin);     EXPECT_LE(hi[i], kMockRpmMax); break;
        case ECUIO_MOCK_SIG_COOLANT_ID:
            EXPECT_GE(lo[i], kMockCoolantMin); EXPECT_LE(hi[i], kMockCoolantMax); break;
        case ECUIO_MOCK_SIG_SPEED_ID:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], kMockSpeedMax); break;
        case ECUIO_MOCK_SIG_IAT_ID:
            EXPECT_GE(lo[i], kMockIatMin);     EXPECT_LE(hi[i], kMockIatMax); break;
        case ECUIO_MOCK_SIG_MAF_ID:
            EXPECT_GE(lo[i], 0.0f);    EXPECT_LE(hi[i], kMockMafMax); break;
        case ECUIO_MOCK_SIG_OIL_ID:
            EXPECT_GE(lo[i], kMockOilMin); EXPECT_LE(hi[i], kMockOilMax); break;
        case ECUIO_MOCK_SIG_LAMBDA_ID:
            EXPECT_GE(lo[i], kMockLambdaMin); EXPECT_LE(hi[i], kMockLambdaMax); break;
        case ECUIO_MOCK_SIG_MAP_ID:
            EXPECT_GE(lo[i], kMockMapMin);
            EXPECT_LE(hi[i], kMockBaroMax);
            break;
        case ECUIO_MOCK_SIG_BARO_ID:
            EXPECT_GE(lo[i], kMockBaroMin);
            EXPECT_LE(hi[i], kMockBaroMax);
            break;
        default:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], 100.0f); break;
        }
    }
}

namespace {

/* Steps a fresh cycle until `phase` starts (in the first loop), then
 * calls `visit` on each of its ticks. */
template <typename F>
void for_each_tick_of(Phase phase, F visit)
{
    DriveCycle dc;
    bool in = false;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f) && dc.loops == 0; ++i) {
        dc.step();
        if (dc.phase == phase) {
            in = true;
            visit(dc);
        } else if (in) {
            return;
        }
    }
}

} // namespace

TEST(CoreTest, MockDriveCycleSignalsStayPhysical)
{
    /* Three loops (cold, warming, hot), every tick, against the rules a
     * real ECU's log keeps:
     * rpm locked to wheel speed in gear, no speed at idle rpm with the
     * clutch in, airflow consistent with rpm and load, lambda lean
     * exactly when fuel is cut, and temperatures that move like masses
     * of metal and coolant. */
    DriveCycle dc;
    DriveCycle prev = dc;
    uint32_t cut_run = 0;
    uint32_t fueled_run = 0;
    uint32_t fueled_at_limit = 0;
    int checked_locked = 0;
    while (dc.loops < 3 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        const float k = dc.gear > 0 ? ecuio_mock::rpm_per_mps(dc.gear) : 0.0f;
        const bool limiter = dc.phase == Phase::Limiter2nd || dc.phase == Phase::Limiter3rd;
        const bool locked = dc.gear > 0 && !dc.clutch_slip && !dc.shifting() && !dc.spinning;

        if (locked && !limiter) {
            /* Within the crank reading's firing-pulse jitter */
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 10.0f) << "tick " << dc.tick;
            ++checked_locked;
        } else if (locked) {
            /* The saw is crank speed against the driveline's compliance */
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 200.0f) << "tick " << dc.tick;
        }
        if (dc.gear == 0 && !dc.shifting()) {
            EXPECT_LE(dc.speed_kmh, 50.0f) << "clutch in at speed, tick " << dc.tick;
        }
        if (dc.gear > 0 && !dc.clutch_slip && !dc.shifting()) {
            EXPECT_GE(dc.rpm, 1000.0f) << "lugging in gear " << dc.gear
                                       << ", tick " << dc.tick;
        }
        if (dc.spinning) {
            EXPECT_GE(dc.wheel_mps, dc.v_mps) << "tick " << dc.tick;
            EXPECT_LE(dc.slip(), 0.5f) << "tick " << dc.tick;
        }

        const float estimate = ecuio_mock::maf_wot_gs(dc.rpm) * dc.load_pct / 100.0f;
        EXPECT_NEAR(dc.maf_gs, estimate, 0.1f * estimate + 0.5f) << "tick " << dc.tick;
        if (dc.running && dc.rpm >= 600.0f) {
            EXPECT_GT(dc.maf_gs, 5.0f) << "engine running, tick " << dc.tick;
        }

        /* The manifold: never above the outside air (an NA engine), the
         * outside air itself with the engine stopped, and once it pumps
         * a full vacuum, the residual plus load's share of the outside
         * air, less the intake's loss; near the outside air at WOT, a
         * deep vacuum at idle */
        EXPECT_LE(dc.map_kpa, dc.baro_kpa) << "tick " << dc.tick;
        if (dc.rpm == 0.0f) {
            EXPECT_FLOAT_EQ(dc.map_kpa, dc.baro_kpa)
                << "stopped, tick " << dc.tick;
        }
        if (dc.rpm >= ecuio_mock::kMapPumpRpm) {
            const float fill = ecuio_mock::kMapResidualKpa +
                               (dc.baro_kpa - ecuio_mock::kMapResidualKpa) *
                                   dc.load_pct / 100.0f;
            EXPECT_LE(dc.map_kpa, fill + 0.01f) << "tick " << dc.tick;
            EXPECT_GE(dc.map_kpa, fill - 1.2f * ecuio_mock::kIntakeLossKpa)
                << "tick " << dc.tick;
            EXPECT_GE(dc.map_kpa, kMockMapMin) << "tick " << dc.tick;
        }
        if (dc.load_pct >= 95.0f) {
            EXPECT_GE(dc.map_kpa, 0.9f * dc.baro_kpa)
                << "WOT, tick " << dc.tick;
        }
        if (dc.phase == Phase::Stage) {
            EXPECT_GE(dc.map_kpa, 25.0f) << "idle, tick " << dc.tick;
            EXPECT_LE(dc.map_kpa, 50.0f) << "idle, tick " << dc.tick;
        }
        EXPECT_GE(dc.throttle_pct, ecuio_mock::kTpClosedPct - 0.01f);
        EXPECT_LE(dc.throttle_pct, ecuio_mock::kTpWotPct + 0.01f);

        cut_run = dc.fuel_cut ? cut_run + 1 : 0;
        fueled_run = dc.fuel_cut ? 0 : fueled_run + 1;
        /* The rev limit cuts fuel the tick after rpm meets it, whatever
         * the phase: a lift off the limiter doesn't sit there fueled */
        fueled_at_limit = (!limiter && !dc.fuel_cut && dc.rpm >= ecuio_mock::kLimiterOnRpm)
            ? fueled_at_limit + 1 : 0;
        EXPECT_LE(fueled_at_limit, 1u) << "fueled at the rev limit, tick " << dc.tick;
        if (cut_run >= 2) {
            EXPECT_GT(dc.lambda, 1.3f) << "fuel cut, tick " << dc.tick;
        }
        if (fueled_run >= 3) {
            EXPECT_GE(dc.lambda, 0.8f) << "tick " << dc.tick;
            EXPECT_LE(dc.lambda, 1.1f) << "tick " << dc.tick;
        }

        EXPECT_LE(std::fabs(dc.coolant_c - prev.coolant_c), 0.5f) << "tick " << dc.tick;
        EXPECT_LE(std::fabs(dc.oil_c - prev.oil_c), 0.5f) << "tick " << dc.tick;
        EXPECT_LE(std::fabs(dc.iat_c - prev.iat_c), 0.5f) << "tick " << dc.tick;
        prev = dc;
    }
    ASSERT_EQ(dc.loops, 3u) << "three loops never completed";
    EXPECT_GT(checked_locked, 1000) << "too few in-gear ticks checked";
}

TEST(CoreTest, MockDriveCycleReadingsNeverHoldDeadStill)
{
    /* A running engine's rpm, as the wire carries it, never holds one
     * value for long: the idle hunts, the driver's speed wanders, the
     * crank reading jitters. A stopped one reads 0 (KeyOn). And away
     * from the moments that are quick on a real car (a shift, the
     * limiter, the launch and the drag run), rpm never steps between
     * ticks by more than an engine can change, phase changes, a stop and
     * a start included. */
    DriveCycle dc;
    uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN] = {};
    uint16_t last_raw = 0;
    uint32_t same = 0, longest = 0;
    float prev_rpm = dc.rpm;
    float worst_step = 0.0f;
    uint64_t worst_tick = 0;
    while (dc.loops < 2 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ecuio_mock::encode_payload(dc, payload);
        const uint16_t raw = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
        same = (dc.tick > 1 && raw == last_raw && dc.phase != Phase::KeyOn)
                   ? same + 1
                   : 0;
        longest = std::max(longest, same);
        last_raw = raw;

        const bool quick = dc.shifting() || dc.phase == Phase::Limiter2nd ||
                           dc.phase == Phase::Limiter3rd || dc.phase == Phase::Launch ||
                           dc.phase == Phase::DragRun;
        if (!quick && std::fabs(dc.rpm - prev_rpm) > worst_step) {
            worst_step = std::fabs(dc.rpm - prev_rpm);
            worst_tick = dc.tick;
        }
        prev_rpm = dc.rpm;
    }
    ASSERT_EQ(dc.loops, 2u) << "two loops never completed";
    EXPECT_LE(longest, 10u) << "rpm read the same for over half a second";
    EXPECT_LE(worst_step, 150.0f) << "rpm stepped at tick " << worst_tick;
}

TEST(CoreTest, MockDriveCycleEachLoopStartsWithTheEngineStopped)
{
    /* Every loop starts with 15 s of the key on and the engine stopped,
     * where a clear can run (ADR-0038): from the first tick on the
     * first loop, and once the engine has run down from idle on later
     * ones, it reads 0 rpm with no air, no speed and the sensor lean,
     * and with the pump stopped the warm coolant soaks heat from the
     * block rather than losing it to the radiator. Then the starter
     * cranks it, and it catches and runs at its idle by the end of the
     * start. */
    constexpr uint32_t kLoops = 3;
    uint32_t key_on[kLoops] = {}, stopped[kLoops] = {};
    float crank_hi[kLoops] = {}, start_end_rpm[kLoops] = {};
    float soak_entry[kLoops] = {}, soak_lo[kLoops] = {};
    bool started[kLoops] = {};
    bool ran_with_key_on = false;
    DriveCycle dc;
    EXPECT_EQ(dc.phase, Phase::KeyOn);
    EXPECT_EQ(dc.rpm, 0.0f);
    while (dc.loops < kLoops && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        if (dc.loops >= kLoops) {
            break;
        }
        const uint32_t i = dc.loops;
        if (dc.phase == Phase::KeyOn) {
            if (key_on[i] == 0) {
                soak_entry[i] = soak_lo[i] = dc.coolant_c;
            }
            soak_lo[i] = std::fmin(soak_lo[i], dc.coolant_c);
            ++key_on[i];
            ran_with_key_on = ran_with_key_on || dc.running;
            if (dc.rpm == 0.0f) {
                ++stopped[i];
                EXPECT_EQ(dc.maf_gs, 0.0f) << "tick " << dc.tick;
                EXPECT_EQ(dc.load_pct, 0.0f) << "tick " << dc.tick;
                EXPECT_EQ(dc.speed_kmh, 0.0f) << "tick " << dc.tick;
                EXPECT_GT(dc.lambda, 1.3f) << "tick " << dc.tick;
            }
        } else if (dc.phase == Phase::Start) {
            if (!dc.running) {
                crank_hi[i] = std::fmax(crank_hi[i], dc.rpm);
            }
            start_end_rpm[i] = dc.rpm;
            started[i] = dc.running;
        }
    }
    ASSERT_EQ(dc.loops, kLoops) << "three loops never completed";
    EXPECT_FALSE(ran_with_key_on);
    ASSERT_GT(soak_entry[kLoops - 1], ecuio_mock::kThermostatOpenC);
    EXPECT_GE(soak_lo[kLoops - 1], soak_entry[kLoops - 1] - 0.1f)
        << "the radiator cooled a stopped engine";
    for (uint32_t i = 0; i < kLoops; ++i) {
        SCOPED_TRACE(i + 1);
        EXPECT_EQ(key_on[i], ecuio_mock::kKeyOnTicks);
        EXPECT_GE(stopped[i], ecuio_mock::ticks_for(14.0f))
            << "it ran down within a second";
        EXPECT_GE(crank_hi[i], 180.0f);
        EXPECT_LE(crank_hi[i], 260.0f);
        EXPECT_TRUE(started[i]);
        EXPECT_GE(start_end_rpm[i], 700.0f);
        EXPECT_LE(start_end_rpm[i], 1500.0f);
    }
}

TEST(CoreTest, MockDriveCycleLimiterCutsFuelNotAir)
{
    /* On the limiter the blade stays open, airflow stays near WOT and the
     * fuel is cut most of each cycle: lambda reads lean, load pegged.
     * Road speed holds while rpm saws. */
    float pull_maf = 0.0f;
    for_each_tick_of(Phase::Pull2nd, [&](const DriveCycle& dc) {
        pull_maf = std::fmax(pull_maf, dc.maf_gs);
    });
    float min_load = 100.0f, min_maf = 1e9f, max_lambda = 0.0f;
    float min_kmh = 1e9f, max_kmh = 0.0f;
    uint32_t cut = 0, ticks = 0;
    for_each_tick_of(Phase::Limiter2nd, [&](const DriveCycle& dc) {
        EXPECT_FLOAT_EQ(dc.throttle_pct, ecuio_mock::kTpWotPct);
        min_load = std::fmin(min_load, dc.load_pct);
        min_maf = std::fmin(min_maf, dc.maf_gs);
        max_lambda = std::fmax(max_lambda, dc.lambda);
        if (dc.phase_ticks >= ecuio_mock::ticks_for(1.0f)) {
            min_kmh = std::fmin(min_kmh, dc.speed_kmh);
            max_kmh = std::fmax(max_kmh, dc.speed_kmh);
        }
        cut += dc.fuel_cut ? 1u : 0u;
        ++ticks;
    });
    ASSERT_EQ(ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(min_load, 94.0f) << "near WOT air, less the MAF noise";
    EXPECT_GE(min_maf, 0.9f * pull_maf);
    EXPECT_GT(max_lambda, 1.6f);
    EXPECT_GT(cut, ticks / 2) << "the limiter should spend most of its time in cut";
    EXPECT_LE(max_kmh - min_kmh, 1.0f) << "speed should hold on the limiter";
}

TEST(CoreTest, MockDriveCycleHighwayCruiseHoldsSpeed)
{
    /* A light, steady cruise in top gear: closed-loop fueling, the low
     * load and airflow of road-load power. */
    float lo_kmh = 1e9f, hi_kmh = 0.0f, lo_rpm = 1e9f, hi_rpm = 0.0f;
    float hi_load = 0.0f, hi_maf = 0.0f, lo_lambda = 9.0f, hi_lambda = 0.0f;
    uint32_t ticks = 0;
    for_each_tick_of(Phase::Cruise4th, [&](const DriveCycle& dc) {
        EXPECT_EQ(dc.gear, ecuio_mock::kTopGear);
        if (dc.phase_ticks < ecuio_mock::ticks_for(5.0f)) {
            return;   /* settling from the lift */
        }
        lo_kmh = std::fmin(lo_kmh, dc.speed_kmh);
        hi_kmh = std::fmax(hi_kmh, dc.speed_kmh);
        lo_rpm = std::fmin(lo_rpm, dc.rpm);
        hi_rpm = std::fmax(hi_rpm, dc.rpm);
        hi_load = std::fmax(hi_load, dc.load_pct);
        hi_maf = std::fmax(hi_maf, dc.maf_gs);
        lo_lambda = std::fmin(lo_lambda, dc.lambda);
        hi_lambda = std::fmax(hi_lambda, dc.lambda);
        ++ticks;
    });
    ASSERT_GE(ticks, ecuio_mock::ticks_for(9.0f));
    EXPECT_GE(lo_kmh, 98.0f);
    EXPECT_LE(hi_kmh, 102.0f);
    EXPECT_GE(lo_rpm, 2850.0f);
    EXPECT_LE(hi_rpm, 3100.0f);
    EXPECT_LE(hi_load, 35.0f);
    EXPECT_LE(hi_maf, 35.0f);
    EXPECT_GE(lo_lambda, 0.97f);
    EXPECT_LE(hi_lambda, 1.03f);
}

TEST(CoreTest, MockDriveCycleColdStartIdlesFastThenSettles)
{
    /* The cold-start strategy as the first vehicle showed it: a fast
     * idle near 1430 rpm on extra air for ~20 s, open-loop fueling, then
     * an 850-900 rpm warm-up idle. */
    float early_lo = 1e9f, early_hi = 0.0f, late_rpm = 0.0f, fast_load = 0.0f;
    float fast_tp = 0.0f, lambda_lo = 9.0f, lambda_hi = 0.0f;
    for_each_tick_of(Phase::WarmUp, [&](const DriveCycle& dc) {
        if (dc.phase_ticks < ecuio_mock::ticks_for(15.0f)) {
            early_lo = std::fmin(early_lo, dc.rpm);
            early_hi = std::fmax(early_hi, dc.rpm);
            fast_load = std::fmax(fast_load, dc.load_pct);
            fast_tp = std::fmax(fast_tp, dc.throttle_pct);
        }
        late_rpm = dc.rpm;
        lambda_lo = std::fmin(lambda_lo, dc.lambda);
        lambda_hi = std::fmax(lambda_hi, dc.lambda);
    });
    EXPECT_GE(early_lo, 1400.0f);
    EXPECT_LE(early_hi, 1460.0f);
    EXPECT_GE(fast_load, 40.0f) << "catalyst heating runs on extra air";
    EXPECT_GT(fast_tp, 30.0f);
    EXPECT_GE(late_rpm, 850.0f);
    EXPECT_LE(late_rpm, 900.0f);
    EXPECT_NEAR(lambda_lo, ecuio_mock::kLambdaColdOpenLoop, 0.01f);
    EXPECT_NEAR(lambda_hi, ecuio_mock::kLambdaColdOpenLoop, 0.01f);
}

TEST(CoreTest, MockDriveCycleLaunchSpinsTheTiresAndHooksUp)
{
    /* Off the line the clutch slips while the tires sit near peak slip;
     * once it locks in 1st the driver feathers the throttle with the
     * tires spinning a few hundred rpm above road speed. From 2nd the
     * drive is below the grip and the tires hook up. */
    float flare_hi = 0.0f;
    float slip_hi = 0.0f;
    float clutch_rpm_lo = 1e9f;
    bool spun_in_second = false;
    for_each_tick_of(Phase::DragRun, [&](const DriveCycle& dc) {
        if (dc.clutch_slip) {
            clutch_rpm_lo = std::fmin(clutch_rpm_lo, dc.rpm);
        }
        if (dc.gear == 1 && !dc.clutch_slip && !dc.shifting()) {
            const float ground_rpm = dc.v_mps * ecuio_mock::rpm_per_mps(1);
            flare_hi = std::fmax(flare_hi, dc.rpm - ground_rpm);
            slip_hi = std::fmax(slip_hi, dc.slip());
        }
        if (dc.gear >= 2 && dc.spinning) {
            spun_in_second = true;
        }
    });
    EXPECT_GE(clutch_rpm_lo, 3000.0f) << "the engine bogged off the line";
    EXPECT_GE(flare_hi, 300.0f);
    EXPECT_LE(flare_hi, 800.0f);
    EXPECT_GT(slip_hi, 0.05f);
    EXPECT_LT(slip_hi, 0.3f);
    EXPECT_FALSE(spun_in_second);
}

namespace {

struct ArbIdCapture {
    std::atomic<int> telemetry_count{0};
    std::atomic<int> diag_count{0};
    std::atomic<int> other_count{0};
    std::atomic<int> extended_mismatch{0};
    std::atomic<int> fd_mismatch{0};
    std::atomic<uint64_t> last_ts{0};
    std::atomic<int> ts_regression{0};
};

void arb_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    ArbIdCapture* c = static_cast<ArbIdCapture*>(user_data);
    const uint64_t prev = c->last_ts.exchange(frame->timestamp_us);
    if (frame->timestamp_us < prev) {
        c->ts_regression.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame->arbitration_id == 0x0C0) {
        c->telemetry_count.fetch_add(1, std::memory_order_relaxed);
        if ((frame->flags & ECUIO_FRAME_FLAG_EXTENDED) != 0) {
            c->extended_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
        if ((frame->flags & ECUIO_FRAME_FLAG_FD) == 0) {
            c->fd_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (frame->arbitration_id == 0x18FF0C00) {
        c->diag_count.fetch_add(1, std::memory_order_relaxed);
        if ((frame->flags & ECUIO_FRAME_FLAG_EXTENDED) == 0) {
            c->extended_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
        if ((frame->flags & ECUIO_FRAME_FLAG_FD) == 0) {
            c->fd_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        c->other_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

/* Contract: the mock emits exactly two arbitration IDs, the
 * telemetry ID the signal set declares (0x0C0) and a 29-bit diagnostic
 * ID every 16th frame: with FLAG_EXTENDED set iff the ID is 29-bit.
 * Combined with MockSignalsDeclareTelemetryMessageId (test_signals),
 * this pins the two halves of the bridge's decode gate; the gate line
 * itself is exercised end-to-end by MockFramesDecodeAgainstMockSignalSet
 * continuing to produce sane values from 0x0C0 traffic only. */
TEST(CoreTest, MockEmitsKnownArbitrationIds)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://arb", &session), ECUIO_OK);

    ArbIdCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, &arb_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Enough ticks for several 16-frame cycles incl. diag frames */
    wait_for([&cap]() {
        return cap.diag_count.load() >= 2 && cap.telemetry_count.load() >= 30;
    }, 10000);

    ecuio_session_close(session);
    ecuio_session_destroy(session);

    EXPECT_EQ(cap.other_count.load(), 0);
    EXPECT_EQ(cap.extended_mismatch.load(), 0);
    EXPECT_GE(cap.diag_count.load(), 2);
    EXPECT_GT(cap.telemetry_count.load(), cap.diag_count.load() * 4);
    /* Session-relative timestamps must never regress (header contract) */
    EXPECT_EQ(cap.ts_regression.load(), 0);
    /* First sample lands within the first tick, not microseconds since
     * boot: session-relative, not absolute. */
    EXPECT_LT(cap.last_ts.load(), uint64_t{600} * 1000 * 1000 * 1000);
    EXPECT_EQ(cap.fd_mismatch.load(), 0) << "FD flag missing on one or more frames";
}

namespace {

struct TickCapture {
    std::atomic<int> count{0};
    uint64_t ts[96] = {}; /* read after close, which joins the worker */
};

void tick_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    TickCapture* c = static_cast<TickCapture*>(user_data);
    const int i = c->count.fetch_add(1);
    if (i < 96) {
        c->ts[i] = frame->timestamp_us;
    }
}

} // namespace

/* Contract: one frame per kTickS of wall time. The drive cycle advances
 * kTickS per frame, so a slower worker plays the car in slow motion
 * against the frames' timestamps (a 0-100 time, the limiter's period).
 *
 * The worker sleeps to a deadline (due += interval) and restarts the
 * schedule only when a wake comes more than an interval late. So between
 * restarts every frame is sent between its due time and one interval
 * past it, and a run of k gaps sums to 50 k ms within one interval,
 * whatever each gap is: on Windows they land on the 15.6 ms sleep step
 * (about 47 or 62 ms). A stall, a gap over two intervals, is left out
 * of the mean. Every restart sends its next frame at once, a gap under
 * half an interval, and each stall or restart moves the sum by less than
 * an interval. Counting stalls and short gaps as e events, the mean of n
 * gaps is 50 ms within 50 (e + 1) / n, plus a millisecond for the
 * clock.
 *
 * Sleeping a fixed 50 ms after each tick makes every gap the long one:
 * 62 ms on Windows, which fails this at once. On Linux a fixed sleep
 * overshoots by a fraction of a millisecond, which no 4 s run can tell
 * from a deadline. A host that stalls so often that the bound can't see
 * a worker 10 % slow skips, and ctest reports the skip as one: the
 * macos-build runner wakes 50 ms sleeps 100-200 ms late, a third of the
 * gaps at a time. */
TEST(CoreTest, MockFramesKeepTheTickInterval)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://cadence", &session), ECUIO_OK);

    TickCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, &tick_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    constexpr int kFrames = 81; /* 80 gaps, 4 s */
    wait_for([&]() { return cap.count.load() >= kFrames; }, 20000);

    ecuio_session_close(session);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count.load(), kFrames);
    double sum_ms = 0.0;
    int counted = 0;
    int stalls = 0;
    int events = 0; /* stalls, and gaps under half an interval */
    std::string gaps; /* every gap, for a failure on a host we can't run */
    for (int i = 1; i < kFrames; ++i) {
        const double gap_ms =
            static_cast<double>(cap.ts[i] - cap.ts[i - 1]) / 1000.0;
        gaps += std::to_string(static_cast<int>(gap_ms)) + " ";
        if (gap_ms <= 100.0) {
            sum_ms += gap_ms;
            ++counted;
            if (gap_ms < 25.0)
                ++events;
        } else {
            ++stalls;
            ++events;
        }
    }
    ASSERT_GT(counted, 0) << "gaps (ms): " << gaps;
    const double bound_ms = 50.0 * (events + 1) / counted + 1.0;
    if (bound_ms >= 5.0) {
        GTEST_SKIP() << "this host stalled the worker " << stalls
                     << " times (" << events << " stalls and restarts), "
                        "so the bound (+/- "
                     << bound_ms << " ms) can't see a worker 10 % slow; "
                        "gaps (ms): " << gaps;
    }
    const double mean_ms = sum_ms / counted;
    EXPECT_GE(mean_ms, 50.0 - bound_ms) << "gaps (ms): " << gaps;
    EXPECT_LE(mean_ms, 50.0 + bound_ms)
        << "the drive cycle would play slower than real time; gaps (ms): "
        << gaps;
}

/* Session time's clock (ADR-0033 section 1) never goes back, nor does its slept
 * count, and while the machine is awake it keeps the waits' pace */
// ---------------------------------------------------------------------------
// A requesting worker's waits (core/src/precise_wait.h)
// ---------------------------------------------------------------------------

namespace {

/* The shortest of n waits of d on w, each timed on the steady clock, and
 * their mean, in microseconds; each must be d or more */
struct Waited {
    int64_t shortest_us = INT64_MAX;
    int64_t mean_us = 0;
    int64_t median_us = 0; /* what a few preempted waits don't move */
};

Waited wait_n(ecuio_precise_wait& w, std::chrono::microseconds d, int n)
{
    Waited out;
    int64_t total = 0;
    std::vector<int64_t> each;
    for (int i = 0; i < n; ++i) {
        const auto from = std::chrono::steady_clock::now();
        w.sleep_for(d);
        const int64_t us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - from)
                .count();
        out.shortest_us = std::min(out.shortest_us, us);
        total += us;
        each.push_back(us);
    }
    out.mean_us = total / n;
    std::sort(each.begin(), each.end());
    out.median_us = each[each.size() / 2];
    return out;
}

} // namespace

/* On Windows the waits run on a high-resolution timer (10 1803 and
 * later), which ends a 2 ms wait about on time where a plain sleep ends
 * it at the 15.6 ms step; elsewhere a plain sleep, on time on Linux.
 * Never shorter than asked either way. The median's bound is a quarter
 * of the step, a timer that rounds to it fails it. Not on macOS: CI's
 * runners end a 2 ms sleep 10 to 18 ms late, a plain sleep core can't
 * shorten, so there only the lower bound is judged. */
TEST(CoreTest, APreciseWaitEndsOnTime)
{
    ecuio_precise_wait w;
#ifdef _WIN32
    ASSERT_TRUE(w.high_resolution())
        << "CreateWaitableTimerExW refused the flag";
#endif
    const Waited got = wait_n(w, std::chrono::milliseconds(2), 20);
    EXPECT_GE(got.shortest_us, 2000) << "a wait ended early";
#ifndef __APPLE__
    EXPECT_LT(got.median_us, 4000) << "the waits ended on a timer's step";
#endif
    RecordProperty("mean_us", static_cast<int>(got.mean_us));
    RecordProperty("median_us", static_cast<int>(got.median_us));
}

/* A wait until a time ends at a stop within a piece: a second's wait
 * stopped 20 ms in returns long before the second (judged against half
 * of it, which load stretches only toward the stop), and one never
 * stopped waits it out, never ending early */
TEST(CoreTest, APreciseWaitUntilEndsAtAStop)
{
    ecuio_precise_wait w;
    std::atomic<bool> stop{false};
    const auto from = std::chrono::steady_clock::now();
    std::thread stopper([&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true);
    });
    const bool stopped = w.sleep_until(from + std::chrono::seconds(1), stop);
    const auto took = std::chrono::steady_clock::now() - from;
    stopper.join();
    EXPECT_TRUE(stopped);
    EXPECT_LT(took, std::chrono::milliseconds(500))
        << "the stop waited the second out";

    std::atomic<bool> never{false};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(w.sleep_until(start + std::chrono::milliseconds(50), never));
    EXPECT_GE(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds(50))
        << "a wait ended early";
    EXPECT_TRUE(w.sleep_until(start, stop))
        << "a stop already set ends it at once";
}

/* Where the flag is refused (built here by asking for none), the wait is
 * a plain sleep: at the system's step, never shorter than asked, and
 * nothing for a wait of nothing */
TEST(CoreTest, APreciseWaitsFallbackNeverEndsEarly)
{
    ecuio_precise_wait w(false);
    EXPECT_FALSE(w.high_resolution());
    const Waited got = wait_n(w, std::chrono::milliseconds(2), 5);
    EXPECT_GE(got.shortest_us, 2000) << "a wait ended early";
    RecordProperty("mean_us", static_cast<int>(got.mean_us));
    const auto from = std::chrono::steady_clock::now();
    w.sleep_for(std::chrono::microseconds(0));
    w.sleep_for(std::chrono::microseconds(-5));
    EXPECT_LT(std::chrono::steady_clock::now() - from,
              std::chrono::milliseconds(1));
}

TEST(CoreTest, ClockNeverGoesBackAndKeepsPace)
{
    uint64_t last = ecuio_clock_us();
    uint64_t slept = ecuio_clock_slept_us();
    for (int i = 0; i < 10000; ++i) {
        const uint64_t now = ecuio_clock_us();
        ASSERT_GE(now, last);
        last = now;
        const uint64_t s = ecuio_clock_slept_us();
        ASSERT_GE(s, slept);
        slept = s;
    }
    const uint64_t c0 = ecuio_clock_us();
    const auto s0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t elapsed = ecuio_clock_us() - c0;
    const auto steady = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - s0)
                            .count();
    EXPECT_GE(elapsed, 50000u);
    EXPECT_NEAR(static_cast<double>(elapsed), static_cast<double>(steady),
                1000.0);
}

/* It is the platform's clock that counts sleep, read between two reads of
 * that clock. Where that clock and the one that stops in sleep agree (a
 * machine that hasn't slept since it started, as a CI runner), this can't
 * tell them apart; a real sleep is the bench's (BENCH prep #27) */
TEST(CoreTest, ClockIsThePlatformsSleepCountingClock)
{
#if defined(__linux__)
    const auto platform = []() {
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
               static_cast<uint64_t>(ts.tv_nsec) / 1000u;
    };
    const uint64_t slack = 0;
#elif defined(__APPLE__)
    const auto platform = []() {
        return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000u;
    };
    const uint64_t slack = 0;
#else
    /* MSVC's steady_clock is QueryPerformanceCounter, to nanoseconds by
     * its own rounding: a microsecond either way */
    const auto platform = []() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    const uint64_t slack = 1;
#endif
    const uint64_t before = platform();
    const uint64_t clock = ecuio_clock_us();
    const uint64_t after = platform();
    EXPECT_LE(before, clock + slack);
    EXPECT_LE(clock, after + slack);
}

namespace {

struct ClockCapture {
    std::atomic<int> count{0};
    uint64_t stamp[8] = {}; /* read after close, which joins the worker */
    uint64_t clock[8] = {};
};

void clock_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    ClockCapture* c = static_cast<ClockCapture*>(user_data);
    const int i = c->count.fetch_add(1);
    if (i < 8) {
        c->clock[i] = ecuio_clock_us();
        c->stamp[i] = frame->timestamp_us;
    }
}

} // namespace

/* A session's time is ecuio_clock_us since its attach, the clock the app
 * stamps a recording's host_us with: each frame's clock less its stamp
 * lands between the reads around create, give or take its dispatch */
TEST(CoreTest, SessionTimeRunsOnTheClock)
{
    ecuio_session_t* session = nullptr;
    const uint64_t before = ecuio_clock_us();
    ASSERT_EQ(ecuio_session_create("mock://clock", &session), ECUIO_OK);
    const uint64_t after = ecuio_clock_us();
    ClockCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, &clock_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&]() { return cap.count.load() >= 8; }, 10000);
    ecuio_session_close(session);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count.load(), 8);
    for (int i = 0; i < 8; ++i) {
        const uint64_t epoch = cap.clock[i] - cap.stamp[i];
        EXPECT_GE(epoch, before) << "frame " << i;
        EXPECT_LE(epoch, after + 20000) << "frame " << i;
    }
}

TEST(CoreTest, CloseIsIdempotentAndDestroyIsSafe)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://lifecycle", &session), ECUIO_OK);

    /* Double close: second returns the error code WITHOUT UB. close()
     * must not free the session (destroy does), or this second call is
     * a use-after-free that only survives on allocator luck */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_ERR_SESSION_CLOSED);

    /* Post-close API calls fail cleanly, not UB */
    EXPECT_EQ(ecuio_session_start(session), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, nullptr), ECUIO_ERR_SESSION_CLOSED);
    ecuio_frame_t frame = {};
    EXPECT_EQ(ecuio_session_send(session, &frame), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(ecuio_session_receive(session, &frame, 0),
              ECUIO_ERR_NOT_IMPLEMENTED);

    /* destroy() is the freeing call; handle invalid after (free()). */
    EXPECT_EQ(ecuio_session_destroy(session), ECUIO_OK);

    /* free(nullptr) semantics */
    EXPECT_EQ(ecuio_session_destroy(nullptr), ECUIO_OK);

    /* Every other call refuses a NULL session. */
    int32_t last_error = 0;
    uint32_t error_count = 0;
    EXPECT_EQ(ecuio_session_start(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_close(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_unregister_frame_callback(nullptr, counting_callback, nullptr),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_error_state(nullptr, &last_error, &error_count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_signal_set(nullptr), nullptr);
}

TEST(CoreTest, DestroyAutoClosesActiveSession)
{
    /* The fclose/free split's convenience path: destroy on an open,
     * RUNNING session must join the worker and free: no hang, no leak. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://autodestroy", &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    std::atomic<int> frames{0};
    static std::atomic<int>* p = &frames;
    EXPECT_EQ(ecuio_register_frame_callback(
        session, [](const ecuio_frame_t*, void*) { (*p)++; }, nullptr), ECUIO_OK);
    wait_for([&frames]() { return frames.load() >= 1; }, 5000);
    EXPECT_EQ(ecuio_session_destroy(session), ECUIO_OK);
}

// ---------------------------------------------------------------------------
// ADR-0013: profile registry and session-level selection
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileLookupRawReturnsOkWithNullSet) {
    /* THE contract the null-profile design lives or dies on: "raw" is
     * REGISTERED with a NULL set. Lookup success + null is distinct
     * from lookup failure: presence and content are separate axes,
     * and the first sketch of this feature collapsed them. */
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("raw", &set), ECUIO_OK);
    EXPECT_EQ(set, nullptr);
}

TEST(CoreTest, ProfileLookupMockReturnsMockSet) {
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("mock", &set), ECUIO_OK);
    EXPECT_EQ(set, ecuio_mock_signal_set());
}

TEST(CoreTest, ProfileLookupUnknownNameFailsWithoutTouchingOut) {
    ecuio_signal_set_t sentinel = {};
    const ecuio_signal_set_t* set = &sentinel;
    EXPECT_EQ(ecuio_profile_lookup("no-such-profile", &set),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(set, &sentinel);   /* untouched on failure: caller may
                                    rely on its own initialization */
}

TEST(CoreTest, ProfileLookupRejectsNullArguments) {
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup(nullptr, &set), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_profile_lookup("raw", nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_profile_register(nullptr, nullptr, nullptr), ECUIO_ERR_NULL_ARGUMENT);
}

TEST(CoreTest, ProfileRegisterOverwritesLastWins) {
    /* Duplicate-registration semantics: last wins, matching the URI
     * grammar's duplicate-key rule. Registered under a throwaway name
     * so no built-in is disturbed. */
    ecuio_signal_set_t mine = {};
    mine.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    EXPECT_EQ(ecuio_profile_register("test-profile-tmp", &mine, nullptr), ECUIO_OK);
    const ecuio_signal_set_t* got = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("test-profile-tmp", &got), ECUIO_OK);
    EXPECT_EQ(got, &mine);

    EXPECT_EQ(ecuio_profile_register("test-profile-tmp", nullptr, nullptr), ECUIO_OK);
    got = &mine;
    EXPECT_EQ(ecuio_profile_lookup("test-profile-tmp", &got), ECUIO_OK);
    EXPECT_EQ(got, nullptr);
    EXPECT_EQ(ecuio_profile_unregister("test-profile-tmp"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterValidatesNameAndSet) {
    /* The registry is the plugin surface (ADR-0013), so it checks what
     * it's handed: a set that fails validation, or a name no URI could
     * select, is refused and leaves nothing registered. */
    ecuio_signal_def_t bad_def = {}; /* bit_length 0 */
    ecuio_signal_set_t bad = {};
    bad.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    bad.count = 1;
    bad.defs = &bad_def;
    const ecuio_signal_set_t* got = nullptr;
    EXPECT_EQ(ecuio_profile_register("test-bad-set", &bad, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_profile_lookup("test-bad-set", &got), ECUIO_ERR_UNKNOWN_PROFILE);

    /* A set written for an earlier layout carries 1 or 2 where
     * layout_version sits, so it's refused instead of decoding garbage. */
    ecuio_signal_set_t old_layout = *ecuio_mock_signal_set();
    old_layout.layout_version = 2;
    EXPECT_EQ(ecuio_profile_register("test-old-layout", &old_layout, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_profile_lookup("test-old-layout", &got), ECUIO_ERR_UNKNOWN_PROFILE);

    for (const char* name : {"", "Upper", "has space", "a&b", "a=b", "q?x"}) {
        EXPECT_EQ(ecuio_profile_register(name, nullptr, nullptr),
                  ECUIO_ERR_INVALID_ARGUMENT) << "'" << name << "'";
    }
    const std::string name64(ECUIO_PROFILE_NAME_MAX + 1, 'q');
    EXPECT_EQ(ecuio_profile_register(name64.c_str(), nullptr, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_profile_register("ok-name_1.2", nullptr, nullptr), ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister("ok-name_1.2"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterReportsTheValidatorResult) {
    /* A refused set says which check failed and on which def, so a
     * plugin author doesn't have to search for it. */
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    ecuio_signal_def_t defs[2] = {mock->defs[0], mock->defs[1]};
    defs[1].bit_length = 0;
    ecuio_signal_set_t bad = {};
    bad.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    bad.count = 2;
    bad.defs = defs;

    ecuio_validation_t v;
    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-why", &bad, &v), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_BIT_LENGTH);
    EXPECT_EQ(v.index, 1u);
    EXPECT_EQ(v.reserved[0], 0u);
    EXPECT_EQ(v.reserved[1], 0u);

    /* A name refused before the set is checked, or a NULL set, which
     * isn't checked at all: code 0 and no index. */
    const struct {
        const char* name;
        const ecuio_signal_set_t* set;
        ecuio_status_t status;
    } kUnchecked[] = {
        {nullptr, &bad, ECUIO_ERR_NULL_ARGUMENT},
        {"Upper", &bad, ECUIO_ERR_INVALID_ARGUMENT},
        {"obd2", &bad, ECUIO_ERR_INVALID_ARGUMENT},
        {"test-why", nullptr, ECUIO_OK},
    };
    for (const auto& c : kUnchecked) {
        const char* label = c.name != nullptr ? c.name : "NULL name";
        std::memset(&v, 0xAB, sizeof v);
        EXPECT_EQ(ecuio_profile_register(c.name, c.set, &v), c.status) << label;
        EXPECT_EQ(v.code, 0) << label;
        EXPECT_EQ(v.index, UINT32_MAX) << label;
        EXPECT_EQ(v.reserved[0], 0u) << label;
        EXPECT_EQ(v.reserved[1], 0u) << label;
    }

    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-why", mock, &v), ECUIO_OK);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(v.index, UINT32_MAX);
    EXPECT_EQ(ecuio_profile_unregister("test-why"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterRefusesMoreRequestsThanTheEngineHolds) {
    /* A set may derive at most ECUIO_PROFILE_MAX_REQUESTS requests
     * (ADR-0015 section 2), so a session never leaves one unasked with nothing
     * to say why. Defs read from one reply count once: def 1 reads DID
     * 0x1000's reply, as def 0 does. One past the limit is refused, naming
     * the first def that didn't fit, and leaves nothing registered.
     * Service 22 DIDs, since the limit is more than Service 01's PIDs. */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    static ecuio_signal_def_t defs[ECUIO_PROFILE_MAX_REQUESTS + 2];
    for (uint32_t i = 0; i < ECUIO_PROFILE_MAX_REQUESTS + 2; ++i) {
        defs[i] = obd2->defs[0];
        defs[i].id = static_cast<uint16_t>(100 + i);
        defs[i].byte_order = ECUIO_BYTE_ORDER_BE;
        defs[i].start_byte = 3;
        defs[i].bit_length = 8;
        defs[i].mux_length = 3;
        defs[i].mux_key = 0x620000u | (0x1000u + ((i == 1) ? 0u : i));
    }
    static ecuio_signal_set_t fits = {};
    fits.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    fits.count = ECUIO_PROFILE_MAX_REQUESTS + 1;
    fits.defs = defs;
    fits.channel_count = obd2->channel_count;
    fits.channels = obd2->channels;
    ecuio_signal_set_t over = fits;
    over.count = ECUIO_PROFILE_MAX_REQUESTS + 2;

    ecuio_validation_t v;
    EXPECT_EQ(ecuio_profile_register("test-requests", &fits, &v), ECUIO_OK);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(ecuio_profile_unregister("test-requests"), ECUIO_OK);

    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-requests", &over, &v),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_REQUEST_COUNT);
    EXPECT_EQ(v.index, ECUIO_PROFILE_MAX_REQUESTS + 1);
    EXPECT_EQ(v.reserved[0], 0u);
    EXPECT_EQ(v.reserved[1], 0u);
    const ecuio_signal_set_t* got = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("test-requests", &got), ECUIO_ERR_UNKNOWN_PROFILE);
}

TEST(CoreTest, ProfileBuiltInsCannotBeReplacedOrRemoved) {
    /* A URI naming a built-in always means the same set. */
    for (const char* name : {"mock", "raw", "obd2"}) {
        EXPECT_EQ(ecuio_profile_register(name, ecuio_mock_signal_set(), nullptr),
                  ECUIO_ERR_INVALID_ARGUMENT) << name;
        EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_ERR_INVALID_ARGUMENT) << name;
    }
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    EXPECT_EQ(set, ecuio_obd2_signal_set());
    ASSERT_EQ(ecuio_profile_lookup("raw", &set), ECUIO_OK);
    EXPECT_EQ(set, nullptr);
}

TEST(CoreTest, ProfileUnregisterKeepsExistingSessionsOnTheirSet) {
    /* Unregister removes the name for new sessions only: a session
     * keeps the set it was created with (the profile.h lifetime rule). */
    static const ecuio_signal_set_t mine = *ecuio_mock_signal_set();
    ASSERT_EQ(ecuio_profile_register("test-unregister", &mine, nullptr), ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=test-unregister", &session),
              ECUIO_OK);

    EXPECT_EQ(ecuio_profile_unregister("test-unregister"), ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister("test-unregister"), ECUIO_OK);     /* idempotent */
    EXPECT_EQ(ecuio_profile_unregister(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_signal_set(session), &mine);

    ecuio_session_t* second = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=test-unregister", &second),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(second, nullptr);
    ecuio_session_destroy(session);
}

TEST(CoreTest, MockSessionSelectsMockSet) {
    /* Scheme default: mock:// -> the mock profile's set. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://profile-default", &session),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_mock_signal_set());
    ecuio_session_destroy(session);
}

TEST(CoreTest, ProfileOverrideRawYieldsNullSet) {
    /* ?profile=raw on a mock session: override to the null profile.
     * ALSO the probe for mock-transport query tolerance (ASSUMED:
     * mock_transport.cpp not reviewed): if this goes red, the mock is
     * choking on the query, and the fix belongs THERE, not here. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=raw", &session),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), nullptr);
    ecuio_session_destroy(session);
}

TEST(CoreTest, MockUriQueryIsStrict) {
    /* mock:// shares j2534://'s query grammar (uri_query.h): profile is
     * its only key, and malformed queries fail at create. The text
     * before the '?' stays a free label. */
    for (const char* uri : {"mock://x?interval=10", "mock://x?",
                            "mock://x?profile=mock&", "mock://x?&profile=mock",
                            "mock://x?profile", "mock://x?PROFILE=mock",
                            "mock://x?profile=mock&&profile=raw"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI) << uri;
        EXPECT_EQ(session, nullptr) << uri;
    }
    for (const char* uri : {"mock://", "mock://any-label", "mock://x?profile=raw",
                            "mock://x?profile=raw&profile=mock"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_OK) << uri;
        ecuio_session_destroy(session);
    }
}

TEST(CoreTest, UnknownProfileRejectedAtCreate) {
    /* Strict grammar: an unknown name is ECUIO_ERR_UNKNOWN_PROFILE at
     * session_create: cross-platform, scheme-independent. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &session),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

#ifdef _WIN32
TEST(CoreTest, UnknownProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, Windows-only: bogus profile on a
     * NONEXISTENT DLL path. Profile validation must run BEFORE
     * transport attach, so this yields ECUIO_ERR_UNKNOWN_PROFILE, not
     * ECUIO_ERR_LOAD_FAILED. If it ever returns LOAD_FAILED, the profile check
     * moved after the attach and the fail-fast contract broke. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(
        "j2534://C:/ecuio-definitely-not-a-real-dll.dll?profile=bogus",
        &session), ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// ADR-0013: profile query tri-state, a present-but-malformed
// ?profile= value must fail session_create, never silently downgrade
// to the scheme default. Core is as strict about ?profile= as
// parse_query is about every other key.
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileQueryEmptyValueRejected) {
    /* ?profile= with nothing after '=': PRESENT but malformed, the
     * old tolerant scanner silently fell back to the scheme default,
     * which could hand a typo'd ?profile=raw a decoding session. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=", &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryTooLongValueRejected) {
    /* 64+ chars can't be a registry name (64-byte scan buffer, room
     * for the terminator). Fail fast, not truncate-and-confuse. */
    std::string uri = "mock://x?profile=";
    uri.append(80, 'a');
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryBoundaryLengthAcceptedAt63) {
    /* 63 chars is the last length the scanner accepts. Registered
     * under exactly that name so this proves the ACCEPTING side of
     * the boundary: the 64 case above pins the rejecting side. */
    const std::string name63(63, 'q');
    const ecuio_signal_set_t* mock_set = ecuio_mock_signal_set();
    ASSERT_EQ(ecuio_profile_register(name63.c_str(), mock_set, nullptr), ECUIO_OK);

    std::string uri = "mock://boundary?profile=" + name63;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), mock_set);
    ecuio_session_destroy(session);
}

TEST(CoreTest, ProfileQueryMalformedFailsEvenWhenLaterValidExists) {
    /* Scanner semantics: ANY malformed occurrence fails, last-wins
     * applies only across syntactically valid values. The empty
     * first value must not be healed by the valid second one. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=&profile=mock",
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);

    /* Reverse order: valid first, malformed LAST, a later malformed
     * value still poisons the URI (it is not a "drop the bad one,
     * keep scanning" grammar: it is strict). */
    EXPECT_EQ(ecuio_session_create("mock://x?profile=mock&profile=",
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryLastValidWinsAcrossDuplicates) {
    /* Across WELL-FORMED values, the last wins: matching the
     * documented duplicate-key rule for every other URI parameter.
     * Distinguished from the malformed cases above by using a real,
     * registered name that differs from the scheme default. */
    const std::string name63(63, 'q');   /* registered by the boundary
                                            test above, but tests
                                            shouldn't depend on order. */
    ASSERT_EQ(ecuio_profile_register(name63.c_str(),
                                     ecuio_mock_signal_set(), nullptr), ECUIO_OK);

    std::string uri1 = "mock://x?profile=raw&profile=" + name63;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri1.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session),
              ecuio_mock_signal_set());    /* last (name63) won */
    ecuio_session_destroy(session);

    /* And the inverse order: raw last -> null set */
    std::string uri2 = "mock://x?profile=" + name63 + "&profile=raw";
    ASSERT_EQ(ecuio_session_create(uri2.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), nullptr);
    ecuio_session_destroy(session);
}

#ifdef _WIN32
TEST(CoreTest, MalformedProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, matching UnknownProfileRejectedBeforeDllLoad:
     * a MALFORMED profile value on a nonexistent DLL path must yield
     * BAD_URI (profile-parse failure), not ECUIO_ERR_LOAD_FAILED.
     * If it returns 7, the tri-state check moved after attach. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(
        "j2534://C:/ecuio-definitely-not-a-real-dll.dll?profile=",
        &session), ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// ADR-0020: uds-generic builds its set from the URI's ?did= entries, a set
// of the session's own; no other profile takes the key.
// ---------------------------------------------------------------------------

TEST(CoreTest, UdsGenericBuildsItsSetFromTheUri) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(
        "mock://x?profile=uds-generic&did=1E1C:u16,F190:text", &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    EXPECT_NE(set, ecuio_uds_generic_template_set()) << "the session's own";
    ASSERT_EQ(set->count, 2u);
    EXPECT_STREQ(set->defs[0].name, "DID 1E1C");
    EXPECT_EQ(set->defs[0].kind, ECUIO_VALUE_UINT);
    EXPECT_STREQ(set->defs[1].name, "DID F190");
    EXPECT_EQ(set->defs[1].kind, ECUIO_VALUE_TEXT);
    ASSERT_EQ(set->channel_count, 1u);
    EXPECT_EQ(set->channels[0].rx_id, ECUIO_OBD2_ECU_ARB_ID);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_STREQ(info.profile, "uds-generic");

    /* A second session gets a set of its own */
    ecuio_session_t* other = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://y?profile=uds-generic&did=1E1C",
                                   &other), ECUIO_OK);
    EXPECT_NE(ecuio_session_signal_set(other), set);
    EXPECT_EQ(ecuio_session_signal_set(other)->count, 1u);
    ecuio_session_destroy(other);
    EXPECT_EQ(set->count, 2u) << "destroying one leaves the other's set";
    ecuio_session_destroy(session);
}

TEST(CoreTest, DidKeyBelongsToUdsGeneric) {
    const struct {
        const char* uri;
        const char* detail;
    } kCases[] = {
        {"mock://x?did=1E1C",
         "?did= belongs to the uds-generic profile: add ?profile=uds-generic"},
        {"mock://x?profile=raw&did=1E1C",
         "?did= belongs to the uds-generic profile: add ?profile=uds-generic"},
        {"mock://x?profile=uds-generic",
         "?profile=uds-generic needs ?did= to say which DIDs to read"},
        {"mock://x?profile=uds-generic&did=1EFF-1E00",
         "?did= needs a range whose end isn't before its start, not '1EFF-1E00'"},
        {"mock://x?profile=uds-generic&did=",
         "?did= needs at least one DID"},
    };
    for (const auto& c : kCases) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(c.uri, &session), ECUIO_ERR_BAD_URI) << c.uri;
        EXPECT_EQ(session, nullptr) << c.uri;
        EXPECT_STREQ(ecuio_last_error_detail(), c.detail) << c.uri;
    }
    /* The profile's name is judged first */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus&did=1E1C", &session),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

namespace {

/* The DID replies the mock answers with, kept whole: at most kMax, each
 * one a frame (4 KB) on the heap. */
struct DidCapture {
    static constexpr size_t kMax = 24;
    std::vector<ecuio_frame_t> frames;
    std::atomic<size_t> count{0};
};

void capture_did_cb(const ecuio_frame_t* frame, void* user_data)
{
    DidCapture* cap = static_cast<DidCapture*>(user_data);
    if (frame->data_length >= 3 && frame->data[0] == 0x62 &&
        cap->count.load() < DidCapture::kMax) {
        cap->frames[cap->count.load()] = *frame;
        cap->count.fetch_add(1);
    }
}

} // namespace

/* Under uds-generic the mock answers each of the session's DIDs every
 * tick, from the ECU the def names, so the app shows the profile with no
 * hardware: a number that counts the ticks and a text naming the DID. */
TEST(CoreTest, MockAnswersTheSessionsDids) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(
        "mock://dids?profile=uds-generic&did=1E1C:u16,F190:text,7E9:1E1C:u8",
        &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_EQ(set->count, 3u);
    DidCapture cap;
    cap.frames.resize(DidCapture::kMax);
    ASSERT_EQ(ecuio_register_frame_callback(session, &capture_did_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&cap]() { return cap.count.load() >= 9; }, 5000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);   /* joins the worker */

    ASSERT_GE(cap.count.load(), size_t{9}) << "three DIDs over three ticks";
    size_t seen[3] = {0, 0, 0};
    double last_word = -1.0;
    for (size_t f = 0; f < cap.count.load(); ++f) {
        const ecuio_frame_t& frame = cap.frames[f];
        EXPECT_EQ(frame.flags, 0u) << "a classic-CAN reply, as J2534 delivers";
        bool applied = false;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (!ecuio_def_applies_to_frame(&d, &frame)) {
                continue;
            }
            applied = true;
            ++seen[i];
            if (d.kind == ECUIO_VALUE_TEXT) {
                char text[64] = "";
                ASSERT_EQ(ecuio_decode_text(&d, frame.data, frame.data_length,
                                            text, sizeof(text)), ECUIO_DECODE_OK);
                EXPECT_EQ(std::string(text).rfind("MOCK F190 #", 0), size_t{0}) << text;
            } else {
                double v = 0.0;
                ASSERT_EQ(ecuio_decode_signal(&d, frame.data, frame.data_length, &v),
                          ECUIO_DECODE_OK);
                if (i == 0) {
                    EXPECT_GT(v, last_word) << "the tick count grows";
                    last_word = v;
                    EXPECT_EQ(frame.arbitration_id, 0x7E8u);
                } else {
                    EXPECT_EQ(frame.arbitration_id, 0x7E9u) << "the ECU the prefix named";
                    EXPECT_EQ(frame.data_length, 4u) << "u8: one value byte";
                }
            }
        }
        EXPECT_TRUE(applied) << "frame " << f << " answers no def";
    }
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_GE(seen[i], size_t{3}) << set->defs[i].name;
    }
    ecuio_session_destroy(session);
}

// ---- replay:// (ADR-0023) ----

namespace {

/* A scratch name carries this process's id besides the test's name, so
 * runs of one case from two checkouts at once never share a file or a
 * folder (tests/temp_name.h) */
TEST(TempNameTest, TheNameCarriesThisProcessAndTheTest)
{
#ifdef _WIN32
    const long pid = _getpid();
#else
    const long pid = static_cast<long>(getpid());
#endif
    EXPECT_EQ(temp_name("ecuio-x"), "ecuio-x-" + std::to_string(pid) +
                                        "-TheNameCarriesThisProcessAndTheTest");
}

/* A scratch recording in the temp folder, named for the running test and
 * removed at the end. path is absolute and uses forward slashes, which
 * the replay path accepts on every platform. */
struct TempRecording {
    std::string dir;
    std::string csv;
    bool made_dir = false;

    explicit TempRecording(const std::string& text, bool as_folder = false)
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr)
            tmp = std::getenv("TMPDIR");
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\')
                c = '/';
        }
        while (base.size() > 1 && base.back() == '/')
            base.pop_back();
        const std::string name = temp_name("ecuio-replay");
        if (as_folder) {
            dir = base + "/" + name;
#ifdef _WIN32
            made_dir = _mkdir(dir.c_str()) == 0 || errno == EEXIST;
#else
            made_dir = mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
#endif
            csv = dir + "/frames.csv";
        } else {
            csv = base + "/" + name + ".csv";
        }
        FILE* f = std::fopen(csv.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    ~TempRecording()
    {
        std::remove(csv.c_str());
        if (made_dir) {
#ifdef _WIN32
            _rmdir(dir.c_str());
#else
            rmdir(dir.c_str());
#endif
        }
    }
    std::string uri(const char* query = "") const
    {
        return "replay://" + (dir.empty() ? csv : dir) + query;
    }
};

const char* const kFramesHeader = "host_us,timestamp_us,id,flags,data\n";

/* Every frame a replay delivers, and when it arrived */
struct ReplayCapture {
    std::mutex mutex;
    std::vector<ecuio_frame_t> frames;
    std::vector<std::chrono::steady_clock::time_point> arrived;
    /* The callback holds the worker this long at this frame (a late
     * frame); 0 never */
    size_t stall_at = 0;
    int stall_ms = 0;
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }
};

void replay_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* c = static_cast<ReplayCapture*>(user_data);
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(c->mutex);
        c->frames.push_back(*frame);
        c->arrived.push_back(std::chrono::steady_clock::now());
        n = c->frames.size();
    }
    if (c->stall_at != 0 && n == c->stall_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds(c->stall_ms));
    }
}

/* Creates, registers and starts a replay session, or fails the test */
ecuio_session_t* start_replay(const std::string& uri, ReplayCapture* capture)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    if (session == nullptr)
        return nullptr;
    EXPECT_EQ(
        ecuio_register_frame_callback(session, replay_capture_cb, capture),
        ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    return session;
}

/* Rows at step_us steps of session time, 0x7E8 and 41 0C 1F 40 each */
std::string paced_rows(int n, int step_us = 100000)
{
    std::string text = kFramesHeader;
    for (int i = 0; i < n; ++i) {
        const std::string t = std::to_string(5000000 + int64_t{step_us} * i);
        text += t + "," + t + ",0x7E8,0x0,41 0C 1F 40\n";
    }
    return text;
}

/* Reads the worker's own operator new count at two frames past its
 * start-up; the callback itself allocates nothing */
struct WorkerNews {
    static constexpr int kFrom = 3;
    int to = 0;
    std::atomic<int> frames{0};
    std::atomic<unsigned long> at_from{0};
    std::atomic<unsigned long> at_to{0};
    int batches = 0; /* values batches, counted on the worker */
};

void worker_news_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<WorkerNews*>(user_data)->batches;
}

void worker_news_cb(const ecuio_frame_t*, void* user_data)
{
    auto* w = static_cast<WorkerNews*>(user_data);
    const int n = w->frames.fetch_add(1) + 1;
    if (n == WorkerNews::kFrom)
        w->at_from.store(new_counter::on_this_thread());
    if (n == w->to)
        w->at_to.store(new_counter::on_this_thread());
}

} // namespace

/* Past start-up a worker's frames cost it no allocation, so none can
 * throw: the mock's ticks, and replay's rows and passes (the file
 * reopened each time round), each decoded for a values callback too */
TEST(CoreTest, WorkersAllocateNothingPerFrame)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(
            ecuio_register_values_callback(session, worker_news_values_cb, &w),
            ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        EXPECT_GT(w.batches, 0) << "the values path ran";
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
}

/* Nor do the pull queues (ecuio/queue.h): their slots are made at open,
 * and each frame and value is copied into one, the oldest dropped when
 * full; the values queue alone makes the worker decode */
TEST(CoreTest, WorkersAllocateNothingPerFrameIntoQueues)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_queue_t* frames = nullptr;
        ecuio_queue_t* values = nullptr;
        ASSERT_EQ(ecuio_queue_open(session, ECUIO_QUEUE_FRAMES, 4, &frames),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_queue_open(session, ECUIO_QUEUE_VALUES, 4, &values),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        ecuio_queue_value_t v[4];
        uint32_t count = 0;
        uint64_t dropped = 0;
        EXPECT_EQ(ecuio_queue_read_values(values, v, 4, &count, &dropped, 0),
                  ECUIO_OK);
        EXPECT_GT(count, 0u) << "the values path ran";
        EXPECT_GT(dropped, 0u) << "and dropped";
        ecuio_queue_close(frames);
        ecuio_queue_close(values);
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
}

/* Recording costs the worker no allocation either: the ring is made at
 * open, and each frame and its values (decoded for the recording, with
 * no values callback registered) are copied into it (ADR-0049 section 1) */
TEST(CoreTest, WorkersAllocateNothingPerFrameWhileRecording)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr)
        tmp = std::getenv("TMPDIR");
    const std::filesystem::path root =
        std::filesystem::path(tmp != nullptr ? tmp : "/tmp") /
        temp_name("ecuio-core-recording-news");
    const std::string root8 = root.generic_u8string();
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_record_options_t o{};
        o.root_utf8 = root8.c_str();
        ecuio_recorder_t* recorder = nullptr;
        ASSERT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_session_record(session, recorder), ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        /* The writer counts the rows it writes, within its next wakes */
        ecuio_recorder_state_t st{};
        wait_for(
            [&] {
                ecuio_recorder_get_state(recorder, &st);
                return st.value_rows > 0;
            },
            2000);
        EXPECT_EQ(ecuio_recorder_close(recorder, nullptr), ECUIO_OK);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        EXPECT_GT(st.value_rows, 0u)
            << "the values were decoded for the recording";
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

/* Each row goes out as recorded (ID, flags, bytes, an empty payload
 * included), stamped with the recording's session time rebased to the
 * first row; the session info says what's playing */
TEST(CoreTest, ReplaySendsTheRecordedFramesAtTheirRebasedTimes)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "10,1000000,0x7E8,0x0,41 0C 1F 40\n"
                      "600,1000500,0x18DAF110,0x1,\n"
                      "2100,1002000,0x7E8,0x40,62 F1 90 41 42\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));

    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_REPLAY);
    EXPECT_EQ(info.replay_frames, 3u);
    EXPECT_EQ(info.replay_span_ms, 2u);
    EXPECT_EQ(info.replay_speed_milli, 0u) << "max";
    EXPECT_EQ(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP, 0u);
    EXPECT_EQ(info.channel_count, 0u) << "passive";
    EXPECT_STREQ(info.profile, "raw") << "replay's default";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int32_t last = -1;
    uint32_t episodes = 99;
    ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
              ECUIO_OK);
    EXPECT_EQ(last, ECUIO_OK) << "the end of the file isn't an error";
    EXPECT_EQ(episodes, 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);

    ASSERT_EQ(cap.frames.size(), 3u)
        << "the end of the file sends nothing more";
    const ecuio_frame_t& a = cap.frames[0];
    EXPECT_EQ(a.arbitration_id, 0x7E8u);
    EXPECT_EQ(a.flags, 0u);
    EXPECT_EQ(a.timestamp_us, 0u);
    ASSERT_EQ(a.data_length, 4u);
    EXPECT_EQ(a.data[0], 0x41);
    EXPECT_EQ(a.data[3], 0x40);
    const ecuio_frame_t& b = cap.frames[1];
    EXPECT_EQ(b.arbitration_id, 0x18DAF110u);
    EXPECT_EQ(b.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_EQ(b.data_length, 0u);
    EXPECT_EQ(b.timestamp_us, 500u);
    const ecuio_frame_t& c = cap.frames[2];
    EXPECT_EQ(c.flags, 0x40u) << "flags as recorded, TRUNCATED among them";
    EXPECT_EQ(c.data_length, 5u);
    EXPECT_EQ(c.timestamp_us, 2000u);
    ecuio_session_destroy(session);
}

/* A raw adapter clock that wraps steps timestamp_us backwards: that row
 * is timed by its host_us step instead, so session time never decreases */
TEST(CoreTest, ReplayTimesABackwardStampByTheHostClock)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,4294967000,0x7E8,0x0,41 0D 00\n"
                      "200,4294967200,0x7E8,0x0,41 0D 01\n"
                      "600,100,0x7E8,0x0,41 0D 02\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(cap.frames[0].timestamp_us, 0u);
    EXPECT_EQ(cap.frames[1].timestamp_us, 200u);
    EXPECT_EQ(cap.frames[2].timestamp_us, 600u) << "the host's 400 us step";
    ecuio_session_destroy(session);
}

/* loop=1 plays the file again, its stamps carried on from the last pass */
TEST(CoreTest, ReplayLoopsWithStampsThatNeverDecrease)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,0,0x7E8,0x0,41 0D 00\n"
                      "1000,1000,0x7E8,0x0,41 0D 01\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max&loop=1"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 6; }, 2000));
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP, 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    for (size_t i = 1; i < cap.frames.size(); ++i) {
        ASSERT_GE(cap.frames[i].timestamp_us, cap.frames[i - 1].timestamp_us)
            << "frame " << i;
    }
    EXPECT_EQ(cap.frames[2].timestamp_us, 1000u)
        << "the second pass starts where the first ended";
    EXPECT_EQ(cap.frames[3].timestamp_us, 2000u);
    EXPECT_EQ(cap.frames[2].data[2], 0x00) << "from the top";
    ecuio_session_destroy(session);
}

/* A recording whose frames all share one time (one frame, or several
 * stamped alike) has no span to pace a pass by: ?loop=1 is refused at
 * create, where the worker would spin; played once, it plays */
TEST(CoreTest, ReplayRefusesToLoopARecordingWithNoSpan)
{
    struct Case {
        const char* rows;
        size_t frames;
    };
    for (const Case& c : {Case{"5,7,0x7E8,0x0,41 0D 00\n", 1},
                          Case{"5,7,0x7E8,0x0,41 0D 00\n"
                               "6,7,0x7E8,0x0,41 0D 01\n", 2}}) {
        TempRecording rec(std::string(kFramesHeader) + c.rows);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri("?loop=1").c_str(), &session),
                  ECUIO_ERR_BAD_URI);
        EXPECT_EQ(session, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "?loop=1 needs a recording that spans time; this one's "
                     "frames all share one time");

        ReplayCapture cap;
        session = start_replay(rec.uri("?speed=max"), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= c.frames; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        EXPECT_EQ(cap.frames.size(), c.frames) << "played once";
        ecuio_session_destroy(session);
    }
}

/* Six rows a second apart by host_us, their adapter stamps 7 s ahead and
 * 0.25 s apart, so a stretch chosen by host_us and timed by timestamp_us
 * shows which clock does which; row i carries 41 0D i */
std::string stretch_rows()
{
    std::string text = kFramesHeader;
    for (int i = 0; i < 6; ++i) {
        char row[64];
        std::snprintf(row, sizeof row, "%d,%d,0x7E8,0x0,41 0D %02X\n",
                      i * 1000000, 7000000 + i * 250000, i);
        text += row;
    }
    return text;
}

/* ?from= and ?to= (ADR-0023's appendix "a stretch replayed") play the
 * rows from the first at or after from to the last at or before to, by
 * host_us, the viewer's and havne-cli view's seconds; the first is
 * stamped 0 and the rest by the recording's session time; the info counts
 * the stretch's frames and span */
TEST(CoreTest, ReplayStretchPlaysItsRowsAlone)
{
    TempRecording rec(stretch_rows());
    struct Case {
        const char* query;
        std::vector<int> rows;
    };
    const Case cases[] = {
        {"?speed=max&from=1.5&to=4", {2, 3, 4}},
        {"?speed=max&from=4", {4, 5}},
        {"?speed=max&to=1", {0, 1}},
        {"?speed=max&from=0.5&to=0.999&from=3&to=3.5", {3}}, /* last wins */
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.query);
        ReplayCapture cap;
        ecuio_session_t* session = start_replay(rec.uri(c.query), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(
            wait_for([&] { return cap.count() >= c.rows.size(); }, 2000));
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
        EXPECT_EQ(info.replay_frames, c.rows.size());
        EXPECT_EQ(info.replay_span_ms, (c.rows.size() - 1) * 250u);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int32_t last = -1;
        uint32_t episodes = 99;
        ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
                  ECUIO_OK);
        EXPECT_EQ(last, ECUIO_OK) << "the stretch's end isn't an early stop";
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        ASSERT_EQ(cap.frames.size(), c.rows.size()) << "nothing past to";
        for (size_t i = 0; i < c.rows.size(); ++i) {
            EXPECT_EQ(int{cap.frames[i].data[2]}, c.rows[i]) << "frame " << i;
            EXPECT_EQ(cap.frames[i].timestamp_us, i * 250000u)
                << "frame " << i << ": 0 first, then session time";
        }
        ecuio_session_destroy(session);
    }
}

/* The rows before a stretch are read but never waited for: at speed 1
 * its first frame goes out at once, the next a second of session time
 * later; a loop plays the stretch again, its stamps carried on */
TEST(CoreTest, ReplayStretchStartsAtOnceAndLoopsItself)
{
    std::string text = kFramesHeader;
    for (int i = 0; i < 5; ++i) {
        char row[64];
        std::snprintf(row, sizeof row, "%d,%d,0x7E8,0x0,41 0D %02X\n",
                      i * 1000000, i * 300000, i);
        text += row;
    }
    TempRecording rec(text);
    ReplayCapture cap;
    const auto started = std::chrono::steady_clock::now();
    ecuio_session_t* session = start_replay(rec.uri("?from=3&to=4"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 3000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    const auto ms = [&](size_t i) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   cap.arrived[i] - started)
            .count();
    };
    EXPECT_LT(ms(0), 250) << "not after the 0.9 s of rows before it";
    EXPECT_GE(ms(1) - ms(0), 250) << "the stretch's own 0.3 s";
    EXPECT_LT(ms(1) - ms(0), 800);
    ecuio_session_destroy(session);

    ReplayCapture looped;
    session = start_replay(rec.uri("?speed=max&loop=1&from=3&to=4"), &looped);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return looped.count() >= 6; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(int{looped.frames[i].data[2]}, static_cast<int>(3 + i % 2))
            << "frame " << i;
        EXPECT_EQ(looped.frames[i].timestamp_us, (i + 1) / 2 * 300000u)
            << "frame " << i;
    }
    ecuio_session_destroy(session);
}

/* What a stretch refuses, at create: a value that isn't seconds or to
 * not after from (BAD_URI), a stretch with no frames, saying where the
 * frames run (LOAD_FAILED), and a loop of one instant (BAD_URI) */
TEST(CoreTest, ReplayStretchRefusals)
{
    TempRecording rec(stretch_rows());
    const char* const bad_values[] = {
        "?from=-1",       "?from=1.2345", "?from=abc", "?to=",   "?from=1e3",
        "?to=1000000001", "?from=.5",     "?from=5.",  "?to=1,5"};
    for (const char* q : bad_values) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "?from= and ?to= take the recording's seconds, as the "
                     "viewer and havne-cli view show them, with at most three "
                     "decimals")
            << q;
    }
    for (const char* q : {"?from=2&to=2", "?from=3&to=1"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_STREQ(ecuio_last_error_detail(), "?to= must come after ?from=")
            << q;
    }
    struct Empty {
        const char* query;
        const char* says;
    };
    const Empty empties[] = {
        {"?from=10", "the recording holds no frames from 10 s on: its frames "
                     "run from 0 s to 5 s"},
        {"?from=1.2&to=1.8", "the recording holds no frames from 1.2 s to "
                             "1.8 s: its frames run from 0 s to 5 s"},
    };
    for (const Empty& e : empties) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(e.query).c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED)
            << e.query;
        EXPECT_STREQ(ecuio_last_error_detail(), e.says);
    }
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(rec.uri("?loop=1&from=2&to=2.5").c_str(),
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?loop=1 needs a stretch that spans time; this one's frames "
                 "all share one time");
}

/* One second of recording takes about a second at speed 1 and a tenth of
 * that at 10; the frames keep the recording's times either way */
TEST(CoreTest, ReplayPacesByTheRecordingsTimeOverTheSpeed)
{
    TempRecording rec(paced_rows(11));
    for (const char* q : {"", "?speed=10"}) {
        SCOPED_TRACE(q);
        ReplayCapture cap;
        ecuio_session_t* session = start_replay(rec.uri(q), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= 11; }, 5000));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            cap.arrived.back() - cap.arrived.front())
                            .count();
        if (q[0] == '\0') {
            EXPECT_GE(ms, 900) << "real time";
            EXPECT_LE(ms, 1600);
        } else {
            EXPECT_GE(ms, 80) << "ten times faster";
            EXPECT_LE(ms, 400);
        }
        EXPECT_EQ(cap.frames.back().timestamp_us, 1000000u)
            << "the recording's time";
        ecuio_session_destroy(session);
    }
}

/* Each frame is due at its time from the start of playback, not a step
 * after the last send: a frame held up 400 ms is followed at once by the
 * ones it delayed, and a second of recording still takes about a second.
 * Waiting each step from the last send would add the stall, and on
 * Windows every short wait's rounding, so 200 steps of 5 ms (speed 1) or
 * 50 ms at 10x (5 ms waits) show it either way. */
TEST(CoreTest, ReplayCatchesUpAfterALateFrameAndNeverDrifts)
{
    struct Case {
        const char* query;
        int step_us;
    };
    for (const Case& c : {Case{"", 5000}, Case{"?speed=10", 50000}}) {
        SCOPED_TRACE(c.query);
        TempRecording rec(paced_rows(201, c.step_us));
        ReplayCapture cap;
        cap.stall_at = 20;
        cap.stall_ms = 400;
        ecuio_session_t* session = start_replay(rec.uri(c.query), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= 201; }, 5000));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            cap.arrived.back() - cap.arrived.front())
                            .count();
        EXPECT_GE(ms, 950) << "the last frame is due a second in";
        EXPECT_LE(ms, 1250) << "late frames catch up; the stall isn't added";
        EXPECT_EQ(cap.frames.back().timestamp_us,
                  200u * static_cast<uint64_t>(c.step_us))
            << "the recording's time";
        ecuio_session_destroy(session);
    }
}

/* Attach checked the file, but it can change before the worker reads it
 * again: playback sends the rows up to the first bad one, then stops, and
 * the session still closes cleanly */
TEST(CoreTest, ReplayStopsAtARowThatTurnedBadAfterAttach)
{
    TempRecording rec(paced_rows(5));
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    {
        const std::string rows = paced_rows(3);
        const std::string cut =
            rows + "5300000,5300000,0x7E8,0x0,41 0"; // cut mid-row
        FILE* f = std::fopen(rec.csv.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(cut.data(), 1, cut.size(), f);
        std::fclose(f);
    }
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_session_get_error_state(session, &last, &episodes);
            return last != ECUIO_OK;
        },
        2000));
    EXPECT_EQ(last, ECUIO_ERR_RECORDING_CHANGED) << "says why it stopped";
    EXPECT_EQ(episodes, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cap.count(), 3u) << "nothing past the bad row";
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A recording cut short after attach plays what's left, then says why it
 * stopped: a reader counting to attach's rows would wait forever. One
 * that grew plays attach's rows and ends as at the end of its file */
TEST(CoreTest, ReplayOfARecordingWithOtherRowsPlaysAttachsAtMost)
{
    struct Case {
        int rows_now;
        ecuio_status_t state;
    };
    for (const Case& c :
         {Case{3, ECUIO_ERR_RECORDING_CHANGED}, Case{7, ECUIO_OK}}) {
        SCOPED_TRACE(c.rows_now);
        TempRecording rec(paced_rows(5));
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        {
            const std::string rows = paced_rows(c.rows_now);
            FILE* f = std::fopen(rec.csv.c_str(), "wb");
            ASSERT_NE(f, nullptr);
            std::fwrite(rows.data(), 1, rows.size(), f);
            std::fclose(f);
        }
        ReplayCapture cap;
        ASSERT_EQ(
            ecuio_register_frame_callback(session, replay_capture_cb, &cap),
            ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
        const size_t played =
            static_cast<size_t>(c.rows_now < 5 ? c.rows_now : 5);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= played; }, 2000));
        int32_t last = ECUIO_OK;
        uint32_t episodes = 0;
        if (c.state != ECUIO_OK) {
            ASSERT_TRUE(wait_for(
                [&] {
                    ecuio_session_get_error_state(session, &last, &episodes);
                    return last != ECUIO_OK;
                },
                2000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ecuio_session_get_error_state(session, &last, &episodes);
        EXPECT_EQ(last, c.state);
        EXPECT_EQ(episodes, c.state == ECUIO_OK ? 0u : 1u);
        EXPECT_EQ(cap.count(), played) << "attach's rows at most";
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
    }
}

/* A recording deleted between attach and start: nothing to play, and the
 * error state says so */
TEST(CoreTest, ReplayOfARecordingThatWentAwaySaysSo)
{
    TempRecording rec(paced_rows(3));
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(std::remove(rec.csv.c_str()), 0);
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_session_get_error_state(session, &last, &episodes);
            return last != ECUIO_OK;
        },
        2000));
    EXPECT_EQ(last, ECUIO_ERR_RECORDING_CHANGED);
    EXPECT_EQ(episodes, 1u);
    EXPECT_EQ(cap.count(), 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A long gap in the recording doesn't hold up a close */
TEST(CoreTest, ReplayClosesAtOnceDuringAGap)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,0,0x7E8,0x0,41 0D 00\n"
                      "3600000000,3600000000,0x7E8,0x0,41 0D 01\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri(), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 1; }, 2000));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    EXPECT_EQ(cap.count(), 1u) << "the hour-later frame never went out";
    ecuio_session_destroy(session);
}

/* Attach reads the whole file: anything but a frames.csv, or a bad row,
 * fails create with its line number. Either line ending reads, and a
 * whole-message payload arrives whole. */
/* ADR-0049 section 7: a last row with no newline is a write cut short,
 * left out and counted; a recording of it alone holds no frames */
TEST(CoreTest, ReplayDropsATornLastRowAndCountsIt)
{
    TempRecording rec(std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0C 1F 40\n"
                                                   "2,2,0x7E8,0x0,41 0D 20\n"
                                                   "3,3,0x7E8,0x0,41 0C 1F");
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_frames, 2u);
    EXPECT_EQ(info.replay_rows_torn, 1u);
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cap.count(), 2u) << "the torn row isn't played";
    int32_t last = -1;
    uint32_t episodes = 99;
    ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
              ECUIO_OK);
    EXPECT_EQ(last, ECUIO_OK) << "a whole pass, not a changed file";
    EXPECT_EQ(episodes, 0u);
    ecuio_session_destroy(session);

    TempRecording whole(std::string(kFramesHeader) +
                        "1,1,0x7E8,0x0,41 0C 1F 40\n");
    ASSERT_EQ(ecuio_session_create(whole.uri().c_str(), &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_rows_torn, 0u);
    ecuio_session_destroy(session);

    TempRecording only(std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0C");
    EXPECT_EQ(ecuio_session_create(only.uri().c_str(), &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the recording's frames.csv holds no frames");
}

/* A row attach read whole that has lost its newline when the worker
 * reaches it is a file changed since attach, not a torn row to skip */
TEST(CoreTest, ReplayOfARowThatLostItsNewlineSaysTheFileChanged)
{
    const std::string rows = std::string(kFramesHeader) +
                             "1,1,0x7E8,0x0,41 0C 1F 40\n"
                             "2,2,0x7E8,0x0,41 0D 20\n";
    TempRecording rec(rows);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK);
    {
        FILE* f = std::fopen(rec.csv.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(rows.data(), 1, rows.size() - 1, f); /* the last '\n' */
        std::fclose(f);
    }
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    int32_t last = -1;
    uint32_t episodes = 0;
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_error_state(session, &last, &episodes) ==
                       ECUIO_OK &&
                   last == ECUIO_ERR_RECORDING_CHANGED;
        },
        5000));
    EXPECT_EQ(cap.count(), 1u);
    ecuio_session_destroy(session);
}

TEST(CoreTest, ReplayRefusesABadRecordingWithItsLineNumber)
{
    const std::string ok_row = "1,1,0x7E8,0x0,41 0C 1F 40\n";
    struct Case {
        std::string text;
        const char* detail;
    };
    const std::vector<Case> cases = {
        {"", "the recording's frames.csv is empty"},
        {"host_us,signal,value,unit\n1,RPM,800,RPM\n",
         "not a Havne frames.csv, or a newer format"},
        {kFramesHeader, "the recording's frames.csv holds no frames"},
        {std::string(kFramesHeader) + ok_row + "2,2,7E8,0x0,41\n",
         "frames.csv line 3: id isn't 0x-prefixed hex"},
        {std::string(kFramesHeader) + "x,1,0x7E8,0x0,41\n",
         "frames.csv line 2: host_us isn't"},
        {std::string(kFramesHeader) + "1,-1,0x7E8,0x0,41\n",
         "frames.csv line 2: timestamp_us isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,1,41\n",
         "frames.csv line 2: flags isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0\n",
         "frames.csv line 2: data isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 \n",
         "frames.csv line 2: data isn't"},
        {std::string(kFramesHeader) + "1,1,0x123456789,0x0,41\n",
         "frames.csv line 2: id isn't"},
        /* 0xEF and two more bytes that aren't a byte-order mark's */
        {std::string("\xEF" "XY") + kFramesHeader + ok_row,
         "not a Havne frames.csv, or a newer format"},
        {std::string("\xEF\n") + ok_row,
         "not a Havne frames.csv, or a newer format"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        TempRecording rec(c.text);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(session, nullptr);
        EXPECT_EQ(std::string(ecuio_last_error_detail()).rfind(c.detail, 0),
                  size_t{0})
            << ecuio_last_error_detail();
    }

    {
        /* A byte-order mark ahead of the header, as an editor may add */
        TempRecording rec(std::string("\xEF\xBB\xBF") + kFramesHeader + ok_row);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_session_destroy(session);
    }

    std::string long_data, too_long;
    for (unsigned i = 0; i < ECUIO_FRAME_DATA_MAX; ++i)
        long_data += (i ? " " : "") + std::string("A5");
    too_long = long_data + " A5";
    {
        TempRecording rec(std::string(kFramesHeader) + "1,1,0x7E8,0x0," +
                          too_long + "\n");
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "frames.csv line 2: data is longer than a frame holds");
    }

    TempRecording crlf(std::string("host_us,timestamp_us,id,flags,data\r\n") +
                       "1,1,0x7E8,0x0," + long_data +
                       "\r\n2,2,0x7E8,0x0,41 0C 1F 40\r\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(crlf.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(cap.frames[0].data_length,
              static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX));
    EXPECT_EQ(cap.frames[0].data[ECUIO_FRAME_DATA_MAX - 1], 0xA5);
    EXPECT_EQ(cap.frames[1].data_length, 4u)
        << "CRLF read as an ending, not data";
    ecuio_session_destroy(session);

    ecuio_session_t* missing = nullptr;
#ifdef _WIN32
    const char* gone = "replay://C:/ecuio-definitely-missing/frames.csv";
#else
    const char* gone = "replay:///ecuio-definitely-missing/frames.csv";
#endif
    EXPECT_EQ(ecuio_session_create(gone, &missing), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "couldn't open the recording's frames.csv");
}

/* The path must be absolute; a folder means its frames.csv; speed, loop
 * and unknown keys are judged before the file is read */
/* ---- Fault injection on a replay (ADR-0041) ---- */

namespace {

/* A faulted replay of n rows at max speed, run to its end: the frames
 * delivered and the counts */
struct FaultedRun {
    std::vector<ecuio_frame_t> frames;
    ecuio_replay_faults_t counts{};
    uint32_t info_flags = 0;
};

FaultedRun run_faulted(const TempRecording& rec, const std::string& query,
                       uint64_t rows)
{
    FaultedRun r;
    ReplayCapture capture;
    ecuio_session_t* session = start_replay(rec.uri(query.c_str()), &capture);
    if (session == nullptr) {
        return r;
    }
    EXPECT_TRUE(wait_for(
        [&] {
            ecuio_replay_faults_t c{};
            ecuio_session_get_replay_faults(session, &c);
            return c.frames == rows &&
                   capture.count() == rows - c.dropped - c.overflowed;
        },
        10000))
        << query;
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_get_replay_faults(session, &r.counts), ECUIO_OK);
    ecuio_session_info_t info{};
    EXPECT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    r.info_flags = info.flags;
    ecuio_session_destroy(session);
    std::lock_guard<std::mutex> lock(capture.mutex);
    r.frames = capture.frames;
    return r;
}

/* Within six standard deviations of n * p: a bound no seed should miss */
void expect_rate(uint64_t got, uint64_t n, double p, const char* what)
{
    const double mean = n * p;
    const double sd = std::sqrt(n * p * (1 - p));
    EXPECT_GE(static_cast<double>(got), mean - 6 * sd) << what;
    EXPECT_LE(static_cast<double>(got), mean + 6 * sd) << what;
}

int bits_apart(const ecuio_frame_t& f, const uint8_t* want, uint16_t len)
{
    int n = 0;
    for (uint16_t i = 0; i < len; ++i) {
        uint8_t x = static_cast<uint8_t>(f.data[i] ^ want[i]);
        for (; x != 0; x &= static_cast<uint8_t>(x - 1)) {
            ++n;
        }
    }
    return n;
}

} // namespace

/* ?faults= and ?seed=: each kind once, a percent over 0 and up to 100
 * with three decimals at most, a stall's length 1 to 10000 ms, a seed
 * from 1; anything else refused, through the session too */
TEST(CoreTest, ReplayFaultsReadTheirQuery)
{
    const auto parses = [](const char* q, replay_faults* f = nullptr) {
        replay_faults scratch;
        replay_faults* out = f != nullptr ? f : &scratch;
        return replay_parse_faults(q, std::strlen(q), out);
    };
    replay_faults f;
    ASSERT_TRUE(parses("drop:2,overflow:0.5,corrupt:100,truncate:12.125,"
                       "stall:1:250",
                       &f));
    EXPECT_TRUE(f.any);
    EXPECT_EQ(f.rate[REPLAY_FAULT_DROP], 2000u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_OVERFLOW], 500u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_CORRUPT], 100000u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_TRUNCATE], 12125u);
    EXPECT_EQ(f.rate[REPLAY_FAULT_STALL], 1000u);
    EXPECT_EQ(f.stall_ms, 250u);
    EXPECT_TRUE(parses("stall:0.001:10000"));
    for (const char* bad :
         {"",           "drop",         "drop:",         "drop:0",
          "drop:0.000", "drop:100.001", "drop:101",      "drop:1.2345",
          "drop:-1",    "drop:1e2",     "drop: 1",       "drop:2,drop:3",
          "stall:5",    "stall:5:0",    "stall:5:10001", "drop:5:10",
          "nope:5",     "drop:5,",      ",drop:5",       "Drop:5"}) {
        EXPECT_FALSE(parses(bad)) << bad;
    }
    uint32_t seed = 0;
    EXPECT_TRUE(replay_parse_seed("4294967295", 10, &seed));
    EXPECT_EQ(seed, 4294967295u);
    for (const char* bad : {"0", "4294967296", "", "x", "-1", "1 "}) {
        EXPECT_FALSE(replay_parse_seed(bad, std::strlen(bad), &seed)) << bad;
    }

    TempRecording rec(paced_rows(2));
    for (const char* q :
         {"?faults=drop:0", "?faults=", "?seed=0", "?faults=drop:5&seed=x"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
    }
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?faults=drop:5").c_str(), &session),
              ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS, 0u);
    ecuio_session_destroy(session);
}

/* The same seed plays the same faults, frame for frame; another seed
 * others. A replay without faults counts its frames and nothing else. */
TEST(CoreTest, ReplayFaultsAreTheSameFromTheSameSeed)
{
    TempRecording rec(paced_rows(300, 1000));
    const std::string q =
        "?speed=max&faults=drop:10,overflow:5,corrupt:10,truncate:10";
    const FaultedRun a = run_faulted(rec, q + "&seed=7", 300);
    const FaultedRun b = run_faulted(rec, q + "&seed=7", 300);
    const FaultedRun c = run_faulted(rec, q + "&seed=8", 300);
    ASSERT_EQ(a.frames.size(), b.frames.size());
    for (size_t i = 0; i < a.frames.size(); ++i) {
        ASSERT_EQ(a.frames[i].data_length, b.frames[i].data_length) << i;
        EXPECT_EQ(std::memcmp(a.frames[i].data, b.frames[i].data,
                              a.frames[i].data_length),
                  0)
            << i;
        EXPECT_EQ(a.frames[i].flags, b.frames[i].flags) << i;
        EXPECT_EQ(a.frames[i].timestamp_us, b.frames[i].timestamp_us) << i;
    }
    EXPECT_EQ(std::memcmp(&a.counts, &b.counts, sizeof a.counts), 0);
    EXPECT_NE(std::memcmp(&a.counts, &c.counts, sizeof a.counts), 0)
        << "seed 8 drew the same counts as seed 7";

    const FaultedRun clean = run_faulted(rec, "?speed=max", 300);
    EXPECT_EQ(clean.frames.size(), 300u);
    EXPECT_EQ(clean.counts.frames, 300u);
    EXPECT_EQ(clean.counts.dropped + clean.counts.overflowed +
                  clean.counts.corrupted + clean.counts.truncated +
                  clean.counts.stalled,
              0u);
    EXPECT_EQ(clean.info_flags & ECUIO_SESSION_INFO_REPLAY_FAULTS, 0u);
}

/* Each kind does what it counts: a drop removes the frame, an overflow
 * too and flags the next one delivered, a corruption flips one bit, a
 * truncation shortens the data, a stall pushes every later stamp back;
 * each near its rate */
TEST(CoreTest, ReplayFaultsDoWhatTheyCount)
{
    constexpr uint64_t kRows = 1000;
    TempRecording rec(paced_rows(static_cast<int>(kRows), 1000));
    const uint8_t clean[4] = {0x41, 0x0C, 0x1F, 0x40};

    FaultedRun r = run_faulted(rec, "?speed=max&faults=drop:20", kRows);
    EXPECT_EQ(r.frames.size() + r.counts.dropped, kRows);
    expect_rate(r.counts.dropped, kRows, 0.2, "drop");
    for (const ecuio_frame_t& f : r.frames) {
        EXPECT_EQ(f.flags & ECUIO_FRAME_FLAG_OVERFLOW, 0u)
            << "a drop says nothing";
    }

    r = run_faulted(rec, "?speed=max&faults=overflow:20", kRows);
    EXPECT_EQ(r.frames.size() + r.counts.overflowed, kRows);
    expect_rate(r.counts.overflowed, kRows, 0.2, "overflow");
    uint64_t flagged = 0;
    for (const ecuio_frame_t& f : r.frames) {
        flagged += (f.flags & ECUIO_FRAME_FLAG_OVERFLOW) != 0 ? 1 : 0;
    }
    EXPECT_GT(flagged, 0u);
    EXPECT_LE(flagged, r.counts.overflowed) << "losses in a row flag one frame";
    /* a flagged frame's stamp is past a missing row's: rows are 1 ms apart */
    for (size_t i = 1; i < r.frames.size(); ++i) {
        if (r.frames[i].flags & ECUIO_FRAME_FLAG_OVERFLOW) {
            EXPECT_GE(r.frames[i].timestamp_us - r.frames[i - 1].timestamp_us,
                      2000u)
                << i;
        }
    }

    r = run_faulted(rec, "?speed=max&faults=corrupt:100", kRows);
    ASSERT_EQ(r.frames.size(), kRows);
    EXPECT_EQ(r.counts.corrupted, kRows);
    for (const ecuio_frame_t& f : r.frames) {
        ASSERT_EQ(f.data_length, 4u);
        EXPECT_EQ(bits_apart(f, clean, 4), 1);
    }

    r = run_faulted(rec, "?speed=max&faults=truncate:30", kRows);
    ASSERT_EQ(r.frames.size(), kRows);
    expect_rate(r.counts.truncated, kRows, 0.3, "truncate");
    uint64_t shorter = 0;
    for (const ecuio_frame_t& f : r.frames) {
        ASSERT_LE(f.data_length, 4u);
        if (f.data_length < 4) {
            ++shorter;
        }
        EXPECT_EQ(std::memcmp(f.data, clean, f.data_length), 0)
            << "a truncation keeps what's left as it was";
    }
    EXPECT_EQ(shorter, r.counts.truncated);

    r = run_faulted(rec, "?speed=max&faults=stall:5:40", kRows);
    ASSERT_EQ(r.frames.size(), kRows);
    expect_rate(r.counts.stalled, kRows, 0.05, "stall");
    /* the recording's own steps are 1 ms; each stall adds 40 */
    uint64_t pushed = 0;
    for (size_t i = 1; i < r.frames.size(); ++i) {
        const uint64_t step =
            r.frames[i].timestamp_us - r.frames[i - 1].timestamp_us;
        ASSERT_TRUE(step == 1000u || step == 41000u) << i << ": " << step;
        pushed += step == 41000u ? 1 : 0;
    }
    const bool first_stalled = r.frames[0].timestamp_us == 40000u;
    EXPECT_EQ(pushed + (first_stalled ? 1 : 0), r.counts.stalled);
}

/* A stall is paced: at speed 1 the frames after it come that much later */
TEST(CoreTest, ReplayFaultsStallThePace)
{
    TempRecording rec(paced_rows(3, 10000));
    ReplayCapture capture;
    const auto t0 = std::chrono::steady_clock::now();
    ecuio_session_t* session =
        start_replay(rec.uri("?faults=stall:100:150"), &capture);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return capture.count() == 3; }, 5000));
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           capture.arrived[2] - t0)
                           .count();
    EXPECT_GE(spent, 3 * 150 + 20) << "three stalls and the recording's 20 ms";
    EXPECT_EQ(capture.frames[2].timestamp_us, 3u * 150000u + 20000u);
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, ReplayJudgesItsPathAndQuery)
{
    TempRecording folder(paced_rows(2), /*as_folder=*/true);
    ASSERT_TRUE(folder.made_dir);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(folder.uri().c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_destroy(session);
    session = nullptr;
    ASSERT_EQ(
        ecuio_session_create((folder.uri() + "/?speed=0.1").c_str(), &session),
        ECUIO_OK)
        << "a trailing separator";
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_speed_milli, 100u);
    ecuio_session_destroy(session);

    for (const char* bad : {"replay://", "replay://?speed=1",
                            "replay://recordings/x", "replay://frames.csv"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(bad, &session), ECUIO_ERR_BAD_URI)
            << bad;
        EXPECT_EQ(session, nullptr);
    }
    for (const char* q :
         {"?speed=0", "?speed=0.05", "?speed=101", "?speed=1.2345", "?speed=1.",
          "?speed=.5", "?speed=fast", "?speed=", "?loop=2", "?loop=yes"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(folder.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
    }
    for (const char* q : {"?speed=100", "?speed=2.5", "?speed=max", "?loop=0",
                          "?loop=1", "?speed=1&speed=3"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(folder.uri(q).c_str(), &session),
                  ECUIO_OK)
            << q;
        ecuio_session_destroy(session);
    }
    session = nullptr;
    EXPECT_EQ(
        ecuio_session_create(folder.uri("?baud=500000").c_str(), &session),
        ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "replay:// takes no query key 'baud', only profile, did, "
                 "speed, loop, faults, seed, from and to");

    TempRecording empty_dir_marker("", /*as_folder=*/true);
    std::remove(empty_dir_marker.csv.c_str());
    session = nullptr;
    EXPECT_EQ(ecuio_session_create(empty_dir_marker.uri().c_str(), &session),
              ECUIO_ERR_LOAD_FAILED)
        << "a folder with no frames.csv";
}

/* Recorded replies decode under the profile the URI names: the defs
 * match by ID and mux key, with no request behind them */
TEST(CoreTest, ReplayDecodesUnderTheProfileItNames)
{
    TempRecording rec(paced_rows(2));
    ReplayCapture cap;
    ecuio_session_t* session =
        start_replay(rec.uri("?profile=obd2&speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    bool decoded = false;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (!ecuio_def_applies_to_frame(&d, &cap.frames[0]))
            continue;
        double v = 0.0;
        ASSERT_EQ(ecuio_decode_signal(&d, cap.frames[0].data,
                                      cap.frames[0].data_length, &v),
                  ECUIO_DECODE_OK);
        EXPECT_DOUBLE_EQ(v, 2000.0) << d.name << ": 0x1F40 / 4";
        decoded = true;
    }
    EXPECT_TRUE(decoded) << "41 0C is obd2's RPM";
    ecuio_session_destroy(session);
}

/* The second vehicle session's engine start (docs/bench-sessions/
 * 2026-10-25-vehicle.md, Phase 4), replayed under obd2: the values the
 * sheet reports come back out of the recorded replies. The fixture is
 * the recording scrubbed of its identification replies (ADR-0023), and
 * this test holds it to that. */
TEST(CoreTest, ReplayOfTheVehicleFixtureDecodesTheSheetsValues)
{
    const std::string dir =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    ReplayCapture cap;
    ecuio_session_t* session =
        start_replay("replay://" + dir + "?profile=obd2&speed=max", &cap);
    ASSERT_NE(session, nullptr);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    const size_t n = info.replay_frames;
    EXPECT_EQ(n, 7074u);
    EXPECT_EQ(info.replay_span_ms, 297446u);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= n; }, 10000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ASSERT_EQ(cap.frames.size(), n);

    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    /* The obd2 def reading a Service 01 PID */
    const auto def_for = [set](uint8_t pid) -> const ecuio_signal_def_t* {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].mux_length == 2 &&
                set->defs[i].mux_key == (0x4100u | pid)) {
                return &set->defs[i];
            }
        }
        return nullptr;
    };
    const ecuio_signal_def_t* rpm = def_for(0x0C);
    const ecuio_signal_def_t* coolant = def_for(0x05);
    const ecuio_signal_def_t* volts = def_for(0x42);
    ASSERT_TRUE(rpm && coolant && volts);

    double rpm_max = 0, volts_min = 1e9, coolant_min = 1e9, coolant_max = -1e9;
    double first_rpm_s = -1;
    size_t identification = 0;
    for (const ecuio_frame_t& f : cap.frames) {
        identification +=
            (f.data_length >= 1 && f.data[0] == 0x49) ||
            (f.data_length >= 2 && f.data[0] == 0x62 && f.data[1] == 0xF1);
        double v = 0;
        if (ecuio_def_applies_to_frame(rpm, &f) &&
            ecuio_decode_signal(rpm, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            rpm_max = std::max(rpm_max, v);
            if (v > 0 && first_rpm_s < 0)
                first_rpm_s = f.timestamp_us / 1e6;
        }
        if (ecuio_def_applies_to_frame(volts, &f) &&
            ecuio_decode_signal(volts, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            volts_min = std::min(volts_min, v);
        }
        if (ecuio_def_applies_to_frame(coolant, &f) &&
            ecuio_decode_signal(coolant, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            coolant_min = std::min(coolant_min, v);
            coolant_max = std::max(coolant_max, v);
        }
    }
    EXPECT_EQ(identification, 0u)
        << "the fixture carries no identification reply";
    EXPECT_NEAR(rpm_max, 1261.25, 0.01) << "the sheet's 1,261 peak";
    EXPECT_NEAR(volts_min, 9.564, 0.0005)
        << "the sheet's 9.56 V dip at the starter";
    EXPECT_EQ(coolant_min, 60.0) << "the sheet's 60 to 86 degrees C";
    EXPECT_EQ(coolant_max, 86.0);
    EXPECT_NEAR(first_rpm_s, 3.45, 0.01)
        << "the start, 3.45 s into the recording";
    EXPECT_EQ(cap.frames.back().timestamp_us, 297446075u)
        << "the recording's span, rebased";
    ecuio_session_destroy(session);
}

/* ---- Decoded values (ADR-0024) ------------------------------------- */

namespace {

/* One delivered value, copied out of its batch */
struct SeenValue {
    uint16_t id = 0;
    uint8_t kind = 0;
    double value = 0.0;
    std::string text;
    const char* text_at = nullptr; /* where the batch held it */
    uint32_t text_len = 0;
};

struct SeenBatch {
    uint64_t timestamp_us = 0;
    uint32_t arbitration_id = 0;
    std::vector<SeenValue> values;
};

/* Every batch a values callback received */
struct ValuesCapture {
    std::mutex mutex;
    std::vector<SeenBatch> batches;
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return batches.size();
    }
};

void values_capture_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    SeenBatch b;
    b.timestamp_us = batch->timestamp_us;
    b.arbitration_id = batch->arbitration_id;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        SeenValue s;
        s.id = v.signal_id;
        s.kind = v.kind;
        s.value = v.value;
        s.text_at = v.text;
        s.text_len = v.text_len;
        if (v.text != nullptr)
            s.text = v.text;
        b.values.push_back(s);
    }
    auto* c = static_cast<ValuesCapture*>(user_data);
    std::lock_guard<std::mutex> lock(c->mutex);
    c->batches.push_back(std::move(b));
}

void values_counting_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++*static_cast<int*>(user_data);
}

ecuio_signal_def_t value_def(uint16_t id, uint8_t kind, uint32_t message_id)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.kind = kind;
    d.message_id = message_id;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof(d.name), "V%u", static_cast<unsigned>(id));
    return d;
}

/* A text def reading byte_length bytes from start, keyed when key_len */
ecuio_signal_def_t text_value_def(uint16_t id, uint8_t kind, uint8_t key_len,
                                  uint32_t key, uint16_t start,
                                  uint16_t byte_length)
{
    ecuio_signal_def_t d = value_def(id, kind, 0x123);
    d.mux_length = key_len;
    d.mux_key = key;
    d.start_byte = start;
    d.byte_length = byte_length;
    return d;
}

ecuio_frame_t value_frame(uint32_t id, std::initializer_list<uint8_t> bytes,
                          uint64_t timestamp_us = 0)
{
    ecuio_frame_t f{};
    f.arbitration_id = id;
    f.timestamp_us = timestamp_us;
    for (uint8_t b : bytes)
        f.data[f.data_length++] = b;
    return f;
}

} // namespace

/* A frame's batch holds every def it decodes, in the set's order, with
 * the values ecuio_decode_signal gives and the frame's time and ID; a
 * def that matches but can't decode (past the payload) is left out; a
 * frame that decodes nothing, and a raw session, deliver no batch */
TEST(CoreTest, ValuesCallbackDeliversEachFramesDecodeWithItsTime)
{
    static ecuio_signal_def_t defs[4];
    defs[0] = value_def(1, ECUIO_VALUE_UINT, 0x123);
    defs[0].bit_length = 8;
    defs[1] = value_def(2, ECUIO_VALUE_UINT, 0x456); /* another ID */
    defs[1].bit_length = 8;
    defs[2] = value_def(3, ECUIO_VALUE_UINT, 0x123);
    defs[2].byte_order = ECUIO_BYTE_ORDER_BE;
    defs[2].start_byte = 1;
    defs[2].bit_length = 16;
    defs[2].scale = 0.5;
    defs[3] = value_def(4, ECUIO_VALUE_UINT, 0x123); /* past 3 bytes */
    defs[3].start_byte = 5;
    defs[3].bit_length = 8;
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 4;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-values", &set, nullptr), ECUIO_OK);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://v?profile=test-values", &session),
              ECUIO_OK);
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t f = value_frame(0x123, {10, 0x01, 0x00}, 777);
    ecuio_session_dispatch_callbacks(session, &f);
    const ecuio_frame_t other = value_frame(0x999, {1, 2, 3});
    ecuio_session_dispatch_callbacks(session, &other);

    ASSERT_EQ(cap.batches.size(), 1u)
        << "no batch for a frame that decodes nothing";
    const SeenBatch& b = cap.batches[0];
    EXPECT_EQ(b.timestamp_us, 777u);
    EXPECT_EQ(b.arbitration_id, 0x123u);
    ASSERT_EQ(b.values.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        const ecuio_signal_def_t& d = defs[i == 0 ? 0 : 2];
        double expected = 0.0;
        ASSERT_EQ(ecuio_decode_signal(&d, f.data, f.data_length, &expected),
                  ECUIO_DECODE_OK);
        EXPECT_EQ(b.values[i].id, d.id);
        EXPECT_EQ(b.values[i].kind, ECUIO_VALUE_UINT);
        EXPECT_EQ(b.values[i].value, expected);
        EXPECT_EQ(b.values[i].text_at, nullptr);
        EXPECT_EQ(b.values[i].text_len, 0u);
    }
    EXPECT_EQ(b.values[1].value, 128.0);
    ecuio_session_destroy(session);

    ASSERT_EQ(ecuio_session_create("mock://v?profile=raw", &session), ECUIO_OK);
    ValuesCapture none;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &none),
              ECUIO_OK);
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(none.batches.size(), 0u) << "raw decodes nothing";
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-values"), ECUIO_OK);
}

/* The frame callback's registration rules: a NULL argument refused, a
 * pair registered twice called once, unregister idempotent, no
 * registering on a closed session */
TEST(CoreTest, ValuesCallbackRegistersAsTheFrameCallbackDoes)
{
    ecuio_session_t* session = make_test_session();
    int batches = 0;
    EXPECT_EQ(
        ecuio_register_values_callback(nullptr, values_counting_cb, &batches),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_register_values_callback(session, nullptr, &batches),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_unregister_values_callback(nullptr, values_counting_cb, &batches),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_register_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    EXPECT_EQ(
        ecuio_register_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);

    /* The mock set's telemetry frame */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    ecuio_frame_t f{};
    f.arbitration_id = set->defs[0].message_id;
    f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(batches, 1) << "deduplicated";

    EXPECT_EQ(
        ecuio_unregister_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    EXPECT_EQ(
        ecuio_unregister_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(batches, 1) << "unregistered";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(
        ecuio_register_values_callback(session, values_counting_cb, &batches),
        ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(
        ecuio_unregister_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    ecuio_session_destroy(session);
}

namespace {

/* The frame callbacks' count when each values callback ran, and a
 * values callback the first frame callback registers */
struct OrderCtx {
    ecuio_session_t* session = nullptr;
    int frames = 0;
    std::vector<int> frames_at_values;
    int late_batches = 0;
};

void late_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<OrderCtx*>(user_data)->late_batches;
}

void order_frame_cb(const ecuio_frame_t*, void* user_data)
{
    auto* c = static_cast<OrderCtx*>(user_data);
    if (++c->frames == 1)
        ecuio_register_values_callback(c->session, late_values_cb, c);
}

void order_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    auto* c = static_cast<OrderCtx*>(user_data);
    c->frames_at_values.push_back(c->frames);
}

} // namespace

/* Each frame reaches the frame callbacks before its values reach the
 * values callbacks, and a values callback registered by a frame
 * callback starts with the next frame: the lists are taken as they
 * stand when the frame arrives */
TEST(CoreTest, ValuesFollowTheFrameCallbacks)
{
    ecuio_session_t* session = make_test_session();
    OrderCtx c;
    c.session = session;
    EXPECT_EQ(ecuio_register_frame_callback(session, order_frame_cb, &c),
              ECUIO_OK);
    EXPECT_EQ(ecuio_register_values_callback(session, order_values_cb, &c),
              ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    ecuio_frame_t f{};
    f.arbitration_id = set->defs[0].message_id;
    f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    ecuio_session_dispatch_callbacks(session, &f);
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(c.frames_at_values, (std::vector<int>{1, 2}))
        << "the frame callback ran first both times";
    EXPECT_EQ(c.late_batches, 1) << "from the second frame only";
    ecuio_session_destroy(session);
}

namespace {

/* The first values callback unregisters the second and registers a
 * third, on the first batch only */
struct ValuesSwap {
    ecuio_session_t* session = nullptr;
    int removed = 0;
    int added = 0;
    bool swapped = false;
};

void values_removed_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<ValuesSwap*>(user_data)->removed;
}

void values_added_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<ValuesSwap*>(user_data)->added;
}

void values_swapping_cb(const ecuio_value_batch_t*, void* user_data)
{
    auto* c = static_cast<ValuesSwap*>(user_data);
    if (!c->swapped) {
        c->swapped = true;
        ecuio_unregister_values_callback(c->session, values_removed_cb, c);
        ecuio_register_values_callback(c->session, values_added_cb, c);
    }
}

} // namespace

/* As for the frame callbacks: a values callback removed mid-dispatch
 * still gets that frame's batch, one added doesn't, the next frame the
 * reverse, on a list that fits the stack and one that doesn't */
TEST(CoreTest, ValuesReachTheListAsItStoodWhenTheFrameArrived)
{
    for (int fillers : {0, 4}) {
        SCOPED_TRACE(fillers);
        ecuio_session_t* session = make_test_session();
        ValuesSwap c;
        c.session = session;
        int filler_batches[4] = {};
        EXPECT_EQ(
            ecuio_register_values_callback(session, values_swapping_cb, &c),
            ECUIO_OK);
        EXPECT_EQ(
            ecuio_register_values_callback(session, values_removed_cb, &c),
            ECUIO_OK);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(ecuio_register_values_callback(
                          session, values_counting_cb, &filler_batches[i]),
                      ECUIO_OK);
        }
        const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
        ASSERT_NE(set, nullptr);
        ecuio_frame_t f{};
        f.arbitration_id = set->defs[0].message_id;
        f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
        ecuio_session_dispatch_callbacks(session, &f);
        EXPECT_EQ(c.removed, 1) << "in the snapshot";
        EXPECT_EQ(c.added, 0) << "not in it";
        ecuio_session_dispatch_callbacks(session, &f);
        EXPECT_EQ(c.removed, 1);
        EXPECT_EQ(c.added, 1);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(filler_batches[i], 2) << "filler " << i;
        }
        ecuio_session_destroy(session);
    }
}

/* Every text value of a batch has bytes of its own (ADR-0024 section 4): an
 * unkeyed def, two defs sharing a one-byte key and one with a two-byte
 * key all match "ABCD", four text values at once; a def keyed on
 * another byte doesn't match. The next frame starts from the first slot
 * again */
TEST(CoreTest, ValuesGiveEachTextValueASlotOfItsOwn)
{
    static ecuio_signal_def_t defs[5];
    defs[0] = text_value_def(1, ECUIO_VALUE_HEX, 0, 0, 0, 2);
    defs[1] = text_value_def(2, ECUIO_VALUE_TEXT, 1, 0x41, 2, 1);
    defs[2] = text_value_def(3, ECUIO_VALUE_TEXT, 1, 0x41, 3, 1);
    defs[3] = text_value_def(4, ECUIO_VALUE_TEXT, 2, 0x4142, 2, 2);
    defs[4] = text_value_def(5, ECUIO_VALUE_TEXT, 1, 0x5A, 1, 1);
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 5;
    set.defs = defs;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-text-slots", &set, &why), ECUIO_OK)
        << "code " << why.code << " at def " << why.index;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-text-slots", &session),
        ECUIO_OK);
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t abcd = value_frame(0x123, {'A', 'B', 'C', 'D'});
    const ecuio_frame_t abef = value_frame(0x123, {'A', 'B', 'E', 'F'});
    ecuio_session_dispatch_callbacks(session, &abcd);
    ecuio_session_dispatch_callbacks(session, &abef);
    ASSERT_EQ(cap.batches.size(), 2u);

    const std::vector<SeenValue>& v = cap.batches[0].values;
    ASSERT_EQ(v.size(), 4u) << "a slot for every text value the frame decodes";
    const char* const expected[] = {"41 42", "C", "D", "CD"};
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(v[i].id, i + 1);
        EXPECT_EQ(v[i].text, expected[i]) << "value " << i;
        EXPECT_EQ(v[i].text_len, std::strlen(expected[i]));
        EXPECT_EQ(v[i].value, 0.0);
        for (size_t j = 0; j < i; ++j) {
            const std::ptrdiff_t apart = v[i].text_at - v[j].text_at;
            EXPECT_GE(std::abs(apart),
                      static_cast<std::ptrdiff_t>(ECUIO_DECODE_TEXT_MAX))
                << "values " << j << " and " << i << " share bytes";
        }
    }
    const std::vector<SeenValue>& next = cap.batches[1].values;
    ASSERT_EQ(next.size(), 4u);
    EXPECT_EQ(next[1].text, "E");
    EXPECT_EQ(next[3].text, "EF");
    EXPECT_EQ(next[0].text_at, v[0].text_at) << "the slots start again";
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-text-slots"), ECUIO_OK);
}

namespace {

/* MAP on 0x123's byte 0, a fuel-system enum on its byte 1 (1 and 2
 * labeled), baro on 0x124's byte 0; then boost (MAP - baro, baro aged
 * up to 30 s) and a ratio (MAP / baro, both the stale age) */
struct ComputedSet {
    ecuio_signal_def_t defs[5];
    ecuio_enum_entry_t enums[2];
    ecuio_computed_t computed[2];
    ecuio_signal_set_t set{};

    ComputedSet()
    {
        defs[0] = value_def(1, ECUIO_VALUE_UINT, 0x123);
        defs[0].bit_length = 8;
        defs[1] = value_def(2, ECUIO_VALUE_ENUM, 0x123);
        defs[1].start_byte = 1;
        defs[1].bit_length = 8;
        defs[1].table_count = 2;
        defs[2] = value_def(3, ECUIO_VALUE_UINT, 0x124);
        defs[2].bit_length = 8;
        for (uint16_t i = 3; i < 5; ++i) {
            defs[i] = value_def(static_cast<uint16_t>(i + 1), ECUIO_VALUE_UINT, 0);
            defs[i].flags = ECUIO_DEF_FLAG_COMPUTED;
            defs[i].table_first = static_cast<uint16_t>(i - 3);
        }
        enums[0].raw = 1;
        std::snprintf(enums[0].label, sizeof enums[0].label, "Open loop");
        enums[1].raw = 2;
        std::snprintf(enums[1].label, sizeof enums[1].label, "Closed loop");
        for (uint32_t p = 0; p < 2; ++p) {
            ecuio_computed_t& c = computed[p];
            c = ecuio_computed_t{};
            c.input_ids[0] = 1;
            c.input_ids[1] = 3;
            c.input_count = 2;
            c.step_count = 3;
            c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
            c.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
            c.steps[2] = p == 0 ? ECUIO_COMPUTED_SUBTRACT : ECUIO_COMPUTED_DIVIDE;
        }
        computed[0].max_age_us[1] = 30000000;
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 5;
        set.defs = defs;
        set.enum_count = 2;
        set.enums = enums;
        set.computed_count = 2;
        set.computed = computed;
    }
};

const SeenValue* value_of(const SeenBatch& b, uint16_t id)
{
    for (const SeenValue& v : b.values) {
        if (v.id == id) {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

/* A computed value follows a frame that decodes one of its inputs,
 * from each input's newest value no older than its bound, after the
 * frame's own values (ADR-0051 section 5); an enum's value is its raw
 * number with its label, or none for a raw value with no entry */
TEST(CoreTest, ComputedValuesFollowTheirInputsWithinTheirAges)
{
    static ComputedSet s;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-computed", &s.set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://t?profile=test-computed", &session),
              ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const auto send = [&](uint32_t id, std::initializer_list<uint8_t> bytes,
                          double seconds) {
        const ecuio_frame_t f =
            value_frame(id, bytes, static_cast<uint64_t>(seconds * 1e6));
        ecuio_session_dispatch_callbacks(session, &f);
    };
    send(0x123, {100, 1}, 0.0);    /* 0: MAP, no baro yet */
    send(0x124, {95}, 1.0);        /* 1: baro, MAP 1 s old */
    send(0x123, {110, 7}, 2.0);    /* 2: an unlabeled mode */
    send(0x123, {120, 2}, 20.0);   /* 3: baro 19 s old: boost's 30 s, not the ratio's 3 s */
    send(0x123, {130, 2}, 40.0);   /* 4: baro 39 s old: neither */
    send(0x124, {0}, 41.0);        /* 5: a zero baro: no ratio */
    send(0x125, {1}, 42.0);        /* 6: decodes nothing, feeds nothing */
    ASSERT_EQ(cap.batches.size(), 6u);
    const std::vector<SeenBatch>& b = cap.batches;

    ASSERT_EQ(b[0].values.size(), 2u) << "MAP and the mode alone";
    EXPECT_EQ(b[0].values[1].text, "Open loop");
    EXPECT_EQ(b[0].values[1].value, 1.0);
    EXPECT_EQ(b[0].values[1].kind, ECUIO_VALUE_ENUM);
    EXPECT_EQ(b[0].values[1].text_len, 9u);

    ASSERT_EQ(b[1].values.size(), 3u);
    EXPECT_EQ(b[1].values[0].id, 3) << "the frame's own value first";
    EXPECT_EQ(b[1].values[1].id, 4);
    EXPECT_EQ(b[1].values[1].value, 5.0);
    EXPECT_EQ(b[1].values[1].kind, ECUIO_VALUE_UINT);
    EXPECT_EQ(b[1].values[1].text_at, nullptr);
    EXPECT_EQ(b[1].values[2].id, 5);
    EXPECT_DOUBLE_EQ(b[1].values[2].value, 100.0 / 95.0);
    EXPECT_EQ(b[1].timestamp_us, 1000000u);

    ASSERT_EQ(b[2].values.size(), 4u);
    EXPECT_EQ(b[2].values[1].value, 7.0);
    EXPECT_EQ(b[2].values[1].text_at, nullptr) << "no label: the number shows";
    EXPECT_EQ(b[2].values[1].text_len, 0u);
    EXPECT_EQ(value_of(b[2], 4)->value, 15.0);

    ASSERT_NE(value_of(b[3], 4), nullptr) << "baro's own bound, 30 s";
    EXPECT_EQ(value_of(b[3], 4)->value, 25.0);
    EXPECT_EQ(value_of(b[3], 5), nullptr) << "the stale age, 3 s";
    EXPECT_EQ(value_of(b[3], 2)->text, "Closed loop");

    EXPECT_EQ(value_of(b[4], 4), nullptr) << "39 s is past 30";
    EXPECT_EQ(b[4].values.size(), 2u);

    ASSERT_NE(value_of(b[5], 4), nullptr) << "MAP 1 s old";
    EXPECT_EQ(value_of(b[5], 4)->value, 130.0);
    EXPECT_EQ(value_of(b[5], 5), nullptr) << "a division by zero";

    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed"), ECUIO_OK);
}

/* A computed value's age bound is inclusive, and an input decoded
 * later than the frame being computed (a clock that stepped back)
 * gives no value */
TEST(CoreTest, AComputedInputAtItsBoundStillCounts)
{
    static ComputedSet s;
    ASSERT_EQ(ecuio_profile_register("test-computed-age", &s.set, nullptr),
              ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-computed-age", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t baro = value_frame(0x124, {100}, 10000000);
    const ecuio_frame_t at = value_frame(0x123, {110, 1},
                                         10000000 + ECUIO_VALUE_STALE_US);
    const ecuio_frame_t past = value_frame(0x123, {111, 1},
                                           10000001 + ECUIO_VALUE_STALE_US);
    const ecuio_frame_t back = value_frame(0x124, {100}, 20000000);
    const ecuio_frame_t earlier = value_frame(0x123, {112, 1}, 15000000);
    for (const ecuio_frame_t* f : {&baro, &at, &past, &back, &earlier}) {
        ecuio_session_dispatch_callbacks(session, f);
    }
    ASSERT_EQ(cap.batches.size(), 5u);
    ASSERT_NE(value_of(cap.batches[1], 5), nullptr) << "exactly 3 s";
    EXPECT_DOUBLE_EQ(value_of(cap.batches[1], 5)->value, 1.1);
    EXPECT_EQ(value_of(cap.batches[2], 5), nullptr) << "1 us past it";
    EXPECT_NE(value_of(cap.batches[2], 4), nullptr) << "boost's 30 s";
    EXPECT_EQ(value_of(cap.batches[4], 4), nullptr) << "baro newer than the frame";
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-age"), ECUIO_OK);
}

/* Computing allocates nothing on the worker, as decoding doesn't
 * (CoreTest.WorkersAllocateNothingPerFrame's rule) */
TEST(CoreTest, ComputingValuesAllocatesNothing)
{
    static ComputedSet s;
    ASSERT_EQ(ecuio_profile_register("test-computed-new", &s.set, nullptr),
              ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-computed-new", &session),
        ECUIO_OK);
    int batches = 0;
    ASSERT_EQ(ecuio_register_values_callback(session, values_counting_cb, &batches),
              ECUIO_OK);
    const ecuio_frame_t map = value_frame(0x123, {120, 2}, 1000000);
    const ecuio_frame_t baro = value_frame(0x124, {100}, 1100000);
    ecuio_session_dispatch_callbacks(session, &map);
    const unsigned long before = new_counter::on_this_thread();
    ecuio_session_dispatch_callbacks(session, &baro);
    ecuio_session_dispatch_callbacks(session, &map);
    EXPECT_EQ(new_counter::on_this_thread() - before, 0u);
    EXPECT_EQ(batches, 3);
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-new"), ECUIO_OK);
}

/* A packed Service 01 reply (ADR-0039 Appendix A) decodes record by
 * record, and its computed values follow once, after every record: MAP
 * (0x0B) and baro (0x33) in one reply give one boost, from that reply's
 * baro, not one per record from the baro before it */
TEST(CoreTest, AComputedValueFollowsAPackedReplyOnce)
{
    static ecuio_signal_def_t defs[3];
    const uint8_t pids[2] = {0x0B, 0x33};
    for (uint16_t i = 0; i < 2; ++i) {
        defs[i] = value_def(static_cast<uint16_t>(i + 1), ECUIO_VALUE_UINT,
                            ECUIO_OBD2_ECU_ARB_ID);
        defs[i].byte_order = ECUIO_BYTE_ORDER_BE;
        defs[i].start_byte = 2;
        defs[i].bit_length = 8;
        defs[i].mux_length = 2;
        defs[i].mux_key = 0x4100u | pids[i];
    }
    defs[2] = value_def(3, ECUIO_VALUE_UINT, 0);
    defs[2].flags = ECUIO_DEF_FLAG_COMPUTED;
    static ecuio_computed_t boost{};
    boost.input_ids[0] = 1;
    boost.input_ids[1] = 2;
    boost.input_count = 2;
    boost.step_count = 3;
    boost.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
    boost.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
    boost.steps[2] = ECUIO_COMPUTED_SUBTRACT;
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    set.computed_count = 1;
    set.computed = &boost;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-computed-packed", &set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://t?profile=test-computed-packed",
                                   &session),
              ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t baro =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x33, 90}, 1000000);
    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0B, 100, 0x33, 95}, 2000000);
    ecuio_session_dispatch_callbacks(session, &baro);
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.batches.size(), 2u);
    const std::vector<SeenValue>& v = cap.batches[1].values;
    ASSERT_EQ(v.size(), 3u) << "MAP, baro, then one boost";
    EXPECT_EQ(v[0].id, 1);
    EXPECT_EQ(v[0].value, 100.0);
    EXPECT_EQ(v[1].id, 2);
    EXPECT_EQ(v[1].value, 95.0);
    EXPECT_EQ(v[2].id, 3);
    EXPECT_EQ(v[2].value, 5.0) << "from this reply's baro";
    EXPECT_EQ(cap.batches[1].timestamp_us, 2000000u);
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-packed"), ECUIO_OK);
}

/* Reply timing (ADR-0042): a session with no rotation has no rows and
 * no clock, the count asked with capacity 0, and the NULLs refused */
TEST(CoreTest, ASessionWithNoRotationHasNoTimingRows)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://a?profile=obd2", &session),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ecuio_session_timing_t t{};
    t.rows = 99;
    ecuio_request_timing_t row{};
    uint32_t count = 99;
    EXPECT_EQ(ecuio_session_get_timing(session, &t, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(t.rows, 0u);
    EXPECT_EQ(t.interval_clock, ECUIO_TIMING_CLOCK_NONE);
    EXPECT_EQ(t.flags, 0u);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, &row, 1, &count), ECUIO_OK);
    EXPECT_EQ(ecuio_session_get_timing(nullptr, &t, &row, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_timing(session, nullptr, &row, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, &row, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_close(session);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, nullptr, 0, &count),
              ECUIO_OK)
        << "readable after close";
    ecuio_session_destroy(session);
}

/* ADR-0039 section 6: the test car answers PID 55 with bank 1's byte alone
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md), so the bank 3
 * field is absent once ECUIO_ABSENT_AFTER_FRAMES replies have ended
 * before it, and leaves the list when a reply carries it. Bank 1 is
 * never absent; a session with no values callback judges nothing; the
 * list stays readable after close. */
TEST(CoreTest, AFieldPastEveryReplyIsAbsent)
{
    const uint16_t b3 = ECUIO_OBD2_PID_SIG_ID(0x55, 1);
    const ecuio_frame_t one =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x55, 0x80});
    const ecuio_frame_t both =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x55, 0x80, 0x7F});
    uint16_t ids[4] = {};
    uint32_t count = 99;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://a?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    for (uint32_t k = 1; k <= ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &one);
        ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
                  ECUIO_OK);
        EXPECT_EQ(count, k < ECUIO_ABSENT_AFTER_FRAMES ? 0u : 1u)
            << "after " << k << " short replies";
    }
    EXPECT_EQ(ids[0], b3);
    EXPECT_EQ(ecuio_session_get_absent_signals(session, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u) << "capacity 0 asks the count";
    EXPECT_EQ(ecuio_session_get_absent_signals(session, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_session_close(session);
    EXPECT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u) << "readable after close";
    ecuio_session_destroy(session);

    ASSERT_EQ(
        ecuio_session_create("mock://b?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &one);
    }
    ecuio_session_dispatch_callbacks(session, &both);
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "a reply that carries it clears it";
    ecuio_session_destroy(session);

    ASSERT_EQ(
        ecuio_session_create("mock://c?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &one);
    }
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "no values callback, nothing decoded or judged";
    ecuio_session_destroy(session);
}

/* ADR-0027 section 3: the VIN a vehicle is known by: 09 02's text, else
 * F190's; letters and digits only, upper case, at 17 characters */
TEST(CoreTest, TheVinIsService09sThenF190s)
{
    const auto item = [](std::initializer_list<uint8_t> request,
                         uint8_t outcome, const std::string& text) {
        ecuio_identity_item_t it{};
        uint8_t k = 0;
        for (uint8_t b : request) {
            it.request[k++] = b;
        }
        it.length = k;
        it.outcome = outcome;
        it.text_length = static_cast<uint8_t>(text.size());
        memcpy(it.text, text.data(), text.size());
        return it;
    };
    const auto vin = [](const ecuio_channel_identity_t& id) {
        char out[ECUIO_VIN_TEXT_LEN] = "x";
        EXPECT_EQ(ecuio_identity_vin(&id, out, sizeof out), ECUIO_OK);
        return std::string(out);
    };
    ecuio_channel_identity_t id{};
    id.flags = ECUIO_IDENTITY_COMPLETE;
    id.items = 2;
    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "f190vin0000000002");
    id.list[1] = item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED,
                      std::string(2, '\0') + " FAKEJ2534VIN00001");
    EXPECT_EQ(vin(id), "FAKEJ2534VIN00001")
        << "09 02 first, its padding dropped";

    id.list[1].outcome = ECUIO_IDENTITY_REFUSED;
    EXPECT_EQ(vin(id), "F190VIN0000000002")
        << "F190 when 09 02 wasn't answered, in upper case";

    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN0001");
    EXPECT_EQ(vin(id), "") << "16 characters";
    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN000012");
    EXPECT_EQ(vin(id), "") << "18 characters";

    id.list[0] =
        item({0x09, 0x04}, ECUIO_IDENTITY_ANSWERED, "CALIBRATION00001X");
    EXPECT_EQ(vin(id), "") << "a CALID of 17 isn't a VIN";

    id.items = 1;
    id.list[1] =
        item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN00001");
    EXPECT_EQ(vin(id), "") << "past the count isn't read";

    char small[ECUIO_VIN_TEXT_LEN - 1] = "x";
    EXPECT_EQ(ecuio_identity_vin(&id, small, sizeof small),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(small[0], 'x') << "untouched";
    EXPECT_EQ(ecuio_identity_vin(nullptr, small, sizeof small),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_identity_vin(&id, nullptr, ECUIO_VIN_TEXT_LEN),
              ECUIO_ERR_NULL_ARGUMENT);
}

/* ecuio_session_get_unsupported_signals' contract: the mock's channel
 * has learned nothing (generation 0), so none, before start and after
 * close; NULLs refused as the absent signals' are. The judgment itself
 * is PidSupportTest's (test_obd2), and a session's through a fake ECU
 * Elm327SessionTest.TheSignalsOfAWithheldPidAreUnsupported */
TEST(CoreTest, TheMockHasNoUnsupportedSignals)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://a?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    uint16_t ids[4] = {};
    uint32_t count = 99;
    ASSERT_EQ(ecuio_session_get_unsupported_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(
        ecuio_session_get_unsupported_signals(session, nullptr, 0, &count),
        ECUIO_OK);
    EXPECT_EQ(
        ecuio_session_get_unsupported_signals(session, nullptr, 1, &count),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unsupported_signals(session, ids, 4, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unsupported_signals(nullptr, ids, 4, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_session_close(session);
    count = 99;
    EXPECT_EQ(ecuio_session_get_unsupported_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "readable after close";
    ecuio_session_destroy(session);
}

namespace {

/* The ids and values of a batch, in its order */
std::vector<std::pair<uint16_t, double>> id_values(const SeenBatch& b)
{
    std::vector<std::pair<uint16_t, double>> out;
    for (const SeenValue& v : b.values) {
        out.push_back({v.id, v.value});
    }
    return out;
}

uint64_t unsplit(const ecuio_session_t* session)
{
    uint64_t n = 99;
    EXPECT_EQ(ecuio_session_get_unsplit_replies(session, &n), ECUIO_OK);
    return n;
}

} // namespace

/* ADR-0039 Appendix A: a Service 01 reply carrying several PIDs decodes
 * record by record, each def once, in the order the ECU sent them; one
 * that won't split decodes its first record and is counted; a frame on
 * an ID no Service 01 def listens on is never split */
TEST(CoreTest, APackedReplyDecodesEachRecord)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://p?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    EXPECT_EQ(unsplit(session), 0u);

    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0D, 0x32, 0x0C, 0x1A, 0xF8, 0x05, 0x5A});
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.count(), 1u);
    EXPECT_EQ(id_values(cap.batches[0]),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_SPEED_ID, 50.0},
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0},
                  {ECUIO_OBD2_SIG_COOLANT_TEMP_ID, 50.0}}));
    EXPECT_EQ(cap.batches[0].arbitration_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(unsplit(session), 0u);

    const ecuio_frame_t stuck = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x1A, 0xF8, 0xB0, 0x01});
    ecuio_session_dispatch_callbacks(session, &stuck);
    ASSERT_EQ(cap.count(), 2u);
    EXPECT_EQ(id_values(cap.batches[1]),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0}}))
        << "its first record alone";
    EXPECT_EQ(unsplit(session), 1u);

    const ecuio_frame_t elsewhere =
        value_frame(0x7E9, {0x41, 0x0D, 0x32, 0x0C, 0x1A, 0xF8});
    ecuio_session_dispatch_callbacks(session, &elsewhere);
    EXPECT_EQ(cap.count(), 2u) << "no def listens on 0x7E9";
    EXPECT_EQ(unsplit(session), 1u);

    ecuio_session_close(session);
    EXPECT_EQ(unsplit(session), 1u) << "readable after close";
    uint64_t n = 0;
    EXPECT_EQ(ecuio_session_get_unsplit_replies(nullptr, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unsplit_replies(session, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(session);
}

/* The test car's PID 55 carries bank 1's byte alone: its single replies
 * teach that length, so a packed reply holding it splits, and bank 3's
 * field, past every record, is still judged absent */
TEST(CoreTest, APackedReplySplitsByTheLengthTheEcuSends)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://p?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t alone =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x55, 0x80});
    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x1A, 0xF8, 0x55, 0x80, 0x0D, 0x32});
    ecuio_session_dispatch_callbacks(session, &alone);
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &packed);
    }
    ASSERT_EQ(cap.count(), 1u + ECUIO_ABSENT_AFTER_FRAMES);
    EXPECT_EQ(id_values(cap.batches.back()),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0},
                  {ECUIO_OBD2_PID_SIG_ID(0x55, 0), 0.0},
                  {ECUIO_OBD2_SIG_SPEED_ID, 50.0}}));
    EXPECT_EQ(unsplit(session), 0u);
    uint16_t ids[4] = {};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(ids[0], ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ecuio_session_destroy(session);
}

/* A recording keeps a packed reply as the ECU sent it, so its replay
 * decodes it the same way, the earlier single reply's length included */
TEST(CoreTest, APackedReplyReplaysToItsRecords)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "10,1000000,0x7E8,0x0,41 55 80\n"
                      "600,1000500,0x7E8,0x0,41 0C 1A F8 55 80 0D 32\n");
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?profile=obd2&speed=max").c_str(),
                                   &session),
              ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ecuio_session_close(session);
    std::lock_guard<std::mutex> lock(cap.mutex);
    ASSERT_EQ(cap.batches.size(), 2u);
    EXPECT_EQ(id_values(cap.batches[1]),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0},
                  {ECUIO_OBD2_PID_SIG_ID(0x55, 0), 0.0},
                  {ECUIO_OBD2_SIG_SPEED_ID, 50.0}}));
    EXPECT_EQ(unsplit(session), 0u);
    ecuio_session_destroy(session);
}

/* uds-generic's set holds a scanned range's DIDs from create, so a
 * reply for one decodes before the scan has found it (ADR-0024 section 1) */
TEST(CoreTest, ValuesOfAScannedDidComeFromTheSetBuiltAtCreate)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://u?profile=uds-generic&did=F180-F182",
                                   &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    uint16_t f181 = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].mux_key == 0x62F181u)
            f181 = set->defs[i].id;
    }
    ASSERT_NE(f181, 0) << "F181 is in the set";
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t reply =
        value_frame(0x7E8, {0x62, 0xF1, 0x81, 0x41, 0x42});
    ecuio_session_dispatch_callbacks(session, &reply);
    ASSERT_EQ(cap.batches.size(), 1u);
    ASSERT_EQ(cap.batches[0].values.size(), 1u);
    EXPECT_EQ(cap.batches[0].values[0].id, f181);
    EXPECT_EQ(cap.batches[0].values[0].kind, ECUIO_VALUE_HEX);
    EXPECT_EQ(cap.batches[0].values[0].text, "41 42");
    ecuio_session_destroy(session);
}

/* A packed Service 01 reply splits into a record for each PID, all in one
 * frame's text slots: a hex def on 41 1C and one on 41 51 both decode
 * from 41 1C 06 51 01, each with bytes of its own */
TEST(CoreTest, APackedReplyGivesEachRecordsTextASlotOfItsOwn)
{
    static ecuio_signal_def_t defs[2];
    defs[0] = text_value_def(1, ECUIO_VALUE_HEX, 2, 0x411C, 2, 1);
    defs[1] = text_value_def(2, ECUIO_VALUE_HEX, 2, 0x4151, 2, 1);
    for (ecuio_signal_def_t& d : defs) {
        d.message_id = ECUIO_OBD2_ECU_ARB_ID;
    }
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-packed-text", &set, &why), ECUIO_OK)
        << "code " << why.code << " at def " << why.index;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-packed-text", &session),
        ECUIO_OK);
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t packed =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x1C, 0x06, 0x51, 0x01});
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.batches.size(), 1u);
    const std::vector<SeenValue>& v = cap.batches[0].values;
    ASSERT_EQ(v.size(), 2u) << "a slot for each record's text";
    EXPECT_EQ(v[0].id, 1);
    EXPECT_EQ(v[0].text, "06");
    EXPECT_EQ(v[1].id, 2);
    EXPECT_EQ(v[1].text, "01");
    EXPECT_GE(std::abs(v[1].text_at - v[0].text_at),
              static_cast<std::ptrdiff_t>(ECUIO_DECODE_TEXT_MAX))
        << "the two records' texts share bytes";
    EXPECT_EQ(unsplit(session), 0u);
    ecuio_session_destroy(session);
}

namespace {

/* What a frame decodes to, by direct calls, set by the frame callback
 * and checked by the values callback that follows it on the worker */
struct DirectCheck {
    const ecuio_signal_set_t* set = nullptr;
    std::vector<SeenValue> expected;
    std::atomic<int> frames{0};
    std::atomic<int> batches{0};
    std::atomic<int> mismatches{0};
    std::atomic<int> computed{0}; /* computed values after a frame's */
    std::atomic<int> missing{0}; /* a frame that decoded, no batch */
    bool pending = false;
};

void direct_frame_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* c = static_cast<DirectCheck*>(user_data);
    if (c->pending)
        ++c->missing;
    c->expected.clear();
    const uint16_t len = frame->data_length;
    char text[ECUIO_DECODE_TEXT_MAX];
    for (uint32_t i = 0; i < c->set->count; ++i) {
        const ecuio_signal_def_t& d = c->set->defs[i];
        if (!ecuio_def_applies_to_frame(&d, frame))
            continue;
        SeenValue s;
        s.id = d.id;
        if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            if (ecuio_decode_text(&d, frame->data, len, text, sizeof(text)) !=
                ECUIO_DECODE_OK)
                continue;
            s.text = text;
        } else if (ecuio_decode_signal(&d, frame->data, len, &s.value) !=
                   ECUIO_DECODE_OK) {
            continue;
        }
        c->expected.push_back(s);
    }
    c->pending = !c->expected.empty();
    ++c->frames;
}

void direct_values_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* c = static_cast<DirectCheck*>(user_data);
    ++c->batches;
    /* The frame's own values, then any computed one its frame fed */
    bool same = c->pending && batch->count >= c->expected.size();
    for (uint32_t i = 0; same && i < c->expected.size(); ++i) {
        const ecuio_value_t& v = batch->values[i];
        const SeenValue& e = c->expected[i];
        same = v.signal_id == e.id && v.value == e.value &&
               (v.text == nullptr ? e.text.empty() : e.text == v.text);
    }
    for (uint32_t i = static_cast<uint32_t>(c->expected.size());
         same && i < batch->count; ++i) {
        same = false;
        for (uint32_t d = 0; d < c->set->count; ++d) {
            const ecuio_signal_def_t& def = c->set->defs[d];
            if (def.id == batch->values[i].signal_id &&
                (def.flags & ECUIO_DEF_FLAG_COMPUTED)) {
                same = true;
                ++c->computed;
            }
        }
    }
    if (!same)
        ++c->mismatches;
    c->pending = false;
}

} // namespace

/* Every batch equals the direct decode of its frame, over the vehicle
 * fixture under obd2 and the mock's drive cycle under its own set, then
 * the computed values it fed: none under obd2, the mock's boost after
 * every frame */
TEST(CoreTest, ValuesMatchDirectDecodesOfEveryFrame)
{
    const std::string fixture =
        "replay://" + std::string(ECUIO_REPLAY_FIXTURES) +
        "/2026-10-25-engine-start?profile=obd2&speed=max";
    struct Case {
        const char* uri;
        int frames;
        bool computed; /* one computed value a batch */
    };
    for (const Case& k : {Case{fixture.c_str(), 7074, false},
                          Case{"mock://direct", 20, true}}) {
        SCOPED_TRACE(k.uri);
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(k.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        DirectCheck c;
        c.set = ecuio_session_signal_set(session);
        ASSERT_NE(c.set, nullptr);
        EXPECT_EQ(ecuio_register_frame_callback(session, direct_frame_cb, &c),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_values_callback(session, direct_values_cb, &c),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return c.frames.load() >= k.frames; }, 10000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << c.frames.load() << " frames";
        EXPECT_GT(c.batches.load(), 0);
        EXPECT_EQ(c.mismatches.load(), 0);
        EXPECT_EQ(c.computed.load(), k.computed ? c.batches.load() : 0);
        EXPECT_EQ(c.missing.load(), 0) << "a frame that decoded with no batch";
    }
}

/* ---- Link health (link_health.h), session-free: the rules every worker
 * that judges a link shares. The J2534 worker's use of them is HARNESS
 * (J2534HealthTest). */

/* The backoff step: from the base every wait grows and none exceeds the
 * cap, including the step that lands on it, and at the cap it stays. A
 * worker test can't see this; reaching the cap takes over 6 s of
 * sustained failure. */
TEST(CoreTest, LinkBackoffStepDoublesToCapWithoutOvershoot)
{
    uint32_t ms = LINK_BACKOFF_BASE_MS;
    uint32_t steps = 0;
    while (ms < LINK_BACKOFF_CAP_MS) {
        const uint32_t next = link_backoff_next(ms);
        EXPECT_GT(next, ms);
        EXPECT_LE(next, LINK_BACKOFF_CAP_MS) << "overshoot after " << ms;
        ms = next;
        ASSERT_LT(++steps, 32u) << "never reached the cap";
    }
    EXPECT_EQ(ms, LINK_BACKOFF_CAP_MS);
    EXPECT_EQ(link_backoff_next(LINK_BACKOFF_CAP_MS), LINK_BACKOFF_CAP_MS);
    EXPECT_EQ(link_backoff_next(LINK_BACKOFF_CAP_MS - 1), LINK_BACKOFF_CAP_MS);
    EXPECT_EQ(link_backoff_next(UINT32_MAX), LINK_BACKOFF_CAP_MS)
        << "doubling must not wrap";
}

/* A hard read failure is one episode however long it lasts. A clean read
 * ends the backoff without ending the failure, and the next hard read
 * backs off from the base again, still in the same episode. A matched
 * reply ends it. */
TEST(CoreTest, LinkHardReadIsOneEpisodeAndBacksOffFromTheBase)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_read(h, true, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes.load(), 1u);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS);

    h.backoff_ms = link_backoff_next(h.backoff_ms); /* the worker's sleep */
    link_on_read(h, true, false, false);
    EXPECT_EQ(episodes.load(), 1u) << "the same failure, no new episode";
    EXPECT_EQ(h.backoff_ms, 2 * LINK_BACKOFF_BASE_MS) << "left to grow";

    link_on_read(h, false, false, false);
    EXPECT_EQ(h.backoff_ms, 0u) << "a clean read ends the backoff";
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE)
        << "but not the failure";

    link_on_read(h, true, false, false);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS) << "restarted from the base";
    EXPECT_EQ(episodes.load(), 1u);

    h.recovered = false;
    link_on_read(h, false, true, false);
    EXPECT_EQ(published.load(), ECUIO_OK);
    EXPECT_EQ(h.backoff_ms, 0u);
    EXPECT_TRUE(h.recovered) << "the probe resumes on recovery";
    EXPECT_EQ(episodes.load(), 1u);
}

/* Quiet is a run of LINK_HEALTH_QUIET_RUN, with no episode: reads count
 * only when told to (passive, or nothing left to ask), request timeouts
 * always, and a matched reply starts the run over. */
TEST(CoreTest, LinkQuietRunGoesQuietWithoutAnEpisode)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, false);
    }
    EXPECT_EQ(published.load(), ECUIO_OK)
        << "empty reads between requests mean nothing";

    for (uint32_t i = 0; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    EXPECT_EQ(published.load(), ECUIO_OK) << "one short of the run";
    link_on_read(h, false, true, true);
    EXPECT_EQ(h.quiet_runs, 0u) << "a reply starts the run over";

    for (uint32_t i = 0; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_request_timeout(h);
    }
    EXPECT_EQ(published.load(), ECUIO_OK);
    link_on_request_timeout(h);
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_EQ(episodes.load(), 0u) << "quiet isn't a failure";
}

/* A failed write latches REQUESTS_FAILED as one episode with a backoff.
 * While it's latched, timeouts don't count toward quiet and replies don't
 * clear it; only a sent write does. */
TEST(CoreTest, LinkFailedWriteLatchesUntilAWriteGoesThrough)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_write(h, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes.load(), 1u);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS);

    link_on_write(h, false);
    EXPECT_EQ(episodes.load(), 1u);
    for (uint32_t i = 0; i < 2 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_request_timeout(h);
    }
    EXPECT_EQ(h.quiet_runs, 0u) << "the state is the writes'";
    link_on_read(h, false, true, true);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED)
        << "a reply doesn't clear the latch";

    h.recovered = false;
    link_on_write(h, true);
    EXPECT_EQ(published.load(), ECUIO_OK);
    EXPECT_EQ(h.backoff_ms, 0u);
    EXPECT_TRUE(h.recovered);
    EXPECT_EQ(episodes.load(), 1u);
}

/* A hard read outranks a failing write and counts its own episode; a
 * write failing under it changes nothing; a clean read under the latch
 * steps down to REQUESTS_FAILED without a new episode. */
TEST(CoreTest, LinkHardReadOutranksTheLatchAndStepsDownToIt)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_write(h, false);
    link_on_read(h, true, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes.load(), 2u);

    link_on_write(h, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes.load(), 2u);

    link_on_read(h, false, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes.load(), 2u) << "a step down, not a new failure";
}

/* The session's atomics are written only on a transition, so a reader
 * polling them never sees churn from a steady state. */
TEST(CoreTest, LinkPublishesOnlyTransitions)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    published.store(-99); /* a marker: a store would overwrite it */
    link_on_read(h, false, true, false);
    link_on_write(h, true);
    EXPECT_EQ(published.load(), -99) << "OK to OK is no transition";
    link_on_read(h, true, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
}

/* The decode's index (def_index.h) finds, for any frame, exactly the
 * defs ecuio_def_applies_to_frame takes, in the set's order: sets that
 * mix unkeyed defs and every key shape on a few IDs, keys shared
 * between defs, against frames too short for a key, error reports and
 * IDs no def listens on. The bytes come from a small alphabet so keys
 * match often. */
TEST(DefIndexTest, FindsExactlyTheDefsThatApplyInTheSetsOrder)
{
    std::mt19937 rng(20261031u);
    const auto pick = [&rng](uint32_t n) {
        return static_cast<uint32_t>(rng() % n);
    };
    const uint32_t ids[] = {0x100u, 0x7E8u, 0x7E9u};
    const uint8_t alphabet[] = {0x00u, 0x01u, 0x41u, 0x0Cu};
    uint64_t found_total = 0;
    for (int round = 0; round < 200; ++round) {
        std::vector<ecuio_signal_def_t> defs(1 + pick(40));
        for (ecuio_signal_def_t& d : defs) {
            d = ecuio_signal_def_t{};
            d.message_id = ids[pick(3)];
            d.mux_length = static_cast<uint8_t>(pick(4));
            if (d.mux_length > 0) {
                d.mux_offset = static_cast<uint8_t>(pick(3));
                for (uint8_t b = 0; b < d.mux_length; ++b) {
                    d.mux_key = (d.mux_key << 8) | alphabet[pick(4)];
                }
            }
        }
        ecuio_def_index index;
        ecuio_def_index_build(index, defs.data(),
                              static_cast<uint32_t>(defs.size()));
        std::vector<uint32_t> found(defs.size());
        for (int f = 0; f < 50; ++f) {
            ecuio_frame_t frame{};
            frame.arbitration_id = pick(8) == 0 ? 0x123u : ids[pick(3)];
            frame.flags = pick(10) == 0 ? ECUIO_FRAME_FLAG_ERROR : 0u;
            frame.data_length = static_cast<uint16_t>(pick(7));
            for (uint16_t b = 0; b < frame.data_length; ++b) {
                frame.data[b] = alphabet[pick(4)];
            }
            std::vector<uint32_t> want;
            for (uint32_t i = 0; i < defs.size(); ++i) {
                if (ecuio_def_applies_to_frame(&defs[i], &frame)) {
                    want.push_back(i);
                }
            }
            const uint32_t n = ecuio_def_index_find(index, frame, found.data());
            ASSERT_EQ(std::vector<uint32_t>(found.begin(), found.begin() + n),
                      want)
                << "round " << round << ", frame " << f;
            found_total += n;
        }
    }
    EXPECT_GT(found_total, 2000u) << "the frames must match defs often";
}

/* The surrogate's session logs (surrogate_log_names.h): each kind's name
 * matched exactly, so pruning TEMP deletes nothing else */
TEST(CoreTest, SurrogateLogNamesAreMatchedExactly)
{
    EXPECT_TRUE(
        sur_log_is_kept_name(L"ecuio-surrogate.20261101-001100-842.log"));
    EXPECT_FALSE(sur_log_is_kept_name(L"ecuio-surrogate.log"));
    EXPECT_FALSE(
        sur_log_is_kept_name(L"ecuio-surrogate.20261101-001100-842.log.bak"));
    EXPECT_FALSE(
        sur_log_is_kept_name(L"ecuio-surrogate.2026110-001100-842.log"));

    EXPECT_TRUE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-1.log"));
    EXPECT_TRUE(sur_log_is_per_spawn_name(
        L"ecuio-surrogate-4294967295-2147483647.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(
        L"ecuio-surrogate-42949672951-1.log")); /* 11 digits */
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate--1.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-1.log.txt"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-1-2.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-x-1.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate.log"));
    EXPECT_FALSE(
        sur_log_is_per_spawn_name(L"ecuio-surrogate.20261101-001100-842.log"));
    EXPECT_FALSE(
        sur_log_is_per_spawn_name(L"ecuio-elm327.20261101-001100-842.log"));
}

/* Past the newest SUR_LOG_KEEP, the oldest go first, by last write, not
 * by name: a per-spawn name's PID sorts in no time order */
TEST(CoreTest, SurrogateLogExcessIsTheOldestByLastWrite)
{
    std::vector<SurLogFile> files;
    for (uint64_t i = 0; i < SUR_LOG_KEEP + 3; ++i) {
        /* names counting down while the writes count up */
        files.push_back(SurLogFile{L"ecuio-surrogate-" +
                                       std::to_wstring(900 - i) + L"-1.log",
                                   1000 + i});
    }
    const std::vector<std::wstring> gone = sur_log_excess(files, SUR_LOG_KEEP);
    ASSERT_EQ(gone.size(), 3u);
    EXPECT_EQ(gone[0], L"ecuio-surrogate-900-1.log");
    EXPECT_EQ(gone[1], L"ecuio-surrogate-899-1.log");
    EXPECT_EQ(gone[2], L"ecuio-surrogate-898-1.log");

    EXPECT_TRUE(sur_log_excess(std::vector<SurLogFile>(
                                   files.begin(), files.begin() + SUR_LOG_KEEP),
                               SUR_LOG_KEEP)
                    .empty());
    /* the same write time: the name decides, so a run is the same each time */
    const std::vector<std::wstring> tie =
        sur_log_excess({{L"b", 5}, {L"a", 5}, {L"c", 9}}, 2);
    ASSERT_EQ(tie.size(), 1u);
    EXPECT_EQ(tie[0], L"a");
}

// ---- VIN decode (ADR-0043): 49 CFR 565.15 and NHTSA vPIC's codes. The
// VINs are made up around "FAKE", their check digits worked out by the
// regulation's Tables III and IV, but for Table VI's own sample.

namespace {

ecuio_vin_info_t vin_decoded(const char* vin, uint32_t latest = 0)
{
    ecuio_vin_info_t info;
    std::memset(&info, 0xA5, sizeof info);
    EXPECT_EQ(ecuio_vin_decode(vin, latest, &info), ECUIO_OK) << vin;
    return info;
}

} // namespace

TEST(VinTest, TheRegulationsSampleDecodes)
{
    // 565.15(c)(5), Table VI: check digit 4; position 7 is a 9, so its
    // year code 5 is 2005
    const ecuio_vin_info_t v = vin_decoded("1G4AH59H45G118341");
    EXPECT_EQ(v.flags, ECUIO_VIN_CHECK_MATCHES);
    EXPECT_EQ(v.check_digit, '4');
    EXPECT_EQ(v.model_year, 2005);
    EXPECT_STREQ(v.wmi, "1G4");
    EXPECT_EQ(v.wmi_length, 3);
    EXPECT_STREQ(ecuio_vin_make(v.wmi), "Buick");
    for (uint32_t r : v.reserved) {
        EXPECT_EQ(r, 0u);
    }
}

TEST(VinTest, PositionSevenPlacesTheYearInItsCycle)
{
    // Table VII's note: a letter in 7 is 2010-2039, a digit 1980-2009
    EXPECT_EQ(vin_decoded("1FAFAKE10M0000001").model_year, 2021);
    EXPECT_EQ(vin_decoded("1FAAB12C5FAKE0001").model_year, 1985);
    // Table VII's own rows, 2005 to 2039, against the decode
    const char* codes = "56789ABCDEFGHJKLMNPRSTVWXY123456789";
    for (int i = 0; codes[i] != '\0'; ++i) {
        char vin[] = "1FAFAKE1?M0000001";
        vin[9] = codes[i];
        const int year = 2005 + i;
        // The check digit doesn't matter to the year; position 7 picks
        // the cycle's half, a digit for the years before 2010
        vin[6] = year < 2010 ? '1' : 'E';
        vin[8] = '0';
        EXPECT_EQ(vin_decoded(vin).model_year, year) << vin;
    }
}

TEST(VinTest, ACodeThatIsNoYearHasNone)
{
    for (char c : {'0', 'U', 'Z'}) {
        char vin[] = "1FAFAKE10M0000001";
        vin[9] = c;
        EXPECT_EQ(vin_decoded(vin).model_year, 0) << c;
    }
}

TEST(VinTest, AYearPastTheLatestIsTheCyclesEarlierOne)
{
    // Y is 2030 by a letter in 7; past 2027, it is 2000
    ecuio_vin_info_t v = vin_decoded("1FAFAKE13Y0000001", 2027);
    EXPECT_EQ(v.model_year, 2000);
    EXPECT_EQ(v.flags, ECUIO_VIN_CHECK_MATCHES | ECUIO_VIN_YEAR_EARLIER);
    // Within the bound, or with none, it stays
    v = vin_decoded("1FAFAKE13Y0000001", 2030);
    EXPECT_EQ(v.model_year, 2030);
    EXPECT_EQ(v.flags, ECUIO_VIN_CHECK_MATCHES);
    EXPECT_EQ(vin_decoded("1FAFAKE13Y0000001").model_year, 2030);
    // A digit in 7 is already the earlier half
    EXPECT_EQ(vin_decoded("1FAAB12C5FAKE0001", 1980).model_year, 1985);
}

TEST(VinTest, TheCheckDigitIsComputedAndCompared)
{
    // Remainder 10 is an X
    ecuio_vin_info_t v = vin_decoded("1FAFAKE1XM0000006");
    EXPECT_EQ(v.check_digit, 'X');
    EXPECT_EQ(v.flags & ECUIO_VIN_CHECK_MATCHES, ECUIO_VIN_CHECK_MATCHES);
    // A wrong digit decodes the same, flagged: many markets need none
    v = vin_decoded("1FAFAKE18M0000001");
    EXPECT_EQ(v.check_digit, '0');
    EXPECT_EQ(v.flags & ECUIO_VIN_CHECK_MATCHES, 0u);
    EXPECT_EQ(v.model_year, 2021);
    EXPECT_STREQ(v.wmi, "1FA");
}

TEST(VinTest, ANineInPositionThreeIsASixCharacterCode)
{
    // 565.15(a): positions 1-3 and 12-14
    const ecuio_vin_info_t v = vin_decoded("1A9FAKE18MA596001");
    EXPECT_STREQ(v.wmi, "1A9596");
    EXPECT_EQ(v.wmi_length, 6);
    EXPECT_STREQ(ecuio_vin_make(v.wmi), "ACG");
    EXPECT_STREQ(ecuio_vin_make("1A9"), "");
}

TEST(VinTest, TheMakeIsVpicsForTheCode)
{
    EXPECT_STREQ(ecuio_vin_make("1FA"), "Ford");
    EXPECT_STREQ(ecuio_vin_make("JN1"), "Datsun, Infiniti or Nissan");
    EXPECT_STREQ(ecuio_vin_make("ZZZ"), "");
    EXPECT_STREQ(ecuio_vin_make("1F"), "");
    EXPECT_STREQ(ecuio_vin_make("1FAX"), "");
    EXPECT_STREQ(ecuio_vin_make("1fa"), "");
    EXPECT_STREQ(ecuio_vin_make(""), "");
    EXPECT_STREQ(ecuio_vin_make(nullptr), "");
    EXPECT_EQ(std::strncmp(ecuio_vin_source(), "vPICList_lite_", 14), 0);
}

TEST(VinTest, OnlyAVinIsDecoded)
{
    ecuio_vin_info_t v;
    for (const char* bad :
         {"1FAFAKE10M000000", "1FAFAKE10M00000011", "1faFAKE10M0000001",
          "1FAFAKE10M000000I", "1FAFAKE10M000000O", "1FAFAKE10M000000Q",
          "1FAFAKE10M00000-1", "", "FAKEJ2534VIN00001"}) {
        std::memset(&v, 0xA5, sizeof v);
        EXPECT_EQ(ecuio_vin_decode(bad, 0, &v), ECUIO_ERR_INVALID_ARGUMENT)
            << bad;
        EXPECT_EQ(v.flags, 0u) << bad;
        EXPECT_EQ(v.wmi[0], '\0') << bad;
    }
    EXPECT_EQ(ecuio_vin_decode(nullptr, 0, &v), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_vin_decode("1FAFAKE10M0000001", 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

// A packed reply decodes each def once: the defs keyed by a record's PID
// from their records, every other def on its ID (unkeyed, or keyed
// otherwise) from the reply whole, so a batch never holds more values
// than the set has defs
TEST(CoreTest, APackedReplyDecodesEachDefOnce)
{
    static ecuio_signal_def_t defs[3];
    std::memset(defs, 0, sizeof defs);
    defs[0].id = 1;
    defs[0].message_id = ECUIO_OBD2_ECU_ARB_ID;
    defs[0].mux_length = 2;
    defs[0].mux_key = 0x410C;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].byte_order = ECUIO_BYTE_ORDER_BE;
    defs[0].start_byte = 2;
    defs[0].bit_length = 16;
    defs[0].scale = 0.25;
    std::strcpy(defs[0].name, "Rpm");
    defs[1].id = 2;
    defs[1].message_id = ECUIO_OBD2_ECU_ARB_ID;
    defs[1].kind = ECUIO_VALUE_UINT;
    defs[1].bit_length = 8;
    defs[1].scale = 1;
    std::strcpy(defs[1].name, "First byte");
    defs[2].id = 3;
    defs[2].message_id = ECUIO_OBD2_ECU_ARB_ID;
    defs[2].mux_length = 1;
    defs[2].mux_key = 0x41;
    defs[2].kind = ECUIO_VALUE_UINT;
    defs[2].start_byte = 1;
    defs[2].bit_length = 8;
    defs[2].scale = 1;
    std::strcpy(defs[2].name, "Second byte");
    static ecuio_signal_set_t set;
    set = ecuio_signal_set_t{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("packed-each-once", &set, nullptr),
              ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://p?profile=packed-each-once", &session),
        ECUIO_OK)
        << ecuio_last_error_detail();
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);

    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x1A, 0xF8, 0x0D, 0x20});
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.count(), 1u);
    EXPECT_EQ(id_values(cap.batches[0]),
              (std::vector<std::pair<uint16_t, double>>{
                  {1, 1726.0}, {2, 65.0}, {3, 12.0}}))
        << "the record's def, then the others from the reply whole";
    EXPECT_EQ(unsplit(session), 0u);

    ecuio_session_destroy(session);
    (void)ecuio_profile_unregister("packed-each-once");
}

/* ADR-0056 section 2: the focus call's contract on a session that asks
 * nothing (the mock): taken before and after start, an id the set lacks
 * refused by name, a count of 0 taken, NULLs and a closed session
 * refused; a session with no set (raw) takes only a count of 0 */
TEST(CoreTest, SetFocusTakesTheSetsIdsAndRefusesTheRest)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    ASSERT_GE(set->count, 2u);
    const uint16_t ids[2] = {set->defs[0].id, set->defs[1].id};
    EXPECT_EQ(ecuio_session_set_focus(nullptr, ids, 2),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 1), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus(s, ids, 2), ECUIO_OK);
    const uint16_t stranger[2] = {ids[0], 65000};
    EXPECT_EQ(ecuio_session_set_focus(s, stranger, 2),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no signal with id 65000 in profile mock");
    EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, ids, 1), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, ids, 1), ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);

    ecuio_session_t* raw = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=raw", &raw), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(raw, ids, 1), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus(raw, nullptr, 0), ECUIO_OK);
    ecuio_session_destroy(raw);
}

/* The focus call clears the thread's detail on entry, so a refusal that
 * sets none (a NULL, a closed session) or a success leaves no stale
 * words from an earlier call */
TEST(CoreTest, SetFocusClearsAStaleDetail)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    ASSERT_GE(set->count, 1u);
    const uint16_t id = set->defs[0].id;
    ecuio_detail_set("stale");
    EXPECT_EQ(ecuio_session_set_focus(nullptr, &id, 1),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    ecuio_detail_set("stale");
    EXPECT_EQ(ecuio_session_set_focus(s, &id, 1), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_detail_set("stale");
    EXPECT_EQ(ecuio_session_set_focus(s, &id, 1), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    ecuio_session_destroy(s);
}

namespace {

/* The id of set's first def whose request is bytes[0..len) */
uint16_t focus_id_of(const ecuio_signal_set_t* set,
                     std::initializer_list<uint8_t> bytes)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (ecuio_engine_request_for_def(&set->defs[i], req, &len) &&
            len == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), req)) {
            return set->defs[i].id;
        }
    }
    ADD_FAILURE() << "no def asks that request";
    return 0;
}

} // namespace

/* A requesting session refuses a focus that leaves it nothing to ask,
 * with the reason in the detail and the focus it had untouched; a focus
 * with one askable pick among others is taken. Under ?poll=0x0C the one
 * request is 01 0C, so a focus on 0x0D alone is refused. The requests
 * are built on a mock session, whose worker never takes the focus. */
TEST(CoreTest, AFocusThatLeavesNothingToRequestIsRefused)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t rpm = focus_id_of(set, {0x01, 0x0C});
    const uint16_t speed = focus_id_of(set, {0x01, 0x0D});
    ASSERT_NE(rpm, speed);
    request_params params;
    params.has_poll = true;
    params.poll_pid = 0x0C;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.channel_count, e.get(), &probe,
                                   &identify),
              ECUIO_OK);
    ASSERT_EQ(e->count, 1u);

    ASSERT_EQ(ecuio_session_set_focus(s, &rpm, 1), ECUIO_OK);
    const uint32_t generation = s->focus_generation.load();
    EXPECT_EQ(ecuio_session_set_focus(s, &speed, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the focus leaves nothing to request: none of the picked "
                 "signals is asked on this session");
    EXPECT_EQ(s->focus_generation.load(), generation) << "nothing changed";
    {
        std::lock_guard<std::mutex> lock(s->focus_mutex);
        EXPECT_EQ(s->focus_ids, std::vector<uint16_t>{rpm});
    }
    const uint16_t both[2] = {speed, rpm};
    EXPECT_EQ(ecuio_session_set_focus(s, both, 2), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    ecuio_session_destroy(s);
}

/* uds-generic: a DID the scan finds under a focus joins parked and is
 * unparked at the worker's next take when the focus names it; one it
 * doesn't name stays parked. A focus on a DID the scan hasn't found is
 * refused alone, and taken once it has. Rows are added as the scan's
 * find adds them (ecuio_engine_scan_poll). */
TEST(CoreTest, AScannedDidFoundUnderAFocusJoinsItWhenPicked)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(
        ecuio_session_create(
            "mock://focus?profile=uds-generic&did=1E1C:u16,1E18-1E1A", &s),
        ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t polled = focus_id_of(set, {0x22, 0x1E, 0x1C});
    const uint16_t picked = focus_id_of(set, {0x22, 0x1E, 0x19});
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.channel_count, e.get(), &probe,
                                   &identify),
              ECUIO_OK);
    ASSERT_EQ(e->count, 1u) << "the polled DID; the scan's are unasked";

    EXPECT_EQ(ecuio_session_set_focus(s, &picked, 1),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "the scan hasn't found it";
    const uint16_t ids[2] = {polled, picked};
    ASSERT_EQ(ecuio_session_set_focus(s, ids, 2), ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);
    ASSERT_TRUE(e->focused);
    EXPECT_EQ(e->requests[0].parked, 0u);

    for (const uint8_t lo : {uint8_t{0x19}, uint8_t{0x1A}}) {
        const uint8_t req[3] = {0x22, 0x1E, lo};
        ASSERT_EQ(ecuio_engine_add(e.get(), 0, req, 3),
                  ecuio_engine_add_result::added);
        ++e->chans[0].generation; /* the scan's settle */
    }
    EXPECT_EQ(e->requests[1].parked, 1u) << "it joins parked";
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(e->requests[1].parked, 0u) << "1E19 is picked";
    EXPECT_EQ(e->requests[2].parked, 1u) << "1E1A isn't";
    EXPECT_EQ(e->chans[0].requests, 2u);
    EXPECT_EQ(ecuio_session_set_focus(s, &picked, 1), ECUIO_OK)
        << "found, it is askable";
    ecuio_session_destroy(s);
}
