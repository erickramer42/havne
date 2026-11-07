// File/layer: core/src/health.cpp | C++17, Qt-free; a car's health as one
//             scrubbed report (ADR-0054's appendix "the health report as
//             built")
// Testing: UNIT: HealthTest (test_core)
//
// One model of the facts, built once from the inputs (health.h), then
// written as JSON or text by two writers that read only the model. The
// identification items' text and the VIN never enter the model: the
// vehicle is the VIN's decode, an item its name and outcome. The file is
// then checked for those texts and for the VIN's shape, as an export's
// (scrub.h), under <out>.partial, renamed into place once it passes.

#include "health.h"

#include "error_detail.h"
#include "folder_files.h"
#include "pid_support.h" /* ecuio_service01_keyed */
#include "scrub.h"
#include "session.h"

#include "ecuio/profile.h"
#include "ecuio/vin.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
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

bool numeric(const ecuio_signal_def_t& d)
{
    return d.kind != ECUIO_VALUE_TEXT && d.kind != ECUIO_VALUE_HEX;
}

const char* outcome_word(uint8_t o)
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
        return "unasked";
    }
}

const char* verdict_word(uint8_t v)
{
    switch (v) {
    case ECUIO_INSPECTION_READY:
        return "ready";
    case ECUIO_INSPECTION_NOT_READY:
        return "not_ready";
    case ECUIO_INSPECTION_MIL_ON:
        return "mil_on";
    case ECUIO_INSPECTION_YEAR_NEEDED:
        return "year_needed";
    case ECUIO_INSPECTION_NOT_COVERED:
        return "not_covered";
    case ECUIO_INSPECTION_READING:
        return "reading";
    default:
        return "unknown";
    }
}

