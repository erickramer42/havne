// File/layer: core/src/mock_drive_cycle.h | internal, not ABI-exported;
//             included by mock_transport.cpp (its only producer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — CoreTest.MockDriveCycle* step whole loops tick by tick
//     and round-trip every tick through the mock signal set (test_core);
//     CoreTest.MockDriveCycleSignalsStayPhysical checks every tick against
//     the consistency rules below, and ...ReadingsNeverHoldDeadStill that
//     rpm neither sits still nor steps; CoreTest.MockFramesDecodeAgainstMockSignalSet
//     covers the worker.

#ifndef ECUIO_MOCK_DRIVE_CYCLE_H
#define ECUIO_MOCK_DRIVE_CYCLE_H

#include "ecuio/signal_sets.h"

#include <cmath>
#include <cstdint>

namespace ecuio_mock {

/* Scripted drive cycle for the mock transport. A vehicle model (gear
 * ratios, an airflow and torque model, tyres with weight transfer,
 * drag, and a block, coolant, oil and under-hood heat model) is driven
 * by a phase script. The aim is readings that look like a real ECU's
 * log of what the car is doing, so the signals stay consistent with
 * each other: rpm with speed and gear, airflow with load and lambda,
 * temperatures with the heat that went in.
 *
 *   WarmUp      cold start: fast idle, open-loop fueling (first loop only)
 *   Roll2nd     pull away in 1st, shift to 2nd, roll at ~27 km/h (~1700 rpm)
 *   Pull2nd     WOT in 2nd to the rev limiter (~113 km/h)
 *   Limiter2nd  5 s on the fuel-cut limiter
 *   Slow2nd     lift (decel fuel cut) and brake to 45 km/h, shift to 3rd
 *   Settle3rd   light throttle in 3rd (~1900 rpm)
 *   Pull3rd     WOT in 3rd to the limiter (~163 km/h). Roughly twice as
 *               long as the 2nd-gear pull.
 *   Limiter3rd  5 s on the limiter
 *   Lift3rd     lift in 3rd, engine braking and light brakes, shift to
 *               4th near 110 km/h
 *   Cruise4th   steady 100 km/h in 4th, closed-loop fueling
 *   Stop        brake to a standstill, clutch in near idle
 *   Stage       idle at the line
 *   Launch      launch control: WOT, clutch in, ~4000 rpm two-step
 *   DragRun     standing 1/4 mile: clutch dump, wheelspin the driver
 *               feathers out, upshifts at 6800 rpm through 4th
 *   Shutdown    lift past the line, brake to a stop
 *   Cooldown    idle, then loop back to Roll2nd
 *
 * Only the first loop starts cold. Each later one starts from where the
 * last ended, so the first loop's runs are on a cold engine, the second
 * warms it through the thermostat, and from the third on it runs at
 * operating temperature: coolant 90-100 C (195-213 F), oil building to
 * 110-118 C (230-245 F) under the repeated pulls.
 *
 * Where the values come from. The cold start (fast idle hunting around
 * 1430 rpm for about 20 s, then 850-900 rpm), warm idle airflow and
 * load, and the warm-up rate follow the first vehicle session
 * (docs/bench-sessions/2026-10-22-vehicle.md), scaled to this engine's
 * 4.5 L. No car has been logged at WOT or on the limiter through
 * QtTune; those states follow the model's physics and the usual
 * signatures (fuel cut leaves airflow up and lambda lean, a NA engine's
 * intake air stays near ambient at speed). So do the warm temperatures:
 * a thermostat that opens at 84 C, coolant near 220 F on hard runs, and
 * oil at what an NA engine without an oil cooler reaches on a track.
 *
 * Nothing a real car reports holds dead still, so neither does this
 * one. The idle controller hunts, the driver's foot can't hold a speed
 * to the km/h, the road rises and falls a little, and the wideband's
 * closed loop wanders. The driver moves the pedal at a human rate
 * (a stab to WOT, a lift, otherwise about half the travel a second)
 * and lifts for every shift. All of the variation is deterministic
 * (tick_noise, drift), so the tests stay exact.
 *
 * One step() per mock frame, each worth kTickS of simulated time. The
 * worker keeps the frames kTickS apart on average (mock_transport.cpp),
 * so the cycle plays in real time against the frames' timestamps. */

constexpr float kTickS = 0.05f;
constexpr float kPi = 3.14159265f;
constexpr float kG = 9.81f;

/* Vehicle: a ~1500 kg, ~330 hp rear-drive coupe, manual gearbox, on
 * street tyres. */
constexpr float kMassKg = 1500.0f;
constexpr float kTyreRadiusM = 0.33f;
constexpr float kFinalDrive = 3.73f;
constexpr float kDrivelineEff = 0.88f;
constexpr float kAeroCoeff = 0.42f;          /* 0.5 * rho * Cd * A */
constexpr float kRollingN = 177.0f;          /* 0.012 * m * g */
constexpr float kGearRatio[5] = {0.0f, 3.36f, 2.07f, 1.43f, 1.00f};
constexpr int kTopGear = 4;

/* Tyres and weight transfer: the rear axle's load grows with
 * acceleration (m * a * h / L), and so does what it can put down. */
constexpr float kRearStaticFrac = 0.52f;
constexpr float kCgOverWheelbase = 0.185f;   /* 0.50 m CG height, 2.70 m */
constexpr float kMuPeak = 1.0f;
constexpr float kMuSlide = 0.85f;
constexpr float kPeakSlip = 0.12f;
/* Rotating inertia, kg*m^2: crank, flywheel and clutch; both rear
 * wheels with the axle. Reflected to the road it adds mass in the low
 * gears and soaks up the torque that would otherwise spin the tyres. */
constexpr float kEngineInertia = 0.20f;
constexpr float kWheelInertia = 3.0f;

/* Engine: 4.5 L NA, 400 N*m at 4500 rpm, ~330 hp at 7000. */
constexpr float kDisplacementL = 4.5f;
constexpr float kAirDensityGpl = 1.18f;
constexpr float kIdleRpm = 780.0f;           /* warm */
constexpr float kFastIdleRpm = 1430.0f;      /* cold start strategy */
constexpr float kFastIdleS = 17.0f;
constexpr float kFastIdleEndS = 1.5f;        /* blend down to the warm-up idle */
constexpr float kRevLimitRpm = 7000.0f;
/* Where the limiter engages. A touch under the limit, because rpm
 * pinned at the limit can round to just below it. */
constexpr float kLimiterOnRpm = kRevLimitRpm - 10.0f;
constexpr float kShiftRpm = 6800.0f;         /* drag-run upshift point */
constexpr float kLaunchRpm = 4000.0f;        /* launch-control two-step */
constexpr float kClutchInRpm = 1300.0f;      /* clutch in below this */
constexpr float kDfcoOnRpm = 1400.0f;        /* decel fuel cut, closed throttle */
constexpr float kDfcoOffRpm = 1200.0f;
constexpr float kPeakTorqueNm = 400.0f;

/* Absolute throttle position (J1979 PID 0x11) never reads 0 or 100:
 * the closed blade and the WOT stop sit inside the sensor's range. */
constexpr float kTpClosedPct = 14.5f;
constexpr float kTpWotPct = 86.0f;

/* Fueling. Lambda is what a wideband sensor reports: 1.0 in closed
 * loop, rich under power enrichment, pegged lean when fuel is cut. */
constexpr float kAfrStoich = 14.7f;
constexpr float kFuelKjPerG = 44.0f;
constexpr float kLambdaWot = 0.86f;
constexpr float kLambdaColdOpenLoop = 0.95f;
constexpr float kLambdaLean = 1.99f;         /* sensor at the top of its range */
constexpr float kO2ReadyS = 25.0f;           /* closed loop from here */

/* Temperatures, degC. */
constexpr float kAmbientC = 24.0f;
constexpr float kColdStartC = 20.0f;
constexpr float kThermostatOpenC = 84.0f;
constexpr float kThermostatFullC = 96.0f;
constexpr float kFanOnC = 102.0f;
constexpr float kFanOffC = 98.0f;

constexpr float kQuarterMileM = 402.336f;

/* The road: a gentle grade, at most this fraction either way, that
 * changes over about this distance. The drag strip is flat. */
constexpr float kGradeMax = 0.012f;
constexpr float kGradeLengthM = 150.0f;

/* The driver's pedal, percent per tick: a stab to WOT and a lift are
 * quick, anything else about half the travel a second. */
constexpr float kPedalStabPct = 20.0f;
constexpr float kPedalLiftPct = 25.0f;
constexpr float kPedalEasePct = 2.5f;

constexpr uint32_t ticks_for(float seconds)
{
    return static_cast<uint32_t>(seconds / kTickS + 0.5f);
}

/* Upshift times: flat out on the strip, and an ordinary change on the
 * road, where the engine falls off the old gear's speed more slowly */
constexpr uint32_t kShiftTicks = ticks_for(0.3f);
constexpr uint32_t kRoadShiftTicks = ticks_for(0.7f);
constexpr uint32_t kDfcoDelayTicks = ticks_for(0.5f);
constexpr uint32_t kLimiterTicks = ticks_for(5.0f);

inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* Piecewise linear lookup, clamped at both ends. */
template <int N>
inline float curve(const float (&x)[N], const float (&y)[N], float v)
{
    if (v <= x[0]) {
        return y[0];
    }
    for (int i = 1; i < N; ++i) {
        if (v <= x[i]) {
            const float f = (v - x[i - 1]) / (x[i] - x[i - 1]);
            return y[i - 1] + f * (y[i] - y[i - 1]);
        }
    }
    return y[N - 1];
}

/* Full-throttle torque curve, N*m. */
inline float torque_nm(float rpm)
{
    static const float kRpm[] = {0.0f, 1000.0f, 2000.0f, 3000.0f,
                                 4500.0f, 6000.0f, 7000.0f};
    static const float kNm[] = {0.0f, 220.0f, 300.0f, 360.0f,
                                400.0f, 380.0f, 330.0f};
    return curve(kRpm, kNm, rpm);
}

/* Volumetric efficiency at WOT, which sets the most air the engine can
 * draw at an rpm. Shaped like the torque curve. */
inline float ve_wot(float rpm)
{
    static const float kRpm[] = {0.0f, 1000.0f, 2000.0f, 3000.0f,
                                 4500.0f, 6000.0f, 7000.0f};
    static const float kVe[] = {0.70f, 0.80f, 0.86f, 0.91f,
                                0.95f, 0.90f, 0.82f};
    return curve(kRpm, kVe, rpm);
}

/* Airflow at WOT, g/s: intake strokes per second (rpm / 120) times
 * displacement times VE times air density. */
inline float maf_wot_gs(float rpm)
{
    return (rpm / 120.0f) * kDisplacementL * ve_wot(rpm) * kAirDensityGpl;
}

/* The load (airflow over the WOT airflow at that rpm) the engine needs
 * to make no torque: friction, pumping, accessories, and at idle the
 * spark retard kept in reserve. Calculated load at a warm idle reads
 * near 29 %, as on the first vehicle, and a light cruise near 20 %. */
inline float zero_torque_load(float rpm)
{
    static const float kRpm[] = {780.0f, 1500.0f, 2500.0f, 4000.0f, 7000.0f};
    static const float kLoad[] = {0.27f, 0.16f, 0.08f, 0.07f, 0.10f};
    return curve(kRpm, kLoad, rpm);
}

/* Closed-throttle airflow, g/s: the idle air path and leakage. */
inline float closed_air_gs(float rpm)
{
    return 8.0f + 0.6f * rpm / 1000.0f;
}

/* Airflow as a fraction of WOT for a blade opening (0..1). A small
 * opening fills the cylinders at low rpm and starves them at high
 * rpm, so the curve flattens with rpm. */
inline float airflow_frac(float blade, float rpm)
{
    const float r = std::fmax(rpm, 600.0f);
    const float closed = clampf(closed_air_gs(r) / maf_wot_gs(r), 0.0f, 1.0f);
    const float n = 1.0f + 6000.0f / r;
    const float h = 1.0f - std::pow(1.0f - clampf(blade, 0.0f, 1.0f), n);
    return closed + (1.0f - closed) * h;
}

/* Torque with fuel cut: friction and pumping, rising with rpm. */
inline float fuel_cut_drag_nm(float rpm)
{
    return 20.0f + 6.0f * rpm / 1000.0f;
}

/* Engine rpm per m/s of driven-wheel speed in a gear (clutch locked). */
inline float rpm_per_mps(int gear)
{
    return kGearRatio[gear] * kFinalDrive * 60.0f / (2.0f * kPi * kTyreRadiusM);
}

/* Wheel force per N*m at the crank in a gear. */
inline float wheel_n_per_nm(int gear)
{
    return kGearRatio[gear] * kFinalDrive * kDrivelineEff / kTyreRadiusM;
}

/* The engine and wheels' inertia as mass at the road, kg. */
inline float rotating_mass_kg(int gear)
{
    const float g = kGearRatio[gear] * kFinalDrive;
    return (kEngineInertia * g * g + kWheelInertia) /
           (kTyreRadiusM * kTyreRadiusM);
}

/* Tyre friction past the traction limit: peak at kPeakSlip, falling to
 * sliding friction as the tyre spins up. */
inline float tyre_mu(float slip)
{
    return kMuPeak -
           (kMuPeak - kMuSlide) * clampf((slip - kPeakSlip) / 0.3f, 0.0f, 1.0f);
}

/* Deterministic noise in [-1, 1] for a tick: the same cycle every run,
 * so the tests stay exact. */
inline float tick_noise(uint64_t tick, uint32_t salt)
{
    uint32_t x = static_cast<uint32_t>(tick) * 2654435761u ^ (salt * 0x9E3779B9u);
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f;
}

/* Smooth deterministic wander in [-1, 1]: tick_noise at knots `period`
 * apart (seconds, or metres for the road), eased between them. */
inline float drift(float t, float period, uint32_t salt)
{
    const float x = t / period;
    const float k = std::floor(x);
    const float f = x - k;
    const float s = f * f * (3.0f - 2.0f * f);
    const uint64_t i = static_cast<uint64_t>(static_cast<int64_t>(k));
    return tick_noise(i, salt) * (1.0f - s) + tick_noise(i + 1, salt) * s;
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
    Lift3rd,
    Cruise4th,
    Stop,
    Stage,
    Launch,
    DragRun,
    Shutdown,
    Cooldown,
};

struct DriveCycle {
    /* Outputs in physical units; encode_payload() reads these. */
    float rpm = kFastIdleRpm;  /* the engine has just started */
    float speed_kmh = 0.0f;    /* driven wheels, as the ECU's VSS reads it */
    float throttle_pct = kTpClosedPct;   /* absolute TP, PID 0x11 */
    float load_pct = 0.0f;     /* calculated load, PID 0x04 */
    float coolant_c = kColdStartC;
    float iat_c = kColdStartC;
    float maf_gs = 0.0f;
    float oil_c = kColdStartC;
    float lambda = kLambdaColdOpenLoop;

