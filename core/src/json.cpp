// File/layer: core/src/json.cpp | internal, C++17, Qt-free; ALL platforms
// Testing: UNIT: JsonTest (test_core); FUZZ: fuzz_json
//
// The strict JSON reader of json.h: one recursive-descent pass over the
// bytes, the depth bounded so the recursion is, every value placed where
// it ends up (no subtree copied). Numbers go through recording_rows.h's
// parse_number, the reader the recordings use, so neither reads the
// locale.

#include "json.h"

#include "recording_rows.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <utility>

namespace ecuio_json {

namespace {

/* Past this many members an object's keys go into a set for the
 * duplicate check, so a large object costs n log n, not n squared */
constexpr size_t kLinearKeys = 32;

/* The length of the valid UTF-8 sequence at [p, end), or 0 (Unicode 15,
 * table 3-7: no overlong forms, no surrogates, nothing past U+10FFFF) */
size_t utf8_length(const unsigned char* p, const unsigned char* end)
{
    const unsigned char b = p[0];
    if (b < 0x80) {
        return 1;
    }
    size_t n = 0;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        n = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        n = 3;
        if (b == 0xE0) {
            lo = 0xA0;
        } else if (b == 0xED) {
            hi = 0x9F;
        }
    } else if (b >= 0xF0 && b <= 0xF4) {
        n = 4;
        if (b == 0xF0) {
            lo = 0x90;
        } else if (b == 0xF4) {
            hi = 0x8F;
        }
    } else {
        return 0;
    }
    if (static_cast<size_t>(end - p) < n || p[1] < lo || p[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < n; ++i) {
        if (p[i] < 0x80 || p[i] > 0xBF) {
            return 0;
        }
    }
    return n;
}

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

struct Pos {
    uint32_t line;
    uint32_t column;
};

class Parser {
public:
    Parser(const unsigned char* p, const unsigned char* end,
           const Limits& limits, Error* error)
        : p_(p), end_(end), limits_(limits), error_(error)
    {
    }

    bool document(Value* out)
    {
        if (end_ - p_ >= 3 && p_[0] == 0xEF && p_[1] == 0xBB &&
            p_[2] == 0xBF) {
            return fail(here(),
                        "the file starts with a byte order mark: save it as "
                        "UTF-8 without one");
        }
        skip_space();
        if (p_ == end_) {
            return fail(here(), "expected a value, found the end of the file");
        }
        if (!value(out)) {
            return false;
        }
        skip_space();
        if (p_ != end_) {
            return fail(here(), "text after the end of the value: " +
                                    describe_here());
        }
        return true;
    }

private:
    const unsigned char* p_;
    const unsigned char* end_;
    const Limits& limits_;
    Error* error_;
    uint32_t line_ = 1;
    /* characters (non-continuation bytes) consumed on this line */
    uint32_t chars_ = 0;
    unsigned depth_ = 0;

    Pos here() const { return {line_, chars_ + 1}; }

    bool fail(Pos at, std::string words)
    {
        error_->line = at.line;
        error_->column = at.column;
        error_->words = std::move(words);
        return false;
    }

    /* Consumes one byte of a line (never a line break) */
    void bump()
    {
        if ((*p_ & 0xC0) != 0x80) {
            ++chars_;
        }
        ++p_;
    }

    void skip_space()
    {
        while (p_ < end_) {
            const unsigned char c = *p_;
            if (c == ' ' || c == '\t') {
                ++chars_;
                ++p_;
            } else if (c == '\n' || c == '\r') {
                ++p_;
                if (c == '\r' && p_ < end_ && *p_ == '\n') {
                    ++p_;
                }
                ++line_;
                chars_ = 0;
            } else {
                return;
            }
        }
    }

    /* The character at p_ in words, for "unexpected ..." messages */
    std::string describe_here() const
    {
        if (p_ == end_) {
            return "the end of the file";
        }
        const unsigned char c = *p_;
        char buf[48];
        if (c >= 0x20 && c < 0x7F) {
            std::snprintf(buf, sizeof buf, "'%c'", static_cast<char>(c));
        } else if (c < 0x80) {
            std::snprintf(buf, sizeof buf, "the control character 0x%02X",
                          static_cast<unsigned>(c));
        } else {
            const size_t n = utf8_length(p_, end_);
            if (n == 0) {
                std::snprintf(buf, sizeof buf, "the byte 0x%02X, not UTF-8",
                              static_cast<unsigned>(c));
            } else {
                auto cp = static_cast<uint32_t>(c & (0x7F >> n));
                for (size_t i = 1; i < n; ++i) {
                    cp = (cp << 6) | (p_[i] & 0x3F);
                }
                std::snprintf(buf, sizeof buf, "the character U+%04X",
                              static_cast<unsigned>(cp));
            }
        }
        return buf;
    }

