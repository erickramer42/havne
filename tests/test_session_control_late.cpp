// File/layer: tests/test_session_control_late.cpp | test_obd2, Qt-free
// Testing: UNIT: SessionControlTest (the request engine's session step,
//          ADR-0060, with the time passed in: a keep-alive too late for
//          S3)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

/* One channel with nothing to ask but its session, entered at 10 ms */
void entered(ecuio_request_engine* e)
{
    ASSERT_TRUE(ecuio_engine_init(e, 1));
    ASSERT_TRUE(ecuio_engine_session(e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    ASSERT_EQ(ecuio_engine_due(e, 0, 0), ECUIO_ENGINE_ONCE);
    ASSERT_EQ(e->chans[0].once.bytes[0], 0x10);
    ecuio_engine_sent(e, 0, ECUIO_ENGINE_ONCE, true, 0);
    const Bytes ok = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
    (void)ecuio_engine_on_frame(e, 0, ok.data(), 6, 10 * kMs, nullptr);
    ASSERT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
}

/* Channel 0's next request at `quiet` past its last one: its bytes */
Bytes next_after(ecuio_request_engine* e, int64_t quiet)
{
    const int64_t at = e->chans[0].last_sent_us + quiet;
    if (ecuio_engine_due(e, 0, at) != ECUIO_ENGINE_ONCE) {
        return {};
    }
    const ecuio_request& q = e->chans[0].once;
    return Bytes(q.bytes, q.bytes + q.len);
}

} // namespace

/* A request sent ECUIO_ENGINE_S3_LATE_US or more after the last can reach
 * the ECU after its own S3 has run out (an ELM327's exchange, a slow
 * link), and a module back in default answers 3E 00 all the same. So
 * from then on the session is entered again, counted lost, and tester
 * present goes only before it */
TEST(SessionControlTest, AKeepAliveTooLateForS3EntersAgain)
{
    static ecuio_request_engine late;
    ASSERT_NO_FATAL_FAILURE(entered(&late));
    EXPECT_EQ(next_after(&late, ECUIO_ENGINE_S3_US - 200 * kMs),
              (Bytes{0x10, 0x03}));
    EXPECT_EQ(late.chans[0].session_lost, 1u);
    EXPECT_EQ(late.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);

    static ecuio_request_engine kept;
    ASSERT_NO_FATAL_FAILURE(entered(&kept));
    EXPECT_EQ(next_after(&kept, ECUIO_ENGINE_S3_US - 300 * kMs),
              (Bytes{0x3E, 0x00}));
    EXPECT_EQ(kept.chans[0].session_lost, 0u);
    EXPECT_EQ(ECUIO_ENGINE_S3_LATE_US, ECUIO_ENGINE_S3_US - 250 * kMs);
}
