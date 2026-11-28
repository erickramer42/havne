// File/layer: tests/test_signals.cpp | GTest, core signal tests, C++17, Qt-free
// Testing: UNIT: SignalDecodeTest, SignalsTest (test_signals)

#include "test_signals_support.h"

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
    
    uint8_t frame[8] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* -1 deg C after offset */
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
    for (int kind : {static_cast<int>(ECUIO_VALUE_TEXT),
                     static_cast<int>(ECUIO_VALUE_HEX), 6, 255}) {
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

TEST_F(SignalDecodeTest, ValidateSignal_NameUnitDecimals)
{
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 8;
    /* Fill ALL 32 bytes with non-null chars: no room for terminator */
    /* Intentional: tests validator's handling of unterminated strings */
    for (size_t i = 0; i < sizeof(signal.name); ++i) {
        signal.name[i] = 'A';
    }

    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_ERR_NAME);

    /* The unit the same way, and at most 3 decimal places */
    signal.name[sizeof(signal.name) - 1] = '\0';
    memset(signal.unit, 'u', sizeof(signal.unit));
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_ERR_UNIT);
    signal.unit[sizeof(signal.unit) - 1] = '\0';
    signal.decimal_places = 4;
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_ERR_DECIMALS);
}

/* A recording writes names and units into signals.csv, where a
 * spreadsheet runs a cell starting with one of these as a formula
 * (ADR-0049 section 8); one anywhere after the first is plain text */
TEST(SignalsTest, ValidatorRefusesANameOrUnitAFormulaWouldStart)
{
    ecuio_signal_def_t signal = {};
    signal.kind = ECUIO_VALUE_UINT;
    signal.bit_length = 8;
    const char* name_ok = "Boost (-1 to +2)";
    memcpy(signal.name, name_ok, strlen(name_ok) + 1);
    memcpy(signal.unit, "km/h", 5);
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_OK);

    const char leads[] = {'=', '+', '-', '@', '\t', '\r'};
    for (char c : leads) {
        SCOPED_TRACE(static_cast<int>(c));
        ecuio_signal_def_t d = signal;
        d.name[0] = c;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_FORMULA);
        d = signal;
        d.unit[0] = c;
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_FORMULA);
    }

    /* An empty name or unit is no formula */
    signal.name[0] = '\0';
    signal.unit[0] = '\0';
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_OK);
    signal.decimal_places = 3;
    EXPECT_EQ(ecuio_validate_signal(&signal), ECUIO_VALIDATE_OK);

    EXPECT_EQ(ecuio_validate_signal(nullptr), ECUIO_VALIDATE_ERR_NULL);
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
     * ranges. The frame is the full mock payload; MAF, oil and lambda
     * are pinned to exact values, so a layout slip fails instead of
     * landing in a wide range. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    uint8_t frame[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN] = {
        0xE8, 0x03,  // RPM raw 1000
        0x64,        // Coolant raw 100 -> 60 deg C
        0x3C,        // Speed raw 60 -> 60 km/h
        0x80,        // Throttle raw 128 -> 50.2%
        0x74,        // IAT raw 116 -> 76 deg C
        0xCD,        // Load raw 0xCD -> 80.4%
        0x84, 0x1D,  // MAF raw 0x1D84 = 7556 -> 75.56 g/s
        0x78,        // Oil raw 120 -> 80 deg C
        0x14, 0x6E,  // Lambda raw 0x6E14 = 28180 -> 0.86
        0x25,        // MAP raw 37 -> 37 kPa
        0x65,        // Baro raw 101 -> 101 kPa
        0xB0, 0x36}; // Battery raw 0x36B0 = 14000 -> 14.0 V

    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) {
            continue; /* boost: no field of its own */
        }
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
        case ECUIO_MOCK_SIG_LAMBDA_ID:
            EXPECT_NEAR(val, 0.86, 0.0001);
            break;
        case ECUIO_MOCK_SIG_MAP_ID:
            EXPECT_DOUBLE_EQ(val, 37.0);
            break;
        case ECUIO_MOCK_SIG_BARO_ID:
            EXPECT_DOUBLE_EQ(val, 101.0);
            break;
        case ECUIO_MOCK_SIG_BATTERY_V_ID:
            EXPECT_NEAR(val, 14.0, 1e-9);
            break;
        }
    }
}

