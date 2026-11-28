// File/layer: tests/test_signals_text.cpp | GTest, core signal tests, C++17, Qt-free
// Testing: UNIT: SignalsTest (test_signals)

#include "test_signals_support.h"

namespace {

/* A text def: a byte field from start_byte, byte_length bytes (0 = to
 * the end of the payload), no bit parameters. */
ecuio_signal_def_t text_def(int kind, uint16_t start_byte, uint16_t byte_length)
{
    ecuio_signal_def_t d = {};
    d.id = 1;
    d.kind = static_cast<uint8_t>(kind);
    d.start_byte = start_byte;
    d.byte_length = byte_length;
    d.scale = 1.0;
    return d;
}

/* A Service 0x22 reply for DID 0xF190, the VIN's: the echo, then
 * bytes with a NUL and a DEL among the letters. */
const uint8_t kVinReply[9] = {0x62, 0xF1, 0x90, 'W', 'V', 'W', 0x00, 0x7F, 'A'};

} // namespace

TEST(SignalsTest, TextKindsDecodeToHexAndAscii)
{
    char out[64];
    ecuio_signal_def_t d = text_def(ECUIO_VALUE_HEX, 0, 0);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, sizeof out),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "62 F1 90 57 56 57 00 7F 41");

    d = text_def(ECUIO_VALUE_HEX, 3, 2);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, sizeof out),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "57 56");

    /* A NUL and 0x7F read as '.', so a value is always one line. */
    d = text_def(ECUIO_VALUE_TEXT, 3, 0);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, sizeof out),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "WVW..A");

    d = text_def(ECUIO_VALUE_TEXT, 3, 3);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, sizeof out),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "WVW");

    /* Byte order means nothing to bytes, and either known one passes. */
    d = text_def(ECUIO_VALUE_HEX, 0, 1);
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, sizeof out),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "62");
}

TEST(SignalsTest, TextDecodeChecksTheFieldAndTheMuxKey)
{
    char out[64];
    double v = 0.0;
    const uint16_t len = sizeof kVinReply;

    /* The field must start inside the payload and end within it. */
    ecuio_signal_def_t d = text_def(ECUIO_VALUE_TEXT, 9, 0);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_PAST_PAYLOAD);
    d = text_def(ECUIO_VALUE_TEXT, 3, 7);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_PAST_PAYLOAD);
    d = text_def(ECUIO_VALUE_TEXT, 3, 6);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "WVW..A");

    /* The mux key gates text as it gates numbers. */
    d = text_def(ECUIO_VALUE_HEX, 3, 0);
    d.mux_length = 3;
    d.mux_key = 0x62F190u;
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "57 56 57 00 7F 41");
    d.mux_key = 0x62F191u;
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_MUX_MISMATCH);
    d.mux_length = 4;
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_BAD_PARAMS);
    d.mux_length = 3;
    d.mux_offset = static_cast<decltype(d.mux_offset)>(len - 1);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out),
              ECUIO_DECODE_ERR_PAST_PAYLOAD)
        << "a key running past the payload";

    /* Each decoder refuses the other's kinds, and the shapes it can't
     * take, in the validator's order: kind and byte order first. */
    d = four_byte_def(ECUIO_VALUE_UINT);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_UNSUPPORTED);
    d = text_def(ECUIO_VALUE_TEXT, 0, 0);
    EXPECT_EQ(ecuio_decode_signal(&d, kVinReply, len, &v), ECUIO_DECODE_ERR_UNSUPPORTED);
    d.bit_length = 8;
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_BAD_PARAMS);
    d.bit_length = 0;
    d.bit_offset = 1;
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_BAD_PARAMS);
    d.bit_offset = 0;
    d.byte_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX + 1u);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_BAD_PARAMS);
    d.byte_length = 0;
    d.byte_order = 2;
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_UNSUPPORTED);

    d = text_def(ECUIO_VALUE_TEXT, 0, 0);
    EXPECT_EQ(ecuio_decode_text(nullptr, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_NULL);
    EXPECT_EQ(ecuio_decode_text(&d, nullptr, len, out, sizeof out), ECUIO_DECODE_ERR_NULL);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, nullptr, sizeof out), ECUIO_DECODE_ERR_NULL);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, 0), ECUIO_DECODE_ERR_NULL);

    /* Nothing is written on failure. */
    strcpy(out, "kept");
    d = text_def(ECUIO_VALUE_TEXT, 9, 0);
    EXPECT_EQ(ecuio_decode_text(&d, kVinReply, len, out, sizeof out), ECUIO_DECODE_ERR_PAST_PAYLOAD);
    EXPECT_STREQ(out, "kept");
}

