// File/layer: core/src/signal_expr.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, SignalExprTest, PluginTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// A computed signal's expression (signal_expr.h): the compiler, obd2's
// keys as an expression names them, and the chain rules.

#include "signal_expr.h"

#include "ecuio/signals.h"
#include "json.h"
#include "signal_file_build.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_sf {

namespace {

/* The base set's signals a computed signal can read, by the stable key
 * obd2 publishes: "pid_0b" for a PID's first field, "pid_14_1" for its
 * second, and a name for the common ones */
struct alias {
    const char* key;
    uint8_t pid;
};
const alias kObd2Aliases[] = {
    {"load", 0x04},     {"coolant", 0x05}, {"map", 0x0B},
    {"rpm", 0x0C},      {"speed", 0x0D},   {"iat", 0x0F},
    {"maf", 0x10},      {"throttle", 0x11}, {"fuel_level", 0x2F},
    {"baro", 0x33},     {"module_voltage", 0x42}, {"ambient", 0x46},
    {"oil_temp", 0x5C},
};

/* The expression compiler (ADR-0051 section 2, ADR-0062): + - * /,
 * parentheses, unary minus, decimal constants and signals by key, into a
 * program's steps; format 2 adds function calls, comparisons and if(),
 * each part a number or a truth, checked as it compiles. A format 1 file
 * meets format 2's forms refused by name. At most 4 inputs, 16
 * constants, 33 steps. */
class compiler {
public:
    compiler(const std::string& text, const expr_scope& scope, uint32_t format)
        : s_(text), scope_(scope), base_(scope.base), format_(format)
    {
    }

    ecuio_computed_t compile(std::vector<std::string>* names,
                             std::vector<size_t>* places)
    {
        p_ = {};
        if (s_.size() > ECUIO_EXPR_MAX_BYTES) {
            fail(0, too_long_words());
        }
        const part whole = comparison(0);
        skip();
        if (pos_ < s_.size()) {
            if (s_[pos_] == ',' && format_ >= 2) {
                fail(pos_, "has ',' outside a call");
            }
            fail(pos_, "has " + q(glyph()) +
                           " where an operator or the end should be");
        }
        if (whole.truth) {
            const std::string shown = s_.size() <= 32 ? s_ : std::string("...");
            fail(whole.at, "is a comparison where a number should be: if(" +
                               shown + ", 1, 0) makes one");
        }
        if (p_.input_count == 0) {
            fail(0, "reads no signal: a constant is no signal of its own");
        }
        p_.step_count = static_cast<uint8_t>(steps_);
        *names = names_;
        if (places != nullptr) {
            *places = places_;
        }
        return p_;
    }

private:
    /* What a part of the expression compiled to: a truth or a number,
     * where it starts, and its value when it is constants alone */
    struct part {
        bool truth = false;
        size_t at = 0;
        bool constant = false;
        double value = 0.0;
    };

    [[noreturn]] void fail(size_t at, const std::string& words)
    {
        throw expr_refused{at, words};
    }

    /* the whole UTF-8 character at pos_; an invalid byte alone */
    std::string glyph() const
    {
        const auto* p = reinterpret_cast<const unsigned char*>(s_.data());
        const size_t n = ecuio_json::utf8_length(p + pos_, p + s_.size());
        return s_.substr(pos_, n != 0 ? n : 1);
    }

    void skip()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }

    void step(uint8_t s)
    {
        if (steps_ >= ECUIO_COMPUTED_MAX_STEPS) {
            /* Format 1's steps are values and two-value operations, so 33
             * steps are 16 operations there */
            fail(pos_, format_ >= 2 ? "is longer than a program's 33 steps"
                                    : "has more than 16 operations");
        }
        p_.steps[steps_++] = s;
    }

    /* A part that must be a number: what verb says what took it */
    void number(const part& x, const std::string& verb)
    {
        if (x.truth) {
            fail(x.at, verb + " a comparison where a number should be");
        }
    }

