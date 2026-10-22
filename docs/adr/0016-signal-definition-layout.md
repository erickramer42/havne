# ADR-0016: Signal definition layout — value kind, byte order, layout version

- **Status:** Proposed. The structures and rules below are decided.
  The record moves to Accepted when the layout lands with its tests,
  before the v0.3.0 tag. The list of changes that ride with it freezes
  then; anything proposed later waits for the next layout version.
- **Date:** 2026-10-22
- **Supersedes, on acceptance:** [ADR-0004](0004-signal-decoding-model.md)'s
  field model: the type enum, `num_bytes`, Intel-only byte order with
  a flags bit held for Motorola, and the 88-byte size. Its linear
  scaling, message-ID gate and mock-encoder rule stand.
- **Amends, on acceptance:** [ADR-0015](0015-request-engine.md) §2.
  Its 3-byte mux keys land here, and its "No struct change" reasoning
  no longer holds, because core reads a layout version.

Names are the core's `ecuio_` names
([ADR-0001](0001-qt-free-core-c-abi.md), naming appendix of
2026-10-22).

## Context

The signal definition is the widest part of the core's ABI. Every
built-in set, and every set a plugin registers, is an array of defs
passed by pointer and kept by the registry
([ADR-0013](0013-protocol-profiles.md)). Its layout has outgrown
ADR-0004:

- **Width is stated three ways.** The type enum (`UINT8` …
  `FLOAT32`), `num_bytes` and `bit_length` each give one. Decode
  extracts `bit_length` bits and never compares it with the type, and
  `num_bytes` only bounds `bit_offset + bit_length`. A 12-bit field
  has to claim `UINT16`, and nothing checks that the three agree.
- **Byte order is a flags bit, and floats ignore it.** Bit 0 of
  `flags` selects big-endian for whole-byte integers. A `FLOAT32` is
  copied in host byte order and the validator refuses a big-endian
  one, so a big-endian float can't be described.
- **Text has no working slot.** `STRING` and a manufacturer range
  exist in the enum, but nothing decodes them, so the validator
  refuses them.
- **Nothing can grow safely.** The set's `version` is set (1 for the
  mock set, 2 for obd2) but never read. The def's `mux_reserved`
  bytes are never checked, and its five padding bytes belong to the
  compiler. A field placed in any of them later would be read from
  sets that hold garbage there.
- **The decode API has a precision ceiling.** `scale`, `offset` and
  the result are `float`, whose 24-bit significand can't hold every
  32-bit raw value: 16,777,217 decodes as 16,777,216. QML numbers are
  already `double`.
- **Two types are too narrow.** `payload_len` is `uint8_t` while the
  frame's `data_length` is `uint16_t`, so every caller narrows. A
  `uint8_t` `start_byte` can't reach past byte 255 of a reassembled
  ISO 15765 message, which can carry 4,095 bytes.
- **Mux keys stop at 2 bytes.** ADR-0015 §2 needs 3, for `62 <DID>`.
- **A refused registration doesn't say why.** Registration returns
  `INVALID_ARGUMENT` for every validator failure, so a set author
  finds the failing def by hand.

No release ships signal sets yet, and no plugin exists outside the
tree, so the break costs least before the v0.3.0 tag.

## Decision

The def, the set, decode, validation and registration change
together, once, and the set carries a layout version from then on.

### 1. The definition

The def is grouped into three blocks: which frames carry the signal,
how the bytes become a value, and how the value is shown. Every byte
is a named field, so there is no compiler padding.

```c
typedef enum {
    ECUIO_VALUE_UINT  = 0,
    ECUIO_VALUE_INT   = 1,   /* two's complement, sign-extended from bit_length */
    ECUIO_VALUE_FLOAT = 2,   /* IEEE-754 binary32 */
    ECUIO_VALUE_TEXT  = 3    /* reserved: no decoder yet */
} ecuio_value_kind_t;

#define ECUIO_BYTE_ORDER_LE 0u   /* Intel: LSB first, bit fields at any offset */
#define ECUIO_BYTE_ORDER_BE 1u   /* Motorola: MSB first, whole bytes only */

typedef struct ecuio_signal_def {
    uint16_t id;
    uint16_t reserved0;           /* zero */

    /* Match: which frames carry this signal */
    uint32_t message_id;          /* the frame's arbitration_id must equal it */
    uint8_t  mux_offset;          /* payload byte where the key starts */
    uint8_t  mux_length;          /* 0 = no mux; 1..3 = key bytes */
    uint8_t  mux_reserved[2];     /* zero */
    uint32_t mux_key;             /* key bytes, first byte most significant */

    /* Decode: how the bytes become a value */
    uint8_t  kind;                /* ecuio_value_kind_t */
    uint8_t  byte_order;          /* ECUIO_BYTE_ORDER_* */
    uint16_t start_byte;
    uint8_t  bit_offset;          /* 0..7; 0 for BE and FLOAT */
    uint8_t  bit_length;          /* 1..32; 32 for FLOAT; whole bytes for BE */
    uint8_t  decode_reserved[2];  /* zero */
    double   scale;
    double   offset;              /* value = raw * scale + offset */

    /* Display */
    char     name[32];            /* UTF-8, terminated inside the buffer */
    char     unit[16];            /* UTF-8, terminated inside the buffer */
    uint8_t  decimal_places;      /* 0..3 */
    uint8_t  display_reserved[3]; /* zero */

    uint32_t flags;               /* none defined; zero */
    uint32_t reserved[6];         /* zero; [0] is earmarked for the unit's quantity */
} ecuio_signal_def_t;             /* 120 bytes */
```

