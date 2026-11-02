// File/layer: core/src/sim_canned.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest.CannedValuesAreTheFakesOwn (test_obd2: every
//          row against tests/fake_ecu.h's answer, byte for byte, and
//          the fake's Service 01 PIDs all here)
//
// The simulated ECU's canned readings (ADR-0046, ?values=canned): every
// linear J1979 PID with one plausible reading, the fakes' own
// (tests/fake_ecu.h), so a session against the simulated ECU reads what
// the J2534 and ELM327 fakes answer. Change them together. The
// readiness PIDs (01, 41) and the ones a clear resets (21, 30, 31, 4D,
// 4E) are the diagnostics' (sim_ecu.h), not here.

#ifndef ECUIO_SIM_CANNED_H
#define ECUIO_SIM_CANNED_H

#include <cstdint>

struct ecuio_sim_canned {
    uint8_t pid;
    uint8_t len;
    uint8_t data[13];
};

/* Sorted by PID */
constexpr ecuio_sim_canned ECUIO_SIM_CANNED[] = {
    {0x04, 1, {25}},                     /* engine load 9.8 % */
    {0x05, 1, {60}},                     /* coolant 20 degC */
    {0x06, 1, {0x82}},                   /* short-term trim B1 +1.6 % */
    {0x07, 1, {0x7C}},                   /* long-term trim B1 -3.1 % */
    {0x08, 1, {0x81}},                   /* short-term trim B2 +0.8 % */
    {0x09, 1, {0x7E}},                   /* long-term trim B2 -1.6 % */
    {0x0A, 1, {0x7F}},                   /* fuel pressure 381 kPa */
    {0x0B, 1, {0x23}},                   /* MAP 35 kPa */
    {0x0C, 2, {0x1F, 0x40}},             /* 2000 rpm */
    {0x0D, 1, {0}},                      /* 0 km/h */
    {0x0E, 1, {0x98}},                   /* timing 12 degrees */
    {0x0F, 1, {60}},                     /* intake air 20 degC */
    {0x10, 2, {0x00, 0x64}},             /* MAF 1.00 g/s */
    {0x11, 1, {20}},                     /* throttle 7.8 % */
    {0x14, 2, {0x5A, 0x82}},             /* O2 1: 0.450 V, trim +1.6 % */
    {0x15, 2, {0x8C, 0xFF}},             /* O2 2: 0.700 V, not in trim */
    {0x16, 2, {0x50, 0x80}},             /* O2 3: 0.400 V, trim 0 % */
    {0x17, 2, {0x8C, 0xFF}},             /* O2 4: 0.700 V, not in trim */
    {0x18, 2, {0x5C, 0x81}},             /* O2 5: 0.460 V, trim +0.8 % */
    {0x19, 2, {0x8A, 0xFF}},             /* O2 6: 0.690 V, not in trim */
    {0x1A, 2, {0x58, 0x7F}},             /* O2 7: 0.440 V, trim -0.8 % */
    {0x1B, 2, {0x8E, 0xFF}},             /* O2 8: 0.710 V, not in trim */
    {0x1F, 2, {0x02, 0x58}},             /* run time 600 s */
    {0x22, 2, {0x0E, 0xD5}},             /* rail pressure 300.0 kPa */
    {0x23, 2, {0x03, 0xE8}},             /* rail gauge 10000 kPa */
    {0x24, 4, {0x80, 0x00, 0x50, 0x00}}, /* wide O2 1: lambda 1.000, 2.50 V */
    {0x25, 4, {0x7F, 0x5C, 0x50, 0x00}}, /* wide O2 2: lambda 0.995, 2.50 V */
    {0x26, 4, {0x80, 0xA4, 0x4F, 0x00}}, /* wide O2 3: lambda 1.005, 2.47 V */
    {0x27, 4, {0x80, 0x00, 0x50, 0x00}}, /* wide O2 4: lambda 1.000, 2.50 V */
    {0x28, 4, {0x7F, 0xAE, 0x50, 0x80}}, /* wide O2 5: lambda 0.998, 2.52 V */
    {0x29, 4, {0x80, 0x52, 0x4F, 0x80}}, /* wide O2 6: lambda 1.003, 2.48 V */
    {0x2A, 4, {0x80, 0x00, 0x50, 0x00}}, /* wide O2 7: lambda 1.000, 2.50 V */
    {0x2B, 4, {0x80, 0x00, 0x50, 0x00}}, /* wide O2 8: lambda 1.000, 2.50 V */
    {0x2C, 1, {0x1A}},                   /* commanded EGR 10.2 % */
    {0x2D, 1, {0x80}},                   /* EGR error 0 % */
    {0x2E, 1, {0x33}},                   /* commanded purge 20 % */
    {0x2F, 1, {64}},                     /* fuel level 25 % */
    {0x32, 2, {0xFE, 0x70}},             /* evap pressure -100 Pa (signed) */
    {0x33, 1, {0x65}},                   /* barometric 101 kPa */
    {0x34, 4, {0x7F, 0x5C, 0x80, 0x0D}}, /* wide O2 1: 0.995, 0.05 mA */
    {0x35, 4, {0x80, 0x00, 0x80, 0x00}}, /* wide O2 2: lambda 1.000, 0 mA */
    {0x36, 4, {0x80, 0xA4, 0x7F, 0xF3}}, /* wide O2 3: 1.005, -0.05 mA */
    {0x37, 4, {0x80, 0x00, 0x80, 0x00}}, /* wide O2 4: lambda 1.000, 0 mA */
    {0x38, 4, {0x7F, 0xAE, 0x80, 0x0D}}, /* wide O2 5: 0.998, 0.05 mA */
    {0x39, 4, {0x80, 0x52, 0x7F, 0xF3}}, /* wide O2 6: 1.003, -0.05 mA */
    {0x3A, 4, {0x80, 0x00, 0x80, 0x00}}, /* wide O2 7: lambda 1.000, 0 mA */
    {0x3B, 4, {0x80, 0x00, 0x80, 0x00}}, /* wide O2 8: lambda 1.000, 0 mA */
    {0x3C, 2, {0x1A, 0xF4}},             /* catalyst B1 S1 650.0 degC */
    {0x3D, 2, {0x1A, 0x90}},             /* catalyst B2 S1 640.0 degC */
    {0x3E, 2, {0x15, 0xE0}},             /* catalyst B1 S2 520.0 degC */
    {0x3F, 2, {0x15, 0x7C}},             /* catalyst B2 S2 510.0 degC */
    {0x42, 2, {0x36, 0x30}},             /* module voltage 13.872 V */
    {0x43, 2, {0x00, 0x40}},             /* absolute load 25.1 % */
    {0x44, 2, {0x80, 0x00}},             /* commanded lambda 1.000 */
    {0x45, 1, {0x0D}},                   /* relative throttle 5.1 % */
    {0x46, 1, {0x3E}},                   /* ambient 22 degC */
    {0x47, 1, {0x26}},                   /* throttle B 14.9 % */
    {0x48, 1, {0x26}},                   /* throttle C 14.9 % */
    {0x49, 1, {0x29}},                   /* pedal D 16.1 % */
    {0x4A, 1, {0x14}},                   /* pedal E 7.8 % */
    {0x4B, 1, {0x14}},                   /* pedal F 7.8 % */
    {0x4C, 1, {0x12}},                   /* commanded throttle 7.1 % */
    {0x4F, 4, {0x02, 0x08, 0x80, 0x19}}, /* maxima 2, 8 V, 128 mA, 250 kPa */
    {0x50, 4, {0x28, 0x00, 0x00, 0x00}}, /* maximum MAF 400 g/s */
    {0x52, 1, {0x1A}},                   /* ethanol 10.2 % */
    {0x53, 2, {0x4E, 0xE8}},             /* evap pressure 101.000 kPa abs. */
    {0x54, 2, {0xFF, 0x9C}},             /* evap pressure -100 Pa (signed) */
    {0x55, 2, {0x81, 0x80}},             /* 2nd short trims B1 +0.8, B3 0 % */
    {0x56, 2, {0x7E, 0x80}},             /* 2nd long trims B1 -1.6, B3 0 % */
    {0x57, 2, {0x80, 0x80}},             /* 2nd short trims B2, B4 0 % */
    {0x58, 2, {0x7F, 0x80}},             /* 2nd long trims B2 -0.8, B4 0 % */
    {0x59, 2, {0x03, 0xF2}},             /* rail absolute 10100 kPa */
    {0x5A, 1, {0x1A}},                   /* relative pedal 10.2 % */
    {0x5B, 1, {0xCC}},                   /* hybrid battery life 80 % */
    {0x5C, 1, {0x78}},                   /* oil 80 degC */
    {0x5D, 2, {0x6E, 0x00}},             /* injection timing 10.0 degrees */
    {0x5E, 2, {0x00, 0x32}},             /* fuel rate 2.50 L/h */
    {0x61, 1, {0x91}},                   /* driver's demand torque 20 % */
    {0x62, 1, {0x8F}},                   /* actual torque 18 % */
    {0x63, 2, {0x01, 0xA4}},             /* reference torque 420 N.m */
    {0x64, 5, {0x8C, 0x9B, 0xB9, 0xD7, 0xE1}}, /* 15 30 60 90 100 % */
    {0x66, 5, {0x03, 0x03, 0x20, 0x03, 0x10}}, /* MAF A 25.0, B 24.5 g/s */
    {0x67, 3, {0x03, 0x80, 0x7E}},             /* coolant 1 88, 2 86 degC */
    {0x68, 7, {0x03, 0x41, 0x4B, 0, 0, 0, 0}}, /* IAT 1 25, 2 35 degC */
    {0x7F,
     13,
     {0x07, 0x00, 0x00, 0x0E, 0x10, 0x00, 0x00, 0x03, 0x84, 0x00, 0x00, 0x00,
      0x00}},                /* run times: 3600 s total, 900 s idle, 0 s PTO */
    {0x8E, 1, {0x85}},       /* friction torque 8 % */
    {0xA2, 2, {0x01, 0xE0}}, /* fuel 15.0 mg/stroke */
    {0xA6, 4, {0x00, 0x01, 0xE2, 0x40}}, /* odometer 12345.6 km */
};

/* PID's row, or nullptr */
inline const ecuio_sim_canned* ecuio_sim_canned_pid(uint8_t pid)
{
    for (const ecuio_sim_canned& c : ECUIO_SIM_CANNED) {
        if (c.pid == pid) {
            return &c;
        }
    }
    return nullptr;
}

#endif /* ECUIO_SIM_CANNED_H */
