// File/layer: tests/test_session_operation_ends.cpp | test, C++17, Qt-free
// Testing: UNIT: SessionOperationTest (test_core)
//
// How a run ends when it keeps no copy (ecuio/operation.h): the unlock
// it makes or is refused, each fault of the module's, a cancel and a
// close in each phase. Every such end leaves the folder empty. And what
// the worker and the start cost: no allocation for a chunk, and a start
// that runs out of memory leaves nothing.

#include "session_operation_support.h"

#include <ecuio/dump.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* test_core.cpp's, over tests/new_counter.h */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace {

using engine_talk_test::Bytes;
using session_operation::Answer;
using session_operation::Bench;
using session_operation::kHighAt;
using session_operation::kHighBytes;
using session_operation::kLowAt;
using session_operation::kLowBytes;
using session_operation::made;
using sim_memory_test::nrc;

const std::vector<std::string> kNoFiles;

ecuio_channel_conversation_t conversation_of(ecuio_session_t* s)
{
    ecuio_channel_conversation_t c{};
    EXPECT_EQ(ecuio_session_get_channel_conversation(s, 0, &c), ECUIO_OK);
    return c;
}

std::string text_of(const std::string& path)
{
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char b[4096];
        size_t n = 0;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) {
            out.append(b, n);
        }
        std::fclose(f);
    }
    return out;
}

struct TempStore {
    std::string path;
    explicit TempStore(const char* name)
        : path(testing::TempDir() + std::string(name))
    {
        std::remove(path.c_str());
    }
    ~TempStore()
    {
        std::remove(path.c_str());
        std::remove((path + ".lock").c_str());
    }
};

/* The kept copy's facts */
ecuio_dump_info_t info_of(const Bench& b, const ecuio_operation_state_t& v)
{
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    const dump_file_test::Dump copy =
        dump_file_test::open_dump(b.folder.at(v.name));
    EXPECT_NE(copy, nullptr);
    if (copy != nullptr) {
        EXPECT_EQ(ecuio_dump_get_info(copy.get(), &info), ECUIO_OK);
    }
    return info;
}

/* Level 01 unlocked by the session's own call, as a user's */
void unlock_01(Bench& b)
{
    ASSERT_EQ(ecuio_session_security_unlock(
                  b.s, 0, 0x01, conversation_of(b.s).generation, nullptr),
              ECUIO_OK)
        << ecuio_last_error_detail();
    for (int i = 0;
         i < 60 && conversation_of(b.s).unlock != ECUIO_UNLOCK_UNLOCKED; ++i) {
        b.pass();
    }
    ASSERT_EQ(conversation_of(b.s).unlock, ECUIO_UNLOCK_UNLOCKED);
}

/* A level open already costs no attempt, with the flag or without */
TEST(SessionOperationTest, AnOpenLevelCostsNoAttempt)
{
    for (const uint32_t flags : {0u, ECUIO_OPERATION_MAY_UNLOCK}) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        ASSERT_NO_FATAL_FAILURE(unlock_01(b));
        const size_t from = b.sent.size();
        ASSERT_EQ(b.begin(flags), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_EQ(b.count(0x27, from), 0u) << "no seed for an open level";
        const dump_file_test::Dump copy =
            dump_file_test::open_dump(b.folder.at(v.name));
        ASSERT_NE(copy, nullptr);
        ecuio_dump_info_t info{};
        info.struct_size = sizeof info;
        ASSERT_EQ(ecuio_dump_get_info(copy.get(), &info), ECUIO_OK);
        EXPECT_EQ(info.unlocks, 0u);
        EXPECT_EQ(conversation_of(b.s).words_ended, 1u)
            << "no word of the run's";
    }
}

/* A level the module itself answers as open (a seed of zeroes) is
 * entered with no key sent: the seed's request is the attempt counted */
