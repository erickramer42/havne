#ifndef ECUIO_SIM_ENHANCED_H
#define ECUIO_SIM_ENHANCED_H

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "sim_dids.h"
#include "sim_enhanced_model.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

constexpr uint16_t ECUIO_SIM_ENHANCED_ID_BASE = 19000u;
static_assert(ECUIO_SIM_ENHANCED_ID_BASE > ECUIO_OBD2_PID_SIG_ID(0xFF, 7) &&
                  ECUIO_SIM_ENHANCED_ID_BASE > ECUIO_MOCK_SIG_BATTERY_V_ID,
              "the enhanced band is past the mock's and obd2's");
static_assert(ECUIO_SIM_ENHANCED_ID_BASE + 100u <=
                  ECUIO_UDS_GENERIC_SIG_ID_BASE,
              "the enhanced band ends before uds-generic's");

enum class ecuio_sim_enh_source : uint8_t {
    rpm,
    coolant,
    iat,
    map,
    speed,
    load,
    throttle,
    maf,
    baro,
    battery,
    timing,
    knock_total,
    knock_1,
    knock_2,
    knock_3,
    knock_4,
    adaptation,
    cam_target,
    cam_actual,
    lambda_commanded,
    lambda,
    oil,
    boost_target,
    boost,
    brake,
    clutch,
    fan,
    door,
    lights,
    ac,
    counter,
    noise,
    constant,
    sum,
    tft,
    gear_commanded,
    gear,
    output_rpm,
    input_rpm,
    slip,
    lock,
    line_pressure,
    tcm_counter,
    xor_sum
};

enum class ecuio_sim_enh_role : uint8_t {
    signal,
    bit,
    counter,
    noise,
    fixed,
    checksum
};

struct ecuio_sim_enhanced_field {
    ecuio_signal_def_t def;
    ecuio_sim_enh_source source;
    ecuio_sim_enh_role role;
    const char* key;
    const char* wanted; /* the wanted list's key, or nullptr */
    uint32_t constant;
    int prompt; /* kPrompts' index for a scripted bit, else -1 */
};

