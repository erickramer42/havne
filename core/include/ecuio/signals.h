// File/layer: core/include/ecuio/signals.h | C ABI, public
// Testing: UNIT: validator edge cases, mux range checks, BE vs LE comparison;
//          SignalFileTest, PluginWrapTest (test_signals), c_abi

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

/* ADR-0013 section 4: a set's channels, one for each ECU (module) it
 * asks. rx_id is the reply ID (each transport's receive filter and
 * ISO-TP's flow-control target); tx_id is the request's. A set that
 * declares none (raw, mock) asks nothing and filters nothing. Validated
 * at registration; read by the requesting transports (J2534, ELM327,
 * SocketCAN) and the simulated ECU. */
#define ECUIO_MAX_CHANNELS 32 /* a whole car's modules (ADR-0065) */

/* POD, 16 bytes; pinned by the assertions below */
typedef struct ecuio_channel_decl {
    uint32_t rx_id;    /* flow-control filter target */
    uint32_t tx_id;    /* request destination */
    uint32_t flags;    /* zero: none defined */
    uint32_t reserved; /* zero; room for what a module is given later */
} ecuio_channel_decl_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_channel_decl_t) == 16, "ecuio_channel_decl_t size is ABI");
static_assert(offsetof(ecuio_channel_decl_t, tx_id) == 4, "decl tx_id offset is ABI");
static_assert(offsetof(ecuio_channel_decl_t, flags) == 8, "decl flags offset is ABI");
static_assert(offsetof(ecuio_channel_decl_t, reserved) == 12, "decl reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_channel_decl_t) == 16, "ecuio_channel_decl_t size is ABI");
_Static_assert(offsetof(ecuio_channel_decl_t, tx_id) == 4, "decl tx_id offset is ABI");
_Static_assert(offsetof(ecuio_channel_decl_t, flags) == 8, "decl flags offset is ABI");
_Static_assert(offsetof(ecuio_channel_decl_t, reserved) == 12, "decl reserved offset is ABI");
#endif

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
 * up to four inputs and up to 16 constants. An input is a number def's
 * newest value no older than its age bound, or a computed def's value
 * worked out at the same frame from the number defs under it (a chain,
 * ADR-0062), which takes no age bound of its own. Each step pushes an
 * input or a constant, or pops one, two or three values and pushes one;
 * a program runs to one number. The low four bits of a push step are
 * the input's or the constant's index.
 *
 * A value is a number or a truth, and the validator checks each step's
 * types: a step given the wrong type, a truth left at the end or a step
 * value not listed here is refused. Any value may instead be no value:
 * an input with none within its age bound, or a step whose number isn't
 * finite. A step with no value in gives no value out, but for
 * ECUIO_COMPUTED_IF, ALL and ANY, as their comments say. "a" is the
 * second from the top and "b" the top; a run of steps over more parts
 * (min of three, clamp) is a run of these. */
#define ECUIO_COMPUTED_MAX_INPUTS    4u
#define ECUIO_COMPUTED_MAX_CONSTANTS 16u
#define ECUIO_COMPUTED_MAX_STEPS     33u   /* 16 operations and their 17 operands */
/* A chain's most levels: a computed def reading only number defs is 1
 * deep, one reading it 2 */
#define ECUIO_COMPUTED_MAX_DEPTH     8u

/* Numbers to a number */
#define ECUIO_COMPUTED_PUSH_INPUT    0x00u  /* | the input's index */
#define ECUIO_COMPUTED_PUSH_CONSTANT 0x10u  /* | the constant's index */
#define ECUIO_COMPUTED_ADD           0x20u
#define ECUIO_COMPUTED_SUBTRACT      0x21u  /* a - b */
#define ECUIO_COMPUTED_MULTIPLY      0x22u
#define ECUIO_COMPUTED_DIVIDE        0x23u  /* a / b; a zero b gives no value */
#define ECUIO_COMPUTED_MIN           0x24u  /* b when b < a, else a */
#define ECUIO_COMPUTED_MAX           0x25u  /* b when b > a, else a */
#define ECUIO_COMPUTED_ABS           0x26u  /* one number: its distance from zero */
#define ECUIO_COMPUTED_SQRT          0x27u  /* one number; below zero gives no value */
#define ECUIO_COMPUTED_ROUND         0x28u  /* one number; a half goes away from zero */
#define ECUIO_COMPUTED_FLOOR         0x29u  /* one number */
#define ECUIO_COMPUTED_CEIL          0x2Au  /* one number */

/* Two numbers to a truth */
#define ECUIO_COMPUTED_LESS          0x30u  /* a < b */
#define ECUIO_COMPUTED_LESS_EQUAL    0x31u  /* a <= b */
#define ECUIO_COMPUTED_GREATER       0x32u  /* a > b */
#define ECUIO_COMPUTED_GREATER_EQUAL 0x33u  /* a >= b */
#define ECUIO_COMPUTED_EQUAL         0x34u  /* a == b, exactly */
#define ECUIO_COMPUTED_NOT_EQUAL     0x35u  /* a != b, exactly */

/* Truths to a truth, and the one conditional */
#define ECUIO_COMPUTED_ALL           0x38u  /* false when either is false, else
                                               no value when either has none */
#define ECUIO_COMPUTED_ANY           0x39u  /* true when either is true, else
                                               no value when either has none */
#define ECUIO_COMPUTED_NOT           0x3Au  /* one truth */
#define ECUIO_COMPUTED_IF            0x3Bu  /* a truth p (third from the top),
                                               then numbers a and b: a when p
                                               holds, else b; no value when p
                                               has none or the one picked
                                               has none */

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
                                     0 for a computed input; zero after
                                     input_count */
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
#define ECUIO_SIGNAL_SET_LAYOUT 5u

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

/* The lowest and highest value a def's field decodes to, in the def's
 * unit: every raw value the field's bits hold (UINT 0 to 2^n - 1, INT
 * sign-extended from bit_length) with scale and offset applied, both
 * finite and lo < hi whatever the scale's sign. An ENUM scales as the
 * decoder scales it, so an ENUM as validated (scale 1, offset 0) gives
 * its raw range. A span, not a gauge's range: a UI narrows it. The mux
 * doesn't move the span and isn't checked (mux_length included). The
 * decoder's checks, in its order: ECUIO_DECODE_ERR_NULL for a NULL
 * argument; ECUIO_DECODE_ERR_UNSUPPORTED for a kind or byte order with
 * no decoder, a text kind or a COMPUTED def; ECUIO_DECODE_ERR_BAD_PARAMS
 * for bit parameters the decoder refuses (bit_length 0 or over 32,
 * bit_offset over 7, a big-endian field not whole bytes); then
 * ECUIO_DECODE_ERR_UNSUPPORTED for a FLOAT, whose field holds any
 * binary32. Last, ECUIO_DECODE_ERR_BAD_PARAMS for an end that isn't
 * finite or a span of zero width (a scale of 0, inf or NaN, an offset
 * NaN, an end past the double range). *lo and *hi are written only on
 * ECUIO_DECODE_OK. */
int ecuio_def_range(const ecuio_signal_def_t* def, double* lo, double* hi);

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
#define ECUIO_VALIDATE_ERR_REQUEST_COUNT    (-17)  /* one module's defs
                                                      derive more requests
                                                      than
                                                      ECUIO_MODULE_MAX_REQUESTS
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
                                                      run to one number: a
                                                      count out of range,
                                                      an unknown step, an
                                                      index past its count,
                                                      too few values for a
                                                      step, a step given a
                                                      truth for a number or
                                                      a number for a truth,
                                                      a truth at the end,
                                                      a constant
                                                      not finite, an age
                                                      bound out of range,
                                                      or a nonzero byte
                                                      past a count or
                                                      reserved */
#define ECUIO_VALIDATE_ERR_COMPUTED_INPUT   (-22)  /* an input id that isn't
                                                      a numeric def of the
                                                      set (UINT, INT, FLOAT
                                                      or ENUM) or a COMPUTED
                                                      def, or a COMPUTED
                                                      input with an age
                                                      bound */
#define ECUIO_VALIDATE_ERR_COMPUTED_CHAIN   (-23)  /* computed defs reading
                                                      each other in a
                                                      circle, or a chain
                                                      past
                                                      ECUIO_COMPUTED_MAX_DEPTH
                                                      */
#define ECUIO_VALIDATE_ERR_OUT_OF_MEMORY    (-24)  /* no room to check the
                                                      chains: the set may
                                                      be valid */
#define ECUIO_VALIDATE_ERR_DUPLICATE_NAME   (-25)  /* retired, never
                                                      returned or reused:
                                                      two defs may share
                                                      a name (ADR-0065
                                                      section 9) */
#define ECUIO_VALIDATE_ERR_CHANNEL_RESERVED (-26)  /* a channel's flags or
                                                      reserved word not
                                                      zero */

/* Why a set failed validation, and where. index names the element the
 * code is about: the failing def; for a duplicate id or name, the
 * second def of the pair; for a bad channel id or a channel's
 * reserved fields, the channel; for a duplicate rx, the second channel
 * of the pair; for too many requests, the first def whose request
 * didn't fit; for a table range, the def;
 * for an enum entry, the entry; for a program or its input, the
 * program; for a chain, the first program in the table it couldn't
 * settle (on a circle, reading one, or past the depth). It is
 * UINT32_MAX for ECUIO_VALIDATE_OK and for a code that names no single
 * element: a NULL, the layout version, the set's reserved fields, the
 * channel count, a NULL table, no memory. */
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
 * def's code); unique def ids (a name may repeat); the channel
 * declarations; then the tables: each NULL only when empty, each def's
 * range in them, each enum entry, each program and its inputs, then the
 * chains (no circle, none past ECUIO_COMPUTED_MAX_DEPTH; checked with a
 * byte per program from the heap, only when one computed def reads
 * another, so a set with no chain allocates nothing). A set with count
 * 0 may leave defs NULL. Returns ECUIO_VALIDATE_OK or a
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
 * ("\xC2\xB0" "C", UTF-8), "kPa", "bar", "g/s", "km", "Pa", "L/h"
 * and "N\xC2\xB7m" become "mph", degrees F, "psi" (one more decimal
 * place from kPa, none from bar), "lb/min" (one more), "mi", "inH2O"
 * (two more), "gal/h" (one more) and "lb\xC2\xB7" "ft", with at most 3
 * decimal places; any other unit, and every unit under
 * ECUIO_UNITS_METRIC, passes through unchanged with converted 0. *out is
 * written only on ECUIO_DISPLAY_OK. */
int ecuio_display_convert(const char* unit, uint8_t decimal_places,
                          double value, uint32_t units,
                          ecuio_display_value_t* out);

/* ---- Signal files (ADR-0051) ----
 * A user's own signals as data: JSON (format 1, docs/signal-files.md),
 * read from bytes, never a path, so a caller reads the file its own way
 * (Android's content:// URIs, iOS's sandbox). Everything a file gets
 * wrong is refused here, at load, not at the car. */

/* A data file's bounds: a file a user writes or imports, and each file
 * of a plugin */
#define ECUIO_SIGNAL_FILE_MAX_BYTES   1048576u   /* 1 MiB */
#define ECUIO_SIGNAL_FILE_MAX_SIGNALS 1024u
/* A set's bounds (ADR-0065 section 3): the signals of its own a set
 * built from several data files holds (a plugin variant's files
 * merged), and the bytes of that set written as one signal file, a
 * recording's signals.json. ecuio_signal_file_load_with reads to these
 * when asked. */
#define ECUIO_SIGNAL_SET_MAX_BYTES    4194304u   /* 4 MiB */
#define ECUIO_SIGNAL_SET_MAX_SIGNALS  16384u
/* The formats this core reads, a file's "havne_signals": the newest and
 * the oldest. Format 2 adds the computed grammar's functions,
 * comparisons and conditional; a format 1 file is held to format 1.
 * Format 3 (ADR-0065 section 9) keeps format 2's grammar and changes
 * what a file means: a name is a label and may repeat, a signal named as
 * a base def is a second signal of that name (in formats 1 and 2 it
 * replaces the base def), and two of the file's own signals at one
 * address are refused. */
#define ECUIO_SIGNAL_FILE_FORMAT        3u
#define ECUIO_SIGNAL_FILE_FORMAT_OLDEST 1u

typedef struct ecuio_signal_file ecuio_signal_file_t;

/* Reads a signal file from its bytes (UTF-8 JSON, length bytes, no BOM).
 * ECUIO_OK with *out a file the caller frees with ecuio_signal_file_free;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL bytes (with a length) or out;
 * ECUIO_ERR_INVALID_ARGUMENT for a file it refuses, with
 * ecuio_last_error_detail() saying where and why ("line 12, column 9:
 * unknown key 'scael'", never a path); ECUIO_ERR_OUT_OF_MEMORY. *out is
 * NULL on any failure. The file owns its set, defs, channels and
 * tables, and its set passes ecuio_validate_signal_set and derives at
 * most ECUIO_MODULE_MAX_REQUESTS requests for each ECU (profile.h).
 * Read to a data file's bounds: more than ECUIO_SIGNAL_FILE_MAX_BYTES,
 * or more than ECUIO_SIGNAL_FILE_MAX_SIGNALS signals of its own, is
 * refused. Thread-safe: each call is its own. */
ecuio_status_t ecuio_signal_file_load(const char* bytes, size_t length,
                                      ecuio_signal_file_t** out);

/* ecuio_signal_file_load_with's flags. Values are append-only. */
#define ECUIO_SIGNAL_FILE_LOAD_SET 0x1u /* read to a set's bounds */

/* ecuio_signal_file_load, read to the bounds flags name: 0 a data
 * file's, the same call; ECUIO_SIGNAL_FILE_LOAD_SET a set's
 * (ECUIO_SIGNAL_SET_MAX_BYTES, ECUIO_SIGNAL_SET_MAX_SIGNALS). The flag
 * is for the signals.json a recording holds, which is a session's whole
 * set and may be one built from several data files; a file a user
 * hands over is a data file. ECUIO_ERR_INVALID_ARGUMENT for any other
 * flag, *out NULL. */
ecuio_status_t ecuio_signal_file_load_with(const char* bytes, size_t length,
                                           uint32_t flags,
                                           ecuio_signal_file_t** out);

/* The file's set, valid until ecuio_signal_file_free; NULL for NULL.
 * A session using it, through the profile it registers under, keeps
 * the file alive (profile.h: the registry keeps the set's pointer). */
const ecuio_signal_set_t* ecuio_signal_file_set(const ecuio_signal_file_t* file);

/* The file's "name", and the profile name it registers under:
 * "user." and the name in lower case, every run of other characters a
 * single '-' ("Daily driver extras" -> "user.daily-driver-extras"). A
 * merged set's is "merged." and 16 hex digits, the start of the SHA-256
 * of "obd2" and a newline when it extends obd2, then each part's profile
 * name and a newline, in the parts' order: the same for the same parts
 * in the same order wherever they are merged, and nothing of what they
 * hold. Valid until ecuio_signal_file_free; "" for NULL. */
const char* ecuio_signal_file_name(const ecuio_signal_file_t* file);
const char* ecuio_signal_file_profile(const ecuio_signal_file_t* file);

/* The set the file extends ("obd2"), or "" when it extends none or for
 * NULL: a front end can offer the file in that set's place */
const char* ecuio_signal_file_extends(const ecuio_signal_file_t* file);

/* A signal of the file by its "key": its def in the set, or NULL for a
 * key the file doesn't hold or a NULL argument. A base set's signals
 * (extends) have no key here. In a merged set a key a later part's
 * signal took the place of ("replaced") finds the signal that stands
 * for it. */
const ecuio_signal_def_t* ecuio_signal_file_find(const ecuio_signal_file_t* file,
                                                 const char* key);

/* The "key" of a def of the file's own: the inverse of
 * ecuio_signal_file_find for the def's own key (never one it replaced:
 * ecuio_signal_file_replaced_keys), by the def's address in the file's
 * set. ""
 * for a base set's def (extends), a def that isn't the file's (another
 * set's, or a copy), or a NULL argument. Valid until
 * ecuio_signal_file_free. */
const char* ecuio_signal_file_key(const ecuio_signal_file_t* file,
                                  const ecuio_signal_def_t* def);

/* The file's format, its "havne_signals" (ECUIO_SIGNAL_FILE_FORMAT_OLDEST
 * to ECUIO_SIGNAL_FILE_FORMAT); a plugin variant's, the newest among its
 * files. 0 for NULL. */
uint32_t ecuio_signal_file_format(const ecuio_signal_file_t* file);

/* A computed def's "expr" as the file wrote it, found by the def's
 * address as ecuio_signal_file_key finds its key. "" for any other def,
 * a base set's def, a def that isn't the file's, or a NULL argument.
 * Valid until ecuio_signal_file_free. */
const char* ecuio_signal_file_expr(const ecuio_signal_file_t* file,
                                   const ecuio_signal_def_t* def);

/* ---- An expression against a file (ADR-0062) ----
 * What an editor needs before a computed signal is written into a file:
 * whether its expression compiles there, and the name an expression
 * reads each signal by. */

#define ECUIO_EXPR_MAX_BYTES 256u /* an expression's most bytes */
/* Room for the name any def has in an expression, and its NUL */
#define ECUIO_EXPR_REF_LEN 64u

/* What ecuio_signal_file_check_expr found */
typedef struct ecuio_expr_check {
    uint32_t struct_size;     /* in: the caller's sizeof, at least this
                                 one's; out: core's own, the bytes filled */
    uint32_t flags;           /* zero */
    uint32_t at;              /* refused: the character it is refused at,
                                 from 1 (a byte and a character are one up
                                 to there: the grammar is ASCII); 0 when
                                 the expression compiles */
    uint32_t format;          /* the oldest "havne_signals" whose loader
                                 takes it: 1 or 2, since no later format
                                 changes the grammar; 0 refused */
    uint32_t depth;           /* its chain's levels: 1 reading no computed
                                 signal, to ECUIO_COMPUTED_MAX_DEPTH; 0
                                 refused */
    uint32_t reserved[5];     /* zero */
    ecuio_computed_t program; /* what it compiles to: its inputs are def
                                 IDs of the file's set and every
                                 max_age_us is 0; all zero when refused */
} ecuio_expr_check_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_expr_check_t) == 248,
              "ecuio_expr_check_t size is ABI");
