#include "module_codes_support.h"
#include "test_obd2_support.h"

#include <algorithm>

namespace {

using module_codes_test::codes_reply;
using module_codes_test::codes_under;

class ModuleCodesTest : public DiagnosticsTest
{};

Bytes with_codes(const Bytes& req)
{
    const Bytes r = codes_reply(req);
    return r.empty() ? fake_reply(req) : r;
}

Bytes stopped_with_codes(const Bytes& req)
{
    if (req == Bytes{0x01, 0x0C}) {
        return {0x41, 0x0C, 0x00, 0x00};
    }
    return with_codes(req);
}

std::vector<Bytes> requests_of(const std::vector<SentRequest>& sent)
{
    std::vector<Bytes> out;
    for (const SentRequest& s : sent) {
        out.push_back(s.bytes);
    }
    return out;
}

/* Channel c's one-shot taken and sent at now: its bytes */
Bytes send_once(ecuio_request_engine* e, uint32_t c, int64_t now)
{
    EXPECT_EQ(ecuio_engine_due(e, c, now), ECUIO_ENGINE_ONCE);
    const ecuio_request& q = ecuio_engine_request_of(e, c, ECUIO_ENGINE_ONCE);
    const Bytes b(q.bytes, q.bytes + q.len);
    ecuio_engine_sent(e, c, ECUIO_ENGINE_ONCE, true, now);
    return b;
}

ecuio_reply answer(ecuio_request_engine* e, uint32_t c, const Bytes& reply,
                   int64_t now)
{
    return ecuio_engine_on_frame(
        e, c, reply.data(), static_cast<uint16_t>(reply.size()), now, nullptr);
}

const Bytes kCountAf = {0x19, 0x01, 0xAF};
const Bytes kListAf = {0x19, 0x02, 0xAF};

} // namespace

TEST_F(ModuleCodesTest, AReadAsksTheCountThenTheListAndEndsComplete)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    EXPECT_EQ(requests_of(drive(&e, &now, with_codes)),
              (std::vector<Bytes>{kCountAf, kListAf}));
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.flags, ECUIO_MODULE_CODES_COMPLETE | ECUIO_MODULE_CODES_FORMAT);
    EXPECT_EQ(r.read_id, 1u);
    EXPECT_EQ(r.generation, 3u) << "the start, the count, the list";
    EXPECT_EQ(r.mask, 0xAF);
    EXPECT_EQ(r.count_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(r.list_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(r.count, 4u);
    ASSERT_EQ(r.held, 4u);
    EXPECT_EQ(r.reported, 4u);
    const auto want = codes_under(0xAF);
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(r.codes[i].code, want[i].code) << i;
        EXPECT_EQ(r.codes[i].status, want[i].status) << i;
    }
    EXPECT_GT(r.settled_us, 0);
}

TEST_F(ModuleCodesTest, TheCountsFormatIsReadNotMarkedStray)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    ASSERT_EQ(send_once(&e, 0, 0), kCountAf);
    EXPECT_EQ(answer(&e, 0, {0x59, 0x01, 0xFF, 0x00, 0x00, 0x04}, 4 * kMs),
              ecuio_reply::positive);
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.count_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_FORMAT, 0u);
    EXPECT_EQ(r.format, ECUIO_DTC_FORMAT_J2012);
    EXPECT_EQ(r.count, 4u);
    EXPECT_EQ(r.available, 0xFF);
    EXPECT_FALSE(e.chans[0].walk_stray);
    EXPECT_EQ(e.chans[0].codes.step, ECUIO_CODES_STEP_LIST);
}

TEST_F(ModuleCodesTest, TheListIsMatchedByServiceAndSubFunctionAlone)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    (void)send_once(&e, 0, now);
    (void)answer(&e, 0, codes_reply(kCountAf), now + 4 * kMs);
    now = std::max(now + 5 * kMs, e.chans[0].next_send_us);
    ASSERT_EQ(send_once(&e, 0, now), kListAf);
    EXPECT_EQ(answer(&e, 0, {0x59, 0x01, 0xFF, 0x00, 0x00, 0x04}, now + kMs),
              ecuio_reply::none)
        << "a count's reply is not the list's";
    EXPECT_EQ(answer(&e, 0, codes_reply(kListAf), now + 4 * kMs),
              ecuio_reply::positive);
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.list_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(r.held, 4u);
}

