// File/layer: core/src/obd2_signal_sets.cpp | C++17, Qt-free, ALL platforms
//             (signal data is transport-agnostic; j2534-only consumers route
//             through the profile registry, which exists everywhere)
// Testing: UNIT — tests/test_obd2.cpp validates the set and round-trips
//          decode against wire bytes matching the fake's canned responses;
//          end-to-end HARNESS coverage rides the Phase 5 stream tests.
//
// SEPARATE FILE (ADR-0013 precedent): one TU per profile-family signal
// set. mock lives in signal_sets.cpp; manufacturer plugins in v0.5
// follow this pattern as new files, never edits to existing sets.

#include "qttune/signals.h"
#include "qttune/signal_sets.h"

#include <cstdint>
#include <cstdio>

namespace {

/* One def per v1 PID. Payload as the decoder sees it, after the
 * transport strips the 4-byte address header: [0x41][PID][A][B]...
 *   - mux key: bytes 0..1, compared as 0x4100|PID (mode echo + PID)
 *   - data: from byte 2; PIDs 0x0C and 0x42 are 2-byte big-endian,
 *     the rest single bytes
 *   - no PCI byte: the adapter does the ISO-TP framing
 *   - temperatures are UINT8 (J1979: A - 40, so -40..215 degC)
 * Scaling follows SAE J1979 Mode 01. Only the PIDs listed here are
 * requested and decoded. Single-frame responses only. */
QttuneSignalDef obd2_def(uint16_t id, const char* name,
                         QttuneSignalType type, uint8_t num_bytes,
                         float scale, float offset_val, const char* unit,
                         uint8_t decimals, uint32_t pid, bool big_endian)
{
    QttuneSignalDef d = {};   /* zero-fill: mux off, BE off by default */
    d.id = id;
    (void)std::snprintf(d.name, sizeof d.name, "%s", name);
    d.type = type;
    d.start_byte = 2;
    d.num_bytes = num_bytes;
    d.bit_offset = 0;
    d.bit_length = static_cast<uint8_t>(num_bytes * 8u);
    d.scale = scale;
    d.offset = offset_val;
    (void)std::snprintf(d.unit, sizeof d.unit, "%s", unit);
    d.decimal_places = decimals;
    d.flags = big_endian ? QTTUNE_SIGNAL_FLAG_BIG_ENDIAN : 0u;
    d.message_id = OBD2_ECU_ARB_ID;       /* 0x7E8 */
    d.mux_offset = 0;
    d.mux_length = 2;
    d.mux_reserved[0] = 0;
    d.mux_reserved[1] = 0;
    d.mux_key = 0x4100u | pid;
    return d;
}

const QttuneSignalDef kObd2Defs[] = {
    obd2_def(OBD2_SIG_ENGINE_LOAD_ID,  "Engine Load",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 100.0f / 255.0f, 0.0f,
             "%",    1, 0x04, false),
    obd2_def(OBD2_SIG_COOLANT_TEMP_ID, "Coolant Temp",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 1.0f, -40.0f,
             "degC", 0, 0x05, false),
    obd2_def(OBD2_SIG_RPM_ID,          "RPM",
             QTTUNE_SIGNAL_TYPE_UINT16, 2, 0.25f, 0.0f,
             "RPM",  0, 0x0C, true),
    obd2_def(OBD2_SIG_SPEED_ID,        "Vehicle Speed",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 1.0f, 0.0f,
             "km/h", 0, 0x0D, false),
    obd2_def(OBD2_SIG_IAT_ID,          "Intake Air Temp",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 1.0f, -40.0f,
             "degC", 0, 0x0F, false),
    obd2_def(OBD2_SIG_THROTTLE_ID,     "Throttle Position",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 100.0f / 255.0f, 0.0f,
             "%",    1, 0x11, false),
    obd2_def(OBD2_SIG_FUEL_LEVEL_ID,   "Fuel Level",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 100.0f / 255.0f, 0.0f,
             "%",    1, 0x2F, false),
    obd2_def(OBD2_SIG_BATTERY_V_ID,    "Battery Voltage",
             QTTUNE_SIGNAL_TYPE_UINT16, 2, 0.001f, 0.0f,
             "V",    2, 0x42, true),
};

/* ADR-0013 §4: the obd2 profile declares the standard physical pair,
 * 0x7E8 (ECU response) / 0x7E0 (tester request). The transport
 * registers its receive filter and addresses requests from this list;
 * ?filter= and ?txid= override it. Multi-ECU profiles add entries. */
const QttuneChannelDecl kObd2Channels[] = {
    { OBD2_ECU_ARB_ID, OBD2_TESTER_ARB_ID },
};

const QttuneSignalSet kObd2Set = {
    2u,                                              /* version 2: adds channels */
    static_cast<uint32_t>(sizeof(kObd2Defs) / sizeof(kObd2Defs[0])),
    kObd2Defs,
    1u,
    kObd2Channels
};

} // namespace

extern "C" const QttuneSignalSet* qttune_obd2_signal_set(void)
{
    return &kObd2Set;
}
