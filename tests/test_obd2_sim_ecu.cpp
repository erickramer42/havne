// File/layer: tests/test_obd2_sim_ecu.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

/* A list reply's DTCs, after checking its count */
std::vector<std::string> dtcs(const std::vector<uint8_t>& reply)
{
    std::vector<std::string> out;
    if (reply.size() < 2 || reply.size() != 2u + 2u * reply[1]) {
        ADD_FAILURE() << "a list reply of " << reply.size() << " bytes";
        return out;
    }
    for (size_t i = 2; i + 1 < reply.size(); i += 2) {
        out.push_back(dtc(reply[i], reply[i + 1]));
    }
    return out;
}

using Codes = std::vector<std::string>;

class FakeEcuTest : public ::testing::Test
{
protected:
    void SetUp() override { fake_ecu::reset_diagnostics(); }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

} // namespace

/* A module answers Service 0x22 from its own rows and refuses the rest;
 * an ECU the table doesn't name answers as the engine ECU; a malformed
 * spec leaves the table empty */
TEST_F(FakeEcuTest, AModuleAnswersItsOwnDidsAndRefusesTheRest)
{
    struct Empty {
        ~Empty() { fake_ecu::set_module_dids(nullptr); }
    } empty;
    const auto ask = [](uint32_t rx, Bytes req, long nrc = -1) {
        uint8_t out[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        if (!fake_ecu::answer_for(rx, req.data(), req.size(), nrc, out, &n)) {
            return Bytes{};
        }
        return Bytes(out, out + n);
    };
    ASSERT_TRUE(fake_ecu::set_module_dids(
        "7E9:D801=00 12; 7e9:d802=ABCDEF;18DAF1E9:0102=01"));
    EXPECT_EQ(fake_ecu::module_table().count.load(), 3u);
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x01}),
              (Bytes{0x62, 0xD8, 0x01, 0x00, 0x12}));
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x02}),
              (Bytes{0x62, 0xD8, 0x02, 0xAB, 0xCD, 0xEF}))
        << "lower case reads the same";
    EXPECT_EQ(ask(0x18DAF1E9, {0x22, 0x01, 0x02}),
              (Bytes{0x62, 0x01, 0x02, 0x01}))
        << "a 29-bit reply ID";
    EXPECT_EQ(ask(0x7E9, {0x22, 0x1E, 0x1C}), (Bytes{0x7F, 0x22, 0x31}))
        << "the engine ECU's DID isn't the module's";
    EXPECT_EQ(ask(0x7E9, {0x01, 0x00}), (Bytes{0x7F, 0x01, 0x11}))
        << "a module serves only 0x22";
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x03}, 0), Bytes{})
        << "silence asked for";
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x03}, 0x33), (Bytes{0x7F, 0x22, 0x33}));
    EXPECT_EQ(ask(0x7E8, {0x22, 0x1E, 0x1C}),
              (Bytes{0x62, 0x1E, 0x1C, 0x12, 0x34}))
        << "an ECU the table doesn't name answers as the engine ECU";
    EXPECT_EQ(ask(0x7E8, {0x22, 0xD8, 0x01}), (Bytes{0x7F, 0x22, 0x31}));

    for (const char* bad :
         {"7E9", "7E9:D801", "7E9:D801=", "7E9:D801=1", "7E9:D801=0G",
          ":D801=00", "7E9:=00", "7E9:1D8010=00", "123456789:D801=00",
          "7E9:D801=00112233445566778899AABBCCDDEEFF00"}) {
        SCOPED_TRACE(bad);
        EXPECT_FALSE(fake_ecu::set_module_dids(bad));
        EXPECT_EQ(fake_ecu::module_table().count.load(), 0u)
            << "a bad spec empties it";
    }
    std::string many;
    for (uint32_t i = 0; i <= fake_ecu::kModuleRowsMax; ++i) {
        char row[16];
        std::snprintf(row, sizeof row, "7E9:D%03X=01;", i);
        many += row;
    }
    EXPECT_FALSE(fake_ecu::set_module_dids(many.c_str()))
        << "kModuleRowsMax + 1 rows";
    ASSERT_TRUE(fake_ecu::set_module_dids(""));
    EXPECT_EQ(ask(0x7E9, {0x22, 0x1E, 0x1C}),
              (Bytes{0x62, 0x1E, 0x1C, 0x12, 0x34}))
        << "with the table empty every ECU is the engine ECU";
}

/* The lists cover all four categories; three stored DTCs make an
 * 8-byte reply, past a Single Frame */
