// File/layer: core/src/signal_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest (test_signals); FUZZ: fuzz_signal_file
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
#include "signal_file_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ecuio_sf {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;

[[noreturn]] void refuse(const Value& at, std::string words)
{
    throw refused{at.line, at.column, std::move(words), &at};
}

[[noreturn]] void refuse(const Member& at, std::string words)
{
    throw refused{at.line, at.column, std::move(words), &at.value};
}

std::string q(std::string_view text)
{
    return ecuio_json::quoted(text);
}

const char* type_words(Type t)
{
    switch (t) {
    case Type::null:
        return "null";
    case Type::boolean:
        return "true or false";
    case Type::number:
        return "a number";
    case Type::string:
        return "a string";
    case Type::array:
        return "a list";
    default:
        return "an object";
    }
}

void expect(const Value& v, Type t, const std::string& what)
{
    if (v.type != t) {
        refuse(v, what + " is " + type_words(v.type) + ", not " + type_words(t));
    }
}

/* Every key of obj is one of allowed, "notes" (a string) or starts
 * "x-" (a tool's own); context names obj in the words */
void only_keys(const Value& obj, std::initializer_list<const char*> allowed,
               const std::string& context)
{
    for (const Member& m : obj.members) {
        if (m.key == "notes") {
            expect(m.value, Type::string, context + "'s notes");
            continue;
        }
        if (m.key.compare(0, 2, "x-") == 0) {
            continue;
        }
        bool known = false;
        for (const char* k : allowed) {
            known = known || m.key == k;
        }
        if (!known) {
            refuse(m, "unknown key " + q(m.key) + " in " + context);
        }
    }
}

/* A string of lo to hi bytes holding no NUL */
const std::string& text_of(const Value& v, size_t lo, size_t hi,
                           const std::string& what)
{
    expect(v, Type::string, what);
    if (v.text.size() < lo || v.text.size() > hi) {
        refuse(v, what + " is " + std::to_string(v.text.size()) +
                      " bytes; it takes " + std::to_string(lo) + " to " +
                      std::to_string(hi));
    }
    if (v.text.find('\0') != std::string::npos) {
        refuse(v, what + " holds a NUL character");
    }
    return v.text;
}

int64_t whole_of(const Value& v, int64_t lo, int64_t hi, const std::string& what)
{
    expect(v, Type::number, what);
    if (!v.integer || !v.in_int64 || v.whole < lo || v.whole > hi) {
        refuse(v, what + " takes a whole number from " + std::to_string(lo) +
                      " to " + std::to_string(hi));
    }
    return v.whole;
}

double finite_of(const Value& v, const std::string& what)
{
    expect(v, Type::number, what);
    if (!std::isfinite(v.number)) {
        refuse(v, what + " isn't a finite number");
    }
    return v.number;
}

/* Hex digits only, digits_lo to digits_hi of them */
uint32_t hex_of(const Value& v, size_t digits_lo, size_t digits_hi,
                const std::string& what)
{
    expect(v, Type::string, what);
    const std::string& s = v.text;
    if (s.size() < digits_lo || s.size() > digits_hi) {
        refuse(v, what + " " + q(s) + " takes " +
                      (digits_lo == digits_hi
                           ? std::to_string(digits_lo)
                           : std::to_string(digits_lo) + " to " +
                                 std::to_string(digits_hi)) +
                      " hex digits");
    }
    uint32_t n = 0;
    for (char c : s) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else {
            refuse(v, what + " " + q(s) + " isn't hex");
        }
        n = (n << 4) | d;
    }
    return n;
}

/* A key: 1 to 31 of [a-z0-9_] */
bool key_ok(const std::string& k)
{
    if (k.empty() || k.size() > 31) {
        return false;
    }
    for (char c : k) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* "user." and the name in lower case, every run of other characters a
 * single '-', none at either end; "" when nothing is left */
std::string profile_of(const std::string& name)
{
    std::string slug;
    bool dash = false;
    for (char c : name) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool upper = c >= 'A' && c <= 'Z';
        if (alnum || upper) {
            if (dash && !slug.empty()) {
                slug += '-';
            }
            dash = false;
            slug += upper ? static_cast<char>(c - 'A' + 'a') : c;
        } else {
            dash = true;
        }
    }
    return slug.empty() ? std::string() : "user." + slug;
}

