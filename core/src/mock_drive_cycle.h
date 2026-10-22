// File/layer: core/src/mock_drive_cycle.h | internal, not ABI-exported;
//             included by mock_transport.cpp (its only producer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — CoreTest.MockDriveCycle* step whole loops tick by tick
//     and round-trip every tick through the mock signal set (test_core);
//     CoreTest.MockFramesDecodeAgainstMockSignalSet covers the worker.

#ifndef QTTUNE_MOCK_DRIVE_CYCLE_H
#define QTTUNE_MOCK_DRIVE_CYCLE_H

#include "qttune/signal_sets.h"

#include <cmath>
#include <cstdint>

namespace qttune_mock {

/* Scripted drive cycle for the mock transport. A small vehicle model
 * (gear ratios, a torque curve, traction, drag and two heat
 * accumulators) is driven by a phase script, so RPM, speed and
 * distance stay consistent with each other:
 *
 *   WarmUp      cold start, neutral idle with two rev blips (first loop only)
 *   Roll2nd     pull away, settle in 2nd at ~25 km/h (~1550 rpm)
 *   Pull2nd     WOT in 2nd to the rev limiter (~113 km/h)
 *   Limiter2nd  5 s on the limiter
 *   Slow2nd     lift and brake to 45 km/h, shift to 3rd
 *   Settle3rd   light throttle in 3rd (~1900 rpm)
 *   Pull3rd     WOT in 3rd to the limiter (~163 km/h). Roughly twice as
 *               long as the 2nd-gear pull, so it runs hotter.
 *   Limiter3rd  5 s on the limiter
 *   Stop        brake to a standstill
 *   Stage       idle at the line
 *   Launch      launch control: WOT, clutch in, ~4000 rpm
 *   DragRun     standing 1/4 mile, upshifts at 6800 rpm through 4th
 *   Shutdown    lift past the line, brake to a stop
 *   Cooldown    idle, then loop back to Roll2nd
 *
 * One step() per mock frame, each worth kTickS of simulated time. The
 * worker's sleep isn't exact, so simulated time runs slightly behind
 * the wall clock; frame timestamps stay wall-clock. */

constexpr float kTickS = 0.05f;
constexpr float kPi = 3.14159265f;

/* Vehicle: a ~1500 kg, ~330 hp rear-drive coupe on street tyres. */
constexpr float kMassKg = 1500.0f;
constexpr float kTyreRadiusM = 0.33f;
constexpr float kFinalDrive = 3.73f;
constexpr float kDrivelineEff = 0.88f;
constexpr float kTractionLimitN = 9500.0f;   /* ~0.65 g */
constexpr float kAeroCoeff = 0.42f;          /* 0.5 * rho * Cd * A */
constexpr float kRollingN = 177.0f;          /* 0.012 * m * g */
constexpr float kEngineBrakeNm = 40.0f;      /* closed-throttle drag */
constexpr float kGearRatio[5] = {0.0f, 3.36f, 2.07f, 1.43f, 1.00f};
constexpr int kTopGear = 4;

/* Engine. */
constexpr float kIdleRpm = 750.0f;
constexpr float kRevLimitRpm = 7000.0f;
/* Where the limiter engages. A touch under the limit, because rpm
 * pinned at the limit can round to just below it. */
constexpr float kLimiterOnRpm = kRevLimitRpm - 10.0f;
constexpr float kShiftRpm = 6800.0f;         /* drag-run upshift point */
constexpr float kLaunchRpm = 4000.0f;        /* launch-control two-step */
constexpr float kClutchInRpm = 1100.0f;      /* clutch in below this */
constexpr float kPeakTorqueNm = 400.0f;
constexpr float kPeakPower = 2.32e6f;        /* max torque * rpm on the curve */

/* Temperatures, degC. */
constexpr float kAmbientC = 24.0f;
constexpr float kThermostatC = 82.0f;
constexpr float kFanOnC = 100.0f;

constexpr float kQuarterMileM = 402.336f;

constexpr uint32_t ticks_for(float seconds)
{
    return static_cast<uint32_t>(seconds / kTickS + 0.5f);
}

constexpr uint32_t kShiftTicks = ticks_for(0.3f);
constexpr uint32_t kLimiterTicks = ticks_for(5.0f);

inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* Full-throttle torque curve, N*m, linear between points. */
inline float torque_nm(float rpm)
{
    static const float kRpm[] = {0.0f, 1000.0f, 2000.0f, 3000.0f,
                                 4500.0f, 6000.0f, 7000.0f};
    static const float kNm[] = {0.0f, 220.0f, 300.0f, 360.0f,
                                400.0f, 380.0f, 330.0f};
    const int n = static_cast<int>(sizeof(kRpm) / sizeof(kRpm[0]));
    if (rpm <= kRpm[0]) {
        return kNm[0];
    }
    for (int i = 1; i < n; ++i) {
        if (rpm <= kRpm[i]) {
            const float f = (rpm - kRpm[i - 1]) / (kRpm[i] - kRpm[i - 1]);
            return kNm[i - 1] + f * (kNm[i] - kNm[i - 1]);
        }
    }
    return kNm[n - 1];
}

/* Engine rpm per m/s of road speed in a gear (clutch locked). */
inline float rpm_per_mps(int gear)
{
    return kGearRatio[gear] * kFinalDrive * 60.0f / (2.0f * kPi * kTyreRadiusM);
}

/* Wheel force per N*m at the crank in a gear. */
inline float wheel_n_per_nm(int gear)
{
    return kGearRatio[gear] * kFinalDrive * kDrivelineEff / kTyreRadiusM;
}

enum class Phase : uint8_t {
    WarmUp,
    Roll2nd,
    Pull2nd,
    Limiter2nd,
    Slow2nd,
    Settle3rd,
    Pull3rd,
    Limiter3rd,
    Stop,
    Stage,
    Launch,
    DragRun,
    Shutdown,
    Cooldown,
};

struct DriveCycle {
    /* Outputs in physical units; encode_payload() reads these. */
    float rpm = kIdleRpm;
    float speed_kmh = 0.0f;
    float throttle_pct = 0.0f;
    float load_pct = 18.0f;
    float coolant_c = 20.0f;   /* cold start */
    float iat_c = 20.0f;
    float maf_gs = 6.0f;
    float oil_c = 20.0f;       /* cold start */

