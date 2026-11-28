// File/layer: tests/test_session_enter.cpp | test_obd2, Qt-free
// Testing: UNIT: SessionControlTest (one channel's session entered alone,
//          ecuio_engine_session_enter, with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

/* An ECU with an extended session: 10 03 and 10 01 answered, 3E 00 too,
 * a 22 of the channel's DID; refuse: 10 03 refused 7F 10 22; quiet: 3E 00
 * unanswered */
struct Ecu {
    bool refuse = false;
    bool quiet = false;
    std::vector<Bytes> heard;

    Bytes operator()(const Bytes& q)
    {
        heard.push_back(q);
        if (q.size() == 2 && q[0] == 0x10) {
            if (refuse && q[1] == 0x03) {
                return {0x7F, 0x10, 0x22};
            }
            return {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == Bytes{0x3E, 0x00}) {
            return quiet ? Bytes{} : Bytes{0x7E, 0x00};
        }
        if (q.size() == 3 && q[0] == 0x22) {
            return {0x62, q[1], q[2], 0x12};
        }
        return {0x7F, q.empty() ? uint8_t{0} : q[0], 0x11};
    }
};

/* Sends channel c's next request at *now and answers it through ecu */
void step(ecuio_request_engine* e, uint32_t c, int64_t* now, Ecu* ecu)
{
    const int32_t r = ecuio_engine_due(e, c, *now);
    if (r == ECUIO_ENGINE_NONE) {
        *now += ECUIO_ENGINE_GAP_US;
        return;
    }
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    const Bytes sent(q.bytes, q.bytes + q.len);
    ecuio_engine_sent(e, c, r, true, *now);
    const Bytes reply = (*ecu)(sent);
    if (reply.empty()) {
        *now = e->chans[c].deadline_us;
        (void)ecuio_engine_expire(e, c, *now);
        *now += ECUIO_ENGINE_GAP_US;
        return;
    }
    *now += 4 * kMs;
    (void)ecuio_engine_on_frame(
        e, c, reply.data(), static_cast<uint16_t>(reply.size()), *now, nullptr);
    *now += ECUIO_ENGINE_GAP_US;
}

/* Channels with a DID each in the rotation (22 1E 1C, 22 1E 1D, ...) */
void engine(ecuio_request_engine* e, uint32_t channels)
{
    ASSERT_TRUE(ecuio_engine_init(e, channels));
    for (uint32_t c = 0; c < channels; ++c) {
        const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(0x1C + c)};
        ASSERT_EQ(ecuio_engine_add(e, c, did, 3),
                  ecuio_engine_add_result::added);
    }
}

size_t count_of(const std::vector<Bytes>& v, const Bytes& want)
{
    size_t n = 0;
    for (const Bytes& b : v) {
        n += b == want ? 1 : 0;
    }
    return n;
}

} // namespace

/* A conversation's module enters the extended session on its own
 * channel: its 10 03 goes first there, its keep-alive follows, and the
 * close leaves it; the other channel is never asked and reports none */
TEST(SessionControlTest, OneChannelEntersTheSessionAlone)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 2));
    const uint32_t before = e.chans[1].generation;
    ASSERT_TRUE(ecuio_engine_session_enter(&e, 1));
    EXPECT_NE(e.chans[1].generation, before);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERING);
    Ecu other;
    Ecu bound;
    int64_t now = 0;
    for (int i = 0; i < 60; ++i) {
        step(&e, 0, &now, &other);
        step(&e, 1, &now, &bound);
    }
    ASSERT_FALSE(bound.heard.empty());
    EXPECT_EQ(bound.heard.front(), (Bytes{0x10, 0x03}));
    EXPECT_EQ(count_of(bound.heard, {0x10, 0x03}), 1u);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(count_of(other.heard, {0x10, 0x03}), 0u);
    EXPECT_EQ(count_of(other.heard, {0x3E, 0x00}), 0u);

    /* the keep-alive, once nothing else went for keepalive_us */
    for (uint32_t r = 0; r < e.count; ++r) {
        e.requests[r].parked = 1;
    }
    ecuio_engine_recount(&e);
    bound.heard.clear();
    const int64_t until = now + ECUIO_ENGINE_KEEPALIVE_US + 100 * kMs;
    while (now < until) {
        step(&e, 1, &now, &bound);
    }
    EXPECT_GE(count_of(bound.heard, {0x3E, 0x00}), 1u);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.session_asked, 0);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_NOT_ASKED);
    EXPECT_EQ(s.session, ECUIO_SESSION_DEFAULT);
    ecuio_engine_support(&e, 1, &s);
    EXPECT_EQ(s.session_asked, 0x03);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(s.session, 0x03);

    ecuio_request leave;
    EXPECT_FALSE(ecuio_engine_session_leave(&e, 0, &leave));
    ASSERT_TRUE(ecuio_engine_session_leave(&e, 1, &leave));
    EXPECT_EQ(Bytes(leave.bytes, leave.bytes + leave.len), (Bytes{0x10, 0x01}));
    ecuio_engine_support(&e, 1, &s);
    EXPECT_EQ(s.session_asked, 0x03); /* the close leaves it asked */
}

