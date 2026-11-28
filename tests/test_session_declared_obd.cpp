#include <gtest/gtest.h>

#include "../core/src/request_engine.h"
#include "../core/src/request_turns.h"

#include <cstdint>
#include <set>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;
constexpr uint8_t kRange = ECUIO_ENGINE_REASON_RANGE;
constexpr uint8_t kRead = ECUIO_ENGINE_REASON_READ;
const Bytes kEnter = {0x10, 0x03};
const Bytes kLeave = {0x10, 0x01};
const Bytes kRow = {0x22, 0x1E, 0x1D};
const Bytes kClear = {0x04};

bool obd(const Bytes& q)
{
    return !q.empty() && q[0] >= 0x01 && q[0] <= 0x0A;
}

/* Its DIDs answer only in the extended session */
struct Ecu {
    struct Heard {
        Bytes q;
        bool inside;
    };
    std::vector<Heard> heard;
    std::set<Bytes> silent;
    bool in = false;
    int refusals = 0;
    int silences = 0;

    Bytes operator()(const Bytes& q)
    {
        heard.push_back({q, in});
        if (silent.count(q) != 0) {
            return {};
        }
        if (q == kEnter) {
            if (silences > 0) {
                --silences;
                return {};
            }
            if (refusals > 0) {
                --refusals;
                return {0x7F, 0x10, 0x22};
            }
            in = true;
            return {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == kLeave) {
            in = false;
            return {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == Bytes{0x3E, 0x00}) {
            return {0x7E, 0x00};
        }
        if (q.size() == 3 && q[0] == 0x22) {
            if (q[1] == 0xF1) {
                return {0x62, q[1], q[2], 0x41};
            }
            return in ? Bytes{0x62, q[1], q[2], 0x12} : Bytes{0x7F, 0x22, 0x31};
        }
        if (q == Bytes{0x01, 0x0C}) {
            return {0x41, 0x0C, 0x00, 0x00};
        }
        if (q == Bytes{0x03}) {
            return {0x43, 0x00};
        }
        if (q == kClear) {
            return {0x44};
        }
        return {0x7F, q[0], 0x11};
    }

    size_t at(const Bytes& want, size_t from = 0) const
    {
        for (size_t i = from; i < heard.size(); ++i) {
            if (heard[i].q == want) {
                return i;
            }
        }
        return SIZE_MAX;
    }

    size_t count(const Bytes& want) const
    {
        size_t n = 0;
        for (const Heard& h : heard) {
            n += h.q == want ? 1 : 0;
        }
        return n;
    }

    size_t obd_inside() const
    {
        size_t n = 0;
        for (const Heard& h : heard) {
            n += h.inside && obd(h.q) ? 1 : 0;
        }
        return n;
    }

    size_t last_obd() const
    {
        size_t last = SIZE_MAX;
        for (size_t i = 0; i < heard.size(); ++i) {
            last = obd(heard[i].q) ? i : last;
        }
        return last;
    }
};

void step(ecuio_request_engine* e, uint32_t c, int64_t* now, Ecu* ecu)
{
    const int32_t r = ecuio_engine_due(e, c, *now);
    if (r == ECUIO_ENGINE_NONE) {
        *now += ECUIO_ENGINE_GAP_US;
        return;
    }
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    const Bytes sent(q.bytes, q.bytes + q.len);
    ecuio_engine_sent(e, c, r, true, *now);
    const Bytes reply = (*ecu)(sent);
    if (reply.empty()) {
        for (int i = 0; i < 100 && e->chans[c].outstanding != ECUIO_ENGINE_NONE;
             ++i) {
            *now += 10 * kMs;
            (void)ecuio_engine_expire(e, c, *now);
        }
    } else {
        *now += 4 * kMs;
        (void)ecuio_engine_on_frame(e, c, reply.data(),
                                    static_cast<uint16_t>(reply.size()), *now,
                                    nullptr);
    }
    *now += ECUIO_ENGINE_GAP_US;
}

void want(ecuio_request_engine* e, uint8_t r0, uint8_t r1)
{
    const uint8_t reasons[2] = {r0, r1};
    ecuio_engine_modules_apply(e, reasons, nullptr, 0);
}

/* Channel 0 the engine module, channel 1 declaring the extended session */
struct Bench {
    ecuio_request_engine* e;
    int64_t now = 0;
    Ecu engine;
    Ecu held;

    explicit Bench(ecuio_request_engine* engine_, bool diagnose = true)
        : e(engine_)
    {
        EXPECT_TRUE(ecuio_engine_init(e, 2));
        const uint8_t rpm[2] = {0x01, 0x0C};
        EXPECT_EQ(ecuio_engine_add(e, 0, rpm, 2),
                  ecuio_engine_add_result::added);
        EXPECT_EQ(ecuio_engine_add(e, 1, kRow.data(), 3),
                  ecuio_engine_add_result::added);
        e->rpm_module = 0;
        ecuio_engine_identify(e);
        if (diagnose) {
            ecuio_engine_diagnose(e, nullptr);
        }
        e->chans[1].session_may_declare = true;
        ecuio_engine_session_allow(e, 1, true);
        want(e, kRange | kRead, kRange | kRead);
    }

    void run(int steps)
    {
        for (int i = 0; i < steps; ++i) {
            step(e, 0, &now, &engine);
            step(e, 1, &now, &held);
        }
    }

    void run_until(bool (*done)(const ecuio_engine_channel&), int most = 400)
    {
        for (int i = 0; i < most && !done(e->chans[1]); ++i) {
            run(1);
        }
    }

    void entered()
    {
        run_until([](const ecuio_engine_channel& ch) {
            return ch.session_step == ECUIO_SESSION_STATE_ENTERED &&
                   ch.diag_pending == 0 && ch.ident_pending == 0;
        });
        ASSERT_EQ(e->chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
        held.heard.clear();
    }

    uint8_t idle() const
    {
        ecuio_channel_support_t s;
        ecuio_engine_support(e, 1, &s);
        return s.session_idle;
    }

    void clear()
    {
        ASSERT_TRUE(ecuio_engine_clear(e, 1, e->chans[1].diag.generation, 0));
        run(1);
        ASSERT_TRUE(e->chans[1].obd_out);
    }
};

bool back_in(const ecuio_engine_channel& ch)
{
    return ch.session_step == ECUIO_SESSION_STATE_ENTERED && !ch.obd_out &&
           ch.diag_pending == 0;
}

} // namespace

TEST(DeclaredSessionTest, TheOpensObdItemsGoBeforeTheEntry)
{
    for (const bool diagnose : {true, false}) {
        SCOPED_TRACE(diagnose ? "both lists" : "identification alone");
        static ecuio_request_engine e;
        Bench b(&e, diagnose);
        b.run(200);
        const size_t entry = b.held.at(kEnter);
        ASSERT_NE(entry, SIZE_MAX);
        EXPECT_GT(entry, b.held.last_obd());
        EXPECT_GT(b.held.at(kRow), entry) << "its rows wait for the entry";
        EXPECT_EQ(b.held.count(kEnter), 1u);
        EXPECT_EQ(b.held.obd_inside(), 0u);
    }
}

TEST(DeclaredSessionTest, AReopenedModulesItemsGoBeforeItsEntry)
{
    static ecuio_request_engine e;
    Bench b(&e);
    b.held.silent = {{0x09, 0x02}};
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.silent.clear();
    want(&e, kRange | kRead, 0);
    b.run(20);
    ASSERT_EQ(ecuio_engine_modules_settle(&e, b.now), 2u);
    b.held.heard.clear();
    want(&e, kRange | kRead, kRange | kRead);
    b.run(40);
    const size_t vin = b.held.at({0x09, 0x02});
    ASSERT_NE(vin, SIZE_MAX);
    EXPECT_LT(vin, b.held.at(kEnter));
    EXPECT_EQ(b.held.obd_inside(), 0u);
}

TEST(DeclaredSessionTest, AReadAgainOnAHeldModuleLeavesTheSessionFirst)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_diagnose_again(&e);
    b.run(1);
    ASSERT_FALSE(b.held.heard.empty());
    EXPECT_EQ(b.held.heard.front().q, kLeave);
    EXPECT_EQ(b.idle(), ECUIO_SESSION_IDLE_OBD_ITEM);
    b.run_until(back_in);
    EXPECT_EQ(b.held.obd_inside(), 0u);
    EXPECT_EQ(b.held.count(kLeave), 1u);
    EXPECT_EQ(b.idle(), ECUIO_SESSION_IDLE_NONE);
}

TEST(DeclaredSessionTest, ReEntryWaitsForEveryObdItemToSettle)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_diagnose_again(&e);
    b.run_until(back_in);
    const size_t entry = b.held.at(kEnter);
    ASSERT_NE(entry, SIZE_MAX);
    EXPECT_GT(entry, b.held.last_obd());
    EXPECT_GE(b.held.count({0x03}), 1u);
    EXPECT_GE(b.held.count({0x0A}), 1u);
    EXPECT_EQ(b.held.count(kEnter), 1u);
    EXPECT_NE(e.chans[1].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
}

TEST(DeclaredSessionTest, ARescansAndAWakesObdItemsWaitForTheNextPassOut)
{
    static ecuio_request_engine e;
    Bench b(&e);
    b.held.silent = {{0x09, 0x02}, {0x22, 0xF1, 0x90}};
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.silent.clear();

    ecuio_engine_rescan(&e);
    b.run(40);
    EXPECT_EQ(b.held.count(kLeave), 0u) << "a re-ask forces no leave";
    EXPECT_EQ(b.held.count({0x09, 0x02}), 0u);
    EXPECT_EQ(b.held.count({0x22, 0xF1, 0x90}), 1u) << "UDS stays inside";
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[1].ident[0].outcome, ECUIO_IDENTITY_UNASKED);

