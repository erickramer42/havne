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

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

/* One def per field of every linear PID in J1979's Service 01 table
 * (ADR-0039): the ten first, then the rest in PID order. Payload as the
 * decoder sees it, after the transport strips
 * the 4-byte address header: [0x41][PID][A][B]...
 *   - mux key: bytes 0..1, compared as 0x4100|PID (mode echo + PID)
 *   - data: from byte 2; multi-byte fields are big-endian (J1979's
 *     256*A + B)
 *   - no PCI byte: the adapter does the ISO-TP framing
 *   - temperatures are 8-bit UINT (J1979: A - 40, so -40..215 degC)
 * Scaling follows SAE J1979 Service 01, typed out here and checked against
 * tests/fixtures/obd2/service01.csv by Obd2TableTest, never generated from
 * it. Only these PIDs are requested; the probe drops those a car's bitmaps
 * leave out. Names, units and decimals are shown as they are (UTF-8); each
 * name is unique in the set (a recording keys values by name) and fits 31
 * bytes. */
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

/* A field of PID pid after the ten: its ID by ECUIO_OBD2_PID_SIG_ID,
 * starting start bytes into the data, big-endian past 8 bits. */
ecuio_signal_def_t pid_def(uint32_t pid, uint32_t field, uint8_t start,
                           const char* name, ecuio_value_kind_t kind,
                           uint8_t bit_length, double scale, double offset_val,
                           const char* unit, uint8_t decimals)
{
    ecuio_signal_def_t d =
        obd2_def(static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(pid, field)), name,
                 kind, bit_length, scale, offset_val, unit, decimals, pid,
                 bit_length > 8 ? ECUIO_BYTE_ORDER_BE : ECUIO_BYTE_ORDER_LE);
    d.start_byte = static_cast<uint8_t>(2 + start);
    return d;
}

/* J1979's repeated scalings: a trim (0 % at 128), a percentage of 255,
 * a 16-bit equivalence ratio and a wide-range sensor's voltage */
constexpr double kTrim = 100.0 / 128.0;
constexpr double kPct = 100.0 / 255.0;
constexpr double kRatio = 2.0 / 65536.0;
constexpr double kWideV = 8.0 / 65536.0;

#define DEG_C "\xC2\xB0" "C"   /* UTF-8 degree sign, whatever the source encoding */
#define DEG    "\xC2\xB0"      /* an angle's degree sign */
#define LAMBDA "\xCE\xBB"      /* lambda, the equivalence ratio */
#define NM     "N\xC2\xB7m"    /* newton meters, a middle dot */
#define U      ECUIO_VALUE_UINT
#define S      ECUIO_VALUE_INT

