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

int qttune_decode_signal(const QttuneSignalDef* signal,
                         const uint8_t* frame_payload,
                         uint8_t payload_len,
                         float* out_value)
{
    if (!signal || !frame_payload || !out_value) {
        return 1;
    }

    /* Check for unsupported types FIRST — before bit validation */
    if (signal->type == QTTUNE_SIGNAL_TYPE_STRING ||
        signal->type > QTTUNE_SIGNAL_TYPE_FLOAT32) {
        return 4; /* unsupported type */
    }

    /* Now safe to validate bit parameters for supported numeric types */
    if (signal->bit_length == 0 || signal->bit_length > 32) {
        return 3;
    }
    if (signal->bit_offset > 7) {
        return 3;
    }
    if ((uint32_t)signal->bit_offset + signal->bit_length
            > 8u * signal->num_bytes) {
        return 3;
    }

    /* Highest bit consumed lies in this byte index — single uniform
       bounds check for every type, including FLOAT32. */
    uint32_t last_bit = (uint32_t)signal->start_byte * 8u
                        + signal->bit_offset + signal->bit_length - 1u;
    if ((last_bit >> 3) >= payload_len) {
        return 2;
    }

    /* MUX check (if enabled) */
    if (signal->mux_length > 0) {
        if (signal->mux_length > 2) {
            return 3;  /* invalid mux_length */
        }
        uint32_t key_end = (uint32_t)signal->mux_offset + signal->mux_length;
        if (key_end > payload_len) {
            return 2;  /* key would overflow payload */
        }
        
        uint32_t mux_match = 0;
        for (uint8_t i = 0; i < signal->mux_length; ++i) {
            mux_match = (mux_match << 8) | frame_payload[signal->mux_offset + i];
        }
        if (mux_match != signal->mux_key) {
            return 5;  /* mux key mismatch */
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
        return 0;
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
        return 0;
    }

    case QTTUNE_SIGNAL_TYPE_FLOAT32: {
        if (signal->bit_offset != 0 || signal->bit_length != 32) {
            return 4; /* float must be byte-aligned, 32-bit */
        }
        float raw_float;
        memcpy(&raw_float, frame_payload + signal->start_byte,
               sizeof raw_float);
        *out_value = raw_float * signal->scale + signal->offset;
        return 0;
    }

    default:
        return 4;
    }
}

int qttune_def_matches_frame(const QttuneSignalDef* signal,
                             const qttune_frame_t* frame)
{
    if (!signal || !frame) {
        return 1;
    }

    /* Arb ID gate */
    if (frame->arbitration_id != signal->message_id) {
        return 1;
    }

    /* Mux gate (if enabled) */
    if (signal->mux_length > 0) {
        if (frame->data_length < (signal->mux_offset + signal->mux_length)) {
            return 1;  /* payload too short for mux key */
        }
        uint32_t mux_match = 0;
        for (uint8_t i = 0; i < signal->mux_length; ++i) {
            mux_match = (mux_match << 8) | frame->data[signal->mux_offset + i];
        }
        if (mux_match != signal->mux_key) {
            return 1;
        }
    }

    return 0;  /* match success */
}

int qttune_validate_signal(const QttuneSignalDef* signal)
{
    if (!signal) {
        return 1;
    }

    if (signal->type < QTTUNE_SIGNAL_TYPE_MANUF_START) {
        if (signal->num_bytes < 1 || signal->num_bytes > 4) {
            return 2;
        }
        if (signal->bit_length == 0 ||
            signal->bit_length > 8u * signal->num_bytes) {
            return 3;
        }
        if (signal->bit_offset > 7) {
            return 4;
        }
        if ((uint32_t)signal->bit_offset + signal->bit_length
                > 8u * signal->num_bytes) {
            return 5;
        }
        
        /* MUX validation — the key region lives in the FRAME payload,
        * not the signal field, so it bounds against the 64-byte frame ABI
        * cap. decode-time checks enforce the actual payload_len. */
        if (signal->mux_length > 2) {
            return 9;   /* invalid mux_length */
        }
        if (signal->mux_length > 0) {
            if ((uint32_t)signal->mux_offset + signal->mux_length > 64u) {
                return 10;   /* key region exceeds frame payload cap */
            }
            const uint32_t mux_max =
                (signal->mux_length == 1) ? 0xFFu : 0xFFFFu;
            if (signal->mux_key > mux_max) {
                return 11;   /* key doesn't fit its own length */
            }
        }

        /* big-endian fields are byte-aligned only — the extractor
        * reads whole bytes MSB-first, so non-aligned shapes are rejected
        * at registration instead of misread at decode. */
        if (signal->flags & QTTUNE_SIGNAL_FLAG_BIG_ENDIAN) {
            if (signal->bit_offset != 0 || (signal->bit_length % 8) != 0) {
                return 12;
            }
        }
    }

    /* Last byte MUST be null — guarantees C-string fits in fixed buffer */
    if (signal->name[sizeof(signal->name) - 1] != '\0') {
        return 6; /* name not null-terminated within buffer */
    }
    if (signal->unit[sizeof(signal->unit) - 1] != '\0') {
        return 7; /* unit not null-terminated within buffer */
    }

    if (signal->decimal_places > 3) {
        return 8;
    }

    return 0;
}
