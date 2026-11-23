// File/layer: tests/test_core_awake.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest's keep-awake cases (test_core), through the seam
//
// The host kept awake (ADR-0033's appendix "the host kept awake while a
// session reads"): the desk transports hold nothing, which backends hold,
// the call's refusals, and Linux's rule for a write's level. A live
// session's hold is Elm327AwakeTest's (test_elm327).

#include "core_test_support.h"
#include "awake_seam.h"
#include "host_awake_platform.h" /* core/src: live_backend, judge_take */

using namespace core_test;

TEST(CoreTest, OnlyALiveBackendHoldsAwake)
{
    for (uint32_t b = 0; b <= ECUIO_BACKEND_SOCKETCAN + 1; ++b) {
        const bool live = b == ECUIO_BACKEND_ELM327 ||
                          b == ECUIO_BACKEND_J2534_SURROGATE ||
                          b == ECUIO_BACKEND_SOCKETCAN;
        EXPECT_EQ(host_awake::live_backend(b), live) << "backend " << b;
    }
}

/* logind refused, the desktop held: a read holds, a write refuses */
TEST(CoreTest, AWriteHoldNeedsLogindsSleepLock)
{
    using host_awake::judge_take;
    using host_awake::level;
    EXPECT_EQ(judge_take(level::idle, false, true, "logind: Access denied"),
              ECUIO_OK);
    EXPECT_EQ(judge_take(level::sleep, false, true, "logind: Access denied"),
              ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "logind didn't hold the sleep lock a write step needs (the "
                 "desktop holds off only its idle timer): logind: Access "
                 "denied");
    EXPECT_EQ(judge_take(level::sleep, true, false, ""), ECUIO_OK);
    EXPECT_EQ(judge_take(level::idle, true, false, ""), ECUIO_OK);
    EXPECT_EQ(judge_take(level::idle, false, false, "logind: gone"),
              ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "neither logind nor the desktop held the host awake: "
                 "logind: gone");
}

TEST(CoreTest, AMockOrReplaySessionHoldsNothing)
{
    AwakeSeam seam;
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max");
    for (const char* uri : {"mock://", "mock://demo", replay.c_str()}) {
        SCOPED_TRACE(uri);
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_session_keep_awake(s, ECUIO_AWAKE_SYSTEM, "a desk"),
                  ECUIO_OK);
        uint32_t what = 99;
        EXPECT_EQ(ecuio_session_get_awake(s, &what), ECUIO_OK);
        EXPECT_EQ(what, 0u);
        ecuio_session_destroy(s);
    }
    EXPECT_EQ(AwakeSeam::takes.load(), 0);
    EXPECT_EQ(AwakeSeam::releases.load(), 0);
}

TEST(CoreTest, KeepAwakeRefusesAnUnknownBitAndALongWhy)
{
    AwakeSeam seam;
    EXPECT_EQ(ecuio_session_keep_awake(nullptr, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://", &s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_keep_awake(s, 0x2u, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_keep_awake(s, ECUIO_AWAKE_SYSTEM | 0x80000000u,
                                       nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    const std::string longest(ECUIO_AWAKE_WHY_MAX - 1, 'w');
    const std::string too_long(ECUIO_AWAKE_WHY_MAX, 'w');
    EXPECT_EQ(ecuio_session_keep_awake(s, ECUIO_AWAKE_SYSTEM, longest.c_str()),
              ECUIO_OK);
    EXPECT_EQ(
        ecuio_session_keep_awake(s, ECUIO_AWAKE_SYSTEM, too_long.c_str()),
        ECUIO_ERR_INVALID_ARGUMENT);
    uint32_t what = 0;
    EXPECT_EQ(ecuio_session_get_awake(s, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_awake(nullptr, &what),
              ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_keep_awake(s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(ecuio_session_keep_awake(s, 0, nullptr),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
    EXPECT_EQ(AwakeSeam::takes.load(), 0);
}
