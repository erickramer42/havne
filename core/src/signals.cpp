// File/layer: core/src/signals.cpp | C++17, Qt-free
// Testing: UNIT — BE vs LE round-trip, mux acceptance/rejection, validator
//          rules, display-unit conversion (SignalsTest.DisplayConvert*)

#include <ecuio/signals.h>
#include <ecuio/internal.h>
#include <ecuio/signal_sets.h>
#include <string.h>
#include <math.h>

/* Internal helper: extract bitfield from byte array (little-endian)
 * Intel (little-endian) bit numbering: the field's LSB sits at bit
 * `bit_offset` inside byte `start_byte`, running upward through
 * consecutive bytes, so a 32-bit field at bit 7 spans five. */
static uint32_t extract_bitfield_le(const uint8_t* data,
                                     uint16_t start_byte,
                                     uint8_t bit_offset,
                                     uint8_t bit_length)
{
    uint32_t result = 0;
    for (uint8_t i = 0; i < bit_length; ++i) {
        uint32_t abs_bit = (uint32_t)start_byte * 8u + bit_offset + i;
        if (data[abs_bit >> 3] & (1u << (abs_bit & 7u))) {
            result |= (1u << i);
        }
    }
    return result;
}

/* Big-endian extraction: whole bytes only (bit_offset 0, bit_length a
 * multiple of 8; the caller checks both). Reads MSB-first: the first
 * byte is most significant. The last_bit bounds check already covers
 * start_byte + n. */
static uint32_t extract_bytes_be(const uint8_t* data,
                                  uint16_t start_byte, uint8_t bit_length)
{
    uint32_t result = 0;
    const uint8_t n = bit_length / 8;
    for (uint8_t i = 0; i < n; ++i) {
        result = (result << 8) | data[(uint32_t)start_byte + i];
    }
    return result;
}

/* The longest mux key: a service echo and a 2-byte identifier, as in
 * 62 <DID>. The validator, the decoder and the gate all stop here. */
static const uint8_t kMuxMaxBytes = 3;

/* Read the def's mux key from a payload into *out_key, first byte most
 * significant whatever the field's byte_order: the key is a byte
 * sequence, not a number. False when the key region ends past
 * payload_len. The caller has checked mux_length (1 to kMuxMaxBytes).
 * Shared by decode and the frame gate, so the two can't disagree on
 * which payloads carry the key. */
static bool read_mux_key(const ecuio_signal_def_t* signal,
                         const uint8_t* payload, uint32_t payload_len,
                         uint32_t* out_key)
{
    const uint32_t key_end = (uint32_t)signal->mux_offset + signal->mux_length;
    if (key_end > payload_len) {
        return false;
    }
    uint32_t key = 0;
    for (uint8_t i = 0; i < signal->mux_length; ++i) {
        key = (key << 8) | payload[signal->mux_offset + i];
    }
    *out_key = key;
    return true;
}

/* The kinds ecuio_decode_signal decodes: numbers. */
static bool is_decodable_kind(uint8_t kind)
{
    return kind == ECUIO_VALUE_UINT || kind == ECUIO_VALUE_INT ||
           kind == ECUIO_VALUE_FLOAT;
}

/* The kinds ecuio_decode_text decodes: byte fields shown as text. */
static bool is_text_kind(uint8_t kind)
{
    return kind == ECUIO_VALUE_TEXT || kind == ECUIO_VALUE_HEX;
}

static bool is_known_byte_order(uint8_t byte_order)
{
    return byte_order == ECUIO_BYTE_ORDER_LE ||
           byte_order == ECUIO_BYTE_ORDER_BE;
}

/* A big-endian field is whole bytes; the extractor can't read less. */
static bool is_whole_bytes(const ecuio_signal_def_t* signal)
{
    return signal->bit_offset == 0 && (signal->bit_length % 8) == 0;
}

