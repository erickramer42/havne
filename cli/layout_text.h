// File/layer: cli/layout_text.h | havne-cli, JavaScript's text rules for a layout file's sizes and labels (ADR-0057's appendix on the layout file)
// Testing: UNIT: CliLayoutTest (test_cli), the shared corpus tests/data/layouts

#ifndef HAVNE_CLI_LAYOUT_TEXT_H
#define HAVNE_CLI_LAYOUT_TEXT_H

#include "layout_json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cli {
namespace layout {

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

// A string's UTF-8 as the app stores what JSON.stringify writes: V4 writes a
// lone surrogate unescaped, which DashboardBook's utf8Bytes counts as 3 bytes
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
            put_utf8(out, 0xFFFD);
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

// V4's String.prototype.trim: ECMAScript's white space and line terminators,
// and U+0085, which V4 takes too (Qt 6.12)
inline bool js_space(char16_t c)
{
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
           c == 0xFEFF;
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

} // namespace layout
} // namespace cli

#endif
