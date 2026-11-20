// File/layer: core/src/mock_drive_cycle.h | internal, not ABI-exported;
//             included by mock_transport.cpp (its only producer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: CoreTest (test_core)

#ifndef ECUIO_MOCK_DRIVE_CYCLE_H
#define ECUIO_MOCK_DRIVE_CYCLE_H

#include "ecuio/signal_sets.h"

#include <cmath>
#include <cstdint>

#include "mock_drive_model.h"

namespace ecuio_mock {

struct DriveCycle {
    /* Outputs in physical units; encode_payload() reads these. */
    float rpm = 0.0f;          /* the key on, the engine stopped */
    float speed_kmh = 0.0f;    /* driven wheels, as the ECU's VSS reads it */
    float throttle_pct = kTpClosedPct;   /* absolute TP, PID 0x11 */
    float load_pct = 0.0f;     /* calculated load, PID 0x04 */
    float coolant_c = kColdStartC;
    float iat_c = kColdStartC;
    float maf_gs = 0.0f;
    float oil_c = kColdStartC;
    float lambda = kLambdaLean; /* no exhaust: the sensor reads air */
    float commanded_lambda = kLambdaLean; /* PID 0x44: the ECU's command */
    float map_kpa = kBaroKpa;   /* PID 0x0B: stopped, the outside air */
    float baro_kpa = kBaroKpa;  /* PID 0x33 */
    float battery_v = kBatteryRestV; /* PID 0x42: key on, the battery */
    /* The engine turning on its own: false stopped or cranking */
    bool running = false;

    /* Script and drivetrain state. */
    Phase phase = Phase::KeyOn;
    uint32_t phase_ticks = 0;
    int gear = 0;              /* 0 = neutral or clutch in */
    bool clutch_slip = false;  /* pulling away: clutch slipping at slip_rpm */
    float slip_rpm = 0.0f;
    uint32_t shift_ticks = 0;  /* > 0 while an upshift is in progress */
    bool spinning = false;     /* driven tires past their traction limit */
    bool fuel_cut = false;     /* no fuel this tick: limiter, two-step, DFCO */
    float v_mps = 0.0f;        /* ground speed */
    float wheel_mps = 0.0f;    /* driven-wheel surface speed */
    float accel_mps2 = 0.0f;
    float blade = 0.0f;        /* throttle blade opening, 0..1 */
    float airflow = 0.0f;      /* the throttle's share of WOT's airflow, 0..1 */
    float boost_kpa = 0.0f;    /* the turbo's boost over the outside air */
    float fill = 0.0f;         /* airflow times charge_fill, past 1 on boost */
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
    uint32_t loops = 0;        /* completed loops back to KeyOn */

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
    bool closing_ = false;     /* the blade is closing: the blow-off opens */
    float hold_i_ = 0.0f;      /* the speed hold's integral, percent of blade */
    float slip_gain_ = 0.4f;   /* how fast the slipping clutch pulls rpm */
    float slip_rise_ = 0.0f;   /* rpm/s the driver lets the slip rpm climb */
    float slip_s_ = 0.0f;      /* time the clutch has been slipping */
    uint32_t shift_len_ = kShiftTicks;
    float shift_from_rpm_ = 0.0f;
    float odo_m_ = 0.0f;       /* distance along the road, for its grade */
    float turbo_oil_ = 0.0f;   /* kW the turbo's housing gives the oil */
    uint64_t catch_tick_ = 0;  /* the tick the engine last caught */
    uint64_t first_catch_tick_ = 0;
    bool caught_ = false;      /* caught once: the cold start's clock runs */

    /* Seconds since the engine first caught, the cold-start strategy's
     * clock (0 before), and since it last did */
    float cold_start_s() const
    {
        return caught_ ? static_cast<float>(tick - first_catch_tick_) * kTickS
                       : 0.0f;
    }
    float since_catch_s() const
    {
        return static_cast<float>(tick - catch_tick_) * kTickS;
    }

