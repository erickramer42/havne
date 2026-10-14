// File/layer: core/include/qttune/surrogate_protocol.h | shared app<->surrogate contract
// Testing: structural — compiled by BOTH x64 client and x86 surrogate, so
//          layout drift is impossible. All types fixed-width, no pointers:
//          identical layout under x86 and x64 processes on shared memory.

#ifndef QTTUNE_SURROGATE_PROTOCOL_H
#define QTTUNE_SURROGATE_PROTOCOL_H

#include <stdint.h>

/* Must be a power of two — index masking depends on it. */
#define QTTUNE_SURROGATE_RING_SIZE 4096
#if (QTTUNE_SURROGATE_RING_SIZE & (QTTUNE_SURROGATE_RING_SIZE - 1)) != 0
#error QTTUNE_SURROGATE_RING_SIZE must be a power of two
#endif

/* Request opcodes — append-only, never renumber */
#define QTTUNE_SUR_OP_OPEN       1   /* handshake; DLL already loaded at spawn */
#define QTTUNE_SUR_OP_CONNECT    2
#define QTTUNE_SUR_OP_IOCTL      3   /* reserved for Part 3 (FIVE_BAUD etc.) */
#define QTTUNE_SUR_OP_WRITE      4   /* reserved */
#define QTTUNE_SUR_OP_READ       5   /* reserved — data plane is the ring */
#define QTTUNE_SUR_OP_SHUTDOWN   6
#define QTTUNE_SUR_OP_DISCONNECT 7
#define QTTUNE_SUR_OP_INJECT     8   /* TEST-ONLY: forwarded to fake DLL hook */

typedef struct qttune_sur_request {
    uint32_t request_id;    /* echoed in response; detects desync */
    uint32_t opcode;
    union {
        struct { uint32_t protocol_id; uint32_t flags; uint32_t baud; } connect;
        struct { uint32_t channel_id; } disconnect;
        struct { uint32_t channel_id; uint32_t ioctl_id;
                 uint8_t input[16]; uint32_t input_len; } ioctl;
        struct { uint32_t channel_id; uint8_t data[64];
                 uint32_t data_len; uint32_t msg_flags; } write;
        struct { uint32_t timeout_ms; } read;
        struct { uint8_t data[64]; uint32_t len;
                 uint32_t timestamp; uint32_t rx_flags; } inject;
    } u;
} qttune_sur_request_t;

typedef struct qttune_sur_response {
    uint32_t request_id;
    int32_t  status;             /* J2534 vendor status, verbatim */
    uint32_t handle_or_channel;  /* device_id (OPEN) or channel (CONNECT) */
} qttune_sur_response_t;

/* Data-plane ring slot — PASSTHRU_MSG normalized to frame-sized payload.
 * NOTE: 64-byte cap means ISO 15765 multiframe (up to 4128) must be
 * REASSEMBLED surrogate-side before push — Part 3 work, documented. */
typedef struct qttune_sur_ring_slot {
    uint8_t  data[64];
    uint32_t data_len;
    uint32_t protocol_id;
    uint32_t rx_status;
    uint32_t timestamp;       /* vendor units, passthrough (ptypes contract) */
    uint32_t extra_data_index;
} qttune_sur_ring_slot_t;

/* SPSC ring: producer (surrogate read pump) owns HEAD, consumer (app)
 * owns TAIL. Both monotonic; occupancy = head - tail; slot index = pos &
 * (SIZE-1). Correct on x86/x64 strong memory models (store order
 * preserved); this is a Win32 pair by construction — do not port to
 * weakly-ordered targets without adding barriers. */
typedef struct qttune_sur_ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t reserved[2];
    qttune_sur_ring_slot_t slots[QTTUNE_SURROGATE_RING_SIZE];
} qttune_sur_ring_t;

#endif /* QTTUNE_SURROGATE_PROTOCOL_H */
