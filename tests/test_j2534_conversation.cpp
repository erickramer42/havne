// File/layer: tests/test_j2534_conversation.cpp | GTest, HARNESS tier,
//             Windows only (cross-arch too)
// Testing: HARNESS: J2534ConversationTest (test_j2534_health)
//
// A plugin's module bound to a J2534 session behind the test door
// (ADR-0066 section 2.4), end to end through the public calls and the
// surrogate against the fake DLL's engine ECU, strict: the module's key
// is the write straight after its seed, to the physical ID, and unlocks;
// the session log keeps the seed and the key out (access_cut.h), and
// names a consent applied or a part withheld for want of one.

#include <gtest/gtest.h>

#include "conversation_session_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "j2534_backend_fixture.h"
#include "sha256.h"       /* core/src */
#include "sim_security.h" /* core/src */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

namespace {

class J2534ConversationTest : public j2534_backends::BackendTest
{};

/* Every surrogate session log in TEMP written from since on */
std::vector<std::string>
logs_since(std::filesystem::file_time_type since)
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(fs::temp_directory_path())) {
        const std::string name = e.path().filename().u8string();
        if (name.rfind("ecuio-surrogate", 0) != 0 ||
            e.path().extension() != ".log" || e.last_write_time() < since) {
            continue;
        }
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream all;
        all << in.rdbuf();
        out.push_back(all.str());
    }
    return out;
}

/* bytes as the session log writes them: "27 02 11 22" */
std::string spaced(const uint8_t* b, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        char h[4];
        std::snprintf(h, sizeof h, i > 0 ? " %02X" : "%02X", b[i]);
        out += h;
    }
    return out;
}

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

} // namespace

TEST_F(J2534ConversationTest, TheModulesKeyUnlocksTheEngineEcu)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_SECURITY_STRICT", "1"));
    const j2534_backends::EnvSwap frame_logging("ECUIO_SUR_LOG_FRAMES", "1");
    const auto since = std::filesystem::file_time_type::clock::now() -
                       std::chrono::seconds(2);
    conversation_session::DoorOpen open;
    conversation_test::Plugin plugin =
        conversation_test::load_packed(conversation_session::made().json());
    ASSERT_NE(plugin, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    {
        const std::string query =
            std::string("?profile=") + name + "&identify=0&diagnose=0&gap=150";
        const std::string u = uri(query.c_str());
        ecuio_session_t* raw = nullptr;
        ASSERT_EQ(ecuio_session_create(u.c_str(), &raw), ECUIO_OK)
            << u << ": " << ecuio_last_error_detail();
        conversation_session::Session s(raw);
        ASSERT_EQ(ecuio_session_start(s.get()), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_channel_conversation_t c =
            report_when(s.get(), [](const ecuio_channel_conversation_t& r) {
                return r.state == ECUIO_CONVERSATION_TALKING;
            });
        ASSERT_EQ(c.state, ECUIO_CONVERSATION_TALKING);
        EXPECT_EQ(c.flags, ECUIO_CONVERSATION_NOT_KEPT);
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

        const ecuio_sim_security_level* l = ecuio_sim_security_find(0x01);
        uint8_t key[16] = {};
        ecuio_sim_security_key(l->seed, l->bytes, key);
        fake_control::Write w;
        int seed_at = -1;
        for (uint32_t i = 0; fake_control::recorded_write(page, i, &w); ++i) {
            const bool seed =
                w.size == 6 && w.data[4] == 0x27 && w.data[5] == 0x01;
            if (seed) {
                seed_at = static_cast<int>(i);
                EXPECT_EQ(w.data[2], 0x07);
                EXPECT_EQ(w.data[3], 0xE0) << "the physical ID";
            } else if (seed_at >= 0 && static_cast<int>(i) == seed_at + 1) {
                ASSERT_EQ(w.size, 4u + 2u + l->bytes);
                EXPECT_EQ(w.data[4], 0x27);
                EXPECT_EQ(w.data[5], 0x02);
                EXPECT_EQ(std::memcmp(w.data + 6, key, l->bytes), 0)
                    << "the module's key straight after its seed";
            }
        }
        EXPECT_GE(seed_at, 0);
    }
    const ecuio_sim_security_level* l = ecuio_sim_security_find(0x01);
    uint8_t key[16] = {};
    ecuio_sim_security_key(l->seed, l->bytes, key);
    const uint8_t zeroes[16] = {};
    const std::string cut_key =
        "27 02 " + spaced(zeroes, l->bytes) + " (4 bytes cut, SHA-256 " +
        ecuio_sha256::hex_of(key, l->bytes) + ")";
    const std::string cut_seed = "07 E8 67 01 " + spaced(zeroes, l->bytes) +
                                 " (4 bytes cut, SHA-256 " +
                                 ecuio_sha256::hex_of(l->seed, l->bytes) + ")";
    bool ours = false;
    for (const std::string& log : logs_since(since)) {
        EXPECT_EQ(log.find(spaced(key, l->bytes)), std::string::npos) << log;
        EXPECT_EQ(log.find(spaced(l->seed, l->bytes)), std::string::npos)
            << log;
        ours = ours || (log.find(cut_key) != std::string::npos &&
                        log.find(cut_seed) != std::string::npos);
    }
    EXPECT_TRUE(ours) << "no session log holds the cut key and seed";
}

namespace {

/* The surrogate's session logs since one session over m's plugin on base
 * (the fake's URI), consented or not, started and closed; prefix: its
 * fingerprint's 16 digits */
std::vector<std::string> session_logs_of(const std::string& base,
                                         const conversation_test::Made& m,
                                         bool consented, std::string* prefix)
{
    const auto since =
        std::filesystem::file_time_type::clock::now() - std::chrono::seconds(2);
    conversation_test::Plugin plugin = conversation_test::load_packed(m.json());
    EXPECT_NE(plugin, nullptr) << ecuio_last_error_detail();
    if (plugin == nullptr) {
        return {};
    }
    *prefix = conversation_session::print_prefix_of(plugin.get());
    char name[64] = {};
    EXPECT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    {
        const std::string u =
            base + "?profile=" + name + "&identify=0&diagnose=0";
        ecuio_session_t* raw = nullptr;
        EXPECT_EQ(ecuio_session_create(u.c_str(), &raw), ECUIO_OK)
            << u << ": " << ecuio_last_error_detail();
        conversation_session::Session s(raw);
        if (consented) {
            conversation_session::give_and_apply("j2534-consent.json",
                                                 plugin.get(), s.get());
        }
        EXPECT_EQ(ecuio_session_start(s.get()), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_session_close(s.get()), ECUIO_OK);
    }
    EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK);
    return logs_since(since);
}

bool any_holds(const std::vector<std::string>& logs, const std::string& line)
{
    for (const std::string& log : logs) {
        if (log.find(line) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

/* The client writes the consent line at start: applied with the
 * fingerprint's 16 digits, or withheld for want of one */
TEST_F(J2534ConversationTest, TheSessionLogNamesAConsentAppliedOrWithheld)
{
    std::string prefix;
    std::vector<std::string> logs =
        session_logs_of(uri(""), conversation_session::made3(), true, &prefix);
    EXPECT_TRUE(any_holds(logs, "consent applied  talker 1.2.0  " + prefix))
        << "no session log names the consent";
    logs =
        session_logs_of(uri(""), conversation_session::made3(), false, &prefix);
    EXPECT_TRUE(any_holds(logs, "consent withheld  talker 1.2.0"))
        << "no session log names the part withheld";
}
