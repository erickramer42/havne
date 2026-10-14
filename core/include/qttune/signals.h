#ifndef QTTUNE_SIGNALS_H
#define QTTUNE_SIGNALS_H

#include <stdint.h>

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
    /* NEW (was reserved[0]): CAN arbitration ID this signal rides on.
     * Consumers gate decode on frame->arbitration_id == message_id
     * BEFORE extraction; qttune_decode_signal itself remains payload-
     * only. Promoting a reserved slot: struct size and all other field
     * offsets are UNCHANGED. */
    uint32_t message_id;
    uint32_t reserved[2];
} QttuneSignalDef;

/* Single canonical definition — no duplicates */
typedef struct QttuneSignalSet {
    uint32_t version;
    uint32_t count;
    const QttuneSignalDef* defs;
} QttuneSignalSet;

int qttune_decode_signal(const QttuneSignalDef* signal,
                         const uint8_t* frame_payload,
                         uint8_t payload_len,
                         float* out_value);

int qttune_validate_signal(const QttuneSignalDef* signal);

/* Set-level APIs */
const QttuneSignalSet* qttune_mock_signal_set(void);
int qttune_validate_signal_set(const QttuneSignalSet* set);

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_SIGNALS_H */
