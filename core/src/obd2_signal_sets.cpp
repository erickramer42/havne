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

/* One def per v1 PID. Wire grammar (post header-strip, what the bridge
 * hands the decoder): [PCI][0x41][PID][A][B]...
 *   - mux key: bytes 1..2 (mode-echo + PID), 16-bit big-endian compare
 *     against 0x4100|PID — the OBD-II response discriminator
 *   - data: starts byte 3 (PID 0x0C/0x42 are 2-byte big-endian; the
 *     rest are single bytes)
 *   - INT8 temperatures carry the -40 offset in `offset`
 * Scale formulas are the standard SAE J1979 Mode-01 conversions; the
 * set enumerates SUPPORTED pids, which is itself the contract (what's
 * absent here is not requested and not decoded).
 * MULTI-FRAME RESPONSES: out of scope v1 — defs assume single-frame;
 * documented as a set-level constraint in ADR-0013 slice 2. */
QttuneSignalDef obd2_def(uint16_t id, const char* name,
                         QttuneSignalType type, uint8_t num_bytes,
                         float scale, float offset_val, const char* unit,
                         uint8_t decimals, uint32_t pid, bool big_endian)
{
    QttuneSignalDef d = {};   /* zero-fill: mux off, BE off by default */
    d.id = id;
    (void)std::snprintf(d.name, sizeof d.name, "%s", name);
    d.type = type;
    d.start_byte = 3;
    d.num_bytes = num_bytes;
    d.bit_offset = 0;
    d.bit_length = static_cast<uint8_t>(num_bytes * 8u);
    d.scale = scale;
    d.offset = offset_val;
    (void)std::snprintf(d.unit, sizeof d.unit, "%s", unit);
    d.decimal_places = decimals;
    d.flags = big_endian ? QTTUNE_SIGNAL_FLAG_BIG_ENDIAN : 0u;
    d.message_id = OBD2_ECU_ARB_ID;       /* 0x7E8 */
    d.mux_offset = 1;
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
             QTTUNE_SIGNAL_TYPE_INT8, 1, 1.0f, -40.0f,
             "degC", 0, 0x05, false),
    obd2_def(OBD2_SIG_RPM_ID,          "RPM",
             QTTUNE_SIGNAL_TYPE_UINT16, 2, 0.25f, 0.0f,
             "RPM",  0, 0x0C, true),
    obd2_def(OBD2_SIG_SPEED_ID,        "Vehicle Speed",
             QTTUNE_SIGNAL_TYPE_UINT8, 1, 1.0f, 0.0f,
             "km/h", 0, 0x0D, false),
    obd2_def(OBD2_SIG_IAT_ID,          "Intake Air Temp",
             QTTUNE_SIGNAL_TYPE_INT8, 1, 1.0f, -40.0f,
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

const QttuneSignalSet kObd2Set = {
    1u,                                              /* version */
    static_cast<uint32_t>(sizeof(kObd2Defs) / sizeof(kObd2Defs[0])),
    kObd2Defs
};

} // namespace

extern "C" const QttuneSignalSet* qttune_obd2_signal_set(void)
{
    return &kObd2Set;
}
