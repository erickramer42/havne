// File/layer: tests/test_conversation_registry.cpp | test, C++17, Qt-free
// Testing: UNIT: ConversationRegistryTest (test_core)
//
// A plugin's conversation part kept beside its registered set
// (conversation_registry.h): kept at register, held by a session from
// create, forgotten at the plugin's free while a session's copy lives on.

#include "core_test_support.h"
#include "plugin_conversation_support.h"

#include "conversation_registry.h" /* core/src */
#include "session.h"               /* core/src */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

using conversation_test::load_packed;
using conversation_test::Made;
using conversation_test::Plugin;

const ecuio_signal_set_t* set_of(const char* name)
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup(name, &set), ECUIO_OK);
    return set;
}

std::string uri_of(const char* name)
{
    return std::string("mock://x?profile=") + name;
}

} // namespace

/* Register keeps the plugin's part, its parts as the loader judged them;
 * a session over the set holds it from create, and free is refused
 * until that session is gone, the table empty after */
TEST(ConversationRegistryTest, ASessionHoldsThePartAndFreeWaitsForIt)
{
    const size_t before = ecuio_conversation_kept();
    Plugin p = load_packed(Made{}.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name), ECUIO_OK);
    EXPECT_EQ(ecuio_conversation_kept(), before + 1);
    const ecuio_conversation_part_ref part =
        ecuio_conversation_of(set_of(name));
    ASSERT_NE(part, nullptr);
    EXPECT_EQ(part->plugin_id, "talker");
    EXPECT_EQ(part->plugin_version, "1.2.0");
    EXPECT_EQ(part->code, Made{}.module);
    ASSERT_EQ(part->ecus.size(), 2u);
    EXPECT_EQ(part->ecus[0].id, "tcm");
    EXPECT_EQ(part->ecus[0].rx, 0x7E9u);
    EXPECT_EQ(part->ecus[0].tx, 0x7E1u);
    EXPECT_EQ(part->levels.size(), 2u);
    EXPECT_EQ(part->services.size(), 4u);
    EXPECT_EQ(part->budget, 250000u);
    ecuio_plugin_conversation_t c = conversation_test::conversation_of(p.get());
    EXPECT_EQ(std::memcmp(part->sha256, c.code_sha256, 32), 0);

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri_of(name).c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(s->conversation, part);
    ecuio_plugin_t* raw = p.release();
    EXPECT_EQ(ecuio_plugin_free(raw), ECUIO_ERR_IN_USE);
    EXPECT_EQ(ecuio_conversation_kept(), before + 1) << "nothing forgotten";
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_plugin_free(raw), ECUIO_OK);
    EXPECT_EQ(ecuio_conversation_kept(), before);
    EXPECT_EQ(part->plugin_id, "talker") << "a holder's copy lives on";
}

/* A running session keeps its copy through the table's forget, and a
 * plugin without the tier keeps nothing at register */
TEST(ConversationRegistryTest, AForgottenPartLivesInItsSessionAndDataKeepsNone)
{
    const size_t before = ecuio_conversation_kept();
    Plugin p = load_packed(Made{}.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name), ECUIO_OK);
    ASSERT_EQ(ecuio_plugin_register(p.get(), 0, nullptr, 0), ECUIO_OK);
    EXPECT_EQ(ecuio_conversation_kept(), before + 1) << "one entry a set";
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri_of(name).c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_conversation_forget(set_of(name));
    EXPECT_EQ(ecuio_conversation_of(set_of(name)), nullptr);
    ASSERT_NE(s->conversation, nullptr);
    EXPECT_EQ(s->conversation->plugin_id, "talker");
    ecuio_session_destroy(s);

    Made data;
    data.format = "1";
    data.tiers = R"(["data"])";
    data.has_group = false;
    data.has_binary = false;
    Plugin plain = load_packed(data.json());
    ASSERT_NE(plain, nullptr) << ecuio_last_error_detail();
    char plain_name[64] = {};
    ASSERT_EQ(
        ecuio_plugin_register(plain.get(), 0, plain_name, sizeof plain_name),
        ECUIO_OK);
    EXPECT_EQ(ecuio_conversation_kept(), before);
    ASSERT_EQ(ecuio_session_create(uri_of(plain_name).c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(s->conversation, nullptr);
    ecuio_session_destroy(s);
}

/* Out of memory at each of register's allocations, then of create's over
 * the registered set: each answers OK or OUT_OF_MEMORY with nothing
 * thrown; a refused register keeps no part and no name, and a session
 * create hands back holds the part */
TEST(ConversationRegistryTest, OutOfMemoryAnywhereInRegisterOrCreateIsAnswered)
{
    const size_t before = ecuio_conversation_kept();
    unsigned long news = 0;
    {
        Plugin p = load_packed(Made{}.json());
        ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
        const unsigned long at = news_on_this_thread();
        ASSERT_EQ(ecuio_plugin_register(p.get(), 0, nullptr, 0), ECUIO_OK);
        news = news_on_this_thread() - at;
    }
    ASSERT_EQ(ecuio_conversation_kept(), before);
    ASSERT_GT(news, 3u) << "the part's copy, the table, the registry";
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        Plugin p = load_packed(Made{}.json());
        ASSERT_NE(p, nullptr);
        char name[64] = {};
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_plugin_register(p.get(), 0, name, sizeof name);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "register threw at allocation " << n;
        if (rc != ECUIO_OK) {
            ++refused;
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(ecuio_conversation_kept(), before) << "at " << n;
        } else {
            EXPECT_NE(ecuio_conversation_of(set_of(name)), nullptr)
                << "at " << n;
        }
    }
    EXPECT_GT(refused, 0u);

    Plugin p = load_packed(Made{}.json());
    ASSERT_NE(p, nullptr);
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name), ECUIO_OK);
    const std::string uri = uri_of(name);
    ecuio_session_t* s = nullptr;
    const unsigned long at = news_on_this_thread();
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    const unsigned long create_news = news_on_this_thread() - at;
    ecuio_session_destroy(s);
    for (unsigned long n = 1; n <= create_news; ++n) {
        s = nullptr;
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_session_create(uri.c_str(), &s);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "create threw at allocation " << n;
        if (rc == ECUIO_OK) {
            ASSERT_NE(s, nullptr);
            EXPECT_NE(s->conversation, nullptr) << "at " << n;
            ecuio_session_destroy(s);
        } else {
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(s, nullptr) << "at " << n;
        }
    }
}
