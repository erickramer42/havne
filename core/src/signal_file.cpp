// File/layer: core/src/signal_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, PluginTest, PluginWrapTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// The signal-file loader (ADR-0051): a file's JSON (json.h) into a
// signal set the validator passes, everything a file can get wrong
// refused here with the line and column, never the file's path.

#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "computed.h"
#include "error_detail.h"
#include "json.h"
#include "request_engine.h"
#include "signal_expr.h"
#include "signal_file_build.h"
#include "signal_file_parts.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecuio_sf {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;

/* The names a file's expressions read: its signals as read so far, each
 * found by its key's place */
struct entries_scope final : expr_scope {
    entries_scope(const std::vector<entry>& all,
                  const std::unordered_map<std::string, size_t>& places,
                  const ecuio_signal_set_t* obd2, const needs_pass* pass)
        : entries(all), at_key(places), needs(pass)
    {
        base = obd2;
    }

    bool find(const std::string& key, uint16_t* id,
              expr_kind* kind) const override
    {
        const auto at = at_key.find(key);
        if (at == at_key.end()) {
            /* A needed key stands for a number while the file is judged */
            if (needs == nullptr) {
                return false;
            }
            const auto needed = needs->ids.find(key);
            if (needed == needs->ids.end()) {
                return false;
            }
            *id = needed->second;
            *kind = expr_kind::number;
            return true;
        }
        const entry* e = &entries[at->second];
        *id = e->id;
        *kind = e->kind == "computed"                     ? expr_kind::computed
                : (e->kind == "text" || e->kind == "hex") ? expr_kind::text
                                                          : expr_kind::number;
        return true;
    }

    const std::vector<entry>& entries;
    const std::unordered_map<std::string, size_t>& at_key;
    const needs_pass* needs;
};

/* A signal's "expr" compiled, a refusal placed at the member */
ecuio_computed_t compiled(const Value& expr, const expr_scope& scope,
                          uint32_t format, std::vector<std::string>* names)
{
    try {
        return compile_expr(expr.text, scope, format, names);
    } catch (const expr_refused& r) {
        refuse(expr, "expr " + q(expr.text) + " " + r.words +
                         " (at character " + std::to_string(r.at + 1) + ")");
    }
}

uint32_t format_of(const Value& root, const char* what)
{
    if (root.members.empty() || root.members.front().key != "havne_signals") {
        refuse(root, std::string(what) + " starts with \"havne_signals\"");
    }
    const Value& version = root.members.front().value;
    expect(version, Type::number, "\"havne_signals\"");
    if (!version.integer || version.whole < ECUIO_SIGNAL_FILE_FORMAT_OLDEST ||
        version.whole > ECUIO_SIGNAL_FILE_FORMAT) {
        refuse(version, "\"havne_signals\" is format " +
                            (version.integer ? std::to_string(version.whole)
                                             : std::string("?")) +
                            ", and this Havne reads formats " +
                            std::to_string(ECUIO_SIGNAL_FILE_FORMAT_OLDEST) +
                            " to " + std::to_string(ECUIO_SIGNAL_FILE_FORMAT));
    }
    return static_cast<uint32_t>(version.whole);
}

void read_file(const Value& root, ecuio_signal_file& f, size_t max_signals)
{
    expect(root, Type::object, "the file");
    const uint32_t format = format_of(root, "a signal file");
    only_keys(root,
              {"havne_signals", "name", "source", "extends", "needs", "ecus",
               "signals"},
              "the file");

    parts p;
    p.at = &root;
    const Value* name = root.find("name");
    if (name == nullptr) {
        refuse(root, "the file needs a \"name\"");
    }
    p.name = text_of(*name, 1, 63, "\"name\"");
    p.profile = profile_of(p.name);
    if (p.profile.empty()) {
        refuse(*name,
               "\"name\" " + q(p.name) +
                   " holds no letter or digit to make a profile name from");
    }
    if (p.profile.size() > ECUIO_PROFILE_NAME_MAX) {
        refuse(*name, "\"name\" makes a profile name longer than " +
                          std::to_string(ECUIO_PROFILE_NAME_MAX) + " characters");
    }

    const Value* source = root.find("source");
    if (source == nullptr) {
        refuse(root, "the file needs a \"source\": where its signals come from");
    }
    read_source(*source);
    take_source(*source, f.source);

    p.extends = root.find("extends");
    p.ecus = root.find("ecus");
    p.signals_at = root.find("signals");
    p.own_list = true;
    p.format = format;
    p.max_signals = max_signals;
    read_needs(root, format, &p.needs);
    build(p, f);
}