/* A run's own session: kept by 3E 00, never entered again however long
 * the channel is quiet, left by the close's 10 01, and back in default
 * once returned */
TEST(SessionControlTest, ARunsOwnSessionIsKeptAndNeverEnteredAgain)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 1));
    for (uint32_t r = 0; r < e.count; ++r) {
        e.requests[r].parked = 1;
    }
    ecuio_engine_recount(&e);
    int64_t now = kMs;
    const uint16_t handed = e.chans[0].session_handed;
    ecuio_engine_session_handed(&e, 0, now);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_NE(e.chans[0].session_handed, handed);
    Ecu ecu;
    const int64_t until = now + 3 * ECUIO_ENGINE_KEEPALIVE_US;
    while (now < until) {
        step(&e, 0, &now, &ecu);
    }
    EXPECT_GE(count_of(ecu.heard, {0x3E, 0x00}), 2u);
    now += ECUIO_ENGINE_S3_US;
    for (int i = 0; i < 20; ++i) {
        step(&e, 0, &now, &ecu);
    }
    EXPECT_EQ(count_of(ecu.heard, {0x10, 0x03}), 0u)
        << "a quiet past S3 enters nothing of the engine's";
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    e.requests[0].parked = 0;
    ecuio_engine_recount(&e);
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_NE(r, ECUIO_ENGINE_NONE);
    ecuio_engine_sent(&e, 0, r, true, now);
    e.chans[0].session_fresh = false;
    EXPECT_FALSE(ecuio_engine_session_reenter(&e, 0, r, 0x7F, now))
        << "a 7F in a run's session is the run's to judge";

    ecuio_request leave;
    ASSERT_TRUE(ecuio_engine_session_leave(&e, 0, &leave));
    EXPECT_EQ(Bytes(leave.bytes, leave.bytes + leave.len), (Bytes{0x10, 0x01}));
    ecuio_engine_session_returned(&e, 0);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    EXPECT_FALSE(e.chans[0].handed);
}

/* A run's session whose keep-alive goes unanswered may be gone: it reads
 * unanswered, and the engine enters nothing in its place */
TEST(SessionControlTest, ARunsSessionUnansweredIsNotEnteredAgain)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 1));
    for (uint32_t r = 0; r < e.count; ++r) {
        e.requests[r].parked = 1;
    }
    ecuio_engine_recount(&e);
    int64_t now = kMs;
    ecuio_engine_session_handed(&e, 0, now);
    Ecu ecu;
    ecu.quiet = true;
    const int64_t until = now + 3 * ECUIO_ENGINE_KEEPALIVE_US;
    while (now < until) {
        step(&e, 0, &now, &ecu);
    }
    EXPECT_GE(count_of(ecu.heard, {0x3E, 0x00}), 1u);
    EXPECT_EQ(count_of(ecu.heard, {0x10, 0x03}), 0u);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_UNANSWERED);
}

/* A run's session returned on a channel the engine had entered: entered
 * again, the conversation's levels locked by the count */
TEST(SessionControlTest, AReturnedSessionGoesBackToTheEnginesOwn)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 1));
    ASSERT_TRUE(ecuio_engine_session_enter(&e, 0));
    Ecu ecu;
    int64_t now = 0;
    for (int i = 0; i < 4; ++i) {
        step(&e, 0, &now, &ecu);
    }
    ASSERT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ecuio_engine_session_handed(&e, 0, now);
    const uint16_t handed = e.chans[0].session_handed;
    ecuio_engine_session_returned(&e, 0);
    EXPECT_NE(e.chans[0].session_handed, handed);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
    ecu.heard.clear();
    step(&e, 0, &now, &ecu);
    ASSERT_FALSE(ecu.heard.empty());
    EXPECT_EQ(ecu.heard.front(), (Bytes{0x10, 0x03}));
    ecuio_engine_session_returned(&e, 0);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED)
        << "a return with nothing handed changes nothing";
}

