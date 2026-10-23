// File/layer: core/include/ecuio/j2534_ptypes.h | INTERNAL shared types
// Testing: structural — consumed identically by loader, surrogate, fake
//
// Single source of truth for the PASSTHRU contract between
// core/src/j2534_loader.cpp, surrogate/surrogate_main.cpp, and
// tests/fake_j2534.cpp. Because everyone compiles from THIS header,
// layout drift between them is structurally impossible.
//
// NOT public ABI — public consumers use ecuio/j2534.h only.

#ifndef ECUIO_J2534_PTYPES_H
#define ECUIO_J2534_PTYPES_H

/* Largest single-protocol-message payload (ISO 15765 multiframe
 * reassembly buffer) per J2534-1 v4.04. VERIFIED. */
#define ECUIO_PT_MSG_DATA_SIZE 4128

/* ---- Status codes (J2534-1 v04.04, VERIFIED) ----
 * The subset the core uses. TIMEOUT and BUFFER_EMPTY are not errors on
 * a read (quiet bus / empty queue); every other nonzero status is a
 * hard failure. Vendor statuses are mapped to ecuio_status_t, never
 * returned as one. Verified against FreeSSM J2534.h and diamondman
 * J2534_v0404.h. */
#define ECUIO_PT_STATUS_NOERROR     0L
#define ECUIO_PT_STATUS_NOT_SUPPORTED 0x01L
#define ECUIO_PT_STATUS_FAILED      0x07L
#define ECUIO_PT_STATUS_TIMEOUT     0x09L
#define ECUIO_PT_STATUS_BUFFER_EMPTY 0x10L

/* J2534-1 v04.04's name for a status, or NULL for one it doesn't
 * define: a vendor's own (the OBDX Pro VT and VX refuse to open with 34
 * when no adapter is attached). The values match the j2534-rs binding's
 * table (github.com/jcmnn/j2534-rs, src/lib.rs) and the four codes above. */
static inline const char* ecuio_pt_status_name(long status)
{
    switch (status) {
    case 0x00: return "STATUS_NOERROR";
    case 0x01: return "ERR_NOT_SUPPORTED";
    case 0x02: return "ERR_INVALID_CHANNEL_ID";
    case 0x03: return "ERR_INVALID_PROTOCOL_ID";
    case 0x04: return "ERR_NULL_PARAMETER";
    case 0x05: return "ERR_INVALID_IOCTL_VALUE";
    case 0x06: return "ERR_INVALID_FLAGS";
    case 0x07: return "ERR_FAILED";
    case 0x08: return "ERR_DEVICE_NOT_CONNECTED";
    case 0x09: return "ERR_TIMEOUT";
    case 0x0A: return "ERR_INVALID_MSG";
    case 0x0B: return "ERR_INVALID_TIME_INTERVAL";
    case 0x0C: return "ERR_EXCEEDED_LIMIT";
    case 0x0D: return "ERR_INVALID_MSG_ID";
    case 0x0E: return "ERR_DEVICE_IN_USE";
    case 0x0F: return "ERR_INVALID_IOCTL_ID";
    case 0x10: return "ERR_BUFFER_EMPTY";
    case 0x11: return "ERR_BUFFER_FULL";
    case 0x12: return "ERR_BUFFER_OVERFLOW";
    case 0x13: return "ERR_PIN_INVALID";
    case 0x14: return "ERR_CHANNEL_IN_USE";
    case 0x15: return "ERR_MSG_PROTOCOL_ID";
    case 0x16: return "ERR_INVALID_FILTER_ID";
    case 0x17: return "ERR_NO_FLOW_CONTROL";
    case 0x18: return "ERR_NOT_UNIQUE";
    case 0x19: return "ERR_INVALID_BAUDRATE";
    case 0x1A: return "ERR_INVALID_DEVICE_ID";
    default:   return 0;
    }
}

/* PassThruGetLastError fills a buffer of at least this many characters
 * (J2534-1 v04.04; the j2534-rs binding allocates the same). */
#define ECUIO_PT_TEXT_LEN 80

/* Trust rule for vendor-reported message counts.
 *
 * Some vendors leave pNumMsgs (and the caller's message buffer)
 * untouched when returning an error, so a nonzero count under a hard
 * error is not evidence any message was written. The OBDX Pro VX does
 * this: a failed write reports "status 7, written 1"
 * (docs/bench-sessions/2026-10-20.md). Trusting such a count on a read
 * would dispatch stale or zeroed frames as live data and keep the link
 * looking healthy. Only these statuses carry a count the caller may
 * trust:
 *
 *   STATUS_NOERROR       - all requested messages read
 *   ERR_TIMEOUT          - partial batch already copied (J2534-1)
 *
 * Every other status (ERR_FAILED, ERR_BUFFER_EMPTY, device errors,
 * ...) means "assume nothing was written". ERR_BUFFER_OVERFLOW (0x12)
 * would qualify per spec (pNumMsgs = messages actually copied) but is
 * not defined here, so an overflow read drops its batch and counts as
 * a hard failure. Applies to ReadMsgs; both write paths check
 * the status before the count, the count is never consumed, and the
 * surrogate logs the raw pair (it is diagnostic evidence of exactly
 * this vendor behavior).
 */
static inline unsigned long ecuio_trusted_count(unsigned long status,
                                                unsigned long reported)
{
    switch (status) {
    case ECUIO_PT_STATUS_NOERROR:
    case ECUIO_PT_STATUS_TIMEOUT:
        return reported;
    default:
        return 0;
    }
}