TEST_F(SignalDecodeTest, DecodeHonorsActualPayloadLength) {
    /* The mock encoder produces 16-byte payloads, but transports may deliver
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
        if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
            continue; /* no field to bound */
        }
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

    /* A key running past the payload is past the payload, as a value
     * would be, even when the value fits. */
    signal.mux_length = 3;
    signal.mux_key = 0x621234u;
    signal.mux_offset = 3;
    EXPECT_EQ(ecuio_decode_signal(&signal, f.data, f.data_length, &value),
              ECUIO_DECODE_ERR_PAST_PAYLOAD);

    /* The gate reads a data_length past data[] as the whole of it. */
    signal.mux_offset = 0;
    f.data_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX + 1u);
    EXPECT_EQ(ecuio_def_applies_to_frame(&signal, &f), 1);
}

/* An error report (SocketCAN's, ID = its class bits) on a def's ID
 * carries the error's detail, never the def's message: 0x0C0, the
 * mock's telemetry ID, is BUSERROR | BUSOFF. Data on the same ID still
 * applies. */
TEST(SignalsTest, AnErrorFrameAppliesToNoDef)
{
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);
    ASSERT_GT(set->count, 0u);
    ecuio_frame_t f = {};
    f.arbitration_id = ECUIO_MOCK_TELEMETRY_ARB_ID;
    f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    for (uint32_t i = 0; i < set->count; ++i) {
        f.flags = ECUIO_FRAME_FLAG_FD;
        /* A computed def applies to no frame at all (boost) */
        const int data = (set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) ? 0 : 1;
        EXPECT_EQ(ecuio_def_applies_to_frame(&set->defs[i], &f), data)
            << set->defs[i].name;
        f.flags = ECUIO_FRAME_FLAG_ERROR;
        EXPECT_EQ(ecuio_def_applies_to_frame(&set->defs[i], &f), 0)
            << set->defs[i].name;
    }
}

TEST(SignalsTest, SignalDefLayoutIsPinned)
{
    /* ABI pin, matching the static asserts in signals.h. No padding:
     *   id=0, reserved0=2, message_id=4, mux_offset=8, mux_length=9,
     *   mux_reserved=10, mux_key=12, kind=16, byte_order=17,
     *   start_byte=18, bit_offset=20, bit_length=21,
     *   byte_length=22, scale=24, offset=32, name=40, unit=72,
     *   decimal_places=88, plot_rank=89, group=90, display_reserved=91,
     *   flags=92, reserved=96  =>  sizeof == 120.
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
    EXPECT_EQ(offsetof(ecuio_signal_def_t, byte_length), 22u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, scale), 24u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, offset), 32u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, name), 40u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, unit), 72u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, decimal_places), 88u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, plot_rank), 89u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, group), 90u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, display_reserved), 91u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, flags), 92u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, quantity_reserved), 96u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, table_first), 100u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, table_count), 102u);
    EXPECT_EQ(offsetof(ecuio_signal_def_t, reserved), 104u);

    /* Core reads layout_version before it knows anything else about a
     * set. 5, so a set that carries an earlier layout's number there is
     * refused: layout 4's channels are 8 bytes each, layout 5's 16. */
    EXPECT_EQ(offsetof(ecuio_signal_set_t, layout_version), 0u);
    EXPECT_EQ(ECUIO_SIGNAL_SET_LAYOUT, 5u);
    EXPECT_EQ(sizeof(ecuio_channel_decl_t), 16u);
    EXPECT_EQ(offsetof(ecuio_channel_decl_t, tx_id), 4u);
    EXPECT_EQ(offsetof(ecuio_channel_decl_t, flags), 8u);
    EXPECT_EQ(offsetof(ecuio_channel_decl_t, session), 12u);
    EXPECT_EQ(offsetof(ecuio_channel_decl_t, reserved), 13u);
    EXPECT_EQ(offsetof(ecuio_signal_set_t, enum_count), 2 * sizeof(void*) + 16);
    EXPECT_EQ(offsetof(ecuio_signal_set_t, computed), 3 * sizeof(void*) + 24);
    EXPECT_EQ(sizeof(ecuio_signal_set_t), 4 * sizeof(void*) + 40);
}

TEST(SignalsTest, DisplayValueLayoutIsPinned)
{
    EXPECT_EQ(sizeof(ecuio_display_value_t), 32u);
    EXPECT_EQ(offsetof(ecuio_display_value_t, unit), 8u);
    EXPECT_EQ(offsetof(ecuio_display_value_t, decimal_places), 24u);
    EXPECT_EQ(offsetof(ecuio_display_value_t, reserved), 26u);
}