    bool literal(const char* word, size_t n)
    {
        if (static_cast<size_t>(end_ - p_) < n ||
            std::memcmp(p_, word, n) != 0) {
            return false;
        }
        p_ += n;
        chars_ += static_cast<uint32_t>(n);
        return true;
    }

    bool value(Value* out)
    {
        const Pos at = here();
        out->line = at.line;
        out->column = at.column;
        const unsigned char c = *p_;
        switch (c) {
        case '{':
            return object(out, at);
        case '[':
            return array(out, at);
        case '"':
            out->type = Type::string;
            return string(&out->text);
        case 't':
            if (literal("true", 4)) {
                out->type = Type::boolean;
                out->boolean = true;
                return true;
            }
            break;
        case 'f':
            if (literal("false", 5)) {
                out->type = Type::boolean;
                return true;
            }
            break;
        case 'n':
            if (literal("null", 4)) {
                out->type = Type::null;
                return true;
            }
            break;
        case 'N':
        case 'I':
            return fail(at, "NaN and Infinity are not JSON numbers");
        case '/':
            return fail(at, "a comment: JSON has none");
        case '\'':
            return fail(at, "a single quote: JSON strings take double quotes");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return number(out, at);
            }
            break;
        }
        return fail(at, "expected a value, found " + describe_here());
    }

    bool enter(Pos at)
    {
        if (++depth_ > limits_.max_depth) {
            return fail(at, "nested deeper than " +
                                std::to_string(limits_.max_depth) +
                                " arrays and objects");
        }
        return true;
    }

    /* Whether p_ is at the closing bracket */
    bool closes(unsigned char close)
    {
        return p_ < end_ && *p_ == close;
    }

    bool array(Value* out, Pos at)
    {
        out->type = Type::array;
        if (!enter(at)) {
            return false;
        }
        bump();
        skip_space();
        if (closes(']')) {
            bump();
            --depth_;
            return true;
        }
        for (;;) {
            if (p_ == end_) {
                return ends_inside("an array", at);
            }
            if (closes(']')) {
                return fail(here(), "a trailing comma before ']'");
            }
            out->items.emplace_back();
            if (!value(&out->items.back())) {
                return false;
            }
            skip_space();
            if (p_ == end_) {
                return ends_inside("an array", at);
            }
            if (*p_ == ',') {
                bump();
                skip_space();
                continue;
            }
            if (*p_ == ']') {
                bump();
                --depth_;
                return true;
            }
            return fail(here(),
                        "expected ',' or ']', found " + describe_here());
        }
    }

    bool object(Value* out, Pos at)
    {
        out->type = Type::object;
        if (!enter(at)) {
            return false;
        }
        bump();
        skip_space();
        if (closes('}')) {
            bump();
            --depth_;
            return true;
        }
        std::unique_ptr<std::unordered_set<std::string>> keys;
        for (;;) {
            if (p_ == end_) {
                return ends_inside("an object", at);
            }
            if (closes('}')) {
                return fail(here(), "a trailing comma before '}'");
            }
            const Pos key_at = here();
            if (*p_ != '"') {
                if (*p_ == '\'') {
                    return fail(key_at, "a single quote: JSON strings take "
                                        "double quotes");
                }
                if (*p_ == '/') {
                    return fail(key_at, "a comment: JSON has none");
                }
                return fail(key_at, "expected a key in double quotes, found " +
                                        describe_here());
            }
            std::string key;
            if (!string(&key)) {
                return false;
            }
            if (duplicate(*out, keys, key)) {
                return fail(key_at, "duplicate key " + quoted(key));
            }
            skip_space();
            if (p_ == end_) {
                return ends_inside("an object", at);
            }
            if (*p_ != ':') {
                return fail(here(), "expected ':' after the key, found " +
                                        describe_here());
            }
            bump();
            skip_space();
            if (p_ == end_) {
                return ends_inside("an object", at);
            }
            out->members.emplace_back();
            Member& m = out->members.back();
            m.key = std::move(key);
            m.line = key_at.line;
            m.column = key_at.column;
            if (!value(&m.value)) {
                return false;
            }
            skip_space();
            if (p_ == end_) {
                return ends_inside("an object", at);
            }
            if (*p_ == ',') {
                bump();
                skip_space();
                continue;
            }
            if (*p_ == '}') {
                bump();
                --depth_;
                return true;
            }
            return fail(here(),
                        "expected ',' or '}', found " + describe_here());
        }
    }

