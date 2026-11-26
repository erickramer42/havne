// File/layer: tests/test_j2534_surrogate_open.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534SurrogateTest (test_j2534_surrogate)

#include "test_j2534_surrogate_support.h"

#ifdef _WIN32

#include "session_log_dir.h"

#include <fcntl.h>
#include <io.h>

/* A vendor PassThruOpen that blocks (adapter absent) must
 * fail at the open's timeout with the child ALIVE, not wedge and
 * not require process-death to detect. The child inherits the parent's
 * environment, so arming the fake-side sleep from here works in the
 * cross-arch configuration too (x64 test -> x86 child). */
TEST(J2534SurrogateTest, BlockedVendorOpenFailsAtHandshakeTimeout)
{
    SetEnvironmentVariableA("ECUIO_FAKE_OPEN_DELAY_MS", "60000");
    /* The open's window only: the child's startup keeps its 8 s, so a
     * loaded machine's slow start isn't taken for the blocked vendor */
    SetEnvironmentVariableA("ECUIO_SUR_OPEN_MS", "2000");
    const auto t0 = std::chrono::steady_clock::now();

    ecuio_j2534_sur_handle_t* h = nullptr;
    SessionLogDir logs;
    const ecuio_status_t rc = ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h);
    const std::string log = logs.text();
    const std::string detail = ecuio_last_error_detail();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    /* Restore BEFORE assertions: a failing EXPECT would otherwise leak
     * the knobs into later tests in this process (sequential runs). */
    SetEnvironmentVariableA("ECUIO_FAKE_OPEN_DELAY_MS", nullptr);
    SetEnvironmentVariableA("ECUIO_SUR_OPEN_MS", nullptr);

    /* The vendor's call never returned: the driver's doing, not the
     * surrogate's. */
    EXPECT_EQ(rc, ECUIO_ERR_VENDOR_REFUSED);
    EXPECT_EQ(detail, "PassThruOpen didn't return within 2000 ms; the "
                      "driver may be waiting for its adapter");
    EXPECT_EQ(h, nullptr);
    /* Each window is judged by the client's own lines, which name the
     * value each wait used, not by the wall clock, which load stretches:
     * the startup kept its 8 s, the open took the knob's 2 s */
    EXPECT_NE(log.find("waiting up to 8000 ms for its pipe"), std::string::npos)
        << log;
    EXPECT_NE(log.find("no reply to OP_OPEN after 2000 ms, child alive"),
              std::string::npos)
        << log;
    /* A floor holds under load: the open was waited out, not cut short */
    EXPECT_GE(elapsed, 2000);
}

/* The client's own lines go into the session log beside the child's, so a GUI launch (no stderr) keeps
 * both halves in one file. Each side writes whole lines, so every line
 * starts "[sur" ("[sur ..." from the client, "[surrogate ..." from the
 * child). */
TEST(J2534SurrogateTest, ClientAndChildLinesShareTheSessionLog)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);
    /* Close first: the child exits, so the file is complete. */
    ASSERT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    ASSERT_FALSE(path.empty()) << "the session ran without a log";

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("[sur "), std::string::npos) << log;
    EXPECT_NE(log.find("open: child pid"), std::string::npos) << log;
    EXPECT_NE(log.find("surrogate '"), std::string::npos)
        << "the log must name the surrogate exe it spawned:\n" << log;
    EXPECT_NE(log.find("[surrogate "), std::string::npos) << log;
    EXPECT_NE(log.find("pipe connected"), std::string::npos) << log;

    int lines = 0;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t eol = log.find('\n', pos);
        if (eol == std::string::npos) eol = log.size();
        std::string line = log.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            ++lines;
            EXPECT_EQ(line.rfind("[sur", 0), 0u) << "spliced line: " << line;
        }
        pos = eol + 1;
    }
    EXPECT_GE(lines, 4);   /* client spawn line + child startup stages */
}

