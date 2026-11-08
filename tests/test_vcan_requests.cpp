// File/layer: tests/test_vcan_requests.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanRequestTest, SocketCanFaultTest, SimBusTest
//          (test_vcan)

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/sim.h"
#include "fake_diagnostics.h"
#include "fake_ecu.h"
#include "vcan_bus.h"
#include "vcan_recording_ecu.h"

#include <gtest/gtest.h>

#include <linux/can/error.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
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
    size_t unflagged = 0; /* replies without ECUIO_FRAME_FLAG_REASSEMBLED */
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
    d->unflagged += (f->flags & ECUIO_FRAME_FLAG_REASSEMBLED) == 0;
}

/* What a session against the recording's ECU came to */
struct FaultRun {
    std::vector<std::vector<uint8_t>> whole;     /* the ECU's, in order */
    std::vector<std::vector<uint8_t>> delivered; /* the session's */
    size_t unflagged = 0; /* of them, not flagged reassembled */
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
            out->unflagged = d.unflagged;
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
     * bus carried whole, in order, flagged reassembled, and nothing else;
     * the walk read the
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
        EXPECT_EQ(r.unflagged, 0u) << "each is ISO-TP's reassembly";
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

/* Two ECUs on the bus in one session: the engine ECU's 1E1C from 0x7E8,
 * and a module's D801 (fake_ecu's table) from a second ECU on
 * 0x7E1/0x7E9, each through its own ISO-TP socket; each reply comes
 * from its own ECU */
TEST_F(SocketCanRequestTest, TwoEcusInOneSessionAnswerEachItsOwn)
{
    struct Module {
        Module() { EXPECT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12")); }
        ~Module() { fake_ecu::set_module_dids(nullptr); }
    } module;
    struct Replies {
        std::mutex mutex;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> got;
        static void cb(const ecuio_frame_t* f, void* user)
        {
            auto* r = static_cast<Replies*>(user);
            std::lock_guard<std::mutex> lock(r->mutex);
            r->got.emplace_back(
                f->arbitration_id,
                std::vector<uint8_t>(f->data, f->data + f->data_length));
        }
    } replies;
    vcan_bus::Ecu engine;
    vcan_bus::Ecu second(-1, vcan_bus::interface_name(), 0x7E1, 0x7E9);
    ASSERT_TRUE(engine.ok() && second.ok());
    Session s("?profile=uds-generic&did=1E1C,7E9:D801&identify=0&diagnose=0",
              Replies::cb, &replies);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const std::vector<uint8_t> want_engine = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    const std::vector<uint8_t> want_module = {0x62, 0xD8, 0x01, 0x00, 0x12};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(replies.mutex);
            int a = 0;
            int b = 0;
            for (const auto& r : replies.got) {
                a += r.first == 0x7E8u && r.second == want_engine;
                b += r.first == 0x7E9u && r.second == want_module;
            }
            return a >= 3 && b >= 3;
        },
        5000ms))
        << "three of each ECU's own replies";
    std::lock_guard<std::mutex> lock(replies.mutex);
    for (const auto& r : replies.got) {
        EXPECT_TRUE((r.first == 0x7E8u && r.second == want_engine) ||
                    (r.first == 0x7E9u && r.second == want_module))
            << "a reply on 0x" << std::hex << r.first;
    }
    EXPECT_GT(second.answered(), 0u);
}

/* A frame of up to 8 bytes onto the bus from a raw socket */
bool put(int fd, canid_t id, const std::vector<uint8_t>& data)
{
    can_frame f{};
    f.can_id = id;
    f.len = static_cast<uint8_t>(data.size());
    std::memcpy(f.data, data.data(), data.size());
    return ::write(fd, &f, sizeof f) == static_cast<ssize_t>(sizeof f);
}

/* Holds the worker 200 ms on the first error frame it dispatches */
void hold_on_error(const ecuio_frame_t* f, void* user_data)
{
    auto* held = static_cast<std::atomic<bool>*>(user_data);
    if ((f->flags & ECUIO_FRAME_FLAG_ERROR) != 0 && !held->exchange(true)) {
        std::this_thread::sleep_for(200ms);
    }
}

/* N_Cr is the gap between frames on the bus, not between their reads:
 * the VIN's First Frame, then an error frame the session's callback
 * holds the worker on, while the ECU sends its Consecutive Frames 10 ms
 * after the Flow Control. The worker reads them 200 ms late, past N_Cr
 * (150 ms), and the VIN is still whole. The ECU answers 09 02 once, so
 * a request asked again can't stand in for the first; other requests
 * get the fakes' Single Frames, or nothing. The ECU's thread can itself
 * be late (ASan on a loaded runner), and so can the kernel's stamps: a
 * run whose last Consecutive Frame went over 100 ms after the First
 * Frame, by the ECU's clock or by the stamps a listener reads (which a
 * starved CPU can put 200 ms late, ADR-0032's appendix "frames out of
 * order"), was late on the bus too, so it tests nothing and runs again,
 * three runs at most. So does a run whose
 * frames the bus carried out of order: vcan queues each frame on the
 * sending CPU's backlog, so a thread moving between CPUs under load can
 * have its frames delivered out of order, which a real bus never does.
 * The ECU's thread stays on one CPU, and a listener checks the order */
struct HeldWorkerRun {
    bool on_time = false;
    bool in_order = false;
    bool held = false;
    bool segmented = false;
    uint32_t outcome = 0;
    std::string text;
};

