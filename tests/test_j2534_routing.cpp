// File/layer: tests/test_j2534_routing.cpp | GTest, HARNESS tier
// Testing: the PE routing decision itself (ADR-0025, replacing
//          ADR-0009's table). ecuio_j2534_open must sniff the DLL's PE
//          architecture and pick the surrogate: x64 fake -> the x64
//          surrogate (CI-verifiable), x86 fake -> the x86 one
//          (dev-machine cross-arch, env-gated), a missing file ->
//          ECUIO_ERR_LOAD_FAILED from the sniff, no spawn. Round trips
//          go through the FULL loader API surface
//          (open/connect/inject/read/write/ioctl/filter/close) — the
//          same functions j2534_transport.cpp consumes.

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/internal.h"
#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

/* Shared device-struct builder: direct-path selectors never touch the
 * registry, so a hand-built device is the minimal, honest input. */
void make_device(ecuio_j2534_device_t* dev, const char* dll_path)
{
    *dev = ecuio_j2534_device_t{};
    strncpy_s(dev->dll_path, sizeof(dev->dll_path), dll_path, _TRUNCATE);
    strncpy_s(dev->vendor_name, sizeof(dev->vendor_name),
              "routing-test", _TRUNCATE);
}

/* Poll until a message arrives or the deadline passes. A single read
 * can come back empty, so don't rely on the first one. */
bool read_until(ecuio_j2534_handle_t* h, unsigned long channel,
                ecuio_pt_rx_t* out_msg, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool got = false;
        if (ecuio_j2534_read_msg(h, channel, out_msg, 50, &got)
                == ECUIO_OK && got) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

const unsigned char kPayload[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};

/* Flattened-SCONFIG helpers (host LE, memcpy — both participants are
 * x86/x64 and both sides unflatten via memcpy). */
void put_pair(unsigned char* buf, uint32_t param, uint32_t value)
{
    memcpy(buf + 0, &param, 4);
    memcpy(buf + 4, &value, 4);
}

uint32_t get_u32(const unsigned char* buf)
{
    uint32_t v = 0;
    memcpy(&v, buf, 4);
    return v;
}

} // namespace

TEST(J2534RoutingTest, X64FakeSelectsTheX64SurrogateAndRoundTrips)
{
    /* Default tree: the fake is x64, so routing picks the x64 surrogate
     * beside this binary (ecuio-j2534-surrogate-x64.exe), not the
     * process itself. */
    ecuio_j2534_device_t dev;
    make_device(&dev, FAKE_J2534_DLL_PATH);

    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_backend(h), ECUIO_BACKEND_J2534_SURROGATE);
    EXPECT_EQ(ecuio_j2534_dll_arch(h), ECUIO_DLL_ARCH_X64);
    EXPECT_EQ(GetModuleHandleA(FAKE_J2534_DLL_PATH), nullptr)
        << "the fake loaded in this process";

    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_connect_channel(h, 500000, 0, &channel), ECUIO_OK);

    /* The inject hook, through the child's OP_INJECT. */
    ASSERT_EQ(ecuio_j2534_inject_test_frame(h, kPayload, 8, 7000, 0),
              ECUIO_OK);

    ecuio_pt_rx_t frame = {};
    ASSERT_TRUE(read_until(h, channel, &frame, 5000));
    EXPECT_EQ(frame.data_size, 8);
    EXPECT_EQ(memcmp(frame.data, kPayload, 8), 0);
    EXPECT_EQ(frame.timestamp, 7000u);

    EXPECT_EQ(ecuio_j2534_disconnect_channel(h, channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);
}

TEST(J2534RoutingTest, MissingDllIsRefusedBeforeAnySpawn)
{
    /* The sniff can't open a nonexistent file, so open refuses it with
     * the open's Windows error and spawns nothing (ADR-0025). */
    ecuio_j2534_device_t dev;
    make_device(&dev, "C:\\does\\not\\exist.dll");

    ecuio_j2534_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(h, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "couldn't open the DLL (Windows error 3)");
}

/* ---- write/ioctl/filter through the loader helpers, the x64 surrogate
 * here, the x86 one below. ---- */
