// File/layer: tests/test_clear_request.cpp | test_obd2, Qt-free
// Testing: UNIT: ClearRequestTest (one clear request over several modules:
//          the engine and vehicle speed judged once, every target's 04
//          after it, each target's gate; request_engine_diag.h,
//          ADR-0038), and the call's refusals on a mock:// session

#include "test_obd2_support.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace {

class ClearRequestTest : public DiagnosticsTest
{};

/* What a channel sent, and when */
struct Sent {
    uint32_t c;
    int64_t at;
    Bytes bytes;
    bool once;
};

const Bytes kRpm = {0x01, 0x0C};
const Bytes kSpeed = {0x01, 0x0D};
const Bytes kClear = {0x04};

/* Modules side by side with the time passed in, 1 ms a step, each due
 * request answered through respond 4 ms later ({}: silence). Channel 0
 * reads PID 0C (the engine module); every channel reads diagnostics.
 * The default answers: 0 rpm, 0 km/h, 44, and the fake ECU's report. */
struct Clears {
    ecuio_request_engine e;
    int64_t now = 0;
    std::vector<Sent> sent;
    std::vector<int64_t> reply_at;
    std::vector<Bytes> reply;
    std::function<Bytes(uint32_t, const Bytes&)> respond =
        [](uint32_t, const Bytes& q) { return answer(q); };

    static Bytes answer(const Bytes& q)
    {
        if (q == kRpm) {
            return {0x41, 0x0C, 0x00, 0x00};
        }
        if (q == kSpeed) {
            return {0x41, 0x0D, 0x00};
        }
        if (q == kClear) {
            return {0x44};
        }
        return fake_reply(q);
    }

    void make(uint32_t chans, bool engine_row = true)
    {
        ASSERT_TRUE(ecuio_engine_init(&e, chans));
        ecuio_engine_slots(&e, ECUIO_MAX_CHANNELS, ECUIO_MAX_CHANNELS);
        reply_at.assign(chans, INT64_MAX);
        reply.assign(chans, {});
        if (engine_row) {
            const uint8_t q[2] = {0x01, 0x0C};
            ASSERT_EQ(ecuio_engine_add(&e, 0, q, 2),
                      ecuio_engine_add_result::added);
        }
        ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
        until(now + 3000 * kMs);
        for (uint32_t c = 0; c < chans; ++c) {
            ASSERT_TRUE(e.chans[c].diag.flags & ECUIO_DIAG_COMPLETE) << c;
        }
        sent.clear();
    }

    void step()
    {
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (reply_at[c] <= now) {
                reply_at[c] = INT64_MAX;
                (void)ecuio_engine_on_frame(
                    &e, c, reply[c].data(),
                    static_cast<uint16_t>(reply[c].size()), now, nullptr);
            }
            (void)ecuio_engine_expire(&e, c, now);
        }
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            const int32_t r = ecuio_engine_due(&e, c, now);
            if (r == ECUIO_ENGINE_NONE) {
                continue;
            }
            const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
            const Bytes b(q.bytes, q.bytes + q.len);
            sent.push_back({c, now, b, r == ECUIO_ENGINE_ONCE});
            ecuio_engine_sent(&e, c, r, true, now);
            reply[c] = respond(c, b);
            reply_at[c] = reply[c].empty() ? INT64_MAX : now + 4 * kMs;
        }
        (void)ecuio_engine_modules_settle(&e, now);
        now += kMs;
    }

    void until(int64_t t_us)
    {
        while (now < t_us) {
            step();
        }
    }

    /* Runs the request over the channels named, each at its report's
     * generation now; the request's answer */
    bool request(const std::vector<uint32_t>& chans, uint32_t flags = 0)
    {
        std::vector<ecuio_clear_target_t> t;
        for (uint32_t c : chans) {
            t.push_back({c, e.chans[c].diag.generation, {0, 0}});
        }
        return ecuio_engine_clear_request(
            &e, t.data(), static_cast<uint32_t>(t.size()), flags);
    }

    /* The channels that sent q through the one-shot, in order */
    std::vector<uint32_t> senders(const Bytes& q) const
    {
        std::vector<uint32_t> out;
        for (const Sent& s : sent) {
            if (s.once && s.bytes == q) {
                out.push_back(s.c);
            }
        }
        return out;
    }

    /* The channels that sent q through the one-shot, in channel order */
    std::vector<uint32_t> set_of(const Bytes& q) const
    {
        std::vector<uint32_t> out = senders(q);
        std::sort(out.begin(), out.end());
        return out;
    }

    /* When q was first sent, INT64_MAX for never */
    int64_t first(const Bytes& q) const
    {
        for (const Sent& s : sent) {
            if (s.once && s.bytes == q) {
                return s.at;
            }
        }
        return INT64_MAX;
    }

    uint8_t clear_of(uint32_t c) const { return e.chans[c].diag.clear; }
};

