// File/layer: tests/test_conversation_requests.cpp | test_obd2, Qt-free
// Testing: UNIT: ConversationTest (one module's conversation over a
//          scripted table, the engine and the strict simulated ECU
//          stepped with the time passed in): the module's requests, its
//          faults, begin, the poll's pace and the end

#include "conversation_support.h"

using namespace conversation_run;
using conv_fault = ecuio_conversation_fault;

namespace {

/* After a fault: the module is never called again, nothing of it went on
 * the wire, and the session core entered stays with the rotation reading
 * on */
void expect_faulted(Bench& b, conv_fault f)
{
    EXPECT_EQ(b.v->report.state, ecuio_conversation_state::faulted);
    EXPECT_EQ(b.v->report.fault, f);
    const int polls = b.guest.polls;
    const size_t accepted = b.guest.accepted.size();
    const size_t before = b.sent.size();
    b.passes(12);
    EXPECT_EQ(b.guest.polls, polls);
    EXPECT_EQ(b.guest.accepted.size(), accepted);
    EXPECT_FALSE(ecuio_conversation_unlock(b.v.get(), 0x01));
    EXPECT_EQ(b.e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_GE(
        engine_talk_test::count_of(
            std::vector<Bytes>(b.sent.begin() + before, b.sent.end()), kRow),
        1u);
    EXPECT_EQ(ecuio_conversation_next_us(b.v.get(), b.e.get(), b.now),
              INT64_MAX);
    ecuio_conversation_end(b.v.get());
    EXPECT_EQ(b.guest.ends, 0);
}

Bytes long_request(size_t n)
{
    Bytes q = {0x22, 0xD3};
    q.resize(n, 0x00);
    return q;
}

} // namespace

/* The module's requests go one at a time as plain talks, each answered
 * whole to accept as a reply, a refusal as its 7F, silence as a timeout */
TEST(ConversationTest, EachRequestIsAnsweredToTheModuleWhole)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const Bytes found = {0x22, 0xD3, 0x01};
    const Bytes denied = kGated; /* 7F 22 33 while locked */
    const Bytes silent = {0x22, 0xD3, 0x02};
    const Bytes code = {0x19, 0x02, 0xFF};
    b.ecu.silent = silent;
    b.guest.requests = {found, denied, silent, code};
    b.passes(30);
    EXPECT_EQ(heard(b, found), 1u);
    EXPECT_EQ(heard(b, denied), 1u);
    EXPECT_EQ(heard(b, silent), 1u);
    EXPECT_EQ(heard(b, code), 1u);
    EXPECT_EQ(b.v->report.requests, 4u);
    std::vector<Guest::Accepted> want = {
        {ECUIO_GUEST_REPLY, {0x62, 0xD3, 0x01, 0x12, 0x34}},
        {ECUIO_GUEST_REPLY, {0x7F, 0x22, 0x33}},
        {ECUIO_GUEST_TIMEOUT, {}},
        {ECUIO_GUEST_REPLY, {0x7F, 0x19, 0x11}},
    };
    ASSERT_EQ(b.guest.accepted.size(), want.size());
    for (size_t k = 0; k < want.size(); ++k) {
        EXPECT_EQ(b.guest.accepted[k].kind, want[k].kind) << k;
        EXPECT_EQ(b.guest.accepted[k].bytes, want[k].bytes) << k;
    }
    EXPECT_EQ(b.v->report.state, ecuio_conversation_state::talking);
}

/* The reply kept is the frame that settled the talk: not a pending 78
 * before it, nor a frame after it */
TEST(ConversationTest, TheReplyKeptIsTheFrameThatSettledTheTalk)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const Bytes read = {0x22, 0xD3, 0x05};
    b.pending_on = read;
    b.guest.requests = {read};
    for (int i = 0; i < 6 && b.guest.accepted.empty(); ++i) {
        b.pass();
    }
    const std::vector<Bytes> first = {{0x62, 0xD3, 0x05, 0x12, 0x34}};
    ASSERT_EQ(b.guest.of_kind(ECUIO_GUEST_REPLY), first);

    const Bytes next = {0x22, 0xD3, 0x06};
    b.guest.requests = {next};
    ASSERT_NO_FATAL_FAILURE(b.until_offered());
    ASSERT_EQ(engine_talk_test::next_once(b.e.get(), &b.now, &b.ecu),
              ECUIO_ENGINE_ONCE);
    ASSERT_EQ(engine_talk_test::wire(b.e.get(), 0, ECUIO_ENGINE_ONCE), next);
    ecuio_engine_sent(b.e.get(), 0, ECUIO_ENGINE_ONCE, true, b.now);
    b.now += 4 * kMs;
    b.frame({0x62, 0xD3, 0x06, 0xAB});
    b.frame({0x62, 0x1E, 0x1C, 0x99, 0x99}); /* late, unasked */
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    const std::vector<Bytes> replies = b.guest.of_kind(ECUIO_GUEST_REPLY);
    ASSERT_EQ(replies.size(), 2u);
    EXPECT_EQ(replies[1], (Bytes{0x62, 0xD3, 0x06, 0xAB}));
}

