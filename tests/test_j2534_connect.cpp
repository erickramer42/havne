// File/layer: tests/test_j2534_connect.cpp | GTest, HARNESS tier
// Testing: why a j2534:// connect failed, through each backend: the
//          status ecuio_session_create or ecuio_session_start returns,
//          and ecuio_last_error_detail's words for it. A refused
//          PassThruOpen, PassThruConnect or filter names the call, its
//          status (by its J2534-1 v04.04 name, or as vendor-specific)
//          and the vendor's own text from PassThruGetLastError, word for
//          word alike on both backends. A DLL that won't load is
//          LOAD_FAILED on both; a success clears the detail.
//          J2534SurrogateConnectTest: the surrogate's own failures (no
//          surrogate exe, one built from other shared headers).

#include <gtest/gtest.h>

#include "ecuio/core.h"

#ifdef _WIN32
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "j2534_backend_fixture.h"

namespace {

using j2534_backends::Backend;
using j2534_backends::EnvSwap;

class J2534ConnectTest : public j2534_backends::BackendTest {
protected:
    ecuio_session_t* session = nullptr;

    void TearDown() override
    {
        if (session != nullptr) {
            (void)ecuio_session_close(session);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        BackendTest::TearDown();
    }
};

TEST_P(J2534ConnectTest, RefusedOpenNamesTheCallStatusAndVendorText)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OPEN_STATUS", "8"));
    EXPECT_EQ(ecuio_session_create(uri("").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(session, nullptr);
    ASSERT_NO_FATAL_FAILURE(expect_opened_on_backend());
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruOpen returned status 8 (ERR_DEVICE_NOT_CONNECTED): "
                 "fake_j2534: PassThruOpen refused (ECUIO_FAKE_OPEN_STATUS)");
}

/* The OBDX Pro VT and VX refuse to open with 34 when no adapter is
 * attached; J2534-1 v04.04 defines no status 34. */
TEST_P(J2534ConnectTest, AVendorsOwnStatusIsCalledVendorSpecific)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OPEN_STATUS", "34"));
    EXPECT_EQ(ecuio_session_create(uri("").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruOpen returned status 34 (vendor-specific): "
                 "fake_j2534: PassThruOpen refused (ECUIO_FAKE_OPEN_STATUS)");
}

TEST_P(J2534ConnectTest, RefusedConnectNamesTheBaud)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_CONNECT_STATUS", "0x19"));
    EXPECT_EQ(ecuio_session_create(uri("?baud=250000").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(session, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruConnect (ISO 15765 at 250000 baud) returned status "
                 "25 (ERR_INVALID_BAUDRATE): fake_j2534: PassThruConnect "
                 "refused (ECUIO_FAKE_CONNECT_STATUS)");
}

/* Filters register at start, so a refused one fails the start. */
TEST_P(J2534ConnectTest, RefusedFilterFailsTheStartWithTheVendorText)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_STATUS", "7"));
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruStartMsgFilter returned status 7 (ERR_FAILED): "
                 "fake_j2534: PassThruStartMsgFilter refused "
                 "(ECUIO_FAKE_FILTER_STATUS)");
}

/* A DLL that isn't there: in-proc LoadLibrary fails; through the
 * surrogate the child's does, and it exits with code 4. */
TEST_P(J2534ConnectTest, MissingDllFailsToLoad)
{
    EXPECT_EQ(ecuio_session_create(
                  "j2534://C:/ecuio-definitely-missing/fake_j2534.dll",
                  &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(strstr(ecuio_last_error_detail(), "load the DLL"), nullptr)
        << ecuio_last_error_detail();
}

/* A connect that works leaves nothing behind from the failure before. */
TEST_P(J2534ConnectTest, SuccessClearsTheDetail)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OPEN_STATUS", "8"));
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    ASSERT_STRNE(ecuio_last_error_detail(), "");

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OPEN_STATUS", nullptr));
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
}

INSTANTIATE_TEST_SUITE_P(Backend, J2534ConnectTest,
                         ::testing::Values(Backend::InProc,
                                           Backend::Surrogate),
                         j2534_backends::label);

/* The surrogate's own failures. Forced routing sends the x64 fake
 * through it; the cross-arch run's x86 fake routes there anyway. */
class J2534SurrogateConnectTest : public ::testing::Test {
protected:
    std::string dll;
    std::unique_ptr<EnvSwap> exe_swap;

    void SetUp() override
    {
        const char* x86 = std::getenv("ECUIO_FAKE_DLL");
        if (x86 != nullptr && x86[0] != '\0') {
            dll = x86;   /* the cross-arch run names its surrogate */
        } else {
            dll = FAKE_J2534_DLL_PATH;
            exe_swap = std::make_unique<EnvSwap>("ECUIO_SURROGATE_EXE",
                                                 SURROGATE_EXE_PATH);
        }
        ecuio_j2534_force_surrogate_for_testing(true);
    }

    void TearDown() override
    {
        ecuio_j2534_force_surrogate_for_testing(false);
        exe_swap.reset();
    }

    std::string uri() const { return "j2534://" + dll; }
};

TEST_F(J2534SurrogateConnectTest, MissingSurrogateExeIsASurrogateFailure)
{
    EnvSwap exe("ECUIO_SURROGATE_EXE",
                "C:/ecuio-definitely-missing/ecuio-j2534-surrogate.exe");
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &session),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(session, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no surrogate (ecuio-j2534-surrogate.exe): "
                 "ECUIO_SURROGATE_EXE names no file");
}

TEST_F(J2534SurrogateConnectTest, SurrogateFromOtherHeadersIsASurrogateFailure)
{
    EnvSwap fingerprint("ECUIO_SUR_TEST_FINGERPRINT", "1");
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &session),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(session, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate was built from other shared headers: "
                 "rebuild build-win32, then build");
}

} // namespace

#endif // _WIN32