/* ---- ProtocolIDs (J2534-1 v04.04, VERIFIED) ----
 * 0x05 is raw CAN; ISO 15765 (ISO-TP) is 0x06. Connecting with 0x05
 * opens an unfiltered raw-CAN channel instead. */
#define ECUIO_PT_PROTOCOL_CAN       0x05UL
#define ECUIO_PT_PROTOCOL_ISO15765 0x06UL
#define ECUIO_PT_BAUD_ISO15765_500K 500000UL

/* ---- RxStatus bits (J2534-1 v04.04, VERIFIED) ---- */
#define ECUIO_PT_TX_MSG_TYPE            0x0001UL
#define ECUIO_PT_START_OF_MESSAGE       0x0002UL
#define ECUIO_PT_RX_BREAK               0x0004UL
#define ECUIO_PT_TX_INDICATION          0x0008UL
#define ECUIO_PT_ISO15765_PADDING_ERR   0x0010UL
#define ECUIO_PT_ISO15765_ADDR_TYPE     0x0080UL
#define ECUIO_PT_CAN_29BIT_ID           0x0100UL

/* Messages a read skips, because none is a received frame: a TX
 * indication or loopback echo (TX_MSG_TYPE or TX_INDICATION; the OBDX
 * Pro VX queues 0x09 after each accepted write and 0x201 after a
 * refused one), and the START_OF_MESSAGE indication a vendor queues
 * when a segmented ISO 15765 reply starts arriving, ahead of the whole
 * message (Quantex's PassThruReadMsgs reference). Skipping one doesn't
 * end a read: both read paths read again with what's left of the
 * timeout, skipping at most ECUIO_PT_MAX_SKIPPED_PER_READ. */
#define ECUIO_PT_MAX_SKIPPED_PER_READ 16

static inline int ecuio_pt_is_indication(unsigned long rx_status)
{
    return (rx_status & (ECUIO_PT_TX_MSG_TYPE | ECUIO_PT_TX_INDICATION |
                         ECUIO_PT_START_OF_MESSAGE)) != 0;
}

/* TxFlags (J2534-1 v04.04, VERIFIED). On ISO 15765 channels the
 * adapter does the ISO-TP framing; FRAME_PAD asks it to zero-pad each
 * CAN frame to 8 bytes. */
#define ECUIO_PT_TX_ISO15765_FRAME_PAD 0x0040UL
#define ECUIO_PT_TX_NORMAL_TRANSMIT 0x0000UL
/* TxFlags CAN_29BIT_ID shares the RxStatus value (0x0100). */

/* ---- Message filters (J2534-1 v04.04, VERIFIED) ---- */
#define ECUIO_PT_PASS_FILTER           0x00000001UL
#define ECUIO_PT_BLOCK_FILTER          0x00000002UL
#define ECUIO_PT_FLOW_CONTROL_FILTER   0x00000003UL
/* Filter patterns ride in PASSTHRU_MSG.Data (big-endian CAN ID in
 * Data[0..3], DataSize=4 for ISO15765); spec max pattern length: */
#define ECUIO_PT_FILTER_PATTERN_MAX   12

/* ---- Ioctl IDs (J2534-1 v04.04, VERIFIED) ---- */
#define ECUIO_PT_IOCTL_GET_CONFIG          0x01UL  /* SCONFIG_LIST  NULL       */
#define ECUIO_PT_IOCTL_SET_CONFIG          0x02UL  /* SCONFIG_LIST  NULL       */
#define ECUIO_PT_IOCTL_READ_VBATT         0x03UL  /* NULL          ULONG      */
#define ECUIO_PT_IOCTL_CLEAR_TX_BUFFER    0x07UL  /* NULL          NULL       */
#define ECUIO_PT_IOCTL_CLEAR_RX_BUFFER     0x08UL  /* NULL          NULL       */
#define ECUIO_PT_IOCTL_CLEAR_MSG_FILTERS 0x0AUL   /* NULL          NULL       */

/* ---- Ioctl SCONFIG structures (J2534-1 v04.04, VERIFIED) ----
 * NOTE: SCONFIG_LIST carries a POINTER (ConfigPtr). The vendor-facing
 * form below is only legal in the process that calls the vendor DLL
 * (in-proc loader, surrogate child). The surrogate pipe protocol must
 * carry a FLATTENED form (bounded param/value array) — never this
 * struct, whose pointer is meaningless across the boundary. */
#define ECUIO_PT_SCONFIG_MAX_PARAMS 8

typedef struct ecuio_pt_sconfig {
    unsigned long Parameter;   /* config parameter name (e.g. DATA_RATE) */
    unsigned long Value;      /* parameter value */
} SCONFIG;

typedef struct ecuio_pt_sconfig_list {
    unsigned long NumOfParams;
    SCONFIG* ConfigPtr;
} SCONFIG_LIST;

/* ---- The message struct (J2534-1 v04.04, VERIFIED) ----
 * Field order is the spec's (DataSize before ExtraDataIndex). The
 * loader, surrogate and fake all compile from this header, so the
 * tests cannot catch a divergence from the spec: verify any change
 * against public references. */
typedef struct ecuio_pt_passthru_msg {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;       /* microseconds (spec v4.04; vendor
                                    * conformance is a BENCH check) */
    unsigned long DataSize;        /* payload bytes in Data[] */
    unsigned long ExtraDataIndex;  /* start of extra data (CRC, etc.);
                                    * == DataSize when none */
    unsigned char  Data[ECUIO_PT_MSG_DATA_SIZE];
} PASSTHRU_MSG;

#endif /* ECUIO_J2534_PTYPES_H */