    /* Script and drivetrain state. */
    Phase phase = Phase::WarmUp;
    uint32_t phase_ticks = 0;
    int gear = 0;              /* 0 = neutral or clutch in */
    bool clutch_slip = false;  /* pulling away: clutch slipping at slip_rpm */
    float slip_rpm = 0.0f;
    uint32_t shift_ticks = 0;  /* > 0 while an upshift is in progress */
    bool spinning = false;     /* driven tyres past their traction limit */
    bool fuel_cut = false;     /* no fuel this tick: limiter, two-step, DFCO */
    float v_mps = 0.0f;        /* ground speed */
    float wheel_mps = 0.0f;    /* driven-wheel surface speed */
    float accel_mps2 = 0.0f;
    float blade = 0.0f;        /* throttle blade opening, 0..1 */
    float airflow = 0.0f;      /* airflow fraction of WOT, 0..1 */
    float block_c = kColdStartC;
    float underhood_c = kColdStartC;
    float thermostat_open = 0.0f;   /* 0..1 */
    bool fan_on = false;
    uint64_t tick = 0;

    /* Drag-run bookkeeping. */
    float run_s = 0.0f;
    float distance_m = 0.0f;
    float drag_et_s = 0.0f;
    float drag_trap_kmh = 0.0f;
    uint32_t loops = 0;        /* completed loops back to Roll2nd */