The size and the key offsets are pinned by static asserts in
`signals.h`, and again in C in `tests/test_c_abi.c`: 120 bytes, with
`kind` at 16, `start_byte` 18, `scale` 24, `name` 40, `flags` 92 and
`reserved` 96. The layout has no padding on x86 or x64.

### 2. Value kinds and byte order

- **Width lives in `bit_length` alone**, 1 to 32. `num_bytes` is
  removed.
- **Kinds.** `UINT` is the raw field. `INT` is two's complement,
  sign-extended from `bit_length`. `FLOAT` is binary32, 32 bits at
  bit 0, in either byte order. `TEXT` is reserved: validation refuses
  it, and decode returns unsupported, until it has a decoder and a
  text output path.
- **Byte order.** A little-endian field's LSB sits at `bit_offset`
  (0..7) of `start_byte`, and the field runs upward through
  consecutive bytes, so it may span five. A big-endian field is whole
  bytes, `bit_offset` 0 and `bit_length` a multiple of 8, first byte
  most significant. Sub-byte Motorola fields can arrive later as a new
  `byte_order` value.
- **`kind` and `byte_order` are fixed-width fields**, because an
  enum's size is up to the compiler. `ecuio_value_kind_t` and
  `ECUIO_BYTE_ORDER_*` name their values, which are append-only.
- **Flags bit 0**, which selected big-endian, is retired and not
  reused. No flag is defined.

### 3. Arithmetic and reach

```c
int ecuio_decode_signal(const ecuio_signal_def_t* signal,
                        const uint8_t* payload,
                        uint16_t payload_len,
                        double* out_value);
```

- `value = raw * scale + offset`, computed in `double`. The result,
  and the bridge's staging buffer and model behind it, are `double`
  too, so every 32-bit raw value decodes exactly.
- `payload_len` and `start_byte` are `uint16_t`. They match the
  frame's `data_length` and reach the end of the longest reassembled
  message.

### 4. Mux keys

- `mux_length` is 0 (no mux) or 1 to 3. The key bytes at `mux_offset`
  compare with `mux_key`, first byte most significant. The key is a
  byte sequence, not a number with a byte order, so neither
  `byte_order` nor `bit_length` applies to it.
- **One Mode-01 key test.** The PID scheduler enrols a def, and the
  J2534 transport routes a PID's request, only when the def's key is
  a 2-byte `41 <PID>` at offset 0. One helper answers for both, so
  they can't disagree, and a `62 <DID>` key whose last byte equals a
  polled PID matches neither. ADR-0015's engine replaces it with
  enrolment by service.

### 5. The set and its layout version

```c
#define ECUIO_SIGNAL_SET_LAYOUT 3u

typedef struct ecuio_signal_set {
    uint32_t layout_version;      /* first member, forever */
    uint32_t count;
    const ecuio_signal_def_t* defs;
    uint32_t channel_count;
    uint32_t reserved0;           /* zero */
    const ecuio_channel_decl_t* channels;
    uint32_t reserved[4];         /* zero */
} ecuio_signal_set_t;
```

- **`layout_version` is the first member, forever,** so core can read
  it before it knows anything else about the struct. Validation
  refuses any value but `ECUIO_SIGNAL_SET_LAYOUT`, and checks it
  before reading any def. The value is 3 so that neither old
  `version` value passes, and the rename from `version` makes every
  set author restate it.
- **It describes the struct, not the set's contents.** Which set it
  is, is the profile name's job.
- The next incompatible layout bumps the constant. A field that takes
  reserved room and reads zero as "not set" needs no bump, because
  old sets carry zero there.

### 6. Reserved room

- Every reserved byte in the def and the set must be zero, and so
  must the def's `flags`. Validation refuses anything else, so a later
  field can take reserved room and old sets keep loading.
