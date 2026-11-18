// File/layer: core/src/health_build.cpp | C++17, Qt-free; the health
//             report's model built from its inputs (health_model.h)
// Testing: UNIT: HealthTest (test_core)
//
// The words the model holds, each ECU's facts, the key values, and the
// number and time texts (health.h's fixed and utc_text).

#include "health_model.h"

#include "pid_support.h" /* ecuio_service01_keyed */
#include "scrub.h"

#include "ecuio/profile.h"
#include "ecuio/vin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ecuio_health {

namespace {

namespace scrub = ecuio_scrub;

/* Service 01's since-the-codes-were-cleared PIDs, in PID order: distance
 * with the MIL on, warm-ups, distance, time with the MIL on, time */
constexpr uint8_t kSinceCleared[] = {0x21, 0x30, 0x31, 0x4D, 0x4E};

bool since_cleared_pid(const ecuio_signal_def_t& d)
{
    if (!ecuio_service01_keyed(d)) {
        return false;
    }
    const uint8_t pid = static_cast<uint8_t>(d.mux_key & 0xFFu);
    return std::find(std::begin(kSinceCleared), std::end(kSinceCleared), pid) !=
           std::end(kSinceCleared);
}

} // namespace

bool numeric(const ecuio_signal_def_t& d)
{
    return d.kind != ECUIO_VALUE_TEXT && d.kind != ECUIO_VALUE_HEX;
}

