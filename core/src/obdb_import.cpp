// File/layer: core/src/obdb_import.cpp | C++17, Qt-free, core: ecuio_obdb_import (obdb.h, ADR-0051 section 7)
// Testing: UNIT: ObdbImportTest (test_core); FUZZ: fuzz_obdb_import
//
// Reads an OBDb signal set (the shape of OBDb's .schemas signals.json, the
// semantics of its python/can/ decoder) and writes a signal file
// (docs/signal-files.md). What converts:
// - commands: Service 01 and 22 on 11-bit IDs, hdr the request ID and rax
//   the reply's (rx = hdr + 8 for 7E0 to 7E7 when OBDb gives none, as
//   ISO 15765-4 does); a module per reply ID, at most ECUIO_MAX_CHANNELS
//   with obd2's when the file extends it; a model year's commands by
//   their filter (obdb_import.h's year_takes);
// - signals: the field by field_of, sign, mul / div as the scale, add as
//   the offset, decimals from the scale's step; the unit by kUnits; a map
//   as an enumeration, each label its description; the path's first part
//   as the group; the description as notes;
// - synthetics: a ratio as a computed signal.
// Each difference is a note: min and max (OBDb holds a value within them),
// nullmin and nullmax, a cut or guarded name or label, a reply ID taken
// from the standard; each signal left out says why. The output is loaded
// before it's returned, so what this gives the caller always loads.

#include "ecuio/obdb.h"

#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "json.h"
#include "obdb_import.h"
#include "request_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using ecuio_json::Member;
using ecuio_json::number_text;
using ecuio_json::Type;
using ecuio_json::Value;

/* The caller's note callback asked to stop */
struct canceled {};

/* Input that isn't an OBDb signal set at all */
struct not_obdb {
    uint32_t line;
    uint32_t column;
    std::string words;
};

/* OBDb's unit words and the spelling the built-in sets give each, so a
 * converted unit converts where theirs do (ADR-0022) */
struct unit_word {
    const char* obdb;
    const char* havne;
};
const unit_word kUnits[] = {
    {"ampereHours", "Ah"},
    {"amps", "A"},
    {"bars", "bar"},
    {"celsius", "\xC2\xB0"
                "C"},
    {"centimeters", "cm"},
    {"coulombs", "C"},
    {"degrees", "\xC2\xB0"},
    {"fahrenheit", "\xC2\xB0"
                   "F"},
    {"feet", "ft"},
    {"framesPerSecond", "fps"},
    {"gallons", "gal"},
    {"gallonsPerHour", "gal/h"},
    {"gigahertz", "GHz"},
    {"gramsPerLiter", "g/L"},
    {"gramsPerSecond", "g/s"},
    {"gravity", "g"},
    {"hertz", "Hz"},
    {"hours", "h"},
    {"inches", "in"},
    {"inchPound", "in\xC2\xB7lb"},
    {"joules", "J"},
    {"kelvin", "K"},
    {"kiloampereHours", "kAh"},
    {"kiloamps", "kA"},
    {"kilogramsPerHour", "kg/h"},
    {"kilohertz", "kHz"},
    {"kilojoules", "kJ"},
    {"kilometers", "km"},
    {"kilometersPerHour", "km/h"},
    {"kiloohms", "k\xCE\xA9"},
    {"kilopascal", "kPa"},
    {"kilovolts", "kV"},
    {"kilowattHours", "kWh"},
    {"kilowattHoursPer100Kilometers", "kWh/100km"},
    {"kilowattHoursPer100Miles", "kWh/100mi"},
    {"kilowatts", "kW"},
    {"liters", "L"},
    {"litersPerHour", "L/h"},
    {"megahertz", "MHz"},
    {"megaohms", "M\xCE\xA9"},
    {"meters", "m"},
    {"metersPerSecond", "m/s"},
    {"metersPerSecondSquared", "m/s\xC2\xB2"},
    {"microhertz", "\xC2\xB5Hz"},
    {"microohms", "\xC2\xB5\xCE\xA9"},
    {"miles", "mi"},
    {"milesPerHour", "mph"},
    {"milesPerKilowattHour", "mi/kWh"},
    {"milliampereHours", "mAh"},
    {"milliamps", "mA"},
    {"milligramsPerDeciliter", "mg/dL"},
    {"milligramsPerStroke", "mg/stroke"},
    {"millihertz", "mHz"},
    {"millimeters", "mm"},
    {"milliohms", "m\xCE\xA9"},
    {"milliseconds", "ms"},
    {"millivolts", "mV"},
    {"milliwatts", "mW"},
    {"minutes", "min"},
    {"nanohertz", "nHz"},
    {"newtonMeters", "N\xC2\xB7m"},
    {"ohms", "\xCE\xA9"},
    {"percent", "%"},
    {"poundFoot", "lb\xC2\xB7"
                  "ft"},
    {"psi", "psi"},
    {"radians", "rad"},
    {"rpm", "RPM"},
    {"seconds", "s"},
    {"terahertz", "THz"},
    {"volts", "V"},
    {"watts", "W"},
    {"yards", "yd"},
    {"scalar", ""},
    {"unknown", ""},
};

/* The first part of an OBDb path and the signal file group it falls in;
 * a path not here is Other */
