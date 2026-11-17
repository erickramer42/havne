// File/layer: core/src/tier2_ceiling.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: Tier2CeilingTest (test_obd2), and the static_assert below
//
// The conversation tier's service ceiling (ADR-0066 section 1): the
// requests a tier-2 plugin's code may cause core to send, whatever its
// manifest declares and whatever it has unlocked. It opens doors (the
// extended session, security access) and reads; it never writes,
// transfers, resets or clears. Where a service's sub-functions split
// read from write, the ceiling is the sub-function: 10 03 and never
// 10 02; 2C 01 and 2C 03 and never 2C 02, which with 22 would read
// memory by address. The request engine's own lists (request_engine.h)
// stand beside this one and are not widened by it.

#ifndef ECUIO_TIER2_CEILING_H
#define ECUIO_TIER2_CEILING_H

#include <cstdint>

/* Whether a tier-2 service takes its ceiling from its sub-function, the
 * request's second byte: a manifest declares these with their
 * sub-functions and every other service without */
constexpr bool ecuio_tier2_splits(uint8_t sid)
{
    return sid == 0x10 || sid == 0x27 || sid == 0x2C || sid == 0x3E;
}

/* Whether tier 2 may cause core to send service sid with sub-function
 * sub (unread for a service that doesn't split):
 *   10  03, the extended session
 *   3E  00, tester present with its reply
 *   27  01 to 7E but 5F and 60 (ISO 26021's end-of-life activation of
 *       pyrotechnic devices); never with bit 7, which suppresses the
 *       positive reply core counts an attempt by
 *   22, 19, 2A  reads; nothing enrols 2A yet
 *   2C  01 (define by identifier) and 03 (clear) */
constexpr bool ecuio_tier2_allowed(uint8_t sid, uint8_t sub)
{
    switch (sid) {
    case 0x10:
        return sub == 0x03;
    case 0x3E:
        return sub == 0x00;
    case 0x27:
        return sub >= 0x01 && sub <= 0x7E && sub != 0x5F && sub != 0x60;
    case 0x2C:
        return sub == 0x01 || sub == 0x03;
    case 0x19:
    case 0x22:
    case 0x2A:
        return true;
    default:
        return false;
    }
}

/* Whether a plugin's own request (its next_request) may carry sid and
 * sub: the reads alone. Never 27, which core builds from the caller's
 * word (ADR-0066 section 2.3); never 10 or 3E, the session and its
 * keep-alive being core's (2.5); not 2A while nothing takes a periodic
 * reply. */
constexpr bool ecuio_tier2_guest_may_send(uint8_t sid, uint8_t sub)
{
    return ecuio_tier2_allowed(sid, sub) &&
           (sid == 0x19 || sid == 0x22 || sid == 0x2C);
}

/* How many of service sid's 256 sub-functions the ceiling admits */
constexpr unsigned ecuio_tier2_count(uint8_t sid)
{
    unsigned n = 0;
    for (unsigned sub = 0; sub <= 0xFF; ++sub) {
        n += ecuio_tier2_allowed(sid, static_cast<uint8_t>(sub)) ? 1u : 0u;
    }
    return n;
}

/* The ceiling whole: no service outside its seven at the sub-functions
 * that open the ones inside, each split service's count, and a guest's
 * requests inside both */
constexpr bool ecuio_tier2_ceiling_holds()
{
    constexpr uint8_t probes[] = {0x00, 0x01, 0x02, 0x03, 0x7E, 0x80, 0xFF};
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        const uint8_t s = static_cast<uint8_t>(sid);
        const bool listed = s == 0x10 || s == 0x19 || s == 0x22 || s == 0x27 ||
                            s == 0x2A || s == 0x2C || s == 0x3E;
        const bool guest = s == 0x19 || s == 0x22 || s == 0x2C;
        for (const uint8_t sub : probes) {
            if (!listed && ecuio_tier2_allowed(s, sub)) {
                return false;
            }
            if (ecuio_tier2_guest_may_send(s, sub) &&
                !(guest && ecuio_tier2_allowed(s, sub))) {
                return false;
            }
        }
    }
    return ecuio_tier2_count(0x10) == 1 && ecuio_tier2_count(0x3E) == 1 &&
           ecuio_tier2_count(0x2C) == 2 && ecuio_tier2_count(0x27) == 124 &&
           ecuio_tier2_count(0x19) == 256 && ecuio_tier2_count(0x22) == 256 &&
           ecuio_tier2_count(0x2A) == 256;
}

