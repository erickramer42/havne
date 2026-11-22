// File/layer: tests/test_obd2_engine.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

/* ---------- request engine ---------- */

namespace {

/* A def read from a reply that opens with key (mux_length bytes) at
 * offset 0, on message_id. Enough for derivation; not a full def. */
ecuio_signal_def_t keyed_def(uint32_t key, uint8_t length,
                             uint32_t message_id = ECUIO_OBD2_ECU_ARB_ID,
                             uint8_t offset = 0)
{
    ecuio_signal_def_t d = {};
    d.message_id = message_id;
    d.mux_offset = offset;
    d.mux_length = length;
    d.mux_key = key;
    return d;
}

} // namespace

TEST(RequestEngineTest, DefsDeriveTheRequestTheirReplyAnswers) {
    uint8_t req[3] = {};
    uint8_t len = 0;
    ecuio_signal_def_t d = keyed_def(0x410Cu, 2);
    ASSERT_TRUE(ecuio_engine_request_for_def(&d, req, &len));
    EXPECT_EQ(len, 2u);
    EXPECT_EQ(req[0], 0x01u);
    EXPECT_EQ(req[1], 0x0Cu);

    d = keyed_def(0x621E1Cu, 3);
    ASSERT_TRUE(ecuio_engine_request_for_def(&d, req, &len));
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(req[0], 0x22u);
    EXPECT_EQ(req[1], 0x1Eu);
    EXPECT_EQ(req[2], 0x1Cu);

    /* None of these asks for anything, though each still decodes. */
    const ecuio_signal_def_t kNot[] = {
        keyed_def(0x410Cu, 2, ECUIO_OBD2_ECU_ARB_ID, 1),   /* not at offset 0 */
        keyed_def(0x620Cu, 2),      /* a 62 key needs its 2-byte DID */
        keyed_def(0x410C00u, 3),    /* a 41 key is the PID alone */
        keyed_def(0x4902u, 2),      /* Service 09: read once, never rotated */
        keyed_def(0x0Cu, 1),
        keyed_def(0x410Cu, 0),      /* no mux */
    };
    for (const auto& n : kNot) {
        uint8_t untouched[3] = {0xEE, 0xEE, 0xEE};
        uint8_t ulen = 0xEE;
        EXPECT_FALSE(ecuio_engine_request_for_def(&n, untouched, &ulen))
            << std::hex << n.mux_key;
        EXPECT_EQ(untouched[0], 0xEEu);
        EXPECT_EQ(ulen, 0xEEu);
    }
}

TEST(RequestEngineTest, Obd2SetEnrolsItsPidsInDefOrder) {
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1,
                                    nullptr));
    /* The ten first, then each other PID once, in PID order: a PID with
     * several fields costs one request */
    const uint8_t ten[] = {0x04, 0x05, 0x0C, 0x0D, 0x0F,
                           0x11, 0x2F, 0x42, 0x10, 0x5C};
    ASSERT_EQ(e.count, 92u) << "J1979's linear PIDs";
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].len, 2u) << "index " << i;
        EXPECT_EQ(e.requests[i].bytes[0], 0x01u) << "index " << i;
        EXPECT_EQ(e.requests[i].channel, 0u) << "index " << i;
        if (i < sizeof(ten)) {
            EXPECT_EQ(e.requests[i].bytes[1], ten[i]) << "index " << i;
        } else if (i > sizeof(ten)) {
            EXPECT_GT(e.requests[i].bytes[1], e.requests[i - 1].bytes[1])
                << "index " << i;
        }
    }
}

/* ADR-0065: the engine holds a channel for each module of its set, one
 * at least and ECUIO_MAX_CHANNELS at most, and a request goes out on the
 * channel it was added to whichever that is */
TEST(RequestEngineTest, TheEngineHoldsAChannelForEachModule) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 0);
    EXPECT_EQ(e.chan_count, 1u);
    EXPECT_EQ(e.chans.size(), 1u);
    ecuio_engine_init(&e, ECUIO_MAX_CHANNELS + 8);
    EXPECT_EQ(e.chan_count, uint32_t{ECUIO_MAX_CHANNELS});
    EXPECT_EQ(e.chans.size(), size_t{ECUIO_MAX_CHANNELS});

    ecuio_engine_init(&e, 6);
    EXPECT_EQ(e.chan_count, 6u);
    ASSERT_EQ(e.chans.size(), 6u);
    const uint8_t did[3] = {0x22, 0xD1, 0x05};
    EXPECT_EQ(ecuio_engine_add(&e, 6, did, 3), ecuio_engine_add_result::refused)
        << "a channel past the engine's";
    ASSERT_EQ(ecuio_engine_add(&e, 5, did, 3), ecuio_engine_add_result::added);
    for (uint32_t c = 0; c < 5; ++c) {
        EXPECT_EQ(ecuio_engine_due(&e, c, 1000), ECUIO_ENGINE_NONE) << c;
    }
    const int32_t r = ecuio_engine_due(&e, 5, 1000);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(e.requests[r].channel, 5u);
    ecuio_engine_sent(&e, 5, r, true, 1000);
    EXPECT_EQ(e.chans[5].outstanding, 0);
    EXPECT_EQ(e.chans[4].outstanding, ECUIO_ENGINE_NONE);
}

