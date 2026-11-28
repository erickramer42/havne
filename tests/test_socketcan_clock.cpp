// File/layer: tests/test_socketcan_clock.cpp | SocketCAN listening's rules (ADR-0032)
// Testing: UNIT: SocketCanTest, SimTest (test_socketcan)

#include "test_socketcan_support.h"

namespace {

/* ---- Frames ---- */

ecuio_frame_t converted(uint32_t can_id, std::vector<uint8_t> data,
                        bool fd = false, uint8_t fd_flags = 0,
                        uint8_t len = 0xFF)
{
    socketcan_raw in;
    in.can_id = can_id;
    in.len = len == 0xFF ? static_cast<uint8_t>(data.size()) : len;
    in.fd = fd;
    in.fd_flags = fd_flags;
    data.resize(64);
    in.data = data.data();
    ecuio_frame_t f{};
    f.arbitration_id = 0xDEAD; /* the conversion owns every field */
    f.flags = 0xFFFF;
    socketcan_to_frame(in, &f);
    return f;
}

/* ---- Sessions ---- */

struct Created {
    ecuio_status_t status;
    std::string detail;
};
Created create(const char* uri)
{
    ecuio_session_t* s = nullptr;
    const ecuio_status_t st = ecuio_session_create(uri, &s);
    Created c{st, ecuio_last_error_detail()};
    if (s != nullptr) {
        ecuio_session_destroy(s);
    }
    return c;
}

} // namespace

TEST(SocketCanTest, AKernelFrameKeepsItsIdFlagsAndBytes)
{
    ecuio_frame_t f = converted(0x7E8, {0x03, 0x41, 0x0C, 0x1A, 0xF8, 0, 0, 0});
    EXPECT_EQ(f.arbitration_id, 0x7E8u);
    EXPECT_EQ(f.flags, 0u);
    EXPECT_EQ(f.data_length, 8u);
    EXPECT_EQ(f.data[1], 0x41);
    EXPECT_EQ(f.channel_id, 0u);
    EXPECT_EQ(f.reserved, 0u);

    f = converted(0x18DAF110u | SOCKETCAN_EFF_FLAG, {1, 2});
    EXPECT_EQ(f.arbitration_id, 0x18DAF110u);
    EXPECT_EQ(f.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_EQ(f.data_length, 2u);

    /* A remote frame asks for 8 bytes and carries none */
    f = converted(0x123u | SOCKETCAN_RTR_FLAG, {}, false, 0, 8);
    EXPECT_EQ(f.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_REMOTE));
    EXPECT_EQ(f.data_length, 0u);

    std::vector<uint8_t> twelve(12, 0xAB);
    f = converted(0x0C0, twelve, true, SOCKETCAN_FD_BRS);
    EXPECT_EQ(f.flags, ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_BITRATE_SWITCH);
    EXPECT_EQ(f.data_length, 12u);
    EXPECT_EQ(f.data[11], 0xAB);
    /* ESI: the sender error passive */
    f = converted(0x0C0, twelve, true, SOCKETCAN_FD_ESI);
    EXPECT_EQ(f.flags, ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_FD_ESI);
    f = converted(0x0C0, twelve, false, SOCKETCAN_FD_ESI);
    EXPECT_EQ(f.flags, 0u) << "a classic frame has no FD flags";

    /* A length past the frame's room is cut to it */
    f = converted(0x100, {1, 2, 3, 4, 5, 6, 7, 8}, false, 0, 15);
    EXPECT_EQ(f.data_length, 8u);
}

