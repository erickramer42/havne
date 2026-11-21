// File/layer: tests/test_j2534_clock.cpp | test-only, C++17, Qt-free
// Testing: UNIT: J2534StreamTest.VendorClock* (test_j2534_stream), the
//     vendor clock trusted again once it agrees with the host's
//
// j2534_clock.h's trust won back: a sound clock after one reply read long
// after its stamp, and a stuck one that never agrees.

#include "j2534_clock.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr uint64_t kMs = 1000;

/* The host reads in bursts: a frame reaches it at the next 20 ms */
uint64_t read_at(uint64_t t)
{
    return (t + 20 * kMs - 1) / (20 * kMs) * (20 * kMs);
}

} // namespace

/* A sound clock, one frame read 30 s after its stamp (it waited across a
 * host sleep, the vendor's clock running on), then frames every 10 ms
 * that the host reads in bursts: the host's clock stamps them until the
 * two agree over a span, then the vendor's 10 ms deltas are back */
TEST(J2534StreamTest, VendorClockIsTrustedAgainOnceItAgreesWithTheHost)
{
    j2534_clock c;
    const uint32_t base = 3000000000u;
    uint64_t t = 0;
    uint64_t at = j2534_clock_step(c, base, read_at(t));
    for (int i = 0; i < 300; ++i) {
        t += 10 * kMs;
        const uint64_t next =
            j2534_clock_step(c, base + static_cast<uint32_t>(t), read_at(t));
        EXPECT_EQ(next - at, 10 * kMs) << "the vendor's delta, " << i;
        at = next;
    }
    ASSERT_FALSE(c.trust_lost);
    ASSERT_EQ(c.host_steps, 0u);

    t += 10 * kMs;
    (void)j2534_clock_step(c, base + static_cast<uint32_t>(t),
                           read_at(t) + 30000 * kMs);
    ASSERT_TRUE(c.trust_lost) << "read 30 s after its stamp";
    const uint64_t lost_at = read_at(t) + 30000 * kMs;
    t += 30000 * kMs;

    uint64_t back_at = 0;
    at = 0;
    for (int i = 0; i < 1000; ++i) {
        t += 10 * kMs;
        const uint64_t host = read_at(t);
        const uint64_t next =
            j2534_clock_step(c, base + static_cast<uint32_t>(t), host);
        EXPECT_GE(next, at) << "never decreases, " << i;
        if (back_at != 0 && at != 0) {
            EXPECT_EQ(next - at, 10 * kMs) << "the vendor's delta, " << i;
        }
        if (back_at == 0 && !c.trust_lost) {
            back_at = host;
        }
        at = next;
    }
    ASSERT_NE(back_at, 0u) << "trusted again";
    EXPECT_GE(back_at - lost_at, J2534_CLOCK_AGREE_SPAN_US)
        << "never before a span";
    EXPECT_LE(back_at - lost_at, 3 * J2534_CLOCK_AGREE_SPAN_US)
        << "the anchor moves once past the late frame";
}

/* A clock that stands still, or counts one a frame, never agrees with
 * the host's: frames 1 ms apart (a span's frames inside the tolerance)
 * and 10 ms apart, for a minute each */
TEST(J2534StreamTest, VendorClockThatIsStuckIsNeverTrustedAgain)
{
    for (const uint64_t gap : {1 * kMs, 10 * kMs}) {
        for (const bool counts : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << "gap " << gap
                         << (counts ? " counting" : " frozen"));
            j2534_clock c;
            uint32_t raw = 7000;
            (void)j2534_clock_step(c, raw, 0);
            bool lost = false;
            for (uint64_t t = gap; t <= 60000 * kMs; t += gap) {
                raw += counts ? 1u : 0u;
                (void)j2534_clock_step(c, raw, t);
                lost = lost || c.trust_lost;
                ASSERT_TRUE(!lost || c.trust_lost) << "trusted again at " << t;
            }
            EXPECT_TRUE(lost);
        }
    }
}

/* Two clocks that agree over a span still wait for a span's frames: a
 * frame a second wins nothing back in a few seconds */
TEST(J2534StreamTest, VendorClockWinsTrustBackOnlyOverASpansFrames)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 1000u, 0);
    (void)j2534_clock_step(c, 1000u, 20000 * kMs); /* frozen: lost */
    ASSERT_TRUE(c.trust_lost);
    uint32_t raw = 1000u;
    uint64_t t = 20000 * kMs;
    for (uint32_t i = 1; i < J2534_CLOCK_AGREE_FRAMES; ++i) {
        t += 1000 * kMs;
        raw += 1000 * 1000;
        (void)j2534_clock_step(c, raw, t);
    }
    EXPECT_TRUE(c.trust_lost) << "fewer frames than a span takes";
    for (int i = 0; i < 4; ++i) {
        t += 1000 * kMs;
        raw += 1000 * 1000;
        (void)j2534_clock_step(c, raw, t);
    }
    EXPECT_FALSE(c.trust_lost) << "a span's frames, the clocks agreeing";
}