HeldWorkerRun run_held_worker()
{
    HeldWorkerRun r;
    vcan_bus::Fd bus = vcan_bus::open_raw();
    EXPECT_TRUE(bus.ok());
    if (!bus.ok()) {
        return r;
    }
    std::atomic<bool> stop{false};
    std::atomic<bool> segmented{false};
    std::atomic<bool> on_time{false};
    vcan_bus::Fd listener = vcan_bus::open_raw();
    EXPECT_TRUE(listener.ok());
    const int stamps = 1;
    EXPECT_EQ(setsockopt(listener.get(), SOL_SOCKET, SO_TIMESTAMPNS, &stamps,
                         sizeof stamps),
              0);
    std::thread ecu([&] {
        const int cpu = sched_getcpu();
        if (cpu >= 0) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            (void)pthread_setaffinity_np(pthread_self(), sizeof one, &one);
        }
        const char* vin = fake_ecu::kVin; /* 17 characters: 20 bytes */
        bool vin_sent = false;
        can_frame f;
        while (!stop.load()) {
            if (vcan_bus::read_for(bus.get(), &f, sizeof f, 20) !=
                    static_cast<long>(sizeof f) ||
                f.can_id != vcan_bus::kRequest || f.len < 2 || f.data[0] > 7 ||
                f.data[0] == 0) {
                continue;
            }
            if (f.data[1] == 0x09 && f.data[2] == 0x02) {
                if (vin_sent) {
                    continue;
                }
                vin_sent = true;
                (void)put(bus.get(), vcan_bus::kReply,
                          {0x10, 0x14, 0x49, 0x02, 0x01, uint8_t(vin[0]),
                           uint8_t(vin[1]), uint8_t(vin[2])});
                const auto first = std::chrono::steady_clock::now();
                (void)put(bus.get(), CAN_ERR_FLAG | CAN_ERR_CRTL,
                          {0, CAN_ERR_CRTL_RX_WARNING, 0, 0, 0, 0, 0, 0});
                bool flowed = false;
                const auto end = std::chrono::steady_clock::now() + 1s;
                while (!flowed && std::chrono::steady_clock::now() < end) {
                    flowed = vcan_bus::read_for(bus.get(), &f, sizeof f, 20) ==
                                 static_cast<long>(sizeof f) &&
                             f.can_id == vcan_bus::kRequest &&
                             (f.data[0] & 0xF0) == 0x30;
                }
                if (!flowed) {
                    continue;
                }
                std::this_thread::sleep_for(10ms);
                std::vector<uint8_t> cf1 = {0x21}, cf2 = {0x22};
                for (int k = 3; k < 10; ++k) {
                    cf1.push_back(uint8_t(vin[k]));
                }
                for (int k = 10; k < 17; ++k) {
                    cf2.push_back(uint8_t(vin[k]));
                }
                segmented = put(bus.get(), vcan_bus::kReply, cf1) &&
                            put(bus.get(), vcan_bus::kReply, cf2);
                on_time = std::chrono::steady_clock::now() - first <= 100ms;
                continue;
            }
            unsigned char reply[ECUIO_PT_MSG_DATA_SIZE];
            size_t n = 0;
            if (fake_ecu::answer(f.data + 1, f.data[0], -1, reply, &n) &&
                n <= 7) {
                std::vector<uint8_t> sf = {uint8_t(n)};
                sf.insert(sf.end(), reply, reply + n);
                sf.resize(8, vcan_bus::kPad);
                (void)put(bus.get(), vcan_bus::kReply, sf);
            }
        }
    });
    std::atomic<bool> held{false};
    {
        Session s("?profile=obd2&probe=0&diagnose=0", hold_on_error, &held);
        EXPECT_EQ(s.status(), ECUIO_OK) << s.detail();
        ecuio_channel_identity_t id{};
        EXPECT_TRUE(wait_for(
            [&] {
                return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                           ECUIO_OK &&
                       id.items > 0 &&
                       id.list[0].outcome != ECUIO_IDENTITY_UNASKED;
            },
            8000ms));
        r.outcome = id.list[0].outcome;
        r.text = id.list[0].text;
    }
    stop = true;
    ecu.join();
    /* The VIN's frames as the bus carried them, 10, 21, 22, and the
     * kernel's stamps of the first and last */
    std::vector<uint8_t> pcis;
    int64_t first_ns = -1, last_ns = -1;
    for (;;) {
        can_frame f;
        iovec iov{&f, sizeof f};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(timespec))];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        if (!listener.ok() ||
            recvmsg(listener.get(), &msg, MSG_DONTWAIT) !=
                static_cast<ssize_t>(sizeof f)) {
            break;
        }
        int64_t ns = -1;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec ts;
                std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
                ns = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
            }
        }
        if (f.can_id == vcan_bus::kReply && f.len == 8 &&
            (f.data[0] == 0x10 || f.data[0] == 0x21 || f.data[0] == 0x22)) {
            pcis.push_back(f.data[0]);
            if (f.data[0] == 0x10) {
                first_ns = ns;
            } else if (f.data[0] == 0x22) {
                last_ns = ns;
            }
        }
    }
    r.in_order = pcis == std::vector<uint8_t>{0x10, 0x21, 0x22};
    r.on_time = on_time.load() && first_ns >= 0 && last_ns >= first_ns &&
                last_ns - first_ns <= 100000000;
    r.held = held.load();
    r.segmented = segmented.load();
    return r;
}

