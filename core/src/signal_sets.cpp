// File/layer: core/src/signal_sets.cpp | C++17, Qt-free, every platform
// Testing: UNIT: Obd2SetTest, Obd2DecodeTest and Obd2MuxTest (test_obd2)
//          for the obd2 set; CoreTest.MockFramesDecodeAgainstMockSignalSet
//          and SignalsTest for the mock set; every set passes the
//          validator at registration

#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
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
                             const char* unit, uint8_t dp)
{
    ecuio_signal_def_t d = {};   /* reserved bytes and flags zero */
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

/* Names, units and decimal places are display strings: the UI shows
 * them as they are (UTF-8). The names match the obd2 set's, so a gauge
 * finds the same signal under either profile. */
#define DEG_C "\xC2\xB0" "C"   /* UTF-8 degree sign, whatever the source encoding */
#define LAMBDA "\xCE\xBB"      /* UTF-8 Greek small lambda */

static const ecuio_signal_def_t kMockDefs[] = {
    mk(ECUIO_MOCK_SIG_RPM_ID,       "RPM",               ECUIO_VALUE_UINT, 0, 0, 16, 0.25,  0.0,  "RPM",    0),
    /* Coolant and IAT are 8-bit UINT with a -40 offset (J1979: A - 40,
     * so -40..215 degC). As INT, anything from 88 degC up would read
     * as about -160 degC. */
    mk(ECUIO_MOCK_SIG_COOLANT_ID,   "Coolant Temp",      ECUIO_VALUE_UINT, 2, 0, 8,   1.0, -40.0, DEG_C,    0),
    mk(ECUIO_MOCK_SIG_SPEED_ID,     "Vehicle Speed",     ECUIO_VALUE_UINT, 3, 0, 8,   1.0,   0.0, "km/h",   0),
    mk(ECUIO_MOCK_SIG_THROTTLE_ID, "Throttle Position", ECUIO_VALUE_UINT,  4, 0, 8,   100.0/255.0, 0.0, "%", 1),
    mk(ECUIO_MOCK_SIG_IAT_ID,       "Intake Air Temp",   ECUIO_VALUE_UINT, 5, 0, 8,   1.0, -40.0, DEG_C,    0),
    mk(ECUIO_MOCK_SIG_LOAD_ID,      "Engine Load",       ECUIO_VALUE_UINT, 6, 0, 8,   100.0/255.0, 0.0, "%", 1),
    /* Layout mirrors encode_payload's tail: MAF at bytes 7-8 (LE,
     * like RPM), oil temp at byte 9. Centi-grams/s scale is the
     * same J1979 0x10 formula the obd2 set uses; oil is A - 40. The
     * 0.01 g/s resolution is shown to one decimal. */
    mk(ECUIO_MOCK_SIG_MAF_ID,       "MAF",               ECUIO_VALUE_UINT, 7, 0, 16, 0.01,  0.0, "g/s",  1),
    mk(ECUIO_MOCK_SIG_OIL_ID,       "Engine Oil Temp",   ECUIO_VALUE_UINT, 9, 0, 8,   1.0, -40.0, DEG_C, 0),
    /* Wideband lambda at bytes 10-11, LE, scaled like J1979's
     * equivalence ratio (2/65536, so 0 to just under 2). Only the mock
     * carries it; the obd2 set doesn't poll an O2 sensor. */
    mk(ECUIO_MOCK_SIG_LAMBDA_ID,    "Lambda",            ECUIO_VALUE_UINT, 10, 0, 16, 1.0/32768.0, 0.0, LAMBDA, 2),
};

#undef DEG_C
#undef LAMBDA

static const ecuio_signal_set_t kMockSet = [] {
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(sizeof(kMockDefs) / sizeof(kMockDefs[0]));
    s.defs = kMockDefs;
    return s;
}();

const ecuio_signal_set_t* ecuio_mock_signal_set(void)
{
    return &kMockSet;
}

/* The set checks, in order. A code that names one def or channel sets
 * *index to it; the others leave it as the caller set it. */
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
    /* ADR-0013 §4: channel declarations, bounded, legal CAN
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
    return ECUIO_VALIDATE_OK;
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