    /* Script and drivetrain state. */
    Phase phase = Phase::WarmUp;
    uint32_t phase_ticks = 0;
    int gear = 0;              /* 0 = neutral or clutch in */
    bool clutch_slip = false;  /* pulling away: clutch slipping at slip_rpm */
    float slip_rpm = 0.0f;
    uint32_t shift_ticks = 0;  /* > 0 while an upshift is in progress */
    float v_mps = 0.0f;
    float hold_mps = 0.0f;     /* speed held on the limiter */
    float heat = 0.0f;         /* fast heat accumulator (charge air), 0..1 */
    float heat_soak = 0.0f;    /* slow accumulator (under-hood), 0..1 */
    uint64_t tick = 0;

    /* Drag-run bookkeeping. */
    float run_s = 0.0f;
    float distance_m = 0.0f;
    float drag_et_s = 0.0f;
    float drag_trap_kmh = 0.0f;
    uint32_t loops = 0;        /* completed loops back to Roll2nd */

    float phase_s() const { return static_cast<float>(phase_ticks) * kTickS; }

    void step()
    {
        const float thr_req = drive_script();
        update_vehicle(thr_req);
        ++phase_ticks;
        ++tick;
    }

private:
    float brake_mps2_ = 0.0f;  /* service-brake request for this tick */
    float rev_cap_ = kRevLimitRpm;

    void enter(Phase next)
    {
        phase = next;
        phase_ticks = 0;
        switch (next) {
        case Phase::Roll2nd:
            pull_away(1, 1300.0f);
            break;
        case Phase::Limiter2nd:
        case Phase::Limiter3rd:
            hold_mps = v_mps;
            break;
        case Phase::Launch:
            gear = 0;           /* 1st selected, clutch in */
            break;
        case Phase::DragRun:
            pull_away(1, kLaunchRpm);
            run_s = 0.0f;
            distance_m = 0.0f;
            break;
        default:
            break;
        }
    }

