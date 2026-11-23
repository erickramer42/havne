// File/layer: tests/test_elm327_modules.cpp | test_elm327, Qt-free
// Testing: HARNESS: Elm327ModulesTest (the modules a session opens by the
//          pick on the fake ELM327: the support report's CLOSED and
//          NO_ROOM flags and open_reasons, the info's module_budget;
//          ADR-0065 step 5)

#include "test_elm327_support.h"

#include "ecuio/signal_sets.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

class Elm327ModulesTest : public ::testing::Test
{
protected:
    fake_elm327::installed fake_;
};

/* The engine ECU (in the emissions range) and two modules outside it,
 * each with a DID: their signals 20000, 20001 and 20002. The fake's ECUs
 * answer only 7E0-7E7's requests, so the two are judged by what is typed
 * to them */
struct ThreeModules {
    ThreeModules()
    {
        EXPECT_TRUE(fake_ecu::set_module_dids("7B8:D801=00 11;7B9:D802=00 12"));
    }
    ~ThreeModules() { fake_ecu::set_module_dids(nullptr); }
    ThreeModules(const ThreeModules&) = delete;
    ThreeModules& operator=(const ThreeModules&) = delete;
};
const char kThreeModules[] = "?profile=uds-generic&did=1E1C,7B8:D801,7B9:D802"
                             "&identify=0&diagnose=0";

ecuio_channel_support_t support_of(ecuio_session_t* s, uint32_t c)
{
    ecuio_channel_support_t sup{};
    EXPECT_EQ(ecuio_session_get_channel_support(s, c, &sup), ECUIO_OK);
    return sup;
}

/* Whether the support reports' flags (CLOSED, NO_ROOM) read want[c] for
 * every channel within the time */
bool states_reach(ecuio_session_t* s, const std::vector<uint32_t>& want)
{
    const uint32_t mask = ECUIO_SUPPORT_CLOSED | ECUIO_SUPPORT_NO_ROOM;
    return wait_for(
        [&] {
            for (uint32_t c = 0; c < want.size(); ++c) {
                if ((support_of(s, c).flags & mask) != want[c]) {
                    return false;
                }
            }
            return true;
        },
        5000);
}

} // namespace

/* A pick of one module's signal: that module open for the read, the
 * engine ECU open as an emissions module, the third closed and asked
 * nothing */
TEST_F(Elm327ModulesTest, TheSupportReportSaysWhichModulesAreClosed)
{
    ThreeModules modules;
    Session s;
    ASSERT_EQ(s.create(uri(kThreeModules)), ECUIO_OK)
        << ecuio_last_error_detail();
    const uint16_t pick = ECUIO_UDS_GENERIC_SIG_ID_BASE + 1;
    ASSERT_EQ(ecuio_session_set_focus(s.s, &pick, 1), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(states_reach(s.s, {0, 0, ECUIO_SUPPORT_CLOSED}));
    EXPECT_EQ(support_of(s.s, 0).open_reasons, ECUIO_MODULE_REASON_RANGE);
    EXPECT_EQ(support_of(s.s, 1).open_reasons, ECUIO_MODULE_REASON_READ);
    EXPECT_EQ(support_of(s.s, 2).open_reasons, 0u);
    ASSERT_TRUE(wait_for([] { return fake_count("22D801") >= 3; }, 5000));
    for (const std::string& cmd : fake_commands()) {
        EXPECT_EQ(cmd.find("22D802"), std::string::npos)
            << "the closed module was asked: " << cmd;
    }
}

/* No focus on a set asked whole: every module is read */
TEST_F(Elm327ModulesTest, NoFocusAsksTheWholeTable)
{
    ThreeModules modules;
    Session s;
    ASSERT_EQ(s.create(uri(kThreeModules)), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([] { return fake_count("22D802") >= 1; }, 5000));
    for (uint32_t c = 0; c < 3; ++c) {
        const ecuio_channel_support_t sup = support_of(s.s, c);
        EXPECT_EQ(sup.flags & (ECUIO_SUPPORT_CLOSED | ECUIO_SUPPORT_NO_ROOM),
                  0u)
            << c;
        EXPECT_NE(sup.open_reasons & ECUIO_MODULE_REASON_READ, 0u) << c;
    }
}

/* A pick moved to the third module closes the second and opens the third */
TEST_F(Elm327ModulesTest, APickMovedClosesOneModuleAndOpensAnother)
{
    ThreeModules modules;
    Session s;
    ASSERT_EQ(s.create(uri(kThreeModules)), ECUIO_OK)
        << ecuio_last_error_detail();
    uint16_t pick = ECUIO_UDS_GENERIC_SIG_ID_BASE + 1;
    ASSERT_EQ(ecuio_session_set_focus(s.s, &pick, 1), ECUIO_OK);
    s.start();
    ASSERT_TRUE(states_reach(s.s, {0, 0, ECUIO_SUPPORT_CLOSED}));
    pick = ECUIO_UDS_GENERIC_SIG_ID_BASE + 2;
    ASSERT_EQ(ecuio_session_set_focus(s.s, &pick, 1), ECUIO_OK);
    ASSERT_TRUE(states_reach(s.s, {0, ECUIO_SUPPORT_CLOSED, 0}));
    ASSERT_TRUE(wait_for([] { return fake_count("22D802") >= 1; }, 5000));
}

/* The info names the budget: the ELM327's one filter over every open
 * module leaves the module cap's 32 less the one kept back */
TEST_F(Elm327ModulesTest, TheInfoGivesTheBudget)
{
    Session s;
    ASSERT_EQ(s.create(uri("?identify=0&diagnose=0")), ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_EQ(info.module_budget, ECUIO_MAX_CHANNELS - 1u);
}
