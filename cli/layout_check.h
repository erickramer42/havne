// File/layer: cli/layout_check.h | havne-cli, a dashboard layout file's check (ADR-0057's appendix on the layout file)
// Testing: UNIT: CliLayoutTest (test_cli), the shared corpus tests/data/layouts
//
// `havne-cli layout check <file>`: the reading rules of the app's
// DashboardBook (app/qml/DashboardBook.qml: parse, pageProblem,
// cellProblem, documentProblem, importProblem), ported to C++ so a layout
// file can be checked with no app. The port is held equal to the QML by
// the corpus in tests/data/layouts: tst_dashboardbook and test_cli read
// each case to the verdicts beside it, so a rule changed on one side
// fails the other. A rule here is the QML's rule, in the QML's order.
//
// The JSON is read as JavaScript's JSON.parse reads it (the QML's reader):
// strings are UTF-16 units, a lone surrogate escape is kept, a repeated
// key takes its last value, a number past a double is a read that fails.
// Sizes are the QML's too: a stored text's bytes are those of what
// JSON.stringify gives, so serialize() writes that form.

#ifndef HAVNE_CLI_LAYOUT_CHECK_H
#define HAVNE_CLI_LAYOUT_CHECK_H

#include "jsonl.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cli {
namespace layout {

constexpr int kMaxPages = 10;
constexpr int kMaxCells = 24;
constexpr int kMaxRefs = 8;
constexpr int kMaxLabel = 40;
constexpr int kMaxColumns = 12;
constexpr int kMaxRows = 48;
constexpr int kFormat1MaxRows = 16;
constexpr int kFormat1MaxBytes = 65536;
constexpr int kMaxBytes = kFormat1MaxBytes + 2 * kMaxPages * kMaxCells;
constexpr int kPhoneSpanScale = 3;
constexpr int kMaxFiles = 32;
constexpr int kMaxFileName = 80;
// What the form and files keys may add to a stored document's bound
constexpr int kFileKeysBytes = 16384;
constexpr int kFileMaxBytes = kMaxBytes + kFileKeysBytes;
constexpr int kMaxDepth = 512;

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Null;
    bool b = false;
    double num = 0;
    std::u16string str;
    std::vector<Json> arr;
    std::vector<std::u16string> keys; // an object's, parallel to vals
    std::vector<Json> vals;

    bool is_object() const { return kind == Obj; }
    bool is_array() const { return kind == Arr; }
    bool is_string() const { return kind == Str; }
    bool is_number() const { return kind == Num; }

    static std::u16string ascii(const char* s)
    {
        std::u16string out;
        for (; *s != '\0'; ++s) {
            out += static_cast<char16_t>(*s);
        }
        return out;
    }
    // The value under an ASCII key; null when the key isn't there (a
    // JSON null is a value: JavaScript's undefined is the missing key)
    const Json* get(const char* key) const
    {
        if (kind != Obj) {
            return nullptr;
        }
        const std::u16string k = ascii(key);
        for (size_t i = keys.size(); i > 0; --i) {
            if (keys[i - 1] == k) {
                return &vals[i - 1];
            }
        }
        return nullptr;
    }
    void set(const std::u16string& key, Json v)
    {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                vals[i] = std::move(v);
                return;
            }
        }
        keys.push_back(key);
        vals.push_back(std::move(v));
    }
    void erase(const char* key)
    {
        const std::u16string k = ascii(key);
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == k) {
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
                vals.erase(vals.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }
};

// Strict UTF-8 (no overlong form, surrogate or code point past U+10FFFF):
// what a reader must hand the checks, as the app's does
inline bool valid_utf8(const std::string& s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        size_t len = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        i += len;
    }
    return true;
}

// A leading UTF-8 BOM is skipped, as the app's reader skips it
inline void strip_bom(std::string* bytes)
{
    if (bytes->compare(0, 3, "\xEF\xBB\xBF") == 0) {
        bytes->erase(0, 3);
    }
}