    e.chans[1].ident[0].outcome = ECUIO_IDENTITY_UNANSWERED;
    e.chans[1].ident_pending = 0;
    e.chans[1].quiet = true;
    e.chans[1].quiet_settled = true;
    ecuio_engine_quiet_heard(&e, 1, b.now);
    b.run(40);
    EXPECT_EQ(b.held.count(kLeave), 0u) << "nor a wake's";
    EXPECT_EQ(b.held.count({0x09, 0x02}), 0u);

    want(&e, kRange | kRead, kRange);
    b.run(40);
    EXPECT_EQ(b.held.count(kLeave), 1u);
    ASSERT_NE(b.held.at({0x09, 0x02}), SIZE_MAX);
    EXPECT_GT(b.held.at({0x09, 0x02}), b.held.at(kLeave))
        << "asked once the last signal left";
    EXPECT_EQ(b.held.obd_inside(), 0u);
}

TEST(DeclaredSessionTest, AReadAgainTakesTheWaitingItemsOut)
{
    static ecuio_request_engine e;
    Bench b(&e);
    b.held.silent = {{0x0A}};
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.silent.clear();
    ecuio_engine_rescan(&e);
    b.run(40);
    ASSERT_EQ(b.held.count({0x0A}), 0u);
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);

    b.held.heard.clear();
    ecuio_engine_diagnose_again(&e);
    b.run(1);
    b.run_until(back_in);
    EXPECT_EQ(b.held.at(kLeave), 0u);
    EXPECT_GT(b.held.at({0x0A}), b.held.at(kLeave));
    EXPECT_GT(b.held.at(kEnter), b.held.last_obd());
    EXPECT_EQ(b.held.obd_inside(), 0u);
}

