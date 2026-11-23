// File/layer: tests/test_elm327_conversation.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327ConversationTest (test_elm327)
//
// A plugin's module bound to an ELM327 session behind the test door
// (ADR-0066 section 2.4), end to end through the public calls against
// the fake ELM327, whose engine ECU answers 27 as the simulated ECU does,
// strict: the module's begin enters the extended session, the unlock
// call's word asks the seed, the module's key is typed straight after
// it under the ECU's physical ID, and the store counts under the label.
// The wire log keeps the seed and the key out (access_cut.h).

#include <gtest/gtest.h>

#include "conversation_session_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "fake_elm327.h"
#include "sha256.h"       /* core/src */
#include "sim_security.h" /* core/src */
#include "test_elm327_support.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> typed()
{
    std::vector<std::string> out;
    fake_elm327::with([&](fake_elm327::adapter& a) {
        for (uint32_t i = 0; i < a.commands && i < a.kRecordDepth; ++i) {
            out.emplace_back(a.recorded[i]);
        }
    });
    return out;
}

std::string key_text(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    uint8_t key[16] = {};
    ecuio_sim_security_key(l->seed, l->bytes, key);
    char out[64] = {};
    int n = std::snprintf(out, sizeof out, "27%02X", level + 1);
    for (uint32_t i = 0; i < l->bytes; ++i) {
        n += std::snprintf(out + n, sizeof out - n, "%02X", key[i]);
    }
    return out;
}

/* The report once pred holds, or after 10 s */
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

class Elm327ConversationTest : public ::testing::Test
{
protected:
    fake_elm327::installed fake_;

    void SetUp() override
    {
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_strict() = true;
    }

    void TearDown() override
    {
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_strict() = false;
    }
};

} // namespace

TEST_F(Elm327ConversationTest, TheModulesKeyUnlocksTheEngineEcu)
{
    conversation_session::DoorOpen open;
    conversation_test::Plugin plugin =
        conversation_test::load_packed(conversation_session::made().json());
    ASSERT_NE(plugin, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    const std::string store = testing::TempDir() + "elm327_conversation.json";
    std::remove(store.c_str());
    const EnvSet frame_logging("ECUIO_SUR_LOG_FRAMES", "1");
    const LogDir logs("elm327-conversation");
    {
        ecuio_session_t* raw = nullptr;
        const std::string uri = std::string("elm327://") + kPort +
                                "?profile=" + name +
                                "&identify=0&diagnose=0&gap=100";
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &raw), ECUIO_OK)
            << ecuio_last_error_detail();
        conversation_session::Session s(raw);
        ASSERT_EQ(ecuio_session_security_store(s.get(), store.c_str(), "Car"),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_session_start(s.get()), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_channel_conversation_t c =
            report_when(s.get(), [](const ecuio_channel_conversation_t& r) {
                return r.state == ECUIO_CONVERSATION_TALKING;
            });
        ASSERT_EQ(c.state, ECUIO_CONVERSATION_TALKING);
        EXPECT_EQ(c.session, 3);
        uint32_t wait = 0;
        ecuio_status_t rc = ECUIO_ERR_TOO_SOON;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            ASSERT_EQ(ecuio_session_get_channel_conversation(s.get(), 0, &c),
                      ECUIO_OK);
            rc = ecuio_session_security_unlock(s.get(), 0, 0x01, c.generation,
                                               &wait);
            if (rc != ECUIO_ERR_REPORT_CHANGED) {
                break;
            }
        }
        ASSERT_EQ(rc, ECUIO_OK) << ecuio_last_error_detail();
        c = report_when(s.get(), [](const ecuio_channel_conversation_t& r) {
            return r.unlock == ECUIO_UNLOCK_UNLOCKED ||
                   r.unlock == ECUIO_UNLOCK_FAILED ||
                   r.unlock == ECUIO_UNLOCK_ENDED;
        });
        EXPECT_EQ(c.unlock, ECUIO_UNLOCK_UNLOCKED) << "nrc " << int{c.nrc};
        const std::vector<std::string> all = typed();
        std::string header;
        bool seed_seen = false;
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].rfind("ATSH", 0) == 0) {
                header = all[i];
            }
            if (all[i] == "2701") {
                seed_seen = true;
                EXPECT_EQ(header, "ATSH7E0") << "the physical ID";
                ASSERT_LT(i + 1, all.size());
                EXPECT_EQ(all[i + 1], key_text(0x01))
                    << "the key straight after its seed";
            }
        }
        EXPECT_TRUE(seed_seen);
        EXPECT_EQ(ecuio_plugin_free(plugin.get()), ECUIO_ERR_IN_USE)
            << "free is refused while a session holds the bound set";
        ASSERT_EQ(ecuio_session_close(s.get()), ECUIO_OK);
        EXPECT_TRUE(s->talk->bound[0].ended_in_session)
            << "the module's end came before the engine's 10 01";
        ASSERT_EQ(ecuio_session_get_channel_conversation(s.get(), 0, &c),
                  ECUIO_OK);
        EXPECT_EQ(c.state, ECUIO_CONVERSATION_ENDED);
    }
    std::remove(store.c_str());
    const ecuio_sim_security_level* l = ecuio_sim_security_find(0x01);
    uint8_t key[16] = {};
    ecuio_sim_security_key(l->seed, l->bytes, key);
    std::string seed_spaced;
    std::string seed_bare;
    for (uint32_t i = 0; i < l->bytes; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, "%02X", l->seed[i]);
        seed_spaced += std::string(i > 0 ? " " : "") + b;
        seed_bare += b;
    }
    const std::string log = logs.only_log();
    EXPECT_EQ(log.find(key_text(0x01)), std::string::npos) << log;
    EXPECT_EQ(log.find(seed_spaced), std::string::npos) << log;
    EXPECT_EQ(log.find(seed_bare), std::string::npos) << log;
    EXPECT_NE(log.find("> 2702 (4 bytes cut, SHA-256 " +
                       ecuio_sha256::hex_of(key, l->bytes) + ")"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("< (a security access's line, cut: "), std::string::npos)
        << log;
}
