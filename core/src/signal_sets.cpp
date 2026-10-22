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
                             ecuio_signal_type_t type,
                             uint8_t sb, uint8_t nb, uint8_t bo, uint8_t bl,
                             double scale, double offset,
                             const char* unit, uint8_t dp)
{
    ecuio_signal_def_t d = {};
    d.id = id;
    d.type = type;
    d.start_byte = sb;
    d.num_bytes = nb;
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
    d.flags = 0;               /* ECUIO_SIGNAL_FLAG_BIG_ENDIAN unset */
    d.mux_offset = 0;
    d.mux_length = 0;           /* mux disabled */
    d.mux_reserved[0] = 0;
    d.mux_reserved[1] = 0;
    d.mux_key = 0;
    copy_str(d.name, sizeof(d.name), name);
    copy_str(d.unit, sizeof(d.unit), unit);
    return d;
}

/* Names, units and decimal places are display strings: the UI shows
 * them as they are (UTF-8). The names match the obd2 set's, so a gauge
 * finds the same signal under either profile. */
#define DEG_C "\xC2\xB0" "C"   /* UTF-8 degree sign, whatever the source encoding */

static const ecuio_signal_def_t kMockDefs[] = {
    mk(ECUIO_MOCK_SIG_RPM_ID,       "RPM",               ECUIO_SIGNAL_TYPE_UINT16,   0, 2, 0, 16, 0.25,  0.0,  "RPM",    0),
    /* Coolant and IAT are UINT8 with a -40 offset (J1979: A - 40, so
     * -40..215 degC). As INT8, anything from 88 degC up would read as
     * about -160 degC. */
    mk(ECUIO_MOCK_SIG_COOLANT_ID,   "Coolant Temp",      ECUIO_SIGNAL_TYPE_UINT8,     2, 1, 0, 8,   1.0, -40.0, DEG_C,    0),
    mk(ECUIO_MOCK_SIG_SPEED_ID,     "Vehicle Speed",     ECUIO_SIGNAL_TYPE_UINT8,     3, 1, 0, 8,   1.0,   0.0, "km/h",   0),
    mk(ECUIO_MOCK_SIG_THROTTLE_ID, "Throttle Position", ECUIO_SIGNAL_TYPE_UINT8,      4, 1, 0, 8,   100.0/255.0, 0.0, "%", 1),
    mk(ECUIO_MOCK_SIG_IAT_ID,       "Intake Air Temp",   ECUIO_SIGNAL_TYPE_UINT8,     5, 1, 0, 8,   1.0, -40.0, DEG_C,    0),
    mk(ECUIO_MOCK_SIG_LOAD_ID,      "Engine Load",       ECUIO_SIGNAL_TYPE_UINT8,     6, 1, 0, 8,   100.0/255.0, 0.0, "%", 1),
    /* Layout mirrors encode_payload's tail: MAF at bytes 7-8 (LE,
     * like RPM), oil temp at byte 9. Centi-grams/s scale is the
     * same J1979 0x10 formula the obd2 set uses; oil is A - 40. The
     * 0.01 g/s resolution is shown to one decimal. */
    mk(ECUIO_MOCK_SIG_MAF_ID,       "MAF",               ECUIO_SIGNAL_TYPE_UINT16,   7, 2, 0, 16, 0.01,  0.0, "g/s",  1),
    mk(ECUIO_MOCK_SIG_OIL_ID,       "Engine Oil Temp",   ECUIO_SIGNAL_TYPE_UINT8,     9, 1, 0, 8,   1.0, -40.0, DEG_C, 0),
};

#undef DEG_C

static const ecuio_signal_set_t kMockSet = [] {
    ecuio_signal_set_t s = {};
    s.version = 1;
    s.count = static_cast<uint32_t>(sizeof(kMockDefs) / sizeof(kMockDefs[0]));
    s.defs = kMockDefs;
    return s;
}();

const ecuio_signal_set_t* ecuio_mock_signal_set(void)
{
    return &kMockSet;
}

int ecuio_validate_signal_set(const ecuio_signal_set_t* set)
{
    /* An empty set (no defs) is legal: it decodes nothing, and may
     * still declare channels. */
    if (!set || (set->count > 0 && !set->defs)) return ECUIO_VALIDATE_ERR_NULL;
    for (uint32_t i = 0; i < set->count; ++i) {
        int rc = ecuio_validate_signal(&set->defs[i]);
        if (rc != ECUIO_VALIDATE_OK) return rc;
    }
    /* The bridge keys its staging buffer and model rows by def id, so
     * two defs with one id would overwrite each other's values. */
    for (uint32_t i = 0; i < set->count; ++i) {
        for (uint32_t j = 0; j < i; ++j) {
            if (set->defs[j].id == set->defs[i].id) {
                return ECUIO_VALIDATE_ERR_DUPLICATE_ID;
            }
        }
    }
    /* ADR-0013 §4: channel declarations — bounded, legal CAN
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
                return ECUIO_VALIDATE_ERR_CHANNEL_ID;
            }
            for (uint32_t j = 0; j < i; ++j) {
                if (set->channels[j].rx_id == rx) return ECUIO_VALIDATE_ERR_DUPLICATE_RX;
            }
        }
    }
    return ECUIO_VALIDATE_OK;
}
