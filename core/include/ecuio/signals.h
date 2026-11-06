// File/layer: core/include/ecuio/signals.h | C ABI, public
// Testing: UNIT: validator edge cases, mux range checks, BE vs LE comparison

#ifndef ECUIO_SIGNALS_H
#define ECUIO_SIGNALS_H

/* ecuio_def_applies_to_frame needs the complete ecuio_frame_t, so
 * include core.h (which never includes this header: no cycle). A
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
    /* The text kinds, which ecuio_decode_text reads from a byte field
     * (start_byte and byte_length; bit_offset and bit_length 0): */
    ECUIO_VALUE_TEXT  = 3,   /* the bytes as text: printable ASCII as it
                                is, any other byte as '.' */
    ECUIO_VALUE_HEX   = 4,   /* the bytes as hex pairs: "41 0C 1F 40" */
    /* A raw number shown as a word (ADR-0051): read as UINT, scale 1
     * and offset 0, its labels in the set's enum table */
    ECUIO_VALUE_ENUM  = 5
} ecuio_value_kind_t;

/* A def's flags. Values are append-only; the validator refuses any
 * other bit. */
#define ECUIO_DEF_FLAG_COMPUTED 0x1u  /* the value is a program over other
                                         defs' values (ADR-0051 section
                                         5): message_id 0, no mux, the
                                         decode block UINT with every
                                         bit field 0, scale 1, offset 0,
                                         and table_first its program in
                                         the set's computed table */

/* A field's byte order. Values are append-only. */
#define ECUIO_BYTE_ORDER_LE 0u   /* Intel: LSB first, bit fields at any offset */
#define ECUIO_BYTE_ORDER_BE 1u   /* Motorola: MSB first, whole bytes only */

/* The part of the car a signal reads, for finding it among many
 * (ADR-0050). Values are append-only; ecuio_signal_group_name names
 * each. */
#define ECUIO_SIGNAL_GROUP_OTHER        0u  /* not placed */
#define ECUIO_SIGNAL_GROUP_ENGINE       1u
#define ECUIO_SIGNAL_GROUP_AIR          2u  /* intake, throttle, pedal */
#define ECUIO_SIGNAL_GROUP_FUEL         3u
#define ECUIO_SIGNAL_GROUP_OXYGEN       4u  /* O2 sensors */
#define ECUIO_SIGNAL_GROUP_EMISSIONS    5u  /* EGR, evap, catalyst */
#define ECUIO_SIGNAL_GROUP_VEHICLE      6u  /* road speed, distance */
#define ECUIO_SIGNAL_GROUP_ELECTRICAL   7u
#define ECUIO_SIGNAL_GROUP_DIAGNOSTICS  8u  /* the MIL, counts since a clear */

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
    uint8_t  bit_length;          /* 1..32; 32 for FLOAT; whole bytes for BE;
                                     0 for a text kind */
    uint16_t byte_length;         /* text kinds: the field's bytes from
                                     start_byte, 0 = to the end of the
                                     payload; 0 for every other kind */
    double   scale;
    double   offset;              /* value = raw * scale + offset, in double */

    /* Display. UIs show the strings as they are. */
    char     name[32];            /* UTF-8, terminated inside the buffer */
    char     unit[16];            /* UTF-8 (a degree sign, "km/h"), terminated */
    uint8_t  decimal_places;      /* 0..3 */
    uint8_t  plot_rank;           /* 0 = not plotted by default; else the
                                     def's place, 1 first, among those a
                                     UI plots when a session starts
                                     (ADR-0039 section 8) */
    uint8_t  group;               /* ECUIO_SIGNAL_GROUP_*: the part of
                                     the car it reads (ADR-0050); any
                                     value, 0 Other */
    uint8_t  display_reserved[1]; /* zero */

    uint32_t flags;               /* ECUIO_DEF_FLAG_*; any other bit zero */
    uint32_t quantity_reserved;   /* zero; ADR-0016 earmarks it for the
                                     unit's quantity */
    uint16_t table_first;         /* ENUM: its first entry in the set's
                                     enums; COMPUTED: its program in the
                                     set's computed; zero otherwise */
    uint16_t table_count;         /* ENUM: its entries, 0 to
                                     ECUIO_ENUM_MAX_ENTRIES; zero
                                     otherwise */
    uint32_t reserved[4];         /* zero; ADR-0016 earmarks this room */
} ecuio_signal_def_t;

