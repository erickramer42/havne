// File/layer: core/src/signals.cpp | C++17, Qt-free
// Testing: UNIT — BE vs LE round-trip, mux acceptance/rejection, validator rules

#include <qttune/signals.h>
#include <qttune/internal.h>
#include <qttune/signal_sets.h>
#include <string.h>
#include <math.h>

/* Internal helper: extract bitfield from byte array (little-endian)
 * Intel (little-endian) bit numbering: the field's LSB sits at bit
 * `bit_offset` inside byte `start_byte`, running upward through
 * consecutive bytes. */
static uint32_t extract_bitfield_le(const uint8_t* data,
                                     uint8_t start_byte,
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

/* Big-endian extraction: byte-aligned fields only (bit_offset 0,
 * bit_length a multiple of 8 — the validator enforces both). Reads
 * whole bytes MSB-first: first byte is most significant. The
 * existing last_bit bounds check already covers start_byte + n. */
static uint32_t extract_bytes_be(const uint8_t* data,
                                  uint8_t start_byte, uint8_t bit_length)
{
    uint32_t result = 0;
    const uint8_t n = bit_length / 8;
    for (uint8_t i = 0; i < n; ++i) {
        result = (result << 8) | data[start_byte + i];
    }
    return result;
}

/* Read the def's mux key from a payload, big-endian, into *out_key.
 * False when the key region ends past payload_len. The caller has
 * checked mux_length (1 or 2). Shared by decode and the frame gate, so
 * the two can't disagree on which payloads carry the key. */
static bool read_mux_key(const QttuneSignalDef* signal,
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

/* The types qttune_decode_signal decodes. */
static bool is_decodable_type(QttuneSignalType type)
{
    /* Through int: a C caller can store any value in the enum. */
    const int t = (int)type;
    return t >= QTTUNE_SIGNAL_TYPE_UINT8 && t <= QTTUNE_SIGNAL_TYPE_FLOAT32;
}

int qttune_decode_signal(const QttuneSignalDef* signal,
                         const uint8_t* frame_payload,
                         uint8_t payload_len,
                         float* out_value)
{
    if (!signal || !frame_payload || !out_value) {
        return QTTUNE_DECODE_ERR_NULL;
    }

    /* Check for unsupported types FIRST — before bit validation */
    if (!is_decodable_type(signal->type)) {
        return QTTUNE_DECODE_ERR_UNSUPPORTED;
    }

    /* Now safe to validate bit parameters for supported numeric types */
    if (signal->bit_length == 0 || signal->bit_length > 32) {
        return QTTUNE_DECODE_ERR_BAD_PARAMS;
    }
    if (signal->bit_offset > 7) {
        return QTTUNE_DECODE_ERR_BAD_PARAMS;
    }
    if ((uint32_t)signal->bit_offset + signal->bit_length
            > 8u * signal->num_bytes) {
        return QTTUNE_DECODE_ERR_BAD_PARAMS;
    }

    /* Highest bit consumed lies in this byte index — single uniform
       bounds check for every type, including FLOAT32. */
    uint32_t last_bit = (uint32_t)signal->start_byte * 8u
                        + signal->bit_offset + signal->bit_length - 1u;
    if ((last_bit >> 3) >= payload_len) {
        return QTTUNE_DECODE_ERR_PAST_PAYLOAD;
    }

    /* MUX check (if enabled) */
    if (signal->mux_length > 0) {
        if (signal->mux_length > 2) {
            return QTTUNE_DECODE_ERR_BAD_PARAMS;
        }
        uint32_t key = 0;
        if (!read_mux_key(signal, frame_payload, payload_len, &key)) {
            return QTTUNE_DECODE_ERR_PAST_PAYLOAD;
        }
        if (key != signal->mux_key) {
            return QTTUNE_DECODE_ERR_MUX_MISMATCH;
        }
    }

    switch (signal->type) {
    case QTTUNE_SIGNAL_TYPE_UINT8:
    case QTTUNE_SIGNAL_TYPE_UINT16:
    case QTTUNE_SIGNAL_TYPE_UINT32: {
        uint32_t raw;
        if (signal->flags & QTTUNE_SIGNAL_FLAG_BIG_ENDIAN) {
            raw = extract_bytes_be(frame_payload, signal->start_byte,
                                      signal->bit_length);
        } else {
            raw = extract_bitfield_le(frame_payload, signal->start_byte,
                                      signal->bit_offset, signal->bit_length);
        }
        *out_value = (float)raw * signal->scale + signal->offset;
        return QTTUNE_DECODE_OK;
    }

    case QTTUNE_SIGNAL_TYPE_INT8:
    case QTTUNE_SIGNAL_TYPE_INT16:
    case QTTUNE_SIGNAL_TYPE_INT32: {
        uint32_t raw;
        if (signal->flags & QTTUNE_SIGNAL_FLAG_BIG_ENDIAN) {
            raw = extract_bytes_be(frame_payload, signal->start_byte,
                                      signal->bit_length);
        } else {
            raw = extract_bitfield_le(frame_payload, signal->start_byte,
                                      signal->bit_offset, signal->bit_length);
        }
        if (signal->bit_length < 32 &&
            (raw & (1u << (signal->bit_length - 1)))) {
            raw |= (0xFFFFFFFFu << signal->bit_length); /* sign-extend */
        }
        int32_t signed_raw;
        memcpy(&signed_raw, &raw, sizeof signed_raw);
        *out_value = (float)signed_raw * signal->scale + signal->offset;
        return QTTUNE_DECODE_OK;
    }

    case QTTUNE_SIGNAL_TYPE_FLOAT32: {
        if (signal->bit_offset != 0 || signal->bit_length != 32) {
            return QTTUNE_DECODE_ERR_UNSUPPORTED;  /* float must be byte-aligned, 32-bit */
        }
        float raw_float;
        memcpy(&raw_float, frame_payload + signal->start_byte,
               sizeof raw_float);
        *out_value = raw_float * signal->scale + signal->offset;
        return QTTUNE_DECODE_OK;
    }

    default:
        return QTTUNE_DECODE_ERR_UNSUPPORTED;
    }
}

int qttune_def_applies_to_frame(const QttuneSignalDef* signal,
                                const qttune_frame_t* frame)
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
        if (signal->mux_length > 2) {
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

int qttune_validate_signal(const QttuneSignalDef* signal)
{
    if (!signal) {
        return QTTUNE_VALIDATE_ERR_NULL;
    }

    /* The decoder rejects every other type, so a def of one could
     * register and then never decode. */
    if (!is_decodable_type(signal->type)) {
        return QTTUNE_VALIDATE_ERR_TYPE;
    }

    if (signal->num_bytes < 1 || signal->num_bytes > 4) {
        return QTTUNE_VALIDATE_ERR_NUM_BYTES;
    }
    if (signal->bit_length == 0 ||
        signal->bit_length > 8u * signal->num_bytes) {
        return QTTUNE_VALIDATE_ERR_BIT_LENGTH;
    }
    if (signal->bit_offset > 7) {
        return QTTUNE_VALIDATE_ERR_BIT_OFFSET;
    }
    if ((uint32_t)signal->bit_offset + signal->bit_length
            > 8u * signal->num_bytes) {
        return QTTUNE_VALIDATE_ERR_BIT_SPAN;
    }

    /* MUX validation — the key region lives in the FRAME payload,
     * not the signal field, so it bounds against the 64-byte frame ABI
     * cap. decode-time checks enforce the actual payload_len. */
    if (signal->mux_length > 2) {
        return QTTUNE_VALIDATE_ERR_MUX_LENGTH;
    }
    if (signal->mux_length > 0) {
        if ((uint32_t)signal->mux_offset + signal->mux_length > 64u) {
            return QTTUNE_VALIDATE_ERR_MUX_RANGE;
        }
        const uint32_t mux_max =
            (signal->mux_length == 1) ? 0xFFu : 0xFFFFu;
        if (signal->mux_key > mux_max) {
            return QTTUNE_VALIDATE_ERR_MUX_KEY;
        }
    }

    /* big-endian fields are byte-aligned only — the extractor
     * reads whole bytes MSB-first, so non-aligned shapes are rejected
     * at registration instead of misread at decode. */
    if (signal->flags & QTTUNE_SIGNAL_FLAG_BIG_ENDIAN) {
        if (signal->bit_offset != 0 || (signal->bit_length % 8) != 0) {
            return QTTUNE_VALIDATE_ERR_BIG_ENDIAN;
        }
    }

    /* The decoder reads a FLOAT32 as four native (little-endian) bytes
     * at bit 0 and ignores the big-endian flag, so any other shape
     * would fail to decode or read as the wrong number. */
    if (signal->type == QTTUNE_SIGNAL_TYPE_FLOAT32 &&
        (signal->bit_length != 32 || signal->bit_offset != 0 ||
         (signal->flags & QTTUNE_SIGNAL_FLAG_BIG_ENDIAN))) {
        return QTTUNE_VALIDATE_ERR_FLOAT32;
    }

    /* Last byte MUST be null — guarantees C-string fits in fixed buffer */
    if (signal->name[sizeof(signal->name) - 1] != '\0') {
        return QTTUNE_VALIDATE_ERR_NAME;
    }
    if (signal->unit[sizeof(signal->unit) - 1] != '\0') {
        return QTTUNE_VALIDATE_ERR_UNIT;
    }

    if (signal->decimal_places > 3) {
        return QTTUNE_VALIDATE_ERR_DECIMALS;
    }

    return QTTUNE_VALIDATE_OK;
}
