// File/layer: tests/test_vcan_requests_simbus.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SimBusTest (test_vcan)

#include "test_vcan_requests_support.h"

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

/* ADR-0060 over socketcan://: the engine ECU enters the extended session
 * and answers D200, which it keeps to it; the second module has none,
 * refuses 10 03 with 12 and is read in the default session; the close
 * returns only the engine ECU to the default session */
TEST_F(SimBusTest, AnExtendedSessionScanReadsEachModuleInItsOwnSession)
{
    Sim sim("mock://sim?modules=2");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    vcan_bus::Fd raw = vcan_bus::open_raw();
    ASSERT_GE(raw.get(), 0);
    Values values;
    Session s("?profile=uds-generic&did=D200:u16,7E9:D100:u8&session=03"
              "&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    values.listen(s.get());
    const uint16_t ext = ECUIO_UDS_GENERIC_SIG_ID_BASE;
    const uint16_t fluid = ext + 1;
    ASSERT_TRUE(
        wait_for([&] { return values.has(ext) && values.has(fluid); }, 5000ms))
        << "requests " << sim.stats().requests;
    EXPECT_EQ(values.number_of(fluid), 130.0) << "D100's byte, 0x82";
    ecuio_channel_support_t engine{}, module{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &engine) ==
                       ECUIO_OK &&
                   ecuio_session_get_channel_support(s.get(), 1, &module) ==
                       ECUIO_OK &&
                   engine.session_state == ECUIO_SESSION_STATE_ENTERED &&
                   module.session_state == ECUIO_SESSION_STATE_REFUSED;
        },
        5000ms))
        << "engine " << int(engine.session_state) << ", module "
        << int(module.session_state);
    EXPECT_EQ(engine.session, 0x03);
    EXPECT_NE(engine.flags & ECUIO_SUPPORT_EXTENDED, 0u);
    EXPECT_EQ(module.session, 0x01);
    EXPECT_EQ(module.session_nrc, 0x12);
    EXPECT_EQ(module.flags & ECUIO_SUPPORT_EXTENDED, 0u);
    /* the close: 02 10 01 on 0x7E0 only; what the session sent before it
     * is read off first, so the socket's queue has room */
    can_frame f{};
    while (vcan_bus::read_for(raw.get(), &f, sizeof f, 0) > 0) {
    }
    ecuio_session_close(s.get());
    bool engine_left = false;
    bool module_left = false;
    while (vcan_bus::read_for(raw.get(), &f, sizeof f, 300) > 0) {
        const bool leave = f.can_dlc >= 3 && f.data[0] == 0x02 &&
                           f.data[1] == 0x10 && f.data[2] == 0x01;
        engine_left = engine_left || (leave && f.can_id == 0x7E0u);
        module_left = module_left || (leave && f.can_id == 0x7E1u);
    }
    EXPECT_TRUE(engine_left);
    EXPECT_FALSE(module_left) << "the module never entered it";
}

/* Without modules=2 nothing answers on 0x7E9: the default stays the
 * engine ECU alone, whose one reply is its answer to the start's probe
 * (it joins the table, asked nothing of its own here) */