TEST_F(SocketCanRequestTest, AReplyOnTimeSurvivesAWorkerHeldUpMidMessage)
{
    HeldWorkerRun r;
    for (int run = 0; run < 3 && !(r.on_time && r.in_order); ++run) {
        r = run_held_worker();
    }
    EXPECT_TRUE(r.held) << "the worker was never held";
    EXPECT_TRUE(r.segmented) << "the Consecutive Frames never went";
    ASSERT_TRUE(r.on_time) << "the ECU's thread was late in three runs";
    ASSERT_TRUE(r.in_order) << "the bus reordered the VIN in three runs";
    EXPECT_EQ(r.outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_EQ(r.text, fake_ecu::kVin);
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

/* ADR-0056 section 2 on a kernel socket: a focus set while the session
 * runs has it ask the picked PIDs alone. The listener opens after the
 * call, so it hears only requests sent under the focus, but for one: the
 * worker takes the focus at the end of a turn and sends at the start of
 * the next, so a call landing between the two lets one request chosen
 * without it go out after the call. The first request heard is
 * therefore left unchecked. */
TEST_F(SocketCanRequestTest, AFocusAsksOnlyThePickedPids)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.get());
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            (d.mux_key == 0x410Cu || d.mux_key == 0x410Du)) {
            ids.push_back(d.id);
        }
    }
    ASSERT_EQ(ids.size(), 2u);
    ASSERT_EQ(ecuio_session_set_focus(s.get(), ids.data(), 2), ECUIO_OK);
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());

    std::vector<uint8_t> pids; /* each request's PID, in order */
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (pids.size() < 12 && std::chrono::steady_clock::now() < end) {
        can_frame f{};
        if (vcan_bus::read_for(listener.get(), &f, sizeof f, 100) ==
                static_cast<long>(sizeof f) &&
            (f.can_id & CAN_ERR_FLAG) == 0 &&
            (f.can_id & CAN_SFF_MASK) == vcan_bus::kRequest && f.len >= 3 &&
            f.data[1] == 0x01) {
            pids.push_back(f.data[2]);
        }
    }
    ASSERT_EQ(pids.size(), 12u);
    for (size_t k = 1; k < pids.size(); ++k) {
        EXPECT_TRUE(pids[k] == 0x0C || pids[k] == 0x0D)
            << "request " << k << " asks PID " << int(pids[k]);
    }
    EXPECT_EQ(ecuio_session_set_focus(s.get(), nullptr, 0), ECUIO_OK);
}

/* vcan has no rate, so a stated one isn't checked against it */
TEST_F(SocketCanRequestTest, ARequestingSessionOnVcanNeedsNoRate)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&baud=250000&identify=0&diagnose=0");
    EXPECT_EQ(s.status(), ECUIO_OK) << "vcan has no rate: " << s.detail();
}

/* Requesting sessions on vcan created while it lives start with the
 * rate unproven, as on a real controller, with this quiet time */
class Unproven
{
public:
    explicit Unproven(int64_t quiet_us)
    {
        ecuio_socketcan_set_vcan_unproven(quiet_us);
    }
    ~Unproven() { ecuio_socketcan_set_vcan_unproven(0); }
    Unproven(const Unproven&) = delete;
    Unproven& operator=(const Unproven&) = delete;
};

/* The session's requests (0x7E0) a raw listener reads within ms */
unsigned requests_within(int fd, int ms)
{
    unsigned n = 0;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return n;
        }
        can_frame f{};
        if (vcan_bus::read_for(fd, &f, sizeof f,
                               static_cast<int>(left.count())) ==
                static_cast<long>(sizeof f) &&
            (f.can_id & CAN_ERR_FLAG) == 0 &&
            (f.can_id & CAN_SFF_MASK) == vcan_bus::kRequest) {
            ++n;
        }
    }
}

/* Unproven, the first echo proves nothing yet: the ECU's reply does (the
 * quiet time here outlasts the test), and the rotation runs */
TEST_F(SocketCanRequestTest, AnUnprovenSessionIsProvenByTheEcusReply)
{
    Unproven unproven(60'000'000);
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_support_t sup{};
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms))
        << "the walk needs requests past the first";
    int32_t e = -1;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
}

/* Unproven with no ECU: once N_As passes after the echo with no error
 * frame, the rate is proven and requests go on (each unanswered) */
TEST_F(SocketCanRequestTest, AnUnprovenSessionIsProvenByQuietAfterItsEcho)
{
    Unproven unproven(25'000); /* N_As */
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    EXPECT_GE(requests_within(listener.get(), 1500), 2u)
        << "sending stopped after the first echo";
    int32_t e = -1;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_NE(e, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
}

/* The desk bus's adapter (ADR-0040's appendix of 2026-11-02): the frame
 * echoed, then the controller reported the missing ACK. An error frame
 * after the first echo, within the quiet time, halts the sending as one
 * before it does */
TEST_F(SocketCanRequestTest, AnErrorFrameAfterTheFirstEchoHalts)
{
    Unproven unproven(1'000'000); /* long, so the test's wake can't miss it */
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    can_frame f{};
    bool first = false;
    const auto end = std::chrono::steady_clock::now() + 2000ms;
    while (!first && std::chrono::steady_clock::now() < end) {
        first = vcan_bus::read_for(listener.get(), &f, sizeof f, 100) ==
                    static_cast<long>(sizeof f) &&
                (f.can_id & CAN_SFF_MASK) == vcan_bus::kRequest;
    }
    ASSERT_TRUE(first) << "no first request";
    can_frame err{};
    err.can_id = CAN_ERR_FLAG | CAN_ERR_ACK;
    err.len = 8;
    ASSERT_EQ(::write(listener.get(), &err, sizeof err),
              static_cast<ssize_t>(sizeof err));
    int32_t e = -1;
    uint32_t n = 0;
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_error_state(s.get(), &e, &n) == ECUIO_OK &&
                   e == ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        },
        1000ms))
        << "an error after the first echo left the rate proven";
    EXPECT_EQ(requests_within(listener.get(), 1500), 0u)
        << "a request sent after the error";
}

