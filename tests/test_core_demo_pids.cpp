// File/layer: tests/test_core_demo_pids.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

/* The values a callback saw of one signal, in order */
struct OneSignal {
    uint16_t id = 0;
    std::mutex m;
    std::vector<double> values;
};

void see_one(const ecuio_value_batch_t* batch, void* user)
{
    auto* one = static_cast<OneSignal*>(user);
    std::lock_guard<std::mutex> lock(one->m);
    for (uint32_t i = 0; i < batch->count; ++i) {
        if (batch->values[i].signal_id == one->id) {
            one->values.push_back(batch->values[i].value);
        }
    }
}

/* A clear on the demo car zeroes what it counts since a clear in its
 * Service 01 values, as its diagnostics report does: the distance with
 * the MIL on reads the fakes' 42 km before and 0 after */
TEST(CoreTest, TheDemoCarsCountsSinceAClearFollowTheClear)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo?profile=obd2", &s), ECUIO_OK);
    OneSignal mil;
    mil.id = static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(0x21, 0));
    ASSERT_EQ(ecuio_register_values_callback(s, see_one, &mil), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ASSERT_EQ(ecuio_session_clear_diagnostics(s, 0, d.generation), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(mil.m);
            return !mil.values.empty() && mil.values.back() == 0.0;
        },
        2000));
    ecuio_session_close(s);
    std::lock_guard<std::mutex> lock(mil.m);
    ASSERT_FALSE(mil.values.empty());
    EXPECT_EQ(mil.values.front(), 42.0) << "the first tick's, before the clear";
    EXPECT_EQ(mil.values.back(), 0.0);
    ecuio_session_destroy(s);
}

/* The demo car's ECU after a number of its drive cycle's ticks, and the
 * obd2 value each of its replies to the set's PIDs decodes to, by def id
 * (mock_pids.h, no session and no clock) */
struct DemoReplies {
    ecuio_sim_ecu ecu;
    ecuio_mock_pids pids;
    std::map<uint16_t, double> value;
    uint32_t replies = 0;

    explicit DemoReplies(int ticks, bool cleared = false)
    {
        for (int i = 0; i < ticks; ++i) {
            ecuio_sim_tick(ecu);
        }
        ecu.cleared = cleared;
        const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
        ecuio_mock_pids_of(set, ecu, &pids);
        auto frame = std::make_unique<ecuio_frame_t>();
        ecuio_mock_pid_replies(
            ecu, pids, 1234, frame.get(), [&](const ecuio_frame_t* f) {
                ++replies;
                EXPECT_EQ(f->arbitration_id, 0x7E8u);
                EXPECT_EQ(f->flags, ECUIO_FRAME_FLAG_REASSEMBLED);
                EXPECT_EQ(f->timestamp_us, 1234u);
                EXPECT_EQ(f->data[0], 0x41);
                for (uint32_t i = 0; i < set->count; ++i) {
                    const ecuio_signal_def_t& d = set->defs[i];
                    double v = 0.0;
                    if (ecuio_def_applies_to_frame(&d, f) &&
                        ecuio_decode_signal(&d, f->data, f->data_length, &v) ==
                            ECUIO_DECODE_OK) {
                        value[d.id] = v;
                    }
                }
            });
    }

    const ecuio_mock::DriveCycle& car() const { return ecu.car; }

    double at(uint16_t id) const
    {
        const auto it = value.find(id);
        EXPECT_NE(it, value.end()) << "no value for " << id;
        return it == value.end() ? std::nan("") : it->second;
    }
};

/* 60 s in: off the cold start, pulling through the gears */
constexpr int kDrivingTicks = 1200;

uint16_t pid_id(uint8_t pid, uint8_t field = 0)
{
    return static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(pid, field));
}

/* The set's PIDs the ECU answers are sent once each a tick, in set
 * order, never a bitmap, each a reply of its own */
TEST(MockPidsTest, EachAnsweredPidOfTheSetIsSentOnceATick)
{
    const DemoReplies d(kDrivingTicks);
    std::vector<uint8_t> want;
    for (const uint8_t pid : kDemoPids) {
        if (pid != 0x01 && pid != 0x41) { /* bit fields: no obd2 def */
            want.push_back(pid);
        }
    }
    std::vector<uint8_t> got(d.pids.pid, d.pids.pid + d.pids.count);
    std::vector<uint8_t> sorted = got;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, want);
    EXPECT_EQ(d.replies, d.pids.count);
    /* set order: the ten first defs' PIDs lead (04 05 0C 0D 0F 11 ...) */
    ASSERT_GE(got.size(), 2u);
    EXPECT_EQ(got[0], 0x04);
    EXPECT_EQ(got[1], 0x05);
}