// JSON.parse's grammar over valid UTF-8; false where it would throw
class Parser {
public:
    explicit Parser(const std::string& text) : m_s(text) {}

    bool parse(Json* out)
    {
        skip();
        if (!value(out, 0)) {
            return false;
        }
        skip();
        return m_i == m_s.size();
    }

private:
    const std::string& m_s;
    size_t m_i = 0;

    void skip()
    {
        while (m_i < m_s.size() &&
               (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' ||
                m_s[m_i] == '\r')) {
            ++m_i;
        }
    }
    bool word(const char* w)
    {
        const size_t n = std::char_traits<char>::length(w);
        if (m_s.compare(m_i, n, w) != 0) {
            return false;
        }
        m_i += n;
        return true;
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool number(Json* out)
    {
        const size_t start = m_i;
        if (m_i < m_s.size() && m_s[m_i] == '-') {
            ++m_i;
        }
        if (m_i >= m_s.size() || !digit(m_s[m_i])) {
            return false;
        }
        if (m_s[m_i] == '0') {
            ++m_i;
        } else {
            while (m_i < m_s.size() && digit(m_s[m_i])) {
                ++m_i;
            }
        }
        if (m_i < m_s.size() && m_s[m_i] == '.') {
            ++m_i;
            if (m_i >= m_s.size() || !digit(m_s[m_i])) {
                return false;
            }
            while (m_i < m_s.size() && digit(m_s[m_i])) {
                ++m_i;
            }
        }
        if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == '+' || m_s[m_i] == '-')) {
                ++m_i;
            }
            if (m_i >= m_s.size() || !digit(m_s[m_i])) {
                return false;
            }
            while (m_i < m_s.size() && digit(m_s[m_i])) {
                ++m_i;
            }
        }
        const std::string slice = m_s.substr(start, m_i - start);
        errno = 0;
        const double v = std::strtod(slice.c_str(), nullptr);
        bool nonzero = false;
        for (const char c : slice) {
            if (c == 'e' || c == 'E') {
                break;
            }
            nonzero = nonzero || (c >= '1' && c <= '9');
        }
        // The app's reader refuses a number past a double, either way: one
        // too large, or a nonzero one too small to be anything but 0
        if (std::isinf(v) || (v == 0 && nonzero)) {
            return false;
        }
        out->kind = Json::Num;
        out->num = v;
        return true;
    }

    static bool hex4(const std::string& s, size_t at, unsigned* v)
    {
        if (at + 4 > s.size()) {
            return false;
        }
        unsigned r = 0;
        for (size_t k = 0; k < 4; ++k) {
            const char c = s[at + k];
            r <<= 4;
            if (c >= '0' && c <= '9') {
                r |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                r |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                r |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        *v = r;
        return true;
    }

    bool string(std::u16string* out)
    {
        ++m_i; // the opening quote
        out->clear();
        while (m_i < m_s.size()) {
            const auto c = static_cast<unsigned char>(m_s[m_i]);
            if (c == '"') {
                ++m_i;
                return true;
            }
            if (c < 0x20) {
                return false;
            }
            if (c == '\\') {
                ++m_i;
                if (m_i >= m_s.size()) {
                    return false;
                }
                const char e = m_s[m_i++];
                switch (e) {
                case '"': *out += u'"'; break;
                case '\\': *out += u'\\'; break;
                case '/': *out += u'/'; break;
                case 'b': *out += u'\b'; break;
                case 'f': *out += u'\f'; break;
                case 'n': *out += u'\n'; break;
                case 'r': *out += u'\r'; break;
                case 't': *out += u'\t'; break;
                case 'u': {
                    unsigned v = 0;
                    if (!hex4(m_s, m_i, &v)) {
                        return false;
                    }
                    m_i += 4;
                    *out += static_cast<char16_t>(v);
                    break;
                }
                default: return false;
                }
                continue;
            }
            if (c < 0x80) {
                *out += static_cast<char16_t>(c);
                ++m_i;
                continue;
            }
            const size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            uint32_t cp = c & (len == 4 ? 0x07u : len == 3 ? 0x0Fu : 0x1Fu);
            for (size_t k = 1; k < len; ++k) {
                cp = (cp << 6) |
                     (static_cast<unsigned char>(m_s[m_i + k]) & 0x3Fu);
            }
            m_i += len;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                *out += static_cast<char16_t>(0xD800 + (cp >> 10));
                *out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
            } else {
                *out += static_cast<char16_t>(cp);
            }
        }
        return false;
    }

    bool value(Json* out, int depth)
    {
        if (depth > kMaxDepth || m_i >= m_s.size()) {
            return false;
        }
        const char c = m_s[m_i];
        if (c == '{') {
            ++m_i;
            out->kind = Json::Obj;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == '}') {
                ++m_i;
                return true;
            }
            for (;;) {
                skip();
                if (m_i >= m_s.size() || m_s[m_i] != '"') {
                    return false;
                }
                std::u16string key;
                if (!string(&key)) {
                    return false;
                }
                skip();
                if (m_i >= m_s.size() || m_s[m_i] != ':') {
                    return false;
                }
                ++m_i;
                skip();
                Json v;
                if (!value(&v, depth + 1)) {
                    return false;
                }
                out->set(key, std::move(v));
                skip();
                if (m_i < m_s.size() && m_s[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_i < m_s.size() && m_s[m_i] == '}') {
                    ++m_i;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++m_i;
            out->kind = Json::Arr;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == ']') {
                ++m_i;
                return true;
            }
            for (;;) {
                skip();
                Json v;
                if (!value(&v, depth + 1)) {
                    return false;
                }
                out->arr.push_back(std::move(v));
                skip();
                if (m_i < m_s.size() && m_s[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_i < m_s.size() && m_s[m_i] == ']') {
                    ++m_i;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out->kind = Json::Str;
            return string(&out->str);
        }
        if (c == 't') {
            out->kind = Json::Bool;
            out->b = true;
            return word("true");
        }
        if (c == 'f') {
            out->kind = Json::Bool;
            out->b = false;
            return word("false");
        }
        if (c == 'n') {
            out->kind = Json::Null;
            return word("null");
        }
        return number(out);
    }
};

// JavaScript's Number to string: the shortest digits that read back, in
// ECMAScript's layout (no exponent from 1e-6 up to 1e21)
inline std::string js_number(double v)
{
    if (!std::isfinite(v)) {
        return "null";
    }
    if (v == 0) {
        return "0";
    }
    std::string sign = v < 0 ? "-" : "";
    v = std::fabs(v);
    char buf[64];
    int precision = 0;
    for (; precision < 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*e", precision, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    const std::string e = buf; // d.ddde+XX
    const size_t at = e.find('e');
    std::string digits;
    for (size_t i = 0; i < at; ++i) {
        if (e[i] != '.') {
            digits += e[i];
        }
    }
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }
    const int n = static_cast<int>(std::strtol(e.c_str() + at + 1, nullptr, 10)) + 1; // the point's place
    const int k = static_cast<int>(digits.size());
    std::string out;
    if (k <= n && n <= 21) {
        out = digits + std::string(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out = digits.substr(0, static_cast<size_t>(n)) + "." +
              digits.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string(static_cast<size_t>(-n), '0') + digits;
    } else {
        const int x = n - 1;
        out = digits.substr(0, 1) +
              (k > 1 ? "." + digits.substr(1) : std::string()) + "e" +
              (x < 0 ? "-" : "+") + std::to_string(x < 0 ? -x : x);
    }
    return sign + out;
}

inline void put_utf8(std::string* out, uint32_t cp)
{
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// A string's UTF-8 as JSON.stringify writes it (a lone surrogate escaped)
inline void write_string(std::string* out, const std::u16string& s)
{
    static const char* hex = "0123456789abcdef";
    *out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned u = s[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            put_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00));
            ++i;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            *out += "\\u";
            for (int sh = 12; sh >= 0; sh -= 4) {
                *out += hex[(u >> sh) & 0xF];
            }
        } else if (u == '"') {
            *out += "\\\"";
        } else if (u == '\\') {
            *out += "\\\\";
        } else if (u == '\b') {
            *out += "\\b";
        } else if (u == '\f') {
            *out += "\\f";
        } else if (u == '\n') {
            *out += "\\n";
        } else if (u == '\r') {
            *out += "\\r";
        } else if (u == '\t') {
            *out += "\\t";
        } else if (u < 0x20) {
            *out += "\\u00";
            *out += hex[u >> 4];
            *out += hex[u & 0xF];
        } else {
            put_utf8(out, u);
        }
    }
    *out += '"';
}

// JSON.stringify(v): the size of a stored text
inline void serialize(const Json& v, std::string* out)
{
    switch (v.kind) {
    case Json::Null: *out += "null"; break;
    case Json::Bool: *out += v.b ? "true" : "false"; break;
    case Json::Num: *out += js_number(v.num); break;
    case Json::Str: write_string(out, v.str); break;
    case Json::Arr:
        *out += '[';
        for (size_t i = 0; i < v.arr.size(); ++i) {
            if (i > 0) {
                *out += ',';
            }
            serialize(v.arr[i], out);
        }
        *out += ']';
        break;
    case Json::Obj:
        *out += '{';
        for (size_t i = 0; i < v.keys.size(); ++i) {
            if (i > 0) {
                *out += ',';
            }
            write_string(out, v.keys[i]);
            *out += ':';
            serialize(v.vals[i], out);
        }
        *out += '}';
        break;
    }
}

inline size_t serialized_bytes(const Json& v)
{
    std::string s;
    serialize(v, &s);
    return s.size();
}

inline bool is_integer(const Json* v)
{
    return v != nullptr && v->kind == Json::Num && std::isfinite(v->num) &&
           std::floor(v->num) == v->num;
}

// A label's length as a reader counts it: a surrogate pair is one
inline size_t code_points(const std::u16string& s)
{
    size_t n = s.size();
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] >= 0xD800 && s[i] <= 0xDBFF && s[i + 1] >= 0xDC00 &&
            s[i + 1] <= 0xDFFF) {
            --n;
            ++i;
        }
    }
    return n;
}