TEST_F(FakeEcuTest, TheDtcListsAndTheMilBeforeAClear)
{
    const Bytes stored = ecu({0x03});
    ASSERT_EQ(stored.size(), 8u);
    EXPECT_EQ(stored[0], 0x43);
    EXPECT_EQ(dtcs(stored), (Codes{"P0301", "P0420", "U0100"}));
    const Bytes pending = ecu({0x07});
    ASSERT_FALSE(pending.empty());
    EXPECT_EQ(pending[0], 0x47);
    EXPECT_EQ(dtcs(pending), (Codes{"P0171", "C0035", "B1001"}));
    const Bytes permanent = ecu({0x0A});
    ASSERT_FALSE(permanent.empty());
    EXPECT_EQ(permanent[0], 0x4A);
    EXPECT_EQ(dtcs(permanent), (Codes{"P0420"}));

    EXPECT_EQ(ecu({0x01, 0x01}), (Bytes{0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}))
        << "MIL on, 3 DTCs; every monitor complete";
    EXPECT_EQ(ecu({0x01, 0x41}), (Bytes{0x41, 0x41, 0x00, 0x07, 0xE5, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x21}), (Bytes{0x41, 0x21, 0x00, 0x2A}));
    EXPECT_EQ(ecu({0x01, 0x30}), (Bytes{0x41, 0x30, 20}));
    EXPECT_EQ(ecu({0x01, 0x31}), (Bytes{0x41, 0x31, 0x04, 0xD2}));
    EXPECT_EQ(ecu({0x01, 0x4D}), (Bytes{0x41, 0x4D, 0x00, 0x3C}));
    EXPECT_EQ(ecu({0x01, 0x4E}), (Bytes{0x41, 0x4E, 0x0B, 0xB8}));
}

TEST_F(FakeEcuTest, TheFreezeFrameHoldsItsOwnValues)
{
    EXPECT_EQ(ecu({0x02, 0x00, 0x00}),
              (Bytes{0x42, 0x00, 0x00, 0x58, 0x18, 0x00, 0x00}))
        << "PIDs 02 04 05 0C 0D";
    const Bytes cause = ecu({0x02, 0x02, 0x00});
    ASSERT_EQ(cause.size(), 5u);
    EXPECT_EQ(dtc(cause[3], cause[4]), "P0301");
    EXPECT_EQ(ecu({0x02, 0x0C, 0x00}), (Bytes{0x42, 0x0C, 0x00, 0x2E, 0xE0}))
        << "3000 RPM";
    EXPECT_EQ(ecu({0x01, 0x0C}), (Bytes{0x41, 0x0C, 0x1F, 0x40}))
        << "the live value differs";
    EXPECT_EQ(ecu({0x02, 0x0C, 0x01}), (Bytes{0x7F, 0x02, 0x12}))
        << "only frame 0";
    EXPECT_EQ(ecu({0x02, 0x0F, 0x00}), (Bytes{0x7F, 0x02, 0x12}))
        << "not in the frame";
}

/* What J1979 lists a clear as resetting, reset; the permanent DTC
 * stays */
TEST_F(FakeEcuTest, AClearEmptiesTheListsAndResetsTheMonitors)
{
    EXPECT_EQ(ecu({0x04}), (Bytes{0x44}));
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
    EXPECT_EQ(ecu({0x03}), (Bytes{0x43, 0x00}));
    EXPECT_EQ(ecu({0x07}), (Bytes{0x47, 0x00}));
    EXPECT_EQ(dtcs(ecu({0x0A})), (Codes{"P0420"}));
    EXPECT_EQ(ecu({0x01, 0x01}), (Bytes{0x41, 0x01, 0x00, 0x07, 0xE5, 0xE5}))
        << "MIL off; C's monitors incomplete";
    EXPECT_EQ(ecu({0x01, 0x41}), (Bytes{0x41, 0x41, 0x00, 0x07, 0xE5, 0xE5}));
    EXPECT_EQ(ecu({0x01, 0x21}), (Bytes{0x41, 0x21, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x30}), (Bytes{0x41, 0x30, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x31}), (Bytes{0x41, 0x31, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x4D}), (Bytes{0x41, 0x4D, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x4E}), (Bytes{0x41, 0x4E, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x02, 0x00, 0x00}),
              (Bytes{0x42, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x02, 0x02, 0x00}), (Bytes{0x42, 0x02, 0x00, 0x00, 0x00}))
        << "J1979: 00 00 when no freeze frame is stored";
    EXPECT_EQ(ecu({0x02, 0x0C, 0x00}), (Bytes{0x7F, 0x02, 0x12}));
    EXPECT_EQ(ecu({0x01, 0x0C}), (Bytes{0x41, 0x0C, 0x1F, 0x40}))
        << "live data stays";
}

TEST_F(FakeEcuTest, ARefusedClearChangesNothing)
{
    fake_ecu::diagnostics().clear_nrc = 0x22; /* conditionsNotCorrect */
    EXPECT_EQ(ecu({0x04}), (Bytes{0x7F, 0x04, 0x22}));
    EXPECT_FALSE(fake_ecu::diagnostics().cleared.load());
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
    EXPECT_EQ(dtcs(ecu({0x03})).size(), 3u);
}

TEST_F(FakeEcuTest, OtherRequestShapesGetNoAnswer)
{
    EXPECT_TRUE(ecu({0x05}).empty()) << "Service 05 isn't on CAN";
    EXPECT_TRUE(ecu({0x03, 0x00}).empty());
    EXPECT_TRUE(ecu({0x04, 0x00}).empty());
    EXPECT_TRUE(ecu({0x02, 0x0C}).empty()) << "no frame byte";
    EXPECT_FALSE(fake_ecu::diagnostics().cleared.load());
}

