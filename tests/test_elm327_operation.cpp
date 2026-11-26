// File/layer: tests/test_elm327_operation.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327OperationTest (test_elm327)
//
// A plugin's dump run in an ELM327 session behind the test door (ADR-0066
// section 4), end to end through the public calls against the fake
// ELM327, whose engine ECU's memory is the simulated ECU's: a plan whose
// requests fit one frame is copied whole, and one asked in more bytes
// than the adapter types is refused before anything is sent.

#include <gtest/gtest.h>

#include "dump_file_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "fake_elm327.h"
#include "operation_plan_support.h"
#include "test_elm327_support.h"

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
    }

    void TearDown() override
    {
        if (name_[0] != '\0') {
            EXPECT_EQ(ecuio_profile_unregister(name_), ECUIO_OK);
        }
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_strict() = false;
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
    conversation_session::Session s;
    ASSERT_NO_FATAL_FAILURE(open(session_operation::one_frame_plan(), &s));
    ASSERT_EQ(begin(s.get()), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = session_operation::ended(s.get());
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.bytes_done, 2u * kLowBytes);
    ASSERT_EQ(files(), std::vector<std::string>{v.name});
    EXPECT_EQ(session_operation::image_at(folder_.at(v.name)),
              session_operation::memory_of(fake_ecu::kVin, kLowAt, kLowBytes));
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