namespace ecuio_sim_enhanced_detail {

constexpr const char* kDegC = "\xC2\xB0"
                              "C";

struct row {
    double scale;
    double offset;
    const char* name;
    const char* unit;
    const char* key;
    const char* wanted;
    uint32_t rx;
    ecuio_value_kind_t kind;
    uint32_t constant;
    int prompt;
    uint16_t did;
    uint8_t byte;
    uint8_t bit;
    uint8_t bits;
    uint8_t order;
    ecuio_sim_enh_source source;
    ecuio_sim_enh_role role;
};

constexpr uint32_t kEcm = 0x7E8u;
constexpr uint32_t kTcm = 0x7E9u;
constexpr uint8_t LE = ECUIO_BYTE_ORDER_LE;
constexpr uint8_t BE = ECUIO_BYTE_ORDER_BE;
constexpr ecuio_value_kind_t U = ECUIO_VALUE_UINT;
constexpr ecuio_value_kind_t S = ECUIO_VALUE_INT;
using src = ecuio_sim_enh_source;
using role = ecuio_sim_enh_role;

constexpr row r(uint32_t rx, uint16_t did, uint8_t byte, uint8_t bit,
                uint8_t bits, ecuio_value_kind_t kind, uint8_t order,
                double scale, double offset, const char* name, const char* unit,
                ecuio_sim_enh_source source, ecuio_sim_enh_role role,
                const char* key, const char* wanted, uint32_t constant,
                int prompt)
{
    return {scale,  offset, name, unit, key,  wanted, rx,     kind, constant,
            prompt, did,    byte, bit,  bits, order,  source, role};
}

// clang-format off
constexpr row kRows[] = {
    r(kEcm, 0xFD00, 0, 0, 16, U, LE, 0.25, 0, "Engine speed", "RPM", src::rpm, role::signal, "rpm", "engine_speed", 0, -1),
    r(kEcm, 0xFD00, 2, 0, 16, S, BE, 0.1, 0, "Coolant", kDegC, src::coolant, role::signal, "coolant", "coolant_temp", 0, -1),
    r(kEcm, 0xFD00, 4, 0, 8, U, LE, 0.75, -48, "Intake air", kDegC, src::iat, role::signal, "iat", "intake_air_temp", 0, -1),
    r(kEcm, 0xFD00, 5, 0, 16, U, BE, 0.01, 0, "Manifold pressure", "kPa", src::map, role::signal, "map", "manifold_pressure", 0, -1),
    r(kEcm, 0xFD00, 7, 0, 16, U, LE, 0.01, 0, "Road speed", "km/h", src::speed, role::signal, "speed", "vehicle_speed", 0, -1),
    r(kEcm, 0xFD01, 0, 0, 16, U, BE, 100.0 / 65535.0, 0, "Load", "%", src::load, role::signal, "load", "engine_load", 0, -1),
    r(kEcm, 0xFD01, 2, 0, 8, U, LE, 100.0 / 255.0, 0, "Throttle", "%", src::throttle, role::signal, "throttle", "throttle_position", 0, -1),
    r(kEcm, 0xFD01, 3, 0, 16, U, LE, 0.01, 0, "Air flow", "g/s", src::maf, role::signal, "maf", "mass_air_flow", 0, -1),
    r(kEcm, 0xFD01, 5, 0, 8, U, LE, 0.5, 0, "Barometric pressure", "kPa", src::baro, role::signal, "baro", "barometric_pressure", 0, -1),
    r(kEcm, 0xFD01, 6, 0, 16, U, BE, 0.001, 0, "Battery", "V", src::battery, role::signal, "battery", "battery_voltage", 0, -1),
    r(kEcm, 0xFD02, 0, 0, 8, U, LE, 0.5, -64, "Ignition timing", "deg", src::timing, role::signal, "timing", "ignition_timing", 0, -1),
    r(kEcm, 0xFD02, 1, 0, 8, S, LE, 0.25, 0, "Knock retard", "deg", src::knock_total, role::signal, "knock", "knock_retard", 0, -1),
    r(kEcm, 0xFD02, 2, 0, 8, S, LE, 0.25, 0, "Knock retard cylinder 1", "deg", src::knock_1, role::signal, "knock_1", "knock_retard", 0, -1),
    r(kEcm, 0xFD02, 3, 0, 8, S, LE, 0.25, 0, "Knock retard cylinder 2", "deg", src::knock_2, role::signal, "knock_2", "knock_retard", 0, -1),
    r(kEcm, 0xFD02, 4, 0, 8, S, LE, 0.25, 0, "Knock retard cylinder 3", "deg", src::knock_3, role::signal, "knock_3", "knock_retard", 0, -1),
    r(kEcm, 0xFD02, 5, 0, 8, S, LE, 0.25, 0, "Knock retard cylinder 4", "deg", src::knock_4, role::signal, "knock_4", "knock_retard", 0, -1),
    r(kEcm, 0xFD02, 6, 0, 8, U, LE, 100.0 / 255.0, 0, "Knock adaptation", "%", src::adaptation, role::signal, "adaptation", "knock_adaptation", 0, -1),
    r(kEcm, 0xFD03, 0, 0, 16, S, BE, 0.01, 0, "Intake cam target", "deg", src::cam_target, role::signal, "cam_target", "intake_cam_target", 0, -1),
    r(kEcm, 0xFD03, 2, 0, 16, S, BE, 0.01, 0, "Intake cam", "deg", src::cam_actual, role::signal, "cam", "intake_cam", 0, -1),
    r(kEcm, 0xFD03, 4, 0, 16, U, LE, 2.0 / 65536.0, 0, "Commanded lambda", "", src::lambda_commanded, role::signal, "lambda_commanded", "lambda_commanded", 0, -1),
    r(kEcm, 0xFD03, 6, 0, 16, U, BE, 2.0 / 65536.0, 0, "Lambda", "", src::lambda, role::signal, "lambda", "lambda", 0, -1),
    r(kEcm, 0xFD04, 0, 0, 16, S, LE, 0.1, 0, "Oil temperature", kDegC, src::oil, role::signal, "oil", "oil_temp", 0, -1),
    r(kEcm, 0xFD04, 2, 0, 16, S, BE, 0.1, 0, "Boost target", "kPa", src::boost_target, role::signal, "boost_target", "boost_target", 0, -1),
    r(kEcm, 0xFD04, 4, 0, 16, S, BE, 0.1, 0, "Boost", "kPa", src::boost, role::signal, "boost", "boost", 0, -1),
    r(kEcm, 0xFD05, 0, 0, 1, U, LE, 1, 0, "Brake", "", src::brake, role::bit, "brake", "brake_switch", 0, -1),
    r(kEcm, 0xFD05, 0, 1, 1, U, LE, 1, 0, "Clutch", "", src::clutch, role::bit, "clutch", "clutch_switch", 0, -1),
    r(kEcm, 0xFD05, 0, 2, 1, U, LE, 1, 0, "Fan", "", src::fan, role::bit, "fan", "cooling_fan", 0, -1),
    r(kEcm, 0xFD05, 0, 3, 1, U, LE, 1, 0, "Door", "", src::door, role::bit, "door", "door_open", 0, 0),
    r(kEcm, 0xFD05, 0, 4, 1, U, LE, 1, 0, "Lights", "", src::lights, role::bit, "lights", "headlights", 0, 1),
    r(kEcm, 0xFD05, 0, 5, 1, U, LE, 1, 0, "A/C request", "", src::ac, role::bit, "ac", "ac_request", 0, 2),
    r(kEcm, 0xFD06, 0, 0, 8, U, LE, 1, 0, "Counter", "", src::counter, role::counter, "counter", nullptr, 0, -1),
    r(kEcm, 0xFD06, 1, 0, 16, U, BE, 1, 0, "Noise", "", src::noise, role::noise, "noise", nullptr, 0, -1),
    r(kEcm, 0xFD06, 3, 0, 8, U, LE, 1, 0, "Static A", "", src::constant, role::fixed, "static_a", nullptr, 0x5A, -1),
    r(kEcm, 0xFD06, 4, 0, 8, U, LE, 1, 0, "Static B", "", src::constant, role::fixed, "static_b", nullptr, 0xC3, -1),
    r(kEcm, 0xFD06, 5, 0, 8, U, LE, 1, 0, "Static C", "", src::constant, role::fixed, "static_c", nullptr, 0x01, -1),
    r(kEcm, 0xFD06, 6, 0, 8, U, LE, 1, 0, "Checksum", "", src::sum, role::checksum, "checksum", nullptr, 0, -1),
    r(kTcm, 0xFD80, 0, 0, 16, U, BE, 0.125, -40, "Fluid temperature", kDegC, src::tft, role::signal, "tft", "transmission_fluid_temp", 0, -1),
    r(kTcm, 0xFD80, 2, 0, 8, U, LE, 1, 0, "Commanded gear", "", src::gear_commanded, role::signal, "gear_commanded", "gear_commanded", 0, -1),
    r(kTcm, 0xFD80, 3, 0, 8, U, LE, 1, 0, "Gear", "", src::gear, role::signal, "gear", "gear", 0, -1),
    r(kTcm, 0xFD80, 4, 0, 16, U, BE, 1, 0, "Output shaft speed", "RPM", src::output_rpm, role::signal, "output_rpm", "output_shaft_speed", 0, -1),
    r(kTcm, 0xFD80, 6, 0, 16, U, LE, 1, 0, "Input shaft speed", "RPM", src::input_rpm, role::signal, "input_rpm", "turbine_speed", 0, -1),
    r(kTcm, 0xFD80, 8, 0, 16, S, BE, 1, 0, "Slip", "RPM", src::slip, role::signal, "slip", "converter_slip", 0, -1),
    r(kTcm, 0xFD80, 10, 0, 1, U, LE, 1, 0, "Locked", "", src::lock, role::bit, "locked", "converter_lock", 0, -1),
    r(kTcm, 0xFD80, 11, 0, 16, U, BE, 1, 0, "Line pressure", "kPa", src::line_pressure, role::signal, "line_pressure", "line_pressure", 0, -1),
    r(kTcm, 0xFD81, 0, 0, 8, U, LE, 1, 0, "Counter", "", src::tcm_counter, role::counter, "tcm_counter", nullptr, 0, -1),
    r(kTcm, 0xFD81, 1, 0, 8, U, LE, 1, 0, "Static A", "", src::constant, role::fixed, "tcm_static_a", nullptr, 0x00, -1),
    r(kTcm, 0xFD81, 2, 0, 8, U, LE, 1, 0, "Static B", "", src::constant, role::fixed, "tcm_static_b", nullptr, 0x7F, -1),
    r(kTcm, 0xFD81, 3, 0, 8, U, LE, 1, 0, "Checksum", "", src::xor_sum, role::checksum, "tcm_checksum", nullptr, 0, -1),
};
// clang-format on

constexpr size_t kFieldCount = sizeof kRows / sizeof kRows[0];
static_assert(kFieldCount <= 100, "the enhanced band holds every field");

inline ecuio_sim_enhanced_field field_of(const row& r, size_t index)
{
    ecuio_sim_enhanced_field f{};
    ecuio_signal_def_t& d = f.def;
    d.id = static_cast<uint16_t>(ECUIO_SIM_ENHANCED_ID_BASE + index);
    d.message_id = r.rx;
    d.mux_offset = 0;
    d.mux_length = 3;
    d.mux_key = 0x620000u | r.did;
    d.kind = static_cast<uint8_t>(r.kind);
    d.byte_order = r.order;
    d.start_byte = static_cast<uint16_t>(3u + r.byte);
    d.bit_offset = r.bit;
    d.bit_length = r.bits;
    d.scale = r.scale;
    d.offset = r.offset;
    std::snprintf(d.name, sizeof d.name, "%s", r.name);
    std::snprintf(d.unit, sizeof d.unit, "%s", r.unit);
    f.source = r.source;
    f.role = r.role;
    f.key = r.key;
    f.wanted = r.wanted;
    f.constant = r.constant;
    f.prompt = r.prompt;
    return f;
}

using table = std::array<ecuio_sim_enhanced_field, kFieldCount>;

inline table make_table()
{
    table t{};
    for (size_t i = 0; i < kFieldCount; ++i) {
        t[i] = field_of(kRows[i], i);
    }
    return t;
}

} // namespace ecuio_sim_enhanced_detail