// JavaScript's String.prototype.trim: white space and line terminators
inline bool js_space(char16_t c)
{
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

inline std::u16string js_trim(const std::u16string& s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && js_space(s[a])) {
        ++a;
    }
    while (b > a && js_space(s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}

struct OptionRule {
    const char* name;
    enum Type { Number, Positive, Whole, Bool, Text, Any } type;
    int min;
    int max;
};

inline const OptionRule* option_rules(size_t* count)
{
    static const OptionRule rules[] = {
        {"from", OptionRule::Number, 0, 0},
        {"to", OptionRule::Number, 0, 0},
        {"coldBelow", OptionRule::Number, 0, 0},
        {"warnFrom", OptionRule::Number, 0, 0},
        {"dangerFrom", OptionRule::Number, 0, 0},
        {"rise", OptionRule::Number, 0, 0},
        {"warnBelow", OptionRule::Number, 0, 0},
        {"dangerBelow", OptionRule::Number, 0, 0},
        {"majorStep", OptionRule::Positive, 0, 0},
        {"minorStep", OptionRule::Positive, 0, 0},
        {"labelDivisor", OptionRule::Positive, 0, 0},
        {"decimals", OptionRule::Whole, 0, 6},
        {"count", OptionRule::Whole, 1, 32},
        {"cells", OptionRule::Whole, 2, 64},
        {"digits", OptionRule::Whole, 1, 8},
        {"window", OptionRule::Whole, 10, 120},
        {"peak", OptionRule::Bool, 0, 0},
        {"progress", OptionRule::Bool, 0, 0},
        {"vertical", OptionRule::Bool, 0, 0},
        {"marks", OptionRule::Bool, 0, 0},
        {"caption", OptionRule::Text, 0, 40},
        {"description", OptionRule::Text, 0, 80},
        {"peakLabel", OptionRule::Text, 0, 8},
        {"accent", OptionRule::Any, 0, 0},
        {"color", OptionRule::Any, 0, 0},
    };
    *count = sizeof rules / sizeof rules[0];
    return rules;
}

inline bool option_fits(const OptionRule& r, const Json& v)
{
    switch (r.type) {
    case OptionRule::Number:
        return v.kind == Json::Num && std::isfinite(v.num);
    case OptionRule::Positive:
        return v.kind == Json::Num && std::isfinite(v.num) && v.num > 0;
    case OptionRule::Whole:
        return is_integer(&v) && v.num >= r.min && v.num <= r.max;
    case OptionRule::Bool:
        return v.kind == Json::Bool;
    case OptionRule::Text:
        return v.kind == Json::Str &&
               code_points(v.str) <= static_cast<size_t>(r.max);
    case OptionRule::Any:
        return v.kind == Json::Str;
    }
    return false;
}

// Whether a cell may be kept (DashboardBook.cellProblem is ""); rows the
// bound on its height
inline bool cell_fits(const Json& c, int rows)
{
    if (!c.is_object()) {
        return false;
    }
    const Json* kind = c.get("kind");
    if (kind == nullptr || !kind->is_string() || kind->str.empty()) {
        return false;
    }
    const Json* w = c.get("w");
    if (!is_integer(w) || w->num < 1 || w->num > kMaxColumns) {
        return false;
    }
    const Json* h = c.get("h");
    if (!is_integer(h) || h->num < 1 || h->num > rows) {
        return false;
    }
    const Json* refs = c.get("refs");
    if (refs == nullptr || !refs->is_array()) {
        return false;
    }
    for (const Json& r : refs->arr) {
        if (!r.is_string()) {
            return false;
        }
    }
    if (refs->arr.size() > static_cast<size_t>(kMaxRefs)) {
        return false;
    }
    const Json* label = c.get("label");
    if (label != nullptr &&
        (!label->is_string() ||
         code_points(label->str) > static_cast<size_t>(kMaxLabel))) {
        return false;
    }
    const Json* unit = c.get("unit");
    if (unit != nullptr && !unit->is_string()) {
        return false;
    }
    const Json* options = c.get("options");
    if (options == nullptr) {
        return true;
    }
    if (!options->is_object()) {
        return false;
    }
    size_t n = 0;
    const OptionRule* rules = option_rules(&n);
    for (size_t i = 0; i < n; ++i) {
        const Json* v = options->get(rules[i].name);
        if (v != nullptr && !option_fits(rules[i], *v)) {
            return false;
        }
    }
    const Json* from = options->get("from");
    const Json* to = options->get("to");
    return from == nullptr || to == nullptr || !(from->num >= to->num);
}

// Whether a page's own fields may be kept (DashboardBook.pageProblem)
inline bool page_fits(const Json& p)
{
    if (!p.is_object()) {
        return false;
    }
    const Json* name = p.get("name");
    if (name == nullptr || !name->is_string()) {
        return false;
    }
    const std::u16string trimmed = js_trim(name->str);
    if (trimmed.empty() ||
        code_points(trimmed) > static_cast<size_t>(kMaxLabel)) {
        return false;
    }
    const Json* cells = p.get("cells");
    return cells != nullptr && cells->is_array();
}

// DashboardBook.documentProblem is "": the document reads back as it is
inline bool document_fits(const Json& d)
{
    const Json* format = d.get("format");
    const Json* pages = d.get("pages");
    if (!d.is_object() || !is_integer(format) || format->num != 2 ||
        pages == nullptr || !pages->is_array() || pages->arr.empty() ||
        pages->arr.size() > static_cast<size_t>(kMaxPages)) {
        return false;
    }
    for (const Json& p : pages->arr) {
        if (!page_fits(p)) {
            return false;
        }
        const Json* cells = p.get("cells");
        if (cells->arr.size() > static_cast<size_t>(kMaxCells)) {
            return false;
        }
        for (const Json& c : cells->arr) {
            if (!cell_fits(c, kMaxRows)) {
                return false;
            }
        }
    }
    return serialized_bytes(d) <= static_cast<size_t>(kMaxBytes);
}

struct File {
    std::string name; // UTF-8
    std::string sha256;
};

struct Verdict {
    // "" (read), "dropped" (read with pages or cells left out), "none"
    // (nothing to read), "damaged", "newer" or "otherform"
    std::string problem;
    std::string as;   // the form it was read as
    std::string form; // the file's own, "" for none
    bool read = false; // problem is "" or "dropped": the counts hold
    int pages = 0;
    int cells = 0;
    int dropped_pages = 0;
    int dropped_cells = 0;
    std::vector<File> files;
    std::vector<std::string> notes;
    bool has_first = false;
    int first_w = 0;
    int first_h = 0;
};

inline std::string utf8_of(const std::u16string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned u = s[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            put_utf8(&out, 0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00));
            ++i;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            put_utf8(&out, 0xFFFD);
        } else {
            put_utf8(&out, u);
        }
    }
    return out;
}