/* Ranked and grouped below (kPlotOrder, kPidGroups), so not const */
ecuio_signal_def_t kObd2Defs[] = {
    obd2_def(ECUIO_OBD2_SIG_ENGINE_LOAD_ID, "Engine Load", ECUIO_VALUE_UINT, 8,
             100.0 / 255.0, 0.0, "%", 1, 0x04, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_COOLANT_TEMP_ID, "Coolant Temp", ECUIO_VALUE_UINT,
             8, 1.0, -40.0, DEG_C, 0, 0x05, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_RPM_ID, "RPM", ECUIO_VALUE_UINT, 16, 0.25, 0.0,
             "RPM", 0, 0x0C, ECUIO_BYTE_ORDER_BE),
    obd2_def(ECUIO_OBD2_SIG_SPEED_ID, "Vehicle Speed", ECUIO_VALUE_UINT, 8, 1.0,
             0.0, "km/h", 0, 0x0D, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_IAT_ID, "Intake Air Temp", ECUIO_VALUE_UINT, 8, 1.0,
             -40.0, DEG_C, 0, 0x0F, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_THROTTLE_ID, "Throttle Position", ECUIO_VALUE_UINT,
             8, 100.0 / 255.0, 0.0, "%", 1, 0x11, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_FUEL_LEVEL_ID, "Fuel Level", ECUIO_VALUE_UINT, 8,
             100.0 / 255.0, 0.0, "%", 1, 0x2F, ECUIO_BYTE_ORDER_LE),
    obd2_def(ECUIO_OBD2_SIG_BATTERY_V_ID, "Battery Voltage", ECUIO_VALUE_UINT,
             16, 0.001, 0.0, "V", 2, 0x42, ECUIO_BYTE_ORDER_BE),
    /* SAE J1979 PID 0x10: air flow rate, (256*A + B)/100 g/s, two-byte
     * big-endian. Two decimals, the PID's step (ADR-0056 section 5). */
    obd2_def(ECUIO_OBD2_SIG_MAF_ID, "MAF", ECUIO_VALUE_UINT, 16, 0.01, 0.0,
             "g/s", 2, 0x10, ECUIO_BYTE_ORDER_BE),
    /* SAE J1979 PID 0x5C: engine oil temperature, A - 40 degC. Transmission
     * fluid isn't a standard Mode-01 PID (manufacturer-specific, usually
     * Mode 22), and PID 0x5F is a bit-encoded byte, not a temperature. */
    obd2_def(ECUIO_OBD2_SIG_OIL_TEMP_ID, "Engine Oil Temp", ECUIO_VALUE_UINT, 8,
             1.0, -40.0, DEG_C, 0, 0x5C, ECUIO_BYTE_ORDER_LE),
    /* The rest of the table, in PID order (ADR-0039 section 1): pid, field,
     * start, name, kind, bits, scale, offset, unit, decimals */
    pid_def(0x06, 0, 0, "Short Term Fuel Trim B1", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x07, 0, 0, "Long Term Fuel Trim B1", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x08, 0, 0, "Short Term Fuel Trim B2", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x09, 0, 0, "Long Term Fuel Trim B2", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x0A, 0, 0, "Fuel Pressure", U, 8, 3.0, 0.0, "kPa", 0),
    pid_def(0x0B, 0, 0, "Intake Manifold Pressure", U, 8, 1.0, 0.0, "kPa", 0),
    pid_def(0x0E, 0, 0, "Timing Advance", U, 8, 0.5, -64.0, DEG, 1),
    pid_def(0x14, 0, 0, "O2 S1 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x14, 1, 1, "O2 S1 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x15, 0, 0, "O2 S2 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x15, 1, 1, "O2 S2 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x16, 0, 0, "O2 S3 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x16, 1, 1, "O2 S3 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x17, 0, 0, "O2 S4 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x17, 1, 1, "O2 S4 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x18, 0, 0, "O2 S5 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x18, 1, 1, "O2 S5 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x19, 0, 0, "O2 S6 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x19, 1, 1, "O2 S6 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x1A, 0, 0, "O2 S7 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x1A, 1, 1, "O2 S7 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x1B, 0, 0, "O2 S8 Voltage", U, 8, 0.005, 0.0, "V", 3),
    pid_def(0x1B, 1, 1, "O2 S8 Short Term Trim", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x1F, 0, 0, "Run Time Since Start", U, 16, 1.0, 0.0, "s", 0),
    pid_def(0x21, 0, 0, "Distance with MIL On", U, 16, 1.0, 0.0, "km", 0),
    pid_def(0x22, 0, 0, "Fuel Rail Pressure (Rel)", U, 16, 0.079, 0.0, "kPa",
            1),
    pid_def(0x23, 0, 0, "Fuel Rail Pressure (Gauge)", U, 16, 10.0, 0.0, "kPa",
            0),
    pid_def(0x24, 0, 0, "O2 S1 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x24, 1, 2, "O2 S1 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x25, 0, 0, "O2 S2 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x25, 1, 2, "O2 S2 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x26, 0, 0, "O2 S3 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x26, 1, 2, "O2 S3 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x27, 0, 0, "O2 S4 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x27, 1, 2, "O2 S4 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x28, 0, 0, "O2 S5 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x28, 1, 2, "O2 S5 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x29, 0, 0, "O2 S6 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x29, 1, 2, "O2 S6 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x2A, 0, 0, "O2 S7 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x2A, 1, 2, "O2 S7 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x2B, 0, 0, "O2 S8 Lambda (V)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x2B, 1, 2, "O2 S8 Voltage (Wide)", U, 16, kWideV, 0.0, "V", 3),
    pid_def(0x2C, 0, 0, "Commanded EGR", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x2D, 0, 0, "EGR Error", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x2E, 0, 0, "Commanded Evap Purge", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x30, 0, 0, "Warm-ups Since Clear", U, 8, 1.0, 0.0, "", 0),
    pid_def(0x31, 0, 0, "Distance Since Clear", U, 16, 1.0, 0.0, "km", 0),
    /* J1979 (2002): two's complement, $8000 is -8192 Pa */
    pid_def(0x32, 0, 0, "Evap Vapor Pressure", S, 16, 0.25, 0.0, "Pa", 2),
    pid_def(0x33, 0, 0, "Barometric Pressure", U, 8, 1.0, 0.0, "kPa", 0),
    pid_def(0x34, 0, 0, "O2 S1 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x34, 1, 2, "O2 S1 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x35, 0, 0, "O2 S2 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x35, 1, 2, "O2 S2 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x36, 0, 0, "O2 S3 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x36, 1, 2, "O2 S3 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x37, 0, 0, "O2 S4 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x37, 1, 2, "O2 S4 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x38, 0, 0, "O2 S5 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x38, 1, 2, "O2 S5 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x39, 0, 0, "O2 S6 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x39, 1, 2, "O2 S6 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x3A, 0, 0, "O2 S7 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x3A, 1, 2, "O2 S7 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x3B, 0, 0, "O2 S8 Lambda (mA)", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x3B, 1, 2, "O2 S8 Current", U, 16, 1.0 / 256.0, -128.0, "mA", 3),
    pid_def(0x3C, 0, 0, "Catalyst Temp B1 S1", U, 16, 0.1, -40.0, DEG_C, 1),
    pid_def(0x3D, 0, 0, "Catalyst Temp B2 S1", U, 16, 0.1, -40.0, DEG_C, 1),
    pid_def(0x3E, 0, 0, "Catalyst Temp B1 S2", U, 16, 0.1, -40.0, DEG_C, 1),
    pid_def(0x3F, 0, 0, "Catalyst Temp B2 S2", U, 16, 0.1, -40.0, DEG_C, 1),
    pid_def(0x43, 0, 0, "Absolute Load", U, 16, kPct, 0.0, "%", 1),
    pid_def(0x44, 0, 0, "Commanded Lambda", U, 16, kRatio, 0.0, LAMBDA, 3),
    pid_def(0x45, 0, 0, "Relative Throttle Position", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x46, 0, 0, "Ambient Air Temp", U, 8, 1.0, -40.0, DEG_C, 0),
    pid_def(0x47, 0, 0, "Absolute Throttle B", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x48, 0, 0, "Absolute Throttle C", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x49, 0, 0, "Accelerator Pedal D", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x4A, 0, 0, "Accelerator Pedal E", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x4B, 0, 0, "Accelerator Pedal F", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x4C, 0, 0, "Commanded Throttle", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x4D, 0, 0, "Time Run with MIL On", U, 16, 1.0, 0.0, "min", 0),
    pid_def(0x4E, 0, 0, "Time Since Clear", U, 16, 1.0, 0.0, "min", 0),
    pid_def(0x4F, 0, 0, "Max Lambda", U, 8, 1.0, 0.0, LAMBDA, 0),
    pid_def(0x4F, 1, 1, "Max O2 Voltage", U, 8, 1.0, 0.0, "V", 0),
    pid_def(0x4F, 2, 2, "Max O2 Current", U, 8, 1.0, 0.0, "mA", 0),
    pid_def(0x4F, 3, 3, "Max Intake Pressure", U, 8, 10.0, 0.0, "kPa", 0),
    pid_def(0x50, 0, 0, "Max MAF", U, 8, 10.0, 0.0, "g/s", 0),
    pid_def(0x52, 0, 0, "Ethanol Fuel", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x53, 0, 0, "Evap Vapor Pressure (Abs)", U, 16, 0.005, 0.0, "kPa",
            3),
    /* The table reads it two's complement; an OBD2 DBC reads it
     * unsigned with offset -32767, and J1979 (2002) reserves it */
    pid_def(0x54, 0, 0, "Evap Vapor Pressure (Wide)", S, 16, 1.0, 0.0, "Pa", 0),
    pid_def(0x55, 0, 0, "Rear O2 Short Trim B1", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x55, 1, 1, "Rear O2 Short Trim B3", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x56, 0, 0, "Rear O2 Long Trim B1", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x56, 1, 1, "Rear O2 Long Trim B3", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x57, 0, 0, "Rear O2 Short Trim B2", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x57, 1, 1, "Rear O2 Short Trim B4", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x58, 0, 0, "Rear O2 Long Trim B2", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x58, 1, 1, "Rear O2 Long Trim B4", U, 8, kTrim, -100.0, "%", 1),
    pid_def(0x59, 0, 0, "Fuel Rail Pressure (Abs)", U, 16, 10.0, 0.0, "kPa", 0),
    pid_def(0x5A, 0, 0, "Relative Pedal Position", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x5B, 0, 0, "Hybrid Battery Life", U, 8, kPct, 0.0, "%", 1),
    pid_def(0x5D, 0, 0, "Fuel Injection Timing", U, 16, 0.0078125, -210.0, DEG,
            2),
    pid_def(0x5E, 0, 0, "Engine Fuel Rate", U, 16, 0.05, 0.0, "L/h", 2),
    pid_def(0x61, 0, 0, "Driver Demand Torque", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0x62, 0, 0, "Actual Engine Torque", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0x63, 0, 0, "Engine Reference Torque", U, 16, 1.0, 0.0, NM, 0),
    pid_def(0x64, 0, 0, "Torque at Idle", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0x64, 1, 1, "Torque at Point 1", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0x64, 2, 2, "Torque at Point 2", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0x64, 3, 3, "Torque at Point 3", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0x64, 4, 4, "Torque at Point 4", U, 8, 1.0, -125.0, "%", 0),
    /* 66, 67 and 68 open with a byte saying which sensors report */
    pid_def(0x66, 0, 1, "MAF Sensor A", U, 16, 0.03125, 0.0, "g/s", 2),
    pid_def(0x66, 1, 3, "MAF Sensor B", U, 16, 0.03125, 0.0, "g/s", 2),
    pid_def(0x67, 0, 1, "Coolant Temp Sensor 1", U, 8, 1.0, -40.0, DEG_C, 0),
    pid_def(0x67, 1, 2, "Coolant Temp Sensor 2", U, 8, 1.0, -40.0, DEG_C, 0),
    pid_def(0x68, 0, 1, "Intake Air Temp Sensor 1", U, 8, 1.0, -40.0, DEG_C, 0),
    pid_def(0x68, 1, 2, "Intake Air Temp Sensor 2", U, 8, 1.0, -40.0, DEG_C, 0),
    pid_def(0x8E, 0, 0, "Engine Friction Torque", U, 8, 1.0, -125.0, "%", 0),
    pid_def(0xA2, 0, 0, "Cylinder Fuel Rate", U, 16, 0.03125, 0.0, "mg/stroke",
            2),
    pid_def(0xA6, 0, 0, "Odometer", U, 32, 0.1, 0.0, "km", 1),
};

/* The signals a UI plots at session start, best first (ADR-0039 section
 * 8): the engine's state, how hard it works, how it fuels. A UI plots the
 * first few the car supports (MAP stands in for a speed-density engine's
 * missing MAF). Throttle is left to load, which says more. */
const uint16_t kPlotOrder[] = {
    ECUIO_OBD2_SIG_RPM_ID,
    ECUIO_OBD2_SIG_COOLANT_TEMP_ID,
    ECUIO_OBD2_SIG_IAT_ID,
    ECUIO_OBD2_SIG_MAF_ID,
    ECUIO_OBD2_SIG_OIL_TEMP_ID,
    ECUIO_OBD2_SIG_SPEED_ID,
    ECUIO_OBD2_SIG_ENGINE_LOAD_ID,
    ECUIO_OBD2_PID_SIG_ID(0x0E, 0),   /* timing advance */
    ECUIO_OBD2_PID_SIG_ID(0x06, 0),   /* short term fuel trim, bank 1 */
    ECUIO_OBD2_PID_SIG_ID(0x07, 0),   /* long term fuel trim, bank 1 */
    ECUIO_OBD2_PID_SIG_ID(0x0B, 0),   /* intake manifold pressure */
    ECUIO_OBD2_SIG_BATTERY_V_ID,
    ECUIO_OBD2_PID_SIG_ID(0x49, 0),   /* accelerator pedal D */
    ECUIO_OBD2_PID_SIG_ID(0x44, 0),   /* commanded lambda */
};

/* Each PID's group (ADR-0050): the part of the car its fields read.
 * Throttle and pedal go with the air they meter; a PID's fields share
 * its group, but for kGroupExceptions */
struct pid_group {
    uint8_t pid;
    uint8_t group;
};
#define G_ENGINE ECUIO_SIGNAL_GROUP_ENGINE
#define G_AIR    ECUIO_SIGNAL_GROUP_AIR
#define G_FUEL   ECUIO_SIGNAL_GROUP_FUEL
#define G_O2     ECUIO_SIGNAL_GROUP_OXYGEN
#define G_EMIT   ECUIO_SIGNAL_GROUP_EMISSIONS
#define G_CAR    ECUIO_SIGNAL_GROUP_VEHICLE
#define G_ELEC   ECUIO_SIGNAL_GROUP_ELECTRICAL
#define G_DIAG   ECUIO_SIGNAL_GROUP_DIAGNOSTICS
const pid_group kPidGroups[] = {
    {0x04, G_ENGINE}, {0x05, G_ENGINE}, {0x06, G_FUEL},   {0x07, G_FUEL},
    {0x08, G_FUEL},   {0x09, G_FUEL},   {0x0A, G_FUEL},   {0x0B, G_AIR},
    {0x0C, G_ENGINE}, {0x0D, G_CAR},    {0x0E, G_ENGINE}, {0x0F, G_AIR},
    {0x10, G_AIR},    {0x11, G_AIR},    {0x14, G_O2},     {0x15, G_O2},
    {0x16, G_O2},     {0x17, G_O2},     {0x18, G_O2},     {0x19, G_O2},
    {0x1A, G_O2},     {0x1B, G_O2},     {0x1F, G_ENGINE}, {0x21, G_DIAG},
    {0x22, G_FUEL},   {0x23, G_FUEL},   {0x24, G_O2},     {0x25, G_O2},
    {0x26, G_O2},     {0x27, G_O2},     {0x28, G_O2},     {0x29, G_O2},
    {0x2A, G_O2},     {0x2B, G_O2},     {0x2C, G_EMIT},   {0x2D, G_EMIT},
    {0x2E, G_EMIT},   {0x2F, G_FUEL},   {0x30, G_DIAG},   {0x31, G_DIAG},
    {0x32, G_EMIT},   {0x33, G_AIR},    {0x34, G_O2},     {0x35, G_O2},
    {0x36, G_O2},     {0x37, G_O2},     {0x38, G_O2},     {0x39, G_O2},
    {0x3A, G_O2},     {0x3B, G_O2},     {0x3C, G_EMIT},   {0x3D, G_EMIT},
    {0x3E, G_EMIT},   {0x3F, G_EMIT},   {0x42, G_ELEC},   {0x43, G_ENGINE},
    {0x44, G_FUEL},   {0x45, G_AIR},    {0x46, G_AIR},    {0x47, G_AIR},
    {0x48, G_AIR},    {0x49, G_AIR},    {0x4A, G_AIR},    {0x4B, G_AIR},
    {0x4C, G_AIR},    {0x4D, G_DIAG},   {0x4E, G_DIAG},   {0x4F, G_O2},
    {0x50, G_AIR},    {0x52, G_FUEL},   {0x53, G_EMIT},   {0x54, G_EMIT},
    {0x55, G_O2},     {0x56, G_O2},     {0x57, G_O2},     {0x58, G_O2},
    {0x59, G_FUEL},   {0x5A, G_AIR},    {0x5B, G_ELEC},   {0x5C, G_ENGINE},
    {0x5D, G_FUEL},   {0x5E, G_FUEL},   {0x61, G_ENGINE}, {0x62, G_ENGINE},
    {0x63, G_ENGINE}, {0x64, G_ENGINE}, {0x66, G_AIR},    {0x67, G_ENGINE},
    {0x68, G_AIR},    {0x8E, G_ENGINE}, {0xA2, G_FUEL},   {0xA6, G_CAR},
};

/* A field placed apart from its PID: 4F's largest intake pressure
 * beside the air, not the O2 maxima it shares a reply with */
const struct {
    uint16_t id;
    uint8_t group;
} kGroupExceptions[] = {
    {ECUIO_OBD2_PID_SIG_ID(0x4F, 3), G_AIR},
};
#undef G_DIAG
#undef G_ELEC
#undef G_CAR
#undef G_EMIT
#undef G_O2
#undef G_FUEL
#undef G_AIR
#undef G_ENGINE

uint8_t obd2_group(const ecuio_signal_def_t& d)
{
    for (const auto& e : kGroupExceptions) {
        if (e.id == d.id) {
            return e.group;
        }
    }
    for (const pid_group& g : kPidGroups) {
        if (g.pid == (d.mux_key & 0xFFu)) {
            return g.group;
        }
    }
    return ECUIO_SIGNAL_GROUP_OTHER;
}

#undef S
#undef U
#undef NM
#undef LAMBDA
#undef DEG
#undef DEG_C

/* ADR-0013 section 4: the standard physical pair, 0x7E8 (response) /
 * 0x7E0 (request). The transport filters and addresses from this list;
 * ?filter= and ?txid= override it. */
const ecuio_channel_decl_t kObd2Channels[] = {
    { ECUIO_OBD2_ECU_ARB_ID, ECUIO_OBD2_TESTER_ARB_ID },
};

const ecuio_signal_set_t kObd2Set = [] {
    for (size_t r = 0; r < sizeof kPlotOrder / sizeof kPlotOrder[0]; ++r) {
        for (ecuio_signal_def_t& d : kObd2Defs) {
            if (d.id == kPlotOrder[r]) {
                d.plot_rank = static_cast<uint8_t>(r + 1);
            }
        }
    }
    for (ecuio_signal_def_t& d : kObd2Defs) {
        d.group = obd2_group(d);
    }
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
