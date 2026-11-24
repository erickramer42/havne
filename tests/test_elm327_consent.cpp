// File/layer: tests/test_elm327_consent.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)
//
// Consent on the fake ELM327 (ADR-0066 section 2.4): the wire log names
// a consent applied, or a part withheld for want of one, at start.

#include "test_elm327_support.h"

#include "conversation_session_support.h"

#include <ecuio/plugin.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using conversation_session::give_and_apply;
using conversation_session::made3;
using conversation_session::print_prefix_of;

/* A plugin registered for a case, its profile gone before it is */
struct Registered {
    conversation_test::Plugin plugin;
    char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
    explicit Registered(const conversation_test::Made& m)
        : plugin(conversation_test::load_packed(m.json()))
    {
        EXPECT_NE(plugin, nullptr) << ecuio_last_error_detail();
        if (plugin != nullptr) {
            EXPECT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
                      ECUIO_OK)
                << ecuio_last_error_detail();
        }
    }
    ~Registered() { (void)ecuio_profile_unregister(name); }
    Registered(const Registered&) = delete;
    Registered& operator=(const Registered&) = delete;
};

/* The wire log of one session over r's profile, consented or not,
 * started and closed */
std::string log_of(const char* dir, Registered& r, bool consented)
{
    const LogDir logs(dir);
    {
        Session s;
        EXPECT_EQ(s.create(uri(std::string("?profile=") + r.name +
                               "&identify=0&diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        if (consented) {
            give_and_apply("elm327-consent.json", r.plugin.get(), s.s);
        }
        s.start();
        EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    }
    return logs.only_log();
}

} // namespace

TEST_F(Elm327SessionTest, TheWireLogNamesAConsentAppliedAtStart)
{
    Registered r(made3());
    ASSERT_NE(r.plugin, nullptr);
    const std::string prefix = print_prefix_of(r.plugin.get());
    const std::string log = log_of("elm327-consent-applied", r, true);
    const size_t at = log.find("consent applied  talker 1.2.0  " + prefix);
    ASSERT_NE(at, std::string::npos) << log;
    const size_t end = at + 31 + 16;
    EXPECT_TRUE(end < log.size() && (log[end] == '\r' || log[end] == '\n'))
        << "16 digits, then the line's end";
    EXPECT_EQ(log.find("consent withheld"), std::string::npos) << log;
}

TEST_F(Elm327SessionTest, TheWireLogNamesAPartWithheldForWantOfConsent)
{
    Registered r(made3());
    ASSERT_NE(r.plugin, nullptr);
    const std::string log = log_of("elm327-consent-withheld", r, false);
    EXPECT_NE(log.find("consent withheld  talker 1.2.0"), std::string::npos)
        << log;
    EXPECT_EQ(log.find("consent applied"), std::string::npos) << log;
}

/* A part with no publisher can't be consented to, and one behind the
 * test door binds: neither is withheld for want of consent */
TEST_F(Elm327SessionTest, NoConsentLineForAFormat2PartOrBehindTheDoor)
{
    {
        Registered r(conversation_session::made());
        ASSERT_NE(r.plugin, nullptr);
        const std::string log = log_of("elm327-consent-format2", r, false);
        EXPECT_EQ(log.find("consent "), std::string::npos) << log;
    }
    conversation_session::DoorOpen open;
    Registered r(made3());
    ASSERT_NE(r.plugin, nullptr);
    const std::string log = log_of("elm327-consent-door", r, false);
    EXPECT_EQ(log.find("consent "), std::string::npos) << log;
}
