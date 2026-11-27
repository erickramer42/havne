// File/layer: tests/test_j2534_connect.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534ConnectTest, J2534SurrogateConnectTest
//          (test_j2534_connect)

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/profile.h"

#ifdef _WIN32
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "j2534_backend_fixture.h"
#include "new_counter.h"
#include "open_handles.h"
#include "surrogate_log_names.h"

#include <tlhelp32.h>

namespace {

using j2534_backends::EnvSwap;

/* The allocation the out-of-memory case's create runs short at, 0 between
 * creates: one still set at TearDown threw past the test body */
unsigned long oom_create_at = 0;

class J2534ConnectTest : public j2534_backends::BackendTest {
protected:
    ecuio_session_t* session = nullptr;
    /* The out-of-memory case's OwnTemp, freed in TearDown: a throw past
     * the test body may skip its frame's destructors */
    std::shared_ptr<void> own_temp;

    /* The cross-arch run's surrogate hosts the Win32 tree's fake; every
     * other case loads this tree's. */
    uint32_t fake_arch() const
    {
        const char* cross = std::getenv("ECUIO_FAKE_DLL");
        return (cross != nullptr && cross[0] != '\0') ? ECUIO_DLL_ARCH_X86
                                                      : ECUIO_DLL_ARCH_X64;
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
        if (oom_create_at != 0) {
            ADD_FAILURE() << "create threw at allocation " << oom_create_at;
            oom_create_at = 0;
        }
        if (session != nullptr) {
            (void)ecuio_session_close(session);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        own_temp.reset();
        BackendTest::TearDown();
    }
};

TEST_F(J2534ConnectTest, RefusedOpenNamesTheCallStatusAndVendorText)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OPEN_STATUS", "8"));
    EXPECT_EQ(ecuio_session_create(uri("").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(session, nullptr);
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "PassThruOpen returned status 8 (0x8, ERR_DEVICE_NOT_CONNECTED): "
        "fake_j2534: PassThruOpen refused (ECUIO_FAKE_OPEN_STATUS)");
}

/* The OBDX Pro VT and VX refuse to open with 34 when no adapter is
 * attached; J2534-1 v04.04 defines no status 34. */
TEST_F(J2534ConnectTest, AVendorsOwnStatusIsCalledVendorSpecific)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_OPEN_STATUS", "34"));
    EXPECT_EQ(ecuio_session_create(uri("").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruOpen returned status 34 (0x22, vendor-specific): "
                 "fake_j2534: PassThruOpen refused (ECUIO_FAKE_OPEN_STATUS)");
}

TEST_F(J2534ConnectTest, RefusedConnectNamesTheBaud)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_CONNECT_STATUS", "0x19"));
    EXPECT_EQ(ecuio_session_create(uri("?baud=250000").c_str(), &session),
              ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(session, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruConnect (ISO 15765 at 250000 baud) returned status "
                 "25 (0x19, ERR_INVALID_BAUDRATE): fake_j2534: PassThruConnect "
                 "refused (ECUIO_FAKE_CONNECT_STATUS)");
}

/* Filters register at start, so a refused one fails the start. */
TEST_F(J2534ConnectTest, RefusedFilterFailsTheStartWithTheVendorText)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_STATUS", "7"));
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruStartMsgFilter returned status 7 (0x7, ERR_FAILED): "
                 "fake_j2534: PassThruStartMsgFilter refused "
                 "(ECUIO_FAKE_FILTER_STATUS)");
}

/* A DLL that isn't there, or a file that isn't a DLL, is refused by the
 * sniff with no spawn (ADR-0025); a PE for another machine goes to the
 * x64 surrogate, whose load refuses it (exit 4), as the app's own
 * process would. A path that isn't UTF-8 never reaches a load. */