static_assert(offsetof(ecuio_expr_check_t, at) == 8, "check at offset is ABI");
static_assert(offsetof(ecuio_expr_check_t, reserved) == 20,
              "check reserved offset is ABI");
static_assert(offsetof(ecuio_expr_check_t, program) == 40,
              "check program offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_expr_check_t) == 248,
               "ecuio_expr_check_t size is ABI");
_Static_assert(offsetof(ecuio_expr_check_t, at) == 8, "check at offset is ABI");
_Static_assert(offsetof(ecuio_expr_check_t, reserved) == 20,
               "check reserved offset is ABI");
_Static_assert(offsetof(ecuio_expr_check_t, program) == 40,
               "check program offset is ABI");
#endif

/* Compiles expr (length bytes of UTF-8, no NUL needed) as the "expr" of
 * a computed signal of file: the names the file's own expressions read
 * (its keys, and "obd2.<key>" when it extends obd2), the loader's
 * grammar at its newest format, its bounds (ECUIO_EXPR_MAX_BYTES, a
 * program's inputs, constants and steps) and its chain rules. key names
 * the signal the expression is for: one of the file's keys when the
 * expression takes that signal's place (an edit), so a circle through
 * it, and what reads it today, are judged; NULL, "" or a key the file
 * lacks for a new signal.
 *
 * ECUIO_OK with out filled: the expression compiles, and a file holding
 * it under key is not refused for it at out->format or newer. A plugin
 * variant made of several files needs that format in each file whose
 * signals read key, the one holding it among them.
 * ECUIO_ERR_INVALID_ARGUMENT with out->at from 1, the rest of out zero
 * and ecuio_last_error_detail() the reason in the loader's words ("the
 * expression reads 'bost', which no signal is"): the expression is
 * refused; a length past ECUIO_EXPR_MAX_BYTES at 1, with none of its
 * bytes read. ECUIO_ERR_INVALID_ARGUMENT with out untouched: a struct_size
 * under the struct's, or a key that is no key (1 to 31 of a-z, 0-9 and
 * _). ECUIO_ERR_NULL_ARGUMENT, out untouched, for a NULL file or out, or
 * a NULL expr with a length. ECUIO_ERR_OUT_OF_MEMORY. A caller zeroes
 * out and sets struct_size first, so at tells the two refusals apart.
 * A plugin variant's file (ecuio_plugin_variant_file) is checked the
 * same. An expression naming a key the file needs from another
 * (ecuio_signal_file_needs), or a signal left out for one, is refused
 * in words that say so: it is checked against the set the files make
 * together.
 *
 * Not judged: the signal's other members (its name, which can put a new
 * signal in a base def's place, its unit, gauge and max_age) and the
 * file's own bounds (ECUIO_SIGNAL_FILE_MAX_SIGNALS, its bytes); the
 * loader judges those when the file is read. Any thread may call it
 * while the file lives: it only reads. */