bool is_11_bit(uint32_t id)
{
    return id <= 0x7FFu;
}

/* ADR-0020's tx, only where it's the standard's: rx - 8 for 0x7E8 to
 * 0x7EF, the 18DA swap for 18DAF1xx; 0 elsewhere */
uint32_t standard_tx(uint32_t rx)
{
    if (rx >= 0x7E8u && rx <= 0x7EFu) {
        return rx - 8u;
    }
    if ((rx & 0xFFFFFF00u) == 0x18DAF100u) {
        return 0x18DA00F1u | ((rx & 0xFFu) << 8);
    }
    return 0;
}

uint8_t group_of(const Value& v, const std::string& what)
{
    expect(v, Type::string, what);
    for (uint32_t g = 0; g <= ECUIO_SIGNAL_GROUP_DIAGNOSTICS; ++g) {
        std::string word = ecuio_signal_group_name(g);
        for (char& c : word) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (v.text == word) {
            return static_cast<uint8_t>(g);
        }
    }
    refuse(v, what + " " + q(v.text) +
                  " isn't a group: engine, air, fuel, oxygen sensors, "
                  "emissions, vehicle, electrical, diagnostics or other");
}

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

/* One signal of the file as read so far */
struct entry {
    const Value* v = nullptr;
    std::string key;
    std::string kind;
    uint32_t def = 0;   /* its index in the set's defs */
};

/* The expression compiler (ADR-0051 section 2): + - * /, parentheses,
 * unary minus, decimal constants and signals by key, into a program's
 * steps. At most 4 inputs, 16 constants, 16 operations. */
class compiler {
public:
    compiler(const Value& at, const std::string& text,
             const std::vector<entry>& entries, const ecuio_signal_set_t* base,
             const std::vector<ecuio_signal_def_t>& defs)
        : at_(at), s_(text), entries_(entries), base_(base), defs_(defs)
    {
    }

    ecuio_computed_t compile(std::vector<std::string>* names)
    {
        p_ = {};
        if (s_.size() > 256) {
            fail(0, "is longer than 256 characters");
        }
        expr(0);
        skip();
        if (pos_ < s_.size()) {
            fail(pos_, "has " + q(s_.substr(pos_, 1)) +
                           " where an operator or the end should be");
        }
        if (p_.input_count == 0) {
            fail(0, "reads no signal: a constant is no signal of its own");
        }
        p_.step_count = static_cast<uint8_t>(steps_);
        *names = names_;
        return p_;
    }

private:
    [[noreturn]] void fail(size_t at, const std::string& words)
    {
        refuse(at_, "expr " + q(s_) + " " + words + " (at character " +
                        std::to_string(at + 1) + ")");
    }

