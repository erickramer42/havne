// File/layer: tests/test_vcan.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS, the simulated bus itself (vcan_bus.h): the fake ECU
//          answering through the kernel's ISO-TP, what a listener on the
//          bus sees of it, and a recording played onto it. Each case
//          skips without the interface; ECUIO_VCAN_REQUIRED=1 (CI's
//          linux-build, which brings vcan0 up) makes that a failure
//
// The SocketCAN transport's tests listen to this same bus, so these pin
// what it puts there: the frames, their PCI bytes and their padding.

#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

class VcanTest : public ::testing::Test {
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

    /* Sends req as a tester on tx and returns the reply on rx (empty on
     * a 2 s timeout) */
    static std::vector<uint8_t> ask(uint32_t tx, std::vector<uint8_t> req)
    {
        vcan_bus::Fd tester = vcan_bus::open_isotp(tx, vcan_bus::kReply);
        EXPECT_TRUE(tester.ok());
        EXPECT_EQ(::write(tester.get(), req.data(), req.size()),
                  static_cast<ssize_t>(req.size()));
        std::vector<uint8_t> reply(4096);
        const long n = vcan_bus::read_for(tester.get(), reply.data(), reply.size(), 2000);
        reply.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return reply;
    }
};

/* Everything a raw socket reads until timeout_ms passes with nothing */
std::vector<can_frame> drain(int fd, int timeout_ms)
{
    std::vector<can_frame> frames;
    can_frame f{};
    while (vcan_bus::read_for(fd, &f, sizeof(f), timeout_ms) == static_cast<long>(sizeof(f))) {
        frames.push_back(f);
    }
    return frames;
}

TEST_F(VcanTest, TheEcuAnswersAPhysicalRequest)
{
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(ecu.ok());
    EXPECT_EQ(ask(vcan_bus::kRequest, {0x01, 0x0C}),
              (std::vector<uint8_t>{0x41, 0x0C, fake_ecu::kPid0CRpm[0], fake_ecu::kPid0CRpm[1]}));
    EXPECT_EQ(ecu.answered(), 1u);
}

TEST_F(VcanTest, TheEcuAnswersAFunctionalRequest)
{
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(ecu.ok());
    EXPECT_EQ(ask(vcan_bus::kFunctional, {0x01, 0x0D}),
              (std::vector<uint8_t>{0x41, 0x0D, fake_ecu::kPid0DSpeed}));
}

TEST_F(VcanTest, ALongReplyComesWhole)
{
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(ecu.ok());
    const std::vector<uint8_t> reply = ask(vcan_bus::kRequest, {0x22, 0xF1, 0xA0});
    ASSERT_EQ(reply.size(), 3 + fake_ecu::kLongDidLen);
    EXPECT_EQ(reply[0], 0x62);
    EXPECT_EQ(reply[1], 0xF1);
    EXPECT_EQ(reply[2], 0xA0);
    for (size_t i = 0; i < fake_ecu::kLongDidLen; ++i) {
        ASSERT_EQ(reply[3 + i], static_cast<uint8_t>(i & 0xFF)) << "byte " << i;
    }
}

TEST_F(VcanTest, AnUnsupportedPidIsRefused)
{
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(ecu.ok());
    EXPECT_EQ(ask(vcan_bus::kRequest, {0x01, 0xC0}),
              (std::vector<uint8_t>{0x7F, 0x01, 0x12}));
}

TEST_F(VcanTest, ASilentEcuAnswersNothing)
{
    vcan_bus::Ecu ecu(0); /* unsupported_nrc 0: refusals are silence */
    ASSERT_TRUE(ecu.ok());
    EXPECT_TRUE(ask(vcan_bus::kRequest, {0x01, 0xC0}).empty());
    EXPECT_EQ(ecu.answered(), 0u);
}

/* A listener sees the conversation as frames: the request's Single
 * Frame, the VIN's First Frame, the tester's flow control and two
 * Consecutive Frames, each padded to 8 bytes */