TEST_F(ModuleCodesTest, OtherOneShotsStillMatchTheirWholeRequest)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    ASSERT_EQ(send_once(&e, 0, 0), (Bytes{0x22, 0x1E, 0x1C}));
    EXPECT_EQ(answer(&e, 0, {0x62, 0x1E, 0x1D, 0x12}, 4 * kMs),
              ecuio_reply::none)
        << "byte 2 differs: another DID's";
    EXPECT_EQ(answer(&e, 0, {0x62, 0x1E, 0x1C, 0x12}, 5 * kMs),
              ecuio_reply::positive);
}

TEST_F(ModuleCodesTest, ARefusedCountStillAsksTheList)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    const Responder no_count = [](const Bytes& q) {
        return q == kCountAf ? Bytes{0x7F, 0x19, 0x12} : with_codes(q);
    };
    EXPECT_EQ(requests_of(drive(&e, &now, no_count)),
              (std::vector<Bytes>{kCountAf, kListAf}));
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.count_outcome, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(r.count_nrc, 0x12);
    EXPECT_EQ(r.flags & ECUIO_MODULE_CODES_FORMAT, 0u);
    EXPECT_EQ(r.list_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(r.held, 4u);
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_COMPLETE, 0u);
}

TEST_F(ModuleCodesTest, ARefusedListIsKeptAndDropsNothing)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t rpm[2] = {0x01, 0x0C};
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, rpm, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    e.probe = true;
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    const Responder refused = [](const Bytes& q) {
        return q == kListAf ? Bytes{0x7F, 0x19, 0x11} : with_codes(q);
    };
    (void)drive(&e, &now, refused);
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.list_outcome, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(r.list_nrc, 0x11);
    EXPECT_EQ(r.count_outcome, ECUIO_DIAG_ANSWERED);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].dropped, 0u) << i;
    }
    EXPECT_EQ(e.chans[0].requests, 2u);
}

TEST_F(ModuleCodesTest, AnUnansweredReadEndsAndARescanLeavesIt)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    const Responder silent = [](const Bytes& q) {
        return q[0] == 0x19 ? Bytes{} : fake_reply(q);
    };
    EXPECT_EQ(requests_of(drive(&e, &now, silent)),
              (std::vector<Bytes>{kCountAf, kListAf}));
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.count_outcome, ECUIO_DIAG_UNANSWERED);
    EXPECT_EQ(r.list_outcome, ECUIO_DIAG_UNANSWERED);
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_COMPLETE, 0u);
    EXPECT_EQ(r.flags & ECUIO_MODULE_CODES_READING, 0u);
    ecuio_engine_rescan(&e);
    for (int i = 0; i < 20; ++i) {
        now += ECUIO_ENGINE_WAIT_US;
        EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    }
}

TEST_F(ModuleCodesTest, ACallDuringAReadIsTakenWhenItEnds)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    (void)send_once(&e, 0, now);
    (void)answer(&e, 0, codes_reply(kCountAf), now + 4 * kMs);
    now = std::max(now + 5 * kMs, e.chans[0].next_send_us);
    ASSERT_EQ(send_once(&e, 0, now), kListAf);
    ecuio_engine_read_codes(&e, 0, 0xFF, 2);
    (void)answer(&e, 0, codes_reply(kListAf), now + 4 * kMs);
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_COMPLETE, 0u);
    EXPECT_EQ(r.read_id, 1u);
    EXPECT_EQ(r.mask, 0xAF);
    EXPECT_EQ(r.held, 4u);
    now = std::max(now + 5 * kMs, e.chans[0].next_send_us);
    EXPECT_EQ(requests_of(drive(&e, &now, with_codes)),
              (std::vector<Bytes>{{0x19, 0x01, 0xFF}, {0x19, 0x02, 0xFF}}));
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_COMPLETE, 0u);
    EXPECT_EQ(r.read_id, 2u);
    EXPECT_EQ(r.mask, 0xFF);
    EXPECT_EQ(r.held, 5u);
    EXPECT_EQ(r.generation, 6u);
}