struct path_group {
    const char* path;
    const char* group;
};
const path_group kGroups[] = {
    {"Engine", "engine"},         {"Fluids", "engine"},
    {"Control", "air"},           {"Fuel", "fuel"},
    {"Emissions", "emissions"},   {"Battery", "electrical"},
    {"Electrical", "electrical"}, {"Movement", "vehicle"},
    {"Drivetrain", "vehicle"},    {"Transmission", "vehicle"},
    {"Tires", "vehicle"},         {"Brakes", "vehicle"},
    {"Suspension", "vehicle"},    {"Trips", "vehicle"},
    {"Stats", "vehicle"},         {"Cruise control", "vehicle"},
    {"DTCs", "diagnostics"},      {"Monitors", "diagnostics"},
    {"ECU", "diagnostics"},       {"OBD", "diagnostics"},
    {"Details", "diagnostics"},
};

/* A JSON string of text with no control characters (clean() made it so) */
void add_string(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
}

/* Text with every control character a space, and every character
 * ecuio_json::invisible_length finds, cut to max bytes at a whole
 * character; *cut says whether it was cut */
std::string clean(const std::string& s, size_t max, bool* cut)
{
    std::string t;
    t.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t hidden =
            ecuio_json::invisible_length(s.data() + i, s.size() - i);
        if (c < 0x20 || c == 0x7F || hidden > 0) {
            t += ' ';
            i += hidden > 0 ? hidden : 1;
        } else {
            t += s[i];
            ++i;
        }
    }
    *cut = t.size() > max;
    if (*cut) {
        size_t n = max;
        while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) {
            --n;
        }
        t.resize(n);
    }
    return t;
}

/* Text a spreadsheet won't run as a formula (ADR-0049 section 8): its
 * leading '=', '+', '-' and '@' dropped, with the spaces between them,
 * so what's left starts with neither; true when any was */
bool unformula(std::string* s)
{
    size_t n = 0;
    bool any = false;
    while (
        n < s->size() && (*s)[n] != '\0' &&
        (std::strchr("=+-@", (*s)[n]) != nullptr || (any && (*s)[n] == ' '))) {
        any = any || (*s)[n] != ' ';
        ++n;
    }
    s->erase(0, n);
    return any;
}

/* Text without its leading and trailing spaces */
std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
        return std::string();
    }
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

/* Hex digits, 1 to max of them, as a value */
bool hex_of(const std::string& s, size_t max, uint32_t* out)
{
    if (s.empty() || s.size() > max) {
        return false;
    }
    uint32_t v = 0;
    for (const char c : s) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

std::string hex_text(uint32_t v, int digits)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, v);
    return buf;
}

/* A string member, or "" */
std::string text_at(const Value& obj, const char* key)
{
    const Value* v = obj.find(key);
    return (v != nullptr && v->type == Type::string) ? v->text : std::string();
}

/* The fewest decimals, 0 to 3, that show each step of a scale and offset */
uint32_t decimals_of(double scale, double offset)
{
    uint32_t d = 0;
    for (const double v : {scale, offset}) {
        uint32_t k = 0;
        double x = v;
        while (k < 3 && std::fabs(x - std::round(x)) >
                            1e-9 * std::max(1.0, std::fabs(x))) {
            ++k;
            x *= 10;
        }
        d = std::max(d, k);
    }
    return d;
}

/* A key of [a-z0-9_], 1 to 31 bytes, from an OBDb id: camel case split,
 * a letter first (an expression reads a leading digit as a number) */
std::string key_base(const std::string& id)
{
    std::string k;
    char prev = '_';
    for (const char c : id) {
        char out = '_';
        if (c >= 'A' && c <= 'Z') {
            if (prev >= 'a' && prev <= 'z') {
                k += '_';
            }
            out = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out = c;
        }
        if (out == '_' && (k.empty() || k.back() == '_')) {
            prev = c;
            continue;
        }
        k += out;
        prev = c;
    }
    while (!k.empty() && k.back() == '_') {
        k.pop_back();
    }
    if (k.empty() || (k[0] >= '0' && k[0] <= '9')) {
        k = "s_" + k;
    }
    if (k.size() > 31) {
        k.resize(31);
        while (k.back() == '_') {
            k.pop_back();
        }
    }
    return k;
}

/* A key not yet taken: the base, else the base cut to fit "_2", "_3"... */
std::string unique_key(const std::string& base, std::set<std::string>& taken)
{
    std::string k = base;
    for (unsigned n = 2; taken.count(k) != 0; ++n) {
        const std::string tail = "_" + std::to_string(n);
        k = base.substr(0, 31 - tail.size()) + tail;
    }
    taken.insert(k);
    return k;
}

/* A name not yet taken: the name, else cut to fit " 2", " 3"... at a whole
 * character, as clean() cuts. A set's names are one each (a recording keys
 * values by name), and a file's signal named as a base def would replace
 * it, which a converted set never means to. */
