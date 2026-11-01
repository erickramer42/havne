// File/layer: tests/test_vcan_requests.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: socketcan:// requesting (ADR-0040) on the simulated
//          bus (vcan_bus.h): the fakes' ECU answers through the kernel's
//          can-isotp, an implementation independent of core's, so the
//          probe's walk, identification's replies across frames (core's
//          Flow Control), the diagnostics report, the requests' and Flow
//          Control's bytes on the wire, and the refusals of a link that
//          can't send. SocketCanFaultTest: the test car's recording
//          answering as the ECU (vcan_recording_ecu.h, ADR-0041 section
//          4), whole and with replies broken at the ISO-TP level. Each
//          case skips without the interface; ECUIO_VCAN_REQUIRED=1 makes
//          that a failure.

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "fake_diagnostics.h"
#include "fake_ecu.h"
#include "vcan_bus.h"
#include "vcan_recording_ecu.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/* A started session on the bus, closed and destroyed when it goes; a
 * frame callback, when given, sees it from its first frame */
class Session
{
public:
    explicit Session(const std::string& query,
                     ecuio_frame_callback_t callback = nullptr,
                     void* user_data = nullptr)
    {
        const std::string uri =
            std::string("socketcan://") + vcan_bus::interface_name() + query;
        status_ = ecuio_session_create(uri.c_str(), &s_);
        detail_ = ecuio_last_error_detail();
        if (status_ == ECUIO_OK && callback != nullptr) {
            status_ = ecuio_register_frame_callback(s_, callback, user_data);
        }
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

/* The ECU's replies a session delivered, in order */
struct Delivered {
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> replies;
    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return replies.size();
    }
};

void on_reply(const ecuio_frame_t* f, void* user_data)
{
    if (f->arbitration_id != vcan_bus::kReply ||
        (f->flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
        return;
    }
    auto* d = static_cast<Delivered*>(user_data);
    std::lock_guard<std::mutex> lock(d->mutex);
    d->replies.emplace_back(f->data, f->data + f->data_length);
}

/* What a session against the recording's ECU came to */
struct FaultRun {
    std::vector<std::vector<uint8_t>> whole;     /* the ECU's, in order */
    std::vector<std::vector<uint8_t>> delivered; /* the session's */
    std::set<uint8_t> refused;
    std::set<uint8_t> answered;
    unsigned silent = 0, late = 0, bad_sequence = 0, stalled = 0;
    unsigned segmented = 0; /* segmented replies sent whole */
    unsigned bad_flow_controls = 0, unflowed = 0;
    ecuio_channel_support_t support{};
    int32_t error = -1;
};

/* Requests through the recording's ECU, the test car's engine start
 * playing at its own pace */
class SocketCanFaultTest : public SocketCanRequestTest
{
protected:
    /* Segmented replies sent whole before a run ends: each one is a
     * pack answered */
    static constexpr unsigned kSegmented = 100;

    void SetUp() override
    {
        SocketCanRequestTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        const std::string path = std::string(ECUIO_REPLAY_FIXTURES) +
                                 "/2026-10-25-engine-start/frames.csv";
        recording_ = vcan_bus::read_frames_csv(path.c_str());
        ASSERT_GT(recording_.size(), 1000u) << path;
        for (const vcan_bus::Message& m : recording_) {
            if (m.data.size() >= 2 && m.data[0] == 0x41 &&
                m.data[1] % 0x20 != 0) {
                recorded_.insert(m.data[1]);
            }
        }
    }

    /* A session until the probe's walk ends and kSegmented segmented
     * replies went out whole; then the ECU stops and the session takes
     * what's on the bus */
    void run(vcan_bus::RecordingEcu::Faults faults, FaultRun* out)
    {
        using Fate = vcan_bus::RecordingEcu::Fate;
        vcan_bus::RecordingEcu ecu(recording_, faults);
        ASSERT_TRUE(ecu.ok());
        Delivered d;
        Session s("?profile=obd2&identify=0&diagnose=0", on_reply, &d);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        const auto whole_segmented = [&] {
            return ecu.count(Fate::intact, true) + ecu.count(Fate::late, true);
        };
        EXPECT_TRUE(wait_for(
            [&] {
                ecuio_channel_support_t sup{};
                return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                           ECUIO_OK &&
                       (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
            },
            5000ms));
        EXPECT_TRUE(
            wait_for([&] { return whole_segmented() >= kSegmented; }, 20000ms))
            << whole_segmented() << " segmented replies whole";
        ecu.stop();
        out->whole = ecu.whole();
        EXPECT_TRUE(
            wait_for([&] { return d.size() >= out->whole.size(); }, 1000ms));
        std::this_thread::sleep_for(50ms); /* nothing more comes */
        {
            std::lock_guard<std::mutex> lock(d.mutex);
            out->delivered = d.replies;
        }
        out->refused = ecu.refused();
        out->answered = ecu.answered();
        out->silent = ecu.count(Fate::silent);
        out->late = ecu.count(Fate::late);
        out->bad_sequence = ecu.count(Fate::bad_sequence);
        out->stalled = ecu.count(Fate::stalled);
        out->segmented = whole_segmented();
        out->bad_flow_controls = ecu.bad_flow_controls();
        out->unflowed = ecu.unflowed();
        ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 0, &out->support),
                  ECUIO_OK);
        uint32_t episodes = 0;
        ASSERT_EQ(
            ecuio_session_get_error_state(s.get(), &out->error, &episodes),
            ECUIO_OK);
    }

    /* What holds under every fault: the session delivered each reply the
     * bus carried whole, in order, and nothing else; the walk read the
     * car's bitmaps; the rotation kept each PID the recording holds and
     * no other (the bitmaps' and the refusals' left), each was answered,
     * and packing held;
     * core's Flow Control was ISO 15765-4's; the link is healthy */
    void expect_in_step(const FaultRun& r)
    {
        EXPECT_EQ(r.delivered.size(), r.whole.size());
        EXPECT_TRUE(r.delivered == r.whole)
            << "a reply delivered that the bus didn't carry whole, or one "
               "it did left out";
        EXPECT_NE(r.support.flags & ECUIO_SUPPORT_COMPLETE, 0u);
        EXPECT_EQ(r.support.ranges, 0x3Fu) << "the car's bitmaps, 00 to A0";
        EXPECT_EQ(r.support.requests - r.support.dropped, recorded_.size())
            << "the rotation keeps the PIDs the recording holds, and only them";
        EXPECT_EQ(r.answered, recorded_);
        EXPECT_NE(r.support.flags & ECUIO_SUPPORT_PACKED, 0u);
        EXPECT_EQ(r.support.flags & ECUIO_SUPPORT_UNPACKED, 0u)
            << "three packs in a row failed";
        EXPECT_GE(r.segmented, kSegmented);
        EXPECT_EQ(r.bad_flow_controls, 0u);
        EXPECT_EQ(r.unflowed, 0u);
        EXPECT_EQ(r.error, ECUIO_OK);
    }

    std::vector<vcan_bus::Message> recording_;
    std::set<uint8_t> recorded_; /* its PIDs, the bitmaps aside */
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

/* The test car's own replies: the probe walks its bitmaps, its PIDs
 * pack into segmented replies through core's ISO-TP, and every reply
 * reaches the session as the car sent it */
TEST_F(SocketCanFaultTest, TheTestCarsRepliesAnswerARequestingSession)
{
    FaultRun r;
    ASSERT_NO_FATAL_FAILURE(run({}, &r));
    expect_in_step(r);
    EXPECT_EQ(r.silent + r.late + r.bad_sequence + r.stalled, 0u);
}

/* A Consecutive Frame out of sequence, and one later than N_Cr: the
 * message is never delivered, its pack fails and the next is asked;
 * the frames after it start nothing */
TEST_F(SocketCanFaultTest, ABrokenSegmentedReplyIsNeverDelivered)
{
    vcan_bus::RecordingEcu::Faults f;
    f.bad_sequence_every = 4;
    f.stalled_every = 7;
    FaultRun r;
    ASSERT_NO_FATAL_FAILURE(run(f, &r));
    expect_in_step(r);
    EXPECT_GE(r.bad_sequence, kSegmented / 4);
    EXPECT_GE(r.stalled, kSegmented / 8);
}

/* No reply, and a reply after the engine's wait: the request times out
 * and the next goes out; the late reply is delivered but answers
 * nothing, so a late refusal drops no PID */
TEST_F(SocketCanFaultTest, SilenceAndLateRepliesLeaveTheSessionInStep)
{
    vcan_bus::RecordingEcu::Faults f;
    f.silent_every = 5;
    f.late_every = 7;
    FaultRun r;
    ASSERT_NO_FATAL_FAILURE(run(f, &r));
    expect_in_step(r);
    EXPECT_GE(r.silent, kSegmented / 5);
    EXPECT_GE(r.late, kSegmented / 8);
}
