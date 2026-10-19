// File/layer: core/include/qttune/surrogate_protocol.h | shared app<->surrogate contract
// Testing: structural — compiled by BOTH x64 client and x86 surrogate, so
//          layout drift is impossible WITHIN a tree. Across trees it fails
//          loudly: byte-mode pipe reads return the WRITER's struct size.

#ifndef QTTUNE_SURROGATE_PROTOCOL_H
#define QTTUNE_SURROGATE_PROTOCOL_H

#include <stdint.h>

/* ---- Wire limits ---- */
#define QTTUNE_SUR_MSG_MAX 64
#define QTTUNE_SUR_IOCTL_DATA_MAX 64
#define QTTUNE_SUR_FILTER_PATTERN_MAX 12
#define QTTUNE_SUR_MAX_FRAMES_PER_READ 16

/* Request opcodes — append-only, never renumber. */
#define QTTUNE_SUR_OP_OPEN       1
#define QTTUNE_SUR_OP_CONNECT    2
#define QTTUNE_SUR_OP_IOCTL      3
#define QTTUNE_SUR_OP_WRITE      4
#define QTTUNE_SUR_OP_READ       5
#define QTTUNE_SUR_OP_SHUTDOWN   6
#define QTTUNE_SUR_OP_DISCONNECT 7
#define QTTUNE_SUR_OP_INJECT     8
#define QTTUNE_SUR_OP_START_FILTER 9
#define QTTUNE_SUR_OP_STOP_FILTER  10

typedef struct qttune_sur_request {
    uint32_t request_id;
    uint32_t opcode;
    union {
        struct { uint32_t protocol_id; uint32_t flags; uint32_t baud; } connect;
        struct { uint32_t channel_id; } disconnect;
        struct { uint32_t channel_id; uint32_t ioctl_id;
                 uint8_t input[QTTUNE_SUR_IOCTL_DATA_MAX];
                 uint32_t input_len; } ioctl;
        struct { uint32_t channel_id;
                 uint8_t data[QTTUNE_SUR_MSG_MAX];
                 uint32_t data_len; uint32_t tx_flags;
                 uint32_t timeout_ms; } write;
        struct { uint32_t timeout_ms; } read;
        struct { uint8_t data[QTTUNE_SUR_MSG_MAX]; uint32_t len;
                 uint32_t timestamp; uint32_t rx_flags; } inject;
        struct { uint32_t channel_id; uint32_t filter_type; uint32_t tx_flags;
                 uint8_t mask[QTTUNE_SUR_FILTER_PATTERN_MAX];
                 uint8_t pattern[QTTUNE_SUR_FILTER_PATTERN_MAX];
                 uint8_t flow_control[QTTUNE_SUR_FILTER_PATTERN_MAX];
                 uint32_t mask_len; uint32_t pattern_len; uint32_t fc_len;
                 } start_filter;
        struct { uint32_t channel_id; uint32_t filter_id; } stop_filter;
    } u;
} qttune_sur_request_t;

/* Single frame for batch read responses. */
typedef struct qttune_sur_frame {
    uint32_t arbitration_id;
    uint32_t timestamp;
    uint32_t rx_flags;
    uint8_t  data[QTTUNE_SUR_MSG_MAX];
    uint8_t  data_len;
} qttune_sur_frame_t;

/* Response with embedded frame array. */
typedef struct qttune_sur_response {
    uint32_t request_id;
    int32_t  status;
    uint32_t handle_or_channel;
    uint32_t aux;
    uint8_t  data[QTTUNE_SUR_IOCTL_DATA_MAX];
    uint32_t data_len;
    uint32_t num_frames;
    qttune_sur_frame_t frames[QTTUNE_SUR_MAX_FRAMES_PER_READ];
} qttune_sur_response_t;

#endif /* QTTUNE_SURROGATE_PROTOCOL_H */
