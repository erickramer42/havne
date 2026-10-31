// File/layer: core/src/obd2_rates.h | internal, not ABI-exported; the
//             obd2 set's rate classes, included by request_setup.h
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest (the classes the obd2 set's requests
//     get, the rotation's ratio, the duplicate pair on the test car's
//     bitmaps); HARNESS: the J2534 and ELM327 rotation
//     tests (J2534AdoptionTest, Elm327SessionTest).

#ifndef ECUIO_OBD2_RATES_H
#define ECUIO_OBD2_RATES_H

#include <stdint.h>

/* ADR-0039 §3: the Service 01 PIDs the obd2 set asks in the fast class,
 * what a driver or a logged pull watches change within a second: load,
 * the short-term trims, fuel pressure in each of J1979's forms, MAP,
 * rpm, speed, timing, MAF, throttle, the wide-range sensors' lambda,
 * commanded lambda and the pedal. Everything else the set asks is slow:
 * temperatures, the other pressures, the voltage, long-term trims, the
 * narrow-band sensors, commanded throttle, torque, counters and
 * distances. */
constexpr uint8_t ECUIO_OBD2_FAST_PIDS[] = {
    0x04,                                           /* load */
    0x06, 0x08,                                     /* short-term trims */
    0x0A, 0x22, 0x23, 0x59,                         /* fuel pressure */
    0x0B, 0x0C, 0x0D, 0x0E, 0x10, 0x11,             /* MAP to throttle */
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, /* wide-range O2 (V) */
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, /* wide-range O2 (mA) */
    0x44,                                           /* commanded lambda */
    0x49,                                           /* pedal D */
};

/* ADR-0039 §3: fast PIDs that can read one sensor, as (keep, demote)
 * pairs: when a channel's bitmaps list both, the second goes slow
 * (ecuio_engine_judge_duplicates). The fuel rail's gauge (23) and
 * absolute (59) forms read one value on the test car
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md). */
constexpr uint8_t ECUIO_OBD2_DUPLICATE_PIDS[][2] = {
    {0x23, 0x59},
};

/* Whether the obd2 set asks pid in the fast class */
inline bool ecuio_obd2_pid_fast(uint8_t pid)
{
    for (uint8_t p : ECUIO_OBD2_FAST_PIDS) {
        if (p == pid) {
            return true;
        }
    }
    return false;
}

#endif /* ECUIO_OBD2_RATES_H */