inline const ecuio_sim_enhanced_detail::table& ecuio_sim_enhanced_fields()
{
    static const ecuio_sim_enhanced_detail::table t =
        ecuio_sim_enhanced_detail::make_table();
    return t;
}

inline int ecuio_sim_enhanced_index(uint16_t id)
{
    const uint32_t k = static_cast<uint32_t>(id) - ECUIO_SIM_ENHANCED_ID_BASE;
    return id >= ECUIO_SIM_ENHANCED_ID_BASE &&
                   k < ecuio_sim_enhanced_detail::kFieldCount
               ? static_cast<int>(k)
               : -1;
}

inline int ecuio_sim_enhanced_bit_place(size_t index)
{
    const auto& t = ecuio_sim_enhanced_fields();
    if (index >= t.size() || t[index].role != ecuio_sim_enh_role::bit) {
        return -1;
    }
    int place = 0;
    for (size_t i = 0; i < index; ++i) {
        place += t[i].role == ecuio_sim_enh_role::bit ? 1 : 0;
    }
    return place;
}

namespace ecuio_sim_enhanced_detail {

inline bool bit_on(const ecuio_sim_enhanced_state& s,
                   const ecuio_mock::DriveCycle& c, src from)
{
    switch (from) {
    case src::brake:
        return c.brake_mps2() > 0.0f;
    case src::clutch:
        return c.running && (c.gear == 0 || c.clutch_slip || c.shifting());
    case src::fan:
        return c.fan_on;
    case src::door:
        return ecuio_sim_enhanced_prompt_on(c, 0);
    case src::lights:
        return ecuio_sim_enhanced_prompt_on(c, 1);
    case src::ac:
        return ecuio_sim_enhanced_prompt_on(c, 2);
    case src::lock: {
        const int g = ecuio_sim_enhanced_gear_actual(s);
        return c.running && g > 0 &&
               std::fabs(c.rpm - input_shaft_rpm(c, g)) < kLockSlipRpm;
    }
    default:
        return false;
    }
}

inline double engine_value(const ecuio_sim_enhanced_state& s,
                           const ecuio_mock::DriveCycle& c, src from)
{
    switch (from) {
    case src::rpm:
        return c.rpm;
    case src::coolant:
        return c.coolant_c;
    case src::iat:
        return c.iat_c;
    case src::map:
        return c.map_kpa;
    case src::speed:
        return c.speed_kmh;
    case src::load:
        return c.load_pct;
    case src::throttle:
        return c.throttle_pct;
    case src::maf:
        return c.maf_gs;
    case src::baro:
        return c.baro_kpa;
    case src::battery:
        return c.battery_v;
    case src::timing:
        return c.running
                   ? base_timing_deg(c) + ecuio_sim_enhanced_knock_total(s)
                   : 0.0;
    case src::knock_total:
        return ecuio_sim_enhanced_knock_total(s);
    case src::knock_1:
        return s.knock_deg[0];
    case src::knock_2:
        return s.knock_deg[1];
    case src::knock_3:
        return s.knock_deg[2];
    case src::knock_4:
        return s.knock_deg[3];
    case src::adaptation:
        return s.adaptation_pct;
    case src::cam_target:
        return cam_target_deg(c);
    case src::cam_actual:
        return s.cam_actual_deg;
    case src::lambda_commanded:
        return c.commanded_lambda;
    case src::lambda:
        return c.lambda;
    case src::oil:
        return c.oil_c;
    case src::boost_target:
        return boost_target_kpa(c);
    case src::boost:
        return c.boost_kpa;
    default:
        return 0.0;
    }
}

inline double tcm_value(const ecuio_sim_enhanced_state& s,
                        const ecuio_mock::DriveCycle& c, src from)
{
    const int g = ecuio_sim_enhanced_gear_actual(s);
    switch (from) {
    case src::tft:
        return s.tft_c;
    case src::gear_commanded:
        return c.gear;
    case src::gear:
        return g;
    case src::output_rpm:
        return output_shaft_rpm(c);
    case src::input_rpm:
        return input_shaft_rpm(c, g);
    case src::slip:
        return c.rpm - input_shaft_rpm(c, g);
    case src::line_pressure:
        if (!c.running) {
            return 0.0;
        }
        return 350.0 + 60.0 * g + 4.0 * c.load_pct +
               (c.shifting() ? 300.0 : 0.0);
    default:
        return engine_value(s, c, from);
    }
}

inline uint32_t clamped_raw(const ecuio_signal_def_t& d, double value)
{
    const double x = std::nearbyint((value - d.offset) / d.scale);
    const int bits = d.bit_length;
    if (d.kind == ECUIO_VALUE_INT) {
        const double lo = -std::ldexp(1.0, bits - 1);
        const double hi = std::ldexp(1.0, bits - 1) - 1.0;
        const int64_t r = static_cast<int64_t>(x < lo ? lo : (x > hi ? hi : x));
        const uint64_t mask = bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        return static_cast<uint32_t>(static_cast<uint64_t>(r) & mask);
    }
    const double hi = std::ldexp(1.0, bits) - 1.0;
    return static_cast<uint32_t>(x < 0.0 ? 0.0 : (x > hi ? hi : x));
}

inline uint32_t raw_of(ecuio_sim_enhanced_state& s,
                       const ecuio_mock::DriveCycle& c,
                       const ecuio_sim_enhanced_field& f, int place)
{
    if (f.role == role::fixed) {
        return f.constant;
    }
    if (f.role == role::counter) {
        return f.source == src::counter ? s.counter++ : s.tcm_counter++;
    }
    if (f.role == role::noise) {
        s.noise = s.noise * 1103515245u + 12345u;
        return (s.noise >> 16) & 0xFFFFu;
    }
    if (f.role == role::bit) {
        if (place >= 0 && (s.held_mask >> place) & 1u) {
            return (s.held_on >> place) & 1u;
        }
        return bit_on(s, c, f.source) ? 1u : 0u;
    }
    return clamped_raw(f.def, tcm_value(s, c, f.source));
}

} // namespace ecuio_sim_enhanced_detail