TEST(SocketCanTest, AnErrorFrameKeepsItsClassAndDetail)
{
    const uint32_t id = SOCKETCAN_ERR_FLAG | SOCKETCAN_ERR_CRTL;
    ecuio_frame_t f = converted(id, {0, 0x04, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(f.arbitration_id, SOCKETCAN_ERR_CRTL);
    EXPECT_EQ(f.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_ERROR));
    EXPECT_EQ(f.data_length, 8u);
    EXPECT_EQ(f.data[1], 0x04);
    EXPECT_FALSE(socketcan_error_is_hard(id)) << "error-passive still receives";
    EXPECT_TRUE(
        socketcan_error_is_hard(SOCKETCAN_ERR_FLAG | SOCKETCAN_ERR_BUSOFF));
    EXPECT_TRUE(
        socketcan_error_is_hard(SOCKETCAN_ERR_FLAG | SOCKETCAN_ERR_RESTARTED));
    EXPECT_FALSE(socketcan_error_is_hard(SOCKETCAN_ERR_BUSOFF))
        << "a data frame's ID";
}

TEST(SocketCanTest, TheClockTakesTheHostsTimeThenTheKernelsSpacing)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull; /* a wall clock, in ns */
    EXPECT_EQ(socketcan_clock_step(c, k0, 5000), 5000u);
    /* A batch read at one host time keeps the kernel's 1 ms spacing */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 1000000, 9000), 6000u);
    EXPECT_EQ(socketcan_clock_step(c, k0 + 2000000, 9000), 7000u);
    /* Read 2.5 s late after a stall: still the kernel's spacing */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 12000000, 2509000), 17000u);
}

/* The kernel's wall clock counts a sleep, and so does the host's
 * (ADR-0033 section 5): the two deltas agree, so the gap stands. A host clock
 * that stopped in the sleep would put the kernel's delta past the read
 * and lose the gap to its own */
TEST(SocketCanTest, ASleepKeepsItsGap)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull;
    EXPECT_EQ(socketcan_clock_step(c, k0, 5000), 5000u);
    /* 100 s asleep, the frame read 5 ms after it arrived */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 100000000000ull, 100010000),
              100005000u);
}

TEST(SocketCanTest, AWallClockStepGivesWayToTheHostsTime)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull;
    socketcan_clock_step(c, k0, 0);
    /* Backward: the host's 10 ms */
    EXPECT_EQ(socketcan_clock_step(c, k0 - 3000000000ull, 10000), 10000u);
    /* An hour forward: the host's 10 ms again */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 3600000000000ull, 20000), 20000u);
    /* And the kernel's spacing from the new reading on */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 3600002000000ull, 30000), 22000u);
    /* A frame with no kernel time takes the host's delta */
    EXPECT_EQ(socketcan_clock_step(c, 0, 40000), 32000u);
}

/* A Flow Control frame's own echo and the Consecutive Frames it brought
 * on, read after an error frame stamped later, across a 200 ms stall: a
 * wall-clock step's host delta would put them 200 ms late, past ISO-TP's
 * N_Cr (SocketCanRequestTest.AReplyOnTimeSurvivesAWorkerHeldUpMidMessage) */
TEST(SocketCanTest, AFrameReadOutOfOrderTakesTheNewestTime)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull;
    EXPECT_EQ(socketcan_clock_step(c, k0, 5000), 5000u);
    EXPECT_EQ(socketcan_clock_step(c, k0 + 30000000, 36000), 35000u);
    /* Stamped before the frame above, read after the stall */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 2000000, 236000), 35000u);
    EXPECT_EQ(socketcan_clock_step(c, k0 + 12000000, 236010), 35000u);
    /* The spacing runs from the newest stamp, not the one read last */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 40000000, 236020), 45000u);
    EXPECT_EQ(c.host_steps, 0u);
    /* Further back than any reordering: a step, the host's delta */
    EXPECT_EQ(socketcan_clock_step(c, k0 - 200000000, 236030), 45010u);
    EXPECT_EQ(c.host_steps, 1u);
}

TEST(SocketCanTest, TheClockNeverDecreases)
{
    socketcan_clock c;
    uint64_t last = 0;
    const uint64_t steps[][2] = {{1000, 100}, {500, 200},
                                 {0, 300},    {2000, 50},
                                 {2000, 60},  {9000000000000ull, 70},
                                 {3000, 400}};
    for (const auto& s : steps) {
        const uint64_t t = socketcan_clock_step(c, s[0], s[1]);
        EXPECT_GE(t, last);
        last = t;
    }
}