std::string unique_name(const std::string& base, std::set<std::string>& taken,
                        bool* repeated)
{
    std::string n = base;
    *repeated = false;
    for (unsigned k = 2; taken.count(n) != 0; ++k) {
        const std::string tail = " " + std::to_string(k);
        size_t cut = 31 - tail.size();
        while (cut > 0 && cut < base.size() &&
               (static_cast<unsigned char>(base[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        n = base.substr(0, cut) + tail;
        *repeated = true;
    }
    taken.insert(n);
    return n;
}

/* A module: its request and reply IDs and its id in the file */
struct module {
    uint32_t tx = 0;
    uint32_t rx = 0;
    std::string id;
    bool listed = false; /* in the file's "ecus" */
};

/* A changed field, held until its signal is known to convert */
struct change {
    const char* field;
    std::string text;
};

struct importer {
    const ecuio_obdb_options_t& options;
    ecuio_obdb_note_fn note_fn;
    void* user;
    const Value* ecu_list = nullptr; /* the set's "ecu" */
    std::vector<module> modules;
    std::set<uint64_t> asked; /* (rx, request) a session asks */
    std::set<std::string> keys;
    std::set<std::string> names; /* the file's names and, extending, obd2's */
    std::set<std::string> ecu_ids;
    std::map<std::string, std::string> numbers; /* OBDb id -> key, numeric */
    std::string signals;                        /* the file's signal entries */
    uint32_t signal_count = 0;

    importer(const ecuio_obdb_options_t& o, ecuio_obdb_note_fn fn, void* u)
        : options(o), note_fn(fn), user(u)
    {}

    void note(uint32_t what, uint32_t line, const std::string& signal,
              const std::string& key, const char* field,
              const std::string& text)
    {
        if (note_fn == nullptr) {
            return;
        }
        // One line each: a quoted id, protocol or address can carry a control
        // character, and a bound keeps a note a line long
        bool cut = false;
        const std::string signal_line = clean(signal, 128, &cut);
        const std::string text_line = clean(text, 512, &cut);
        ecuio_obdb_note_t n{};
        n.what = what;
        n.line = line;
        n.signal = signal_line.c_str();
        n.key = key.c_str();
        n.field = field;
        n.text = text_line.c_str();
        if (note_fn(&n, user) != 0) {
            throw canceled{};
        }
    }

    bool extends() const { return (options.flags & ECUIO_OBDB_ALONE) == 0; }

    /* The (rx, request) code the engine enrols for a request */
    static uint64_t request_code(uint32_t rx, const uint8_t* req, uint8_t len)
    {
        uint64_t code = len;
        for (uint8_t i = 0; i < 3; ++i) {
            code = (code << 8) | (i < len ? req[i] : 0u);
        }
        return (static_cast<uint64_t>(rx) << 32) | code;
    }

    /* obd2's modules and requests, which a file that extends it shares */
    void take_base()
    {
        if (!extends()) {
            return;
        }
        const ecuio_signal_set_t* base = ecuio_obd2_signal_set();
        for (uint32_t i = 0; i < base->count; ++i) {
            names.insert(base->defs[i].name);
        }
        for (uint32_t i = 0; i < base->channel_count; ++i) {
            module m;
            m.tx = base->channels[i].tx_id;
            m.rx = base->channels[i].rx_id;
            modules.push_back(m);
        }
        for (uint32_t i = 0; i < base->count; ++i) {
            uint8_t req[3] = {};
            uint8_t len = 0;
            if (ecuio_engine_request_for_def(&base->defs[i], req, &len)) {
                asked.insert(request_code(base->defs[i].message_id, req, len));
            }
        }
    }

    /* A module's id: the type the set's "ecu" list gives its request and
     * reply IDs (the entry that names the most wins, as OBDb's decoder
     * picks), else ecu_<tx> */
    std::string module_id(uint32_t tx, uint32_t rx)
    {
        std::string type;
        int best = -1;
        if (ecu_list != nullptr && ecu_list->type == Type::array) {
            for (const Value& e : ecu_list->items) {
                if (e.type != Type::object) {
                    continue;
                }
                uint32_t hdr = 0;
                if (!hex_of(text_at(e, "hdr"), 3, &hdr) || hdr != tx) {
                    continue;
                }
                int named = 0;
                const std::string rax = text_at(e, "rax");
                if (!rax.empty()) {
                    uint32_t r = 0;
                    if (!hex_of(rax, 3, &r) || r != rx) {
                        continue;
                    }
                    ++named;
                }
                if (const Value* f = e.find("filter")) {
                    if (!ecuio_obdb::year_takes(f, options.model_year)) {
                        continue;
                    }
                    ++named;
                }
                if (named > best && !text_at(e, "type").empty()) {
                    best = named;
                    type = text_at(e, "type");
                }
            }
        }
        const std::string base =
            key_base(type.empty() ? "ecu_" + hex_text(tx, 3) : type);
        std::string id = base;
        for (unsigned n = 2; ecu_ids.count(id) != 0; ++n) {
            const std::string tail = "_" + std::to_string(n);
            id = base.substr(0, 31 - tail.size()) + tail;
        }
        ecu_ids.insert(id);
        return id;
    }

    void skip_signals(const Value& command, const std::string& why)
    {
        const Value* sigs = command.find("signals");
        bool any = false;
        if (sigs != nullptr && sigs->type == Type::array) {
            for (const Value& s : sigs->items) {
                const std::string id =
                    s.type == Type::object ? text_at(s, "id") : std::string();
                note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "", why);
                any = true;
            }
        }
        if (!any) {
            note(ECUIO_OBDB_NOTE_SKIPPED, command.line, "", "", "", why);
        }
    }

    /* One command and its signals */
    void command(const Value& c)
    {
        if (c.type != Type::object) {
            note(ECUIO_OBDB_NOTE_SKIPPED, c.line, "", "", "",
                 "a command that isn't an object");
            return;
        }
        const Value* sigs = c.find("signals");
        if (sigs == nullptr || sigs->type != Type::array) {
            note(ECUIO_OBDB_NOTE_SKIPPED, c.line, "", "", "",
                 "a command with no \"signals\" list");
            return;
        }
        const Value* filter = c.find("filter");
        if (!ecuio_obdb::year_takes(filter, options.model_year)) {
            skip_signals(c, options.model_year == 0
                                ? "its command is for some model years only: "
                                  "give the vehicle's model year"
                                : "its command isn't for model year " +
                                      std::to_string(options.model_year));
            return;
        }
        const std::string proto = text_at(c, "proto");
        if (!proto.empty() && proto != "15765-4-11bit") {
            skip_signals(c, proto == "15765-4-29bit"
                                ? "29-bit IDs aren't imported"
                                : "its command is on " + proto +
                                      ", a K-line protocol Havne doesn't read");
            return;
        }
        if (c.find("eax") != nullptr) {
            skip_signals(c, "extended addressing (eax) isn't supported");
            return;
        }
        const Value* cmd = c.find("cmd");
        if (cmd == nullptr || cmd->type != Type::object ||
            cmd->members.size() != 1 ||
            cmd->members[0].value.type != Type::string) {
            skip_signals(c, "its command has no \"cmd\" of one service");
            return;
        }
        const std::string service = cmd->members[0].key;
        const std::string number_hex = cmd->members[0].value.text;
        uint32_t number = 0;
        if (service == "21") {
            skip_signals(c, "Service 21 isn't one Havne reads");
            return;
        }
        if ((service != "01" || !hex_of(number_hex, 2, &number) ||
             number_hex.size() != 2) &&
            (service != "22" || !hex_of(number_hex, 4, &number) ||
             number_hex.size() != 4)) {
            skip_signals(c, "its command asks service " + service +
                                ", which Havne doesn't read");
            return;
        }
        const std::string hdr_text = text_at(c, "hdr");
        uint32_t tx = 0;
        if (hdr_text.size() == 4) {
            skip_signals(c, "29-bit IDs aren't imported");
            return;
        }
        if (!hex_of(hdr_text, 3, &tx) || hdr_text.size() != 3 || tx == 0 ||
            tx > 0x7FF) {
            skip_signals(c, "its command has no 11-bit request ID (\"hdr\")");
            return;
        }
        if (tx == 0x7DF) {
            skip_signals(
                c, "a functional request (7DF) has no one module's reply");
            return;
        }
        std::vector<change> command_changes;
        uint32_t rx = 0;
        const std::string rax = text_at(c, "rax");
        if (!rax.empty()) {
            if (!hex_of(rax, 3, &rx) || rx == 0 || rx > 0x7FF) {
                skip_signals(c, "its reply ID (\"rax\" " + rax +
                                    ") isn't an 11-bit one");
                return;
            }
        } else if (tx >= 0x7E0 && tx <= 0x7E7) {
            rx = tx + 8;
            command_changes.push_back(
                {"rax", "no reply ID given: " + hex_text(rx, 3) +
                            ", the one ISO 15765-4 gives " + hex_text(tx, 3) +
                            " (OBDb takes any module's)"});
        } else {
            skip_signals(c, "no reply ID (\"rax\"), and none the standard "
                            "gives request ID " +
                                hex_text(tx, 3));
            return;
        }
        const std::string din = text_at(c, "din");
        if (!din.empty()) {
            command_changes.push_back(
                {"din", "OBDb enters diagnostic session " + din +
                            " first; Havne asks in the default session, where "
                            "the module may refuse it"});
        }

        // The module and the request, taken only once a signal converts
        size_t m = modules.size();
        for (size_t i = 0; i < modules.size(); ++i) {
            if (modules[i].rx == rx) {
                m = i;
            }
        }
        if (m < modules.size() && modules[m].tx != tx) {
            skip_signals(c, "its reply ID " + hex_text(rx, 3) +
                                " answers request ID " +
                                hex_text(modules[m].tx, 3) + " already");
            return;
        }
        if (m == modules.size() && modules.size() >= ECUIO_MAX_CHANNELS) {
            skip_signals(c, std::string("more modules than a session asks (") +
                                std::to_string(ECUIO_MAX_CHANNELS) +
                                (extends() ? ", obd2's counted)" : ")"));
            return;
        }
        const uint8_t req[3] = {
            static_cast<uint8_t>(service == "01" ? 0x01 : 0x22),
            static_cast<uint8_t>(service == "01" ? number : number >> 8),
            static_cast<uint8_t>(number & 0xFF)};
        const uint8_t req_len = service == "01" ? 2 : 3;
        const uint64_t code = request_code(rx, req, req_len);
        const bool new_request = asked.count(code) == 0;
        if (new_request && asked.size() >= ECUIO_PROFILE_MAX_REQUESTS) {
            skip_signals(c, std::string("past the ") +
                                std::to_string(ECUIO_PROFILE_MAX_REQUESTS) +
                                " requests a session asks" +
                                (extends() ? ", obd2's counted" : ""));
            return;
        }
        std::string module_id =
            m < modules.size() ? modules[m].id : std::string();
        if (module_id.empty()) {
            module_id = module_id_for(tx, rx);
        }

        const std::string read = std::string("{\"service\":\"") + service +
                                 "\",\"number\":\"" + number_hex + "\"}";
        bool converted = false;
        for (const Value& s : sigs->items) {
            converted =
                signal(s, module_id, read, command_changes) || converted;
        }
        if (!converted) {
            return;
        }
        if (m == modules.size()) {
            module mod;
            mod.tx = tx;
            mod.rx = rx;
            modules.push_back(mod);
        }
        modules[m].id = module_id;
        modules[m].listed = true;
        ecu_ids.insert(module_id);
        if (new_request) {
            asked.insert(code);
        }
    }

    /* The id a module will take, without taking it yet */
    std::string module_id_for(uint32_t tx, uint32_t rx)
    {
        std::string id = module_id(tx, rx);
        ecu_ids.erase(id);
        return id;
    }

    /* One signal of a command; true when it converted */
    bool signal(const Value& s, const std::string& module_id,
                const std::string& read,
                const std::vector<change>& command_changes)
    {
        if (s.type != Type::object) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, "", "", "",
                 "a signal that isn't an object");
            return false;
        }
        const std::string id = text_at(s, "id");
        auto skip = [&](const std::string& why) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "", why);
            return false;
        };
        if (id.empty()) {
            return skip("a signal with no \"id\"");
        }
        if (const Value* hidden = s.find("hidden")) {
            if (hidden->type == Type::boolean && hidden->boolean) {
                return skip("hidden in OBDb");
            }
        }
        if (signal_count >= ECUIO_SIGNAL_FILE_MAX_SIGNALS) {
            return skip("past the " +
                        std::to_string(ECUIO_SIGNAL_FILE_MAX_SIGNALS) +
                        " signals a file holds");
        }
        const Value* fmt = s.find("fmt");
        if (fmt == nullptr || fmt->type != Type::object) {
            return skip("a signal with no \"fmt\"");
        }
        auto whole = [&](const char* key, int64_t fallback, int64_t* out) {
            const Value* v = fmt->find(key);
            if (v == nullptr) {
                *out = fallback;
                return true;
            }
            if (v->type != Type::number || !v->in_int64) {
                return false;
            }
            *out = v->whole;
            return true;
        };
        int64_t len = 0;
        int64_t bix = 0;
        if (!whole("len", -1, &len) || len < 0) {
            return skip("its \"len\" isn't a whole number of bits");
        }
        if (!whole("bix", 0, &bix) || bix < 0 || bix > 0x7FFFFFFF) {
            return skip("its \"bix\" isn't a bit index");
        }
        const Value* blsb_v = fmt->find("blsb");
        const Value* sign_v = fmt->find("sign");
        const bool blsb = blsb_v != nullptr && blsb_v->type == Type::boolean &&
                          blsb_v->boolean;
        const bool sign = sign_v != nullptr && sign_v->type == Type::boolean &&
                          sign_v->boolean;
        const Value* map = fmt->find("map");
        const bool enumeration = map != nullptr;
        ecuio_obdb::field f;
        // An enumeration reads its bits as they are: OBDb's decoder applies
        // blsb to numbers only
        if (!ecuio_obdb::field_of(static_cast<uint32_t>(bix),
                                  static_cast<uint32_t>(len),
                                  blsb && !enumeration, &f)) {
            if (len == 0 || len > 32) {
                return skip("a field of " + std::to_string(len) +
                            " bits: a signal file reads 1 to 32");
            }
            return skip("a field across bytes that isn't whole bytes (bix " +
                        std::to_string(bix) + ", len " + std::to_string(len) +
                        "), which a signal file can't say");
        }

        // The field within a reply core holds (ECUIO_FRAME_DATA_MAX), the
        // echo's 3 bytes counted
        if (f.byte + (f.bits + 7) / 8 + 3 > ECUIO_FRAME_DATA_MAX) {
            return skip("a field past the 4096 bytes a reply holds");
        }
        std::vector<change> changes = command_changes;
        const std::string key = unique_key(key_base(id), keys);
        bool cut = false;
        std::string name = trimmed(clean(text_at(s, "name"), 31, &cut));
        if (cut) {
            changes.push_back({"name", "cut to 31 bytes"});
        }
        if (unformula(&name)) {
            changes.push_back({"name",
                               "its leading = + - or @ dropped: a "
                               "spreadsheet would run it as a formula"});
            name = trimmed(name);
        }
        if (name.empty()) {
            name = key;
            changes.push_back({"name", "none Havne can show: the key instead"});
        }
        bool repeated = false;
        name = unique_name(name, names, &repeated);
        if (repeated) {
            changes.push_back({"name",
                               "the same as another signal's: a number added"});
        }

        std::string e = "{\"key\":";
        add_string(e, key);
        e += ",\"name\":";
        add_string(e, name);
        e += ",\"ecu\":";
        add_string(e, module_id);
        e += ",\"read\":" + read;
        e += ",\"field\":{\"byte\":" + std::to_string(f.byte) +
             ",\"bits\":" + std::to_string(f.bits);
        if (f.little_endian) {
            if (f.bits < 8 || f.bit != 0) {
                e += ",\"bit\":" + std::to_string(f.bit);
            }
            e += ",\"order\":\"le\"";
        }
        if (sign && !enumeration) {
            e += ",\"signed\":true";
        }
        e += "}";

        const bool fields =
            enumeration ? enum_values(s, *map, static_cast<uint32_t>(len), id,
                                      &e, &changes)
                        : number_fields(s, *fmt, id, &e, &changes);
        if (!fields) {
            keys.erase(key);
            return false;
        }

        const std::string path = text_at(s, "path");
        const std::string first = path.substr(0, path.find('.'));
        for (const path_group& g : kGroups) {
            if (first == g.path) {
                e += ",\"group\":\"" + std::string(g.group) + "\"";
            }
        }
        const std::string description = text_at(s, "description");
        if (!description.empty()) {
            bool long_text = false;
            e += ",\"notes\":";
            add_string(e, clean(description, 512, &long_text));
        }
        e += "}";

        if (!signals.empty()) {
            signals += ",\n    ";
        }
        signals += e;
        ++signal_count;
        if (!enumeration) {
            numbers[id] = key;
        }
        note(ECUIO_OBDB_NOTE_CONVERTED, s.line, id, key, "", "converted");
        for (const change& ch : changes) {
            note(ECUIO_OBDB_NOTE_CHANGED, s.line, id, key, ch.field, ch.text);
        }
        return true;
    }

    /* A number's scale, offset, unit and decimals, and what OBDb does that
     * a signal file doesn't */
    bool number_fields(const Value& s, const Value& fmt, const std::string& id,
                       std::string* e, std::vector<change>* changes)
    {
        auto number = [&](const char* key, double fallback, double* out) {
            const Value* v = fmt.find(key);
            if (v == nullptr) {
                *out = fallback;
                return true;
            }
            if (v->type != Type::number) {
                return false;
            }
            *out = v->number;
            return true;
        };
        double mul = 1;
        double div = 1;
        double add = 0;
        if (!number("mul", 1, &mul) || !number("div", 1, &div) ||
            !number("add", 0, &add)) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "",
                 "its mul, div or add isn't a number");
            return false;
        }
        const double scale = div != 0 ? mul / div : 0;
        if (div == 0 || scale == 0 || !std::isfinite(scale)) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "",
                 "a scale of 0 or none (mul " + number_text(mul) + ", div " +
                     number_text(div) + ")");
            return false;
        }
        if (scale != 1) {
            *e += ",\"scale\":" + number_text(scale);
        }
        if (add != 0) {
            *e += ",\"offset\":" + number_text(add);
        }
        const std::string word = text_at(fmt, "unit");
        std::string unit;
        bool known = word.empty();
        for (const unit_word& u : kUnits) {
            if (word == u.obdb) {
                unit = u.havne;
                known = true;
            }
        }
        if (!known) {
            changes->push_back(
                {"unit",
                 "OBDb's '" + word +
                     "' isn't a unit Havne shows: the value is its number"});
        }
        if (!unit.empty()) {
            *e += ",\"unit\":";
            add_string(*e, unit);
        }
        const uint32_t decimals = decimals_of(scale, add);
        if (decimals != 0) {
            *e += ",\"decimals\":" + std::to_string(decimals);
        }
        double lo = 0;
        double hi = 0;
        const Value* max_v = fmt.find("max");
        if (max_v != nullptr && max_v->type == Type::number &&
            number("min", 0, &lo)) {
            hi = max_v->number;
            if (hi > lo) {
                changes->push_back({"max", "OBDb holds the value within " +
                                               number_text(lo) + " to " +
                                               number_text(hi) +
                                               "; Havne shows it as read"});
            }
        }
        double null_at = 0;
        if (fmt.find("nullmin") != nullptr && number("nullmin", 0, &null_at)) {
            changes->push_back({"nullmin", "OBDb shows nothing at or below " +
                                               number_text(null_at) +
                                               "; Havne shows the value"});
        }
        if (fmt.find("nullmax") != nullptr && number("nullmax", 0, &null_at)) {
            changes->push_back({"nullmax", "OBDb shows nothing at or above " +
                                               number_text(null_at) +
                                               "; Havne shows the value"});
        }
        return true;
    }

    /* An enumeration's labels, as the file's "values" */
    bool enum_values(const Value& s, const Value& map, uint32_t len,
                     const std::string& id, std::string* e,
                     std::vector<change>* changes)
    {
        if (map.type != Type::object) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "",
                 "its \"map\" isn't an object");
            return false;
        }
        const uint64_t top = (uint64_t{1} << len) - 1;
        std::vector<std::pair<uint32_t, std::string>> labels;
        for (const Member& m : map.members) {
            const std::string& k = m.key;
            bool decimal =
                !k.empty() && k.size() <= 10 && (k == "0" || k[0] != '0');
            uint64_t raw = 0;
            for (const char c : k) {
                decimal = decimal && c >= '0' && c <= '9';
                raw = raw * 10 + static_cast<uint64_t>(c - '0');
            }
            if (!decimal) {
                changes->push_back({"map", "the key '" +
                                               clean(k, 16, &decimal) +
                                               "' isn't a raw value OBDb "
                                               "matches: left out"});
                continue;
            }
            if (raw > top) {
                changes->push_back({"map", "raw " + k + " can't occur in " +
                                               std::to_string(len) +
                                               " bits: left out"});
                continue;
            }
            std::string label;
            if (m.value.type == Type::string) {
                label = m.value.text;
            } else if (m.value.type == Type::object) {
                label = text_at(m.value, "description");
                if (label.empty()) {
                    label = text_at(m.value, "value");
                }
            }
            bool cut = false;
            label = trimmed(clean(label, ECUIO_ENUM_LABEL_MAX, &cut));
            if (cut) {
                changes->push_back(
                    {"map", "raw " + k + "'s label cut to " +
                                std::to_string(ECUIO_ENUM_LABEL_MAX) +
                                " bytes"});
            }
            if (unformula(&label)) {
                changes->push_back({"map", "raw " + k +
                                               "'s label: its leading = + - or "
                                               "@ dropped"});
                label = trimmed(label);
            }
            if (label.empty()) {
                changes->push_back({"map", "raw " + k +
                                               " has no label Havne can show: "
                                               "left out"});
                continue;
            }
            labels.emplace_back(static_cast<uint32_t>(raw), label);
        }
        std::sort(labels.begin(), labels.end());
        if (labels.size() > ECUIO_ENUM_MAX_ENTRIES) {
            changes->push_back(
                {"map", "past " + std::to_string(ECUIO_ENUM_MAX_ENTRIES) +
                            " labels: the rest left out"});
            labels.resize(ECUIO_ENUM_MAX_ENTRIES);
        }
        if (labels.empty()) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "",
                 "an enumeration with no label Havne can show");
            return false;
        }
        *e += ",\"kind\":\"enum\",\"values\":{";
        for (size_t i = 0; i < labels.size(); ++i) {
            if (i > 0) {
                *e += ',';
            }
            *e += "\"" + std::to_string(labels[i].first) + "\":";
            add_string(*e, labels[i].second);
        }
        *e += "}";
        return true;
    }

    /* A synthetic: a ratio of two converted numbers is a computed signal */
    void synthetic(const Value& s)
    {
        if (s.type != Type::object) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, "", "", "",
                 "a synthetic that isn't an object");
            return;
        }
        const std::string id = text_at(s, "id");
        auto skip = [&](const std::string& why) {
            note(ECUIO_OBDB_NOTE_SKIPPED, s.line, id, "", "", why);
        };
        if (id.empty()) {
            skip("a synthetic with no \"id\"");
            return;
        }
        const Value* formula = s.find("formula");
        if (formula == nullptr || formula->type != Type::object) {
            skip("a synthetic with no \"formula\"");
            return;
        }
        const std::string op = text_at(*formula, "op");
        if (op != "ratio") {
            skip("a synthetic's '" + op +
                 "' formula isn't one a signal file computes");
            return;
        }
        const std::string a = text_at(*formula, "a");
        const std::string b = text_at(*formula, "b");
        if (numbers.count(a) == 0 || numbers.count(b) == 0) {
            skip("a ratio of a signal that isn't a converted number");
            return;
        }
        if (signal_count >= ECUIO_SIGNAL_FILE_MAX_SIGNALS) {
            skip("past the " + std::to_string(ECUIO_SIGNAL_FILE_MAX_SIGNALS) +
                 " signals a file holds");
            return;
        }
        std::vector<change> changes;
        const std::string key = unique_key(key_base(id), keys);
        bool cut = false;
        std::string name = trimmed(clean(text_at(s, "name"), 31, &cut));
        if (cut) {
            changes.push_back({"name", "cut to 31 bytes"});
        }
        if (unformula(&name)) {
            changes.push_back({"name", "its leading = + - or @ dropped"});
            name = trimmed(name);
        }
        if (name.empty()) {
            name = key;
            changes.push_back({"name", "none Havne can show: the key instead"});
        }
        bool repeated = false;
        name = unique_name(name, names, &repeated);
        if (repeated) {
            changes.push_back({"name",
                               "the same as another signal's: a number added"});
        }
        std::string e = "{\"key\":";
        add_string(e, key);
        e += ",\"name\":";
        add_string(e, name);
        e += ",\"kind\":\"computed\",\"expr\":\"" + numbers[a] + " / " +
             numbers[b] + "\"";
        const std::string word = text_at(s, "unit");
        for (const unit_word& u : kUnits) {
            if (word == u.obdb && u.havne[0] != '\0') {
                e += ",\"unit\":";
                add_string(e, u.havne);
            }
        }
        e += ",\"decimals\":2";
        const std::string path = text_at(s, "path");
        const std::string first = path.substr(0, path.find('.'));
        for (const path_group& g : kGroups) {
            if (first == g.path) {
                e += ",\"group\":\"" + std::string(g.group) + "\"";
            }
        }
        e += "}";
        if (!signals.empty()) {
            signals += ",\n    ";
        }
        signals += e;
        ++signal_count;
        note(ECUIO_OBDB_NOTE_CONVERTED, s.line, id, key, "", "converted");
        for (const change& ch : changes) {
            note(ECUIO_OBDB_NOTE_CHANGED, s.line, id, key, ch.field, ch.text);
        }
    }

    /* The whole set: the signal file's text */
    std::string run(const Value& root)
    {
        const Value* commands =
            root.type == Type::object ? root.find("commands") : nullptr;
        if (commands == nullptr || commands->type != Type::array) {
            throw not_obdb{root.line, root.column,
                           "this isn't an OBDb signal set: it has no "
                           "\"commands\" list"};
        }
        for (const Member& m : root.members) {
            if (m.key == "commands" || m.key == "synthetics" ||
                m.key == "ecu") {
                continue;
            }
            if (m.key == "diagnosticLevel") {
                note(ECUIO_OBDB_NOTE_FILE, m.line, "", "", "diagnosticLevel",
                     "OBDb enters a diagnostic level for the vehicle; Havne "
                     "asks in the default session");
            } else if (m.key == "signalGroups") {
                note(ECUIO_OBDB_NOTE_FILE, m.line, "", "", "signalGroups",
                     "signal groups aren't imported: each signal keeps its "
                     "own name");
            } else {
                note(ECUIO_OBDB_NOTE_FILE, m.line, "", "", "",
                     "the key " + ecuio_json::quoted(m.key) +
                         " isn't one OBDb's format has: left out");
            }
        }
        ecu_list = root.find("ecu");
        take_base();
        if (!commands->items.empty()) {
            note(ECUIO_OBDB_NOTE_FILE, commands->line, "", "", "freq",
                 "Havne asks each request in turn, never on OBDb's own period");
        }
        for (const Value& c : commands->items) {
            command(c);
        }
        if (const Value* synthetics = root.find("synthetics")) {
            if (synthetics->type == Type::array) {
                for (const Value& s : synthetics->items) {
                    synthetic(s);
                }
            }
        }
        if (signal_count == 0) {
            throw not_obdb{0, 0, "no signal in the set is one Havne can read"};
        }
        return file_text();
    }

    std::string file_text() const
    {
        bool cut = false;
        std::string name = options.name != nullptr && options.name[0] != '\0'
                               ? clean(options.name, 63, &cut)
                               : std::string("OBDb import");
        std::string out = "{\n  \"havne_signals\": 1,\n  \"name\": ";
        add_string(out, name);
        out +=
            ",\n  \"notes\": \"Converted from an OBDb signal set\",\n"
            "  \"source\": {\n    \"name\": \"OBDb editors and contributors\"";
        if (options.url != nullptr && options.url[0] != '\0') {
            out += ",\n    \"url\": ";
            add_string(out, clean(options.url, 512, &cut));
        }
        if (options.commit != nullptr && options.commit[0] != '\0') {
            out += ",\n    \"commit\": ";
            add_string(out, clean(options.commit, 512, &cut));
        }
        out += ",\n    \"license\": \"CC-BY-SA-4.0\",\n"
               "    \"modified\": \"Converted to Havne's signal-file format\"\n"
               "  },\n";
        if ((options.flags & ECUIO_OBDB_ALONE) == 0) {
            out += "  \"extends\": \"obd2\",\n";
        }
        out += "  \"ecus\": [";
        bool first = true;
        for (const module& m : modules) {
            if (!m.listed) {
                continue;
            }
            out += first ? "\n    " : ",\n    ";
            first = false;
            out += "{\"id\":";
            add_string(out, m.id);
            out += ",\"rx\":\"" + hex_text(m.rx, 3) + "\",\"tx\":\"" +
                   hex_text(m.tx, 3) + "\"}";
        }
        out += "\n  ],\n  \"signals\": [\n    " + signals + "\n  ]\n}\n";
        return out;
    }
};

} // namespace