TEST(RequestEngineTest, MockAndNullSetsEnrolNothing) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    EXPECT_TRUE(ecuio_engine_derive(&e, ecuio_mock_signal_set(), nullptr, 0,
                                    nullptr));
    EXPECT_TRUE(ecuio_engine_derive(&e, nullptr, nullptr, 0, nullptr));
    EXPECT_EQ(e.count, 0u);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), -1);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), INT64_MAX);
}

/* One request per reply and channel: two defs read from one reply cost
 * one request, the same PID asked of two ECUs costs two, and a def whose
 * message_id is no channel's goes to channel 0. Service 01 and 0x22 mix
 * in one set. */
TEST(RequestEngineTest, RequestsAreKeptOncePerChannel) {
    const ecuio_signal_def_t defs[] = {
        keyed_def(0x4114u, 2, 0x7E8u),     /* O2 sensor voltage */
        keyed_def(0x4114u, 2, 0x7E8u),     /* its trim, same reply */
        keyed_def(0x4114u, 2, 0x7E9u),     /* the same PID, the TCM */
        keyed_def(0x621E1Cu, 3, 0x7E9u),
        keyed_def(0x410Cu, 2, 0x7EAu),     /* no such channel */
    };
    const ecuio_signal_set_t set = set_of(defs, 5);
    const uint32_t rx[2] = {0x7E8u, 0x7E9u};
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, rx, 2, nullptr));
    ASSERT_EQ(e.count, 4u);
    EXPECT_EQ(e.requests[0].channel, 0u);
    EXPECT_EQ(e.requests[1].channel, 1u);
    EXPECT_EQ(e.requests[1].bytes[1], 0x14u);
    EXPECT_EQ(e.requests[2].channel, 1u);
    EXPECT_EQ(e.requests[2].bytes[0], 0x22u);
    EXPECT_EQ(e.requests[3].channel, 0u);
    EXPECT_EQ(e.requests[3].bytes[1], 0x0Cu);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(e.chans[1].requests, 2u);
}

/* Section 3: whatever reaches the table, only reads go out (ADR-0037 adds the
 * trouble codes' and the freeze frame's). Service 0x04 clears the
 * emission DTCs; 0x10, 0x27, 0x2E, 0x31 and 0x11 change a session,
 * unlock, write, run a routine or reset. */
TEST(RequestEngineTest, OnlyReadServicesEnterTheTable) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t kReads[] = {0x01, 0x02, 0x03, 0x07, 0x09, 0x0A, 0x22};
    for (uint8_t sid : kReads) {
        const uint8_t req[3] = {sid, 0x02, 0x00};
        EXPECT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::added) << std::hex << int{sid};
    }
    const uint8_t kOthers[] = {0x04, 0x10, 0x11, 0x14, 0x19, 0x27, 0x28,
                               0x2E, 0x2F, 0x31, 0x34, 0x3B, 0x3E, 0x85};
    for (uint8_t sid : kOthers) {
        const uint8_t req[3] = {sid, 0x01, 0x00};
        EXPECT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::refused) << std::hex << int{sid};
    }
    const uint8_t pid[4] = {0x01, 0x0C, 0x00, 0x00};
    EXPECT_EQ(ecuio_engine_add(&e, 0, pid, 1), ecuio_engine_add_result::refused);
    EXPECT_EQ(ecuio_engine_add(&e, 0, pid, 4), ecuio_engine_add_result::refused);
    EXPECT_EQ(ecuio_engine_add(&e, 1, pid, 2), ecuio_engine_add_result::refused)
        << "a channel past the table";
    EXPECT_EQ(e.count, sizeof kReads);
}