ecuio_status_t ecuio_signal_file_check_expr(const ecuio_signal_file_t* file,
                                            const char* key, const char* expr,
                                            size_t length,
                                            ecuio_expr_check_t* out);

/* The name file's expressions read def by, into out (cap bytes;
 * ECUIO_EXPR_REF_LEN always fits): its key for a signal of the file's
 * own ("boost"), "obd2.<key>" for a base def of a file that extends obd2
 * (obd2's short name where it has one, "obd2.map", else "obd2.pid_14"
 * for a PID's first field and "obd2.pid_14_1" for its second). Compiling
 * the name reads that def. A text def has its name too, though no
 * expression reads text, which ecuio_signal_file_check_expr says.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL argument;
 * ECUIO_ERR_INVALID_ARGUMENT, out "" where cap allows, for a def no
 * expression of the file can name (another set's, a copy of one, a base
 * def that is no Service 01 field: obd2's computed Boost; a signal whose
 * key starts with a digit, which an expression reads as a number, unless
 * it stands in a base def's place and so has obd2's name) and for a cap
 * under the name and its NUL, each with its line in
 * ecuio_last_error_detail(). Any thread may call it while the file
 * lives: it only reads. */
ecuio_status_t ecuio_signal_file_expr_ref(const ecuio_signal_file_t* file,
                                          const ecuio_signal_def_t* def,
                                          char* out, size_t cap);