// A file's check: DashboardBook.importProblem's verdicts. as is the form
// loading it, "phone" or "desktop", or "" to take the file's own form
// ("desktop", noted "assumed-desktop", when it names none). text holds
// valid UTF-8 with no BOM
inline Verdict check(const std::string& text, const std::string& as)
{
    Verdict v;
    v.as = as;
    if (text.empty()) {
        v.problem = "none";
        return v;
    }
    if (text.size() > static_cast<size_t>(kFileMaxBytes)) {
        v.problem = "damaged";
        return v;
    }
    Json d;
    if (!Parser(text).parse(&d) || !d.is_object()) {
        v.problem = "damaged";
        return v;
    }
    const Json* declared_p = d.get("form");
    const Json* listed_p = d.get("files");
    const bool has_declared = declared_p != nullptr;
    const bool has_listed = listed_p != nullptr;
    const Json declared = has_declared ? *declared_p : Json();
    const Json listed = has_listed ? *listed_p : Json();
    d.erase("form");
    d.erase("files");
    const bool declared_ok =
        has_declared && declared.is_string() &&
        (declared.str == u"phone" || declared.str == u"desktop");
    std::string form = as;
    std::vector<std::string> notes;
    if (form.empty()) {
        form = declared_ok ? utf8_of(declared.str) : "desktop";
        if (!has_declared) {
            notes.push_back("assumed-desktop");
        }
    }
    v.as = form;

    // parse: the stored text's rules, over what a write would store
    const size_t bytes = serialized_bytes(d);
    if (bytes > static_cast<size_t>(kMaxBytes)) {
        v.problem = "damaged";
        return v;
    }
    const Json* format = d.get("format");
    if (!is_integer(format) || format->num < 1) {
        v.problem = "damaged";
        return v;
    }
    if (format->num > 2) {
        v.problem = "newer";
        return v;
    }
    Json* pages = nullptr;
    for (size_t i = 0; i < d.keys.size(); ++i) {
        if (d.keys[i] == u"pages") {
            pages = &d.vals[i];
        }
    }
    if (pages == nullptr || !pages->is_array() ||
        pages->arr.size() > static_cast<size_t>(kMaxPages)) {
        v.problem = "damaged";
        return v;
    }
    for (const Json& p : pages->arr) {
        const Json* cells = p.is_object() ? p.get("cells") : nullptr;
        if (cells != nullptr && cells->is_array() &&
            cells->arr.size() > static_cast<size_t>(kMaxCells)) {
            v.problem = "damaged";
            return v;
        }
    }
    const int rows = format->num == 1 ? kFormat1MaxRows : kMaxRows;
    const size_t pages_read = pages->arr.size();
    std::vector<Json> kept;
    for (Json& p : pages->arr) {
        if (page_fits(p)) {
            kept.push_back(std::move(p));
        }
    }
    pages->arr = std::move(kept);
    size_t cells_dropped = 0;
    for (Json& p : pages->arr) {
        for (size_t i = 0; i < p.keys.size(); ++i) {
            if (p.keys[i] != u"cells") {
                continue;
            }
            std::vector<Json> kept_cells;
            for (Json& c : p.vals[i].arr) {
                if (cell_fits(c, rows)) {
                    kept_cells.push_back(std::move(c));
                } else {
                    ++cells_dropped;
                }
            }
            p.vals[i].arr = std::move(kept_cells);
        }
    }
    if (format->num == 1) {
        if (bytes > static_cast<size_t>(kFormat1MaxBytes) ||
            serialized_bytes(d) > static_cast<size_t>(kFormat1MaxBytes)) {
            v.problem = "damaged";
            return v;
        }
        // The upgrade: the form is the key the text came from
        for (size_t i = 0; i < d.keys.size(); ++i) {
            if (d.keys[i] == u"format") {
                d.vals[i].num = 2;
            }
        }
        if (form == "phone") {
            for (Json& p : pages->arr) {
                for (size_t i = 0; i < p.keys.size(); ++i) {
                    if (p.keys[i] != u"cells") {
                        continue;
                    }
                    for (Json& c : p.vals[i].arr) {
                        for (size_t k = 0; k < c.keys.size(); ++k) {
                            if (c.keys[k] == u"w") {
                                c.vals[k].num = std::fmin(
                                    kMaxColumns, c.vals[k].num * kPhoneSpanScale);
                            } else if (c.keys[k] == u"h") {
                                c.vals[k].num *= kPhoneSpanScale;
                            }
                        }
                    }
                }
            }
        }
    }
    if (!document_fits(d)) {
        v.problem = "damaged";
        return v;
    }

    // importProblem: the file's own keys
    if ((has_declared && !declared_ok) ||
        (has_listed &&
         (!listed.is_array() ||
          listed.arr.size() > static_cast<size_t>(kMaxFiles)))) {
        v.problem = "damaged";
        return v;
    }
    std::vector<File> files;
    if (has_listed) {
        for (const Json& f : listed.arr) {
            const Json* name = f.is_object() ? f.get("name") : nullptr;
            const Json* sha = f.is_object() ? f.get("sha256") : nullptr;
            bool hex = sha != nullptr && sha->is_string() && sha->str.size() == 64;
            for (size_t i = 0; hex && i < sha->str.size(); ++i) {
                const char16_t c = sha->str[i];
                hex = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
            }
            if (name == nullptr || !name->is_string() ||
                js_trim(name->str).empty() ||
                code_points(name->str) > static_cast<size_t>(kMaxFileName) ||
                !hex) {
                v.problem = "damaged";
                return v;
            }
            files.push_back({utf8_of(name->str), utf8_of(sha->str)});
        }
    }
    if (has_declared) {
        v.form = utf8_of(declared.str);
    }
    if (has_declared && v.form != form) {
        v.problem = "otherform";
        return v;
    }
    v.read = true;
    v.files = std::move(files);
    v.notes = notes;
    if (!has_declared) {
        v.notes.insert(v.notes.begin(), "no-form");
    }
    v.pages = static_cast<int>(pages->arr.size());
    for (const Json& p : pages->arr) {
        v.cells += static_cast<int>(p.get("cells")->arr.size());
    }
    v.dropped_pages = static_cast<int>(pages_read - pages->arr.size());
    v.dropped_cells = static_cast<int>(cells_dropped);
    v.problem = v.dropped_pages > 0 || v.dropped_cells > 0 ? "dropped" : "";
    const Json& first = pages->arr[0];
    const Json& first_cells = *first.get("cells");
    if (!first_cells.arr.empty()) {
        v.has_first = true;
        v.first_w = static_cast<int>(first_cells.arr[0].get("w")->num);
        v.first_h = static_cast<int>(first_cells.arr[0].get("h")->num);
    }
    return v;
}

