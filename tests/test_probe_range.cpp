// File/layer: tests/test_probe_range.cpp | test_obd2, Qt-free
// Testing: UNIT: ProbeRangeTest (the start's emissions probe's rules:
//          request_probe.h; ADR-0065 step 5's RANGE); the transports' own
//          cases run it on their fakes

#include "test_obd2_support.h"

#include "../core/src/request_probe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

const uint8_t kAnswer[] = {0x41, 0x00, 0xBE, 0x3F, 0xA8, 0x13};
const uint8_t kRefusal[] = {0x7F, 0x01, 0x11};

std::vector<uint32_t> group_of(const std::vector<uint32_t>& rx, bool ext29,
                               uint32_t budget, uint32_t g)
{
    uint32_t out[ECUIO_MAX_CHANNELS];
    const uint32_t n = ecuio_probe_group(
        rx.data(), static_cast<uint32_t>(rx.size()), ext29, budget, g, out);
    return std::vector<uint32_t>(out, out + n);
}

uint32_t range_of(const ecuio_probe& p, const std::vector<uint32_t>& rx,
                  uint32_t engine = UINT32_MAX)
{
    return ecuio_probe_range(p, rx.data(), static_cast<uint32_t>(rx.size()),
                             engine);
}

} // namespace

/* 11-bit: one request, hearing the eight emissions replies whatever the
 * table declares */
TEST(ProbeRangeTest, TheProbeIsOneFunctionalRequestOnElevenBit)
{
    const std::vector<uint32_t> table = {0x7E8, 0x726};
    EXPECT_EQ(group_of(table, false, 9, 0),
              (std::vector<uint32_t>{0x7E8, 0x7E9, 0x7EA, 0x7EB, 0x7EC, 0x7ED,
                                     0x7EE, 0x7EF}));
    EXPECT_TRUE(group_of(table, false, 9, 1).empty()) << "one group alone";
    EXPECT_EQ(ECUIO_PROBE_TX_11, 0x7DFu);
    EXPECT_EQ(ECUIO_PROBE_REQUEST[0], 0x01u);
    EXPECT_EQ(ECUIO_PROBE_REQUEST[1], 0x00u);
}

/* 29-bit: the declared 18DAF1xx modules alone, in groups no larger than
 * the budget (12 candidates, budget 9: two requests) */
TEST(ProbeRangeTest, A29BitProbeGoesInGroupsWithinTheBudget)
{
    std::vector<uint32_t> table;
    for (uint32_t k = 0; k < 12; ++k) {
        table.push_back(0x18DAF110u + k);
    }
    table.push_back(0x18DA10F1u); /* not a reply to the tester */
    const std::vector<uint32_t> first = group_of(table, true, 9, 0);
    const std::vector<uint32_t> second = group_of(table, true, 9, 1);
    ASSERT_EQ(first.size(), 9u);
    ASSERT_EQ(second.size(), 3u);
    EXPECT_EQ(first.front(), 0x18DAF110u);
    EXPECT_EQ(second.back(), 0x18DAF11Bu);
    EXPECT_TRUE(group_of(table, true, 9, 2).empty());
    EXPECT_EQ(ECUIO_PROBE_TX_29, 0x18DB33F1u);
}

/* Only a positive answer counts, once; a refusal is no answer */
TEST(ProbeRangeTest, ARefusalOfTheProbeIsNoAnswer)
{
    ecuio_probe p;
    ecuio_probe_take(p, 0x7EB, kRefusal, sizeof kRefusal);
    EXPECT_EQ(p.answered_count, 0u);
    ecuio_probe_take(p, 0x7E8, kAnswer, sizeof kAnswer);
    ecuio_probe_take(p, 0x7E8, kAnswer, sizeof kAnswer);
    ecuio_probe_take(p, 0x7E9, kAnswer, 1);
    EXPECT_EQ(p.answered_count, 1u);
    EXPECT_TRUE(ecuio_probe_answered(p, 0x7E8));
    EXPECT_FALSE(ecuio_probe_answered(p, 0x7EB));
}

/* 7E8 and 7E9 answer: the declared 7E8 is in range, a declared 7EA that
 * kept silent and 726 are not; the undeclared 7E9 joins the table */