/* The verdict in a sentence, for the text */
const char* verdict_words(uint8_t v)
{
    switch (v) {
    case ECUIO_INSPECTION_READY:
        return "ready: the incomplete monitors are within what the rule "
               "allows";
    case ECUIO_INSPECTION_NOT_READY:
        return "not ready: more monitors are incomplete than the rule "
               "allows; the car is sent away to be driven";
    case ECUIO_INSPECTION_MIL_ON:
        return "fails: an ECU commands the MIL";
    case ECUIO_INSPECTION_YEAR_NEEDED:
        return "depends on the model year: two monitors are incomplete, "
               "which 1996-2000 allows and 2001 on doesn't";
    case ECUIO_INSPECTION_NOT_COVERED:
        return "not covered: the rule starts at model year 1996";
    case ECUIO_INSPECTION_READING:
        return "not known yet: a report is still being read";
    default:
        return "not known: no ECU answered readiness (Service 01 PID 01)";
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

/* ---- The model ---- */

struct shown {
    std::string name;
    std::string value; /* as text: the number to its decimals, or a label */
    bool number = true;
    std::string unit;
    bool has_age = false;
    uint64_t age_ms = 0;
    bool stale = false;
};

struct code {
    std::string code;
    std::string title;
};

struct dtc_list {
    const char* key = "";
    const char* label = "";
    uint8_t service = 0;
    const char* outcome = "unasked";
    std::string reply;
    std::vector<code> codes;
    uint32_t more = 0;
};

struct monitor {
    std::string name;
    bool complete = false;
    bool cycle = false; /* PID 41 answered */
    bool cycle_enabled = false;
    bool cycle_complete = false;
};

struct item {
    std::string name;
    const char* outcome = "unasked";
};

struct ecu {
    std::string id;
    const char* state = "complete"; /* complete, reading, not asked */
    const char* readiness = "unasked";
    std::string readiness_reply;
    bool mil = false;
    uint32_t dtc_count = 0;
    bool readiness_known = false;
    std::vector<monitor> monitors;
    dtc_list lists[ECUIO_DIAG_LISTS];
    const char* freeze = "unasked";
    std::string freeze_reply;
    bool freeze_stored = false;
    code freeze_code;
    std::vector<shown> freeze_values;
    std::vector<shown> since_cleared;
    std::vector<item> identification;
};

struct model {
    std::string uri;
    std::string backend;
    uint32_t baud = 0;
    std::string profile;
    bool has_utc = false;
    std::string utc;
    const char* units = "metric";
    bool vin = false;
    uint16_t model_year = 0;
    std::string make;
    bool check_matches = false;
    ecuio_inspection_t inspection{};
    std::vector<std::string> incomplete;
    std::vector<ecu> ecus;
    bool ranks = false; /* the set ranks a signal */
    std::vector<shown> key_values;
    uint64_t identification_left_out = 0;
};

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
    if (d.kind == ECUIO_VALUE_ENUM) {
        const char* label =
            ecuio_enum_label(set, &d, static_cast<uint32_t>(v >= 0 ? v : 0));
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
    s->age_ms = (v.age_us + 500) / 1000;
    s->stale = v.age_us > ECUIO_VALUE_STALE_US;
}

ecu ecu_of(const inputs& in, uint32_t c, const ecuio_signal_set_t* obd2,
           model& m, scrub::values* identity)
{
    const ecuio_channel_diagnostics_t& d = in.diagnostics[c];
    ecu e;
    e.id = hex_id(d.rx_id != 0 ? d.rx_id : in.identity[c].rx_id);
    if ((d.flags & ECUIO_DIAG_ASKED) == 0) {
        e.state = "not asked";
    } else if ((d.flags & ECUIO_DIAG_COMPLETE) == 0) {
        e.state = "reading";
    }
    e.readiness = outcome_word(d.readiness);
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
        dl.outcome = outcome_word(src.outcome);
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
    e.freeze = outcome_word(d.freeze);
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
        x.outcome = outcome_word(it.outcome);
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

model build(const inputs& in, scrub::values* identity)
{
    model m;
    m.uri = scrub::uri(in.uri);
    if (in.info != nullptr) {
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
    return m;
}

/* ---- JSON ---- */

class json_out
{
public:
    std::string out;

    void open(const char* key, char bracket)
    {
        item(key);
        out += bracket;
        first_.push_back(true);
    }
    void close(char bracket)
    {
        const bool empty = first_.back();
        first_.pop_back();
        if (!empty) {
            newline();
        }
        out += bracket;
    }
    void str(const char* key, const std::string& s)
    {
        item(key);
        quote(s);
    }
    void raw(const char* key, const std::string& text)
    {
        item(key);
        out += text;
    }
    void boolean(const char* key, bool b) { raw(key, b ? "true" : "false"); }
    void u64(const char* key, uint64_t v) { raw(key, std::to_string(v)); }
    void null(const char* key) { raw(key, "null"); }

private:
    void newline()
    {
        out += '\n';
        out.append(2 * first_.size(), ' ');
    }
    void item(const char* key)
    {
        if (!first_.empty()) {
            if (!first_.back()) {
                out += ',';
            }
            first_.back() = false;
            newline();
        }
        if (key != nullptr) {
            quote(key);
            out += ": ";
        }
    }
    void quote(const std::string& s)
    {
        out += '"';
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += static_cast<char>(c);
            } else if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04X", c);
                out += b;
            } else {
                out += static_cast<char>(c);
            }
        }
        out += '"';
    }

    std::vector<bool> first_;
};

void json_shown(json_out& j, const shown& s)
{
    j.open(nullptr, '{');
    j.str("name", s.name);
    if (s.number) {
        j.raw("value", s.value);
        j.str("unit", s.unit);
    } else {
        j.str("value", s.value);
    }
    if (s.has_age) {
        j.u64("age_ms", s.age_ms);
        j.boolean("stale", s.stale);
    }
    j.close('}');
}

void json_code(json_out& j, const char* key, const code& c)
{
    j.open(key, '{');
    j.str("code", c.code);
    if (c.title.empty()) {
        j.null("title");
    } else {
        j.str("title", c.title);
    }
    j.close('}');
}

std::string json(const model& m)
{
    json_out j;
    j.open(nullptr, '{');
    j.u64("havne_health", 1);
    j.str("core", ecuio_version_string());
    j.str("adr", "ADR-0054");
    j.boolean("scrubbed", true);
    j.open("source", '{');
    j.str("kind", "session");
    j.str("uri", m.uri);
    j.str("backend", m.backend);
    if (m.baud != 0) {
        j.u64("baud", m.baud);
    } else {
        j.null("baud");
    }
    j.str("profile", m.profile);
    j.close('}');
    if (m.has_utc) {
        j.str("utc", m.utc);
    } else {
        j.null("utc");
    }
    j.str("units", m.units);

    j.open("vehicle", '{');
    j.boolean("vin_read", m.vin);
    if (m.vin && m.model_year != 0) {
        j.u64("model_year", m.model_year);
    } else {
        j.null("model_year");
    }
    if (m.vin && !m.make.empty()) {
        j.str("make", m.make);
    } else {
        j.null("make");
    }
    if (m.vin) {
        j.boolean("check_digit_matches", m.check_matches);
    } else {
        j.null("check_digit_matches");
    }
    j.close('}');

    j.open("inspection", '{');
    j.str("rule", "40 CFR 85.2222");
    j.str("verdict", verdict_word(m.inspection.verdict));
    if (m.model_year != 0) {
        j.u64("model_year", m.model_year);
        j.u64("allowed", m.inspection.allowed);
    } else {
        j.null("model_year");
        j.null("allowed");
    }
    j.open("incomplete", '[');
    for (const std::string& name : m.incomplete) {
        j.str(nullptr, name);
    }
    j.close(']');
    j.u64("reports", m.inspection.reports);
    j.close('}');

    j.open("ecus", '[');
    for (const ecu& e : m.ecus) {
        j.open(nullptr, '{');
        j.str("ecu", e.id);
        j.str("state", e.state);
        j.open("readiness", '{');
        j.str("outcome", e.readiness);
        if (!e.readiness_reply.empty()) {
            j.str("reply", e.readiness_reply);
        }
        if (e.readiness_known) {
            j.boolean("mil", e.mil);
            j.u64("dtc_count", e.dtc_count);
        }
        j.open("monitors", '[');
        for (const monitor& mo : e.monitors) {
            j.open(nullptr, '{');
            j.str("name", mo.name);
            j.boolean("complete", mo.complete);
            if (mo.cycle) {
                j.boolean("cycle_enabled", mo.cycle_enabled);
                j.boolean("cycle_complete", mo.cycle_complete);
            }
            j.close('}');
        }
        j.close(']');
        j.close('}');
        for (const dtc_list& dl : e.lists) {
            j.open(dl.key, '{');
            j.str("outcome", dl.outcome);
            if (!dl.reply.empty()) {
                j.str("reply", dl.reply);
            }
            j.open("codes", '[');
            for (const code& c : dl.codes) {
                json_code(j, nullptr, c);
            }
            j.close(']');
            j.u64("more", dl.more);
            j.close('}');
        }
        j.open("freeze", '{');
        j.str("outcome", e.freeze);
        if (!e.freeze_reply.empty()) {
            j.str("reply", e.freeze_reply);
        }
        if (e.freeze_stored) {
            json_code(j, "stored_by", e.freeze_code);
        } else {
            j.null("stored_by");
        }
        j.open("values", '[');
        for (const shown& s : e.freeze_values) {
            json_shown(j, s);
        }
        j.close(']');
        j.close('}');
        j.open("since_cleared", '[');
        for (const shown& s : e.since_cleared) {
            json_shown(j, s);
        }
        j.close(']');
        j.open("identification", '[');
        for (const item& x : e.identification) {
            j.open(nullptr, '{');
            j.str("name", x.name);
            j.str("outcome", x.outcome);
            j.close('}');
        }
        j.close(']');
        j.close('}');
    }
    j.close(']');

    j.boolean("profile_ranks_signals", m.ranks);
    j.open("key_values", '[');
    for (const shown& s : m.key_values) {
        json_shown(j, s);
    }
    j.close(']');
    j.open("left_out", '{');
    j.u64("identification_texts", m.identification_left_out);
    j.close('}');
    j.close('}');
    j.out += '\n';
    return j.out;
}

/* ---- Text ---- */

/* n and its noun, "1 code", "2 codes" */
std::string counted(uint64_t n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string age_words(const shown& s)
{
    if (!s.has_age) {
        return {};
    }
    std::string w = " (" + std::to_string(s.age_ms / 1000) + "." +
                    std::to_string(s.age_ms % 1000 / 100) + " s old";
    return w + (s.stale ? ", stale)" : ")");
}

void text_shown(std::string& t, const char* indent, const shown& s)
{
    t += indent;
    t += s.name + ": " + s.value;
    if (s.number && !s.unit.empty()) {
        t += " " + s.unit;
    }
    t += age_words(s) + "\n";
}

std::string code_words(const code& c)
{
    return c.title.empty() ? c.code : c.code + "  " + c.title;
}

std::string text(const model& m)
{
    std::string t;
    t += std::string("Havne ") + ecuio_version_string() +
         ": a scrubbed health report (ADR-0054)\n";
    t += "Source: a live session, " + m.uri + " (" + m.backend;
    if (m.baud != 0) {
        t += ", " + std::to_string(m.baud) + " bit/s";
    }
    t += "), profile " + m.profile + "\n";
    if (m.has_utc) {
        t += "Time: " + m.utc + "\n";
    }
    t += std::string("Units: ") + m.units + "\n";

    t += "\nVehicle (from its VIN, which is left out)\n";
    if (!m.vin) {
        t += "  No VIN was read.\n";
    } else {
        t += "  Model year: " +
             (m.model_year != 0 ? std::to_string(m.model_year)
                                : std::string("not in the VIN")) +
             "\n";
        t += "  Make: " + (m.make.empty() ? std::string("not known") : m.make) +
             "\n";
        t += std::string("  Check digit: ") +
             (m.check_matches ? "matches" : "doesn't match") + "\n";
    }

    t += "\nInspection (40 CFR 85.2222)\n";
    t += std::string("  Verdict: ") + verdict_word(m.inspection.verdict) +
         ", " + verdict_words(m.inspection.verdict) + "\n";
    if (m.model_year != 0) {
        t += "  Model year " + std::to_string(m.model_year) + ": " +
             counted(m.inspection.allowed, "incomplete monitor",
                     "incomplete monitors") +
             " allowed\n";
    } else {
        t += "  No model year: one incomplete monitor passes either "
             "model year's rule, two pass only 1996-2000's, three or more "
             "pass neither\n";
    }
    t += "  Incomplete: " +
         (m.incomplete.empty() ? std::string("none") : std::string());
    for (size_t i = 0; i < m.incomplete.size(); ++i) {
        t += (i > 0 ? ", " : "") + m.incomplete[i];
    }
    t += "\n  Reports counted: " + std::to_string(m.inspection.reports) + "\n";

    for (const ecu& e : m.ecus) {
        t += "\nECU " + e.id + " (" + e.state + ")\n";
        t += std::string("  Readiness: ") + e.readiness;
        if (!e.readiness_reply.empty()) {
            t += " (" + e.readiness_reply + ")";
        }
        if (e.readiness_known) {
            t += std::string(", MIL ") + (e.mil ? "on" : "off") + ", " +
                 counted(e.dtc_count, "confirmed code", "confirmed codes");
        }
        t += "\n";
        for (const monitor& mo : e.monitors) {
            t += "    " + mo.name + ": " +
                 (mo.complete ? "complete" : "incomplete");
            if (mo.cycle) {
                t += std::string(", this drive cycle ") +
                     (!mo.cycle_enabled   ? "disabled"
                      : mo.cycle_complete ? "complete"
                                          : "incomplete");
            }
            t += "\n";
        }
        for (const dtc_list& dl : e.lists) {
            t += std::string("  ") + dl.label + ": " + dl.outcome;
            if (!dl.reply.empty()) {
                t += " (" + dl.reply + ")";
            }
            if (std::strcmp(dl.outcome, "answered") == 0 && dl.codes.empty() &&
                dl.more == 0) {
                t += ", none";
            }
            t += "\n";
            for (const code& c : dl.codes) {
                t += "    " + code_words(c) + "\n";
            }
            if (dl.more != 0) {
                t += "    and " + std::to_string(dl.more) + " more\n";
            }
        }
        t += std::string("  Freeze frame: ") + e.freeze;
        if (!e.freeze_reply.empty()) {
            t += " (" + e.freeze_reply + ")";
        }
        if (e.freeze_stored) {
            t += ", stored by " + code_words(e.freeze_code);
        }
        t += "\n";
        for (const shown& s : e.freeze_values) {
            text_shown(t, "    ", s);
        }
        if (!e.since_cleared.empty()) {
            t += "  Since the codes were cleared:\n";
            for (const shown& s : e.since_cleared) {
                text_shown(t, "    ", s);
            }
        }
        if (!e.identification.empty()) {
            t += "  Identification (each item's text left out):";
            for (size_t i = 0; i < e.identification.size(); ++i) {
                t += std::string(i > 0 ? "," : "") + " " +
                     e.identification[i].name + " " +
                     e.identification[i].outcome;
            }
            t += "\n";
        }
    }

    t += "\nKey values (the profile's ranked signals)\n";
    if (!m.ranks) {
        t += "  The profile ranks no signals.\n";
    } else if (m.key_values.empty()) {
        t += "  None of the ranked signals has a value.\n";
    }
    for (const shown& s : m.key_values) {
        text_shown(t, "  ", s);
    }
    t += "\nLeft out: " +
         counted(m.identification_left_out, "identification text",
                 "identification texts") +
         " (the VIN, calibration IDs, serials and the like)\n";
    return t;
}

void count(const model& m, ecuio_health_report_t* r)
{
    *r = ecuio_health_report_t{};
    r->ecus = static_cast<uint32_t>(m.ecus.size());
    for (const ecu& e : m.ecus) {
        for (const dtc_list& dl : e.lists) {
            r->codes += static_cast<uint32_t>(dl.codes.size());
        }
        r->since_cleared += static_cast<uint32_t>(e.since_cleared.size());
    }
    r->values = static_cast<uint32_t>(m.key_values.size());
    r->left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] =
        m.identification_left_out;
    r->flags = m.vin ? ECUIO_HEALTH_VIN_DECODED : 0u;
}

} // namespace

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
    char b[32];
    std::snprintf(b, sizeof b, "%04lld-%02llu-%02lluT%02llu:%02llu:%02lluZ",
                  static_cast<long long>(y + (mo <= 2 ? 1 : 0)),
                  static_cast<unsigned long long>(mo),
                  static_cast<unsigned long long>(d),
                  static_cast<unsigned long long>(rem / 3600),
                  static_cast<unsigned long long>(rem % 3600 / 60),
                  static_cast<unsigned long long>(rem % 60));
    return b;
}