    void enter(Phase next)
    {
        phase = next;
        phase_ticks = 0;
        /* A new speed hold starts from the blade as it is, so the pedal
         * doesn't jump when the script hands over */
        hold_i_ = blade * 100.0f;
        switch (next) {
        case Phase::KeyOn:
            running = false; /* the driver stops the engine, key left on */
            break;
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
        case Phase::KeyOn:
            if (phase_ticks >= kKeyOnTicks) {
                enter(Phase::Start);
            }
            break;
        case Phase::Start:
            /* The starter cranks until the engine catches, and it
             * settles to its idle before the driver pulls away; the
             * first loop's cold start idles on through WarmUp */
            if (phase_ticks == kCrankTicks) {
                running = true;
                catch_tick_ = tick;
                if (!caught_) {
                    caught_ = true;
                    first_catch_tick_ = tick;
                }
            }
            if (phase_ticks >= kStartTicks) {
                enter(loops == 0 ? Phase::WarmUp : Phase::Roll2nd);
            }
            break;
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
            if (phase_ticks >= ticks_for(20.0f)) {
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
            /* Full throttle, feathered while the tires spin: the driver
             * holds the slip near the tire's peak. */
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
                enter(Phase::KeyOn);
            }
            break;
        }
        if (shifting()) {
            thr = 0.0f;   /* the driver lifts for every shift */
        }
        return thr;
    }

    /* Idle speed target: the cold-start strategy's fast idle for the
     * first kFastIdleS after the first start, then a warm-up idle that
     * falls as the coolant warms. */
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
        return clampf(1.0f - (cold_start_s() - kFastIdleS) / kFastIdleEndS,
                      0.0f, 1.0f);
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

    /* Power enrichment: richer as the boost builds */
    float power_lambda() const
    {
        return kLambdaWot + (kLambdaBoost - kLambdaWot) *
                                clampf(boost_kpa / kBoostMaxKpa, 0.0f, 1.0f);
    }

    /* The turbo. Its boost needs the throttle open past 60 % (every shift
     * lifts it shut) and follows boost_target_kpa: up at the spool's
     * rate, quicker at high rpm where the exhaust flows more; down with
     * the exhaust's energy when the fuel is cut; and at once when the
     * blade shuts on it, through the blow-off valve. */
    void update_boost()
    {
        const float open = clampf((blade - 0.6f) / 0.3f, 0.0f, 1.0f);
        float target = boost_target_kpa(rpm) * open;
        if (!running) {
            target = 0.0f;
        } else if (phase == Phase::Launch) {
            target *= kLaunchBoostShare;
        } else if (fuel_cut) {
            target *= 0.5f;
        }
        if (target >= boost_kpa) {
            const float rate =
                0.10f + 0.10f * clampf((rpm - 2500.0f) / 3000.0f, 0.0f, 1.0f);
            boost_kpa += (target - boost_kpa) * rate;
        } else if (closing_) {
            boost_kpa *= 0.3f;
        } else {
            boost_kpa += (target - boost_kpa) * 0.25f;
        }
        if (boost_kpa < 0.05f) {
            boost_kpa = 0.0f;
        }
    }