    void pull_away(int g, float at_rpm)
    {
        gear = g;
        clutch_slip = true;
        slip_rpm = at_rpm;
        shift_ticks = 0;
    }

    void shift_to(int g)
    {
        gear = g;
        clutch_slip = false;
        shift_ticks = kShiftTicks;
    }

    bool shifting() const { return shift_ticks > 0; }

    /* Light-throttle speed hold: a P controller on road speed. */
    float cruise(float target_kmh, float cap_pct) const
    {
        return clampf(4.0f + 6.0f * (target_kmh - speed_kmh), 0.0f, cap_pct);
    }

    /* A 1.5 s rev blip in neutral starting at `start_s`. */
    float blip(float start_s) const
    {
        const float t = phase_s() - start_s;
        if (t < 0.0f || t > 1.5f) {
            return 0.0f;
        }
        return 35.0f * std::sin(kPi * t / 1.5f);
    }

    /* The driver: picks throttle, brake and gear for this tick and
     * advances the script. Returns the throttle request in percent. */
    float drive_script()
    {
        float thr = 0.0f;
        brake_mps2_ = 0.0f;
        rev_cap_ = kRevLimitRpm;

        switch (phase) {
        case Phase::WarmUp:
            thr = blip(6.0f) + blip(13.0f);
            if (phase_ticks >= ticks_for(20.0f)) {
                enter(Phase::Roll2nd);
            }
            break;
        case Phase::Roll2nd:
            thr = cruise(25.0f, 30.0f);
            if (gear == 1 && !clutch_slip && speed_kmh >= 22.0f) {
                shift_to(2);
            }
            if (gear == 2 && !shifting() && phase_ticks >= ticks_for(9.0f)) {
                enter(Phase::Pull2nd);
            }
            break;
        case Phase::Pull2nd:
        case Phase::Pull3rd:
            thr = 100.0f;
            if (rpm >= kLimiterOnRpm) {
                enter(phase == Phase::Pull2nd ? Phase::Limiter2nd
                                              : Phase::Limiter3rd);
            }
            break;
        case Phase::Limiter2nd:
        case Phase::Limiter3rd:
            thr = 100.0f;
            if (phase_ticks >= kLimiterTicks) {
                enter(phase == Phase::Limiter2nd ? Phase::Slow2nd : Phase::Stop);
            }
            break;
        case Phase::Slow2nd:
            brake_mps2_ = 3.0f;
            if (speed_kmh <= 45.0f) {
                shift_to(3);
                enter(Phase::Settle3rd);
            }
            break;
        case Phase::Settle3rd:
            thr = cruise(45.0f, 35.0f);
            if (!shifting() && phase_ticks >= ticks_for(3.0f)) {
                enter(Phase::Pull3rd);
            }
            break;
        case Phase::Stop:
        case Phase::Shutdown:
            brake_mps2_ = (phase == Phase::Stop) ? 5.0f : 4.0f;
            if (gear > 0 && rpm < kClutchInRpm) {
                gear = 0;
            }
            if (v_mps <= 0.0f) {
                enter(phase == Phase::Stop ? Phase::Stage : Phase::Cooldown);
            }
            break;
        case Phase::Stage:
            if (phase_ticks >= ticks_for(6.0f)) {
                enter(Phase::Launch);
            }
            break;
        case Phase::Launch:
            thr = 100.0f;
            rev_cap_ = kLaunchRpm -
                       120.0f * std::fabs(std::sin(phase_s() * kPi * 4.0f));
            if (phase_ticks >= ticks_for(1.5f)) {
                enter(Phase::DragRun);
            }
            break;
        case Phase::DragRun:
            thr = 100.0f;
            if (!clutch_slip && !shifting() && gear < kTopGear &&
                rpm >= kShiftRpm) {
                shift_to(gear + 1);
            }
            if (distance_m >= kQuarterMileM) {
                drag_et_s = run_s;
                drag_trap_kmh = speed_kmh;
                enter(Phase::Shutdown);
            }
            break;
        case Phase::Cooldown:
            if (phase_ticks >= ticks_for(10.0f)) {
                ++loops;
                enter(Phase::Roll2nd);
            }
            break;
        }
        return thr;
    }

