// File/layer: core/src/j2534_clock.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its only consumer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: J2534StreamTest.VendorClock* (test_j2534_stream); the
//     worker's use of it is HARNESS (J2534HealthTest.VendorStampsBecomeSessionTime).

#ifndef ECUIO_J2534_CLOCK_H
#define ECUIO_J2534_CLOCK_H

#include <stdint.h>

/* A frame's timestamp_us is microseconds since the session's transport
 * attached (core.h). A J2534 vendor stamps each message from a 32-bit
 * microsecond clock of its own (J2534-1 v04.04), which wraps every
 * 71.6 min and starts wherever it likes: the VX's keeps running across
 * connects (docs/bench-sessions/2026-10-22.md). j2534_clock_step maps
 * one onto the other:
 *   - the first frame takes the host's time, as it reaches the
 *     transport;
 *   - each later frame adds the vendor's delta since the one before, so
 *     intervals keep the vendor's resolution, frames that arrive in one
 *     batch included;
 *   - the host's elapsed time decides how often the vendor clock wrapped
 *     in between, so a gap longer than 71.6 min still counts in full;
 *   - a vendor delta that runs ahead of the host's by more than
 *     J2534_CLOCK_SLACK_US means the vendor clock stepped (backwards, or
 *     a reset), and the host's delta stands in for it.
 * The result never decreases. */
constexpr uint64_t J2534_CLOCK_WRAP_US = 1ull << 32;

/* How far a vendor delta may run ahead of the host's before it counts
 * as a step. Frames that waited in the adapter's queue reach the host
 * together, so the vendor's delta between them exceeds the host's by
 * up to how long the first one waited: a read's hold, or the worker's
 * backoff sleep (5 s at most). A minute covers both with room; a clock
 * that stepped backwards reads as about 71 minutes ahead. */
constexpr uint64_t J2534_CLOCK_SLACK_US = 60ull * 1000 * 1000;

struct j2534_clock {
    bool started = false;
    uint32_t last_raw = 0;      /* the previous frame's vendor stamp */
    uint64_t last_host_us = 0;  /* when it reached the transport */
    uint64_t last_us = 0;       /* the timestamp it was given */
};

/* The timestamp for a frame stamped raw by the vendor, reaching the
 * transport host_us after it attached. */
inline uint64_t j2534_clock_step(j2534_clock& c, uint32_t raw, uint64_t host_us)
{
    if (!c.started) {
        c.started = true;
        c.last_raw = raw;
        c.last_host_us = host_us;
        c.last_us = host_us;
        return host_us;
    }
    const uint64_t host_delta =
        (host_us > c.last_host_us) ? host_us - c.last_host_us : 0;
    const uint64_t mod = static_cast<uint32_t>(raw - c.last_raw);
    uint64_t wraps = 0;
    if (host_delta > mod) {   /* the nearest whole number of wraps */
        wraps = (host_delta - mod + J2534_CLOCK_WRAP_US / 2) /
                J2534_CLOCK_WRAP_US;
    }
    uint64_t delta = mod + wraps * J2534_CLOCK_WRAP_US;
    if (delta > host_delta + J2534_CLOCK_SLACK_US) {
        delta = host_delta;   /* the vendor clock stepped */
    }
    c.last_raw = raw;
    c.last_host_us = host_us;
    c.last_us += delta;
    return c.last_us;
}

#endif /* ECUIO_J2534_CLOCK_H */
