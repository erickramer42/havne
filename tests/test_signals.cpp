#include <gtest/gtest.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <utility>

class SignalDecodeTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SignalDecodeTest, ValidUint8Decode) {
    ecuio_signal_def_t signal = {};
    signal.id = 1001;
    strncpy(signal.name, "Engine_RPM", sizeof(signal.name));
    signal.kind = ECUIO_VALUE_UINT;
    signal.start_byte = 0;
    signal.bit_offset = 0;
    signal.bit_length = 16;
    signal.scale = 0.1;
    signal.offset = 0.0;
    strncpy(signal.unit, "RPM", sizeof(signal.unit));
    signal.decimal_places = 1;
    
    uint8_t frame[8] = {0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* 1000 RPM */
    double value = 0.0;
    
    int result = ecuio_decode_signal(&signal, frame, 8, &value);
    
    EXPECT_EQ(result, 0);
    EXPECT_NEAR(value, 100.0, 0.01);
}

TEST_F(SignalDecodeTest, ValidInt8WithSignExtension) {
    ecuio_signal_def_t signal = {};
    signal.id = 2001;
    strncpy(signal.name, "Coolant_Temp", sizeof(signal.name));
    signal.kind = ECUIO_VALUE_INT;
    signal.start_byte = 1;
    signal.bit_offset = 0;
    signal.bit_length = 8;
    signal.scale = 1.0;
    signal.offset = -40.0;
    strncpy(signal.unit, "°C", sizeof(signal.unit));
    signal.decimal_places = 0;
    
    uint8_t frame[8] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* -1 °C after offset */
    double value = 0.0;
    
    int result = ecuio_decode_signal(&signal, frame, 8, &value);
    
    EXPECT_EQ(result, 0);
    EXPECT_NEAR(value, -41.0, 0.01); /* -1 + (-40) = -41 */
}

TEST_F(SignalDecodeTest, NullPointerReturnsError) {
    ecuio_signal_def_t signal = {};
    uint8_t frame[8] = {0x00};
    double value = 0.0;
    
    EXPECT_EQ(ecuio_decode_signal(nullptr, frame, 8, &value), 1);
    EXPECT_EQ(ecuio_decode_signal(&signal, nullptr, 8, &value), 1);
    EXPECT_EQ(ecuio_decode_signal(&signal, frame, 8, nullptr), 1);
}

TEST_F(SignalDecodeTest, BoundsViolationReturnsError) {
    ecuio_signal_def_t signal = {};
    signal.start_byte = 10; /* Beyond frame length */
    signal.bit_offset = 0;
    signal.bit_length = 16;
    
    uint8_t frame[8] = {0x00};
    double value = 0.0;
    
    EXPECT_EQ(ecuio_decode_signal(&signal, frame, 8, &value), 2);
}

TEST_F(SignalDecodeTest, InvalidBitLengthReturnsError) {
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 0; /* Invalid */

    uint8_t frame[8] = {0x00};
    double value = 0.0;

    EXPECT_EQ(ecuio_decode_signal(&signal, frame, 8, &value), 3);
}

TEST_F(SignalDecodeTest, UnsupportedKindOrByteOrderReturnsError) {
    /* Both are checked before the bit parameters: bit_length 0 here
     * would otherwise report 3. */
    uint8_t frame[8] = {0x00};
    double value = 0.0;
    for (int kind : {static_cast<int>(ECUIO_VALUE_TEXT), 4, 255}) {
        ecuio_signal_def_t signal = {};
        signal.kind = static_cast<uint8_t>(kind);
        EXPECT_EQ(ecuio_decode_signal(&signal, frame, 8, &value), 4) << "kind " << kind;
    }
    for (int order : {2, 255}) {
        ecuio_signal_def_t signal = {};
        signal.byte_order = static_cast<uint8_t>(order);
        EXPECT_EQ(ecuio_decode_signal(&signal, frame, 8, &value), 4) << "order " << order;
    }
}

TEST_F(SignalDecodeTest, ValidateSignal_Success) {
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 16;
    signal.bit_offset = 0;
    strncpy(signal.name, "Test_Signal", sizeof(signal.name));
    strncpy(signal.unit, "units", sizeof(signal.unit));
    signal.decimal_places = 2;
    
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_OK);
}

TEST_F(SignalDecodeTest, ValidateSignal_InvalidBitLength) {
    /* Width lives in bit_length alone: 1..32. */
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 0;
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_ERR_BIT_LENGTH);
    signal.bit_length = 33;
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_ERR_BIT_LENGTH);
    signal.bit_length = 32;
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_OK);
}