TEST_F(ModuleCodesTest, TheReadGoesAfterAClearAndAheadOfIdentification)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    e.rpm_module = 0;
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    (void)drive(&e, &now, fake_reply);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_read_codes(&e, 0, 0, 1);
    ecuio_engine_identify(&e);
    const std::vector<Bytes> sent =
        requests_of(drive(&e, &now, stopped_with_codes));
    ASSERT_GE(sent.size(), 5u);
    EXPECT_EQ(sent[0], (Bytes{0x01, 0x0C}));
    EXPECT_EQ(sent[1], (Bytes{0x04}));
    EXPECT_EQ(sent[2], kCountAf);
    EXPECT_EQ(sent[3], kListAf);
    EXPECT_EQ(sent[4], (Bytes{0x09, 0x02}));
}

TEST_F(ModuleCodesTest, AClearReadsTheCodesAgain)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    e.rpm_module = 0;
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    (void)drive(&e, &now, with_codes);
    const uint32_t before = e.chans[0].codes.report.generation;
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    const std::vector<Bytes> sent =
        requests_of(drive(&e, &now, stopped_with_codes));
    ASSERT_GE(sent.size(), 5u);
    EXPECT_EQ(sent[1], (Bytes{0x04}));
    EXPECT_EQ(sent[2], kCountAf);
    EXPECT_EQ(sent[3], kListAf);
    EXPECT_EQ(sent[4], (Bytes{0x01, 0x01})) << "the J1979 list restarts";
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.read_id, 1u);
    EXPECT_EQ(r.generation, before + 3);
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_COMPLETE, 0u);
}

namespace {

/* An ECU with an extended session that answers 0x19 in it alone when
 * extended_only */
struct SessionEcu {
    bool extended = false;
    bool extended_only = false;
    int lose_once = 0;

    Bytes operator()(const Bytes& q)
    {
        if (q == Bytes{0x10, 0x03}) {
            extended = true;
            return {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == Bytes{0x3E, 0x00}) {
            return {0x7E, 0x00};
        }
        if (q.size() == 3 && q[0] == 0x22) {
            return {0x62, q[1], q[2], 0x12, 0x34};
        }
        if (!q.empty() && q[0] == 0x19) {
            if (lose_once > 0) {
                --lose_once;
                extended = false;
                return {0x7F, 0x19, 0x7E};
            }
            if (extended_only && !extended) {
                return {0x7F, 0x19, 0x7F};
            }
            return codes_reply(q);
        }
        return {0x7F, q.empty() ? uint8_t{0} : q[0], 0x11};
    }
};

std::vector<Bytes> steps(ecuio_request_engine* e, int64_t* now, SessionEcu& ecu,
                         int n)
{
    std::vector<Bytes> out;
    for (int i = 0; i < n; ++i) {
        const int32_t r = ecuio_engine_due(e, 0, *now);
        if (r == ECUIO_ENGINE_NONE) {
            *now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        const Bytes b(q.bytes, q.bytes + q.len);
        out.push_back(b);
        ecuio_engine_sent(e, 0, r, true, *now);
        const Bytes reply = ecu(b);
        *now += 4 * kMs;
        (void)ecuio_engine_on_frame(e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()), *now,
                                    nullptr);
        *now += ECUIO_ENGINE_GAP_US;
    }
    return out;
}

void session_engine(ecuio_request_engine* e)
{
    ecuio_engine_init(e, 1);
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(e, 0, did, 3), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_session(e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
}

size_t index_of(const std::vector<Bytes>& v, const Bytes& b, size_t from = 0)
{
    for (size_t i = from; i < v.size(); ++i) {
        if (v[i] == b) {
            return i;
        }
    }
    return v.size();
}

} // namespace

TEST_F(ModuleCodesTest, UnderTheSessionTheReadGoesAfterTheEntry)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(session_engine(&e));
    ecuio_engine_read_codes(&e, 0, 0, 1);
    SessionEcu ecu;
    ecu.extended_only = true;
    int64_t now = 0;
    const std::vector<Bytes> sent = steps(&e, &now, ecu, 8);
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(sent[0], (Bytes{0x10, 0x03}));
    EXPECT_LT(index_of(sent, kCountAf), index_of(sent, kListAf));
    EXPECT_EQ(e.chans[0].codes.report.list_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(e.chans[0].codes.report.held, 4u);
}

TEST_F(ModuleCodesTest, AReEntryAsksTheItemAgain)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(session_engine(&e));
    SessionEcu ecu;
    int64_t now = 0;
    (void)steps(&e, &now, ecu, 4);
    ASSERT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ecu.lose_once = 1;
    ecuio_engine_read_codes(&e, 0, 0, 1);
    const std::vector<Bytes> sent = steps(&e, &now, ecu, 8);
    const size_t first = index_of(sent, kCountAf);
    ASSERT_LT(first, sent.size());
    const size_t entry = index_of(sent, {0x10, 0x03}, first);
    ASSERT_LT(entry, sent.size()) << "the loss enters the session again";
    EXPECT_LT(index_of(sent, kCountAf, entry), sent.size())
        << "then the count again";
    const ecuio_module_codes_t& r = e.chans[0].codes.report;
    EXPECT_EQ(r.count_outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(r.list_outcome, ECUIO_DIAG_ANSWERED);
}

TEST_F(ModuleCodesTest, TheListCountsSegmentedAndTheCountOneFrame)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    int64_t now = 0;
    ASSERT_EQ(send_once(&e, 0, now), kCountAf);
    EXPECT_FALSE(ecuio_engine_reply_segmented(&e, 0, ECUIO_ENGINE_ONCE));
    (void)answer(&e, 0, codes_reply(kCountAf), now + 4 * kMs);
    now = std::max(now + 5 * kMs, e.chans[0].next_send_us);
    ASSERT_EQ(send_once(&e, 0, now), kListAf);
    EXPECT_TRUE(ecuio_engine_reply_segmented(&e, 0, ECUIO_ENGINE_ONCE));
}

TEST_F(ModuleCodesTest, AQuietModulesReadGoesAsAClearDoes)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    e.chans[1].quiet = true;
    ASSERT_TRUE(ecuio_engine_quiet_held(&e, e.chans[1]));
    e.chans[1].quiet_next_us = INT64_MAX / 2;
    ecuio_engine_read_codes(&e, 1, 0, 1);
    ecuio_engine_quiet_settle(&e, 1);
    EXPECT_EQ(e.chans[1].codes.step, ECUIO_CODES_STEP_COUNT);
    EXPECT_EQ(e.chans[1].codes.report.count_outcome, ECUIO_DIAG_UNASKED);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0, 1u), 0) << "channel 0 left out";
    EXPECT_EQ(send_once(&e, 1, 0), kCountAf) << "before the step";
}

