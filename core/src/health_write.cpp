// File/layer: core/src/health_write.cpp | C++17, Qt-free; the health
//             report's model written as JSON or text
// Testing: UNIT: HealthTest (test_core)
//
// Two writers that read only the model (health_model.h), so the
// formats can't hold different facts, and the report's counts.

#include "health_model.h"

#include "json.h" /* utf8_length */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ecuio_health {

namespace {

namespace scrub = ecuio_scrub;

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
    /* RFC 8259's string: valid UTF-8 as it is, a byte no valid sequence
     * holds as U+FFFD (json.h's rule, which the reader holds it to) */
    void quote(const std::string& s)
    {
        out += '"';
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        const unsigned char* const end = p + s.size();
        while (p < end) {
            const unsigned char c = *p;
            if (c >= 0x80) {
                const size_t n = ecuio_json::utf8_length(p, end);
                if (n == 0) {
                    out += "\xEF\xBF\xBD";
                    ++p;
                } else {
                    out.append(reinterpret_cast<const char*>(p), n);
                    p += n;
                }
                continue;
            }
            ++p;
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

/* Why a recording's report has no ECU */
const char* ecus_note(const model& m)
{
    return m.frames ? "no ECU answered a diagnostic or identification "
                      "request in the range"
                    : "the recording has no frames";
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
    j.str("kind", m.recording ? "recording" : "session");
    if (m.recording && m.uri.empty()) {
        j.null("uri");
    } else {
        j.str("uri", m.uri);
    }
    j.str("backend", m.backend);
    if (m.baud != 0) {
        j.u64("baud", m.baud);
    } else {
        j.null("baud");
    }
    j.str("profile", m.profile);
    if (m.recording) {
        if (m.has_started) {
            j.str("started", m.started);
        } else {
            j.null("started");
        }
        j.raw("from_s", m.from_s);
        j.raw("to_s", m.to_s);
        j.boolean("frames", m.frames);
    }
    j.close('}');
    if (!m.baseline_named) {
        j.null("baseline");
    } else {
        j.open("baseline", '{');
        j.boolean("read", m.baseline_read);
        if (!m.baseline_read) {
            j.str("why", m.baseline_why);
        } else {
            if (m.baseline_has_started) {
                j.str("started", m.baseline_started);
            } else {
                j.null("started");
            }
            if (m.baseline_has_length) {
                j.raw("length_s", m.baseline_length_s);
            } else {
                j.null("length_s");
            }
            j.str("profile", m.baseline_profile);
            j.str("backend", m.baseline_backend);
            j.boolean("is_source", m.baseline_is_source);
        }
        j.close('}');
    }
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
    if (m.recording && m.ecus.empty()) {
        j.str("ecus_note", ecus_note(m));
    }

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
    if (m.recording) {
        t += "Source: a recording, " +
             (m.uri.empty() ? std::string("no URI") : m.uri) + " (" +
             m.backend + "), profile " +
             (m.profile.empty() ? std::string("none") : m.profile) + "\n";
        if (m.has_started) {
            t += "Recorded: " + m.started + "\n";
        }
        t += "Range: " + m.from_s + " s to " + m.to_s + " s\n";
    } else {
        t += "Source: a live session, " + m.uri + " (" + m.backend;
        if (m.baud != 0) {
            t += ", " + std::to_string(m.baud) + " bit/s";
        }
        t += "), profile " + m.profile + "\n";
    }
    if (m.baseline_named && !m.baseline_read) {
        t += "Baseline: the vehicle's baseline recording can't be read: " +
             m.baseline_why + "\n";
    } else if (m.baseline_named) {
        t += "Baseline: the vehicle's baseline recording, " +
             (m.baseline_has_started ? "started " + m.baseline_started
                                     : std::string("with no start time"));
        if (m.baseline_has_length) {
            t += ", " + m.baseline_length_s + " s";
        }
        t += " (" + m.baseline_backend + "), profile " +
             (m.baseline_profile.empty() ? std::string("none")
                                         : m.baseline_profile);
        t += m.baseline_is_source ? ": this recording\n" : "\n";
    }
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

    if (m.recording && m.ecus.empty()) {
        t += std::string("\nECUs: ") + ecus_note(m) + "\n";
    }

    t += "\nKey values (the profile's ranked signals)\n";
    const size_t ranked = m.key_values.size() - m.key_since_cleared;
    if (!m.ranks) {
        t += "  The profile ranks no signals.\n";
    } else if (ranked == 0) {
        t += "  None of the ranked signals has a value.\n";
    }
    for (size_t i = 0; i < m.key_values.size(); ++i) {
        if (i == ranked) {
            t += "  Since the codes were cleared, on a reply ID no ECU is "
                 "listed for:\n";
        }
        text_shown(t, "  ", m.key_values[i]);
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
    r->since_cleared += static_cast<uint32_t>(m.key_since_cleared);
    r->values = static_cast<uint32_t>(m.key_values.size());
    r->left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] =
        m.identification_left_out;
    r->flags = (m.vin ? ECUIO_HEALTH_VIN_DECODED : 0u) |
               (m.baseline_read ? ECUIO_HEALTH_BASELINE_READ : 0u);
}

} // namespace

std::string write(const inputs& in, uint32_t format,
                  ecuio_health_report_t* report, scrub::values* identity)
{
    const model m = build(in, identity);
    if (report != nullptr) {
        count(m, report);
    }
    return format == ECUIO_HEALTH_TEXT ? text(m) : json(m);
}

} // namespace ecuio_health