TEST(SessionOperationTest, ASeedOfZeroesOpensTheLevelWithNoKey)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.node.s.security.unlocked = 0x01; /* the session never saw it open */
    const size_t from = b.sent.size();
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(b.count(0x27, from), 1u) << "the seed alone";
    EXPECT_EQ(info_of(b, v).unlocks, 1u);
}

/* A level the worker's side holds open by the time the run's word is
 * given (the start judged a report a pass old) is entered with nothing
 * sent and nothing counted */
TEST(SessionOperationTest, AWordRefusedForAnOpenLevelCountsNoAttempt)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_security::module_access& access =
        ecuio_bound_channel_of(b.s, 0)->conversation.access;
    const uint32_t k = ecuio_security::find_level(access, 0x01);
    ASSERT_LT(k, access.count);
    access.levels[k].unlocked = true;
    b.node.s.security.unlocked = 0x01;
    const size_t from = b.sent.size();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(b.count(0x27, from), 0u);
    EXPECT_EQ(info_of(b, v).unlocks, 0u);
}

/* The record's VIN is the session's, when it is one a record takes */
TEST(SessionOperationTest, TheRecordsVinIsTheSessionsWhenARecordTakesIt)
{
    const struct {
        const char* answered; /* 09 02's text, NULL for none */
        const char* kept;
    } rows[] = {
        {"ZZZFAKE11M0000001", "ZZZFAKE11M0000001"},
        {"ZZZFAKE11MQ000001", ""}, /* no VIN holds a Q */
        {nullptr, ""},
    };
    for (const auto& row : rows) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        if (row.answered != nullptr) {
            ecuio_channel_identity_t& id = b.s->identity[0];
            ecuio_identity_item_t& vin = id.list[id.items++];
            const uint8_t request[3] = {0x09, 0x02, 0x00};
            std::memcpy(vin.request, request, 3);
            vin.length = 2;
            vin.outcome = ECUIO_IDENTITY_ANSWERED;
            std::snprintf(vin.text, sizeof vin.text, "%s", row.answered);
            vin.text_length = 17;
        }
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_STREQ(info_of(b, v).vin, row.kept);
    }
}

/* A level the start found open and the worker finds locked again is
 * not opened without the flag: nothing is read */
TEST(SessionOperationTest, ALevelLockedAgainBeforeTheRunIsNotEntered)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_NO_FATAL_FAILURE(unlock_01(b));
    ASSERT_EQ(b.begin(0), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_security::module_access& access =
        ecuio_bound_channel_of(b.s, 0)->conversation.access;
    const uint32_t k = ecuio_security::find_level(access, 0x01);
    ASSERT_LT(k, access.count);
    access.levels[k].unlocked = false;
    b.node.s.security.unlocked = 0;
    const size_t from = b.sent.size();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_UNLOCK_REFUSED) << v.words;
    EXPECT_STREQ(v.words, "the module's level stayed locked: nothing was read");
    EXPECT_EQ(b.count(0x27, from) + b.count(0x23, from), 0u);
    EXPECT_EQ(b.files(), kNoFiles);
}

/* Another level's unlock opens nothing for the plan's */
TEST(SessionOperationTest, AnotherLevelsUnlockIsNotThePlans)
{
    session_operation::PlanShape plan;
    plan.level = "03";
    conversation_test::Made m = made({}, plan);
    m.group.security =
        R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4, "key_bytes": 4, )"
        R"("delay_ms": 1000, "key_window_ms": 1000}, {"ecu": "pcm", )"
        R"("level": "03", "seed_bytes": 16, "key_bytes": 16, )"
        R"("delay_ms": 1000, "key_window_ms": 1000}])";
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(m));
    ASSERT_NO_FATAL_FAILURE(unlock_01(b));
    const size_t from = b.sent.size();
    EXPECT_EQ(b.begin(0), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "unlock level 03 first, or start the run with the unlock "
                 "allowed");
    b.passes(6);
    EXPECT_EQ(b.count(0x23, from), 0u);
    EXPECT_EQ(b.files(), kNoFiles);
}

/* A refused key ends the run: one failure counted, in the store too, no
 * second seed, and the delay refuses the next start */