/* The number defs under program, through any chain of computed defs:
 * what ecuio_signal_file_evaluate takes a value for. Their IDs, each
 * once, in the order the program and the programs of its computed
 * inputs first read them (the serial ports' contract: *count all of
 * them, the first capacity copied, capacity 0 with ids NULL for the
 * count). program is one of the set's own
 * (set->computed[def->table_first]), a check's, or a caller's:
 * ECUIO_ERR_INVALID_ARGUMENT, *count untouched and a line in
 * ecuio_last_error_detail(), for one the set's validator would refuse
 * as the set's own (its counts, steps, constants or age bounds; an input
 * that is no number, enum or computed def of the set; an age bound on a
 * computed input; a chain past ECUIO_COMPUTED_MAX_DEPTH).
 * ECUIO_ERR_NULL_ARGUMENT for a NULL file, program or count, or NULL ids
 * with a capacity; ECUIO_ERR_OUT_OF_MEMORY. Any thread may call it while
 * the file lives: it only reads. */
ecuio_status_t ecuio_signal_file_expr_leaves(const ecuio_signal_file_t* file,
                                             const ecuio_computed_t* program,
                                             uint16_t* ids, uint32_t capacity,
                                             uint32_t* count);

/* One number def's value for ecuio_signal_file_evaluate */
typedef struct ecuio_eval_input {
    double value;       /* as decoded, in the def's own unit (an enum's
                           raw number); a NaN is no value */
    uint64_t age_us;    /* how long ago it was decoded */
    uint16_t def_id;    /* the number def it is a value of */
    uint16_t reserved0; /* zero */
    uint32_t flags;     /* zero */
} ecuio_eval_input_t;

/* ecuio_eval_result_t's flags. Values are append-only. */
#define ECUIO_EVAL_VALUE 0x1u /* value holds the result */

/* Why an evaluation has no value. Values are append-only. */
#define ECUIO_EVAL_WHY_NONE 0u
/* A leaf was given no value */
#define ECUIO_EVAL_WHY_MISSING 1u
/* A leaf's value is older than the bound it is read within */
#define ECUIO_EVAL_WHY_STALE 2u
/* No leaf lacked a value and the steps still gave none: a division by
 * zero, a square root below zero, a result past a double */
#define ECUIO_EVAL_WHY_NO_NUMBER 3u

typedef struct ecuio_eval_result {
    uint32_t struct_size; /* in: the caller's sizeof, at least this one's;
                             out: core's own, the bytes filled */
    uint32_t flags;       /* ECUIO_EVAL_*; any other bit zero */
    double value;         /* with ECUIO_EVAL_VALUE, else 0 */
    uint32_t why;         /* ECUIO_EVAL_WHY_*; NONE with a value */
    uint16_t leaf;        /* MISSING, STALE: the leaf of the first read
                             without a value, its def ID; else 0 */
    uint16_t reserved0;   /* zero */
    uint64_t bound_us;    /* STALE: the bound that leaf is past; else 0 */
    uint32_t reserved[4]; /* zero */
} ecuio_eval_result_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_eval_input_t) == 24,
              "ecuio_eval_input_t size is ABI");
static_assert(offsetof(ecuio_eval_input_t, age_us) == 8,
              "eval input age_us offset is ABI");
static_assert(offsetof(ecuio_eval_input_t, def_id) == 16,
              "eval input def_id offset is ABI");
static_assert(offsetof(ecuio_eval_input_t, flags) == 20,
              "eval input flags offset is ABI");
static_assert(sizeof(ecuio_eval_result_t) == 48,
              "ecuio_eval_result_t size is ABI");
static_assert(offsetof(ecuio_eval_result_t, value) == 8,
              "eval result value offset is ABI");
static_assert(offsetof(ecuio_eval_result_t, why) == 16,
              "eval result why offset is ABI");
static_assert(offsetof(ecuio_eval_result_t, leaf) == 20,
              "eval result leaf offset is ABI");
static_assert(offsetof(ecuio_eval_result_t, bound_us) == 24,
              "eval result bound_us offset is ABI");
static_assert(offsetof(ecuio_eval_result_t, reserved) == 32,
              "eval result reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_eval_input_t) == 24,
               "ecuio_eval_input_t size is ABI");
_Static_assert(offsetof(ecuio_eval_input_t, age_us) == 8,
               "eval input age_us offset is ABI");
_Static_assert(offsetof(ecuio_eval_input_t, def_id) == 16,
               "eval input def_id offset is ABI");
_Static_assert(offsetof(ecuio_eval_input_t, flags) == 20,
               "eval input flags offset is ABI");
_Static_assert(sizeof(ecuio_eval_result_t) == 48,
               "ecuio_eval_result_t size is ABI");
_Static_assert(offsetof(ecuio_eval_result_t, value) == 8,
               "eval result value offset is ABI");
_Static_assert(offsetof(ecuio_eval_result_t, why) == 16,
               "eval result why offset is ABI");
_Static_assert(offsetof(ecuio_eval_result_t, leaf) == 20,
               "eval result leaf offset is ABI");
_Static_assert(offsetof(ecuio_eval_result_t, bound_us) == 24,
               "eval result bound_us offset is ABI");
_Static_assert(offsetof(ecuio_eval_result_t, reserved) == 32,
               "eval result reserved offset is ABI");
#endif