/* A request outside the rules is a fault, judged in turn: the channel's
 * bound, the guest's list, the manifest, then the engine's door */
TEST(ConversationTest, EachRequestOutsideTheRulesIsAFault)
{
    struct Row {
        const char* what;
        Bytes q;
        conv_fault f;
    };
    const Row rows[] = {
        {"a seed", {0x27, 0x01}, conv_fault::service},
        {"a key", {0x27, 0x02, 0x00, 0x00, 0x00, 0x00}, conv_fault::service},
        {"the session", {0x10, 0x03}, conv_fault::service},
        {"the keep-alive", {0x3E, 0x00}, conv_fault::service},
        {"a periodic read", {0x2A, 0x01, 0x10}, conv_fault::service},
        {"a write", {0x2E, 0xF1, 0x90, 0x00}, conv_fault::service},
        {"a clear", {0x14, 0xFF, 0xFF, 0xFF}, conv_fault::service},
        /* tier 3's reads are core's own, never a module's */
        {"a memory read",
         {0x23, 0x24, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00},
         conv_fault::service},
        {"an upload", {0x35, 0x00, 0x44}, conv_fault::service},
        {"a transfer", {0x36, 0x01}, conv_fault::service},
        {"a transfer's exit", {0x37}, conv_fault::service},
        {"2C outside the ceiling", {0x2C, 0x02, 0xF3}, conv_fault::service},
        {"2C 03, not declared",
         {0x2C, 0x03, 0xF3, 0x00},
         conv_fault::undeclared},
        {"19 0A, not declared", {0x19, 0x0A}, conv_fault::undeclared},
        {"19 alone", {0x19}, conv_fault::undeclared},
        {"22 of one byte", {0x22}, conv_fault::door},
        {"22 of two bytes", {0x22, 0xD3}, conv_fault::door},
        {"past the bound", long_request(ECUIO_ENGINE_PDU_MAX + 1),
         conv_fault::bound},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.what);
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.guest.requests = {row.q};
        b.until_faulted();
        EXPECT_EQ(heard(b, row.q), 0u);
        const size_t n = row.q.size() < 4 ? row.q.size() : 4;
        EXPECT_EQ(b.v->report.fault_len, n);
        EXPECT_EQ(Bytes(b.v->report.fault_bytes, b.v->report.fault_bytes + n),
                  Bytes(row.q.begin(), row.q.begin() + static_cast<long>(n)));
        expect_faulted(b, row.f);
    }
}

/* A service the manifest doesn't declare is refused whatever the ceiling
 * allows; the bound is the transport's (one frame on ELM327) */
TEST(ConversationTest, TheManifestAndTheTransportBoundTheRequests)
{
    Bench b;
    b.services = {service(0x19, {0x02})};
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.guest.requests = {{0x22, 0xD3, 0x00}};
    b.until_faulted();
    expect_faulted(b, conv_fault::undeclared);

    Bench elm;
    ASSERT_NO_FATAL_FAILURE(elm.start());
    elm.e->pdu_max = ECUIO_ENGINE_PDU_SINGLE;
    elm.guest.requests = {long_request(7), long_request(8)};
    elm.until_faulted();
    EXPECT_EQ(heard(elm, long_request(7)), 1u);
    expect_faulted(elm, conv_fault::bound);
}

/* A length outside the exchange buffer, and each fault of a call, end the
 * conversation */
