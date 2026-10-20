// File/layer: core/include/qttune/j2534_ptypes.h | INTERNAL shared types
// Testing: structural — consumed identically by loader, surrogate, fake
//
// Single source of truth for the PASSTHRU contract between
// core/src/j2534_loader.cpp, surrogate/surrogate_main.cpp, and
// tests/fake_j2534.cpp. Because everyone compiles from THIS header,
// layout drift between them is structurally impossible.
//
// NOT public ABI — public consumers use qttune/j2534.h only.

#ifndef QTTUNE_J2534_PTYPES_H
#define QTTUNE_J2534_PTYPES_H

/* Largest single-protocol-message payload (ISO 15765 multiframe
 * reassembly buffer) per J2534-1 v4.04. VERIFIED. */
#define QTTUNE_PT_MSG_DATA_SIZE 4128

/* ---- Status codes (J2534-1 v04.04, VERIFIED) ----
 * The subset qttune uses. TIMEOUT and BUFFER_EMPTY are not errors on
 * a read (quiet bus / empty queue); every other nonzero status is a
 * hard failure. Vendor statuses are mapped to qttune_status_t, never
 * returned as one. Verified against FreeSSM J2534.h and diamondman
 * J2534_v0404.h. */
#define QTTUNE_PT_STATUS_NOERROR    0L
#define QTTUNE_PT_STATUS_FAILED     0x07L
#define QTTUNE_PT_STATUS_TIMEOUT    0x09L
#define QTTUNE_PT_STATUS_BUFFER_EMPTY 0x10L

/* Finding 1 (code review 2026-10-20): trust rule for vendor-reported
 * message counts.
 *
 * Some vendors leave pNumMsgs (and the caller's message buffer)
 * untouched when returning an error, so a nonzero count under a hard
 * error is not evidence any message was written. Observed live on the
 * OBDX Pro VX quiet bus: "OP_WRITE: status 7, written 1"
 * (bench-sessions/2026-10-20.md). Only these statuses carry a count
 * the caller may trust:
 *
 *   STATUS_NOERROR       - all requested messages read
 *   ERR_TIMEOUT          - partial batch already copied (J2534-1)
 *   ERR_BUFFER_OVERFLOW  - pNumMsgs = messages actually copied
 *
 * Every other status (ERR_FAILED, ERR_BUFFER_EMPTY, device errors,
 * ...) means "assume nothing was written". Applies to ReadMsgs; the
 * write-side count is only ever logged, never consumed, and the raw
 * pair is deliberately preserved in logs (it is diagnostic evidence
 * of exactly this vendor behavior).
 */
static inline unsigned long qttune_trusted_count(unsigned long status,
                                                 unsigned long reported)
{
    switch (status) {
    case QTTUNE_PT_STATUS_NOERROR:
    case QTTUNE_PT_STATUS_TIMEOUT:
        return reported;
    default:
        return 0;
    }
}

/* ---- ProtocolIDs (J2534-1 v04.04, VERIFIED) ----
 * 0x05 is raw CAN; ISO 15765 (ISO-TP) is 0x06. Connecting with 0x05
 * opens an unfiltered raw-CAN channel instead. */
#define QTTUNE_PT_PROTOCOL_CAN      0x05UL
#define QTTUNE_PT_PROTOCOL_ISO15765 0x06UL
#define QTTUNE_PT_BAUD_ISO15765_500K 500000UL

/* ---- RxStatus bits (J2534-1 v04.04, VERIFIED) ---- */
#define QTTUNE_PT_TX_MSG_TYPE           0x0001UL
#define QTTUNE_PT_START_OF_MESSAGE      0x0002UL
#define QTTUNE_PT_RX_BREAK              0x0004UL
#define QTTUNE_PT_TX_INDICATION         0x0008UL
#define QTTUNE_PT_ISO15765_PADDING_ERR  0x0010UL
#define QTTUNE_PT_ISO15765_ADDR_TYPE    0x0080UL
#define QTTUNE_PT_CAN_29BIT_ID          0x0100UL

/* TxFlags (J2534-1 v04.04, VERIFIED). On ISO 15765 channels the
 * adapter does the ISO-TP framing; FRAME_PAD asks it to zero-pad each
 * CAN frame to 8 bytes. */
#define QTTUNE_PT_TX_ISO15765_FRAME_PAD 0x0040UL
#define QTTUNE_PT_TX_NORMAL_TRANSMIT 0x0000UL
/* TxFlags CAN_29BIT_ID shares the RxStatus value (0x0100). */

/* ---- Message filters (J2534-1 v04.04, VERIFIED) ---- */
#define QTTUNE_PT_PASS_FILTER          0x00000001UL
#define QTTUNE_PT_BLOCK_FILTER         0x00000002UL
#define QTTUNE_PT_FLOW_CONTROL_FILTER  0x00000003UL
/* Filter patterns ride in PASSTHRU_MSG.Data (big-endian CAN ID in
 * Data[0..3], DataSize=4 for ISO15765); spec max pattern length: */
#define QTTUNE_PT_FILTER_PATTERN_MAX  12

/* ---- Ioctl IDs (J2534-1 v04.04, VERIFIED) ---- */
#define QTTUNE_PT_IOCTL_GET_CONFIG         0x01UL  /* SCONFIG_LIST  NULL       */
#define QTTUNE_PT_IOCTL_SET_CONFIG         0x02UL  /* SCONFIG_LIST  NULL       */
#define QTTUNE_PT_IOCTL_READ_VBATT        0x03UL  /* NULL          ULONG      */
#define QTTUNE_PT_IOCTL_CLEAR_TX_BUFFER   0x07UL  /* NULL          NULL       */
#define QTTUNE_PT_IOCTL_CLEAR_RX_BUFFER    0x08UL  /* NULL          NULL       */
#define QTTUNE_PT_IOCTL_CLEAR_MSG_FILTERS 0x0AUL  /* NULL          NULL       */

/* ---- Ioctl SCONFIG structures (J2534-1 v04.04, VERIFIED) ----
 * NOTE: SCONFIG_LIST carries a POINTER (ConfigPtr). The vendor-facing
 * form below is only legal in the process that calls the vendor DLL
 * (in-proc loader, surrogate child). The surrogate pipe protocol must
 * carry a FLATTENED form (bounded param/value array) — never this
 * struct, whose pointer is meaningless across the boundary. */
#define QTTUNE_PT_SCONFIG_MAX_PARAMS 8

typedef struct qttune_pt_sconfig {
    unsigned long Parameter;   /* config parameter name (e.g. DATA_RATE) */
    unsigned long Value;      /* parameter value */
} SCONFIG;

typedef struct qttune_pt_sconfig_list {
    unsigned long NumOfParams;
    SCONFIG* ConfigPtr;
} SCONFIG_LIST;

/* ---- The message struct (J2534-1 v04.04, VERIFIED) ----
 * Field order is the spec's (DataSize before ExtraDataIndex). The
 * loader, surrogate and fake all compile from this header, so the
 * tests cannot catch a divergence from the spec: verify any change
 * against public references. */
typedef struct qttune_pt_passthru_msg {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;       /* microseconds (spec v4.04; vendor
                                    * conformance is a BENCH check) */
    unsigned long DataSize;        /* payload bytes in Data[] */
    unsigned long ExtraDataIndex;  /* start of extra data (CRC, etc.);
                                    * == DataSize when none */
    unsigned char  Data[QTTUNE_PT_MSG_DATA_SIZE];
} PASSTHRU_MSG;

#endif /* QTTUNE_J2534_PTYPES_H */
