// File/layer: tests/test_consent_session.cpp | test, C++17, Qt-free
// Testing: UNIT: ConsentSessionTest (test_core)
//
// The consent gate (consent_gate.h, core_consent.cpp): a format 3
// plugin's module binds only with a consent applied between create and
// start, then unlocks the simulated ECU's gated level; apply's refusals;
// the unlock's ECUIO_ERR_CONSENT_REQUIRED; a session that asked nothing
// asks once a consent binds; the test door unchanged.

#include "conversation_session_support.h"
#include "core_test_support.h"

#include <gtest/gtest.h>
#include <ecuio/plugin.h>

#include <cstdio>
#include <cstring>
#include <string>

using conversation_session::Bench;
using conversation_session::Bound;
using conversation_session::DoorOpen;
using conversation_session::made;
using conversation_session::make_session;

unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace {

constexpr uint64_t kUtc = 1795132800; /* a made-up second in 2026 */

/* The bench's plugin in format 3, naming its publisher */
conversation_test::Made made3()
{
    conversation_test::Made m = made();
    m.format = "3";
    m.publisher = R"({"name": "Made-up Tools", "contact": "c", )"
                  R"("license": "MIT", "terms": "Use at your own risk."})";
    return m;
}

std::string print_of(const ecuio_plugin_t* p)
{
    ecuio_plugin_fingerprint_t f{};
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_fingerprint(p, &f), ECUIO_OK);
    return std::string(reinterpret_cast<const char*>(f.fingerprint), 32);
}

const uint8_t* bytes_of(const std::string& s)
{
    return reinterpret_cast<const uint8_t*>(s.data());
}

/* A consent store of the case's own, gone before and after */
struct Store {
    std::string path;
    ecuio_consent_store_t* store = nullptr;
    explicit Store(const char* name)
        : path(testing::TempDir() + std::string(name))
    {
        std::remove(path.c_str());
        EXPECT_EQ(ecuio_consent_store_open(path.c_str(), &store), ECUIO_OK);
    }
    ~Store()
    {
        ecuio_consent_store_close(store);
        std::remove(path.c_str());
        std::remove((path + ".lock").c_str());
    }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
};

ecuio_status_t unlock(ecuio_session_t* s)
{
    ecuio_channel_conversation_t r{};
    EXPECT_EQ(ecuio_session_get_channel_conversation(s, 0, &r), ECUIO_OK);
    return ecuio_session_security_unlock(s, 0, 0x01, r.generation, nullptr);
}

} // namespace

/* No consent: nothing binds, the bind says why, the unlock says what is
 * missing, nothing is sent */
TEST(ConsentSessionTest, NoConsentBindsNothingAndTheUnlockSaysWhy)
{
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
    EXPECT_EQ(b.session->talk, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "plugin talker's conversation part isn't in use: no consent "
                 "given");
    EXPECT_EQ(unlock(b.session.get()), ECUIO_ERR_CONSENT_REQUIRED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no consent was given to the plugin whose code talks on "
                 "this channel");
}