/* ---------- the simulated ECU's answers (ADR-0046) ---------- */

namespace {

ecuio_sim_ecu sim_of(ecuio_sim_values values)
{
    ecuio_sim_ecu e;
    e.config.values = values;
    std::memcpy(e.config.vin, ECUIO_SIM_DEFAULT_VIN,
                sizeof ECUIO_SIM_DEFAULT_VIN);
    return e;
}

/* A Service 01 reply decoded by the obd2 def id */
double drive_value(const std::vector<uint8_t>& reply, uint16_t id)
{
    const ecuio_signal_def_t* d = find_def(ecuio_obd2_signal_set(), id);
    EXPECT_NE(d, nullptr);
    const ecuio_frame_t f =
        make_obd2_frame(reply.data(), static_cast<uint8_t>(reply.size()));
    double v = -1.0;
    EXPECT_TRUE(ecuio_def_applies_to_frame(d, &f)) << id;
    EXPECT_EQ(decode(d, f, &v), ECUIO_DECODE_OK) << id;
    return v;
}

} // namespace

// values=drive: each PID through the obd2 set's own def reads the drive
// cycle's value, to its resolution, while the car moves
TEST(SimEcuTest, DriveValuesAreTheDriveCyclesThroughTheObd2Defs)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    // 60 s in: off the cold start, pulling through the gears
    for (int i = 0; i < 1200; ++i) {
        ecuio_sim_tick(e);
    }
    const ecuio_mock::DriveCycle& car = e.car;
    ASSERT_TRUE(car.running);
    ASSERT_GT(car.speed_kmh, 1.0f);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x0C}), ECUIO_OBD2_SIG_RPM_ID),
                car.rpm, 0.25);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x0D}), ECUIO_OBD2_SIG_SPEED_ID),
                car.speed_kmh, 0.5);
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x04}), ECUIO_OBD2_SIG_ENGINE_LOAD_ID),
        car.load_pct, 0.2);
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x05}), ECUIO_OBD2_SIG_COOLANT_TEMP_ID),
        car.coolant_c, 0.5);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x0F}), ECUIO_OBD2_SIG_IAT_ID),
                car.iat_c, 0.5);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x10}), ECUIO_OBD2_SIG_MAF_ID),
                car.maf_gs, 0.005);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x11}), ECUIO_OBD2_SIG_THROTTLE_ID),
                car.throttle_pct, 0.2);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x5C}), ECUIO_OBD2_SIG_OIL_TEMP_ID),
                car.oil_c, 0.5);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x42}), ECUIO_OBD2_SIG_BATTERY_V_ID),
                car.battery_v, 0.001);
    EXPECT_GT(car.battery_v, 13.5f) << "charging";
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x0B}), ECUIO_OBD2_PID_SIG_ID(0x0B, 0)),
        car.map_kpa, 0.5);
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x44}), ECUIO_OBD2_PID_SIG_ID(0x44, 0)),
        car.commanded_lambda, 0.0001);
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x33}), ECUIO_OBD2_PID_SIG_ID(0x33, 0)),
        car.baro_kpa, 0.5);
    // run time since start, lambda and ambient by J1979's scaling
    const std::vector<uint8_t> run = ask(e, {0x01, 0x1F});
    ASSERT_EQ(run.size(), 4u);
    EXPECT_NEAR((run[2] << 8) | run[3], e.engine_s, 1.0);
    EXPECT_GT(e.engine_s, 30.0f);
    const std::vector<uint8_t> lambda = ask(e, {0x01, 0x24});
    ASSERT_EQ(lambda.size(), 6u);
    EXPECT_NEAR(((lambda[2] << 8) | lambda[3]) * 2.0 / 65536.0, car.lambda,
                0.0001);
    EXPECT_EQ(ask(e, {0x01, 0x46}), (std::vector<uint8_t>{0x41, 0x46, 60}));
    // the values move: rpm over the next 5 s
    const double before =
        drive_value(ask(e, {0x01, 0x0C}), ECUIO_OBD2_SIG_RPM_ID);
    for (int i = 0; i < 100; ++i) {
        ecuio_sim_tick(e);
    }
    EXPECT_NE(drive_value(ask(e, {0x01, 0x0C}), ECUIO_OBD2_SIG_RPM_ID), before);
}

// The key on, the engine stopped: 0 rpm, the battery's voltage, no run
// time
TEST(SimEcuTest, TheKeyOnWithTheEngineStopped)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    ecuio_sim_tick(e);
    EXPECT_EQ(ask(e, {0x01, 0x0C}), (std::vector<uint8_t>{0x41, 0x0C, 0, 0}));
    const uint16_t mv = ecuio_mock::clamp_u16(e.car.battery_v * 1000.0f);
    EXPECT_EQ(ask(e, {0x01, 0x42}),
              (std::vector<uint8_t>{0x41, 0x42, static_cast<uint8_t>(mv >> 8),
                                    static_cast<uint8_t>(mv)}));
    EXPECT_LT(e.car.battery_v, 13.0f) << "the battery's own";
    EXPECT_EQ(ask(e, {0x01, 0x1F}), (std::vector<uint8_t>{0x41, 0x1F, 0, 0}));
}

