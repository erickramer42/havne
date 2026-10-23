// File/layer: core/include/ecuio/signals.h | C ABI, public
// Testing: UNIT — validator edge cases, mux range checks, BE vs LE comparison

#ifndef ECUIO_SIGNALS_H
#define ECUIO_SIGNALS_H

/* ecuio_def_applies_to_frame needs the complete ecuio_frame_t, so
 * include core.h (which never includes this header — no cycle). A
 * forward declaration would clash with core.h's typedef (C2371). */
#include "ecuio/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a field's bits hold. Values are append-only. */
typedef enum {
    ECUIO_VALUE_UINT  = 0,
    ECUIO_VALUE_INT   = 1,   /* two's complement, sign-extended from bit_length */
    ECUIO_VALUE_FLOAT = 2,   /* IEEE-754 binary32 */
    ECUIO_VALUE_TEXT  = 3    /* reserved: no decoder yet */
} ecuio_value_kind_t;

/* A field's byte order. Values are append-only. */
#define ECUIO_BYTE_ORDER_LE 0u   /* Intel: LSB first, bit fields at any offset */
#define ECUIO_BYTE_ORDER_BE 1u   /* Motorola: MSB first, whole bytes only */

/* One signal, in three blocks: which frames carry it, how its bytes
 * become a value, and how the value is shown. Every byte is a named
 * field, so the compiler adds no padding. Reserved bytes and flags
 * must be zero, so a later field can take that room and existing sets
 * still load (ADR-0016). kind and byte_order are fixed-width fields
 * because an enum's size is up to the compiler. */
typedef struct ecuio_signal_def {
    uint16_t id;
    uint16_t reserved0;           /* zero */

    /* Match. ecuio_def_applies_to_frame checks both before decode, and
     * ecuio_decode_signal checks the key again. */
    uint32_t message_id;          /* the frame's arbitration_id must equal it */
    uint8_t  mux_offset;          /* payload byte where the key starts */
    uint8_t  mux_length;          /* 0 = no mux; 1 to 3 = key bytes */
    uint8_t  mux_reserved[2];     /* zero */
    uint32_t mux_key;             /* key bytes, first byte most significant,
                                     whatever byte_order says */

    /* Decode */
    uint8_t  kind;                /* ecuio_value_kind_t */
    uint8_t  byte_order;          /* ECUIO_BYTE_ORDER_* */
    uint16_t start_byte;
    uint8_t  bit_offset;          /* 0..7; 0 for BE and FLOAT */
    uint8_t  bit_length;          /* 1..32; 32 for FLOAT; whole bytes for BE */
    uint8_t  decode_reserved[2];  /* zero */
    double   scale;
    double   offset;              /* value = raw * scale + offset, in double */

    /* Display. UIs show the strings as they are. */
    char     name[32];            /* UTF-8, terminated inside the buffer */
    char     unit[16];            /* UTF-8 (a degree sign, "km/h"), terminated */
    uint8_t  decimal_places;      /* 0..3 */
    uint8_t  display_reserved[3]; /* zero */

    uint32_t flags;               /* none defined; zero */
    uint32_t reserved[6];         /* zero; ADR-0016 earmarks this room */
} ecuio_signal_def_t;

/* Compile-time ABI pinning, as for ecuio_frame_t in core.h; C99 falls
 * back to tests/test_c_abi.c. A layout change bumps
 * ECUIO_SIGNAL_SET_LAYOUT. */
#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_def_t) == 120, "ecuio_signal_def_t size is ABI");
static_assert(offsetof(ecuio_signal_def_t, kind) == 16, "def kind offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, start_byte) == 18, "def start_byte offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, scale) == 24, "def scale offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, name) == 40, "def name offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, flags) == 92, "def flags offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, reserved) == 96, "def reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_def_t) == 120, "ecuio_signal_def_t size is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, kind) == 16, "def kind offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, start_byte) == 18, "def start_byte offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, scale) == 24, "def scale offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, name) == 40, "def name offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, flags) == 92, "def flags offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, reserved) == 96, "def reserved offset is ABI");
#endif