/* Works program out over file's set from count inputs, as a session's
 * decoder does at a frame that feeds it: a leaf's value counts while its
 * age is no more than the bound of the program reading it (that input's
 * max_age_us, or ECUIO_VALUE_STALE_US for 0; a value at its bound
 * counts), a computed input is worked out from the leaves under it, a
 * step with no number gives no value, and an if's branch not taken
 * changes nothing. Nothing is kept and no session is touched. An input
 * for a def the program doesn't read is passed by, so a caller may give
 * every value it has; of two for one def the first counts. program as
 * ecuio_signal_file_expr_leaves takes it, refused the same with out
 * untouched.
 *
 * ECUIO_OK with out filled, with a value or without: with none, why is
 * MISSING or STALE and leaf the leaf of the first read without a value,
 * in the order the program and the programs of its computed inputs read
 * (a leaf an if's untaken branch alone reads among them; one read within
 * two bounds can be fresh at one read and stale at a later one), and
 * NO_NUMBER only when no leaf lacked one. ECUIO_ERR_INVALID_ARGUMENT, out
 * untouched, for a struct_size under the struct's, or an input whose
 * flags or reserved0 is not zero (no flag is defined);
 * ECUIO_ERR_NULL_ARGUMENT for a NULL file, program or out, or
 * NULL inputs with a count; ECUIO_ERR_OUT_OF_MEMORY. Any thread may call
 * it while the file lives: it only reads. */
ecuio_status_t ecuio_signal_file_evaluate(const ecuio_signal_file_t* file,
                                          const ecuio_computed_t* program,
                                          const ecuio_eval_input_t* inputs,
                                          uint32_t count,
                                          ecuio_eval_result_t* out);

/* A gauge's warning levels (ecuio_signal_gauge_t's flags). Values are
 * append-only. */
#define ECUIO_GAUGE_WARN_LOW  0x1u  /* warn_low is given */
#define ECUIO_GAUGE_WARN_HIGH 0x2u  /* warn_high is given */

/* A signal's "gauge" (ADR-0051's appendix "a signal's gauge"): the range
 * a dial shows and the levels it warns at, a value at or below warn_low
 * or at or above warn_high, as the file wrote them, in the signal's own
 * unit: the numbers a decoded value has before
 * ecuio_display_convert. All finite, min < max; a level given is
 * within them, and warn_low < warn_high when both are. A level not given
 * is 0 with its flag clear. */
typedef struct ecuio_signal_gauge {
    double   min;
    double   max;
    double   warn_low;
    double   warn_high;
    uint32_t flags;               /* ECUIO_GAUGE_*; any other bit zero */
    uint32_t reserved[3];         /* zero */
} ecuio_signal_gauge_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_gauge_t) == 48, "ecuio_signal_gauge_t size is ABI");
static_assert(offsetof(ecuio_signal_gauge_t, warn_high) == 24, "gauge warn_high offset is ABI");
static_assert(offsetof(ecuio_signal_gauge_t, flags) == 32, "gauge flags offset is ABI");
static_assert(offsetof(ecuio_signal_gauge_t, reserved) == 36, "gauge reserved offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_gauge_t) == 48, "ecuio_signal_gauge_t size is ABI");
_Static_assert(offsetof(ecuio_signal_gauge_t, warn_high) == 24, "gauge warn_high offset is ABI");
_Static_assert(offsetof(ecuio_signal_gauge_t, flags) == 32, "gauge flags offset is ABI");
_Static_assert(offsetof(ecuio_signal_gauge_t, reserved) == 36, "gauge reserved offset is ABI");
#endif

/* The gauge a def of the file's own gives, found by the def's address
 * as ecuio_signal_file_key finds its key. NULL for a def that gives
 * none, a base set's def (extends), a def that isn't the file's
 * (another set's, or a copy), or a NULL argument. Borrowed: valid until
 * ecuio_signal_file_free. Any thread may call it while the file lives:
 * it only reads. */
const ecuio_signal_gauge_t*
ecuio_signal_file_gauge(const ecuio_signal_file_t* file,
                        const ecuio_signal_def_t* def);

/* A file's "source" (ADR-0051 section 7): where its signals come from */
#define ECUIO_SOURCE_TEXT_MAX 512u /* bytes of one of its texts */
/* A text's room: its bytes, the NUL, the rest zero */
#define ECUIO_SOURCE_TEXT_LEN 520u

/* ecuio_signal_source_t's flags. Values are append-only. SHARE_ALIKE: the
 * license is Creative Commons' share-alike, so whoever passes the signals
 * on keeps the license and this source with them. MERGED: the file is a
 * set made of several parts, and this source says only that: where its
 * signals come from, and share-alike, are each part's
 * (ecuio_signal_file_part_source), never the whole file's. */
#define ECUIO_SOURCE_SHARE_ALIKE 0x1u
#define ECUIO_SOURCE_MERGED      0x2u

/* A file's "source" as it wrote it: name always, each other text "" when
 * not given (none holds a NUL). A plugin variant's file holds its first
 * data file's own source, else the manifest's (ADR-0064 Q1), so every
 * variant names where it comes from. SHARE_ALIKE is set when the license,
 * read as an SPDX expression in any case with a space taken as a hyphen,
 * names CC-BY-SA or CC-BY-NC-SA as a word ("CC-BY-SA-4.0", OBDb's). */
typedef struct ecuio_signal_source {
    uint32_t struct_size; /* in: the caller's sizeof, at least this one's;
                           * out: core's own, the bytes filled */
    uint32_t flags;       /* ECUIO_SOURCE_*; any other bit zero */
    uint32_t reserved[6]; /* zero */
    char name[ECUIO_SOURCE_TEXT_LEN];
    char url[ECUIO_SOURCE_TEXT_LEN];
    char commit[ECUIO_SOURCE_TEXT_LEN];
    char license[ECUIO_SOURCE_TEXT_LEN];
    char modified[ECUIO_SOURCE_TEXT_LEN];
} ecuio_signal_source_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_source_t) == 2632,
              "ecuio_signal_source_t size is ABI");
static_assert(offsetof(ecuio_signal_source_t, name) == 32,
              "source name offset is ABI");
static_assert(offsetof(ecuio_signal_source_t, license) == 1592,
              "source license offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_source_t) == 2632,
               "ecuio_signal_source_t size is ABI");
_Static_assert(offsetof(ecuio_signal_source_t, name) == 32,
               "source name offset is ABI");
_Static_assert(offsetof(ecuio_signal_source_t, license) == 1592,
               "source license offset is ABI");
#endif

/* Copies the file's source into out, every byte past each text zero, and
 * sets struct_size to core's own size, what was filled (a caller's larger
 * struct keeps the rest untouched). ECUIO_ERR_NULL_ARGUMENT for a NULL
 * file or out; ECUIO_ERR_INVALID_ARGUMENT for a struct_size smaller than
 * the struct, out untouched; each with a line in
 * ecuio_last_error_detail(). Any thread may call it while the file lives:
 * it only reads. */
ecuio_status_t ecuio_signal_file_source(const ecuio_signal_file_t* file,
                                        ecuio_signal_source_t* out);

/* The file as signal-file text, what a recording copies as signals.json:
 * the bytes ecuio_signal_file_load read, or for a plugin variant's file
 * (ecuio_plugin_variant_file) its files merged and written as one, which
 * ecuio_signal_file_load reads back to the variant's set. NUL-terminated,
 * no NUL inside; *length (NULL skips it) is its length without the NUL.
 * Borrowed: valid, and unchanged, until the file is freed (a variant's
 * until its plugin is), never released by the caller. "" with *length 0
 * for a NULL file. Any thread may call it while the file lives: it only
 * reads. */
