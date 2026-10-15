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

/* ---- Status codes (J2534-1 v04.04) ----
 * Subset used by qttune; only NOERROR is load-bearing today — the
 * loader treats any nonzero return as "call failed / no frame." */
#define QTTUNE_PT_STATUS_NOERROR  0L
#define QTTUNE_PT_STATUS_FAILED   1L   /* ERR_FAILED */

/* ---- ProtocolIDs (J2534-1 v04.04, VERIFIED) ----
 * FIX 2026-10-15: ISO15765 is 0x06. The previous value 0x05 is CAN
 * (raw) — connecting with it on a real device opens an unfiltered
 * raw-CAN channel instead of an ISO-TP channel. */
#define QTTUNE_PT_PROTOCOL_CAN      0x05UL
#define QTTUNE_PT_PROTOCOL_ISO15765 0x06UL
#define QTTUNE_PT_BAUD_ISO15765_500K 500000UL

/* ---- RxStatus bits (J2534-1 v04.04, VERIFIED) ----
 * FIX 2026-10-15: CAN_29BIT_ID is bit 8 (0x0100), not 0x01000000.
 * Previously masked with a value no conforming vendor ever sets —
 * masked cleanly by the fake compiling from the same header. */
#define QTTUNE_PT_TX_MSG_TYPE           0x0001UL
#define QTTUNE_PT_START_OF_MESSAGE      0x0002UL
#define QTTUNE_PT_RX_BREAK              0x0004UL
#define QTTUNE_PT_TX_INDICATION         0x0008UL
#define QTTUNE_PT_ISO15765_PADDING_ERR  0x0010UL
#define QTTUNE_PT_ISO15765_ADDR_TYPE    0x0080UL
#define QTTUNE_PT_CAN_29BIT_ID          0x0100UL

/* TxFlags (J2534-1 v04.04, VERIFIED) — Part 4 write path needs these. */
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
 * struct, whose pointer is meaningless across the boundary. The
 * flattened wire type lives in surrogate_protocol.h (Part 4). */
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
 * FIX 2026-10-15: field order was Timestamp, ExtraDataIndex, DataSize
 * — the last two were SWAPPED vs. the spec. All in-tree access is by
 * field name and every consumer compiles from this header, so the fix
 * is invisible to the test suite by construction; against a real
 * vendor DLL the old order read ExtraDataIndex as DataSize (equal
 * values on unpadded single frames — silent corruption only on
 * padded/multiframe, and wrong tx lengths for Part 4 writes). */
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