    void skip()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }

    void step(uint8_t s)
    {
        /* 33 steps hold 16 operations and their 17 values, so a step
         * past them is an operation past 16 */
        if (steps_ >= ECUIO_COMPUTED_MAX_STEPS) {
            fail(pos_, "has more than 16 operations");
        }
        p_.steps[steps_++] = s;
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
            const entry* e = nullptr;
            for (const entry& x : entries_) {
                if (x.key == name) {
                    e = &x;
                }
            }
            if (e == nullptr) {
                fail(at, "reads " + q(name) + ", which no signal is");
            }
            if (e->kind == "computed") {
                fail(at, "reads " + q(name) +
                             ", a computed signal: one can't read another");
            }
            if (e->kind == "text" || e->kind == "hex") {
                fail(at, "reads " + q(name) + ", which is text");
            }
            /* The id build gives the entry's def (its place in the
             * list), set or not yet: a later signal's is not */
            id = static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE +
                                       (e - entries_.data()));
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
        step(static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT | p_.input_count));
        ++p_.input_count;
    }

    void expr(unsigned depth)
    {
        term(depth);
        for (;;) {
            skip();
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                const bool add = s_[pos_++] == '+';
                term(depth);
                operation(add ? ECUIO_COMPUTED_ADD : ECUIO_COMPUTED_SUBTRACT);
            } else {
                return;
            }
        }
    }

    void term(unsigned depth)
    {
        factor(depth);
        for (;;) {
            skip();
            if (pos_ < s_.size() && (s_[pos_] == '*' || s_[pos_] == '/')) {
                const bool mul = s_[pos_++] == '*';
                factor(depth);
                operation(mul ? ECUIO_COMPUTED_MULTIPLY : ECUIO_COMPUTED_DIVIDE);
            } else {
                return;
            }
        }
    }

    void factor(unsigned depth)
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
            expr(depth + 1);
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ')') {
                fail(pos_, "has no ')' to close its '('");
            }
            ++pos_;
            return;
        }
        if (c == '-') {
            ++pos_;
            constant(0.0);
            factor(depth + 1);
            operation(ECUIO_COMPUTED_SUBTRACT);
            return;
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
            return;
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            const size_t start = pos_;
            while (pos_ < s_.size() &&
                   ((s_[pos_] >= 'a' && s_[pos_] <= 'z') ||
                    (s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '_' ||
                    s_[pos_] == '.')) {
                ++pos_;
            }
            input(start, s_.substr(start, pos_ - start));
            return;
        }
        fail(pos_, "has " + q(s_.substr(pos_, 1)) + " where a value should be");
    }

    const Value& at_;
    const std::string& s_;
    const std::vector<entry>& entries_;
    const ecuio_signal_set_t* base_;
    const std::vector<ecuio_signal_def_t>& defs_;
    ecuio_computed_t p_{};
    std::vector<std::string> names_;
    size_t pos_ = 0;
    uint32_t steps_ = 0;
};

/* s into a zeroed buffer of cap bytes, cut to cap - 1 */
void copy_text(char* dst, size_t cap, const std::string& s)
{
    const size_t n = std::min(s.size(), cap - 1);
    std::memset(dst, 0, cap);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = s[i];
    }
}

bool starts_like_formula(const std::string& s)
{
    return !s.empty() && (s[0] == '=' || s[0] == '+' || s[0] == '-' ||
                          s[0] == '@' || s[0] == '\t' || s[0] == '\r');
}

void no_formula(const Value& v, const std::string& what)
{
    if (starts_like_formula(v.text)) {
        refuse(v, what + " " + q(v.text) +
                      " starts as a spreadsheet formula would (= + - @, a tab "
                      "or a CR), which a recording's CSV can't hold");
    }
}

/* The ECUs: the base's channels first, then the file's, one width,
 * at most ECUIO_MAX_CHANNELS; ids to their rx */