using Chans = std::vector<uint32_t>;

/* Polls pred every 10 ms until it holds or ms pass */
template <typename Pred> bool poll(Pred pred, int ms)
{
    for (int waited = 0; waited < ms; waited += 10) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

} // namespace

/* Four targets: one 01 0C to the engine module, one 01 0D, then a 04 to
 * each; every target holds the readings */
TEST_F(ClearRequestTest, TheRpmAndSpeedAreJudgedOnceForAllTargets)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(4));
    ASSERT_TRUE(m.request({0, 1, 2, 3}, ECUIO_CLEAR_ALLOW_RUNNING));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.senders(kRpm), Chans{0});
    EXPECT_EQ(m.senders(kSpeed), Chans{0});
    EXPECT_EQ(m.set_of(kClear), (Chans{0, 1, 2, 3}));
    for (uint32_t c = 0; c < 4; ++c) {
        EXPECT_EQ(m.clear_of(c), ECUIO_CLEAR_CLEARED) << c;
        EXPECT_EQ(m.e.chans[c].diag.flags &
                      (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH),
                  ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH)
            << c;
    }
}

/* No 04 goes out before the vehicle speed's answer has settled */
TEST_F(ClearRequestTest, NoTargetsZeroFourBeforeTheJudgmentEnds)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(3));
    ASSERT_TRUE(m.request({1, 2}, ECUIO_CLEAR_ALLOW_RUNNING));
    m.until(m.now + 3000 * kMs);
    const int64_t speed = m.first(kSpeed);
    ASSERT_NE(speed, INT64_MAX);
    for (const Sent& s : m.sent) {
        if (s.bytes == kClear) {
            EXPECT_GE(s.at, speed + 4 * kMs) << "on " << s.c;
        }
    }
    EXPECT_EQ(m.set_of(kClear), (Chans{1, 2}));
}

/* A request of 7E9 alone: the rpm is still the engine module's */
TEST_F(ClearRequestTest, TheRpmComesFromTheEngineModuleWhenItIsNoTarget)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_TRUE(m.request({1}));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.senders(kRpm), Chans{0});
    EXPECT_EQ(m.set_of(kClear), Chans{1});
    EXPECT_EQ(m.clear_of(1), ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(m.clear_of(0), ECUIO_CLEAR_NONE) << "no target";
}

/* ecuio_engine_clear is a request of one: the same judgment */
TEST_F(ClearRequestTest, ClearWithIsARequestOfOne)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_TRUE(ecuio_engine_clear(&m.e, 1, m.e.chans[1].diag.generation));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.senders(kRpm), Chans{0});
    EXPECT_EQ(m.set_of(kClear), Chans{1});
}

/* The engine module refuses 01 0D: the first target answering it gives
 * the speed, and every 04 goes */
TEST_F(ClearRequestTest, TheSpeedComesFromTheFirstModuleAnsweringOneD)
{
    static Clears m;
    m.respond = [](uint32_t c, const Bytes& q) {
        return c == 0 && q == kSpeed ? Bytes{0x7F, 0x01, 0x12}
                                     : Clears::answer(q);
    };
    ASSERT_NO_FATAL_FAILURE(m.make(3));
    ASSERT_TRUE(m.request({1, 2}, ECUIO_CLEAR_ALLOW_RUNNING));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.senders(kSpeed), (Chans{0, 1}));
    EXPECT_EQ(m.set_of(kClear), (Chans{1, 2}));
    EXPECT_EQ(m.clear_of(2), ECUIO_CLEAR_CLEARED);
}