    float phase_s() const { return static_cast<float>(phase_ticks) * kTickS; }
    float run_time_s() const { return static_cast<float>(tick) * kTickS; }
    bool shifting() const { return shift_ticks > 0; }

    /* Driven-wheel slip over ground speed. */
    float slip() const
    {
        return (wheel_mps - v_mps) / std::fmax(v_mps, 2.0f);
    }

    void step()
    {
        /* The driver's foot moves at a human rate toward what the
         * script asks for */
        const float want = drive_script();
        if (want > pedal_) {
            pedal_ = std::fmin(want, pedal_ + ((want >= 90.0f) ? kPedalStabPct
                                                              : kPedalEasePct));
        } else {
            pedal_ = std::fmax(want, pedal_ - kPedalLiftPct);
        }
        update_vehicle(pedal_);
        ++phase_ticks;
        ++tick;
    }

private:
    float brake_mps2_ = 0.0f;  /* service-brake request for this tick */
    float rev_cap_ = kRevLimitRpm;
    bool dfco_ = false;
    uint32_t lift_ticks_ = 0;  /* consecutive ticks with the pedal released */
    float pedal_ = 0.0f;       /* the driver's pedal, percent */
    float hold_i_ = 0.0f;      /* the speed hold's integral, percent of blade */
    float slip_gain_ = 0.4f;   /* how fast the slipping clutch pulls rpm */
    float slip_rise_ = 0.0f;   /* rpm/s the driver lets the slip rpm climb */
    float slip_s_ = 0.0f;      /* time the clutch has been slipping */
    uint32_t shift_len_ = kShiftTicks;
    float shift_from_rpm_ = 0.0f;
    float odo_m_ = 0.0f;       /* distance along the road, for its grade */