TEST(RequestEngineTest, TableHoldsTheLimitAndNamesTheFirstDefPastIt) {
    static ecuio_signal_def_t defs[ECUIO_PROFILE_MAX_REQUESTS + 2];
    for (uint32_t i = 0; i < ECUIO_PROFILE_MAX_REQUESTS + 2; ++i) {
        /* Distinct DIDs, with one repeat at index 1. */
        defs[i] = keyed_def(0x620000u | ((i == 1) ? 0u : i), 3);
    }
    ecuio_signal_set_t set = set_of(defs, ECUIO_PROFILE_MAX_REQUESTS + 1);
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    EXPECT_TRUE(ecuio_engine_derive(&e, &set, nullptr, 0, nullptr))
        << "the repeat fits a full table";
    EXPECT_EQ(e.count, ECUIO_PROFILE_MAX_REQUESTS);

    set.count = ECUIO_PROFILE_MAX_REQUESTS + 2;
    ecuio_engine_init(&e, 1);
    uint32_t overflow = UINT32_MAX;
    EXPECT_FALSE(ecuio_engine_derive(&e, &set, nullptr, 0, &overflow));
    EXPECT_EQ(e.count, ECUIO_PROFILE_MAX_REQUESTS);
    EXPECT_EQ(overflow, ECUIO_PROFILE_MAX_REQUESTS + 1);
}

/* A channel's rotation runs in table order and wraps, and each channel
 * walks its own. */
TEST(RequestEngineTest, EachChannelRotatesItsOwnRequests) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t a[2] = {0x01, 0x0A}, b[2] = {0x01, 0x0B},
                  c[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, c, 2), ecuio_engine_add_result::added);

    int64_t now = 0;
    uint8_t sent0[4] = {}, sent1[4] = {};
    for (int i = 0; i < 4; ++i) {
        const int32_t r0 = ecuio_engine_due(&e, 0, now);
        const int32_t r1 = ecuio_engine_due(&e, 1, now);
        ASSERT_GE(r0, 0);
        ASSERT_GE(r1, 0);
        sent0[i] = e.requests[r0].bytes[1];
        sent1[i] = e.requests[r1].bytes[1];
        ecuio_engine_sent(&e, 0, r0, true, now);
        ecuio_engine_sent(&e, 1, r1, true, now);
        now += ECUIO_ENGINE_WAIT_US;   /* both wait out */
        EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        EXPECT_TRUE(ecuio_engine_expire(&e, 1, now));
    }
    const uint8_t want0[4] = {0x0A, 0x0C, 0x0A, 0x0C};
    const uint8_t want1[4] = {0x0B, 0x0B, 0x0B, 0x0B};
    EXPECT_EQ(memcmp(sent0, want0, 4), 0);
    EXPECT_EQ(memcmp(sent1, want1, 4), 0);
}

/* The next request waits for the reply and for the floor: a reply in
 * 4 ms still leaves 16 ms of it. */
TEST(RequestEngineTest, NextRequestWaitsForTheReplyAndTheFloor) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    const int32_t r = ecuio_engine_due(&e, 0, 0);
    ASSERT_EQ(r, 0);
    ecuio_engine_sent(&e, 0, r, true, 0);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 30 * kMs), -1) << "one outstanding";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), ECUIO_ENGINE_WAIT_US);

    const uint8_t reply[4] = {0x41, 0x0C, 0x1F, 0x40};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, reply, 4, 4 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 4 * kMs), -1) << "inside the floor";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 4 * kMs),
              ECUIO_ENGINE_GAP_US - 4 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US), 1);
}

TEST(RequestEngineTest, UnansweredRequestTimesOutAtTheWait) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C}));
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US - 1));
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US))
        << "a request times out once";
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), 0) << "wrapped";

    /* A reply that comes after its request timed out answers nothing. */
    const uint8_t late[3] = {0x41, 0x0C, 0x00};
    ecuio_request_engine idle;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&idle, {0x0C}));
    EXPECT_EQ(ecuio_engine_on_frame(&idle, 0, late, 3, 0, nullptr),
              ecuio_reply::none);
}

/* Positive: the service ID + 0x40 and the whole key. Negative: 7F and
 * the request's service ID. Pending holds the request for P2*. */
