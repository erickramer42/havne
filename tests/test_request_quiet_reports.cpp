// File/layer: tests/test_request_quiet_reports.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestQuietTest (what a quiet module's waking does to
//          its session, and Read again and a clear beside its step's
//          diagnostics item, through the turns' adapter with the time
//          passed in)

#include "request_quiet_support.h"

using namespace request_quiet_test;

namespace {

/* A Pair under the extended session (tester present at its longest), the
 * engine ECU answering it; channel 1 answers as ecu1 says while awake */
std::unique_ptr<Pair>
in_session_pair(std::function<Bytes(const Bytes&)> ecu1 = in_session)
{
    auto p = std::make_unique<Pair>();
    EXPECT_TRUE(
        ecuio_engine_session(&p->a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MAX_US));
    Pair* self = p.get();
    p->a->ecus = [self, ecu1](uint32_t c, const Bytes& q) {
        return c == 0 ? in_session(q) : self->awake ? ecu1(q) : Bytes{};
    };
    return p;
}

/* Passes until channel 1 is quiet */
void to_quiet(Adapter& a)
{
    for (int i = 0; i < 100000 && !a.e.chans[1].quiet; ++i) {
        a.pass();
    }
    ASSERT_TRUE(a.e.chans[1].quiet);
}

/* Channel 1's requests from the first sent at or after from_us */
std::vector<Bytes> asked_from(const Adapter& a, int64_t from_us)
{
    std::vector<Bytes> out;
    for (const Sent& s : a.sent) {
        if (s.c == 1 && s.at_us >= from_us) {
            out.push_back(s.bytes);
        }
    }
    return out;
}

size_t entries_asked(const Adapter& a)
{
    size_t n = 0;
    for (const Sent& s : a.sent) {
        n += s.c == 1 && s.bytes == Bytes{0x10, 0x03} ? 1 : 0;
    }
    return n;
}

} // namespace

/* A module silent from the connect: its 10 03 goes unanswered, it goes
 * quiet, then wakes. Its waking reply is the step's row; its next request
 * is 10 03, and it reads entered. Without rule 5's session it read in
 * default until a recovery, its extended-only DIDs refused. */
TEST(RequestQuietTest, AModuleWakingAfterItsSessionWentUnansweredEntersItAgain)
{
    auto p = in_session_pair();
    to_quiet(*p->a);
    ASSERT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_UNANSWERED);
    const int64_t woke = p->a->now;
    p->awake = true;
    p->a->until(woke + 10000 * kMs);
    const std::vector<Bytes> after = asked_from(*p->a, woke);
    ASSERT_GE(after.size(), 2u);
    EXPECT_EQ(after[0][0], 0x22) << "the step's row wakes it";
    EXPECT_EQ(after[1], (Bytes{0x10, 0x03}));
    EXPECT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(p->a->e.chans[1].session_entries, 1u);
    EXPECT_EQ(p->a->e.chans[1].session_lost, 0u);
}

/* A module that refused 10 03 reads in default; going quiet and waking
 * asks no 10 03 again */
TEST(RequestQuietTest, AWakeLeavesARefusedSessionInDefault)
{
    auto p = in_session_pair([](const Bytes& q) {
        return q.size() == 2 && q[0] == 0x10 ? Bytes{0x7F, 0x10, 0x22}
                                             : answer(q);
    });
    p->awake = true;
    p->a->until(2000 * kMs);
    ASSERT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED);
    p->awake = false;
    to_quiet(*p->a);
    p->awake = true;
    p->a->until(p->a->now + 10000 * kMs);
    EXPECT_FALSE(p->a->e.chans[1].quiet);
    EXPECT_EQ(entries_asked(*p->a), 1u);
    EXPECT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED);
}

/* The wake's re-entry moves the channel's generation, so the report
 * shows the session entering at once */
TEST(RequestQuietTest, AWakeThatEntersTheSessionAgainMovesTheGeneration)
{
    auto p = in_session_pair();
    to_quiet(*p->a);
    const ecuio_engine_channel& ch = p->a->e.chans[1];
    ASSERT_EQ(ch.session_step, ECUIO_SESSION_STATE_UNANSWERED);
    const uint32_t g = ch.generation;
    const Bytes stray = {0x7F, 0x22, 0x7F};
    (void)ecuio_engine_on_frame(&p->a->e, 1, stray.data(),
                                static_cast<uint16_t>(stray.size()), p->a->now,
                                nullptr);
    EXPECT_EQ(ch.session_step, ECUIO_SESSION_STATE_ENTERING);
    EXPECT_EQ(ch.generation, g + 1);
}

/* A module quiet while its session reads entered, heard again within S3,
 * wakes into the same session: no 10 03, no loss */
