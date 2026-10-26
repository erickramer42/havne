// File/layer: core/include/ecuio/j2534.h | C ABI, internal/public discovery API
// Testing: see tests/test_j2534_discovery.cpp

#ifndef ECUIO_J2534_H
#define ECUIO_J2534_H

#include "ecuio/core.h"    /* ecuio_status_t */

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
#define ECUIO_J2534_MAX_VENDORS 16
#define ECUIO_J2534_VENDOR_NAME_LEN 64
#define ECUIO_J2534_DLL_PATH_LEN 256

/* Text is UTF-8 throughout: vendor_name, dll_path, and the path forms
 * of a j2534:// URI. Core passes paths to Windows as UTF-16. */
typedef struct ecuio_j2534_device {
    uint32_t vendor_id;           /* Index in discovery order (0-based) */
    char vendor_name[ECUIO_J2534_VENDOR_NAME_LEN];   /* registry key name */
    char dll_path[ECUIO_J2534_DLL_PATH_LEN];         /* FUNCTIONLIBRARY,
                                                        expanded; a path
                                                        too long for this,
                                                        or relative, isn't
                                                        listed */
    uint32_t supported_protocols; /* 0 — reserved for PROTOCOLS bitmask parse */
    uint32_t reserved[8];         /* ABI-stable expansion room
                                   * [0]: architecture (1=x86, 2=x64, 0=unknown) */
} ecuio_j2534_device_t;

/*
 * Discover J2534 vendor function libraries.
 *
 * CONTRACTS:
 * - Returns ECUIO_OK with count=0 when no vendors are installed (not an error)
 * - On error: *devices_out = NULL, *device_count = 0
 * - Caller must ecuio_j2534_free_devices() the array (even for count=0
 *   it is safe to call with NULL)
 * - Non-Windows: returns ECUIO_ERR_NOT_IMPLEMENTED
 */
ecuio_status_t ecuio_j2534_discover_devices(ecuio_j2534_device_t** devices_out,
                                            uint32_t* device_count);

/* Frees array from discover. NULL-safe no-op. */
void ecuio_j2534_free_devices(ecuio_j2534_device_t* devices);

/* Opaque handle to an opened vendor function-library connection */
typedef struct ecuio_j2534_handle ecuio_j2534_handle_t;

/*
 * Load a discovered vendor's function library and open a device.
 *
 * CONTRACTS:
 * - Loads the DLL named by device->dll_path (an x86 DLL loads in the
 *   surrogate process, ADR-0009), resolves all nine mandatory
 *   PassThru exports, and calls the vendor's PassThruOpen
 * - device->dll_path must be absolute: a relative one would follow
 *   the DLL search order, so it fails before anything opens it
 * - A relative path, a missing or unloadable DLL, or a missing export,
 *   returns ECUIO_ERR_LOAD_FAILED; a refused PassThruOpen
 *   ECUIO_ERR_VENDOR_REFUSED; a surrogate that won't start
 *   ECUIO_ERR_SURROGATE_FAILED. No module is left loaded, and
 *   ecuio_last_error_detail says why (ADR-0018)
 * - On error: *out_handle = NULL
 * - Caller owns the handle; release with ecuio_j2534_close()
 * - Non-Windows: ECUIO_ERR_NOT_IMPLEMENTED
 */
ecuio_status_t ecuio_j2534_open(const ecuio_j2534_device_t* device,
                                ecuio_j2534_handle_t** out_handle);

/*
 * Close device, unload module, free handle. NULL is a safe no-op.
 * Closing INVALIDATES the handle — reuse after close is UB.
 */
ecuio_status_t ecuio_j2534_close(ecuio_j2534_handle_t* handle);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_J2534_H */