void read_ecus(const Value* ecus, ecuio_signal_file& f,
               std::vector<std::pair<std::string, uint32_t>>* ids)
{
    if (ecus == nullptr) {
        return;
    }
    expect(*ecus, Type::array, "\"ecus\"");
    for (const Value& e : ecus->items) {
        expect(e, Type::object, "an ECU");
        only_keys(e, {"id", "rx", "tx"}, "an ECU");
        const Value* id_v = e.find("id");
        if (id_v == nullptr) {
            refuse(e, "an ECU needs an \"id\"");
        }
        const std::string id = text_of(*id_v, 1, 31, "an ECU's id");
        if (!key_ok(id)) {
            refuse(*id_v, "ECU id " + q(id) + " takes 1 to 31 of a-z, 0-9 and _");
        }
        for (const auto& known : *ids) {
            if (known.first == id) {
                refuse(*id_v, "ECU id " + q(id) + " is given twice");
            }
        }
        const Value* rx_v = e.find("rx");
        if (rx_v == nullptr) {
            refuse(e, "ecu " + q(id) + " needs an \"rx\"");
        }
        const uint32_t rx = hex_of(*rx_v, 1, 8, "ecu " + q(id) + "'s rx");
        if (rx == 0 || rx > 0x1FFFFFFFu) {
            refuse(*rx_v, "ecu " + q(id) + "'s rx is outside 1 to 1FFFFFFF");
        }
        uint32_t tx = 0;
        if (const Value* tx_v = e.find("tx")) {
            tx = hex_of(*tx_v, 1, 8, "ecu " + q(id) + "'s tx");
            if (tx == 0 || tx > 0x1FFFFFFFu) {
                refuse(*tx_v, "ecu " + q(id) + "'s tx is outside 1 to 1FFFFFFF");
            }
            if (is_11_bit(tx) != is_11_bit(rx)) {
                refuse(*tx_v, "ecu " + q(id) + "'s tx is another width than its rx");
            }
        } else {
            tx = standard_tx(rx);
            if (tx == 0) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "%X", static_cast<unsigned>(rx));
                refuse(e, "ecu " + q(id) + ": rx " + hex + " needs a tx");
            }
        }
        bool shared = false;
        for (const ecuio_channel_decl_t& c : f.channels) {
            if (c.rx_id == rx) {
                if (c.tx_id != tx) {
                    refuse(e, "ecu " + q(id) + " shares its rx with another "
                              "ECU but not its tx");
                }
                shared = true;
            }
        }
        for (const auto& known : *ids) {
            if (known.second == rx) {
                refuse(e, "ecu " + q(id) + " shares its rx with ecu " +
                              q(known.first));
            }
        }
        if (!shared) {
            if (f.channels.size() >= ECUIO_MAX_CHANNELS) {
                refuse(e, "ecu " + q(id) + " is past the 4 ECUs one session "
                          "reads, the base's counted");
            }
            for (const ecuio_channel_decl_t& c : f.channels) {
                if (is_11_bit(c.rx_id) != is_11_bit(rx)) {
                    refuse(e, "ecu " + q(id) + " is " +
                                  (is_11_bit(rx) ? "11-bit" : "29-bit") +
                                  " beside " +
                                  (is_11_bit(rx) ? "29-bit" : "11-bit") +
                                  " IDs: one session carries one width");
                }
            }
            ecuio_channel_decl_t c{};
            c.rx_id = rx;
            c.tx_id = tx;
            f.channels.push_back(c);
        }
        ids->emplace_back(id, rx);
    }
}