// Each mode's bitmaps list exactly the PIDs it answers, and chain
TEST(SimEcuTest, TheBitmapsListWhatEachModeAnswers)
{
    for (ecuio_sim_values values :
         {ecuio_sim_values::drive, ecuio_sim_values::canned}) {
        ecuio_sim_ecu e = sim_of(values);
        int listed = 0;
        for (unsigned base = 0; base <= 0xE0; base += 0x20) {
            const std::vector<uint8_t> b =
                ask(e, {0x01, static_cast<uint8_t>(base)});
            if (b.size() != 6) {
                // a bitmap the chain doesn't reach is refused
                EXPECT_EQ(b, (std::vector<uint8_t>{0x7F, 0x01, 0x12})) << base;
                continue;
            }
            for (unsigned k = 1; k <= 32 && base + k <= 0xFF; ++k) {
                const uint8_t pid = static_cast<uint8_t>(base + k);
                const bool bit =
                    (b[2 + (k - 1) / 8] & (0x80u >> ((k - 1) % 8))) != 0;
                const std::vector<uint8_t> r = ask(e, {0x01, pid});
                EXPECT_EQ(bit, r.size() >= 3 && r[0] == 0x41)
                    << int(values) << " PID " << std::hex << int(pid);
                listed += (bit && pid % 0x20 != 0) ? 1 : 0;
            }
        }
        EXPECT_EQ(listed, values == ecuio_sim_values::drive ? 22 : 95);
    }
}

// values=canned: every Service 01 PID answered as the fakes' ECU answers
// it, refusals included (sim_canned.h mirrors tests/fake_ecu.h)
TEST(SimEcuTest, CannedValuesAreTheFakesOwn)
{
    fake_ecu::reset_diagnostics();
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::canned);
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        const unsigned char req[2] = {0x01, static_cast<unsigned char>(pid)};
        unsigned char fake[fake_ecu::kReplyMax];
        size_t n = 0;
        ASSERT_TRUE(fake_ecu::answer(req, 2, -1, fake, &n));
        EXPECT_EQ(ask(e, {0x01, static_cast<uint8_t>(pid)}),
                  std::vector<uint8_t>(fake, fake + n))
            << std::hex << pid;
    }
    // and after a clear
    const unsigned char clear[1] = {0x04};
    unsigned char out[fake_ecu::kReplyMax];
    size_t n = 0;
    ASSERT_TRUE(fake_ecu::answer(clear, 1, -1, out, &n));
    EXPECT_EQ(ask(e, {0x04}), (std::vector<uint8_t>{0x44}));
    for (unsigned p : {0x01, 0x21, 0x30, 0x31, 0x41, 0x4D, 0x4E}) {
        const uint8_t pid = static_cast<uint8_t>(p);
        const unsigned char req[2] = {0x01, pid};
        ASSERT_TRUE(fake_ecu::answer(req, 2, -1, out, &n));
        EXPECT_EQ(ask(e, {0x01, pid}), std::vector<uint8_t>(out, out + n))
            << std::hex << int(pid);
    }
    fake_ecu::reset_diagnostics();
}

// The diagnostics are the mock's (the fakes' codes), before and after a
// clear
TEST(SimEcuTest, TheDiagnosticsAreTheMocksAndAClearClearsThem)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    EXPECT_EQ(ask(e, {0x03}), (std::vector<uint8_t>{0x43, 0x03, 0x03, 0x01,
                                                    0x04, 0x20, 0xC1, 0x00}));
    EXPECT_EQ(ask(e, {0x07}).size(), 8u);
    EXPECT_EQ(ask(e, {0x0A}), (std::vector<uint8_t>{0x4A, 0x01, 0x04, 0x20}));
    EXPECT_EQ(ask(e, {0x01, 0x01}),
              (std::vector<uint8_t>{0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}));
    EXPECT_EQ(ask(e, {0x02, 0x02, 0x00}),
              (std::vector<uint8_t>{0x42, 0x02, 0x00, 0x03, 0x01}));
    EXPECT_EQ(ask(e, {0x02, 0x0C, 0x01}),
              (std::vector<uint8_t>{0x7F, 0x02, 0x12}));
    EXPECT_EQ(ask(e, {0x01, 0x31}),
              (std::vector<uint8_t>{0x41, 0x31, 0x04, 0xD2}));
    EXPECT_EQ(ask(e, {0x04}), (std::vector<uint8_t>{0x44}));
    EXPECT_TRUE(e.cleared);
    EXPECT_EQ(ask(e, {0x03}), (std::vector<uint8_t>{0x43, 0x00}));
    EXPECT_EQ(ask(e, {0x0A}), (std::vector<uint8_t>{0x4A, 0x01, 0x04, 0x20}))
        << "permanent codes stay";
    EXPECT_EQ(ask(e, {0x01, 0x01}),
              (std::vector<uint8_t>{0x41, 0x01, 0x00, 0x07, 0xE5, 0xE5}));
    EXPECT_EQ(ask(e, {0x01, 0x31}), (std::vector<uint8_t>{0x41, 0x31, 0, 0}));
    EXPECT_EQ(ask(e, {0x02, 0x0C, 0x00}),
              (std::vector<uint8_t>{0x7F, 0x02, 0x12}));
}