    /* Whether key is already among the object's members; past kLinearKeys
     * members, through the set, which then holds every key */
    static bool duplicate(const Value& obj,
                          std::unique_ptr<std::unordered_set<std::string>>& keys,
                          const std::string& key)
    {
        const size_t n = obj.members.size();
        if (n < kLinearKeys) {
            for (const Member& m : obj.members) {
                if (m.key == key) {
                    return true;
                }
            }
            return false;
        }
        if (!keys) {
            keys = std::make_unique<std::unordered_set<std::string>>();
            keys->reserve(n * 2);
            for (const Member& m : obj.members) {
                keys->insert(m.key);
            }
        }
        return !keys->insert(key).second;
    }

    bool ends_inside(const char* what, Pos at)
    {
        return fail(here(), std::string("the file ends inside ") + what +
                                " opened at line " + std::to_string(at.line) +
                                ", column " + std::to_string(at.column));
    }

    /* A string at its opening quote, unescaped into *out */
    bool string(std::string* out)
    {
        const Pos open = here();
        bump();
        for (;;) {
            if (p_ == end_) {
                return fail(here(),
                            "the file ends inside a string opened at line " +
                                std::to_string(open.line) + ", column " +
                                std::to_string(open.column));
            }
            const unsigned char c = *p_;
            if (c == '"') {
                bump();
                return true;
            }
            if (c == '\\') {
                if (!escape(out)) {
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                return fail(here(), describe_here() +
                                        " in a string: write it as an escape "
                                        "(\\n, \\t, \\u0001)");
            }
            if (c < 0x80) {
                out->push_back(static_cast<char>(c));
                bump();
                continue;
            }
            const size_t n = utf8_length(p_, end_);
            if (n == 0) {
                return fail(here(), "invalid UTF-8: " + describe_here());
            }
            out->append(reinterpret_cast<const char*>(p_), n);
            for (size_t i = 0; i < n; ++i) {
                bump();
            }
        }
    }

    /* Four hex digits after a "\u" at p_ (the backslash), or -1 */
    long u_escape() const
    {
        if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') {
            return -1;
        }
        long v = 0;
        for (int i = 2; i < 6; ++i) {
            const int d = hex_digit(p_[i]);
            if (d < 0) {
                return -1;
            }
            v = v * 16 + d;
        }
        return v;
    }