TEST_F(SimBusTest, TheSecondModuleIsOptIn)
{
    Sim sim("mock://sim");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Values values;
    Session s("?profile=uds-generic&did=7E9:D100:u8&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    values.listen(s.get());
    std::this_thread::sleep_for(1500ms);
    EXPECT_FALSE(values.has(ECUIO_UDS_GENERIC_SIG_ID_BASE));
    EXPECT_EQ(sim.stats().replies, 1u);
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
    static const ecuio_channel_decl_t channels[] = {{0x7E8, 0x7E0, 0, 0},
                                                    {0x7EA, 0x7E2, 0, 0}};
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

/* ADR-0065: a set of six modules is six nodes on the bus and six channels
 * of the session, which names the two past the info's four and reads
 * each module's own value */
TEST_F(SimBusTest, ASessionReadsSixModulesEachItsOwnValue)
{
    /* The registry keeps the pointers: static storage */
    static ecuio_signal_def_t defs[6];
    static ecuio_channel_decl_t channels[6];
    for (uint32_t m = 0; m < 6; ++m) {
        char name[32];
        std::snprintf(name, sizeof name, "Module %u", m);
        defs[m] = vcan_did(static_cast<uint16_t>(1 + m), 0x7E8u + m,
                           static_cast<uint16_t>(0xD300 + m), ECUIO_VALUE_UINT,
                           ECUIO_BYTE_ORDER_BE, 3, 8, 1.0, 0.0, name);
        channels[m] = ecuio_channel_decl_t{};
        channels[m].rx_id = 0x7E8u + m;
        channels[m].tx_id = 0x7E0u + m;
    }
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 6;
    set.defs = defs;
    set.channel_count = 6;
    set.channels = channels;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("sim-six", &set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    {
        Sim sim("mock://sim?profile=sim-six");
        ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
        for (uint16_t m = 0; m < 6; ++m) {
            ASSERT_EQ(ecuio_sim_set_value(sim.get(),
                                          static_cast<uint16_t>(1 + m),
                                          10.0 + m),
                      ECUIO_OK)
                << m;
        }
        Values values;
        Session s("?profile=sim-six");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
        EXPECT_EQ(info.module_count, 6u);
        EXPECT_EQ(info.channel_count,
                  uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS});
        ecuio_channel_decl_t sixth{};
        ASSERT_EQ(ecuio_session_get_module(s.get(), 5, &sixth), ECUIO_OK);
        EXPECT_EQ(sixth.rx_id, 0x7EDu);
        EXPECT_EQ(sixth.tx_id, 0x7E5u);
        values.listen(s.get());
        ASSERT_TRUE(wait_for(
            [&] {
                for (uint16_t m = 0; m < 6; ++m) {
                    if (!values.has(static_cast<uint16_t>(1 + m))) {
                        return false;
                    }
                }
                return true;
            },
            10000ms))
            << "requests " << sim.stats().requests << ", replies "
            << sim.stats().replies;
        for (uint16_t m = 0; m < 6; ++m) {
            EXPECT_EQ(values.number_of(static_cast<uint16_t>(1 + m)), 10.0 + m)
                << m;
        }
        EXPECT_EQ(sim.stats().flags, 0u);
        int32_t e = 0;
        uint32_t n = 0;
        ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
        EXPECT_EQ(e, ECUIO_OK);
    }
    EXPECT_EQ(ecuio_profile_unregister("sim-six"), ECUIO_OK);
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

/* A flood on the bus (another program on the interface) holds the sim's
 * worker no longer than a pass: a destroy under it ends within a bound,
 * not when the flood does */
TEST_F(SimBusTest, ADestroyUnderAFloodEndsWithinABound)
{
    ecuio_sim_t* sim = nullptr;
    ASSERT_EQ(ecuio_sim_create("mock://sim", vcan_bus::interface_name(), &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    std::atomic<bool> flooding{true};
    std::vector<std::thread> floods;
    for (int k = 0; k < 4; ++k) {
        floods.emplace_back([&flooding] {
            vcan_bus::Fd bus = vcan_bus::open_raw();
            struct can_frame f;
            std::memset(&f, 0, sizeof f);
            f.can_id = 0x7DF; /* every node feeds it */
            f.can_dlc = 8;
            f.data[0] = 0x30; /* a Flow Control nothing waits for */
            const auto end = std::chrono::steady_clock::now() + 4s;
            while (flooding.load() && std::chrono::steady_clock::now() < end) {
                // a full queue drops the frame, which is the flood's point
                if (::write(bus.get(), &f, sizeof f) < 0) {
                    std::this_thread::yield();
                }
            }
        });
    }
    std::this_thread::sleep_for(300ms);
    const auto start = std::chrono::steady_clock::now();
    ecuio_sim_destroy(sim);
    const auto took = std::chrono::steady_clock::now() - start;
    flooding = false;
    for (std::thread& t : floods) {
        t.join();
    }
    EXPECT_LT(took, 1s) << "the destroy waited for the flood";
}