/* No module gives a speed: every target ends NO_VEHICLE_SPEED, nothing
 * sent; the last refusal's code kept */
TEST_F(ClearRequestTest, NoSpeedFromAnyModuleSendsNothing)
{
    static Clears m;
    m.respond = [](uint32_t, const Bytes& q) {
        return q == kSpeed ? Bytes{0x7F, 0x01, 0x31} : Clears::answer(q);
    };
    ASSERT_NO_FATAL_FAILURE(m.make(3));
    ASSERT_TRUE(m.request({1, 2}, ECUIO_CLEAR_ALLOW_RUNNING));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.senders(kSpeed), (Chans{0, 1, 2}));
    EXPECT_TRUE(m.senders(kClear).empty());
    EXPECT_EQ(m.clear_of(1), ECUIO_CLEAR_NO_VEHICLE_SPEED);
    EXPECT_EQ(m.e.chans[2].diag.clear_nrc, 0x31u);
}

/* No engine module (no row reads PID 0C), or one that never answers:
 * every target NO_ENGINE_SPEED, nothing sent; ALLOW_RUNNING clears */
TEST_F(ClearRequestTest, NoEngineModuleMeansNoRpmForEveryTarget)
{
    static Clears none;
    ASSERT_NO_FATAL_FAILURE(none.make(3, false));
    ASSERT_TRUE(none.request({1, 2}));
    none.until(none.now + 3000 * kMs);
    EXPECT_TRUE(none.senders(kRpm).empty());
    EXPECT_TRUE(none.senders(kClear).empty());
    EXPECT_EQ(none.clear_of(1), ECUIO_CLEAR_NO_ENGINE_SPEED);
    EXPECT_EQ(none.clear_of(2), ECUIO_CLEAR_NO_ENGINE_SPEED);

    static Clears silent;
    silent.respond = [](uint32_t c, const Bytes& q) {
        return c == 0 && q == kRpm ? Bytes{} : Clears::answer(q);
    };
    ASSERT_NO_FATAL_FAILURE(silent.make(3));
    ASSERT_TRUE(silent.request({1, 2}));
    silent.until(silent.now + 3000 * kMs);
    EXPECT_EQ(silent.senders(kRpm), Chans{0}) << "never the targets";
    EXPECT_TRUE(silent.senders(kClear).empty());
    EXPECT_EQ(silent.clear_of(2), ECUIO_CLEAR_NO_ENGINE_SPEED);

    static Clears allowed;
    ASSERT_NO_FATAL_FAILURE(allowed.make(3, false));
    ASSERT_TRUE(allowed.request({1, 2}, ECUIO_CLEAR_ALLOW_RUNNING));
    allowed.until(allowed.now + 3000 * kMs);
    EXPECT_EQ(allowed.senders(kSpeed), Chans{1});
    EXPECT_EQ(allowed.set_of(kClear), (Chans{1, 2}));
}

/* Running: every target ENGINE_RUNNING without ALLOW_RUNNING; moving:
 * every target MOVING without ALLOW_MOVING, and cleared with it */
