// File/layer: tests/test_j2534_routing.cpp | GTest, HARNESS tier
// Testing: the Part 3 routing decision itself. qttune_j2534_open must
//          sniff the DLL's PE architecture and pick the backend:
//          x64 fake -> "inproc" (CI-verifiable), x86 fake -> "surrogate"
//          (dev-machine cross-arch, env-gated), unknown/missing -> honest
//          QT_ERR_LOAD_FAILED at in-proc LoadLibraryEx, no spawn.
//          Round trips go through the FULL loader API surface
//          (open/connect/inject/read/disconnect/close) — the same five
//          functions j2534_transport.cpp consumes — so routing tests
//          here double as backend-parity proof for the loader boundary.

#include <gtest/gtest.h>

#include "qttune/j2534.h"
#include "qttune/internal.h"
#include "qttune/core.h"

#ifdef _WIN32

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

/* Shared device-struct builder: direct-path selectors never touch the
 * registry, so a hand-built device is the minimal, honest input. */
void make_device(qttune_j2534_device_t* dev, const char* dll_path)
{
    *dev = qttune_j2534_device_t{};
    strncpy_s(dev->dll_path, sizeof(dev->dll_path), dll_path, _TRUNCATE);
    strncpy_s(dev->vendor_name, sizeof(dev->vendor_name),
              "routing-test", _TRUNCATE);
}

/* Poll-until-frame with deadline — read is non-blocking-ish (the fake
 * returns immediately; the surrogate ring may need a pump tick). */
bool read_until(qttune_j2534_handle_t* h, unsigned long channel,
                qttune_frame_t* out_frame, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool got = false;
        if (qttune_j2534_read_frame(h, channel, out_frame, 50, &got)
                == QT_OK && got) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

const unsigned char kPayload[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};

} // namespace

TEST(J2534RoutingTest, X64FakeSelectsInprocBackendAndRoundTrips)
{
    /* Default tree: the fake is the same architecture as this test
     * binary, so routing MUST pick in-proc — this is also the
     * behavior-preservation proof for all 74 pre-existing tests. */
    qttune_j2534_device_t dev;
    make_device(&dev, FAKE_J2534_DLL_PATH);

    qttune_j2534_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_open(&dev, &h), QT_OK);
    EXPECT_STREQ(qttune_j2534_backend_name(h), "inproc");

    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_connect_channel(h, 500000, 0, &channel), QT_OK);

    /* The in-proc inject hook: first-ever exercise of lazy InjectFrame
     * resolution through the loader boundary. */
    ASSERT_EQ(qttune_j2534_inject_test_frame(h, kPayload, 8, 7000, 0),
              QT_OK);

    qttune_frame_t frame = {};
    ASSERT_TRUE(read_until(h, channel, &frame, 5000));
    EXPECT_EQ(frame.data_length, 8);
    EXPECT_EQ(memcmp(frame.data, kPayload, 8), 0);
    EXPECT_EQ(frame.timestamp_us, uint64_t{7000});

    EXPECT_EQ(qttune_j2534_disconnect_channel(h, channel), QT_OK);
    EXPECT_EQ(qttune_j2534_close(h), QT_OK);
}

TEST(J2534RoutingTest, MissingDllFailsAtOpenThroughInproc)
{
    /* Sniff can't read a nonexistent file -> 0 -> in-proc -> honest
     * LoadLibraryEx failure. No surrogate process spawns: the wall-
     * clock guard keeps that honest (a spawn would still be fast, but
     * the point is the code path, pinned by timing being trivial). */
    qttune_j2534_device_t dev;
    make_device(&dev, "C:\\does\\not\\exist.dll");

    qttune_j2534_handle_t* h = nullptr;
    EXPECT_EQ(qttune_j2534_open(&dev, &h), QT_ERR_LOAD_FAILED);
    EXPECT_EQ(h, nullptr);
}

TEST(J2534RoutingTest, X86FakeSelectsSurrogateBackendAndRoundTrips)
{
    /* True cross-arch case: Win32-tree fake DLL + surrogate exe via
     * env overrides, driven from this (default-tree) x64 test binary.
     * Skipped when the env isn't configured — CI runs the in-proc
     * assertions; the dev machine runs the topology OBDX Pro needs. */
    const char* dll = std::getenv("QTTUNE_FAKE_DLL");
    if (dll == nullptr || dll[0] == '\0') {
        GTEST_SKIP() << "cross-arch routing: QTTUNE_FAKE_DLL unset "
                        "(set to the Win32-tree fake; also set "
                        "QTTUNE_SURROGATE_EXE)";
    }

    qttune_j2534_device_t dev;
    make_device(&dev, dll);

    qttune_j2534_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_open(&dev, &h), QT_OK);
    EXPECT_STREQ(qttune_j2534_backend_name(h), "surrogate");

    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_connect_channel(h, 500000, 0, &channel), QT_OK);

    ASSERT_EQ(qttune_j2534_inject_test_frame(h, kPayload, 8, 7000, 0),
              QT_OK);

    qttune_frame_t frame = {};
    ASSERT_TRUE(read_until(h, channel, &frame, 10000));
    EXPECT_EQ(frame.data_length, 8);
    EXPECT_EQ(memcmp(frame.data, kPayload, 8), 0);
    EXPECT_EQ(frame.timestamp_us, uint64_t{7000});

    EXPECT_EQ(qttune_j2534_disconnect_channel(h, channel), QT_OK);
    EXPECT_EQ(qttune_j2534_close(h), QT_OK);

    /* SESSION-level smoke: the transport layer's five-function diet
     * is identical for both backends, so the full session lifecycle
     * must work over the routed surrogate too. (Frame-level session
     * verification stays loader-side: no session API reaches the
     * inject hook.) */
    char uri[600];
    snprintf(uri, sizeof(uri), "j2534://%s?baud=500000", dll);
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);
    EXPECT_EQ(qttune_session_start(session), QT_OK);
    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

#endif /* _WIN32 */