inline uint32_t ecuio_sim_enhanced_answer(ecuio_sim_enhanced_state& s,
                                          const ecuio_mock::DriveCycle& c,
                                          uint32_t rx, const uint8_t* req,
                                          uint32_t len, uint8_t* out)
{
    using namespace ecuio_sim_enhanced_detail;
    if (len != 3 || req[0] != 0x22) {
        return 0;
    }
    const uint32_t key =
        0x620000u | (static_cast<uint32_t>(req[1]) << 8) | req[2];
    const auto& t = ecuio_sim_enhanced_fields();
    uint32_t n = 0;
    int place = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const ecuio_sim_enhanced_field& f = t[i];
        const int here = f.role == role::bit ? place++ : -1;
        if (f.def.message_id != rx || f.def.mux_key != key) {
            continue;
        }
        const uint32_t end = ecuio_sim_field_end(f.def);
        if (n == 0) {
            out[0] = 0x62;
            out[1] = req[1];
            out[2] = req[2];
            n = 3;
        }
        if (end > n) {
            std::memset(out + n, 0, end - n);
            n = end;
        }
        uint32_t raw = 0;
        if (f.role == role::checksum) {
            for (uint32_t b = 3; b < f.def.start_byte; ++b) {
                raw = f.source == src::sum ? (raw + out[b]) & 0xFFu
                                           : raw ^ out[b];
            }
        } else {
            raw = raw_of(s, c, f, here);
        }
        ecuio_sim_put_field(f.def, raw, out);
    }
    return n;
}