TEST(SessionOperationTest, AWrongKeyEndsTheRunWithOneFailure)
{
    TempStore file("session_operation_key.json");
    session_operation::GuestShape wrong;
    wrong.flip = 1;
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(
            b.start(made(wrong, session_operation::PlanShape()),
                    ECUIO_ENGINE_PDU_MAX, file.path.c_str()));
        /* the store names the module by the car's VIN here */
        ecuio_channel_identity_t& id = b.s->identity[0];
        ecuio_identity_item_t& serial = id.list[id.items++];
        const uint8_t request[3] = {0x22, 0xF1, 0x8C};
        std::memcpy(serial.request, request, 3);
        serial.length = 3;
        serial.outcome = ECUIO_IDENTITY_ANSWERED;
        std::snprintf(serial.text, sizeof serial.text, "SER-0001");
        serial.text_length = 8;

        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_NE(text_of(file.path).find("\"pending_utc\": 1"),
                  std::string::npos)
            << "the mark is saved before the seed: " << text_of(file.path);
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_UNLOCK_REFUSED) << v.words;
        EXPECT_EQ(v.nrc, 0x35);
        EXPECT_STREQ(v.words,
                     "the module's level stayed locked: nothing was read");
        EXPECT_EQ(v.bytes_done, 0u);
        b.passes(10);
        EXPECT_EQ(b.count(0x27), 2u) << "one seed, one key, never a second";
        EXPECT_EQ(b.count(0x23), 0u);
        EXPECT_EQ(b.files(), kNoFiles);
        EXPECT_EQ(conversation_of(b.s).failed, 1u);
        /* The delay runs: the next start is refused and leaves nothing */
        EXPECT_EQ(b.begin(), ECUIO_ERR_TOO_SOON);
        EXPECT_EQ(b.files(), kNoFiles);
        EXPECT_EQ(b.state().generation, 1u);
        b.passes(4);
        EXPECT_EQ(b.count(0x27), 2u);
        ecuio_conversations_end(b.s, b.e.get());
        EXPECT_EQ(ecuio_session_close(b.s), ECUIO_OK);
    }
    const std::string store = text_of(file.path);
    EXPECT_NE(store.find("\"failed\": 1"), std::string::npos) << store;
    EXPECT_NE(store.find("\"pending_utc\": 0"), std::string::npos) << store;
}

/* Each fault of the module's ends the run in its own words, keeps
 * nothing and asks nothing more of the range */
TEST(SessionOperationTest, AFaultEndsTheRunAndKeepsNothing)
{
    const Bytes third = sim_memory_test::read_of(kLowAt + 512, 256);
    const struct {
        const char* what;
        std::vector<Bytes> frames; /* the third chunk's answer, always */
        uint32_t end;
        uint8_t nrc;
        size_t asked; /* how often that chunk is asked */
    } faults[] = {
        {"refused", {nrc(0x23, 0x31)}, ECUIO_OPERATION_END_REFUSED, 0x31, 1},
        {"silent", {}, ECUIO_OPERATION_END_NO_ANSWER, 0, 3},
        {"short", {Bytes{0x63, 1, 2, 3}}, ECUIO_OPERATION_END_BAD_REPLY, 0, 1},
        {"the session left",
         {nrc(0x23, 0x7F)},
         ECUIO_OPERATION_END_SESSION_LOST,
         0,
         1},
        {"locked again",
         {nrc(0x23, 0x33)},
         ECUIO_OPERATION_END_SESSION_LOST,
         0,
         1},
    };
    for (const auto& f : faults) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.script = [&](const Bytes& q, size_t, Answer* out) {
            out->frames = f.frames;
            return q == third;
        };
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, f.end) << f.what << ": " << v.words;
        EXPECT_EQ(v.nrc, f.nrc) << f.what;
        EXPECT_STRNE(v.words, "") << f.what;
        EXPECT_STREQ(v.name, "") << f.what;
        EXPECT_EQ(v.bytes_done, 512u) << f.what;
        size_t asked = 0;
        for (const Bytes& q : b.sent) {
            asked += q == third ? 1u : 0u;
        }
        EXPECT_EQ(asked, f.asked) << f.what;
        const size_t reads = b.count(0x23);
        b.passes(6);
        EXPECT_EQ(b.count(0x23), reads) << f.what << ": nothing after";
        EXPECT_EQ(b.files(), kNoFiles) << f.what;
    }
}

