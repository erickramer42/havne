#include "test_j2534_stream_support.h"

#include <chrono>
#include <string>
#include <thread>

namespace {

template <typename Predicate> bool wait_for(Predicate pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

struct CodesRun {
    ecuio_session_t* session = nullptr;
    ecuio_module_codes_t report{};

    ~CodesRun()
    {
        if (session != nullptr) {
            ecuio_session_destroy(session);
        }
    }

    void start(const std::string& query)
    {
        const std::string uri =
            std::string("j2534://") + FAKE_J2534_DLL_PATH + query;
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }

    void read(uint8_t mask)
    {
        uint32_t id = 0;
        ASSERT_EQ(ecuio_session_read_module_codes(session, 0, mask, &id),
                  ECUIO_OK);
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_get_module_codes(session, 0, &report) ==
                           ECUIO_OK &&
                       report.read_id >= id &&
                       (report.flags & ECUIO_MODULE_CODES_COMPLETE) != 0;
            },
            8000))
            << "the read didn't complete";
    }
};

constexpr char kPlain[] = "?profile=obd2&probe=0&identify=0";

} // namespace

TEST_F(J2534AdoptionTest, AModulesCodesAreReadThroughTheFakeDll)
{
    CodesRun run;
    ASSERT_NO_FATAL_FAILURE(run.start(kPlain));
    ASSERT_NO_FATAL_FAILURE(run.read(0));
    fake_diagnostics::expect_module_codes(run.report, 0xAF);
    ASSERT_NO_FATAL_FAILURE(run.read(0xFF));
    fake_diagnostics::expect_module_codes(run.report, 0xFF);

    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(run.session, 0, &d) ==
                       ECUIO_OK &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        8000));
    const ecuio_module_code_t& shared = run.report.codes[3];
    ASSERT_EQ(shared.code, 0x030100u);
    EXPECT_EQ(
        ecuio_module_code_emissions_lists(&d, shared.code, run.report.format),
        1u << ECUIO_DIAG_STORED)
        << "P0301 is in the J1979 stored list too";
}

TEST_F(J2534AdoptionTest, ARefusedOrEmptyListIsReported)
{
    {
        ASSERT_TRUE(fake_control::set(fake_page(), "ECUIO_FAKE_MODULE_CODES",
                                      "refused"));
        CodesRun run;
        ASSERT_NO_FATAL_FAILURE(run.start(kPlain));
        ASSERT_NO_FATAL_FAILURE(run.read(0));
        EXPECT_EQ(run.report.count_outcome, ECUIO_DIAG_REFUSED);
        EXPECT_EQ(run.report.list_outcome, ECUIO_DIAG_REFUSED);
        EXPECT_EQ(run.report.list_nrc, 0x11);
    }
    ASSERT_TRUE(
        fake_control::set(fake_page(), "ECUIO_FAKE_MODULE_CODES", "none"));
    CodesRun run;
    ASSERT_NO_FATAL_FAILURE(run.start(kPlain));
    ASSERT_NO_FATAL_FAILURE(run.read(0));
    EXPECT_EQ(run.report.list_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(run.report.reported, 0u);
    EXPECT_EQ(run.report.count, 0u);
}

TEST_F(J2534AdoptionTest, AModuleThatAnswersNothingReadsNoResponse)
{
    ASSERT_TRUE(
        fake_control::set(fake_page(), "ECUIO_FAKE_MODULE_CODES", "silent"));
    CodesRun run;
    ASSERT_NO_FATAL_FAILURE(run.start(kPlain));
    ASSERT_NO_FATAL_FAILURE(run.read(0));
    EXPECT_EQ(run.report.count_outcome, ECUIO_DIAG_UNANSWERED);
    EXPECT_EQ(run.report.list_outcome, ECUIO_DIAG_UNANSWERED);
}

TEST_F(J2534AdoptionTest, AModuleThatReadsOnlyInTheSessionIsRefusedInDefault)
{
    ASSERT_TRUE(
        fake_control::set(fake_page(), "ECUIO_FAKE_MODULE_CODES", "extended"));
    {
        CodesRun run;
        ASSERT_NO_FATAL_FAILURE(run.start(kPlain));
        ASSERT_NO_FATAL_FAILURE(run.read(0));
        EXPECT_EQ(run.report.list_outcome, ECUIO_DIAG_REFUSED);
        EXPECT_EQ(run.report.list_nrc, 0x7F);
    }
    CodesRun run;
    ASSERT_NO_FATAL_FAILURE(run.start(
        "?profile=uds-generic&did=1E1C&session=03&identify=0&diagnose=0"));
    ASSERT_NO_FATAL_FAILURE(run.read(0));
    EXPECT_EQ(run.report.list_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(run.report.held, 4u);
}