TEST(SocketCanTest, TheUriIsCheckedBeforeAnyInterface)
{
#if ECUIO_HAVE_SOCKETCAN
    Created c = create("socketcan://");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("interface's name"), std::string::npos) << c.detail;
    EXPECT_EQ(create("socketcan://can 0").status, ECUIO_ERR_BAD_URI);
    c = create("socketcan://vcan0?baud=125000");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("500000 or 250000"), std::string::npos) << c.detail;
    c = create("socketcan://vcan0?filter=0x7E8,");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("?filter="), std::string::npos) << c.detail;
    c = create("socketcan://vcan0?poll=0x0C");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("?poll= belongs to a profile that asks an ECU"),
              std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?gap=5");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("?gap= belongs to a profile that asks an ECU"),
              std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?packframes=1");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(
        c.detail.find("?packframes= belongs to a profile that asks an ECU"),
        std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2&packframes=16");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("a frame count from 1 to 15"), std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2&gap=1001");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("a time in ms from 0 to 1000"), std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2&poll=0x1G");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("a PID from 0x00 to 0xFF"), std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2&filter=0x7E8,0x7E9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI)
        << "a profile that requests takes one receive ID";
    c = create("socketcan://lo?profile=obd2");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("lo isn't a CAN interface"), std::string::npos)
        << c.detail;
    c = create("socketcan://lo?profile=uds-generic&did=7E0:F190");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED) << c.detail;
#else
    const Created c = create("socketcan://can0");
    EXPECT_EQ(c.status, ECUIO_ERR_NO_TRANSPORT);
    EXPECT_NE(c.detail.find("needs Linux"), std::string::npos) << c.detail;
#endif
}

#if ECUIO_HAVE_SOCKETCAN
TEST(SocketCanTest, AnInterfaceThatIsntCanIsRefused)
{
    Created c = create("socketcan://nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no network interface is named nosuchcan9"),
              std::string::npos)
        << c.detail;
    c = create("socketcan://lo"); /* every Linux machine has the loopback */
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("lo isn't a CAN interface"), std::string::npos)
        << c.detail;
}
#endif

/* ---- The simulated ECU's contract (ADR-0046) ---- */

namespace {

struct SimCreated {
    ecuio_status_t status;
    std::string detail;
};

SimCreated sim_create(const char* uri, const char* ifname)
{
    ecuio_sim_t* sim = reinterpret_cast<ecuio_sim_t*>(1);
    SimCreated c{ecuio_sim_create(uri, ifname, &sim),
                 ecuio_last_error_detail()};
    EXPECT_EQ(sim, nullptr) << uri;
    return c;
}

} // namespace

