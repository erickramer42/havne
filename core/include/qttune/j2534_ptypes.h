// File/layer: core/include/qttune/j2534_ptypes.h | INTERNAL shared types
// Testing: structural — consumed identically by loader and fake DLL
//
// Single source of truth for the PASSTHRU contract between
// core/src/j2534_loader.cpp and tests/fake_j2534.cpp. Because both
// compile from THIS header, layout drift is structurally impossible.
// NOT public ABI — public consumers use qttune/j2534.h only.
//
// VERIFICATION CAVEAT (BENCH tier): field order mirrors the SAE
// J2534-1 documented PASSTHRU_MSG layout from training knowledge.
// Before talking to a REAL vendor DLL, diff against the vendor's
// j2534.h. Our fake accepts whatever this header says, so nothing
// today can catch a divergence — only a live device can.

#ifndef QTTUNE_J2534_PTYPES_H
#define QTTUNE_J2534_PTYPES_H

/* Largest single-protocol-message payload (ISO 15765 multiframe
 * reassembly buffer) per J2534-1 guidance. */
#define QTTUNE_PT_MSG_DATA_SIZE 4128

/* Status codes (subset used by qttune; only NOERROR's value is
 * load-bearing today — the loader treats any nonzero return as
 * "no frame available / call failed" uniformly. */
#define QTTUNE_PT_STATUS_NOERROR  0L
#define QTTUNE_PT_STATUS_FAILED   1L   /* internal use: fake rejects */

/* Protocol constants (value per J2534-1 from training knowledge —
 * verify against vendor header before BENCH work). */
#define QTTUNE_PT_PROTOCOL_ISO15765  0x05UL
#define QTTUNE_PT_BAUD_ISO15765_500K 500000UL

typedef struct qttune_pt_passthru_msg {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;       /* units: vendor-defined; v0.3.0
                                    * passes through untranslated */
    unsigned long ExtraDataIndex;
    unsigned long DataSize;
    unsigned char  Data[QTTUNE_PT_MSG_DATA_SIZE];
} PASSTHRU_MSG;

#endif /* QTTUNE_J2534_PTYPES_H */
