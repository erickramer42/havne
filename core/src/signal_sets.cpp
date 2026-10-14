#include <qttune/signals.h>
#include <qttune/signal_sets.h>
#include <string.h>

/* Safe copy into a fixed-width, always-null-terminated char buffer.
 * Terminates, never overruns, doesn't zero-pad the tail like strncpy
 * (irrelevant here, but why keep the debate alive). */
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
    copy_str(d.name, sizeof(d.name), name);
    copy_str(d.unit, sizeof(d.unit), unit);
    return d;
}

static const QttuneSignalDef kMockDefs[] = {
    mk(MOCK_SIG_RPM_ID,      "Engine_RPM",       QTTUNE_SIGNAL_TYPE_UINT16,  0, 2, 0, 16, 0.25f,  0.0f,  "RPM",    0),
    mk(MOCK_SIG_COOLANT_ID,  "Coolant_Temp",     QTTUNE_SIGNAL_TYPE_INT8,    2, 1, 0, 8,   1.0f, -40.0f, "degC",   0),
    mk(MOCK_SIG_SPEED_ID,    "Vehicle_Speed",    QTTUNE_SIGNAL_TYPE_UINT8,   3, 1, 0, 8,   1.0f,   0.0f, "km/h",   0),
    mk(MOCK_SIG_THROTTLE_ID, "Throttle_Pos",     QTTUNE_SIGNAL_TYPE_UINT8,   4, 1, 0, 8,   100.0f/255.0f, 0.0f, "%", 1),
    mk(MOCK_SIG_IAT_ID,      "Intake_Air_Temp",  QTTUNE_SIGNAL_TYPE_INT8,   5, 1, 0, 8,   1.0f, -40.0f, "degC",   0),
    mk(MOCK_SIG_LOAD_ID,     "Engine_Load",      QTTUNE_SIGNAL_TYPE_UINT8,   6, 1, 0, 8,   100.0f/255.0f, 0.0f, "%", 1),
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
    return 0;
}
