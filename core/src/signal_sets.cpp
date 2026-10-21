#include <qttune/signals.h>
#include <qttune/signal_sets.h>
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

static QttuneSignalDef mk(uint16_t id, const char* name,
                          QttuneSignalType type,
                          uint8_t sb, uint8_t nb, uint8_t bo, uint8_t bl,
                          float scale, float offset,
                          const char* unit, uint8_t dp)
{
    QttuneSignalDef d = {};
    d.id = id;
    d.type = type;
    d.start_byte = sb;
    d.num_bytes = nb;
    d.bit_offset = bo;
    d.bit_length = bl;
    d.scale = scale;
    d.offset = offset;
    d.decimal_places = dp;
    d.message_id =  MOCK_TELEMETRY_ARB_ID;   /* all mock signals ride 0x0C0 */
    /* The mock set is deliberately mux-less and little-endian, matching
     * the plain LE payloads mock_transport.cpp encodes. d = {} already
     * zeroes these; stating them keeps a future default from silently
     * turning on mux or big-endian decode here. */
    d.flags = 0;               /* QTTUNE_SIGNAL_FLAG_BIG_ENDIAN unset */
    d.mux_offset = 0;
    d.mux_length = 0;           /* mux disabled */
    d.mux_reserved[0] = 0;
    d.mux_reserved[1] = 0;
    d.mux_key = 0;
    copy_str(d.name, sizeof(d.name), name);
    copy_str(d.unit, sizeof(d.unit), unit);
    return d;
}

static const QttuneSignalDef kMockDefs[] = {
    mk(MOCK_SIG_RPM_ID,      "Engine_RPM",       QTTUNE_SIGNAL_TYPE_UINT16,  0, 2, 0, 16, 0.25f,  0.0f,  "RPM",    0),
    /* Coolant and IAT are UINT8 with a -40 offset (J1979: A - 40, so
     * -40..215 degC). As INT8, anything from 88 degC up would read as
     * about -160 degC. */
    mk(MOCK_SIG_COOLANT_ID,  "Coolant_Temp",     QTTUNE_SIGNAL_TYPE_UINT8,    2, 1, 0, 8,   1.0f, -40.0f, "degC",   0),
    mk(MOCK_SIG_SPEED_ID,    "Vehicle_Speed",    QTTUNE_SIGNAL_TYPE_UINT8,    3, 1, 0, 8,   1.0f,   0.0f, "km/h",   0),
    mk(MOCK_SIG_THROTTLE_ID, "Throttle_Pos",    QTTUNE_SIGNAL_TYPE_UINT8,    4, 1, 0, 8,   100.0f/255.0f, 0.0f, "%", 1),
    mk(MOCK_SIG_IAT_ID,      "Intake_Air_Temp",  QTTUNE_SIGNAL_TYPE_UINT8,    5, 1, 0, 8,   1.0f, -40.0f, "degC",   0),
    mk(MOCK_SIG_LOAD_ID,     "Engine_Load",     QTTUNE_SIGNAL_TYPE_UINT8,    6, 1, 0, 8,   100.0f/255.0f, 0.0f, "%", 1),
    /* Layout mirrors encode_payload's tail: MAF at bytes 7-8 (LE,
     * like RPM), oil temp at byte 9. Centi-grams/s scale is the
     * same J1979 0x10 formula the obd2 set uses; oil is A - 40. */
    mk(MOCK_SIG_MAF_ID,      "Mass_Air_Flow",    QTTUNE_SIGNAL_TYPE_UINT16,  7, 2, 0, 16, 0.01f,  0.0f, "g/s",  2),
    mk(MOCK_SIG_OIL_ID,      "Oil_Temp",         QTTUNE_SIGNAL_TYPE_UINT8,    9, 1, 0, 8,   1.0f, -40.0f, "degC", 0),
};

static const QttuneSignalSet kMockSet = [] {
    QttuneSignalSet s = {};
    s.version = 1;
    s.count = static_cast<uint32_t>(sizeof(kMockDefs) / sizeof(kMockDefs[0]));
    s.defs = kMockDefs;
    return s;
}();

const QttuneSignalSet* qttune_mock_signal_set(void)
{
    return &kMockSet;
}

int qttune_validate_signal_set(const QttuneSignalSet* set)
{
    if (!set || !set->defs) return 1;
    for (uint32_t i = 0; i < set->count; ++i) {
        int rc = qttune_validate_signal(&set->defs[i]);
        if (rc != 0) return rc;
    }
    /* ADR-0013 §4: channel declarations — bounded, legal CAN
     * ids, unique rx. Negative codes so they can't collide with
     * qttune_validate_signal's per-def returns. */
    if (set->channel_count > 0) {
        if (!set->channels) return -10;
        if (set->channel_count > QTTUNE_MAX_CHANNELS) return -11;
        for (uint32_t i = 0; i < set->channel_count; ++i) {
            const uint32_t rx = set->channels[i].rx_id;
            const uint32_t tx = set->channels[i].tx_id;
            if (rx == 0 || rx > 0x1FFFFFFFu ||
                tx == 0 || tx > 0x1FFFFFFFu) {
                return -12;
            }
            for (uint32_t j = 0; j < i; ++j) {
                if (set->channels[j].rx_id == rx) return -13;
            }
        }
    }
    return 0;
}