TEST(RequestEngineTest, RepliesCorrelateByServiceAndKey) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);

    const uint8_t other_did[3] = {0x62, 0x1E, 0x1D};
    const uint8_t other_service[3] = {0x41, 0x1E, 0x1C};
    const uint8_t neg_other[3] = {0x7F, 0x01, 0x31};
    const uint8_t short_frame[2] = {0x62, 0x1E};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other_did, 3, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other_service, 3, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, neg_other, 3, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, short_frame, 2, 0, nullptr),
              ecuio_reply::none);

    const uint8_t pending[3] = {0x7F, 0x22, 0x78};
    uint8_t nrc = 0;
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, pending, 3, 40 * kMs, &nrc),
              ecuio_reply::pending);
    EXPECT_EQ(nrc, 0x78u);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US))
        << "pending extends the wait";
    EXPECT_FALSE(ecuio_engine_expire(
        &e, 0, 40 * kMs + ECUIO_ENGINE_PENDING_WAIT_US - 1));

    const uint8_t positive[4] = {0x62, 0x1E, 0x1C, 0x2A};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, positive, 4, 300 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].outstanding, -1);

    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 300 * kMs), true,
                      300 * kMs);
    const uint8_t negative[3] = {0x7F, 0x22, 0x31};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, negative, 3, 310 * kMs, &nrc),
              ecuio_reply::negative);
    EXPECT_EQ(nrc, 0x31u);
    EXPECT_EQ(e.chans[0].outstanding, -1);
}

/* A write that didn't go out leaves nothing to wait for; the floor
 * still spaces the next attempt. */
TEST(RequestEngineTest, UnsentRequestLeavesNothingOutstanding) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), false, 0);
    EXPECT_EQ(e.chans[0].outstanding, -1);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US - 1), -1);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US), 1);
}

/* The floor runs from the write's start, which the driver gives: a
 * write that took 3 ms doesn't add them to the floor. The wait still
 * runs from the write's return. */
TEST(RequestEngineTest, TheFloorRunsFromTheWritesStart)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 1 * kMs), true, 4 * kMs,
                      1 * kMs);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, 4 * kMs + ECUIO_ENGINE_WAIT_US - 1))
        << "the wait runs from the return";
    const uint8_t reply[4] = {0x41, 0x0C, 0x1F, 0x40};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply, 4, 8 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 1 * kMs + ECUIO_ENGINE_GAP_US - 1), -1);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 1 * kMs + ECUIO_ENGINE_GAP_US), 1)
        << "the floor ran from the return";
}

/* With no start given, or one after the return, the floor runs from the
 * return. */
TEST(RequestEngineTest, WithoutAStartTheFloorRunsFromTheReturn)
{
    for (const int64_t start : {int64_t{-1}, int64_t{9 * kMs}}) {
        SCOPED_TRACE(start);
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
        ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), false, 4 * kMs,
                          start);
        EXPECT_EQ(ecuio_engine_due(&e, 0, 4 * kMs + ECUIO_ENGINE_GAP_US - 1),
                  -1);
        EXPECT_EQ(ecuio_engine_due(&e, 0, 4 * kMs + ECUIO_ENGINE_GAP_US), 1);
    }
}

/* The driver's floor (?gap=) replaces the default: 5 ms, and 0, where
 * the next request is due as soon as the reply is in. */
TEST(RequestEngineTest, TheDriversGapIsTheFloor)
{
    for (const int64_t gap : {int64_t{5 * kMs}, int64_t{0}}) {
        SCOPED_TRACE(gap);
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
        e.gap_us = gap;
        ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0, 0);
        EXPECT_EQ(ecuio_engine_due(&e, 0, gap), -1) << "one outstanding";
        const uint8_t reply[4] = {0x41, 0x0C, 0x1F, 0x40};
        ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply, 4, 0, nullptr),
                  ecuio_reply::positive);
        if (gap > 0) {
            EXPECT_EQ(ecuio_engine_due(&e, 0, gap - 1), -1);
            EXPECT_EQ(ecuio_engine_next_event_us(&e, gap - 1 * kMs), 1 * kMs);
        }
        EXPECT_EQ(ecuio_engine_due(&e, 0, gap), 1);
    }
}

/* A reply may come while a request is out, or after its wait ended until
 * a positive reply puts the channel back in step; otherwise none can, on
 * any channel. */
TEST(RequestEngineTest, AReplyIsAwaitedWhileOutAndOutOfStep)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t a[2] = {0x01, 0x0C}, b[2] = {0x01, 0x0D};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 2), ecuio_engine_add_result::added);
    EXPECT_FALSE(ecuio_engine_awaiting(&e)) << "nothing asked yet";

    ecuio_engine_sent(&e, 1, ecuio_engine_due(&e, 1, 0), true, 0, 0);
    EXPECT_TRUE(ecuio_engine_awaiting(&e)) << "channel 1's request is out";
    const uint8_t reply_b[3] = {0x41, 0x0D, 0x32};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 1, reply_b, 3, 4 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_FALSE(ecuio_engine_awaiting(&e)) << "every reply in";

    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 5 * kMs), true, 5 * kMs,
                      5 * kMs);
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, 5 * kMs + ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(e.chans[0].outstanding, -1);
    EXPECT_TRUE(ecuio_engine_awaiting(&e)) << "its reply may come late";
    const uint8_t reply_a[4] = {0x41, 0x0C, 0x1F, 0x40};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply_a, 4, 200 * kMs, nullptr),
              ecuio_reply::none)
        << "a late reply answers nothing";
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 200 * kMs), true,
                      200 * kMs, 200 * kMs);
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply_a, 4, 204 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_FALSE(ecuio_engine_awaiting(&e)) << "back in step";
}