TEST_F(VcanTest, AListenerSeesTheConversationFrameByFrame)
{
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(ecu.ok());
    const std::vector<uint8_t> vin = ask(vcan_bus::kRequest, {0x09, 0x02});
    ASSERT_EQ(vin.size(), 3 + sizeof(fake_ecu::kVin) - 1);

    const std::vector<can_frame> frames = drain(listener.get(), 200);
    struct Seen {
        canid_t id;
        uint8_t pci;
    };
    const Seen want[] = {{vcan_bus::kRequest, 0x02}, {vcan_bus::kReply, 0x10},
                         {vcan_bus::kRequest, 0x30}, {vcan_bus::kReply, 0x21},
                         {vcan_bus::kReply, 0x22}};
    ASSERT_EQ(frames.size(), sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].can_id, want[i].id) << "frame " << i;
        EXPECT_EQ(frames[i].can_dlc, 8) << "frame " << i;
        EXPECT_EQ(frames[i].data[0], want[i].pci) << "frame " << i;
    }
    /* The request's padding past its three bytes, the First Frame's
     * length (20 = 49 02 01 and the VIN), and the last Consecutive
     * Frame's seven bytes, which end the reply exactly (6 + 7 + 7) */
    for (int i = 3; i < 8; ++i)
        EXPECT_EQ(frames[0].data[i], vcan_bus::kPad) << "request byte " << i;
    EXPECT_EQ(frames[1].data[1], vin.size());
    EXPECT_EQ(std::vector<uint8_t>(frames[4].data + 1, frames[4].data + 8),
              std::vector<uint8_t>(vin.end() - 7, vin.end()));
}

/* The diagnostics through the kernel's ISO-TP: Service 03's three
 * stored DTCs (8 bytes with the service and the count) take a First
 * Frame and a Consecutive Frame, and come back whole; a clear empties
 * the list */
TEST_F(VcanTest, TheDtcListTakesTwoFramesAndAClearEmptiesIt)
{
    fake_ecu::reset_diagnostics();
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(ecu.ok());
    EXPECT_EQ(
        ask(vcan_bus::kRequest, {0x03}),
        (std::vector<uint8_t>{0x43, 0x03, 0x03, 0x01, 0x04, 0x20, 0xC1, 0x00}));
    std::vector<can_frame> replies;
    for (const can_frame& f : drain(listener.get(), 200)) {
        if (f.can_id == vcan_bus::kReply) {
            replies.push_back(f);
        }
    }
    ASSERT_EQ(replies.size(), 2u);
    EXPECT_EQ(replies[0].data[0], 0x10) << "a First Frame";
    EXPECT_EQ(replies[0].data[1], 8) << "of 8 bytes";
    EXPECT_EQ(replies[1].data[0], 0x21) << "its Consecutive Frame";

    EXPECT_EQ(ask(vcan_bus::kRequest, {0x04}), (std::vector<uint8_t>{0x44}));
    EXPECT_EQ(ask(vcan_bus::kRequest, {0x03}),
              (std::vector<uint8_t>{0x43, 0x00}));
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
    fake_ecu::reset_diagnostics();
}

/* The recorded engine start's replies go out in order, each a padded
 * Single Frame, and a listener reads them back unchanged */
TEST_F(VcanTest, ARecordingPlaysOntoTheBus)
{
    const std::string path =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start/frames.csv";
    const std::vector<vcan_bus::Message> messages =
        vcan_bus::read_frames_csv(path.c_str(), 1000);
    ASSERT_EQ(messages.size(), 1000u) << path;

    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    std::vector<can_frame> heard;
    std::atomic<bool> done{false};
    /* Read as they come: vcan has no flow control, so a listener that
     * falls behind loses frames from its socket's queue */
    std::thread reader([&] {
        can_frame f{};
        for (;;) {
            if (vcan_bus::read_for(listener.get(), &f, sizeof(f), 50)
                == static_cast<long>(sizeof(f))) {
                heard.push_back(f);
            } else if (done.load()) {
                return;
            }
        }
    });
    EXPECT_EQ(vcan_bus::play(messages, 0), messages.size());
    done = true;
    reader.join();

    ASSERT_EQ(heard.size(), messages.size());
    for (size_t i = 0; i < heard.size(); ++i) {
        const vcan_bus::Message& m = messages[i];
        ASSERT_EQ(heard[i].can_id, m.id) << "message " << i;
        ASSERT_EQ(heard[i].data[0], m.data.size()) << "message " << i;
        ASSERT_EQ(std::vector<uint8_t>(heard[i].data + 1, heard[i].data + 1 + m.data.size()),
                  m.data)
            << "message " << i;
    }
}

/* At speed 1 the player keeps the recording's pace: its first 50
 * messages span the recording's own time between them */
TEST_F(VcanTest, ThePlayerKeepsTheRecordingsPace)
{
    const std::string path =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start/frames.csv";
    const std::vector<vcan_bus::Message> messages = vcan_bus::read_frames_csv(path.c_str(), 50);
    ASSERT_EQ(messages.size(), 50u);
    const auto span = std::chrono::microseconds(messages.back().host_us - messages.front().host_us);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(vcan_bus::play(messages, 1), messages.size());
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_GE(took, span);
    EXPECT_LT(took, span + std::chrono::milliseconds(500));
}

} // namespace