    void operation(uint8_t s)
    {
        step(s);
    }

    void constant(double c)
    {
        for (uint32_t k = 0; k < p_.constant_count; ++k) {
            if (p_.constants[k] == c && std::signbit(p_.constants[k]) == std::signbit(c)) {
                step(static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_CONSTANT | k));
                return;
            }
        }
        if (p_.constant_count >= ECUIO_COMPUTED_MAX_CONSTANTS) {
            fail(pos_, "has more than 16 constants");
        }
        p_.constants[p_.constant_count] = c;
        step(static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_CONSTANT | p_.constant_count));
        ++p_.constant_count;
    }

    void input(size_t at, const std::string& name)
    {
        uint16_t id = 0;
        if (name.compare(0, 5, "obd2.") == 0) {
            if (base_ == nullptr) {
                fail(at, "reads " + q(name) + ", which needs \"extends\": \"obd2\"");
            }
            const long d = obd2_def(base_, name.substr(5));
            if (d < 0) {
                fail(at, "reads " + q(name) + ", which isn't one of obd2's keys");
            }
            id = base_->defs[d].id;
        } else {
            expr_kind kind = expr_kind::number;
            if (!scope_.find(name, &id, &kind)) {
                fail(at, "reads " + q(name) + ", which no signal is");
            }
            if (kind == expr_kind::computed && format_ < 2) {
                fail(at, "reads " + q(name) +
                             ", a computed signal, which needs "
                             "\"havne_signals\": 2");
            }
            if (kind == expr_kind::text) {
                fail(at, "reads " + q(name) + ", which is text");
            }
        }
        for (uint32_t k = 0; k < p_.input_count; ++k) {
            if (p_.input_ids[k] == id) {
                step(static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT | k));
                return;
            }
        }
        if (p_.input_count >= ECUIO_COMPUTED_MAX_INPUTS) {
            fail(at, "reads more than 4 signals");
        }
        p_.input_ids[p_.input_count] = id;
        names_.push_back(name);
        places_.push_back(at);
        step(static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT | p_.input_count));
        ++p_.input_count;
    }

    /* The comparison at pos_, if any: its step and length, or 0 */
    size_t comparison_at(uint8_t* op) const
    {
        const char c = pos_ < s_.size() ? s_[pos_] : '\0';
        const char d = pos_ + 1 < s_.size() ? s_[pos_ + 1] : '\0';
        if (c == '<' || c == '>') {
            const bool less = c == '<';
            *op = static_cast<uint8_t>(
                d == '='
                    ? (less ? ECUIO_COMPUTED_LESS_EQUAL
                            : ECUIO_COMPUTED_GREATER_EQUAL)
                    : (less ? ECUIO_COMPUTED_LESS : ECUIO_COMPUTED_GREATER));
            return d == '=' ? 2 : 1;
        }
        if ((c == '=' || c == '!') && d == '=') {
            *op = static_cast<uint8_t>(c == '=' ? ECUIO_COMPUTED_EQUAL
                                                : ECUIO_COMPUTED_NOT_EQUAL);
            return 2;
        }
        return 0;
    }

    /* A sum, then at most one comparison with another: a comparison binds
     * looser than + and - and takes no neighbor */
    part comparison(unsigned depth)
    {
        const part a = sum(depth);
        skip();
        uint8_t op = 0;
        const size_t length = comparison_at(&op);
        if (length == 0) {
            if (format_ >= 2 && pos_ < s_.size() && s_[pos_] == '=') {
                fail(pos_, "has '=' where '==' should be");
            }
            return a;
        }
        const std::string written = s_.substr(pos_, length);
        if (format_ < 2) {
            fail(pos_,
                 "uses " + q(written) + ", which needs \"havne_signals\": 2");
        }
        number(a, "compares");
        pos_ += length;
        number(sum(depth), "compares");
        step(op);
        skip();
        uint8_t next = 0;
        const size_t neighbor = comparison_at(&next);
        if (neighbor != 0 || (pos_ < s_.size() && s_[pos_] == '=')) {
            fail(pos_, "has " +
                           q(s_.substr(pos_, neighbor != 0 ? neighbor : 1)) +
                           " beside a comparison: all() or any() joins two");
        }
        part r;
        r.truth = true;
        r.at = a.at;
        return r;
    }

    /* x op y over two constants, as the machine works it out */
    static part folded(const part& x, const part& y, char op)
    {
        part r;
        r.at = x.at;
        if (x.constant && y.constant) {
            const double v = op == '+'   ? x.value + y.value
                             : op == '-' ? x.value - y.value
                             : op == '*' ? x.value * y.value
                                         : x.value / y.value;
            r.constant = std::isfinite(v);
            r.value = v;
        }
        return r;
    }

    part sum(unsigned depth)
    {
        part a = term(depth);
        for (;;) {
            skip();
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                const bool add = s_[pos_++] == '+';
                const std::string verb = add ? "adds" : "subtracts";
                number(a, verb);
                const part b = term(depth);
                number(b, verb);
                operation(add ? ECUIO_COMPUTED_ADD : ECUIO_COMPUTED_SUBTRACT);
                a = folded(a, b, add ? '+' : '-');
            } else {
                return a;
            }
        }
    }

    part term(unsigned depth)
    {
        part a = factor(depth);
        for (;;) {
            skip();
            if (pos_ < s_.size() && (s_[pos_] == '*' || s_[pos_] == '/')) {
                const bool mul = s_[pos_++] == '*';
                const std::string verb = mul ? "multiplies" : "divides";
                number(a, verb);
                const part b = factor(depth);
                number(b, verb);
                operation(mul ? ECUIO_COMPUTED_MULTIPLY : ECUIO_COMPUTED_DIVIDE);
                a = folded(a, b, mul ? '*' : '/');
            } else {
                return a;
            }
        }
    }

    /* A function: what it takes, what it gives, its steps */
    struct function {
        const char* name;
        uint8_t takes; /* the fewest parts */
        bool more;     /* and any more past them (min, max, all, any) */
        bool truths;   /* its parts are truths (if's first alone) */
        bool gives_truth;
    };

    static const function* function_named(const std::string& name)
    {
        static const function kFunctions[] = {
            {"min", 2, true, false, false},
            {"max", 2, true, false, false},
            {"abs", 1, false, false, false},
            {"clamp", 3, false, false, false},
            {"sqrt", 1, false, false, false},
            {"round", 1, false, false, false},
            {"floor", 1, false, false, false},
            {"ceil", 1, false, false, false},
            {"if", 3, false, true, false},
            {"all", 2, true, true, true},
            {"any", 2, true, true, true},
            {"not", 1, false, true, true},
        };
        for (const function& f : kFunctions) {
            if (name == f.name) {
                return &f;
            }
        }
        return nullptr;
    }

    /* name(...) at start, pos_ on its '(' */
    part call(size_t start, const std::string& name, unsigned depth)
    {
        const function* f = function_named(name);
        if (f == nullptr && format_ < 2) {
            fail(start, "calls " + name +
                            "(), which isn't a function, and a format 1 file "
                            "calls none (functions need \"havne_signals\": 2)");
        }
        if (f == nullptr) {
            fail(start, "calls " + name +
                            "(), which isn't a function: min, max, abs, clamp, "
                            "sqrt, round, floor, ceil, if, all, any or not");
        }
        if (format_ < 2) {
            fail(start,
                 "uses " + name + "(), which needs \"havne_signals\": 2");
        }
        const bool parts_word = f->truths;
        ++pos_;
        std::vector<part> args;
        skip();
        if (pos_ < s_.size() && s_[pos_] == ')') {
            ++pos_;
        } else {
            for (;;) {
                const part x = comparison(depth + 1);
                const size_t k = args.size();
                const bool want_truth =
                    f->truths && !(std::strcmp(f->name, "if") == 0 && k > 0);
                if (x.truth != want_truth) {
                    fail(x.at, "gives " + name + "() a " +
                                   (x.truth ? "comparison" : "number") +
                                   " where a " +
                                   (want_truth ? "comparison" : "number") +
                                   " should be");
                }
                args.push_back(x);
                emit(*f, args);
                skip();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ')') {
                    ++pos_;
                    break;
                }
                if (pos_ >= s_.size()) {
                    fail(pos_, "has no ')' to close its " + name + "(");
                }
                fail(pos_, "has " + q(glyph()) +
                               " where ',' or ')' should be");
            }
        }
        const size_t n = args.size();
        if (n < f->takes || (!f->more && n > f->takes)) {
            const char* unit = parts_word ? " part" : " value";
            fail(start, "gives " + name + "() " + std::to_string(n) + unit +
                            (n == 1 ? "" : "s") + " where it takes " +
                            std::to_string(f->takes) +
                            (f->more ? " or more" : ""));
        }
        if (std::strcmp(f->name, "clamp") == 0 && args[1].constant &&
            args[2].constant && args[1].value > args[2].value) {
            using ecuio_json::number_text;
            fail(args[1].at,
                 "gives clamp() a low of " + number_text(args[1].value) +
                     " above its high of " + number_text(args[2].value));
        }
        part r;
        r.truth = f->gives_truth;
        r.at = start;
        return r;
    }

    /* A call's steps as its parts arrive: a run of min, max, all or any
     * after each part past the first; clamp is max, then min; the rest
     * after their last part */
    void emit(const function& f, const std::vector<part>& args)
    {
        const size_t n = args.size();
        const std::string name = f.name;
        if (f.more) {
            if (n >= 2) {
                operation(
                    static_cast<uint8_t>(name == "min"   ? ECUIO_COMPUTED_MIN
                                         : name == "max" ? ECUIO_COMPUTED_MAX
                                         : name == "all" ? ECUIO_COMPUTED_ALL
                                                         : ECUIO_COMPUTED_ANY));
            }
            return;
        }
        if (name == "clamp") {
            if (n == 2) {
                operation(ECUIO_COMPUTED_MAX);
            } else if (n == 3) {
                operation(ECUIO_COMPUTED_MIN);
            }
            return;
        }
        if (n != f.takes) {
            return;
        }
        operation(static_cast<uint8_t>(name == "abs"     ? ECUIO_COMPUTED_ABS
                                       : name == "sqrt"  ? ECUIO_COMPUTED_SQRT
                                       : name == "round" ? ECUIO_COMPUTED_ROUND
                                       : name == "floor" ? ECUIO_COMPUTED_FLOOR
                                       : name == "ceil"  ? ECUIO_COMPUTED_CEIL
                                       : name == "not"   ? ECUIO_COMPUTED_NOT
                                                         : ECUIO_COMPUTED_IF));
    }

    part factor(unsigned depth)
    {
        if (depth > 16) {
            fail(pos_, "nests deeper than 16");
        }
        skip();
        if (pos_ >= s_.size()) {
            fail(pos_, "ends where a value should be");
        }
        const char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            const part inner = comparison(depth + 1);
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',' && format_ >= 2) {
                fail(pos_, "has ',' outside a call");
            }
            if (pos_ >= s_.size() || s_[pos_] != ')') {
                fail(pos_, "has no ')' to close its '('");
            }
            ++pos_;
            return inner;
        }
        if (c == '-') {
            const size_t start = pos_;
            ++pos_;
            constant(0.0);
            const part x = factor(depth + 1);
            number(x, "negates");
            operation(ECUIO_COMPUTED_SUBTRACT);
            part zero;
            zero.constant = true;
            part r = folded(zero, x, '-');
            r.at = start;
            return r;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            const size_t start = pos_;
            bool point = false;
            bool digits = false;
            while (pos_ < s_.size() &&
                   ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.')) {
                if (s_[pos_] == '.') {
                    if (point) {
                        fail(pos_, "has a number with two points");
                    }
                    point = true;
                } else {
                    digits = true;
                }
                ++pos_;
            }
            if (!digits) {
                fail(start, "has a point with no digits");
            }
            /* Decimal digits by hand, never strtod, whose decimal point is
             * the locale's */
            double whole = 0.0;
            double frac = 0.0;
            double scale = 1.0;
            bool after = false;
            for (size_t i = start; i < pos_; ++i) {
                if (s_[i] == '.') {
                    after = true;
                } else if (after) {
                    scale /= 10.0;
                    frac += (s_[i] - '0') * scale;
                } else {
                    whole = whole * 10.0 + (s_[i] - '0');
                }
            }
            const double value = whole + frac;
            if (!std::isfinite(value)) {
                fail(start, "has a number too large");
            }
            constant(value);
            part r;
            r.at = start;
            r.constant = true;
            r.value = value;
            return r;
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            const size_t start = pos_;
            while (pos_ < s_.size() &&
                   ((s_[pos_] >= 'a' && s_[pos_] <= 'z') ||
                    (s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '_' ||
                    s_[pos_] == '.')) {
                ++pos_;
            }
            const std::string name = s_.substr(start, pos_ - start);
            /* A name before '(' is a call: no key ever was one, since
             * format 1 refused a '(' after a value */
            const size_t after = pos_;
            skip();
            if (pos_ < s_.size() && s_[pos_] == '(') {
                return call(start, name, depth);
            }
            pos_ = after;
            input(start, name);
            part r;
            r.at = start;
            return r;
        }
        fail(pos_, "has " + q(glyph()) + " where a value should be");
    }

    const std::string& s_;
    const expr_scope& scope_;
    const ecuio_signal_set_t* base_;
    uint32_t format_;
    ecuio_computed_t p_{};
    std::vector<std::string> names_;
    std::vector<size_t> places_; /* where each input is first read */
    size_t pos_ = 0;
    uint32_t steps_ = 0;
};

} // namespace

