// File/layer: tests/test_vcan_requests_focus.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanRequestTest (test_vcan)

#include "test_vcan_requests_support.h"

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

/* ADR-0056 section 4 off the bus, a raw listener's kernel-side order and
 * its read times: two picks with a minimum time of 200 ms, sweeps at
 * least that far apart at their median */
TEST_F(SocketCanRequestTest, AMinimumTimeSpacesTheFocusSweeps)
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
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    ASSERT_EQ(ecuio_session_set_focus_with(s.get(), ids.data(), 2, 200, 0),
              ECUIO_OK);

    std::vector<std::chrono::steady_clock::time_point> at;
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (at.size() < 12 && std::chrono::steady_clock::now() < end) {
        can_frame f{};
        if (vcan_bus::read_for(listener.get(), &f, sizeof f, 100) ==
                static_cast<long>(sizeof f) &&
            (f.can_id & CAN_ERR_FLAG) == 0 &&
            (f.can_id & CAN_SFF_MASK) == vcan_bus::kRequest && f.len >= 3 &&
            f.data[1] == 0x01) {
            at.push_back(std::chrono::steady_clock::now());
        }
    }
    ASSERT_EQ(at.size(), 12u);
    std::vector<int64_t> sweeps;
    for (size_t k = 0; k + 2 < at.size(); k += 2) {
        sweeps.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                             at[k + 2] - at[k])
                             .count());
    }
    std::sort(sweeps.begin(), sweeps.end());
    EXPECT_GE(sweeps[sweeps.size() / 2], 180000)
        << "sweeps came faster than the minimum";
}

static int64_t process_cpu_us()
{
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return int64_t{t.tv_sec} * 1000000 + t.tv_nsec / 1000;
}

/* The worker sleeps through a minimum time's gaps: a session older than
 * the gap uses well under a second of CPU in a second of them, where a
 * worker polling without a timeout uses all of it. Load only lowers
 * the figure, so it can't fail this. */
TEST_F(SocketCanRequestTest, AMinimumTimesGapIsSleptThrough)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s.get());
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 && d.mux_key == 0x410Cu) {
            ids.push_back(d.id);
        }
    }
    ASSERT_EQ(ids.size(), 1u);
    ASSERT_EQ(ecuio_session_set_focus_with(s.get(), ids.data(), 1, 500, 0),
              ECUIO_OK);
    std::this_thread::sleep_for(1200ms); /* older than a gap */
    const int64_t before = process_cpu_us();
    std::this_thread::sleep_for(1s);
    const int64_t used = process_cpu_us() - before;
    RecordProperty("cpu_us", static_cast<int>(used));
    EXPECT_LT(used, 400000) << "the worker polled through the gap";
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

/* A halted session that asks only its picks, its pick taken away: the
 * halt is for the session's life, so its hard failure stays. The idle
 * passes with nothing picked, clean reads on any other session, end
 * nothing here: OK would read as a link that can send. */
TEST_F(SocketCanRequestTest, AHaltedSessionsFailureStaysWithNothingPicked)
{
    Unproven unproven(1'000'000);
    const CarProfile profile("socketcan-halted-car");
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    Session s("?profile=socketcan-halted-car&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const uint16_t pick = car_sized::id_of(7);
    ASSERT_EQ(ecuio_session_set_focus(s.get(), &pick, 1), ECUIO_OK)
        << ecuio_last_error_detail();
    can_frame f{};
    bool first = false;
    const auto end = std::chrono::steady_clock::now() + 2000ms;
    while (!first && std::chrono::steady_clock::now() < end) {
        first = vcan_bus::read_for(listener.get(), &f, sizeof f, 100) ==
                    static_cast<long>(sizeof f) &&
                (f.can_id & CAN_SFF_MASK) == vcan_bus::kRequest;
    }
    ASSERT_TRUE(first) << "the pick's request never went out";
    can_frame err{};
    err.can_id = CAN_ERR_FLAG | CAN_ERR_ACK;
    err.len = 8;
    ASSERT_EQ(::write(listener.get(), &err, sizeof err),
              static_cast<ssize_t>(sizeof err));
    ASSERT_TRUE(wait_for(
        [&] { return state_of(s) == ECUIO_ERR_TRANSPORT_HARD_FAILURE; },
        1000ms));
    ASSERT_EQ(ecuio_session_set_focus(s.get(), nullptr, 0), ECUIO_OK);
    /* Past a quiet run's passes twice over */
    const auto still = std::chrono::steady_clock::now() + 2500ms;
    while (std::chrono::steady_clock::now() < still) {
        ASSERT_EQ(state_of(s), ECUIO_ERR_TRANSPORT_HARD_FAILURE)
            << "a halted session read as a link that can send";
        std::this_thread::sleep_for(10ms);
    }
    uint32_t episodes = 0;
    EXPECT_EQ(state_of(s, &episodes), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes, 1u);
}

/* A halted session has requests due and sends none: its worker sleeps
 * through its reads instead of polling with no wait, still asking, so
 * no quiet read moves the link off the hard failure. Load only lowers
 * the figure, so it can't fail this. */
TEST_F(SocketCanRequestTest, AHaltedSessionSleepsThroughItsReads)
{
    Unproven unproven(1'000'000);
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
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_error_state(s.get(), &e, &n) == ECUIO_OK &&
                   e == ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        },
        1000ms));
    const int64_t before = process_cpu_us();
    std::this_thread::sleep_for(1500ms);
    const int64_t used = process_cpu_us() - before;
    RecordProperty("cpu_us", static_cast<int>(used));
    EXPECT_LT(used, 600000) << "the halted worker polled with no wait";
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_ERR_TRANSPORT_HARD_FAILURE)
        << "a halted session stays asking: no quiet read counts";
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