TEST(DeclaredSessionTest, NoReEntryBetweenTheClearsJudgmentAndItsZeroFour)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until(back_in);
    const size_t leave = b.held.at(kLeave);
    const size_t clear = b.held.at(kClear);
    ASSERT_NE(clear, SIZE_MAX);
    EXPECT_LT(leave, clear);
    EXPECT_EQ(b.engine.count({0x01, 0x0C}) > 0, true);
    EXPECT_EQ(b.held.at(kEnter), b.held.at(kEnter, clear))
        << "no 10 03 between the leave and the 04";
    EXPECT_EQ(b.held.obd_inside(), 0u);
    EXPECT_EQ(e.chans[1].diag.clear, ECUIO_CLEAR_CLEARED);
}

TEST(DeclaredSessionTest, ReEntryAfterAClearWaitsForTheReadAfter)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until(back_in);
    const size_t clear = b.held.at(kClear);
    const size_t read = b.held.at({0x03}, clear);
    ASSERT_NE(read, SIZE_MAX) << "the read after the 44";
    EXPECT_GT(b.held.at(kEnter, clear), b.held.last_obd());
    EXPECT_NE(e.chans[1].diag.flags & ECUIO_DIAG_COMPLETE, 0u);
}

TEST(DeclaredSessionTest, ARefusedReEntryAfterAClearIsTriedOnceMore)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.refusals = 1;
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until([](const ecuio_engine_channel& ch) {
        return ch.session_step == ECUIO_SESSION_STATE_REFUSED;
    });
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED);
    EXPECT_EQ(b.idle(), ECUIO_SESSION_IDLE_RETRY);
    const size_t asked = b.held.heard.size();
    while (b.now < e.chans[1].retry_us - 50 * kMs) {
        b.run(1);
    }
    EXPECT_EQ(b.held.heard.size(), asked) << "nothing asked while it waits";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, b.now, 1u),
              e.chans[1].retry_us - b.now)
        << "the retry is its next event";
    EXPECT_EQ(ecuio_engine_turn_next_us(&e, e.chans[1]), e.chans[1].retry_us)
        << "and an ELM327 turn's";
    b.run_until(back_in);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(b.held.count(kEnter), 2u);
    EXPECT_EQ(b.idle(), ECUIO_SESSION_IDLE_NONE);
    EXPECT_EQ(e.chans[1].session_lost, 0u) << "the first refusal is no loss";
}

