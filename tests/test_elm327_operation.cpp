// File/layer: tests/test_elm327_operation.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327OperationTest (test_elm327)
//
// A plugin's dump run in an ELM327 session behind the test door (ADR-0066
// section 4), end to end through the public calls against the fake
// ELM327, whose engine ECU's memory is the simulated ECU's: a plan whose
// requests fit one frame is copied whole with none of its bytes in the
// wire log, and one asked in more bytes than the adapter types is refused
// before anything is sent.

#include <gtest/gtest.h>

#include "dump_file_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "fake_elm327.h"
#include "operation_plan_support.h"
#include "test_elm327_support.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using session_operation::kLowAt;
using session_operation::kLowBytes;

class Elm327OperationTest : public ::testing::Test
{
protected:
    fake_elm327::installed fake_;
    conversation_session::DoorOpen door_;
    conversation_test::Plugin plugin_;
    char name_[64] = {};
    dump_file_test::Folder folder_;

    void SetUp() override
    {
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_strict() = true;
        fake_ecu::diagnostics().engine_off = true;
    }

    void TearDown() override
    {
        if (name_[0] != '\0') {
            EXPECT_EQ(ecuio_profile_unregister(name_), ECUIO_OK);
        }
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_strict() = false;
        fake_ecu::reset_diagnostics();
    }

    /* A started session over a plugin with plan */
    void open(const session_operation::PlanShape& plan,
              conversation_session::Session* s)
    {
        plugin_ = conversation_test::load_packed(
            session_operation::made(session_operation::GuestShape(), plan)
                .json());
        ASSERT_NE(plugin_, nullptr) << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_plugin_register(plugin_.get(), 0, name_, sizeof name_),
                  ECUIO_OK);
        const std::string uri = std::string("elm327://") + kPort +
                                "?profile=" + name_ + "&identify=0&diagnose=0";
        ecuio_session_t* raw = nullptr;
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &raw), ECUIO_OK)
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

TEST_F(Elm327OperationTest, APlanThatFitsAFrameIsCopiedWhole)
{
    const EnvSet frame_logging("ECUIO_SUR_LOG_FRAMES", "1");
    const LogDir logs("elm327-operation");
    const std::vector<uint8_t> memory =
        session_operation::memory_of(fake_ecu::kVin, kLowAt, kLowBytes);
    {
        conversation_session::Session s;
        ASSERT_NO_FATAL_FAILURE(open(session_operation::one_frame_plan(), &s));
        ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = session_operation::ended(s.get());
        ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_EQ(v.bytes_done, 2u * kLowBytes);
        ASSERT_EQ(files(), std::vector<std::string>{v.name});
        EXPECT_EQ(session_operation::image_at(folder_.at(v.name)), memory);
    }
    /* No hash either: a frame's few bytes would be found from one */
    const std::string log = logs.only_log();
    EXPECT_NE(log.find("> 231301000080"), std::string::npos) << log;
    const std::string cut = "< (a memory read's line, cut: ";
    size_t cut_lines = 0;
    for (size_t at = log.find(cut); at != std::string::npos;
         at = log.find(cut, at + 1)) {
        const size_t from = at + cut.size();
        const size_t words = log.find_first_not_of("0123456789", from);
        EXPECT_GT(words, from) << "the line's length";
        EXPECT_EQ(log.compare(words, 12, " characters)"), 0)
            << log.substr(at, 80);
        ++cut_lines;
    }
    EXPECT_GT(cut_lines, 0u) << log;
    EXPECT_NE(log.find("< (a security access's line, cut: "),
              std::string::npos);
    std::string spaced;
    std::string bare;
    for (size_t i = 5; i < 12; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, "%02X", memory[i]);
        spaced += std::string(i > 5 ? " " : "") + b;
        bare += b;
    }
    EXPECT_EQ(log.find(spaced), std::string::npos) << spaced;
    EXPECT_EQ(log.find(bare), std::string::npos) << bare;
}

TEST_F(Elm327OperationTest, APlanAskedInMoreThanAFrameIsRefusedAtTheStart)
{
    conversation_session::Session s;
    ASSERT_NO_FATAL_FAILURE(open(session_operation::PlanShape(), &s));
    EXPECT_EQ(begin(s.get()), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "step \"low\" is asked in 8 bytes: it doesn't fit one "
                 "request on this adapter (7 bytes)");
    EXPECT_EQ(files(), std::vector<std::string>{});
    ecuio_operation_state_t v{};
    v.struct_size = sizeof v;
    ASSERT_EQ(ecuio_session_get_operation(s.get(), &v), ECUIO_OK);
    EXPECT_EQ(v.generation, 0u) << "no run began";
}

/* AT RV between the run's commands: a reading under 11.0 V warns; an
 * adapter that reads none leaves it to the engine module's 01 42 */
TEST_F(Elm327OperationTest, TheBatteryIsReadThroughTheRun)
{
    {
        fake_elm327::with([](fake_elm327::adapter& a) {
            a.k.vbatt_sag = "10.9V";
            a.k.vbatt_sag_at = a.battery_reads + 3;
        });
        conversation_session::Session s;
        ASSERT_NO_FATAL_FAILURE(open(session_operation::one_frame_plan(), &s));
        ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = session_operation::ended(s.get());
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_VOLTS_SAG, 0u);
        EXPECT_EQ(v.volts_min_mv, 10900u);
    }
    fake_elm327::with([](fake_elm327::adapter& a) {
        a.k.vbatt_sag = nullptr;
        a.k.vbatt = "?";
    });
    conversation_session::Session s;
    ASSERT_NO_FATAL_FAILURE(open(session_operation::one_frame_plan(), &s));
    ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = session_operation::ended(s.get());
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_VOLTS_FROM_ECU, 0u);
    EXPECT_GT(v.volts_mv, 0u);
}
