// File/layer: tests/test_j2534_discovery.cpp | GTest, HARNESS tier (registry injection)
// Testing: Win32-only; skipped on Linux/macOS via compile guard

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/core.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstring>

namespace {

TEST(J2534DiscoveryTest, NullArgumentsRejected) {
    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    EXPECT_EQ(ecuio_j2534_discover_devices(nullptr, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_discover_devices(&devices, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(J2534DiscoveryTest, FreeDevicesHandlesNull) {
    /* delete[] nullptr is legal — this test documents that contract */
    ecuio_j2534_free_devices(nullptr);
    /* No assertion needed — just verifies no crash */
}

TEST(J2534DiscoveryTest, DiscoverySucceedsWithConsistentResultOnAnyMachine) {
    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    EXPECT_EQ(ecuio_j2534_discover_devices(&devices, &count), ECUIO_OK);

    /* Array/count consistency: pair or nothing, never one-sided */
    EXPECT_TRUE((devices != nullptr) == (count > 0));

    if (count > 0 && devices != nullptr) {
        EXPECT_LE(count, ECUIO_J2534_MAX_VENDORS);
        for (uint32_t i = 0; i < count; ++i) {
            EXPECT_GT(strlen(devices[i].vendor_name), 0u);
            EXPECT_GT(strlen(devices[i].dll_path), 0u);
            EXPECT_EQ(devices[i].vendor_id, i);   /* discovery-order indexing */
        }
    }

    ecuio_j2534_free_devices(devices);
}

/* Integration test with real registry (HARNESS tier).
 * Cannot be fully unit-tested without mocking Reg* APIs, which
 * adds complexity disproportionate to value at v0.3.0. This test
 * documents behavior on the actual OS. */
TEST(J2534DiscoveryTest, DiscoveryReturnsValidVendorNames) {
#ifdef _WIN32
    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    EXPECT_EQ(ecuio_j2534_discover_devices(&devices, &count), ECUIO_OK);

    if (count > 0 && devices != nullptr) {
        for (uint32_t i = 0; i < count; ++i) {
            /* Vendor names should be non-empty ASCII strings */
            EXPECT_GT(strlen(devices[i].vendor_name), 0u);
            EXPECT_LT(strlen(devices[i].vendor_name),
                      ECUIO_J2534_VENDOR_NAME_LEN);

            /* DLL paths should be absolute paths (typically) */
            /* Don't assert specific format — some vendors may store relative */
        }

        ecuio_j2534_free_devices(devices);
    }
#else
    GTEST_SKIP() << "Skipping J2534 discovery test on non-Windows platform";
#endif
}

} // namespace