/* A host's stderr may be a stale handle that the session's pipe reuses */
TEST(J2534SurrogateTest, TheClientsLinesNeverReachTheHostsStderr)
{
    char dir[MAX_PATH] = {};
    char path[MAX_PATH] = {};
    ASSERT_GT(GetTempPathA(MAX_PATH, dir), 0u);
    ASSERT_NE(GetTempFileNameA(dir, "ers", 0, path), 0u);
    fflush(stderr);
    const int saved = _dup(_fileno(stderr));
    const int into = _open(path, _O_WRONLY | _O_TRUNC);
    ASSERT_GE(saved, 0);
    ASSERT_GE(into, 0);
    ASSERT_EQ(_dup2(into, _fileno(stderr)), 0);
    ecuio_j2534_sur_handle_t* h = nullptr;
    const ecuio_status_t opened =
        ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h);
    const ecuio_status_t closed = ecuio_j2534_sur_close(h);
    fflush(stderr);
    _dup2(saved, _fileno(stderr));
    _close(saved);
    _close(into);
    EXPECT_EQ(opened, ECUIO_OK);
    EXPECT_EQ(closed, ECUIO_OK);
    const std::string err = read_session_log(path);
    DeleteFileA(path);
    EXPECT_EQ(err.find("[sur "), std::string::npos) << err;
}

namespace {

/* Replaces a variable for one scope and puts the previous value back:
 * the cross-arch run sets ECUIO_SURROGATE_EXE itself. */
struct EnvSwap {
    const char* name;
    bool had = false;
    char old[1024] = {};
    EnvSwap(const char* n, const char* v) : name(n)
    {
        const DWORD k = GetEnvironmentVariableA(n, old, sizeof(old));
        had = k > 0 && k < sizeof(old);
        SetEnvironmentVariableA(n, v);
    }
    ~EnvSwap() { SetEnvironmentVariableA(name, had ? old : nullptr); }
    EnvSwap(const EnvSwap&) = delete;
    EnvSwap& operator=(const EnvSwap&) = delete;
};

} // namespace

/* The surrogate's variable (ECUIO_SURROGATE_EXE, or _X64_EXE)
 * relocates the surrogate and nothing more: a
 * relative path, another program, or a missing file is refused before
 * anything spawns. */
TEST(J2534SurrogateTest, SurrogateExeOverrideIsRestricted)
{
    char self[MAX_PATH] = {};
    ASSERT_GT(GetModuleFileNameA(nullptr, self, MAX_PATH), 0u);
    const std::string too_long =
        "C:/" + std::string(MAX_PATH, 'a') + "/" + surrogate_name();
    const std::string missing =
        std::string("C:/ecuio-definitely-missing/") + surrogate_name();
    for (const char* exe :
         {surrogate_name(), static_cast<const char*>(self), missing.c_str(),
          too_long.c_str()}) {
        EnvSwap swap(surrogate_env(), exe);
        ecuio_j2534_sur_handle_t* h = nullptr;
        EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_ERR_SURROGATE_FAILED) << exe;
        EXPECT_EQ(h, nullptr) << exe;
    }
}

/* A surrogate that exits before it opens the pipe: the client sees the
 * process end during the handshake and names its exit code. Not ours:
 * whoami.exe under the surrogate's name, which refuses the surrogate's
 * two arguments and exits with 1. */
