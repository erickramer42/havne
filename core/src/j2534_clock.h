// File/layer: core/src/j2534_clock.h | internal, not ABI-exported;
//             included by j2534_transport.cpp, j2534_worker.cpp and, for
//             the lag seam, core.cpp
// C++17, Qt-free, no platform headers.
// Testing: UNIT: J2534StreamTest.VendorClock* (test_j2534_stream); the
//     worker's use of it is HARNESS (J2534HealthTest.VendorStampsBecomeSessionTime,
//     .AVendorClockThatStaysFrozenIsCaughtUpToTheHost).
//     its host_steps count: J2534HealthTest.VxTiming* (ADR-0042)

#ifndef ECUIO_J2534_CLOCK_H
#define ECUIO_J2534_CLOCK_H

#include <stdint.h>

#include <atomic>

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
 *     a reset), and the host's delta stands in for it;
 *   - a vendor clock that falls more than J2534_CLOCK_LAG_US behind the
 *     host's (frozen, or counting frames and not microseconds) is
 *     caught up to the host, and from then on the host's clock stamps
 *     every frame of the session (trust_lost), since a clock that
 *     stuck once is stuck.
 * The result never decreases. */
constexpr uint64_t J2534_CLOCK_WRAP_US = 1ull << 32;

/* How far a vendor delta may run ahead of the host's before it counts as a
 * step. Queued frames reach the host together, so their delta exceeds the
 * host's by how long the first waited (a read's hold, or the worker's 5 s
 * backoff at most); a minute covers that, and a clock stepped backwards
 * reads about 71 minutes ahead. */
constexpr uint64_t J2534_CLOCK_SLACK_US = 60ull * 1000 * 1000;

/* How far session time may fall behind the host's before the vendor clock
 * is judged stuck. It is the sum over frames, since a frozen clock or a
 * per-frame counter lags one frame's interval at a step. Queued frames lag
 * the host by the worker's 5 s backoff at most, and a vendor crystal drifts
 * well under a second an hour. */
constexpr uint64_t J2534_CLOCK_LAG_US = 10ull * 1000 * 1000;

/* TEST-ONLY (ecuio_set_j2534_clock_lag_us, internal.h): a lag other than
 * J2534_CLOCK_LAG_US, 0 for it. */
inline std::atomic<uint64_t>& j2534_clock_lag_override_us()
{
    static std::atomic<uint64_t> v{0};
    return v;
}

struct j2534_clock {
    bool started = false;
    uint32_t last_raw = 0;      /* the previous frame's vendor stamp */
    uint64_t last_host_us = 0;  /* when it reached the transport */
    uint64_t last_us = 0;       /* the timestamp it was given */
    /* Frames given the host's delta for a step: an interval across one
     * is part host time (ADR-0042) */
    uint32_t host_steps = 0;
    /* The lag guard fired: the vendor clock is no longer read. A new
     * attach starts with a new struct. */
    bool trust_lost = false;
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
    if (c.trust_lost) {
        c.last_raw = raw;
        c.last_host_us = host_us;
        c.last_us += host_delta;
        ++c.host_steps;
        return c.last_us;
    }
    const uint64_t mod = static_cast<uint32_t>(raw - c.last_raw);
    uint64_t wraps = 0;
    if (host_delta > mod) {   /* the nearest whole number of wraps */
        wraps = (host_delta - mod + J2534_CLOCK_WRAP_US / 2) /
                J2534_CLOCK_WRAP_US;
    }
    uint64_t delta = mod + wraps * J2534_CLOCK_WRAP_US;
    bool stepped = false;
    if (delta > host_delta + J2534_CLOCK_SLACK_US) {
        delta = host_delta;   /* the vendor clock stepped */
        stepped = true;
    }
    const uint64_t lag_override = j2534_clock_lag_override_us().load();
    const uint64_t lag = lag_override != 0 ? lag_override : J2534_CLOCK_LAG_US;
    if (host_us > c.last_us + delta + lag) {
        delta = host_us - c.last_us; /* the vendor clock is stuck */
        stepped = true;
        c.trust_lost = true;
    }
    if (stepped) {
        ++c.host_steps;
    }
    c.last_raw = raw;
    c.last_host_us = host_us;
    c.last_us += delta;
    return c.last_us;
}

#endif /* ECUIO_J2534_CLOCK_H */
