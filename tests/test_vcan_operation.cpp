// File/layer: tests/test_vcan_operation.cpp | test-only, C++17, Qt-free,
//             Linux only
// Testing: HARNESS: SocketCanOperationTest (test_vcan)
//
// A plugin's dump run in a requesting socketcan:// session behind the
// test door (ADR-0066 section 4), end to end on vcan0 against the strict
// simulated ECU: the read's requests go out as ISO-TP sends of more than
// a frame, the upload's blocks come back segmented, and the copy saved is
// the simulated memory's bytes.

#include "dump_file_support.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/sim.h"
#include "operation_plan_support.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

using session_operation::kHighAt;
using session_operation::kHighBytes;
using session_operation::kLowAt;
using session_operation::kLowBytes;

/* The simulated ECU's VIN with values=canned */
constexpr char kSimVin[] = "ZZZFAKE11M0000001";

class SocketCanOperationTest : public ::testing::Test
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

TEST_F(SocketCanOperationTest, ARunCopiesTheSimulatedEcusMemory)
{
    ecuio_sim_t* sim = nullptr;
    ASSERT_EQ(ecuio_sim_create_with("mock://sim?values=canned",
                                    vcan_bus::interface_name(), 0, &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_sim_test_set_strict(sim, 1);
    conversation_session::DoorOpen open;
    conversation_test::Plugin plugin =
        conversation_test::load_packed(session_operation::made().json());
    ASSERT_NE(plugin, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    const dump_file_test::Folder folder;
    {
        ecuio_session_t* raw = nullptr;
        const std::string uri = std::string("socketcan://") +
                                vcan_bus::interface_name() +
                                "?profile=" + name + "&identify=0&diagnose=0";
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &raw), ECUIO_OK)
            << ecuio_last_error_detail();
        conversation_session::Session s(raw);
        ASSERT_EQ(ecuio_session_start(s.get()), ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_NO_FATAL_FAILURE(session_operation::talking(s.get()));
        const ecuio_operation_request_t q =
            session_operation::request(ECUIO_OPERATION_MAY_UNLOCK);
        ASSERT_EQ(
            ecuio_session_operation_start(s.get(), &q, folder.str().c_str()),
            ECUIO_OK)
            << ecuio_last_error_detail();
        const ecuio_operation_state_t v = session_operation::ended(s.get());
        ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
        EXPECT_EQ(v.bytes_done, 2u * (kLowBytes + kHighBytes));
        std::vector<uint8_t> want =
            session_operation::memory_of(kSimVin, kLowAt, kLowBytes);
        const std::vector<uint8_t> high =
            session_operation::memory_of(kSimVin, kHighAt, kHighBytes);
        want.insert(want.end(), high.begin(), high.end());
        EXPECT_EQ(session_operation::image_at(folder.at(v.name)), want);
        EXPECT_EQ(ecuio_session_close(s.get()), ECUIO_OK);
    }
    EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK);
    ecuio_sim_destroy(sim);
}