/* The base def an obd2 key names, or -1 */
long obd2_def(const ecuio_signal_set_t* base, const std::string& key)
{
    int pid = -1;
    unsigned field = 0;
    for (const alias& a : kObd2Aliases) {
        if (key == a.key) {
            pid = a.pid;
        }
    }
    if (pid < 0 && key.size() >= 6 && key.compare(0, 4, "pid_") == 0) {
        auto hex = [](char c) {
            return (c >= '0' && c <= '9') ? c - '0'
                   : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                            : -1;
        };
        const int hi = hex(key[4]);
        const int lo = hex(key[5]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        if (key.size() == 6) {
            pid = hi * 16 + lo;
        } else if (key.size() == 8 && key[6] == '_' && key[7] >= '1' &&
                   key[7] <= '7') {
            pid = hi * 16 + lo;
            field = static_cast<unsigned>(key[7] - '0');
        } else {
            return -1;
        }
    }
    if (pid < 0) {
        return -1;
    }
    unsigned seen = 0;
    for (uint32_t i = 0; i < base->count; ++i) {
        const ecuio_signal_def_t& d = base->defs[i];
        if (d.mux_length == 2 && d.mux_offset == 0 &&
            d.mux_key == (0x4100u | static_cast<uint32_t>(pid))) {
            if (seen == field) {
                return static_cast<long>(i);
            }
            ++seen;
        }
    }
    return -1;
}

/* The key obd2's def at index is read by, the inverse of obd2_def: its
 * short name, else "pid_XX" for a PID's first field and "pid_XX_N" for
 * its next seven */
std::string obd2_key_of(const ecuio_signal_set_t* base, uint32_t index)
{
    const ecuio_signal_def_t& d = base->defs[index];
    if (d.mux_length != 2 || d.mux_offset != 0 ||
        (d.mux_key & 0xFFFFFF00u) != 0x4100u) {
        return std::string();
    }
    const auto pid = static_cast<uint8_t>(d.mux_key & 0xFFu);
    unsigned field = 0;
    for (uint32_t i = 0; i < index; ++i) {
        const ecuio_signal_def_t& x = base->defs[i];
        if (x.mux_length == 2 && x.mux_offset == 0 && x.mux_key == d.mux_key) {
            ++field;
        }
    }
    if (field > 7) {
        return std::string();
    }
    if (field == 0) {
        for (const alias& a : kObd2Aliases) {
            if (a.pid == pid) {
                return a.key;
            }
        }
    }
    static const char kHex[] = "0123456789abcdef";
    std::string key = "pid_";
    key += kHex[pid >> 4];
    key += kHex[pid & 0xFu];
    if (field != 0) {
        key += '_';
        key += static_cast<char>('0' + field);
    }
    return key;
}

ecuio_computed_t compile_expr(const std::string& text, const expr_scope& scope,
                              uint32_t format, std::vector<std::string>* names,
                              std::vector<size_t>* places)
{
    return compiler(text, scope, format).compile(names, places);
}

std::string too_long_words()
{
    return "is longer than " + std::to_string(ECUIO_EXPR_MAX_BYTES) +
           " characters";
}

std::string aged_computed_words(const std::string& reader,
                                const std::string& key)
{
    return "signal " + q(reader) + "'s max_age names " + q(key) +
           ", a computed signal: its own inputs carry their ages";
}

std::string replaced_words(const std::string& key, const std::string& name,
                           const std::string& reader)
{
    return "signal " + q(key) + " replaces " + q(name) + ", which " + reader +
           " reads";
}

const char* const kReplacedNotANumber =
    ": a computed signal reads a number, an enum or another computed signal";
const char* const kReplacedFormat =
    ": a computed signal read by another needs \"havne_signals\": 2";
const char* const kReplacedAged =
    " with an age bound: a computed input's ages are its own inputs'";

std::string too_deep_words(const std::string& name, uint32_t depth)
{
    return ": that puts " + q(name) + " " + std::to_string(depth) +
           " computed signals deep: a chain goes " +
           std::to_string(ECUIO_COMPUTED_MAX_DEPTH) + " deep at most";
}

namespace {

/* A node as a refusal names it: one with no key is the expression a
 * check was given */
std::string named(const chain_node& node)
{
    return node.key != nullptr ? "signal " + q(*node.key)
                               : std::string("the expression");
}

/* Each node's computed inputs, as node indexes. An input is found by its
 * ID, a replacement's (the base def's) too. */
std::vector<std::vector<size_t>>
chain_reads(const std::vector<chain_node>& nodes)
{
    const size_t n = nodes.size();
    /* The nodes by ID, to find an input's */
    std::vector<std::pair<uint16_t, size_t>> by_id(n);
    for (size_t i = 0; i < n; ++i) {
        by_id[i] = {nodes[i].id, i};
    }
    std::sort(by_id.begin(), by_id.end());
    std::vector<std::vector<size_t>> reads(n);
    for (size_t i = 0; i < n; ++i) {
        if (nodes[i].program == nullptr) {
            continue;
        }
        const ecuio_computed_t& c = *nodes[i].program;
        for (uint32_t k = 0; k < c.input_count; ++k) {
            const auto it =
                std::lower_bound(by_id.begin(), by_id.end(),
                                 std::pair<uint16_t, size_t>{
                                     static_cast<uint16_t>(c.input_ids[k]), 0});
            if (it != by_id.end() && it->first == c.input_ids[k] &&
                nodes[it->second].program != nullptr) {
                reads[i].push_back(it->second);
            }
        }
    }
    return reads;
}

/* Depths settled pass by pass: 1 for a node reading no computed node,
 * else one more than the deepest it reads. One left at 0 is on a circle
 * or reads one. */
std::vector<uint32_t>
settled_depths(const std::vector<chain_node>& nodes,
               const std::vector<std::vector<size_t>>& reads)
{
    const size_t n = nodes.size();
    std::vector<uint32_t> depth(n, 0);
    for (bool more = true; more;) {
        more = false;
        for (size_t i = 0; i < n; ++i) {
            if (nodes[i].program == nullptr || depth[i] != 0) {
                continue;
            }
            uint32_t d = 1;
            for (const size_t j : reads[i]) {
                d = depth[j] == 0 ? 0 : std::max(d, depth[j] + 1);
                if (d == 0) {
                    break;
                }
            }
            if (d != 0) {
                depth[i] = d;
                more = true;
            }
        }
    }
    return depth;
}

/* The shortest way from i back to it through what each node reads: i,
 * then the nodes back from it. Empty when i is on no circle. */
std::vector<size_t> way_round(const std::vector<std::vector<size_t>>& reads,
                              size_t i)
{
    const size_t n = reads.size();
    std::vector<size_t> from(n, n);
    std::vector<size_t> queue(reads[i]);
    for (const size_t j : reads[i]) {
        from[j] = i;
    }
    for (size_t at = 0; at < queue.size() && from[i] == n; ++at) {
        for (const size_t j : reads[queue[at]]) {
            if (from[j] == n) {
                from[j] = queue[at];
                queue.push_back(j);
            }
        }
    }
    if (from[i] == n) {
        return {};
    }
    std::vector<size_t> path{i};
    for (size_t j = from[i]; j != i; j = from[j]) {
        path.push_back(j);
    }
    return path;
}

/* The refusal of the circle way_round found from path's first node */
chain_refused circle_refused(const std::vector<chain_node>& nodes,
                             const std::vector<size_t>& path,
                             const std::vector<uint32_t>& depth)
{
    const size_t i = path[0];
    /* Only a signal with a key is read, so every node here has one */
    std::string words = "signal " + q(*nodes[i].key);
    std::vector<size_t> circle{i};
    for (size_t k = path.size(); k-- > 1;) {
        words += (k + 1 == path.size() ? " reads " : ", which reads ") +
                 q(*nodes[path[k]].key);
        circle.push_back(path[k]);
    }
    words += (path.size() == 1 ? " reads " : ", which reads ") +
             q(*nodes[i].key) + ": computed signals can't read in a circle";
    return chain_refused{i, words, circle, depth};
}

} // namespace

/* Computed signals reading each other (ADR-0062): no circle and no
 * chain past ECUIO_COMPUTED_MAX_DEPTH, refused at the first signal of
 * either in the nodes' order */
std::vector<uint32_t> chain_depths(const std::vector<chain_node>& nodes)
{
    const std::vector<std::vector<size_t>> reads = chain_reads(nodes);
    std::vector<uint32_t> depth = settled_depths(nodes, reads);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].program == nullptr) {
            continue;
        }
        if (depth[i] > ECUIO_COMPUTED_MAX_DEPTH) {
            throw chain_refused{
                i,
                named(nodes[i]) + " sits " + std::to_string(depth[i]) +
                    " computed signals deep: a chain goes " +
                    std::to_string(ECUIO_COMPUTED_MAX_DEPTH) + " deep at most",
                {},
                depth};
        }
        if (depth[i] != 0) {
            continue;
        }
        /* One that only reads a circle is passed by: a later signal is
         * on it */
        const std::vector<size_t> path = way_round(reads, i);
        if (!path.empty()) {
            throw circle_refused(nodes, path, depth);
        }
    }
    return depth;
}

} // namespace ecuio_sf