TEST(DeclaredSessionTest, ARefusalAtTheRetryCountsAsALoss)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.refusals = 100;
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until(
        [](const ecuio_engine_channel& ch) {
            return ch.session_step == ECUIO_SESSION_STATE_REFUSED &&
                   ch.reentry == ECUIO_ENGINE_REENTRY_RETRIED &&
                   ch.session_lost == 1;
        },
        2000);
    EXPECT_EQ(e.chans[1].session_lost, 1u);
    EXPECT_EQ(e.chans[1].refused_losses, 1u);
    EXPECT_EQ(b.held.count(kEnter), 2u);
    b.run_until(
        [](const ecuio_engine_channel& ch) {
            return ch.session_step == ECUIO_SESSION_STATE_ABANDONED;
        },
        4000);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ABANDONED);
    EXPECT_EQ(b.held.count(kEnter), 1u + ECUIO_SESSION_LOSSES_MAX);
    b.run(200);
    EXPECT_EQ(b.held.count(kEnter), 1u + ECUIO_SESSION_LOSSES_MAX);
}

TEST(DeclaredSessionTest, TheHeldSignalsWaitAndAreNotDroppedAcrossTheLeave)
{
    static ecuio_request_engine e;
    Bench b(&e);
    e.probe = true;
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_diagnose_again(&e);
    b.run_until(back_in);
    const size_t leave = b.held.at(kLeave);
    const size_t entry = b.held.at(kEnter);
    ASSERT_LT(leave, entry);
    EXPECT_EQ(b.held.at(kRow, leave), b.held.at(kRow, entry))
        << "no row between the leave and the entry";
    EXPECT_EQ(e.requests[1].dropped, 0);
    b.run(10);
    EXPECT_NE(b.held.at(kRow, entry), SIZE_MAX);
}

TEST(DeclaredSessionTest, AClosingModuleLeavesBeforeItsRead)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_diagnose_again(&e);
    want(&e, kRange | kRead, 0);
    b.run(200);
    ASSERT_FALSE(b.held.heard.empty());
    EXPECT_EQ(b.held.heard.front().q, kLeave);
    EXPECT_EQ(b.held.count(kLeave), 1u);
    EXPECT_GE(b.held.count({0x03}), 1u) << "the read still goes";
    EXPECT_EQ(b.held.obd_inside(), 0u);
}

TEST(DeclaredSessionTest, AnUnansweredReEntryAfterAClearIsTriedOnceMore)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.silences = 1;
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until([](const ecuio_engine_channel& ch) {
        return ch.session_step == ECUIO_SESSION_STATE_UNANSWERED;
    });
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_UNANSWERED);
    EXPECT_EQ(b.idle(), ECUIO_SESSION_IDLE_RETRY);
    ecuio_engine_rescan(&e);
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERING);
    EXPECT_EQ(ecuio_engine_turn_next_us(&e, e.chans[1]), e.chans[1].retry_us)
        << "a rescan's entry still waits for the retry";
    const size_t entries = b.held.count(kEnter);
    while (b.now < e.chans[1].retry_us - 50 * kMs) {
        b.run(1);
    }
    EXPECT_EQ(b.held.count(kEnter), entries);
    b.run_until(back_in, 2000);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(b.held.count(kEnter), 2u);
}

TEST(DeclaredSessionTest, AnUntickLetsGoOfARetryAtOnce)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.refusals = 1;
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until(
        [](const ecuio_engine_channel& ch) { return ch.retry_us != 0; });
    ASSERT_NE(e.chans[1].retry_us, 0);
    want(&e, kRange | kRead, kRange);
    b.run(2);
    EXPECT_EQ(e.chans[1].retry_us, 0);
    EXPECT_EQ(e.chans[1].session_source, ECUIO_SESSION_SOURCE_NONE);
    EXPECT_EQ(b.idle(), ECUIO_SESSION_IDLE_NOT_READ);
    b.run(200);
    EXPECT_EQ(b.held.count(kEnter), 1u) << "no retry once nothing is read";
}