    /* Drivetrain, airflow, fueling and temperatures for one tick. */
    void update_vehicle(float thr_req)
    {
        /* Blade travel: a full sweep takes 0.25 s. */
        const float req = clampf(thr_req / 100.0f, 0.0f, 1.0f);
        const float was = blade;
        blade += clampf(req - blade, -0.2f, 0.2f);
        closing_ = blade < was;
        update_boost();

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

        if (!running) {
            /* The engine stopped: it runs down at friction's rate, or
             * turns at the starter's speed while it cranks, drawing air
             * past the closed blade unfueled; the car stands */
            const float target = (phase == Phase::Start)
                                     ? kCrankRpm + 30.0f * tick_noise(tick, 71)
                                     : 0.0f;
            rpm += clampf(target - rpm, -120.0f, 120.0f);
            fuel_cut = true;
            airflow = (rpm > 0.0f) ? airflow_frac(0.0f, rpm) : 0.0f;
            lambda_cmd = kLambdaLean;
            accel_mps2 = 0.0f;
            v_mps = 0.0f;
            wheel_mps = 0.0f;
            spinning = false;
        } else if (limiter) {
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
            const float zl = zero_torque_load(rpm);
            const float boosted =
                (airflow * charge_fill(boost_kpa, baro_kpa) - zl) / (1.0f - zl);
            const float wot_kw =
                torque_nm(rpm) * boosted * rpm * 2.0f * kPi / 60.0f / 1000.0f;
            fuel_duty = clampf(road_kw / (wot_kw * kDrivelineEff), 0.05f, 1.0f);
            lambda_cmd = fuel_cut ? kLambdaLean : power_lambda();
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
                 * than its friction slows it, ~2400 rpm/s; it flares
                 * more slowly in the second after it catches */
                const float rise =
                    (since_catch_s() < 1.0f) ? kCatchRiseRpm : 1e9f;
                rpm += std::fmin(
                    std::fmax((clampf(target, 0.0f, rev_cap_) - rpm) * 0.35f,
                              -120.0f),
                    rise);
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
                                  : (airflow > 0.85f ? power_lambda() : 1.0f);
            /* Rolling with no drive: road load and brakes only, the
             * wheels' inertia but not the engine's */
            const float m = kMassKg + kWheelInertia / (kTireRadiusM * kTireRadiusM);
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
                                  : (airflow > 0.85f ? power_lambda() : 1.0f);
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
         * rpm without boost (J1979 PID 0x04), so boost reads 100 %, as
         * on most turbo cars, and MAF and MAP carry what it adds. MAF has
         * a little sensor noise, more at high flow, and the ECU computes
         * load from it, so load carries some of the same noise. */
        fill = airflow * charge_fill(boost_kpa, baro_kpa);
        const float noise =
            (0.005f + 0.015f * std::fmin(fill, 1.0f)) * tick_noise(tick, 3);
        load_pct = clampf(fill * (1.0f + 0.5f * noise) * 100.0f, 0.0f, 100.0f);
        maf_gs = maf_wot_gs(rpm) * fill * (1.0f + noise);
        /* The throttle's part of the manifold, as without a turbo, plus
         * the boost behind it in the share the blade lets through */
        const float throttled_pct =
            clampf(airflow * (1.0f + 0.5f * noise) * 100.0f, 0.0f, 100.0f);
        map_kpa = manifold_kpa(baro_kpa, rpm, throttled_pct,
                               maf_wot_gs(rpm) * airflow * (1.0f + noise)) +
                  boost_kpa * airflow;
        throttle_pct = kTpClosedPct + (kTpWotPct - kTpClosedPct) * blade;

        update_lambda(lambda_cmd);
        update_battery();

        /* Fuel burned, kW. On the limiter the refires burn at the WOT
         * mixture for the share of the time road load needs. */
        const float fuel_lambda = limiter ? power_lambda() : lambda_cmd;
        const float fuel_kw = (fuel_cut && !limiter)
            ? 0.0f
            : fuel_duty * maf_gs / (kAfrStoich * fuel_lambda) * kFuelKjPerG;
        update_temperatures(fuel_kw);
    }

    /* Clutch engaged (or slipping), in gear: engine torque through the
     * driveline to the tires, wheelspin past the traction limit. */
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
            fill = airflow * charge_fill(boost_kpa, baro_kpa);
            tq = torque_nm(rpm) * (fill - zl) / (1.0f - zl);
        }
        float drive_n = (tq >= 0.0f)
            ? tq * wheel_n_per_nm(gear)
            : tq * wheel_n_per_nm(gear) / (kDrivelineEff * kDrivelineEff);

