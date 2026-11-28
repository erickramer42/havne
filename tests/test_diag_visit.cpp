// File/layer: tests/test_diag_visit.cpp | test_obd2, Qt-free
// Testing: UNIT: DiagVisitTest (a read of a closed module's codes opens it
//          in the filter kept back, one module at a time, asks its pass and
//          closes it after, and a clear of one: request_diag_visit.h;
//          ADR-0065 step 5)

#include "module_codes_support.h"
#include "test_obd2_support.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace {

constexpr uint8_t kRange = ECUIO_ENGINE_REASON_RANGE;
constexpr uint8_t kRead = ECUIO_ENGINE_REASON_READ;

struct Sent {
    uint32_t c;
    int64_t at;
    Bytes bytes;
};

/* Modules driven as a worker drives them, 1 ms a step: replies handed
 * over 4 ms after their request (respond's; {} silence), waits ended,
 * each channel's due request sent, then the closes settled, a freed
 * filter's module opened and the visits stepped */
struct Visits {
    ecuio_request_engine e;
    int64_t now = 0;
    std::vector<Sent> sent;
    std::vector<int64_t> reply_at;
    std::vector<Bytes> reply;
    uint32_t most_visiting = 0; /* the most modules visiting at once */
    std::function<Bytes(uint32_t, const Bytes&)> respond =
        [](uint32_t, const Bytes& q) { return fake_reply(q); };

    /* n modules reading diagnostics; reasons[c] each, budget live data's
     * and filters the transport's */
    void make(const std::vector<uint8_t>& reasons, uint32_t budget,
              uint32_t filters)
    {
        const uint32_t n = static_cast<uint32_t>(reasons.size());
        ASSERT_TRUE(ecuio_engine_init(&e, n));
        ecuio_engine_slots(&e, ECUIO_MAX_CHANNELS, ECUIO_MAX_CHANNELS);
        e.module_budget = budget;
        e.filters_most = filters;
        ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
        ecuio_engine_modules_apply(&e, reasons.data(), nullptr, 0);
        reply_at.assign(n, INT64_MAX);
        reply.assign(n, {});
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
            sent.push_back({c, now, b});
            ecuio_engine_sent(&e, c, r, true, now);
            reply[c] = respond(c, b);
            reply_at[c] = reply[c].empty() ? INT64_MAX : now + 4 * kMs;
        }
        if (ecuio_engine_modules_settle(&e, now) != 0) {
            ecuio_engine_modules_fill(&e);
        }
        ecuio_engine_visits_step(&e);
        uint32_t visiting = 0;
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            visiting += e.chans[c].visiting ? 1u : 0u;
        }
        most_visiting = visiting > most_visiting ? visiting : most_visiting;
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

    int64_t first_on(uint32_t c, int64_t from_us) const
    {
        for (const Sent& s : sent) {
            if (s.c == c && s.at >= from_us) {
                return s.at;
            }
        }
        return INT64_MAX;
    }

    ecuio_channel_diagnostics_t report(uint32_t c) const
    {
        ecuio_channel_diagnostics_t d;
        ecuio_engine_diagnostics(&e, c, &d);
        return d;
    }

    uint8_t state(uint32_t c) const { return e.chans[c].module_state; }
};

/* The outcomes of the items every read asks: readiness (01 01), the
 * cycle's (01 41) and each code list's */
std::vector<uint8_t> outcomes(const ecuio_channel_diagnostics_t& d)
{
    std::vector<uint8_t> out = {d.readiness, d.cycle};
    for (const ecuio_dtc_list_t& l : d.lists) {
        out.push_back(l.outcome);
    }
    return out;
}

class DiagVisitTest : public ::testing::Test
{
protected:
    void SetUp() override { fake_ecu::reset_diagnostics(); }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

} // namespace

/* A closed module is sent nothing until a read: then it opens, reads its
 * codes to the end and closes again */
TEST_F(DiagVisitTest, AReadOnAClosedModuleOpensItAndClosesItAfter)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 31, 32));
    v.until(1000 * kMs);
    ASSERT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED);
    ASSERT_EQ(v.sent_on(1), 0u);
    EXPECT_FALSE(v.report(1).flags & ECUIO_DIAG_COMPLETE) << "never read";
    ecuio_engine_visit_closed(&v.e);
    v.until(4000 * kMs);
    EXPECT_GT(v.sent_on(1), 3u);
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_NOT_OPENED);
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED) << "closed after";
    EXPECT_FALSE(v.e.chans[1].visiting);
    EXPECT_EQ(v.e.visit_now, UINT32_MAX);
}