namespace ecuio_sim_enhanced_detail {

inline void put_number(std::string& out, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    if (std::strtod(buf, nullptr) != v) {
        std::snprintf(buf, sizeof buf, "%.17g", v);
    }
    out += buf;
}

inline const char* role_name(role r)
{
    switch (r) {
    case role::signal:
        return "signal";
    case role::bit:
        return "bit";
    case role::counter:
        return "counter";
    case role::noise:
        return "noise";
    case role::fixed:
        return "static";
    default:
        return "checksum";
    }
}

inline void put_field(std::string& out, const ecuio_sim_enhanced_field& f)
{
    const ecuio_signal_def_t& d = f.def;
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "        {\"id\": %u, \"key\": \"%s\", \"name\": \"%s\", "
                  "\"kind\": \"%s\", \"byte\": %u, \"bit\": %u, \"bits\": %u, ",
                  d.id, f.key, d.name, role_name(f.role), d.start_byte - 3u,
                  d.bit_offset, d.bit_length);
    out += buf;
    out += d.byte_order == ECUIO_BYTE_ORDER_BE ? "\"order\": \"be\", "
                                               : "\"order\": \"le\", ";
    out += d.kind == ECUIO_VALUE_INT ? "\"signed\": true, \"scale\": "
                                     : "\"signed\": false, \"scale\": ";
    put_number(out, d.scale);
    out += ", \"offset\": ";
    put_number(out, d.offset);
    out += ", \"unit\": \"";
    out += d.unit;
    out += "\", \"wanted\": ";
    out += f.wanted != nullptr ? std::string("\"") + f.wanted + "\"" : "null";
    if (f.role == role::fixed) {
        std::snprintf(buf, sizeof buf, ", \"value\": %u", f.constant);
        out += buf;
    }
    if (f.prompt >= 0) {
        const prompt& p = kPrompts[f.prompt];
        out += ", \"on_in_key_on\": [";
        put_number(out, p.from_s);
        out += ", ";
        put_number(out, p.to_s);
        out += "]";
    }
    out += "}";
}

} // namespace ecuio_sim_enhanced_detail

