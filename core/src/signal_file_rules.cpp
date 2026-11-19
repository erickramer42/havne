// File/layer: core/src/signal_file_rules.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, PluginTest, ConditionFileTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// The rules the signal-file loader applies to any JSON it reads
// (signal_file_build.h), shared with the condition and plugin loaders:
// a value's type, keys, text and numbers, each refusal at its place.

#include "json.h"
#include "signal_file_build.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

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
                           std::string_view what_view)
{
    const std::string what(what_view);
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

} // namespace ecuio_sf