TEST(SimTest, TheArgumentsAndTheUriAreCheckedFirst)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create(nullptr, "vcan0", &sim),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create("mock://sim", nullptr, &sim),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create("mock://sim", "vcan0", nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_sim_stats_t stats;
    EXPECT_EQ(ecuio_sim_get_stats(nullptr, &stats), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_sim_destroy(nullptr);
    // the URI's refusals, on every platform, before any interface
    SimCreated c = sim_create("mock://sim?did=1E1C", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("no query key 'did'"), std::string::npos)
        << c.detail;
    c = sim_create("socketcan://vcan0", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    c = sim_create("mock://sim?faults=silent:0", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("faults:"), std::string::npos) << c.detail;
    c = sim_create("replay://recordings/run", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("absolute"), std::string::npos) << c.detail;
    c = sim_create("mock://sim?loop=1", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
#if !ECUIO_HAVE_SOCKETCAN
    c = sim_create("mock://sim", "vcan0");
    EXPECT_EQ(c.status, ECUIO_ERR_NO_TRANSPORT);
    EXPECT_NE(c.detail.find("needs Linux"), std::string::npos) << c.detail;
#endif
}

// profile=: looked up after the URI and before the platform and the
// interface, on every platform: a name not registered, then a set the
// simulated ECU can't stand in for (a 29-bit module); then a set it
// can, which goes on to the interface. ecuio_sim_set_value's NULL
TEST(SimTest, TheProfileIsLookedUpBeforeTheInterface)
{
    SimCreated c = sim_create("mock://sim?profile=sim-test-none", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_NE(c.detail.find("sim-test-none"), std::string::npos) << c.detail;

    /* The registry keeps the pointers: static storage */
    static ecuio_signal_def_t wide_def{};
    wide_def.id = 1;
    wide_def.message_id = 0x18DAF110u;
    wide_def.mux_length = 3;
    wide_def.mux_key = 0x621234u;
    wide_def.kind = ECUIO_VALUE_UINT;
    wide_def.byte_order = ECUIO_BYTE_ORDER_BE;
    wide_def.start_byte = 3;
    wide_def.bit_length = 8;
    wide_def.scale = 1.0;
    std::snprintf(wide_def.name, sizeof wide_def.name, "Wide");
    static const ecuio_channel_decl_t wide_channel = {
        0x18DAF110u, 0x18DA10F1u, 0, 0, {}};
    static ecuio_signal_set_t wide{};
    wide.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    wide.count = 1;
    wide.defs = &wide_def;
    wide.channel_count = 1;
    wide.channels = &wide_channel;
    ASSERT_EQ(ecuio_profile_register("sim-test-wide", &wide, nullptr),
              ECUIO_OK);
    c = sim_create("mock://sim?profile=sim-test-wide", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(c.detail.find("11-bit"), std::string::npos) << c.detail;
    EXPECT_EQ(ecuio_profile_unregister("sim-test-wide"), ECUIO_OK);

    c = sim_create("mock://sim?profile=obd2", "nosuchcan9");
#if ECUIO_HAVE_SOCKETCAN
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED) << "on to the interface";
#else
    EXPECT_EQ(c.status, ECUIO_ERR_NO_TRANSPORT);
#endif
    EXPECT_EQ(ecuio_sim_set_value(nullptr, 1, 0.0), ECUIO_ERR_NULL_ARGUMENT);
}

// The options: NULLs first, then a bit that isn't one, before the URI
TEST(SimTest, AnOptionThatIsntOneIsRefusedBeforeTheUri)
{
    ecuio_sim_t* sim = reinterpret_cast<ecuio_sim_t*>(1);
    EXPECT_EQ(
        ecuio_sim_create_with(nullptr, "vcan0", ECUIO_SIM_ALLOW_DESK_BUS, &sim),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_with("mock://sim", "vcan0", 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_with("socketcan://vcan0", "nosuchcan9",
                                    ECUIO_SIM_ALLOW_DESK_BUS << 1, &sim),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sim, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("0x2"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_sim_create_with("socketcan://vcan0", "nosuchcan9",
                                    ECUIO_SIM_ALLOW_DESK_BUS, &sim),
              ECUIO_ERR_BAD_URI)
        << "the option itself is taken";
}

/* vcan always, once up; a real controller only on a desk bus said by the
 * caller, and then only up, able to transmit and error-active: any rate,
 * no one-shot mode needed, restarting itself or not */
TEST(SimTest, ARealControllerAnswersOnlyOnADeskBusReadyToSend)
{
    using V = socketcan_sim_verdict;
    LinkSpec vcan;
    vcan.name = "vcan0";
    vcan.kind = "vcan";
    EXPECT_EQ(socketcan_judge_sim(parsed(vcan), false), V::ok);
    EXPECT_EQ(socketcan_judge_sim(parsed(vcan), true), V::ok);
    vcan.up = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(vcan), false), V::down);

    LinkSpec eth;
    eth.name = "eth0";
    eth.kind = nullptr;
    EXPECT_EQ(socketcan_judge_sim(parsed(eth), true), V::not_can);

    LinkSpec desk = usb2can(); /* normal mode, no one-shot offered */
    EXPECT_EQ(socketcan_judge_sim(parsed(desk), false), V::real_controller);
    EXPECT_EQ(socketcan_judge_sim(parsed(desk), true), V::ok);
    desk.rate = 250000;
    desk.restart_ms = 100;
    EXPECT_EQ(socketcan_judge_sim(parsed(desk), true), V::ok)
        << "the rate it is up at; a restart does no harm on a desk";

    LinkSpec down = usb2can();
    down.up = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(down), false), V::real_controller)
        << "no desk bus said: refused whatever its state";
    EXPECT_EQ(socketcan_judge_sim(parsed(down), true), V::down);

    LinkSpec listening = usb2can();
    listening.modes = SOCKETCAN_CTRLMODE_LISTENONLY;
    EXPECT_EQ(socketcan_judge_sim(parsed(listening), true), V::listen_only);
    LinkSpec unsaid = usb2can();
    unsaid.ctrlmode = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(unsaid), true), V::listen_only)
        << "no mode reported: not known to transmit";

    LinkSpec passive = usb2can();
    passive.can_state = 2; /* CAN_STATE_ERROR_PASSIVE */
    EXPECT_EQ(socketcan_judge_sim(parsed(passive), true), V::not_error_active);
    LinkSpec stateless = usb2can();
    stateless.state = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(stateless), true),
              V::not_error_active);
}

