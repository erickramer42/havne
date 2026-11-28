#include "test_obd2_support.h"

namespace {

class DiagnosticsChannelTest : public DiagnosticsTest
{};

/* Every channel's due request sent and answered through the fake until
 * nothing is left; what each channel sent */
std::vector<std::vector<Bytes>> drive_all(ecuio_request_engine* e, int64_t* now)
{
    std::vector<std::vector<Bytes>> sent(e->chan_count);
    for (int step = 0; step < 2000; ++step) {
        bool any = false;
        for (uint32_t c = 0; c < e->chan_count; ++c) {
            const int32_t r = ecuio_engine_due(e, c, *now);
            if (r == ECUIO_ENGINE_NONE) {
                continue;
            }
            any = true;
            const ecuio_request& q = ecuio_engine_request_of(e, c, r);
            const Bytes b(q.bytes, q.bytes + q.len);
            sent[c].push_back(b);
            ecuio_engine_sent(e, c, r, true, *now);
            const Bytes reply = fake_reply(b);
            if (reply.empty()) {
                (void)ecuio_engine_expire(e, c, *now + ECUIO_ENGINE_WAIT_US);
            } else {
                (void)ecuio_engine_on_frame(e, c, reply.data(),
                                            static_cast<uint16_t>(reply.size()),
                                            *now + 4 * kMs, nullptr);
            }
        }
        *now += any ? ECUIO_ENGINE_WAIT_US : ECUIO_ENGINE_GAP_US;
        bool pending = false;
        for (uint32_t c = 0; c < e->chan_count; ++c) {
            pending = pending || e->chans[c].diag_pending > 0 ||
                      e->chans[c].outstanding != ECUIO_ENGINE_NONE;
        }
        if (!pending && !any) {
            return sent;
        }
    }
    ADD_FAILURE() << "the reads never settled";
    return sent;
}

void two_modules(ecuio_request_engine* e, int64_t* now)
{
    ecuio_engine_init(e, 2);
    ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
    (void)drive_all(e, now);
}

size_t count_of(const std::vector<Bytes>& v, const Bytes& b)
{
    size_t n = 0;
    for (const Bytes& x : v) {
        n += x == b ? 1u : 0u;
    }
    return n;
}

} // namespace

TEST_F(DiagnosticsChannelTest, OneChannelsListRestartsAndTheOthersStay)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    two_modules(&e, &now);
    ASSERT_NE(e.chans[0].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
    ASSERT_NE(e.chans[1].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
    const uint32_t g0 = e.chans[0].diag.generation;
    const uint32_t g1 = e.chans[1].diag.generation;
    ecuio_engine_diagnose_channels_again(&e, 1u << 1);
    const std::vector<std::vector<Bytes>> sent = drive_all(&e, &now);
    EXPECT_TRUE(sent[0].empty());
    EXPECT_EQ(count_of(sent[1], {0x03}), 1u);
    EXPECT_EQ(count_of(sent[1], {0x07}), 1u);
    EXPECT_EQ(count_of(sent[1], {0x0A}), 1u);
    EXPECT_EQ(e.chans[0].diag.generation, g0);
    EXPECT_GT(e.chans[1].diag.generation, g1);
    EXPECT_NE(e.chans[1].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
}

TEST_F(DiagnosticsChannelTest, ACallDuringAReadLeavesItToEnd)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    ecuio_engine_diagnose_channels_again(&e, 1u);
    (void)ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US);
    now += ECUIO_ENGINE_WAIT_US;
    const std::vector<std::vector<Bytes>> sent = drive_all(&e, &now);
    EXPECT_EQ(count_of(sent[0], {0x01, 0x01}), 0u)
        << "the read under way goes on; its first item isn't asked again";
    EXPECT_EQ(count_of(sent[0], {0x03}), 1u);
    EXPECT_NE(e.chans[0].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
}

TEST(DiagnosticsChannelSessionTest, AChannelPastTheCountIsRefused)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://", &s), ECUIO_OK);
    ecuio_session_info_t info;
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics_channel(s, 0), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics_channel(s, info.module_count),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("modules"),
              std::string::npos);
    EXPECT_EQ(ecuio_session_read_diagnostics_channel(nullptr, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_close(s);
    EXPECT_EQ(ecuio_session_read_diagnostics_channel(s, 0),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
}
