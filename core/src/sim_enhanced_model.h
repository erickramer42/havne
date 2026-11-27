#ifndef ECUIO_SIM_ENHANCED_MODEL_H
#define ECUIO_SIM_ENHANCED_MODEL_H

#include "mock_drive_cycle.h"

#include <cmath>
#include <cstdint>

namespace ecuio_sim_enhanced_detail {

constexpr int kCylinders = 4;
constexpr float kKnockStepDeg = -1.5f;
constexpr float kKnockFloorDeg = -8.0f;
constexpr float kKnockRecoverDeg = 0.25f;
constexpr uint32_t kKnockEveryTicks = 7;
constexpr uint32_t kKnockRecoverTicks = 10;
constexpr float kKnockLoadPct = 85.0f;
constexpr float kKnockRpm = 3000.0f;
constexpr float kAdaptationStepPct = 2.0f;
constexpr float kAdaptationRecoverPctPerS = 0.02f;
constexpr float kCamTauS = 0.3f;
constexpr float kTftTauS = 300.0f;
constexpr float kTftLoadCPerS = 0.01f;
constexpr float kTftShiftC = 0.2f;
constexpr uint32_t kGearLagTicks = 6;
constexpr float kLockSlipRpm = 50.0f;
constexpr float kLoadTauS = 1.5f;
constexpr float kAirFuel = 14.7f;

struct prompt {
    float from_s;
    float to_s;
};
/* door, lights and A/C: on in every key-on between these phase times */
constexpr prompt kPrompts[3] = {{2.0f, 4.0f}, {6.0f, 8.0f}, {10.0f, 12.0f}};

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float base_timing_deg(const ecuio_mock::DriveCycle& c)
{
    if (!c.running) {
        return 0.0f;
    }
    return clampf(10.0f + c.rpm / 400.0f - c.load_pct / 10.0f, 5.0f, 40.0f);
}

inline float cam_target_deg(const ecuio_mock::DriveCycle& c)
{
    if (!c.running || c.rpm < 1000.0f || c.load_pct < 20.0f) {
        return 0.0f;
    }
    return clampf((c.rpm - 1000.0f) / 5500.0f * 30.0f + c.load_pct / 10.0f,
                  0.0f, 40.0f);
}

inline float boost_target_kpa(const ecuio_mock::DriveCycle& c)
{
    if (!c.running || c.rpm < ecuio_mock::kSpoolRpm) {
        return 0.0f;
    }
    return ecuio_mock::kBoostMaxKpa * clampf(c.airflow, 0.0f, 1.0f);
}

inline float output_shaft_rpm(const ecuio_mock::DriveCycle& c)
{
    return c.wheel_mps * ecuio_mock::kFinalDrive * 60.0f /
           (2.0f * ecuio_mock::kPi * ecuio_mock::kTireRadiusM);
}

inline float input_shaft_rpm(const ecuio_mock::DriveCycle& c, int gear)
{
    if (gear <= 0 || gear > ecuio_mock::kTopGear) {
        return c.rpm;
    }
    return output_shaft_rpm(c) * ecuio_mock::kGearRatio[gear];
}

inline float maf_sensor_v(const ecuio_mock::DriveCycle& c)
{
    return 0.5f + 4.2f * (1.0f - std::exp(-c.maf_gs / 80.0f));
}

inline float injected_mg(const ecuio_mock::DriveCycle& c)
{
    if (!c.running || c.rpm < 100.0f || c.lambda <= 0.0f) {
        return 0.0f;
    }
    const float air_g =
        c.maf_gs / (c.rpm / 120.0f) / static_cast<float>(kCylinders);
    return clampf(air_g / (kAirFuel * c.lambda) * 1000.0f, 0.0f, 60.0f);
}

} // namespace ecuio_sim_enhanced_detail

struct ecuio_sim_enhanced_state {
    float tft_c = ecuio_mock::kColdStartC;
    float cam_actual_deg = 0.0f;
    float knock_deg[ecuio_sim_enhanced_detail::kCylinders] = {};
    float adaptation_pct = 100.0f;
    uint32_t heavy_ticks = 0;
    uint32_t quiet_ticks = 0;
    uint32_t knock_events = 0;
    int gears[ecuio_sim_enhanced_detail::kGearLagTicks + 1] = {};
    int last_gear = 0;
    uint8_t counter = 0;
    uint8_t tcm_counter = 0;
    uint32_t noise = 0x2545F491u;
    uint32_t held_mask = 0; /* a bit's place in the table's bits, set_value's */
    uint32_t held_on = 0;
    float engine_s = 0.0f;
    bool was_running = false;
    float load_filtered_pct = 0.0f;
    uint32_t ticks = 0;
};