TEST_F(SignalDecodeTest, ValidateSignal_UnterminatedName) {
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 8;
    /* Fill ALL 32 bytes with non-null chars — no room for terminator */
    /* Intentional: tests validator's handling of unterminated strings */
    for (size_t i = 0; i < sizeof(signal.name); ++i) {
        signal.name[i] = 'A';
    }

    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_ERR_NAME);
}

TEST_F(SignalDecodeTest, ScaleOffsetCalculation) {
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.start_byte = 0;
    signal.bit_offset = 0;
    signal.bit_length = 16;
    signal.scale = 0.01;
    signal.offset = 100.0;
    
    uint8_t frame[8] = {0xD0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* 2000 raw */
    double value = 0.0;
    
    int result = ecuio_decode_signal(&signal, frame, 8, &value);
    
    EXPECT_EQ(result, 0);
    EXPECT_NEAR(value, 120.0, 0.01); /* 2000 * 0.01 + 100 = 120 */
}

TEST(SignalsTest, MockSignalSetPassesValidation) {
    EXPECT_EQ(ecuio_validate_signal_set(ecuio_mock_signal_set(), nullptr), ECUIO_VALIDATE_OK);
}

TEST(SignalsTest, MockSetIdsAreUnique) {
    const ecuio_signal_set_t* s = ecuio_mock_signal_set();
    for (uint32_t i = 0; i < s->count; ++i)
        for (uint32_t j = i + 1; j < s->count; ++j)
            EXPECT_NE(s->defs[i].id, s->defs[j].id)
                << "duplicate ids at " << i << "," << j;
}

TEST(SignalsTest, MockSetDecodesReasonably) {
    /* Integration test: verify mock set definitions decode to plausible
     * ranges. The frame is the full mock payload; MAF and oil are pinned
     * to exact values, so a layout slip fails instead of landing in a
     * wide range. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    uint8_t frame[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN] = {
                         0xE8, 0x03, // RPM raw 1000
                         0x64,       // Coolant raw 100 → 60°C
                         0x3C,       // Speed raw 60 → 60 km/h
                         0x80,       // Throttle raw 128 → 50.2%
                         0x74,       // IAT raw 116 → 76°C
                         0xCD,       // Load raw 0xCD → 80.4%
                         0x84, 0x1D, // MAF raw 0x1D84 = 7556 → 75.56 g/s
                         0x78,       // Oil raw 120 → 80°C
                         0x00, 0x00};// bytes 10-11, pad

    for (uint32_t i = 0; i < set->count; ++i) {
        double val = 0.0;
        int rc = ecuio_decode_signal(&set->defs[i], frame, ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &val);
        EXPECT_EQ(rc, 0) << "decode failed for " << set->defs[i].name << " at index " << i;

        /* Plausibility checks per signal */
        switch (set->defs[i].id) {
        case ECUIO_MOCK_SIG_RPM_ID:
            EXPECT_GE(val, 0.0);
            EXPECT_LE(val, 8000.0);
            break;
        case ECUIO_MOCK_SIG_COOLANT_ID:
        case ECUIO_MOCK_SIG_IAT_ID:
            EXPECT_GE(val, -40.0);
            EXPECT_LE(val, 215.0);
            break;
        case ECUIO_MOCK_SIG_SPEED_ID:
            EXPECT_GE(val, 0.0);
            EXPECT_LE(val, 255.0);
            break;
        case ECUIO_MOCK_SIG_THROTTLE_ID:
        case ECUIO_MOCK_SIG_LOAD_ID:
            EXPECT_GE(val, 0.0);
            EXPECT_LE(val, 100.0);
            break;
        case ECUIO_MOCK_SIG_MAF_ID:
            EXPECT_NEAR(val, 75.56, 0.01);
            break;
        case ECUIO_MOCK_SIG_OIL_ID:
            EXPECT_DOUBLE_EQ(val, 80.0);
            break;
        }
    }
}

