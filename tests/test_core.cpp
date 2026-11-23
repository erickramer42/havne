// File/layer: tests/test_core.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "new_counter.h"   /* tests: operator new counted per thread */

#include "core_test_support.h"

using namespace core_test;


/* new_counter's count for the binary's other files, which can't include
 * the header that replaces operator new (test_recording_check.cpp) */
unsigned long news_on_this_thread()
{
    return new_counter::on_this_thread();
}

/* The same for new_counter::fail_new (test_export_frames.cpp and the other core tests that fail an allocation) */
void fail_new_on_this_thread(unsigned long n)
{
    new_counter::fail_new(n);
}


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
    for (int i = 0; i < 31; ++i) {
        long_query += "&profile=mock";
    }
    ASSERT_EQ(ecuio_session_create(long_query.c_str(), &s), ECUIO_OK)
        << "32 pairs, the most a query holds: " << ecuio_last_error_detail();
    ecuio_session_destroy(s);
    s = nullptr;
    long_query += "&profile=mock";
    EXPECT_EQ(ecuio_session_create(long_query.c_str(), &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the query has more than 32 key=value pairs");
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
    EXPECT_EQ(info.module_count, 1u);
    ecuio_channel_decl_t module{};
    ASSERT_EQ(ecuio_session_get_module(s, 0, &module), ECUIO_OK);
    EXPECT_EQ(module.rx_id, 0x7E8u);
    EXPECT_EQ(module.tx_id, 0x7E0u);
    EXPECT_EQ(ecuio_session_get_module(s, 1, &module),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(s);
}

/* Out of memory at each of create's allocations in turn, for a session
 * of one module, one that walks its PIDs and one of six modules: the call
 * answers OK or OUT_OF_MEMORY, throws nothing, and hands a session back
 * only with OK, every module in it */
TEST(CoreTest, OutOfMemoryAnywhereInCreateIsAnsweredNotThrown)
{
    const char* const six_modules =
        "mock://x?profile=uds-generic&did=7E8:D100:u8,7E9:D101:u8,"
        "7EA:D102:u8,7EB:D103:u8,7EC:D104:u8,7ED:D105:u8";
    const char* const uris[] = {
        "mock://demo",
        "mock://x?profile=obd2",
        six_modules,
    };
    for (const char* uri : uris) {
        SCOPED_TRACE(uri);
        ecuio_session_t* s = nullptr;
        const unsigned long before = news_on_this_thread();
        ASSERT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
        const unsigned long news = news_on_this_thread() - before;
        ecuio_session_info_t whole{};
        ASSERT_EQ(ecuio_session_get_info(s, &whole), ECUIO_OK);
        ecuio_session_destroy(s);
        ASSERT_GT(news, 5u);
        unsigned long refused = 0;
        for (unsigned long n = 1; n <= news; ++n) {
            s = nullptr;
            ecuio_status_t rc = ECUIO_OK;
            bool threw = false;
            fail_new_on_this_thread(n);
            try {
                rc = ecuio_session_create(uri, &s);
            } catch (...) {
                threw = true;
            }
            fail_new_on_this_thread(0);
            ASSERT_FALSE(threw) << "create threw at allocation " << n;
            if (rc == ECUIO_OK) {
                ASSERT_NE(s, nullptr) << "at " << n;
                ecuio_session_info_t info{};
                EXPECT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
                EXPECT_EQ(info.module_count, whole.module_count) << "at " << n;
                ecuio_channel_support_t last{};
                EXPECT_EQ(ecuio_session_get_channel_support(
                              s, whole.module_count - 1, &last),
                          ECUIO_OK)
                    << "at " << n;
                ecuio_session_destroy(s);
            } else {
                ++refused;
                EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
                EXPECT_EQ(s, nullptr) << "at " << n;
            }
        }
        EXPECT_GT(refused, 0u);
    }
}

/* The same over ecuio_profile_register, for a set that declares a
 * channel: registered or OUT_OF_MEMORY, nothing thrown, and a refused
 * name isn't in the registry */
TEST(CoreTest, OutOfMemoryAnywhereInRegisterIsAnsweredNotThrown)
{
    const char* const name = "test-register-oom";
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_profile_register(name, set, nullptr), ECUIO_OK);
    const unsigned long news = news_on_this_thread() - before;
    ASSERT_EQ(ecuio_profile_unregister(name), ECUIO_OK);
    ASSERT_GT(news, 1u) << "the engine's channels, the registry's entry";
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_profile_register(name, set, nullptr);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "register threw at allocation " << n;
        const ecuio_signal_set_t* found = nullptr;
        if (rc == ECUIO_OK) {
            EXPECT_EQ(ecuio_profile_lookup(name, &found), ECUIO_OK);
            EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK);
        } else {
            ++refused;
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(ecuio_profile_lookup(name, &found),
                      ECUIO_ERR_UNKNOWN_PROFILE)
                << "at " << n;
        }
    }
    EXPECT_GT(refused, 0u);
}