    void enter(Phase next)
    {
        phase = next;
        phase_ticks = 0;
        /* A new speed hold starts from the blade as it is, so the pedal
         * doesn't jump when the script hands over */
        hold_i_ = blade * 100.0f;
        switch (next) {
        case Phase::Roll2nd:
            /* Easing the clutch out from idle */
            pull_away(1, 1250.0f, 0.15f, 150.0f);
            break;
        case Phase::Launch:
            gear = 0;           /* 1st selected, clutch in */
            break;
        case Phase::DragRun:
            /* Off the two-step: the clutch takes up the drive and pulls
             * the engine down to ~3500 rpm while it slips */
            pull_away(1, 3500.0f, 0.4f, 300.0f);
            run_s = 0.0f;
            distance_m = 0.0f;
            break;
        default:
            break;
        }
    }

    /* The clutch slips from at_rpm, which the engine reaches at `gain`
     * a tick and which climbs `rise` rpm/s as the driver feeds it in,
     * until the wheels' speed catches up and it locks */
    void pull_away(int g, float at_rpm, float gain, float rise)
    {
        gear = g;
        clutch_slip = true;
        slip_rpm = at_rpm;
        slip_gain_ = gain;
        slip_rise_ = rise;
        slip_s_ = 0.0f;
        shift_ticks = 0;
    }

    void shift_to(int g, uint32_t ticks = kRoadShiftTicks)
    {
        gear = g;
        clutch_slip = false;
        shift_ticks = ticks;
        shift_len_ = ticks;
        shift_from_rpm_ = rpm;
    }

    /* A driver holding a speed by eye: proportional and integral on the
     * speedometer, a target that wanders a km/h or so, and a foot that
     * isn't still. Returns a blade opening in percent. */
    float hold(float target_kmh, float cap_pct)
    {
        const float t = run_time_s();
        const float err = target_kmh + 1.0f * drift(t, 7.0f, 11) - speed_kmh;
        if (!shifting()) {   /* off the pedal for a shift, nothing to correct */
            hold_i_ = clampf(hold_i_ + 1.2f * err * kTickS, 0.0f, cap_pct);
        }
        const float foot = 0.7f * drift(t, 1.3f, 12) + 0.3f * drift(t, 0.37f, 13);
        return clampf(hold_i_ + 3.0f * err + foot, 0.0f, cap_pct);
    }

    /* The road's grade as a force against the car, N. Only while
     * rolling: a stationary car is on its brakes. */
    float grade_n() const
    {
        const bool strip = phase == Phase::Stage || phase == Phase::Launch ||
                           phase == Phase::DragRun;
        if (v_mps <= 0.0f || strip) {
            return 0.0f;
        }
        return kMassKg * kG * kGradeMax * drift(odo_m_, kGradeLengthM, 51);
    }