/* Compile-time ABI pinning, as for ecuio_frame_t in core.h; C99 falls
 * back to tests/test_c_abi.c. A layout change bumps
 * ECUIO_SIGNAL_SET_LAYOUT. */
#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_def_t) == 120, "ecuio_signal_def_t size is ABI");
static_assert(offsetof(ecuio_signal_def_t, kind) == 16, "def kind offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, start_byte) == 18, "def start_byte offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, byte_length) == 22, "def byte_length offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, scale) == 24, "def scale offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, name) == 40, "def name offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, plot_rank) == 89, "def plot_rank offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, group) == 90, "def group offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, flags) == 92, "def flags offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, quantity_reserved) == 96, "def quantity_reserved offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, table_first) == 100, "def table_first offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, table_count) == 102, "def table_count offset is ABI");
static_assert(offsetof(ecuio_signal_def_t, reserved) == 104, "def reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_def_t) == 120, "ecuio_signal_def_t size is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, kind) == 16, "def kind offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, start_byte) == 18, "def start_byte offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, byte_length) == 22, "def byte_length offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, scale) == 24, "def scale offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, name) == 40, "def name offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, plot_rank) == 89, "def plot_rank offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, group) == 90, "def group offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, flags) == 92, "def flags offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, quantity_reserved) == 96, "def quantity_reserved offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, table_first) == 100, "def table_first offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, table_count) == 102, "def table_count offset is ABI");
_Static_assert(offsetof(ecuio_signal_def_t, reserved) == 104, "def reserved offset is ABI");
#endif

/* ADR-0013 section 4: profile-declared channels. rx_id is the
 * flow-control filter target (receive); tx_id is the request
 * destination. Sets declaring none (raw/mock) keep channel_count
 * 0: no filters, no requests, exactly today's behavior. This is
 * the seed of the v0.5.0 plugin filter surface: bounded, validated
 * at registration, consumed by the j2534 transport only. */
#define ECUIO_MAX_CHANNELS 4

typedef struct ecuio_channel_decl {
    uint32_t rx_id;   /* flow-control filter target */
    uint32_t tx_id;   /* request destination */
} ecuio_channel_decl_t;

/* An enum def's labels (ADR-0051): its table_count entries from
 * table_first in the set's enums, raw values strictly ascending. A raw
 * value with no entry has no label, and a UI shows its number. */
#define ECUIO_ENUM_MAX_ENTRIES 256u   /* one def's entries */
/* The most entries the enum table, and programs the computed table,
 * hold: what a def's uint16_t table_first can reach */
#define ECUIO_SET_MAX_TABLE    65536u
#define ECUIO_ENUM_LABEL_MAX   27u    /* a label's bytes, the NUL not counted */

typedef struct ecuio_enum_entry {
    uint32_t raw;                 /* the field's value */
    char     label[28];           /* UTF-8, 1 to ECUIO_ENUM_LABEL_MAX
                                     bytes, terminated inside the buffer,
                                     zero after the NUL */
} ecuio_enum_entry_t;

/* A computed def's program (ADR-0051 section 5): a stack machine over
 * up to four inputs, each another def's newest value no older than its
 * age bound, and up to 16 constants. Each step pushes an input or a
 * constant, or pops two values and pushes one; a program runs to one
 * value. The low four bits of a push step are the input's or the
 * constant's index. */
#define ECUIO_COMPUTED_MAX_INPUTS    4u
#define ECUIO_COMPUTED_MAX_CONSTANTS 16u
#define ECUIO_COMPUTED_MAX_STEPS     33u   /* 16 operations and their 17 operands */

#define ECUIO_COMPUTED_PUSH_INPUT    0x00u  /* | the input's index */
#define ECUIO_COMPUTED_PUSH_CONSTANT 0x10u  /* | the constant's index */
#define ECUIO_COMPUTED_ADD           0x20u
#define ECUIO_COMPUTED_SUBTRACT      0x21u  /* the second from the top minus the top */
#define ECUIO_COMPUTED_MULTIPLY      0x22u
#define ECUIO_COMPUTED_DIVIDE        0x23u  /* the second from the top over the top;
                                               a zero top gives no value */