inline int ecuio_sim_enhanced_gear_actual(const ecuio_sim_enhanced_state& s)
{
    return s.gears[0];
}

inline float ecuio_sim_enhanced_knock_total(const ecuio_sim_enhanced_state& s)
{
    float m = 0.0f;
    for (float k : s.knock_deg) {
        m = std::fmin(m, k);
    }
    return m;
}

inline bool ecuio_sim_enhanced_prompt_on(const ecuio_mock::DriveCycle& c,
                                         int which)
{
    const ecuio_sim_enhanced_detail::prompt& p =
        ecuio_sim_enhanced_detail::kPrompts[which];
    const float t = c.phase_s();
    return c.phase == ecuio_mock::Phase::KeyOn && t >= p.from_s && t < p.to_s;
}

inline void ecuio_sim_enhanced_knock(ecuio_sim_enhanced_state& s,
                                     const ecuio_mock::DriveCycle& c)
{
    using namespace ecuio_sim_enhanced_detail;
    const bool heavy = c.running && c.load_pct >= kKnockLoadPct &&
                       c.rpm >= kKnockRpm && !c.fuel_cut;
    s.heavy_ticks = heavy ? s.heavy_ticks + 1 : 0;
    if (heavy && s.heavy_ticks % kKnockEveryTicks == 0) {
        /* cylinder 3 knocks on every other event, the rest in turn */
        const uint32_t e = s.knock_events++;
        const int cyl = (e % 2 == 0) ? 2 : static_cast<int>((e / 2) % 4);
        s.knock_deg[cyl] =
            std::fmax(s.knock_deg[cyl] + kKnockStepDeg, kKnockFloorDeg);
        s.adaptation_pct =
            std::fmax(s.adaptation_pct - kAdaptationStepPct, 0.0f);
        s.quiet_ticks = 0;
        return;
    }
    if (++s.quiet_ticks % kKnockRecoverTicks == 0) {
        for (float& k : s.knock_deg) {
            k = std::fmin(k + kKnockRecoverDeg, 0.0f);
        }
    }
    s.adaptation_pct = std::fmin(s.adaptation_pct + kAdaptationRecoverPctPerS *
                                                        ecuio_mock::kTickS,
                                 100.0f);
}

inline void ecuio_sim_enhanced_step(ecuio_sim_enhanced_state& s,
                                    const ecuio_mock::DriveCycle& c)
{
    using namespace ecuio_sim_enhanced_detail;
    const float dt = ecuio_mock::kTickS;
    ecuio_sim_enhanced_knock(s, c);
    s.cam_actual_deg += (cam_target_deg(c) - s.cam_actual_deg) * dt / kCamTauS;
    float heat = (c.coolant_c - s.tft_c) / kTftTauS * dt;
    if (c.running) {
        heat += kTftLoadCPerS * c.load_pct / 100.0f * dt;
    }
    if (c.gear != s.last_gear && c.gear > 0) {
        heat += kTftShiftC;
    }
    s.tft_c += heat;
    s.last_gear = c.gear;
    for (uint32_t i = 0; i < kGearLagTicks; ++i) {
        s.gears[i] = s.gears[i + 1];
    }
    s.gears[kGearLagTicks] = c.gear;
    s.engine_s = c.running && s.was_running ? s.engine_s + dt : 0.0f;
    s.was_running = c.running;
    s.load_filtered_pct += (c.load_pct - s.load_filtered_pct) * dt / kLoadTauS;
    ++s.ticks;
}

inline float
ecuio_sim_enhanced_coolant_sensor(const ecuio_sim_enhanced_state& s,
                                  const ecuio_mock::DriveCycle& c, int which)
{
    const float ripple = 0.15f * std::sin(static_cast<float>(s.ticks) * 0.37f);
    return which == 0 ? c.coolant_c + 0.3f : c.coolant_c - 0.4f + ripple;
}

#endif /* ECUIO_SIM_ENHANCED_MODEL_H */
