#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "sim_ecu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using ecuio_mock::Phase;

constexpr uint32_t kEcm = ECUIO_SIM_REPLY_ID;
constexpr uint32_t kTcm = ECUIO_SIM_MODULE_REPLY_ID;

ecuio_sim_config parsed(const char* uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri, &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << uri << ": " << why;
    return c;
}

std::string refusal(const char* uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri, &c, why, sizeof why),
              ecuio_sim_uri_result::bad)
        << uri;
    return why;
}

struct Car {
    ecuio_sim_ecu e;
    explicit Car(bool enhanced = true) { e.config.enhanced = enhanced; }

    Bytes ask(uint32_t rx, uint16_t did)
    {
        const uint8_t req[3] = {0x22, static_cast<uint8_t>(did >> 8),
                                static_cast<uint8_t>(did)};
        std::vector<uint8_t> out(ECUIO_SIM_REPLY_MAX);
        const uint32_t n =
            rx == kEcm
                ? ecuio_sim_answer(e, req, 3, false, out.data())
                : ecuio_sim_module_answer(req, 3, false, out.data(),
                                          e.config.enhanced ? &e : nullptr);
        out.resize(n);
        return out;
    }

    double field(const ecuio_sim_enhanced_field& f)
    {
        const Bytes r = ask(f.def.message_id,
                            static_cast<uint16_t>(f.def.mux_key & 0xFFFFu));
        double v = NAN;
        EXPECT_EQ(ecuio_decode_signal(&f.def, r.data(),
                                      static_cast<uint16_t>(r.size()), &v),
                  ECUIO_DECODE_OK)
            << f.key;
        return v;
    }

    double field(const char* key) { return field(by_key(key)); }

    static const ecuio_sim_enhanced_field& by_key(const char* key)
    {
        for (const ecuio_sim_enhanced_field& f : ecuio_sim_enhanced_fields()) {
            if (std::string(f.key) == key) {
                return f;
            }
        }
        ADD_FAILURE() << "no field " << key;
        return ecuio_sim_enhanced_fields()[0];
    }

    void tick() { ecuio_sim_tick(e); }
};

double model_value(const Car& c, const ecuio_sim_enhanced_field& f)
{
    const ecuio_mock::DriveCycle& d = c.e.car;
    const ecuio_sim_enhanced_state& s = c.e.enhanced;
    const std::string k = f.key;
    if (k == "rpm")
        return d.rpm;
    if (k == "coolant")
        return d.coolant_c;
    if (k == "iat")
        return d.iat_c;
    if (k == "map")
        return d.map_kpa;
    if (k == "speed")
        return d.speed_kmh;
    if (k == "load")
        return d.load_pct;
    if (k == "throttle")
        return d.throttle_pct;
    if (k == "maf")
        return d.maf_gs;
    if (k == "baro")
        return d.baro_kpa;
    if (k == "battery")
        return d.battery_v;
    if (k == "oil")
        return d.oil_c;
    if (k == "lambda")
        return d.lambda;
    if (k == "lambda_commanded")
        return d.commanded_lambda;
    if (k == "boost")
        return d.boost_kpa;
    if (k == "tft")
        return s.tft_c;
    if (k == "gear_commanded")
        return d.gear;
    if (k == "gear")
        return ecuio_sim_enhanced_gear_actual(s);
    if (k == "knock")
        return ecuio_sim_enhanced_knock_total(s);
    if (k == "cam")
        return s.cam_actual_deg;
    if (k == "adaptation")
        return s.adaptation_pct;
    if (k == "run_time")
        return c.e.engine_s;
    if (k == "load_filtered")
        return s.load_filtered_pct;
    if (k == "maf_voltage")
        return 0.5 + 4.2 * (1.0 - std::exp(-d.maf_gs / 80.0));
    if (k == "coolant_1")
        return d.coolant_c + 0.3;
    if (k == "ambient")
        return ecuio_mock::kColdStartC;
    return NAN;
}