inline std::string ecuio_sim_enhanced_truth()
{
    using namespace ecuio_sim_enhanced_detail;
    const auto& t = ecuio_sim_enhanced_fields();
    std::string out =
        "{\n  \"sim_enhanced_truth\": 1,\n"
        "  \"uri\": \"mock://?enhanced=1\",\n"
        "  \"byte\": \"from the first data byte, after 62 and the DID\",\n"
        "  \"dids\": [\n";
    for (size_t i = 0; i < t.size(); ++i) {
        const ecuio_signal_def_t& d = t[i].def;
        const bool first = i == 0 || t[i - 1].def.mux_key != d.mux_key ||
                           t[i - 1].def.message_id != d.message_id;
        const bool last = i + 1 == t.size() ||
                          t[i + 1].def.mux_key != d.mux_key ||
                          t[i + 1].def.message_id != d.message_id;
        if (first) {
            char buf[96];
            std::snprintf(buf, sizeof buf,
                          "    {\"module\": \"%03X\", \"request\": \"%03X\", "
                          "\"did\": \"%04X\", \"fields\": [\n",
                          d.message_id, d.message_id - 8u, d.mux_key & 0xFFFFu);
            out += buf;
        }
        put_field(out, t[i]);
        out += last ? "\n    ]}" : ",\n";
        if (last) {
            out += i + 1 == t.size() ? "\n" : ",\n";
        }
    }
    out += "  ]\n}\n";
    return out;
}

#endif /* ECUIO_SIM_ENHANCED_H */
