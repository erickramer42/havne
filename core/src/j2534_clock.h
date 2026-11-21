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
 *     caught up to the host, and the host's clock stamps the frames
 *     after it (trust_lost) until the vendor's agrees with the host's
 *     again: over at least J2534_CLOCK_AGREE_FRAMES frames and
 *     J2534_CLOCK_AGREE_SPAN_US of host time, the two clocks' elapsed
 *     times within J2534_CLOCK_AGREE_US. One reply read long after its
 *     stamp (a frame that waited across a host sleep) costs the
 *     vendor's resolution for that span alone.
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

/* Trust comes back once the vendor's clock and the host's agree over a
 * span. A frozen or frame-counting clock falls a whole span behind, 8x
 * the tolerance, so it never agrees; a sound one agrees within a read's
 * hold (10 ms asking, 100 ms idle: J2534_READ_SLICE_MS,
 * J2534_IDLE_READ_TIMEOUT_MS) and Windows' 15.6 ms sleep step. The
 * frames make the span more than one burst of a read. */
constexpr uint64_t J2534_CLOCK_AGREE_SPAN_US = 2ull * 1000 * 1000;
constexpr uint64_t J2534_CLOCK_AGREE_US = 250ull * 1000;
constexpr uint32_t J2534_CLOCK_AGREE_FRAMES = 64;

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
    /* The lag guard fired: the host's clock stamps the frames until the
     * vendor's agrees again from the anchor on. A new attach starts with
     * a new struct. */
    bool trust_lost = false;
    uint32_t anchor_raw = 0;
    uint64_t anchor_host_us = 0;
    uint32_t anchor_frames = 0; /* frames since the anchor */
};

/* The vendor's elapsed time from raw stamp from to to, the host's elapsed
 * time host_delta choosing how often the 32-bit clock wrapped between */
inline uint64_t j2534_vendor_elapsed(uint32_t from, uint32_t to,
                                     uint64_t host_delta)
{
    const uint64_t mod = static_cast<uint32_t>(to - from);
    uint64_t wraps = 0;
    if (host_delta > mod) { /* the nearest whole number of wraps */
        wraps =
            (host_delta - mod + J2534_CLOCK_WRAP_US / 2) / J2534_CLOCK_WRAP_US;
    }
    return mod + wraps * J2534_CLOCK_WRAP_US;
}

/* A frame while trust is lost: back once the span from the anchor is
 * long enough and the clocks agree over it, else the anchor moves here */
inline void j2534_clock_judge_again(j2534_clock& c, uint32_t raw,
                                    uint64_t host_us)
{
    ++c.anchor_frames;
    const uint64_t host_span =
        host_us > c.anchor_host_us ? host_us - c.anchor_host_us : 0;
    if (c.anchor_frames < J2534_CLOCK_AGREE_FRAMES ||
        host_span < J2534_CLOCK_AGREE_SPAN_US) {
        return;
    }
    const uint64_t vendor_span =
        j2534_vendor_elapsed(c.anchor_raw, raw, host_span);
    const uint64_t apart = vendor_span > host_span ? vendor_span - host_span
                                                   : host_span - vendor_span;
    if (apart <= J2534_CLOCK_AGREE_US) {
        c.trust_lost = false;
        return;
    }
    c.anchor_raw = raw;
    c.anchor_host_us = host_us;
    c.anchor_frames = 0;
}

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
        j2534_clock_judge_again(c, raw, host_us);
        return c.last_us;
    }
    uint64_t delta = j2534_vendor_elapsed(c.last_raw, raw, host_delta);
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
        c.anchor_raw = raw;
        c.anchor_host_us = host_us;
        c.anchor_frames = 0;
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