template <class Each> void drive(Car& car, uint32_t loops, Each each)
{
    while (car.e.car.loops < loops) {
        car.tick();
        each();
    }
}

TEST(SimEnhancedTest, TheKeyIsOffByDefault)
{
    EXPECT_FALSE(parsed("mock://x").enhanced);
    EXPECT_FALSE(parsed("mock://x?modules=2").enhanced);
    Car off(false);
    off.tick();
    EXPECT_EQ(off.ask(kEcm, 0xFD00), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(off.ask(kTcm, 0xFD80), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(off.ask(kTcm, 0xD100), (Bytes{0x62, 0xD1, 0x00, 0x82}));
}

TEST(SimEnhancedTest, TheKeyBringsTheSecondModuleAndIsRefusedWhereItCantHold)
{
    const ecuio_sim_config c = parsed("mock://x?enhanced=1");
    EXPECT_TRUE(c.enhanced);
    EXPECT_TRUE(c.second_module);
    EXPECT_NE(refusal("mock://x?enhanced=2").find("enhanced: 0 or 1"),
              std::string::npos);
    EXPECT_NE(refusal("mock://x?enhanced=1&profile=obd2").find("profile's set"),
              std::string::npos);
    EXPECT_NE(refusal("mock://x?enhanced=1&values=canned").find("values=drive"),
              std::string::npos);
#ifdef _WIN32
    const char* replay = "replay://C:/rec/frames.csv?enhanced=1";
#else
    const char* replay = "replay:///rec/frames.csv?enhanced=1";
#endif
    EXPECT_NE(refusal(replay).find("no query key 'enhanced'"),
              std::string::npos);
    EXPECT_NE(refusal("mock://x?bogus=1").find("enhanced, values"),
              std::string::npos);
    Car on;
    on.tick();
    EXPECT_EQ(on.ask(kTcm, 0xD100), (Bytes{0x62, 0xD1, 0x00, 0x82}));
    EXPECT_EQ(on.ask(kEcm, 0xFD80), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(on.ask(kTcm, 0xFD00), (Bytes{0x7F, 0x22, 0x31}));
}

TEST(SimEnhancedTest, EachSignalFieldDecodesToTheModelsValue)
{
    Car car;
    uint32_t checked = 0;
    drive(car, 3, [&] {
        if (car.e.car.tick % 50 != 0) {
            return;
        }
        for (const ecuio_sim_enhanced_field& f : ecuio_sim_enhanced_fields()) {
            const double want = model_value(car, f);
            if (f.role != ecuio_sim_enh_role::signal || std::isnan(want)) {
                continue;
            }
            double lo = 0.0, hi = 0.0;
            ASSERT_EQ(ecuio_def_range(&f.def, &lo, &hi), 0) << f.key;
            const double clamped = std::fmin(std::fmax(want, lo), hi);
            EXPECT_NEAR(car.field(f), clamped, f.def.scale / 2.0 + 1e-9)
                << f.key << " at tick " << car.e.car.tick;
            ++checked;
        }
    });
    EXPECT_GT(checked, 1000u);
}

TEST(SimEnhancedTest, TheFluidTemperatureStartsAtAmbientAndLagsCoolant)
{
    Car car;
    EXPECT_FLOAT_EQ(car.e.enhanced.tft_c, ecuio_mock::kColdStartC);
    EXPECT_NEAR(car.field("tft"), ecuio_mock::kColdStartC, 0.125);
    bool lagged = false;
    drive(car, 1, [&] {
        if (car.e.car.coolant_c > 40.0f) {
            EXPECT_LT(car.e.enhanced.tft_c, car.e.car.coolant_c)
                << "tick " << car.e.car.tick;
            lagged = true;
        }
    });
    EXPECT_TRUE(lagged);
    float rose = 0.0f;
    float last = car.e.enhanced.tft_c;
    drive(car, 6, [&] {
        rose = std::fmax(rose, car.e.enhanced.tft_c - last);
        last = car.e.enhanced.tft_c;
    });
    EXPECT_GT(rose, 0.0f);
    EXPECT_NEAR(car.e.enhanced.tft_c, car.e.car.coolant_c, 10.0f);
}

TEST(SimEnhancedTest, KnockIsZeroButInPullsWhereOneCylinderGoesDeepest)
{
    Car car;
    float deepest[4] = {};
    uint32_t pulls_knocked = 0;
    bool in_pull_knock = false;
    Phase last = car.e.car.phase;
    drive(car, 2, [&] {
        const ecuio_mock::DriveCycle& d = car.e.car;
        const ecuio_sim_enhanced_state& s = car.e.enhanced;
        if (d.phase == Phase::WarmUp && d.loops == 0) {
            EXPECT_EQ(ecuio_sim_enhanced_knock_total(s), 0.0f);
        }
        for (int k = 0; k < 4; ++k) {
            deepest[k] = std::fmin(deepest[k], s.knock_deg[k]);
            EXPECT_EQ(std::fmod(s.knock_deg[k], 0.25f), 0.0f);
        }
        const bool pull = d.phase == Phase::Pull2nd ||
                          d.phase == Phase::Pull3rd ||
                          d.phase == Phase::DragRun;
        if (d.phase != last) {
            in_pull_knock = false;
            last = d.phase;
        }
        if (pull && !in_pull_knock &&
            ecuio_sim_enhanced_knock_total(s) < 0.0f) {
            in_pull_knock = true;
            ++pulls_knocked;
        }
    });
    EXPECT_GE(pulls_knocked, 4u);
    for (int k = 0; k < 4; ++k) {
        EXPECT_LT(deepest[k], 0.0f) << "cylinder " << k + 1;
        if (k != 2) {
            EXPECT_LT(deepest[2], deepest[k]) << "cylinder " << k + 1;
        }
    }
    EXPECT_LT(car.e.enhanced.adaptation_pct, 100.0f);
}

TEST(SimEnhancedTest, TheCamFollowsItsTargetWithALag)
{
    Car car;
    uint32_t steps = 0;
    float target_was = 0.0f, actual_was = 0.0f;
    drive(car, 1, [&] {
        const float target =
            ecuio_sim_enhanced_detail::cam_target_deg(car.e.car);
        const float actual = car.e.enhanced.cam_actual_deg;
        const float lo = std::fmin(actual_was, target);
        const float hi = std::fmax(actual_was, target);
        EXPECT_TRUE(actual >= lo - 1e-4f && actual <= hi + 1e-4f)
            << "tick " << car.e.car.tick;
        if (std::fabs(target - target_was) > 5.0f) {
            EXPECT_LT(std::fabs(actual - actual_was),
                      0.5f * std::fabs(target - actual_was))
                << "tick " << car.e.car.tick;
            ++steps;
        }
        target_was = target;
        actual_was = actual;
    });
    EXPECT_GT(steps, 0u);
    EXPECT_GT(car.field("cam_target"), -0.01);
}

TEST(SimEnhancedTest, TheGearsAreTheModelsTheActualSixTicksLater)
{
    Car car;
    std::vector<int> gears;
    uint32_t changes = 0;
    drive(car, 1, [&] {
        gears.push_back(car.e.car.gear);
        EXPECT_EQ(car.field("gear_commanded"), car.e.car.gear);
        if (gears.size() > 6) {
            EXPECT_EQ(car.field("gear"), gears[gears.size() - 7]);
        }
        changes += gears.size() > 1 && gears.back() != gears[gears.size() - 2];
    });
    EXPECT_GE(changes, 4u);
}

TEST(SimEnhancedTest, TheDecoysCountStayAndSum)
{
    Car car;
    for (int i = 0; i < 40; ++i) {
        car.tick();
    }
    uint8_t counter = 0, tcm = 0;
    uint32_t noise_moves = 0, noise = 0;
    for (int i = 0; i < 300; ++i) {
        const Bytes r = car.ask(kEcm, 0xFD06);
        ASSERT_EQ(r.size(), 10u);
        if (i > 0) {
            EXPECT_EQ(r[3], static_cast<uint8_t>(counter + 1));
            noise_moves += static_cast<uint32_t>((r[4] << 8) | r[5]) != noise;
        }
        counter = r[3];
        noise = static_cast<uint32_t>((r[4] << 8) | r[5]);
        EXPECT_EQ(r[6], 0x5A);
        EXPECT_EQ(r[7], 0xC3);
        EXPECT_EQ(r[8], 0x01);
        uint32_t sum = 0;
        for (int b = 3; b < 9; ++b) {
            sum += r[b];
        }
        EXPECT_EQ(r[9], sum & 0xFFu);
        const Bytes t = car.ask(kTcm, 0xFD81);
        ASSERT_EQ(t.size(), 7u);
        if (i > 0) {
            EXPECT_EQ(t[3], static_cast<uint8_t>(tcm + 1));
        }
        tcm = t[3];
        EXPECT_EQ(t[4], 0x00);
        EXPECT_EQ(t[5], 0x7F);
        EXPECT_EQ(t[6], t[3] ^ t[4] ^ t[5]);
    }
    EXPECT_GT(noise_moves, 290u);
}

TEST(SimEnhancedTest, TheScriptedBitsAreOnInTheirKeyOnWindows)
{
    Car car;
    const char* keys[3] = {"door", "lights", "ac"};
    uint32_t on[3] = {};
    drive(car, 2, [&] {
        const ecuio_mock::DriveCycle& d = car.e.car;
        for (int k = 0; k < 3; ++k) {
            const auto& p = ecuio_sim_enhanced_detail::kPrompts[k];
            const bool want = d.phase == Phase::KeyOn &&
                              d.phase_s() >= p.from_s && d.phase_s() < p.to_s;
            EXPECT_EQ(car.field(keys[k]), want ? 1.0 : 0.0)
                << keys[k] << " at tick " << d.tick;
            on[k] += want;
        }
    });
    for (uint32_t n : on) {
        EXPECT_GE(n, 2u * 39u);
    }
}

TEST(SimEnhancedTest, TheBrakeClutchAndFanBitsFollowTheCar)
{
    Car car;
    uint32_t brake = 0, clutch = 0, fan = 0;
    drive(car, 3, [&] {
        const ecuio_mock::DriveCycle& d = car.e.car;
        EXPECT_EQ(car.field("brake"), d.brake_mps2() > 0.0f ? 1.0 : 0.0);
        brake += car.field("brake") == 1.0;
        clutch += car.field("clutch") == 1.0;
        fan += car.field("fan") == 1.0;
        EXPECT_EQ(car.field("fan"), d.fan_on ? 1.0 : 0.0);
    });
    EXPECT_GT(brake, 0u);
    EXPECT_GT(clutch, 0u);
    EXPECT_GT(fan, 0u);
}

struct Unbound {
    ecuio_sim_t* sim = nullptr;
    explicit Unbound(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    Bytes ask(uint32_t request_id, uint16_t did)
    {
        const uint8_t q[3] = {0x22, static_cast<uint8_t>(did >> 8),
                              static_cast<uint8_t>(did)};
        std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(sim, request_id, q, 3, ECUIO_SIM_NOW,
                                out.data(), static_cast<uint32_t>(out.size()),
                                &n),
                  ECUIO_OK);
        if (n != 1) {
            ADD_FAILURE() << n << " replies";
            return {};
        }
        return Bytes(out[0].data, out[0].data + out[0].length);
    }
};

TEST(SimEnhancedTest, SetValueHoldsABitAndRefusesTheRest)
{
    const uint16_t door = Car::by_key("door").def.id;
    const uint16_t rpm = Car::by_key("rpm").def.id;
    Unbound on("mock://x?enhanced=1");
    EXPECT_EQ(on.ask(0x7E0, 0xFD05).at(3) & 0x08, 0);
    ASSERT_EQ(ecuio_sim_set_value(on.sim, door, 1.0), ECUIO_OK);
    EXPECT_EQ(on.ask(0x7E0, 0xFD05).at(3) & 0x08, 0x08);
    ASSERT_EQ(ecuio_sim_set_value(on.sim, door, 0.0), ECUIO_OK);
    EXPECT_EQ(on.ask(0x7E0, 0xFD05).at(3) & 0x08, 0);
    EXPECT_EQ(ecuio_sim_set_value(on.sim, door, 2.0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("0 or 1"),
              std::string::npos);
    EXPECT_EQ(ecuio_sim_set_value(on.sim, rpm, 1.0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("drive cycle"),
              std::string::npos);
    EXPECT_EQ(on.ask(0x7E1, 0xFD80).size(), 16u);

    Unbound off("mock://x?modules=2");
    EXPECT_EQ(ecuio_sim_set_value(off.sim, door, 1.0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(
        std::string(ecuio_last_error_detail()).find("only with enhanced=1"),
        std::string::npos);
    EXPECT_EQ(off.ask(0x7E0, 0xFD05), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(ecuio_sim_set_value(off.sim, 7003, 1.0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("no profile's DIDs"),
              std::string::npos);
}

TEST(SimEnhancedTest, TheEnhancedIdsMeetNoBuiltInSetsIds)
{
    const auto& t = ecuio_sim_enhanced_fields();
    const uint32_t first = t.front().def.id, last = t.back().def.id;
    EXPECT_EQ(last - first + 1u, t.size());
    for (const ecuio_signal_set_t* set :
         {ecuio_mock_signal_set(), ecuio_obd2_signal_set()}) {
        for (uint32_t i = 0; i < set->count; ++i) {
            EXPECT_FALSE(set->defs[i].id >= first && set->defs[i].id <= last)
                << set->defs[i].name;
        }
    }
    EXPECT_LT(last, ECUIO_UDS_GENERIC_SIG_ID_BASE);
}

TEST(SimEnhancedTest, EachFieldIsAValidDefAndFieldsNeverOverlap)
{
    const auto& t = ecuio_sim_enhanced_fields();
    for (size_t i = 0; i < t.size(); ++i) {
        EXPECT_EQ(ecuio_validate_signal(&t[i].def), ECUIO_VALIDATE_OK)
            << t[i].key;
        for (size_t k = 0; k < i; ++k) {
            const ecuio_signal_def_t& a = t[i].def;
            const ecuio_signal_def_t& b = t[k].def;
            if (a.mux_key != b.mux_key || a.message_id != b.message_id) {
                continue;
            }
            const uint32_t a0 = a.start_byte * 8u + a.bit_offset;
            const uint32_t b0 = b.start_byte * 8u + b.bit_offset;
            EXPECT_TRUE(a0 >= b0 + b.bit_length || b0 >= a0 + a.bit_length)
                << t[i].key << " and " << t[k].key;
        }
    }
}

bool vendor_did(const ecuio_sim_enhanced_field& f)
{
    const uint32_t did = f.def.mux_key & 0xFFFFu;
    return did >= 0xFD07u && did <= 0xFD0Au;
}

TEST(SimEnhancedTest, TheVendorDidsAreBigEndianAndOnePacksEveryKind)
{
    uint32_t packed_bits = 0, packed_fixed = 0, packed_signals = 0, vendor = 0;
    for (const ecuio_sim_enhanced_field& f : ecuio_sim_enhanced_fields()) {
        if (!vendor_did(f)) {
            continue;
        }
        ++vendor;
        EXPECT_EQ(f.def.byte_order, ECUIO_BYTE_ORDER_BE) << f.key;
        if ((f.def.mux_key & 0xFFFFu) == 0xFD07u) {
            packed_bits = std::max<uint32_t>(packed_bits, f.def.bit_length);
            packed_fixed += f.role == ecuio_sim_enh_role::fixed ? 1u : 0u;
            packed_signals += f.role == ecuio_sim_enh_role::signal ? 1u : 0u;
        }
    }
    EXPECT_GT(vendor, 15u);
    EXPECT_EQ(packed_bits, 24u);
    EXPECT_EQ(packed_fixed, 1u);
    EXPECT_GE(packed_signals, 4u);
    const ecuio_sim_enhanced_field& run = Car::by_key("run_time");
    EXPECT_EQ(run.def.bit_length, 24u);
    EXPECT_DOUBLE_EQ(Car::by_key("coolant_1").def.offset, -40.0);
    EXPECT_DOUBLE_EQ(Car::by_key("load_filtered").def.offset, -10.0);
}

TEST(SimEnhancedTest, TheRunTimeIsTheEnginesOwnAndTheLoadLagsTheModels)
{
    Car car;
    uint32_t lagging = 0, checked = 0;
    drive(car, 1, [&] {
        EXPECT_FLOAT_EQ(car.e.enhanced.engine_s, car.e.engine_s);
        if (car.e.car.tick % 10 == 0) {
            lagging +=
                std::fabs(car.field("load_filtered") - car.e.car.load_pct) > 5.0
                    ? 1u
                    : 0u;
            ++checked;
        }
    });
    EXPECT_GT(lagging, checked / 50u);
    EXPECT_LT(lagging, checked / 2u);
}

TEST(SimEnhancedTest, TheCoolantSensorsTrackCoolantApart)
{
    Car car;
    double lo = 1e9, hi = -1e9;
    drive(car, 1, [&] {
        if (car.e.car.tick % 20 != 0) {
            return;
        }
        const double one = car.field("coolant_1");
        const double two = car.field("coolant_2");
        EXPECT_NEAR(one, car.e.car.coolant_c, 1.0);
        EXPECT_NEAR(two, car.e.car.coolant_c, 1.0);
        lo = std::fmin(lo, one - two);
        hi = std::fmax(hi, one - two);
    });
    EXPECT_GT(lo, 0.3);
    EXPECT_GT(hi - lo, 0.1);
}

TEST(SimEnhancedTest, TheInjectedQuantityIsTheFuelForEachStroke)
{
    Car car;
    uint32_t running = 0;
    drive(car, 1, [&] {
        const ecuio_mock::DriveCycle& d = car.e.car;
        if (d.tick % 20 != 0) {
            return;
        }
        if (!d.running) {
            EXPECT_EQ(car.field("injected"), 0.0);
            return;
        }
        const double want =
            d.maf_gs / (d.rpm / 120.0) / 4.0 / (14.7 * d.lambda) * 1000.0;
        EXPECT_NEAR(car.field("injected"), std::fmin(want, 60.0), 0.01);
        ++running;
    });
    EXPECT_GT(running, 100u);
}

TEST(SimEnhancedTest, TheMostlyStaticDidsHoldStill)
{
    Car car;
    car.tick();
    const Bytes config = car.ask(kEcm, 0xFD09);
    const Bytes ident = car.ask(kEcm, 0xFD0A);
    ASSERT_EQ(config.size(), 3u + 8u);
    ASSERT_EQ(ident.size(), 3u + 6u);
    drive(car, 1, [&] {
        if (car.e.car.tick % 100 == 0) {
            EXPECT_EQ(car.ask(kEcm, 0xFD09), config);
            EXPECT_EQ(car.ask(kEcm, 0xFD0A), ident);
        }
    });
}

TEST(SimEnhancedTest, TheTruthFileIsTheTable)
{
    const std::string truth = ecuio_sim_enhanced_truth();
    const std::string path =
        std::string(ECUIO_DISCOVERY_FIXTURES) + "/sim-enhanced-truth.json";
    if (const char* to = std::getenv("ECUIO_WRITE_TRUTH")) {
        std::ofstream(to, std::ios::binary) << truth;
    }
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << path;
    std::stringstream file;
    file << in.rdbuf();
    EXPECT_EQ(file.str(), truth)
        << "rewrite it: ECUIO_WRITE_TRUTH=<path> test_obd2 "
           "--gtest_filter=SimEnhancedTest.TheTruthFileIsTheTable";
}

} // namespace
