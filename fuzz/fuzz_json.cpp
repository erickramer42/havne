// File/layer: fuzz/fuzz_json.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_json

#include "json.h"
#include "recording_rows.h" /* parse_number: what number_text checks */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cmath>
#include <set>
#include <string>

namespace {

using ecuio_json::Error;
using ecuio_json::Limits;
using ecuio_json::Type;
using ecuio_json::Value;

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The harness's own UTF-8 check, written apart from the reader's */
bool valid_utf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t n = 0;
        if (b < 0x80) {
            ++i;
            continue;
        }
        if ((b & 0xE0) == 0xC0) {
            n = 2;
            cp = b & 0x1Fu;
        } else if ((b & 0xF0) == 0xE0) {
            n = 3;
            cp = b & 0x0Fu;
        } else if ((b & 0xF8) == 0xF0) {
            n = 4;
            cp = b & 0x07u;
        } else {
            return false;
        }
        if (s.size() - i < n) {
            return false;
        }
        for (size_t k = 1; k < n; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        const uint32_t least = n == 2 ? 0x80 : n == 3 ? 0x800 : 0x10000;
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n;
    }
    return true;
}

struct Walk {
    unsigned max_depth;
    uint32_t last_line = 0;
    uint32_t last_column = 0;
    /* a number near the double's limit, which a close re-read of its
     * 17 digits may carry past it */
    bool huge = false;

    /* Positions strictly after the last one seen */
    void advance(uint32_t line, uint32_t column)
    {
        check(line >= 1 && column >= 1);
        check(line > last_line ||
              (line == last_line && column > last_column));
        last_line = line;
        last_column = column;
    }

    void value(const Value& v, unsigned depth)
    {
        advance(v.line, v.column);
        switch (v.type) {
        case Type::null:
        case Type::boolean:
            break;
        case Type::number:
            check(std::isfinite(v.number));
            check(!v.in_int64 || v.integer);
            check(!v.in_int64 ||
                  v.number == static_cast<double>(v.whole));
            huge = huge || std::fabs(v.number) > 1e300;
            break;
        case Type::string:
            check(valid_utf8(v.text));
            break;
        case Type::array:
            check(depth + 1 <= max_depth);
            check(v.members.empty());
            for (const Value& item : v.items) {
                value(item, depth + 1);
            }
            break;
        case Type::object: {
            check(depth + 1 <= max_depth);
            check(v.items.empty());
            std::set<std::string> keys;
            for (const auto& m : v.members) {
                check(valid_utf8(m.key));
                check(keys.insert(m.key).second);
                advance(m.line, m.column);
                value(m.value, depth + 1);
            }
            break;
        }
        }
    }
};

void write_string(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (b < 0x20) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", b);
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v)
{
    char buf[40];
    switch (v.type) {
    case Type::null:
        out += "null";
        break;
    case Type::boolean:
        out += v.boolean ? "true" : "false";
        break;
    case Type::number:
        if (v.in_int64) {
            snprintf(buf, sizeof buf, "%s%lld",
                     v.whole == 0 && std::signbit(v.number) ? "-" : "",
                     static_cast<long long>(v.whole));
        } else {
            snprintf(buf, sizeof buf, "%.17g", v.number);
        }
        out += buf;
        break;
    case Type::string:
        write_string(out, v.text);
        break;
    case Type::array:
        out += '[';
        for (size_t i = 0; i < v.items.size(); ++i) {
            out += i == 0 ? "" : ",";
            write(out, v.items[i]);
        }
        out += ']';
        break;
    case Type::object:
        out += '{';
        for (size_t i = 0; i < v.members.size(); ++i) {
            out += i == 0 ? "" : ",";
            write_string(out, v.members[i].key);
            out += ':';
            write(out, v.members[i].value);
        }
        out += '}';
        break;
    }
}

