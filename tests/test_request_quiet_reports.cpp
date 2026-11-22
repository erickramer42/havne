// File/layer: tests/test_request_quiet_reports.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestQuietTest (what a quiet module's waking does to
//          its session, request_engine_quiet.h's rule 5, through the
//          turns' adapter with the time passed in)

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

/* A module quiet while its session reads entered (silent for less than
 * tester present's run) wakes into the same session: no 10 03, no loss.
 * One that reset would answer 7F xx 7F, and that re-enters. */
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
    p->awake = true;
    p->a->until(woke + 10000 * kMs);
    EXPECT_FALSE(p->a->e.chans[1].quiet);
    EXPECT_EQ(entries_asked(*p->a), 1u);
    EXPECT_EQ(p->a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(p->a->e.chans[1].session_lost, 0u);
}
