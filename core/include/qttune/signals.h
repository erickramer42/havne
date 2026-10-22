// File/layer: core/include/qttune/signals.h | C ABI, public
// Testing: UNIT — validator edge cases, mux range checks, BE vs LE comparison

#ifndef QTTUNE_SIGNALS_H
#define QTTUNE_SIGNALS_H

/* qttune_def_applies_to_frame needs the complete qttune_frame_t, so
 * include core.h (which never includes this header — no cycle). A
 * forward declaration would clash with core.h's typedef (C2371). */
#include "qttune/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QTTUNE_SIGNAL_TYPE_UINT8   = 0,
    QTTUNE_SIGNAL_TYPE_UINT16  = 1,
    QTTUNE_SIGNAL_TYPE_UINT32  = 2,
    QTTUNE_SIGNAL_TYPE_INT8    = 3,
    QTTUNE_SIGNAL_TYPE_INT16   = 4,
    QTTUNE_SIGNAL_TYPE_INT32   = 5,
    QTTUNE_SIGNAL_TYPE_FLOAT32 = 6,
    QTTUNE_SIGNAL_TYPE_STRING  = 7,  /* Reserved: no decoder yet */
    QTTUNE_SIGNAL_TYPE_MANUF_START = 256  /* Reserved range: no decoder
                                             yet */
} QttuneSignalType;

/* Signal definition flags */
#define QTTUNE_SIGNAL_FLAG_BIG_ENDIAN  (1u << 0)

typedef struct QttuneSignalDef {
    uint16_t id;
    char name[32];            /* display name, UTF-8; UIs show it as is */
    QttuneSignalType type;
    uint8_t start_byte;
    uint8_t num_bytes;
    uint8_t bit_offset;
    uint8_t bit_length;
    float scale;
    float offset;
    char unit[16];            /* display unit, UTF-8 (a degree sign, "km/h") */
    uint8_t decimal_places;   /* display precision of the decoded value */
    uint32_t flags;
    
    /* CAN arbitration ID this signal rides on. Consumers gate decode
     * on frame->arbitration_id == message_id BEFORE extraction;
     * qttune_decode_signal itself remains payload-only. */
    uint32_t message_id;
    
    /* MUX descriptor: packed [offset:8][length:8][reserved:16]
     * - offset: byte offset in payload where mux key sits (0..payload_len-1)
     * - length: 0 = mux disabled; 1 or 2 = key size in bytes
     * - reserved: must be zero
     * When mux is enabled (length > 0), decode proceeds ONLY if
     * the payload's key bytes match mux_key exactly. This enables
     * OBD-II response discrimination (e.g., 0x41 <PID> echo). */
    uint8_t mux_offset;
    uint8_t mux_length;
    uint8_t mux_reserved[2];
    uint32_t mux_key;  /* The byte pattern to match at mux_offset */
} QttuneSignalDef;

/* ADR-0013 §4: profile-declared channels. rx_id is the
 * flow-control filter target (receive); tx_id is the request
 * destination. Sets declaring none (raw/mock) keep channel_count
 * 0 — no filters, no requests, exactly today's behavior. This is
 * the seed of the v0.5.0 plugin filter surface: bounded, validated
 * at registration, consumed by the j2534 transport only. */
#define QTTUNE_MAX_CHANNELS 4

typedef struct QttuneChannelDecl {
    uint32_t rx_id;   /* flow-control filter target */
    uint32_t tx_id;   /* request destination */
} QttuneChannelDecl;

typedef struct QttuneSignalSet {
    uint32_t version;
    uint32_t count;
    const QttuneSignalDef* defs;
    /* Bounded channel list. 0/null = none declared. Aggregate
     * initializers with three members still compile (remaining
     * members value-initialize), but update them explicitly when
     * touched — see test_obd2.cpp. */
    uint32_t channel_count;
    const QttuneChannelDecl* channels;
} QttuneSignalSet;

/* qttune_decode_signal return codes. Values are append-only. */
#define QTTUNE_DECODE_OK                0
#define QTTUNE_DECODE_ERR_NULL          1  /* a NULL argument */
#define QTTUNE_DECODE_ERR_PAST_PAYLOAD  2  /* the field or the mux key
                                              ends past payload_len */
#define QTTUNE_DECODE_ERR_BAD_PARAMS    3  /* bit parameters or mux
                                              length out of range */
#define QTTUNE_DECODE_ERR_UNSUPPORTED   4  /* a type with no decoder, or a
                                              FLOAT32 not 32 bits at bit 0 */