    /* Drivetrain, road load and temperatures for one tick. */
    void update_vehicle(float thr_req)
    {
        /* Pedal travel: a full sweep takes 0.25 s. */
        throttle_pct += clampf(thr_req - throttle_pct, -20.0f, 20.0f);
        const float thr = throttle_pct / 100.0f;

        float power_frac = 0.0f;   /* engine power / peak, drives the heat model */

        if (phase == Phase::Limiter2nd || phase == Phase::Limiter3rd) {
            /* Fuel-cut bounce: rpm saws just under the limit while the
             * car holds speed. */
            const float bounce =
                90.0f - 90.0f * std::cos(phase_s() * 2.0f * kPi * 3.3f);
            rpm = kRevLimitRpm - bounce;
            v_mps = hold_mps;
            load_pct = 100.0f - bounce * (55.0f / 180.0f);
            power_frac = (1.0f - bounce / 360.0f) *
                         torque_nm(kRevLimitRpm) * kRevLimitRpm / kPeakPower;
        } else {
            float drive_n = 0.0f;
            if (gear == 0) {
                /* Neutral or clutch in: rpm follows the pedal. */
                const float wobble =
                    25.0f * std::sin(static_cast<float>(tick) * 0.13f);
                const float target = clampf(kIdleRpm + wobble +
                                            throttle_pct * 60.0f,
                                            0.0f, rev_cap_);
                rpm += (target - rpm) * 0.35f;
                load_pct = 18.0f + throttle_pct * 0.25f;
                power_frac = 0.01f + thr * 0.15f;
            } else if (shifting()) {
                /* Clutch in for the shift: rpm drops toward the new gear. */
                --shift_ticks;
                rpm += (v_mps * rpm_per_mps(gear) - rpm) * 0.4f;
                load_pct = 15.0f;
                power_frac = 0.02f;
            } else {
                const float locked = v_mps * rpm_per_mps(gear);
                if (clutch_slip && locked < slip_rpm) {
                    rpm += (slip_rpm - rpm) * 0.4f;
                } else {
                    clutch_slip = false;
                    rpm = locked;
                }
                if (thr < 0.05f) {
                    /* Closed throttle: fuel cut and engine braking. */
                    drive_n = -kEngineBrakeNm * wheel_n_per_nm(gear) /
                              kDrivelineEff;
                    load_pct = (rpm > 1200.0f) ? 2.0f : 18.0f;
                    power_frac = 0.0f;
                } else if (rpm >= kLimiterOnRpm) {
                    drive_n = 0.0f;   /* limiter fuel cut */
                    load_pct = 40.0f;
                    power_frac = 0.5f;
                } else {
                    const float tq = torque_nm(rpm) * thr;
                    drive_n = clampf(tq * wheel_n_per_nm(gear),
                                     0.0f, kTractionLimitN);
                    load_pct = 18.0f + 82.0f * tq / kPeakTorqueNm;
                    power_frac = tq * rpm / kPeakPower;
                }
            }

            const float resist = kAeroCoeff * v_mps * v_mps +
                                 ((v_mps > 0.0f) ? kRollingN : 0.0f);
            const float accel = (drive_n - resist) / kMassKg - brake_mps2_;
            v_mps += accel * kTickS;
            if (v_mps < 0.0f) {
                v_mps = 0.0f;
            }
            /* In gear the limiter caps road speed, so rpm never
             * overshoots it. */
            if (gear > 0 && !shifting()) {
                v_mps = std::fmin(v_mps, kRevLimitRpm / rpm_per_mps(gear));
            }
        }

        speed_kmh = v_mps * 3.6f;
        load_pct = clampf(load_pct, 0.0f, 100.0f);
        if (phase == Phase::DragRun) {
            run_s += kTickS;
            distance_m += v_mps * kTickS;
        }

        /* Mass airflow, g/s, speed-density: intake strokes per second
         * (rpm / 120) times displacement times load-scaled volumetric
         * efficiency times air density (1.18 g/L). 4.5 L suits the
         * vehicle model's 400 N*m peak, and VE reaches 0.95 at full
         * load. Warm idle reads ~7 g/s; WOT near the limiter ~255 g/s,
         * about 1.3 hp per g/s against the model's ~330 hp, the usual
         * rule of thumb. */
        const float ve = 0.05f + 0.90f * (load_pct / 100.0f);
        maf_gs = (rpm / 120.0f) * 4.5f * ve * 1.18f;

        update_temperatures(clampf(power_frac, 0.0f, 1.0f));
    }

