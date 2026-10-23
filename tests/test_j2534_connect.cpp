// File/layer: tests/test_j2534_connect.cpp | GTest, HARNESS tier
// Testing: why a j2534:// connect failed, through each backend: the
//          status ecuio_session_create or ecuio_session_start returns,
//          and ecuio_last_error_detail's words for it. A refused
//          PassThruOpen, PassThruConnect or filter names the call, its
//          status (by its J2534-1 v04.04 name, or as vendor-specific)
//          and the vendor's own text from PassThruGetLastError, word for
//          word alike on both backends. A DLL that won't load is
//          LOAD_FAILED on both; a success clears the detail. A session
//          that connects describes what it runs on
//          (ecuio_session_get_info): backend, DLL architecture, the
//          channel as connected, and the adapter's battery and version
//          readings, which may be refused without failing the connect.
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

    /* The cross-arch run's surrogate hosts the Win32 tree's fake; every
     * other case loads this tree's. */
    uint32_t fake_arch() const
    {
        const char* cross = std::getenv("ECUIO_FAKE_DLL");
        return (GetParam() == Backend::Surrogate && cross != nullptr &&
                cross[0] != '\0')
                   ? ECUIO_DLL_ARCH_X86 : ECUIO_DLL_ARCH_X64;
    }

    ecuio_session_info_t info_of_session() const
    {
        ecuio_session_info_t info;
        memset(&info, 0xEE, sizeof(info));   /* every byte must be written */
        EXPECT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
        return info;
    }

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

/* What a default session runs on: the obd2 profile's channel at the
 * ISO 15765 default, and the fake's battery and version readings. */
TEST_P(J2534ConnectTest, InfoDescribesTheAdapter)
{
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_on_backend());
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.backend, GetParam() == Backend::InProc
                                ? ECUIO_BACKEND_J2534_INPROC
                                : ECUIO_BACKEND_J2534_SURROGATE);
    EXPECT_EQ(info.dll_arch, fake_arch());
    EXPECT_EQ(info.protocol_id, 6u);   /* ISO 15765 */
    EXPECT_EQ(info.baud, 500000u);
    EXPECT_EQ(info.connect_flags, 0u);
    ASSERT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    for (uint32_t i = 1; i < ECUIO_SESSION_INFO_MAX_CHANNELS; ++i) {
        EXPECT_EQ(info.rx_ids[i], 0u);
        EXPECT_EQ(info.tx_ids[i], 0u);
    }
    EXPECT_EQ(info.flags, ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_VERSIONS);
    EXPECT_EQ(info.vbatt_mv, 13800u);
    EXPECT_STREQ(info.firmware_version, "fake-fw 1.0");
    EXPECT_STREQ(info.dll_version, "fake_j2534 0.3");
    EXPECT_STREQ(info.api_version, "04.04");
    EXPECT_STREQ(info.profile, "obd2");   /* j2534://'s default */
    for (uint32_t r : info.reserved) {
        EXPECT_EQ(r, 0u);
    }
}

/* The URI's baud and channel, and the 29-bit flag its IDs need. */
TEST_P(J2534ConnectTest, InfoCarriesTheUrisBaudAndChannel)
{
    ASSERT_EQ(ecuio_session_create(
                  uri("?profile=raw&baud=250000&filter=0x18DAF110"
                      "&txid=0x18DA10F1").c_str(), &session),
              ECUIO_OK);
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.baud, 250000u);
    EXPECT_EQ(info.connect_flags, 0x100u);   /* CAN_29BIT_ID */
    ASSERT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x18DAF110u);
    EXPECT_EQ(info.tx_ids[0], 0x18DA10F1u);
    EXPECT_STREQ(info.profile, "raw");
}

/* Neither reading is needed to stream: a refused one leaves its flag
 * clear, and the connect succeeds without a detail. */
TEST_P(J2534ConnectTest, RefusedReadingsLeaveTheirFlagsClear)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VBATT_STATUS", "7"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VERSION_STATUS", "7"));
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.flags, 0u);
    EXPECT_EQ(info.vbatt_mv, 0u);
    EXPECT_STREQ(info.firmware_version, "");
    EXPECT_STREQ(info.dll_version, "");
    EXPECT_STREQ(info.api_version, "");
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
}

/* PassThruReadVersion is optional, as PassThruGetLastError is: the
 * legacy fixture, beside the fake in each tree, doesn't export it. */
TEST_P(J2534ConnectTest, ADllWithoutReadVersionReportsNoVersions)
{
    const std::string legacy =
        dll.substr(0, dll.find_last_of("\\/") + 1) + "fake_j2534_legacy.dll";
    ASSERT_EQ(ecuio_session_create(("j2534://" + legacy).c_str(), &session),
              ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_on_backend());
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.flags, ECUIO_SESSION_INFO_VBATT);
    EXPECT_STREQ(info.firmware_version, "");
    EXPECT_STREQ(info.api_version, "");
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
