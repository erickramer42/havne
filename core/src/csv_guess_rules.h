// File/layer: core/src/csv_guess_rules.h | internal, not ABI-exported; included by csv_guess.h alone
// C++17, Qt-free
// Testing: UNIT: CsvGuessTest (test_core); FUZZ: fuzz_csv_import
//
// The guess's small rules (csv_guess.h): header and cell tests, the JSON
// writer and the header row's search.

#ifndef ECUIO_CSV_GUESS_RULES_H
#define ECUIO_CSV_GUESS_RULES_H

#include "csv_import.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ecuio_map_guess {

namespace csv = ecuio_csv_import;

/* The header's search: the first 1000 rows (the map's header_row bound)
 * within the first 1 MiB; the sample: 200 rows or 64 KiB from the
 * header, whichever ends first */
constexpr size_t kSearchBytes = size_t{1} << 20;
constexpr size_t kSampleBytes = size_t{64} << 10;
constexpr uint32_t kSampleRows = 200;
/* A header candidate is judged by the rows right below it, read within
 * this many bytes of it (a quote left open would read on to the end) */
constexpr uint32_t kCheckRows = 5;
constexpr size_t kCheckBytes = size_t{64} << 10;
/* What the caller reads of a file: the search, the sample past it and a
 * row's room. A file longer than this is cut at its last line feed */
constexpr size_t kReadMax = kSearchBytes + kSampleBytes + csv::kRowBytesMax;
/* The notes listed before "...and N more" */
constexpr size_t kNotesMax = 64;

/* result.flags: recording.h's ECUIO_CSV_GUESS_* (import.cpp asserts the
 * values) */
constexpr uint32_t kTimeUnitKnown = 0x1;
constexpr uint32_t kDecimalComma = 0x2;
constexpr uint32_t kPreamble = 0x4;
constexpr uint32_t kTimeNumbers = 0x8;

struct result {
    std::string text;      /* {"map": ..., "columns": [...], "notes": [...]} */
    size_t map_offset = 0; /* the map's value in text, its braces included */
    size_t map_length = 0;
    uint32_t flags = 0;
    uint32_t columns = 0;   /* signals in the map */
    uint32_t left_out = 0;  /* header columns left out */
    uint32_t rows_read = 0; /* the sample's rows */
};

/* ---- small rules ---- */

inline bool blank(char c)
{
    return c == ' ' || c == '\t';
}

inline std::string trim(const std::string& s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && blank(s[a])) {
        ++a;
    }
    while (b > a && blank(s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}

inline std::string lower_ascii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

/* "Name (unit)" or "Name [unit]": the name and the bracket's text, both
 * trimmed; *bracketed whether the header ended in one */
inline void split_header(const std::string& header, std::string* name,
                         std::string* unit, bool* bracketed)
{
    const std::string t = trim(header);
    *bracketed = false;
    unit->clear();
    *name = t;
    if (t.size() < 2 || (t.back() != ')' && t.back() != ']')) {
        return;
    }
    const char open = t.back() == ')' ? '(' : '[';
    const size_t at = t.rfind(open);
    if (at == std::string::npos) {
        return;
    }
    *bracketed = true;
    *unit = trim(t.substr(at + 1, t.size() - at - 2));
    *name = trim(t.substr(0, at));
}

/* A unit in a time column's header: s, ms or us, else 0 */
inline char time_unit_of(const std::string& unit)
{
    const std::string u = lower_ascii(unit);
    if (u == "s" || u == "sec" || u == "secs" || u == "seconds") {
        return 's';
    }
    if (u == "ms" || u == "msec") {
        return 'm';
    }
    /* the micro sign (U+00B5) and the Greek mu (U+03BC) */
    if (u == "us" || u == "usec" || u == "\xC2\xB5s" || u == "\xCE\xBCs") {
        return 'u';
    }
    return 0;
}

/* A cell of digits alone, a sign before them allowed */
inline bool whole_number(const std::string& c)
{
    size_t at = !c.empty() && (c[0] == '-' || c[0] == '+') ? 1 : 0;
    if (at == c.size()) {
        return false;
    }
    for (; at < c.size(); ++at) {
        if (c[at] < '0' || c[at] > '9') {
            return false;
        }
    }
    return true;
}

/* A header named as a time column, before its unit */
inline bool named_as_time(const std::string& name)
{
    const std::string n = lower_ascii(name);
    return n == "time" || n == "timestamp" || n == "t" || n == "elapsed" ||
           n == "time stamp";
}

/* A name or unit the map would refuse as a formula (parse_signal's) */
inline bool formula_start(const std::string& t)
{
    return !t.empty() && (t[0] == '=' || t[0] == '+' || t[0] == '-' ||
                          t[0] == '@' || t[0] == '\t' || t[0] == '\r');
}

/* At most max bytes of s, cut on a character boundary */
inline std::string cut_utf8(const std::string& s, size_t max)
{
    if (s.size() <= max) {
        return s;
    }
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    return s.substr(0, n);
}

inline bool is_number(const std::string& c, char decimal)
{
    double v = 0;
    return csv::parse_cell_number(c, decimal, &v);
}

/* "YYYY-MM-DD hh:mm:ss[.f]" with no zone */
inline bool zoneless_iso(const std::string& c)
{
    int64_t days = 0;
    int64_t us = 0;
    return c.size() >= 19 && (c[10] == 'T' || c[10] == ' ') &&
           csv::parse_date(c.data(), 10, &days) &&
           (csv::parse_clock(c.data() + 11, c.size() - 11, '.', &us) ||
            csv::parse_clock(c.data() + 11, c.size() - 11, ',', &us));
}

inline bool iso_time(const std::string& c, char decimal, int64_t* us)
{
    return csv::parse_iso(c.data(), c.size(), decimal, us);
}

/* A cell that reads as a number or a time in any of the forms the import
 * knows, its decimal mark either (while the delimiter allows a comma) */
inline bool numberish(const std::string& c, char delimiter)
{
    int64_t us = 0;
    return is_number(c, '.') || (delimiter != ',' && is_number(c, ',')) ||
           iso_time(c, '.', &us) || zoneless_iso(c) ||
           csv::parse_clock(c.data(), c.size(), '.', &us);
}

/* Fraction digits of a number cell, at most 3 */
inline uint8_t decimals_of(const std::string& c, char decimal)
{
    const size_t at = c.find(decimal);
    if (at == std::string::npos) {
        return 0;
    }
    uint8_t n = 0;
    for (size_t i = at + 1; i < c.size() && c[i] >= '0' && c[i] <= '9'; ++i) {
        if (n < 3) {
            ++n;
        }
    }
    return n;
}

/* ---- JSON out ---- */

inline void json_string(std::string& o, const std::string& s)
{
    o += '"';
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            o += '\\';
            o += ch;
        } else if (ch == '\n') {
            o += "\\n";
        } else if (ch == '\r') {
            o += "\\r";
        } else if (ch == '\t') {
            o += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            o += buf;
        } else {
            o += ch; /* the reader took only valid UTF-8 */
        }
    }
    o += '"';
}