#define QTTUNE_DECODE_ERR_MUX_MISMATCH  5  /* the payload's mux key differs */

/* Extract one signal from a payload, scale and offset applied. Checks
 * the type before the bit parameters, then the bounds, then the mux
 * key. *out_value is written only on QTTUNE_DECODE_OK. */
int qttune_decode_signal(const QttuneSignalDef* signal,
                         const uint8_t* frame_payload,
                         uint8_t payload_len,
                         float* out_value);

/* qttune_validate_signal and qttune_validate_signal_set return codes.
 * Values are append-only. Per-def codes are positive; the set-level
 * codes are negative, so a set's result says which kind failed. */
#define QTTUNE_VALIDATE_OK                    0
#define QTTUNE_VALIDATE_ERR_NULL              1  /* a NULL def or set, or
                                                    count > 0 with NULL
                                                    defs */
#define QTTUNE_VALIDATE_ERR_NUM_BYTES         2  /* num_bytes not 1..4 */
#define QTTUNE_VALIDATE_ERR_BIT_LENGTH        3  /* 0, or over 8 * num_bytes */
#define QTTUNE_VALIDATE_ERR_BIT_OFFSET        4  /* over 7 */
#define QTTUNE_VALIDATE_ERR_BIT_SPAN          5  /* offset + length over
                                                    8 * num_bytes */
#define QTTUNE_VALIDATE_ERR_NAME              6  /* name not terminated
                                                    inside its buffer */
#define QTTUNE_VALIDATE_ERR_UNIT              7  /* unit not terminated */
#define QTTUNE_VALIDATE_ERR_DECIMALS          8  /* decimal_places over 3 */
#define QTTUNE_VALIDATE_ERR_MUX_LENGTH        9  /* mux_length over 2 */
#define QTTUNE_VALIDATE_ERR_MUX_RANGE        10  /* key region past the
                                                    64-byte frame payload */
#define QTTUNE_VALIDATE_ERR_MUX_KEY          11  /* key wider than its
                                                    length */
#define QTTUNE_VALIDATE_ERR_BIG_ENDIAN       12  /* big-endian field not
                                                    byte-aligned */
#define QTTUNE_VALIDATE_ERR_TYPE             13  /* a type the decoder
                                                    can't decode: STRING,
                                                    an unknown value, or
                                                    the MANUF range */
#define QTTUNE_VALIDATE_ERR_FLOAT32          14  /* FLOAT32 not 4 bytes,
                                                    32 bits at bit 0,
                                                    little-endian */
#define QTTUNE_VALIDATE_ERR_CHANNELS_NULL   (-10)  /* channel_count > 0 with
                                                      NULL channels */
#define QTTUNE_VALIDATE_ERR_CHANNEL_COUNT   (-11)  /* over QTTUNE_MAX_CHANNELS */
#define QTTUNE_VALIDATE_ERR_CHANNEL_ID      (-12)  /* rx or tx 0, or past
                                                      0x1FFFFFFF */
#define QTTUNE_VALIDATE_ERR_DUPLICATE_RX    (-13)  /* two channels share an rx */
#define QTTUNE_VALIDATE_ERR_DUPLICATE_ID    (-14)  /* two defs share an id */

/* Structural check of one def: accepts exactly the defs
 * qttune_decode_signal can decode. The type is checked first. */
int qttune_validate_signal(const QttuneSignalDef* signal);

/* Frame gate, called BEFORE qttune_decode_signal (the bridge's decode
 * loop): returns 1 when the def applies to the frame (its message_id
 * is the frame's arbitration ID and, with mux on, the frame carries
 * its mux key), and 0 otherwise or for a NULL argument. Saves the
 * extraction for frames the def can't read. */
int qttune_def_applies_to_frame(const QttuneSignalDef* signal,
                                const qttune_frame_t* frame);

/* Set-level APIs */
const QttuneSignalSet* qttune_mock_signal_set(void);
const QttuneSignalSet* qttune_obd2_signal_set(void);  /* ADR-0013 obd2 profile */

/* Every def (qttune_validate_signal; the first failing def's code),
 * then the set: unique def ids, then the channel declarations. A set
 * with count 0 may leave defs NULL. Returns QTTUNE_VALIDATE_OK or a
 * QTTUNE_VALIDATE_ERR_* code. */
int qttune_validate_signal_set(const QttuneSignalSet* set);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_SIGNALS_H */
