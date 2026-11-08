// File/layer: core/src/signal_sets.cpp | C++17, Qt-free, every platform
// Testing: UNIT: Obd2SetTest, Obd2DecodeTest, Obd2MuxTest (test_obd2);
//          CoreTest (test_core); SignalsTest, SignalTablesTest (test_signals)

#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include "computed.h"
#include <string.h>

/* Bounded copy into a fixed-width buffer, always null-terminated.
 * Portable stand-in for strncpy_s, which is MSVC-only. */
static void copy_str(char* dst, size_t dst_size, const char* src)
{
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static ecuio_signal_def_t mk(uint16_t id, const char* name,
                             ecuio_value_kind_t kind,
                             uint16_t sb, uint8_t bo, uint8_t bl,
                             double scale, double offset,
                             const char* unit, uint8_t dp, uint8_t group)
{
    ecuio_signal_def_t d = {};   /* reserved bytes and flags zero */
    d.group = group;             /* ECUIO_SIGNAL_GROUP_*, as obd2's (ADR-0050) */
    d.id = id;
    d.kind = static_cast<uint8_t>(kind);
    d.start_byte = sb;
    d.bit_offset = bo;
    d.bit_length = bl;
    d.scale = scale;
    d.offset = offset;
    d.decimal_places = dp;
    d.message_id =  ECUIO_MOCK_TELEMETRY_ARB_ID;    /* all mock signals ride 0x0C0 */
    /* The mock set is deliberately mux-less and little-endian, matching
     * the plain LE payloads mock_transport.cpp encodes. d = {} already
     * zeroes these; stating them keeps a future default from silently
     * turning on mux or big-endian decode here. */
    d.byte_order = ECUIO_BYTE_ORDER_LE;
    d.mux_offset = 0;
    d.mux_length = 0;           /* mux disabled */
    d.mux_key = 0;
    copy_str(d.name, sizeof(d.name), name);
    copy_str(d.unit, sizeof(d.unit), unit);
    return d;
}

/* A computed def (ADR-0051 section 5): no frame of its own, its value
 * worked out from other defs' by its program, kMockComputed[program].
 * The validator wants the decode block plain: message 0, UINT, LE,
 * every bit field 0, scale 1. */
static ecuio_signal_def_t computed_def(uint16_t id, const char* name,
                                       const char* unit, uint8_t dp,
                                       uint8_t group, uint16_t program)
{
    ecuio_signal_def_t d = {};
    d.id = id;
    d.flags = ECUIO_DEF_FLAG_COMPUTED;
    d.kind = static_cast<uint8_t>(ECUIO_VALUE_UINT);
    d.byte_order = ECUIO_BYTE_ORDER_LE;
    d.scale = 1.0;
    d.decimal_places = dp;
    d.group = group;
    d.table_first = program;
    copy_str(d.name, sizeof(d.name), name);
    copy_str(d.unit, sizeof(d.unit), unit);
    return d;
}

/* Names, units and decimal places are display strings: the UI shows
 * them as they are (UTF-8). The names match the obd2 set's, so a gauge
 * finds the same signal under either profile. */
#define DEG_C "\xC2\xB0" "C"   /* UTF-8 degree sign, whatever the source encoding */
#define LAMBDA "\xCE\xBB"      /* UTF-8 Greek small lambda */

// clang-format off
static const ecuio_signal_def_t kMockDefs[] = {
    mk(ECUIO_MOCK_SIG_RPM_ID,       "RPM",               ECUIO_VALUE_UINT, 0, 0, 16, 0.25,  0.0,  "RPM",    0, ECUIO_SIGNAL_GROUP_ENGINE),
    /* Coolant and IAT are 8-bit UINT with a -40 offset (J1979: A - 40,
     * so -40..215 degC). As INT, anything from 88 degC up would read
     * as about -160 degC. */
    mk(ECUIO_MOCK_SIG_COOLANT_ID,   "Coolant Temp",      ECUIO_VALUE_UINT, 2, 0, 8,   1.0, -40.0, DEG_C,    0, ECUIO_SIGNAL_GROUP_ENGINE),
    mk(ECUIO_MOCK_SIG_SPEED_ID,     "Vehicle Speed",     ECUIO_VALUE_UINT, 3, 0, 8,   1.0,   0.0, "km/h",   0, ECUIO_SIGNAL_GROUP_VEHICLE),
    mk(ECUIO_MOCK_SIG_THROTTLE_ID, "Throttle Position", ECUIO_VALUE_UINT,  4, 0, 8,   100.0/255.0, 0.0, "%", 1, ECUIO_SIGNAL_GROUP_AIR),
    mk(ECUIO_MOCK_SIG_IAT_ID,       "Intake Air Temp",   ECUIO_VALUE_UINT, 5, 0, 8,   1.0, -40.0, DEG_C,    0, ECUIO_SIGNAL_GROUP_AIR),
    mk(ECUIO_MOCK_SIG_LOAD_ID,      "Engine Load",       ECUIO_VALUE_UINT, 6, 0, 8,   100.0/255.0, 0.0, "%", 1, ECUIO_SIGNAL_GROUP_ENGINE),
    /* Layout mirrors encode_payload's tail: MAF at bytes 7-8 (LE,
     * like RPM), oil temp at byte 9. Centi-grams/s scale is the
     * same J1979 0x10 formula the obd2 set uses; oil is A - 40. The
     * 0.01 g/s resolution is shown to two decimals, the step of the PID
     * (ADR-0056 section 5). */
    mk(ECUIO_MOCK_SIG_MAF_ID,       "MAF",               ECUIO_VALUE_UINT, 7, 0, 16, 0.01,  0.0, "g/s",  2, ECUIO_SIGNAL_GROUP_AIR),
    mk(ECUIO_MOCK_SIG_OIL_ID,       "Engine Oil Temp",   ECUIO_VALUE_UINT, 9, 0, 8,   1.0, -40.0, DEG_C, 0, ECUIO_SIGNAL_GROUP_ENGINE),
    /* Wideband lambda at bytes 10-11, LE, scaled like J1979's
     * equivalence ratio (2/65536, so 0 to just under 2). Only the mock
     * carries it; the obd2 set doesn't poll an O2 sensor. */
    mk(ECUIO_MOCK_SIG_LAMBDA_ID,    "Lambda",            ECUIO_VALUE_UINT, 10, 0, 16, 1.0/32768.0, 0.0, LAMBDA, 2, ECUIO_SIGNAL_GROUP_OXYGEN),
    /* MAP and baro at bytes 12 and 13, a byte of kPa each, as J1979's
     * 0x0B and 0x33 */
    mk(ECUIO_MOCK_SIG_MAP_ID,       "Intake Manifold Pressure", ECUIO_VALUE_UINT, 12, 0, 8, 1.0, 0.0, "kPa", 0, ECUIO_SIGNAL_GROUP_AIR),
    mk(ECUIO_MOCK_SIG_BARO_ID,      "Barometric Pressure", ECUIO_VALUE_UINT, 13, 0, 8, 1.0, 0.0, "kPa", 0, ECUIO_SIGNAL_GROUP_AIR),
    computed_def(ECUIO_MOCK_SIG_BOOST_ID, "Boost", "kPa", 0, ECUIO_SIGNAL_GROUP_AIR, 0),
};
// clang-format on

#undef DEG_C
#undef LAMBDA

/* The programs of kMockDefs' computed defs, by their table_first.
 * Boost is MAP minus baro, as ADR-0051's example file computes it from
 * obd2's: below zero a vacuum, which is all an NA engine pulls. Both
 * ride every frame, so the default age bound holds. */
static const ecuio_computed_t kMockComputed[] = {
    [] {
        ecuio_computed_t c = {};
        c.input_ids[0] = ECUIO_MOCK_SIG_MAP_ID;
        c.input_ids[1] = ECUIO_MOCK_SIG_BARO_ID;
        c.input_count = 2;
        c.step_count = 3;
        c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
        c.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
        c.steps[2] = ECUIO_COMPUTED_SUBTRACT;
        return c;
    }(),
};

static const ecuio_signal_set_t kMockSet = [] {
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(sizeof(kMockDefs) / sizeof(kMockDefs[0]));
    s.defs = kMockDefs;
    s.computed_count =
        static_cast<uint32_t>(sizeof(kMockComputed) / sizeof(kMockComputed[0]));
    s.computed = kMockComputed;
    return s;
}();

const ecuio_signal_set_t* ecuio_mock_signal_set(void)
{
    return &kMockSet;
}

static bool label_like_formula(const char* text)
{
    const char c = text[0];
    return c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' ||
           c == '\r';
}

/* An enum entry's label: 1 to ECUIO_ENUM_LABEL_MAX bytes, zero after
 * its NUL, and nothing a spreadsheet runs as a formula, since a
 * recording writes it (ADR-0049 section 8) */
static bool label_ok(const ecuio_enum_entry_t& e)
{
    const void* end = memchr(e.label, '\0', sizeof e.label);
    if (end == nullptr) {
        return false;
    }
    const size_t n =
        static_cast<size_t>(static_cast<const char*>(end) - e.label);
    if (n == 0 || n > ECUIO_ENUM_LABEL_MAX) {
        return false;
    }
    for (size_t i = n; i < sizeof e.label; ++i) {
        if (e.label[i] != '\0') {
            return false;
        }
    }
    return !label_like_formula(e.label);
}

/* Whether id names a def of the set a program can read: a number */
static bool numeric_input(const ecuio_signal_set_t* set, uint16_t id)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.id != id) {
            continue;
        }
        return (d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 &&
               (d.kind == ECUIO_VALUE_UINT || d.kind == ECUIO_VALUE_INT ||
                d.kind == ECUIO_VALUE_FLOAT || d.kind == ECUIO_VALUE_ENUM);
    }
    return false;
}