TEST_F(SignalDecodeTest, DecodeHonorsActualPayloadLength) {
    /* The mock encoder produces 12-byte payloads, but transports may deliver
       shorter frames (CAN classic subsets, partial ISO-TP fragments).
       Signals whose bits live past payload_len must be REJECTED, and
       signals within it must still decode. Out-value must stay untouched
       on failure. Pins the contract the bridge depends on. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);
    ASSERT_NE(set->defs, nullptr);

    uint8_t buf[64] = {0};
    /* Mark bytes we expect decoders to reject - would surface as bogus
       values if a bounds check regressed */
    memset(buf + 4, 0xEE, 4);

    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        double val = -999.0;
        const uint32_t end_bit =
            def.start_byte * 8u + def.bit_offset + def.bit_length;
        const bool inRange = end_bit <= 4u * 8u;

        if (inRange) {
            EXPECT_EQ(ecuio_decode_signal(&def, buf, 4, &val), 0)
                << "signal '" << def.name << "' wrongly rejected within payload";
        } else {
            /* Bytes at indices >= 4 hold 0xEE - a regression would decode
               0xEE instead of failing, so out_value is also asserted */
            EXPECT_EQ(ecuio_decode_signal(&def, buf, 4, &val), 2)
                << "signal '" << def.name << "' decoded beyond payload length";
            EXPECT_EQ(val, -999.0)
                << "out_value written on failure for '" << def.name << "'";
        }
    }
}

TEST_F(SignalDecodeTest, DecodeRejectsNonMultipleOfEightLengths) {
    /* Off-by-one boundary: a signal starting at byte 4 with 8 bits needs
       bytes 0-4 inclusive (5 bytes). A 4-byte payload must reject it;
       a 5-byte payload must accept it. Guards against anyone replacing
       the last_bit computation with an off-by-one byte-count check. */
    ecuio_signal_def_t signal = {};
    signal.id = 3001;
    strncpy(signal.name, "Boundary", sizeof(signal.name));
    signal.kind = ECUIO_VALUE_UINT;
    signal.start_byte = 4;
    signal.bit_offset = 0;
    signal.bit_length = 8;
    signal.scale = 1.0;
    signal.offset = 0.0;
    strncpy(signal.unit, "u", sizeof(signal.unit));

    uint8_t frame[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00};
    double value = 0.0;

    EXPECT_EQ(ecuio_decode_signal(&signal, frame, 4, &value), 2);
    EXPECT_EQ(ecuio_decode_signal(&signal, frame, 5, &value), 0);
    EXPECT_NEAR(value, 255.0, 0.01);
}

TEST_F(SignalDecodeTest, Uint32DecodesExactly)
{
    /* 2^24 + 1 is the first integer a float can't hold: a float result
     * reads it as 16,777,216. */
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 32;
    signal.scale = 1.0;

    const uint8_t frame[4] = {0x01, 0x00, 0x00, 0x01};   /* LE 0x01000001 */
    double value = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&signal, frame, 4, &value), 0);
    EXPECT_EQ(value, 16777217.0);
}

TEST_F(SignalDecodeTest, PayloadLengthPastByte255IsNotNarrowed)
{
    /* 300 narrowed to 8 bits is 44, which would put byte 250 past the
     * payload. */
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.start_byte = 250;
    signal.bit_length = 8;
    signal.scale = 1.0;

    uint8_t payload[300] = {};
    payload[250] = 0x2A;
    double value = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&signal, payload, 300, &value), 0);
    EXPECT_EQ(value, 42.0);
}

TEST_F(SignalDecodeTest, StartBytePastByte255Reaches)
{
    /* A reassembled message can run past byte 255, and start_byte is
     * 16 bits. 300 in 8 bits is 44, which holds 0xEE here. */
    uint8_t payload[320] = {};
    payload[44] = 0xEE;
    payload[45] = 0xEE;
    payload[300] = 0x12;
    payload[301] = 0x34;
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.start_byte = 300;
    signal.bit_length = 16;
    signal.scale = 1.0;
    double value = 0.0;

    ASSERT_EQ(ecuio_decode_signal(&signal, payload, 320, &value), 0);
    EXPECT_EQ(value, 13330.0);   /* 0x3412 */
    signal.byte_order = ECUIO_BYTE_ORDER_BE;
    ASSERT_EQ(ecuio_decode_signal(&signal, payload, 320, &value), 0);
    EXPECT_EQ(value, 4660.0);    /* 0x1234 */
    EXPECT_EQ(ecuio_decode_signal(&signal, payload, 301, &value), 2);
}