/* A signal that reads a reply: its ECU, request and field into d */
void read_field(const Value& s, const std::string& kind,
                const std::vector<std::pair<std::string, uint32_t>>& ecus,
                const ecuio_signal_file& f, ecuio_signal_def_t& d,
                const std::string& what)
{
    uint32_t rx = 0;
    if (const Value* ecu_v = s.find("ecu")) {
        const std::string id = text_of(*ecu_v, 1, 31, what + "'s ecu");
        for (const auto& e : ecus) {
            if (e.first == id) {
                rx = e.second;
            }
        }
        if (rx == 0) {
            refuse(*ecu_v, what + " reads ecu " + q(id) + ", which \"ecus\" doesn't list");
        }
    } else if (!ecus.empty()) {
        rx = ecus.front().second;
    } else if (!f.channels.empty()) {
        rx = f.channels.front().rx_id;
    } else {
        refuse(s, what + " reads a reply, but the file lists no ECU");
    }
    d.message_id = rx;

    const Value* read = s.find("read");
    if (read == nullptr) {
        refuse(s, what + " needs \"read\"");
    }
    expect(*read, Type::object, what + "'s read");
    only_keys(*read, {"service", "number"}, what + "'s read");
    const Value* service = read->find("service");
    const Value* number = read->find("number");
    if (service == nullptr || number == nullptr) {
        refuse(*read, what + "'s read needs \"service\" and \"number\"");
    }
    expect(*service, Type::string, what + "'s service");
    uint16_t echo = 0;
    if (service->text == "01") {
        d.mux_length = 2;
        d.mux_key = 0x4100u | hex_of(*number, 2, 2, what + "'s PID");
        echo = 2;
    } else if (service->text == "22") {
        d.mux_length = 3;
        d.mux_key = 0x620000u | hex_of(*number, 4, 4, what + "'s DID");
        echo = 3;
    } else {
        refuse(*service, what + "'s service " + q(service->text) +
                             " isn't \"01\" or \"22\"");
    }

    const Value* field = s.find("field");
    if (field == nullptr) {
        refuse(s, what + " needs \"field\"");
    }
    expect(*field, Type::object, what + "'s field");
    const bool text = kind == "text" || kind == "hex";
    if (text) {
        only_keys(*field, {"byte", "bytes"}, what + "'s field");
    } else if (kind == "enum") {
        only_keys(*field, {"byte", "bits", "bit", "order"}, what + "'s field");
    } else {
        only_keys(*field, {"byte", "bits", "bit", "order", "signed"},
                  what + "'s field");
    }
    const Value* byte = field->find("byte");
    if (byte == nullptr) {
        refuse(*field, what + "'s field needs \"byte\"");
    }
    d.start_byte = static_cast<uint16_t>(
        echo + whole_of(*byte, 0, ECUIO_FRAME_DATA_MAX - 1 - echo, what + "'s byte"));
    if (text) {
        if (const Value* bytes = field->find("bytes")) {
            d.byte_length = static_cast<uint16_t>(
                whole_of(*bytes, 0, ECUIO_FRAME_DATA_MAX, what + "'s bytes"));
            if (uint32_t{d.start_byte} + d.byte_length > ECUIO_FRAME_DATA_MAX) {
                refuse(*field, what + "'s field ends past the " +
                                   std::to_string(ECUIO_FRAME_DATA_MAX) +
                                   " bytes a reply holds");
            }
        }
        d.kind = static_cast<uint8_t>(kind == "hex" ? ECUIO_VALUE_HEX : ECUIO_VALUE_TEXT);
        d.byte_order = ECUIO_BYTE_ORDER_BE;
        return;
    }
    d.bit_length = 8;
    if (const Value* bits = field->find("bits")) {
        d.bit_length = static_cast<uint8_t>(whole_of(*bits, 1, 32, what + "'s bits"));
    }
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    if (const Value* order = field->find("order")) {
        expect(*order, Type::string, what + "'s order");
        if (order->text == "le") {
            d.byte_order = ECUIO_BYTE_ORDER_LE;
        } else if (order->text != "be") {
            refuse(*order, what + "'s order " + q(order->text) +
                               " isn't \"be\" or \"le\"");
        }
    }
    if (const Value* bit = field->find("bit")) {
        d.bit_offset = static_cast<uint8_t>(whole_of(*bit, 0, 7, what + "'s bit"));
    }
    if (d.byte_order == ECUIO_BYTE_ORDER_BE &&
        (d.bit_offset != 0 || d.bit_length % 8 != 0)) {
        refuse(*field, what + "'s field is big-endian, so whole bytes: bits a "
                              "multiple of 8 and no bit (\"order\": \"le\" for a "
                              "bit field)");
    }
    if (uint32_t{d.start_byte} + (d.bit_offset + d.bit_length + 7u) / 8u >
        ECUIO_FRAME_DATA_MAX) {
        refuse(*field, what + "'s field ends past the " +
                           std::to_string(ECUIO_FRAME_DATA_MAX) +
                           " bytes a reply holds");
    }
    bool is_signed = false;
    if (const Value* sv = field->find("signed")) {
        expect(*sv, Type::boolean, what + "'s signed");
        is_signed = sv->boolean;
    }
    d.kind = static_cast<uint8_t>(kind == "enum" ? ECUIO_VALUE_ENUM
                                  : is_signed      ? ECUIO_VALUE_INT
                                                   : ECUIO_VALUE_UINT);
}