std::string write(const inputs& in, uint32_t format,
                  ecuio_health_report_t* report, scrub::values* identity)
{
    const model m = build(in, identity);
    if (report != nullptr) {
        count(m, report);
    }
    return format == ECUIO_HEALTH_TEXT ? text(m) : json(m);
}

namespace {

namespace files = ecuio_files;

bool refused(const char* fmt, const std::string& a = std::string())
{
    ecuio_detail_set(fmt, a.c_str());
    return false;
}

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};

} // namespace

bool put(const std::string& out, const std::string& bytes,
         const ecuio_scrub::values& identity, ecuio_health_report_t& r)
{
    ecuio_scrub::check check(identity);
    check.feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    const uint32_t found = check.finish();
    if (found != 0) {
        r.failed_class = found;
        ecuio_detail_set("the check found %s in the report: nothing was "
                         "written",
                         ecuio_scrub::failed_words(found));
        return false;
    }
    if (files::exists(out)) {
        return refused("the file exists already");
    }
    const std::string partial = out + ".partial";
    std::string why;
    std::unique_ptr<FILE, file_closer> f(files::open_file(partial, true, &why));
    if (!f) {
        return refused("the report can't be written: %s", why);
    }
    const bool wrote =
        std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size();
    const int err = errno;
    if (std::fclose(f.release()) != 0 || !wrote) {
        files::remove_file(partial);
        return refused("the report can't be written: %s",
                       files::errno_words(err != 0 ? err : EIO));
    }
    if (!files::rename_to(partial, out, false, &why)) {
        files::remove_file(partial);
        return refused("the report can't be put in place: %s", why);
    }
    return true;
}

} // namespace ecuio_health