TEST_F(SignalDecodeTest, FloatDecodesInBothByteOrders)
{
    /* pi as binary32 is 0x40490FDB. */
    const uint32_t bits = 0x40490FDBu;
    float pi = 0.0f;
    std::memcpy(&pi, &bits, sizeof pi);

    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_FLOAT;
    signal.bit_length = 32;
    signal.scale = 1.0;
    double value = 0.0;

    const uint8_t le[4] = {0xDB, 0x0F, 0x49, 0x40};
    ASSERT_EQ(ecuio_decode_signal(&signal, le, 4, &value), 0);
    EXPECT_EQ(value, static_cast<double>(pi));
    EXPECT_NEAR(value, 3.14159274, 1e-8);

    const uint8_t be[4] = {0x40, 0x49, 0x0F, 0xDB};
    signal.byte_order = ECUIO_BYTE_ORDER_BE;
    value = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&signal, be, 4, &value), 0);
    EXPECT_EQ(value, static_cast<double>(pi));
}

TEST_F(SignalDecodeTest, BigEndianIntSignExtends)
{
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_INT;
    signal.byte_order = ECUIO_BYTE_ORDER_BE;
    signal.start_byte = 1;
    signal.bit_length = 16;
    signal.scale = 0.5;

    const uint8_t frame[3] = {0x00, 0xFF, 0x38};   /* 0xFF38 = -200 */
    double value = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&signal, frame, 3, &value), 0);
    EXPECT_EQ(value, -100.0);
}

TEST_F(SignalDecodeTest, MuxKeyReadsFirstByteMostSignificantInEitherOrder)
{
    /* The key is a byte sequence, so byte_order doesn't apply to it:
     * 0x4105 matches payload bytes 41 05 for a little-endian field too. */
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.start_byte = 2;
    signal.bit_length = 8;
    signal.scale = 1.0;
    signal.message_id = 0x7E8u;
    signal.mux_offset = 0;
    signal.mux_length = 2;
    signal.mux_key = 0x4105u;

    ecuio_frame_t f = {};
    f.arbitration_id = 0x7E8u;
    f.data_length = 3;
    f.data[0] = 0x41;
    f.data[1] = 0x05;
    f.data[2] = 0x7B;
    for (uint8_t order : {static_cast<uint8_t>(ECUIO_BYTE_ORDER_LE),
                          static_cast<uint8_t>(ECUIO_BYTE_ORDER_BE)}) {
        signal.byte_order = order;
        double value = 0.0;
        EXPECT_EQ(ecuio_def_applies_to_frame(&signal, &f), 1) << int(order);
        EXPECT_EQ(ecuio_decode_signal(&signal, f.data, f.data_length, &value), 0) << int(order);
        EXPECT_EQ(value, 123.0) << int(order);
    }
    std::swap(f.data[0], f.data[1]);
    double value = 0.0;
    EXPECT_EQ(ecuio_def_applies_to_frame(&signal, &f), 0);
    EXPECT_EQ(ecuio_decode_signal(&signal, f.data, f.data_length, &value), 5);
}

TEST_F(SignalDecodeTest, ThreeByteMuxKeyGatesAndDecodes)
{
    /* A 62 <DID> response: the service echo and a 2-byte identifier,
     * one key, first byte most significant. Every key byte counts. */
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.byte_order = ECUIO_BYTE_ORDER_BE;
    signal.start_byte = 3;
    signal.bit_length = 16;
    signal.scale = 1.0;
    signal.message_id = 0x7E8u;
    signal.mux_offset = 0;
    signal.mux_length = 3;
    signal.mux_key = 0x621234u;
    ASSERT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_OK);

    ecuio_frame_t f = {};
    f.arbitration_id = 0x7E8u;
    f.data_length = 5;
    const uint8_t wire[5] = {0x62, 0x12, 0x34, 0x01, 0x02};
    std::memcpy(f.data, wire, sizeof wire);
    double value = 0.0;
    EXPECT_EQ(ecuio_def_applies_to_frame(&signal, &f), 1);
    ASSERT_EQ(ecuio_decode_signal(&signal, f.data, f.data_length, &value), 0);
    EXPECT_EQ(value, 258.0);

    for (int i = 0; i < 3; ++i) {
        ecuio_frame_t g = f;
        g.data[i] ^= 0x01;
        EXPECT_EQ(ecuio_def_applies_to_frame(&signal, &g), 0) << "key byte " << i;
        EXPECT_EQ(ecuio_decode_signal(&signal, g.data, g.data_length, &value), 5)
            << "key byte " << i;
    }

    /* A fourth key byte is past the limit: the gate never applies the
     * def and decode refuses it as a bad parameter. */
    signal.mux_length = 4;
    signal.mux_key = 0x62123401u;
    EXPECT_EQ(ecuio_def_applies_to_frame(&signal, &f), 0);
    EXPECT_EQ(ecuio_decode_signal(&signal, f.data, f.data_length, &value), 3);
}

