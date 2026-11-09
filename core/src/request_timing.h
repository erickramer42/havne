// File/layer: core/src/request_timing.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: RequestTimingTest (test_obd2: the buckets, quantiles and
//          sample floors, intervals across a clock's fallback);
//          RequestEngineTest.Timing* (the engine's hooks)
//
// Reply timing per request (ADR-0042): for each request in the rotation,
// its outcomes, its reply latency on the host's clock and its refresh
// interval on the frames' own stamps, as histograms in fixed memory. The
// engine (request_engine.h) records into it from the worker, with no
// lock and no allocation; the worker turns it into the public rows
// (ecuio_request_timing_t) at most once a second.

#ifndef ECUIO_REQUEST_TIMING_H
#define ECUIO_REQUEST_TIMING_H

#include "ecuio/profile.h"

#include <cstdint>

/* A distribution of microseconds, log-linear like the bridge's delivery
 * figures (app/latencystats.h): exact below 16 us, then eight buckets to
 * a power of two, so a quantile reads at most 12.5 % above the true
 * value and never below it. Up to 2^24 us (16.8 s); longer samples go in
 * the last bucket, whose quantile is the exact maximum. */
struct timing_hist {
    static constexpr int kSub = 8;
    static constexpr int kTopBand = 23; /* 2^23 to 2^24 us */
    static constexpr int kBuckets = (kTopBand - 1) * kSub;

    uint32_t buckets[kBuckets] = {};
    uint32_t count = 0;
    uint32_t max_us = 0;

    static int bucket_of(uint32_t v)
    {
        if (v < 2 * kSub) {
            return static_cast<int>(v);
        }
        int b = 0;
        while (b < 31 && (v >> (b + 1)) != 0) {
            ++b; /* floor(log2 v), at least 4; 31 at most (a shift by the
                    width is undefined, and on x86 shifts by nothing) */
        }
        const int i =
            (b - 2) * kSub + static_cast<int>((v >> (b - 3)) & (kSub - 1));
        return i < kBuckets ? i : kBuckets - 1;
    }

    /* The largest value a bucket holds */
    static uint32_t top_of(int i)
    {
        if (i < 2 * kSub) {
            return static_cast<uint32_t>(i);
        }
        const int b = i / kSub + 2;
        const uint32_t sub = static_cast<uint32_t>(i % kSub);
        return ((kSub + sub + 1) << (b - 3)) - 1;
    }

    void add(int64_t us)
    {
        const uint32_t v = us <= 0 ? 0u
                           : us >= INT64_C(0xFFFFFFFF)
                               ? 0xFFFFFFFFu
                               : static_cast<uint32_t>(us);
        ++buckets[bucket_of(v)];
        ++count;
        if (v > max_us) {
            max_us = v;
        }
    }

    /* The q quantile (0 to 1): the top of the bucket holding it, capped
     * at the largest sample; 0 with no samples */
    uint32_t quantile(double q) const
    {
        if (count == 0) {
            return 0;
        }
        uint64_t rank =
            static_cast<uint64_t>(q * static_cast<double>(count) + 0.999999);
        if (rank == 0) {
            rank = 1;
        }
        uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += buckets[i];
            if (seen >= rank) {
                if (i == kBuckets - 1) {
                    return max_us;
                }
                const uint32_t top = top_of(i);
                return top < max_us ? top : max_us;
            }
        }
        return max_us;
    }
};

/* One request's figures */
struct timing_row {
    uint32_t answered = 0;
    uint32_t refused = 0;
    uint32_t timeouts = 0;
    uint32_t packed = 0;
    timing_hist latency;
    timing_hist interval;
    uint32_t intervals_mixed = 0;
    bool has_last = false;
    int64_t last_stamp_us = 0;
    uint32_t last_breaks = 0; /* the clock's fallbacks at the last answer */
};

/* Every request's, indexed as the engine's table */
struct ecuio_timing_table {
    timing_row rows[ECUIO_PROFILE_MAX_REQUESTS];
    /* The clock the frames' stamps come from (ECUIO_TIMING_CLOCK_*) */
    uint32_t interval_clock = ECUIO_TIMING_CLOCK_HOST;
};

/* An answer (positive) at stamp_us on the frames' clock, latency_us after
 * its request went out; breaks is how many times the clock has taken
 * the host's delta for a frame so far. An interval with a fallback
 * between its two answers is only counted. */
inline void timing_answered(timing_row& r, int64_t latency_us, int64_t stamp_us,
                            uint32_t breaks, bool packed)
{
    ++r.answered;
    if (packed) {
        ++r.packed;
    }
    r.latency.add(latency_us);
    if (r.has_last) {
        if (breaks == r.last_breaks && stamp_us >= r.last_stamp_us) {
            r.interval.add(stamp_us - r.last_stamp_us);
        } else {
            ++r.intervals_mixed;
        }
    }
    r.has_last = true;
    r.last_stamp_us = stamp_us;
    r.last_breaks = breaks;
}

inline void timing_refused(timing_row& r, int64_t latency_us, bool packed)
{
    ++r.refused;
    if (packed) {
        ++r.packed;
    }
    r.latency.add(latency_us);
}

/* A row as the report gives it; a percentile with too few samples is 0
 * and its flag clear */
inline ecuio_request_timing_t timing_public(const timing_row& r)
{
    ecuio_request_timing_t out{};
    out.answered = r.answered;
    out.refused = r.refused;
    out.timeouts = r.timeouts;
    out.packed = r.packed;
    const uint32_t n = r.latency.count;
    out.latency_p50_us = r.latency.quantile(0.5);
    if (n >= ECUIO_TIMING_P99_SAMPLES) {
        out.flags |= ECUIO_TIMING_LATENCY_P99;
        out.latency_p99_us = r.latency.quantile(0.99);
    }
    if (n >= ECUIO_TIMING_P999_SAMPLES) {
        out.flags |= ECUIO_TIMING_LATENCY_P999;
        out.latency_p999_us = r.latency.quantile(0.999);
    }
    out.latency_max_us = r.latency.max_us;
    const uint32_t m = r.interval.count;
    out.intervals = m;
    out.intervals_mixed = r.intervals_mixed;
    out.interval_p50_us = r.interval.quantile(0.5);
    if (m >= ECUIO_TIMING_P99_SAMPLES) {
        out.flags |= ECUIO_TIMING_INTERVAL_P99;
        out.interval_p99_us = r.interval.quantile(0.99);
    }
    if (m >= ECUIO_TIMING_P999_SAMPLES) {
        out.flags |= ECUIO_TIMING_INTERVAL_P999;
        out.interval_p999_us = r.interval.quantile(0.999);
    }
    out.interval_max_us = r.interval.max_us;
    return out;
}

#endif /* ECUIO_REQUEST_TIMING_H */