TEST(SignalsTest, TextDecodeCutsToItsBuffer)
{
    char out[8];
    memset(out, 'x', sizeof out);   /* guard bytes past each cap */
    ecuio_signal_def_t d = text_def(ECUIO_VALUE_HEX, 0, 3);   /* "62 F1 90" needs 9 */
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, 6), ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "62 F1");   /* whole pairs, no trailing space */
    EXPECT_EQ(out[6], 'x');
    /* A pair, its separator and the NUL must all fit: five bytes hold
     * one pair, not a pair and a space. */
    memset(out, 'x', sizeof out);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, 5), ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "62");
    EXPECT_EQ(out[5], 'x');
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, 1), ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "");
    d = text_def(ECUIO_VALUE_TEXT, 3, 0);
    ASSERT_EQ(ecuio_decode_text(&d, kVinReply, sizeof kVinReply, out, 3), ECUIO_DECODE_OK);
    EXPECT_STREQ(out, "WV");

    /* ECUIO_DECODE_TEXT_MAX holds the hex of a whole frame. */
    static const uint8_t full[ECUIO_FRAME_DATA_MAX] = {};
    static char big[ECUIO_DECODE_TEXT_MAX];
    d = text_def(ECUIO_VALUE_HEX, 0, 0);
    ASSERT_EQ(ecuio_decode_text(&d, full, sizeof full, big, sizeof big), ECUIO_DECODE_OK);
    EXPECT_EQ(strlen(big), 3u * ECUIO_FRAME_DATA_MAX - 1u);
}

TEST(SignalsTest, ValidatorChecksTextKinds)
{
    ecuio_signal_def_t d = text_def(ECUIO_VALUE_TEXT, 0, 0);
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
    d = text_def(ECUIO_VALUE_HEX, 3, static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX));
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
    d.byte_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX + 1u);
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BYTE_LENGTH);

    /* A text kind is a byte field: no bit parameters. */
    d = text_def(ECUIO_VALUE_TEXT, 0, 0);
    d.bit_length = 8;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BIT_LENGTH);
    d = text_def(ECUIO_VALUE_TEXT, 0, 0);
    d.bit_offset = 1;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BIT_OFFSET);

    d = text_def(ECUIO_VALUE_HEX, 0, 0);
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
    d.byte_order = 2;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BYTE_ORDER);

    /* byte_length is the text kinds' alone. */
    d = four_byte_def(ECUIO_VALUE_UINT);
    d.byte_length = 1;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BYTE_LENGTH);

    /* The kind is checked before the shape. */
    d = text_def(6, 0, 0);
    d.bit_length = 8;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_KIND);
}