/* A record applied: the module binds and unlocks the gated level */
TEST(ConsentSessionTest, AConsentAppliedBindsAndTheUnlockOpensTheGatedLevel)
{
    Store store("consent-session-binds.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string print = print_of(b.plugin.get());
    ASSERT_EQ(ecuio_session_apply_consent(b.session.get(), store.store,
                                          bytes_of(print)),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_TRUE(b.session->consent_applied);
    EXPECT_EQ(b.session->consent_given_utc, kUtc);
    EXPECT_EQ(std::memcmp(b.session->consent_fingerprint, print.data(), 32), 0);
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_NE(b.session->talk, nullptr);

    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bench.start(b.session.get()));
    bench.passes(8);
    EXPECT_EQ(bench.report().state, ecuio_conversation_state::talking);
    uint32_t generation = 0;
    const uint32_t before = bench.report(&generation).words_ended;
    ASSERT_EQ(ecuio_conversation_give_word(b.session.get(), 0, 0x01, 7, 1),
              ECUIO_OK);
    bench.until_word_ended(before);
    EXPECT_EQ(bench.report().unlock, ecuio_unlock_state::unlocked);
    EXPECT_EQ(bench.ecu.security.unlocked, 0x01);
}

TEST(ConsentSessionTest, ApplyRefusesWhatIsNotThisPluginsConsent)
{
    Store store("consent-session-refuses.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    const std::string print = print_of(b.plugin.get());
    ecuio_session_t* s = b.session.get();

    /* No record */
    EXPECT_EQ(ecuio_session_apply_consent(s, store.store, bytes_of(print)),
              ECUIO_ERR_CONSENT_REQUIRED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the consent store holds no consent to plugin talker 1.2.0");

    /* Another plugin's fingerprint, though the store holds it */
    conversation_test::Made other = made3();
    other.group.budget = "1000";
    const conversation_test::Plugin p2 =
        conversation_test::load_packed(other.json());
    ASSERT_NE(p2, nullptr);
    ASSERT_EQ(ecuio_consent_store_give(store.store, p2.get(), kUtc), ECUIO_OK);
    EXPECT_EQ(ecuio_session_apply_consent(s, store.store,
                                          bytes_of(print_of(p2.get()))),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the fingerprint isn't plugin talker 1.2.0's");

    /* After start */
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    s->started = true;
    EXPECT_EQ(ecuio_session_apply_consent(s, store.store, bytes_of(print)),
              ECUIO_ERR_INVALID_ARGUMENT);
    s->started = false;
    EXPECT_FALSE(s->consent_applied);

    EXPECT_EQ(
        ecuio_session_apply_consent(nullptr, store.store, bytes_of(print)),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_apply_consent(s, nullptr, bytes_of(print)),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_apply_consent(s, store.store, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    /* A format 2 plugin names no publisher: no consent can name it */
    Bound two;
    ASSERT_NO_FATAL_FAILURE(make_session(&two, made()));
    EXPECT_EQ(ecuio_session_apply_consent(two.session.get(), store.store,
                                          bytes_of(print_of(two.plugin.get()))),
              ECUIO_ERR_CONSENT_REQUIRED);
    ASSERT_EQ(ecuio_conversation_bind(two.session.get()), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "plugin talker's conversation part isn't in use: not "
                 "enabled");

    /* A session over no plugin's set */
    ecuio_session_t* plain = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x", &plain), ECUIO_OK);
    EXPECT_EQ(ecuio_session_apply_consent(plain, store.store, bytes_of(print)),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(plain);

    /* mock:// takes the consent and still binds nothing */
    Bound mock;
    ASSERT_NO_FATAL_FAILURE(make_session(&mock, made3(), "mock://x"));
    ASSERT_EQ(ecuio_session_apply_consent(mock.session.get(), store.store,
                                          bytes_of(print)),
              ECUIO_OK);
    ASSERT_EQ(ecuio_conversation_bind(mock.session.get()), ECUIO_OK);
    EXPECT_EQ(mock.session->talk, nullptr);
}

/* A session that asked nothing at create asks from start once a consent
 * would bind its module */
TEST(ConsentSessionTest, AConsentMakesAPassiveSessionAsk)
{
    Store store("consent-session-passive.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ecuio_session_t* s = b.session.get();
    s->info.flags &= ~ECUIO_SESSION_INFO_REQUESTS;
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_apply_consent(s, store.store,
                                          bytes_of(print_of(b.plugin.get()))),
              ECUIO_OK);
    EXPECT_NE(s->info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);

    Bound none;
    ASSERT_NO_FATAL_FAILURE(make_session(&none, made3()));
    none.session->info.flags &= ~ECUIO_SESSION_INFO_REQUESTS;
    EXPECT_EQ(ecuio_conversation_bind(none.session.get()), ECUIO_OK);
    EXPECT_EQ(none.session->info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);
}

/* The test door binds a format 3 plugin without a consent, as it does a
 * format 2 one */
TEST(ConsentSessionTest, TheDoorStillBindsWithoutConsent)
{
    DoorOpen open;
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_NE(b.session->talk, nullptr);
    EXPECT_FALSE(b.session->consent_applied);
}

TEST(ConsentSessionTest, OutOfMemoryInAnApplyIsAnswered)
{
    Store store("consent-session-memory.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    const std::string print = print_of(b.plugin.get());
    for (unsigned long n = 1; n <= 8; ++n) {
        fail_new_on_this_thread(n);
        const ecuio_status_t st = ecuio_session_apply_consent(
            b.session.get(), store.store, bytes_of(print));
        fail_new_on_this_thread(0);
        ASSERT_TRUE(st == ECUIO_OK || st == ECUIO_ERR_OUT_OF_MEMORY) << n;
    }
    EXPECT_TRUE(b.session->consent_applied);
}