TEST(ConversationTest, ALengthOutsideTheBufferOrACallsFaultEndsIt)
{
    for (const int32_t len :
         {-1, static_cast<int32_t>(ECUIO_GUEST_BUFFER_BYTES) + 1}) {
        SCOPED_TRACE(len);
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.guest.raw = true;
        b.guest.raw_length = len;
        b.until_faulted();
        EXPECT_EQ(b.v->report.fault_len, 0u);
        expect_faulted(b, conv_fault::length);
    }
    const std::pair<ecuio_guest_fault, conv_fault> calls[] = {
        {ecuio_guest_fault::budget, conv_fault::budget},
        {ecuio_guest_fault::trap, conv_fault::trap},
        {ecuio_guest_fault::buffer, conv_fault::buffer},
    };
    for (const auto& c : calls) {
        SCOPED_TRACE(static_cast<int>(c.first));
        Bench next;
        ASSERT_NO_FATAL_FAILURE(next.start());
        next.guest.next_fault = c.first;
        next.until_faulted();
        expect_faulted(next, c.second);

        Bench reply;
        ASSERT_NO_FATAL_FAILURE(reply.start());
        reply.guest.fault_kind = ECUIO_GUEST_REPLY;
        reply.guest.accept_fault = c.first;
        reply.guest.requests = {{0x22, 0xD3, 0x00}};
        reply.until_faulted();
        EXPECT_EQ(reply.guest.of_kind(ECUIO_GUEST_REPLY).size(), 1u);
        expect_faulted(reply, c.second);
    }
}

/* A fault with the seed ends the attempt with nothing counted: no key
 * goes, the window ends; so does a key of the wrong length */
TEST(ConversationTest, AFaultWithTheSeedSendsNoKeyAndCountsNothing)
{
    Bench trap;
    ASSERT_NO_FATAL_FAILURE(trap.start());
    trap.guest.fault_kind = ECUIO_GUEST_SEED;
    trap.guest.accept_fault = ecuio_guest_fault::trap;
    ASSERT_TRUE(ecuio_conversation_unlock(trap.v.get(), 0x01));
    trap.until_word_ended();
    EXPECT_EQ(trap.v->report.unlock, ecuio_unlock_state::ended);
    EXPECT_EQ(trap.e->chans[0].key.until_us, 0);
    EXPECT_EQ(trap.lvl(0x01).failed, 0u);
    EXPECT_EQ(heard(trap, engine_talk_test::key_for(0x01)), 0u);
    expect_faulted(trap, conv_fault::trap);

    for (const int32_t len : {3, 5, -1}) {
        SCOPED_TRACE(len);
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.guest.key_len = len;
        ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
        b.until_word_ended();
        EXPECT_EQ(b.v->report.words_ended, 1u);
        EXPECT_EQ(b.e->chans[0].key.until_us, 0);
        EXPECT_EQ(b.lvl(0x01).failed, 0u);
        for (const Bytes& q : b.sent) {
            EXPECT_FALSE(q.size() >= 2 && q[0] == 0x27 && q[1] == 0x02);
        }
        expect_faulted(b, conv_fault::key);
    }
}

/* begin's answer: 0 the default session (no 10 03), 3 the extended one
 * entered, a negative declines (data only, end still called), anything
 * else or a call's fault is a fault (end never called) */
