// File/layer: tests/test_j2534_hold_session.cpp | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest (core/src/j2534_hold.h: a session request
//          against the hold)

#include <gtest/gtest.h>

#include "../core/src/j2534_hold.h"

#include <cstdint>

namespace {

constexpr int64_t kMs = 1000;

/* How long since channel 1's last request */
enum Since { recent, keepalive, near_s3, past_s3 };

struct Variant {
    const char* name;
    Since since;
    /* 0 none; 1-5 identifying to the talk out; 6 talk first; 7 talk first
     * with 3E 00 taken (its write failed); 8 3E 00 taken */
    int taken;
    bool entering;
    uint8_t sid; /* what goes out, 0 held */
    int64_t keepalive_us = ECUIO_ENGINE_KEEPALIVE_US;
};

/* Channel 1's session as v has it, in its extended session at 3 s with
 * channel 0's segmented request out: what j2534_hold_next gives channel
 * 1 (*r, the one-shot's first byte in *sid) and the bits it held */
void next_beside_a_segmented_reply(const Variant& v, int32_t* r, uint8_t* sid,
                                   uint32_t* held)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 2));
    ASSERT_TRUE(
        ecuio_engine_session(&e, ECUIO_SESSION_EXTENDED, v.keepalive_us));
    const uint8_t a[3] = {0x22, 0x1E, 0x00};
    const uint8_t b[3] = {0x22, 0x1E, 0x80};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 3), ecuio_engine_add_result::added);
    const int64_t now = 3000 * kMs;
    for (uint32_t c = 0; c < 2; ++c) {
        e.chans[c].session_step = ECUIO_SESSION_STATE_ENTERED;
    }
    ecuio_engine_channel& ch = e.chans[1];
    const int64_t since[] = {kMs, v.keepalive_us,
                             ECUIO_ENGINE_S3_US - J2534_HOLD_S3_MARGIN_US,
                             ECUIO_ENGINE_S3_US};
    ch.last_sent_us = now - since[v.since];
    if (v.entering) {
        ch.session_step = ECUIO_SESSION_STATE_ENTERING;
    }
    e.requests[0].reply_frames = 3;
    ecuio_engine_sent(&e, 0, 0, true, now - kMs); /* segmented, out */
    ch.identifying = v.taken == 1;
    ch.diagnosing = v.taken == 2;
    ch.scanning = v.taken == 3;
    ch.clearing = v.taken == 4;
    ch.talk.out = v.taken == 5;
    if (v.taken >= 7) {
        ch.sessioning = true;
        ch.once.bytes[0] = 0x3E;
        ch.once.len = 2;
    }
    if (v.taken == 6 || v.taken == 7) {
        const uint8_t pdu[3] = {0x22, 0xF1, 0x90};
        ASSERT_EQ(ecuio_engine_talk(ch.talk, e.pdu_max, pdu, 3, 3, true),
                  ecuio_talk_offer::taken);
    }
    j2534_hold hold;
    *held = 0;
    *r = j2534_hold_next(hold, &e, 1, now, held);
    *sid = ch.once.bytes[0];
}

} // namespace

/* A held channel's session request goes through the hold only while it is
 * in its session with S3 near (J2534_HOLD_S3_MARGIN_US or less left, past
 * the longest keep-alive) or past: a hold never ages a session past S3,
 * and a 3E 00 with time to spare, or an entry still to make, waits like
 * any request (on the VX its reply could be lost beside a segmented one).
 * Through, it follows due's order: a one-shot taken goes first, and a talk
 * offered first goes ahead of a keep-alive but not of an entry. */
TEST(J2534HoldTest, AHeldChannelsSessionRequestGoesThroughOnlyNearS3)
{
    const int64_t longest = ECUIO_ENGINE_KEEPALIVE_MAX_US;
    const Variant variants[] = {
        {"keep-alive due", keepalive, 0, false, 0},
        {"keep-alive due, S3 near", near_s3, 0, false, 0x3E},
        {"the longest keep-alive due", keepalive, 0, false, 0, longest},
        {"the longest keep-alive, S3 near", near_s3, 0, false, 0x3E, longest},
        {"not due", recent, 0, false, 0},
        {"identifying, S3 near", near_s3, 1, false, 0},
        {"diagnosing, S3 near", near_s3, 2, false, 0},
        {"scanning, S3 near", near_s3, 3, false, 0},
        {"clearing, S3 near", near_s3, 4, false, 0},
        {"talk out, S3 near", near_s3, 5, false, 0},
        {"talk first, S3 near", near_s3, 6, false, 0},
        {"entering", recent, 0, true, 0},
        {"entering, talk first", recent, 6, true, 0},
        {"entering, S3 passed", past_s3, 0, true, 0},
        {"S3 passed", past_s3, 0, false, 0x10},
        {"talk first, S3 passed", past_s3, 6, false, 0x10},
        {"talk first, 3E 00 taken, S3 near", near_s3, 7, false, 0x3E},
        {"3E 00 taken", keepalive, 8, false, 0},
    };
    for (const Variant& v : variants) {
        int32_t r = 0;
        uint8_t sid = 0;
        uint32_t held = 0;
        ASSERT_NO_FATAL_FAILURE(
            next_beside_a_segmented_reply(v, &r, &sid, &held));
        if (v.sid != 0) {
            ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << v.name;
            EXPECT_EQ(sid, v.sid) << v.name;
            EXPECT_EQ(held, 0u) << v.name;
        } else {
            EXPECT_EQ(r, ECUIO_ENGINE_NONE) << v.name;
            EXPECT_EQ(held, 2u) << v.name;
        }
    }
}