TEST(SignalsTest, SignalDefLayoutIsPinned)
{
    /* ABI pin, matching the static asserts in signals.h. No padding:
     *   id=0, reserved0=2, message_id=4, mux_offset=8, mux_length=9,
     *   mux_reserved=10, mux_key=12, kind=16, byte_order=17,
     *   start_byte=18, bit_offset=20, bit_length=21,
     *   decode_reserved=22, scale=24, offset=32, name=40, unit=72,
     *   decimal_places=88, display_reserved=89, flags=92,
     *   reserved=96  =>  sizeof == 120.
     * If this fires, a field moved: bump ECUIO_SIGNAL_SET_LAYOUT. */
    EXPECT_EQ(sizeof(ecuio_signal_def_t), 120u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, reserved0), 2u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, message_id), 4u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, mux_offset), 8u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, mux_key), 12u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, kind), 16u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, byte_order), 17u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, start_byte), 18u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, bit_offset), 20u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, decode_reserved), 22u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, scale), 24u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, offset), 32u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, name), 40u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, unit), 72u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, decimal_places), 88u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, display_reserved), 89u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, flags), 92u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, reserved), 96u);

    /* Core reads layout_version before it knows anything else about a
     * set. 3, so a set that carries an earlier layout's 1 or 2 there
     * is refused. */
    EXPECT_EQ(offsetof(ecuio_signal_set_t, layout_version), 0u);
    EXPECT_EQ(ECUIO_SIGNAL_SET_LAYOUT, 3u);
}

TEST(SignalsTest, MockAndObd2ShareDisplayNamesAndUnits)
{
    /* The UI shows names, units and decimal places as core gives them,
     * and the Dashboard's gauges find their signals by name (the Taps in
     * DashboardPage.qml). A quantity both sets carry has one name, unit
     * and precision in both, so a gauge reads under either profile.
     * Temperatures are a UTF-8 degree sign, never "degC". */
    const char* const kShared[] = {"RPM", "Vehicle Speed", "Coolant Temp",
                                   "Engine Oil Temp", "Intake Air Temp", "MAF",
                                   "Throttle Position", "Engine Load"};
    auto find = [](const ecuio_signal_set_t* set, const char* name) -> const ecuio_signal_def_t* {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (std::strcmp(set->defs[i].name, name) == 0) return &set->defs[i];
        }
        return nullptr;
    };
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();

    for (const char* name : kShared) {
        const ecuio_signal_def_t* m = find(mock, name);
        const ecuio_signal_def_t* o = find(obd2, name);
        ASSERT_NE(m, nullptr) << "mock set lacks '" << name << "'";
        ASSERT_NE(o, nullptr) << "obd2 set lacks '" << name << "'";
        EXPECT_STREQ(m->unit, o->unit) << name;
        EXPECT_EQ(m->decimal_places, o->decimal_places) << name;
    }
    for (const ecuio_signal_set_t* set : {mock, obd2}) {
        for (uint32_t i = 0; i < set->count; ++i) {
            EXPECT_STRNE(set->defs[i].unit, "degC") << set->defs[i].name;
        }
    }
    EXPECT_STREQ(find(mock, "Coolant Temp")->unit, "\xC2\xB0" "C");
}

TEST(SignalsTest, MockSignalsDeclareTelemetryMessageId)
{
    /* Every mock signal rides the telemetry CAN message. Half of the
     * bridge's F2 decode gate: transport side is pinned by
     * MockEmitsKnownArbitrationIds (test_core). */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);
    for (uint32_t i = 0; i < set->count; ++i) {
        EXPECT_EQ(set->defs[i].message_id, 0x0C0u)
            << "signal '" << set->defs[i].name << "' not on telemetry ID";
    }
}

namespace {

/* A valid 32-bit field at byte 0: only the kind, the byte order and
 * the field shape vary in the tests below. Takes ints so a test can
 * pass values no enum names. */
ecuio_signal_def_t four_byte_def(int kind, int byte_order = ECUIO_BYTE_ORDER_LE)
{
    ecuio_signal_def_t d = {};
    d.id = 1;
    d.kind = static_cast<uint8_t>(kind);
    d.byte_order = static_cast<uint8_t>(byte_order);
    d.start_byte = 0;
    d.bit_offset = 0;
    d.bit_length = 32;
    d.scale = 1.0;
    return d;
}

} // namespace