    bool escape(std::string* out)
    {
        const Pos at = here();
        if (end_ - p_ < 2) {
            bump();
            return fail(here(), "the file ends inside an escape");
        }
        const unsigned char e = p_[1];
        char plain = 0;
        switch (e) {
        case '"': plain = '"'; break;
        case '\\': plain = '\\'; break;
        case '/': plain = '/'; break;
        case 'b': plain = '\b'; break;
        case 'f': plain = '\f'; break;
        case 'n': plain = '\n'; break;
        case 'r': plain = '\r'; break;
        case 't': plain = '\t'; break;
        case 'u': break;
        default: {
            const std::string shown =
                e >= 0x21 && e < 0x7F ? std::string(1, static_cast<char>(e))
                                      : std::string("?");
            return fail(at, "an unknown escape: \\" + shown);
        }
        }
        if (plain != 0) {
            out->push_back(plain);
            p_ += 2;
            chars_ += 2;
            return true;
        }
        const long u = u_escape();
        if (u < 0) {
            return fail(at, "\\u takes four hex digits");
        }
        p_ += 6;
        chars_ += 6;
        uint32_t cp = static_cast<uint32_t>(u);
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail(at, "a lone surrogate in a \\u escape");
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            const long low = u_escape();
            if (low < 0xDC00 || low > 0xDFFF) {
                return fail(at, "a lone surrogate in a \\u escape");
            }
            p_ += 6;
            chars_ += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) +
                 (static_cast<uint32_t>(low) - 0xDC00);
        }
        append_utf8(*out, cp);
        return true;
    }

    bool digits(const char* after)
    {
        if (p_ == end_ || *p_ < '0' || *p_ > '9') {
            return fail(here(), std::string("expected a digit ") + after +
                                    ", found " + describe_here());
        }
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
            bump();
        }
        return true;
    }

    bool number(Value* out, Pos at)
    {
        const unsigned char* start = p_;
        const bool negative = *p_ == '-';
        if (negative) {
            bump();
            if (p_ < end_ && *p_ == 'I') {
                return fail(at, "NaN and Infinity are not JSON numbers");
            }
        }
        const unsigned char* int_start = p_;
        if (p_ < end_ && *p_ == '0') {
            bump();
            if (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
                return fail(at, "a number with a leading zero");
            }
        } else if (!digits("after '-'")) {
            return false;
        }
        const unsigned char* int_end = p_;
        bool integer = true;
        if (p_ < end_ && *p_ == '.') {
            integer = false;
            bump();
            if (!digits("after '.'")) {
                return false;
            }
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            integer = false;
            bump();
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) {
                bump();
            }
            if (!digits("in the exponent")) {
                return false;
            }
        }
        out->type = Type::number;
        out->integer = integer;
        if (integer) {
            /* The magnitude, up to 2^63 (INT64_MIN's) */
            constexpr uint64_t kMax = uint64_t{1} << 63;
            uint64_t v = 0;
            bool fits = true;
            for (const unsigned char* q = int_start; q < int_end; ++q) {
                const auto d = static_cast<uint64_t>(*q - '0');
                if (v > (kMax - d) / 10) {
                    fits = false;
                    break;
                }
                v = v * 10 + d;
            }
            if (fits && (negative || v < kMax)) {
                out->in_int64 = true;
                out->whole = negative ? static_cast<int64_t>(0 - v)
                                      : static_cast<int64_t>(v);
                out->number = static_cast<double>(out->whole);
                if (negative && v == 0) {
                    out->number = -0.0;
                }
                return true;
            }
        }
        double d = 0;
        const char* text = reinterpret_cast<const char*>(start);
        const size_t len = static_cast<size_t>(p_ - start);
        if (!ecuio_recording_rows::parse_number(text, len, &d)) {
            return fail(at, "a number that can't be read");
        }
        if (!std::isfinite(d)) {
            return fail(at, "a number past the range of a double");
        }
        out->number = d;
        return true;
    }
};

}  // namespace

const Value* Value::find(std::string_view key) const
{
    for (const Member& m : members) {
        if (m.key == key) {
            return &m.value;
        }
    }
    return nullptr;
}

std::string number_text(double v)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe): reads the decimal point only
    const char* point = std::localeconv()->decimal_point;
    const char p = (point != nullptr && point[0] != '\0') ? point[0] : '.';
    std::string best;
    for (int digits = 15; digits <= 17; ++digits) {
        char buf[40];
        const int n = std::snprintf(buf, sizeof buf, "%.*g", digits, v);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof buf) {
            continue;
        }
        best.assign(buf, static_cast<size_t>(n));
        for (char& c : best) {
            if (c == p) {
                c = '.';
            }
        }
        double back = 0;
        if (ecuio_recording_rows::parse_number(best.data(), best.size(),
                                               &back) &&
            back == v) {
            break;
        }
    }
    return best;
}

