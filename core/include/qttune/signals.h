// File/layer: core/include/qttune/signals.h | C ABI, public
// Testing: UNIT — validator edge cases, mux range checks, BE vs LE comparison

#ifndef QTTUNE_SIGNALS_H
#define QTTUNE_SIGNALS_H

/* qttune_def_matches_frame needs the complete qttune_frame_t, so include
 * core.h (which never includes this header — no cycle). A forward
 * declaration would clash with core.h's typedef (C2371). */
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
    QTTUNE_SIGNAL_TYPE_STRING  = 7,  /* Reserved */
    QTTUNE_SIGNAL_TYPE_MANUF_START = 256
} QttuneSignalType;

/* Signal definition flags */
#define QTTUNE_SIGNAL_FLAG_BIG_ENDIAN  (1u << 0)

typedef struct QttuneSignalDef {
    uint16_t id;
    char name[32];
    QttuneSignalType type;
    uint8_t start_byte;
    uint8_t num_bytes;
    uint8_t bit_offset;
    uint8_t bit_length;
    float scale;
    float offset;
    char unit[16];
    uint8_t decimal_places;
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

int qttune_decode_signal(const QttuneSignalDef* signal,
                         const uint8_t* frame_payload,
                         uint8_t payload_len,
                         float* out_value);

int qttune_validate_signal(const QttuneSignalDef* signal);

/* Match helper: arb ID + mux check. Returns 0 if signal applies to
 * frame, non-zero otherwise. Called BEFORE decode_signal in the
 * bridge loop; avoids unnecessary extraction work. Requires the
 * complete qttune_frame_t type, hence signals.h includes core.h. */
int qttune_def_matches_frame(const QttuneSignalDef* signal,
                             const qttune_frame_t* frame);

/* Set-level APIs */
const QttuneSignalSet* qttune_mock_signal_set(void);
const QttuneSignalSet* qttune_obd2_signal_set(void);  /* ADR-0013 obd2 profile */
int qttune_validate_signal_set(const QttuneSignalSet* set);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_SIGNALS_H */
