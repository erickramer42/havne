// File/layer: tests/test_request_modules.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestModulesTest (a module open while a reason holds it,
//          closing once what it began settles, and what a reopen keeps:
//          request_engine_modules.h; ADR-0065 step 5), RequestQuietTest's and
//          RequestTurnsTest's closed-module cases

#include "test_obd2_support.h"

#include "../core/src/request_turns.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace {

constexpr uint8_t kRange = ECUIO_ENGINE_REASON_RANGE;
constexpr uint8_t kRead = ECUIO_ENGINE_REASON_READ;

/* What a channel sent, and when */
struct Sent {
    uint32_t c;
    int64_t at;
    Bytes bytes;
};

/* Channels driven side by side with the time passed in, 1 ms a step: each
 * wait ended, each reply due handed over, each channel's due request
 * sent and answered through respond 4 ms later ({}: silence), and the
 * modules' closes settled */
struct Modules {
    ecuio_request_engine e;
    int64_t now = 0;
    std::vector<Sent> sent;
    std::vector<int64_t> reply_at;
    std::vector<Bytes> reply;
    std::function<Bytes(uint32_t, const Bytes&)> respond =
        [](uint32_t, const Bytes& q) { return fake_reply(q); };

    void make(uint32_t chans)
    {
        ASSERT_TRUE(ecuio_engine_init(&e, chans));
        ecuio_engine_slots(&e, ECUIO_MAX_CHANNELS, ECUIO_MAX_CHANNELS);
        reply_at.assign(chans, INT64_MAX);
        reply.assign(chans, {});
    }

    void row(uint32_t c, uint8_t pid)
    {
        const uint8_t q[2] = {0x01, pid};
        ASSERT_EQ(ecuio_engine_add(&e, c, q, 2),
                  ecuio_engine_add_result::added);
    }

    void step()
    {
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (reply_at[c] <= now) {
                reply_at[c] = INT64_MAX;
                (void)ecuio_engine_on_frame(&e, c, reply[c].data(),
                                            static_cast<uint16_t>(
                                                reply[c].size()),
                                            now, nullptr);
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
            sent.push_back({c, now, b});
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

    size_t sent_on(uint32_t c, int64_t from_us = 0) const
    {
        size_t n = 0;
        for (const Sent& s : sent) {
            n += s.c == c && s.at >= from_us ? 1u : 0u;
        }
        return n;
    }

    uint8_t state(uint32_t c) const { return e.chans[c].module_state; }
};

/* Two channels, each a row, identification, diagnostics, a scanned DID,
 * the walk and the extended session */
void everything(Modules& m)
{
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    ASSERT_NO_FATAL_FAILURE(m.row(1, 0x0D));
    ASSERT_TRUE(ecuio_engine_scan_add(&m.e, 1, 0xF190));
    ecuio_engine_identify(&m.e);
    ecuio_engine_diagnose(&m.e, ecuio_obd2_signal_set());
    ecuio_engine_probe(&m.e);
    ASSERT_TRUE(ecuio_engine_session(&m.e, ECUIO_SESSION_EXTENDED,
                                      ECUIO_ENGINE_KEEPALIVE_US));
}

} // namespace

TEST(RequestModulesTest, EveryModuleStartsOpenForRangeAndRead)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(3));
    for (uint32_t c = 0; c < 3; ++c) {
        EXPECT_EQ(m.state(c), ECUIO_MODULE_STATE_OPEN);
        EXPECT_EQ(m.e.chans[c].reasons, kRange | kRead);
    }
}

/* Nothing at all goes to a closed module: no row, identification,
 * diagnostics, walk, scanned DID, session entry, keep-alive or check
 * (the diagnostics read queued when it closed runs to its end first) */
TEST(RequestModulesTest, AClosedModuleIsSentNothing)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(everything(m));
    ecuio_engine_module_want(&m.e, 1, 0);
    EXPECT_EQ(m.state(1), ECUIO_MODULE_STATE_CLOSING);
    m.until(2000 * kMs);
    ASSERT_EQ(m.state(1), ECUIO_MODULE_STATE_CLOSED);
    const int64_t from = m.now;
    m.until(from + 10000 * kMs);
    EXPECT_EQ(m.sent_on(1, from), 0u);
    EXPECT_GT(m.sent_on(0), 100u) << "the open module reads on";
    EXPECT_FALSE(ecuio_engine_ask_check(&m.e, 1));
    EXPECT_EQ(ecuio_engine_turn_next_us(&m.e, m.e.chans[1]), INT64_MAX);
}

TEST(RequestModulesTest, AClosedModuleHasNoNextEvent)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    EXPECT_EQ(ecuio_engine_next_event_us(&m.e, 0), 0);
    ecuio_engine_module_want(&m.e, 0, 0);
    (void)ecuio_engine_modules_settle(&m.e, 0);
    EXPECT_EQ(ecuio_engine_next_event_us(&m.e, 0), INT64_MAX);
}

