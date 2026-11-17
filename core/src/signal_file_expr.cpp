// File/layer: core/src/signal_file_expr.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalExprTest (test_signals); FUZZ: fuzz_signal_file
//
// An expression against a loaded signal file (signals.h, ADR-0062): the
// check an editor runs before the file is written, and the name an
// expression reads a def by. The compiler and the chain rules are the
// loader's (signal_expr.h), so a check and a load judge alike.

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "signal_expr.h"
#include "signal_file_build.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

using ecuio_sf::chain_node;
using ecuio_sf::chain_refused;
using ecuio_sf::expr_kind;
using ecuio_sf::expr_refused;
using ecuio_sf::q;

constexpr size_t kNone = SIZE_MAX;

/* A refused expression: the character, from 1, and the whole sentence */
struct refusal {
    uint32_t at;
    std::string words;
};

/* One of the file's own signals as the file would hold it: the checked
 * expression's signal is computed whatever it is today */
struct own_signal {
    const std::string* key; /* NULL: the checked expression with no key */
    uint16_t id;
    expr_kind kind;
    const ecuio_computed_t* program; /* a computed signal's */
    size_t def;                      /* its def's index; kNone for a new one */
};

/* The names a file's expressions read, from the file as loaded */
struct file_scope final : ecuio_sf::expr_scope {
    bool find(const std::string& key, uint16_t* id,
              expr_kind* kind) const override
    {
        for (const own_signal& s : own) {
            if (s.key != nullptr && *s.key == key) {
                *id = s.id;
                *kind = s.kind;
                return true;
            }
        }
        return false;
    }

    size_t by_id(uint16_t id) const
    {
        for (size_t i = 0; i < own.size(); ++i) {
            if (own[i].id == id) {
                return i;
            }
        }
        return kNone;
    }

    std::vector<own_signal> own;
};

