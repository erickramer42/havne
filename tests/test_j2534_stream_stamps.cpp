// File/layer: tests/test_j2534_stream_stamps.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534StreamTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

/* The vendor clock (j2534_clock.h): the first frame takes the host's
 * time; later ones the vendor's delta, even when the host saw them
 * arrive together or further apart. */
/* Both read paths copy a vendor's message through ecuio_pt_rx_from_msg:
 * RxStatus and the stamp as they came, and Data up to its DataSize,
 * never past the end of PASSTHRU_MSG.Data however large a size a
 * vendor reports. */
TEST(J2534StreamTest, ReceivedMessageCopyStopsAtTheEndOfData)
{
    static PASSTHRU_MSG m = {};
    static ecuio_pt_rx_t rx = {};
    for (unsigned long i = 0; i < ECUIO_PT_MSG_DATA_SIZE; ++i) {
        m.Data[i] = static_cast<unsigned char>(i * 7);
    }
    m.RxStatus = ECUIO_PT_CAN_29BIT_ID;
    m.Timestamp = 0xFFFFFFF0ul;
    m.DataSize = 7;
    ecuio_pt_rx_from_msg(&m, &rx);
    EXPECT_EQ(rx.rx_status, ECUIO_PT_CAN_29BIT_ID);
    EXPECT_EQ(rx.timestamp, 0xFFFFFFF0u);
    EXPECT_EQ(rx.data_size, 7u);
    EXPECT_EQ(memcmp(rx.data, m.Data, 7), 0);

    m.DataSize = ECUIO_PT_MSG_DATA_SIZE + 1000;
    ecuio_pt_rx_from_msg(&m, &rx);
    EXPECT_EQ(rx.data_size, static_cast<uint32_t>(ECUIO_PT_MSG_DATA_SIZE));
    EXPECT_EQ(memcmp(rx.data, m.Data, ECUIO_PT_MSG_DATA_SIZE), 0);
}

/* A refusal's detail names the vendor's status (ecuio_detail_set_vendor):
 * every status J2534-1 v04.04 defines, 0x00 to 0x1A, has its own name,
 * and a vendor's own (the VX's 34 with no adapter) has none, so the
 * detail calls it vendor-specific. */
TEST(J2534StreamTest, StatusNamesCoverTheSpecRange)
{
    const long kLast = 0x1A;
    for (long s = 0; s <= kLast; ++s) {
        const char* name = ecuio_pt_status_name(s);
        ASSERT_NE(name, nullptr) << "status " << s;
        for (long t = 0; t < s; ++t) {
            EXPECT_STRNE(name, ecuio_pt_status_name(t))
                << "statuses " << t << " and " << s;
        }
    }
    EXPECT_STREQ(ecuio_pt_status_name(0x00), "STATUS_NOERROR");
    EXPECT_STREQ(ecuio_pt_status_name(0x08), "ERR_DEVICE_NOT_CONNECTED");
    EXPECT_STREQ(ecuio_pt_status_name(0x1A), "ERR_INVALID_DEVICE_ID");
    EXPECT_EQ(ecuio_pt_status_name(kLast + 1), nullptr);
    EXPECT_EQ(ecuio_pt_status_name(34), nullptr);
}

TEST(J2534StreamTest, VendorClockAnchorsAtTheHostAndKeepsVendorDeltas)
{
    j2534_clock c;
    EXPECT_EQ(j2534_clock_step(c, 3000000000u, 1500), 1500u);
    EXPECT_EQ(j2534_clock_step(c, 3000004000u, 1500), 5500u) << "one batch";
    EXPECT_EQ(j2534_clock_step(c, 3000010000u, 20000), 11500u)
        << "the vendor's 6 ms, not the host's 18.5";
}

/* Across the 32-bit wrap, and across a gap of several wraps, which only
 * the host's clock can count. */
TEST(J2534StreamTest, VendorClockCarriesAcrossTheWrap)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 0xFFFFFFF0u, 1000);
    EXPECT_EQ(j2534_clock_step(c, 0x10u, 1100), uint64_t{1000 + 0x20});
    const uint64_t gap = 3 * J2534_CLOCK_WRAP_US + 5000000;
    EXPECT_EQ(j2534_clock_step(c, 0x10u + 5000000u, 1100 + gap),
              uint64_t{1000 + 0x20} + gap);
}

/* A vendor clock that steps, back or far ahead, doesn't take the
 * session's time with it: the host's delta stands in, and the vendor's
 * deltas resume from the new stamp. */
