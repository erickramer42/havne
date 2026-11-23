// File/layer: tests/test_conversation_session.cpp | test, C++17, Qt-free
// Testing: UNIT: ConversationSessionTest (test_core)
//
// A plugin's module bound to its session behind the test door
// (conversation_bind.h, request_conversation.h, consent_gate.h): what the
// door shut leaves, start's refusals of a module, the unlock end to end
// through the worker's half, the gated read's rejoin, and the store's
// mark before the seed and settle after.

#include "conversation_session_support.h"
#include "core_test_support.h"

#include "../core/src/security_store_owner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using conversation_session::Bench;
using conversation_session::Bound;
using conversation_session::DoorOpen;
using conversation_session::GuestShape;
using conversation_session::made;
using conversation_session::make_session;

namespace {

const engine_talk_test::Bytes kSeed01 = {0x27, 0x01};

/* A path for a store of the case's own, gone before and after */
struct TempStore {
    std::string path;
    explicit TempStore(const char* name)
        : path(testing::TempDir() + std::string(name))
    {
        std::remove(path.c_str());
    }
    ~TempStore() { std::remove(path.c_str()); }
};

ecuio_security::store_entry entry_on_disk(const std::string& path)
{
    ecuio_security::store s;
    std::string why;
    EXPECT_EQ(ecuio_security::store_load(path, &s, &why),
              ecuio_security::store_io::ok)
        << why;
    ecuio_security::store_entry e;
    for (const ecuio_security::store_entry& x : s.entries) {
        e = x;
    }
    return e;
}

/* The file's text as it is now ("" for none) */
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

} // namespace

/* The door shut: a session over the plugin's set binds nothing, runs no
 * byte of the module and says why; mock:// says the simulated source
 * asks nothing, the door open or not */
TEST(ConversationSessionTest, TheDoorShutBindsNothingAndSaysWhy)
{
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
    EXPECT_EQ(b.session->talk, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "plugin talker's conversation part isn't in use: not "
                 "enabled");
    EXPECT_EQ(ecuio_bound_channel_of(b.session.get(), 0), nullptr);
    EXPECT_EQ(ecuio_conversation_give_word(b.session.get(), 0, 0x01, 1, 1),
              ECUIO_ERR_INVALID_ARGUMENT);

    DoorOpen open;
    Bound mock;
    ASSERT_NO_FATAL_FAILURE(make_session(&mock, made(), "mock://x"));
    ASSERT_EQ(ecuio_conversation_bind(mock.session.get()), ECUIO_OK);
    EXPECT_EQ(mock.session->talk, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "plugin talker's conversation part isn't in use: the "
                 "simulated source asks nothing");
}

/* Start refuses a module it can't run, in words, with nothing bound: an
 * ecuio_abi() of 2, a buffer outside its memory */
TEST(ConversationSessionTest, StartRefusesAModuleItCantRun)
{
    DoorOpen open;
    struct Row {
        GuestShape shape;
        const char* words;
    };
    GuestShape abi2;
    abi2.abi = 2;
    GuestShape outside;
    outside.buffer_inside = false;
    const Row rows[] = {
        {abi2, "plugin talker's module answers no guest ABI this core "
               "speaks (ecuio_abi() must answer 1)"},
        {outside, "plugin talker's module names an exchange buffer that "
                  "doesn't lie inside its memory"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.words);
        Bound b;
        ASSERT_NO_FATAL_FAILURE(make_session(&b, made(row.shape)));
        EXPECT_EQ(ecuio_conversation_bind(b.session.get()),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), row.words);
        EXPECT_EQ(b.session->talk, nullptr);
    }
}

/* Bound: begin's 3 enters the extended session through the engine's
 * own step, the gated DID drops on 7F 22 33, the word unlocks level 01
 * with the module's key, and the gated row rejoins once and is read */