/* Engine: load, coolant, rpm, run time and oil, the drive cycle's */
TEST(MockPidsTest, TheEngineGroupIsTheDriveCycles)
{
    const DemoReplies d(kDrivingTicks);
    const ecuio_mock::DriveCycle& car = d.car();
    ASSERT_TRUE(car.running);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_ENGINE_LOAD_ID), car.load_pct, 0.2);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_COOLANT_TEMP_ID), car.coolant_c, 0.5);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_RPM_ID), car.rpm, 0.25);
    EXPECT_NEAR(d.at(pid_id(0x1F)), d.ecu.engine_s, 1.0);
    EXPECT_GT(d.at(pid_id(0x1F)), 30.0);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_OIL_TEMP_ID), car.oil_c, 0.5);
}

/* Air: MAP, baro, intake temperature, airflow, throttle and ambient; and
 * obd2's Boost from MAP and baro */
TEST(MockPidsTest, TheAirGroupIsTheDriveCycles)
{
    const DemoReplies d(kDrivingTicks);
    const ecuio_mock::DriveCycle& car = d.car();
    EXPECT_NEAR(d.at(pid_id(0x0B)), car.map_kpa, 0.5);
    EXPECT_NEAR(d.at(pid_id(0x33)), car.baro_kpa, 0.5);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_IAT_ID), car.iat_c, 0.5);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_MAF_ID), car.maf_gs, 0.005);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_THROTTLE_ID), car.throttle_pct, 0.2);
    EXPECT_NEAR(d.at(pid_id(0x46)), ecuio_mock::kColdStartC, 0.5);
    /* the computed Boost's inputs both come, so it computes */
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t& boost = set->defs[set->count - 1];
    ASSERT_EQ(boost.id, ECUIO_OBD2_SIG_BOOST_ID);
    const ecuio_computed_t& program = set->computed[boost.table_first];
    for (uint32_t k = 0; k < program.input_count; ++k) {
        EXPECT_EQ(d.value.count(program.input_ids[k]), 1u) << k;
    }
}

/* Vehicle: the road speed */
TEST(MockPidsTest, TheVehicleGroupIsTheDriveCycles)
{
    const DemoReplies d(kDrivingTicks);
    ASSERT_GT(d.car().speed_kmh, 1.0f);
    EXPECT_NEAR(d.at(ECUIO_OBD2_SIG_SPEED_ID), d.car().speed_kmh, 0.5);
}

/* Oxygen sensors: lambda, and the steady voltage the model doesn't make */
TEST(MockPidsTest, TheOxygenGroupIsTheDriveCycles)
{
    const DemoReplies d(kDrivingTicks);
    EXPECT_NEAR(d.at(pid_id(0x24)), d.car().lambda, 0.0001);
    EXPECT_NEAR(d.at(pid_id(0x24, 1)), 2.5, 0.001);
}

/* Fuel: the lambda the ECU commands, which the sensor follows: read in
 * closed loop, where the command is 1.00 and the sensor wanders off it */
TEST(MockPidsTest, TheFuelGroupIsTheDriveCycles)
{
    ecuio_sim_ecu probe;
    int ticks = 0;
    for (; ticks < 6000; ++ticks) {
        ecuio_sim_tick(probe);
        if (ticks > kDrivingTicks && probe.car.running &&
            probe.car.commanded_lambda == 1.0f &&
            std::fabs(probe.car.lambda - 1.0f) > 0.003f) {
            ++ticks;
            break;
        }
    }
    ASSERT_LT(ticks, 6000) << "closed loop, the sensor off the command";
    const DemoReplies d(ticks);
    EXPECT_NEAR(d.at(pid_id(0x44)), d.car().commanded_lambda, 0.0001);
    EXPECT_GT(std::fabs(d.at(pid_id(0x44)) - d.car().lambda), 0.002)
        << "the command, not the sensor";
}

/* Electrical: the battery charging while the engine runs, its own with
 * the key on and the engine stopped */
TEST(MockPidsTest, TheElectricalGroupIsTheDriveCycles)
{
    const DemoReplies running(kDrivingTicks);
    EXPECT_NEAR(running.at(ECUIO_OBD2_SIG_BATTERY_V_ID),
                running.car().battery_v, 0.001);
    EXPECT_GT(running.at(ECUIO_OBD2_SIG_BATTERY_V_ID), 13.5);
    const DemoReplies key_on(1);
    ASSERT_FALSE(key_on.car().running);
    EXPECT_NEAR(key_on.at(ECUIO_OBD2_SIG_BATTERY_V_ID), key_on.car().battery_v,
                0.001);
    EXPECT_LT(key_on.at(ECUIO_OBD2_SIG_BATTERY_V_ID), 13.0);
}

/* Diagnostics: the counts since a clear, the fakes' before one and 0
 * after, as the diagnostics report says */
