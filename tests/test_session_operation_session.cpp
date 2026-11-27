#include "session_operation_support.h"

#include <ecuio/dump.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using engine_talk_test::Bytes;
using session_operation::Answer;
using session_operation::Bench;
using session_operation::made;
using session_operation::PlanShape;
using sim_memory_test::nrc;

constexpr uint32_t kCodeAt = ECUIO_SIM_MEMORY_CODE_ADDRESS;
constexpr uint32_t kCodeBytes = 2048;

/* A plan read in the programming session: region B's first bytes */
PlanShape programming_plan(const std::string& extra = "")
{
    PlanShape p;
    p.session = "programming";
    p.address = "00080000";
    p.upload = false;
    p.extra = extra;
    return p;
}

/* A module whose begin leaves it in the default session */
conversation_test::Made in_default(const PlanShape& plan)
{
    session_operation::GuestShape g;
    g.begin = 0;
    return made(g, plan);
}

bool has(const std::vector<Bytes>& sent, const Bytes& q)
{
    for (const Bytes& s : sent) {
        if (s == q) {
            return true;
        }
    }
    return false;
}

/* The last session request (10 or 11) sent */
Bytes last_session(const std::vector<Bytes>& sent)
{
    for (auto i = sent.rbegin(); i != sent.rend(); ++i) {
        if (!i->empty() && ((*i)[0] == 0x10 || (*i)[0] == 0x11)) {
            return *i;
        }
    }
    return {};
}

TEST(SessionOperationTest, AProgrammingPlanFromDefaultNeverSends1003)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(in_default(programming_plan())));
    EXPECT_FALSE(b.node.s.extended);
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_FALSE(has(b.sent, Bytes{0x10, 0x03}))
        << "the plan's steps decide the path: 10 02 straight from default";
    EXPECT_TRUE(has(b.sent, Bytes{0x10, 0x02}));
    EXPECT_EQ(last_session(b.sent), (Bytes{0x10, 0x01}));
    EXPECT_FALSE(b.node.s.programming) << "the run left its session";
    EXPECT_FALSE(b.node.s.extended);

    const std::vector<uint8_t> image =
        session_operation::image_at(b.folder.at(v.name));
    EXPECT_EQ(image, b.region(kCodeAt, kCodeBytes));
    const dump_file_test::Dump copy =
        dump_file_test::open_dump(b.folder.at(v.name));
    ASSERT_NE(copy, nullptr);
    ecuio_dump_range_t g{};
    g.struct_size = sizeof g;
    ASSERT_EQ(ecuio_dump_get_range(copy.get(), 0, &g), ECUIO_OK);
    EXPECT_EQ(g.session, ECUIO_DUMP_SESSION_PROGRAMMING)
        << "each range keeps its step's session";
}

TEST(SessionOperationTest, AProgrammingPlanLeavesByTheResetItNames)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(
        b.start(in_default(programming_plan(R"("leave": "reset", )"))));
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(last_session(b.sent), (Bytes{0x11, 0x01}));
    EXPECT_FALSE(has(b.sent, Bytes{0x10, 0x01}));
    EXPECT_FALSE(b.node.s.programming);
}

TEST(SessionOperationTest, ACancelInTheProgrammingSessionLeavesIt)
{
    Bench b;
    PlanShape plan = programming_plan();
    plan.ident = false; /* a run's own reads alone, past the cancel */
    ASSERT_NO_FATAL_FAILURE(b.start(in_default(plan)));
    for (uint16_t did = 0xD300; did < 0xD308; ++did) {
        ASSERT_TRUE(ecuio_engine_scan_add(b.e.get(), 0, did)); /* held */
    }
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    b.until_phase(ECUIO_OPERATION_PHASE_READING);
    ASSERT_TRUE(b.node.s.programming);
    ASSERT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
    const size_t canceled = b.sent.size();
    b.until_phase(ECUIO_OPERATION_PHASE_LEAVING);
    EXPECT_EQ(b.state().phase, ECUIO_OPERATION_PHASE_LEAVING);
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED) << v.words;
    EXPECT_EQ(last_session(b.sent), (Bytes{0x10, 0x01}));
    for (size_t i = canceled; i < b.sent.size(); ++i) {
        if (b.sent[i] == Bytes{0x10, 0x01}) {
            break;
        }
        EXPECT_TRUE(b.sent[i][0] == 0x23 || b.sent[i][0] == 0x3E)
            << "the hold stands until the session is left: "
            << static_cast<int>(b.sent[i][0]);
    }
    EXPECT_FALSE(b.node.s.programming);
    EXPECT_EQ(b.files(), std::vector<std::string>{});
}