/* Reply timing (ADR-0042) on SocketCAN: a row per request in the
 * rotation, the intervals on the kernel's receive times, and ISO-TP's
 * counts from core's receiver, the VIN's First Frame among them */
TEST_F(SocketCanRequestTest, TimingReportsEachRequestOnTheKernelsClock)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    const auto intervals = [&] {
        uint32_t n = 0;
        for (uint32_t i = 0; i < count && i < rows.size(); ++i) {
            n += rows[i].intervals;
        }
        return n;
    };
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_timing(s.get(), &t, rows.data(),
                                            static_cast<uint32_t>(rows.size()),
                                            &count) == ECUIO_OK &&
                   intervals() >= 40 && t.isotp_first_frames > 0;
        },
        10000ms))
        << intervals() << " intervals published";
    EXPECT_EQ(t.interval_clock, ECUIO_TIMING_CLOCK_KERNEL);
    EXPECT_NE(t.flags & ECUIO_TIMING_ISOTP, 0u);
    EXPECT_GE(t.isotp_completed, t.isotp_first_frames);
    EXPECT_EQ(t.isotp_dropped, 0u);
    EXPECT_EQ(t.isotp_malformed, 0u);
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_request_timing_t& r = rows[i];
        if (r.answered == 0) {
            continue;
        }
        SCOPED_TRACE("PID " + std::to_string(r.request[1]));
        EXPECT_GT(r.latency_max_us, 0u);
        EXPECT_LT(r.latency_p50_us, 100000u) << "within the engine's wait";
        EXPECT_EQ(r.intervals_mixed, 0u) << "every frame has a kernel time";
    }
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

/* ---- The simulated ECU on the bus (ADR-0046) ---- */

/* The simulated ECU on the test's interface, destroyed when it goes */
class Sim
{
public:
    explicit Sim(const std::string& uri, uint32_t options = 0)
    {
        status_ = ecuio_sim_create_with(uri.c_str(), vcan_bus::interface_name(),
                                        options, &sim_);
        detail_ = ecuio_last_error_detail();
    }
    ~Sim() { ecuio_sim_destroy(sim_); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_sim_t* get() const { return sim_; }
    ecuio_sim_stats_t stats() const
    {
        ecuio_sim_stats_t s{};
        EXPECT_EQ(ecuio_sim_get_stats(sim_, &s), ECUIO_OK);
        return s;
    }

private:
    ecuio_sim_t* sim_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

/* The engine speeds a session decoded (obd2's def, packed replies
 * split by core), in r/min */
struct Rpms {
    std::mutex mutex;
    std::vector<double> rpm;
    static void on_values(const ecuio_value_batch_t* b, void* user)
    {
        auto* self = static_cast<Rpms*>(user);
        for (uint32_t i = 0; i < b->count; ++i) {
            if (b->values[i].signal_id == ECUIO_OBD2_SIG_RPM_ID) {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->rpm.push_back(b->values[i].value);
            }
        }
    }
    void listen(ecuio_session_t* s)
    {
        ASSERT_EQ(ecuio_register_values_callback(s, &Rpms::on_values, this),
                  ECUIO_OK);
    }
    std::vector<double> seen()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return rpm;
    }
};

class SimBusTest : public SocketCanRequestTest
{};

/* values=drive: the obd2 session reads the simulated car: its bitmaps,
 * its VIN, the mock's diagnostics, and the engine starting after the key
 * on (0 rpm for the first 15 s, then the idle) */
TEST_F(SimBusTest, AnObd2SessionReadsTheSimulatedCar)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Rpms rpms;
    Session s("?profile=obd2");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    rpms.listen(s.get());
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms));
    EXPECT_EQ(sup.ranges, 0x07u) << "the bitmaps at 0x00, 0x20 and 0x40";
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        10000ms));
    char vin[ECUIO_VIN_TEXT_LEN];
    ASSERT_EQ(ecuio_identity_vin(&id, vin, sizeof vin), ECUIO_OK);
    EXPECT_STREQ(vin, "ZZZFAKE11M0000001");
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s.get(), 0, &d) ==
                       ECUIO_OK &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        10000ms));
    fake_diagnostics::expect_report(d);
    ASSERT_FALSE(rpms.seen().empty());
    EXPECT_EQ(rpms.seen().front(), 0.0) << "the key on, the engine stopped";
    EXPECT_TRUE(wait_for(
        [&] {
            const std::vector<double> r = rpms.seen();
            return !r.empty() && r.back() > 500.0;
        },
        25000ms))
        << "the engine starts";
    const ecuio_sim_stats_t st = sim.stats();
    EXPECT_GT(st.requests, 0u);
    EXPECT_GT(st.replies, 0u);
    EXPECT_EQ(st.flags, 0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
}

/* A functional session (?txid=0x7DF): the VIN, a segmented reply from
 * 0x7E8, completes because core sends its Flow Control on 0x7E0, the
 * physical request ID, where the simulated ECU waits for it (ISO 15765-4
 * section 6.3.2.1); on 0x7DF the ECU would never send the rest */