TEST(ConversationTest, BeginsAnswerSaysTheSession)
{
    Bench def;
    def.guest.begin_answer = 0;
    ASSERT_NO_FATAL_FAILURE(def.start());
    def.guest.requests = {{0x22, 0xD3, 0x00}};
    def.passes(8);
    EXPECT_EQ(def.v->report.state, ecuio_conversation_state::talking);
    EXPECT_EQ(def.v->report.session, 0);
    EXPECT_EQ(heard(def, {0x10, 0x03}), 0u);
    EXPECT_EQ(heard(def, {0x22, 0xD3, 0x00}), 1u);
    EXPECT_EQ(def.e->chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);

    Bench ext;
    ASSERT_NO_FATAL_FAILURE(ext.start());
    EXPECT_EQ(ext.v->report.session, 3);
    EXPECT_EQ(ext.e->chans[0].session_entries, 1u);

    Bench declined;
    declined.guest.begin_answer = -1;
    ASSERT_NO_FATAL_FAILURE(declined.start());
    declined.passes(8);
    EXPECT_EQ(declined.v->report.state, ecuio_conversation_state::declined);
    EXPECT_EQ(declined.guest.polls, 0);
    EXPECT_FALSE(ecuio_conversation_unlock(declined.v.get(), 0x01));
    ecuio_conversation_end(declined.v.get());
    EXPECT_EQ(declined.guest.ends, 1);

    for (const int32_t answer : {1, 2, 7}) {
        SCOPED_TRACE(answer);
        Bench odd;
        odd.guest.begin_answer = answer;
        ASSERT_NO_FATAL_FAILURE(odd.start());
        EXPECT_EQ(odd.v->report.state, ecuio_conversation_state::faulted);
        EXPECT_EQ(odd.v->report.fault, conv_fault::begin);
        EXPECT_EQ(odd.e->chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
        ecuio_conversation_end(odd.v.get());
        EXPECT_EQ(odd.guest.ends, 0);
    }

    Bench trapped;
    trapped.guest.begin_fault = ecuio_guest_fault::trap;
    ASSERT_NO_FATAL_FAILURE(trapped.start());
    EXPECT_EQ(trapped.v->report.fault, conv_fault::trap);
    EXPECT_EQ(trapped.e->chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    EXPECT_EQ(trapped.guest.begins, 1);
}

/* A channel the engine lacks is a fault at begin: the module never runs,
 * its end isn't called, and nothing reaches the engine */
TEST(ConversationTest, ABeginOnAChannelTheEngineLacksIsAFault)
{
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(engine_talk_test::engine(e.get()));
    Guest g;
    auto v = std::make_unique<ecuio_conversation>();
    const auto services = default_services();
    const auto levels = default_levels();
    ASSERT_TRUE(ecuio_conversation_init(v.get(), ops_of(&g), 1, kRx, kTx,
                                        services.data(), 4, levels.data(), 2));
    ecuio_conversation_begin(v.get(), e.get(), 0);
    EXPECT_EQ(v->report.state, ecuio_conversation_state::faulted);
    EXPECT_EQ(v->report.fault, conv_fault::channel);
    EXPECT_EQ(g.begins, 0);
    ecuio_conversation_step(v.get(), e.get(), 0);
    EXPECT_EQ(ecuio_conversation_next_us(v.get(), e.get(), 0), INT64_MAX);
    ecuio_conversation_end(v.get());
    EXPECT_EQ(g.polls, 0);
    EXPECT_EQ(g.ends, 0);
    EXPECT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
}

/* While the module has nothing, it is asked at most once each
 * ECUIO_CONVERSATION_POLL_US; with a request, at once after its reply */
TEST(ConversationTest, ThePollIsPacedWhileTheModuleHasNothing)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const int before = b.guest.polls;
    const int64_t from = b.now;
    while (b.now - from < 1000 * kMs) {
        b.pass();
    }
    const int64_t spent = b.now - from;
    const int polls = b.guest.polls - before;
    EXPECT_LE(polls, spent / ECUIO_CONVERSATION_POLL_US + 1);
    EXPECT_GE(polls, spent / ECUIO_CONVERSATION_POLL_US / 2);
    const int64_t next =
        ecuio_conversation_next_us(b.v.get(), b.e.get(), b.now);
    EXPECT_GT(next, b.now - 1);
    EXPECT_LE(next, b.now + ECUIO_CONVERSATION_POLL_US);

    /* a module with requests is asked again once each has settled */
    for (uint8_t k = 0; k < 8; ++k) {
        b.guest.requests.push_back({0x22, 0xD3, k});
    }
    const int64_t start = b.now;
    for (int i = 0; i < 40 && !b.guest.requests.empty(); ++i) {
        b.pass();
    }
    EXPECT_TRUE(b.guest.requests.empty());
    EXPECT_LT(b.now - start, 8 * ECUIO_CONVERSATION_POLL_US);
}

/* Under an operation's hold the module is asked for no request, so none
 * of its reads goes between a run's; a word (the run's own unlock) still
 * runs, and the module's request goes once the hold ends */
TEST(ConversationTest, TheModuleIsAskedNothingUnderAnOperationsHold)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const Bytes q = {0x22, 0xD3, 0x05};
    b.e->operation_hold = true;
    b.guest.requests = {q};
    const int polls = b.guest.polls;
    const int64_t from = b.now;
    while (b.now - from < 1000 * kMs) {
        b.pass();
    }
    EXPECT_EQ(b.guest.polls, polls);
    EXPECT_EQ(heard(b, q), 0u);
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, ecuio_unlock_state::unlocked);
    EXPECT_EQ(heard(b, q), 0u);

    b.e->operation_hold = false;
    for (int i = 0; i < 40 && !b.guest.requests.empty(); ++i) {
        b.pass();
    }
    b.passes(4);
    EXPECT_EQ(heard(b, q), 1u);
}