/* The next event is the nearest of every channel's: a wait ending, or
 * an idle channel's floor; a channel with no requests has none. */
TEST(RequestEngineTest, NextEventIsTheNearestAcrossChannels) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 3);
    const uint8_t a[2] = {0x01, 0x0A}, b[2] = {0x01, 0x0B};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 2), ecuio_engine_add_result::added);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), 0) << "both due now";

    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);
    ecuio_engine_sent(&e, 1, ecuio_engine_due(&e, 1, 0), false, 0);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 5 * kMs),
              ECUIO_ENGINE_GAP_US - 5 * kMs) << "channel 1's floor";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, ECUIO_ENGINE_GAP_US + 5 * kMs), 0)
        << "channel 1 due";
}

/* The obd2 set's requests take the classes obd2_rates.h gives their
 * PIDs, and the rotation sends two fast ones for each slow one: the
 * first turns by hand, then over three times the fast class's length,
 * every third request slow, each class in set order and wrapping on
 * its own. */
TEST(RequestEngineTest, TheObd2SetRunsTwoFastRequestsToOneSlow)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ASSERT_TRUE(
        ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1, nullptr));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].slow != 0,
                  !ecuio_obd2_pid_fast(e.requests[i].bytes[1]))
            << "PID " << int(e.requests[i].bytes[1]);
    }
    const std::vector<uint8_t> first = {0x04, 0x0C, 0x05, 0x0D, 0x11, 0x0F,
                                        0x10, 0x06, 0x2F, 0x08, 0x0A, 0x42};
    std::vector<uint8_t> fast, slow;
    for (uint32_t i = 0; i < e.count; ++i) {
        (e.requests[i].slow ? slow : fast).push_back(e.requests[i].bytes[1]);
    }
    ASSERT_FALSE(fast.empty());
    ASSERT_FALSE(slow.empty());
    const std::vector<uint8_t> sent =
        rotation_of(&e, static_cast<int>(3 * fast.size()));
    ASSERT_EQ(sent.size(), 3 * fast.size());
    EXPECT_EQ(std::vector<uint8_t>(sent.begin(), sent.begin() + 12), first);
    size_t f = 0, s = 0;
    for (size_t k = 0; k < sent.size(); ++k) {
        if (k % 3 == 2) {
            EXPECT_EQ(sent[k], slow[s++ % slow.size()]) << "turn " << k;
        } else {
            EXPECT_EQ(sent[k], fast[f++ % fast.size()]) << "turn " << k;
        }
    }
}

/* A class with nothing to send gives its turns to the other: a table of
 * one class, or one whose other class was dropped, rotates in table
 * order. A table never classified is all fast. */
TEST(RequestEngineTest, AClassWithNothingToSendGivesItsTurnsToTheOther)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x05, 0x0F, 0x42}));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    EXPECT_EQ(rotation_of(&e, 7),
              (std::vector<uint8_t>{0x05, 0x0F, 0x42, 0x05, 0x0F, 0x42, 0x05}))
        << "slow alone";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x04, 0x0C, 0x0D}));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    EXPECT_EQ(rotation_of(&e, 7),
              (std::vector<uint8_t>{0x04, 0x0C, 0x0D, 0x04, 0x0C, 0x0D, 0x04}))
        << "fast alone";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x04, 0x05, 0x0C}));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    e.requests[1].dropped = ECUIO_DROP_BITMAP;
    EXPECT_EQ(rotation_of(&e, 5),
              (std::vector<uint8_t>{0x04, 0x0C, 0x04, 0x0C, 0x04}))
        << "the slow class dropped";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x05, 0x04}));
    EXPECT_EQ(rotation_of(&e, 4),
              (std::vector<uint8_t>{0x05, 0x04, 0x05, 0x04}))
        << "never classified";
}

/* Only Service 01 requests take a class: a DID stays fast whatever the
 * predicate says of its high byte. */
TEST(RequestEngineTest, OnlyServiceOneRequestsAreClassified)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did[3] = {0x22, 0x05, 0x01}, pid[2] = {0x01, 0x05};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    EXPECT_EQ(e.requests[0].slow, 0u);
    EXPECT_EQ(e.requests[1].slow, 1u);
}
