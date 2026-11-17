// File/layer: cli/expr_lines.h | havne-cli, signals expr's arguments and lines (ADR-0062)
// Testing: UNIT: CliExprTest (test_cli)
//
// What `havne-cli signals expr` needs besides core's calls (signals.h's
// ecuio_signal_file_check_expr, _expr_ref, _expr_leaves and _evaluate): its
// arguments, the def a typed name reads, and the lines that say what core
// found. Public headers only.

#ifndef HAVNE_CLI_EXPR_LINES_H
#define HAVNE_CLI_EXPR_LINES_H

#include "jsonl.h"
#include "signal_file_lines.h" /* kind_word, own_signal */
#include "view_lines.h"        /* parse_decimal, parse_seconds */

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cli {

// One --value: a signal by the name an expression reads it by, its value
// in the signal's own unit and how long ago it was read
struct ExprValue {
    std::string ref;
    double value = 0;
    uint64_t age_us = 0;
    uint16_t def_id = 0; // resolve_expr_values' answer
};

// signals expr (--signals <file> | --plugin <pkg> [--variant <id>])
//              [--key <key>] [<expr> [--value <name>=<n>[@<age s>]]...]
struct ExprArgs {
    const char* signals = nullptr;
    const char* plugin = nullptr;
    const char* variant = nullptr;
    const char* key = nullptr;  // the signal the expression is for
    const char* expr = nullptr; // none: the names an expression can read
    std::vector<ExprValue> values;
};

// A signal file's key: 1 to 31 of a-z, 0-9 and _
inline bool expr_key_ok(const char* key)
{
    const size_t n = std::strlen(key);
    if (n < 1 || n > 31) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

// <name>=<number>[@<age in seconds>]: the name up to the first '=', the
// age after the last '@' past it. A number that isn't finite, an age
// below 0 or past a day, or a part left empty is none
inline bool parse_expr_value(const char* text, ExprValue* out)
{
    const char* eq = std::strchr(text, '=');
    if (eq == nullptr || eq == text) {
        return false;
    }
    std::string number(eq + 1);
    int64_t age = 0;
    const size_t at = number.rfind('@');
    if (at != std::string::npos) {
        if (!parse_seconds(number.c_str() + at + 1, age) || age < 0 ||
            age > 86400000000ll) {
            return false;
        }
        number.erase(at);
    }
    double v = 0;
    if (!parse_decimal(number.c_str(), v)) {
        return false;
    }
    out->ref.assign(text, static_cast<size_t>(eq - text));
    out->value = v;
    out->age_us = static_cast<uint64_t>(age);
    out->def_id = 0;
    return true;
}

// The arguments after "signals expr". False with why set for a usage
// error: no file or two, an option without its value or given twice, two
// expressions, --variant with no --plugin, --key or --value with no
// expression, a key or a value not in its form, a name given two values,
// anything else starting "--"
inline bool parse_signals_expr(int argc, const char* const* argv, ExprArgs* out,
                               std::string* why)
{
    static const struct {
        const char* option;
        const char* ExprArgs::* slot;
        const char* takes;
    } kOptions[] = {
        {"--signals", &ExprArgs::signals, "signal file"},
        {"--plugin", &ExprArgs::plugin, "package"},
        {"--variant", &ExprArgs::variant, "variant's id"},
        {"--key", &ExprArgs::key, "signal's key"},
    };
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        const char* ExprArgs::* slot = nullptr;
        const char* takes = "";
        for (const auto& o : kOptions) {
            if (a == o.option) {
                slot = o.slot;
                takes = o.takes;
            }
        }
        if (slot != nullptr) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' ||
                out->*slot != nullptr) {
                *why = a + " takes one " + takes;
                return false;
            }
            out->*slot = argv[++i];
        } else if (a == "--value") {
            ExprValue v;
            if (i + 1 >= argc || !parse_expr_value(argv[i + 1], &v)) {
                *why = "--value takes <name>=<number>[@<age in seconds>]";
                return false;
            }
            for (const ExprValue& given : out->values) {
                if (given.ref == v.ref) {
                    *why = "--value gives '" + v.ref + "' two values";
                    return false;
                }
            }
            out->values.push_back(v);
            ++i;
        } else if (a.compare(0, 2, "--") == 0) {
            *why = "unexpected '" + a +
                   "': an expression that starts with -- goes in parentheses";
            return false;
        } else if (out->expr != nullptr) {
            *why = "signals expr takes one expression: quote it whole";
            return false;
        } else {
            out->expr = argv[i];
        }
    }
    if ((out->signals != nullptr) == (out->plugin != nullptr)) {
        *why = "signals expr takes --signals <file> or --plugin <pkg>";
        return false;
    }
    if (out->variant != nullptr && out->plugin == nullptr) {
        *why = "--variant names a --plugin's variant";
        return false;
    }
    if (out->key != nullptr && !expr_key_ok(out->key)) {
        *why = "--key takes a signal's key: 1 to 31 of a-z, 0-9 and _";
        return false;
    }
    if (out->expr == nullptr && out->key != nullptr) {
        *why = "--key goes with an expression: the signal it is for";
        return false;
    }
    if (out->expr == nullptr && !out->values.empty()) {
        *why = "--value goes with an expression";
        return false;
    }
    return true;
}