TEST(RequestQuietTest, AWakeKeepsAnEnteredSession)
{
    auto p = in_session_pair();
    p->awake = true;
    p->a->until(2000 * kMs);
    ASSERT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    p->awake = false;
    to_quiet(*p->a);
    ASSERT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    const int64_t woke = p->a->now;
    ASSERT_LT(woke + 1000 * kMs - p->a->e.chans[1].heard_us, ECUIO_ENGINE_S3_US)
        << "its next step falls within S3";
    p->awake = true;
    p->a->until(woke + 10000 * kMs);
    EXPECT_FALSE(p->a->e.chans[1].quiet);
    EXPECT_EQ(entries_asked(*p->a), 1u);
    EXPECT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(p->a->e.chans[1].session_lost, 0u);
}

/* A module that resets (a crank brown-out) while its session reads
 * entered, silent past S3, wakes in default and answers each
 * extended-only DID 7F 22 7F. Out of step that re-enters nothing, so
 * the wake enters the session again (one loss) and its readings return. */
TEST(RequestQuietTest, AModuleWakingPastS3InDefaultIsEnteredAgain)
{
    auto extended = std::make_shared<bool>(false);
    auto answered = std::make_shared<int>(0);
    auto p = in_session_pair([extended, answered](const Bytes& q) {
        if (q.size() == 2 && q[0] == 0x10) {
            *extended = true;
            return in_session(q);
        }
        if (q.size() == 3 && q[0] == 0x22 && !*extended) {
            return Bytes{0x7F, 0x22, 0x7F};
        }
        *answered += q[0] == 0x22 ? 1 : 0;
        return in_session(q);
    });
    p->awake = true;
    p->a->until(2000 * kMs);
    ASSERT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    p->awake = false;
    *extended = false;
    const ecuio_engine_channel& ch = p->a->e.chans[1];
    for (int i = 0; i < 100000 && !(ch.quiet && p->a->now - ch.heard_us >=
                                                    ECUIO_ENGINE_S3_US);
         ++i) {
        p->a->pass();
    }
    ASSERT_TRUE(ch.quiet);
    ASSERT_EQ(ch.session_step, ECUIO_SESSION_STATE_ENTERED)
        << "the quiet steps outrun tester present";
    const int64_t woke = p->a->now;
    *answered = 0;
    p->awake = true;
    p->a->until(woke + 10000 * kMs);
    const std::vector<Bytes> after = asked_from(*p->a, woke);
    ASSERT_GE(after.size(), 2u);
    EXPECT_EQ(after[0][0], 0x22) << "the step's row wakes it";
    EXPECT_EQ(after[1], (Bytes{0x10, 0x03}));
    EXPECT_EQ(ch.session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(ch.session_entries, 2u);
    EXPECT_EQ(ch.session_lost, 1u);
    EXPECT_GT(*answered, 0) << "its readings answered again";
}

namespace {

/* The engine ECU's 4 rows on channel 0, answered in 30 ms; on channel 1 a
 * module with no rows that never answers; diagnostics on both */
std::unique_ptr<Adapter> quiet_reports()
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    a->exchange_us = 30 * kMs;
    a->ecus = [](uint32_t c, const Bytes& q) {
        return c == 0 ? answer(q) : Bytes{};
    };
    ecuio_engine_diagnose(&a->e, nullptr);
    return a;
}

/* Passes until channel 1, quiet with its report settled, has a step's
 * diagnostics item out, channel 0's report settled too */
void to_step_item_out(Adapter& a)
{
    const ecuio_engine_channel& ch = a.e.chans[1];
    for (int i = 0; i < 100000 && !(ch.quiet && ch.diagnosing &&
                                    ch.outstanding != ECUIO_ENGINE_NONE);
         ++i) {
        a.pass();
    }
    ASSERT_TRUE(ch.diagnosing && ch.outstanding != ECUIO_ENGINE_NONE);
    ASSERT_TRUE(ch.diag_step);
    ASSERT_NE(ch.diag.flags & ECUIO_DIAG_COMPLETE, 0u);
    ASSERT_NE(a.e.chans[0].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
}

/* A positive answer to the diagnostics item q, padded to six bytes */
Bytes diag_answer(const ecuio_request& q)
{
    Bytes r(q.bytes, q.bytes + q.len);
    r[0] = static_cast<uint8_t>(r[0] + 0x40);
    r.resize(6, 0x00);
    return r;
}

/* Channel 1's requests from its nth on */
std::vector<Bytes> asked_after(const Adapter& a, size_t n)
{
    std::vector<Bytes> out;
    for (size_t i = n; i < a.sent.size(); ++i) {
        if (a.sent[i].c == 1) {
            out.push_back(a.sent[i].bytes);
        }
    }
    return out;
}

} // namespace

/* Read again pressed while a quiet module's step asks an item
 * of its settled report. The live channel starts over at once; the quiet
 * one at the item's settle (its generation moves), so the press is never
 * lost and the item's reply settles no slot of the new list. */
TEST(RequestQuietTest,
     AReadAgainWhileAQuietModulesItemIsOutRestartsItAtTheSettle)
{
    auto a = quiet_reports();
    to_step_item_out(*a);
    const uint32_t g0 = a->e.chans[0].diag.generation;
    const uint32_t g1 = a->e.chans[1].diag.generation;
    ecuio_engine_diagnose_again(&a->e);
    EXPECT_EQ(a->e.chans[0].diag.generation, g0 + 1);
    EXPECT_EQ(a->e.chans[1].diag.generation, g1) << "not while its item is out";
    a->until(a->now + ECUIO_ENGINE_WAIT_US);
    EXPECT_GT(a->e.chans[1].diag.generation, g1) << "started over";
    EXPECT_FALSE(a->e.chans[1].diag_again);
    a->until(a->now + 1000 * kMs);
    EXPECT_NE(a->e.chans[1].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
}

/* Read again pressed while the step's item is out, then the module wakes
 * and answers it: the read still starts over at the item's settle */
TEST(RequestQuietTest, AReadAgainWaitingOnTheStepsItemStartsOverWhenItAnswers)
{
    auto a = quiet_reports();
    to_step_item_out(*a);
    ecuio_engine_diagnose_again(&a->e);
    ASSERT_TRUE(a->e.chans[1].diag_again);
    const Bytes r = diag_answer(a->e.chans[1].once);
    (void)ecuio_engine_on_frame(
        &a->e, 1, r.data(), static_cast<uint16_t>(r.size()), a->now, nullptr);
    const ecuio_engine_channel& ch = a->e.chans[1];
    EXPECT_FALSE(ch.diag_again);
    EXPECT_EQ(ch.diag.flags & ECUIO_DIAG_COMPLETE, 0u) << "a new read";
    EXPECT_GT(ch.diag_pending, 0u) << "its items listed again";
}

/* A clear at the report's generation while the step's item is
 * out is accepted; its first request goes once the item has settled
 * unanswered again */
TEST(RequestQuietTest,
     AClearAskedWhileAQuietModulesItemIsOutIsSentAfterTheSettle)
{
    auto a = quiet_reports();
    to_step_item_out(*a);
    const size_t n = a->sent.size();
    ASSERT_TRUE(ecuio_engine_clear(&a->e, 1, a->e.chans[1].diag.generation));
    a->until(a->now + 2000 * kMs);
    const std::vector<Bytes> after = asked_after(*a, n);
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(after[0], (Bytes{0x01, 0x0C}));
    EXPECT_EQ(a->e.chans[1].clear_step, ECUIO_CLEAR_STEP_NONE);
    EXPECT_EQ(a->e.chans[1].diag.clear, ECUIO_CLEAR_NO_ENGINE_SPEED);
}

/* A clear waiting on the step's item when the module wakes and answers
 * it: the report the user agreed to is gone, so the clear ends
 * REPORT_CHANGED with nothing of it sent */
TEST(RequestQuietTest, AClearWaitingOnAQuietModulesItemEndsChangedWhenItAnswers)
{
    auto a = quiet_reports();
    to_step_item_out(*a);
    const size_t n = a->sent.size();
    ASSERT_TRUE(ecuio_engine_clear(&a->e, 1, a->e.chans[1].diag.generation));
    const Bytes r = diag_answer(a->e.chans[1].once);
    (void)ecuio_engine_on_frame(
        &a->e, 1, r.data(), static_cast<uint16_t>(r.size()), a->now, nullptr);
    const ecuio_engine_channel& ch = a->e.chans[1];
    EXPECT_EQ(ch.diag.clear, ECUIO_CLEAR_REPORT_CHANGED);
    EXPECT_EQ(ch.clear_step, ECUIO_CLEAR_STEP_NONE);
    EXPECT_EQ(ch.diag.flags & ECUIO_DIAG_CLEARING, 0u);
    a->until(a->now + 10000 * kMs);
    for (const Bytes& q : asked_after(*a, n)) {
        EXPECT_NE(q, (Bytes{0x01, 0x0C}));
        EXPECT_NE(q, (Bytes{0x04}));
    }
}

/* A Read again waiting on the step's item restarts the report at its
 * settle, so a clear asked after it is refused as changed */
TEST(RequestQuietTest, AClearAfterAReadAgainWaitingOnTheStepsItemIsRefused)
{
    auto a = quiet_reports();
    to_step_item_out(*a);
    const uint32_t g = a->e.chans[1].diag.generation;
    ecuio_engine_diagnose_again(&a->e);
    EXPECT_FALSE(ecuio_engine_clear(&a->e, 1, g));
    EXPECT_EQ(a->e.chans[1].diag.clear, ECUIO_CLEAR_REPORT_CHANGED);
    EXPECT_EQ(a->e.chans[1].clear_step, ECUIO_CLEAR_STEP_NONE);
}