TEST_F(ModuleCodesTest, TheGateTakesTheTwoReadsAlone)
{
    uint32_t allowed = 0;
    for (uint32_t sid = 0; sid < 256; ++sid) {
        for (uint32_t sub = 0; sub < 256; ++sub) {
            if (ecuio_engine_codes_request_allowed(static_cast<uint8_t>(sid),
                                                   static_cast<uint8_t>(sub))) {
                ++allowed;
                EXPECT_EQ(sid, 0x19u);
                EXPECT_TRUE(sub == 0x01 || sub == 0x02) << sub;
            }
        }
    }
    EXPECT_EQ(allowed, 2u);
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_read_codes(&e, 0, 0xFF, 1);
    int64_t now = 0;
    for (const Bytes& b : requests_of(drive(&e, &now, with_codes))) {
        EXPECT_TRUE(ecuio_engine_codes_request_allowed(b[0], b[1]));
    }
}

TEST_F(ModuleCodesTest, AClearWaitsForTheListOfACountAnswered)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    e.rpm_module = 0;
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    (void)drive(&e, &now, fake_reply);
    ecuio_engine_read_codes(&e, 0, 0, 1);
    EXPECT_EQ(e.chans[0].codes.report.flags, ECUIO_MODULE_CODES_READING);
    EXPECT_EQ(e.chans[0].codes.report.generation, 1u) << "the start is news";
    ASSERT_EQ(send_once(&e, 0, now), kCountAf);
    (void)answer(&e, 0, codes_reply(kCountAf), now + 4 * kMs);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    now = std::max(now + 5 * kMs, e.chans[0].next_send_us);
    const std::vector<Bytes> sent =
        requests_of(drive(&e, &now, stopped_with_codes));
    ASSERT_GE(sent.size(), 5u);
    EXPECT_EQ(sent[0], kListAf) << "the list before the clear";
    EXPECT_EQ(sent[1], (Bytes{0x01, 0x0C}));
    EXPECT_EQ(sent[2], (Bytes{0x04}));
    EXPECT_EQ(sent[3], kCountAf) << "then read again whole";
    EXPECT_EQ(sent[4], kListAf);
}