TEST(SignalsTest, DisplayConvertMapsEachMetricUnitToImperial)
{
    /* Every conversion's factor and offset, exact, with its unit and its
     * decimal places: a coarser unit gains one. */
    struct Case {
        const char* from;
        uint8_t dp;
        double in;
        const char* to;
        double out;
        uint8_t out_dp;
    };
    const Case kCases[] = {
        {"km/h", 0, 100.0, "mph", 62.1371192237334, 0},
        {"\xC2\xB0" "C", 0, 100.0, "\xC2\xB0" "F", 212.0, 0},
        {"\xC2\xB0" "C", 0, -40.0, "\xC2\xB0" "F", -40.0, 0},
        {"kPa", 0, 101.325, "psi", 14.6959487755134, 1},
        {"kPa", 0, 100.0, "psi", 14.5037737730209, 1},   /* the same pressure as 1 bar */
        {"bar", 0, 1.0, "psi", 14.5037737730209, 0},   /* psi is finer than bar: no extra place */
        {"bar", 2, 2.4, "psi", 34.80905705525016, 2},
        {"g/s", 1, 100.0, "lb/min", 13.2277357312033, 2},
        {"g/s", 3, 1.0, "lb/min", 0.132277357312033, 3},   /* capped at 3 */
        {"km", 1, 402.336 / 1000.0, "mi", 0.25, 1},
        {"Pa", 0, 249.08891, "inH2O", 1.0, 2},
        {"Pa", 2, -8192.0, "inH2O", -32.8878551839180, 3},   /* capped at 3 */
        {"L/h", 2, 3.785411784, "gal/h", 1.0, 3},
        {"N\xC2\xB7m", 0, 1.3558179483314004, "lb\xC2\xB7" "ft", 1.0, 0},
    };
    for (const Case& c : kCases) {
        ecuio_display_value_t out;
        std::memset(&out, 0xAB, sizeof out);
        ASSERT_EQ(ecuio_display_convert(c.from, c.dp, c.in, ECUIO_UNITS_IMPERIAL, &out),
                  ECUIO_DISPLAY_OK) << c.from;
        EXPECT_NEAR(out.value, c.out, 1e-9) << c.from;
        EXPECT_STREQ(out.unit, c.to) << c.from;
        EXPECT_EQ(out.decimal_places, c.out_dp) << c.from;
        EXPECT_EQ(out.converted, 1) << c.from;
        for (uint8_t r : out.reserved) {
            EXPECT_EQ(r, 0) << c.from;
        }
    }
}

TEST(SignalsTest, EveryImperialConversionIsAffine)
{
    /* The app turns a level typed in the display unit back into the def's
     * from two points (SessionBridge::decodedValue), which holds only while
     * each conversion is a factor and an offset. */
    const char* const kUnits[] = {
        "km/h", ("\xC2\xB0" "C"), "kPa", "bar", "g/s", "km", "Pa", "L/h", "N\xC2\xB7m",
    };
    const double kValues[] = {-8192.0, -40.0, -1.5, 0.25, 2.0, 101.325, 1000.0, 65535.0};
    for (const char* unit : kUnits) {
        ecuio_display_value_t at0;
        ecuio_display_value_t at1;
        ASSERT_EQ(ecuio_display_convert(unit, 0, 0.0, ECUIO_UNITS_IMPERIAL, &at0), ECUIO_DISPLAY_OK);
        ASSERT_EQ(ecuio_display_convert(unit, 0, 1.0, ECUIO_UNITS_IMPERIAL, &at1), ECUIO_DISPLAY_OK);
        ASSERT_EQ(at0.converted, 1) << unit;
        ASSERT_NE(at1.value, at0.value) << unit;
        for (double v : kValues) {
            ecuio_display_value_t out;
            ASSERT_EQ(ecuio_display_convert(unit, 0, v, ECUIO_UNITS_IMPERIAL, &out), ECUIO_DISPLAY_OK);
            const double line = at0.value + v * (at1.value - at0.value);
            EXPECT_NEAR(out.value, line, 1e-9 * (1.0 + std::fabs(line))) << unit << " at " << v;
            EXPECT_NEAR((out.value - at0.value) / (at1.value - at0.value), v,
                        1e-9 * (1.0 + std::fabs(v))) << unit << " at " << v;
        }
    }
}