namespace {

/* An item's outcome; one a recording holds no reply for is "not
 * recorded", since a recording can't say whether it was asked */
const char* outcome_word(uint8_t o, bool recorded = false)
{
    switch (o) {
    case ECUIO_DIAG_ANSWERED:
        return "answered";
    case ECUIO_DIAG_REFUSED:
        return "refused";
    case ECUIO_DIAG_UNANSWERED:
        return "unanswered";
    case ECUIO_DIAG_UNSUPPORTED:
        return "unsupported";
    default:
        return recorded ? "not recorded" : "unasked";
    }
}

const char* backend_word(uint32_t b)
{
    switch (b) {
    case ECUIO_BACKEND_MOCK:
        return "mock";
    case ECUIO_BACKEND_J2534_SURROGATE:
        return "j2534";
    case ECUIO_BACKEND_REPLAY:
        return "replay";
    case ECUIO_BACKEND_ELM327:
        return "elm327";
    case ECUIO_BACKEND_SOCKETCAN:
        return "socketcan";
    default:
        return "unknown";
    }
}

/* A recording's backend, by its URI's scheme */
const char* scheme_word(const std::string& uri)
{
    static const char* const kSchemes[] = {"mock", "j2534", "replay", "elm327",
                                           "socketcan"};
    const size_t end = uri.find("://");
    for (const char* k : kSchemes) {
        if (end != std::string::npos && uri.compare(0, end, k) == 0) {
            return k;
        }
    }
    return "unknown";
}

std::string hex_id(uint32_t id)
{
    char b[16];
    std::snprintf(b, sizeof b, id > 0x7FFu ? "0x%08X" : "0x%03X", id);
    return b;
}

std::string reply_words(uint8_t service, uint8_t nrc)
{
    char b[16];
    std::snprintf(b, sizeof b, "7F %02X %02X", service, nrc);
    return b;
}

std::string dtc_text(uint16_t code)
{
    char b[8] = {};
    (void)ecuio_dtc_format(code, b, sizeof b);
    return b;
}

const ecuio_signal_def_t* def_of(const ecuio_signal_set_t* set, uint32_t id)
{
    if (set == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

/* A def's value as shown, in the units asked; false for a value that
 * can't be written (not finite) */
bool show(const ecuio_signal_set_t* set, const ecuio_signal_def_t& d, double v,
          uint32_t units, shown* out)
{
    if (!std::isfinite(v)) {
        return false;
    }
    out->name = d.name;
    /* a label only for a raw value a table can hold: whole, 0 to
     * UINT32_MAX (a caller's value past it converts to no uint32_t) */
    if (d.kind == ECUIO_VALUE_ENUM && v >= 0 && v <= 4294967295.0 &&
        v == std::floor(v)) {
        const char* label = ecuio_enum_label(set, &d, static_cast<uint32_t>(v));
        if (label != nullptr) {
            out->value = label;
            out->number = false;
            return true;
        }
    }
    ecuio_display_value_t dv{};
    if (ecuio_display_convert(d.unit, d.decimal_places, v, units, &dv) !=
        ECUIO_DISPLAY_OK) {
        return false;
    }
    out->value = fixed(dv.value, dv.decimal_places);
    out->unit = dv.unit;
    return true;
}

void with_age(const ecuio_health_value_t& v, shown* s)
{
    s->has_age = true;
    s->age_ms = v.age_us > UINT64_MAX - 500 ? UINT64_MAX / 1000
                                            : (v.age_us + 500) / 1000;
    s->stale = v.age_us > ECUIO_VALUE_STALE_US;
}

ecu ecu_of(const inputs& in, uint32_t c, const ecuio_signal_set_t* obd2,
           model& m, scrub::values* identity)
{
    const ecuio_channel_diagnostics_t& d = in.diagnostics[c];
    ecu e;
    e.id = hex_id(d.rx_id != 0 ? d.rx_id : in.identity[c].rx_id);
    const bool rec = in.recording;
    if (rec) {
        e.state = "recorded";
    } else if ((d.flags & ECUIO_DIAG_ASKED) == 0) {
        e.state = "not asked";
    } else if ((d.flags & ECUIO_DIAG_COMPLETE) == 0) {
        e.state = "reading";
    }
    e.readiness = outcome_word(d.readiness, rec);
    if (d.readiness == ECUIO_DIAG_REFUSED) {
        e.readiness_reply = reply_words(0x01, d.readiness_nrc);
    }
    e.readiness_known = (d.flags & ECUIO_DIAG_READINESS) != 0;
    e.mil = (d.flags & ECUIO_DIAG_MIL_ON) != 0;
    e.dtc_count = d.dtc_count;
    const uint8_t mn =
        std::min<uint8_t>(d.monitor_count, ECUIO_DIAG_MAX_MONITORS);
    for (uint8_t i = 0; i < mn; ++i) {
        monitor mo;
        mo.name = ecuio_monitor_name(d.monitors[i].id, d.flags);
        if (mo.name.empty()) {
            continue;
        }
        mo.complete = (d.monitors[i].flags & ECUIO_MONITOR_COMPLETE) != 0;
        mo.cycle = (d.flags & ECUIO_DIAG_CYCLE) != 0;
        mo.cycle_enabled =
            (d.monitors[i].flags & ECUIO_MONITOR_CYCLE_ENABLED) != 0;
        mo.cycle_complete =
            (d.monitors[i].flags & ECUIO_MONITOR_CYCLE_COMPLETE) != 0;
        e.monitors.push_back(mo);
    }
    static const char* const kKeys[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                        "permanent"};
    static const char* const kLabels[ECUIO_DIAG_LISTS] = {
        "Stored codes", "Pending codes", "Permanent codes"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& src = d.lists[l];
        dtc_list& dl = e.lists[l];
        dl.key = kKeys[l];
        dl.label = kLabels[l];
        dl.service = kServices[l];
        dl.outcome = outcome_word(src.outcome, rec);
        if (src.outcome == ECUIO_DIAG_REFUSED) {
            dl.reply = reply_words(kServices[l], src.nrc);
        }
        const uint8_t held = std::min<uint8_t>(src.held, ECUIO_DIAG_MAX_DTCS);
        for (uint8_t i = 0; i < held; ++i) {
            dl.codes.push_back(
                {dtc_text(src.codes[i]), ecuio_dtc_title(src.codes[i])});
        }
        dl.more = src.reported > held ? src.reported - held : 0u;
    }
    e.freeze = outcome_word(d.freeze, rec);
    if (d.freeze == ECUIO_DIAG_REFUSED) {
        e.freeze_reply = reply_words(0x02, d.freeze_nrc);
    }
    if ((d.flags & ECUIO_DIAG_FREEZE_FRAME) != 0 && d.freeze_dtc != 0) {
        e.freeze_stored = true;
        e.freeze_code = {dtc_text(d.freeze_dtc), ecuio_dtc_title(d.freeze_dtc)};
    }
    const uint8_t fn = std::min<uint8_t>(d.freeze_count, ECUIO_DIAG_MAX_FREEZE);
    for (uint8_t i = 0; i < fn; ++i) {
        const ecuio_signal_def_t* def = def_of(obd2, d.freeze_values[i].def_id);
        shown s;
        if (def != nullptr &&
            show(obd2, *def, d.freeze_values[i].value, in.units, &s)) {
            e.freeze_values.push_back(s);
        }
    }
    /* The since-cleared values on this ECU's reply ID, in PID order */
    for (uint8_t pid : kSinceCleared) {
        for (uint32_t i = 0; i < in.value_count; ++i) {
            const ecuio_signal_def_t* def = def_of(in.set, in.values[i].def_id);
            shown s;
            if (def != nullptr && since_cleared_pid(*def) &&
                (def->mux_key & 0xFFu) == pid && def->message_id == d.rx_id &&
                show(in.set, *def, in.values[i].value, in.units, &s)) {
                with_age(in.values[i], &s);
                e.since_cleared.push_back(s);
                break;
            }
        }
    }
    const ecuio_channel_identity_t& id = in.identity[c];
    const uint32_t items =
        std::min<uint32_t>(id.items, ECUIO_IDENTITY_MAX_ITEMS);
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = id.list[i];
        item x;
        x.name = ecuio_identity_request_name(it.request, it.length);
        if (x.name.empty()) {
            char b[16];
            std::snprintf(b, sizeof b,
                          it.length == 3 ? "%02X %02X%02X" : "%02X %02X",
                          it.request[0], it.request[1], it.request[2]);
            x.name = b;
        }
        x.outcome = outcome_word(it.outcome, rec);
        if (it.outcome == ECUIO_IDENTITY_ANSWERED) {
            ++m.identification_left_out;
            const size_t n = strnlen(it.text, sizeof it.text);
            if (identity != nullptr && n > 0) {
                (void)identity->add(it.text, n,
                                    ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION);
            }
        }
        e.identification.push_back(x);
    }
    return e;
}

} // namespace

model build(const inputs& in, scrub::values* identity)
{
    model m;
    if (!in.uri.empty()) {
        m.uri = scrub::uri(in.uri); /* a recording may have none */
    }
    if (in.recording) {
        m.recording = true;
        m.frames = in.frames;
        m.backend = scheme_word(in.uri);
        m.profile = in.profile;
        m.has_started = in.started_us != 0;
        if (m.has_started) {
            m.started = utc_text(in.started_us);
        }
        m.from_s = fixed(static_cast<double>(in.from_us) / 1e6, 3);
        m.to_s = fixed(static_cast<double>(in.to_us) / 1e6, 3);
    } else if (in.info != nullptr) {
        m.backend = backend_word(in.info->backend);
        m.baud = in.info->baud;
        m.profile =
            std::string(in.info->profile,
                        strnlen(in.info->profile, sizeof in.info->profile));
    }
    if (in.utc_us != 0) {
        m.has_utc = true;
        m.utc = utc_text(in.utc_us);
    }
    m.units = in.units == ECUIO_UNITS_IMPERIAL ? "imperial" : "metric";
    const baseline_facts& b = in.baseline;
    m.baseline_named = b.named;
    if (b.named && !b.read) {
        m.baseline_why = b.why;
    } else if (b.named) {
        m.baseline_read = true;
        m.baseline_has_started = b.started_us != 0;
        if (m.baseline_has_started) {
            m.baseline_started = utc_text(b.started_us);
        }
        m.baseline_has_length = b.has_length;
        m.baseline_length_s = fixed(static_cast<double>(b.length_us) / 1e6, 3);
        m.baseline_profile = b.profile;
        m.baseline_backend = scheme_word(b.uri);
        /* A recording's start is its own: a copy of the baseline (the
         * folder export) has it too, and is the same run */
        m.baseline_is_source =
            in.recording && in.started_us != 0 && b.started_us == in.started_us;
    }

    /* The vehicle: the first ECU's VIN that decodes, never the VIN */
    for (uint32_t c = 0; c < in.channels && !m.vin; ++c) {
        char vin[ECUIO_VIN_TEXT_LEN] = {};
        ecuio_vin_info_t vi{};
        if (ecuio_identity_vin(&in.identity[c], vin, sizeof vin) == ECUIO_OK &&
            vin[0] != '\0' &&
            ecuio_vin_decode(vin, in.latest_model_year, &vi) == ECUIO_OK) {
            m.vin = true;
            m.model_year = vi.model_year;
            m.make = ecuio_vin_make(vi.wmi);
            m.check_matches = (vi.flags & ECUIO_VIN_CHECK_MATCHES) != 0;
        }
    }
    (void)ecuio_diag_inspect(in.diagnostics, in.channels, m.model_year,
                             &m.inspection);
    for (uint8_t id = 0; id < 16; ++id) {
        if ((m.inspection.incomplete_ids & (1u << id)) != 0) {
            const char* name = ecuio_monitor_name(id, m.inspection.diag_flags);
            if (name[0] != '\0') {
                m.incomplete.emplace_back(name);
            }
        }
    }

    const ecuio_signal_set_t* obd2 = nullptr;
    (void)ecuio_profile_lookup("obd2", &obd2);
    for (uint32_t c = 0; c < in.channels; ++c) {
        m.ecus.push_back(ecu_of(in, c, obd2, m, identity));
    }

    /* The key values: the set's ranked signals among the values given,
     * by rank, then the set's order; the since-cleared PIDs aren't
     * repeated here */
    struct ranked {
        uint8_t rank;
        uint32_t place;
        const ecuio_signal_def_t* def;
        const ecuio_health_value_t* value;
    };
    std::vector<ranked> keys;
    if (in.set != nullptr) {
        for (uint32_t i = 0; i < in.set->count; ++i) {
            const ecuio_signal_def_t& d = in.set->defs[i];
            if (d.plot_rank == 0 || !numeric(d)) {
                continue;
            }
            m.ranks = true;
            if (since_cleared_pid(d)) {
                continue;
            }
            for (uint32_t v = 0; v < in.value_count; ++v) {
                if (in.values[v].def_id == d.id) {
                    keys.push_back({d.plot_rank, i, &d, &in.values[v]});
                    break;
                }
            }
        }
    }
    std::stable_sort(
        keys.begin(), keys.end(), [](const ranked& a, const ranked& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.place < b.place;
        });
    for (const ranked& k : keys) {
        shown s;
        if (show(in.set, *k.def, k.value->value, in.units, &s)) {
            with_age(*k.value, &s);
            m.key_values.push_back(s);
        }
    }

    /* A since-cleared value on a reply ID no ECU is listed for (a
     * recording whose range holds none of that ECU's replies): after the
     * ranked ones, in PID order, so the report still shows it */
    const auto listed = [&in](uint32_t id) {
        for (uint32_t c = 0; c < in.channels; ++c) {
            if (in.diagnostics[c].rx_id == id) {
                return true;
            }
        }
        return false;
    };
    for (uint8_t pid : kSinceCleared) {
        for (uint32_t i = 0; i < in.value_count; ++i) {
            const ecuio_signal_def_t* def = def_of(in.set, in.values[i].def_id);
            shown s;
            if (def != nullptr && since_cleared_pid(*def) &&
                (def->mux_key & 0xFFu) == pid && !listed(def->message_id) &&
                show(in.set, *def, in.values[i].value, in.units, &s)) {
                with_age(in.values[i], &s);
                m.key_values.push_back(s);
                ++m.key_since_cleared;
            }
        }
    }
    return m;
}

std::string fixed(double v, uint8_t dp)
{
    dp = std::min<uint8_t>(dp, 6);
    uint64_t scale = 1;
    for (uint8_t i = 0; i < dp; ++i) {
        scale *= 10;
    }
    const double scaled = std::fabs(v) * static_cast<double>(scale);
    if (!(scaled < 9.0e15)) {
        return v < 0 ? "-9e15" : "9e15";
    }
    const auto n = static_cast<uint64_t>(std::llround(scaled));
    std::string out = (v < 0 && n != 0) ? "-" : "";
    out += std::to_string(n / scale);
    if (dp > 0) {
        std::string frac = std::to_string(n % scale);
        out += '.';
        out.append(dp - frac.size(), '0');
        out += frac;
    }
    return out;
}

std::string utc_text(uint64_t utc_us)
{
    /* ISO 8601's last four-digit second, 9999-12-31T23:59:59Z: a later
       stamp (a header's garbage) reads as it */
    constexpr uint64_t kLastUs = 253402300799999999u;
    utc_us = (std::min)(utc_us, kLastUs);
    /* Civil from days (Howard Hinnant's algorithm), no gmtime */
    const uint64_t secs = utc_us / 1000000u;
    const int64_t days = static_cast<int64_t>(secs / 86400u);
    const uint64_t rem = secs % 86400u;
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<uint64_t>(z - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
    /* Each field's remainder changes no value in range: it lets the
       compiler bound the widths (GCC's -Wformat-truncation) */
    char b[32];
    std::snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  static_cast<unsigned>(
                      static_cast<uint64_t>(y + (mo <= 2 ? 1 : 0)) % 10000u),
                  static_cast<unsigned>(mo % 100),
                  static_cast<unsigned>(d % 100),
                  static_cast<unsigned>(rem / 3600 % 100),
                  static_cast<unsigned>(rem % 3600 / 60),
                  static_cast<unsigned>(rem % 60));
    return b;
}

} // namespace ecuio_health