    void update_temperatures(float power_frac)
    {
        heat += (power_frac - heat) * (kTickS / 6.0f);
        heat_soak += (power_frac - heat_soak) * (kTickS / 30.0f);

        const float airflow = clampf(speed_kmh / 90.0f, 0.0f, 1.0f);

        if (coolant_c < kThermostatC) {
            /* Thermostat closed: the small loop warms quickly. */
            coolant_c += (95.0f - coolant_c) * (kTickS / 8.0f);
        } else {
            float target = 88.0f + 40.0f * heat_soak + 8.0f * heat -
                           4.0f * airflow;
            if (coolant_c > kFanOnC) {
                target -= 6.0f;   /* radiator fan */
            }
            coolant_c += (target - coolant_c) * (kTickS / 10.0f);
        }

        /* Intake air: heat soak from the engine bay at low speed, plus
         * charge heating that builds under sustained load. */
        const float soak = (coolant_c - kAmbientC) * 0.32f * (1.0f - airflow);
        const float charge = 45.0f * heat + 25.0f * heat_soak;
        iat_c += (kAmbientC + soak + charge - iat_c) * (kTickS / 4.0f);

        /* Oil: chases coolant with a slower constant plus a load
         * bias — oil runs hotter than coolant under sustained power
         * and lags both warm-up and cool-down. */
        const float oil_target = coolant_c + 4.0f + 14.0f * heat_soak;
        oil_c += (oil_target - oil_c) * (kTickS / 25.0f);
    }
};

inline uint8_t clamp_u8(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return static_cast<uint8_t>(v + 0.5f);
}

inline uint16_t clamp_u16(float v)
{
    if (v < 0.0f) return 0;
    if (v > 65535.0f) return 65535;
    return static_cast<uint16_t>(v + 0.5f);
}

/* Encode side of the signal contract. Layout MUST mirror kMockDefs in
 * signal_sets.cpp:
 *   [0..1] RPM      LE uint16, scale 0.25
 *   [2]    coolant  uint8, offset -40
 *   [3]    speed    uint8, km/h
 *   [4]    throttle uint8, scale 100/255
 *   [5]    IAT      uint8, offset -40
 *   [6]    load     uint8, scale 100/255
 *   [7..8] MAF      LE uint16, scale 0.01 g/s
 *   [9]    oil      uint8, offset -40
 *   [10..11] pad, up to the CAN FD length (QTTUNE_MOCK_TELEMETRY_PAYLOAD_LEN)
 * The transport never decodes (rule 2); the tests round-trip every
 * tick of the cycle through qttune_mock_signal_set(). */
inline void encode_payload(const DriveCycle& s, uint8_t out[QTTUNE_MOCK_TELEMETRY_PAYLOAD_LEN])
{
    const uint16_t rpm_raw = clamp_u16(s.rpm / 0.25f);
    out[0] = static_cast<uint8_t>(rpm_raw & 0xFF);
    out[1] = static_cast<uint8_t>((rpm_raw >> 8) & 0xFF);

    out[2] = clamp_u8(s.coolant_c + 40.0f);
    out[3] = clamp_u8(s.speed_kmh);
    out[4] = clamp_u8(s.throttle_pct * 255.0f / 100.0f);
    out[5] = clamp_u8(s.iat_c + 40.0f);
    out[6] = clamp_u8(s.load_pct * 255.0f / 100.0f);

    /* MAF: centigrams per second, little-endian like RPM. */
    const uint16_t maf_raw = clamp_u16(s.maf_gs / 0.01f);
    out[7] = static_cast<uint8_t>(maf_raw & 0xFF);
    out[8] = static_cast<uint8_t>((maf_raw >> 8) & 0xFF);
    out[9] = clamp_u8(s.oil_c + 40.0f);
    out[10] = 0; out[11] = 0;   /* pad */
}

} // namespace qttune_mock

#endif /* QTTUNE_MOCK_DRIVE_CYCLE_H */