TEST(MockPidsTest, TheDiagnosticsGroupFollowsAClear)
{
    const DemoReplies before(kDrivingTicks);
    EXPECT_EQ(before.at(pid_id(0x21)), 42.0);
    EXPECT_EQ(before.at(pid_id(0x30)), 20.0);
    EXPECT_EQ(before.at(pid_id(0x31)), 1234.0);
    EXPECT_EQ(before.at(pid_id(0x4D)), 60.0);
    EXPECT_EQ(before.at(pid_id(0x4E)), 3000.0);
    const DemoReplies after(kDrivingTicks, true);
    for (const uint8_t pid : {uint8_t{0x21}, uint8_t{0x30}, uint8_t{0x31},
                              uint8_t{0x4D}, uint8_t{0x4E}}) {
        EXPECT_EQ(after.at(pid_id(pid)), 0.0) << std::hex << int(pid);
    }
}

/* A profile with no Service 01 signal learns nothing and lists none:
 * the mock's own, raw, and uds-generic, whose DIDs the mock answers */
TEST(CoreTest, TheMockDisclaimsNothingUnderAProfileWithNoPids)
{
    for (const char* uri :
         {"mock://demo", "mock://demo?profile=raw",
          "mock://demo?profile=uds-generic&did=1E1C,F190:text"}) {
        SCOPED_TRACE(uri);
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_channel_support_t s{};
        ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
        EXPECT_EQ(s.generation, 0u);
        EXPECT_EQ(s.flags, 0u);
        EXPECT_EQ(s.requests, 0u);
        EXPECT_TRUE(unsupported_ids(session).empty());
        ecuio_session_destroy(session);
    }
}

/* A computed signal with an absent input is absent with it, and leaves
 * the list when the input arrives: three replies to 01 0B with no data
 * byte make obd2's MAP absent, and Boost, which reads it */
TEST(CoreTest, AComputedSignalWithAnAbsentInputIsAbsentToo)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    uint16_t map = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        if ((set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) == 0 &&
            set->defs[i].mux_key == 0x410Bu) {
            map = set->defs[i].id;
        }
    }
    ASSERT_NE(map, 0u);
    const ecuio_frame_t none = value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0B});
    const ecuio_frame_t one =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0B, 0x64});
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://a?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    uint16_t ids[4] = {};
    uint32_t count = 99;
    for (uint32_t k = 1; k <= ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &none);
        ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
                  ECUIO_OK);
        EXPECT_EQ(count, k < ECUIO_ABSENT_AFTER_FRAMES ? 0u : 2u)
            << "after " << k << " short replies";
    }
    EXPECT_EQ(ids[0], map);
    EXPECT_EQ(ids[1], ECUIO_OBD2_SIG_BOOST_ID) << "in the set's order";
    ecuio_session_dispatch_callbacks(session, &one);
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "a reply that carries MAP clears both";
    ecuio_session_destroy(session);
}


/* ADR-0039 Appendix A: a Service 01 reply carrying several PIDs decodes
 * record by record, each def once, in the order the ECU sent them; one
 * that won't split decodes its first record and is counted; a frame on
 * an ID no Service 01 def listens on is never split */
TEST(CoreTest, APackedReplyDecodesEachRecord)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://p?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    EXPECT_EQ(unsplit(session), 0u);

    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0D, 0x32, 0x0C, 0x1A, 0xF8, 0x05, 0x5A});
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.count(), 1u);
    EXPECT_EQ(id_values(cap.batches[0]),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_SPEED_ID, 50.0},
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0},
                  {ECUIO_OBD2_SIG_COOLANT_TEMP_ID, 50.0}}));
    EXPECT_EQ(cap.batches[0].arbitration_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(unsplit(session), 0u);

    const ecuio_frame_t stuck = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x1A, 0xF8, 0xB0, 0x01});
    ecuio_session_dispatch_callbacks(session, &stuck);
    ASSERT_EQ(cap.count(), 2u);
    EXPECT_EQ(id_values(cap.batches[1]),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0}}))
        << "its first record alone";
    EXPECT_EQ(unsplit(session), 1u);

    const ecuio_frame_t elsewhere =
        value_frame(0x7E9, {0x41, 0x0D, 0x32, 0x0C, 0x1A, 0xF8});
    ecuio_session_dispatch_callbacks(session, &elsewhere);
    EXPECT_EQ(cap.count(), 2u) << "no def listens on 0x7E9";
    EXPECT_EQ(unsplit(session), 1u);

    ecuio_session_close(session);
    EXPECT_EQ(unsplit(session), 1u) << "readable after close";
    uint64_t n = 0;
    EXPECT_EQ(ecuio_session_get_unsplit_replies(nullptr, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unsplit_replies(session, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(session);
}

/* The test car's PID 55 carries bank 1's byte alone: its single replies
 * teach that length, so a packed reply holding it splits, and bank 3's
 * field, past every record, is still judged absent */
TEST(CoreTest, APackedReplySplitsByTheLengthTheEcuSends)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://p?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t alone =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x55, 0x80});
    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x1A, 0xF8, 0x55, 0x80, 0x0D, 0x32});
    ecuio_session_dispatch_callbacks(session, &alone);
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &packed);
    }
    ASSERT_EQ(cap.count(), 1u + ECUIO_ABSENT_AFTER_FRAMES);
    EXPECT_EQ(id_values(cap.batches.back()),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0},
                  {static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(0x55, 0)), 0.0},
                  {ECUIO_OBD2_SIG_SPEED_ID, 50.0}}));
    EXPECT_EQ(unsplit(session), 0u);
    uint16_t ids[4] = {};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(ids[0], ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ecuio_session_destroy(session);
}