/* What the next step is due for: now with a word or a settled talk,
 * never while the talk is out */
TEST(ConversationTest, TheNextStepIsDueWhenThereIsWorkForIt)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    EXPECT_EQ(ecuio_conversation_next_us(b.v.get(), b.e.get(), b.now), b.now);
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    EXPECT_EQ(ecuio_conversation_next_us(b.v.get(), b.e.get(), b.now),
              INT64_MAX);
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, ecuio_unlock_state::unlocked);
}

/* end is called once at the close, never before begin or after a fault */
TEST(ConversationTest, EndIsCalledOnce)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ecuio_conversation_end(b.v.get());
    ecuio_conversation_end(b.v.get());
    EXPECT_EQ(b.guest.ends, 1);
    EXPECT_EQ(b.v->report.state, ecuio_conversation_state::ended);
    EXPECT_FALSE(ecuio_conversation_unlock(b.v.get(), 0x01));
    const int polls = b.guest.polls;
    b.passes(4);
    EXPECT_EQ(b.guest.polls, polls);

    Guest g;
    auto v = std::make_unique<ecuio_conversation>();
    const auto services = default_services();
    ASSERT_TRUE(ecuio_conversation_init(v.get(), ops_of(&g), 0, kRx, kTx,
                                        services.data(), 4, nullptr, 0));
    ecuio_conversation_end(v.get());
    EXPECT_EQ(g.ends, 0);
}

/* The table is checked whole at init: every function, the lists' bounds,
 * each level fitting the machine; levels of other modules are left out */
TEST(ConversationTest, InitTakesTheModulesOwnLevelsAndAWholeTable)
{
    Guest g;
    auto v = std::make_unique<ecuio_conversation>();
    const auto services = default_services();
    const std::vector<ecuio_plugin_security_level_t> levels = {
        level(0x01, 4), level(0x01, 4, 0x7E9), level(0x03, 16)};
    ASSERT_TRUE(ecuio_conversation_init(v.get(), ops_of(&g), 0, kRx, kTx,
                                        services.data(), 4, levels.data(), 3));
    EXPECT_EQ(v->access.count, 2u);
    EXPECT_EQ(v->access.levels[1].level, 0x03);
    EXPECT_EQ(v->access.levels[1].window_us, 1000 * kMs);

    ecuio_conversation_ops none = ops_of(&g);
    none.end = nullptr;
    EXPECT_FALSE(ecuio_conversation_init(v.get(), none, 0, kRx, kTx,
                                         services.data(), 4, levels.data(), 3));
    EXPECT_FALSE(ecuio_conversation_init(
        v.get(), ops_of(&g), 0, kRx, kTx, services.data(),
        ECUIO_PLUGIN_MAX_SERVICES + 1, levels.data(), 3));
    const std::vector<ecuio_plugin_security_level_t> twice = {level(0x01, 4),
                                                              level(0x01, 4)};
    EXPECT_FALSE(ecuio_conversation_init(v.get(), ops_of(&g), 0, kRx, kTx,
                                         services.data(), 4, twice.data(), 2));

    /* a key inside one PDU, a window the door takes: the edges and past */
    struct Edge {
        uint8_t key_bytes;
        uint32_t window_ms;
        bool taken;
    };
    for (const Edge& edge :
         {Edge{58, 50, true}, Edge{58, 4000, true}, Edge{59, 1000, false},
          Edge{4, 49, false}, Edge{4, 4001, false}}) {
        SCOPED_TRACE(testing::Message() << int{edge.key_bytes} << " bytes, "
                                        << edge.window_ms << " ms");
        ecuio_plugin_security_level_t l = level(0x01, 4);
        l.key_bytes = edge.key_bytes;
        l.key_window_ms = edge.window_ms;
        EXPECT_EQ(ecuio_conversation_init(v.get(), ops_of(&g), 0, kRx, kTx,
                                          services.data(), 4, &l, 1),
                  edge.taken);
        l.rx_id = 0x7E9; /* another module's: not this one's to judge */
        EXPECT_TRUE(ecuio_conversation_init(v.get(), ops_of(&g), 0, kRx, kTx,
                                            services.data(), 4, &l, 1));
    }
}