static_assert(
    ecuio_tier2_ceiling_holds() &&
        /* the doors */
        ecuio_tier2_allowed(0x10, 0x03) && ecuio_tier2_allowed(0x3E, 0x00) &&
        ecuio_tier2_allowed(0x27, 0x01) && ecuio_tier2_allowed(0x27, 0x02) &&
        ecuio_tier2_allowed(0x27, 0x7D) && ecuio_tier2_allowed(0x27, 0x7E) &&
        /* the reads */
        ecuio_tier2_allowed(0x22, 0xF1) && ecuio_tier2_allowed(0x19, 0x02) &&
        ecuio_tier2_allowed(0x2A, 0x01) && ecuio_tier2_allowed(0x2C, 0x01) &&
        ecuio_tier2_allowed(0x2C, 0x03) &&
        /* the sub-function split */
        !ecuio_tier2_allowed(0x10, 0x02) && !ecuio_tier2_allowed(0x10, 0x01) &&
        !ecuio_tier2_allowed(0x10, 0x83) && !ecuio_tier2_allowed(0x2C, 0x02) &&
        !ecuio_tier2_allowed(0x3E, 0x80) && !ecuio_tier2_allowed(0x27, 0x00) &&
        !ecuio_tier2_allowed(0x27, 0x5F) && !ecuio_tier2_allowed(0x27, 0x60) &&
        !ecuio_tier2_allowed(0x27, 0x7F) && !ecuio_tier2_allowed(0x27, 0x81) &&
        /* never a write, a transfer, a routine, a reset or a clear */
        !ecuio_tier2_allowed(0x23, 0x00) && !ecuio_tier2_allowed(0x2E, 0xF1) &&
        !ecuio_tier2_allowed(0x2F, 0x00) && !ecuio_tier2_allowed(0x31, 0x01) &&
        !ecuio_tier2_allowed(0x34, 0x00) && !ecuio_tier2_allowed(0x35, 0x00) &&
        !ecuio_tier2_allowed(0x36, 0x01) && !ecuio_tier2_allowed(0x37, 0x00) &&
        !ecuio_tier2_allowed(0x3D, 0x00) && !ecuio_tier2_allowed(0x14, 0xFF) &&
        !ecuio_tier2_allowed(0x04, 0x00) && !ecuio_tier2_allowed(0x11, 0x01) &&
        !ecuio_tier2_allowed(0x28, 0x00) && !ecuio_tier2_allowed(0x85, 0x02) &&
        /* a plugin's own requests: reads, and no door */
        ecuio_tier2_guest_may_send(0x22, 0xD2) &&
        ecuio_tier2_guest_may_send(0x19, 0x02) &&
        ecuio_tier2_guest_may_send(0x2C, 0x01) &&
        ecuio_tier2_guest_may_send(0x2C, 0x03) &&
        !ecuio_tier2_guest_may_send(0x2C, 0x02) &&
        !ecuio_tier2_guest_may_send(0x27, 0x01) &&
        !ecuio_tier2_guest_may_send(0x27, 0x02) &&
        !ecuio_tier2_guest_may_send(0x10, 0x03) &&
        !ecuio_tier2_guest_may_send(0x10, 0x01) &&
        !ecuio_tier2_guest_may_send(0x3E, 0x00) &&
        !ecuio_tier2_guest_may_send(0x2A, 0x01),
    "tier 2: 10 03, 3E 00, 27, 22, 19, 2A, 2C 01 and 2C 03, and nothing "
    "that writes, transfers, resets or clears");

#endif /* ECUIO_TIER2_CEILING_H */