TEST_F(SimBusTest, AFunctionalRequestsSegmentedReplyCompletes)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Session s("?profile=obd2&txid=0x7DF&probe=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                       ECUIO_OK &&
                   id.items > 0 && id.list[0].outcome != ECUIO_IDENTITY_UNASKED;
        },
        10000ms));
    EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_ANSWERED);
    char vin[ECUIO_VIN_TEXT_LEN];
    ASSERT_EQ(ecuio_identity_vin(&id, vin, sizeof vin), ECUIO_OK);
    EXPECT_STREQ(vin, "ZZZFAKE11M0000001");
    EXPECT_EQ(sim.stats().unflowed, 0u) << "a reply waited for Flow Control";
}

/* values=canned: the fakes' ECU, so the probe walks the same bitmaps and
 * drops nothing from obd2 */
TEST_F(SimBusTest, CannedValuesWalkTheFakesBitmaps)
{
    Sim sim("mock://sim?values=canned");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Rpms rpms;
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    rpms.listen(s.get());
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms));
    EXPECT_EQ(sup.ranges, 0x3Fu);
    EXPECT_EQ(sup.dropped, 0u);
    ASSERT_TRUE(wait_for([&] { return !rpms.seen().empty(); }, 5000ms));
    EXPECT_EQ(rpms.seen().front(), 2000.0) << "the fakes' reading";
}

/* A clear at the key on (0 rpm): 04 obeyed, the report read again
 * cleared */
TEST_F(SimBusTest, AClearClearsTheSimulatedCar)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Session s("?profile=obd2&probe=0&identify=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_diagnostics_t d{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s.get(), 0, &d) ==
                       ECUIO_OK &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        8000ms));
    ASSERT_EQ(ecuio_session_clear_diagnostics(s.get(), 0, d.generation),
              ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_diagnostics(s.get(), 0, &d) ==
                       ECUIO_OK &&
                   d.clear == ECUIO_CLEAR_CLEARED &&
                   (d.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        8000ms));
    fake_diagnostics::expect_cleared_report(d);
    EXPECT_EQ(sim.stats().clears, 1u);
}

/* Every fault, each on a period of its own (two faults on one period
 * would leave the second never chosen): the session stays in step (the
 * walk reads the bitmaps, which are never broken; the rotation keeps
 * going; the link stays healthy) and the simulated ECU counts each */
TEST_F(SimBusTest, FaultsLeaveTheSessionInStep)
{
    Sim sim("mock://sim?values=canned&faults=silent:7,late:11,"
            "bad_sequence:13,stalled:17");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Rpms rpms;
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    rpms.listen(s.get());
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms));
    EXPECT_EQ(sup.ranges, 0x3Fu);
    ASSERT_TRUE(wait_for(
        [&] {
            const ecuio_sim_stats_t st = sim.stats();
            return st.silent > 0 && st.late > 0 && st.bad_sequence > 0 &&
                   st.stalled > 0;
        },
        20000ms))
        << "silent " << sim.stats().silent << ", late " << sim.stats().late
        << ", bad sequence " << sim.stats().bad_sequence << ", stalled "
        << sim.stats().stalled;
    const size_t before = rpms.seen().size();
    const ecuio_sim_stats_t st0 = sim.stats();
    /* each silent or late reply costs the session its 100 ms wait, and
     * packs that keep failing go back to one PID per request, so the
     * engine speed comes round every second or two */
    const bool going =
        wait_for([&] { return rpms.seen().size() > before; }, 10000ms);
    const ecuio_sim_stats_t st1 = sim.stats();
    (void)ecuio_session_get_channel_support(s.get(), 0, &sup);
    EXPECT_TRUE(going) << "the rotation keeps answering: rpm values " << before
                       << " then " << rpms.seen().size() << "; requests "
                       << st0.requests << " then " << st1.requests
                       << ", replies " << st0.replies << " then " << st1.replies
                       << ", unflowed " << st1.unflowed << "; support flags "
                       << sup.flags;
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
}

/* A segmented reply broken at the frame level (a wrong sequence number
 * on its last Consecutive Frame, or that frame past N_Cr) is one the
 * session's ISO-TP receiver begins and never finishes: never delivered
 * whole. One fault at a time, every second segmented reply, so packs
 * never fail three in a row and keep coming. */
TEST_F(SimBusTest, ABrokenSegmentedReplyIsNeverDelivered)
{
    for (const char* fault : {"bad_sequence", "stalled"}) {
        SCOPED_TRACE(fault);
        Sim sim(std::string("mock://sim?values=canned&faults=") + fault + ":2");
        ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
        Session s("?profile=obd2&identify=0&diagnose=0");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        const auto broken = [&sim, fault] {
            const ecuio_sim_stats_t st = sim.stats();
            return std::string(fault) == "stalled" ? st.stalled
                                                   : st.bad_sequence;
        };
        ASSERT_TRUE(wait_for([&] { return broken() >= 5; }, 15000ms))
            << broken();
        const uint64_t n = broken();
        /* the timing snapshot comes once a second */
        ecuio_session_timing_t timing{};
        EXPECT_TRUE(wait_for(
            [&] {
                uint32_t rows = 0;
                return ecuio_session_get_timing(s.get(), &timing, nullptr, 0,
                                                &rows) == ECUIO_OK &&
                       timing.isotp_dropped >= n;
            },
            3000ms))
            << "dropped " << timing.isotp_dropped << " of " << n;
        ecuio_channel_support_t sup{};
        ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 0, &sup),
                  ECUIO_OK);
        EXPECT_NE(sup.flags & ECUIO_SUPPORT_PACKED, 0u);
        EXPECT_EQ(sup.flags & ECUIO_SUPPORT_UNPACKED, 0u)
            << "packs kept coming";
    }
}