        const float resist = kAeroCoeff * v_mps * v_mps +
                             ((v_mps > 0.0f) ? kRollingN : 0.0f) + grade_n();
        const float rear_n = kMassKg * (kG * kRearStaticFrac +
                                        std::fmax(accel_mps2, 0.0f) * kCgOverWheelbase);
        const float grip_n = kMuPeak * rear_n;
        /* Traction control: once the clutch is in, the ECU retards the
         * spark until the tires take the drive */
        if (!clutch_slip && drive_n > grip_n) {
            drive_n = grip_n;
        }
        /* A slipping clutch leaves the engine's inertia out of it */
        const float m_rot = clutch_slip
            ? kWheelInertia / (kTireRadiusM * kTireRadiusM)
            : rotating_mass_kg(gear);

        if (clutch_slip && drive_n >= grip_n) {
            /* The driver feeds the clutch in, holding the tires at about
             * their peak slip: the car takes all the grip there is */
            spinning = true;
            accel_mps2 = (grip_n - resist) / kMassKg - brake_mps2_;
            v_mps = std::fmax(v_mps + accel_mps2 * kTickS, 0.0f);
            wheel_mps = v_mps + std::fmax(v_mps, 2.0f) * 0.8f * kPeakSlip;
        } else if (spinning || drive_n > grip_n) {
            spinning = true;
            const float tire_n = tire_mu(slip()) * rear_n;
            wheel_mps += (drive_n - tire_n) / m_rot * kTickS;
            accel_mps2 = (tire_n - resist) / kMassKg - brake_mps2_;
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

    /* Battery voltage follows what feeds it: the starter's load while it
     * cranks (the first ticks the deepest), the alternator once the engine
     * runs (regulated lower as the engine warms, and a touch lower at idle,
     * where it is turned slowly), the battery alone otherwise. It gets
     * there in a few ticks, not at once. */
    void update_battery()
    {
        const float t = run_time_s();
        float target;
        if (running) {
            const float warm = clampf((coolant_c - 20.0f) / 70.0f, 0.0f, 1.0f);
            target = kAlternatorColdV +
                     (kAlternatorHotV - kAlternatorColdV) * warm -
                     0.15f * clampf((1500.0f - rpm) / 700.0f, 0.0f, 1.0f) +
                     0.04f * drift(t, 0.8f, 81) + 0.01f * tick_noise(tick, 82);
        } else if (phase == Phase::Start) {
            target = kBatteryCrankV -
                     0.5f * clampf(1.0f - phase_s() / 0.3f, 0.0f, 1.0f) +
                     0.1f * tick_noise(tick, 83);
        } else {
            target = kBatteryRestV + 0.02f * drift(t, 3.0f, 84);
        }
        battery_v += (target - battery_v) * 0.45f;
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
            target = (cold_start_s() < kO2ReadyS)
                         ? kLambdaColdOpenLoop + 0.003f * drift(t, 1.1f, 41)
                         : 1.0f + 0.012f * drift(t, 0.7f, 42) +
                               0.004f * tick_noise(tick, 43);
        } else if (cmd < 1.0f) {
            target = cmd + 0.01f * drift(t, 0.9f, 44); /* power enrichment */
        }
        lambda += (target - lambda) * 0.7f;
        commanded_lambda = cmd;
    }