TEST(ConversationSessionTest, ABoundModuleUnlocksAndTheGatedReadRejoins)
{
    DoorOpen open;
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_NE(b.session->talk, nullptr);
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bench.start(b.session.get()));
    bench.passes(8);
    ASSERT_EQ(bench.e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(bench.report().state, ecuio_conversation_state::talking);
    const int32_t gated = 1; /* the bench's second row */
    ASSERT_EQ(bench.e->requests[gated].dropped, ECUIO_DROP_ACCESS);

    uint32_t generation = 0;
    const uint32_t before = bench.report(&generation).words_ended;
    ASSERT_EQ(ecuio_conversation_give_word(b.session.get(), 0, 0x01, 7, 1),
              ECUIO_OK);
    bench.until_word_ended(before);
    uint32_t after = 0;
    const ecuio_conversation_report r = bench.report(&after);
    EXPECT_EQ(r.unlock, ecuio_unlock_state::unlocked);
    EXPECT_EQ(r.level, 0x01);
    EXPECT_GT(after, generation);
    EXPECT_EQ(bench.ecu.security.unlocked, 0x01);
    EXPECT_EQ(bench.heard(kSeed01), 1u);
    EXPECT_EQ(bench.e->requests[gated].dropped, 0) << "rejoined";
    const size_t gated_before = bench.heard(engine_talk_test::kGated);
    bench.passes(6);
    EXPECT_GT(bench.heard(engine_talk_test::kGated), gated_before);
    EXPECT_EQ(bench.e->requests[gated].dropped, 0) << "read, not dropped";
}

/* begin's 0: the module keeps the default session, and a built-in
 * ?session=03 step is never run on its channel */
TEST(ConversationSessionTest, NoBuiltInSessionStepRunsOnABoundChannel)
{
    DoorOpen open;
    GuestShape stays;
    stays.begin = 0;
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made(stays)));
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(engine_talk_test::engine(bench.e.get()));
    ASSERT_TRUE(
        ecuio_engine_session(bench.e.get(), 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    bench.s = b.session.get();
    bench.ecu.now = &bench.now;
    ecuio_conversations_start(bench.s, bench.e.get());
    EXPECT_EQ(bench.e->chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    bench.passes(6);
    EXPECT_EQ(bench.heard(engine_talk_test::kEnter), 0u);
    EXPECT_EQ(bench.report().state, ecuio_conversation_state::talking);
}

/* The mark is on the disk before the seed is asked; a refused key's end
 * reaches the file through the owner's writer, the worker calling no
 * store step: one failure counted, the delay dated, no mark left */
TEST(ConversationSessionTest, TheMarkGoesBeforeTheSeedAndTheWriterSettles)
{
    DoorOpen open;
    TempStore file("conversation_session_store.json");
    GuestShape wrong;
    wrong.flip = 0x01;
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made(wrong)));
    std::string why;
    ASSERT_EQ(ecuio_security::store_open(file.path,
                                         &b.session->conversation_store, &why),
              ecuio_security::store_io::ok)
        << why;
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK)
        << ecuio_last_error_detail();
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bench.start(b.session.get()));
    bench.passes(8);
    const uint32_t before = bench.report().words_ended;
    ASSERT_EQ(ecuio_conversation_give_word(b.session.get(), 0, 0x01,
                                           0x0123456789ABCDEFull, 1795132800),
              ECUIO_OK);
    EXPECT_EQ(bench.heard(kSeed01), 0u);
    /* the text, since a load counts a mark it finds as one failure */
    const std::string marked_text = text_of(file.path);
    for (const char* part : {"\"vehicle\": \"0123456789ABCDEF\"",
                             "\"rx\": \"7E8\"", "\"level\": 1,"}) {
        EXPECT_NE(marked_text.find(part), std::string::npos) << marked_text;
    }
    EXPECT_NE(marked_text.find("\"pending_utc\": 1795132800"),
              std::string::npos)
        << marked_text;

    bench.until_word_ended(before);
    ASSERT_EQ(bench.report().unlock, ecuio_unlock_state::failed);
    EXPECT_EQ(bench.heard(kSeed01), 1u);
    bool settled = false;
    for (int i = 0; i < 200 && !settled; ++i) {
        settled = text_of(file.path) != marked_text;
        if (!settled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_TRUE(settled) << "the writer saved the settle unasked";
    const ecuio_security::store_entry ended = entry_on_disk(file.path);
    EXPECT_EQ(ended.failed, 1u);
    EXPECT_EQ(ended.pending_utc, 0);
    EXPECT_GT(ended.last_utc, 0);
}

/* Out of memory at each of the bind's allocations: OK or
 * OUT_OF_MEMORY, nothing thrown, and nothing bound on a refusal */
TEST(ConversationSessionTest, OutOfMemoryAnywhereInTheBindIsAnswered)
{
    DoorOpen open;
    unsigned long news = 0;
    {
        Bound b;
        ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
        const unsigned long at = news_on_this_thread();
        ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
        news = news_on_this_thread() - at;
    }
    ASSERT_GE(news, 2u) << "the binding, its channels";
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        Bound b;
        ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_conversation_bind(b.session.get());
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "the bind threw at allocation " << n;
        if (rc == ECUIO_OK) {
            EXPECT_NE(b.session->talk, nullptr) << "at " << n;
        } else {
            ++refused;
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(b.session->talk, nullptr) << "at " << n;
        }
    }
    EXPECT_GT(refused, 0u);
}

