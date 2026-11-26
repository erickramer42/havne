// File/layer: tests/test_engine_hold.cpp | test_obd2, Qt-free
// Testing: UNIT: EngineTalkTest's read cases (core's own read of a module's
//          memory at the talk's door) and EngineHoldTest (what the request
//          engine sends under an operation's hold), with the time passed in

#include "engine_talk_support.h"

#include "../core/src/j2534_hold.h"
#include "../core/src/request_turns.h"
#include "../core/src/tier3_ceiling.h"

#include <cstdint>
#include <utility>
#include <vector>

using namespace engine_talk_test;

namespace {

constexpr uint8_t kRange = ECUIO_ENGINE_REASON_RANGE;
constexpr uint8_t kReadReason = ECUIO_ENGINE_REASON_READ;

/* A read of 256 bytes at 00010000: read memory by address, a 4-byte
 * address and a 2-byte size */
const Bytes kMemory = {0x23, 0x24, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00};
const Bytes kLeave = {0x10, 0x01};

ecuio_talk_offer offer_read(ecuio_request_engine* e, const Bytes& pdu,
                            int64_t now_us, uint32_t c = 0)
{
    return ecuio_engine_offer_talk(
        e, c, request(pdu, 1, ecuio_talk_kind::read, 0), now_us);
}

/* A module that answers whatever a hold's cases ask */
Bytes answer(const Bytes& q)
{
    if (q.size() == 2 && q[0] == 0x10) {
        return {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
    }
    if (q == kKeep) {
        return {0x7E, 0x00};
    }
    if (q.size() == 3 && q[0] == 0x22) {
        return {0x62, q[1], q[2], 0x12, 0x34};
    }
    if (q.size() == 2 && q[0] == 0x01) {
        return {0x41, q[1], 0x00, 0x00};
    }
    if (q[0] == 0x23) {
        return {0x63, 0xAA, 0xBB};
    }
    if (q == Bytes{0x03}) {
        return {0x43, 0x00};
    }
    return {0x7F, q[0], 0x11};
}

/* Channels stepped side by side, 1 ms a step: each wait ended, each due
 * request sent and answered at once, the closes settled and the visits
 * stepped */
struct Side {
    ecuio_request_engine e;
    int64_t now = 0;
    std::vector<std::pair<uint32_t, Bytes>> sent;

    void step()
    {
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            (void)ecuio_engine_expire(&e, c, now);
            const int32_t r = ecuio_engine_due(&e, c, now);
            if (r == ECUIO_ENGINE_NONE) {
                continue;
            }
            const Bytes q = wire(&e, c, r);
            sent.emplace_back(c, q);
            ecuio_engine_sent(&e, c, r, true, now);
            const Bytes a = answer(q);
            (void)ecuio_engine_on_frame(
                &e, c, a.data(), static_cast<uint16_t>(a.size()), now, nullptr);
        }
        if (ecuio_engine_modules_settle(&e, now) != 0) {
            ecuio_engine_modules_fill(&e);
        }
        ecuio_engine_visits_step(&e);
        now += kMs;
    }

    void until(int64_t t_us)
    {
        while (now < t_us) {
            step();
        }
    }

    /* How many of the requests from the ith on went to channel c */
    size_t on(uint32_t c, size_t from) const
    {
        size_t n = 0;
        for (size_t i = from; i < sent.size(); ++i) {
            n += sent[i].first == c ? 1u : 0u;
        }
        return n;
    }

    /* How many of them were want */
    size_t count(const Bytes& want, size_t from) const
    {
        size_t n = 0;
        for (size_t i = from; i < sent.size(); ++i) {
            n += sent[i].second == want ? 1u : 0u;
        }
        return n;
    }
};

/* Two modules, each a row of its own, both in the extended session */
void two_in_session(Side& s)
{
    ASSERT_TRUE(ecuio_engine_init(&s.e, 2));
    s.e.pdu_max = ECUIO_ENGINE_PDU_MAX;
    ecuio_engine_slots(&s.e, ECUIO_MAX_CHANNELS, ECUIO_MAX_CHANNELS);
    for (uint32_t c = 0; c < 2; ++c) {
        const uint8_t q[2] = {0x01, static_cast<uint8_t>(0x0C + c)};
        ASSERT_EQ(ecuio_engine_add(&s.e, c, q, 2),
                  ecuio_engine_add_result::added);
    }
    ASSERT_TRUE(ecuio_engine_session(&s.e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    s.until(200 * kMs);
    ASSERT_EQ(s.e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_EQ(s.e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
}

} // namespace

/* Core's own read is taken exactly inside tier 3's read ceiling, whatever
 * tier 2 or the guest's list allows, and never as a plugin's plain talk */
TEST(EngineTalkTest, AReadIsTheReadCeilingForEveryServiceAndSubFunction)
{
    std::vector<unsigned> services;
    for (unsigned sid = 0; sid < 256; ++sid) {
        for (unsigned sub = 0; sub < 256; ++sub) {
            ecuio_engine_talk_state t;
            ecuio_key_window w;
            const Bytes pdu = {static_cast<uint8_t>(sid),
                               static_cast<uint8_t>(sub), 0x00};
            const bool listed =
                sid == 0x23 || sid == 0x35 || sid == 0x36 || sid == 0x37;
            ASSERT_EQ(door(t, w, pdu, 1, ecuio_talk_kind::read),
                      listed ? ecuio_talk_offer::taken
                             : ecuio_talk_offer::refused)
                << sid << " " << sub;
            ASSERT_EQ(t.offered, listed);
            if (listed) {
                ASSERT_EQ(t.kind, ecuio_talk_kind::read);
                /* and by no other name */
                ecuio_engine_talk_state other;
                for (const uint8_t echo : {uint8_t{1}, uint8_t{2}}) {
                    ASSERT_EQ(door(other, w, pdu, echo),
                              ecuio_talk_offer::refused)
                        << sid;
                    ASSERT_EQ(door(other, w, pdu, echo, ecuio_talk_kind::seed,
                                   kWindow),
                              ecuio_talk_offer::refused)
                        << sid;
                }
                if (sub == 0) {
                    services.push_back(sid);
                }
            }
        }
    }
    EXPECT_EQ(services, (std::vector<unsigned>{0x23, 0x35, 0x36, 0x37}));
    /* By name: a download, a write to memory, a DID's read, a seed and a
     * key inside an open window are no read */
    for (const Bytes& pdu : {Bytes{0x34, 0x00, 0x44}, Bytes{0x3D, 0x24, 0x00},
                             Bytes{0x22, 0xD2, 0x10}, Bytes{0x27, 0x01},
                             Bytes{0x27, 0x02, 0x00}}) {
        ecuio_engine_talk_state t;
        ecuio_key_window w;
        w.until_us = 1000;
        w.seed = 0x01;
        EXPECT_EQ(door(t, w, pdu, 1, ecuio_talk_kind::read),
                  ecuio_talk_offer::refused)
            << int{pdu[0]};
        EXPECT_FALSE(t.offered);
    }
}

/* A read's reply is matched on its service alone (the run judges a 76's
 * counter and a 63's length), so its echo is one byte and no more; past
 * the transport's bound it is too long, and one alone is taken */
TEST(EngineTalkTest, AReadEchoesItsServiceAlone)
{
    for (const uint8_t echo : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
        ecuio_engine_talk_state t;
        ecuio_key_window w;
        EXPECT_EQ(door(t, w, kMemory, echo, ecuio_talk_kind::read),
                  echo == 1 ? ecuio_talk_offer::taken
                            : ecuio_talk_offer::refused)
            << int{echo};
    }
    ecuio_engine_talk_state t;
    ecuio_key_window w;
    EXPECT_EQ(door(t, w, kMemory, 1, ecuio_talk_kind::read, 0, 0,
                   ECUIO_ENGINE_PDU_SINGLE),
              ecuio_talk_offer::too_long);
    EXPECT_FALSE(t.offered);
    EXPECT_EQ(door(t, w, {0x37}, 1, ecuio_talk_kind::read, 0, 0,
                   ECUIO_ENGINE_PDU_SINGLE),
              ecuio_talk_offer::taken);
    EXPECT_EQ(door(t, w, kMemory, 1, ecuio_talk_kind::read),
              ecuio_talk_offer::busy);
}

/* Whatever follows the service answers a read: a 76 with another counter
 * settles it answered for the run to judge; a refusal settles it with its
 * code; and its settle ends no key's window */
TEST(EngineTalkTest, AReadIsAnsweredByItsServiceWhateverFollows)
{
    ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(entered(&e, &ecu, &now));
    const Bytes block = {0x36, 0x01};
    ASSERT_EQ(offer_read(&e, block, now), ecuio_talk_offer::taken);
    std::vector<Bytes> went = until_settled(&e, &now, [&](const Bytes& q) {
        return q == block ? Bytes{0x76, 0x02, 0xAA} : ecu(q);
    });
    EXPECT_EQ(count_of(went, block), 1u);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);

    ASSERT_EQ(offer_read(&e, kMemory, now), ecuio_talk_offer::taken);
    went = until_settled(&e, &now, std::ref(ecu));
    EXPECT_EQ(count_of(went, kMemory), 1u) << "the whole request, once";
    uint8_t nrc = 0;
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x11);

    ecuio_engine_talk_state t;
    ecuio_key_window w;
    ASSERT_EQ(door(t, w, kMemory, 1, ecuio_talk_kind::read),
              ecuio_talk_offer::taken);
    uint8_t slot[3] = {};
    ASSERT_EQ(ecuio_talk_take_slot(t, slot), 1);
    w.until_us = 5000;
    ecuio_talk_settle(t, w, ecuio_talk_end::answered, 0, 1000);
    EXPECT_EQ(w.until_us, 5000) << "a read is no key";
    EXPECT_EQ(t.result.end, ecuio_talk_end::answered);
}

/* Under the hold an open module is sent a talk and its session's
 * keep-alive: no row, and no scan entry asked meanwhile */
TEST(EngineHoldTest, AnOpenModuleSendsATalkAndItsKeepAliveAndNothingElse)
{
    Side s;
    ASSERT_NO_FATAL_FAILURE(engine(&s.e, 2));
    ASSERT_TRUE(ecuio_engine_session(&s.e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    s.until(200 * kMs);
    ASSERT_EQ(s.e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_GT(s.count(kRow, 0), 2u);
    const Bytes scanned = {0x22, 0xD3, 0x01};

    s.e.operation_hold = true;
    size_t from = s.sent.size();
    ASSERT_TRUE(ecuio_engine_scan_add(&s.e, 0, 0xD301));
    s.until(s.now + 3 * ECUIO_ENGINE_KEEPALIVE_US + 100 * kMs);
    const size_t kept = s.count(kKeep, from);
    EXPECT_EQ(s.sent.size() - from, kept) << "the keep-alive alone";
    EXPECT_GE(kept, 2u);
    EXPECT_LE(kept, 4u) << "at its own pace";

    /* core's read, then a plain talk (an identifier's read) */
    from = s.sent.size();
    ASSERT_EQ(offer_read(&s.e, kMemory, s.now), ecuio_talk_offer::taken);
    s.until(s.now + 20 * kMs);
    ASSERT_EQ(s.sent.size(), from + 1);
    EXPECT_EQ(s.sent[from].second, kMemory);
    EXPECT_EQ(taken(&s.e), ecuio_talk_end::answered);
    const Bytes ident = {0x22, 0xD3, 0x00};
    ASSERT_EQ(offer(&s.e, ident, 3), ecuio_talk_offer::taken);
    s.until(s.now + 20 * kMs);
    ASSERT_EQ(s.sent.size(), from + 2);
    EXPECT_EQ(s.sent[from + 1].second, ident);
    EXPECT_EQ(taken(&s.e), ecuio_talk_end::answered);

    /* the hold's end: the rotation and the scan go on */
    s.e.operation_hold = false;
    from = s.sent.size();
    s.until(s.now + 200 * kMs);
    EXPECT_GT(s.count(kRow, from), 2u);
    EXPECT_GE(s.count(scanned, from), 1u) << "asked, and a row once answered";
}

/* A one-shot the module had taken when the hold began is still sent: its
 * reply settles it, and nothing after it is taken */
TEST(EngineHoldTest, AOneShotTakenBeforeTheHoldStillGoes)
{
    Side s;
    ASSERT_NO_FATAL_FAILURE(engine(&s.e, 1));
    ASSERT_TRUE(ecuio_engine_scan_add(&s.e, 0, 0xD301));
    ASSERT_TRUE(ecuio_engine_scan_add(&s.e, 0, 0xD302));
    int32_t r = ECUIO_ENGINE_NONE;
    for (int i = 0; i < 50 && !s.e.chans[0].scanning; ++i) {
        r = ecuio_engine_due(&s.e, 0, s.now);
        if (r != ECUIO_ENGINE_NONE && !s.e.chans[0].scanning) {
            ecuio_engine_sent(&s.e, 0, r, true, s.now);
            const Bytes a = answer(wire(&s.e, 0, r));
            (void)ecuio_engine_on_frame(&s.e, 0, a.data(),
                                        static_cast<uint16_t>(a.size()), s.now,
                                        nullptr);
        }
        s.now += s.e.chans[0].scanning ? 0 : kMs;
    }
    ASSERT_TRUE(s.e.chans[0].scanning) << "an entry taken and not yet sent";
    const Bytes first = wire(&s.e, 0, r);

    s.e.operation_hold = true;
    EXPECT_EQ(ecuio_engine_next_event_us(&s.e, s.now), 0);
    EXPECT_EQ(ecuio_engine_turn_own_us(&s.e, s.e.chans[0]),
              s.e.chans[0].next_send_us);
    const size_t from = s.sent.size();
    s.until(s.now + 300 * kMs);
    ASSERT_EQ(s.sent.size(), from + 1);
    EXPECT_EQ(s.sent[from].second, first);
    EXPECT_EQ(ecuio_engine_next_event_us(&s.e, s.now), INT64_MAX);
}

/* A module that isn't open sends nothing under the hold, not what it
 * owes as it closes, and gives the worker nothing to wait for; it leaves
 * its session once the hold ends */
TEST(EngineHoldTest, AClosingModuleSendsNothingUntilTheHoldEnds)
{
    Side s;
    ASSERT_NO_FATAL_FAILURE(two_in_session(s));
    ecuio_engine_module_want(&s.e, 1, 0);
    ASSERT_EQ(s.e.chans[1].module_state, ECUIO_MODULE_STATE_CLOSING);
    s.e.operation_hold = true;
    size_t from = s.sent.size();
    s.until(s.now + 500 * kMs);
    EXPECT_EQ(s.on(1, from), 0u);
    EXPECT_EQ(s.e.chans[1].module_state, ECUIO_MODULE_STATE_CLOSING);
    EXPECT_EQ(ecuio_engine_limited_in_us(&s.e, s.e.chans[1], s.now), INT64_MAX);
    EXPECT_EQ(ecuio_engine_turn_next_us(&s.e, s.e.chans[1]), INT64_MAX);

    s.e.operation_hold = false;
    from = s.sent.size();
    s.until(s.now + 200 * kMs);
    ASSERT_EQ(s.on(1, from), 1u);
    EXPECT_EQ(s.count(kLeave, from), 1u);
    EXPECT_EQ(s.e.chans[1].module_state, ECUIO_MODULE_STATE_CLOSED);
}

/* A closed module's visit (a diagnostics read) sends nothing under the
 * hold and goes on after it */
TEST(EngineHoldTest, AVisitingModuleSendsNothingUntilTheHoldEnds)
{
    Side s;
    ASSERT_TRUE(ecuio_engine_init(&s.e, 2));
    ecuio_engine_slots(&s.e, ECUIO_MAX_CHANNELS, ECUIO_MAX_CHANNELS);
    s.e.module_budget = 1;
    s.e.filters_most = 2;
    ecuio_engine_diagnose(&s.e, ecuio_obd2_signal_set());
    const uint8_t reasons[2] = {kRange | kReadReason, kReadReason};
    ecuio_engine_modules_apply(&s.e, reasons, nullptr, 0);
    ASSERT_EQ(s.e.chans[1].module_state, ECUIO_MODULE_STATE_NO_ROOM);
    s.until(500 * kMs);
    ecuio_engine_visit_want(&s.e, 1);
    ecuio_engine_visits_step(&s.e);
    ASSERT_TRUE(s.e.chans[1].visiting);

    s.e.operation_hold = true;
    size_t from = s.sent.size();
    s.until(s.now + 500 * kMs);
    EXPECT_EQ(s.on(1, from), 0u);
    EXPECT_TRUE(s.e.chans[1].visiting);
    EXPECT_EQ(ecuio_engine_next_event_us(&s.e, s.now), INT64_MAX);

    s.e.operation_hold = false;
    from = s.sent.size();
    s.until(s.now + 500 * kMs);
    EXPECT_GT(s.on(1, from), 0u);
}

/* The worker's next event under the hold is a talk's or the session's,
 * never the rotation's; and the hold ends no turn-taking of the bus */
TEST(EngineHoldTest, TheNextEventIsATalksOrTheSessionsAlone)
{
    Side s;
    ASSERT_NO_FATAL_FAILURE(engine(&s.e, 2));
    s.until(100 * kMs);
    ASSERT_LE(ecuio_engine_next_event_us(&s.e, s.now), ECUIO_ENGINE_GAP_US);
    ecuio_engine_turns turns;
    uint32_t c = 9;
    int64_t wait_us = 0;

    s.e.operation_hold = true;
    s.now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_next_event_us(&s.e, s.now), INT64_MAX);
    EXPECT_EQ(ecuio_engine_turn_own_us(&s.e, s.e.chans[0]), INT64_MAX);
    EXPECT_EQ(ecuio_engine_take_turn(&s.e, turns, 0, s.now, &c, &wait_us),
              ECUIO_ENGINE_NONE);

    ASSERT_EQ(offer_read(&s.e, kMemory, s.now), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_next_event_us(&s.e, s.now), 0);
    EXPECT_EQ(ecuio_engine_turn_own_us(&s.e, s.e.chans[0]),
              s.e.chans[0].next_send_us);
    EXPECT_EQ(ecuio_engine_take_turn(&s.e, turns, 0, s.now, &c, &wait_us),
              ECUIO_ENGINE_ONCE);
    EXPECT_EQ(c, 0u);
    EXPECT_EQ(wire(&s.e, 0, ECUIO_ENGINE_ONCE), kMemory);
}

/* J2534's turns around a segmented reply are for the rotation: under an
 * operation's hold the pass is the plain one */
TEST(EngineHoldTest, TheJ2534HoldStandsAsideForIt)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 2));
    j2534_hold h;
    ASSERT_TRUE(j2534_hold_applies(h, &e));
    e.operation_hold = true;
    EXPECT_FALSE(j2534_hold_applies(h, &e));
}
