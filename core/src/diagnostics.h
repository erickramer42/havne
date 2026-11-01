// File/layer: core/src/diagnostics.h | internal, not ABI-exported;
//             included by core.cpp (the names) and the request engine
//             (the replies into the report)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: the code's letter and digits, every monitor's name
//     against tests/fixtures/obd2/monitors.csv, PID 01 into the
//     readiness fields, a code list's count, codes and cut
//     (DiagnosticsTest, test_obd2).

#ifndef ECUIO_DIAGNOSTICS_H
#define ECUIO_DIAGNOSTICS_H

#include "ecuio/core.h"

#include <stdint.h>

/* ADR-0037: the words and the reply layouts of SAE J1979's emissions
 * diagnostics. Pure: no session, no clock. */

/* Which engine a monitor's name holds for: byte B's monitors are
 * common; C's and D's bits name others on a compression-ignition engine
 * (PID 01's B3). */
enum class ecuio_monitor_engine : uint8_t { any, spark, compression };

struct ecuio_monitor_def {
    uint8_t id; /* the availability bit's place: B 0-7, C 8-15 */
    ecuio_monitor_engine engine;
    const char* name;
    const char* short_name;
};

/* tests/fixtures/obd2/monitors.csv, row for row (DiagnosticsTest checks
 * it). Bit 4 of C names the gasoline particulate filter, as J1979's
 * later revisions do; the 2002 edition named it A/C refrigerant. */
constexpr ecuio_monitor_def ECUIO_MONITORS[] = {
    {0, ecuio_monitor_engine::any, "Misfire", "misfire"},
    {1, ecuio_monitor_engine::any, "Fuel system", "fuel system"},
    {2, ecuio_monitor_engine::any, "Comprehensive components", "components"},
    {8, ecuio_monitor_engine::spark, "Catalyst", "catalyst"},
    {9, ecuio_monitor_engine::spark, "Heated catalyst", "heated catalyst"},
    {10, ecuio_monitor_engine::spark, "Evaporative system", "EVAP"},
    {11, ecuio_monitor_engine::spark, "Secondary air system", "secondary air"},
    {12, ecuio_monitor_engine::spark, "Gasoline particulate filter", "GPF"},
    {13, ecuio_monitor_engine::spark, "Oxygen sensor", "O2 sensor"},
    {14, ecuio_monitor_engine::spark, "Oxygen sensor heater", "O2 heater"},
    {15, ecuio_monitor_engine::spark, "EGR and/or VVT system", "EGR/VVT"},
    {8, ecuio_monitor_engine::compression, "NMHC catalyst", "NMHC catalyst"},
    {9, ecuio_monitor_engine::compression, "NOx/SCR aftertreatment", "NOx/SCR"},
    {11, ecuio_monitor_engine::compression, "Boost pressure", "boost pressure"},
    {13, ecuio_monitor_engine::compression, "Exhaust gas sensor",
     "exhaust gas sensor"},
    {14, ecuio_monitor_engine::compression, "PM filter", "PM filter"},
    {15, ecuio_monitor_engine::compression, "EGR and/or VVT system", "EGR/VVT"},
};

/* The monitor id names on the engine diag_flags describe; nullptr for a
 * reserved bit. */
inline const ecuio_monitor_def* ecuio_monitor_find(uint8_t id,
                                                   uint32_t diag_flags)
{
    const ecuio_monitor_engine engine =
        (diag_flags & ECUIO_DIAG_COMPRESSION) != 0
            ? ecuio_monitor_engine::compression
            : ecuio_monitor_engine::spark;
    for (const ecuio_monitor_def& m : ECUIO_MONITORS) {
        if (m.id == id &&
            (m.engine == ecuio_monitor_engine::any || m.engine == engine)) {
            return &m;
        }
    }
    return nullptr;
}

/* "P0301": the letter from A7-A6, then four hex digits. out holds at
 * least 6 bytes. */
inline void ecuio_dtc_text(uint16_t code, char out[6])
{
    static const char kLetter[] = "PCBU";
    static const char kHex[] = "0123456789ABCDEF";
    out[0] = kLetter[code >> 14];
    out[1] = kHex[(code >> 12) & 0x3u];
    out[2] = kHex[(code >> 8) & 0xFu];
    out[3] = kHex[(code >> 4) & 0xFu];
    out[4] = kHex[code & 0xFu];
    out[5] = '\0';
}

/* PID 01's four data bytes (A B C D, after 41 01) into the report: the
 * MIL, the confirmed-code count, the engine type, and each monitor the
 * ECU has, complete or not. A bit that names no monitor on this engine
 * (a reserved one) is left out. Sets ECUIO_DIAG_READINESS. */
inline void ecuio_diag_take_readiness(const uint8_t abcd[4],
                                      ecuio_channel_diagnostics_t* out)
{
    const uint8_t a = abcd[0], b = abcd[1], c = abcd[2], d = abcd[3];
    out->flags &= ~(ECUIO_DIAG_MIL_ON | ECUIO_DIAG_COMPRESSION);
    out->flags |= ECUIO_DIAG_READINESS |
                  ((a & 0x80u) != 0 ? ECUIO_DIAG_MIL_ON : 0u) |
                  ((b & 0x08u) != 0 ? ECUIO_DIAG_COMPRESSION : 0u);
    out->dtc_count = static_cast<uint8_t>(a & 0x7Fu);
    out->monitor_count = 0;
    for (uint8_t id = 0; id < 16; ++id) {
        /* B: availability in bits 0-2, incompleteness in 4-6; C and D
         * the same, bit for bit */
        const bool in_b = id < 8;
        const uint8_t bit = static_cast<uint8_t>(in_b ? id : id - 8);
        if (in_b && bit > 2) {
            continue;
        }
        const uint8_t available = in_b ? b : c;
        if ((available & (1u << bit)) == 0 ||
            ecuio_monitor_find(id, out->flags) == nullptr ||
            out->monitor_count >= ECUIO_DIAG_MAX_MONITORS) {
            continue;
        }
        const bool incomplete =
            in_b ? (b & (0x10u << bit)) != 0 : (d & (1u << bit)) != 0;
        ecuio_monitor_t& m = out->monitors[out->monitor_count++];
        m = ecuio_monitor_t{};
        m.id = id;
        m.flags = incomplete ? 0u : ECUIO_MONITOR_COMPLETE;
    }
}

/* A code list's reply (43, 47 or 4A, the count, then two bytes a code)
 * into list: the count as reported, and the codes that fit the list and
 * the reply. The outcome is the caller's. */
inline void ecuio_diag_take_codes(const uint8_t* data, uint32_t len,
                                  ecuio_dtc_list_t* list)
{
    list->reported = (len >= 2) ? data[1] : 0;
    list->held = 0;
    for (uint32_t i = 0;
         i < list->reported && i < ECUIO_DIAG_MAX_DTCS && 2 + 2 * i + 1 < len;
         ++i) {
        list->codes[i] =
            static_cast<uint16_t>((data[2 + 2 * i] << 8) | data[3 + 2 * i]);
        ++list->held;
    }
}

#endif /* ECUIO_DIAGNOSTICS_H */