    /* The driver: picks throttle, brake and gear for this tick and
     * advances the script. Returns the blade request in percent. */
    float drive_script()
    {
        float thr = 0.0f;
        brake_mps2_ = 0.0f;
        rev_cap_ = kRevLimitRpm;

        switch (phase) {
        case Phase::WarmUp:
            if (phase_ticks >= ticks_for(20.0f)) {
                enter(Phase::Roll2nd);
            }
            break;
        case Phase::Roll2nd:
            /* Away in 1st, gathering speed to a roll in 2nd */
            thr = hold(27.0f * clampf(phase_s() / 3.5f, 0.0f, 1.0f), 25.0f);
            if (clutch_slip) {
                thr = std::fmax(thr, 10.0f);   /* feeding the clutch */
            }
            if (gear == 1 && !clutch_slip && rpm >= 2300.0f) {
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
                enter(phase == Phase::Limiter2nd ? Phase::Slow2nd
                                                 : Phase::Lift3rd);
            }
            break;
        case Phase::Slow2nd:
            brake_mps2_ = clampf(phase_s() * 4.0f, 0.0f, 3.0f);
            if (speed_kmh <= 45.0f) {
                shift_to(3);
                enter(Phase::Settle3rd);
            }
            break;
        case Phase::Settle3rd:
            thr = hold(45.0f, 35.0f);
            if (!shifting() && phase_ticks >= ticks_for(3.0f)) {
                enter(Phase::Pull3rd);
            }
            break;
        case Phase::Lift3rd:
            brake_mps2_ = clampf(phase_s() * 2.0f, 0.0f, 1.5f);
            if (gear == 3 && speed_kmh <= 110.0f) {
                shift_to(4);
                enter(Phase::Cruise4th);
            }
            break;
        case Phase::Cruise4th:
            thr = hold(100.0f, 40.0f);
            if (phase_ticks >= ticks_for(15.0f)) {
                enter(Phase::Stop);
            }
            break;
        case Phase::Stop:
        case Phase::Shutdown:
            /* Firm after the drag run, an ordinary stop otherwise, eased
             * off in the last few km/h so the car doesn't pitch */
            brake_mps2_ = clampf(phase_s() * 4.0f,
                                 0.0f, (phase == Phase::Stop) ? 3.5f : 4.0f) *
                          (0.5f + 0.5f * clampf(v_mps / 3.0f, 0.0f, 1.0f));
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
            rev_cap_ = kLaunchRpm;
            if (phase_ticks >= ticks_for(1.5f)) {
                enter(Phase::DragRun);
            }
            break;
        case Phase::DragRun:
            /* Full throttle, feathered while the tyres spin: the driver
             * holds the slip near the tyre's peak. */
            thr = spinning
                ? clampf(100.0f - 800.0f * (slip() - 0.08f), 25.0f, 100.0f)
                : 100.0f;
            if (!clutch_slip && !shifting() && gear < kTopGear &&
                rpm >= kShiftRpm) {
                shift_to(gear + 1, kShiftTicks);
            }
            if (distance_m >= kQuarterMileM) {
                drag_et_s = run_s;
                drag_trap_kmh = v_mps * 3.6f;
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
        if (shifting()) {
            thr = 0.0f;   /* the driver lifts for every shift */
        }
        return thr;
    }

    /* Idle speed target: the cold-start strategy's fast idle for the
     * first kFastIdleS, then a warm-up idle that falls as the coolant
     * warms. */
    float idle_target() const
    {
        const float warm = kIdleRpm +
                           90.0f * clampf((70.0f - coolant_c) / 40.0f, 0.0f, 1.0f);
        return warm + (kFastIdleRpm - warm) * fast_idle_weight();
    }

    /* 1 during the cold-start strategy's fast idle, easing to 0 over
     * kFastIdleEndS */
    float fast_idle_weight() const
    {
        return clampf(1.0f - (run_time_s() - kFastIdleS) / kFastIdleEndS, 0.0f, 1.0f);
    }

    /* The idle controller never sits still: it hunts slowly around its
     * target, more on the cold fast idle (the first vehicle's held
     * 1400-1460 rpm), with a little firing-to-firing jitter on top. */
    float idle_hunt() const
    {
        const float t = run_time_s();   /* the jitter's 8 is ~3 after the 0.35 lag */
        return (12.0f + 10.0f * fast_idle_weight()) * drift(t, 1.6f, 31) +
               4.0f * drift(t, 0.4f, 32) + 8.0f * tick_noise(tick, 33);
    }

    /* Drivetrain, airflow, fueling and temperatures for one tick. */
    void update_vehicle(float thr_req)
    {
        /* Blade travel: a full sweep takes 0.25 s. */
        const float req = clampf(thr_req / 100.0f, 0.0f, 1.0f);
        blade += clampf(req - blade, -0.2f, 0.2f);

        const bool limiter = phase == Phase::Limiter2nd || phase == Phase::Limiter3rd;
        float fuel_duty = 1.0f;   /* share of the airflow that's fueled */
        float lambda_cmd = 1.0f;

        /* Decel fuel cut: the pedal released for half a second above
         * kDfcoOnRpm, with hysteresis, in gear or with the clutch in.
         * A light throttle, however small, keeps the fuel on. */
        lift_ticks_ = (req <= 0.0f) ? lift_ticks_ + 1 : 0;
        if (lift_ticks_ == 0 || rpm < kDfcoOffRpm) {
            dfco_ = false;
        } else if (lift_ticks_ >= kDfcoDelayTicks && rpm > kDfcoOnRpm && v_mps > 0.5f) {
            dfco_ = true;
        }

        const float resist = kAeroCoeff * v_mps * v_mps +
                             ((v_mps > 0.0f) ? kRollingN : 0.0f) + grade_n();

        if (limiter) {
            /* Fuel cut, not throttle: the blade stays open and the engine
             * keeps pumping air. The engine only needs road-load torque,
             * so it spends most of each ~150 ms cycle in cut. Crank speed
             * saws against the driveline's compliance while road speed
             * holds where the mean rpm puts it. */
            static const float kSaw[3] = {7000.0f, 6925.0f, 6860.0f};
            const float k = rpm_per_mps(gear);
            float target;
            if (phase_ticks == 0) {
                target = kRevLimitRpm + 60.0f;   /* first contact, before the cut lands */
            } else if (phase_ticks == 1) {
                target = kRevLimitRpm - 180.0f;  /* first bounce, the deepest */
            } else {
                target = kSaw[(phase_ticks - 2) % 3];
            }
            rpm = target + 8.0f * tick_noise(tick, 1);
            const float v_mean = (kSaw[0] + kSaw[1] + kSaw[2]) / 3.0f / k;
            v_mps += (v_mean - v_mps) * 0.1f;
            wheel_mps = v_mps * (1.0f + 0.0015f * (rpm - 6928.0f) / 70.0f);
            accel_mps2 = 0.0f;
            spinning = false;
            fuel_cut = phase_ticks < 2 || (phase_ticks - 2) % 3 != 0;
            airflow = clampf(0.985f - 0.015f * (rpm - 6928.0f) / 70.0f, 0.0f, 1.0f);
            const float road_kw = resist * v_mps / 1000.0f;
            const float wot_kw = torque_nm(rpm) * rpm * 2.0f * kPi / 60.0f / 1000.0f;
            fuel_duty = clampf(road_kw / (wot_kw * kDrivelineEff), 0.05f, 1.0f);
            lambda_cmd = fuel_cut ? kLambdaLean : kLambdaWot;
        } else if (gear == 0 || shifting()) {
            /* Neutral, clutch in, or mid-shift: rpm follows the blade and
             * the idle controller; the car rolls on its own. */
            if (shifting()) {
                /* Off the old gear's speed and onto the new one's, eased
                 * at both ends, so the clutch takes up without a step */
                --shift_ticks;
                const float p = 1.0f - static_cast<float>(shift_ticks) /
                                           static_cast<float>(shift_len_);
                const float s = p * p * (3.0f - 2.0f * p);
                rpm = shift_from_rpm_ +
                      (wheel_mps * rpm_per_mps(gear) - shift_from_rpm_) * s;
            } else {
                /* The driver's pedal, not the blade: at a cold start the
                 * ECU opens the blade itself */
                const float pedal = clampf(thr_req / 100.0f, 0.0f, 1.0f);
                float target = idle_target() + std::fmin(pedal, blade) * 6000.0f;
                if (pedal <= 0.0f) {
                    target += idle_hunt();
                }
                /* Unloaded, the engine spins up fast but falls no faster
                 * than its friction slows it, ~2400 rpm/s */
                rpm += std::fmax((clampf(target, 0.0f, rev_cap_) - rpm) * 0.35f, -120.0f);
            }
            const bool two_step = phase == Phase::Launch && rpm >= rev_cap_ - 40.0f;
            if (two_step) {
                /* Launch control cuts fuel at the cap: rpm saws below it */
                rpm = rev_cap_ - 60.0f - 60.0f * std::fabs(tick_noise(tick, 2));
            }
            fuel_cut = two_step || dfco_;
            const float w = (gear == 0 && thr_req <= 0.0f) ? fast_idle_weight() : 0.0f;
            if (w > 0.0f) {
                /* Catalyst heating: the ECU opens the blade and retards
                 * the spark, so the cold engine idles fast on more air.
                 * The blade moves with the idle controller, and hands the
                 * air back to the idle path as the fast idle ends. */
                const float t = run_time_s();
                blade = w * (0.35f + 0.05f * drift(t, 2.3f, 21));
                airflow = (1.0f - w) * airflow_frac(0.0f, rpm) +
                          w * (0.50f + 0.03f * drift(t, 1.6f, 31));
            } else {
                airflow = airflow_frac(blade, rpm);
            }
            lambda_cmd = fuel_cut ? kLambdaLean
                       : (airflow > 0.85f ? kLambdaWot : 1.0f);
            /* Rolling with no drive: road load and brakes only, the
             * wheels' inertia but not the engine's */
            const float m = kMassKg + kWheelInertia / (kTyreRadiusM * kTyreRadiusM);
            accel_mps2 = -resist / m - brake_mps2_;
            v_mps = std::fmax(v_mps + accel_mps2 * kTickS, 0.0f);
            wheel_mps = v_mps;
            spinning = false;
            if (gear > 0 && !shifting()) {
                rpm = wheel_mps * rpm_per_mps(gear);   /* the clutch is back in */
            }
        } else {
            drive_in_gear();
            lambda_cmd = fuel_cut ? kLambdaLean
                       : (airflow > 0.85f ? kLambdaWot : 1.0f);
        }

        if (rpm < 0.0f) {
            rpm = 0.0f;
        }
        speed_kmh = wheel_mps * 3.6f;
        odo_m_ += v_mps * kTickS;
        if (phase == Phase::DragRun) {
            run_s += kTickS;
            distance_m += v_mps * kTickS;
        }

        /* Calculated load is the airflow over the WOT airflow at this
         * rpm (J1979 PID 0x04). MAF has a little sensor noise, more at
         * high flow, and the ECU computes load from it, so load carries
         * some of the same noise. */
        const float noise = (0.005f + 0.015f * airflow) * tick_noise(tick, 3);
        load_pct = clampf(airflow * (1.0f + 0.5f * noise) * 100.0f, 0.0f, 100.0f);
        maf_gs = maf_wot_gs(rpm) * airflow * (1.0f + noise);
        throttle_pct = kTpClosedPct + (kTpWotPct - kTpClosedPct) * blade;

        update_lambda(lambda_cmd);

        /* Fuel burned, kW. On the limiter the refires burn at the WOT
         * mixture for the share of the time road load needs. */
        const float fuel_lambda = limiter ? kLambdaWot : lambda_cmd;
        const float fuel_kw = (fuel_cut && !limiter)
            ? 0.0f
            : fuel_duty * maf_gs / (kAfrStoich * fuel_lambda) * kFuelKjPerG;
        update_temperatures(fuel_kw);
    }

    /* Clutch engaged (or slipping), in gear: engine torque through the
     * driveline to the tyres, wheelspin past the traction limit. */
    void drive_in_gear()
    {
        const float k = rpm_per_mps(gear);
        const float locked = wheel_mps * k;
        const float slip_target = slip_rpm + slip_rise_ * slip_s_ +
                                  25.0f * drift(run_time_s(), 0.3f, 61);
        if (clutch_slip && locked < slip_target) {
            rpm += (slip_target - rpm) * slip_gain_;
            slip_s_ += kTickS;
        } else {
            clutch_slip = false;
            rpm = locked;
        }

        airflow = airflow_frac(blade, rpm);
        /* The rev limit cuts fuel wherever the car is in the script: a
         * lift off the limiter still meets it while the blade closes */
        fuel_cut = dfco_ || (!clutch_slip && rpm >= kLimiterOnRpm);
        float tq;
        if (fuel_cut) {
            tq = -fuel_cut_drag_nm(rpm);
        } else {
            const float zl = zero_torque_load(rpm);
            tq = torque_nm(rpm) * (airflow - zl) / (1.0f - zl);
        }
        float drive_n = (tq >= 0.0f)
            ? tq * wheel_n_per_nm(gear)
            : tq * wheel_n_per_nm(gear) / (kDrivelineEff * kDrivelineEff);

        const float resist = kAeroCoeff * v_mps * v_mps +
                             ((v_mps > 0.0f) ? kRollingN : 0.0f) + grade_n();
        const float rear_n = kMassKg * (kG * kRearStaticFrac +
                                        std::fmax(accel_mps2, 0.0f) * kCgOverWheelbase);
        const float grip_n = kMuPeak * rear_n;
        /* A slipping clutch leaves the engine's inertia out of it */
        const float m_rot = clutch_slip
            ? kWheelInertia / (kTyreRadiusM * kTyreRadiusM)
            : rotating_mass_kg(gear);

        if (clutch_slip && drive_n >= grip_n) {
            /* The driver feeds the clutch in, holding the tyres at about
             * their peak slip: the car takes all the grip there is */
            spinning = true;
            accel_mps2 = (grip_n - resist) / kMassKg - brake_mps2_;
            v_mps = std::fmax(v_mps + accel_mps2 * kTickS, 0.0f);
            wheel_mps = v_mps + std::fmax(v_mps, 2.0f) * 0.8f * kPeakSlip;
        } else if (spinning || drive_n > grip_n) {
            spinning = true;
            const float tyre_n = tyre_mu(slip()) * rear_n;
            wheel_mps += (drive_n - tyre_n) / m_rot * kTickS;
            accel_mps2 = (tyre_n - resist) / kMassKg - brake_mps2_;
            v_mps = std::fmax(v_mps + accel_mps2 * kTickS, 0.0f);
            /* Hooked up again once the wheels are back at road speed and
             * the drive no longer exceeds the grip */
            if (wheel_mps <= v_mps + 0.05f) {
                wheel_mps = v_mps;
                spinning = drive_n > grip_n;
            }
        } else {
            accel_mps2 = (drive_n - resist) / (kMassKg + m_rot) - brake_mps2_;
            v_mps = std::fmax(v_mps + accel_mps2 * kTickS, 0.0f);
            wheel_mps = v_mps;
        }
        /* In gear the limiter caps wheel speed, so rpm never overshoots
         * it before the limiter phase takes over. */
        if (!clutch_slip) {
            const float cap = kRevLimitRpm / k;
            if (wheel_mps > cap) {
                wheel_mps = cap;
                v_mps = std::fmin(v_mps, cap);
            }
            /* The ECU's crank-speed reading carries a few rpm of firing-
             * pulse jitter even with the driveline locked */
            rpm = std::fmin(wheel_mps * k + 4.0f * tick_noise(tick, 7), kRevLimitRpm);
        }
    }

    /* Wideband lambda: a command, and a sensor that follows it in about
     * 70 ms. Closed loop wanders a percent or so around 1.00 as the trims
     * chase it; before the sensor is warm the cold start runs a little
     * rich; power enrichment isn't exact either. */
    void update_lambda(float cmd)
    {
        const float t = run_time_s();
        float target = cmd;
        if (cmd == 1.0f) {
            target = (t < kO2ReadyS)
                ? kLambdaColdOpenLoop + 0.003f * drift(t, 1.1f, 41)
                : 1.0f + 0.012f * drift(t, 0.7f, 42) + 0.004f * tick_noise(tick, 43);
        } else if (cmd == kLambdaWot) {
            target = kLambdaWot + 0.01f * drift(t, 0.9f, 44);
        }
        lambda += (target - lambda) * 0.7f;
    }

    /* Heat, kW and kJ/K. A quarter of the fuel's energy goes to the
     * cooling jacket, block and oil. The coolant warms quickly while the
     * thermostat is shut (the small loop) and holds near it once open;
     * at WOT the radiator can't keep up, so it climbs until the driver
     * lifts. The oil takes heat from the block, the pistons and bearing
     * friction; with the sump's mass behind it, one pull moves it a few
     * degrees, and back-to-back hard loops build it to track-day levels.
     * The block and oil store heat and give it back after a hard run
     * (heat soak); the radiator and block heat the engine bay, which
     * the intake draws from at low speed. */
    void update_temperatures(float fuel_kw)
    {
        const float heat_kw = 0.24f * fuel_kw;
        const float speed = clampf(v_mps * 3.6f, 0.0f, 160.0f);
        /* Friction power, a share of which heats the oil directly */
        const float friction_kw = fuel_cut_drag_nm(rpm) * rpm * 2.0f * kPi / 60.0f / 1000.0f;

        /* The thermostat's wax element follows the coolant in ~4 s */
        const float open_target = clampf((coolant_c - kThermostatOpenC) /
                                         (kThermostatFullC - kThermostatOpenC), 0.0f, 1.0f);
        thermostat_open += (open_target - thermostat_open) * (kTickS / 4.0f);
        const float open = thermostat_open;
        if (coolant_c >= kFanOnC) {
            fan_on = true;
        } else if (coolant_c <= kFanOffC) {
            fan_on = false;
        }
        const float ua = 0.25f + 0.006f * speed + (fan_on ? 0.9f : 0.0f);
        const float q_rad = open * ua * (coolant_c - kAmbientC);
        const float q_bc = 1.5f * (block_c - coolant_c);
        const float q_bo = 0.3f * (block_c - oil_c);
        const float q_block_loss = 0.03f * (block_c - kAmbientC);

        const float c_coolant = 40.0f + 20.0f * open;
        coolant_c += (0.45f * heat_kw + q_bc - q_rad -
                      0.015f * (coolant_c - kAmbientC)) *
                     kTickS / c_coolant;
        block_c +=
            (0.45f * heat_kw - q_bc - q_bo - q_block_loss) * kTickS / 60.0f;
        oil_c += (q_bo + 0.10f * heat_kw + 0.5f * friction_kw -
                  (0.06f + 0.001f * speed) * (oil_c - kAmbientC)) *
                 kTickS / 35.0f;

        /* The bay: the block, the exhaust manifold and part of the
         * radiator's air heat it; the car's speed ventilates it. */
        const float q_bay = q_block_loss + 0.2f * q_rad + 0.03f * fuel_kw;
        const float vent = 0.2f + 0.015f * speed;
        underhood_c += (q_bay - vent * (underhood_c - kAmbientC)) * kTickS / 25.0f;

        /* Intake air: drawn partly from the bay (less at speed, when the
         * intake gets fresh air), plus heat picked up in the manifold,
         * more when the airflow is slow. The sensor lags ~4 s. */
        const float mix = 0.8f - 0.45f * clampf(speed / 80.0f, 0.0f, 1.0f);
        const float pickup = 0.04f * (block_c - kAmbientC) / (1.0f + maf_gs / 30.0f);
        const float iat_target = kAmbientC + mix * (underhood_c - kAmbientC) + pickup;
        iat_c += (iat_target - iat_c) * (kTickS / 4.0f);
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
 *   [0..1]   RPM      LE uint16, scale 0.25
 *   [2]      coolant  uint8, offset -40
 *   [3]      speed    uint8, km/h
 *   [4]      throttle uint8, scale 100/255
 *   [5]      IAT      uint8, offset -40
 *   [6]      load     uint8, scale 100/255
 *   [7..8]   MAF      LE uint16, scale 0.01 g/s
 *   [9]      oil      uint8, offset -40
 *   [10..11] lambda   LE uint16, scale 1/32768 (J1979's equivalence ratio)
 * The transport never decodes (rule 2); the tests round-trip every
 * tick of the cycle through ecuio_mock_signal_set(). */
inline void encode_payload(const DriveCycle& s, uint8_t out[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN])
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

    const uint16_t lambda_raw = clamp_u16(s.lambda * 32768.0f);
    out[10] = static_cast<uint8_t>(lambda_raw & 0xFF);
    out[11] = static_cast<uint8_t>((lambda_raw >> 8) & 0xFF);
}

} // namespace ecuio_mock

#endif /* ECUIO_MOCK_DRIVE_CYCLE_H */