/* Computed signals reading each other (ADR-0062), refused at the first
 * signal of a circle or a chain too deep in the file's order, at its
 * expr. Returns each signal's depth, 0 for one that isn't computed. */
std::vector<uint32_t> check_chains(const std::vector<entry>& entries,
                                   const ecuio_signal_file& f)
{
    std::vector<chain_node> nodes(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const entry& e = entries[i];
        nodes[i] = {&e.key, e.id,
                    e.kind == "computed"
                        ? &f.computed[f.defs[e.def].table_first]
                        : nullptr};
    }
    try {
        return chain_depths(nodes);
    } catch (const chain_refused& r) {
        refuse(*entries[r.node].v->find("expr"), r.words);
    }
}

/* The base's defs into the set, less those a format 1 or 2 file
 * replaces: a signal named as a base def stands in its place with its
 * ID, so what reads that ID (a base program, a layout's cell, a focus)
 * reads the file's, as a plugin variant's later file replaces a signal
 * by key. A file of format 1 or 2 names each signal once; format 3
 * replaces no base def and may repeat a name (ADR-0065 section 9). */
void take_base(const parts& in, const ecuio_signal_set_t* base,
               std::vector<entry>& entries, ecuio_signal_file& f)
{
    std::vector<ecuio_signal_def_t> kept;
    for (const ecuio_signal_def_t& d : f.defs) {
        entry* by = nullptr;
        for (entry& e : entries) {
            if (e.name != d.name) {
                continue;
            }
            if (in.format >= 3) {
                named_as_base(in, e, static_cast<size_t>(&e - entries.data()),
                              d);
            } else {
                by = &e;
            }
        }
        if (by == nullptr) {
            kept.push_back(d);
        } else {
            by->id = d.id;
            by->replaces = true;
            ecuio_least::raise(f.least, ecuio_least::kReplacesByName);
        }
    }
    f.computed.clear();
    for (ecuio_signal_def_t& d : kept) {
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            continue;
        }
        const ecuio_computed_t p = base->computed[d.table_first];
        d.table_first = static_cast<uint16_t>(f.computed.size());
        f.computed.push_back(p);
    }
    f.defs.swap(kept);
}

/* Formats 1 and 2: the signal at i, named as an earlier one, is refused */
void one_name_each(std::unordered_map<std::string, size_t>& first_named,
                   const std::vector<entry>& entries, size_t i,
                   const Value& name_v, const std::string& what)
{
    const auto named = first_named.emplace(entries[i].name, i);
    if (!named.second) {
        refuse(name_v, what + " has the name " + q(entries[i].name) +
                           ", which signal " +
                           q(entries[named.first->second].key) +
                           " has too: a name names one signal");
    }
}

void build(const parts& in, ecuio_signal_file& f)
{
    if (in.needs.empty()) {
        build(in, f, nullptr);
    } else {
        build_around_needs(in, f);
    }
}