// Service 09: the VIN the URI gave and the ECU name; the rest refused,
// and a functional request's refusals unsaid (ISO 14229-1)
TEST(SimEcuTest, IdentificationAndRefusals)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    std::vector<uint8_t> vin = ask(e, {0x09, 0x02});
    ASSERT_EQ(vin.size(), 20u);
    EXPECT_EQ(std::string(vin.begin() + 3, vin.end()), ECUIO_SIM_DEFAULT_VIN);
    EXPECT_EQ(vin[2], 0x01);
    const std::vector<uint8_t> name = ask(e, {0x09, 0x0A});
    ASSERT_EQ(name.size(), 23u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(name.data()) + 3),
              "ECM-HavneSim");
    EXPECT_EQ(ask(e, {0x09, 0x00}),
              (std::vector<uint8_t>{0x49, 0x00, 0x40, 0x40, 0x00, 0x00}));
    EXPECT_EQ(ask(e, {0x09, 0x04}), (std::vector<uint8_t>{0x7F, 0x09, 0x12}));
    EXPECT_EQ(ask(e, {0x22, 0xF1, 0x90}),
              (std::vector<uint8_t>{0x7F, 0x22, 0x31}));
    EXPECT_EQ(ask(e, {0x10, 0x03}), (std::vector<uint8_t>{0x7F, 0x10, 0x11}));
    EXPECT_EQ(ask(e, {0x01, 0x12}), (std::vector<uint8_t>{0x7F, 0x01, 0x12}));
    EXPECT_TRUE(ask(e, {0x01, 0x12}, true).empty());
    EXPECT_TRUE(ask(e, {0x10, 0x03}, true).empty());
    EXPECT_EQ(ask(e, {0x01, 0x0D}, true).size(), 3u) << "a positive one is";
    ecuio_sim_config c;
    char why[256];
    ASSERT_EQ(ecuio_sim_parse_uri("mock://x?vin=1FAFAKE10M0000001", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok);
    e.config = c;
    vin = ask(e, {0x09, 0x02});
    EXPECT_EQ(std::string(vin.begin() + 3, vin.end()), "1FAFAKE10M0000001");
}

// J1979's pack: each PID it has, in the order asked; none, no answer
TEST(SimEcuTest, APackAnswersEachPidItHas)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::canned);
    EXPECT_EQ(
        ask(e, {0x01, 0x0D, 0x12, 0x05, 0x0C}),
        (std::vector<uint8_t>{0x41, 0x0D, 0x00, 0x05, 60, 0x0C, 0x1F, 0x40}));
    EXPECT_TRUE(ask(e, {0x01, 0x12, 0x13}).empty());
    // a pack holding run time's 13 bytes is past a Single Frame
    EXPECT_EQ(ask(e, {0x01, 0x7F, 0x0D}).size(), 1u + 14 + 2);
}