namespace {

void write_string(std::string& out, const std::string& s)
{
    static const char digits[] = "0123456789abcdef";
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += digits[c >> 4];
                out += digits[c & 0xF];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

void write_value(std::string& out, const Value& v, size_t depth)
{
    const std::string indent(2 * (depth + 1), ' ');
    const std::string close(2 * depth, ' ');
    switch (v.type) {
    case Type::null:
        out += "null";
        break;
    case Type::boolean:
        out += v.boolean ? "true" : "false";
        break;
    case Type::number:
        if (v.in_int64) {
            /* "-0" is a whole number whose double keeps the sign */
            if (v.whole == 0 && std::signbit(v.number)) {
                out += '-';
            }
            out += std::to_string(v.whole);
        } else if (v.integer) {
            /* Past int64: its double's digits, still a whole number */
            char buf[400];
            const int n = std::snprintf(buf, sizeof buf, "%.0f", v.number);
            out.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
        } else {
            /* A number written with a fraction or an exponent keeps
             * one, so it reads back as no integer and -0.0 as -0.0: ".0"
             * within the 15 digits the reader reads exactly, "e0" past
             * them, which adds no digit */
            const std::string n = number_text(v.number);
            out += n;
            if (!v.integer && n.find_first_of(".e") == std::string::npos) {
                const size_t digits = n.size() - (n[0] == '-' ? 1 : 0);
                out += digits < 15 ? ".0" : "e0";
            }
        }
        break;
    case Type::string:
        write_string(out, v.text);
        break;
    case Type::array:
        if (v.items.empty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (size_t i = 0; i < v.items.size(); ++i) {
            out += indent;
            write_value(out, v.items[i], depth + 1);
            out += i + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += close + "]";
        break;
    case Type::object:
        if (v.members.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (size_t i = 0; i < v.members.size(); ++i) {
            out += indent;
            write_string(out, v.members[i].key);
            out += ": ";
            write_value(out, v.members[i].value, depth + 1);
            out += i + 1 < v.members.size() ? ",\n" : "\n";
        }
        out += close + "}";
        break;
    }
}

} // namespace

std::string write(const Value& v)
{
    std::string out;
    write_value(out, v, 0);
    out += '\n';
    return out;
}

std::string Error::to_string() const
{
    if (line == 0) {
        return words;
    }
    return "line " + std::to_string(line) + ", column " +
           std::to_string(column) + ": " + words;
}

size_t invisible_length(const char* p, size_t n)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    if (n >= 2 && u[0] == 0xC2 &&
        ((u[1] >= 0x80 && u[1] <= 0x9F) || u[1] == 0xAD)) {
        return 2; /* U+0080 to U+009F, U+00AD */
    }
    if (n >= 2 && u[0] == 0xD8 && u[1] == 0x9C) {
        return 2; /* U+061C */
    }
    if (n >= 3 && u[0] == 0xE2) {
        if (u[1] == 0x80 && ((u[2] >= 0x8B && u[2] <= 0x8F) ||
                             (u[2] >= 0xA8 && u[2] <= 0xAE))) {
            return 3; /* U+200B to U+200F, U+2028 to U+202E */
        }
        if (u[1] == 0x81 && ((u[2] >= 0xA0 && u[2] <= 0xA4) ||
                             (u[2] >= 0xA6 && u[2] <= 0xA9))) {
            return 3; /* U+2060 to U+2064, U+2066 to U+2069 */
        }
    }
    if (n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
        return 3; /* U+FEFF */
    }
    return 0;
}

std::string quoted(std::string_view text)
{
    constexpr size_t kChars = 32;
    std::string out = "'";
    size_t chars = 0;
    for (size_t i = 0; i < text.size();) {
        if (chars == kChars) {
            out += "...";
            break;
        }
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t hidden =
            invisible_length(text.data() + i, text.size() - i);
        if (c < 0x20 || c == 0x7F) {
            out += '?';
            ++i;
        } else if (hidden > 0) {
            out += '?';
            i += hidden;
        } else if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else {
            const auto* p = reinterpret_cast<const unsigned char*>(text.data());
            const size_t n = utf8_length(p + i, p + text.size());
            if (n == 0) {
                out += '?';
                ++i;
            } else {
                out.append(text.data() + i, n);
                i += n;
            }
        }
        ++chars;
    }
    out += '\'';
    return out;
}

/* The only throw the check sees is the handler's string assignment, which
 * fits the short-string buffer and allocates nothing */
// NOLINTNEXTLINE(bugprone-exception-escape)
bool parse(const char* data, size_t size, Value* out, Error* error,
           const Limits& limits) noexcept
{
    if (out == nullptr || error == nullptr) {
        return false;
    }
    try {
        *out = Value();
        *error = Error();
        if (data == nullptr && size != 0) {
            error->words = "no input";
            return false;
        }
        if (size > limits.max_bytes) {
            error->words = "the input is " + std::to_string(size) +
                           " bytes, past the limit of " +
                           std::to_string(limits.max_bytes);
            return false;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(data);
        Parser parser(p, p + size, limits, error);
        if (parser.document(out)) {
            return true;
        }
        *out = Value();
        return false;
    } catch (...) {
        /* bad_alloc: a refusal like any other. Clearing frees what the
         * parse held and allocates nothing, and the words fit every
         * standard library's short-string buffer, so nothing here
         * throws. */
        *out = Value();
        error->line = 0;
        error->column = 0;
        error->words = "out of memory";
        error->out_of_memory = true;
        return false;
    }
}

}  // namespace ecuio_json