void build(const parts& in, ecuio_signal_file& f, needs_pass* pass)
{
    f.name = in.name;
    f.profile = in.profile;
    f.format = in.format;
    /* An older core refuses a format it doesn't know, whatever the file
     * uses of it, so the format's row covers each gate inside it */
    ecuio_least::raise(f.least, ecuio_least::kSignalFormatSince[in.format]);
    const ecuio_signal_set_t* base = nullptr;
    if (in.extends != nullptr) {
        const Value* extends = in.extends;
        expect(*extends, Type::string, "\"extends\"");
        if (extends->text != "obd2") {
            refuse(*extends, "\"extends\" " + q(extends->text) +
                                 " isn't a set a file extends: only \"obd2\"");
        }
        base = ecuio_obd2_signal_set();
        f.extends = extends->text;
        f.defs.assign(base->defs, base->defs + base->count);
        f.channels.assign(base->channels, base->channels + base->channel_count);
    }

    std::vector<std::pair<std::string, uint32_t>> ecus;
    read_ecus(in.ecus, f, &ecus);
    if (f.channels.size() > ecuio_least::kEcusBefore) {
        ecuio_least::raise(f.least, ecuio_least::kEcus);
    }

    const Value* signals = in.signals_at;
    if (signals == nullptr) {
        refuse(*in.at, "the file needs \"signals\"");
    }
    std::vector<const Value*> list = in.signals;
    if (in.own_list) {
        expect(*signals, Type::array, "\"signals\"");
        for (const Value& v : signals->items) {
            list.push_back(&v);
        }
    }
    if (list.empty() && !in.may_be_empty) {
        refuse(*signals, "\"signals\" lists none");
    }
    if (list.size() > in.max_signals) {
        refuse(*signals, "\"signals\" lists " + std::to_string(list.size()) +
                             ", more than " + std::to_string(in.max_signals));
    }
    if (list.size() > ECUIO_SIGNAL_FILE_MAX_SIGNALS) {
        ecuio_least::raise(f.least, ecuio_least::kSetSignals);
    }

    /* First every signal's key and kind, so an expression can read a
     * signal listed after it */
    std::vector<entry> entries;
    std::unordered_map<std::string, size_t> at_key; /* a key's place */
    for (const Value* sp : list) {
        const Value& s = *sp;
        expect(s, Type::object, "a signal");
        const Value* key = s.find("key");
        if (key == nullptr) {
            refuse(s, "a signal needs a \"key\"");
        }
        entry e;
        e.v = &s;
        e.key = text_of(*key, 1, 31, "a signal's key");
        if (!key_ok(e.key)) {
            refuse(*key, "key " + q(e.key) + " takes 1 to 31 of a-z, 0-9 and _");
        }
        if (!at_key.emplace(e.key, entries.size()).second) {
            refuse(*key, "key " + q(e.key) + " is given twice");
        }
        e.kind = "number";
        if (const Value* kind = s.find("kind")) {
            expect(*kind, Type::string, "signal " + q(e.key) + "'s kind");
            e.kind = kind->text;
            if (e.kind != "number" && e.kind != "text" && e.kind != "hex" &&
                e.kind != "enum" && e.kind != "computed") {
                refuse(*kind, "signal " + q(e.key) + "'s kind " + q(e.kind) +
                                  " isn't number, text, hex, enum or computed");
            }
        }
        if (const Value* n = s.find("name")) {
            if (n->type == Type::string) {
                e.name = n->text;
            }
        }
        entries.push_back(e);
    }

    if (base != nullptr) {
        take_base(in, base, entries, f);
    }
    const size_t first_own = f.defs.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].def = static_cast<uint32_t>(first_own + i);
        if (entries[i].id == 0) {
            entries[i].id = static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE + i);
        }
    }

    f.defs.resize(first_own + entries.size());
    const entries_scope scope(entries, at_key, base, pass);
    std::unordered_map<std::string, size_t> first_named; /* a name's first */
    std::vector<std::pair<const entry*, std::vector<std::string>>> programs;
    for (size_t i = 0; i < entries.size(); ++i) {
        const entry& e = entries[i];
        const Value& s = *e.v;
        const std::string what = "signal " + q(e.key);
        ecuio_signal_def_t& d = f.defs[e.def];
        d = ecuio_signal_def_t{};
        d.id = e.id;
        d.scale = 1.0;

        const bool computed = e.kind == "computed";
        const bool text = e.kind == "text" || e.kind == "hex";
        if (computed) {
            only_keys(s,
                      {"key", "name", "kind", "expr", "max_age", "unit",
                       "decimals", "group", "plot", "gauge"},
                      what);
        } else if (text) {
            only_keys(s, {"key", "name", "kind", "ecu", "read", "field", "group",
                          "plot"},
                      what);
        } else if (e.kind == "enum") {
            only_keys(s, {"key", "name", "kind", "ecu", "read", "field", "values",
                          "group", "plot"},
                      what);
        } else {
            only_keys(s,
                      {"key", "name", "kind", "ecu", "read", "field", "scale",
                       "offset", "unit", "decimals", "group", "plot", "gauge"},
                      what);
        }

        const Value* name_v = s.find("name");
        if (name_v == nullptr) {
            refuse(s, what + " needs a \"name\"");
        }
        copy_text(d.name, sizeof d.name, text_of(*name_v, 1, 31, what + "'s name"));
        no_formula(*name_v, what + "'s name");
        if (in.format < 3) {
            one_name_each(first_named, entries, i, *name_v, what);
        }
        if (const Value* unit = s.find("unit")) {
            copy_text(d.unit, sizeof d.unit, text_of(*unit, 0, 15, what + "'s unit"));
            no_formula(*unit, what + "'s unit");
        }
        if (const Value* decimals = s.find("decimals")) {
            d.decimal_places =
                static_cast<uint8_t>(whole_of(*decimals, 0, 3, what + "'s decimals"));
        }
        if (const Value* group = s.find("group")) {
            d.group = group_of(*group, what + "'s group");
        }
        if (const Value* plot = s.find("plot")) {
            d.plot_rank = static_cast<uint8_t>(whole_of(*plot, 0, 255, what + "'s plot"));
        }

        if (computed) {
            d.flags = ECUIO_DEF_FLAG_COMPUTED;
            const Value* expr = s.find("expr");
            if (expr == nullptr) {
                refuse(s, what + " is computed and needs an \"expr\"");
            }
            expect(*expr, Type::string, what + "'s expr");
            std::vector<std::string> names;
            ecuio_computed_t p = compiled(
                *expr, scope, i < in.formats.size() ? in.formats[i] : in.format,
                &names);
            if (const Value* ages = s.find("max_age")) {
                expect(*ages, Type::object, what + "'s max_age");
                for (const Member& m : ages->members) {
                    size_t k = 0;
                    while (k < names.size() && names[k] != m.key) {
                        ++k;
                    }
                    if (k == names.size()) {
                        refuse(m, what + "'s max_age names " + q(m.key) +
                                      ", which its expr doesn't read");
                    }
                    for (const entry& x : entries) {
                        if (x.key == m.key && x.kind == "computed") {
                            refuse(m, aged_computed_words(e.key, m.key));
                        }
                    }
                    const double seconds = finite_of(m.value, what + "'s max_age");
                    if (seconds < 0.1 || seconds > 60.0) {
                        refuse(m.value, what + "'s max_age for " + q(m.key) +
                                            " takes 0.1 to 60 seconds");
                    }
                    p.max_age_us[k] = static_cast<uint32_t>(std::llround(seconds * 1e6));
                }
            }
            if (f.computed.size() >= ECUIO_SET_MAX_TABLE) {
                refuse(s, "the file holds more than 65536 computed signals");
            }
            d.table_first = static_cast<uint16_t>(f.computed.size());
            f.computed.push_back(p);
            f.exprs.emplace_back(e.def, expr->text);
            read_gauge(s, d, e.def, f, what);
            continue;
        }

        read_field(s, e.kind, ecus, f, d, what);
        if (e.kind == "enum") {
            read_values(s, f, d, what);
        } else if (!text) {
            if (const Value* scale = s.find("scale")) {
                d.scale = finite_of(*scale, what + "'s scale");
                if (d.scale == 0.0) {
                    refuse(*scale, what + "'s scale is 0, which reads every reply as its offset");
                }
            }
            if (const Value* offset = s.find("offset")) {
                d.offset = finite_of(*offset, what + "'s offset");
            }
            read_gauge(s, d, e.def, f, what);
        }
    }
    if (in.format >= 3) {
        one_signal_each_address(entries, f);
    }
    const std::vector<uint32_t> depth = check_chains(entries, f);
    if (pass != nullptr) {
        settle_needs(in, entries, f, *pass);
        return;
    }
    for (const entry& e : entries) {
        f.keys.emplace_back(e.key, e.def);
    }

    /* A replacement keeps the base def's ID, so a computed signal reading
     * that ID (obd2's Boost reads MAP and baro) reads the file's def. A
     * computed one is read in format 2: the reader's, or for a base
     * program the replacement's. */
    const auto format_of = [&](size_t i) {
        return i < in.formats.size() ? in.formats[i] : in.format;
    };
    for (size_t r = 0; r < f.defs.size(); ++r) {
        const ecuio_signal_def_t& d = f.defs[r];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            continue;
        }
        const bool base_program = r < first_own;
        const ecuio_computed_t& p = f.computed[d.table_first];
        for (uint32_t k = 0; k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS;
             ++k) {
            for (size_t i = 0; i < entries.size(); ++i) {
                const entry& e = entries[i];
                if (!e.replaces || e.id != p.input_ids[k] ||
                    e.kind == "number" || e.kind == "enum") {
                    continue;
                }
                const std::string reads =
                    replaced_words(e.key, e.name, q(d.name));
                if (e.kind != "computed") {
                    refuse(*e.v, reads + kReplacedNotANumber);
                }
                if (format_of(base_program ? i : r - first_own) < 2) {
                    refuse(*e.v, reads + kReplacedFormat);
                }
                if (p.max_age_us[k] != 0) {
                    refuse(*e.v, reads + kReplacedAged);
                }
                if (base_program && depth[i] + 1 > ECUIO_COMPUTED_MAX_DEPTH) {
                    refuse(*e.v, reads + too_deep_words(d.name, depth[i] + 1));
                }
            }
        }
    }

    f.set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    f.set.count = static_cast<uint32_t>(f.defs.size());
    f.set.defs = f.defs.data();
    f.set.channel_count = static_cast<uint32_t>(f.channels.size());
    f.set.channels = f.channels.empty() ? nullptr : f.channels.data();
    f.set.enum_count = static_cast<uint32_t>(f.enums.size());
    f.set.enums = f.enums.empty() ? nullptr : f.enums.data();
    f.set.computed_count = static_cast<uint32_t>(f.computed.size());
    f.set.computed = f.computed.empty() ? nullptr : f.computed.data();

    /* The rules above leave the validator nothing to refuse; if it does,
     * the words name the signal it named */
    ecuio_validation_t why{};
    if (ecuio_validate_signal_set(&f.set, &why) ==
        ECUIO_VALIDATE_ERR_OUT_OF_MEMORY) {
        throw std::bad_alloc();
    }
    if (why.code != ECUIO_VALIDATE_OK) {
        const size_t first = first_own;
        /* Only these codes' index is a def's */
        const bool names_a_def =
            why.code > 0 || why.code == ECUIO_VALIDATE_ERR_DUPLICATE_ID ||
            why.code == ECUIO_VALIDATE_ERR_TABLE_RANGE;
        if (names_a_def && why.index != UINT32_MAX && why.index >= first &&
            why.index - first < entries.size()) {
            refuse(*entries[why.index - first].v,
                   "signal " + q(entries[why.index - first].key) +
                       " is refused by the set's validator (code " +
                       std::to_string(why.code) + ")");
        }
        refuse(*in.at, "the set is refused by its validator (code " +
                           std::to_string(why.code) + ")");
    }

    /* Each ECU's requests within a module's most, as registration checks */
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    for (size_t i = 0; i < f.channels.size() && i < ECUIO_MAX_CHANNELS; ++i) {
        rx_ids[i] = f.channels[i].rx_id;
    }
    ecuio_derived derived;
    const ecuio_plan_result fits = ecuio_engine_plan(
        &f.set, rx_ids, f.set.channel_count, nullptr, &derived);
    if (fits == ecuio_plan_result::out_of_memory) {
        throw std::bad_alloc();
    }
    if (fits == ecuio_plan_result::module_full) {
        const size_t first = first_own;
        const uint32_t overflow = derived.overflow_def;
        const Value& at =
            (overflow >= first && overflow - first < entries.size())
                ? *entries[overflow - first].v
                : *signals;
        /* The ECU by its id, the base's by its reply ID */
        const uint32_t rx =
            overflow < f.defs.size() ? f.defs[overflow].message_id : 0;
        char hex[16];
        std::snprintf(hex, sizeof hex, "%X", static_cast<unsigned>(rx));
        std::string ecu = std::string("the ECU at ") + hex;
        for (const auto& known : ecus) {
            if (known.second == rx) {
                ecu = "ecu " + q(known.first);
            }
        }
        refuse(at, "the signals ask " + ecu + " more than " +
                       std::to_string(ECUIO_MODULE_MAX_REQUESTS) +
                       " requests, the base's counted");
    }
    if (derived.total > ecuio_least::kRequestsBefore) {
        ecuio_least::raise(f.least, ecuio_least::kRequests);
    }
    if (derived.total > ecuio_least::kSetRequestsBefore) {
        ecuio_least::raise(f.least, ecuio_least::kSetRequests);
    }
}