// Each refusal names the interface and the ip link commands that change it
TEST(SimTest, ADeskBusRefusalSaysWhatToChange)
{
    using V = socketcan_sim_verdict;
    char out[512];
    const auto refusal = [&](const LinkSpec& s, V v) {
        socketcan_sim_refusal(parsed(s), v, out, sizeof out);
        return std::string(out);
    };
    LinkSpec eth;
    eth.name = "eth0";
    eth.kind = nullptr;
    EXPECT_EQ(refusal(eth, V::not_can),
              "eth0 is a non-CAN link, not vcan: the simulated ECU answers "
              "only on a virtual bus, never for a car's own ECU");
    const LinkSpec desk = usb2can();
    std::string r = refusal(desk, V::real_controller);
    EXPECT_NE(r.find("can0 is a CAN controller, not vcan"), std::string::npos)
        << r;
    EXPECT_NE(r.find("a desk bus with no car"), std::string::npos) << r;
    EXPECT_EQ(refusal(desk, V::down),
              "can0 is down: sudo ip link set can0 up type can bitrate "
              "500000 listen-only off");
    LinkSpec vcan;
    vcan.name = "vcan0";
    vcan.kind = "vcan";
    EXPECT_EQ(refusal(vcan, V::down),
              "vcan0 is down (sudo ip link set vcan0 up)");
    EXPECT_EQ(refusal(desk, V::listen_only),
              "can0 is listen-only, so the simulated ECU couldn't answer: "
              "sudo ip link set can0 down, then sudo ip link set can0 up "
              "type can bitrate 500000 listen-only off");
    LinkSpec rateless = usb2can();
    rateless.bitrate = false;
    r = refusal(rateless, V::listen_only);
    EXPECT_NE(r.find("bitrate <the bus's rate> listen-only off"),
              std::string::npos)
        << r;
    LinkSpec passive = usb2can();
    passive.can_state = 2;
    r = refusal(passive, V::not_error_active);
    EXPECT_NE(r.find("can0 is error-passive, so the bus already counts "
                     "errors"),
              std::string::npos)
        << r;
    LinkSpec stateless = usb2can();
    stateless.state = false;
    r = refusal(stateless, V::not_error_active);
    EXPECT_NE(r.find("can0 reports no controller state"), std::string::npos)
        << r;
    EXPECT_EQ(refusal(desk, V::ok), "");
}

#if ECUIO_HAVE_SOCKETCAN
// Only a virtual bus: never a car's, whatever its state
TEST(SimTest, OnlyAVcanInterfaceIsTaken)
{
    SimCreated c = sim_create("mock://sim", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no interface nosuchcan9"), std::string::npos)
        << c.detail;
    c = sim_create("mock://sim", "lo"); /* every Linux machine has one */
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("not vcan"), std::string::npos) << c.detail;
    c = sim_create("mock://sim", "bad name!");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    // A desk bus said changes nothing for a link that isn't CAN
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_with("mock://sim", "lo",
                                    ECUIO_SIM_ALLOW_DESK_BUS, &sim),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(sim, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("not vcan"),
              std::string::npos);
}

// A recording is read before the interface is judged: its own refusal
// first, then the interface's
TEST(SimTest, ARecordingIsReadBeforeTheInterface)
{
    SimCreated c = sim_create("replay:///no/such/recording", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("couldn't open"), std::string::npos) << c.detail;
    const std::string fixture = std::string("replay://") +
                                ECUIO_REPLAY_FIXTURES +
                                "/2026-10-25-engine-start";
    c = sim_create(fixture.c_str(), "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no interface nosuchcan9"), std::string::npos)
        << c.detail;
    c = sim_create((fixture + "?loop=1").c_str(), "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED) << "it spans time";
    // The URI's stretch reaches the loader: one past the recording's end
    // is the recording's refusal, one inside it the interface's
    c = sim_create((fixture + "?from=1000").c_str(), "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no frames from 1000 s on"), std::string::npos)
        << c.detail;
    c = sim_create((fixture + "?from=10&to=40").c_str(), "nosuchcan9");
    EXPECT_NE(c.detail.find("no interface nosuchcan9"), std::string::npos)
        << c.detail;
}
#endif