// The set's def with this ID, or nullptr
inline const ecuio_signal_def_t* expr_def(const ecuio_signal_set_t& set,
                                          uint16_t id)
{
    for (uint32_t i = 0; i < set.count; ++i) {
        if (set.defs[i].id == id) {
            return &set.defs[i];
        }
    }
    return nullptr;
}

// The name the file's expressions read def by; "" for a def with none
inline std::string expr_ref(const ecuio_signal_file_t* file,
                            const ecuio_signal_def_t& d)
{
    char name[ECUIO_EXPR_REF_LEN] = "";
    if (ecuio_signal_file_expr_ref(file, &d, name, sizeof name) != ECUIO_OK) {
        return std::string();
    }
    return name;
}

// Whether an expression of the file can read def: a number, an
// enumeration or a computed signal that has a name there
inline bool expr_reads(const ecuio_signal_file_t* file,
                       const ecuio_signal_def_t& d)
{
    const bool computed = (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0;
    if (!computed &&
        (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX)) {
        return false;
    }
    return !expr_ref(file, d).empty();
}

// Each --value's def: its name compiled alone, as the expression would
// read it, so every spelling core takes names its def. False with why
// for a name no signal is read by, one that is more than a name, a
// computed signal's (its value comes from the signals under it) and a
// signal given two values under two names
inline bool resolve_expr_values(const ecuio_signal_file_t* file,
                                std::vector<ExprValue>* values,
                                std::string* why)
{
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    for (size_t i = 0; i < values->size(); ++i) {
        ExprValue& v = (*values)[i];
        ecuio_expr_check_t c;
        std::memset(&c, 0, sizeof c);
        c.struct_size = sizeof c;
        if (ecuio_signal_file_check_expr(file, nullptr, v.ref.data(),
                                         v.ref.size(), &c) != ECUIO_OK) {
            *why = "--value " + v.ref + ": " + ecuio_last_error_detail();
            return false;
        }
        const ecuio_signal_def_t* d = expr_def(set, c.program.input_ids[0]);
        if (c.program.input_count != 1 || c.program.step_count != 1 ||
            c.program.steps[0] != ECUIO_COMPUTED_PUSH_INPUT || d == nullptr) {
            *why =
                "--value " + v.ref + ": a value goes to a signal by its name";
            return false;
        }
        if ((d->flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            *why = "--value " + v.ref +
                   ": the signal is computed: give the signals it reads "
                   "their values";
            return false;
        }
        v.def_id = d->id;
        for (size_t k = 0; k < i; ++k) {
            if ((*values)[k].def_id == v.def_id) {
                *why = "--value gives one signal two values, as '" +
                       (*values)[k].ref + "' and '" + v.ref + "'";
                return false;
            }
        }
    }
    return true;
}

// A def's object: its fields, a name and a unit each at most six bytes
// for each byte, escaped
inline constexpr size_t kExprDefBytes =
    128 + 6 * (ECUIO_EXPR_REF_LEN + sizeof(ecuio_signal_def_t::name) +
               sizeof(ecuio_signal_def_t::unit));

// "ref", "id", "name", "unit", "kind" of a def an expression reads; ref
// null for a def with no name there
inline void expr_def_fields(Line& l, const ecuio_signal_file_t* file,
                            const ecuio_signal_def_t& d)
{
    const std::string ref = expr_ref(file, d);
    const char* kind = kind_word(d);
    l.key("ref");
    if (ref.empty()) {
        l.raw("null");
    } else {
        l.string(ref.data(), ref.size());
    }
    l.key("id");
    l.u64(d.id);
    l.key("name");
    l.string(d.name, sizeof d.name);
    l.key("unit");
    l.string(d.unit, sizeof d.unit);
    l.key("kind");
    l.string(kind, std::strlen(kind));
}

// {"type":"input", ref, id, name, unit, kind, own}: a def an expression
// of the file can read; own false for its base set's
inline void input_line(Line& l, const ecuio_signal_file_t* file,
                       const ecuio_signal_def_t& d)
{
    l.raw("{\"type\":\"input\"");
    expr_def_fields(l, file, d);
    l.key("own");
    l.raw(own_signal(file, d) ? "true}" : "false}");
}

// The bytes an expr line takes at most: the expression and core's words
// as typed, and a def's object for each input and leaf
inline size_t expr_line_bytes(const char* expr, const char* key,
                              const char* detail, uint32_t leaves)
{
    return 512 + Line::string_bytes(expr) + Line::string_bytes(key) +
           Line::string_bytes(detail) +
           (size_t{ECUIO_COMPUTED_MAX_INPUTS} + leaves) * kExprDefBytes;
}

// {"type":"expr", expr, key|null, ok, at|null (the character it is
// refused at, from 1), detail|null (core's words), format|null (the
// oldest "havne_signals" that takes it), file_format, depth|null, inputs
// [{ref, id, name, unit, kind}] (what the expression reads, in its
// order), leaves [the same] (the numbers under it through any computed
// signal: what --value gives a value)}. refused: core's detail, or
// nullptr for an expression that compiles
inline void expr_line(Line& l, const ecuio_signal_file_t* file,
                      const char* expr, const char* key,
                      const ecuio_expr_check_t& c, const char* refused,
                      const uint16_t* leaves, uint32_t leaf_count)
{
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    const bool ok = refused == nullptr;
    l.raw("{\"type\":\"expr\"");
    l.key("expr");
    l.string(expr, std::strlen(expr));
    l.key("key");
    if (key != nullptr) {
        l.string(key, std::strlen(key));
    } else {
        l.raw("null");
    }
    l.key("ok");
    l.raw(ok ? "true" : "false");
    l.key("at");
    if (ok) {
        l.raw("null");
    } else {
        l.u64(c.at);
    }
    l.key("detail");
    if (ok) {
        l.raw("null");
    } else {
        l.string(refused, std::strlen(refused));
    }
    l.key("format");
    if (ok) {
        l.u64(c.format);
    } else {
        l.raw("null");
    }
    l.key("file_format");
    l.u64(ecuio_signal_file_format(file));
    l.key("depth");
    if (ok) {
        l.u64(c.depth);
    } else {
        l.raw("null");
    }
    l.key("inputs");
    l.ch('[');
    for (uint32_t i = 0;
         ok && i < c.program.input_count && i < ECUIO_COMPUTED_MAX_INPUTS;
         ++i) {
        if (const ecuio_signal_def_t* d =
                expr_def(set, c.program.input_ids[i])) {
            l.raw(i > 0 ? ",{" : "{");
            expr_def_fields(l, file, *d);
            l.ch('}');
        }
    }
    l.ch(']');
    l.key("leaves");
    l.ch('[');
    for (uint32_t i = 0; ok && i < leaf_count; ++i) {
        if (const ecuio_signal_def_t* d = expr_def(set, leaves[i])) {
            l.raw(i > 0 ? ",{" : "{");
            expr_def_fields(l, file, *d);
            l.ch('}');
        }
    }
    l.raw("]}");
}

// Why an evaluation has no value, as a word; "unknown" for one this CLI
// doesn't know
inline const char* eval_why_word(uint32_t why)
{
    switch (why) {
    case ECUIO_EVAL_WHY_MISSING:
        return "missing";
    case ECUIO_EVAL_WHY_STALE:
        return "stale";
    case ECUIO_EVAL_WHY_NO_NUMBER:
        return "no_number";
    default:
        return "unknown";
    }
}

inline constexpr size_t kValueLineBytes = 256 + 6 * ECUIO_EXPR_REF_LEN;

// {"type":"value", value|null, why null|missing|stale|no_number, leaf
// null|its name (the first signal read without a value), bound_us null|
// the age a stale one is past}
inline void value_line(Line& l, const ecuio_signal_file_t* file,
                       const ecuio_eval_result_t& r)
{
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    const bool has = (r.flags & ECUIO_EVAL_VALUE) != 0;
    l.raw("{\"type\":\"value\"");
    l.key("value");
    if (has) {
        l.number(r.value);
    } else {
        l.raw("null");
    }
    l.key("why");
    if (has) {
        l.raw("null");
    } else {
        const char* why = eval_why_word(r.why);
        l.string(why, std::strlen(why));
    }
    const bool named = !has && (r.why == ECUIO_EVAL_WHY_MISSING ||
                                r.why == ECUIO_EVAL_WHY_STALE);
    const ecuio_signal_def_t* d = named ? expr_def(set, r.leaf) : nullptr;
    const std::string ref = d != nullptr ? expr_ref(file, *d) : std::string();
    l.key("leaf");
    if (ref.empty()) {
        l.raw("null");
    } else {
        l.string(ref.data(), ref.size());
    }
    l.key("bound_us");
    if (!has && r.why == ECUIO_EVAL_WHY_STALE) {
        l.u64(r.bound_us);
    } else {
        l.raw("null");
    }
    l.ch('}');
}

// A refused expression and a caret under the character it is refused at,
// two indented lines; "" when a caret can't sit under it: a place past
// the end's next column, or anything but printable ASCII before it
inline std::string expr_caret(const char* expr, uint32_t at)
{
    const size_t n = std::strlen(expr);
    if (at < 1 || at > n + 1) {
        return std::string();
    }
    for (size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(expr[i]);
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && i + 1 < at)) {
            return std::string();
        }
    }
    return "  " + std::string(expr) + "\n  " + std::string(at - 1, ' ') + "^\n";
}

} // namespace cli

#endif // HAVNE_CLI_EXPR_LINES_H