/* replay://: the test car's engine start answers as the car did. The
 * walk reads its own bitmaps (00 to A0), the rotation keeps the PIDs the
 * recording holds and only them, packs hold (put together from each
 * PID's reply), every engine speed decoded is one the car sent, and the
 * VIN given answers identification */
TEST_F(SimBusTest, ARecordingAnswersAsTheTestCar)
{
    const std::string dir =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    std::set<uint8_t> pids;
    std::set<double> recorded_rpm;
    for (const vcan_bus::Message& m :
         vcan_bus::read_frames_csv((dir + "/frames.csv").c_str())) {
        if (m.data.size() >= 2 && m.data[0] == 0x41 && m.data[1] % 0x20 != 0) {
            pids.insert(m.data[1]);
        }
        if (m.data.size() == 4 && m.data[0] == 0x41 && m.data[1] == 0x0C) {
            recorded_rpm.insert((m.data[2] * 256 + m.data[3]) / 4.0);
        }
    }
    ASSERT_FALSE(recorded_rpm.empty());
    Sim sim("replay://" + dir + "?vin=1FAFAKE10M0000001");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Rpms rpms;
    Session s("?profile=obd2&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    rpms.listen(s.get());
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms));
    EXPECT_EQ(sup.ranges, 0x3Fu) << "the car's bitmaps, 00 to A0";
    /* each PID the car listed and never sent is dropped once asked */
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   sup.requests - sup.dropped == pids.size();
        },
        10000ms))
        << sup.requests - sup.dropped << " kept of " << pids.size()
        << ": the rotation keeps the PIDs the recording holds, and only them";
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        10000ms));
    char vin[ECUIO_VIN_TEXT_LEN];
    ASSERT_EQ(ecuio_identity_vin(&id, vin, sizeof vin), ECUIO_OK);
    EXPECT_STREQ(vin, "1FAFAKE10M0000001");
    ASSERT_TRUE(wait_for([&] { return rpms.seen().size() >= 30; }, 10000ms))
        << rpms.seen().size();
    for (double r : rpms.seen()) {
        EXPECT_EQ(recorded_rpm.count(r), 1u) << r << " r/min isn't the car's";
    }
    ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 0, &sup), ECUIO_OK);
    EXPECT_NE(sup.flags & ECUIO_SUPPORT_PACKED, 0u);
    EXPECT_EQ(sup.flags & ECUIO_SUPPORT_UNPACKED, 0u);
    const ecuio_sim_stats_t st = sim.stats();
    EXPECT_GT(st.replies, 0u);
    EXPECT_GT(st.refused, 0u) << "the PIDs the car listed and never sent";
    EXPECT_EQ(st.flags, 0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
}

/* Another node on 0x7E8, the reply ID, means an ECU answers there
 * already (a car's, on a bus taken for a desk's): from then on the sim
 * sends nothing, no reply and no Flow Control, and says so. Its own
 * replies never count. Under the desk bus's option, which vcan takes as
 * it is. */
TEST_F(SimBusTest, AnotherNodeOnTheReplyIdSilencesItForGood)
{
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Sim sim("mock://sim", ECUIO_SIM_ALLOW_DESK_BUS);
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    const auto send = [&](uint32_t id, std::initializer_list<uint8_t> data) {
        struct can_frame f;
        std::memset(&f, 0, sizeof f);
        f.can_id = id;
        f.can_dlc = 8;
        std::copy(data.begin(), data.end(), f.data);
        ASSERT_EQ(::write(bus.get(), &f, sizeof f),
                  static_cast<ssize_t>(sizeof f));
    };
    /* Whether a frame on 0x7E8 comes within ms (the bus socket isn't
     * sent its own writes, so it is the sim's) */
    const auto heard_reply = [&](int ms) {
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    end - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }
            struct can_frame f;
            if (vcan_bus::read_for(bus.get(), &f, sizeof f,
                                   static_cast<int>(left.count())) ==
                    static_cast<long>(sizeof f) &&
                (f.can_id & CAN_SFF_MASK) == 0x7E8u) {
                return true;
            }
        }
    };
    send(0x7E0, {0x02, 0x01, 0x00});
    ASSERT_TRUE(heard_reply(1000)) << "the sim answers";
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(sim.stats().flags, 0u) << "its own reply isn't another node";

    send(0x7E8, {0x06, 0x41, 0x00, 0xBE, 0x1F, 0xA8, 0x13});
    ASSERT_TRUE(wait_for(
        [&] { return (sim.stats().flags & ECUIO_SIM_YIELDED) != 0; }, 2000ms));
    send(0x7E0, {0x02, 0x01, 0x00});
    send(0x7E0, {0x10, 0x0A, 0x22, 0xF1, 0x90, 0x00, 0x00, 0x00});
    EXPECT_FALSE(heard_reply(500)) << "no reply, no Flow Control";
    const ecuio_sim_stats_t st = sim.stats();
    EXPECT_EQ(st.requests, 2u) << "the request after it is still counted";
    EXPECT_EQ(st.replies, 1u);
    EXPECT_EQ(st.flags, ECUIO_SIM_YIELDED);
}

/* Each signal's newest value or text a session decoded, by def id */
struct Values {
    std::mutex mutex;
    std::map<uint16_t, double> number;
    std::map<uint16_t, std::string> text;
    static void on_values(const ecuio_value_batch_t* b, void* user)
    {
        auto* self = static_cast<Values*>(user);
        std::lock_guard<std::mutex> lock(self->mutex);
        for (uint32_t i = 0; i < b->count; ++i) {
            const ecuio_value_t& v = b->values[i];
            if (v.text != nullptr) {
                self->text[v.signal_id] = std::string(v.text, v.text_len);
            } else {
                self->number[v.signal_id] = v.value;
            }
        }
    }
    void listen(ecuio_session_t* s)
    {
        ASSERT_EQ(ecuio_register_values_callback(s, &Values::on_values, this),
                  ECUIO_OK);
    }
    bool has(uint16_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return number.count(id) != 0 || text.count(id) != 0;
    }
    double number_of(uint16_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return number.count(id) != 0 ? number[id] : -1.0;
    }
    std::string text_of(uint16_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return text.count(id) != 0 ? text[id] : std::string();
    }
};