bool is_computed(const ecuio_signal_def_t& d)
{
    return (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0;
}

bool reads(const ecuio_computed_t& p, uint16_t id, bool aged = false)
{
    for (uint32_t k = 0; k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS;
         ++k) {
        if (p.input_ids[k] == id && (!aged || p.max_age_us[k] != 0)) {
            return true;
        }
    }
    return false;
}

/* Whether the file's own def at i, whose program p keeps an age bound on
 * the input with that ID, reads it by key and not as "obd2.<key>", a
 * stand-in's other name: the loader words the bound by the name the
 * reader's "max_age" gave */
bool aged_by_key(const ecuio_signal_file& f, const file_scope& scope, size_t i,
                 const ecuio_computed_t& p, uint16_t id, const std::string& key)
{
    for (const auto& e : f.exprs) {
        if (e.first != i) {
            continue;
        }
        std::vector<std::string> names;
        try {
            ecuio_sf::compile_expr(e.second, scope, ECUIO_SIGNAL_FILE_FORMAT,
                                   &names, nullptr);
        } catch (const expr_refused&) {
            return false;
        }
        for (uint32_t k = 0; k < p.input_count && k < names.size(); ++k) {
            if (p.input_ids[k] == id && p.max_age_us[k] != 0) {
                return names[k] == key;
            }
        }
    }
    return false;
}

/* The place, from 1, of the input a chain's refusal comes through: the
 * one a circle leaves the checked signal by, or its deepest; 1 when the
 * refusal is another signal's */
uint32_t chain_place(const chain_refused& r, const file_scope& scope,
                     size_t checked, const ecuio_computed_t& program,
                     const std::vector<size_t>& places)
{
    size_t through = kNone;
    for (size_t i = 0; i < r.circle.size(); ++i) {
        if (r.circle[i] == checked) {
            through = r.circle[(i + 1) % r.circle.size()];
        }
    }
    if (r.circle.empty() && r.node == checked) {
        uint32_t deepest = 0;
        for (uint32_t k = 0; k < program.input_count; ++k) {
            const size_t n = scope.by_id(program.input_ids[k]);
            if (n != kNone && r.depths[n] > deepest) {
                deepest = r.depths[n];
                through = n;
            }
        }
    }
    if (through == kNone) {
        return 1;
    }
    for (uint32_t k = 0; k < program.input_count; ++k) {
        if (program.input_ids[k] == scope.own[through].id) {
            return static_cast<uint32_t>(places[k] + 1);
        }
    }
    return 1;
}

/* text as key's "expr" in f: out filled, or a refusal thrown */
void check(const ecuio_signal_file& f, const std::string& key,
           const std::string& text, ecuio_expr_check_t& out)
{
    file_scope scope;
    scope.base = f.extends == "obd2" ? ecuio_obd2_signal_set() : nullptr;
    scope.own.reserve(f.keys.size() + 1);
    /* Each def's place among the file's own, kNone for a base def */
    std::vector<size_t> own_of(f.defs.size(), kNone);
    size_t checked = kNone;
    for (const auto& k : f.keys) {
        const ecuio_signal_def_t& d = f.defs[k.second];
        own_signal s{&k.first, d.id, expr_kind::number, nullptr, k.second};
        if (is_computed(d)) {
            s.kind = expr_kind::computed;
            s.program = &f.computed[d.table_first];
        } else if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            s.kind = expr_kind::text;
        }
        if (!key.empty() && k.first == key) {
            checked = scope.own.size();
        }
        own_of[k.second] = scope.own.size();
        scope.own.push_back(s);
    }
    /* A new signal takes the ID the loader gives the next in the list */
    const auto next_id =
        static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE + f.keys.size());
    const bool was_computed =
        checked != kNone && scope.own[checked].kind == expr_kind::computed;
    if (checked != kNone) {
        scope.own[checked].kind = expr_kind::computed;
    } else if (!key.empty()) {
        checked = scope.own.size();
        scope.own.push_back(
            own_signal{&key, next_id, expr_kind::computed, nullptr, kNone});
    }

    std::vector<std::string> names;
    std::vector<size_t> places;
    ecuio_computed_t program{};
    try {
        program = ecuio_sf::compile_expr(text, scope, ECUIO_SIGNAL_FILE_FORMAT,
                                         &names, &places);
    } catch (const expr_refused& r) {
        throw refusal{static_cast<uint32_t>(r.at + 1),
                      "the expression " + r.words};
    }
    if (checked == kNone) {
        checked = scope.own.size();
        scope.own.push_back(
            own_signal{nullptr, next_id, expr_kind::computed, nullptr, kNone});
    }
    own_signal& self = scope.own[checked];
    self.program = &program;
    const std::string reader =
        self.def != kNone ? q(f.defs[self.def].name) : "the expression";

    /* "signal 'k' replaces 'Name', which <reader> reads": a signal named
     * as a base def stands in its place, read as "obd2.<key>" too */
    const auto replaced = [&](const own_signal& s, const std::string& by) {
        return ecuio_sf::replaced_words(*s.key, f.defs[s.def].name, by);
    };

    bool format2 = false;
    for (uint32_t k = 0; k < program.input_count; ++k) {
        const size_t n = scope.by_id(program.input_ids[k]);
        if (n == kNone) {
            continue;
        }
        if (scope.own[n].kind == expr_kind::text) {
            throw refusal{static_cast<uint32_t>(places[k] + 1),
                          replaced(scope.own[n], reader) +
                              ecuio_sf::kReplacedNotANumber};
        }
        format2 = format2 || scope.own[n].kind == expr_kind::computed;
    }

    std::vector<chain_node> nodes(scope.own.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const own_signal& s = scope.own[i];
        nodes[i] = {s.key, s.id,
                    s.kind == expr_kind::computed ? s.program : nullptr};
    }
    std::vector<uint32_t> depth;
    try {
        depth = ecuio_sf::chain_depths(nodes);
    } catch (const chain_refused& r) {
        throw refusal{chain_place(r, scope, checked, program, places), r.words};
    }

    /* What reads the checked signal today reads a computed one once the
     * file holds the expression: none may keep an age bound on it, and
     * all of them need format 2 */
    for (size_t i = 0; i < f.defs.size() && self.def != kNone; ++i) {
        const ecuio_signal_def_t& d = f.defs[i];
        if (!is_computed(d) || i == self.def) {
            continue;
        }
        const ecuio_computed_t& p = f.computed[d.table_first];
        if (!reads(p, self.id)) {
            continue;
        }
        format2 = true;
        if (was_computed || !reads(p, self.id, true)) {
            continue;
        }
        const bool stands_in = self.id < ECUIO_USER_SIG_ID_BASE;
        if (own_of[i] != kNone &&
            (!stands_in || aged_by_key(f, scope, i, p, self.id, key))) {
            throw refusal{1, ecuio_sf::aged_computed_words(
                                 *scope.own[own_of[i]].key, key)};
        }
        throw refusal{1, replaced(self, q(d.name)) + ecuio_sf::kReplacedAged};
    }

    /* A base program over a signal that stands in for its input sits one
     * deeper than it */
    for (size_t i = 0; i < f.defs.size(); ++i) {
        const ecuio_signal_def_t& d = f.defs[i];
        if (!is_computed(d) || own_of[i] != kNone) {
            continue;
        }
        const ecuio_computed_t& p = f.computed[d.table_first];
        for (uint32_t k = 0; k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS;
             ++k) {
            const size_t n = scope.by_id(p.input_ids[k]);
            if (n != kNone && scope.own[n].kind == expr_kind::computed &&
                scope.own[n].def != kNone &&
                depth[n] + 1 > ECUIO_COMPUTED_MAX_DEPTH) {
                throw refusal{
                    1, replaced(scope.own[n], q(d.name)) +
                           ecuio_sf::too_deep_words(d.name, depth[n] + 1)};
            }
        }
    }

    if (!format2) {
        try {
            std::vector<std::string> unused;
            ecuio_sf::compile_expr(text, scope, ECUIO_SIGNAL_FILE_FORMAT_OLDEST,
                                   &unused, nullptr);
        } catch (const expr_refused&) {
            format2 = true;
        }
    }
    out.format =
        format2 ? ECUIO_SIGNAL_FILE_FORMAT : ECUIO_SIGNAL_FILE_FORMAT_OLDEST;
    out.depth = depth[checked];
    out.program = program;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_signal_file_check_expr(const ecuio_signal_file_t* file,
                                            const char* key, const char* expr,
                                            size_t length,
                                            ecuio_expr_check_t* out)
{
    ecuio_detail_clear();
    if (file == nullptr || out == nullptr || (expr == nullptr && length != 0)) {
        ecuio_detail_set("the check needs a file, an expression and a struct "
                         "to fill");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof(ecuio_expr_check_t)) {
        ecuio_detail_set("the check's struct_size is %u, under its %u bytes",
                         static_cast<unsigned>(out->struct_size),
                         static_cast<unsigned>(sizeof(ecuio_expr_check_t)));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        const std::string k = key != nullptr ? key : "";
        if (!k.empty() && !ecuio_sf::key_ok(k)) {
            ecuio_detail_set("key %s takes 1 to 31 of a-z, 0-9 and _",
                             q(k).c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        ecuio_expr_check_t filled{};
        filled.struct_size = sizeof(ecuio_expr_check_t);
        try {
            /* Judged before the copy: no byte past the bound is read */
            if (length > ECUIO_EXPR_MAX_BYTES) {
                throw refusal{1,
                              "the expression " + ecuio_sf::too_long_words()};
            }
            check(*file, k, std::string(expr != nullptr ? expr : "", length),
                  filled);
        } catch (const refusal& r) {
            filled = ecuio_expr_check_t{};
            filled.struct_size = sizeof(ecuio_expr_check_t);
            filled.at = r.at;
            std::memcpy(out, &filled, sizeof filled);
            ecuio_detail_set("%s", r.words.c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::memcpy(out, &filled, sizeof filled);
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_signal_file_expr_ref(const ecuio_signal_file_t* file,
                                          const ecuio_signal_def_t* def,
                                          char* out, size_t cap)
{
    ecuio_detail_clear();
    if (file == nullptr || def == nullptr || out == nullptr) {
        ecuio_detail_set("the name needs a file, a def and a buffer");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (cap > 0) {
        out[0] = '\0';
    }
    try {
        const std::string own = ecuio_signal_file_key(file, def);
        /* A key that starts with a digit is a number to the grammar: such
         * a signal has obd2's name when it stands in a base def's place */
        const bool digit = !own.empty() && own[0] >= '0' && own[0] <= '9';
        std::string name = digit ? std::string() : own;
        bool the_files = !own.empty();
        if (!the_files) {
            for (const ecuio_signal_def_t& d : file->defs) {
                the_files = the_files || &d == def;
            }
        }
        if (name.empty() && the_files && file->extends == "obd2") {
            const ecuio_signal_set_t* base = ecuio_obd2_signal_set();
            for (uint32_t i = 0; i < base->count && name.empty(); ++i) {
                if (base->defs[i].id == def->id) {
                    const std::string key = ecuio_sf::obd2_key_of(base, i);
                    name = key.empty() ? key : "obd2." + key;
                }
            }
        }
        if (!the_files) {
            ecuio_detail_set("the def isn't one of the file's");
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        if (name.empty() && digit) {
            ecuio_detail_set("%s has no name in an expression: its key %s "
                             "starts with a digit, which an expression reads "
                             "as a number",
                             q(def->name).c_str(), q(own).c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        if (name.empty()) {
            ecuio_detail_set("%s has no name in an expression: obd2's keys "
                             "name its Service 01 fields",
                             q(def->name).c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        if (name.size() + 1 > cap) {
            ecuio_detail_set("the name %s needs a buffer of %u bytes, and "
                             "this one holds %u",
                             q(name).c_str(),
                             static_cast<unsigned>(name.size() + 1),
                             static_cast<unsigned>(cap));
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::memcpy(out, name.c_str(), name.size() + 1);
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
