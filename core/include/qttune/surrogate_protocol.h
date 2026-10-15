// File/layer: core/include/qttune/surrogate_protocol.h | shared app<->surrogate contract
// Testing: structural — compiled by BOTH x64 client and x86 surrogate, so
//          layout drift is impossible WITHIN a tree. Across trees it fails
//          loudly: byte-mode pipe reads return the WRITER's struct size, and
//          ov_read/transact compare against the READER's — a skewed
//          build-win32 tree now fails the length check with a SUR_LOG line
//          instead of silently misinterpreting (lesson: 2026-10-15, the
//          CAN_29BIT_ID version-skew round).

#ifndef QTTUNE_SURROGATE_PROTOCOL_H
#define QTTUNE_SURROGATE_PROTOCOL_H

#include <stdint.h>

/* Must be a power of two — index masking depends on it. */
#define QTTUNE_SURROGATE_RING_SIZE 4096
#if (QTTUNE_SURROGATE_RING_SIZE & (QTTUNE_SURROGATE_RING_SIZE - 1)) != 0
#error QTTUNE_SURROGATE_RING_SIZE must be a power of two
#endif

/* ---- Wire limits (Part 4) ---- */
/* Payload per control-plane message. Matches the qttune_frame_t ABI cap —
 * ISO 15765 multiframe must be reassembled surrogate-side before push
 * (documented pre-existing limitation, unchanged). */
#define QTTUNE_SUR_MSG_MAX 64
/* Flattened SCONFIG array: QTTUNE_PT_SCONFIG_MAX_PARAMS pairs of two
 * 32-bit words. SCONFIG_LIST itself carries a POINTER (ConfigPtr) and may
 * NEVER cross this boundary in struct form — the surrogate materializes a
 * real SCONFIG_LIST in its own address space before calling the vendor. */
#define QTTUNE_SUR_IOCTL_DATA_MAX 64
/* J2534-1 v4.04 filter pattern maximum (spec: 1-12 bytes). */
#define QTTUNE_SUR_FILTER_PATTERN_MAX 12

/* Request opcodes — append-only, never renumber. */
#define QTTUNE_SUR_OP_OPEN       1   /* handshake; DLL already loaded at spawn */
#define QTTUNE_SUR_OP_CONNECT    2   /* ACTIVE: PassThruConnect relay */
#define QTTUNE_SUR_OP_IOCTL      3   /* ACTIVE (Part 4): GET/SET_CONFIG, READ_VBATT, CLEAR_* */
#define QTTUNE_SUR_OP_WRITE      4   /* ACTIVE (Part 4): single-message PassThruWriteMsgs */
#define QTTUNE_SUR_OP_READ       5   /* reserved — data plane is the ring */
#define QTTUNE_SUR_OP_SHUTDOWN   6
#define QTTUNE_SUR_OP_DISCONNECT 7
#define QTTUNE_SUR_OP_INJECT     8   /* TEST-ONLY: forwarded to fake DLL hook */
#define QTTUNE_SUR_OP_START_FILTER 9   /* Part 4: PassThruStartMsgFilter relay */
#define QTTUNE_SUR_OP_STOP_FILTER  10  /* Part 4: PassThruStopMsgFilter relay */

typedef struct qttune_sur_request {
    uint32_t request_id;    /* echoed in response; detects desync */
    uint32_t opcode;
    union {
        struct { uint32_t protocol_id; uint32_t flags; uint32_t baud; } connect;
        struct { uint32_t channel_id; } disconnect;
        /* Ioctl: flattened (Parameter, Value) u32 pairs, little-endian
         * memcpy order (both sides are x86/x64 LE — strong Win32 pair).
         * input_len must be a multiple of 8 for GET/SET_CONFIG; 0 for
         * ioctls taking NULL input. Output lands in the response. */
        struct { uint32_t channel_id; uint32_t ioctl_id;
                 uint8_t input[QTTUNE_SUR_IOCTL_DATA_MAX];
                 uint32_t input_len; } ioctl;
        /* Write: ONE PASSTHRU_MSG per request. tx_flags carries
         * TxFlags (CAN_29BIT_ID for 29-bit). Vendor timeout passed
         * through verbatim — 0 is the non-blocking choice for the
         * synchronous control-pump relay. */
        struct { uint32_t channel_id;
                 uint8_t data[QTTUNE_SUR_MSG_MAX];
                 uint32_t data_len; uint32_t tx_flags;
                 uint32_t timeout_ms; } write;
        struct { uint32_t timeout_ms; } read;
        struct { uint8_t data[QTTUNE_SUR_MSG_MAX]; uint32_t len;
                 uint32_t timestamp; uint32_t rx_flags; } inject;
        /* Filter: mask/pattern(/flow-control) as flat byte arrays —
         * the surrogate expands each into a full PASSTHRU_MSG
         * (ProtocolID=ISO15765, TxFlags from tx_flags, DataSize=len,
         * Data=bytes). flow-control is optional: fc_len==0 means the
         * vendor receives NULL (PASS/BLOCK filter semantics). */
        struct { uint32_t channel_id; uint32_t filter_type; uint32_t tx_flags;
                 uint8_t mask[QTTUNE_SUR_FILTER_PATTERN_MAX];
                 uint8_t pattern[QTTUNE_SUR_FILTER_PATTERN_MAX];
                 uint8_t flow_control[QTTUNE_SUR_FILTER_PATTERN_MAX];
                 uint32_t mask_len; uint32_t pattern_len; uint32_t fc_len;
                 } start_filter;
        struct { uint32_t channel_id; uint32_t filter_id; } stop_filter;
    } u;
} qttune_sur_request_t;

/* Response: GROWS downward-compatibly (new fields appended). Old child +
 * new parent fails the pipe length check loudly; never silent garbage.
 *
 * Field semantics by opcode:
 *   handle_or_channel: device_id (OPEN), channel (CONNECT/WRITE),
 *                      filter_id (START_FILTER)
 *   aux:               messages actually written (WRITE) — vendors may
 *                      partially write; the caller decides if partial
 *                      is an error
 *   data/data_len:     GET_CONFIG write-back (updated flattened SCONFIG
 *                      pairs), or READ_VBATT's 4-byte result */
typedef struct qttune_sur_response {
    uint32_t request_id;
    int32_t  status;             /* J2534 vendor status, verbatim */
    uint32_t handle_or_channel;
    uint32_t aux;
    uint8_t  data[QTTUNE_SUR_IOCTL_DATA_MAX];
    uint32_t data_len;
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
