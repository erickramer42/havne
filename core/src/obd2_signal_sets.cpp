// File/layer: core/src/obd2_signal_sets.cpp | C++17, Qt-free, ALL platforms
//             (signal data is transport-agnostic; j2534-only consumers route
//             through the profile registry, which exists everywhere)
// Testing: UNIT: tests/test_obd2.cpp validates the set and round-trips
//          decode against wire bytes matching the fake's canned responses;
//          end-to-end HARNESS coverage rides the Phase 5 stream tests.
//
// SEPARATE FILE (ADR-0013 precedent): one TU per profile-family signal
// set. mock lives in signal_sets.cpp; manufacturer plugins in v0.5
// follow this pattern as new files, never edits to existing sets.

#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"

#include <cstdint>
#include <cstdio>

namespace {

/* One def per v1 PID. Payload as the decoder sees it, after the
 * transport strips the 4-byte address header: [0x41][PID][A][B]...
 *   - mux key: bytes 0..1, compared as 0x4100|PID (mode echo + PID)
 *   - data: from byte 2; the 2-byte PIDs are big-endian (J1979's
 *     256·A + B), the rest single bytes
 *   - no PCI byte: the adapter does the ISO-TP framing
 *   - temperatures are 8-bit UINT (J1979: A - 40, so -40..215 degC)
 * Scaling follows SAE J1979 Mode 01. Only the PIDs listed here are
 * requested and decoded. Single-frame responses only.
 * Names, units and decimal places are display strings: the UI shows
 * them as they are (UTF-8). */
ecuio_signal_def_t obd2_def(uint16_t id, const char* name,
                            ecuio_value_kind_t kind, uint8_t bit_length,
                            double scale, double offset_val, const char* unit,
                            uint8_t decimals, uint32_t pid, uint8_t byte_order)
{
    ecuio_signal_def_t d = {}; /* zero-fill: reserved bytes and flags zero */
    d.id = id;
    (void)std::snprintf(d.name, sizeof d.name, "%s", name);
    d.kind = static_cast<uint8_t>(kind);
    d.byte_order = byte_order;
    d.start_byte = 2;
    d.bit_offset = 0;
    d.bit_length = bit_length;
    d.scale = scale;
    d.offset = offset_val;
    (void)std::snprintf(d.unit, sizeof d.unit, "%s", unit);
    d.decimal_places = decimals;
    d.message_id = ECUIO_OBD2_ECU_ARB_ID;        /* 0x7E8 */
    d.mux_offset = 0;
    d.mux_length = 2;
    d.mux_key = 0x4100u | pid;
    return d;
}

#define DEG_C "\xC2\xB0" "C"   /* UTF-8 degree sign, whatever the source encoding */

const ecuio_signal_def_t kObd2Defs[] = {
    obd2_def(ECUIO_OBD2_SIG_ENGINE_LOAD_ID,   "Engine Load",
             ECUIO_VALUE_UINT,  8, 100.0 / 255.0, 0.0,
             "%",    1, 0x04, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_COOLANT_TEMP_ID, "Coolant Temp",
             ECUIO_VALUE_UINT,  8, 1.0, -40.0,
             DEG_C,  0, 0x05, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_RPM_ID,           "RPM",
             ECUIO_VALUE_UINT, 16, 0.25, 0.0,
             "RPM",  0, 0x0C, ECUIO_BYTE_ORDER_BE),
    obd2_def(ECUIO_OBD2_SIG_SPEED_ID,         "Vehicle Speed",
             ECUIO_VALUE_UINT,  8, 1.0, 0.0,
             "km/h", 0, 0x0D, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_IAT_ID,           "Intake Air Temp",
             ECUIO_VALUE_UINT,  8, 1.0, -40.0,
             DEG_C,  0, 0x0F, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_THROTTLE_ID,      "Throttle Position",
             ECUIO_VALUE_UINT,  8, 100.0 / 255.0, 0.0,
             "%",    1, 0x11, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_FUEL_LEVEL_ID,    "Fuel Level",
             ECUIO_VALUE_UINT,  8, 100.0 / 255.0, 0.0,
             "%",    1, 0x2F, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_BATTERY_V_ID,     "Battery Voltage",
             ECUIO_VALUE_UINT, 16, 0.001, 0.0,
             "V",    2, 0x42, ECUIO_BYTE_ORDER_BE),
    /* SAE J1979 PID 0x10: air flow rate, (256·A + B)/100 g/s. Two-byte
     * big-endian like RPM/0x42; the 0.01 scale needs no decoder
     * extension (generic raw·scale + offset). Shown to one decimal. */
    obd2_def(ECUIO_OBD2_SIG_MAF_ID,             "MAF",
             ECUIO_VALUE_UINT, 16, 0.01, 0.0,
             "g/s",  1, 0x10, ECUIO_BYTE_ORDER_BE),
    /* SAE J1979 PID 0x5C: engine oil temperature, A - 40 degC, the same
     * def shape as coolant and IAT. Transmission fluid temperature isn't
     * a standard Mode-01 PID (it's manufacturer-specific, usually
     * Mode 22), and PID 0x5F is the bit-encoded emission-requirements
     * byte, not a temperature. */
    obd2_def(ECUIO_OBD2_SIG_OIL_TEMP_ID,          "Engine Oil Temp",
             ECUIO_VALUE_UINT,  8, 1.0, -40.0,
             DEG_C,  0, 0x5C, ECUIO_BYTE_ORDER_LE),
};

#undef DEG_C

/* ADR-0013 §4: the obd2 profile declares the standard physical pair,
 * 0x7E8 (ECU response) / 0x7E0 (tester request). The transport
 * registers its receive filter and addresses requests from this list;
 * ?filter= and ?txid= override it. Multi-ECU profiles add entries. */
const ecuio_channel_decl_t kObd2Channels[] = {
    { ECUIO_OBD2_ECU_ARB_ID, ECUIO_OBD2_TESTER_ARB_ID },
};

const ecuio_signal_set_t kObd2Set = [] {
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(sizeof(kObd2Defs) / sizeof(kObd2Defs[0]));
    s.defs = kObd2Defs;
    s.channel_count = static_cast<uint32_t>(sizeof(kObd2Channels) / sizeof(kObd2Channels[0]));
    s.channels = kObd2Channels;
    return s;
}();

} // namespace

extern "C" const ecuio_signal_set_t* ecuio_obd2_signal_set(void)
{
    return &kObd2Set;
}