/* modules=2: one uds-generic session asks both ECUs. The engine ECU
 * (0x7E8, channel 0) refuses F190, so it leaves the rotation; the second
 * module (0x7E9, channel 1) answers its DIDs, the name a segmented reply
 * whose Flow Control the tester sends on 0x7E1, the module's request ID */
TEST_F(SimBusTest, OneSessionAsksBothModules)
{
    Sim sim("mock://sim?modules=2");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Values values;
    Session s("?profile=uds-generic&did=F190:text,7E9:D100:u8,7E9:D101:u8,"
              "7E9:D102:text");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    values.listen(s.get());
    const uint16_t vin = ECUIO_UDS_GENERIC_SIG_ID_BASE;
    const uint16_t fluid = vin + 1, gear = vin + 2, name = vin + 3;
    ASSERT_TRUE(wait_for(
        [&] {
            return values.has(fluid) && values.has(gear) && values.has(name);
        },
        5000ms))
        << "requests " << sim.stats().requests << ", replies "
        << sim.stats().replies << ", unflowed " << sim.stats().unflowed;
    EXPECT_EQ(values.number_of(fluid), 130.0) << "D100's byte, 0x82";
    EXPECT_EQ(values.number_of(gear), 3.0);
    EXPECT_EQ(values.text_of(name), "TCM-HavneSim");
    EXPECT_FALSE(values.has(vin)) << "the engine ECU has no F190";
    ecuio_channel_support_t engine{}, module{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &engine) ==
                       ECUIO_OK &&
                   engine.dropped == 1;
        },
        5000ms))
        << "dropped " << engine.dropped;
    EXPECT_EQ(engine.rx_id, 0x7E8u);
    ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 1, &module), ECUIO_OK);
    EXPECT_EQ(module.rx_id, 0x7E9u);
    EXPECT_EQ(module.dropped, 0u);
    const ecuio_sim_stats_t st = sim.stats();
    EXPECT_EQ(st.unflowed, 0u);
    EXPECT_EQ(st.flags, 0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
}

/* Without modules=2 nothing answers on 0x7E9: the default stays the
 * engine ECU alone */
TEST_F(SimBusTest, TheSecondModuleIsOptIn)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Values values;
    Session s("?profile=uds-generic&did=7E9:D100:u8");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    values.listen(s.get());
    std::this_thread::sleep_for(1500ms);
    EXPECT_FALSE(values.has(ECUIO_UDS_GENERIC_SIG_ID_BASE));
    EXPECT_EQ(sim.stats().replies, 0u);
}

/* pending: every third reply comes as 7F <service> 78 and then, 300 ms
 * later, the answer. The request engine waits on (P2*), so no request
 * times out, the rotation keeps answering, and the replies the 78 put
 * off show in the session's reply timing as its slowest */
TEST_F(SimBusTest, AResponsePendingIsWaitedFor)
{
    Sim sim("mock://sim?values=canned&faults=pending:3");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Rpms rpms;
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    rpms.listen(s.get());
    ASSERT_TRUE(wait_for([&] { return sim.stats().pending >= 5; }, 15000ms))
        << "pending " << sim.stats().pending;
    const size_t before = rpms.seen().size();
    EXPECT_TRUE(wait_for([&] { return rpms.seen().size() > before; }, 10000ms))
        << "the rotation keeps answering";
    /* the worker publishes timing at most once a second */
    std::this_thread::sleep_for(1200ms);
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_timing(s.get(), &t, rows.data(),
                                       static_cast<uint32_t>(rows.size()),
                                       &count),
              ECUIO_OK);
    ASSERT_GT(count, 0u);
    uint32_t timeouts = 0, answered = 0, slowest_us = 0;
    for (uint32_t i = 0; i < count && i < rows.size(); ++i) {
        timeouts += rows[i].timeouts;
        answered += rows[i].answered;
        slowest_us = (std::max)(slowest_us, rows[i].latency_max_us);
    }
    EXPECT_EQ(timeouts, 0u) << "a 78 extends the wait";
    EXPECT_GT(answered, 0u);
    EXPECT_GE(slowest_us, static_cast<uint32_t>(250000))
        << "a pending reply's latency is the 300 ms after its 78";
    const ecuio_sim_stats_t st = sim.stats();
    EXPECT_EQ(st.silent + st.late + st.bad_sequence + st.stalled, 0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
}

/* pending on the second module: its own replies, counted apart, are put
 * off and still arrive */
TEST_F(SimBusTest, TheSecondModulesPendingRepliesArrive)
{
    Sim sim("mock://sim?modules=2&faults=pending:2");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Values values;
    Session s("?profile=uds-generic&did=7E9:D100:u8,7E9:D102:text");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    values.listen(s.get());
    const uint16_t fluid = ECUIO_UDS_GENERIC_SIG_ID_BASE;
    const uint16_t name = fluid + 1;
    ASSERT_TRUE(wait_for(
        [&] {
            return values.has(fluid) && values.has(name) &&
                   sim.stats().pending >= 4;
        },
        10000ms))
        << "pending " << sim.stats().pending;
    EXPECT_EQ(values.number_of(fluid), 130.0);
    EXPECT_EQ(values.text_of(name), "TCM-HavneSim");
    ecuio_channel_support_t module{};
    ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 0, &module), ECUIO_OK);
    EXPECT_EQ(module.dropped, 0u) << "a 78 is no refusal";
}