TEST(DeclaredSessionTest, AModuleClosedInItsRetryForgetsIt)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    b.held.refusals = 1;
    ASSERT_NO_FATAL_FAILURE(b.clear());
    b.run_until(
        [](const ecuio_engine_channel& ch) { return ch.retry_us != 0; });
    want(&e, kRange | kRead, 0);
    b.run(4);
    EXPECT_EQ(e.chans[1].retry_us, 0);
    EXPECT_EQ(e.chans[1].reentry, ECUIO_ENGINE_REENTRY_NONE);
    want(&e, kRange | kRead, kRange | kRead);
    b.held.heard.clear();
    b.held.refusals = 1;
    b.run(4);
    EXPECT_EQ(b.held.count(kEnter), 1u) << "entered at once, no retry wait";
    b.run(200);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED)
        << "a refusal after the reopen stands";
    EXPECT_EQ(e.chans[1].session_lost, 0u);
    EXPECT_EQ(b.held.count(kEnter), 1u);

    static ecuio_request_engine mid;
    Bench m(&mid);
    ASSERT_NO_FATAL_FAILURE(m.entered());
    ecuio_engine_diagnose_again(&mid);
    m.run(1);
    ASSERT_TRUE(mid.chans[1].obd_out);
    want(&mid, kRange | kRead, 0);
    m.run(40);
    EXPECT_FALSE(mid.chans[1].obd_out) << "closed mid-pass";
    EXPECT_EQ(mid.chans[1].session_source, ECUIO_SESSION_SOURCE_NONE);
}

TEST(DeclaredSessionTest, AClearWhileEnteringIsRetriedAfterToo)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_rescan(&e);
    b.held.in = false;
    ecuio_engine_diagnose_again(&e);
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERING);
    b.run_until([](const ecuio_engine_channel& ch) {
        return ch.diag_pending == 0 && !ch.diagnosing;
    });
    ASSERT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERING);
    ASSERT_TRUE(ecuio_engine_clear(&e, 1, e.chans[1].diag.generation, 0));
    b.held.refusals = 1;
    b.run_until(back_in, 2000);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[1].diag.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(b.held.obd_inside(), 0u);
}

TEST(DeclaredSessionTest, NoLeaveUnderAnOperationsHold)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    e.operation_hold = true;
    ecuio_engine_diagnose_again(&e);
    b.run(20);
    EXPECT_EQ(b.held.count(kLeave), 0u);
    e.operation_hold = false;
    b.run_until(back_in);
    EXPECT_EQ(b.held.at(kLeave), 0u);
}

TEST(DeclaredSessionTest, ARunsSessionIsNeitherLeftNorEnteredForObdItems)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_session_handed(&e, 1, b.now);
    ecuio_engine_diagnose_again(&e);
    b.run(20);
    EXPECT_EQ(b.held.count(kLeave), 0u) << "the run's session stays";

    static ecuio_request_engine mid;
    Bench m(&mid);
    ASSERT_NO_FATAL_FAILURE(m.entered());
    ecuio_engine_diagnose_again(&mid);
    m.run(1);
    ASSERT_TRUE(mid.chans[1].obd_out);
    ecuio_engine_session_handed(&mid, 1, m.now);
    m.held.in = true;
    m.held.heard.clear();
    m.run(100);
    EXPECT_EQ(mid.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_GE(m.held.count(kRow), 1u) << "read on in the run's session";
    EXPECT_EQ(m.held.count(kEnter), 0u);
}

TEST(DeclaredSessionTest, AConversationsEntryWaitsForTheObdItems)
{
    static ecuio_request_engine e;
    Bench b(&e);
    ASSERT_NO_FATAL_FAILURE(b.entered());
    ecuio_engine_diagnose_again(&e);
    b.run(1);
    ASSERT_TRUE(e.chans[1].obd_out);
    ASSERT_TRUE(ecuio_engine_session_enter(&e, 1));
    EXPECT_FALSE(e.chans[1].obd_out);
    b.run_until([](const ecuio_engine_channel& ch) {
        return ch.session_step == ECUIO_SESSION_STATE_ENTERED;
    });
    EXPECT_GT(b.held.at(kEnter), b.held.last_obd());
    EXPECT_EQ(b.held.obd_inside(), 0u);
    EXPECT_EQ(e.chans[1].session_source, ECUIO_SESSION_SOURCE_CONVERSATION);

    static ecuio_request_engine retry;
    Bench r(&retry);
    ASSERT_NO_FATAL_FAILURE(r.entered());
    r.held.refusals = 1;
    ASSERT_NO_FATAL_FAILURE(r.clear());
    r.run_until(
        [](const ecuio_engine_channel& ch) { return ch.retry_us != 0; });
    (void)ecuio_engine_session_enter(&retry, 1);
    EXPECT_EQ(retry.chans[1].retry_us, 0);
    EXPECT_EQ(retry.chans[1].reentry, ECUIO_ENGINE_REENTRY_NONE);
    EXPECT_EQ(r.idle(), ECUIO_SESSION_IDLE_NONE);
}