/* A recording keeps a packed reply as the ECU sent it, so its replay
 * decodes it the same way, the earlier single reply's length included */
TEST(CoreTest, APackedReplyReplaysToItsRecords)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "10,1000000,0x7E8,0x0,41 55 80\n"
                      "600,1000500,0x7E8,0x0,41 0C 1A F8 55 80 0D 32\n");
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?profile=obd2&speed=max").c_str(),
                                   &session),
              ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ecuio_session_close(session);
    std::lock_guard<std::mutex> lock(cap.mutex);
    ASSERT_EQ(cap.batches.size(), 2u);
    EXPECT_EQ(id_values(cap.batches[1]),
              (std::vector<std::pair<uint16_t, double>>{
                  {ECUIO_OBD2_SIG_RPM_ID, 1726.0},
                  {static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(0x55, 0)), 0.0},
                  {ECUIO_OBD2_SIG_SPEED_ID, 50.0}}));
    EXPECT_EQ(unsplit(session), 0u);
    ecuio_session_destroy(session);
}

/* uds-generic's set holds a scanned range's DIDs from create, so a
 * reply for one decodes before the scan has found it (ADR-0024 section 1) */
TEST(CoreTest, ValuesOfAScannedDidComeFromTheSetBuiltAtCreate)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://u?profile=uds-generic&did=F180-F182",
                                   &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    uint16_t f181 = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].mux_key == 0x62F181u)
            f181 = set->defs[i].id;
    }
    ASSERT_NE(f181, 0) << "F181 is in the set";
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t reply =
        value_frame(0x7E8, {0x62, 0xF1, 0x81, 0x41, 0x42});
    ecuio_session_dispatch_callbacks(session, &reply);
    ASSERT_EQ(cap.batches.size(), 1u);
    ASSERT_EQ(cap.batches[0].values.size(), 1u);
    EXPECT_EQ(cap.batches[0].values[0].id, f181);
    EXPECT_EQ(cap.batches[0].values[0].kind, ECUIO_VALUE_HEX);
    EXPECT_EQ(cap.batches[0].values[0].text, "41 42");
    ecuio_session_destroy(session);
}

/* A packed Service 01 reply splits into a record for each PID, all in one
 * frame's text slots: a hex def on 41 1C and one on 41 51 both decode
 * from 41 1C 06 51 01, each with bytes of its own */
TEST(CoreTest, APackedReplyGivesEachRecordsTextASlotOfItsOwn)
{
    static ecuio_signal_def_t defs[2];
    defs[0] = text_value_def(1, ECUIO_VALUE_HEX, 2, 0x411C, 2, 1);
    defs[1] = text_value_def(2, ECUIO_VALUE_HEX, 2, 0x4151, 2, 1);
    for (ecuio_signal_def_t& d : defs) {
        d.message_id = ECUIO_OBD2_ECU_ARB_ID;
    }
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-packed-text", &set, &why), ECUIO_OK)
        << "code " << why.code << " at def " << why.index;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-packed-text", &session),
        ECUIO_OK);
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t packed =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x1C, 0x06, 0x51, 0x01});
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.batches.size(), 1u);
    const std::vector<SeenValue>& v = cap.batches[0].values;
    ASSERT_EQ(v.size(), 2u) << "a slot for each record's text";
    EXPECT_EQ(v[0].id, 1);
    EXPECT_EQ(v[0].text, "06");
    EXPECT_EQ(v[1].id, 2);
    EXPECT_EQ(v[1].text, "01");
    EXPECT_GE(std::abs(v[1].text_at - v[0].text_at),
              static_cast<std::ptrdiff_t>(ECUIO_DECODE_TEXT_MAX))
        << "the two records' texts share bytes";
    EXPECT_EQ(unsplit(session), 0u);
    ecuio_session_destroy(session);
}