/* ---- DIDs from a signal set (ADR-0046's appendix) ---- */

namespace {

ecuio_signal_def_t vcan_did(uint16_t id, uint32_t rx, uint16_t did,
                            uint8_t kind, uint8_t order, uint16_t start,
                            uint8_t bits, double scale, double offset,
                            const char* name)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = kind;
    d.byte_order = order;
    d.start_byte = start;
    d.bit_length = bits;
    d.scale = scale;
    d.offset = offset;
    std::snprintf(d.name, sizeof d.name, "%s", name);
    return d;
}

/* A profile of the engine ECU's DID and a module's own (0x7E2 to
 * 0x7EA): a word, a signed word and a flag in one reply, a text in a
 * segmented one. The registry keeps the pointers: static storage */
const char* register_did_profile()
{
    static const ecuio_signal_def_t defs[] = {
        vcan_did(1, 0x7E8, 0x1E1C, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 16,
                 0.1, -40.0, "Oil Temp"),
        vcan_did(2, 0x7EA, 0xD200, ECUIO_VALUE_INT, ECUIO_BYTE_ORDER_BE, 3, 16,
                 0.01, 0.0, "Trim"),
        vcan_did(3, 0x7EA, 0xD200, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_LE, 5, 1,
                 1.0, 0.0, "Lockup"),
        vcan_did(4, 0x7EA, 0xD201, ECUIO_VALUE_TEXT, ECUIO_BYTE_ORDER_LE, 3, 0,
                 1.0, 0.0, "MODULE-HavneSim"),
    };
    static const ecuio_channel_decl_t channels[] = {{0x7E8, 0x7E0},
                                                    {0x7EA, 0x7E2}};
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 4;
    set.defs = defs;
    set.channel_count = 2;
    set.channels = channels;
    ecuio_validation_t why{};
    EXPECT_EQ(ecuio_profile_register("sim-dids", &set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    return "sim-dids";
}

} // namespace

/* profile=: a session on the same profile reads, on the engine ECU and
 * on the profile's own module, the values the simulated ECU was given,
 * each field inverted from its def and decoded back, and a value given
 * mid-session; the text field's segmented reply arrives whole */
TEST_F(SimBusTest, ASessionReadsTheValuesTheProfilesDidsWereGiven)
{
    const std::string profile = register_did_profile();
    Sim sim("mock://sim?profile=" + profile);
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    ASSERT_EQ(ecuio_sim_set_value(sim.get(), 1, 92.3), ECUIO_OK);
    ASSERT_EQ(ecuio_sim_set_value(sim.get(), 2, -12.5), ECUIO_OK);
    ASSERT_EQ(ecuio_sim_set_value(sim.get(), 3, 1.0), ECUIO_OK);
    EXPECT_EQ(ecuio_sim_set_value(sim.get(), 2, 400.0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "past a signed word at 0.01";
    EXPECT_EQ(ecuio_sim_set_value(sim.get(), 4, 1.0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a text answers its name";
    EXPECT_EQ(ecuio_sim_set_value(sim.get(), 99, 1.0),
              ECUIO_ERR_INVALID_ARGUMENT);
    Values values;
    Session s("?profile=" + profile);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    values.listen(s.get());
    ASSERT_TRUE(wait_for(
        [&] {
            return values.has(1) && values.has(2) && values.has(3) &&
                   values.has(4);
        },
        5000ms))
        << "requests " << sim.stats().requests << ", replies "
        << sim.stats().replies << ", unflowed " << sim.stats().unflowed;
    EXPECT_NEAR(values.number_of(1), 92.3, 1e-9);
    EXPECT_NEAR(values.number_of(2), -12.5, 1e-9);
    EXPECT_EQ(values.number_of(3), 1.0);
    EXPECT_EQ(values.text_of(4), "MODULE-HavneSim");
    ASSERT_EQ(ecuio_sim_set_value(sim.get(), 1, 104.0), ECUIO_OK);
    EXPECT_TRUE(wait_for(
        [&] { return std::fabs(values.number_of(1) - 104.0) < 1e-9; }, 5000ms))
        << "now " << values.number_of(1);
    const ecuio_sim_stats_t st = sim.stats();
    EXPECT_EQ(st.unflowed, 0u);
    EXPECT_EQ(st.flags, 0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister(profile.c_str()), ECUIO_OK);
}

/* Without profile= a sim answers no set's DIDs, and takes no value */
TEST_F(SimBusTest, ASimWithNoProfileTakesNoValue)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    EXPECT_EQ(ecuio_sim_set_value(sim.get(), 1, 0.0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("profile"),
              std::string::npos);
}

/* A sim on a bus answers on it: ecuio_sim_ask is an unbound sim's
 * alone (ADR-0055), so the two never race over the ECU */
TEST_F(SimBusTest, ASimOnABusRefusesAnAsk)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    const uint8_t q[2] = {0x01, 0x00};
    uint32_t n = 99;
    EXPECT_EQ(ecuio_sim_ask(sim.get(), 0x7E0, q, 2, ECUIO_SIM_NOW, nullptr, 0,
                            &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(n, 99u);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("bus"),
              std::string::npos);
    EXPECT_EQ(sim.stats().requests, 0u);
}