ecuio_status_t ecuio_obdb_import(const char* bytes, size_t length,
                                 const ecuio_obdb_options_t* options,
                                 ecuio_obdb_note_fn note, void* user,
                                 char** out_json, size_t* out_length)
{
    ecuio_detail_clear();
    if (out_json == nullptr || out_length == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_json = nullptr;
    *out_length = 0;
    if (bytes == nullptr && length > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_obdb_options_t o{};
    if (options != nullptr) {
        o = *options;
    }
    if ((o.flags & ~ECUIO_OBDB_ALONE) != 0) {
        ecuio_detail_set("an option flag this core doesn't define");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (const uint32_t r : o.reserved) {
        if (r != 0) {
            ecuio_detail_set("the options' reserved words must be zero");
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    try {
        ecuio_json::Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = ECUIO_OBDB_MAX_BYTES;
        if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root,
                               &error, limits)) {
            ecuio_detail_set("%s", error.to_string().c_str());
            return error.words == "out of memory" ? ECUIO_ERR_OUT_OF_MEMORY
                                                  : ECUIO_ERR_INVALID_ARGUMENT;
        }
        importer im(o, note, user);
        const std::string text = im.run(root);
        // What the caller gets always loads: a refusal here is the
        // importer's own mistake, or a name the caller gave
        ecuio_signal_file_t* check = nullptr;
        const ecuio_status_t st =
            ecuio_signal_file_load(text.data(), text.size(), &check);
        if (st != ECUIO_OK) {
            const std::string why = ecuio_last_error_detail();
            ecuio_detail_set("the converted file is refused: %s", why.c_str());
            return st;
        }
        ecuio_signal_file_free(check);
        auto* p = static_cast<char*>(std::malloc(text.size() + 1));
        if (p == nullptr) {
            ecuio_detail_set("out of memory");
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(p, text.c_str(), text.size() + 1);
        *out_json = p;
        *out_length = text.size();
        return ECUIO_OK;
    } catch (const canceled&) {
        ecuio_detail_set("the import was canceled");
        return ECUIO_ERR_CANCELED;
    } catch (const not_obdb& n) {
        if (n.line == 0) {
            ecuio_detail_set("%s", n.words.c_str());
        } else {
            ecuio_detail_set("line %u, column %u: %s",
                             static_cast<unsigned>(n.line),
                             static_cast<unsigned>(n.column), n.words.c_str());
        }
        return ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the set couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

void ecuio_obdb_free(char* json)
{
    std::free(json);
}