// The words a refused verdict gives
inline const char* layout_words(const Verdict& v)
{
    if (v.problem == "none") {
        return "the file is empty";
    }
    if (v.problem == "newer") {
        return "the file is from a newer version of the dashboard format";
    }
    if (v.problem == "otherform") {
        return v.form == "phone"
                   ? "the file is a phone layout, and a layout is never scaled to another form"
                   : "the file is a desktop layout, and a layout is never scaled to another form";
    }
    if (v.problem == "damaged") {
        return "the file isn't a layout this version reads: not JSON, past a bound, or a field of the wrong kind";
    }
    return "";
}

inline size_t layout_line_bytes(const Verdict& v)
{
    size_t n = 1024;
    for (const File& f : v.files) {
        n += Line::string_bytes(f.name.c_str()) + 128;
    }
    return n;
}

// The `layout` line: what the file is and what loading it would drop. The
// counts, the first cell's size and `dropped` are there when it reads;
// `needs_files` is the signal files its layout names (the check has no
// vehicle to say which are missing); `words` when it is refused
inline void layout_line(Line& l, const Verdict& v)
{
    l.raw("{\"type\":\"layout\"");
    l.key("ok");
    l.raw(v.read ? "true" : "false");
    l.key("problem");
    l.string(v.problem.c_str(), SIZE_MAX);
    l.key("as");
    l.string(v.as.c_str(), SIZE_MAX);
    l.key("form");
    if (v.form.empty()) {
        l.raw("null");
    } else {
        l.string(v.form.c_str(), SIZE_MAX);
    }
    if (v.read) {
        l.key("pages");
        l.u64(static_cast<uint64_t>(v.pages));
        l.key("cells");
        l.u64(static_cast<uint64_t>(v.cells));
        l.key("dropped");
        l.raw("{\"pages\":");
        l.u64(static_cast<uint64_t>(v.dropped_pages));
        l.raw(",\"cells\":");
        l.u64(static_cast<uint64_t>(v.dropped_cells));
        l.ch('}');
        l.key("first_cell");
        if (v.has_first) {
            l.raw("{\"w\":");
            l.u64(static_cast<uint64_t>(v.first_w));
            l.raw(",\"h\":");
            l.u64(static_cast<uint64_t>(v.first_h));
            l.ch('}');
        } else {
            l.raw("null");
        }
    }
    l.key("needs_files");
    l.ch('[');
    for (size_t i = 0; i < v.files.size(); ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.raw("{\"name\":");
        l.string(v.files[i].name.c_str(), SIZE_MAX);
        l.raw(",\"sha256\":");
        l.string(v.files[i].sha256.c_str(), SIZE_MAX);
        l.ch('}');
    }
    l.ch(']');
    l.key("notes");
    l.ch('[');
    for (size_t i = 0; i < v.notes.size(); ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.string(v.notes[i].c_str(), SIZE_MAX);
    }
    l.ch(']');
    l.key("words");
    if (v.read) {
        l.raw("null");
    } else {
        l.string(layout_words(v), SIZE_MAX);
    }
    l.ch('}');
}

} // namespace layout
} // namespace cli

#endif
