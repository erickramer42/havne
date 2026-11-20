// File/layer: core/src/mock_drive_model.h | internal, not ABI-exported; included by mock_drive_cycle.h alone
// C++17, Qt-free
// Testing: UNIT: CoreTest (test_core)
//
// The mock drive cycle's vehicle: its parameters, the model's functions
// and the phase list. DriveCycle (mock_drive_cycle.h) steps it.

#ifndef ECUIO_MOCK_DRIVE_MODEL_H
#define ECUIO_MOCK_DRIVE_MODEL_H

#include "ecuio/signal_sets.h"

#include <cmath>
#include <cstdint>

namespace ecuio_mock {

/* Scripted drive cycle for the mock transport. A vehicle model (gear
 * ratios, an airflow and torque model, tires with weight transfer,
 * drag, and a block, coolant, oil and under-hood heat model) is driven
 * by a phase script. The aim is readings that look like a real ECU's
 * log of what the car is doing, so the signals stay consistent with
 * each other: rpm with speed and gear, airflow with load and lambda,
 * temperatures with the heat that went in.
 *
 *   KeyOn       15 s with the key on and the engine stopped (0 rpm), where
 *               a clear can run (ADR-0038); after the first loop the
 *               engine runs down from idle
 *   Start       the starter cranks (~220 rpm), the engine catches and
 *               settles to its idle (2 s in all)
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
 *   DragRun     standing 1/4 mile: off the two-step on its boost, the
 *               clutch slipping with the tires at their peak, then
 *               traction control holding them at their grip; upshifts
 *               at 6800 rpm through 4th
 *   Shutdown    lift past the line, brake to a stop
 *   Cooldown    idle, then the engine stops: back to KeyOn
 *
 * Only the first loop starts cold. Each later one starts from where the
 * last ended, so the first loop's runs are on a cold engine and the
 * second warms it through the thermostat. From the third on the coolant
 * runs 87-102 C (189-216 F), the fan taking over at 102 C on the quarter
 * mile, and the oil builds over the next two loops to 107-117 C
 * (225-243 F) under the repeated pulls and the turbo's heat.
 *
 * Where the values come from. The cold start (fast idle hunting around
 * 1430 rpm for about 20 s, then 850-900 rpm), warm idle airflow and
 * load, and the warm-up rate follow the first vehicle session
 * (docs/bench-sessions/2026-10-22-vehicle.md), scaled to this engine's
 * 4.5 L. No car has been logged at WOT or on the limiter through
 * Havne; those states follow the model's physics and the usual
 * signatures (fuel cut leaves airflow up and lambda lean). So does the
 * manifold's pressure, worked out from load (manifold_kpa), and so does
 * the turbo: a street turbo's spool, a wastegate holding 17 psi, a
 * blow-off valve venting the charge on a lift, a compressor and
 * intercooler of typical efficiency setting the charge's density and the
 * heat the intake air carries, and the richer mixture a tune runs on
 * boost (boost_target_kpa, charge_fill, update_boost). So do the warm
 * temperatures: a thermostat that opens at 84 C, coolant climbing past
 * 210 F on a hard run until the fan cuts in, and oil at what an engine
 * without an oil cooler reaches on a track.
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

/* Vehicle: a ~1500 kg, ~600 hp rear-drive coupe, manual transmission, on
 * street tires, with traction control. */
constexpr float kMassKg = 1500.0f;
constexpr float kTireRadiusM = 0.33f;
constexpr float kFinalDrive = 3.73f;
constexpr float kDrivelineEff = 0.88f;
constexpr float kAeroCoeff = 0.42f;          /* 0.5 * rho * Cd * A */
constexpr float kRollingN = 177.0f;          /* 0.012 * m * g */
constexpr float kGearRatio[5] = {0.0f, 3.36f, 2.07f, 1.43f, 1.00f};
constexpr int kTopGear = 4;

/* Tires and weight transfer: the rear axle's load grows with
 * acceleration (m * a * h / L), and so does what it can put down. */
constexpr float kRearStaticFrac = 0.52f;
constexpr float kCgOverWheelbase = 0.185f;   /* 0.50 m CG height, 2.70 m */
constexpr float kMuPeak = 1.0f;
constexpr float kMuSlide = 0.85f;
constexpr float kPeakSlip = 0.12f;
/* Rotating inertia, kg*m^2: crank, flywheel and clutch; both rear
 * wheels with the axle. Reflected to the road it adds mass in the low
 * gears and soaks up the torque that would otherwise spin the tires. */
constexpr float kEngineInertia = 0.20f;
constexpr float kWheelInertia = 3.0f;

/* Engine: 4.5 L turbocharged; 400 N*m at 4500 rpm without boost
 * (torque_nm), about 750 N*m on full boost, ~600 hp at 6000. */
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

/* The start: the starter's speed, and how fast rpm flares in the first
 * second after the engine catches (rpm a tick, ~2800 rpm/s) */
constexpr float kCrankRpm = 220.0f;
constexpr float kCatchRiseRpm = 140.0f;

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

/* Heat paths (update_temperatures): how the fuel's cooling heat splits
 * between coolant, block and oil (the shares sum to 1), the share of
 * friction power that heats the oil, the heat masses in kJ/K (the
 * coolant's grows as the thermostat opens the radiator's loop), and how
 * the radiator's conductance grows with road speed, kW/K per km/h. */
constexpr float kCoolantHeatShare = 0.50f;
constexpr float kBlockHeatShare = 0.34f;
constexpr float kOilHeatShare = 0.16f;
constexpr float kOilFrictionShare = 0.5f;
constexpr float kCoolantKjK = 30.0f;
constexpr float kCoolantOpenKjK = 15.0f;
constexpr float kOilKjK = 45.0f;
constexpr float kRadiatorUaPerKmh = 0.005f;

constexpr float kQuarterMileM = 402.336f;

/* The road: a gentle grade, at most this fraction either way, that
 * changes over about this distance. The drag strip is flat. */
constexpr float kGradeMax = 0.012f;
constexpr float kGradeLengthM = 150.0f;

/* Pressures, kPa (manifold_kpa): the outside air near sea level, what
 * the exhaust leaves in a cylinder (a fill of none still reads this
 * much), the intake's loss at a flow near the WOT peak, and the rpm
 * from which the engine pumps a full vacuum. */
constexpr float kBaroKpa = 101.3f;
constexpr float kMapResidualKpa = 15.0f;
constexpr float kIntakeLossKpa = 3.0f;
constexpr float kIntakeLossAtGs = 250.0f;
constexpr float kMapPumpRpm = 600.0f;

/* The turbo (boost_target_kpa, charge_fill): the most boost the wastegate
 * holds, 17 psi; the rpm below which the exhaust can't drive the turbine;
 * the compressor's and the intercooler's efficiencies, typical of a
 * street turbo; the share of the denser charge the cylinders take past
 * the exhaust's back-pressure; the richer mixture a tune runs at full
 * boost; and the share of boost launch control's retarded spark builds
 * on the two-step. */
constexpr float kBoostMaxKpa = 118.0f;
constexpr float kSpoolRpm = 1800.0f;
constexpr float kCompressorEff = 0.72f;
constexpr float kIntercoolerEff = 0.80f;
constexpr float kBoostFillEff = 0.80f;
constexpr float kLambdaBoost = 0.82f;
constexpr float kLaunchBoostShare = 0.45f;
/* How fast the cooling jacket's share of the fuel's heat falls with the
 * fill past WOT's (update_temperatures) */
constexpr float kJacketBoostExp = 0.8f;
/* The turbo's oil-fed bearing housing: the share of the heat the jacket
 * gives up on boost that it takes, and how long, in s, it takes to pass
 * that heat on to the oil */
constexpr float kTurboOilShare = 0.12f;
constexpr float kTurboSoakS = 60.0f;

/* The driver's pedal, percent per tick: a stab to WOT and a lift are
 * quick, anything else about half the travel a second. */
constexpr float kPedalStabPct = 20.0f;
constexpr float kPedalLiftPct = 25.0f;
constexpr float kPedalEasePct = 2.5f;

/* Rounds half up by hand: std::lround isn't constexpr in C++17, and
 * every caller passes a positive constant. */
constexpr uint32_t ticks_for(float seconds)
{
    // NOLINTNEXTLINE(bugprone-incorrect-roundings)
    return static_cast<uint32_t>(seconds / kTickS + 0.5f);
}

/* Upshift times: flat out on the strip, and an ordinary change on the
 * road, where the engine falls off the old gear's speed more slowly */
constexpr uint32_t kShiftTicks = ticks_for(0.3f);
constexpr uint32_t kRoadShiftTicks = ticks_for(0.7f);
constexpr uint32_t kDfcoDelayTicks = ticks_for(0.5f);
constexpr uint32_t kLimiterTicks = ticks_for(5.0f);
/* Battery voltage: at rest, under the starter (deepest as it engages,
 * easing as the engine turns), and the alternator's regulated output, a
 * little higher cold than hot and a little lower with the engine at idle */
constexpr float kBatteryRestV = 12.45f;
constexpr float kBatteryCrankV = 9.9f;
constexpr float kAlternatorColdV = 14.55f;
constexpr float kAlternatorHotV = 14.15f;
/* Key on with the engine stopped, cranking before it catches, and the
 * start's whole phase */
constexpr uint32_t kKeyOnTicks = ticks_for(15.0f);
constexpr uint32_t kCrankTicks = ticks_for(0.6f);
constexpr uint32_t kStartTicks = ticks_for(2.0f);

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

/* Manifold pressure, kPa (PID 0x0B). The cylinders fill in proportion
 * to the manifold's pressure above what the exhaust left in them, so a
 * load (the fill over WOT's) reads kMapResidualKpa plus that share of
 * WOT's pressure: the outside air less the intake's loss, which grows
 * with the square of the flow. A stopped engine pumps nothing, so its
 * manifold reads the outside air; one turning slower than an idle (the
 * starter, a run-down) pulls part of the vacuum. This is the throttle's
 * part, never above the outside air: the turbo's boost adds to it. */
inline float manifold_kpa(float baro_kpa, float rpm, float load_pct,
                          float maf_gs)
{
    const float flow = maf_gs / kIntakeLossAtGs;
    const float wot = baro_kpa - kIntakeLossKpa * flow * flow;
    const float fill = clampf(load_pct / 100.0f, 0.0f, 1.0f);
    const float filled = kMapResidualKpa + (wot - kMapResidualKpa) * fill;
    const float pumping = clampf(rpm / kMapPumpRpm, 0.0f, 1.0f);
    return clampf(baro_kpa + pumping * (filled - baro_kpa), 0.0f, baro_kpa);
}

/* The boost the wastegate holds at WOT, kPa over the outside air: none
 * below kSpoolRpm, where the exhaust can't drive the turbine, building
 * through the spool to kBoostMaxKpa near peak torque, and tapering as
 * the turbo runs out of flow toward the limiter. */
inline float boost_target_kpa(float rpm)
{
    static const float kRpm[] = {0.0f,    kSpoolRpm, 2500.0f, 3300.0f,
                                 4500.0f, 6000.0f,   7000.0f};
    static const float kKpa[] = {0.0f,         0.0f,   35.0f, 112.0f,
                                 kBoostMaxKpa, 112.0f, 100.0f};
    return curve(kRpm, kKpa, rpm);
}

/* The charge's heating, degC: the compressor's adiabatic rise over its
 * efficiency, of which the intercooler takes back its share. */
inline float charge_rise_c(float boost_kpa, float baro_kpa)
{
    const float pr = (baro_kpa + std::fmax(boost_kpa, 0.0f)) / baro_kpa;
    const float t_k = kAmbientC + 273.15f;
    const float rise = t_k * (std::pow(pr, 0.2857f) - 1.0f) / kCompressorEff;
    return rise * (1.0f - kIntercoolerEff);
}

/* How much more the cylinders take under boost than at WOT without it:
 * the charge's density over the outside air's (the pressure ratio, less
 * the heat the intercooler left in it), of which the exhaust's
 * back-pressure lets kBoostFillEff of the gain in. 1 with no boost. */
inline float charge_fill(float boost_kpa, float baro_kpa)
{
    const float pr = (baro_kpa + std::fmax(boost_kpa, 0.0f)) / baro_kpa;
    const float t_k = kAmbientC + 273.15f;
    const float density = pr * t_k / (t_k + charge_rise_c(boost_kpa, baro_kpa));
    return 1.0f + kBoostFillEff * (density - 1.0f);
}

/* Torque with fuel cut: friction and pumping, rising with rpm. */
inline float fuel_cut_drag_nm(float rpm)
{
    return 20.0f + 6.0f * rpm / 1000.0f;
}

/* Engine rpm per m/s of driven-wheel speed in a gear (clutch locked). */
inline float rpm_per_mps(int gear)
{
    return kGearRatio[gear] * kFinalDrive * 60.0f / (2.0f * kPi * kTireRadiusM);
}

/* Wheel force per N*m at the crank in a gear. */
inline float wheel_n_per_nm(int gear)
{
    return kGearRatio[gear] * kFinalDrive * kDrivelineEff / kTireRadiusM;
}

/* The engine and wheels' inertia as mass at the road, kg. */
inline float rotating_mass_kg(int gear)
{
    const float g = kGearRatio[gear] * kFinalDrive;
    return (kEngineInertia * g * g + kWheelInertia) /
           (kTireRadiusM * kTireRadiusM);
}

/* Tire friction past the traction limit: peak at kPeakSlip, falling to
 * sliding friction as the tire spins up. */
inline float tire_mu(float slip)
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
 * apart (seconds, or meters for the road), eased between them. */
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
    KeyOn,
    Start,
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

} // namespace ecuio_mock

#endif /* ECUIO_MOCK_DRIVE_MODEL_H */