/* The age past which a value is stale: the app dims it and a computed
 * input older than this, with no max_age of its own, gives no value
 * (ADR-0051 section 5). On the samples' session time. */
#define ECUIO_VALUE_STALE_US 3000000u

/* The bounds of a computed input's own age bound, max_age_us */
#define ECUIO_COMPUTED_MIN_AGE_US 100000u     /* 0.1 s */
#define ECUIO_COMPUTED_MAX_AGE_US 60000000u   /* 60 s */

typedef struct ecuio_computed {
    uint16_t input_ids[4];        /* the inputs' def ids, input_count of
                                     them; zero after */
    uint32_t max_age_us[4];       /* each input's age bound: 0 for
                                     ECUIO_VALUE_STALE_US, else
                                     ECUIO_COMPUTED_MIN_AGE_US to _MAX_;
                                     zero after input_count */
    uint8_t  input_count;         /* 1 to ECUIO_COMPUTED_MAX_INPUTS */
    uint8_t  step_count;          /* 1 to ECUIO_COMPUTED_MAX_STEPS */
    uint8_t  constant_count;      /* 0 to ECUIO_COMPUTED_MAX_CONSTANTS */
    uint8_t  reserved0;           /* zero */
    uint8_t  steps[36];           /* step_count of them; zero after */
    double   constants[16];       /* constant_count of them, finite;
                                     zero after */
    uint32_t reserved[4];         /* zero */
} ecuio_computed_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_enum_entry_t) == 32, "ecuio_enum_entry_t size is ABI");
static_assert(sizeof(ecuio_computed_t) == 208, "ecuio_computed_t size is ABI");
static_assert(offsetof(ecuio_computed_t, max_age_us) == 8, "computed max_age_us offset is ABI");
static_assert(offsetof(ecuio_computed_t, input_count) == 24, "computed input_count offset is ABI");
static_assert(offsetof(ecuio_computed_t, steps) == 28, "computed steps offset is ABI");
static_assert(offsetof(ecuio_computed_t, constants) == 64, "computed constants offset is ABI");
static_assert(offsetof(ecuio_computed_t, reserved) == 192, "computed reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_enum_entry_t) == 32, "ecuio_enum_entry_t size is ABI");
_Static_assert(sizeof(ecuio_computed_t) == 208, "ecuio_computed_t size is ABI");
_Static_assert(offsetof(ecuio_computed_t, max_age_us) == 8, "computed max_age_us offset is ABI");
_Static_assert(offsetof(ecuio_computed_t, input_count) == 24, "computed input_count offset is ABI");
_Static_assert(offsetof(ecuio_computed_t, steps) == 28, "computed steps offset is ABI");
_Static_assert(offsetof(ecuio_computed_t, constants) == 64, "computed constants offset is ABI");
_Static_assert(offsetof(ecuio_computed_t, reserved) == 192, "computed reserved offset is ABI");
#endif

/* The set layout this core reads. ecuio_validate_signal_set refuses
 * any other layout_version; the next incompatible layout bumps it. */
#define ECUIO_SIGNAL_SET_LAYOUT 4u

/* Fill a set member by member from a zeroed struct, never
 * positionally: reserved0 sits between channel_count and channels.
 * Every member is at its natural alignment on 32- and 64-bit builds,
 * so the compiler adds no padding. */
typedef struct ecuio_signal_set {
    uint32_t layout_version;      /* ECUIO_SIGNAL_SET_LAYOUT; the first member, always */
    uint32_t count;
    const ecuio_signal_def_t* defs;
    uint32_t channel_count;       /* 0 with NULL channels: none declared */
    uint32_t reserved0;           /* zero */
    const ecuio_channel_decl_t* channels;
    uint32_t enum_count;          /* 0 with NULL enums: none */
    uint32_t computed_count;      /* 0 with NULL computed: none */
    const ecuio_enum_entry_t* enums;
    const ecuio_computed_t* computed;
    uint32_t reserved[4];         /* zero */
} ecuio_signal_set_t;

/* The set's layout on either width: no padding, the pointers in place */
#if defined(__cplusplus)
static_assert(offsetof(ecuio_signal_set_t, enum_count) == 2 * sizeof(void*) + 16, "set enum_count offset is ABI");
static_assert(offsetof(ecuio_signal_set_t, enums) == 2 * sizeof(void*) + 24, "set enums offset is ABI");
static_assert(offsetof(ecuio_signal_set_t, reserved) == 4 * sizeof(void*) + 24, "set reserved offset is ABI");
static_assert(sizeof(ecuio_signal_set_t) == 4 * sizeof(void*) + 40, "ecuio_signal_set_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_signal_set_t, enum_count) == 2 * sizeof(void*) + 16, "set enum_count offset is ABI");
_Static_assert(offsetof(ecuio_signal_set_t, enums) == 2 * sizeof(void*) + 24, "set enums offset is ABI");
_Static_assert(offsetof(ecuio_signal_set_t, reserved) == 4 * sizeof(void*) + 24, "set reserved offset is ABI");
_Static_assert(sizeof(ecuio_signal_set_t) == 4 * sizeof(void*) + 40, "ecuio_signal_set_t size is ABI");
#endif

/* ecuio_decode_signal return codes. Values are append-only. */
#define ECUIO_DECODE_OK                 0
#define ECUIO_DECODE_ERR_NULL           1  /* a NULL argument */
#define ECUIO_DECODE_ERR_PAST_PAYLOAD   2  /* the field or the mux key
                                              ends past payload_len */
#define ECUIO_DECODE_ERR_BAD_PARAMS     3  /* bit parameters (a big-endian
                                              field not whole bytes, too)
                                              or mux length out of range */
#define ECUIO_DECODE_ERR_UNSUPPORTED    4  /* a kind or byte order with no
                                              decoder, a FLOAT not 32 bits
                                              at bit 0, a text kind given
                                              to ecuio_decode_signal or a
                                              numeric one to
                                              ecuio_decode_text */
#define ECUIO_DECODE_ERR_MUX_MISMATCH   5  /* the payload's mux key differs */

/* Extract one signal from a payload, scale and offset applied; an
 * ENUM reads as a UINT, and a COMPUTED def, which no frame carries, is
 * ECUIO_DECODE_ERR_UNSUPPORTED. Checks the kind and byte order before
 * the bit parameters, then the bounds, then the mux key. *out_value is
 * written only on ECUIO_DECODE_OK. */
int ecuio_decode_signal(const ecuio_signal_def_t* signal,
                        const uint8_t* frame_payload,
                        uint16_t payload_len,
                        double* out_value);

/* A buffer this long holds any text a frame can decode to: the hex of
 * a whole payload, three characters a byte, the last pair's separator
 * giving way to the NUL. */
#define ECUIO_DECODE_TEXT_MAX (3u * ECUIO_FRAME_DATA_MAX)

/* Extract one text signal (ECUIO_VALUE_TEXT or ECUIO_VALUE_HEX): the
 * field's bytes from start_byte, byte_length of them or to the end of
 * the payload, as that kind's text, NUL-terminated in out. Checks the
 * kind and byte order, then bit_offset, bit_length and byte_length,
 * then the bounds, then the mux key, like ecuio_decode_signal. A field
 * that starts at or past the end of the payload, or runs past it, is
 * ECUIO_DECODE_ERR_PAST_PAYLOAD. Text longer than out_cap - 1 is cut
 * to fit, at a whole hex pair; out is written only on
 * ECUIO_DECODE_OK. */
int ecuio_decode_text(const ecuio_signal_def_t* signal,
                      const uint8_t* frame_payload,
                      uint16_t payload_len,
                      char* out,
                      uint32_t out_cap);

/* An ENUM def's label for a raw value, from the set's enums: NULL for
 * a raw value with no entry (a UI shows its number), a def that isn't
 * an ENUM, or a NULL argument. The def must be one of the set's, and
 * the set valid. */
const char* ecuio_enum_label(const ecuio_signal_set_t* set,
                             const ecuio_signal_def_t* signal,
                             uint32_t raw);

/* ecuio_validate_signal and ecuio_validate_signal_set return codes.
 * Values are append-only. Per-def codes are positive; the set-level
 * codes are negative, so a set's result says which kind failed. */
#define ECUIO_VALIDATE_OK                     0
#define ECUIO_VALIDATE_ERR_NULL               1  /* a NULL def or set, or
                                                    count > 0 with NULL
                                                    defs */
/* 2, 5 and 10 are retired, and never reused. 10 was a mux key past a
 * 64-byte payload; every key a mux_offset can place fits the frame. */
#define ECUIO_VALIDATE_ERR_BIT_LENGTH         3  /* 0, or over 32; a text
                                                    kind's not 0 */
#define ECUIO_VALIDATE_ERR_BIT_OFFSET         4  /* over 7; a text kind's
                                                    not 0 */
#define ECUIO_VALIDATE_ERR_NAME               6  /* name not terminated
                                                    inside its buffer */
#define ECUIO_VALIDATE_ERR_UNIT               7  /* unit not terminated */
#define ECUIO_VALIDATE_ERR_DECIMALS           8  /* decimal_places over 3 */
#define ECUIO_VALIDATE_ERR_MUX_LENGTH         9  /* mux_length over 3 */
#define ECUIO_VALIDATE_ERR_MUX_KEY           11  /* key wider than its
                                                    length */
#define ECUIO_VALIDATE_ERR_BIG_ENDIAN        12  /* big-endian field not
                                                    whole bytes */
#define ECUIO_VALIDATE_ERR_KIND              13  /* an unknown kind */
#define ECUIO_VALIDATE_ERR_FLOAT32           14  /* a FLOAT not 32 bits
                                                    at bit 0 */
#define ECUIO_VALIDATE_ERR_RESERVED          15  /* a reserved byte, or
                                                    flags, not zero */
#define ECUIO_VALIDATE_ERR_BYTE_ORDER        16  /* an unknown byte_order */
#define ECUIO_VALIDATE_ERR_BYTE_LENGTH       17  /* byte_length on a numeric
                                                    kind, or over
                                                    ECUIO_FRAME_DATA_MAX */
#define ECUIO_VALIDATE_ERR_FORMULA           18  /* name or unit starts with
                                                    '=', '+', '-', '@', a tab
                                                    or a CR, which a
                                                    spreadsheet runs as a
                                                    formula (ADR-0049) */
#define ECUIO_VALIDATE_ERR_ENUM              19  /* an ENUM def's scale not
                                                    1 or offset not 0, or
                                                    table_count over
                                                    ECUIO_ENUM_MAX_ENTRIES */
#define ECUIO_VALIDATE_ERR_COMPUTED          20  /* a COMPUTED def with a
                                                    message_id, a mux, or a
                                                    decode block other than
                                                    UINT, zeros, scale 1
                                                    and offset 0; or a
                                                    table_count */
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
#define ECUIO_VALIDATE_ERR_TABLES_NULL      (-18)  /* enum_count or
                                                      computed_count > 0
                                                      with its table NULL,
                                                      or either over
                                                      ECUIO_SET_MAX_TABLE */
#define ECUIO_VALIDATE_ERR_TABLE_RANGE      (-19)  /* an ENUM def's entries
                                                      or a COMPUTED def's
                                                      program past the
                                                      set's table */
#define ECUIO_VALIDATE_ERR_ENUM_ENTRY       (-20)  /* a label empty, not
                                                      terminated, not zero
                                                      after its NUL or one
                                                      a formula would start
                                                      (code 18's rule); or
                                                      one def's raw values
                                                      not strictly
                                                      ascending */
#define ECUIO_VALIDATE_ERR_PROGRAM          (-21)  /* a program that doesn't
                                                      run to one value: a
                                                      count out of range,
                                                      an unknown step, an
                                                      index past its count,
                                                      too few values for an
                                                      operation, a constant
                                                      not finite, an age
                                                      bound out of range,
                                                      or a nonzero byte
                                                      past a count or
                                                      reserved */
#define ECUIO_VALIDATE_ERR_COMPUTED_INPUT   (-22)  /* an input id that isn't
                                                      a numeric def of the
                                                      set (UINT, INT, FLOAT
                                                      or ENUM, not
                                                      COMPUTED) */

/* Why a set failed validation, and where. index names the element the
 * code is about: the failing def; for a duplicate id, the second def
 * of the pair; for a bad channel id, the channel; for a duplicate rx,
 * the second channel of the pair; for too many requests, the first def
 * whose request didn't fit; for a table range, the def; for an enum
 * entry, the entry; for a program or its input, the program. It is
 * UINT32_MAX for ECUIO_VALIDATE_OK and for a code that names no single
 * element: a NULL, the layout version, the set's reserved fields, the
 * channel count, a NULL table. */
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
 * ecuio_decode_signal (numeric kinds, ENUM with scale 1 and offset 0)
 * or ecuio_decode_text (text kinds) can decode, and COMPUTED defs
 * (their decode block as ECUIO_DEF_FLAG_COMPUTED says), with every
 * reserved byte zero and no flag but ECUIO_DEF_FLAG_COMPUTED. The kind
 * is checked first, then the byte order. A def's table_first and
 * table_count are checked against the set's tables by
 * ecuio_validate_signal_set. */
int ecuio_validate_signal(const ecuio_signal_def_t* signal);

/* Frame gate, called BEFORE ecuio_decode_signal (dispatch's decode
 * loop): returns 1 when the def applies to the frame (its message_id
 * is the frame's arbitration ID and, with mux on, the frame carries
 * its mux key), and 0 otherwise, for a COMPUTED def, for a frame
 * flagged ECUIO_FRAME_FLAG_ERROR (its ID is an error class) or for a
 * NULL argument. Saves the extraction for frames the def can't read. */
int ecuio_def_applies_to_frame(const ecuio_signal_def_t* signal,
                               const ecuio_frame_t* frame);

/* Set-level APIs */
const ecuio_signal_set_t* ecuio_mock_signal_set(void);
const ecuio_signal_set_t* ecuio_obd2_signal_set(void); /* ADR-0013 obd2 profile */

/* The layout version, before any other member is read; the set's
 * reserved fields; every def (ecuio_validate_signal; the first failing
 * def's code); unique def ids; the channel declarations; then the
 * tables: each NULL only when empty, each def's range in them, each
 * enum entry, each program and its inputs. A set with count 0 may
 * leave defs NULL. Returns ECUIO_VALIDATE_OK or a
 * ECUIO_VALIDATE_ERR_* code. When out isn't NULL, it receives the same
 * code and the index of the element at fault; a caller that only needs
 * pass or fail passes NULL. */
int ecuio_validate_signal_set(const ecuio_signal_set_t* set,
                              ecuio_validation_t* out);

/* ---- Display units (ADR-0022) ----
 * A decoded value is in its def's unit. The UI shows it in the user's
 * unit system, converted here so the UI never does arithmetic on a
 * value (rule 2). Recordings keep the decoded value and unit. */

/* Unit systems. Values are append-only. */
#define ECUIO_UNITS_METRIC    0u   /* as decoded */
#define ECUIO_UNITS_IMPERIAL  1u   /* mph, degF, psi, lb/min and more */

/* ecuio_display_convert return codes. Values are append-only. */
#define ECUIO_DISPLAY_OK              0
#define ECUIO_DISPLAY_ERR_NULL        1  /* a NULL unit or out */
#define ECUIO_DISPLAY_ERR_UNITS       2  /* an unknown unit system */

/* A value as displayed: the value, its unit (UTF-8, terminated) and
 * its decimal places, which a conversion can raise (kPa to psi needs
 * one more). converted is 1 when the unit changed. */
typedef struct ecuio_display_value {
    double   value;
    char     unit[16];
    uint8_t  decimal_places;      /* 0..3 */
    uint8_t  converted;           /* 0 or 1 */
    uint8_t  reserved[6];         /* written as zero */
} ecuio_display_value_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_display_value_t) == 32, "ecuio_display_value_t size is ABI");
static_assert(offsetof(ecuio_display_value_t, unit) == 8, "display unit offset is ABI");
static_assert(offsetof(ecuio_display_value_t, decimal_places) == 24, "display decimals offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_display_value_t) == 32, "ecuio_display_value_t size is ABI");
_Static_assert(offsetof(ecuio_display_value_t, unit) == 8, "display unit offset is ABI");
_Static_assert(offsetof(ecuio_display_value_t, decimal_places) == 24, "display decimals offset is ABI");
#endif

/* Converts a value in `unit` (a def's unit string) with that def's
 * decimal_places to the unit system `units`. The conversion is chosen
 * by the unit string: under ECUIO_UNITS_IMPERIAL "km/h", degrees C
 * ("\xC2\xB0" "C", UTF-8), "kPa", "g/s", "km", "Pa", "L/h" and
 * "N\xC2\xB7m" become "mph", degrees F, "psi" (one more decimal
 * place), "lb/min" (one more), "mi", "inH2O" (two more), "gal/h"
 * (one more) and "lb\xC2\xB7" "ft", with at most 3 decimal places;
 * any other unit, and every unit under ECUIO_UNITS_METRIC,
 * passes through unchanged with converted 0. *out is written only on
 * ECUIO_DISPLAY_OK. */
int ecuio_display_convert(const char* unit, uint8_t decimal_places,
                          double value, uint32_t units,
                          ecuio_display_value_t* out);

/* ---- Signal files (ADR-0051) ----
 * A user's own signals as data: JSON (format 1, docs/signal-files.md),
 * read from bytes, never a path, so a caller reads the file its own way
 * (Android's content:// URIs, iOS's sandbox). Everything a file gets
 * wrong is refused here, at load, not at the car. */

/* A file's bounds */
#define ECUIO_SIGNAL_FILE_MAX_BYTES   1048576u   /* 1 MiB */
#define ECUIO_SIGNAL_FILE_MAX_SIGNALS 1024u
/* The format this core reads: a file's "havne_signals" */
#define ECUIO_SIGNAL_FILE_FORMAT      1u

typedef struct ecuio_signal_file ecuio_signal_file_t;

/* Reads a signal file from its bytes (UTF-8 JSON, length bytes, no BOM).
 * ECUIO_OK with *out a file the caller frees with ecuio_signal_file_free;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL bytes (with a length) or out;
 * ECUIO_ERR_INVALID_ARGUMENT for a file it refuses, with
 * ecuio_last_error_detail() saying where and why ("line 12, column 9:
 * unknown key 'scael'", never a path); ECUIO_ERR_OUT_OF_MEMORY. *out is
 * NULL on any failure. The file owns its set, defs, channels and
 * tables, and its set passes ecuio_validate_signal_set and fits
 * ECUIO_PROFILE_MAX_REQUESTS. Thread-safe: each call is its own. */
ecuio_status_t ecuio_signal_file_load(const char* bytes, size_t length,
                                      ecuio_signal_file_t** out);

/* The file's set, valid until ecuio_signal_file_free; NULL for NULL.
 * A session using it, through the profile it registers under, keeps
 * the file alive (profile.h: the registry keeps the set's pointer). */
const ecuio_signal_set_t* ecuio_signal_file_set(const ecuio_signal_file_t* file);

/* The file's "name", and the profile name it registers under:
 * "user." and the name in lower case, every run of other characters a
 * single '-' ("Daily driver extras" -> "user.daily-driver-extras").
 * Valid until ecuio_signal_file_free; "" for NULL. */
const char* ecuio_signal_file_name(const ecuio_signal_file_t* file);
const char* ecuio_signal_file_profile(const ecuio_signal_file_t* file);

/* The set the file extends ("obd2"), or "" when it extends none or for
 * NULL: a front end can offer the file in that set's place */
const char* ecuio_signal_file_extends(const ecuio_signal_file_t* file);

/* A signal of the file by its "key": its def in the set, or NULL for a
 * key the file doesn't hold or a NULL argument. A base set's signals
 * (extends) have no key here. */
const ecuio_signal_def_t* ecuio_signal_file_find(const ecuio_signal_file_t* file,
                                                 const char* key);

/* Frees a file; NULL is a no-op. Unregister its profile first: a
 * session using the set must be destroyed before this. */
void ecuio_signal_file_free(ecuio_signal_file_t* file);

/* A group's name ("Engine", "Oxygen sensors"; ECUIO_SIGNAL_GROUP_*),
 * static and never NULL: "" for a code this core doesn't know, which a
 * UI shows as Other (ADR-0050). */
const char* ecuio_signal_group_name(uint32_t group);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIGNALS_H */