/* ADR-0065: the mock lists every module its set declares, past the four
 * the info holds itself, with a report for each, and answers each one's
 * DID; the modules are still there after the close */
TEST(CoreTest, AMockSessionListsEveryModuleItsSetDeclares)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(
                  "mock://x?profile=uds-generic&did=7E8:D100:u8,7E9:D101:u8,"
                  "7EA:D102:u8,7EB:D103:u8,7EC:D104:u8,7ED:D105:u8,7EE:D106:u8"
                  "&diagnose=0",
                  &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 7u);
    EXPECT_EQ(info.channel_count, uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS});
    const auto check = [&] {
        for (uint32_t c = 0; c < 7; ++c) {
            ecuio_channel_decl_t m{};
            ASSERT_EQ(ecuio_session_get_module(s, c, &m), ECUIO_OK) << c;
            EXPECT_EQ(m.rx_id, 0x7E8u + c);
            EXPECT_EQ(m.tx_id, 0x7E0u + c);
            ecuio_channel_support_t support{};
            ecuio_channel_identity_t identity{};
            ecuio_channel_diagnostics_t diagnostics{};
            ASSERT_EQ(ecuio_session_get_channel_support(s, c, &support),
                      ECUIO_OK);
            ASSERT_EQ(ecuio_session_get_channel_identity(s, c, &identity),
                      ECUIO_OK);
            ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, c, &diagnostics),
                      ECUIO_OK);
            EXPECT_EQ(support.rx_id, m.rx_id);
            EXPECT_EQ(identity.rx_id, m.rx_id);
            EXPECT_EQ(diagnostics.rx_id, m.rx_id);
        }
        ecuio_channel_decl_t past{};
        EXPECT_EQ(ecuio_session_get_module(s, 7, &past),
                  ECUIO_ERR_INVALID_ARGUMENT);
        ecuio_channel_diagnostics_t none{};
        EXPECT_EQ(ecuio_session_get_channel_diagnostics(s, 7, &none),
                  ECUIO_ERR_INVALID_ARGUMENT);
    };
    ASSERT_NO_FATAL_FAILURE(check());

    /* Each module's DID is answered on its own reply ID */
    struct Seen {
        std::mutex m;
        int frames[7] = {};
    } seen;
    ASSERT_EQ(ecuio_register_frame_callback(
                  s,
                  [](const ecuio_frame_t* f, void* user) {
                      auto* got = static_cast<Seen*>(user);
                      std::lock_guard<std::mutex> lock(got->m);
                      if (f->arbitration_id >= 0x7E8u &&
                          f->arbitration_id <= 0x7EEu && f->data_length >= 3 &&
                          f->data[0] == 0x62 && f->data[1] == 0xD1 &&
                          f->data[2] == f->arbitration_id - 0x7E8u) {
                          ++got->frames[f->arbitration_id - 0x7E8u];
                      }
                  },
                  &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool all = false;
    while (!all && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lock(seen.m);
        all = true;
        for (int n : seen.frames) {
            all = all && n >= 2;
        }
    }
    EXPECT_TRUE(all) << "two replies from each of the seven modules";
    ASSERT_EQ(ecuio_session_close(s), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(check());
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
    /* Both uses: a plugin a session decodes with, and the conditions a
     * recording holds */
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_IN_USE), "still in use");

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