/* ADR-0013 §4: profile-declared channels. rx_id is the
 * flow-control filter target (receive); tx_id is the request
 * destination. Sets declaring none (raw/mock) keep channel_count
 * 0 — no filters, no requests, exactly today's behavior. This is
 * the seed of the v0.5.0 plugin filter surface: bounded, validated
 * at registration, consumed by the j2534 transport only. */
#define ECUIO_MAX_CHANNELS 4

typedef struct ecuio_channel_decl {
    uint32_t rx_id;   /* flow-control filter target */
    uint32_t tx_id;   /* request destination */
} ecuio_channel_decl_t;

/* The set layout this core reads. ecuio_validate_signal_set refuses
 * any other layout_version; the next incompatible layout bumps it. */
#define ECUIO_SIGNAL_SET_LAYOUT 3u

/* Fill a set member by member from a zeroed struct, never
 * positionally: reserved0 sits between channel_count and channels. */
typedef struct ecuio_signal_set {
    uint32_t layout_version;      /* ECUIO_SIGNAL_SET_LAYOUT; the first member, always */
    uint32_t count;
    const ecuio_signal_def_t* defs;
    uint32_t channel_count;       /* 0 with NULL channels: none declared */
    uint32_t reserved0;           /* zero */
    const ecuio_channel_decl_t* channels;
    uint32_t reserved[4];         /* zero */
} ecuio_signal_set_t;

/* ecuio_decode_signal return codes. Values are append-only. */
#define ECUIO_DECODE_OK                 0
#define ECUIO_DECODE_ERR_NULL           1  /* a NULL argument */
#define ECUIO_DECODE_ERR_PAST_PAYLOAD   2  /* the field or the mux key
                                              ends past payload_len */
#define ECUIO_DECODE_ERR_BAD_PARAMS     3  /* bit parameters (a big-endian
                                              field not whole bytes, too)
                                              or mux length out of range */
#define ECUIO_DECODE_ERR_UNSUPPORTED    4  /* a kind or byte order with no
                                              decoder, or a FLOAT not 32
                                              bits at bit 0 */
#define ECUIO_DECODE_ERR_MUX_MISMATCH   5  /* the payload's mux key differs */

/* Extract one signal from a payload, scale and offset applied. Checks
 * the kind and byte order before the bit parameters, then the bounds,
 * then the mux key. *out_value is written only on ECUIO_DECODE_OK. */
int ecuio_decode_signal(const ecuio_signal_def_t* signal,
                        const uint8_t* frame_payload,
                        uint16_t payload_len,
                        double* out_value);

/* ecuio_validate_signal and ecuio_validate_signal_set return codes.
 * Values are append-only. Per-def codes are positive; the set-level
 * codes are negative, so a set's result says which kind failed. */
#define ECUIO_VALIDATE_OK                     0
#define ECUIO_VALIDATE_ERR_NULL               1  /* a NULL def or set, or
                                                    count > 0 with NULL
                                                    defs */
/* 2 and 5 are retired, and never reused. */
#define ECUIO_VALIDATE_ERR_BIT_LENGTH         3  /* 0, or over 32 */
#define ECUIO_VALIDATE_ERR_BIT_OFFSET         4  /* over 7 */
#define ECUIO_VALIDATE_ERR_NAME               6  /* name not terminated
                                                    inside its buffer */
#define ECUIO_VALIDATE_ERR_UNIT               7  /* unit not terminated */
#define ECUIO_VALIDATE_ERR_DECIMALS           8  /* decimal_places over 3 */
#define ECUIO_VALIDATE_ERR_MUX_LENGTH         9  /* mux_length over 3 */
#define ECUIO_VALIDATE_ERR_MUX_RANGE         10  /* key region past the
                                                    64-byte frame payload */
#define ECUIO_VALIDATE_ERR_MUX_KEY           11  /* key wider than its
                                                    length */
#define ECUIO_VALIDATE_ERR_BIG_ENDIAN        12  /* big-endian field not
                                                    whole bytes */
#define ECUIO_VALIDATE_ERR_KIND              13  /* TEXT or an unknown
                                                    kind */
#define ECUIO_VALIDATE_ERR_FLOAT32           14  /* a FLOAT not 32 bits
                                                    at bit 0 */