const char* ecuio_signal_file_text(const ecuio_signal_file_t* file,
                                   size_t* length);

/* ---- A file's modules (ADR-0065 section 9) ---- */

/* No part: a def, a module or a note that belongs to none */
#define ECUIO_SIGNAL_PART_NONE 0xFFFFFFFFu

/* ecuio_signal_module_t's flags. Values are append-only. BASE: a channel
 * of the set the file extends that no "ecus" entry of the file names. */
#define ECUIO_MODULE_BASE 0x1u

/* One module of a file's set, as its "ecus" entry names it */
typedef struct ecuio_signal_module {
    uint32_t struct_size; /* in: the caller's sizeof, at least this one's;
                             out: core's own, the bytes filled */
    uint32_t flags;       /* ECUIO_MODULE_*; any other bit zero */
    uint32_t rx_id;       /* the ID it answers on */
    uint32_t tx_id;       /* the ID it is asked on */
    uint32_t part;        /* in a merged set, the part whose id stands
                             for the module; else ECUIO_SIGNAL_PART_NONE */
    uint32_t reserved[5]; /* zero */
    char id[32];          /* its entry's "id"; "" with ECUIO_MODULE_BASE */
    char name[64];        /* its entry's "name", a label for a screen; ""
                             when the entry gives none */
} ecuio_signal_module_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_module_t) == 136,
              "ecuio_signal_module_t size is ABI");
static_assert(offsetof(ecuio_signal_module_t, part) == 16,
              "module part offset is ABI");
static_assert(offsetof(ecuio_signal_module_t, id) == 40,
              "module id offset is ABI");
static_assert(offsetof(ecuio_signal_module_t, name) == 72,
              "module name offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_module_t) == 136,
               "ecuio_signal_module_t size is ABI");
_Static_assert(offsetof(ecuio_signal_module_t, part) == 16,
               "module part offset is ABI");
_Static_assert(offsetof(ecuio_signal_module_t, id) == 40,
               "module id offset is ABI");
_Static_assert(offsetof(ecuio_signal_module_t, name) == 72,
               "module name offset is ABI");
#endif

/* The module at index among the file's set's channels
 * (ecuio_signal_file_set's channel_count, the index a session of that
 * set gives ecuio_session_get_module), into out: its IDs, and the id and
 * name the file's "ecus" entry gives it (a plugin variant's: the
 * manifest's entry). ECUIO_ERR_NULL_ARGUMENT for a NULL file or out;
 * ECUIO_ERR_INVALID_ARGUMENT, out untouched, for an index at or past the
 * count and for a struct_size under the struct's; each with a line in
 * ecuio_last_error_detail(). Any thread may call it while the file
 * lives: it only reads. */
ecuio_status_t ecuio_signal_file_module(const ecuio_signal_file_t* file,
                                        uint32_t index,
                                        ecuio_signal_module_t* out);

/* Frees a file; NULL is a no-op. Unregister its profile first: a
 * session using the set must be destroyed before this. */
void ecuio_signal_file_free(ecuio_signal_file_t* file);

/*
 * A signal's key (ADR-0065 section 9): what identifies it wherever
 * something is kept, a recording's values and a vehicle's ticks. A
 * name is a label, and two signals of a set may share one.
 *
 * Most signals are kept under their address key, core's text for where
 * the value is read:
 *
 *   <id>:<key>[@<offset>]:<byte>.<bit>:<bits>    a number or an enum
 *   <id>:<key>[@<offset>]:<byte>:t<bytes>        a text or hex field
 *
 * <id> is message_id in upper-case hex, 3 digits up to 0x7FF and 8
 * above it; <key> the mux key's mux_length bytes in upper-case hex,
 * empty with no mux, and @<offset> its mux_offset when that isn't 0;
 * then start_byte, bit_offset and bit_length in decimal, or for a text
 * kind start_byte and byte_length (t0: to the payload's end).
 * "7E8:410C:2.0:16" is two bytes of the engine ECU's PID 0C,
 * "7E9:62F190:3:t17" seventeen bytes of a DID's reply. The byte order,
 * the kind, the scale and the offset are no part of it: two defs over
 * the same bits are one address, however each reads them.
 *
 * One address has one text in every set, so the key stands whatever
 * the signal is called and whatever else is read with it. It is never
 * an author's key (1 to 31 of a-z, 0-9 and _) nor "id:<n>", holds no
 * comma, quote, space or line break, starts with a hex digit, and is
 * at most ECUIO_SIGNAL_KEY_MAX bytes.
 */
#define ECUIO_SIGNAL_KEY_MAX 31u /* bytes, as an author's key */
#define ECUIO_SIGNAL_KEY_LEN 32u /* its room: bytes, NUL, zeros after */

typedef struct ecuio_signal_key {
    char text[ECUIO_SIGNAL_KEY_LEN];
} ecuio_signal_key_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_key_t) == 32, "ecuio_signal_key_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_key_t) == 32, "ecuio_signal_key_t size is ABI");
#endif

/* The def's address key, out zeroed past it. "" for a computed def
 * (ECUIO_DEF_FLAG_COMPUTED), which reads no address.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL argument, out untouched;
 * ECUIO_ERR_INVALID_ARGUMENT, out "", for a def whose text has no room:
 * a def ecuio_validate_signal refuses may have none, one it takes
 * always has. Reads the def alone, so any thread may call it. */
ecuio_status_t ecuio_signal_address_key(const ecuio_signal_def_t* def,
                                        ecuio_signal_key_t* out);

/*
 * The key each def of set is kept under, in the set's order, by the
 * serial ports' contract (serial.h): *count is every def, the first
 * capacity are copied, and a capacity of 0 with keys NULL asks for the
 * count. A def's key is its address key, unless it has none of its own:
 * a computed def, and a def at the address of one before it in the set,
 * take the author's key file gives them (ecuio_signal_file_key), and
 * "id:<n>", n the def's id in decimal, where it gives none. In a set
 * ecuio_validate_signal_set takes, no two defs share a key. Of two defs
 * at one address the earlier in the set's order keeps the address key
 * whichever was written first, so a def placed ahead of another at its
 * address takes the key from it, and what was kept under that key (a
 * vehicle's ticks, a recording's column) follows the new def.
 *
 * file is the signal file whose set this is, or NULL for a set that has
 * none (a built-in set, one registered by hand): pass the file wherever
 * there is one, or a computed signal is kept under an id that means
 * nothing outside its set. ECUIO_ERR_NULL_ARGUMENT for a NULL set or
 * count, or NULL keys with a capacity; ECUIO_ERR_INVALID_ARGUMENT for a
 * file of another set; ECUIO_ERR_OUT_OF_MEMORY. On a failure nothing is
 * written but *count, 0. Reads the set and the file alone.
 */
ecuio_status_t ecuio_signal_set_keys(const ecuio_signal_set_t* set,
                                     const ecuio_signal_file_t* file,
                                     ecuio_signal_key_t* keys,
                                     uint32_t capacity, uint32_t* count);

