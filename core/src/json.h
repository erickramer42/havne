// File/layer: core/src/json.h | internal, C++17, Qt-free; ALL platforms
// Testing: UNIT: JsonTest (test_core): every accepted form, each node's
//          line and column, every refusal with its position and words,
//          the bounds at their edges, numbers under a comma locale; what
//          write writes read back the same (JsonTest.Written*, .What*,
//          .ANumber*); FUZZ: fuzz_json
//
// A strict JSON reader (RFC 8259) for the files core reads: signal files
// (ADR-0051 section 1) and what their loader and importers build on. Not
// public ABI. UTF-8 bytes in, a small DOM out: every value keeps the line
// and column it starts at, and every object member its key's, so a reader
// of the DOM can say "line 12, column 9: unknown key 'scael'". Objects
// keep their members in file order.
//
// It refuses what RFC 8259 leaves out and what a lenient reader lets
// through: duplicate keys, comments, trailing commas, NaN and Infinity,
// leading zeros and a leading '+', single-quoted strings, raw control
// characters in a string, invalid UTF-8 (overlong forms, encoded
// surrogates, past U+10FFFF, cut sequences), lone surrogates in \u
// escapes, a byte order mark, text after the value, and input past its
// bounds. A refusal is a value (line, column, words), never an exception:
// parse() is noexcept, an allocation failure included.
//
// A byte order mark is refused: RFC 8259 section 8.1 says a JSON text
// carries none, and Python's json refuses one, so a file that loads here
// loads in the tools people check their files with. The words say how to
// save the file without it.
//
// Lines count from 1, a "\n", a "\r\n" or a lone "\r" ending one; columns
// count from 1 in characters (code points), a tab as one.

#ifndef ECUIO_JSON_H
#define ECUIO_JSON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ecuio_json {

/* The input bound: five times the largest OBDb signal set measured
 * (about 200 KB, 2026-11-05) and far past what a hand-written signal file
 * holds. It also bounds the DOM: a value takes at least two bytes of
 * input ("0,"), so a 1 MiB input makes at most half a million values. */
constexpr size_t kMaxBytes = size_t{1} << 20;

/* The nesting bound: arrays and objects inside one another. A signal file
 * nests 4 deep and an OBDb signal set 8; parsing recurses once a level,
 * so the bound also bounds the stack a parse uses, on any thread. */
constexpr unsigned kMaxDepth = 32;

struct Limits {
    size_t max_bytes = kMaxBytes;
    unsigned max_depth = kMaxDepth;
};

enum class Type : uint8_t { null, boolean, number, string, array, object };

struct Member;

struct Value {
    Type type = Type::null;
    bool boolean = false;  /* Type::boolean */
    /* Type::number: the text had no fraction and no exponent ("12", not
     * "12.0" or "1e1") */
    bool integer = false;
    /* integer, and within int64_t: `whole` is the text's exact value */
    bool in_int64 = false;
    uint32_t line = 0;   /* where the value's first character is, from 1 */
    uint32_t column = 0;
    /* Type::number, read without the locale: exact for an integer in
     * int64_t, and for up to 15 significant digits with a power of ten
     * within 22 (recording_rows.h's parse_number), close otherwise.
     * Always finite: a number past the double range is refused. */
    double number = 0;
    int64_t whole = 0;
    std::string text;              /* Type::string: unescaped, valid UTF-8 */
    std::vector<Value> items;      /* Type::array */
    std::vector<Member> members;   /* Type::object, in file order */

    /* The member named key, or nullptr: a linear search, so for the small
     * objects a schema reads */
    const Value* find(std::string_view key) const;
};

struct Member {
    std::string key;     /* unescaped, valid UTF-8, unique in its object */
    uint32_t line = 0;   /* where the key's opening quote is */
    uint32_t column = 0;
    Value value;
};

struct Error {
    /* Where the problem is, from 1; both 0 when it is the input as a whole
     * (too long, or no input at all) */
    uint32_t line = 0;
    uint32_t column = 0;
    std::string words;   /* one line, never the input's bytes but quoted() */
    bool out_of_memory = false; /* the parse ran out of memory */

    /* "line 12, column 9: words", or the words alone at line 0 */
    std::string to_string() const;
};

/*
 * Reads [data, data + size) as one JSON value. True with *out set; false
 * with *error set and *out reset to null. data may be NULL only with a
 * size of 0.
 */
bool parse(const char* data, size_t size, Value* out, Error* error,
           const Limits& limits = Limits()) noexcept;

/*
 * v as JSON text, the reader's inverse: objects and arrays indented two
 * spaces a level, members in their order, each line ending "\n" (the
 * last too). A string is escaped where JSON must (the quote, the
 * backslash, control characters, "\u0000" kept) and its UTF-8 is kept
 * as is; a number with in_int64 is its whole value ("-0" kept), a
 * whole number past int64 its double's digits, any other the shortest
 * text (number_text) that parse_number reads back to it, with ".0" (or,
 * past 14 digits, "e0") after one that has no fraction. A number keeps
 * its kind (integer, in_int64) and its sign, and its value whenever 15
 * significant digits hold it: past that, the reader is close, not exact.
 * Throws std::bad_alloc only.
 */
std::string write(const Value& v);

/* A finite double as the shortest of 15, 16 or 17 significant digits
 * that recording_rows.h's parse_number reads back to it (17 when none
 * does), with a '.' whatever the locale */
std::string number_text(double v);

/* Text from the input shown in words: single-quoted, control characters
 * as '?', cut to 32 characters with "..." after. For keys and strings
 * (valid UTF-8), so a message stays one line of reasonable length. */
std::string quoted(std::string_view text);

}  // namespace ecuio_json

#endif