TEST(SessionOperationTest, AProgrammingSessionLostIsNeverEnteredAgain)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(in_default(programming_plan())));
    size_t reads = 0;
    b.script = [&reads](const Bytes& q, size_t, Answer* a) {
        if (q[0] == 0x23 && ++reads == 2) {
            a->frames = {nrc(0x23, 0x7F)};
            return true;
        }
        return false;
    };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SESSION_LOST) << v.words;
    b.passes(8);
    EXPECT_FALSE(has(b.sent, Bytes{0x10, 0x03}))
        << "the engine enters nothing of its own into a run's session";
    EXPECT_EQ(b.count(0x10), 2u) << "10 02, then the leave's 10 01";
    EXPECT_EQ(b.files(), std::vector<std::string>{});
}

TEST(SessionOperationTest, AProgrammingEntryRefusedEndsTheRunUnread)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(in_default(programming_plan())));
    b.script = [](const Bytes& q, size_t, Answer* a) {
        if (q == Bytes{0x10, 0x02}) {
            a->frames = {nrc(0x10, 0x22)};
            return true;
        }
        return false;
    };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_REFUSED) << v.words;
    EXPECT_EQ(v.nrc, 0x22);
    EXPECT_EQ(b.count(0x27), 0u);
    EXPECT_EQ(b.count(0x23), 0u);
    EXPECT_EQ(b.count(0x10), 1u) << "a session never entered isn't left";
}

/* A level open in the extended session is locked by the programming
 * session: the start wants the unlock allowed, and the run asks it there */
TEST(SessionOperationTest, AProgrammingPlanUnlocksInItsOwnSession)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(made({}, programming_plan())));
    ecuio_bound_channel* bound = ecuio_bound_channel_of(b.s, 0);
    ecuio_security::module_access& access = bound->conversation.access;
    const uint32_t k = ecuio_security::find_level(access, 0x01);
    ASSERT_LT(k, access.count);
    access.levels[k].unlocked = true;
    bound->conversation.report.level = 0x01;
    bound->conversation.report.unlock = ecuio_unlock_state::unlocked;
    bound->published = bound->conversation.report;
    b.node.s.security.unlocked = 0x01;
    EXPECT_EQ(b.begin(0), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "level 01 is unlocked in the programming session the run "
                 "enters: start it with the unlock allowed");
    const size_t from = b.sent.size();
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(b.count(0x27, from), 2u) << "the seed and the key, in 02";
    const dump_file_test::Dump copy =
        dump_file_test::open_dump(b.folder.at(v.name));
    ASSERT_NE(copy, nullptr);
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_dump_get_info(copy.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.unlocks, 1u);
}

TEST(SessionOperationTest, AnExtendedConversationGoesBackToItsSessionAfter)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(made({}, programming_plan())));
    ASSERT_TRUE(b.node.s.extended);
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    b.passes(8);
    size_t left = 0;
    for (size_t i = 0; i < b.sent.size(); ++i) {
        left = b.sent[i] == Bytes{0x10, 0x01} ? i : left;
    }
    ASSERT_GT(left, 0u);
    bool entered = false;
    for (size_t i = left; i < b.sent.size(); ++i) {
        entered = entered || b.sent[i] == Bytes{0x10, 0x03};
    }
    EXPECT_TRUE(entered) << "the module's own session is entered again";
    EXPECT_TRUE(b.node.s.extended);
}

} // namespace