TEST_F(ClearRequestTest, TheAllowFlagsApplyToTheWholeRequest)
{
    const auto driving = [](uint32_t, const Bytes& q) {
        if (q == kRpm) {
            return Bytes{0x41, 0x0C, 0x1F, 0x40}; /* 2000 rpm */
        }
        return q == kSpeed ? Bytes{0x41, 0x0D, 30} : Clears::answer(q);
    };
    static Clears running;
    running.respond = driving;
    ASSERT_NO_FATAL_FAILURE(running.make(3));
    ASSERT_TRUE(running.request({1, 2}));
    running.until(running.now + 3000 * kMs);
    EXPECT_TRUE(running.senders(kClear).empty());
    for (uint32_t c : {1u, 2u}) {
        EXPECT_EQ(running.clear_of(c), ECUIO_CLEAR_ENGINE_RUNNING) << c;
        EXPECT_EQ(running.e.chans[c].diag.clear_rpm, 2000u) << c;
    }

    static Clears moving;
    moving.respond = driving;
    ASSERT_NO_FATAL_FAILURE(moving.make(3));
    ASSERT_TRUE(moving.request({1, 2}, ECUIO_CLEAR_ALLOW_RUNNING));
    moving.until(moving.now + 3000 * kMs);
    EXPECT_TRUE(moving.senders(kClear).empty());
    for (uint32_t c : {1u, 2u}) {
        EXPECT_EQ(moving.clear_of(c), ECUIO_CLEAR_MOVING) << c;
        EXPECT_EQ(moving.e.chans[c].diag.clear_kmh, 30u) << c;
    }

    static Clears allowed;
    allowed.respond = driving;
    ASSERT_NO_FATAL_FAILURE(allowed.make(3));
    ASSERT_TRUE(allowed.request({1, 2}, ECUIO_CLEAR_ALLOW_RUNNING |
                                            ECUIO_CLEAR_ALLOW_MOVING));
    allowed.until(allowed.now + 3000 * kMs);
    EXPECT_EQ(allowed.set_of(kClear), (Chans{1, 2}));
}

/* A target whose report moved ends REPORT_CHANGED; the rest go on */
TEST_F(ClearRequestTest, ATargetWhoseReportMovedIsReportChangedAndTheRestGo)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(3));
    const ecuio_clear_target_t t[2] = {
        {1, m.e.chans[1].diag.generation - 1, {0, 0}},
        {2, m.e.chans[2].diag.generation, {0, 0}},
    };
    ASSERT_TRUE(ecuio_engine_clear_request(&m.e, t, 2, 0));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.clear_of(1), ECUIO_CLEAR_REPORT_CHANGED);
    EXPECT_EQ(m.clear_of(2), ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(m.set_of(kClear), Chans{2});
}

/* A module that never answered and one refusing every OBD service read
 * to the end, but neither answered 01 00 or 03: NOT_EMISSIONS, nothing
 * sent to them */
TEST_F(ClearRequestTest, ATargetWithNoPositiveEmissionsAnswerIsNotCleared)
{
    static Clears m;
    m.respond = [](uint32_t c, const Bytes& q) {
        if (c == 2) {
            return Bytes{};
        }
        if (c == 3) {
            return Bytes{0x7F, q[0], 0x11};
        }
        return Clears::answer(q);
    };
    ASSERT_NO_FATAL_FAILURE(m.make(4));
    ASSERT_TRUE(m.request({1, 2, 3}));
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.clear_of(1), ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(m.clear_of(2), ECUIO_CLEAR_NOT_EMISSIONS);
    EXPECT_EQ(m.clear_of(3), ECUIO_CLEAR_NOT_EMISSIONS);
    EXPECT_EQ(m.set_of(kClear), Chans{1});

    m.sent.clear();
    EXPECT_FALSE(m.request({2, 3})) << "no target passed its gate";
    m.until(m.now + 1000 * kMs);
    EXPECT_TRUE(m.senders(kRpm).empty());
}

/* A second request while one runs is refused; the first goes on */
TEST_F(ClearRequestTest, ASecondRequestWhileOneRunsIsRefused)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(3));
    ASSERT_TRUE(m.request({1}));
    const uint32_t g2 = m.e.chans[2].diag.generation;
    EXPECT_FALSE(m.request({2}));
    EXPECT_EQ(m.e.chans[2].diag.generation, g2) << "untouched";
    m.until(m.now + 3000 * kMs);
    EXPECT_EQ(m.set_of(kClear), Chans{1});
    EXPECT_TRUE(m.request({2})) << "once it has ended";
}

/* A target open for a read alone stays open through its 04 and the read
 * after it, both lists COMPLETE, then closes */
