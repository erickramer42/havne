// File/layer: tests/test_request_turns_session.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestTurnsTest (request_turns.h and the extended
//          session, ADR-0060: a row standing in for the keep-alive, the
//          hand-over at an urgent session, with the time passed in)

#include "request_turns_support.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

using namespace request_turns_test;

namespace {

/* Each channel's longest time between two of its requests from from_us
 * to the run's end, the tail counted */
std::vector<int64_t> longest_gaps(const Adapter& a, int64_t from_us)
{
    std::vector<int64_t> last(a.e.chan_count, from_us);
    std::vector<int64_t> gap(a.e.chan_count, 0);
    for (const Sent& x : a.sent) {
        if (x.at_us < from_us) {
            continue;
        }
        gap[x.c] = std::max(gap[x.c], x.at_us - last[x.c]);
        last[x.c] = x.at_us;
    }
    for (uint32_t c = 0; c < a.e.chan_count; ++c) {
        gap[c] = std::max(gap[c], a.now - last[c]);
    }
    return gap;
}

/* The 3E 00 sent from from_us on */
int keepalives(const Adapter& a, int64_t from_us)
{
    int n = 0;
    for (const Sent& x : a.sent) {
        n += x.at_us >= from_us && x.bytes[0] == 0x3E ? 1 : 0;
    }
    return n;
}

/* n channels of rows DIDs each in the extended session at keepalive_us */
std::unique_ptr<Adapter> in_session_with(uint32_t n, int rows,
                                         int64_t keepalive_us)
{
    auto a = adapter(n);
    for (uint32_t c = 0; c < n; ++c) {
        dids(&a->e, c, rows);
    }
    EXPECT_TRUE(ecuio_engine_session(&a->e, 0x03, keepalive_us));
    a->ecus = [](uint32_t, const Bytes& q) { return in_session(q); };
    return a;
}

} // namespace

/* 32 channels of one row in the extended session, the keep-alive at its
 * least and a 25 ms exchange: a lap (800 ms) outlasts the keep-alive, so
 * each channel's row stands in for its 3E 00. Every row is asked once a
 * lap and no 3E 00 goes */
TEST(RequestTurnsTest, ARowStandsInForTheKeepAliveOnEveryChannel)
{
    auto a = in_session_with(32, 1, ECUIO_ENGINE_KEEPALIVE_MIN_US);
    a->exchange_us = 25 * kMs;
    a->until(60000 * kMs);
    const int64_t from = 10000 * kMs;
    EXPECT_EQ(keepalives(*a, from), 0);
    const std::vector<int64_t> gap = longest_gaps(*a, from);
    for (uint32_t c = 0; c < 32; ++c) {
        EXPECT_LE(gap[c], 32 * a->exchange_us * 3 / 2) << "channel " << c;
        EXPECT_EQ(a->e.chans[c].session_lost, 0u) << "channel " << c;
    }
}

/* 14 channels of four rows, the keep-alive at its least, a 25 ms
 * exchange: each lap (1.4 s) outlasts the keep-alive but not the urgent
 * time, so every turn asks its channel's four rows whole */
TEST(RequestTurnsTest, AStandInKeepsEachTurnWholeUntilItsSessionIsUrgent)
{
    auto a = in_session_with(14, 4, ECUIO_ENGINE_KEEPALIVE_MIN_US);
    a->exchange_us = 25 * kMs;
    a->until(5000 * kMs);
    const size_t from = a->sent.size();
    a->until(30000 * kMs);
    const auto r = runs(a->channels(from));
    ASSERT_GE(r.size(), 20u);
    for (size_t i = 1; i + 1 < r.size(); ++i) {
        EXPECT_EQ(r[i].second, 4)
            << "run " << i << " of channel " << r[i].first;
    }
    EXPECT_EQ(keepalives(*a, 5000 * kMs), 0);
}

/* 32 channels of four rows at the default keep-alive and a 90 ms
 * exchange: a lap (11.5 s) outlasts S3, so each session falls urgent and
 * is asked a second before S3 runs out; none is lost and every row is
 * asked */
TEST(RequestTurnsTest, EverySessionIsKeptWhenTheLapOutlastsS3)
{
    auto a = in_session_with(32, 4, ECUIO_ENGINE_KEEPALIVE_US);
    a->until(70000 * kMs);
    const int64_t from = 10000 * kMs;
    const std::vector<int64_t> gap = longest_gaps(*a, from);
    std::set<std::pair<uint32_t, uint8_t>> asked;
    for (const Sent& x : a->sent) {
        if (x.at_us >= from && x.bytes[0] == 0x22) {
            asked.insert({x.c, x.bytes[2]});
        }
    }
    EXPECT_EQ(asked.size(), 32u * 4u);
    for (uint32_t c = 0; c < 32; ++c) {
        EXPECT_LT(gap[c], ECUIO_ENGINE_S3_US - 500 * kMs) << "channel " << c;
        EXPECT_EQ(a->e.chans[c].session_lost, 0u) << "channel " << c;
    }
}