TEST_F(DiagVisitTest, VisitsGoOneClosedModuleAtATime)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0, 0, 0, 0, 0, 0}, 31, 32));
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    v.until(20000 * kMs);
    EXPECT_EQ(v.most_visiting, 1u);
    for (uint32_t c = 1; c < 7; ++c) {
        EXPECT_TRUE(v.report(c).flags & ECUIO_DIAG_COMPLETE) << c;
        EXPECT_EQ(v.state(c), ECUIO_MODULE_STATE_CLOSED) << c;
    }
}

/* An open module's read goes in place: no visit, no filter, never closed */
TEST_F(DiagVisitTest, AVisitOnAnOpenModuleTakesNoFilterAndNeverClosesIt)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, kRead}, 31, 32));
    v.until(3000 * kMs);
    ecuio_engine_diagnose_again(&v.e);
    ecuio_engine_visit_closed(&v.e);
    EXPECT_EQ(v.e.visit_count, 0u);
    v.until(6000 * kMs);
    EXPECT_EQ(v.most_visiting, 0u);
    EXPECT_EQ(v.state(0), ECUIO_MODULE_STATE_OPEN);
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_OPEN);
    EXPECT_TRUE(v.report(1).flags & ECUIO_DIAG_COMPLETE);
}

/* A visit asks the module's pass alone: identification and its codes, no
 * walk (01 00), row, scanned DID or session (10 03) */
TEST_F(DiagVisitTest, AVisitAsksIdentificationAndCodesAndNoWalkNoSession)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 31, 32));
    const uint8_t row[2] = {0x01, 0x0D};
    ASSERT_EQ(ecuio_engine_add(&v.e, 1, row, 2), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&v.e, 1, 0xD100));
    ecuio_engine_identify(&v.e);
    ecuio_engine_probe(&v.e);
    ASSERT_TRUE(ecuio_engine_session(&v.e, ECUIO_SESSION_EXTENDED,
                                      ECUIO_ENGINE_KEEPALIVE_US));
    v.until(500 * kMs);
    ASSERT_EQ(v.sent_on(1), 0u);
    ecuio_engine_visit_closed(&v.e);
    v.until(6000 * kMs);
    bool ident = false;
    for (const Sent& s : v.sent) {
        if (s.c != 1) {
            continue;
        }
        ident = ident || s.bytes[0] == 0x09;
        EXPECT_NE(s.bytes, (Bytes{0x01, 0x00})) << "the walk";
        EXPECT_NE(s.bytes, (Bytes{0x01, 0x0D})) << "a row";
        EXPECT_NE(s.bytes, (Bytes{0x22, 0xD1, 0x00})) << "a scanned DID";
        EXPECT_NE(s.bytes[0], 0x10) << "a session";
    }
    EXPECT_TRUE(ident) << "identification asked";
    EXPECT_TRUE(v.report(1).flags & ECUIO_DIAG_COMPLETE);
}

/* No filter free (the transport's bound reached): the read ends at once,
 * complete with every item unasked, NOT_OPENED; nothing sent to the
 * module, and the open ones read on */
TEST_F(DiagVisitTest, AVisitWithNoFilterEndsNotOpened)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 1, 1));
    v.until(500 * kMs);
    const uint32_t generation = v.report(1).generation;
    ecuio_engine_visit_closed(&v.e);
    v.step();
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_EQ(d.flags & (ECUIO_DIAG_COMPLETE | ECUIO_DIAG_NOT_OPENED),
              ECUIO_DIAG_COMPLETE | ECUIO_DIAG_NOT_OPENED);
    EXPECT_NE(d.generation, generation);
    EXPECT_EQ(outcomes(d), std::vector<uint8_t>(2 + ECUIO_DIAG_LISTS,
                                                 ECUIO_DIAG_UNASKED));
    v.until(2000 * kMs);
    EXPECT_EQ(v.sent_on(1), 0u);
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED);
}

TEST_F(DiagVisitTest, ANotOpenedReadKeepsEveryOpenModuleOpen)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 1, 1));
    const uint8_t row[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&v.e, 0, row, 2), ecuio_engine_add_result::added);
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    const int64_t from = v.now;
    v.until(3000 * kMs);
    EXPECT_EQ(v.state(0), ECUIO_MODULE_STATE_OPEN);
    EXPECT_GT(v.sent_on(0, from), 0u);
}

/* A module that answers nothing reads as such: every item unanswered,
 * never NOT_OPENED */
