// File/layer: tests/test_vcan_conversation.cpp | test-only, C++17, Qt-free,
//             Linux only
// Testing: HARNESS: SocketCanConversationTest (test_vcan)
//
// A plugin's module bound to a requesting socketcan:// session behind the
// test door (ADR-0066's appendix of 2026-11-22), end to end on vcan0
// against the strict simulated ECU: the module's begin enters the
// extended session, the unlock call's word asks the seed, the module's
// key unlocks, and at close the module's end comes before the 10 01.
// The consent gate's bus: vcan binds a format 3 plugin with no consent,
// and a link judged a car's doesn't.

#include "conversation_session_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/sim.h"
#include "fake_ecus.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

template <typename Pred>
ecuio_channel_conversation_t report_when(ecuio_session_t* s, Pred pred)
{
    ecuio_channel_conversation_t c{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        EXPECT_EQ(ecuio_session_get_channel_conversation(s, 0, &c), ECUIO_OK);
        if (pred(c)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return c;
}

class SocketCanConversationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string why;
        if (!vcan_bus::available(&why)) {
            const char* required = std::getenv("ECUIO_VCAN_REQUIRED");
            if (required != nullptr && std::string(required) == "1") {
                FAIL() << "ECUIO_VCAN_REQUIRED=1, but " << why;
            }
            GTEST_SKIP() << why;
        }
    }
};

} // namespace

TEST_F(SocketCanConversationTest, TheModulesKeyUnlocksTheSimulatedEcu)
{
    ecuio_sim_t* sim = nullptr;
    ASSERT_EQ(ecuio_sim_create_with("mock://sim?values=canned",
                                    vcan_bus::interface_name(), 0, &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_sim_test_set_strict(sim, 1);
    conversation_session::DoorOpen open;
    conversation_test::Plugin plugin =
        conversation_test::load_packed(conversation_session::made().json());
    ASSERT_NE(plugin, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    {
        ecuio_session_t* raw = nullptr;
        const std::string uri = std::string("socketcan://") +
                                vcan_bus::interface_name() + "?profile=" +
                                name + "&identify=0&diagnose=0";
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &raw), ECUIO_OK)
            << ecuio_last_error_detail();
        conversation_session::Session s(raw);
        ASSERT_EQ(ecuio_session_start(s.get()), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_channel_conversation_t c =
            report_when(s.get(), [](const ecuio_channel_conversation_t& r) {
                return r.state == ECUIO_CONVERSATION_TALKING;
            });
        ASSERT_EQ(c.state, ECUIO_CONVERSATION_TALKING);
        EXPECT_EQ(c.session, 3);
        ecuio_status_t rc = ECUIO_ERR_REPORT_CHANGED;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (rc == ECUIO_ERR_REPORT_CHANGED &&
               std::chrono::steady_clock::now() < deadline) {
            ASSERT_EQ(ecuio_session_get_channel_conversation(s.get(), 0, &c),
                      ECUIO_OK);
            rc = ecuio_session_security_unlock(s.get(), 0, 0x01, c.generation,
                                               nullptr);
        }
        ASSERT_EQ(rc, ECUIO_OK) << ecuio_last_error_detail();
        c = report_when(s.get(), [](const ecuio_channel_conversation_t& r) {
            return r.unlock == ECUIO_UNLOCK_UNLOCKED ||
                   r.unlock == ECUIO_UNLOCK_FAILED ||
                   r.unlock == ECUIO_UNLOCK_ENDED;
        });
        EXPECT_EQ(c.unlock, ECUIO_UNLOCK_UNLOCKED) << "nrc " << int{c.nrc};
        ASSERT_EQ(ecuio_session_close(s.get()), ECUIO_OK);
        EXPECT_TRUE(s->talk->bound[0].ended_in_session)
            << "the module's end came before the engine's 10 01";
    }
    ecuio_sim_destroy(sim);
}

namespace {

/* vcan judged a car's link for a case's life */
struct VcanAsCan {
    VcanAsCan() { ecuio_socketcan_test_vcan_as_can(1); }
    ~VcanAsCan() { ecuio_socketcan_test_vcan_as_can(0); }
    VcanAsCan(const VcanAsCan&) = delete;
    VcanAsCan& operator=(const VcanAsCan&) = delete;
};

/* Whether a session over profile on vcan0 bound its part at start */
bool binds(const char* profile)
{
    ecuio_session_t* raw = nullptr;
    const std::string uri = std::string("socketcan://") +
                            vcan_bus::interface_name() + "?profile=" + profile +
                            "&identify=0&diagnose=0";
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &raw), ECUIO_OK)
        << ecuio_last_error_detail();
    conversation_session::Session s(raw);
    EXPECT_EQ(ecuio_session_start(s.get()), ECUIO_OK)
        << ecuio_last_error_detail();
    const bool bound = s->talk != nullptr;
    EXPECT_EQ(ecuio_session_close(s.get()), ECUIO_OK);
    return bound;
}

} // namespace

TEST_F(SocketCanConversationTest, VcanBindsWithoutConsentAndACarsLinkDoesnt)
{
    ecuio_sim_t* sim = nullptr;
    ASSERT_EQ(ecuio_sim_create_with("mock://sim?values=canned",
                                    vcan_bus::interface_name(), 0, &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    conversation_test::Plugin plugin =
        conversation_test::load_packed(conversation_session::made3().json());
    ASSERT_NE(plugin, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    EXPECT_TRUE(binds(name)) << "no car is on vcan: no consent is asked";
    {
        const VcanAsCan as_can;
        EXPECT_FALSE(binds(name)) << "a car's link binds only with consent";
    }
    EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK);
    ecuio_sim_destroy(sim);
}