TEST(ProbeRangeTest, AModuleThatAnswersTheProbeIsInRange)
{
    ecuio_probe p;
    ecuio_probe_take(p, 0x7E8, kAnswer, sizeof kAnswer);
    ecuio_probe_take(p, 0x7E9, kAnswer, sizeof kAnswer);
    uint32_t rx[ECUIO_MAX_CHANNELS] = {0x7E8, 0x7EA, 0x726};
    uint32_t tx[ECUIO_MAX_CHANNELS] = {0x7E0, 0x7E2, 0x72E};
    uint32_t count = 3;
    EXPECT_EQ(ecuio_probe_join(p, rx, tx, &count), 1u);
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(rx[3], 0x7E9u);
    EXPECT_EQ(tx[3], 0x7E1u);
    EXPECT_EQ(ecuio_probe_range(p, rx, count, 0), 0x1u | 0x8u);
}

/* A 29-bit candidate that answers is in range, one that doesn't is left
 * to the picks; 29-bit answerers never join (declared ones alone heard) */
TEST(ProbeRangeTest, A29BitCandidateThatAnswersJoinsTheRange)
{
    ecuio_probe p;
    ecuio_probe_take(p, 0x18DAF110u, kAnswer, sizeof kAnswer);
    ecuio_probe_take(p, 0x18DAF1A0u, kAnswer, sizeof kAnswer);
    uint32_t rx[ECUIO_MAX_CHANNELS] = {0x18DAF110u, 0x18DAF118u};
    uint32_t tx[ECUIO_MAX_CHANNELS] = {0x18DA10F1u, 0x18DA18F1u};
    uint32_t count = 2;
    EXPECT_EQ(ecuio_probe_join(p, rx, tx, &count), 0u);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(ecuio_probe_range(p, rx, count, 0), 0x1u)
        << "the candidate that kept silent is left to the picks";
}

/* No answer at all learns nothing: the engine module (the channel asking
 * PID 0C), else the first, stands in alone (the ignition off at the
 * start), never every declared emissions module */
TEST(ProbeRangeTest, NoAnswerLeavesTheEngineModuleAlone)
{
    const ecuio_probe none;
    EXPECT_EQ(range_of(none, {0x7E9, 0x7E8, 0x7EF, 0x7F0, 0x726}, 1), 0x2u);
    std::vector<uint32_t> wide;
    for (uint32_t k = 0; k < 12; ++k) {
        wide.push_back(0x18DAF110u + k);
    }
    EXPECT_EQ(range_of(none, wide, 0), 0x1u)
        << "the declared 29-bit modules are left to the picks";
    EXPECT_EQ(range_of(none, {0x7E9, 0x7E8}), 0x1u) << "no engine module";
    EXPECT_EQ(range_of(none, {0x7E9, 0x7E8}, 2), 0x1u) << "past the table";
    EXPECT_EQ(range_of(none, {}), 0u) << "no module at all";
    ecuio_probe one;
    ecuio_probe_take(one, 0x7E9, kAnswer, sizeof kAnswer);
    EXPECT_EQ(range_of(one, {0x7E8, 0x7E9}, 0), 0x2u)
        << "an answer anywhere and the silent are left to the picks";
}

/* The join stops at the table's room */
TEST(ProbeRangeTest, TheJoinStopsAtTheTableRoom)
{
    ecuio_probe p;
    ecuio_probe_take(p, 0x7E9, kAnswer, sizeof kAnswer);
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
    uint32_t tx[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t c = 0; c < ECUIO_MAX_CHANNELS; ++c) {
        rx[c] = 0x700 + c;
        tx[c] = 0x600 + c;
    }
    uint32_t count = ECUIO_MAX_CHANNELS;
    EXPECT_EQ(ecuio_probe_join(p, rx, tx, &count), 0u);
    EXPECT_EQ(count, ECUIO_MAX_CHANNELS);
}

/* The session log's line: the requests, the time, the answerers */
TEST(ProbeRangeTest, TheLogLineNamesTheAnswerers)
{
    ecuio_probe p;
    p.requests = 1;
    p.took_us = 104000;
    char line[160];
    ecuio_probe_describe(p, line, sizeof line);
    EXPECT_EQ(std::string(line),
              "emissions probe: 1 functional 01 00 in 104 ms, no answer");
    ecuio_probe_take(p, 0x7E8, kAnswer, sizeof kAnswer);
    ecuio_probe_take(p, 0x7E9, kAnswer, sizeof kAnswer);
    ecuio_probe_describe(p, line, sizeof line);
    EXPECT_EQ(std::string(line), "emissions probe: 1 functional 01 00 in "
                                 "104 ms, answered by 7E8 7E9");
}