TEST_F(DiagVisitTest, AModuleThatAnswersNothingReadsAsNoResponse)
{
    Visits v;
    v.respond = [](uint32_t c, const Bytes& q) {
        return c == 1 ? Bytes{} : fake_reply(q);
    };
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 31, 32));
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    v.until(5000 * kMs);
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_NOT_OPENED);
    EXPECT_EQ(outcomes(d), std::vector<uint8_t>(2 + ECUIO_DIAG_LISTS,
                                                 ECUIO_DIAG_UNANSWERED));
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED);
}

TEST_F(DiagVisitTest, TheQueueKeepsTheOrderTheCallsCame)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0, 0, 0}, 31, 32));
    v.until(500 * kMs);
    const int64_t from = v.now;
    for (const uint32_t c : {3u, 1u, 2u}) {
        ecuio_engine_visit_want(&v.e, c);
    }
    v.until(15000 * kMs);
    EXPECT_LT(v.first_on(3, from), v.first_on(1, from));
    EXPECT_LT(v.first_on(1, from), v.first_on(2, from));
}

/* A module waiting for a live filter, visited in the one kept back, waits
 * again after its pass: the visit never makes it live */
TEST_F(DiagVisitTest, AVisitedModuleWaitingForAFilterWaitsAgain)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRead, kRead}, 1, 2));
    ASSERT_EQ(v.state(1), ECUIO_MODULE_STATE_NO_ROOM);
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    v.until(4000 * kMs);
    EXPECT_TRUE(v.report(1).flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_NO_ROOM);
    EXPECT_EQ(v.e.visit_now, UINT32_MAX);
    EXPECT_EQ(v.e.chans[1].reasons, kRead);
}

/* A live module's filter refused while a visit holds the one kept back:
 * the live budget falls to the filters live data holds, so no module
 * opens into the visit's filter, and that filter is the next visit's */
TEST_F(DiagVisitTest, ARefusalDuringAVisitLeavesItsFilterToTheNextVisit)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRead, kRead, kRead, kRead}, 3, 4));
    v.until(500 * kMs);
    ASSERT_EQ(v.state(3), ECUIO_MODULE_STATE_NO_ROOM);
    ecuio_engine_visit_want(&v.e, 3);
    v.step();
    ASSERT_TRUE(v.e.chans[3].visiting);
    /* module 2's start refused: modules 0 and 1 and the visit hold one */
    ecuio_engine_filter_refused(&v.e, 2, 0x1u | 0x2u | 0x8u);
    EXPECT_EQ(v.e.filters_most, 3u);
    EXPECT_EQ(v.e.module_budget, 2u);
    EXPECT_EQ(v.state(2), ECUIO_MODULE_STATE_NO_ROOM);
    v.until(v.now + 4000 * kMs);
    EXPECT_EQ(v.state(2), ECUIO_MODULE_STATE_NO_ROOM)
        << "opened into the visit's filter";
    EXPECT_EQ(v.state(3), ECUIO_MODULE_STATE_NO_ROOM);
    ecuio_engine_diagnose_again(&v.e);
    ecuio_engine_visit_closed(&v.e);
    v.until(v.now + 8000 * kMs);
    for (const uint32_t c : {2u, 3u}) {
        const ecuio_channel_diagnostics_t d = v.report(c);
        EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE) << c;
        EXPECT_FALSE(d.flags & ECUIO_DIAG_NOT_OPENED)
            << c << ": the next visit found no filter";
    }
}

namespace {

/* A Visits whose engine module (channel 0) reads 0 rpm, the rest the
 * fake's; module 1 read by a visit and closed again */
void read_and_close(Visits& v, std::vector<uint8_t> reasons)
{
    ASSERT_NO_FATAL_FAILURE(v.make(reasons, 31, 32));
    v.e.rpm_module = 0;
    v.respond = [](uint32_t, const Bytes& q) {
        return q == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x00, 0x00}
                                      : fake_reply(q);
    };
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    v.until(v.now + 6000 * kMs);
    for (uint32_t c = 1; c < reasons.size(); ++c) {
        ASSERT_TRUE(v.report(c).flags & ECUIO_DIAG_COMPLETE) << c;
        ASSERT_EQ(v.state(c), ECUIO_MODULE_STATE_CLOSED) << c;
    }
}

bool sent_04(const Visits& v, uint32_t c, int64_t from_us)
{
    for (const Sent& s : v.sent) {
        if (s.c == c && s.at >= from_us && s.bytes == Bytes{0x04}) {
            return true;
        }
    }
    return false;
}

} // namespace