/* A bounded view of the bytes as a Source */
struct bounded_source {
    const unsigned char* p;
    size_t n;
    size_t at = 0;
    int get() { return at < n ? p[at++] : -1; }
};

/* The column the guess would take as the time under this header: the
 * first named as one and not twice, else the first (decide_time's order) */
inline size_t time_column_of(const std::vector<std::string>& head)
{
    for (size_t i = 0; i < head.size(); ++i) {
        std::string name;
        std::string unit;
        bool bracketed = false;
        split_header(head[i], &name, &unit, &bracketed);
        if (named_as_time(name) &&
            std::count(head.begin(), head.end(), head[i]) == 1) {
            return i;
        }
    }
    return 0;
}

/* Whether the record at off reads as a header split by delimiter: two or
 * more cells, more than half of its non-empty ones not numbers or times,
 * then up to kCheckRows rows of its cell count, each with more than half
 * of its non-empty cells numbers or times, read as the import reads them,
 * padding trimmed (a row of empty cells skipped, and so is one that fails
 * the numbers' rule with its cell in time_column_of empty;
 * a row the kCheckBytes window ends inside ends the rows, as the file's
 * end would) */
inline bool header_at(const unsigned char* p, size_t n, size_t off,
                      char delimiter)
{
    const size_t room = n - off < kCheckBytes ? n - off : kCheckBytes;
    /* The window can end inside a row: that row, and whatever the reader
     * makes of it, is the window's doing, not the file's */
    const bool window_cut = room < n - off;
    bounded_source src{p + off, room};
    csv::reader<bounded_source> r(src, delimiter);
    std::vector<std::string> head;
    std::vector<std::string> row;
    uint64_t line = 0;
    std::string why;
    if (r.next(head, &line, &why) != 1 || line != 1 || head.size() < 2) {
        return false;
    }
    r.trim_padding(true);
    size_t filled = 0;
    size_t numbers = 0;
    for (const std::string& c : head) {
        if (!c.empty()) {
            ++filled;
            numbers += numberish(c, delimiter) ? 1 : 0;
        }
    }
    if (filled == 0 || (filled - numbers) * 2 <= filled) {
        return false;
    }
    const size_t time_at = time_column_of(head);
    uint32_t rows = 0;
    while (rows < kCheckRows) {
        const int got = r.next(row, &line, &why);
        const bool cut_row = window_cut && src.at == room &&
                             (got != 1 || p[off + room - 1] != '\n');
        if (got < 0 || cut_row) {
            /* A later row's refusal is the sample's to say, in its words,
             * and a row the window cut is judged there whole */
            if (rows == 0) {
                return false;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        if (row.size() != head.size()) {
            return false;
        }
        /* Each row on its own, so a preamble line of the data's field
         * count can't pass on the rows below its header */
        size_t row_filled = 0;
        size_t row_numbers = 0;
        for (const std::string& c : row) {
            if (!c.empty()) {
                ++row_filled;
                row_numbers += numberish(c, delimiter) ? 1 : 0;
            }
        }
        if (row_filled == 0) {
            continue; /* a row of empty cells says nothing */
        }
        if (row_numbers * 2 <= row_filled) {
            if (row[time_at].empty()) {
                continue; /* the import leaves it out unread */
            }
            return false;
        }
        ++rows;
    }
    return rows > 0;
}

} // namespace ecuio_map_guess

#endif /* ECUIO_CSV_GUESS_RULES_H */