/* The tables (ADR-0051): NULL only when empty, each def's range within
 * its table, each enum def's raw values strictly ascending, each label
 * and each program, then each program's inputs */
static int check_tables(const ecuio_signal_set_t* set, uint32_t* index)
{
    if ((set->enum_count > 0 && !set->enums) ||
        (set->computed_count > 0 && !set->computed) ||
        set->enum_count > ECUIO_SET_MAX_TABLE ||
        set->computed_count > ECUIO_SET_MAX_TABLE) {
        return ECUIO_VALIDATE_ERR_TABLES_NULL;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.flags & ECUIO_DEF_FLAG_COMPUTED) {
            if (d.table_first >= set->computed_count) {
                *index = i;
                return ECUIO_VALIDATE_ERR_TABLE_RANGE;
            }
        } else if (d.kind == ECUIO_VALUE_ENUM) {
            if (uint32_t{d.table_first} + d.table_count > set->enum_count) {
                *index = i;
                return ECUIO_VALIDATE_ERR_TABLE_RANGE;
            }
            for (uint32_t k = 1; k < d.table_count; ++k) {
                const uint32_t e = uint32_t{d.table_first} + k;
                if (set->enums[e].raw <= set->enums[e - 1].raw) {
                    *index = e;
                    return ECUIO_VALIDATE_ERR_ENUM_ENTRY;
                }
            }
        }
    }
    for (uint32_t e = 0; e < set->enum_count; ++e) {
        if (!label_ok(set->enums[e])) {
            *index = e;
            return ECUIO_VALIDATE_ERR_ENUM_ENTRY;
        }
    }
    for (uint32_t p = 0; p < set->computed_count; ++p) {
        if (!ecuio_computed_check(set->computed[p])) {
            *index = p;
            return ECUIO_VALIDATE_ERR_PROGRAM;
        }
    }
    for (uint32_t p = 0; p < set->computed_count; ++p) {
        const ecuio_computed_t& c = set->computed[p];
        for (uint32_t k = 0; k < c.input_count; ++k) {
            if (!numeric_input(set, c.input_ids[k])) {
                *index = p;
                return ECUIO_VALIDATE_ERR_COMPUTED_INPUT;
            }
        }
    }
    return ECUIO_VALIDATE_OK;
}