/* A request the adapter didn't take ends the run as the link's, asked
 * once: no second try says whether the first reached the bus */
TEST(SessionOperationTest, AWriteThatFailsEndsTheRunAsTheLinks)
{
    const Bytes third = sim_memory_test::read_of(kLowAt + 512, 256);
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.unsent = [&](const Bytes& q) { return q == third; };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_LINK_LOST) << v.words;
    EXPECT_STREQ(v.words, "a request could not be sent: nothing was kept");
    size_t asked = 0;
    for (const Bytes& q : b.sent) {
        asked += q == third ? 1u : 0u;
    }
    EXPECT_EQ(asked, 1u);
    EXPECT_EQ(b.files(), kNoFiles);
}

/* A file that fails ends the run in the writer's own words */
TEST(SessionOperationTest, AFileThatFailsEndsTheRunInItsOwnWords)
{
    struct Seam {
        Seam()
        {
            ecuio_operation::dump_test_fail_at.store(
                ecuio_operation::kSinkFailWrite);
        }
        ~Seam() { ecuio_operation::dump_test_fail_at.store(0); }
    };
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const Seam seam;
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_FILE) << v.words;
    EXPECT_STREQ(v.words, "the copy can't be written");
    EXPECT_EQ(b.count(0x35), 0u) << "the next range is never asked";
    EXPECT_EQ(b.files(), kNoFiles);
}

/* A session whose entry is answered pending for ever is waited for to
 * the run's own bound, the entry asked once */
TEST(SessionOperationTest, ASessionNeverEnteredEndsTheRunAtItsBound)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    bool lost = false;
    b.while_out = [&lost](Bench& bench) {
        if (!lost && bench.sent.back() == Bytes{0x01, 0x0D}) {
            /* the session lost as a keep-alive's silence loses it, once
             * the checks are read */
            lost = true;
            ecuio_engine_session_step(bench.e->chans[0],
                                      ECUIO_SESSION_STATE_ENTERING);
        }
    };
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    for (int i = 0; i < 20 && !lost; ++i) {
        b.pass();
    }
    ASSERT_TRUE(lost);
    const size_t from = b.sent.size();
    for (int second = 0; second < 12; ++second) {
        ecuio_step_conversations(b.s, b.e.get(), b.now);
        const int32_t r = ecuio_engine_due(b.e.get(), 0, b.now);
        if (r != ECUIO_ENGINE_NONE) {
            b.sent.push_back(engine_talk_test::wire(b.e.get(), 0, r));
            ecuio_engine_sent(b.e.get(), 0, r, true, b.now);
        }
        if (second == 9) {
            EXPECT_EQ(b.state().phase, ECUIO_OPERATION_PHASE_UNLOCKING)
                << "still waited for inside the bound";
        }
        b.now += 1000000;
        b.frame(nrc(0x10, 0x78));
    }
    const ecuio_operation_state_t v = b.until_ended(400);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SESSION_LOST) << v.words;
    EXPECT_EQ(b.count(0x10, from), 1u);
    EXPECT_EQ(b.count(0x27, from) + b.count(0x23, from), 0u);
    EXPECT_EQ(b.files(), kNoFiles);
}