TEST_F(J2534ConnectTest, MissingDllFailsToLoad)
{
    EXPECT_EQ(ecuio_session_create(
                  "j2534://C:/ecuio-definitely-missing/fake_j2534.dll",
                  &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "couldn't open the DLL (Windows error 3)");

    /* Text; an MS-DOS header with no PE header behind it: no PE, so
     * no spawn. A PE header for an ARM64 image: the child's load. */
    std::string text = "not a DLL";
    std::string mz_only(64, '\0');
    mz_only[0] = 'M';
    mz_only[1] = 'Z';
    std::string arm64 = mz_only;
    arm64[0x3C] = 0x40; /* e_lfanew: the PE header follows at 64 */
    arm64 += std::string("PE\0\0", 4) + "\x64\xAA" + std::string(260, '\0');
    char tmp[MAX_PATH] = {};
    ASSERT_GT(GetTempPathA(MAX_PATH, tmp), 0u);
    const std::string path = std::string(tmp) + "ecuio-not-a-dll-" +
                             std::to_string(GetCurrentProcessId()) + ".dll";
    struct Remove {
        std::string path;
        ~Remove() { DeleteFileA(path.c_str()); }
    } remove{path};
    struct Case {
        const std::string* bytes;
        const char* detail;
    };
    for (const Case& c :
         {Case{&text, "the file isn't a Windows DLL"},
          Case{&mz_only, "the file isn't a Windows DLL"},
          Case{&arm64, "the surrogate couldn't load the DLL (the Windows "
                       "error is in its session log, in %TEMP%)"}}) {
        FILE* f = fopen(path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        fwrite(c.bytes->data(), 1, c.bytes->size(), f);
        fclose(f);
        EXPECT_EQ(ecuio_session_create(("j2534://" + path).c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), c.detail);
    }

    EXPECT_EQ(ecuio_session_create("j2534://C:/ecuio-\xFF.dll", &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the DLL path isn't valid UTF-8, or is too long");
    EXPECT_EQ(session, nullptr);
}

/* A DLL that loads but isn't a J2534 driver: kernel32 exports none of
 * the nine mandatory calls. In-proc the loader names the first one it
 * missed; through the surrogate the child exits with code 5 and its
 * session log names them all. (An x86 child reads System32 as
 * SysWOW64, so the cross-arch run loads a kernel32 too.) */
TEST_F(J2534ConnectTest, ADllWithoutTheJ2534ExportsFailsToLoad)
{
    char sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    ASSERT_GT(n, 0u);
    ASSERT_LT(n, static_cast<UINT>(MAX_PATH));
    const std::string u = std::string("j2534://") + sys + "\\kernel32.dll";
    EXPECT_EQ(ecuio_session_create(u.c_str(), &session), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(session, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the DLL lacks a mandatory J2534 export (the "
                 "surrogate's session log, in %TEMP%, names it)");
}

/* A relative path never reaches a load, whoever passes it: the URI's
 * selector is refused earlier as BAD_URI, so this calls the loader
 * directly, as a registry entry would reach it. The current directory
 * (which the child inherits) holds the fake, so a relative load would
 * find it there and open it. */
TEST_F(J2534ConnectTest, ARelativeDllPathIsRefusedBeforeAnyLoad)
{
    const size_t sep = dll.find_last_of("\\/");
    ASSERT_NE(sep, std::string::npos) << dll;
    const std::string dir = dll.substr(0, sep);
    const std::string name = dll.substr(sep + 1);

    char old_dir[MAX_PATH] = {};
    ASSERT_GT(GetCurrentDirectoryA(MAX_PATH, old_dir), 0u);
    ASSERT_TRUE(SetCurrentDirectoryA(dir.c_str())) << dir;
    struct Restore {
        const char* dir;
        ~Restore() { SetCurrentDirectoryA(dir); }
    } restore{old_dir};

    for (const std::string& path : {name, ".\\" + name, "\\" + name}) {
        SCOPED_TRACE(path);
        ecuio_j2534_device_t dev{};
        strncpy(dev.dll_path, path.c_str(), sizeof(dev.dll_path) - 1);
        ecuio_j2534_handle_t* h = nullptr;
        EXPECT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(h, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "the DLL path must be absolute, like "
                     "C:\\path\\to\\vendor.dll");
        if (h != nullptr) {
            ecuio_j2534_close(h);
        }
    }
    EXPECT_EQ(fake_control::opener_pid(page), 0ul) << "the fake opened";
}

/* A connect that works leaves nothing behind from the failure before. */
TEST_F(J2534ConnectTest, SuccessClearsTheDetail)
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
TEST_F(J2534ConnectTest, InfoDescribesTheAdapter)
{
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.backend, ECUIO_BACKEND_J2534_SURROGATE);
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
    EXPECT_EQ(info.flags, ECUIO_SESSION_INFO_VBATT |
                              ECUIO_SESSION_INFO_VERSIONS |
                              ECUIO_SESSION_INFO_REQUESTS);
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
TEST_F(J2534ConnectTest, InfoCarriesTheUrisBaudAndChannel)
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
TEST_F(J2534ConnectTest, RefusedReadingsLeaveTheirFlagsClear)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VBATT_STATUS", "7"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VERSION_STATUS", "7"));
    ASSERT_EQ(ecuio_session_create(uri("").c_str(), &session), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.flags, ECUIO_SESSION_INFO_REQUESTS);
    EXPECT_EQ(info.vbatt_mv, 0u);
    EXPECT_STREQ(info.firmware_version, "");
    EXPECT_STREQ(info.dll_version, "");
    EXPECT_STREQ(info.api_version, "");
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
}

/* Below the connect path, the loader seam's helpers (ADR-0018, appendix
 * of 2026-10-23), called directly: a NULL pointer, a length or shape
 * refused before any vendor call, an ioctl outside the whitelist and a
 * vendor's refusal each have their own code, the same on both
 * backends. */
TEST_F(J2534ConnectTest, HelperFailuresHaveOneCodePerCause)
{
    struct HandleGuard {
        ecuio_j2534_handle_t* h = nullptr;
        ~HandleGuard() { (void)ecuio_j2534_close(h); }
    } guard;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &guard.h), ECUIO_OK);
    ecuio_j2534_handle_t* h = guard.h;
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    unsigned long ch = 0;
    ASSERT_EQ(ecuio_j2534_connect_channel(h, 0, 0, &ch), ECUIO_OK);

    /* Refused before the vendor: no write reaches the fake. */
    const uint8_t req[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    bool got = false;
    EXPECT_EQ(ecuio_j2534_connect_channel(h, 0, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_disconnect_channel(nullptr, ch),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_read_msg(h, ch, nullptr, 0, &got),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_ioctl(nullptr, ch, ECUIO_PT_IOCTL_CLEAR_RX_BUFFER,
                                nullptr, 0, nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(nullptr, ch, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_read_version(h, nullptr, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_inject_test_frame(h, nullptr, 6, 0, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_inject_test_frame(h, req, 0, 0, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_inject_test_frame(h, req, 6, 0, 0), ECUIO_OK)
        << "the test seam reaches the fake in the child";
    EXPECT_EQ(ecuio_j2534_backend(nullptr), 0u);
    const uint8_t big[65] = {};
    uint32_t written = 0;
    const uint32_t writes0 = fake_control::writes(page);
    EXPECT_EQ(ecuio_j2534_write_frame(h, ch, nullptr, 6, 0, 0, &written),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_write_frame(h, ch, req, 0, 0, 0, &written),
              ECUIO_ERR_INVALID_ARGUMENT);
    const std::vector<uint8_t> past(ECUIO_PT_WRITE_LONG_MAX + 1);
    EXPECT_EQ(ecuio_j2534_write_frame(h, ch, past.data(),
                                      ECUIO_PT_WRITE_LONG_MAX + 1, 0, 0,
                                      &written),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(fake_control::writes(page), writes0);

    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, ECUIO_PT_IOCTL_SET_CONFIG, big, 7,
                                nullptr, 0, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT) << "not whole pairs";
    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, ECUIO_PT_IOCTL_SET_CONFIG, nullptr, 0,
                                nullptr, 0, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT) << "no pairs";
    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, ECUIO_PT_IOCTL_SET_CONFIG, big, 65,
                                nullptr, 0, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT) << "past the wire's 64 bytes";
    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, ECUIO_PT_IOCTL_SET_CONFIG, nullptr, 8,
                                nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT)
        << "a pair promised, none given";
    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, 0x05 /* FIVE_BAUD_INIT */, nullptr, 0,
                                nullptr, 0, nullptr),
              ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, ECUIO_PT_IOCTL_CLEAR_RX_BUFFER, nullptr,
                                0, nullptr, 0, nullptr),
              ECUIO_OK)
        << "on the whitelist, so it reaches the vendor";

    const uint8_t mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t id[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long fid = 0;
    EXPECT_EQ(ecuio_j2534_start_msg_filter(h, ch, ECUIO_PT_PASS_FILTER, mask,
                                           4, id, 3, nullptr, 0, 0, &fid),
              ECUIO_ERR_INVALID_ARGUMENT) << "mask and pattern differ";
    EXPECT_EQ(ecuio_j2534_start_msg_filter(h, ch, ECUIO_PT_PASS_FILTER, mask,
                                           4, id, 4, id, 4, 0, &fid),
              ECUIO_ERR_INVALID_ARGUMENT) << "flow control on a pass filter";
    EXPECT_EQ(ecuio_j2534_start_msg_filter(h, ch, ECUIO_PT_FLOW_CONTROL_FILTER,
                                           mask, 4, id, 4, nullptr, 4, 0,
                                           &fid),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_start_msg_filter(h, ch, ECUIO_PT_FLOW_CONTROL_FILTER,
                                           mask, 4, id, 4, id, 3, 0, &fid),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "flow control a byte short";
    EXPECT_EQ(ecuio_j2534_start_msg_filter(h, ch, ECUIO_PT_PASS_FILTER, nullptr,
                                           4, id, 4, nullptr, 0, 0, &fid),
              ECUIO_ERR_NULL_ARGUMENT);

    /* The vendor's refusals. */
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", "7"));
    EXPECT_EQ(ecuio_j2534_write_frame(h, ch, req, 6, 0, 0, &written),
              ECUIO_ERR_VENDOR_REFUSED);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VBATT_STATUS", "7"));
    uint8_t mv[4] = {};
    uint32_t mv_len = 0;
    EXPECT_EQ(ecuio_j2534_ioctl(h, ch, ECUIO_PT_IOCTL_READ_VBATT, nullptr, 0,
                                mv, sizeof(mv), &mv_len),
              ECUIO_ERR_VENDOR_REFUSED);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VERSION_STATUS", "7"));
    char fw[ECUIO_SESSION_INFO_TEXT_LEN], dl[ECUIO_SESSION_INFO_TEXT_LEN],
        api[ECUIO_SESSION_INFO_TEXT_LEN];
    EXPECT_EQ(ecuio_j2534_read_version(h, fw, dl, api),
              ECUIO_ERR_VENDOR_REFUSED);
    ASSERT_EQ(ecuio_j2534_start_msg_filter(h, ch, ECUIO_PT_PASS_FILTER, mask,
                                           4, id, 4, nullptr, 0, 0, &fid),
              ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(h, ch, fid), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(h, ch, fid),
              ECUIO_ERR_VENDOR_REFUSED) << "a strict vendor's second stop";

    EXPECT_EQ(ecuio_j2534_disconnect_channel(h, ch), ECUIO_OK);
}

/* PassThruReadVersion is optional, as PassThruGetLastError is: the
 * legacy fixture, beside the fake in each tree, doesn't export it. */
TEST_F(J2534ConnectTest, ADllWithoutReadVersionReportsNoVersions)
{
    const std::string legacy =
        dll.substr(0, dll.find_last_of("\\/") + 1) + "fake_j2534_legacy.dll";
    ASSERT_EQ(ecuio_session_create(("j2534://" + legacy).c_str(), &session),
              ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    const ecuio_session_info_t info = info_of_session();
    EXPECT_EQ(info.flags,
              ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_REQUESTS);
    EXPECT_STREQ(info.firmware_version, "");
    EXPECT_STREQ(info.api_version, "");
}

/* ---- Out of memory in create (tests.md's out-of-memory loops) ---- */

namespace fs = std::filesystem;

/* The surrogates this process has running: the leak check. Another test
 * process's children have another parent. A child closed a moment ago
 * may still be listed, so it waits up to 10 s for none. */
unsigned surrogate_children()
{
    static const wchar_t kName[] = L"ecuio-j2534-surrogate";
    const DWORD self = GetCurrentProcessId();
    unsigned n = 0;
    for (int waited_ms = 0;; waited_ms += 10) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            ADD_FAILURE() << "no process snapshot (error " << GetLastError()
                          << ")";
            return 0;
        }
        n = 0;
        PROCESSENTRY32W e{};
        e.dwSize = sizeof(e);
        for (BOOL more = Process32FirstW(snap, &e); more;
             more = Process32NextW(snap, &e)) {
            if (e.th32ParentProcessID == self &&
                _wcsnicmp(e.szExeFile, kName, wcslen(kName)) == 0) {
                ++n;
            }
        }
        CloseHandle(snap);
        if (n == 0 || waited_ms >= 10000) {
            return n;
        }
        Sleep(10);
    }
}

/* TMP and TEMP (GetTempPathW reads TMP first) name a folder of the case's
 * own while it lives: the session logs go there, and the folder's names
 * decide the prune's allocations. */
class OwnTemp {
public:
    OwnTemp()
    {
        wchar_t base[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH + 1, base);
        dir_ = fs::path(base) /
               (L"havne-j2534-oom-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
        const std::wstring value = dir_.wstring() + L"\\";
        for (Saved& v : saved_) {
            const DWORD n = GetEnvironmentVariableW(v.name, nullptr, 0);
            v.had = n > 0;
            if (v.had) {
                v.value.resize(n);
                GetEnvironmentVariableW(v.name, &v.value[0], n);
                v.value.resize(n - 1);
            }
            SetEnvironmentVariableW(v.name, value.c_str());
        }
    }
    ~OwnTemp()
    {
        for (const Saved& v : saved_) {
            SetEnvironmentVariableW(v.name, v.had ? v.value.c_str() : nullptr);
        }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    OwnTemp(const OwnTemp&) = delete;
    OwnTemp& operator=(const OwnTemp&) = delete;

    /* SUR_LOG_KEEP + 1 of the names a concurrent session's log takes, so
     * each prune holds them all and deletes one: the one it took is put
     * back before the next try */
    void top_up() const
    {
        for (size_t i = 0; i <= SUR_LOG_KEEP; ++i) {
            const std::wstring name =
                L"ecuio-surrogate-9" + std::to_wstring(i) + L"-1.log";
            ASSERT_TRUE(sur_log_is_per_spawn_name(name.c_str()));
            const fs::path p = dir_ / name;
            if (!fs::exists(p)) {
                std::ofstream(p) << 'x';
            }
        }
    }

private:
    struct Saved {
        const wchar_t* name;
        bool had = false;
        std::wstring value;
    };
    fs::path dir_;
    Saved saved_[2] = {{L"TMP"}, {L"TEMP"}};
};

struct CreateLoop {
    unsigned long news = 0;
    unsigned long refused = 0;
    unsigned long spawned = 0;
    unsigned long refused_after_spawn = 0;
};

/* An OK session holds every module, and the last one's report */
void expect_whole(ecuio_session_t* s, uint32_t modules, unsigned long n)
{
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK) << "at " << n;
    EXPECT_EQ(info.module_count, modules) << "at " << n;
    ecuio_channel_decl_t last{};
    ASSERT_EQ(ecuio_session_get_module(s, modules - 1, &last), ECUIO_OK)
        << "at " << n;
    ecuio_channel_support_t support{};
    ASSERT_EQ(ecuio_session_get_channel_support(s, modules - 1, &support),
              ECUIO_OK)
        << "at " << n;
    EXPECT_EQ(support.rx_id, last.rx_id) << "at " << n;
}

/* Create with its nth allocation failed: answered, nothing thrown, and the
 * child it spawned shut down and gone */
void one_try(const std::string& uri, unsigned long n, uint32_t modules,
             const OwnTemp& temp, fake_control::Page* page, CreateLoop* c)
{
    ASSERT_NO_FATAL_FAILURE(temp.top_up());
    fake_control::reset(page);
    ecuio_session_t* s = nullptr;
    /* MSVC lets a throw out of the C call pass any catch here (/EHsc), so
     * the fixture's TearDown names the allocation */
    oom_create_at = n;
    new_counter::fail_new(n);
    const ecuio_status_t rc = ecuio_session_create(uri.c_str(), &s);
    new_counter::fail_new(0);
    oom_create_at = 0;
    const bool spawned = fake_control::opener_pid(page) != 0;
    if (rc == ECUIO_OK) {
        ASSERT_NE(s, nullptr) << "at " << n;
        EXPECT_NO_FATAL_FAILURE(expect_whole(s, modules, n));
        ecuio_session_destroy(s);
    } else {
        ++c->refused;
        c->refused_after_spawn += spawned ? 1 : 0;
        EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY)
            << "at " << n << ": " << ecuio_last_error_detail();
        EXPECT_EQ(s, nullptr) << "at " << n;
    }
    if (spawned) {
        ++c->spawned;
        EXPECT_EQ(fake_control::closes(page), 1u)
            << "at " << n << ": the child wasn't shut down";
    }
    EXPECT_EQ(surrogate_children(), 0u)
        << "at " << n << ": a surrogate outlived its create";
}

/* Every allocation of one URI's create in turn */
void create_loop(const std::string& uri, const OwnTemp& temp,
                 fake_control::Page* page, uint32_t modules, CreateLoop* c)
{
    ASSERT_NO_FATAL_FAILURE(temp.top_up());
    ecuio_session_t* s = nullptr;
    const unsigned long before = new_counter::on_this_thread();
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    c->news = new_counter::on_this_thread() - before;
    ASSERT_NO_FATAL_FAILURE(expect_whole(s, modules, 0));
    ecuio_session_destroy(s);
    ASSERT_GT(c->news, 5u);
    /* Windows makes a thread-pool wait handle at a process's second session
     * and keeps it, so the count starts after a second create */
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    ecuio_session_destroy(s);
    ASSERT_EQ(surrogate_children(), 0u);
    const unsigned long handles = open_handles();
    for (unsigned long n = 1; n <= c->news; ++n) {
        ASSERT_NO_FATAL_FAILURE(one_try(uri, n, modules, temp, page, c));
    }
    EXPECT_EQ(open_handles(), handles) << "a refused create left a handle open";
}

/* Out of memory at each of create's allocations in turn, for obd2's
 * default (the probe, identification, diagnostics, the timing table) and
 * a session of six modules with a DID scan: OK or OUT_OF_MEMORY, nothing
 * thrown, an OK session whole, and no surrogate or handle left. Only the
 * tries failing at or past the spawn start a child. */
TEST_F(J2534ConnectTest, OutOfMemoryAnywhereInCreateIsAnsweredNotThrown)
{
    own_temp = std::make_shared<OwnTemp>();
    const OwnTemp& temp = *static_cast<const OwnTemp*>(own_temp.get());
    const struct {
        const char* query;
        uint32_t modules;
    } cases[] = {
        {"", 1},
        {"?profile=uds-generic&did=1E1C,7E9:D800-D801,7EA:D800-D801,"
         "7EB:D800-D801,7EC:D800-D801,7ED:D800-D801&identify=0&diagnose=0",
         6},
    };
    for (const auto& one : cases) {
        SCOPED_TRACE(one.query);
        CreateLoop c;
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_NO_FATAL_FAILURE(
            create_loop(uri(one.query), temp, page, one.modules, &c));
        const double s = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        std::printf("[ loop ] '%s': %lu allocations, %lu spawned a child, "
                    "%lu refused, %.2f s\n",
                    one.query, c.news, c.spawned, c.refused, s);
        EXPECT_GT(c.refused, 0u);
        EXPECT_GT(c.refused_after_spawn, 0u) << "no refusal reached the spawn";
    }
}

/* The surrogate's own failures: the x64 fake in the x64 surrogate, or
 * the cross-arch run's x86 fake in the x86 one. */
class J2534SurrogateConnectTest : public ::testing::Test {
protected:
    std::string dll;
    bool x86 = false;

    void SetUp() override
    {
        const char* cross = std::getenv("ECUIO_FAKE_DLL");
        x86 = cross != nullptr && cross[0] != '\0';
        dll = x86 ? cross : FAKE_J2534_DLL_PATH;
    }

    std::string uri() const { return "j2534://" + dll; }
};

/* Each architecture's surrogate is found by its own name and variable,
 * and a missing one fails the session: nothing loads in this process. */
TEST_F(J2534SurrogateConnectTest, MissingSurrogateExeIsASurrogateFailure)
{
    const char* name = x86 ? "ecuio-j2534-surrogate.exe"
                           : "ecuio-j2534-surrogate-x64.exe";
    const char* var = x86 ? "ECUIO_SURROGATE_EXE" : "ECUIO_SURROGATE_X64_EXE";
    EnvSwap exe(var, (std::string("C:/ecuio-definitely-missing/") + name).c_str());
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri().c_str(), &session),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(session, nullptr);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string("no surrogate (") + name + "): " + var +
                  " names no file");
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