/* A closed module's frame is no reply: it answers nothing and marks the
 * module neither answering nor heard */
TEST(RequestModulesTest, AClosedModulesFrameIsNotTaken)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_NO_FATAL_FAILURE(m.row(1, 0x0C));
    ecuio_engine_module_want(&m.e, 1, 0);
    (void)ecuio_engine_modules_settle(&m.e, 0);
    const Bytes rpm = {0x41, 0x0C, 0x1A, 0xF8};
    EXPECT_EQ(ecuio_engine_on_frame(&m.e, 1, rpm.data(), 4, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(m.e.answering & (1u << 1), 0u);
    EXPECT_FALSE(m.e.chans[1].heard);
}

TEST(RequestModulesTest, AModuleClosesOnlyOnceItsRequestSettles)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    const int32_t r = ecuio_engine_due(&m.e, 0, 0);
    ASSERT_NE(r, ECUIO_ENGINE_NONE);
    ecuio_engine_sent(&m.e, 0, r, true, 0);
    ecuio_engine_module_want(&m.e, 0, 0);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSING);
    EXPECT_EQ(ecuio_engine_modules_settle(&m.e, 2 * kMs), 0u);
    EXPECT_EQ(ecuio_engine_due(&m.e, 0, 50 * kMs), ECUIO_ENGINE_NONE)
        << "a closing module sends nothing new";
    const Bytes rpm = {0x41, 0x0C, 0x1A, 0xF8};
    EXPECT_EQ(ecuio_engine_on_frame(&m.e, 0, rpm.data(), 4, 5 * kMs, nullptr),
              ecuio_reply::positive)
        << "its reply is still taken";
    EXPECT_EQ(ecuio_engine_modules_settle(&m.e, 6 * kMs), 1u);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
}

TEST(RequestModulesTest, ReasonsAddUpAndEachDropClosesOnlyTheLast)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    ecuio_engine_module_want(&m.e, 0, kRange);
    (void)ecuio_engine_modules_settle(&m.e, 0);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_OPEN) << "RANGE still holds it";
    ecuio_engine_module_want(&m.e, 0, 0);
    (void)ecuio_engine_modules_settle(&m.e, 0);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
    ecuio_engine_module_want(&m.e, 0, kRead);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_OPEN);
}

/* A diagnostics read under way when the last reason drops runs to its
 * end: its items still go, and the module closes after */
TEST(RequestModulesTest, AModuleStaysOpenWhileADiagnosticsReadIsPending)
{
    fake_ecu::reset_diagnostics();
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    ecuio_engine_diagnose(&m.e, ecuio_obd2_signal_set());
    m.until(10 * kMs);
    ASSERT_GT(m.e.chans[0].diag_pending, 0u);
    ecuio_engine_module_want(&m.e, 0, 0);
    const int64_t from = m.now;
    m.until(2000 * kMs);
    EXPECT_GT(m.sent_on(0, from), 2u) << "the read went on";
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&m.e, 0, &d);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
}

/* A clear holds its module through its steps and the read after it (44,
 * then the list again): closed only once that read is complete */
TEST(RequestModulesTest, AModuleStaysOpenWhileItsClearRuns)
{
    fake_ecu::reset_diagnostics();
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    m.respond = [](uint32_t, const Bytes& q) {
        return q == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x00, 0x00}
                                      : fake_reply(q);
    };
    ecuio_engine_diagnose(&m.e, ecuio_obd2_signal_set());
    m.until(2000 * kMs);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&m.e, 0, &d);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    m.e.rpm_module = 0; /* the engine module */
    ASSERT_TRUE(ecuio_engine_clear(&m.e, 0, d.generation));
    ecuio_engine_module_want(&m.e, 0, 0);
    bool closed_mid_clear = false;
    bool cleared = false;
    while (m.now < 6000 * kMs) {
        m.step();
        ecuio_engine_diagnostics(&m.e, 0, &d);
        cleared = cleared || d.clear == ECUIO_CLEAR_CLEARED;
        const bool reading = (d.flags & ECUIO_DIAG_COMPLETE) == 0;
        closed_mid_clear = closed_mid_clear ||
                           (m.state(0) == ECUIO_MODULE_STATE_CLOSED &&
                            (reading || !cleared));
    }
    EXPECT_TRUE(cleared);
    EXPECT_FALSE(closed_mid_clear) << "closed before the read after the clear";
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
}

/* A reopen asks only what was never answered: identification answered
 * stays, an item left unanswered is asked again, and a complete
 * diagnostics report keeps its generation (a clear saved against it
 * still goes) */