/* A pending answer is waited for up to its bound, then the run ends */
TEST(SessionOperationTest, APendingAnswerIsWaitedForToItsBound)
{
    const Bytes third = sim_memory_test::read_of(kLowAt + 512, 256);
    Bench once;
    ASSERT_NO_FATAL_FAILURE(once.start());
    int answered = 0;
    once.script = [&](const Bytes& q, size_t, Answer* out) {
        if (q != third || answered++ != 0) {
            return false;
        }
        once.node.now = once.now;
        out->frames = {nrc(0x23, 0x78), nrc(0x23, 0x78), once.node.ask(q)};
        out->pending_us = 2000000;
        return true;
    };
    ASSERT_EQ(once.begin(), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_operation_state_t v = once.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;

    Bench never;
    ASSERT_NO_FATAL_FAILURE(never.start());
    never.script = [&](const Bytes& q, size_t, Answer* out) {
        out->frames.assign(20, nrc(0x23, 0x78));
        out->pending_us = 4000000;
        return q == third;
    };
    ASSERT_EQ(never.begin(), ECUIO_OK) << ecuio_last_error_detail();
    v = never.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_NO_ANSWER) << v.words;
    size_t asked = 0;
    for (const Bytes& q : never.sent) {
        asked += q == third ? 1u : 0u;
    }
    EXPECT_EQ(asked, 1u) << "a wait is no second request";
    EXPECT_EQ(never.files(), kNoFiles);
}

/* A reading that differs from the first, or an identifier that changed,
 * ends the run where it was found */
TEST(SessionOperationTest, ADifferenceSaysWhereAndKeepsNothing)
{
    Bench moving;
    ASSERT_NO_FATAL_FAILURE(moving.start());
    moving.node.s.memory.faults.moving = true;
    moving.node.s.memory.faults.moving_at = 700;
    ASSERT_EQ(moving.begin(), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_operation_state_t v = moving.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_DIFFER) << v.words;
    EXPECT_EQ(v.step, 0u);
    EXPECT_EQ(v.differ_offset, 700u);
    EXPECT_STREQ(v.words, "the two readings differ: nothing was kept");
    EXPECT_EQ(moving.count(0x35), 0u) << "the next range is never asked";
    EXPECT_EQ(moving.files(), kNoFiles);

    Bench later;
    ASSERT_NO_FATAL_FAILURE(later.start());
    later.node.s.memory.faults.moving = true;
    later.node.s.memory.faults.moving_at = (kHighAt - kLowAt) + 100;
    ASSERT_EQ(later.begin(), ECUIO_OK) << ecuio_last_error_detail();
    v = later.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_DIFFER) << v.words;
    EXPECT_EQ(v.step, 1u) << "the second range";
    EXPECT_EQ(v.differ_offset, 100u);
    EXPECT_EQ(later.files(), kNoFiles);

    Bench other;
    ASSERT_NO_FATAL_FAILURE(other.start());
    other.node.s.memory.faults.ident = 3;
    ASSERT_EQ(other.begin(), ECUIO_OK) << ecuio_last_error_detail();
    v = other.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_IDENTITY_CHANGED) << v.words;
    EXPECT_EQ(v.bytes_done, v.bytes_total) << "found at the last reading";
    EXPECT_EQ(other.files(), kNoFiles);
}

/* A cancel in each phase ends the run canceled with nothing kept; inside
 * an upload the module's transfer is closed first, with one 37 */