TEST(SimEcuTest, TheUrisKeysAndRefusals)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_EQ(c.values, ecuio_sim_values::drive);
    EXPECT_STREQ(c.vin, ECUIO_SIM_DEFAULT_VIN);
    EXPECT_EQ(c.faults.silent + c.faults.late + c.faults.bad_sequence +
                  c.faults.stalled,
              0u);
    ASSERT_EQ(
        ecuio_sim_parse_uri("mock://sim?values=canned&faults=silent:5,late:7,"
                            "bad_sequence:3,stalled:11",
                            &c, why, sizeof why),
        ecuio_sim_uri_result::ok);
    EXPECT_EQ(c.values, ecuio_sim_values::canned);
    EXPECT_EQ(c.faults.silent, 5u);
    EXPECT_EQ(c.faults.late, 7u);
    EXPECT_EQ(c.faults.bad_sequence, 3u);
    EXPECT_EQ(c.faults.stalled, 11u);
    for (const char* bad :
         {"socketcan://vcan0", "mock://sim?did=1E1C", "mock://sim?values=x",
          "mock://sim?vin=1FAFAKE10M000000", "mock://sim?vin=1FAFAKEI0M0000001",
          "mock://sim?vin=1fafake10m0000001",
          "mock://sim?faults=", "mock://sim?faults=silent",
          "mock://sim?faults=silent:0", "mock://sim?faults=silent:10001",
          "mock://sim?faults=drop:5", "mock://sim?faults=silent:5,",
          "mock://sim?faults=silent:x", "mock://sim?values"}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad, &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
    // replay://: an absolute path, its own keys
#ifdef _WIN32
    const std::string abs = "C:/recordings/run 1";
#else
    const std::string abs = "/recordings/run 1";
#endif
    const std::string replay = "replay://" + abs;
    ASSERT_EQ(ecuio_sim_parse_uri(replay.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.source, ecuio_sim_source::replay);
    EXPECT_EQ(std::string(c.path, c.path_len), abs);
    EXPECT_FALSE(c.loop);
    EXPECT_FALSE(c.vin_given);
    const std::string looped =
        replay + "?loop=1&vin=1FAFAKE10M0000001&faults=late:7";
    ASSERT_EQ(ecuio_sim_parse_uri(looped.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(std::string(c.path, c.path_len), abs) << "the path ends at ?";
    EXPECT_TRUE(c.loop);
    EXPECT_TRUE(c.vin_given);
    EXPECT_STREQ(c.vin, "1FAFAKE10M0000001");
    EXPECT_EQ(c.faults.late, 7u);
    EXPECT_FALSE(c.stretch.given());
    const std::string stretched = replay + "?from=12.5&to=40";
    ASSERT_EQ(ecuio_sim_parse_uri(stretched.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.stretch.from_us, 12500000u);
    EXPECT_EQ(c.stretch.to_us, 40000000u);
    for (const std::string& bad :
         {std::string("replay://"), std::string("replay://?loop=1"),
          std::string("replay://recordings/run"), replay + "?values=canned",
          replay + "?loop=2", replay + "?speed=2", replay + "?from=x",
          replay + "?to=1.2345", replay + "?from=5&to=5",
          std::string("mock://sim?from=1")}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad.c_str(), &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
    // and mock:// never takes replay's key
    EXPECT_EQ(ecuio_sim_parse_uri("mock://sim?loop=1", &c, why, sizeof why),
              ecuio_sim_uri_result::bad);
}

// Every Nth reply of its kind, single-frame and segmented counted apart;
// the bitmaps never; the frame faults on segmented replies only
TEST(SimEcuTest, FaultsBreakEveryNthReplyOfItsKind)
{
    ecuio_sim_faults f;
    f.silent = 3;
    f.bad_sequence = 2;
    ecuio_sim_counts e;
    const uint8_t pid[2] = {0x01, 0x0C};
    const uint8_t bitmap[2] = {0x01, 0x00};
    std::vector<ecuio_sim_fate> singles;
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(ecuio_sim_next_fate(f, e, bitmap, 2, 6),
                  ecuio_sim_fate::intact);
        singles.push_back(ecuio_sim_next_fate(f, e, pid, 2, 4));
    }
    EXPECT_EQ(singles, (std::vector<ecuio_sim_fate>{
                           ecuio_sim_fate::intact, ecuio_sim_fate::intact,
                           ecuio_sim_fate::silent, ecuio_sim_fate::intact,
                           ecuio_sim_fate::intact, ecuio_sim_fate::silent}));
    const uint8_t pack[4] = {0x01, 0x0C, 0x0D, 0x05};
    EXPECT_EQ(ecuio_sim_next_fate(f, e, pack, 4, 9), ecuio_sim_fate::intact);
    EXPECT_EQ(ecuio_sim_next_fate(f, e, pack, 4, 9),
              ecuio_sim_fate::bad_sequence);
    EXPECT_EQ(ecuio_sim_next_fate(f, e, pack, 4, 9), ecuio_sim_fate::silent);
    EXPECT_EQ(e.singles, 6u);
    EXPECT_EQ(e.segmented, 3u);
}

// pending: a fault like the others, every Nth reply of its kind, never a
// bitmap; a module's replies counted on its own counts, apart from the
// engine ECU's, so its period is its own
TEST(SimEcuTest, PendingIsEveryNthReplyAndEachModuleCountsItsOwn)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?faults=pending:3,late:5", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.faults.pending, 3u);
    EXPECT_EQ(c.faults.late, 5u);
    EXPECT_EQ(
        ecuio_sim_parse_uri("mock://sim?faults=pending:0", &c, why, sizeof why),
        ecuio_sim_uri_result::bad);
    ecuio_sim_faults f;
    f.pending = 3;
    ecuio_sim_counts e, m;
    const uint8_t pid[2] = {0x01, 0x0C};
    const uint8_t bitmap[2] = {0x01, 0x20};
    const uint8_t did[3] = {0x22, 0xD1, 0x00};
    std::vector<ecuio_sim_fate> engine, module;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(ecuio_sim_next_fate(f, e, bitmap, 2, 6),
                  ecuio_sim_fate::intact);
        engine.push_back(ecuio_sim_next_fate(f, e, pid, 2, 4));
    }
    module.push_back(ecuio_sim_next_fate(f, m, did, 3, 4));
    module.push_back(ecuio_sim_next_fate(f, m, did, 3, 4));
    module.push_back(ecuio_sim_next_fate(f, m, did, 3, 4));
    const std::vector<ecuio_sim_fate> third = {ecuio_sim_fate::intact,
                                               ecuio_sim_fate::intact,
                                               ecuio_sim_fate::pending};
    EXPECT_EQ(engine, third);
    EXPECT_EQ(module, third) << "the module's own third reply, not the "
                                "engine ECU's sixth";
    EXPECT_EQ(e.singles, 3u);
    EXPECT_EQ(m.singles, 3u);
}

namespace {

std::vector<uint8_t> module_ask(std::initializer_list<uint8_t> req,
                                bool functional = false)
{
    const std::vector<uint8_t> q(req);
    uint8_t out[ECUIO_SIM_REPLY_MAX];
    const uint32_t n = ecuio_sim_module_answer(
        q.data(), static_cast<uint32_t>(q.size()), functional, out);
    return std::vector<uint8_t>(out, out + n);
}

} // namespace

// modules=2's module: Service 22 from its own made-up DIDs, 7F 22 31 for
// one it lacks, 7F <service> 11 for any other service, and a functional
// request's refusals unsaid; only mock:// takes modules, 1 or 2
TEST(SimEcuTest, TheSecondModuleAnswersItsOwnDidsAndRefusesTheRest)
{
    EXPECT_EQ(module_ask({0x22, 0xD1, 0x00}),
              (std::vector<uint8_t>{0x62, 0xD1, 0x00, 0x82}));
    EXPECT_EQ(module_ask({0x22, 0xD1, 0x01}),
              (std::vector<uint8_t>{0x62, 0xD1, 0x01, 0x03}));
    const std::vector<uint8_t> name = module_ask({0x22, 0xD1, 0x02});
    ASSERT_EQ(name.size(), 15u) << "past a Single Frame";
    EXPECT_EQ(std::string(name.begin() + 3, name.end()), "TCM-HavneSim");
    EXPECT_EQ(module_ask({0x22, 0xF1, 0x90}),
              (std::vector<uint8_t>{0x7F, 0x22, 0x31}));
    EXPECT_EQ(module_ask({0x22, 0xD1}),
              (std::vector<uint8_t>{0x7F, 0x22, 0x31}));
    EXPECT_EQ(module_ask({0x01, 0x0C}),
              (std::vector<uint8_t>{0x7F, 0x01, 0x11}));
    EXPECT_EQ(module_ask({0x09, 0x02}),
              (std::vector<uint8_t>{0x7F, 0x09, 0x11}));
    EXPECT_EQ(module_ask({0x22, 0xD1, 0x00}, true),
              (std::vector<uint8_t>{0x62, 0xD1, 0x00, 0x82}))
        << "a positive reply to a functional request is said";
    EXPECT_TRUE(module_ask({0x01, 0x0C}, true).empty());
    EXPECT_TRUE(module_ask({0x22, 0xF1, 0x90}, true).empty());
    EXPECT_TRUE(module_ask({}).empty());

    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_FALSE(c.second_module) << "one ECU by default";
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?modules=2&values=canned", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_TRUE(c.second_module);
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?modules=1", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_FALSE(c.second_module);
#ifdef _WIN32
    const std::string replay = "replay://C:/recordings/run?modules=2";
#else
    const std::string replay = "replay:///recordings/run?modules=2";
#endif
    for (const std::string& bad :
         {std::string("mock://sim?modules=3"),
          std::string("mock://sim?modules=0"),
          std::string("mock://sim?modules="), replay}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad.c_str(), &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
}

/* ---------- Diagnostic sessions (ADR-0060) ---------- */

// The engine ECU's D200-D203 answer in the extended session alone; the
// default session refuses them 7F 22 31
TEST(SimEcuTest, TheExtendedDidsAnswerOnlyInTheExtendedSession)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    const std::vector<uint8_t> refused = {0x7F, 0x22, 0x31};
    const auto ask_in = [&](std::initializer_list<uint8_t> req, bool ext) {
        const std::vector<uint8_t> q(req);
        uint8_t out[ECUIO_SIM_REPLY_MAX];
        const uint32_t n = ecuio_sim_answer(
            e, q.data(), static_cast<uint32_t>(q.size()), false, out, ext);
        return std::vector<uint8_t>(out, out + n);
    };
    for (uint8_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(static_cast<int>(i));
        EXPECT_EQ(ask_in({0x22, 0xD2, i}, false), refused);
        const std::vector<uint8_t> got = ask_in({0x22, 0xD2, i}, true);
        ASSERT_EQ(got.size(), 5u);
        EXPECT_EQ(got[0], 0x62);
        EXPECT_EQ(got[2], i);
        EXPECT_EQ(got[3], ECUIO_SIM_EXTENDED_DIDS[i].data[0]);
    }
    EXPECT_EQ(ask_in({0x22, 0xD2, 0x04}, true), refused) << "past D203";
    EXPECT_EQ(ask_in({0x22, 0xD1, 0x00}, true), refused)
        << "the module's DID isn't the engine ECU's";
}

// 10 and 3E as the sim answers them: the session entered, kept by 3E,
// left by 10 01 or by S3 of silence on the ECU's own time; a node
// without an extended session refuses 10 03 with 12 and still answers
// 3E 00
TEST(SimEcuTest, SessionControlEntersKeepsAndLeaves)
{
    ecuio_sim_session s;
    uint8_t out[8];
    bool handled = false;
    const auto call = [&](bool has, int64_t t,
                          std::initializer_list<uint8_t> req) {
        const std::vector<uint8_t> q(req);
        const uint32_t n = ecuio_sim_session_answer(
            s, has, has, t, q.data(), static_cast<uint32_t>(q.size()), out,
            &handled);
        return std::vector<uint8_t>(out, out + n);
    };
    const int64_t s3 = ECUIO_SIM_S3_US;
    EXPECT_EQ(call(true, 0, {0x10, 0x03}),
              (std::vector<uint8_t>{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_TRUE(handled);
    EXPECT_TRUE(s.extended);
    EXPECT_EQ(call(true, 1000000, {0x3E, 0x00}),
              (std::vector<uint8_t>{0x7E, 0x00}));
    EXPECT_TRUE(call(true, 2000000, {0x3E, 0x80}).empty())
        << "the suppress bit: no reply";
    EXPECT_TRUE(handled);
    EXPECT_TRUE(s.extended) << "and the session kept";
    EXPECT_EQ(call(true, 3000000, {0x10, 0x05}),
              (std::vector<uint8_t>{0x7F, 0x10, 0x12}));
    EXPECT_EQ(call(true, 3000000, {0x10}),
              (std::vector<uint8_t>{0x7F, 0x10, 0x13}));
    // each request restarts S3
    call(true, 3000000 + s3 - 1, {0x3E, 0x00});
    EXPECT_TRUE(s.extended);
    // S3 of silence: back in the default session before the request
    const int64_t quiet = 3000000 + s3 - 1 + s3;
    EXPECT_TRUE(call(true, quiet, {0x22, 0xD2, 0x00}).empty());
    EXPECT_FALSE(handled) << "a service that isn't the session's";
    EXPECT_FALSE(s.extended);
    call(true, quiet, {0x10, 0x03});
    EXPECT_TRUE(s.extended);
    EXPECT_EQ(call(true, quiet, {0x10, 0x01}),
              (std::vector<uint8_t>{0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_FALSE(s.extended);

    EXPECT_EQ(call(false, 0, {0x10, 0x03}),
              (std::vector<uint8_t>{0x7F, 0x10, 0x12}));
    EXPECT_FALSE(s.extended);
    EXPECT_EQ(call(false, 0, {0x3E, 0x00}), (std::vector<uint8_t>{0x7E, 0x00}));
}

// Session control to the functional ID: unanswered, whatever it says
TEST(SimEcuTest, SessionControlToTheFunctionalIdIsUnanswered)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    EXPECT_TRUE(ask(e, {0x10, 0x03}, true).empty());
    EXPECT_TRUE(ask(e, {0x3E, 0x00}, true).empty());
    EXPECT_TRUE(ask(e, {0x22, 0xD2, 0x00}, true).empty())
        << "a functional refusal is unsaid";
}

// The fakes' engine ECU enters and leaves its extended session per reply
// ID and answers D200 only in it; a module has none and its refusal code
// is the knob's
TEST_F(FakeEcuTest, TheEngineEcuHasAnExtendedSessionAndAModuleNone)
{
    struct Restore {
        ~Restore()
        {
            fake_ecu::set_module_dids(nullptr);
            fake_ecu::reset_sessions();
            fake_ecu::session_nrc() = 0;
        }
    } restore;
    fake_ecu::reset_sessions();
    const auto ask = [](uint32_t rx, Bytes req) {
        uint8_t out[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        if (!fake_ecu::answer_for(rx, req.data(), req.size(), -1, out, &n)) {
            return Bytes{};
        }
        return Bytes(out, out + n);
    };
    const Bytes d200{0x22, 0xD2, 0x00};
    const Bytes ok3{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
    EXPECT_EQ(ask(0x7E8, d200), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(ask(0x7E8, {0x10, 0x03}), ok3);
    EXPECT_EQ(ask(0x7E8, d200), (Bytes{0x62, 0xD2, 0x00, 0x1A, 0x2C}));
    EXPECT_EQ(ask(0x7E9, d200), (Bytes{0x7F, 0x22, 0x31}))
        << "the session is the ECU's own, by its reply ID";
    EXPECT_EQ(ask(0x7E8, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    EXPECT_TRUE(ask(0x7E8, {0x3E, 0x80}).empty());
    EXPECT_EQ(ask(0x7E8, {0x10, 0x01}),
              (Bytes{0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_EQ(ask(0x7E8, d200), (Bytes{0x7F, 0x22, 0x31}));
    ask(0x7E8, {0x10, 0x03});
    fake_ecu::reset_sessions();
    EXPECT_EQ(ask(0x7E8, d200), (Bytes{0x7F, 0x22, 0x31}));

    ASSERT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12"));
    EXPECT_EQ(ask(0x7E9, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x12}));
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x01}),
              (Bytes{0x62, 0xD8, 0x01, 0x00, 0x12}))
        << "its DIDs answer in the default session";
    EXPECT_EQ(ask(0x7E9, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    fake_ecu::session_nrc() = 0x22;
    EXPECT_EQ(ask(0x7E9, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x22}));
    EXPECT_EQ(ask(0x7E8, {0x10, 0x03}), ok3) << "the knob is a module's code";
}