- The def's 24-byte tail is earmarked. `reserved[0]` (offset 96) holds
  the unit's quantity when unit conversion lands. `reserved[1]` is
  free. `reserved[2..5]` (offsets 104–119) are free and 8-byte
  aligned, which fits a two-double display range.

### 7. Validation says why

```c
typedef struct ecuio_validation {
    int32_t  code;                /* ECUIO_VALIDATE_*; 0 = OK */
    uint32_t index;               /* the element the code names, or UINT32_MAX */
    uint32_t reserved[2];         /* written as zero */
} ecuio_validation_t;

int ecuio_validate_signal_set(const ecuio_signal_set_t* set,
                              ecuio_validation_t* out);   /* out may be NULL */

ecuio_status_t ecuio_profile_register(const char* name,
                                      const ecuio_signal_set_t* set,
                                      ecuio_validation_t* out);   /* out may be NULL */
```

- **The result is optional.** The validator still returns its code,
  so a caller that only needs pass or fail passes NULL.
- **`index`** names the failing def; for a duplicate ID, the second
  def of the pair; for a bad channel ID, the channel; for a duplicate
  rx ID, the second channel of the pair. It is `UINT32_MAX` for codes
  that name no single element: NULL, the layout version, the set's
  reserved fields and the channel count.
- **Registration hands back the validator's result** when it checked
  the set. When it refused before that (a NULL, malformed or built-in
  name), or the set is NULL (a log-only profile), it reports code 0
  and index `UINT32_MAX`.

### 8. Validator codes

Codes stay append-only, and a retired number is never reused. Codes
not listed are unchanged.

| Code | Name | After this record |
|---|---|---|
| 2 | `NUM_BYTES` | Retired with its field |
| 3 | `BIT_LENGTH` | 0, or over 32 |
| 5 | `BIT_SPAN` | Retired: without `num_bytes` there is no window to overrun |
| 9 | `MUX_LENGTH` | Over 3 |
| 11 | `MUX_KEY` | A key wider than its length, now up to 3 bytes |
| 12 | `BIG_ENDIAN` | Unchanged: a big-endian field that isn't whole bytes |
| 13 | `KIND` | Renamed from `TYPE`, same value: `TEXT` or an unknown kind |
| 14 | `FLOAT32` | Narrowed: a `FLOAT` that isn't 32 bits at bit 0; both byte orders decode |
| 15 | `RESERVED` | New: a reserved byte, or `flags`, isn't zero |
| 16 | `BYTE_ORDER` | New: an unknown `byte_order` |
| −15 | `LAYOUT` | New: `layout_version` isn't `ECUIO_SIGNAL_SET_LAYOUT`, checked before any def is read |
| −16 | `SET_RESERVED` | New: a reserved field of the set isn't zero |

The validator still accepts exactly what the decoder decodes, for
the fields decode reads. The layout version and the reserved bytes
are registration rules that the per-frame decoder doesn't re-check.
Check order stays a tested contract: the kind is checked before the
bit parameters, in both the decoder and the validator.

### What this record doesn't claim

No field layout makes a def correct. A def that reads a J1979
quantity as signed when the standard defines it as unsigned decodes
without complaint. Only checking each def against a published J1979
table catches that.

## Alternatives considered

- **Keep the type enum, drop `num_bytes`, add a byte-order field.**
  The smallest change, and a real contender. Rejected because width
  is still stated twice, and a 12-bit field still has to claim
  `UINT16`.
- **`bit_length` plus an `is_signed` flag.** Rejected because a float
  needs a second flag, `is_signed` on a float is a meaningless state,
  and text has nowhere to go.
- **A struct size instead of a layout version.** Rejected because two
  layouts of the same size look identical.
- **A `double` result over `float` fields.** Rejected because it
  claims precision the arithmetic doesn't have.
- **A bare code out-parameter on registration.** Rejected because a
  code without the failing def's index still leaves the author
  searching by hand.

## Consequences

- One ABI break before v0.3.0 covers the def, the set, decode,
  validation and registration. A set written against the old struct
  carries 1 or 2 where `layout_version` now sits, so registration
  refuses it instead of decoding garbage.
- Later growth, such as the unit's quantity, a display range, or a
  new kind or byte order, takes reserved room or appends a value
  without another break.
- Each def grows from 88 to 120 bytes, and each staged value from 4
  to 8.
- The `obd2` set moves to named-member initialisation, like the mock
  set, so a new field can't shift its values.
- The mock encoder and the fake DLL's canned replies don't change,
  because the wire bytes are the same. The existing round-trip tests
  show the new defs read them.
- `TEXT` stays unusable until a decoder and a text output path exist.
- The J2534 read path still keeps 64 bytes of each message. That
  limit belongs to the frame and the read path, not the def, and its
  fix is additive. The 16-bit lengths let the decoder read a longer
  buffer once one is delivered.