extern "C" ecuio_status_t
ecuio_health_export(const ecuio_health_options_t* options,
                    ecuio_health_report_t* report)
{
    if (report != nullptr) {
        *report = ecuio_health_report_t{};
    }
    if (options == nullptr || options->out_path_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const ecuio_health_options_t& o = *options;
    bool reserved = o.reserved0 != 0;
    for (uint64_t v : o.reserved) {
        reserved = reserved || v != 0;
    }
    if ((o.format != ECUIO_HEALTH_JSON && o.format != ECUIO_HEALTH_TEXT) ||
        o.flags != 0 || reserved ||
        (o.units != ECUIO_UNITS_METRIC && o.units != ECUIO_UNITS_IMPERIAL) ||
        ((o.session != nullptr) == (o.folder_utf8 != nullptr)) ||
        (o.values == nullptr && o.value_count != 0) ||
        (o.to_us != 0 && o.to_us < o.from_us)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < o.value_count; ++i) {
        if (o.values[i].flags != 0 || o.values[i].reserved != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    if (o.folder_utf8 != nullptr) {
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    ecuio_detail_clear();
    ecuio_health_report_t r{};
    bool ok = false;
    try {
        ecuio_session_info_t info{};
        if (ecuio_session_get_info(o.session, &info) != ECUIO_OK) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        const uint32_t channels = std::min<uint32_t>(
            info.channel_count, ECUIO_SESSION_INFO_MAX_CHANNELS);
        std::vector<ecuio_channel_diagnostics_t> diags(channels);
        std::vector<ecuio_channel_identity_t> ids(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            (void)ecuio_session_get_channel_diagnostics(o.session, c,
                                                        &diags[c]);
            (void)ecuio_session_get_channel_identity(o.session, c, &ids[c]);
        }
        ecuio_health::inputs in;
        in.info = &info;
        in.uri = o.session->uri;
        in.set = ecuio_session_signal_set(o.session);
        in.diagnostics = diags.data();
        in.identity = ids.data();
        in.channels = channels;
        in.values = o.values;
        in.value_count = o.value_count;
        in.units = o.units;
        in.latest_model_year = o.latest_model_year;
        in.utc_us = o.utc_us;
        ecuio_scrub::values identity;
        const std::string bytes =
            ecuio_health::write(in, o.format, &r, &identity);
        ok = ecuio_health::put(o.out_path_utf8, bytes, identity, r);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (report != nullptr) {
        *report = r;
    }
    return ok ? ECUIO_OK : ECUIO_ERR_LOAD_FAILED;
}
