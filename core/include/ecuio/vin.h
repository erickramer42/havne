// File/layer: core/include/ecuio/vin.h | C ABI, public interface
// Testing: UNIT: VinTest (test_core): 49 CFR 565.15's sample VIN, each
//          year rule, the check digit's X, the six-character codes, the
//          refusals and the table's lookups

#ifndef ECUIO_VIN_H
#define ECUIO_VIN_H

#include "ecuio/core.h"    /* ecuio_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What a VIN says of the vehicle without asking anyone (ADR-0043):
 * the model year and the check digit by 49 CFR 565.15, and the make by
 * the manufacturer code (WMI) in NHTSA's vPIC list, a table built into
 * core from vPIC's standalone database. Nothing is looked up online:
 * the VIN never leaves the machine (ADR-0027).
 *
 * vPIC lists the codes of vehicles made for sale in the US, so a code
 * used only in other markets reads as unknown. The check digit is
 * required in North America and optional in many other markets, so a
 * VIN whose digit doesn't match may still be the car's own.
 */
#define ECUIO_VIN_LEN            17
#define ECUIO_VIN_WMI_LEN        8     /* six characters at most, NUL included */

/* flags */
#define ECUIO_VIN_CHECK_MATCHES  0x00000001u   /* position 9 is check_digit */
#define ECUIO_VIN_YEAR_EARLIER   0x00000002u   /* model_year is the cycle's
                                                  year 30 before the one
                                                  position 7 points to,
                                                  which was past
                                                  latest_model_year */

/* POD, 32 bytes, no hidden padding; pinned by the assertions below */
typedef struct ecuio_vin_info {
    uint32_t flags;          /* ECUIO_VIN_* */
    uint16_t model_year;     /* 1980-2039; 0 when position 10 isn't a
                                year code (0, U and Z aren't) */
    char check_digit;        /* what position 9 should be: '0'-'9' or 'X' */
    uint8_t wmi_length;      /* 3, or 6 when position 3 is a 9 (a
                                low-volume manufacturer: positions 1-3
                                and 12-14) */
    char wmi[ECUIO_VIN_WMI_LEN];   /* the manufacturer code, NUL-terminated */
    uint32_t reserved[4];    /* zero; ABI growth space */
} ecuio_vin_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_vin_info_t) == 32, "ecuio_vin_info_t size is ABI");
static_assert(offsetof(ecuio_vin_info_t, wmi) == 8, "vin info wmi offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_vin_info_t) == 32, "ecuio_vin_info_t size is ABI");
_Static_assert(offsetof(ecuio_vin_info_t, wmi) == 8, "vin info wmi offset is ABI");
#endif

/*
 * Decodes a VIN.
 *
 * CONTRACTS:
 * - vin is 17 characters of the VIN alphabet, digits and the upper-case
 *   letters but I, O and Q, then a NUL; anything else is
 *   ECUIO_ERR_INVALID_ARGUMENT, with *out zeroed. (ecuio_identity_vin's
 *   VIN is upper case already, and may hold an I, O or Q, which isn't a
 *   VIN by 49 CFR 565.)
 * - The model year is position 10's code by 565.15's Table VII, a cycle
 *   of 30, placed by its note: 1980-2009 when position 7 is a digit,
 *   2010-2039 when it is a letter. The note covers passenger cars, and
 *   multipurpose vehicles and trucks of 10,000 lb GVWR or less, and
 *   vehicles built to it; when the year it gives is past
 *   latest_model_year (the caller's year plus one, say), the year 30
 *   before is taken and ECUIO_VIN_YEAR_EARLIER set. latest_model_year
 *   0 sets no bound.
 * - The check digit is 565.15(c)'s: each character's value (Table III)
 *   times its position's weight (Table IV), summed, modulo 11, 10 read
 *   as X. ECUIO_VIN_CHECK_MATCHES says whether position 9 holds it; a
 *   mismatch is still ECUIO_OK.
 * - NULL vin or out: ECUIO_ERR_NULL_ARGUMENT.
 */
ecuio_status_t ecuio_vin_decode(const char* vin, uint32_t latest_model_year,
                                ecuio_vin_info_t* out);

/*
 * The make vPIC lists under a manufacturer code (an ecuio_vin_info_t's
 * wmi): "Ford", or "Datsun, Infiniti or Nissan" for a code several
 * makes share. "" for a code vPIC doesn't list, a NULL, or anything
 * else. Static, never NULL.
 */
const char* ecuio_vin_make(const char* wmi);

/* The vPIC release the table was built from: "vPICList_lite_2026_10".
 * Static, never NULL. */
const char* ecuio_vin_source(void);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_VIN_H */
