// File/layer: tests/test_elm327_consent.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)
//
// Consent on the fake ELM327 (ADR-0066 section 2.4): the wire log names
// a consent applied, or a part withheld for want of one, at start; and
// an adapter set up to listen at attach is probed and set up again at
// start once a consent made the session ask.

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

size_t index_of(const std::vector<std::string>& v, const std::string& what,
                size_t from = 0)
{
    const auto at =
        std::find(v.begin() + static_cast<long>(from), v.end(), what);
    return static_cast<size_t>(at - v.begin());
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

TEST_F(Elm327SessionTest, TheSessionSaysWhetherAPartWantsConsent)
{
    for (const bool consented : {false, true}) {
        Registered r(made3());
        ASSERT_NE(r.plugin, nullptr);
        Session s;
        ASSERT_EQ(s.create(uri(std::string("?profile=") + r.name +
                               "&identify=0&diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(conversation_session::wants_consent(s.s), 1u);
        if (consented) {
            give_and_apply("elm327-consent-wants.json", r.plugin.get(), s.s);
        }
        EXPECT_EQ(conversation_session::wants_consent(s.s),
                  consented ? 0u : 1u);
        s.start();
        EXPECT_EQ(conversation_session::wants_consent(s.s),
                  consented ? 0u : 1u);
    }
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

/* Attach set the adapter up to listen (flow control off); the consent
 * applied after it makes the session ask, so start sets it up again
 * before the module's first request */
TEST_F(Elm327SessionTest, AConsentAfterAPassiveSetupSetsTheAdapterUpAgain)
{
    Registered r(made3(true));
    ASSERT_NE(r.plugin, nullptr);
    Session s;
    ASSERT_EQ(s.create(uri(std::string("?profile=") + r.name +
                           "&identify=0&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const std::vector<std::string> at_attach = fake_commands();
    ASSERT_LT(index_of(at_attach, "ATCFC0"), at_attach.size())
        << "attach set the adapter up to listen";
    ASSERT_NO_FATAL_FAILURE(
        give_and_apply("elm327-consent-passive.json", r.plugin.get(), s.s));
    s.start();
    ASSERT_TRUE(wait_for([] { return fake_count("1003") > 0; }, 5000))
        << "the module entered its session";
    const std::vector<std::string> all = fake_commands();
    const size_t again = index_of(all, "ATCFC1", at_attach.size());
    EXPECT_LT(again, all.size()) << "the setup ran again, flow control on";
    EXPECT_LT(again, index_of(all, "1003")) << "before the first request";
    EXPECT_EQ(index_of(all, "ATMA"), all.size()) << "never a monitor";
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
}

/* Such a session's start probe runs at start, before the setup again,
 * and its line follows the consent's in the wire log */
TEST_F(Elm327SessionTest, AConsentAfterAPassiveSetupProbesAtStart)
{
    Registered r(made3(true));
    ASSERT_NE(r.plugin, nullptr);
    const LogDir logs("elm327-consent-probe");
    {
        Session s;
        ASSERT_EQ(s.create(uri(std::string("?profile=") + r.name +
                               "&identify=0&diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        const std::vector<std::string> at_attach = fake_commands();
        ASSERT_EQ(index_of(at_attach, kProbeCommand), at_attach.size())
            << "a listening attach probes nothing";
        ASSERT_NO_FATAL_FAILURE(
            give_and_apply("elm327-consent-probe.json", r.plugin.get(), s.s));
        s.start();
        ASSERT_TRUE(wait_for([] { return fake_count("1003") > 0; }, 5000))
            << "the module entered its session";
        const std::vector<std::string> all = fake_commands();
        const size_t probe = index_of(all, kProbeCommand, at_attach.size());
        ASSERT_LT(probe, all.size()) << "the probe ran at start";
        EXPECT_EQ(all[probe - 1], "ATSH7DF") << "under the functional header";
        EXPECT_LT(probe, index_of(all, "ATCFC1", at_attach.size()))
            << "before the setup again";
        ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    }
    const std::string log = logs.only_log();
    const size_t consent = log.find("] consent applied  talker 1.2.0  ");
    ASSERT_NE(consent, std::string::npos) << log;
    EXPECT_NE(log.find("] emissions probe: 1 functional 01 00 in ", consent),
              std::string::npos)
        << log;
}

static size_t count_of(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos;
         at = text.find(what, at + what.size())) {
        ++n;
    }
    return n;
}

/* The start's probe refused (the functional header answered '?'): one
 * line, the refusal's words */
TEST_F(Elm327SessionTest, ARefusedStartProbeWritesOneLine)
{
    Registered r(made3(true));
    ASSERT_NE(r.plugin, nullptr);
    const LogDir logs("elm327-probe-refused");
    {
        Session s;
        ASSERT_EQ(s.create(uri(std::string("?profile=") + r.name +
                               "&identify=0&diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_NO_FATAL_FAILURE(
            give_and_apply("elm327-probe-refused.json", r.plugin.get(), s.s));
        set_knobs([](fake_elm327::knobs& k) { k.refuse = "ATSH7DF"; });
        s.start();
        ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    }
    const std::string log = logs.only_log();
    EXPECT_EQ(count_of(log, "ATSH7DF with '?'"), 1u) << log;
    EXPECT_EQ(count_of(log, "refused"), 0u) << log;
}

/* The probe switched off: no probe at start and no probe line */
TEST_F(Elm327SessionTest, NoProbeLineWhenTheStartProbeIsOff)
{
    Registered r(made3(true));
    ASSERT_NE(r.plugin, nullptr);
    const LogDir logs("elm327-probe-off");
    ecuio_set_start_probe(0);
    {
        Session s;
        ASSERT_EQ(s.create(uri(std::string("?profile=") + r.name +
                               "&identify=0&diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_NO_FATAL_FAILURE(
            give_and_apply("elm327-probe-off.json", r.plugin.get(), s.s));
        s.start();
        ASSERT_TRUE(wait_for([] { return fake_count("1003") > 0; }, 5000));
        EXPECT_EQ(fake_count(kProbeCommand), 0u);
        ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    }
    ecuio_set_start_probe(1);
    const std::string log = logs.only_log();
    EXPECT_EQ(log.find("emissions probe"), std::string::npos) << log;
    EXPECT_EQ(log.find("start's probe"), std::string::npos) << log;
}