TEST(SessionOperationTest, ACancelLeavesNothing)
{
    EXPECT_EQ(ecuio_session_operation_cancel(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    const struct {
        const char* what;
        uint8_t service; /* the cancel comes once this is the last sent */
        uint32_t pass;   /* in this reading (1 or 2); 0 at once */
        size_t exits;    /* the 37s the canceled run sends */
    } places[] = {
        {"at once", 0, 0, 0},         {"unlocking", 0x27, 0, 0},
        {"reading", 0x23, 1, 0},      {"comparing", 0x23, 2, 0},
        {"in an upload", 0x36, 1, 1}, {"in the second upload", 0x36, 2, 2},
    };
    for (const auto& place : places) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        const uint32_t words = conversation_of(b.s).words_ended;
        EXPECT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK)
            << "with no run under way";
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        for (int i = 0; i < 400 && place.service != 0; ++i) {
            const ecuio_operation_state_t at = b.state();
            if (!b.sent.empty() && b.sent.back()[0] == place.service &&
                at.pass == place.pass) {
                break;
            }
            b.pass();
        }
        ASSERT_EQ(b.state().end, ECUIO_OPERATION_END_NONE) << place.what;
        EXPECT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
        const ecuio_operation_state_t v = b.until_ended();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED) << place.what;
        EXPECT_STREQ(v.words, "the read was stopped: nothing was kept");
        EXPECT_EQ(b.count(0x37), place.exits) << place.what;
        const size_t run = b.count(0x23) + b.count(0x36) + b.count(0x35);
        b.passes(6);
        EXPECT_EQ(conversation_of(b.s).words_ended, words + 1)
            << place.what << ": the run's word is settled, given or not";
        EXPECT_EQ(b.count(0x23) + b.count(0x36) + b.count(0x35), run)
            << place.what << ": nothing after";
        EXPECT_EQ(b.files(), kNoFiles) << place.what;
        /* The session takes the next run */
        if (place.pass == 1 && place.service == 0x23) {
            ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
            const ecuio_operation_state_t again = b.until_ended();
            EXPECT_EQ(again.end, ECUIO_OPERATION_END_SAVED) << again.words;
            EXPECT_EQ(again.generation, 2u);
            EXPECT_EQ(b.files().size(), 1u);
        }
    }
}

/* A close in each phase ends the run canceled and keeps nothing */
TEST(SessionOperationTest, ACloseMidRunLeavesNothing)
{
    for (const int passes : {0, 1, 8, 20, 30}) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        b.passes(passes);
        ASSERT_EQ(b.state().end, ECUIO_OPERATION_END_NONE) << passes;
        ecuio_conversations_end(b.s, b.e.get());
        EXPECT_EQ(ecuio_session_close(b.s), ECUIO_OK);
        const ecuio_operation_state_t v = b.state();
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED) << passes;
        EXPECT_EQ(v.phase, ECUIO_OPERATION_PHASE_DONE);
        EXPECT_STREQ(v.words, "the session closed before the read ended: "
                              "nothing was kept");
        EXPECT_EQ(b.files(), kNoFiles) << passes;
    }
    /* While the copy is sealed: the seal gets its end, and the state and
     * the folder agree whichever way it went */
    Bench sealing;
    ASSERT_NO_FATAL_FAILURE(sealing.start());
    ASSERT_EQ(sealing.begin(), ECUIO_OK) << ecuio_last_error_detail();
    sealing.until_phase(ECUIO_OPERATION_PHASE_SAVING);
    ecuio_conversations_end(sealing.s, sealing.e.get());
    EXPECT_EQ(ecuio_session_close(sealing.s), ECUIO_OK);
    const ecuio_operation_state_t v = sealing.state();
    if (v.end == ECUIO_OPERATION_END_SAVED) {
        EXPECT_EQ(sealing.files(), std::vector<std::string>{v.name});
    } else {
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED);
        EXPECT_EQ(sealing.files(), kNoFiles);
    }
}

