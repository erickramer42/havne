// File/layer: core/include/qttune/j2534.h | C ABI, internal/public discovery API
// Testing: see tests/test_j2534_discovery.cpp

#ifndef QTTUNE_J2534_H
#define QTTUNE_J2534_H

#include "qttune/core.h"   /* qttune_status_t */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * J2534 vendor discovery.
 *
 * Enumerates registered Pass-Thru function libraries per SAE J2534-1 (v4.04):
 *   HKLM\SOFTWARE\Pass-Thru Support.04.04\<VendorName>  → FUNCTIONLIBRARY
 * (both WOW64 registry views, plus the unhyphenated root some
 * installers write; see implementation).
 */
#define QTTUNE_J2534_MAX_VENDORS 16
#define QTTUNE_J2534_VENDOR_NAME_LEN 64
#define QTTUNE_J2534_DLL_PATH_LEN 256

typedef struct qttune_j2534_device {
    uint32_t vendor_id;           /* Index in discovery order (0-based) */
    char vendor_name[QTTUNE_J2534_VENDOR_NAME_LEN];
    char dll_path[QTTUNE_J2534_DLL_PATH_LEN];
    uint32_t supported_protocols; /* 0 — reserved for PROTOCOLS bitmask parse */
    uint32_t reserved[8];         /* ABI-stable expansion room
                                   * [0]: architecture (1=x86, 2=x64, 0=unknown) */
} qttune_j2534_device_t;

/*
 * Discover J2534 vendor function libraries.
 *
 * CONTRACTS:
 * - Returns QT_OK with count=0 when no vendors are installed (not an error)
 * - On error: *devices_out = NULL, *device_count = 0
 * - Caller must qttune_j2534_free_devices() the array (even for count=0
 *   it is safe to call with NULL)
 * - Non-Windows: returns QT_ERR_NOT_IMPLEMENTED
 */
qttune_status_t qttune_j2534_discover_devices(qttune_j2534_device_t** devices_out,
                                              uint32_t* device_count);

/* Frees array from discover. NULL-safe no-op. */
void qttune_j2534_free_devices(qttune_j2534_device_t* devices);

/* Opaque handle to an opened vendor function-library connection */
typedef struct qttune_j2534_handle qttune_j2534_handle_t;

/*
 * Load a discovered vendor's function library and open a device.
 *
 * CONTRACTS:
 * - Loads the DLL named by device->dll_path (an x86 DLL loads in the
 *   surrogate process, ADR-0009), resolves all nine mandatory
 *   PassThru exports, and calls the vendor's PassThruOpen
 * - Any failure (missing DLL, missing export, PassThruOpen refused)
 *   returns QT_ERR_LOAD_FAILED with NO module left loaded
 * - On error: *out_handle = NULL
 * - Caller owns the handle; release with qttune_j2534_close()
 * - Non-Windows: QT_ERR_NOT_IMPLEMENTED
 */
qttune_status_t qttune_j2534_open(const qttune_j2534_device_t* device,
                                  qttune_j2534_handle_t** out_handle);

/*
 * Close device, unload module, free handle. NULL is a safe no-op.
 * Closing INVALIDATES the handle — reuse after close is UB.
 */
qttune_status_t qttune_j2534_close(qttune_j2534_handle_t* handle);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_J2534_H */
