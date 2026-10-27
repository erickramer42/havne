// File/layer: app/latencystats.h | C++17, app layer, no Qt
// Testing: test_bridge — BridgeTest.LatencyStatsReadsQuantilesWithinABucket
//          (bucketing and quantiles), .DeliveryText* and
//          .DisconnectLogsTheDelivery (through the bridge)

#pragma once

#include <cstdint>

// A latency distribution in fixed memory, for the bridge's delivery
// figures. Each sample in microseconds falls in a bucket an eighth of
// its power of two wide (exact below 16 µs), so a quantile reads back
// at most 12.5 % above the true value and never below it; recording one
// is a few instructions and allocates nothing.
class LatencyStats
{
public:
    void add(int64_t us)
    {
        const uint64_t v = us > 0 ? static_cast<uint64_t>(us) : 0;
        ++m_buckets[bucketOf(v)];
        ++m_count;
        if (v > m_max) m_max = v;
    }

    void clear() { *this = LatencyStats(); }

    uint64_t count() const { return m_count; }
    uint64_t maxUs() const { return m_max; }

    // The q quantile (0 to 1) in µs: the top of the bucket holding it,
    // capped at the largest sample, so 1.0 gives the maximum exactly. 0
    // with no samples.
    uint64_t quantileUs(double q) const
    {
        if (m_count == 0) return 0;
        if (q < 0.0) q = 0.0;
        if (q > 1.0) q = 1.0;
        uint64_t rank = static_cast<uint64_t>(q * static_cast<double>(m_count) + 0.999999);
        if (rank == 0) rank = 1;
        uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += m_buckets[i];
            if (seen >= rank) {
                if (i == kBuckets - 1) return m_max;   // the open-ended last bucket
                const uint64_t top = topOf(i);
                return top < m_max ? top : m_max;
            }
        }
        return m_max;
    }

private:
    static constexpr int kSub = 8;      // buckets per power of two
    static constexpr int kBands = 40;   // up to 2^42 µs, about 50 days
    static constexpr int kBuckets = (kBands + 1) * kSub;

    // Below 16 µs a bucket per µs; from 2^b up (b >= 4), eight buckets
    // across [2^b, 2^(b+1))
    static int bucketOf(uint64_t v)
    {
        if (v < 2 * kSub) return static_cast<int>(v);
        int b = 0;
        while ((v >> (b + 1)) != 0) ++b;   // b = floor(log2 v), >= 4
        const int sub = static_cast<int>((v >> (b - 3)) & (kSub - 1));
        const int i = (b - 2) * kSub + sub;
        return i < kBuckets ? i : kBuckets - 1;
    }

    // The largest value a bucket holds
    static uint64_t topOf(int i)
    {
        if (i < 2 * kSub) return static_cast<uint64_t>(i);
        const int b = i / kSub + 2;
        const uint64_t sub = static_cast<uint64_t>(i % kSub);
        return ((kSub + sub + 1) << (b - 3)) - 1;
    }

    uint64_t m_buckets[kBuckets] = {};
    uint64_t m_count = 0;
    uint64_t m_max = 0;
};
