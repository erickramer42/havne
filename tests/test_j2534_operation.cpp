// File/layer: tests/test_j2534_operation.cpp | GTest, HARNESS tier,
//             Windows only (cross-arch too)
// Testing: HARNESS: J2534OperationTest (test_j2534_health)
//
// A plugin's dump run in a J2534 session behind the test door (ADR-0066
// section 4), end to end through the public calls and the surrogate
// against the fake DLL's engine ECU, whose memory is the simulated ECU's:
// the copy saved is the memory's bytes and none of them is in the
// surrogate's session log, a read that differs between the two passes
// keeps nothing, and a cancel leaves the folder empty.

#include <gtest/gtest.h>

#include "dump_file_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "fake_ecu.h"
#include "j2534_backend_fixture.h"
#include "operation_plan_support.h"
#include "sha256.h" /* core/src */

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using session_operation::kHighAt;
using session_operation::kHighBytes;
using session_operation::kLowAt;
using session_operation::kLowBytes;

/* Every surrogate session log in TEMP written from since on */
std::vector<std::string> logs_since(std::filesystem::file_time_type since)
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

class J2534OperationTest : public j2534_backends::BackendTest
{
protected:
    conversation_session::DoorOpen door_;
    conversation_test::Plugin plugin_;
    char name_[64] = {};
    dump_file_test::Folder folder_;

    void SetUp() override
    {
        j2534_backends::BackendTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        knob("ECUIO_FAKE_ENGINE_OFF", "1");
        plugin_ =
            conversation_test::load_packed(session_operation::made().json());
        ASSERT_NE(plugin_, nullptr) << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_plugin_register(plugin_.get(), 0, name_, sizeof name_),
                  ECUIO_OK);
    }

    void TearDown() override
    {
        if (name_[0] != '\0') {
            EXPECT_EQ(ecuio_profile_unregister(name_), ECUIO_OK);
        }
        j2534_backends::BackendTest::TearDown();
    }

    /* A started session over the plugin, its module talking */
    void open(conversation_session::Session* s)
    {
        const std::string u =
            uri((std::string("?profile=") + name_ + "&identify=0&diagnose=0")
                    .c_str());
        ecuio_session_t* raw = nullptr;
        ASSERT_EQ(ecuio_session_create(u.c_str(), &raw), ECUIO_OK)
            << ecuio_last_error_detail();
        s->reset(raw);
        ASSERT_EQ(ecuio_session_start(raw), ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_NO_FATAL_FAILURE(session_operation::talking(raw));
    }

    ecuio_status_t begin(ecuio_session_t* s)
    {
        const ecuio_operation_request_t q =
            session_operation::request(ECUIO_OPERATION_MAY_UNLOCK);
        return ecuio_session_operation_start(s, &q, folder_.str().c_str());
    }

    std::vector<std::string> files() const
    {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e :
             dump_file_test::fs::directory_iterator(folder_.path, ec)) {
            names.push_back(e.path().filename().generic_u8string());
        }
        return names;
    }
};

} // namespace

TEST_F(J2534OperationTest, ARunCopiesTheModulesMemoryThroughTheSurrogate)
{
    const j2534_backends::EnvSwap frame_logging("ECUIO_SUR_LOG_FRAMES", "1");
    const auto since =
        std::filesystem::file_time_type::clock::now() - std::chrono::seconds(2);
    conversation_session::Session s;
    ASSERT_NO_FATAL_FAILURE(open(&s));
    ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(begin(s.get()), ECUIO_ERR_IN_USE);
    const ecuio_operation_state_t v = session_operation::ended(s.get());
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.bytes_done, 2u * (kLowBytes + kHighBytes));
    ASSERT_EQ(files(), std::vector<std::string>{v.name});
    std::vector<uint8_t> want =
        session_operation::memory_of(fake_ecu::kVin, kLowAt, kLowBytes);
    const std::vector<uint8_t> high =
        session_operation::memory_of(fake_ecu::kVin, kHighAt, kHighBytes);
    want.insert(want.end(), high.begin(), high.end());
    EXPECT_EQ(session_operation::image_at(folder_.at(v.name)), want);
    EXPECT_EQ(ecuio_session_close(s.get()), ECUIO_OK);
    EXPECT_EQ(files(), std::vector<std::string>{v.name})
        << "the close keeps a sealed copy";
    s.reset();
    /* The session log holds neither the memory's bytes nor their SHA-256 */
    const std::string cut_read = "07 E8 63 00 00 00 00";
    const std::string first_chunk = "(256 bytes cut)";
    const std::string first_sum = ecuio_sha256::hex_of(want.data(), 256);
    std::string eight;
    for (size_t i = 0; i < 8; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, i > 0 ? " %02X" : "%02X", want[i]);
        eight += b;
    }
    bool ours = false;
    for (const std::string& log : logs_since(since)) {
        EXPECT_EQ(log.find(eight), std::string::npos) << eight;
        EXPECT_EQ(log.find(first_sum), std::string::npos) << first_sum;
        ours = ours || (log.find(cut_read) != std::string::npos &&
                        log.find(first_chunk) != std::string::npos &&
                        log.find("07 E8 76 01 00 00 00") != std::string::npos);
    }
    EXPECT_TRUE(ours) << "no session log holds the cut answers";
}

/* The fake's memory answers every third data reply with one bit wrong:
 * the two passes differ, and nothing is kept */
TEST_F(J2534OperationTest, AReadThatDiffersKeepsNothing)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_MEM", "flip:3"));
    conversation_session::Session s;
    ASSERT_NO_FATAL_FAILURE(open(&s));
    ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = session_operation::ended(s.get());
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_DIFFER) << v.words;
    EXPECT_STREQ(v.words, "the two readings differ: nothing was kept");
    EXPECT_STREQ(v.name, "");
    EXPECT_EQ(files(), std::vector<std::string>{});
}

TEST_F(J2534OperationTest, ACancelMidRunLeavesTheFolderEmpty)
{
    conversation_session::Session s;
    ASSERT_NO_FATAL_FAILURE(open(&s));
    ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_operation_cancel(s.get()), ECUIO_OK);
    const ecuio_operation_state_t v = session_operation::ended(s.get());
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED) << v.words;
    EXPECT_EQ(files(), std::vector<std::string>{});
    /* and the session takes the next run */
    ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t again = session_operation::ended(s.get());
    EXPECT_EQ(again.end, ECUIO_OPERATION_END_SAVED) << again.words;
    EXPECT_EQ(again.generation, v.generation + 1);
    EXPECT_EQ(files(), std::vector<std::string>{again.name});
}