TEST(SignalsTest, SetValidationChecksLayoutVersionFirst)
{
    /* A set built for another layout has its members elsewhere, so the
     * version is read before anything else, even NULL defs or a bad
     * def. */
    ecuio_signal_def_t bad_def = four_byte_def(ECUIO_VALUE_UINT);
    bad_def.bit_length = 0;
    for (uint32_t version : {0u, 1u, 2u, 3u, 4u, 6u}) {
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
    std::snprintf(defs[1].name, sizeof defs[1].name, "%s", "Second");
    EXPECT_EQ(ecuio_validate_signal_set(&set, nullptr), ECUIO_VALIDATE_OK);
}

/* Two defs may share a name (ADR-0065 section 9: a recording keeps a
 * value under its key), byte for byte and the empty one too; an id still
 * names one def */
TEST(SignalsTest, SetValidationTakesARepeatedName)
{
    ecuio_signal_def_t defs[3] = {four_byte_def(ECUIO_VALUE_UINT),
                                  four_byte_def(ECUIO_VALUE_UINT),
                                  four_byte_def(ECUIO_VALUE_UINT)};
    defs[1].id = 2;
    defs[2].id = 3;
    ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    ecuio_validation_t v{};
    EXPECT_EQ(ecuio_validate_signal_set(&set, &v), ECUIO_VALIDATE_OK)
        << "three empty names";
    EXPECT_EQ(v.index, UINT32_MAX);
    std::snprintf(defs[0].name, sizeof defs[0].name, "%s", "Boost");
    std::snprintf(defs[1].name, sizeof defs[1].name, "%s", "Lambda");
    std::snprintf(defs[2].name, sizeof defs[2].name, "%s", "Boost");
    EXPECT_EQ(ecuio_validate_signal_set(&set, &v), ECUIO_VALIDATE_OK);
    defs[2].id = 1;
    EXPECT_EQ(ecuio_validate_signal_set(&set, &v),
              ECUIO_VALIDATE_ERR_DUPLICATE_ID);
    EXPECT_EQ(v.index, 2u);
}

TEST(SignalsTest, SetValidationAcceptsAnEmptySet)
{
    /* No defs: decodes nothing, may still declare channels (a filtered
     * log-only profile). count > 0 with no defs stays an error. */
    static const ecuio_channel_decl_t kChan[] = {{0x7E8u, 0x7E0u, 0, 0, {}}};
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
    for (int i = 0; i < 3; ++i) {
        std::snprintf(defs[i].name, sizeof defs[i].name, "Signal %d", i);
    }
    ecuio_channel_decl_t channels[3] = {{0x7E8u, 0x7E0u, 0, 0, {}},
                                        {0x7E9u, 0x7E1u, 0, 0, {}},
                                        {0x7EAu, 0x7E2u, 0, 0, {}}};
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
    std::memcpy(defs[2].name, defs[1].name, sizeof defs[2].name);
    expect("repeated name", &set, ECUIO_VALIDATE_OK, UINT32_MAX);
    std::snprintf(defs[2].name, sizeof defs[2].name, "%s", "Signal 2");
    channels[1].tx_id = 0;
    expect("bad channel id", &set, ECUIO_VALIDATE_ERR_CHANNEL_ID, 1u);
    channels[1].tx_id = 0x7E1u;
    channels[2].rx_id = channels[1].rx_id;
    expect("duplicate rx", &set, ECUIO_VALIDATE_ERR_DUPLICATE_RX, 2u);
    channels[2].rx_id = 0x7EAu;
    channels[1].flags = 1;
    expect("channel flags", &set, ECUIO_VALIDATE_ERR_CHANNEL_RESERVED, 1u);
    channels[1].flags = 0;
    for (uint8_t& r : channels[2].reserved) {
        r = 0x80;
        expect("channel reserved", &set, ECUIO_VALIDATE_ERR_CHANNEL_RESERVED,
               2u);
        r = 0;
    }
    expect("valid again", &set, ECUIO_VALIDATE_OK, UINT32_MAX);
    for (unsigned v = 0; v < 256; ++v) {
        channels[1].session = static_cast<uint8_t>(v);
        const bool taken = v == ECUIO_CHANNEL_SESSION_NONE ||
                           v == ECUIO_CHANNEL_SESSION_DEFAULT ||
                           v == ECUIO_CHANNEL_SESSION_EXTENDED;
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_validate_signal_set(&set, &why),
                  taken ? ECUIO_VALIDATE_OK
                        : ECUIO_VALIDATE_ERR_CHANNEL_SESSION)
            << "session " << v;
        EXPECT_EQ(why.index, taken ? UINT32_MAX : 1u) << "session " << v;
    }
    channels[1].session = 0;

    expect("NULL set", nullptr, ECUIO_VALIDATE_ERR_NULL, UINT32_MAX);
    ecuio_signal_set_t whole = set;
    whole.layout_version = 2;
    expect("layout", &whole, ECUIO_VALIDATE_ERR_LAYOUT, UINT32_MAX);
    whole.layout_version = ECUIO_SIGNAL_SET_LAYOUT - 1; /* 8-byte channels */
    expect("the layout before", &whole, ECUIO_VALIDATE_ERR_LAYOUT, UINT32_MAX);
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

namespace {

/* A field of `bits` at byte 1 (bit `bit_offset` for LE), so a range
 * test sees the field's start move nothing */
ecuio_signal_def_t field_def(int kind, uint8_t bits, double scale = 1.0,
                             double offset = 0.0,
                             int order = ECUIO_BYTE_ORDER_LE,
                             uint8_t bit_offset = 0)
{
    ecuio_signal_def_t d = {};
    d.id = 1;
    d.kind = static_cast<uint8_t>(kind);
    d.byte_order = static_cast<uint8_t>(order);
    d.start_byte = 1;
    d.bit_offset = bit_offset;
    d.bit_length = bits;
    d.scale = scale;
    d.offset = offset;
    return d;
}

struct Span {
    int rc;
    double lo;
    double hi;
};

Span range_of(const ecuio_signal_def_t& d)
{
    Span s{-1, -7.0, -7.0};
    s.rc = ecuio_def_range(&d, &s.lo, &s.hi);
    return s;
}

/* Puts raw into the def's field in payload, as the decoder reads it
 * back, and the def's mux key at its place */
void put_raw(const ecuio_signal_def_t& d, uint32_t raw, uint8_t* payload)
{
    if (d.byte_order == ECUIO_BYTE_ORDER_BE) {
        const int n = d.bit_length / 8;
        for (int i = 0; i < n; ++i) {
            payload[d.start_byte + i] =
                static_cast<uint8_t>(raw >> (8 * (n - 1 - i)));
        }
    } else {
        for (int i = 0; i < d.bit_length; ++i) {
            const uint32_t at = d.start_byte * 8u + d.bit_offset + i;
            const uint8_t bit = static_cast<uint8_t>(1u << (at & 7u));
            if ((raw >> i) & 1u) {
                payload[at >> 3] |= bit;
            } else {
                payload[at >> 3] &= static_cast<uint8_t>(~bit);
            }
        }
    }
    for (int i = 0; i < d.mux_length; ++i) {
        payload[d.mux_offset + i] =
            static_cast<uint8_t>(d.mux_key >> (8 * (d.mux_length - 1 - i)));
    }
}

} // namespace

// A field's span is every raw value its bits hold, scaled: the lowest
// and highest a decode can give, whatever the scale's sign
TEST(SignalsTest, ADefsRangeIsItsFieldsSpanScaled)
{
    const auto expect = [](const ecuio_signal_def_t& d, double lo, double hi,
                           const char* what) {
        const Span s = range_of(d);
        EXPECT_EQ(s.rc, ECUIO_DECODE_OK) << what;
        EXPECT_EQ(s.lo, lo) << what;
        EXPECT_EQ(s.hi, hi) << what;
    };
    expect(field_def(ECUIO_VALUE_UINT, 8), 0.0, 255.0, "a byte");
    expect(field_def(ECUIO_VALUE_UINT, 1, 1.0, 0.0, ECUIO_BYTE_ORDER_LE, 7),
           0.0, 1.0, "one bit at bit 7");
    expect(field_def(ECUIO_VALUE_UINT, 16, 0.25, 0.0, ECUIO_BYTE_ORDER_BE), 0.0,
           16383.75, "the engine speed field");
    expect(field_def(ECUIO_VALUE_UINT, 32), 0.0, 4294967295.0, "32 bits");
    expect(field_def(ECUIO_VALUE_UINT, 8, 1.0, -40.0), -40.0, 215.0,
           "an offset");
    expect(field_def(ECUIO_VALUE_INT, 8), -128.0, 127.0, "a signed byte");
    expect(field_def(ECUIO_VALUE_INT, 12, 0.5, 10.0), -1014.0, 1033.5,
           "signed, scaled, offset");
    expect(field_def(ECUIO_VALUE_INT, 32), -2147483648.0, 2147483647.0,
           "signed 32 bits");
    expect(field_def(ECUIO_VALUE_UINT, 8, -0.5, 100.0), -27.5, 100.0,
           "a negative scale swaps the ends");
    expect(field_def(ECUIO_VALUE_ENUM, 3), 0.0, 7.0,
           "an enum as validated, its raw range");
    expect(field_def(ECUIO_VALUE_ENUM, 3, 2.0, 5.0), 5.0, 19.0,
           "an enum scaled as the decoder scales it");
    expect(field_def(ECUIO_VALUE_UINT, 32, 4e298), 0.0, 4294967295.0 * 4e298,
           "a large span, still finite");
}

// What has no span, and a field the decoder refuses, in the decoder's
// order: the kind and byte order, the bits, then a FLOAT; then an end
// that isn't finite or a span of zero width. *lo and *hi untouched
TEST(SignalsTest, ADefsRangeRefusesWhatHasNoSpan)
{
    ecuio_signal_def_t d = field_def(ECUIO_VALUE_UINT, 8);
    double lo = -7.0;
    double hi = -7.0;
    EXPECT_EQ(ecuio_def_range(nullptr, &lo, &hi), ECUIO_DECODE_ERR_NULL);
    EXPECT_EQ(ecuio_def_range(&d, nullptr, &hi), ECUIO_DECODE_ERR_NULL);
    EXPECT_EQ(ecuio_def_range(&d, &lo, nullptr), ECUIO_DECODE_ERR_NULL);
    EXPECT_EQ(lo, -7.0);
    EXPECT_EQ(hi, -7.0);

    const auto refused = [](const ecuio_signal_def_t& def, int rc,
                            const char* what) {
        const Span s = range_of(def);
        EXPECT_EQ(s.rc, rc) << what;
        EXPECT_EQ(s.lo, -7.0) << what;
        EXPECT_EQ(s.hi, -7.0) << what;
    };
    const int unsupported = ECUIO_DECODE_ERR_UNSUPPORTED;
    const int bad = ECUIO_DECODE_ERR_BAD_PARAMS;
    refused(four_byte_def(ECUIO_VALUE_FLOAT), unsupported, "a FLOAT");
    refused(field_def(ECUIO_VALUE_TEXT, 0), unsupported, "TEXT");
    refused(field_def(ECUIO_VALUE_HEX, 0), unsupported, "HEX");
    refused(field_def(9, 8), unsupported, "a kind with no decoder");
    refused(field_def(ECUIO_VALUE_UINT, 8, 1.0, 0.0, 2), unsupported,
            "a byte order with none");
    ecuio_signal_def_t computed = field_def(ECUIO_VALUE_UINT, 0);
    computed.flags = ECUIO_DEF_FLAG_COMPUTED;
    refused(computed, unsupported, "a COMPUTED def, before its zero bits");
    computed.bit_length = 8;
    refused(computed, unsupported, "a COMPUTED def with bits");

    refused(field_def(ECUIO_VALUE_UINT, 0), bad, "no bits");
    refused(field_def(ECUIO_VALUE_UINT, 33), bad, "33 bits");
    refused(field_def(ECUIO_VALUE_INT, 8, 1.0, 0.0, ECUIO_BYTE_ORDER_LE, 8),
            bad, "bit 8");
    refused(field_def(ECUIO_VALUE_UINT, 12, 1.0, 0.0, ECUIO_BYTE_ORDER_BE), bad,
            "big-endian, not whole bytes");
    refused(field_def(ECUIO_VALUE_ENUM, 0), bad, "an enum with no bits");

    /* The decoder's order: a byte order with no decoder before the bits,
     * the bits before the FLOAT check */
    refused(field_def(ECUIO_VALUE_UINT, 0, 1.0, 0.0, 2), unsupported,
            "an unknown order, no bits");
    refused(field_def(ECUIO_VALUE_FLOAT, 0), bad, "FLOAT, no bits");

    refused(field_def(ECUIO_VALUE_UINT, 8, INFINITY), bad, "scale inf");
    refused(field_def(ECUIO_VALUE_UINT, 8, NAN), bad, "scale NaN");
    refused(field_def(ECUIO_VALUE_UINT, 8, 1.0, NAN), bad, "offset NaN");
    refused(field_def(ECUIO_VALUE_UINT, 32, 1e300), bad,
            "an end past the double range");
    refused(field_def(ECUIO_VALUE_UINT, 8, 0.0), bad, "scale 0, zero width");
}

// Every def of the built-in sets with a range: its ends are what the
// decoder gives for the field's lowest and highest raw values, so the
// range is the decoder's, not a second copy of its rules
TEST(SignalsTest, EachBuiltInDefsRangeIsTheDecodersExtremes)
{
    for (const ecuio_signal_set_t* set :
         {ecuio_mock_signal_set(), ecuio_obd2_signal_set()}) {
        ASSERT_NE(set, nullptr);
        uint32_t ranged = 0;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const Span s = range_of(d);
            const bool spanless = (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ||
                                  d.kind == ECUIO_VALUE_FLOAT ||
                                  d.kind == ECUIO_VALUE_TEXT ||
                                  d.kind == ECUIO_VALUE_HEX;
            if (spanless) {
                EXPECT_EQ(s.rc, ECUIO_DECODE_ERR_UNSUPPORTED) << d.name;
                continue;
            }
            ASSERT_EQ(s.rc, ECUIO_DECODE_OK) << d.name;
            ++ranged;
            const bool is_int = d.kind == ECUIO_VALUE_INT;
            const uint32_t all =
                d.bit_length == 32 ? 0xFFFFFFFFu : (1u << d.bit_length) - 1u;
            const uint32_t raw_min = is_int ? 1u << (d.bit_length - 1) : 0u;
            const uint32_t raw_max = is_int ? all >> 1 : all;
            uint8_t payload[ECUIO_FRAME_DATA_MAX] = {};
            const uint16_t len = ECUIO_FRAME_DATA_MAX;
            double a = 0.0;
            double b = 0.0;
            put_raw(d, raw_min, payload);
            ASSERT_EQ(ecuio_decode_signal(&d, payload, len, &a),
                      ECUIO_DECODE_OK)
                << d.name;
            put_raw(d, raw_max, payload);
            ASSERT_EQ(ecuio_decode_signal(&d, payload, len, &b),
                      ECUIO_DECODE_OK)
                << d.name;
            EXPECT_EQ(s.lo, std::fmin(a, b)) << d.name;
            EXPECT_EQ(s.hi, std::fmax(a, b)) << d.name;
        }
        EXPECT_GT(ranged, 0u);
    }
}

// The range against the decoder over field shapes: each kind, byte
// order, width, scale's sign and bit offset, and a muxed def. Its ends
// are the decoder's values at the lowest and highest raw, and a shape
// the decoder refuses the range refuses with the same code
TEST(SignalsTest, ADefsRangeIsTheDecodersExtremesForEveryFieldShape)
{
    const auto against_the_decoder = [](const ecuio_signal_def_t& d) {
        const Span s = range_of(d);
        const bool is_int = d.kind == ECUIO_VALUE_INT;
        const uint32_t all =
            d.bit_length == 32 ? 0xFFFFFFFFu : (1u << d.bit_length) - 1u;
        const uint32_t raw_min = is_int ? 1u << (d.bit_length - 1) : 0u;
        const uint32_t raw_max = is_int ? all >> 1 : all;
        uint8_t payload[ECUIO_FRAME_DATA_MAX] = {};
        const uint16_t len = ECUIO_FRAME_DATA_MAX;
        double a = 0.0;
        double b = 0.0;
        put_raw(d, raw_min, payload);
        const int rc = ecuio_decode_signal(&d, payload, len, &a);
        if (rc != ECUIO_DECODE_OK) {
            EXPECT_EQ(s.rc, rc);
            EXPECT_EQ(s.lo, -7.0);
            EXPECT_EQ(s.hi, -7.0);
            return;
        }
        put_raw(d, raw_max, payload);
        ASSERT_EQ(ecuio_decode_signal(&d, payload, len, &b), ECUIO_DECODE_OK);
        EXPECT_EQ(s.rc, ECUIO_DECODE_OK);
        EXPECT_EQ(s.lo, std::fmin(a, b));
        EXPECT_EQ(s.hi, std::fmax(a, b));
    };

    /* Binary-exact scales and offsets, so a fused multiply-add gives
     * the same double as the decoder's */
    const double scaling[][2] = {{0.25, -40.0}, {-0.5, 12.5}};
    int refused = 0;
    for (const int kind : {ECUIO_VALUE_UINT, ECUIO_VALUE_INT}) {
        for (const int order : {ECUIO_BYTE_ORDER_LE, ECUIO_BYTE_ORDER_BE}) {
            for (const int bits : {1, 8, 13, 32}) {
                for (const auto& so : scaling) {
                    for (const int at : {0, 3}) {
                        const ecuio_signal_def_t d =
                            field_def(kind, static_cast<uint8_t>(bits), so[0],
                                      so[1], order, static_cast<uint8_t>(at));
                        SCOPED_TRACE(testing::Message()
                                     << "kind " << kind << ", order " << order
                                     << ", " << bits << " bits at bit " << at
                                     << ", scale " << so[0]);
                        against_the_decoder(d);
                        refused += range_of(d).rc != ECUIO_DECODE_OK;
                    }
                }
            }
        }
    }
    /* Big-endian at bit 3, and big-endian 1 and 13 bits */
    EXPECT_GT(refused, 0);

    ecuio_signal_def_t muxed =
        field_def(ECUIO_VALUE_UINT, 16, 0.25, 0.0, ECUIO_BYTE_ORDER_BE);
    muxed.start_byte = 2;
    muxed.mux_offset = 0;
    muxed.mux_length = 2;
    muxed.mux_key = 0x410C;
    SCOPED_TRACE("a muxed def");
    against_the_decoder(muxed);
    EXPECT_EQ(range_of(muxed).rc, ECUIO_DECODE_OK);
}