TEST(SignalsTest, DisplayConvertPassesOtherUnitsAndMetricThrough)
{
    /* Units with no imperial form, an empty unit, and every unit under
     * metric come back as they went in. A unit is matched whole, so a
     * longer string that starts with "km" is not "km". */
    const char* const kOthers[] = {"RPM", "%", "V", "\xCE\xBB", "", "km/l", "degC"};
    for (const char* unit : kOthers) {
        ecuio_display_value_t out;
        ASSERT_EQ(ecuio_display_convert(unit, 2, 12.5, ECUIO_UNITS_IMPERIAL, &out),
                  ECUIO_DISPLAY_OK) << unit;
        EXPECT_EQ(out.value, 12.5) << unit;
        EXPECT_STREQ(out.unit, unit);
        EXPECT_EQ(out.decimal_places, 2) << unit;
        EXPECT_EQ(out.converted, 0) << unit;
    }
    ecuio_display_value_t out;
    ASSERT_EQ(ecuio_display_convert("km/h", 0, 100.0, ECUIO_UNITS_METRIC, &out),
              ECUIO_DISPLAY_OK);
    EXPECT_EQ(out.value, 100.0);
    EXPECT_STREQ(out.unit, "km/h");
    EXPECT_EQ(out.converted, 0);

    /* A unit longer than the field is cut, still terminated */
    ASSERT_EQ(ecuio_display_convert("a-unit-far-too-long", 0, 1.0, ECUIO_UNITS_METRIC, &out),
              ECUIO_DISPLAY_OK);
    EXPECT_EQ(std::strlen(out.unit), sizeof out.unit - 1);
}

TEST(SignalsTest, DisplayConvertRefusesNullAndUnknownSystems)
{
    ecuio_display_value_t out;
    std::memset(&out, 0x5A, sizeof out);
    const ecuio_display_value_t before = out;
    EXPECT_EQ(ecuio_display_convert(nullptr, 0, 1.0, ECUIO_UNITS_IMPERIAL, &out),
              ECUIO_DISPLAY_ERR_NULL);
    EXPECT_EQ(ecuio_display_convert("km/h", 0, 1.0, ECUIO_UNITS_IMPERIAL, nullptr),
              ECUIO_DISPLAY_ERR_NULL);
    EXPECT_EQ(ecuio_display_convert("km/h", 0, 1.0, 2u, &out), ECUIO_DISPLAY_ERR_UNITS);
    EXPECT_EQ(std::memcmp(&out, &before, sizeof out), 0) << "out written on failure";
}

TEST(SignalsTest, MockAndObd2ShareDisplayNamesAndUnits)
{
    /* The UI shows names, units and decimal places as core gives them,
     * and the Dashboard's gauges find their signals by name (the Taps in
     * DashboardPage.qml). A quantity both sets carry has one name, unit
     * and precision in both, so a gauge reads under either profile.
     * Temperatures are a UTF-8 degree sign, never "degC". */
    const char* const kShared[] = {"RPM",
                                   "Vehicle Speed",
                                   "Coolant Temp",
                                   "Engine Oil Temp",
                                   "Intake Air Temp",
                                   "MAF",
                                   "Throttle Position",
                                   "Engine Load",
                                   "Intake Manifold Pressure",
                                   "Barometric Pressure",
                                   "Battery Voltage"};
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
        /* A computed def rides no message (boost): ID 0 */
        const uint32_t id = (set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) ? 0u : 0x0C0u;
        EXPECT_EQ(set->defs[i].message_id, id)
            << "signal '" << set->defs[i].name << "' not on telemetry ID";
    }
}

TEST(SignalsTest, ValidatorRejectsKindsWithNoDecoder)
{
    /* Unknown kinds have no decoder, so a def of one would register
     * and never decode. */
    for (int k : {6, 7, 255}) {
        ecuio_signal_def_t d = four_byte_def(k);
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_KIND) << "kind " << k;
    }
    /* The kind is checked first, as in the decoder: a bad kind with a
     * bad byte order and bad bit parameters reports the kind. */
    ecuio_signal_def_t d = four_byte_def(6, 2);
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
    /* Each reserved byte, every flag but ECUIO_DEF_FLAG_COMPUTED, and
     * the table fields of a def that is neither an enum nor computed
     * must be zero, so a later field can take the room and existing
     * sets still load. 0x02 is in no flag's place. */
    struct Room { size_t offset; size_t size; const char* name; };
