// File/layer: core/src/obdb_importer.h | internal, not ABI-exported; included by obdb_import.cpp alone
// C++17, Qt-free
// Testing: UNIT: ObdbImportTest (test_core); FUZZ: fuzz_obdb_import
//
// The OBDb import's converter (obdb_import.cpp): the modules, the commands,
// the signals and the file it writes.

#ifndef ECUIO_OBDB_IMPORTER_H
#define ECUIO_OBDB_IMPORTER_H

#include "ecuio/obdb.h"

#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "json.h"
#include "obdb_import.h"
#include "obdb_import_words.h"
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

namespace ecuio_obdb_impl {

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
    std::map<uint32_t, uint32_t> asked_of; /* rx: its requests in asked */
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
            if (ecuio_engine_request_for_def(&base->defs[i], req, &len) &&
                asked.insert(request_code(base->defs[i].message_id, req, len))
                    .second) {
                ++asked_of[base->defs[i].message_id];
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
        if (new_request && asked_of[rx] >= ECUIO_MODULE_MAX_REQUESTS) {
            skip_signals(c, std::string("past the ") +
                                std::to_string(ECUIO_MODULE_MAX_REQUESTS) +
                                " requests one module is asked" +
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
            ++asked_of[rx];
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

} // namespace ecuio_obdb_impl

#endif /* ECUIO_OBDB_IMPORTER_H */
