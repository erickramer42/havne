#include "test_elm327_support.h"

#include <ecuio/plugin.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const std::string kDeclaring =
    R"({"havne_signals": 1, "name": "Declaring", "source": {"name": "t"},)"
    R"( "ecus": [{"id": "pcm", "rx": "7E8", "session": "extended"}],)"
    R"( "signals": [{"key": "a", "name": "A", "ecu": "pcm", "read":)"
    R"( {"service": "22", "number": "1E1C"}, "field": {"byte": 0, "bits": 16}}]})";

struct Declared {
    ecuio_signal_file_t* file = nullptr;
    Declared()
    {
        EXPECT_EQ(
            ecuio_signal_file_load(kDeclaring.data(), kDeclaring.size(), &file),
            ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_profile_register("user.elm-declared",
                                         ecuio_signal_file_set(file), nullptr),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Declared()
    {
        (void)ecuio_profile_unregister("user.elm-declared");
        ecuio_signal_file_free(file);
    }
    Declared(const Declared&) = delete;
    Declared& operator=(const Declared&) = delete;
};

/* The person's consent to the file's bytes, applied through the print
 * the session lists */
void consent(ecuio_session_t* s)
{
    const std::string path = testing::TempDir() + "elm327-declared.json";
    std::remove(path.c_str());
    ecuio_consent_store_t* store = nullptr;
    ASSERT_EQ(ecuio_consent_store_open(path.c_str(), &store), ECUIO_OK);
    EXPECT_EQ(ecuio_consent_store_give_file(store, kDeclaring.data(),
                                            kDeclaring.size(), "Declaring.json",
                                            1795132800),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_declared_print_t print{};
    uint32_t count = 0;
    EXPECT_EQ(ecuio_session_declared_prints(s, &print, 1, &count), ECUIO_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(ecuio_session_apply_consent(s, store, print.fingerprint),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_consent_store_close(store);
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
}

/* 1E1C answered n times */
bool reads(Session& s, int n)
{
    const std::vector<uint8_t> engine = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    return wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            int a = 0;
            for (const Capture::Frame& f : s.cap.frames) {
                a += f.id == 0x7E8u && f.data == engine;
            }
            return a >= n;
        },
        5000);
}

size_t entries_of(const char* query, bool consented)
{
    SessionsReset reset;
    Session s;
    EXPECT_EQ(s.create(uri(query)), ECUIO_OK) << ecuio_last_error_detail();
    if (consented) {
        consent(s.s);
    }
    s.start();
    EXPECT_TRUE(reads(s, 3));
    const size_t n = fake_count("1003");
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    return n;
}

constexpr char kQuery[] = "?profile=user.elm-declared&identify=0&diagnose=0";

} // namespace

TEST_F(Elm327SessionTest, ADeclaredSessionWaitsForConsent)
{
    Declared d;
    EXPECT_EQ(entries_of(kQuery, false), 0u);
}

TEST_F(Elm327SessionTest, AConsentToTheFileEntersItsDeclaredSession)
{
    Declared d;
    EXPECT_EQ(entries_of(kQuery, true), 1u);
}

TEST_F(Elm327SessionTest, SessionOneKeepsAConsentedDeclarationInDefault)
{
    Declared d;
    EXPECT_EQ(entries_of("?profile=user.elm-declared&session=01&identify=0"
                         "&diagnose=0",
                         true),
              0u);
}

/* Through the fake: consent applied, 10 03; a clear leaves first
 * (10 01), sends its 04, reads again, then enters again */
TEST_F(Elm327SessionTest, AClearOnAHeldModuleLeavesItsSessionAndEntersAgain)
{
    Declared d;
    SessionsReset reset;
    fake_ecu::reset_diagnostics();
    Session s;
    ASSERT_EQ(s.create(uri("?profile=user.elm-declared&identify=0")), ECUIO_OK)
        << ecuio_last_error_detail();
    consent(s.s);
    s.start();
    ASSERT_TRUE(reads(s, 3));
    ASSERT_EQ(fake_count("1003"), 1u);
    ecuio_channel_diagnostics_t r{};
    const auto report = [&] {
        return ecuio_session_get_channel_diagnostics(s.s, 0, &r) == ECUIO_OK &&
               (r.flags & ECUIO_DIAG_COMPLETE) != 0;
    };
    ASSERT_TRUE(wait_for(report, 8000));
    fake_elm327::with([](fake_elm327::adapter& a) { a.commands = 0; });
    ASSERT_EQ(ecuio_session_clear_diagnostics_with(
                  s.s, 0, r.generation,
                  ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING),
              ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            return report() && r.clear == ECUIO_CLEAR_CLEARED &&
                   fake_count("1003") == 1;
        },
        8000))
        << "clear " << int{r.clear};
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    const std::vector<std::string> sent = fake_commands();
    const auto at = [&](const char* want) {
        for (size_t i = 0; i < sent.size(); ++i) {
            if (sent[i].rfind(want, 0) == 0) {
                return i;
            }
        }
        return sent.size();
    };
    const size_t leave = at("1001");
    const size_t clear = static_cast<size_t>(
        std::find(sent.begin(), sent.end(), "04") - sent.begin());
    const size_t entry = at("1003");
    EXPECT_LT(leave, clear);
    EXPECT_LT(clear, entry);
    EXPECT_LT(entry, sent.size());
    fake_ecu::reset_diagnostics();
}