    /* Heat, kW and kJ/K. A quarter of the fuel's energy goes to the
     * cooling jacket, block and oil. The coolant warms quickly while the
     * thermostat is shut (the small loop) and holds near it once open;
     * at WOT the radiator can't keep up, so it climbs until the driver
     * lifts or the fan cuts in. The oil takes heat from the block, the
     * pistons and bearing friction; with the sump's mass behind it, one
     * pull moves it several degrees, and back-to-back hard loops build it
     * to track-day levels.
     * The block and oil store heat and give it back after a hard run
     * (heat soak); the radiator and block heat the engine bay, which
     * the intake draws from at low speed. */
    void update_temperatures(float fuel_kw)
    {
        /* The jacket's share falls as the fill rises past WOT's: more of
         * the heat leaves with the exhaust, some through the turbine */
        const float heat_kw =
            0.24f * fuel_kw / std::pow(std::fmax(fill, 1.0f), kJacketBoostExp);
        turbo_oil_ +=
            (kTurboOilShare * (0.24f * fuel_kw - heat_kw) - turbo_oil_) *
            kTickS / kTurboSoakS;
        const float speed = clampf(v_mps * 3.6f, 0.0f, 160.0f);
        /* Friction power, a share of which heats the oil directly. Fueled,
         * that share is part of the fuel's heat and comes out of the
         * block's; with the fuel cut, the car's momentum turns the engine
         * and the friction is heat on top. */
        const float friction_kw = fuel_cut_drag_nm(rpm) * rpm * 2.0f * kPi / 60.0f / 1000.0f;
        const bool fueled = fuel_kw > 0.0f;
        const float oil_friction_kw =
            fueled ? std::fmin(kOilFrictionShare * friction_kw,
                               kBlockHeatShare * heat_kw)
                   : kOilFrictionShare * friction_kw;

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
        const float ua =
            0.25f + kRadiatorUaPerKmh * speed + (fan_on ? 0.9f : 0.0f);
        /* The water pump turns with the engine: stopped, the coolant
         * stands in the radiator and the block soaks into it */
        const float pump = running ? 1.0f : 0.0f;
        const float q_rad = pump * open * ua * (coolant_c - kAmbientC);
        const float q_bc = 1.5f * (block_c - coolant_c);
        const float q_bo = 0.3f * (block_c - oil_c);
        const float q_block_loss = 0.03f * (block_c - kAmbientC);

        const float c_coolant = kCoolantKjK + kCoolantOpenKjK * open;
        coolant_c += (kCoolantHeatShare * heat_kw + q_bc - q_rad -
                      0.015f * (coolant_c - kAmbientC)) *
                     kTickS / c_coolant;
        block_c +=
            (kBlockHeatShare * heat_kw - (fueled ? oil_friction_kw : 0.0f) -
             q_bc - q_bo - q_block_loss) *
            kTickS / 60.0f;
        oil_c += (q_bo + kOilHeatShare * heat_kw + oil_friction_kw +
                  turbo_oil_ - (0.06f + 0.001f * speed) * (oil_c - kAmbientC)) *
                 kTickS / kOilKjK;

        /* The bay: the block, the exhaust manifold and part of the
         * radiator's air heat it; the car's speed ventilates it. */
        const float q_bay = q_block_loss + 0.2f * q_rad + 0.03f * fuel_kw;
        const float vent = 0.2f + 0.015f * speed;
        underhood_c += (q_bay - vent * (underhood_c - kAmbientC)) * kTickS / 25.0f;

        /* Intake air: drawn partly from the bay (less at speed, when the
         * intake gets fresh air), plus heat picked up in the manifold,
         * more when the airflow is slow, plus the charge's heat the
         * intercooler leaves under boost. The sensor lags ~4 s. */
        const float mix = 0.8f - 0.45f * clampf(speed / 80.0f, 0.0f, 1.0f);
        const float pickup = 0.04f * (block_c - kAmbientC) / (1.0f + maf_gs / 30.0f);
        const float iat_target = kAmbientC + mix * (underhood_c - kAmbientC) +
                                 pickup + charge_rise_c(boost_kpa, baro_kpa);
        iat_c += (iat_target - iat_c) * (kTickS / 4.0f);
    }
};

inline uint8_t clamp_u8(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return static_cast<uint8_t>(std::lround(v));
}

inline uint16_t clamp_u16(float v)
{
    if (v < 0.0f) return 0;
    if (v > 65535.0f) return 65535;
    return static_cast<uint16_t>(std::lround(v));
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
 *   [12]     MAP      uint8, kPa
 *   [13]     baro     uint8, kPa
 *   [14..15] battery  LE uint16, scale 0.001 V
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

    out[12] = clamp_u8(s.map_kpa);
    out[13] = clamp_u8(s.baro_kpa);

    const uint16_t battery_raw = clamp_u16(s.battery_v / 0.001f);
    out[14] = static_cast<uint8_t>(battery_raw & 0xFF);
    out[15] = static_cast<uint8_t>((battery_raw >> 8) & 0xFF);
}

} // namespace ecuio_mock

#endif /* ECUIO_MOCK_DRIVE_CYCLE_H */