#define ROOM(field) {offsetof(ecuio_signal_def_t, field), sizeof(ecuio_signal_def_t::field), #field}
    const Room rooms[] = {
        ROOM(reserved0), ROOM(mux_reserved),
        ROOM(display_reserved), ROOM(flags), ROOM(quantity_reserved),
        ROOM(table_first), ROOM(table_count), ROOM(reserved),
    };
#undef ROOM
    const ecuio_signal_def_t good = four_byte_def(ECUIO_VALUE_UINT);
    ASSERT_EQ(ecuio_validate_signal(&good), ECUIO_VALIDATE_OK);
    size_t checked = 0;
    for (const Room& room : rooms) {
        for (size_t i = 0; i < room.size; ++i) {
            ecuio_signal_def_t d = good;
            reinterpret_cast<unsigned char*>(&d)[room.offset + i] = 0x02;
            EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_RESERVED)
                << room.name << " byte " << i << " (offset " << room.offset + i << ")";
            ++checked;
        }
    }
    /* Every byte that isn't a named value: 2 + 2 + 1 + 4 + 4 + 2 + 2 + 16. */
    EXPECT_EQ(checked, 33u);

    /* plot_rank and group took reserved bytes' room: any value is a
     * rank, and any a group (one this core doesn't know shows as
     * Other) */
    ecuio_signal_def_t ranked = good;
    for (unsigned rank : {1u, 10u, 255u}) {
        ranked.plot_rank = static_cast<uint8_t>(rank);
        EXPECT_EQ(ecuio_validate_signal(&ranked), ECUIO_VALIDATE_OK) << rank;
        ranked.group = static_cast<uint8_t>(rank);
        EXPECT_EQ(ecuio_validate_signal(&ranked), ECUIO_VALIDATE_OK) << rank;
    }
}

/* ADR-0050: each code its name, in the list's order; "" past it */
TEST(SignalsTest, EachGroupHasAName)
{
    const struct {
        uint32_t code;
        const char* name;
    } groups[] = {
        {ECUIO_SIGNAL_GROUP_OTHER, "Other"},
        {ECUIO_SIGNAL_GROUP_ENGINE, "Engine"},
        {ECUIO_SIGNAL_GROUP_AIR, "Air"},
        {ECUIO_SIGNAL_GROUP_FUEL, "Fuel"},
        {ECUIO_SIGNAL_GROUP_OXYGEN, "Oxygen sensors"},
        {ECUIO_SIGNAL_GROUP_EMISSIONS, "Emissions"},
        {ECUIO_SIGNAL_GROUP_VEHICLE, "Vehicle"},
        {ECUIO_SIGNAL_GROUP_ELECTRICAL, "Electrical"},
        {ECUIO_SIGNAL_GROUP_DIAGNOSTICS, "Diagnostics"},
    };
    uint32_t next = 0;
    for (const auto& g : groups) {
        EXPECT_EQ(g.code, next++) << g.name << ": the codes run unbroken";
        EXPECT_STREQ(ecuio_signal_group_name(g.code), g.name);
    }
    EXPECT_STREQ(ecuio_signal_group_name(next), "");
    EXPECT_STREQ(ecuio_signal_group_name(255), "");
    EXPECT_STREQ(ecuio_signal_group_name(UINT32_MAX), "");
}

TEST(SignalsTest, ValidatorAcceptsExactlyWhatTheDecoderDecodes)
{
    /* The contract the validator exists for: over every kind, both byte
     * orders and an unknown one, and a spread of field shapes, a def
     * validates if and only if one of the decoders (numbers, or text)
     * decodes an in-bounds payload. */
    const uint8_t payload[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const int kinds[] = {0, 1, 2, 3, 4, 5, 6, 255};
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
                    char t[64];
                    const bool decodes =
                        ecuio_decode_signal(&d, payload, sizeof payload, &v) == ECUIO_DECODE_OK ||
                        ecuio_decode_text(&d, payload, sizeof payload, t, sizeof t) == ECUIO_DECODE_OK;
                    EXPECT_EQ(valid, decodes) << "kind " << k << " order " << order
                                              << " len " << int(len) << " off " << off;
                }
            }
        }
    }
}