static bool all_zero(const void* bytes, size_t n)
{
    const unsigned char* p = (const unsigned char*)bytes;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

int ecuio_decode_signal(const ecuio_signal_def_t* signal,
                        const uint8_t* frame_payload,
                        uint16_t payload_len,
                        double* out_value)
{
    if (!signal || !frame_payload || !out_value) {
        return ECUIO_DECODE_ERR_NULL;
    }

    /* Kind and byte order FIRST, before the bit parameters */
    if (!is_decodable_kind(signal->kind) ||
        !is_known_byte_order(signal->byte_order)) {
        return ECUIO_DECODE_ERR_UNSUPPORTED;
    }

    const bool big_endian = signal->byte_order == ECUIO_BYTE_ORDER_BE;
    if (signal->bit_length == 0 || signal->bit_length > 32) {
        return ECUIO_DECODE_ERR_BAD_PARAMS;
    }
    if (signal->bit_offset > 7) {
        return ECUIO_DECODE_ERR_BAD_PARAMS;
    }
    if (big_endian && !is_whole_bytes(signal)) {
        return ECUIO_DECODE_ERR_BAD_PARAMS;
    }
    if (signal->kind == ECUIO_VALUE_FLOAT &&
        (signal->bit_offset != 0 || signal->bit_length != 32)) {
        return ECUIO_DECODE_ERR_UNSUPPORTED;   /* binary32 only */
    }

    /* Highest bit consumed lies in this byte index — single uniform
       bounds check for every kind and byte order. */
    uint32_t last_bit = (uint32_t)signal->start_byte * 8u
                        + signal->bit_offset + signal->bit_length - 1u;
    if ((last_bit >> 3) >= payload_len) {
        return ECUIO_DECODE_ERR_PAST_PAYLOAD;
    }

    /* MUX check (if enabled) */
    if (signal->mux_length > 0) {
        if (signal->mux_length > kMuxMaxBytes) {
            return ECUIO_DECODE_ERR_BAD_PARAMS;
        }
        uint32_t key = 0;
        if (!read_mux_key(signal, frame_payload, payload_len, &key)) {
            return ECUIO_DECODE_ERR_PAST_PAYLOAD;
        }
        if (key != signal->mux_key) {
            return ECUIO_DECODE_ERR_MUX_MISMATCH;
        }
    }

    uint32_t raw;
    if (big_endian) {
        raw = extract_bytes_be(frame_payload, signal->start_byte,
                                  signal->bit_length);
    } else {
        raw = extract_bitfield_le(frame_payload, signal->start_byte,
                                  signal->bit_offset, signal->bit_length);
    }

    switch (signal->kind) {
    case ECUIO_VALUE_UINT:
        *out_value = (double)raw * signal->scale + signal->offset;
        return ECUIO_DECODE_OK;

    case ECUIO_VALUE_INT: {
        if (signal->bit_length < 32 &&
            (raw & (1u << (signal->bit_length - 1)))) {
            raw |= (0xFFFFFFFFu << signal->bit_length); /* sign-extend */
        }
        int32_t signed_raw;
        memcpy(&signed_raw, &raw, sizeof signed_raw);
        *out_value = (double)signed_raw * signal->scale + signal->offset;
        return ECUIO_DECODE_OK;
    }

    case ECUIO_VALUE_FLOAT: {
        /* raw holds the binary32 bit pattern in host order, whichever
         * byte order the payload used. */
        float raw_float;
        memcpy(&raw_float, &raw, sizeof raw_float);
        *out_value = (double)raw_float * signal->scale + signal->offset;
        return ECUIO_DECODE_OK;
    }

    default:
        return ECUIO_DECODE_ERR_UNSUPPORTED;
    }
}

int ecuio_decode_text(const ecuio_signal_def_t* signal,
                      const uint8_t* frame_payload,
                      uint16_t payload_len,
                      char* out,
                      uint32_t out_cap)
{
    if (!signal || !frame_payload || !out || out_cap == 0) {
        return ECUIO_DECODE_ERR_NULL;
    }

    /* Kind and byte order first, as in ecuio_decode_signal; the order
     * means nothing to bytes, but an unknown one is still a def the
     * validator refuses. */
    if (!is_text_kind(signal->kind) ||
        !is_known_byte_order(signal->byte_order)) {
        return ECUIO_DECODE_ERR_UNSUPPORTED;
    }
    if (signal->bit_length != 0 || signal->bit_offset != 0 ||
        signal->byte_length > ECUIO_FRAME_DATA_MAX) {
        return ECUIO_DECODE_ERR_BAD_PARAMS;
    }

    /* The field: byte_length bytes from start_byte, or the rest of the
     * payload. An empty field has nothing to show. */
    if (signal->start_byte >= payload_len) {
        return ECUIO_DECODE_ERR_PAST_PAYLOAD;
    }
    uint32_t n = signal->byte_length;
    if (n == 0) {
        n = (uint32_t)payload_len - signal->start_byte;
    } else if ((uint32_t)signal->start_byte + n > payload_len) {
        return ECUIO_DECODE_ERR_PAST_PAYLOAD;
    }

    if (signal->mux_length > 0) {
        if (signal->mux_length > kMuxMaxBytes) {
            return ECUIO_DECODE_ERR_BAD_PARAMS;
        }
        uint32_t key = 0;
        if (!read_mux_key(signal, frame_payload, payload_len, &key)) {
            return ECUIO_DECODE_ERR_PAST_PAYLOAD;
        }
        if (key != signal->mux_key) {
            return ECUIO_DECODE_ERR_MUX_MISMATCH;
        }
    }

    const uint8_t* field = frame_payload + signal->start_byte;
    uint32_t pos = 0;
    if (signal->kind == ECUIO_VALUE_HEX) {
        static const char digits[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < n; ++i) {
            /* A pair, its separator and the NUL must all fit, so a cut
             * text ends on a whole pair. */
            const uint32_t need = (i > 0) ? 3u : 2u;
            if (pos + need + 1u > out_cap) {
                break;
            }
            if (i > 0) {
                out[pos++] = ' ';
            }
            out[pos++] = digits[field[i] >> 4];
            out[pos++] = digits[field[i] & 0x0Fu];
        }
    } else {
        for (uint32_t i = 0; i < n && pos + 1u < out_cap; ++i) {
            const uint8_t c = field[i];
            out[pos++] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '.';
        }
    }
    out[pos] = '\0';
    return ECUIO_DECODE_OK;
}

int ecuio_def_applies_to_frame(const ecuio_signal_def_t* signal,
                               const ecuio_frame_t* frame)
{
    if (!signal || !frame) {
        return 0;
    }

    /* Arb ID gate */
    if (frame->arbitration_id != signal->message_id) {
        return 0;
    }

    /* Mux gate (if enabled). A key length decode would reject doesn't
     * apply either. */
    if (signal->mux_length > 0) {
        if (signal->mux_length > kMuxMaxBytes) {
            return 0;
        }
        uint32_t len = frame->data_length;
        if (len > sizeof(frame->data)) {
            len = sizeof(frame->data);
        }
        uint32_t key = 0;
        if (!read_mux_key(signal, frame->data, len, &key) ||
            key != signal->mux_key) {
            return 0;
        }
    }

    return 1;
}

int ecuio_validate_signal(const ecuio_signal_def_t* signal)
{
    if (!signal) {
        return ECUIO_VALIDATE_ERR_NULL;
    }

    /* The decoders reject every other kind and byte order, so a def of
     * one could register and then never decode. */
    if (!is_decodable_kind(signal->kind) && !is_text_kind(signal->kind)) {
        return ECUIO_VALIDATE_ERR_KIND;
    }
    if (!is_known_byte_order(signal->byte_order)) {
        return ECUIO_VALIDATE_ERR_BYTE_ORDER;
    }

    /* A text kind is a byte field: no bit parameters, and byte_length
     * within a frame. A numeric kind is bits, and byte_length is the
     * text kinds' alone. The shapes ecuio_decode_text and
     * ecuio_decode_signal take. */
    if (is_text_kind(signal->kind)) {
        if (signal->bit_length != 0) {
            return ECUIO_VALIDATE_ERR_BIT_LENGTH;
        }
        if (signal->bit_offset != 0) {
            return ECUIO_VALIDATE_ERR_BIT_OFFSET;
        }
        if (signal->byte_length > ECUIO_FRAME_DATA_MAX) {
            return ECUIO_VALIDATE_ERR_BYTE_LENGTH;
        }
    } else {
        if (signal->bit_length == 0 || signal->bit_length > 32) {
            return ECUIO_VALIDATE_ERR_BIT_LENGTH;
        }
        if (signal->bit_offset > 7) {
            return ECUIO_VALIDATE_ERR_BIT_OFFSET;
        }
        if (signal->byte_length != 0) {
            return ECUIO_VALIDATE_ERR_BYTE_LENGTH;
        }
    }

    /* MUX validation. The key lives in the frame's payload, and any
     * region a one-byte mux_offset can name fits ECUIO_FRAME_DATA_MAX,
     * so only decode can find it past the end (payload_len). */
    if (signal->mux_length > kMuxMaxBytes) {
        return ECUIO_VALIDATE_ERR_MUX_LENGTH;
    }
    if (signal->mux_length > 0) {
        /* 0xFF, 0xFFFF or 0xFFFFFF: the shift is at most 24 bits */
        const uint32_t mux_max = (1u << (8u * signal->mux_length)) - 1u;
        if (signal->mux_key > mux_max) {
            return ECUIO_VALIDATE_ERR_MUX_KEY;
        }
    }

    /* The big-endian extractor reads whole bytes MSB-first, and the
     * decoder refuses any other shape. */
    if (signal->byte_order == ECUIO_BYTE_ORDER_BE && !is_whole_bytes(signal)) {
        return ECUIO_VALIDATE_ERR_BIG_ENDIAN;
    }

    /* A FLOAT is binary32: 32 bits at bit 0, in either byte order. */
    if (signal->kind == ECUIO_VALUE_FLOAT &&
        (signal->bit_length != 32 || signal->bit_offset != 0)) {
        return ECUIO_VALIDATE_ERR_FLOAT32;
    }

    /* Last byte MUST be null — guarantees C-string fits in fixed buffer */
    if (signal->name[sizeof(signal->name) - 1] != '\0') {
        return ECUIO_VALIDATE_ERR_NAME;
    }
    if (signal->unit[sizeof(signal->unit) - 1] != '\0') {
        return ECUIO_VALIDATE_ERR_UNIT;
    }

    if (signal->decimal_places > 3) {
        return ECUIO_VALIDATE_ERR_DECIMALS;
    }

    /* Zero, so a later field can take this room and existing sets
     * still load. No flag is defined. */
    if (signal->reserved0 != 0 ||
        !all_zero(signal->mux_reserved, sizeof signal->mux_reserved) ||
        !all_zero(signal->display_reserved, sizeof signal->display_reserved) ||
        signal->flags != 0 ||
        !all_zero(signal->reserved, sizeof signal->reserved)) {
        return ECUIO_VALIDATE_ERR_RESERVED;
    }

    return ECUIO_VALIDATE_OK;
}

/* Imperial conversions, keyed by the decoded unit: to = from * scale +
 * offset, with extra decimal places where the new unit is coarser. */
struct display_conversion {
    const char* from;
    const char* to;
    double scale;
    double offset;
    uint8_t extra_decimals;
};

static const display_conversion kImperial[] = {
    {"km/h",              "mph",               0.621371192237334, 0.0,  0},
    {"\xC2\xB0" "C",      "\xC2\xB0" "F",      1.8,               32.0, 0},
    {"kPa",               "psi",               0.145037737730209, 0.0,  1},
    {"g/s",               "lb/min",            0.132277357312033, 0.0,  1},
    {"km",                "mi",                0.621371192237334, 0.0,  0},
};

int ecuio_display_convert(const char* unit, uint8_t decimal_places,
                          double value, uint32_t units,
                          ecuio_display_value_t* out)
{
    if (!unit || !out) {
        return ECUIO_DISPLAY_ERR_NULL;
    }
    if (units != ECUIO_UNITS_METRIC && units != ECUIO_UNITS_IMPERIAL) {
        return ECUIO_DISPLAY_ERR_UNITS;
    }

    ecuio_display_value_t result;
    memset(&result, 0, sizeof result);
    result.value = value;
    result.decimal_places = decimal_places;
    /* Bounded copy: a def's unit is at most 15 bytes, but this takes
     * any string */
    size_t n = strlen(unit);
    if (n >= sizeof result.unit) {
        n = sizeof result.unit - 1;
    }
    memcpy(result.unit, unit, n);

    if (units == ECUIO_UNITS_IMPERIAL) {
        for (const display_conversion& c : kImperial) {
            if (strcmp(unit, c.from) == 0) {
                result.value = value * c.scale + c.offset;
                memset(result.unit, 0, sizeof result.unit);
                memcpy(result.unit, c.to, strlen(c.to));
                const unsigned dp = (unsigned)decimal_places + c.extra_decimals;
                result.decimal_places = (uint8_t)(dp > 3u ? 3u : dp);
                result.converted = 1;
                break;
            }
        }
    }
    *out = result;
    return ECUIO_DISPLAY_OK;
}