/* The same document; with positions when both came from one text */
bool same(const Value& a, const Value& b, bool positions)
{
    if (a.type != b.type ||
        (positions && (a.line != b.line || a.column != b.column))) {
        return false;
    }
    switch (a.type) {
    case Type::null:
        return true;
    case Type::boolean:
        return a.boolean == b.boolean;
    case Type::number:
        if (positions) {
            return a.integer == b.integer && a.in_int64 == b.in_int64 &&
                   a.whole == b.whole && a.number == b.number &&
                   std::signbit(a.number) == std::signbit(b.number);
        }
        if (a.in_int64) {
            return b.in_int64 && a.whole == b.whole &&
                   std::signbit(a.number) == std::signbit(b.number);
        }
        /* written with 17 digits and read back: close, not exact
         * (json.h's number); a subnormal keeps no relative precision */
        return std::fabs(a.number - b.number) <=
                   std::fabs(a.number) * 1e-13 ||
               std::fabs(a.number) < 1e-290;
    case Type::string:
        return a.text == b.text;
    case Type::array:
        if (a.items.size() != b.items.size()) {
            return false;
        }
        for (size_t i = 0; i < a.items.size(); ++i) {
            if (!same(a.items[i], b.items[i], positions)) {
                return false;
            }
        }
        return true;
    case Type::object:
        if (a.members.size() != b.members.size()) {
            return false;
        }
        for (size_t i = 0; i < a.members.size(); ++i) {
            const auto& x = a.members[i];
            const auto& y = b.members[i];
            if (x.key != y.key ||
                (positions && (x.line != y.line || x.column != y.column)) ||
                !same(x.value, y.value, positions)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/* A number 15 significant digits hold: the reader reads those exactly */
bool fifteen_digits(double x)
{
    char buf[40];
    const int n = snprintf(buf, sizeof buf, "%.15g", x);
    double read = 0;
    return n > 0 && static_cast<size_t>(n) < sizeof buf &&
           ecuio_recording_rows::parse_number(buf, static_cast<size_t>(n),
                                              &read) &&
           read == x;
}

/* core's writer kept each number's kind and sign, and its value where 15
 * digits hold it */
bool same_kind(const Value& a, const Value& b)
{
    if (a.type != b.type) {
        return false;
    }
    if (a.type == Type::number) {
        if (a.integer != b.integer || a.in_int64 != b.in_int64 ||
            std::signbit(a.number) != std::signbit(b.number)) {
            return false;
        }
        const bool exact =
            a.in_int64 || (!a.integer && fifteen_digits(a.number));
        return !exact || a.number == b.number;
    }
    if (a.items.size() != b.items.size() ||
        a.members.size() != b.members.size()) {
        return false;
    }
    for (size_t i = 0; i < a.items.size(); ++i) {
        if (!same_kind(a.items[i], b.items[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < a.members.size(); ++i) {
        if (!same_kind(a.members[i].value, b.members[i].value)) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    /* The first byte: depth 1 to 64 (the default, 32, among them), and
     * with its top bit a size bound at half the text */
    Limits limits;
    limits.max_depth = 1u + (data[0] & 0x3Fu);
    const char* text = reinterpret_cast<const char*>(data + 1);
    const size_t n = size - 1;
    if ((data[0] & 0x80) != 0) {
        limits.max_bytes = n / 2;
    }

    Value v;
    Error e;
    const bool ok = ecuio_json::parse(text, n, &v, &e, limits);
    Value again;
    Error e2;
    check(ecuio_json::parse(text, n, &again, &e2, limits) == ok);

    if (!ok) {
        check(v.type == Type::null && v.items.empty() && v.members.empty());
        check(!e.words.empty() && e.words.find('\n') == std::string::npos);
        check((e.line == 0) == (e.column == 0));
        check(e.line != 0 || n > limits.max_bytes);
        uint32_t lines = 1;
        for (size_t i = 0; i < n; ++i) {
            if (text[i] == '\n' ||
                (text[i] == '\r' && (i + 1 == n || text[i + 1] != '\n'))) {
                ++lines;
            }
        }
        check(e.line <= lines);
        check(e.line == e2.line && e.column == e2.column &&
              e.words == e2.words);
        return 0;
    }

    check(n <= limits.max_bytes);
    check(e.line == 0 && e.words.empty());
    check(same(v, again, true));
    Walk walk{limits.max_depth};
    walk.value(v, 0);
    if (walk.huge) {
        return 0;
    }

    /* Written back out (minified, so never deeper or longer than the
     * input's bounds allow) and read again: the same document */
    std::string out;
    write(out, v);
    Limits roomy;
    roomy.max_depth = limits.max_depth;
    roomy.max_bytes = out.size();
    Value back;
    Error eb;
    check(ecuio_json::parse(out.data(), out.size(), &back, &eb, roomy));
    check(same(v, back, false));

    /* core's writer: indented, so longer than the input, never deeper */
    const std::string pretty = ecuio_json::write(v);
    roomy.max_bytes = pretty.size();
    Value written;
    check(
        ecuio_json::parse(pretty.data(), pretty.size(), &written, &eb, roomy));
    check(same(v, written, false));
    check(same_kind(v, written));
    return 0;
}