/* An enum's labels, ascending by raw value, into f's table */
void read_values(const Value& s, ecuio_signal_file& f, ecuio_signal_def_t& d,
                 const std::string& what)
{
    const Value* values = s.find("values");
    if (values == nullptr) {
        refuse(s, what + " is an enum and needs \"values\"");
    }
    expect(*values, Type::object, what + "'s values");
    if (values->members.size() > ECUIO_ENUM_MAX_ENTRIES) {
        refuse(*values, what + " has " + std::to_string(values->members.size()) +
                            " values; an enum holds at most 256");
    }
    const uint64_t top = d.bit_length == 32 ? 0xFFFFFFFFull
                                            : ((1ull << d.bit_length) - 1);
    std::vector<ecuio_enum_entry_t> mine;
    for (const Member& m : values->members) {
        if (m.key.empty() || m.key.size() > 10 ||
            m.key.find_first_not_of("0123456789") != std::string::npos ||
            (m.key.size() > 1 && m.key[0] == '0')) {
            refuse(m, what + "'s value " + q(m.key) + " isn't a decimal number");
        }
        const uint64_t raw = std::stoull(m.key);
        if (raw > top) {
            refuse(m, what + "'s value " + m.key + " can't be in its " +
                          std::to_string(d.bit_length) + "-bit field");
        }
        const std::string label =
            text_of(m.value, 1, ECUIO_ENUM_LABEL_MAX, what + "'s label for " + m.key);
        no_formula(m.value, what + "'s label");
        ecuio_enum_entry_t e{};
        e.raw = static_cast<uint32_t>(raw);
        copy_text(e.label, sizeof e.label, label);
        mine.push_back(e);
    }
    std::sort(mine.begin(), mine.end(),
              [](const ecuio_enum_entry_t& a, const ecuio_enum_entry_t& b) {
                  return a.raw < b.raw;
              });
    if (f.enums.size() + mine.size() > ECUIO_SET_MAX_TABLE) {
        refuse(*values, "the file's enums hold more than 65536 labels");
    }
    d.table_first = static_cast<uint16_t>(f.enums.size());
    d.table_count = static_cast<uint16_t>(mine.size());
    f.enums.insert(f.enums.end(), mine.begin(), mine.end());
}

void read_source(const Value& source)
{
    expect(source, Type::object, "\"source\"");
    only_keys(source, {"name", "url", "commit", "license", "modified"},
              "\"source\"");
    const Value* source_name = source.find("name");
    if (source_name == nullptr) {
        refuse(source, "\"source\" needs a \"name\"");
    }
    text_of(*source_name, 1, 512, "the source's name");
    for (const char* k : {"url", "commit", "license", "modified"}) {
        if (const Value* v = source.find(k)) {
            text_of(*v, 1, 512, std::string("the source's ") + k);
        }
    }
}

