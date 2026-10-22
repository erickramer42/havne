// File/layer: tests/test_j2534_load.cpp | GTest, HARNESS tier
// Testing: Exercises real LoadLibraryEx/export-resolution/open/close
//          against the fake vendor DLLs — no hardware, no polling.
//          The everyday fake (spec spelling) path is baked in by CMake
//          ($<TARGET_FILE:fake_j2534>); the legacy fixture (fallback
//          spelling) via FAKE_J2534_LEGACY_DLL_PATH.

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32
#include <windows.h>
#include "non_ascii_copy.h"
#endif

#include <cstring>

namespace {

#ifdef _WIN32

/* Test's own module reference: bump the refcount so the DLL stays
 * mapped after core closes it, letting us assert on post-close state
 * (GetCurrentHandle == 0) via the hook. Dropped at teardown. */
typedef int (*fn_get_current_handle)(void);
typedef void (*fn_inject_frame)(const unsigned char*, unsigned long,
                                unsigned long, unsigned long, unsigned long);

struct FakeFixture : public ::testing::Test {
    ecuio_j2534_device_t device{};
    HMODULE self_module = nullptr;

    void SetUp() override
    {
        strncpy(device.dll_path, FAKE_J2534_DLL_PATH,
                sizeof(device.dll_path) - 1);
        strcpy(device.vendor_name, "ecuio-fake");
    }

    void TearDown() override
    {
        if (self_module != nullptr) {
            FreeLibrary(self_module);
            self_module = nullptr;
        }
    }

    fn_get_current_handle hook_handle()
    {
        if (self_module == nullptr) {
            self_module = LoadLibraryExA(FAKE_J2534_DLL_PATH, nullptr,
                                         LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (self_module == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<fn_get_current_handle>(
            GetProcAddress(self_module, "GetCurrentHandle"));
    }
};

TEST_F(FakeFixture, NullArgumentsRejected) {
    ecuio_j2534_handle_t* h = nullptr;
    ecuio_j2534_device_t dev{};

    EXPECT_EQ(ecuio_j2534_open(nullptr, &h), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_open(&dev, nullptr), ECUIO_ERR_NULL_ARGUMENT);

    dev.dll_path[0] = '\0';
    EXPECT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(h, nullptr);
}

TEST_F(FakeFixture, MissingDllFailsClean) {
    ecuio_j2534_device_t bogus{};
    strcpy(bogus.vendor_name, "nonexistent");
    strcpy(bogus.dll_path, "C:\\definitely\\not\\a\\real\\vendor.dll");

    ecuio_j2534_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_open(&bogus, &h), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(h, nullptr);

    /* Prove the failed load left nothing mapped behind */
    EXPECT_EQ(GetModuleHandleA(bogus.dll_path), nullptr);
}

TEST_F(FakeFixture, OpenCloseCycleHitsVendorApi) {
    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&device, &h), ECUIO_OK);
    ASSERT_NE(h, nullptr);

    auto hook = hook_handle();
    ASSERT_NE(hook, nullptr);

    EXPECT_EQ(hook(), 1);   /* PassThruOpen ran */

    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);

    /* Fixture holds its own ref, so the DLL survives the close and
     * we can prove PassThruClose ran BEFORE FreeLibrary. */
    EXPECT_EQ(hook(), 0);

    /* Reopen: same DLL, fresh cycle — proves FreeLibrary released it */
    ecuio_j2534_handle_t* h2 = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&device, &h2), ECUIO_OK);
    EXPECT_EQ(hook(), 1);
    EXPECT_EQ(ecuio_j2534_close(h2), ECUIO_OK);
}

TEST_F(FakeFixture, CloseNullIsSafeNoOp) {
    EXPECT_EQ(ecuio_j2534_close(nullptr), ECUIO_OK);
}

/* Spec-spelling PRIMARY resolution (the conformant-vendor case —
 * what the OBDX Pro VX and every spec-following DLL ships): the
 * everyday fake exports 'PassThruIoctl' DIRECTLY. Every routing and
 * surrogate test exercises this resolution implicitly; this pins it
 * by name, including the negative half: the
 * legacy spelling must NOT resolve against this fixture, so a loader
 * that looked up only the legacy name couldn't pass by a coincidental
 * fallback. */
TEST_F(FakeFixture, SpecSpellingPassThruIoctlResolvesAndWorks) {
    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&device, &h), ECUIO_OK);
    ASSERT_NE(h, nullptr);

    /* Negative assertion: the everyday fake must not export the
     * legacy spelling — else this test passes vacuously (either
     * spelling works, nothing is pinned). */
    HMODULE probe = LoadLibraryExA(FAKE_J2534_DLL_PATH, nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(GetProcAddress(probe, "PassThruIoCtl"), nullptr);
    FreeLibrary(probe);

    uint8_t out[4] = {};
    uint32_t out_len = 0;
    /* VBATT through the seam proves the resolved pointer is the right
     * function, not merely non-null. The fake gates on device_id=1,
     * so a loader that passed the channel arg would fail this. */
    EXPECT_EQ(ecuio_j2534_ioctl(h, 0, ECUIO_PT_IOCTL_READ_VBATT,
                                nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 4u);
    unsigned long vbatt = 0;
    memcpy(&vbatt, out, 4);
    EXPECT_EQ(vbatt, 13800ul);   /* the fake's fixed VBATT — right function */

    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);
}

/* Legacy-spelling FALLBACK: the legacy fixture exports
 * 'PassThruIoCtl' ONLY — a spelling some real vendors still ship
 * (the spec's is 'PassThruIoctl', which the VX uses). Resolution must
 * fall back to it and land on a working function. */
TEST_F(FakeFixture, LegacySpellingPassThruIoCtlFallsBackAndWorks) {
    ecuio_j2534_device_t dev{};
    strncpy(dev.dll_path, FAKE_J2534_LEGACY_DLL_PATH, sizeof(dev.dll_path) - 1);
    strcpy(dev.vendor_name, "ecuio-fake-legacy");

    /* The legacy fixture is x64 in this tree — in-proc path, the same
     * seam the surrogate child's resolve helper mirrors. */
    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_OK);
    ASSERT_NE(h, nullptr);

    uint8_t out[4] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_ioctl(h, 0, ECUIO_PT_IOCTL_READ_VBATT,
                                nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 4u);
    unsigned long vbatt = 0;
    memcpy(&vbatt, out, 4);
    EXPECT_EQ(vbatt, 13800ul);

    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);
}

/* dll_path is UTF-8 (j2534.h): a path no ANSI code page can spell
 * still loads, in-proc. Through the A APIs it would fail to load. */
TEST_F(FakeFixture, NonAsciiDllPathLoadsInProc) {
    NonAsciiCopy copy(FAKE_J2534_DLL_PATH);
    ASSERT_TRUE(copy.ok) << "couldn't stage the fake under a non-ASCII path";

    ecuio_j2534_device_t dev{};
    ASSERT_LT(strlen(copy.utf8), sizeof(dev.dll_path));
    memcpy(dev.dll_path, copy.utf8, strlen(copy.utf8) + 1);
    ecuio_j2534_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_open(&dev, &h), ECUIO_OK);
    EXPECT_STREQ(ecuio_j2534_backend_name(h), "inproc");
    EXPECT_EQ(ecuio_j2534_close(h), ECUIO_OK);     /* unloads, so the copy can go */
}

#endif /* _WIN32 */

} // namespace