TEST(J2534StreamTest, VendorClockThatStepsFallsBackToTheHost)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 50000000u, 0);
    EXPECT_EQ(j2534_clock_step(c, 10000000u, 2000), 2000u) << "back 40 s";
    EXPECT_EQ(j2534_clock_step(c, 10001000u, 2500), 3000u);
    EXPECT_EQ(j2534_clock_step(c, 10001000u + 120000000u, 3500), 4000u)
        << "two minutes ahead of the host";
    EXPECT_EQ(j2534_clock_step(c, 10001000u + 120000000u, 3400), 4000u)
        << "never decreases";
}

/* A vendor clock that stands still or counts frames lags the host a frame
 * at a time: the lag is judged over the session, so it still catches up. */
TEST(J2534StreamTest, VendorClockThatStaysFrozenIsCaughtUpToTheHost)
{
    j2534_clock c;
    constexpr uint64_t kMs = 1000;
    constexpr uint64_t kFrame = 10 * kMs;
    (void)j2534_clock_step(c, 7000u, 0);
    uint64_t caught_at = 0;
    for (uint64_t t = kFrame; t <= 40000 * kMs; t += kFrame) {
        const uint64_t at = j2534_clock_step(c, 7000u, t);
        if (caught_at == 0 && c.trust_lost) {
            caught_at = t;
            EXPECT_GT(t, J2534_CLOCK_LAG_US) << "never before the lag";
        }
        if (caught_at != 0) {
            EXPECT_LE(at, t) << t;
            EXPECT_GE(at + kFrame, t) << "within a frame of the host, " << t;
        }
    }
    EXPECT_NE(caught_at, 0u);
    EXPECT_LE(caught_at, J2534_CLOCK_LAG_US + 2 * kFrame);
    EXPECT_GT(c.host_steps, 2900u) << "every frame after the catch-up";
}

TEST(J2534StreamTest, VendorClockThatCountsOnePerFrameIsCaughtUp)
{
    j2534_clock c;
    uint32_t raw = 1;
    (void)j2534_clock_step(c, raw, 0);
    uint64_t at = 0;
    uint64_t t = 0;
    for (int i = 0; i < 4000; ++i) {
        t += 10 * 1000; /* a frame every 10 ms of host time */
        at = j2534_clock_step(c, ++raw, t);
    }
    EXPECT_GE(c.host_steps, 1u);
    EXPECT_GE(at + 10 * 1000, t) << "within a frame of the host";
    EXPECT_LE(at, t);
}

/* Stuck after N honest frames: the honest part keeps the vendor's own
 * resolution, and the catch-up comes only once the lag passes the bound. */
TEST(J2534StreamTest, VendorClockFrozenAfterSomeFramesKeepsItsEarlyDeltas)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 1000u, 0);
    EXPECT_EQ(j2534_clock_step(c, 1500u, 500), 500u);
    EXPECT_EQ(j2534_clock_step(c, 2000u, 1000), 1000u);
    EXPECT_EQ(c.host_steps, 0u);
    /* frozen at 2000 from here, the host at 1 s steps */
    uint64_t at = 0;
    uint32_t steps_seen = 0;
    for (uint64_t t = 1000000; t <= 20000000; t += 1000000) {
        at = j2534_clock_step(c, 2000u, t);
        if (c.host_steps != steps_seen) {
            steps_seen = c.host_steps;
            EXPECT_GT(t, J2534_CLOCK_LAG_US) << "never before the lag";
        }
    }
    EXPECT_GE(steps_seen, 1u);
    EXPECT_EQ(at, 20000000u) << "the host's clock from the catch-up on";
}

/* Queued frames reach the host together, up to the worker's 5 s backoff
 * after their stamps: a legitimate batch is not a stuck clock. */
TEST(J2534StreamTest, VendorClockBatchQueuedForFiveSecondsIsNotCaughtUp)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 1000000u, 0);
    /* the vendor ran 5 s while the worker backed off; the frames reach
     * the host at 5 s and the vendor's stamps say 1.0 to 5.0 s */
    uint64_t at = 0;
    for (int i = 1; i <= 50; ++i) {
        at = j2534_clock_step(c, 1000000u + i * 100000u, 5000000u);
    }
    EXPECT_EQ(c.host_steps, 0u);
    EXPECT_EQ(at, 5000000u);
}

/* ECUIO_ERR_REQUEST_FAILED has a distinct, mapped string. */
TEST(J2534StreamTest, RequestFailedStatusHasDistinctString)
{
    const char* s = ecuio_status_string(ECUIO_ERR_REQUEST_FAILED);
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "unknown status");
    EXPECT_STRNE(s, ecuio_status_string(ECUIO_ERR_TRANSPORT_HARD_FAILURE));
}

} // namespace

#endif // _WIN32