TEST(SignalsTest, ValidatorRejectsKindsWithNoDecoder)
{
    /* TEXT and unknown kinds have no decoder, so a def of one would
     * register and never decode. */
    for (int k : {static_cast<int>(ECUIO_VALUE_TEXT), 4, 255}) {
        ecuio_signal_def_t d = four_byte_def(k);
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_KIND) << "kind " << k;
    }
    /* The kind is checked first, as in the decoder: a bad kind with a
     * bad byte order and bad bit parameters reports the kind. */
    ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_TEXT, 2);
    d.bit_length = 0;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_KIND);
}

TEST(SignalsTest, ValidatorRejectsUnknownByteOrder)
{
    for (int order : {2, 255}) {
        ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_UINT, order);
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BYTE_ORDER) << "order " << order;
        /* Before the bit parameters, as in the decoder. */
        d.bit_length = 0;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BYTE_ORDER) << "order " << order;
    }
}

TEST(SignalsTest, ValidatorRequiresFloatAs32BitsAtBitZero)
{
    for (int order : {static_cast<int>(ECUIO_BYTE_ORDER_LE),
                      static_cast<int>(ECUIO_BYTE_ORDER_BE)}) {
        ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_FLOAT, order);
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK) << "order " << order;
        d.bit_length = 16;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_FLOAT32) << "order " << order;
    }
    ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_FLOAT);
    d.bit_offset = 1;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_FLOAT32);
}

TEST(SignalsTest, ValidatorRequiresByteAlignedBigEndian)
{
    /* The big-endian extractor reads whole bytes. */
    ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE);
    d.bit_length = 16;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
    d.bit_length = 12;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BIG_ENDIAN);
    d.bit_length = 16;
    d.bit_offset = 1;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BIG_ENDIAN);
}

TEST(SignalsTest, ValidatorBoundsMuxKeysToThreeBytes)
{
    /* 1 to 3 key bytes, and a key no wider than its length. The key
     * may start at any byte a mux_offset names: each fits the frame's
     * payload, and decode finds a key past a shorter one. */
    const struct { uint8_t length; uint32_t widest; } kLengths[] = {
        {1, 0xFFu}, {2, 0xFFFFu}, {3, 0xFFFFFFu}};
    for (const auto& l : kLengths) {
        ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_UINT);
        d.mux_length = l.length;
        d.mux_key = l.widest;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK) << int(l.length);
        d.mux_key = l.widest + 1u;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_MUX_KEY) << int(l.length);
        d.mux_key = 0;
        d.mux_offset = 255;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK) << int(l.length);
    }
    for (int length : {4, 5, 255}) {
        ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_UINT);
        d.mux_length = static_cast<uint8_t>(length);
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_MUX_LENGTH) << length;
    }

    /* The decoder draws the same line, with the key present in the
     * payload at every length. */
    const uint8_t payload[4] = {0x62, 0x12, 0x34, 0x56};
    for (uint8_t length = 0; length <= 4; ++length) {
        ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_UINT);
        d.mux_length = length;
        for (uint8_t i = 0; i < length; ++i) {
            d.mux_key = (d.mux_key << 8) | payload[i];
        }
        double v = 0.0;
        const bool valid = ecuio_validate_signal(&d) == ECUIO_VALIDATE_OK;
        const bool decodes =
            ecuio_decode_signal(&d, payload, sizeof payload, &v) == ECUIO_DECODE_OK;
        EXPECT_EQ(valid, decodes) << int(length);
        EXPECT_EQ(valid, length <= 3) << int(length);
    }
}

TEST(SignalsTest, ValidatorRefusesEveryNonzeroReservedByte)
{
    /* Each reserved byte, and flags, must be zero, so a later field
     * can take the room and existing sets still load. */
    struct Room { size_t offset; size_t size; const char* name; };
#define ROOM(field) {offsetof(ecuio_signal_def_t, field), sizeof(ecuio_signal_def_t::field), #field}
    const Room rooms[] = {
        ROOM(reserved0), ROOM(mux_reserved), ROOM(decode_reserved),
        ROOM(display_reserved), ROOM(flags), ROOM(reserved),
    };
#undef ROOM
    const ecuio_signal_def_t good = four_byte_def(ECUIO_VALUE_UINT);
    ASSERT_EQ(ecuio_validate_signal(&good), ECUIO_VALIDATE_OK);
    size_t checked = 0;
    for (const Room& room : rooms) {
        for (size_t i = 0; i < room.size; ++i) {
            ecuio_signal_def_t d = good;
            reinterpret_cast<unsigned char*>(&d)[room.offset + i] = 0x01;
            EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_RESERVED)
                << room.name << " byte " << i << " (offset " << room.offset + i << ")";
            ++checked;
        }
    }
    /* Every byte that isn't a named value: 2 + 2 + 2 + 3 + 4 + 24. */
    EXPECT_EQ(checked, 37u);
}

