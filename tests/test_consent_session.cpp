// File/layer: tests/test_consent_session.cpp | test, C++17, Qt-free
// Testing: UNIT: ConsentSessionTest (test_core)
//
// The consent gate (consent_gate.h, core_consent.cpp): a format 3
// plugin's module binds only with a consent applied between create and
// start, then unlocks the simulated ECU's gated level; apply's refusals;
// the unlock's ECUIO_ERR_CONSENT_REQUIRED; a session that asked nothing
// asks once a consent binds; the test door unchanged; the recording's
// consent line.

#include "conversation_session_support.h"
#include "core_test_support.h"

#include "../core/src/sha256.h"
#include "ecuio/recording.h"

#include <gtest/gtest.h>
#include <ecuio/plugin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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

namespace {

namespace fs = std::filesystem;

/* events.log of a recording of b's session, its line ends LF, after a
 * folder export too (copy_events) */
std::string recorded_events(Bound& b, const char* name,
                            std::string* copy_events = nullptr)
{
    const fs::path root = fs::u8path(testing::TempDir()) / name;
    fs::remove_all(root);
    const std::string root_text = root.u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root_text.c_str();
    ecuio_recorder_t* rec = nullptr;
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char folder[1024] = {};
    uint32_t len = 0;
    EXPECT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(b.session.get(), rec), ECUIO_OK);
    b.session.reset();
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
    const auto events_of = [](const fs::path& f) {
        std::ifstream in(f / "events.log", std::ios::binary);
        std::stringstream all;
        all << in.rdbuf();
        std::string text = all.str();
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    };
    const std::string events = events_of(fs::u8path(folder));
    ecuio_recording_t* r = nullptr;
    EXPECT_EQ(ecuio_recording_open(folder, nullptr, nullptr, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_close(r);
    if (copy_events != nullptr) {
        const std::string copy = (root / "copy").u8string();
        ecuio_export_options_t x{};
        x.format = ECUIO_EXPORT_FOLDER;
        x.folder_utf8 = folder;
        x.out_path_utf8 = copy.c_str();
        EXPECT_EQ(ecuio_export(&x, nullptr), ECUIO_OK)
            << ecuio_last_error_detail();
        *copy_events = events_of(fs::u8path(copy));
    }
    fs::remove_all(root);
    return events;
}

} // namespace

/* A consented session's header says what was consented to and when,
 * after the conversation line; the reader opens it and a folder export
 * keeps it */
TEST(ConsentSessionTest, AConsentedSessionsHeaderSaysWhatAndWhen)
{
    Store store("consent-session-header.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    const std::string print = print_of(b.plugin.get());
    ASSERT_EQ(ecuio_session_apply_consent(b.session.get(), store.store,
                                          bytes_of(print)),
              ECUIO_OK);
    const std::string line =
        "consent  " +
        ecuio_sha256::hex(reinterpret_cast<const uint8_t*>(print.data())) +
        ",1795132800\n";
    std::string copied;
    const std::string events = recorded_events(b, "consent_header", &copied);
    const size_t at = events.find("\n" + line);
    ASSERT_NE(at, std::string::npos) << events;
    EXPECT_LT(events.find("\nconversation  "), at);
    EXPECT_NE(copied.find("\n" + line), std::string::npos) << copied;
}

/* Behind the door, or with no consent, no consent line */
TEST(ConsentSessionTest, TheDoorWritesNoConsentLine)
{
    {
        DoorOpen open;
        Bound b;
        ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
        const std::string events = recorded_events(b, "consent_door");
        EXPECT_NE(events.find("\nconversation  "), std::string::npos);
        EXPECT_EQ(events.find("\nconsent  "), std::string::npos) << events;
    }
    Bound none;
    ASSERT_NO_FATAL_FAILURE(make_session(&none, made3()));
    EXPECT_EQ(recorded_events(none, "consent_none").find("\nconsent  "),
              std::string::npos);
}

/* A recorder attached first has written its header without the consent:
 * apply is refused while it is attached, and taken once it has closed */
TEST(ConsentSessionTest, ApplyIsRefusedWhileARecorderIsAttached)
{
    Store store("consent-session-recorder.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    const std::string print = print_of(b.plugin.get());
    const fs::path root = fs::u8path(testing::TempDir()) / "consent_first";
    fs::remove_all(root);
    const std::string root_text = root.u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root_text.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(b.session.get(), rec), ECUIO_OK);

    EXPECT_EQ(ecuio_session_apply_consent(b.session.get(), store.store,
                                          bytes_of(print)),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "consent is applied before a recorder attaches");
    EXPECT_FALSE(b.session->consent_applied);

    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
    fs::remove_all(root);
    ASSERT_EQ(ecuio_session_apply_consent(b.session.get(), store.store,
                                          bytes_of(print)),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_NE(recorded_events(b, "consent_after").find("\nconsent  "),
              std::string::npos);
}

TEST(ConsentSessionTest, TheSessionSaysWhetherAPartWantsConsent)
{
    using conversation_session::wants_consent;
    Store store("consent-session-wants.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made3()));
    EXPECT_EQ(wants_consent(b.session.get()), 1u);
    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_apply_consent(b.session.get(), store.store,
                                          bytes_of(print_of(b.plugin.get()))),
              ECUIO_OK);
    EXPECT_EQ(wants_consent(b.session.get()), 0u);

    for (const char* uri : {"mock://x", "replay://x"}) {
        Bound none;
        ASSERT_NO_FATAL_FAILURE(make_session(&none, made3(), uri));
        EXPECT_EQ(wants_consent(none.session.get()), 0u) << uri;
    }
    Bound two;
    ASSERT_NO_FATAL_FAILURE(make_session(&two, made()));
    EXPECT_EQ(wants_consent(two.session.get()), 0u);
    {
        DoorOpen open;
        Bound door;
        ASSERT_NO_FATAL_FAILURE(make_session(&door, made3()));
        EXPECT_EQ(wants_consent(door.session.get()), 0u);
    }
    ecuio_session_t* plain = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x", &plain), ECUIO_OK);
    EXPECT_EQ(wants_consent(plain), 0u);
    ecuio_session_destroy(plain);

    Bound closed;
    ASSERT_NO_FATAL_FAILURE(make_session(&closed, made3()));
    ASSERT_EQ(ecuio_session_close(closed.session.get()), ECUIO_OK);
    uint32_t out = 7;
    EXPECT_EQ(ecuio_session_wants_consent(closed.session.get(), &out),
              ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(out, 0u);
    EXPECT_EQ(ecuio_session_wants_consent(closed.session.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

/* A data plugin of format 3 that declares its second module extended */
conversation_test::Made declaring()
{
    conversation_test::Made m = made3();
    m.tiers = R"(["data"])";
    m.has_group = false;
    m.has_binary = false;
    m.ecus = R"([{"id": "pcm", "rx": "7E8"}, {"id": "tcm", "rx": "7E9", )"
             R"("session": "extended"}, {"id": "abs", "rx": "7EA"}])";
    return m;
}

uint32_t channel_of(const ecuio_session_t* s, uint32_t rx)
{
    for (uint32_t c = 0; c < s->module_rx.size(); ++c) {
        if (s->module_rx[c] == rx) {
            return c;
        }
    }
    return UINT32_MAX;
}

TEST(ConsentSessionTest, AConsentToTheDeclaringPluginAllowsItsModulesSession)
{
    Store store("consent-session-declared.json");
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, declaring()));
    ecuio_session_t* s = b.session.get();
    const std::string print = print_of(b.plugin.get());
    ASSERT_EQ(s->declared_prints.size(), 1u);
    EXPECT_EQ(s->declared_prints[0].rx, 0x7E9u);
    EXPECT_EQ(std::memcmp(s->declared_prints[0].fingerprint, print.data(), 32),
              0);
    const uint32_t tcm = channel_of(s, 0x7E9);
    ASSERT_LT(tcm, 32u);

    uint32_t count = 99;
    ASSERT_EQ(ecuio_session_declared_prints(s, nullptr, 0, &count), ECUIO_OK);
    EXPECT_EQ(count, 1u);
    ecuio_declared_print_t got[2];
    std::memset(got, 0xA5, sizeof got);
    ASSERT_EQ(ecuio_session_declared_prints(s, got, 2, &count), ECUIO_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(got[0].rx_id, 0x7E9u);
    EXPECT_EQ(got[0].flags, 0u);
    EXPECT_EQ(std::memcmp(got[0].fingerprint, print.data(), 32), 0);
    for (const uint32_t x : got[0].reserved) {
        EXPECT_EQ(x, 0u);
    }
    EXPECT_EQ(got[1].rx_id, 0xA5A5A5A5u) << "past the count untouched";
    EXPECT_EQ(ecuio_session_declared_prints(s, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_declared_prints(s, got, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_declared_prints(nullptr, got, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);

    EXPECT_EQ(ecuio_session_apply_consent(s, store.store, bytes_of(print)),
              ECUIO_ERR_CONSENT_REQUIRED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the consent store holds no consent to the part that "
                 "declares a module's session");
    EXPECT_EQ(s->declared_allowed, 0u);

    conversation_test::Made other = declaring();
    other.publisher = R"({"name": "Other Tools", "contact": "c", )"
                      R"("license": "MIT", "terms": "Mine."})";
    const conversation_test::Plugin p2 =
        conversation_test::load_packed(other.json());
    ASSERT_NE(p2, nullptr) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_consent_store_give(store.store, p2.get(), kUtc), ECUIO_OK);
    EXPECT_EQ(ecuio_session_apply_consent(s, store.store,
                                          bytes_of(print_of(p2.get()))),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the fingerprint is no part's that declares a module's "
                 "session");

    ASSERT_EQ(ecuio_consent_store_give(store.store, b.plugin.get(), kUtc),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_apply_consent(s, store.store, bytes_of(print)),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(s->declared_allowed, 1u << tcm);
    EXPECT_FALSE(s->consent_applied) << "no conversation was consented to";
}

TEST(ConsentSessionTest, AConsentToAnOwnFilesBytesAllowsItsModulesSession)
{
    const std::string text =
        R"({"havne_signals": 1, "name": "Own declared", "source": {"name": "t"},)"
        R"( "ecus": [{"id": "tcm", "rx": "7E9", "session": "extended"}],)"
        R"( "signals": [{"key": "a", "name": "A", "read": {"service": "22",)"
        R"( "number": "D100"}, "field": {"byte": 0}}]})";
    ecuio_signal_file_t* f = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.data(), text.size(), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_profile_register("user.own-declared",
                                     ecuio_signal_file_set(f), nullptr),
              ECUIO_OK);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=user.own-declared", &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(s->declared_prints.size(), 1u);
    EXPECT_EQ(s->declared_prints[0].rx, 0x7E9u);
    EXPECT_EQ(ecuio_sha256::hex(s->declared_prints[0].fingerprint),
              ecuio_sha256::hex_of(text.data(), text.size()));

    Store store("consent-session-own.json");
    ASSERT_EQ(ecuio_consent_store_give_file(store.store, text.data(),
                                            text.size(), "Own.json", kUtc),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_apply_consent(s, store.store,
                                          s->declared_prints[0].fingerprint),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const uint32_t tcm = channel_of(s, 0x7E9);
    ASSERT_LT(tcm, 32u);
    EXPECT_EQ(s->declared_allowed, 1u << tcm);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_profile_unregister("user.own-declared"), ECUIO_OK);
    ecuio_signal_file_free(f);
}
