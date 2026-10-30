// File/layer: tests/test_j2534_discovery.cpp | GTest, HARNESS tier (the live registry)
// Testing: Win32-only; skipped on Linux/macOS via compile guard. Against
//          the machine's own registry: the shape of the result anywhere,
//          and at least ECUIO_EXPECT_J2534_DEVICES devices where that
//          variable is set (scripts/verify.sh sets it to 1 on the dev
//          machine, where a Pass-Thru driver is installed), so the test
//          can't pass vacuously there.

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/core.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <cstring>

namespace {

/* The devices the machine is declared to have, from
 * ECUIO_EXPECT_J2534_DEVICES; 0 when unset or not a positive number */
uint32_t expected_devices() {
    const char* value = std::getenv("ECUIO_EXPECT_J2534_DEVICES");
    if (value == nullptr) return 0;
    const long n = std::strtol(value, nullptr, 10);
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}

TEST(J2534DiscoveryTest, NullArgumentsRejected) {
    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    EXPECT_EQ(ecuio_j2534_discover_devices(nullptr, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_discover_devices(&devices, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(J2534DiscoveryTest, FreeDevicesHandlesNull) {
    /* delete[] nullptr is legal: this test documents that contract */
    ecuio_j2534_free_devices(nullptr);
    /* No assertion needed: just verifies no crash */
}

TEST(J2534DiscoveryTest, DiscoverySucceedsWithConsistentResultOnAnyMachine) {
    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    EXPECT_EQ(ecuio_j2534_discover_devices(&devices, &count), ECUIO_OK);

    /* Array/count consistency: pair or nothing, never one-sided */
    EXPECT_TRUE((devices != nullptr) == (count > 0));

    /* Where the machine is declared to have drivers, the shape checks
     * below must run on real entries: an empty result would be a
     * discovery bug, not a quiet pass */
    EXPECT_GE(count, expected_devices())
        << "ECUIO_EXPECT_J2534_DEVICES says this machine has Pass-Thru drivers";

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

            /* A relative FUNCTIONLIBRARY is skipped: the loader refuses
             * one. CI registers a relative entry to check that. */
            const char* p = devices[i].dll_path;
            const bool drive = ((p[0] >= 'A' && p[0] <= 'Z') ||
                                (p[0] >= 'a' && p[0] <= 'z')) &&
                               p[1] == ':' && (p[2] == '\\' || p[2] == '/');
            const bool unc = p[0] == '\\' && p[1] == '\\';
            EXPECT_TRUE(drive || unc) << "listed relative: " << p;
        }

        ecuio_j2534_free_devices(devices);
    }
#else
    GTEST_SKIP() << "Skipping J2534 discovery test on non-Windows platform";
#endif
}

} // namespace