/* The set checks, in order. A code that names one def, channel, entry
 * or program sets *index to it; the others leave it as the caller set
 * it. */
static int check_set(const ecuio_signal_set_t* set, uint32_t* index)
{
    if (!set) return ECUIO_VALIDATE_ERR_NULL;
    /* Before any other member: a set built for another layout has its
     * fields elsewhere. */
    if (set->layout_version != ECUIO_SIGNAL_SET_LAYOUT) return ECUIO_VALIDATE_ERR_LAYOUT;
    if (set->reserved0 != 0) return ECUIO_VALIDATE_ERR_SET_RESERVED;
    for (uint32_t i = 0; i < sizeof(set->reserved) / sizeof(set->reserved[0]); ++i) {
        if (set->reserved[i] != 0) return ECUIO_VALIDATE_ERR_SET_RESERVED;
    }
    /* An empty set (no defs) is legal: it decodes nothing, and may
     * still declare channels. */
    if (set->count > 0 && !set->defs) return ECUIO_VALIDATE_ERR_NULL;
    for (uint32_t i = 0; i < set->count; ++i) {
        int rc = ecuio_validate_signal(&set->defs[i]);
        if (rc != ECUIO_VALIDATE_OK) {
            *index = i;
            return rc;
        }
    }
    /* The bridge keys its staging buffer and model rows by def id, so
     * two defs with one id would overwrite each other's values. */
    for (uint32_t i = 0; i < set->count; ++i) {
        for (uint32_t j = 0; j < i; ++j) {
            if (set->defs[j].id == set->defs[i].id) {
                *index = i;   /* the second of the pair */
                return ECUIO_VALIDATE_ERR_DUPLICATE_ID;
            }
        }
    }
    /* ADR-0013 section 4: channel declarations, bounded, legal CAN
     * ids, unique rx. Negative codes so they can't collide with
     * ecuio_validate_signal's per-def returns. */
    if (set->channel_count > 0) {
        if (!set->channels) return ECUIO_VALIDATE_ERR_CHANNELS_NULL;
        if (set->channel_count > ECUIO_MAX_CHANNELS) return ECUIO_VALIDATE_ERR_CHANNEL_COUNT;
        for (uint32_t i = 0; i < set->channel_count; ++i) {
            const uint32_t rx = set->channels[i].rx_id;
            const uint32_t tx = set->channels[i].tx_id;
            if (rx == 0 || rx > 0x1FFFFFFFu ||
                tx == 0 || tx > 0x1FFFFFFFu) {
                *index = i;
                return ECUIO_VALIDATE_ERR_CHANNEL_ID;
            }
            for (uint32_t j = 0; j < i; ++j) {
                if (set->channels[j].rx_id == rx) {
                    *index = i;   /* the second of the pair */
                    return ECUIO_VALIDATE_ERR_DUPLICATE_RX;
                }
            }
        }
    }
    return check_tables(set, index);
}

int ecuio_validate_signal_set(const ecuio_signal_set_t* set,
                              ecuio_validation_t* out)
{
    uint32_t index = UINT32_MAX;
    const int code = check_set(set, &index);
    if (out) {
        ecuio_validation_t result = {};   /* reserved zero */
        result.code = code;
        result.index = index;
        *out = result;
    }
    return code;
}