TEST(RequestModulesTest, AReopenedModuleKeepsWhatItLearnedAndAReportsGeneration)
{
    fake_ecu::reset_diagnostics();
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    const Bytes calid = {0x09, 0x04};
    m.respond = [&calid](uint32_t, const Bytes& q) {
        return q == calid ? Bytes{} : fake_reply(q);
    };
    ecuio_engine_identify(&m.e);
    ecuio_engine_diagnose(&m.e, ecuio_obd2_signal_set());
    m.until(3000 * kMs);
    ecuio_channel_diagnostics_t before;
    ecuio_engine_diagnostics(&m.e, 0, &before);
    ASSERT_TRUE(before.flags & ECUIO_DIAG_COMPLETE);
    ecuio_engine_module_want(&m.e, 0, 0);
    m.until(m.now + 100 * kMs);
    ASSERT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
    const size_t asked = m.sent.size();
    ecuio_engine_module_want(&m.e, 0, kRead);
    m.until(m.now + 1000 * kMs);
    std::vector<Bytes> again;
    for (size_t i = asked; i < m.sent.size(); ++i) {
        again.push_back(m.sent[i].bytes);
    }
    EXPECT_EQ(again, std::vector<Bytes>{calid})
        << "only the unanswered item is asked again";
    ecuio_channel_diagnostics_t after;
    ecuio_engine_diagnostics(&m.e, 0, &after);
    EXPECT_EQ(after.generation, before.generation);
    EXPECT_TRUE(after.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_TRUE(ecuio_engine_clear(&m.e, 0, before.generation));
}

/* A module closing in its session leaves it (10 01) before it closes; a
 * reopen enters it again only for a read (L10 makes the entry follow
 * READ), never for RANGE alone */
TEST(RequestModulesTest, AClosingModuleLeavesItsSessionAndOnlyAReadEntersIt)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(1));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    m.respond = [](uint32_t, const Bytes& q) {
        if (q[0] == 0x10) {
            return Bytes{0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
        }
        return fake_reply(q);
    };
    ASSERT_TRUE(ecuio_engine_session(&m.e, ECUIO_SESSION_EXTENDED,
                                      ECUIO_ENGINE_KEEPALIVE_US));
    m.until(200 * kMs);
    ASSERT_EQ(m.e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ecuio_engine_module_want(&m.e, 0, 0);
    const size_t at = m.sent.size();
    m.until(m.now + 200 * kMs);
    ASSERT_EQ(m.sent.size(), at + 1);
    EXPECT_EQ(m.sent[at].bytes, (Bytes{0x10, 0x01}));
    EXPECT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
    ecuio_engine_module_want(&m.e, 0, kRange);
    EXPECT_EQ(m.e.chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    ecuio_engine_module_want(&m.e, 0, 0);
    m.until(m.now + 200 * kMs);
    ASSERT_EQ(m.state(0), ECUIO_MODULE_STATE_CLOSED);
    ecuio_engine_module_want(&m.e, 0, kRange | kRead);
    EXPECT_EQ(m.e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
}

/* A closed module isn't live: a quiet one beside it isn't held */
TEST(RequestQuietTest, AClosedModuleIsNotLive)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    ASSERT_NO_FATAL_FAILURE(m.row(1, 0x0D));
    m.e.chans[0].quiet = true;
    EXPECT_TRUE(ecuio_engine_quiet_held(&m.e, m.e.chans[0]));
    ecuio_engine_module_want(&m.e, 1, 0);
    (void)ecuio_engine_modules_settle(&m.e, 0);
    EXPECT_FALSE(ecuio_engine_quiet_held(&m.e, m.e.chans[0]));
}

/* The turns give a closed module no time */
TEST(RequestTurnsTest, TheTurnsPassAClosedModuleBy)
{
    Modules m;
    ASSERT_NO_FATAL_FAILURE(m.make(2));
    ASSERT_NO_FATAL_FAILURE(m.row(0, 0x0C));
    ASSERT_NO_FATAL_FAILURE(m.row(1, 0x0D));
    ecuio_engine_module_want(&m.e, 0, 0);
    (void)ecuio_engine_modules_settle(&m.e, 0);
    ecuio_engine_turns t;
    int64_t now = 0;
    for (int i = 0; i < 20; ++i) {
        uint32_t c = 99;
        int64_t wait_us = 0;
        const int32_t r = ecuio_engine_take_turn(&m.e, t, 0, now, &c, &wait_us);
        if (r != ECUIO_ENGINE_NONE) {
            EXPECT_EQ(c, 1u);
            ecuio_engine_sent(&m.e, c, r, true, now);
            const Bytes b = {0x41, 0x0D, 0x00};
            (void)ecuio_engine_on_frame(&m.e, c, b.data(), 3, now + 4 * kMs,
                                        nullptr);
        }
        now += ECUIO_ENGINE_GAP_US;
    }
}
