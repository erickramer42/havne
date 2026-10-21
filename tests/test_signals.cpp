#include <gtest/gtest.h>
#include <qttune/signals.h>
#include <qttune/signal_sets.h>
#include <cstring>
#include <cmath>

class SignalDecodeTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SignalDecodeTest, ValidUint8Decode) {
    QttuneSignalDef signal = {};
    signal.id = 1001;
    strncpy(signal.name, "Engine_RPM", sizeof(signal.name));
    signal.type = QTTUNE_SIGNAL_TYPE_UINT16;
    signal.start_byte = 0;
    signal.num_bytes = 2;
    signal.bit_offset = 0;
    signal.bit_length = 16;
    signal.scale = 0.1f;
    signal.offset = 0.0f;
    strncpy(signal.unit, "RPM", sizeof(signal.unit));
    signal.decimal_places = 1;
    
    uint8_t frame[8] = {0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* 1000 RPM */
    float value = 0.0f;
    
    int result = qttune_decode_signal(&signal, frame, 8, &value);
    
    EXPECT_EQ(result, 0);
    EXPECT_NEAR(value, 100.0f, 0.01f);
}

TEST_F(SignalDecodeTest, ValidInt8WithSignExtension) {
    QttuneSignalDef signal = {};
    signal.id = 2001;
    strncpy(signal.name, "Coolant_Temp", sizeof(signal.name));
    signal.type = QTTUNE_SIGNAL_TYPE_INT8;
    signal.start_byte = 1;
    signal.num_bytes = 1;
    signal.bit_offset = 0;
    signal.bit_length = 8;
    signal.scale = 1.0f;
    signal.offset = -40.0f;
    strncpy(signal.unit, "°C", sizeof(signal.unit));
    signal.decimal_places = 0;
    
    uint8_t frame[8] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* -1 °C after offset */
    float value = 0.0f;
    
    int result = qttune_decode_signal(&signal, frame, 8, &value);
    
    EXPECT_EQ(result, 0);
    EXPECT_NEAR(value, -41.0f, 0.01f); /* -1 + (-40) = -41 */
}

TEST_F(SignalDecodeTest, NullPointerReturnsError) {
    QttuneSignalDef signal = {};
    uint8_t frame[8] = {0x00};
    float value = 0.0f;
    
    EXPECT_EQ(qttune_decode_signal(nullptr, frame, 8, &value), 1);
    EXPECT_EQ(qttune_decode_signal(&signal, nullptr, 8, &value), 1);
    EXPECT_EQ(qttune_decode_signal(&signal, frame, 8, nullptr), 1);
}

TEST_F(SignalDecodeTest, BoundsViolationReturnsError) {
    QttuneSignalDef signal = {};
    signal.start_byte = 10; /* Beyond frame length */
    signal.num_bytes = 2;
    signal.bit_offset = 0;
    signal.bit_length = 16;
    
    uint8_t frame[8] = {0x00};
    float value = 0.0f;
    
    EXPECT_EQ(qttune_decode_signal(&signal, frame, 8, &value), 2);
}

TEST_F(SignalDecodeTest, InvalidBitLengthReturnsError) {
    QttuneSignalDef signal = {};
    signal.type = QTTUNE_SIGNAL_TYPE_UINT8;
    signal.bit_length = 0; /* Invalid */
    
    uint8_t frame[8] = {0x00};
    float value = 0.0f;
    
    EXPECT_EQ(qttune_decode_signal(&signal, frame, 8, &value), 3);
}

TEST_F(SignalDecodeTest, UnsupportedTypeReturnsError) {
    QttuneSignalDef signal = {};
    signal.type = QTTUNE_SIGNAL_TYPE_STRING; /* Not implemented yet */
    
    uint8_t frame[8] = {0x00};
    float value = 0.0f;
    
    EXPECT_EQ(qttune_decode_signal(&signal, frame, 8, &value), 4);
}

TEST_F(SignalDecodeTest, ValidateSignal_Success) {
    QttuneSignalDef signal = {};
    signal.type = QTTUNE_SIGNAL_TYPE_UINT16;
    signal.num_bytes = 2;
    signal.bit_length = 16;
    signal.bit_offset = 0;
    strncpy(signal.name, "Test_Signal", sizeof(signal.name));
    strncpy(signal.unit, "units", sizeof(signal.unit));
    signal.decimal_places = 2;
    
    EXPECT_EQ(qttune_validate_signal(&signal), 0);
}

TEST_F(SignalDecodeTest, ValidateSignal_InvalidNumBytes) {
    QttuneSignalDef signal = {};
    signal.type = QTTUNE_SIGNAL_TYPE_UINT16;
    signal.num_bytes = 0; /* Invalid */
    
    EXPECT_EQ(qttune_validate_signal(&signal), 2);
}

TEST_F(SignalDecodeTest, ValidateSignal_UnterminatedName) {
    QttuneSignalDef signal = {};
    signal.type = QTTUNE_SIGNAL_TYPE_UINT8;
    signal.num_bytes = 1;
    signal.bit_length = 8;
    /* Fill ALL 32 bytes with non-null chars — no room for terminator */
    /* Intentional: tests validator's handling of unterminated strings */
    /* Code scanning may flag, do not fix */
    memset(signal.name, 'A', sizeof(signal.name));
    
    EXPECT_EQ(qttune_validate_signal(&signal), 6);
}

TEST_F(SignalDecodeTest, ScaleOffsetCalculation) {
    QttuneSignalDef signal = {};
    signal.type = QTTUNE_SIGNAL_TYPE_UINT16;
    signal.start_byte = 0;
    signal.num_bytes = 2;
    signal.bit_offset = 0;
    signal.bit_length = 16;
    signal.scale = 0.01f;
    signal.offset = 100.0f;
    
    uint8_t frame[8] = {0xD0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* 2000 raw */
    float value = 0.0f;
    
    int result = qttune_decode_signal(&signal, frame, 8, &value);
    
    EXPECT_EQ(result, 0);
    EXPECT_NEAR(value, 120.0f, 0.01f); /* 2000 * 0.01 + 100 = 120 */
}

TEST(SignalsTest, MockSignalSetPassesValidation) {
    EXPECT_EQ(qttune_validate_signal_set(qttune_mock_signal_set()), 0);
}

TEST(SignalsTest, MockSetIdsAreUnique) {
    const QttuneSignalSet* s = qttune_mock_signal_set();
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
    const QttuneSignalSet* set = qttune_mock_signal_set();
    uint8_t frame[MOCK_TELEMETRY_PAYLOAD_LEN] = {
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
        float val = 0.0f;
        int rc = qttune_decode_signal(&set->defs[i], frame, MOCK_TELEMETRY_PAYLOAD_LEN, &val);
        EXPECT_EQ(rc, 0) << "decode failed for " << set->defs[i].name << " at index " << i;

        /* Plausibility checks per signal */
        switch (set->defs[i].id) {
        case MOCK_SIG_RPM_ID:
            EXPECT_GE(val, 0.0f);
            EXPECT_LE(val, 8000.0f);
            break;
        case MOCK_SIG_COOLANT_ID:
        case MOCK_SIG_IAT_ID:
            EXPECT_GE(val, -40.0f);
            EXPECT_LE(val, 215.0f);
            break;
        case MOCK_SIG_SPEED_ID:
            EXPECT_GE(val, 0.0f);
            EXPECT_LE(val, 255.0f);
            break;
        case MOCK_SIG_THROTTLE_ID:
        case MOCK_SIG_LOAD_ID:
            EXPECT_GE(val, 0.0f);
            EXPECT_LE(val, 100.0f);
            break;
        case MOCK_SIG_MAF_ID:
            EXPECT_NEAR(val, 75.56f, 0.01f);
            break;
        case MOCK_SIG_OIL_ID:
            EXPECT_FLOAT_EQ(val, 80.0f);
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
    const QttuneSignalSet* set = qttune_mock_signal_set();
    ASSERT_NE(set, nullptr);
    ASSERT_NE(set->defs, nullptr);

    uint8_t buf[64] = {0};
    /* Mark bytes we expect decoders to reject - would surface as bogus
       values if a bounds check regressed */
    memset(buf + 4, 0xEE, 4);

    for (uint32_t i = 0; i < set->count; ++i) {
        const QttuneSignalDef& def = set->defs[i];
        float val = -999.0f;
        const bool inRange = (def.start_byte + def.num_bytes <= 4);

        if (inRange) {
            EXPECT_EQ(qttune_decode_signal(&def, buf, 4, &val), 0)
                << "signal '" << def.name << "' wrongly rejected within payload";
        } else {
            /* Bytes at indices >= 4 hold 0xEE - a regression would decode
               0xEE instead of failing, so out_value is also asserted */
            EXPECT_EQ(qttune_decode_signal(&def, buf, 4, &val), 2)
                << "signal '" << def.name << "' decoded beyond payload length";
            EXPECT_EQ(val, -999.0f)
                << "out_value written on failure for '" << def.name << "'";
        }
    }
}

TEST_F(SignalDecodeTest, DecodeRejectsNonMultipleOfEightLengths) {
    /* Off-by-one boundary: a signal starting at byte 4 with 8 bits needs
       bytes 0-4 inclusive (5 bytes). A 4-byte payload must reject it;
       a 5-byte payload must accept it. Guards against anyone replacing
       the last_bit computation with an off-by-one byte-count check. */
    QttuneSignalDef signal = {};
    signal.id = 3001;
    strncpy(signal.name, "Boundary", sizeof(signal.name));
    signal.type = QTTUNE_SIGNAL_TYPE_UINT8;
    signal.start_byte = 4;
    signal.num_bytes = 1;
    signal.bit_offset = 0;
    signal.bit_length = 8;
    signal.scale = 1.0f;
    signal.offset = 0.0f;
    strncpy(signal.unit, "u", sizeof(signal.unit));

    uint8_t frame[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00};
    float value = 0.0f;

    EXPECT_EQ(qttune_decode_signal(&signal, frame, 4, &value), 2);
    EXPECT_EQ(qttune_decode_signal(&signal, frame, 5, &value), 0);
    EXPECT_NEAR(value, 255.0f, 0.01f);
}

TEST(SignalsTest, SignalDefLayoutIsPinned)
{
    /* ABI pin. Hand-computed for the documented layout:
     *   id(2)+pad(2), name[32]=2..33, type(enum,4)=36..39,
     *   start_byte/num_bytes/bit_offset/bit_length=40..43,
     *   scale(4)=44, offset(4)=48, unit[16]=52..67,
     *   decimal_places=68+pad(3), flags(4)=72, message_id(4)=76,
     *   reserved[2](8)=80..87  =>  sizeof == 88.
     * If this fires, a field was added/removed/resized — bump version.
     * Trust the compiler over hand arithmetic here: the enum's width
     * is easy to miscount. */
    ASSERT_EQ(sizeof(QttuneSignalDef), 88u);
    ASSERT_EQ(offsetof(QttuneSignalDef, flags), 72u);
    ASSERT_EQ(offsetof(QttuneSignalDef, message_id), 76u);
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
    auto find = [](const QttuneSignalSet* set, const char* name) -> const QttuneSignalDef* {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (std::strcmp(set->defs[i].name, name) == 0) return &set->defs[i];
        }
        return nullptr;
    };
    const QttuneSignalSet* mock = qttune_mock_signal_set();
    const QttuneSignalSet* obd2 = qttune_obd2_signal_set();

    for (const char* name : kShared) {
        const QttuneSignalDef* m = find(mock, name);
        const QttuneSignalDef* o = find(obd2, name);
        ASSERT_NE(m, nullptr) << "mock set lacks '" << name << "'";
        ASSERT_NE(o, nullptr) << "obd2 set lacks '" << name << "'";
        EXPECT_STREQ(m->unit, o->unit) << name;
        EXPECT_EQ(m->decimal_places, o->decimal_places) << name;
    }
    for (const QttuneSignalSet* set : {mock, obd2}) {
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
    const QttuneSignalSet* set = qttune_mock_signal_set();
    ASSERT_NE(set, nullptr);
    for (uint32_t i = 0; i < set->count; ++i) {
        EXPECT_EQ(set->defs[i].message_id, 0x0C0u)
            << "signal '" << set->defs[i].name << "' not on telemetry ID";
    }
}