/*
 * The def text_utf8 names, among set's first count defs and their keys
 * (ecuio_signal_set_keys'): the def with that key, else the one def
 * with that name. *index is its place in the set. One rule wherever a
 * person names a signal (havne-cli's --signal, a tick kept by name).
 * ECUIO_ERR_NULL_ARGUMENT for a NULL argument;
 * ECUIO_ERR_INVALID_ARGUMENT, *index untouched and the detail
 * (ecuio_last_error_detail) saying why, for text no def has ("no signal
 * with the key or name 'Boost'") and for a name several defs have,
 * which lists their keys ("2 signals are named 'Battery voltage': name
 * one by its key (7E8:6201A4:3.0:8, 7E9:6201A4:3.0:8)"; the first 8,
 * then "and N more").
 */
ecuio_status_t ecuio_signal_set_find(const ecuio_signal_set_t* set,
                                     const ecuio_signal_key_t* keys,
                                     uint32_t count, const char* text_utf8,
                                     uint32_t* index);

/* ---- A merged set's parts (ADR-0065 section 9) ----
 * A set made of several files read together, in the order given: its
 * written form is a format 3 file with "parts", which names each part,
 * gives every signal the part it came from and keeps, as a signal's
 * "replaced", each key a later part's signal took the place of. A file
 * with no "parts" has none, and every call below says so. */

#define ECUIO_MERGE_MAX_PARTS 16u /* a merged set's most parts */

/* ecuio_signal_part_t's flags. Values are append-only. PLUGIN: the part
 * is a plugin's variant, and plugin_id, plugin_version and
 * plugin_variant name it. */
#define ECUIO_PART_PLUGIN 0x1u

typedef struct ecuio_signal_part {
    uint32_t struct_size;  /* in: the caller's sizeof, at least this
                              one's; out: core's own, the bytes filled */
    uint32_t flags;        /* ECUIO_PART_*; any other bit zero */
    uint32_t format;       /* the part's own "havne_signals" */
    uint32_t signal_count; /* its signals in this set: none when every
                              one was replaced or left out */
    uint32_t module_count; /* the modules those signals read */
    uint32_t reserved[5];  /* zero */
    char profile[64];      /* the profile the part registers under alone */
    char name[64];         /* the part's own "name" */
    char plugin_id[32];    /* "" without ECUIO_PART_PLUGIN, as the next two */
    char plugin_version[16];
    char plugin_variant[32];
} ecuio_signal_part_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_part_t) == 248,
              "ecuio_signal_part_t size is ABI");
static_assert(offsetof(ecuio_signal_part_t, profile) == 40,
              "part profile offset is ABI");
static_assert(offsetof(ecuio_signal_part_t, plugin_id) == 168,
              "part plugin_id offset is ABI");
static_assert(offsetof(ecuio_signal_part_t, plugin_variant) == 216,
              "part plugin_variant offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_part_t) == 248,
               "ecuio_signal_part_t size is ABI");
_Static_assert(offsetof(ecuio_signal_part_t, profile) == 40,
               "part profile offset is ABI");
_Static_assert(offsetof(ecuio_signal_part_t, plugin_id) == 168,
               "part plugin_id offset is ABI");
_Static_assert(offsetof(ecuio_signal_part_t, plugin_variant) == 216,
               "part plugin_variant offset is ABI");
#endif

/* How many parts the file is made of: 0 for a file with no "parts" and
 * for NULL */
uint32_t ecuio_signal_file_part_count(const ecuio_signal_file_t* file);

/* The part at index, in merge order, into out.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL file or out;
 * ECUIO_ERR_INVALID_ARGUMENT, out untouched, for an index at or past the
 * count and for a struct_size under the struct's, each with a line in
 * ecuio_last_error_detail(). Any thread may call it while the file
 * lives: it only reads. */
ecuio_status_t ecuio_signal_file_part(const ecuio_signal_file_t* file,
                                      uint32_t index, ecuio_signal_part_t* out);

/* The source of the part at index, as the part's own file gave it, with
 * its own ECUIO_SOURCE_SHARE_ALIKE: what a front end reads to say which
 * signals carry a license on. The contract is ecuio_signal_file_part's,
 * over ecuio_signal_source_t. */
ecuio_status_t ecuio_signal_file_part_source(const ecuio_signal_file_t* file,
                                             uint32_t index,
                                             ecuio_signal_source_t* out);

/* The part a def of the file's set came from, its index.
 * ECUIO_SIGNAL_PART_NONE for a base set's def (extends), a def of a file
 * with no "parts", a def that isn't the file's (another set's, or a
 * copy) and a NULL argument. A signal that replaced another's is its own
 * part's. */
uint32_t ecuio_signal_file_def_part(const ecuio_signal_file_t* file,
                                    const ecuio_signal_def_t* def);

/* The keys def stands for besides its own: those of the signals it took
 * the place of ("replaced"), which ecuio_signal_file_find still answers
 * with def. By the serial ports' contract (serial.h): *count is all of
 * them, the first capacity are copied, and a capacity of 0 with keys
 * NULL asks for the count; none for a def that replaced nothing or isn't
 * the file's. ECUIO_ERR_NULL_ARGUMENT for a NULL file, def or count, or
 * NULL keys with a capacity. Any thread may call it while the file
 * lives: it only reads. */
ecuio_status_t ecuio_signal_file_replaced_keys(const ecuio_signal_file_t* file,
                                               const ecuio_signal_def_t* def,
                                               ecuio_signal_key_t* keys,
                                               uint32_t capacity,
                                               uint32_t* count);

/* ---- What a file needs, and what a load left out (ADR-0065 section 9) ----
 * A format 3 file's "needs" lists keys its expressions read that another
 * file provides. Loaded alone, each computed signal reading a needed key
 * is judged whole and then left out of the set with a note, and so is
 * each signal computed from it; the rest of the file reads as written. */

/* The file's "needs" as written, in their order, by the serial ports'
 * contract (serial.h): *count is all of them, the first capacity are
 * copied, each zeroed past its text, and a capacity of 0 with keys NULL
 * asks for the count. None for a file with no "needs".
 * ECUIO_ERR_NULL_ARGUMENT for a NULL file or count, or NULL keys with a
 * capacity. Any thread may call it while the file lives: it only reads. */
ecuio_status_t ecuio_signal_file_needs(const ecuio_signal_file_t* file,
                                       ecuio_signal_key_t* keys,
                                       uint32_t capacity, uint32_t* count);

/* A note's kinds. Values are append-only. A load alone leaves only
 * SIGNAL_LEFT_OUT; the rest are a merge's (ecuio_signal_file_merge). */
/* A later part's signal reads an earlier one's address and took its place */
#define ECUIO_NOTE_REPLACED_BY_ADDRESS 1u
/* A later part's signal has an earlier one's key and took its place */
#define ECUIO_NOTE_REPLACED_BY_KEY 2u
/* A signal's own key was one another kept for a signal it replaced */
#define ECUIO_NOTE_KEY_TAKEN 3u
/* A signal its part read in an obd2 signal's place is read beside it */
#define ECUIO_NOTE_BESIDE_BASE 4u
/* A part's ECU id was another module's and took a suffix */
#define ECUIO_NOTE_MODULE_RENAMED 5u
/* A part's ECU isn't in the set, nor the signals that read it */
#define ECUIO_NOTE_MODULE_LEFT_OUT 6u
/* No signal of a part is in the set */
#define ECUIO_NOTE_PART_LEFT_OUT 7u
/* A signal isn't in the set */
#define ECUIO_NOTE_SIGNAL_LEFT_OUT 8u

