// File/layer: core/src/tier3_ceiling.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: Tier3CeilingTest (test_obd2), and the static_assert below
//
// The operation tier's read ceiling (ADR-0066 sections 1 and 4): the
// requests a read-class operation, a dump, may cause core to send beside
// tier 2's. They read a module's memory and change nothing in it: read
// memory by address, and an upload's three services. Only core's own
// runner makes them, from a plan the manifest declares; a plugin's code
// never does, whatever it has unlocked (tier2_ceiling.h's
// ecuio_tier2_guest_may_send stays the guest's whole list). The session,
// its keep-alive and the unlock stay tier 2's.

#ifndef ECUIO_TIER3_CEILING_H
#define ECUIO_TIER3_CEILING_H

#include "tier2_ceiling.h"

#include <cstdint>

/* Whether a read-class operation may cause core to send service sid:
 *   23  read memory by address
 *   35  request upload
 *   36  transfer data, an upload's alone: no 34 opens a download here
 *   37  request transfer exit
 * None of the four takes its ceiling from a sub-function (a 23's and a
 * 35's second byte is a format, a 36's a counter), so sub is unread. */
constexpr bool ecuio_tier3_read_allowed(uint8_t sid, uint8_t /* sub */)
{
    return sid == 0x23 || sid == 0x35 || sid == 0x36 || sid == 0x37;
}

/* The read ceiling whole: its four services and no other at any
 * sub-function, none of them tier 2's, and none a plugin's own request */
constexpr bool ecuio_tier3_read_ceiling_holds()
{
    constexpr uint8_t probes[] = {0x00, 0x01, 0x02, 0x03,
                                  0x44, 0x7E, 0x80, 0xFF};
    unsigned services = 0;
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        const uint8_t s = static_cast<uint8_t>(sid);
        const bool listed = s == 0x23 || s == 0x35 || s == 0x36 || s == 0x37;
        services += ecuio_tier3_read_allowed(s, 0x00) ? 1u : 0u;
        for (const uint8_t sub : probes) {
            if (ecuio_tier3_read_allowed(s, sub) != listed) {
                return false;
            }
            if (listed && (ecuio_tier2_allowed(s, sub) ||
                           ecuio_tier2_guest_may_send(s, sub))) {
                return false;
            }
        }
    }
    return services == 4;
}

static_assert(
    ecuio_tier3_read_ceiling_holds() &&
        /* the reads */
        ecuio_tier3_read_allowed(0x23, 0x44) &&
        ecuio_tier3_read_allowed(0x35, 0x00) &&
        ecuio_tier3_read_allowed(0x36, 0x01) &&
        ecuio_tier3_read_allowed(0x37, 0x00) &&
        /* never a download, a write, a routine, a reset or a clear */
        !ecuio_tier3_read_allowed(0x34, 0x00) &&
        !ecuio_tier3_read_allowed(0x3D, 0x44) &&
        !ecuio_tier3_read_allowed(0x2E, 0xF1) &&
        !ecuio_tier3_read_allowed(0x2F, 0x00) &&
        !ecuio_tier3_read_allowed(0x31, 0x01) &&
        !ecuio_tier3_read_allowed(0x11, 0x01) &&
        !ecuio_tier3_read_allowed(0x14, 0xFF) &&
        !ecuio_tier3_read_allowed(0x04, 0x00) &&
        !ecuio_tier3_read_allowed(0x28, 0x00) &&
        !ecuio_tier3_read_allowed(0x85, 0x02) &&
        /* the session, the keep-alive and the unlock are tier 2's */
        !ecuio_tier3_read_allowed(0x10, 0x02) &&
        !ecuio_tier3_read_allowed(0x10, 0x03) &&
        !ecuio_tier3_read_allowed(0x3E, 0x00) &&
        !ecuio_tier3_read_allowed(0x27, 0x01) &&
        !ecuio_tier3_read_allowed(0x22, 0xF1) &&
        /* a plugin's own request reads no memory */
        !ecuio_tier2_guest_may_send(0x23, 0x44) &&
        !ecuio_tier2_guest_may_send(0x35, 0x00) &&
        !ecuio_tier2_guest_may_send(0x36, 0x01) &&
        !ecuio_tier2_guest_may_send(0x37, 0x00),
    "tier 3's read ceiling: 23, 35, 36 and 37, core's alone, and nothing "
    "that downloads, writes, resets or clears");

#endif /* ECUIO_TIER3_CEILING_H */
