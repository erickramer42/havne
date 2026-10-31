// File/layer: tests/test_vcan_requests.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: socketcan:// requesting (ADR-0040) on the simulated
//          bus (vcan_bus.h): the fakes' ECU answers through the kernel's
//          can-isotp, an implementation independent of core's, so the
//          probe's walk, identification's replies across frames (core's
//          Flow Control), the diagnostics report, the requests' and Flow
//          Control's bytes on the wire, and the refusals of a link that
//          can't send. Each case skips without the interface;
//          ECUIO_VCAN_REQUIRED=1 makes that a failure.

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "fake_diagnostics.h"
#include "fake_ecu.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/* A started session on the bus, closed and destroyed when it goes */
class Session
{
public:
    explicit Session(const std::string& query)
    {
        const std::string uri =
            std::string("socketcan://") + vcan_bus::interface_name() + query;
        status_ = ecuio_session_create(uri.c_str(), &s_);
        detail_ = ecuio_last_error_detail();
        if (status_ == ECUIO_OK) {
            status_ = ecuio_session_start(s_);
        }
    }
    ~Session()
    {
        if (s_ != nullptr) {
            ecuio_session_close(s_);
            ecuio_session_destroy(s_);
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_session_t* get() const { return s_; }

private:
    ecuio_session_t* s_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

class SocketCanRequestTest : public ::testing::Test
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
        fake_ecu::reset_diagnostics();
    }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

} // namespace

/* The probe walks the fake's bitmaps (00 to A0) and the rotation runs:
 * whole replies, the service byte first, from the ECU's reply ID */
TEST_F(SocketCanRequestTest, TheProbeWalksTheFakeEcusBitmaps)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms));
    EXPECT_EQ(sup.rx_id, 0x7E8u);
    EXPECT_EQ(sup.ranges, 0x3Fu) << "the bitmaps at 0x00 to 0xA0";
    EXPECT_EQ(sup.dropped, 0u) << "the fake supports every obd2 PID";

    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.backend, static_cast<uint32_t>(ECUIO_BACKEND_SOCKETCAN));
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
    EXPECT_EQ(n, 0u);
}

/* Identification's replies span frames (the VIN's 20 bytes: a First
 * Frame and two Consecutive Frames): the kernel's ISO-TP sends the rest
 * only after core's Flow Control */
TEST_F(SocketCanRequestTest, IdentificationReadsTheVinAcrossFrames)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        8000ms));
    ASSERT_GT(id.items, 0u);
    EXPECT_EQ(id.list[0].request[0], 0x09);
    EXPECT_EQ(id.list[0].request[1], 0x02);
    EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(id.list[0].text, fake_ecu::kVin);
}

/* The whole diagnostics report (ADR-0037), the stored list's 8 bytes a
 * First Frame and a Consecutive Frame */
TEST_F(SocketCanRequestTest, DiagnosticsFillTheFakeEcusReport)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&identify=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s.get(), 0, &d) ==
                       ECUIO_OK &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        8000ms))
        << "generation " << d.generation;
    EXPECT_EQ(d.rx_id, 0x7E8u);
    fake_diagnostics::expect_report(d);
}

/* On the wire: every request is a Single Frame of 8 bytes to 0x7E0, and
 * each First Frame from 0x7E8 gets exactly one Flow Control frame, Clear
 * To Send with block size 0 and STmin 0, before its Consecutive Frames */
TEST_F(SocketCanRequestTest,
       RequestsAreSingleFramesAndEachFirstFrameGetsOneFlowControl)
{
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();

    unsigned requests = 0, first_frames = 0, flow_controls = 0;
    bool fc_awaited = false;
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < end) {
        struct can_frame f;
        if (vcan_bus::read_for(listener.get(), &f, sizeof f, 100) !=
            static_cast<long>(sizeof f)) {
            continue;
        }
        const uint32_t id = f.can_id & CAN_SFF_MASK;
        const uint8_t pci = f.data[0] >> 4;
        if (id == 0x7E0) {
            EXPECT_EQ(f.len, 8u) << "ISO 15765-4: DLC 8";
            if (pci == 0) {
                ++requests;
            } else if (pci == 3) {
                ++flow_controls;
                EXPECT_TRUE(fc_awaited) << "a Flow Control frame unasked";
                EXPECT_EQ(f.data[0], 0x30);
                EXPECT_EQ(f.data[1], 0x00);
                EXPECT_EQ(f.data[2], 0x00);
                fc_awaited = false;
            } else {
                ADD_FAILURE() << "PCI type " << int(pci) << " to the ECU";
            }
        } else if (id == 0x7E8 && pci == 1) {
            ++first_frames;
            EXPECT_FALSE(fc_awaited)
                << "a First Frame before the last's Flow Control";
            fc_awaited = true;
        } else if (id == 0x7E8 && pci == 2) {
            EXPECT_FALSE(fc_awaited)
                << "a Consecutive Frame before Flow Control";
        }
    }
    EXPECT_GT(requests, 10u);
    EXPECT_GE(first_frames, 1u) << "the VIN at least";
    EXPECT_EQ(flow_controls, first_frames);
}

/* vcan has no rate, so a stated one isn't checked against it */
TEST_F(SocketCanRequestTest, ARequestingSessionOnVcanNeedsNoRate)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&baud=250000&identify=0&diagnose=0");
    EXPECT_EQ(s.status(), ECUIO_OK) << "vcan has no rate: " << s.detail();
}