/* A clear of a module that reported and closed visits it: the 04 goes,
 * the read after it, and it closes again; the request ends, so the next
 * is taken */
TEST_F(DiagVisitTest, AClearOfAClosedModuleVisitsItAndClosesItAfter)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(read_and_close(v, {kRange, 0}));
    const int64_t from = v.now;
    ASSERT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation));
    v.until(v.now + 6000 * kMs);
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_TRUE(sent_04(v, 1, from));
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE) << "read again after";
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED) << "closed after";
    EXPECT_FALSE(ecuio_engine_clear_running(&v.e));
    EXPECT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation))
        << "a later clear refused";
}

namespace {

/* The 04s sent to module c from from_us on */
size_t count_04(const Visits& v, uint32_t c, int64_t from_us)
{
    size_t n = 0;
    for (const Sent& s : v.sent) {
        n += s.c == c && s.at >= from_us && s.bytes == Bytes{0x04} ? 1u : 0u;
    }
    return n;
}

/* Module 1 picked and waiting for a live filter (module 0 holds the one
 * live data has, the engine module at 0 rpm), its codes read by a visit
 * in the filter kept back */
void read_waiting(Visits& v)
{
    ASSERT_NO_FATAL_FAILURE(v.make({kRead, kRead}, 1, 2));
    v.e.rpm_module = 0;
    v.respond = [](uint32_t, const Bytes& q) {
        return q == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x00, 0x00}
                                      : fake_reply(q);
    };
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    v.until(v.now + 4000 * kMs);
    ASSERT_TRUE(v.report(1).flags & ECUIO_DIAG_COMPLETE);
    ASSERT_EQ(v.state(1), ECUIO_MODULE_STATE_NO_ROOM);
}

} // namespace

/* A clear of a module waiting for a live filter visits it once: the 04
 * and the read after go in the one visit, which opens it once and leaves
 * it waiting again; the request ends, so the next is taken */
TEST_F(DiagVisitTest, AClearOfAModuleWaitingForAFilterTakesOneVisit)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(read_waiting(v));
    const int64_t from = v.now;
    ASSERT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation));
    uint32_t opened = 0;
    uint32_t left = 0;
    uint8_t was = v.state(1);
    while (v.now < from + 6000 * kMs) {
        v.step();
        const uint8_t now = v.state(1);
        opened += now == ECUIO_MODULE_STATE_OPEN && was != now ? 1u : 0u;
        left += was == ECUIO_MODULE_STATE_OPEN && was != now ? 1u : 0u;
        was = now;
    }
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(count_04(v, 1, from), 1u);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE) << "read again after";
    EXPECT_EQ(opened, 1u) << "the visit's filter started more than once";
    EXPECT_EQ(left, 1u) << "the visit's filter stopped more than once";
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_NO_ROOM) << "waiting again";
    EXPECT_EQ(v.e.visit_now, UINT32_MAX);
    EXPECT_FALSE(ecuio_engine_clear_running(&v.e));
    EXPECT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation))
        << "a later clear refused";
}

/* A module picked between its visit's 04 and the reply holds the visit's
 * filter live from then: the clear ends CLEARED with its read after, and
 * the module stays open and asks its row, though live data had no room */
TEST_F(DiagVisitTest, AModulePickedMidClearVisitStaysOpenAndClears)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 1, 2));
    const uint8_t row[2] = {0x01, 0x05};
    ASSERT_EQ(ecuio_engine_add(&v.e, 1, row, 2),
              ecuio_engine_add_result::added);
    v.e.rpm_module = 0;
    v.respond = [](uint32_t, const Bytes& q) {
        return q == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x00, 0x00}
                                      : fake_reply(q);
    };
    v.until(500 * kMs);
    ecuio_engine_visit_closed(&v.e);
    v.until(v.now + 4000 * kMs);
    ASSERT_TRUE(v.report(1).flags & ECUIO_DIAG_COMPLETE);
    ASSERT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED);
    const int64_t from = v.now;
    ASSERT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation));
    while (count_04(v, 1, from) == 0 && v.now < from + 2000 * kMs) {
        v.step();
    }
    ASSERT_EQ(count_04(v, 1, from), 1u);
    ASSERT_TRUE(v.e.chans[1].visiting) << "the 04 went out of a visit";
    const uint8_t picked[2] = {kRange, kRead};
    ecuio_engine_modules_apply(&v.e, picked, nullptr, 0);
    const int64_t pick = v.now;
    v.until(v.now + 6000 * kMs);
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE) << "read again after";
    EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_OPEN);
    EXPECT_FALSE(v.e.chans[1].visiting);
    EXPECT_EQ(v.e.visit_now, UINT32_MAX);
    EXPECT_EQ(v.e.chans[1].reasons, kRead);
    size_t rows = 0;
    for (const Sent& s : v.sent) {
        rows +=
            s.c == 1 && s.at >= pick && s.bytes == Bytes{0x01, 0x05} ? 1u : 0u;
    }
    EXPECT_GT(rows, 0u) << "the picked row never asked";
    EXPECT_FALSE(ecuio_engine_clear_running(&v.e));
}