TEST(J2534SurrogateTest, SurrogateExitingAtStartupNamesItsExitCode)
{
    char sys[MAX_PATH] = {};
    char tmp[MAX_PATH] = {};
    ASSERT_GT(GetSystemDirectoryA(sys, MAX_PATH), 0u);
    ASSERT_GT(GetTempPathA(MAX_PATH, tmp), 0u);
    struct Cleanup {
        std::string dir, exe;
        ~Cleanup()
        {
            DeleteFileA(exe.c_str());
            RemoveDirectoryA(dir.c_str());
        }
    } cleanup;
    cleanup.dir = std::string(tmp) + "ecuio-foreign-surrogate-" +
                  std::to_string(GetCurrentProcessId());
    cleanup.exe = cleanup.dir + "\\" + surrogate_name();
    ASSERT_TRUE(CreateDirectoryA(cleanup.dir.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS);
    ASSERT_TRUE(CopyFileA((std::string(sys) + "\\whoami.exe").c_str(),
                          cleanup.exe.c_str(), FALSE));

    EnvSwap swap(surrogate_env(), cleanup.exe.c_str());
    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate exited during startup with code 1");
    EXPECT_EQ(h, nullptr);
}

/* The client and the child each carry a fingerprint of the shared
 * headers, so trees built from different headers refuse each other at
 * OP_OPEN. ECUIO_SUR_TEST_FINGERPRINT stands in for a child from
 * another build. The refusal comes before the vendor is called: the
 * fake's open is armed to block for 60 s, so a child that called
 * PassThruOpen would hold the open until the 8 s handshake deadline. */
TEST(J2534SurrogateTest, MismatchedFingerprintRefusesOpenBeforeVendor)
{
    EnvArm fingerprint("ECUIO_SUR_TEST_FINGERPRINT", "1");
    EnvArm blocked_open("ECUIO_FAKE_OPEN_DELAY_MS", "60000");
    const auto t0 = std::chrono::steady_clock::now();

    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(h, nullptr);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5000) << "the child called PassThruOpen";
}

/* The DLL path reaches the child as UTF-16 (wmain), so a path no ANSI
 * code page can spell loads there too. */
TEST(J2534SurrogateTest, NonAsciiDllPathLoadsInTheChild)
{
    NonAsciiCopy copy(fake_dll_path());
    ASSERT_TRUE(copy.ok) << "couldn't stage the fake under a non-ASCII path";
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(copy.utf8, fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    EXPECT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);     /* the child exits, so the copy can go */
}

/* ECUIO_SUR_LOG_FRAMES=1 puts every message written and read into the
 * session log verbatim: status, flags, the vendor timestamp and the
 * bytes. Without it the log keeps its counts and status changes only. */
TEST(J2534SurrogateTest, FrameLoggingRecordsPayloadsVerbatimWhenEnabled)
{
    const uint8_t req[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    const uint8_t rsp[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    std::string logs[2];   /* [0] disabled, [1] enabled */

    for (int enabled = 1; enabled >= 0; --enabled) {
        EnvArm arm("ECUIO_SUR_LOG_FRAMES", enabled ? "1" : nullptr);
        ecuio_j2534_sur_handle_t* h = nullptr;
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
        unsigned long channel = 0;
        ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

        uint32_t written = 0;
        ASSERT_EQ(ecuio_j2534_sur_write_frame(h, channel, req, sizeof(req),
                                              ECUIO_PT_TX_ISO15765_FRAME_PAD,
                                              0, &written), ECUIO_OK);
        ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, rsp, sizeof(rsp),
                                                    4242, 0), ECUIO_OK);
        /* The fake returns one message per read: the write's echo, then
         * the injected frame. Extra reads come back empty. */
        for (int i = 0; i < 4; ++i) {
            ecuio_pt_rx_t f = {};
            bool got = false;
            (void)ecuio_j2534_sur_read_msg(h, channel, &f, 20, &got);
        }

        const std::string path = ecuio_j2534_sur_log_path(h);
        ASSERT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);     /* child exits: log complete */
        ASSERT_FALSE(path.empty());
        logs[enabled] = read_session_log(path);
    }

    const std::string& on = logs[1];
    EXPECT_NE(on.find("frame logging on (ECUIO_SUR_LOG_FRAMES=1)"), std::string::npos) << on;
    EXPECT_NE(on.find("OP_WRITE frame: status 0, tx 0x00000040, len 6: 00 00 07 E0 01 0C"),
              std::string::npos) << on;
    EXPECT_NE(on.find("ts 4242, len 8: 00 00 07 E8 41 0C 1F 40"), std::string::npos) << on;

    const std::string& off = logs[0];
    EXPECT_EQ(off.find(" frame: "), std::string::npos) << "frame lines without the knob:\n" << off;
    EXPECT_NE(off.find("OP_WRITE: status 0"), std::string::npos) << off;
}

#endif // _WIN32