TEST(J2534RoutingTest, X64WriteIoctlFilterRoundTrips)
{
    ecuio_j2534_device_t dev;
    make_device(&dev, FAKE_J2534_DLL_PATH);

    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_backend(h), ECUIO_BACKEND_J2534_SURROGATE);

    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_connect_channel(h, 500000, 0, &channel), ECUIO_OK);

    /* WRITE through the loader seam. */
    const unsigned char req[3] = {0x01, 0x0C, 0x00};
    const unsigned char expect[3] = {0x41, 0x0C, 0x00};
    uint32_t num_written = 0;
    ASSERT_EQ(ecuio_j2534_write_frame(h, channel, req, 3, 0, 1000,
                                      &num_written),
              ECUIO_OK);
    EXPECT_EQ(num_written, 1u);

    ecuio_pt_rx_t frame = {};
    ASSERT_TRUE(read_until(h, channel, &frame, 5000));
    EXPECT_EQ(frame.data_size, 3);
    EXPECT_EQ(memcmp(frame.data, expect, 3), 0);

    /* IOCTL: SET then GET a config pair through the flattened form. */
    constexpr uint32_t kParam = 0x11;
    constexpr uint32_t kValue = 0xCAFE;
    unsigned char in[8];
    put_pair(in, kParam, kValue);
    ASSERT_EQ(ecuio_j2534_ioctl(h, channel, ECUIO_PT_IOCTL_SET_CONFIG,
                                  in, 8, nullptr, 0, nullptr),
              ECUIO_OK);
    unsigned char out[64];
    uint32_t out_len = 0;
    put_pair(in, kParam, 0);
    ASSERT_EQ(ecuio_j2534_ioctl(h, channel, ECUIO_PT_IOCTL_GET_CONFIG,
                                in, 8, out, sizeof(out), &out_len),
              ECUIO_OK);
    ASSERT_EQ(out_len, 8u);
    EXPECT_EQ(get_u32(out), kParam);
    EXPECT_EQ(get_u32(out + 4), kValue);

    /* Out-of-contract ioctl refused AT THE SEAM. */
    EXPECT_EQ(ecuio_j2534_ioctl(h, channel, 0x05,
                                nullptr, 0, nullptr, 0, nullptr),
              ECUIO_ERR_NOT_IMPLEMENTED);

    /* FILTER: start, stop, stop-twice strict-vendor refusal. */
    const unsigned char mask[4]   = {0x00, 0x00, 0x07, 0xFF};
    const unsigned char pattern[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter_id = 0;
    ASSERT_EQ(ecuio_j2534_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER,
                  mask, 4, pattern, 4, nullptr, 0, 0, &filter_id),
              ECUIO_OK);
    EXPECT_GE(filter_id, 1ul);
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(h, channel, filter_id), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(h, channel, filter_id),
              ECUIO_ERR_VENDOR_REFUSED);

    EXPECT_EQ(ecuio_j2534_disconnect_channel(h, channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);
}

TEST(J2534RoutingTest, X86FakeSelectsSurrogateBackendAndRoundTrips)
{
    /* True cross-arch case: Win32-tree fake DLL + surrogate exe via
     * env overrides, driven from this (default-tree) x64 test binary.
     * Skipped when the env isn't configured — CI runs the x64
     * surrogate's; the dev machine runs the topology OBDX Pro needs.
     * It also runs a WRITE->echo round trip over the routed surrogate
     * — the OBDX topology doing request/response with zero hardware. */
    const char* dll = std::getenv("ECUIO_FAKE_DLL");
    if (dll == nullptr || dll[0] == '\0') {
        GTEST_SKIP() << "cross-arch routing: ECUIO_FAKE_DLL unset "
                        "(set to the Win32-tree fake; also set "
                        "ECUIO_SURROGATE_EXE)";
    }

    ecuio_j2534_device_t dev;
    make_device(&dev, dll);

    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_backend(h), ECUIO_BACKEND_J2534_SURROGATE);

    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_connect_channel(h, 500000, 0, &channel), ECUIO_OK);

    ASSERT_EQ(ecuio_j2534_inject_test_frame(h, kPayload, 8, 7000, 0),
              ECUIO_OK);

    ecuio_pt_rx_t frame = {};
    ASSERT_TRUE(read_until(h, channel, &frame, 10000));
    EXPECT_EQ(frame.data_size, 8);
    EXPECT_EQ(memcmp(frame.data, kPayload, 8), 0);
    EXPECT_EQ(frame.timestamp, 7000u);

    /* WRITE through the loader seam routes to the surrogate with no
     * caller change — backend parity in one assertion. */
    const unsigned char req[3] = {0x01, 0x0D, 0x00};    /* Mode 01, PID 0D */
    const unsigned char expect[3] = {0x41, 0x0D, 0x00};
    uint32_t num_written = 0;
    ASSERT_EQ(ecuio_j2534_write_frame(h, channel, req, 3, 0, 1000,
                                      &num_written),
              ECUIO_OK);
    EXPECT_EQ(num_written, 1u);
    ASSERT_TRUE(read_until(h, channel, &frame, 10000));
    EXPECT_EQ(frame.data_size, 3);
    EXPECT_EQ(memcmp(frame.data, expect, 3), 0);

    /* And the filter pair over the surrogate too. */
    const unsigned char mask[4]   = {0x00, 0x00, 0x07, 0xFF};
    const unsigned char pattern[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter_id = 0;
    ASSERT_EQ(ecuio_j2534_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER,
                  mask, 4, pattern, 4, nullptr, 0, 0, &filter_id),
              ECUIO_OK);
    EXPECT_GE(filter_id, 1ul);
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(h, channel, filter_id), ECUIO_OK);

    EXPECT_EQ(ecuio_j2534_disconnect_channel(h, channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);

    /* SESSION-level smoke: the full session lifecycle over the routed
     * surrogate. (Frame-level session
     * verification stays loader-side: no session API reaches the
     * inject hook.) */
    char uri[600];
    snprintf(uri, sizeof(uri), "j2534://%s?baud=500000&profile=raw", dll);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

#endif /* _WIN32 */