/* A channel whose rx is one of the part's ECUs and whose tx isn't binds
 * nothing: the match is by both IDs */
TEST(ConversationSessionTest, AChannelMatchesByBothIds)
{
    DoorOpen open;
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    b.session->module_tx[0] = 0x7E1;
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
    EXPECT_EQ(b.session->talk, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "plugin talker's conversation part isn't in use: no channel "
                 "of the session is one of its ECUs");
}

/* A mark the store can't save is the word's refusal, in the store's
 * words: no word given, no seed asked */
TEST(ConversationSessionTest, NoWordIsGivenWithoutItsMark)
{
    DoorOpen open;
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    const std::string path =
        testing::TempDir() + std::string("no-such-folder/store.json");
    std::string why;
    ASSERT_EQ(
        ecuio_security::store_open(path, &b.session->conversation_store, &why),
        ecuio_security::store_io::ok)
        << why;
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bench.start(b.session.get()));
    bench.passes(8);
    EXPECT_EQ(
        ecuio_conversation_give_word(b.session.get(), 0, 0x01, 7, 1795132800),
        ECUIO_ERR_LOAD_FAILED);
    EXPECT_STRNE(ecuio_last_error_detail(), "");
    bench.passes(12);
    EXPECT_EQ(bench.heard(kSeed01), 0u);
    EXPECT_EQ(bench.report().unlock, ecuio_unlock_state::none);
}

/* A mark saves every box's posted end first: a settle saved after the
 * next mark on its key would take that mark away */
TEST(ConversationSessionTest, AMarkSavesThePostedSettlesFirst)
{
    TempStore file("conversation_session_drain.json");
    ecuio_security::store_ref owner;
    std::string why;
    ASSERT_EQ(ecuio_security::store_open(file.path, &owner, &why),
              ecuio_security::store_io::ok)
        << why;
    ecuio_security::settle_box box;
    ASSERT_EQ(owner->attach(&box, &why), ecuio_security::store_io::ok) << why;
    const ecuio_security::store_key key{0x0123456789ABCDEFull, 0x7E8, 0x01};
    ASSERT_EQ(owner->began(key, 1795132800, nullptr, &why),
              ecuio_security::store_io::ok);
    box.key = key;
    box.failed = true;
    box.delay = true;
    box.utc = 1795132801;
    box.posted.fetch_add(1, std::memory_order_release); /* no wake */
    ASSERT_EQ(owner->began(key, 1795132802, nullptr, &why),
              ecuio_security::store_io::ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string text = text_of(file.path);
    EXPECT_NE(text.find("\"failed\": 1,"), std::string::npos) << text;
    EXPECT_NE(text.find("\"pending_utc\": 1795132802"), std::string::npos)
        << text;
    owner->detach(&box);
}