#define ECUIO_VALIDATE_ERR_RESERVED          15  /* a reserved byte, or
                                                    flags, not zero */
#define ECUIO_VALIDATE_ERR_BYTE_ORDER        16  /* an unknown byte_order */
#define ECUIO_VALIDATE_ERR_CHANNELS_NULL    (-10)  /* channel_count > 0 with
                                                      NULL channels */
#define ECUIO_VALIDATE_ERR_CHANNEL_COUNT    (-11)  /* over ECUIO_MAX_CHANNELS */
#define ECUIO_VALIDATE_ERR_CHANNEL_ID       (-12)  /* rx or tx 0, or past
                                                      0x1FFFFFFF */
#define ECUIO_VALIDATE_ERR_DUPLICATE_RX     (-13)  /* two channels share an rx */
#define ECUIO_VALIDATE_ERR_DUPLICATE_ID     (-14)  /* two defs share an id */
#define ECUIO_VALIDATE_ERR_LAYOUT           (-15)  /* layout_version isn't
                                                      ECUIO_SIGNAL_SET_LAYOUT */
#define ECUIO_VALIDATE_ERR_SET_RESERVED     (-16)  /* a reserved field of
                                                      the set not zero */
#define ECUIO_VALIDATE_ERR_REQUEST_COUNT    (-17)  /* the defs derive more
                                                      requests than
                                                      ECUIO_PROFILE_MAX_REQUESTS
                                                      (profile.h); reported
                                                      by ecuio_profile_register */

/* Why a set failed validation, and where. index names the element the
 * code is about: the failing def; for a duplicate id, the second def
 * of the pair; for a bad channel id, the channel; for a duplicate rx,
 * the second channel of the pair; for too many requests, the first def
 * whose request didn't fit. It is UINT32_MAX for
 * ECUIO_VALIDATE_OK and for a code that names no single element: a
 * NULL, the layout version, the set's reserved fields, the channel
 * count. */
typedef struct ecuio_validation {
    int32_t  code;                /* ECUIO_VALIDATE_*; 0 = OK */
    uint32_t index;               /* the def or channel, or UINT32_MAX */
    uint32_t reserved[2];         /* written as zero */
} ecuio_validation_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_validation_t) == 16, "ecuio_validation_t size is ABI");
static_assert(offsetof(ecuio_validation_t, index) == 4, "validation index offset is ABI");
static_assert(offsetof(ecuio_validation_t, reserved) == 8, "validation reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_validation_t) == 16, "ecuio_validation_t size is ABI");
_Static_assert(offsetof(ecuio_validation_t, index) == 4, "validation index offset is ABI");
_Static_assert(offsetof(ecuio_validation_t, reserved) == 8, "validation reserved offset is ABI");
#endif

/* Structural check of one def: accepts exactly the defs
 * ecuio_decode_signal can decode, with every reserved byte and flags
 * zero. The kind is checked first, then the byte order. */
int ecuio_validate_signal(const ecuio_signal_def_t* signal);

/* Frame gate, called BEFORE ecuio_decode_signal (the bridge's decode
 * loop): returns 1 when the def applies to the frame (its message_id
 * is the frame's arbitration ID and, with mux on, the frame carries
 * its mux key), and 0 otherwise or for a NULL argument. Saves the
 * extraction for frames the def can't read. */
int ecuio_def_applies_to_frame(const ecuio_signal_def_t* signal,
                               const ecuio_frame_t* frame);

/* Set-level APIs */
const ecuio_signal_set_t* ecuio_mock_signal_set(void);
const ecuio_signal_set_t* ecuio_obd2_signal_set(void); /* ADR-0013 obd2 profile */

/* The layout version, before any other member is read; the set's
 * reserved fields; every def (ecuio_validate_signal; the first failing
 * def's code); unique def ids; then the channel declarations. A set
 * with count 0 may leave defs NULL. Returns ECUIO_VALIDATE_OK or a
 * ECUIO_VALIDATE_ERR_* code. When out isn't NULL, it receives the same
 * code and the index of the element at fault; a caller that only needs
 * pass or fail passes NULL. */
int ecuio_validate_signal_set(const ecuio_signal_set_t* set,
                              ecuio_validation_t* out);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIGNALS_H */