TEST_F(ClearRequestTest, AModuleStaysOpenUntilItsAfterReadEnds)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_TRUE(m.request({1}));
    ecuio_engine_module_want(&m.e, 1, 0);
    const ecuio_engine_channel& ch = m.e.chans[1];
    EXPECT_EQ(ch.module_state, ECUIO_MODULE_STATE_CLOSING);
    for (int i = 0; i < 3000 && ch.diag.clear != ECUIO_CLEAR_CLEARED; ++i) {
        m.step();
    }
    ASSERT_EQ(ch.diag.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(ch.module_state, ECUIO_MODULE_STATE_CLOSING)
        << "the read after it still to come";
    EXPECT_EQ(ch.diag.flags & ECUIO_DIAG_COMPLETE, 0u);
    m.until(m.now + 3000 * kMs);
    EXPECT_NE(ch.diag.flags & ECUIO_DIAG_COMPLETE, 0u);
    EXPECT_EQ(ch.module_state, ECUIO_MODULE_STATE_CLOSED);
}

/* A closed module never read has no report to clear */
TEST_F(ClearRequestTest, AClearOfAClosedNeverReadModuleIsReportChanged)
{
    static Clears m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ecuio_engine_module_want(&m.e, 1, 0);
    m.until(m.now + 100 * kMs);
    ASSERT_EQ(m.e.chans[1].module_state, ECUIO_MODULE_STATE_CLOSED);
    ecuio_engine_diagnose_again(&m.e);
    m.until(m.now + 1000 * kMs);
    ASSERT_EQ(m.e.chans[1].diag.flags & ECUIO_DIAG_COMPLETE, 0u)
        << "a closed module's read waits";
    m.sent.clear();
    EXPECT_FALSE(m.request({1}));
    EXPECT_EQ(m.clear_of(1), ECUIO_CLEAR_REPORT_CHANGED);
    m.until(m.now + 1000 * kMs);
    EXPECT_TRUE(m.senders(kClear).empty());
}

/* The call's refusals, before the worker sees anything: a NULL list, a
 * bad list, any other flag, a report not the caller's, and a second
 * request before the first has ended (the worker not started here) */
TEST_F(ClearRequestTest, ARequestRefusesABadTargetList)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    const ecuio_clear_target_t ok = {0, d.generation, {0, 0}};
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, nullptr, 1, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &ok, 0, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "count 0";
    std::vector<ecuio_clear_target_t> many(ECUIO_MAX_CHANNELS + 1, ok);
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, many.data(),
                                                  ECUIO_MAX_CHANNELS + 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "past the cap";
    const ecuio_clear_target_t twice[2] = {ok, ok};
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, twice, 2, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a channel twice";
    const ecuio_clear_target_t past = {1, d.generation, {0, 0}};
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &past, 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "past module_count";
    const ecuio_clear_target_t dirty = {0, d.generation, {0, 1}};
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &dirty, 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "reserved room not zero";
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &ok, 1, 0x4u),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a flag core doesn't know";
    const ecuio_clear_target_t old = {0, d.generation - 1, {0, 0}};
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &old, 1, 0),
              ECUIO_ERR_REPORT_CHANGED);

    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &ok, 1, 0), ECUIO_OK);
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &ok, 1, 0),
              ECUIO_ERR_IN_USE)
        << "the first not yet ended";
    EXPECT_EQ(ecuio_session_clear_diagnostics_with(s, 0, d.generation, 0),
              ECUIO_ERR_IN_USE)
        << "_with is a request too";
    ASSERT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_clear_diagnostics_set(s, &ok, 1, 0),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
}

/* Through the worker: the request ends, and the session takes the next */
TEST_F(ClearRequestTest, TheSessionTakesTheNextRequestOnceOneEnds)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    const ecuio_clear_target_t first = {0, d.generation, {0, 0}};
    ASSERT_EQ(ecuio_session_clear_diagnostics_set(s, &first, 1, 0), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_status_t again = ECUIO_ERR_IN_USE;
    EXPECT_TRUE(poll(
        [&] {
            if (ecuio_session_get_channel_diagnostics(s, 0, &d) != ECUIO_OK ||
                d.clear != ECUIO_CLEAR_CLEARED ||
                (d.flags & ECUIO_DIAG_COMPLETE) == 0) {
                return false;
            }
            const ecuio_clear_target_t next = {0, d.generation, {0, 0}};
            again = ecuio_session_clear_diagnostics_set(s, &next, 1, 0);
            return again != ECUIO_ERR_IN_USE;
        },
        3000));
    EXPECT_EQ(again, ECUIO_OK);
    ecuio_session_destroy(s);
}