/* A note's room for its sentence: its bytes, the NUL, the rest zero */
#define ECUIO_NOTE_TEXT_LEN 384u

/* One thing a load or a merge did that its files didn't say outright,
 * for a front end to show: a kind to fold the notes by and a sentence in
 * words, which names each signal with its part */
typedef struct ecuio_signal_note {
    uint32_t struct_size; /* in: the caller's sizeof, at least this one's;
                             out: core's own, the bytes filled */
    uint32_t kind;        /* ECUIO_NOTE_* */
    uint32_t part;        /* the part the note is about: the replacing,
                             taking or left-out signal's, the module's;
                             ECUIO_SIGNAL_PART_NONE in a file with none */
    uint32_t other_part;  /* the part of the signal that was replaced or
                             lost the key; else ECUIO_SIGNAL_PART_NONE */
    uint32_t def;         /* the def the note is about, its place in the
                             set: the signal that stands where the note's
                             was; UINT32_MAX for what isn't in the set */
    uint32_t count;       /* a left-out module's or part's signals; else 0 */
    uint32_t reserved[4]; /* zero */
    char text[ECUIO_NOTE_TEXT_LEN]; /* one line, UTF-8, no control
                                       character */
} ecuio_signal_note_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_signal_note_t) == 424,
              "ecuio_signal_note_t size is ABI");
static_assert(offsetof(ecuio_signal_note_t, def) == 16,
              "note def offset is ABI");
static_assert(offsetof(ecuio_signal_note_t, text) == 40,
              "note text offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_signal_note_t) == 424,
               "ecuio_signal_note_t size is ABI");
_Static_assert(offsetof(ecuio_signal_note_t, def) == 16,
               "note def offset is ABI");
_Static_assert(offsetof(ecuio_signal_note_t, text) == 40,
               "note text offset is ABI");
#endif

/* How many notes the file's load, or the merge that made it, left; 0 for
 * NULL. A merged set read back from its text has none: the notes are the
 * merge's, not the set's. */
uint32_t ecuio_signal_file_note_count(const ecuio_signal_file_t* file);

/* The note at index, in the order the load or the merge made them, into out.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL file or out;
 * ECUIO_ERR_INVALID_ARGUMENT, out untouched, for an index at or past the
 * count and for a struct_size under the struct's, each with a line in
 * ecuio_last_error_detail(); ECUIO_ERR_OUT_OF_MEMORY. Any thread may
 * call it while the file lives: it only reads. */
ecuio_status_t ecuio_signal_file_note(const ecuio_signal_file_t* file,
                                      uint32_t index, ecuio_signal_note_t* out);

/* ---- Signal files merged into one set (ADR-0065 section 9) ---- */

/* ecuio_merge_options_t's flags. Values are append-only. BASE_OBD2: the
 * set extends obd2 even when no part does. */
#define ECUIO_MERGE_BASE_OBD2 0x1u

typedef struct ecuio_merge_options {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t flags;       /* ECUIO_MERGE_*; any other bit is refused */
    uint32_t reserved[6]; /* zero */
} ecuio_merge_options_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_merge_options_t) == 32,
              "ecuio_merge_options_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_merge_options_t) == 32,
               "ecuio_merge_options_t size is ABI");
#endif

/* Merges count loaded files (1 to ECUIO_MERGE_MAX_PARTS: signal files,
 * plugin variants' files) into one set, a new file in *out, freed with
 * ecuio_signal_file_free. It only reads the parts and keeps nothing of
 * them, so the result outlives them. The set extends obd2 when a part
 * does or options asks; options NULL asks nothing.
 *
 * The parts are read in the order given, a later one over an earlier:
 * - A signal with an earlier part's signal's key takes its place. So
 *   does one reading an earlier part's signal's address
 *   (ecuio_signal_address_key), and it keeps that signal's key beside its
 *   own (ecuio_signal_file_replaced_keys), so what named the earlier
 *   signal reads the later. One matching both ways replaces both. Any
 *   other signal is added at the end. A name replaces nothing, and two
 *   signals may share one.
 * - No obd2 signal is replaced: a part's signal at an obd2 signal's
 *   address, or of its name, is read beside it, and "obd2.<key>" in any
 *   part's expression reads obd2's.
 * - ECUs at the same reply and request IDs are one module, under the
 *   earliest part's id and the earliest name given. An id another module
 *   holds takes "_2" ("_3" and on). Left out, each with a note: an ECU
 *   whose reply ID another module answers under another request ID, an
 *   ECU past ECUIO_MAX_CHANNELS, and a part whose IDs are of the other
 *   width than the set's.
 * - A computed signal is compiled against every part's keys, so a key a
 *   part "needs" is met by the part that defines it. One that then reads
 *   a key no signal has, a text, or a signal left out, names a computed
 *   signal in "max_age", sits on a circle or sits past
 *   ECUIO_COMPUTED_MAX_DEPTH is left out with a note, and so is each
 *   signal computed from it.
 *
 * The result is the set its own text loads to (ecuio_signal_file_text,
 * read with ECUIO_SIGNAL_FILE_LOAD_SET): a format 3 file with "parts".
 * Its profile is "merged." and 16 hex digits (ecuio_signal_file_profile),
 * the same for the same parts in the same order; its name is "N parts
 * read together"; its notes (ecuio_signal_file_note) say what the merge
 * replaced, renamed and left out. The same parts give the same bytes.
 *
 * ECUIO_ERR_NULL_ARGUMENT for a NULL out, NULL parts with a count, or a
 * NULL part. ECUIO_ERR_INVALID_ARGUMENT with the reason in
 * ecuio_last_error_detail(): no part or too many; a part given twice or
 * two parts of one profile; a part that is itself a merged set; a part
 * with no text (a variant whose copy doesn't read back); options with a
 * struct_size under the struct's, an unknown flag or a nonzero reserved
 * word; no signal left to read; and a bound of the set passed
 * (ECUIO_SIGNAL_SET_MAX_SIGNALS signals or replaced keys,
 * ECUIO_SIGNAL_SET_MAX_BYTES of text, ECUIO_SET_MAX_TABLE enum labels,
 * ECUIO_MODULE_MAX_REQUESTS requests of one module with the base's
 * counted), each naming its count. ECUIO_ERR_OUT_OF_MEMORY. *out is NULL
 * on every failure. Any thread may call it while the parts live. */
ecuio_status_t ecuio_signal_file_merge(const ecuio_signal_file_t* const* parts,
                                       uint32_t count,
                                       const ecuio_merge_options_t* options,
                                       ecuio_signal_file_t** out);

/* A group's name ("Engine", "Oxygen sensors"; ECUIO_SIGNAL_GROUP_*),
 * static and never NULL: "" for a code this core doesn't know, which a
 * UI shows as Other (ADR-0050). */
const char* ecuio_signal_group_name(uint32_t group);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIGNALS_H */