/* The state's own refusals */
TEST(SessionOperationTest, TheStateIsCopiedWhole)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ecuio_operation_state_t v{};
    EXPECT_EQ(ecuio_session_get_operation(nullptr, &v),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_operation(b.s, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    std::memset(&v, 0xEE, sizeof v);
    v.struct_size = sizeof v - 1;
    EXPECT_EQ(ecuio_session_get_operation(b.s, &v), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(v.phase, 0xEEEEEEEEu) << "nothing written";
    v.struct_size = sizeof v + 8; /* a later caller's */
    EXPECT_EQ(ecuio_session_get_operation(b.s, &v), ECUIO_OK);
    EXPECT_EQ(v.struct_size, sizeof v);
    EXPECT_EQ(v.generation, 0u);
    EXPECT_EQ(v.phase, ECUIO_OPERATION_PHASE_NONE);
    EXPECT_EQ(v.reserved[5], 0u);
}

/* The worker makes no allocation for a chunk: a reading's passes, the
 * frame and the step hooks included, on stack buffers alone */
TEST(SessionOperationTest, TheWorkerAllocatesNothingForAChunk)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    b.until_phase(ECUIO_OPERATION_PHASE_READING);
    b.passes(2);
    ASSERT_EQ(b.state().phase, ECUIO_OPERATION_PHASE_READING);
    static uint8_t reply[ECUIO_SIM_REPLY_MAX];
    uint8_t wire[ECUIO_ENGINE_PDU_MAX];
    ecuio_request_engine* e = b.e.get();
    const uint64_t done = b.state().bytes_done;
    const unsigned long before = news_on_this_thread();
    int chunks = 0;
    for (int i = 0; i < 12 && chunks < 4; ++i) {
        ecuio_step_conversations(b.s, e, b.now);
        const int32_t r = ecuio_engine_due(e, 0, b.now);
        if (r == ECUIO_ENGINE_NONE) {
            b.now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        uint32_t len = 0;
        const uint8_t* pdu = ecuio_engine_pdu_of(
            e->chans[0].talk, r == ECUIO_ENGINE_ONCE, q.bytes, q.len, &len);
        std::memcpy(wire, pdu, len);
        chunks += wire[0] == 0x23 ? 1 : 0;
        ecuio_engine_sent(e, 0, r, true, b.now);
        bool handled = false;
        const uint32_t n = ecuio_sim_session_answer(b.node.s, true, true, b.now,
                                                    wire, len, reply, &handled);
        b.now += 4 * engine_talk_test::kMs;
        (void)ecuio_engine_on_frame(e, 0, reply, static_cast<uint16_t>(n),
                                    b.now, nullptr);
        ecuio_conversations_frame(b.s, e, 0, reply, n);
        b.now += ECUIO_ENGINE_GAP_US;
    }
    ecuio_step_conversations(b.s, e, b.now); /* the last chunk's publish */
    const unsigned long news = news_on_this_thread() - before;
    EXPECT_EQ(news, 0u);
    EXPECT_EQ(b.state().bytes_done, done + 4 * 256) << "four chunks went";
    EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_SAVED);
}

/* Each allocation of the start failing in turn: OUT_OF_MEMORY with
 * nothing made, no file and no attempt marked, until one start runs */
TEST(SessionOperationTest, AStartOutOfMemoryLeavesNothing)
{
    TempStore file("session_operation_oom.json");
    Bench b;
    ASSERT_NO_FATAL_FAILURE(
        b.start(made(), ECUIO_ENGINE_PDU_MAX, file.path.c_str()));
    const std::string folder = b.folder.str();
    const ecuio_operation_request_t q =
        session_operation::request(ECUIO_OPERATION_MAY_UNLOCK);
    bool started = false;
    unsigned long failed = 0;
    for (unsigned long n = 1; n < 4000 && !started; ++n) {
        fail_new_on_this_thread(n);
        const ecuio_status_t st =
            ecuio_session_operation_start(b.s, &q, folder.c_str());
        fail_new_on_this_thread(0);
        if (st == ECUIO_OK) {
            started = true;
            continue;
        }
        ++failed;
        ASSERT_EQ(st, ECUIO_ERR_OUT_OF_MEMORY)
            << n << ": " << ecuio_last_error_detail();
        ASSERT_EQ(b.files(), kNoFiles) << n;
        ASSERT_EQ(b.state().generation, 0u) << n;
        ASSERT_EQ(text_of(file.path).find("\"pending_utc\": 1"),
                  std::string::npos)
            << n << ": a mark with no run to end it";
    }
    ASSERT_TRUE(started);
    EXPECT_GT(failed, 20u);
    const ecuio_operation_state_t v = b.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    b.passes(4);
    EXPECT_EQ(b.count(0x27), 2u) << "one attempt for all those starts";
}

} // namespace