/* A channel ?session=03 asked already keeps its step and its keep-alive:
 * a refusal stands, an entered session isn't entered again */
TEST(SessionControlTest, EnteringAChannelAskedAlreadyChangesNothing)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 2));
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, 2500 * kMs));
    Ecu entered;
    Ecu refusing;
    refusing.refuse = true;
    int64_t now = 0;
    for (int i = 0; i < 4; ++i) {
        step(&e, 0, &now, &entered);
        step(&e, 1, &now, &refusing);
    }
    ASSERT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED);
    const uint32_t gen0 = e.chans[0].generation;
    const uint32_t gen1 = e.chans[1].generation;
    EXPECT_TRUE(ecuio_engine_session_enter(&e, 0));
    EXPECT_FALSE(ecuio_engine_session_enter(&e, 1));
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED);
    EXPECT_EQ(e.chans[1].session_nrc, 0x22);
    EXPECT_EQ(e.chans[0].generation, gen0);
    EXPECT_EQ(e.chans[1].generation, gen1);
    EXPECT_EQ(e.keepalive_us, 2500 * kMs);
    entered.heard.clear();
    for (int i = 0; i < 8; ++i) {
        step(&e, 0, &now, &entered);
    }
    EXPECT_EQ(count_of(entered.heard, {0x10, 0x03}), 0u);
}

/* A functional channel is never entered alone (its 10 03 reaches every
 * module), nor a channel the engine lacks; neither starts a session */
TEST(SessionControlTest, AFunctionalChannelIsNeverEnteredAlone)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 2));
    e.chans[1].functional = true;
    EXPECT_FALSE(ecuio_engine_session_enter(&e, 1));
    EXPECT_FALSE(ecuio_engine_session_enter(&e, 2));
    EXPECT_EQ(e.chans[1].session_asked, 0);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    EXPECT_FALSE(ecuio_engine_functional_session(&e));
    EXPECT_TRUE(ecuio_engine_session_enter(&e, 0));
    EXPECT_FALSE(ecuio_engine_functional_session(&e));
}

/* A session entered alone is the built-in step's: a 7F 22 7F to its read
 * enters it again and asks the read again, on that channel only */
TEST(SessionControlTest, ASessionEnteredAloneIsEnteredAgainWhenLost)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 2));
    ASSERT_TRUE(ecuio_engine_session_enter(&e, 0));
    Ecu ecu;
    Ecu other;
    int64_t now = 0;
    for (int i = 0; i < 4; ++i) {
        step(&e, 0, &now, &ecu);
        step(&e, 1, &now, &other);
    }
    ASSERT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), 0); /* the read's row */
    ecuio_engine_sent(&e, 0, 0, true, now);
    const uint8_t lost[3] = {0x7F, 0x22, 0x7F};
    (void)ecuio_engine_on_frame(&e, 0, lost, 3, now + 4 * kMs, nullptr);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
    EXPECT_EQ(e.chans[0].session_lost, 1u);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    now += 10 * kMs;
    ecu.heard.clear();
    for (int i = 0; i < 6 && ecu.heard.size() < 2; ++i) {
        step(&e, 0, &now, &ecu);
    }
    ASSERT_EQ(ecu.heard.size(), 2u);
    EXPECT_EQ(ecu.heard[0], (Bytes{0x10, 0x03}));
    EXPECT_EQ(ecu.heard[1], (Bytes{0x22, 0x1E, 0x1C}));
}

/* Each channel asks its own session: a conversation's on one channel
 * never reaches another channel's reopen */
TEST(SessionControlTest, EachChannelAsksItsOwnSession)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, 2));
    ASSERT_TRUE(ecuio_engine_session_enter(&e, 1));
    int64_t now = 0;
    ecuio_engine_module_want(&e, 0, 0);
    (void)ecuio_engine_modules_settle(&e, now);
    ASSERT_EQ(e.chans[0].module_state, ECUIO_MODULE_STATE_CLOSED);
    ecuio_engine_module_want(&e, 0, ECUIO_ENGINE_REASON_READ);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    Ecu other;
    Ecu bound;
    for (int i = 0; i < 20; ++i) {
        step(&e, 0, &now, &other);
        step(&e, 1, &now, &bound);
    }
    EXPECT_EQ(count_of(other.heard, {0x10, 0x03}), 0u);
    EXPECT_GE(count_of(other.heard, {0x22, 0x1E, 0x1C}), 1u);
    EXPECT_EQ(count_of(bound.heard, {0x10, 0x03}), 1u);
}
