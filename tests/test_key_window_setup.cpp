// File/layer: tests/test_key_window_setup.cpp | test_core, Qt-free, C++17
// Testing: UNIT: KeyWindowSetupTest (what a requesting transport's setup
//          gives the key's window, core/src/request_setup.h: the channels
//          marked functional from their tx IDs, and every window ended
//          while the link isn't OK)

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "link_health.h"
#include "request_setup.h"

#include <atomic>
#include <memory>

namespace {

/* A seed sent and answered on channel c at now_us: its window is open */
void open_window(ecuio_request_engine* e, uint32_t c, int64_t now_us)
{
    const uint8_t seed[2] = {0x27, 0x01};
    ecuio_talk_request q;
    q.pdu = seed;
    q.len = 2;
    q.echo = 2;
    q.kind = ecuio_talk_kind::seed;
    q.window_us = ECUIO_KEY_WINDOW_MAX_US;
    ASSERT_EQ(ecuio_engine_offer_talk(e, c, q, now_us),
              ecuio_talk_offer::taken);
    ASSERT_EQ(ecuio_engine_due(e, c, now_us), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(e, c, ECUIO_ENGINE_ONCE, true, now_us);
    const uint8_t reply[6] = {0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08};
    ASSERT_EQ(ecuio_engine_on_frame(e, c, reply, 6, now_us, nullptr),
              ecuio_reply::positive);
    ecuio_talk_result settled;
    ASSERT_TRUE(ecuio_engine_talk_take(e->chans[c].talk, &settled));
    ASSERT_TRUE(ecuio_key_window_open(e->chans[c].key, now_us));
}

} // namespace

/* The setup marks a channel functional by its request ID alone: 0x7DF
 * and 0x18DB33F1, whatever it receives on */
TEST(KeyWindowSetupTest, AChannelIsFunctionalByItsRequestId)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://functional", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    const uint32_t rx[5] = {0x7E8, 0x7E9, 0x18DAF110, 0x18DAF111, 0x7DF};
    const uint32_t tx[5] = {0x7DF, 0x7E1, 0x18DB33F1, 0x18DA11F1, 0x7E7};
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, rx, tx, 5, e.get(), &probe,
                                   &identify),
              ECUIO_OK);
    ASSERT_EQ(e->chan_count, 5u);
    EXPECT_TRUE(e->chans[0].functional);
    EXPECT_FALSE(e->chans[1].functional);
    EXPECT_TRUE(e->chans[2].functional);
    EXPECT_FALSE(e->chans[3].functional);
    EXPECT_FALSE(e->chans[4].functional)
        << "by the request ID, not the reply's";
    EXPECT_TRUE(isotp_functional_id(0x7DF));
    EXPECT_TRUE(isotp_functional_id(0x18DB33F1));
    EXPECT_FALSE(isotp_functional_id(0x7E0));
    EXPECT_FALSE(isotp_functional_id(0x18DA10F1));
    ecuio_session_destroy(s);
}

/* The worker's judge: while its link is in any state but OK no window
 * stands, a module's or another's; an OK link leaves them */
TEST(KeyWindowSetupTest, EveryWindowEndsWhileTheLinkIsNotOk)
{
    for (const int32_t state :
         {ECUIO_ERR_VEHICLE_TIMEOUT, ECUIO_ERR_TRANSPORT_HARD_FAILURE,
          ECUIO_ERR_REQUEST_FAILED}) {
        auto e = std::make_unique<ecuio_request_engine>();
        ASSERT_TRUE(ecuio_engine_init(e.get(), 2));
        ASSERT_NO_FATAL_FAILURE(open_window(e.get(), 0, 1000));
        ASSERT_NO_FATAL_FAILURE(open_window(e.get(), 1, 1000));
        std::atomic<int32_t> published{ECUIO_OK};
        std::atomic<uint32_t> episodes{0};
        link_health h(published, episodes);
        ecuio_key_windows_judge(e.get(), h);
        EXPECT_TRUE(ecuio_key_window_open(e->chans[0].key, 2000)) << "OK";
        EXPECT_TRUE(ecuio_key_window_open(e->chans[1].key, 2000));
        link_set_state(h, state);
        ecuio_key_windows_judge(e.get(), h);
        EXPECT_EQ(e->chans[0].key.until_us, 0) << state;
        EXPECT_EQ(e->chans[1].key.until_us, 0) << state;
    }
}