/* With no filter free for the visit the target ends NOT_OPENED, nothing
 * sent, its report's read untouched; the request ends */
TEST_F(DiagVisitTest, AClearOfAClosedModuleWithNoFilterEndsNotOpened)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(read_and_close(v, {kRange, 0}));
    v.e.filters_most = 1;
    const int64_t from = v.now;
    ASSERT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation));
    v.until(v.now + 1000 * kMs);
    const ecuio_channel_diagnostics_t d = v.report(1);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_NOT_OPENED);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_NOT_OPENED) << "its read stands";
    EXPECT_EQ(v.sent_on(1, from), 0u);
    EXPECT_FALSE(ecuio_engine_clear_running(&v.e));
    EXPECT_TRUE(ecuio_engine_clear(&v.e, 1, v.report(1).generation))
        << "a later clear refused";
}

/* Two closed targets and the engine running: the judgment ends both
 * ENGINE_RUNNING while the first is visiting, and the second, queued,
 * owes nothing then and is never opened */
TEST_F(DiagVisitTest, AQueuedVisitOwingNothingNowNeverOpens)
{
    Visits v;
    ASSERT_NO_FATAL_FAILURE(read_and_close(v, {kRange, 0, 0}));
    v.respond = [](uint32_t, const Bytes& q) {
        return q == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x0B, 0xB8}
                                      : fake_reply(q);
    };
    const uint32_t generation = v.e.chans[2].generation;
    const ecuio_clear_target_t targets[2] = {
        {1, v.report(1).generation, {0, 0}},
        {2, v.report(2).generation, {0, 0}}};
    ASSERT_TRUE(ecuio_engine_clear_request(&v.e, targets, 2, 0));
    v.until(v.now + 2000 * kMs);
    EXPECT_EQ(v.report(1).clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(v.report(2).clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(v.e.chans[2].generation, generation) << "module 2 opened";
    EXPECT_EQ(v.state(2), ECUIO_MODULE_STATE_CLOSED);
}

TEST_F(DiagVisitTest, AClosedModulesCodesReadOpensItOrEndsUnasked)
{
    for (const uint32_t filters : {32u, 1u}) {
        Visits v;
        v.respond = [](uint32_t, const Bytes& q) {
            const Bytes r = module_codes_test::codes_reply(q);
            return r.empty() ? fake_reply(q) : r;
        };
        ASSERT_NO_FATAL_FAILURE(v.make({kRange, 0}, 31, filters));
        v.until(1000 * kMs);
        ASSERT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED);
        ecuio_engine_read_codes(&v.e, 1, 0, 1);
        ecuio_engine_visit_want(&v.e, 1);
        v.until(3000 * kMs);
        size_t asked = 0;
        for (const Sent& s : v.sent) {
            asked += s.c == 1 && s.bytes[0] == 0x19 ? 1u : 0u;
        }
        const ecuio_module_codes_t& r = v.e.chans[1].codes.report;
        EXPECT_NE(r.flags & ECUIO_MODULE_CODES_COMPLETE, 0u) << filters;
        EXPECT_EQ(r.flags & ECUIO_MODULE_CODES_READING, 0u) << filters;
        EXPECT_EQ(v.state(1), ECUIO_MODULE_STATE_CLOSED) << filters;
        if (filters == 32u) {
            EXPECT_EQ(asked, 2u);
            EXPECT_EQ(r.flags & ECUIO_MODULE_CODES_NOT_OPENED, 0u);
            EXPECT_EQ(r.list_outcome, ECUIO_DIAG_ANSWERED);
            EXPECT_EQ(r.held, 4u);
        } else {
            EXPECT_EQ(v.sent_on(1), 0u);
            EXPECT_NE(r.flags & ECUIO_MODULE_CODES_NOT_OPENED, 0u);
            EXPECT_EQ(r.count_outcome, ECUIO_DIAG_UNASKED);
            EXPECT_EQ(r.list_outcome, ECUIO_DIAG_UNASKED);
        }
    }
}
