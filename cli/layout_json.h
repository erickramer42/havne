// File/layer: cli/layout_json.h | havne-cli, a layout file's JSON reader (ADR-0057's appendix on the layout file)
// Testing: UNIT: CliLayoutTest (test_cli), the shared corpus tests/data/layouts

#ifndef HAVNE_CLI_LAYOUT_JSON_H
#define HAVNE_CLI_LAYOUT_JSON_H

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace cli {
namespace layout {

// V4's JSON.parse refuses a 1025th level of objects and arrays (Qt 6.12)
constexpr int kMaxDepth = 1024;

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
        if (m_i >= m_s.size()) {
            return false;
        }
        const char c = m_s[m_i];
        if ((c == '{' || c == '[') && depth >= kMaxDepth) {
            return false;
        }
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

inline bool is_integer(const Json* v)
{
    return v != nullptr && v->kind == Json::Num && std::isfinite(v->num) &&
           std::floor(v->num) == v->num;
}

} // namespace layout
} // namespace cli

#endif