TEST(SignalsTest, ValidatorAcceptsExactlyWhatTheDecoderDecodes)
{
    /* The contract the validator exists for: over every kind, both byte
     * orders and an unknown one, and a spread of field shapes, a def
     * validates if and only if it decodes an in-bounds payload. */
    const uint8_t payload[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const int kinds[] = {0, 1, 2, 3, 4, 255};
    const int orders[] = {0, 1, 2};
    const uint8_t lengths[] = {0, 1, 7, 8, 12, 16, 24, 32, 33};
    for (int k : kinds) {
        for (int order : orders) {
            for (uint8_t len : lengths) {
                for (int off : {0, 1, 7, 8}) {
                    ecuio_signal_def_t d = four_byte_def(k, order);
                    d.bit_length = len;
                    d.bit_offset = static_cast<uint8_t>(off);
                    const bool valid = ecuio_validate_signal(&d) == ECUIO_VALIDATE_OK;
                    double v = 0.0;
                    const bool decodes =
                        ecuio_decode_signal(&d, payload, sizeof payload, &v) == ECUIO_DECODE_OK;
                    EXPECT_EQ(valid, decodes) << "kind " << k << " order " << order
                                              << " len " << int(len) << " off " << off;
                }
            }
        }
    }
}

TEST(SignalsTest, SetValidationChecksLayoutVersionFirst)
{
    /* A set built for another layout has its members elsewhere, so the
     * version is read before anything else, even NULL defs or a bad
     * def. */
    ecuio_signal_def_t bad_def = four_byte_def(ECUIO_VALUE_UINT);
    bad_def.bit_length = 0;
    for (uint32_t version : {0u, 1u, 2u, 4u}) {
        ecuio_signal_set_t set = {};
        set.layout_version = version;
        EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_LAYOUT) << version;
        set.count = 1;
        EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_LAYOUT) << version;
        set.defs = &bad_def;
        EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_LAYOUT) << version;
    }
}

TEST(SignalsTest, SetValidationRefusesNonzeroReservedFields)
{
    ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    ASSERT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_OK);
    set.reserved0 = 1;
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_SET_RESERVED);
    set.reserved0 = 0;
    for (size_t i = 0; i < sizeof(set.reserved) / sizeof(set.reserved[0]); ++i) {
        set.reserved[i] = 0x80000000u;
        EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_SET_RESERVED) << i;
        set.reserved[i] = 0;
    }
}

TEST(SignalsTest, SetValidationRejectsDuplicateIds)
{
    ecuio_signal_def_t defs[2] = {four_byte_def(ECUIO_VALUE_UINT),
                               four_byte_def(ECUIO_VALUE_INT)};
    ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_DUPLICATE_ID);
    defs[1].id = 2;
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_OK);
}

TEST(SignalsTest, SetValidationAcceptsAnEmptySet)
{
    /* No defs: decodes nothing, may still declare channels (a filtered
     * log-only profile). count > 0 with no defs stays an error. */
    static const ecuio_channel_decl_t kChan[] = {{0x7E8u, 0x7E0u}};
    ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_OK);
    set.channel_count = 1;
    set.channels = kChan;
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_OK);
    set.count = 1;
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_ERR_NULL);
    EXPECT_EQ(ecuio_validate_signal_set(nullptr, nullptr), ECUIO_VALIDATE_ERR_NULL);
}

