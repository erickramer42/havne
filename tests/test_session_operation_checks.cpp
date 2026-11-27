#include "session_operation_support.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using engine_talk_test::Bytes;
using session_operation::Answer;
using session_operation::Bench;

const Bytes kRpm = {0x01, 0x0C};
const Bytes kKmh = {0x01, 0x0D};
constexpr uint32_t kSpeeds =
    ECUIO_OPERATION_CHECK_RPM | ECUIO_OPERATION_CHECK_KMH;

/* The run's requests from from on: the rotation's row and the keep-alive
 * left out */
std::vector<Bytes> run_sent(const Bench& b, size_t from)
{
    std::vector<Bytes> out;
    for (size_t i = from; i < b.sent.size(); ++i) {
        if (b.sent[i] != engine_talk_test::kRow &&
            b.sent[i] != engine_talk_test::kKeep) {
            out.push_back(b.sent[i]);
        }
    }
    return out;
}

/* A node that answers q with reply, and the rest as the node does */
Bench::Script answering(const Bytes& q, const Bytes& reply)
{
    return [q, reply](const Bytes& asked, size_t, Answer* a) {
        if (asked != q) {
            return false;
        }
        if (!reply.empty()) {
            a->frames = {reply};
        }
        return true;
    };
}

TEST(SessionOperationTest, AnEngineRunningEndsTheRunWithOnlyItsSpeedAsked)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.script = answering(kRpm, {0x41, 0x0C, 0x0D, 0x48});
    const size_t from = b.sent.size();
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_ENGINE_RUNNING) << v.words;
    EXPECT_STREQ(v.words, "the engine is running (850 rpm): switch it off by "
                          "the key; nothing was read");
    EXPECT_EQ(v.rpm, 850u);
    EXPECT_EQ(v.checks & kSpeeds, ECUIO_OPERATION_CHECK_RPM);
    EXPECT_EQ(run_sent(b, from), std::vector<Bytes>{kRpm});
    EXPECT_TRUE(b.files().empty());
}

TEST(SessionOperationTest, AMovingVehicleEndsTheRunBeforeTheUnlock)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.script = answering(kKmh, {0x41, 0x0D, 0x0C});
    const size_t from = b.sent.size();
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_MOVING) << v.words;
    EXPECT_STREQ(v.words, "the vehicle is moving (12 km/h): nothing was read");
    EXPECT_EQ(v.kmh, 12u);
    EXPECT_EQ(v.rpm, 0u);
    EXPECT_EQ(v.checks & kSpeeds,
              ECUIO_OPERATION_CHECK_RPM | ECUIO_OPERATION_CHECK_KMH);
    EXPECT_EQ(run_sent(b, from), (std::vector<Bytes>{kRpm, kKmh}));
    EXPECT_TRUE(b.files().empty());
}

TEST(SessionOperationTest, AParkedVehicleIsReadAndTheRunGoesOn)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    b.until_phase(ECUIO_OPERATION_PHASE_PRECONDITIONS);
    EXPECT_EQ(b.state().phase, ECUIO_OPERATION_PHASE_PRECONDITIONS);
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.checks & kSpeeds,
              ECUIO_OPERATION_CHECK_RPM | ECUIO_OPERATION_CHECK_KMH);
    EXPECT_EQ(v.rpm, 0u);
    EXPECT_EQ(v.kmh, 0u);
}

/* No answer is no reading, never a zero: the parked confirmation stands
 * for it */
TEST(SessionOperationTest, SpeedsUnansweredOrRefusedLetTheRunGoOn)
{
    for (const Bytes& reply : {Bytes{}, Bytes{0x7F, 0x01, 0x12}}) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.script = [reply](const Bytes& q, size_t, Answer* a) {
            if (q != kRpm && q != kKmh) {
                return false;
            }
            if (!reply.empty()) {
                a->frames = {reply};
            }
            return true;
        };
        const size_t from = b.sent.size();
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_EQ(v.checks & kSpeeds, 0u);
        const std::vector<Bytes> sent = run_sent(b, from);
        ASSERT_GT(sent.size(), 3u);
        EXPECT_EQ(sent[0], kRpm);
        EXPECT_EQ(sent[1], kKmh);
        EXPECT_EQ(sent[2], (Bytes{0x27, 0x01}));
    }
}

TEST(SessionOperationTest, AnEnvelopeWithNothingReadLogsADashForEach)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.battery_source = false;
    b.script = [](const Bytes& q, size_t, Answer*) {
        return q.size() == 2 && q[0] == 0x01 &&
               (q[1] == 0x0C || q[1] == 0x0D || q[1] == 0x40 || q[1] == 0x42);
    };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    const ecuio_run_lines& l = b.s->run_lines;
    ASSERT_GE(l.count, 1u);
    char want[128];
    std::snprintf(want, sizeof want,
                  "run envelope  7E8  battery - mV  rpm -  km/h -  checks %X",
                  static_cast<unsigned>(v.checks));
    EXPECT_STREQ(l.text[0], want);
}

TEST(SessionOperationTest, ACancelDuringTheChecksSendsNothingMore)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.script = answering(kRpm, {});
    b.while_out = [](Bench& bench) {
        if (bench.sent.back() == kRpm) {
            EXPECT_EQ(ecuio_session_operation_cancel(bench.s), ECUIO_OK);
        }
    };
    const size_t from = b.sent.size();
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED) << v.words;
    EXPECT_EQ(run_sent(b, from), std::vector<Bytes>{kRpm});
    EXPECT_TRUE(b.files().empty());
}

/* A session entry pending without end holds the checks' talk off the
 * wire: it is given up at the bound, the speeds unread, and the entry's
 * own bound ends the run */
TEST(SessionOperationTest, AChecksTalkThatNeverGoesOutIsGivenUpAtTheBound)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_engine_session_step(b.e->chans[0], ECUIO_SESSION_STATE_ENTERING);
    const size_t from = b.sent.size();
    for (int second = 0; second < 24; ++second) {
        ecuio_step_conversations(b.s, b.e.get(), b.now);
        const int32_t r = ecuio_engine_due(b.e.get(), 0, b.now);
        if (r != ECUIO_ENGINE_NONE) {
            b.sent.push_back(engine_talk_test::wire(b.e.get(), 0, r));
            ecuio_engine_sent(b.e.get(), 0, r, true, b.now);
        }
        if (second == 9) {
            EXPECT_EQ(b.state().phase, ECUIO_OPERATION_PHASE_PRECONDITIONS)
                << "still waited for inside the bound";
        }
        b.now += 1000000;
        b.frame(sim_memory_test::nrc(0x10, 0x78));
    }
    const ecuio_operation_state_t v = b.until_ended(400);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SESSION_LOST) << v.words;
    EXPECT_EQ(v.checks & kSpeeds, 0u);
    EXPECT_EQ(b.count(0x01, from), 0u);
    EXPECT_EQ(b.count(0x27, from) + b.count(0x23, from), 0u);
    EXPECT_TRUE(b.files().empty());
}

} // namespace