void read_file(const Value& root, ecuio_signal_file& f)
{
    expect(root, Type::object, "the file");
    if (root.members.empty() || root.members.front().key != "havne_signals") {
        refuse(root, "a signal file starts with \"havne_signals\"");
    }
    const Value& version = root.members.front().value;
    expect(version, Type::number, "\"havne_signals\"");
    if (!version.integer || version.whole != ECUIO_SIGNAL_FILE_FORMAT) {
        refuse(version, "\"havne_signals\" is format " +
                            (version.integer ? std::to_string(version.whole)
                                             : std::string("?")) +
                            ", and this Havne reads format 1");
    }
    only_keys(root, {"havne_signals", "name", "source", "extends", "ecus", "signals"},
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

    p.extends = root.find("extends");
    p.ecus = root.find("ecus");
    p.signals_at = root.find("signals");
    p.own_list = true;
    build(p, f);
}

void build(const parts& in, ecuio_signal_file& f)
{
    f.name = in.name;
    f.profile = in.profile;
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
    if (list.empty()) {
        refuse(*signals, "\"signals\" lists none");
    }
    if (list.size() > ECUIO_SIGNAL_FILE_MAX_SIGNALS) {
        refuse(*signals, "\"signals\" lists " + std::to_string(list.size()) +
                             ", more than 1024");
    }

    /* First every signal's key and kind, so an expression can read a
     * signal listed after it */
    std::vector<entry> entries;
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
        for (const entry& x : entries) {
            if (x.key == e.key) {
                refuse(*key, "key " + q(e.key) + " is given twice");
            }
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
        e.def = static_cast<uint32_t>(f.defs.size() + entries.size());
        entries.push_back(e);
    }

    f.defs.resize(f.defs.size() + entries.size());
    std::vector<std::pair<const entry*, std::vector<std::string>>> programs;
    for (size_t i = 0; i < entries.size(); ++i) {
        const entry& e = entries[i];
        const Value& s = *e.v;
        const std::string what = "signal " + q(e.key);
        ecuio_signal_def_t& d = f.defs[e.def];
        d = ecuio_signal_def_t{};
        d.id = static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE + i);
        d.scale = 1.0;

        const bool computed = e.kind == "computed";
        const bool text = e.kind == "text" || e.kind == "hex";
        if (computed) {
            only_keys(s, {"key", "name", "kind", "expr", "max_age", "unit",
                          "decimals", "group", "plot"},
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
            only_keys(s, {"key", "name", "kind", "ecu", "read", "field", "scale",
                          "offset", "unit", "decimals", "group", "plot"},
                      what);
        }

        const Value* name_v = s.find("name");
        if (name_v == nullptr) {
            refuse(s, what + " needs a \"name\"");
        }
        copy_text(d.name, sizeof d.name, text_of(*name_v, 1, 31, what + "'s name"));
        no_formula(*name_v, what + "'s name");
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
            compiler c(*expr, expr->text, entries, base, f.defs);
            std::vector<std::string> names;
            ecuio_computed_t p = c.compile(&names);
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
        }
    }
    for (const entry& e : entries) {
        f.keys.emplace_back(e.key, e.def);
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
    if (ecuio_validate_signal_set(&f.set, &why) != ECUIO_VALIDATE_OK) {
        const size_t first = base != nullptr ? base->count : 0;
        /* Only these codes' index is a def's */
        const bool names_a_def = why.code > 0 ||
                                 why.code == ECUIO_VALIDATE_ERR_DUPLICATE_ID ||
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

    /* Every request must fit the engine's table, as registration checks */
    std::unique_ptr<ecuio_request_engine> engine(new ecuio_request_engine());
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    for (size_t i = 0; i < f.channels.size() && i < ECUIO_MAX_CHANNELS; ++i) {
        rx_ids[i] = f.channels[i].rx_id;
    }
    ecuio_engine_init(engine.get(), f.set.channel_count);
    uint32_t overflow = UINT32_MAX;
    if (!ecuio_engine_derive(engine.get(), &f.set, rx_ids, f.set.channel_count,
                             &overflow)) {
        const size_t first = base != nullptr ? base->count : 0;
        const Value& at = (overflow != UINT32_MAX && overflow >= first &&
                           overflow - first < entries.size())
                              ? *entries[overflow - first].v
                              : *signals;
        refuse(at, "the signals ask more than " +
                       std::to_string(ECUIO_PROFILE_MAX_REQUESTS) +
                       " requests, the base's counted");
    }
}

} // namespace ecuio_sf

using ecuio_sf::refused;

extern "C" {

ecuio_status_t ecuio_signal_file_load(const char* bytes, size_t length,
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
    try {
        ecuio_json::Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = ECUIO_SIGNAL_FILE_MAX_BYTES;
        if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root,
                               &error, limits)) {
            ecuio_detail_set("%s", error.to_string().c_str());
            return error.words == "out of memory" ? ECUIO_ERR_OUT_OF_MEMORY
                                                   : ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::unique_ptr<ecuio_signal_file> f(new ecuio_signal_file());
        ecuio_sf::read_file(root, *f);
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

void ecuio_signal_file_free(ecuio_signal_file_t* file)
{
    delete file;
}

} // extern "C"