ecuio_status_t load(const char* bytes, size_t length, size_t max_bytes,
                    size_t max_signals, ecuio_signal_file** out)
{
    try {
        ecuio_json::Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = max_bytes;
        if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root,
                               &error, limits)) {
            ecuio_detail_set("%s", error.to_string().c_str());
            return error.words == "out of memory" ? ECUIO_ERR_OUT_OF_MEMORY
                                                   : ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::unique_ptr<ecuio_signal_file> f(new ecuio_signal_file());
        read_file(root, *f, max_signals);
        if (length > 0) {
            f->bytes.assign(bytes, length);
        }
        *out = f.release();
        return ECUIO_OK;
    } catch (const refused& r) {
        ecuio_detail_set("line %u, column %u: %s", static_cast<unsigned>(r.line),
                         static_cast<unsigned>(r.column), r.words.c_str());
        return ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the file couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

} // namespace ecuio_sf

extern "C" {

ecuio_status_t ecuio_signal_file_load(const char* bytes, size_t length,
                                      ecuio_signal_file_t** out)
{
    return ecuio_signal_file_load_with(bytes, length, 0, out);
}

ecuio_status_t ecuio_signal_file_load_with(const char* bytes, size_t length,
                                           uint32_t flags,
                                           ecuio_signal_file_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (bytes == nullptr && length > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if ((flags & ~ECUIO_SIGNAL_FILE_LOAD_SET) != 0) {
        ecuio_detail_set(
            "a signal file's load takes no flag 0x%x",
            static_cast<unsigned>(flags & ~ECUIO_SIGNAL_FILE_LOAD_SET));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const bool set = (flags & ECUIO_SIGNAL_FILE_LOAD_SET) != 0;
    return ecuio_sf::load(
        bytes, length,
        set ? ECUIO_SIGNAL_SET_MAX_BYTES : ECUIO_SIGNAL_FILE_MAX_BYTES,
        set ? ECUIO_SIGNAL_SET_MAX_SIGNALS : ECUIO_SIGNAL_FILE_MAX_SIGNALS,
        out);
}

const ecuio_signal_set_t* ecuio_signal_file_set(const ecuio_signal_file_t* file)
{
    return file != nullptr ? &file->set : nullptr;
}

const char* ecuio_signal_file_name(const ecuio_signal_file_t* file)
{
    return file != nullptr ? file->name.c_str() : "";
}

const char* ecuio_signal_file_extends(const ecuio_signal_file_t* file)
{
    return file != nullptr ? file->extends.c_str() : "";
}

const char* ecuio_signal_file_profile(const ecuio_signal_file_t* file)
{
    return file != nullptr ? file->profile.c_str() : "";
}

const ecuio_signal_def_t* ecuio_signal_file_find(const ecuio_signal_file_t* file,
                                                 const char* key)
{
    if (file == nullptr || key == nullptr) {
        return nullptr;
    }
    for (const auto& k : file->keys) {
        if (k.first == key) {
            return &file->defs[k.second];
        }
    }
    return nullptr;
}

const char* ecuio_signal_file_key(const ecuio_signal_file_t* file,
                                  const ecuio_signal_def_t* def)
{
    if (file == nullptr || def == nullptr) {
        return "";
    }
    /* By address: a def of another set, or a copy, with the same ID is
     * not this file's */
    for (const auto& k : file->keys) {
        if (&file->defs[k.second] == def) {
            return k.first.c_str();
        }
    }
    return "";
}

uint32_t ecuio_signal_file_format(const ecuio_signal_file_t* file)
{
    return file != nullptr ? file->format : 0;
}

const char* ecuio_signal_file_expr(const ecuio_signal_file_t* file,
                                   const ecuio_signal_def_t* def)
{
    if (file == nullptr || def == nullptr) {
        return "";
    }
    for (const auto& e : file->exprs) {
        if (&file->defs[e.first] == def) {
            return e.second.c_str();
        }
    }
    return "";
}

const ecuio_signal_gauge_t*
ecuio_signal_file_gauge(const ecuio_signal_file_t* file,
                        const ecuio_signal_def_t* def)
{
    if (file == nullptr || def == nullptr) {
        return nullptr;
    }
    for (const auto& g : file->gauges) {
        if (&file->defs[g.first] == def) {
            return &g.second;
        }
    }
    return nullptr;
}

ecuio_status_t ecuio_signal_file_source(const ecuio_signal_file_t* file,
                                        ecuio_signal_source_t* out)
{
    if (file == nullptr || out == nullptr) {
        ecuio_detail_set("the source needs a file and a struct to fill");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof(ecuio_signal_source_t)) {
        ecuio_detail_set("the source's struct_size is %u, under its %u bytes",
                         static_cast<unsigned>(out->struct_size),
                         static_cast<unsigned>(sizeof(ecuio_signal_source_t)));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out = file->source;
    out->struct_size = sizeof(ecuio_signal_source_t);
    return ECUIO_OK;
}

const char* ecuio_signal_file_text(const ecuio_signal_file_t* file,
                                   size_t* length)
{
    if (length != nullptr) {
        *length = file != nullptr ? file->bytes.size() : 0;
    }
    return file != nullptr ? file->bytes.c_str() : "";
}

void ecuio_signal_file_free(ecuio_signal_file_t* file)
{
    delete file;
}

} // extern "C"
