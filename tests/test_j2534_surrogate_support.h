// File/layer: tests/test_j2534_surrogate_support.h | GTest, HARNESS tier
// Testing: HARNESS: the helpers of test_j2534_surrogate*.cpp (test_j2534_surrogate)

#pragma once

#include <gtest/gtest.h>

#include "ecuio/j2534_surrogate.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <windows.h>

#include "fake_control.h"
#include "non_ascii_copy.h"

/* Env override for the cross-arch case: an x86 surrogate can't load
 * the x64 fake baked at generate time, so the Win32 tree's fake DLL
 * takes precedence when ECUIO_FAKE_DLL is set. Mirrors the client's
 * ECUIO_SURROGATE_EXE override. */
inline const char* fake_dll_path()
{
    const char* env = std::getenv("ECUIO_FAKE_DLL");
    return (env != nullptr && env[0] != '\0') ? env : FAKE_J2534_DLL_PATH;
}

/* The fake's architecture, which picks the surrogate (ADR-0025): the
 * Win32 tree's x86 one in the cross-arch run, this tree's x64 one
 * otherwise. */
inline uint32_t fake_dll_arch()
{
    const char* env = std::getenv("ECUIO_FAKE_DLL");
    return (env != nullptr && env[0] != '\0') ? ECUIO_DLL_ARCH_X86
                                               : ECUIO_DLL_ARCH_X64;
}

/* That surrogate's relocation variable and exe name. */
inline const char* surrogate_env()
{
    return fake_dll_arch() == ECUIO_DLL_ARCH_X86 ? "ECUIO_SURROGATE_EXE"
                                                 : "ECUIO_SURROGATE_X64_EXE";
}
inline const char* surrogate_name()
{
    return fake_dll_arch() == ECUIO_DLL_ARCH_X86
               ? "ecuio-j2534-surrogate.exe"
               : "ecuio-j2534-surrogate-x64.exe";
}

namespace {

struct SurCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][64]{};
    uint32_t rx_status[8]{};
    uint32_t timestamps[8]{};
};

inline void sur_capture(const ecuio_pt_rx_t* msg, SurCapture* cap)
{
    const int i = cap->count.fetch_add(1);
    if (i < 8) {
        memcpy(cap->payloads[i], msg->data,
               (msg->data_size < 64) ? msg->data_size : 64);
        cap->rx_status[i] = msg->rx_status;
        cap->timestamps[i] = msg->timestamp;
    }
}

/* Poll-until-condition with deadline, no wall-clock bets: a starved
 * CI runner can take far longer than the local cadence suggests. */
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms, int step_ms = 10)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

/* Drain the surrogate ring via the client read API into a capture. */
inline void drain_sur_frames(ecuio_j2534_sur_handle_t* h, SurCapture* cap)
{
    for (;;) {
        ecuio_pt_rx_t f = {};
        bool got = false;
        ecuio_status_t st = ecuio_j2534_sur_read_msg(h, 0, &f, 50, &got);
        if (st != ECUIO_OK) return;
        if (!got) return;
        sur_capture(&f, cap);
    }
}

inline uint32_t get_u32(const unsigned char* buf)
{
    uint32_t v = 0;
    memcpy(&v, buf, 4);
    return v;
}

} // namespace

/* Shares read access with the session's writers. "" if it can't open.
 * The path is UTF-8 (ecuio_j2534_sur_log_path). */
inline std::string read_session_log(const std::string& path)
{
    std::string out;
    wchar_t wpath[MAX_PATH + 64] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath,
                            static_cast<int>(MAX_PATH + 64)) <= 0) {
        return out;
    }
    HANDLE f = CreateFileW(wpath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(f);
    return out;
}

namespace {

/* Sets an environment variable for one scope. The child snapshots its
 * environment at spawn, so arm before open; the destructor clears the
 * variable even when an ASSERT returns early. */
struct EnvArm {
    const char* name;
    EnvArm(const char* n, const char* v) : name(n) { SetEnvironmentVariableA(n, v); }
    ~EnvArm() { SetEnvironmentVariableA(name, nullptr); }
    EnvArm(const EnvArm&) = delete;
    EnvArm& operator=(const EnvArm&) = delete;
};

} // namespace

#endif // _WIN32
