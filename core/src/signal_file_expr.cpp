// File/layer: core/src/signal_file_expr.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalExprTest (test_signals); FUZZ: fuzz_signal_file
//
// An expression against a loaded signal file (signals.h, ADR-0062): the
// check an editor runs before the file is written, the name an
// expression reads a def by, and a program worked out from given values.
// The compiler and the chain rules are the loader's (signal_expr.h), so a
// check and a load judge alike; the machine and the age rule are the
// frame decoder's (computed.h), so a preview and a session give one value.

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "computed.h"
#include "error_detail.h"
#include "signal_expr.h"
#include "signal_file_build.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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

    ecuio_sf::absent why_absent(const std::string& key) const override
    {
        if (std::find(file->needs.begin(), file->needs.end(), key) !=
            file->needs.end()) {
            return ecuio_sf::absent::needed;
        }
        return std::find(file->waiting.begin(), file->waiting.end(), key) !=
                       file->waiting.end()
                   ? ecuio_sf::absent::waiting
                   : ecuio_sf::absent::unknown;
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
    const ecuio_signal_file* file = nullptr;
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
            ecuio_sf::compile_expr(e.second, scope, ecuio_sf::kGrammarFormat,
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

/* One check: the file's own signals with the checked one among them as
 * the file would hold it, the expression's program, and where each of
 * its inputs is first written. Never copied: the checked signal points
 * at program. */
struct checking {
    const ecuio_signal_file& f;
    const std::string& key;
    file_scope scope;
    /* Each def's place among the file's own, kNone for a base def */
    std::vector<size_t> own_of;
    size_t checked = kNone;
    bool was_computed = false; /* the file holds the checked signal computed */
    ecuio_computed_t program{};
    std::vector<size_t> places;
    std::vector<uint32_t> depth; /* each own signal's, by the chain rules */

    checking(const ecuio_signal_file& file, const std::string& its_key)
        : f(file), key(its_key), own_of(file.defs.size(), kNone)
    {
        scope.base = f.extends == "obd2" ? ecuio_obd2_signal_set() : nullptr;
        scope.file = &f;
        scope.own.reserve(f.keys.size() + f.second_keys.size() + 1);
        for (const auto& k : f.keys) {
            const ecuio_signal_def_t& d = f.defs[k.second];
            own_signal s{&k.first, d.id, expr_kind::number, nullptr, k.second};
            if (is_computed(d)) {
                s.kind = expr_kind::computed;
                s.program = &f.computed[d.table_first];
            } else if (d.kind == ECUIO_VALUE_TEXT ||
                       d.kind == ECUIO_VALUE_HEX) {
                s.kind = expr_kind::text;
            }
            if (!key.empty() && k.first == key) {
                checked = scope.own.size();
            }
            own_of[k.second] = scope.own.size();
            scope.own.push_back(s);
        }
        /* A key a signal replaced reads the signal that stands for it */
        for (const auto& k : f.second_keys) {
            const size_t own = own_of[k.second];
            if (own != kNone) {
                own_signal s = scope.own[own];
                s.key = &k.first;
                scope.own.push_back(s);
            }
        }
        was_computed =
            checked != kNone && scope.own[checked].kind == expr_kind::computed;
        if (checked != kNone) {
            scope.own[checked].kind = expr_kind::computed;
        } else if (!key.empty()) {
            checked = scope.own.size();
            scope.own.push_back(own_signal{&key, next_id(), expr_kind::computed,
                                           nullptr, kNone});
        }
    }
    checking(const checking&) = delete;
    checking& operator=(const checking&) = delete;

    /* A new signal takes the ID the loader gives the next in the list */
    uint16_t next_id() const
    {
        return static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE + f.keys.size());
    }

    /* text compiled against the scope into program, which the checked
     * signal then reads. With no key the expression is a signal of its
     * own, after the file's. */
    void compile(const std::string& text)
    {
        std::vector<std::string> names;
        try {
            program = ecuio_sf::compile_expr(
                text, scope, ecuio_sf::kGrammarFormat, &names, &places);
        } catch (const expr_refused& r) {
            throw refusal{static_cast<uint32_t>(r.at + 1),
                          "the expression " + r.words};
        }
        if (checked == kNone) {
            checked = scope.own.size();
            scope.own.push_back(own_signal{
                nullptr, next_id(), expr_kind::computed, nullptr, kNone});
        }
        scope.own[checked].program = &program;
    }

    /* "signal 'k' replaces 'Name', which <by> reads": a signal named as
     * a base def stands in its place, read as "obd2.<key>" too */
    std::string replaced(const own_signal& s, const std::string& by) const
    {
        return ecuio_sf::replaced_words(*s.key, f.defs[s.def].name, by);
    }

    /* The program's inputs among the file's own: a text signal standing
     * in for a base def refused. True when one is computed. */
    bool reads_a_computed_signal() const
    {
        const own_signal& self = scope.own[checked];
        const std::string reader =
            self.def != kNone ? q(f.defs[self.def].name) : "the expression";
        bool computed = false;
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
            computed = computed || scope.own[n].kind == expr_kind::computed;
        }
        return computed;
    }

    /* Every own signal's depth by the loader's chain rules, a refusal
     * placed at the input it comes through */
    void settle_depths()
    {
        std::vector<chain_node> nodes(scope.own.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            const own_signal& s = scope.own[i];
            nodes[i] = {s.key, s.id,
                        s.kind == expr_kind::computed ? s.program : nullptr};
        }
        try {
            depth = ecuio_sf::chain_depths(nodes);
        } catch (const chain_refused& r) {
            throw refusal{chain_place(r, scope, checked, program, places),
                          r.words};
        }
    }

    /* What reads the checked signal today reads a computed one once the
     * file holds the expression: none may keep an age bound on it, and
     * all of them need format 2. True when a computed def reads it. */
    bool read_by_a_computed_def() const
    {
        const own_signal& self = scope.own[checked];
        bool read = false;
        for (size_t i = 0; i < f.defs.size() && self.def != kNone; ++i) {
            const ecuio_signal_def_t& d = f.defs[i];
            if (!is_computed(d) || i == self.def) {
                continue;
            }
            const ecuio_computed_t& p = f.computed[d.table_first];
            if (!reads(p, self.id)) {
                continue;
            }
            read = true;
            if (was_computed || !reads(p, self.id, true)) {
                continue;
            }
            const bool stands_in = self.id < ECUIO_USER_SIG_ID_BASE;
            if (own_of[i] != kNone &&
                (!stands_in || aged_by_key(f, scope, i, p, self.id, key))) {
                throw refusal{1, ecuio_sf::aged_computed_words(
                                     *scope.own[own_of[i]].key, key)};
            }
            throw refusal{1,
                          replaced(self, q(d.name)) + ecuio_sf::kReplacedAged};
        }
        return read;
    }

    /* A base program over a signal that stands in for its input sits one
     * deeper than it: refused past the bound */
    void base_programs_fit() const
    {
        for (size_t i = 0; i < f.defs.size(); ++i) {
            const ecuio_signal_def_t& d = f.defs[i];
            if (!is_computed(d) || own_of[i] != kNone) {
                continue;
            }
            const ecuio_computed_t& p = f.computed[d.table_first];
            for (uint32_t k = 0;
                 k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS; ++k) {
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
    }

    /* Whether the oldest format refuses the text the newest took */
    bool oldest_format_refuses(const std::string& text) const
    {
        try {
            std::vector<std::string> unused;
            ecuio_sf::compile_expr(text, scope, ECUIO_SIGNAL_FILE_FORMAT_OLDEST,
                                   &unused, nullptr);
        } catch (const expr_refused&) {
            return true;
        }
        return false;
    }
};

/* text as key's "expr" in f: out filled, or a refusal thrown */
void check(const ecuio_signal_file& f, const std::string& key,
           const std::string& text, ecuio_expr_check_t& out)
{
    checking c(f, key);
    c.compile(text);
    bool format2 = c.reads_a_computed_signal();
    c.settle_depths();
    format2 = c.read_by_a_computed_def() || format2;
    c.base_programs_fit();
    format2 = format2 || c.oldest_format_refuses(text);
    out.format =
        format2 ? ecuio_sf::kGrammarFormat : ECUIO_SIGNAL_FILE_FORMAT_OLDEST;
    out.depth = c.depth[c.checked];
    out.program = c.program;
}

/* The def of f with that ID: its index, or kNone */
size_t def_by_id(const ecuio_signal_file& f, uint16_t id)
{
    for (size_t i = 0; i < f.defs.size(); ++i) {
        if (f.defs[i].id == id) {
            return i;
        }
    }
    return kNone;
}

/* How deep the computed def at i sits (1 reading no computed def), kept
 * in depth once known. A loaded set has no circle and nothing past the
 * bound; level stops a walk that met one anyway. */
uint32_t depth_of(const ecuio_signal_file& f, size_t i,
                  std::vector<uint32_t>& depth, uint32_t level)
{
    if (depth[i] != 0) {
        return depth[i];
    }
    if (level > ECUIO_COMPUTED_MAX_DEPTH) {
        return ECUIO_COMPUTED_MAX_DEPTH + 1;
    }
    uint32_t d = 1;
    const ecuio_computed_t& p = f.computed[f.defs[i].table_first];
    for (uint32_t k = 0; k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS;
         ++k) {
        const size_t j = def_by_id(f, p.input_ids[k]);
        if (j != kNone && is_computed(f.defs[j])) {
            d = std::max(d, depth_of(f, j, depth, level + 1) + 1);
        }
    }
    depth[i] = d;
    return d;
}

/* Why program can't be worked out over f's set, "" when it can: what
 * the set's validator would refuse of it were it one of the set's own */
std::string program_fault(const ecuio_signal_file& f,
                          const ecuio_computed_t& program)
{
    if (!ecuio_computed_check(program)) {
        return "the program fails the validator's check of its counts, "
               "steps, constants and age bounds";
    }
    std::vector<uint32_t> depth(f.defs.size(), 0);
    uint32_t deep = 1;
    size_t deepest = kNone; /* the input that sets deep */
    for (uint32_t k = 0; k < program.input_count; ++k) {
        const size_t j = def_by_id(f, program.input_ids[k]);
        if (j == kNone) {
            return "the program reads ID " +
                   std::to_string(program.input_ids[k]) +
                   ", which no def of the file's set has";
        }
        const ecuio_signal_def_t& d = f.defs[j];
        if (is_computed(d)) {
            if (program.max_age_us[k] != 0) {
                return "the program keeps an age bound on " + q(d.name) +
                       ", a computed signal: its own inputs carry their ages";
            }
            const uint32_t at = depth_of(f, j, depth, 1) + 1;
            if (at > deep) {
                deep = at;
                deepest = j;
            }
        } else if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            return "the program reads " + q(d.name) + ", which is text";
        }
    }
    if (deep > ECUIO_COMPUTED_MAX_DEPTH) {
        /* The depth comes through a base program (no key of the file's):
         * name it */
        if (deepest != kNone &&
            *ecuio_signal_file_key(&f, &f.defs[deepest]) == '\0') {
            return ecuio_sf::reads_deep_base_words(f.defs[deepest].name, deep);
        }
        return "the program sits " + std::to_string(deep) +
               " computed signals deep: a chain goes " +
               std::to_string(ECUIO_COMPUTED_MAX_DEPTH) + " deep at most";
    }
    return std::string();
}

/* The number defs under program, each once, in the order it and the
 * programs of its computed inputs first read them. seen is a byte per
 * def. A program program_fault passed goes no deeper than the bound. */
void leaves_under(const ecuio_signal_file& f, const ecuio_computed_t& program,
                  std::vector<uint8_t>& seen, std::vector<uint16_t>& out)
{
    for (uint32_t k = 0; k < program.input_count; ++k) {
        const size_t j = def_by_id(f, program.input_ids[k]);
        if (j == kNone || seen[j] != 0) {
            continue;
        }
        seen[j] = 1;
        const ecuio_signal_def_t& d = f.defs[j];
        if (is_computed(d)) {
            leaves_under(f, f.computed[d.table_first], seen, out);
        } else {
            out.push_back(d.id);
        }
    }
}

/* One evaluation: each computed def under the program worked out once,
 * as the decoder works a chain out at a frame, and the first leaf read
 * without a value kept for the answer */
struct evaluation {
    const ecuio_signal_file& f;
    const ecuio_eval_input_t* inputs;
    uint32_t count;
    std::vector<uint8_t> done; /* a computed def is worked out */
    std::vector<double> value; /* its value, NaN for none */
    uint32_t why = ECUIO_EVAL_WHY_NONE;
    uint16_t leaf = 0;
    uint64_t bound_us = 0;

    void lacks(uint32_t reason, uint16_t id, uint64_t bound)
    {
        if (why == ECUIO_EVAL_WHY_NONE) {
            why = reason;
            leaf = id;
            bound_us = bound;
        }
    }

    /* A number def's value as the program reading it within max_age_us
     * sees it: the first given for it, no older than the bound */
    double leaf_value(uint16_t id, uint32_t max_age_us)
    {
        const uint64_t bound = ecuio_computed_bound_us(max_age_us);
        for (uint32_t i = 0; i < count; ++i) {
            if (inputs[i].def_id != id) {
                continue;
            }
            if (std::isnan(inputs[i].value)) {
                break;
            }
            if (!ecuio_computed_fresh(inputs[i].age_us, bound)) {
                lacks(ECUIO_EVAL_WHY_STALE, id, bound);
                return std::numeric_limits<double>::quiet_NaN();
            }
            return inputs[i].value;
        }
        lacks(ECUIO_EVAL_WHY_MISSING, id, 0);
        return std::numeric_limits<double>::quiet_NaN();
    }

    double run(const ecuio_computed_t& program)
    {
        double in[ECUIO_COMPUTED_MAX_INPUTS] = {};
        for (uint32_t k = 0; k < program.input_count; ++k) {
            const size_t j = def_by_id(f, program.input_ids[k]);
            const ecuio_signal_def_t& d = f.defs[j];
            if (!is_computed(d)) {
                in[k] = leaf_value(d.id, program.max_age_us[k]);
                continue;
            }
            if (done[j] == 0) {
                const double under = run(f.computed[d.table_first]);
                value[j] = under;
                done[j] = 1;
            }
            in[k] = value[j];
        }
        double out = 0.0;
        return ecuio_computed_run(program, in, &out)
                   ? out
                   : std::numeric_limits<double>::quiet_NaN();
    }
};

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

ecuio_status_t ecuio_signal_file_expr_leaves(const ecuio_signal_file_t* file,
                                             const ecuio_computed_t* program,
                                             uint16_t* ids, uint32_t capacity,
                                             uint32_t* count)
{
    ecuio_detail_clear();
    if (file == nullptr || program == nullptr || count == nullptr ||
        (ids == nullptr && capacity != 0)) {
        ecuio_detail_set("the leaves need a file, a program, a count and "
                         "room for what the capacity says");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        const std::string fault = program_fault(*file, *program);
        if (!fault.empty()) {
            ecuio_detail_set("%s", fault.c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::vector<uint8_t> seen(file->defs.size(), 0);
        std::vector<uint16_t> leaves;
        leaves_under(*file, *program, seen, leaves);
        for (size_t i = 0; i < leaves.size() && i < capacity; ++i) {
            ids[i] = leaves[i];
        }
        *count = static_cast<uint32_t>(leaves.size());
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_signal_file_evaluate(const ecuio_signal_file_t* file,
                                          const ecuio_computed_t* program,
                                          const ecuio_eval_input_t* inputs,
                                          uint32_t count,
                                          ecuio_eval_result_t* out)
{
    ecuio_detail_clear();
    if (file == nullptr || program == nullptr || out == nullptr ||
        (inputs == nullptr && count != 0)) {
        ecuio_detail_set("the evaluation needs a file, a program, the inputs "
                         "its count says and a struct to fill");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof(ecuio_eval_result_t)) {
        ecuio_detail_set("the result's struct_size is %u, under its %u bytes",
                         static_cast<unsigned>(out->struct_size),
                         static_cast<unsigned>(sizeof(ecuio_eval_result_t)));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (inputs[i].flags != 0 || inputs[i].reserved0 != 0) {
            ecuio_detail_set("the evaluation's input at index %u has a nonzero "
                             "flags or reserved0: no flag is defined",
                             static_cast<unsigned>(i));
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    try {
        const std::string fault = program_fault(*file, *program);
        if (!fault.empty()) {
            ecuio_detail_set("%s", fault.c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        evaluation e{*file, inputs, count,
                     std::vector<uint8_t>(file->defs.size(), 0),
                     std::vector<double>(file->defs.size(), 0.0)};
        const double value = e.run(*program);
        ecuio_eval_result_t filled{};
        filled.struct_size = sizeof(ecuio_eval_result_t);
        if (!std::isnan(value)) {
            filled.flags = ECUIO_EVAL_VALUE;
            filled.value = value;
        } else if (e.why != ECUIO_EVAL_WHY_NONE) {
            filled.why = e.why;
            filled.leaf = e.leaf;
            filled.bound_us = e.bound_us;
        } else {
            filled.why = ECUIO_EVAL_WHY_NO_NUMBER;
        }
        std::memcpy(out, &filled, sizeof filled);
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