TEST(SignalsTest, SetValidationNamesTheFailingElement)
{
    /* out carries the returned code and the def or channel at fault,
     * the second of a duplicate pair. A pass, and a code about the set
     * as a whole, carry UINT32_MAX. The reserved words come back zero
     * whatever out held before. */
    auto expect = [](const char* what, const ecuio_signal_set_t* set,
                     int code, uint32_t index) {
        SCOPED_TRACE(what);
        ecuio_validation_t v;
        std::memset(&v, 0xAB, sizeof v);
        EXPECT_EQ(ecuio_validate_signal_set(set, &v), code);
        EXPECT_EQ(v.code, code);
        EXPECT_EQ(v.index, index);
        EXPECT_EQ(v.reserved[0], 0u);
        EXPECT_EQ(v.reserved[1], 0u);
    };
    ecuio_signal_def_t defs[3] = {four_byte_def(ECUIO_VALUE_UINT),
                                  four_byte_def(ECUIO_VALUE_UINT),
                                  four_byte_def(ECUIO_VALUE_UINT)};
    defs[1].id = 2;
    defs[2].id = 3;
    ecuio_channel_decl_t channels[3] = {{0x7E8u, 0x7E0u},
                                        {0x7E9u, 0x7E1u},
                                        {0x7EAu, 0x7E2u}};
    ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    set.channel_count = 3;
    set.channels = channels;
    expect("valid", &set, ECUIO_VALIDATE_OK, UINT32_MAX);

    defs[2].bit_length = 0;
    expect("bad def", &set, ECUIO_VALIDATE_ERR_BIT_LENGTH, 2u);
    defs[2].bit_length = 32;
    defs[2].id = defs[0].id;
    expect("duplicate id", &set, ECUIO_VALIDATE_ERR_DUPLICATE_ID, 2u);
    defs[2].id = 3;
    channels[1].tx_id = 0;
    expect("bad channel id", &set, ECUIO_VALIDATE_ERR_CHANNEL_ID, 1u);
    channels[1].tx_id = 0x7E1u;
    channels[2].rx_id = channels[1].rx_id;
    expect("duplicate rx", &set, ECUIO_VALIDATE_ERR_DUPLICATE_RX, 2u);
    channels[2].rx_id = 0x7EAu;
    expect("valid again", &set, ECUIO_VALIDATE_OK, UINT32_MAX);

    expect("NULL set", nullptr, ECUIO_VALIDATE_ERR_NULL, UINT32_MAX);
    ecuio_signal_set_t whole = set;
    whole.layout_version = 2;
    expect("layout", &whole, ECUIO_VALIDATE_ERR_LAYOUT, UINT32_MAX);
    whole = set;
    whole.reserved[3] = 1;
    expect("set reserved", &whole, ECUIO_VALIDATE_ERR_SET_RESERVED, UINT32_MAX);
    whole = set;
    whole.defs = nullptr;
    expect("NULL defs", &whole, ECUIO_VALIDATE_ERR_NULL, UINT32_MAX);
    whole = set;
    whole.channels = nullptr;
    expect("NULL channels", &whole, ECUIO_VALIDATE_ERR_CHANNELS_NULL, UINT32_MAX);
    whole = set;
    whole.channel_count = ECUIO_MAX_CHANNELS + 1;
    expect("channel count", &whole, ECUIO_VALIDATE_ERR_CHANNEL_COUNT, UINT32_MAX);
}

TEST(SignalsTest, ApplyGateIsTruePredicate)
{
    /* 1 when the def applies, 0 otherwise, including NULL arguments:
     * callers write if (ecuio_def_applies_to_frame(...)). */
    ecuio_signal_def_t d = four_byte_def(ECUIO_VALUE_UINT);
    d.message_id = 0x123u;
    ecuio_frame_t f = {};
    f.arbitration_id = 0x123u;
    f.data_length = 8;
    EXPECT_EQ(ecuio_def_applies_to_frame(&d, &f), 1);
    f.arbitration_id = 0x124u;
    EXPECT_EQ(ecuio_def_applies_to_frame(&d, &f), 0);
    EXPECT_EQ(ecuio_def_applies_to_frame(nullptr, &f), 0);
    EXPECT_EQ(ecuio_def_applies_to_frame(&d, nullptr), 0);

    /* Mux: the key must be inside data_length and equal. */
    f.arbitration_id = 0x123u;
    d.mux_offset = 1;
    d.mux_length = 1;
    d.mux_key = 0xAAu;
    f.data[1] = 0xAA;
    EXPECT_EQ(ecuio_def_applies_to_frame(&d, &f), 1);
    f.data_length = 1;
    EXPECT_EQ(ecuio_def_applies_to_frame(&d, &f), 0);
    f.data_length = 8;
    f.data[1] = 0xAB;
    EXPECT_EQ(ecuio_def_applies_to_frame(&d, &f), 0);
}
