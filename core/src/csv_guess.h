// File/layer: core/src/csv_guess.h | C++17, Qt-free; a CSV's column map
//             guessed from its header (ADR-0054's appendix "a column map
//             guessed from the header")
// Testing: UNIT: CsvGuessTest (test_core); FUZZ: fuzz_csv_import
//
// The guess, apart from the file: given a file's first bytes it finds the
// header row, the delimiter, the decimal mark, the time column and its
// unit, and a signal for each other column, and writes them as one JSON
// text: the map (havne_column_map 1, which csv_import.h's parse_map takes
// before it is handed out), every header column used or left out with
// why, and the notes a front end shows. The same bytes always give the
// same text. Every refusal and note names columns by the header's text
// without its padding, through ecuio_json::quoted (the map's column key
// keeps the text exactly), and never a cell's value.

#ifndef ECUIO_CSV_GUESS_H
#define ECUIO_CSV_GUESS_H

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

/* ---- the guess ---- */

class guesser
{
public:
    /* unit_given: NULL, "s", "ms" or "us" (the caller checked it) */
    guesser(const unsigned char* p, size_t n, bool cut, const char* unit_given)
        : p_(p), n_(n), cut_(cut), given_(unit_given)
    {}

    bool run(result& out, std::string* why)
    {
        out = result{};
        if (n_ == 0) {
            *why = "the file is empty";
            return false;
        }
        if (!find_header()) {
            *why = "no row within the file's first 1000 reads as a header "
                   "with rows of numbers below it, split by a comma, a "
                   "semicolon or a tab";
            return false;
        }
        if (!sample(why)) {
            return false;
        }
        decide_decimal();
        if (!decide_time(why)) {
            return false;
        }
        decide_signals();
        if (signals_.empty()) {
            *why = "no column besides the time holds values in the rows read";
            return false;
        }
        return write(out, why);
    }

private:
    struct signal {
        size_t at;
        std::string name;
        std::string unit;
        bool text;
        uint8_t decimals;
    };

    void note(const std::string& words)
    {
        if (notes_.size() < kNotesMax) {
            notes_.push_back(words);
        } else {
            ++notes_dropped_;
        }
    }

    void leave_out(size_t at, const std::string& words)
    {
        why_[at] = words;
        note(ecuio_json::quoted(trim(header_[at])) + " is left out: " + words);
    }

    /* The first line, of the first 1000 within kSearchBytes, that reads
     * as a header; at one line the delimiters in order */
    bool find_header()
    {
        static const char kDelimiters[] = {',', ';', '\t'};
        size_t off = 0;
        for (uint32_t line = 1;
             line <= csv::kHeaderRowMax && off < n_ && off < kSearchBytes;
             ++line) {
            for (char d : kDelimiters) {
                if (header_at(p_, n_, off, d)) {
                    header_row_ = line;
                    delimiter_ = d;
                    return true;
                }
            }
            const void* lf = std::memchr(p_ + off, '\n', n_ - off);
            if (lf == nullptr) {
                break;
            }
            off = static_cast<size_t>(static_cast<const unsigned char*>(lf) -
                                      p_) +
                  1;
        }
        return false;
    }

    /* The header and the sample's rows, read as the import reads them */
    bool sample(std::string* why)
    {
        csv::memory_source src{p_, n_};
        csv::reader<csv::memory_source> r(src, delimiter_);
        for (uint32_t i = 1; i < header_row_; ++i) {
            if (!r.skip_line(why)) {
                /* The import's words for a preamble line it refuses, as
                 * it would refuse the map's header_row */
                *why = why->empty()
                           ? std::string("the file changed shape while it "
                                         "was read")
                           : csv::line_words(r.lines() + 1, *why);
                return false;
            }
        }
        uint64_t line = 0;
        if (r.next(header_, &line, why) != 1) {
            return false;
        }
        r.trim_padding(true);
        const size_t start = src.at;
        std::vector<std::string> row;
        uint64_t short_rows = 0;
        uint64_t first_short = 0;
        while (rows_read_ < kSampleRows && src.at - start < kSampleBytes) {
            const int got = r.next(row, &line, why);
            if (got < 0) {
                /* A quoted field the read's end cut short ends the sample */
                if (cut_ && why->find("a quoted field is never closed") !=
                                std::string::npos) {
                    why->clear();
                    break;
                }
                return false;
            }
            if (got == 0) {
                whole_ = !cut_;
                break;
            }
            ++rows_read_;
            if (row.size() != header_.size()) {
                if (short_rows++ == 0) {
                    first_short = line;
                }
                continue;
            }
            rows_.push_back(row);
            lines_.push_back(line);
        }
        why->clear();
        if (short_rows > 0) {
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "line %llu and %llu more %s read have another "
                          "number of fields than the header: the import "
                          "refuses such a row",
                          static_cast<unsigned long long>(first_short),
                          static_cast<unsigned long long>(short_rows - 1),
                          short_rows == 2 ? "row" : "rows");
            note(short_rows == 1
                     ? csv::line_words(first_short,
                                       "the row has another number of "
                                       "fields than the header: the import "
                                       "refuses it")
                     : std::string(buf));
        }
        if (rows_.empty()) {
            *why = "no row below the header has its number of fields";
            return false;
        }
        why_.assign(header_.size(), std::string());
        /* A header text twice can't be named by a map */
        twice_.assign(header_.size(), false);
        for (size_t i = 0; i < header_.size(); ++i) {
            for (size_t k = 0; k < header_.size(); ++k) {
                if (k != i && header_[k] == header_[i]) {
                    twice_[i] = true;
                }
            }
        }
        if (header_row_ > 1) {
            char buf[96];
            std::snprintf(buf, sizeof buf,
                          "the header is line %u: the lines above it are "
                          "left out",
                          static_cast<unsigned>(header_row_));
            note(buf);
        }
        return true;
    }

    /* A decimal comma: with a ';' or tab delimiter, number cells hold a
     * comma and none a point */
    void decide_decimal()
    {
        if (delimiter_ == ',') {
            return;
        }
        bool comma = false;
        bool point = false;
        for (const std::vector<std::string>& row : rows_) {
            for (const std::string& c : row) {
                if (c.find(',') != std::string::npos && is_number(c, ',')) {
                    comma = true;
                }
                if (c.find('.') != std::string::npos && is_number(c, '.')) {
                    point = true;
                }
            }
        }
        if (comma && !point) {
            decimal_ = ',';
            note("the numbers' decimal mark is a comma");
        }
    }

    /* Each sample row's cell at a column, empty ones included */
    std::vector<std::string> cells(size_t at) const
    {
        std::vector<std::string> v;
        v.reserve(rows_.size());
        for (const std::vector<std::string>& row : rows_) {
            v.push_back(row[at]);
        }
        return v;
    }

    /* The first column only rises: numbers or ISO times with an offset,
     * never less than the one before, two or more distinct. An empty cell
     * is passed by, as the import leaves its row out, while more than half
     * of the rows hold one: a sparse counter is no time */
    bool rises(size_t at) const
    {
        bool any = false;
        bool apart = false;
        double prev = 0;
        size_t filled = 0;
        for (const std::string& c : cells(at)) {
            double v = 0;
            int64_t us = 0;
            if (c.empty()) {
                continue;
            }
            ++filled;
            if (csv::parse_cell_number(c, decimal_, &v)) {
            } else if (iso_time(c, decimal_, &us)) {
                v = static_cast<double>(us);
            } else {
                return false;
            }
            if (any && v < prev) {
                return false;
            }
            apart = apart || (any && v > prev);
            prev = v;
            any = true;
        }
        return apart && filled * 2 > rows_.size();
    }

    bool decide_time(std::string* why)
    {
        for (size_t i = 0; i < header_.size() && time_ == npos; ++i) {
            std::string name;
            std::string unit;
            bool bracketed = false;
            split_header(header_[i], &name, &unit, &bracketed);
            if (!twice_[i] && named_as_time(name)) {
                time_ = i;
            }
        }
        const bool named = time_ != npos;
        if (time_ == npos && !twice_[0] && rises(0)) {
            time_ = 0;
        }
        if (time_ == npos) {
            *why = "no column reads as the time: none is named time, "
                   "timestamp, t, elapsed or time stamp, and the first "
                   "column's cells don't rise";
            return false;
        }
        const std::string col = ecuio_json::quoted(trim(header_[time_]));
        std::string name;
        std::string unit;
        bool bracketed = false;
        split_header(header_[time_], &name, &unit, &bracketed);
        bool all_iso = true;
        bool all_number = true;
        bool all_zoneless = true;
        bool all_clock = true;
        bool all_whole = true;
        bool any = false;
        for (const std::string& c : cells(time_)) {
            if (c.empty()) {
                continue;
            }
            any = true;
            int64_t us = 0;
            all_iso = all_iso && iso_time(c, decimal_, &us);
            all_number = all_number && is_number(c, decimal_);
            all_whole = all_whole && whole_number(c);
            all_zoneless =
                all_zoneless && (zoneless_iso(c) || iso_time(c, decimal_, &us));
            all_clock = all_clock &&
                        csv::parse_clock(c.data(), c.size(), decimal_, &us);
        }
        if (!any && whole_) {
            /* The import's words for the same file */
            *why = "no row below the header has a time";
            return false;
        }
        if (!any) {
            note("the time column " + col + " is empty in the rows read");
        } else if (all_iso) {
            unit_ = "iso8601";
            note(col + " is the time, as ISO 8601 times with their offset" +
                 std::string(given_ != nullptr ? ": the time unit given isn't "
                                                 "used"
                                               : ""));
        } else if (all_number) {
            flags_ |= kTimeNumbers;
            const char from_header = time_unit_of(unit);
            const char u = given_ != nullptr ? given_[0] : from_header;
            if (u != 0) {
                unit_ = u == 's' ? "s" : u == 'm' ? "ms" : "us";
                note(col + " is the time, in " +
                     (u == 's'   ? std::string("seconds")
                      : u == 'm' ? std::string("milliseconds")
                                 : std::string("microseconds")) +
                     (given_ != nullptr ? " as given" : " by its header"));
            } else if (named && all_whole) {
                /* A bare whole-number Time is milliseconds far more often
                 * than not; a fraction may be seconds, a counter rows */
                unit_ = "ms";
                note("the time column " + col +
                     " has no unit: read as ms; change it if it isn't");
            } else {
                note(col + " is the time, its unit not known: say s, ms or "
                           "us");
            }
        } else if (all_zoneless) {
            note(col + " holds ISO 8601 times without a UTC offset, which "
                       "the import needs in each: a column map can't add one");
        } else if (all_clock) {
            note(col + " holds times of day, which need a date and a UTC "
                       "offset: give a column map with them");
        } else {
            note(col + " is the time, but not every cell read is a time");
        }
        if (!unit_.empty()) {
            flags_ |= kTimeUnitKnown;
        }
        if (any) {
            drop_untimed();
        }
        return true;
    }

    /* The import leaves a row with no time out whole, so the signals are
     * decided without it */
    void drop_untimed()
    {
        const size_t read = rows_.size();
        size_t kept = 0;
        uint64_t first = 0;
        for (size_t i = 0; i < read; ++i) {
            if (rows_[i][time_].empty()) {
                first = first == 0 ? lines_[i] : first;
                continue;
            }
            if (kept != i) {
                rows_[kept].swap(rows_[i]);
                lines_[kept] = lines_[i];
            }
            ++kept;
        }
        rows_.resize(kept);
        lines_.resize(kept);
        const size_t dropped = read - kept;
        if (dropped == 0) {
            return;
        }
        untimed_dropped_ = true;
        note(std::to_string(dropped) + " of the " + std::to_string(rows_read_) +
             " rows read " +
             (dropped == 1
                  ? "has no time (line " + std::to_string(first) +
                        "): the import leaves it out"
                  : "have no time (the first on line " + std::to_string(first) +
                        "): the import leaves such a row out"));
    }

    void decide_signals()
    {
        static const char* const kPlaceholders[] = {"---", "--",  "NaN",
                                                    "nan", "N/A", "n/a"};
        constexpr size_t kPlaceholderCount =
            sizeof kPlaceholders / sizeof kPlaceholders[0];
        bool seen[kPlaceholderCount] = {};
        std::vector<std::string> names;
        size_t past = 0;
        for (size_t i = 0; i < header_.size(); ++i) {
            if (i == time_) {
                continue;
            }
            const std::string& head = header_[i];
            const std::string shown = ecuio_json::quoted(trim(head));
            std::string base;
            std::string unit;
            bool bracketed = false;
            split_header(head, &base, &unit, &bracketed);
            if (trim(head).empty()) {
                leave_out(i, "its header cell is empty");
                continue;
            }
            if (twice_[i]) {
                leave_out(i, "the header names it twice");
                continue;
            }
            if (base.empty()) {
                leave_out(i, "its header has no name before its unit");
                continue;
            }
            if (!csv::plain_text(base)) {
                leave_out(i, "its name holds a control character");
                continue;
            }
            if (formula_start(base)) {
                leave_out(i, "its name starts as a spreadsheet formula does");
                continue;
            }
            /* Its cells: empty ones and placeholders hold no sample */
            bool number = true;
            bool any = false;
            size_t values = 0;
            bool long_text = false;
            uint8_t decimals = 0;
            bool here[kPlaceholderCount] = {};
            for (const std::string& c : cells(i)) {
                if (c.empty()) {
                    continue;
                }
                bool held = false;
                for (size_t k = 0; k < kPlaceholderCount; ++k) {
                    if (c == kPlaceholders[k]) {
                        here[k] = held = true;
                    }
                }
                if (held) {
                    continue;
                }
                any = true;
                ++values;
                long_text = long_text || c.size() > csv::kTextMax;
                if (number && is_number(c, decimal_)) {
                    const uint8_t d = decimals_of(c, decimal_);
                    decimals = d > decimals ? d : decimals;
                } else {
                    number = false;
                }
            }
            if (!any) {
                /* The rows the signals are decided on: "read", or "with a
                 * time" once rows with none were taken out */
                const std::string kind =
                    untimed_dropped_ ? " with a time" : " read";
                leave_out(
                    i, whole_ ? "it is empty in every row" +
                                    std::string(
                                        untimed_dropped_ ? " with a time" : "")
                       : rows_.size() == 1
                           ? "it is empty in the one row" + kind +
                                 ": a later value is imported only if it "
                                 "is put back"
                           : "it is empty in the first " +
                                 std::to_string(rows_.size()) + " rows" + kind +
                                 ": a later value is imported only if it "
                                 "is put back");
                continue;
            }
            if (!number && long_text) {
                leave_out(i, "it holds more text than a recording keeps");
                continue;
            }
            if (signals_.size() == csv::kSignalsMax) {
                why_[i] = "a map takes 256 signals";
                ++past;
                continue;
            }
            if (number) {
                for (size_t k = 0; k < kPlaceholderCount; ++k) {
                    seen[k] = seen[k] || here[k];
                }
            }
            if (bracketed && !unit.empty() &&
                (unit.size() > ecuio_recording_rows::kUnitMax ||
                 !csv::plain_text(unit) || formula_start(unit))) {
                note(shown + "'s unit is left out: " +
                     (unit.size() > ecuio_recording_rows::kUnitMax
                          ? "it is longer than 15 bytes"
                          : "a map refuses it"));
                unit.clear();
            }
            /* The name: 31 bytes at most, a repeat numbered " 2", " 3" */
            std::string name = cut_utf8(base, ecuio_recording_rows::kNameMax);
            const bool cut = name.size() < base.size();
            bool renamed = false;
            for (unsigned k = 2; taken(names, name); ++k) {
                const std::string suffix = " " + std::to_string(k);
                name = cut_utf8(base, ecuio_recording_rows::kNameMax -
                                          suffix.size()) +
                       suffix;
                renamed = true;
            }
            if (cut || renamed) {
                note(shown + " is named " + ecuio_json::quoted(name) + ": " +
                     (renamed ? "another column has its name"
                              : "a name is 31 bytes at most"));
            }
            names.push_back(name);
            signals_.push_back(signal{i, name, unit, !number, decimals});
            /* A wide export's sweep row waits for every column but one
             * far sparser than the rest, whose newest value it holds */
            if (number && values * 2 < rows_.size()) {
                const bool far =
                    values * ecuio_recording_rows::kSparseTimes < rows_.size();
                note(shown + " holds a value in " + std::to_string(values) +
                     (untimed_dropped_ ? " of the " : " of ") +
                     std::to_string(rows_.size()) +
                     (untimed_dropped_ ? " rows with a time: "
                                       : " rows read: ") +
                     (far ? "a wide export by sweep holds its newest value "
                            "in the rows the other columns set"
                          : "a wide export by sweep follows its rate; pick a "
                            "row interval"));
            }
        }
        if (past > 0) {
            note(std::to_string(past) + " more columns are left out: a map "
                                        "takes 256 signals");
        }
        for (size_t k = 0; k < kPlaceholderCount; ++k) {
            if (seen[k]) {
                missing_.push_back(kPlaceholders[k]);
            }
        }
        if (!missing_.empty()) {
            std::string list;
            for (const std::string& m : missing_) {
                list += (list.empty() ? "" : ", ") + ecuio_json::quoted(m);
            }
            note(list + (missing_.size() == 1
                             ? " in a column of numbers holds no sample"
                             : " in a column of numbers hold no sample"));
        }
    }

    static bool taken(const std::vector<std::string>& names,
                      const std::string& name)
    {
        for (const std::string& n : names) {
            if (n == name) {
                return true;
            }
        }
        return false;
    }

    /* The map's JSON, its outer braces at column 0; unit: the time's, or
     * NULL for none */
    std::string map_json(const char* unit) const
    {
        std::string o = "{\n  \"havne_column_map\": 1,\n  \"header_row\": ";
        o += std::to_string(header_row_);
        o += ",\n  \"delimiter\": ";
        json_string(o, std::string(1, delimiter_));
        o += ",\n  \"decimal\": ";
        json_string(o, std::string(1, decimal_));
        o += ",\n  \"time\": {\"column\": ";
        json_string(o, header_[time_]);
        if (unit != nullptr) {
            o += ", \"unit\": ";
            json_string(o, unit);
        }
        o += "},\n";
        if (!missing_.empty()) {
            o += "  \"missing\": [";
            for (size_t i = 0; i < missing_.size(); ++i) {
                o += i == 0 ? "" : ", ";
                json_string(o, missing_[i]);
            }
            o += "],\n";
        }
        o += "  \"signals\": [\n";
        for (size_t i = 0; i < signals_.size(); ++i) {
            const signal& s = signals_[i];
            o += "    {\"column\": ";
            json_string(o, header_[s.at]);
            o += ", \"name\": ";
            json_string(o, s.name);
            if (!s.unit.empty()) {
                o += ", \"unit\": ";
                json_string(o, s.unit);
            }
            o += s.text ? ", \"kind\": \"text\"}" : ", \"kind\": \"number\"";
            if (!s.text) {
                o += ", \"decimals\": " + std::to_string(s.decimals) + "}";
            }
            o += i + 1 < signals_.size() ? ",\n" : "\n";
        }
        o += "  ]\n}";
        return o;
    }

    bool write(result& out, std::string* why)
    {
        /* A guess core would refuse is never handed out; one with no unit
         * is checked with one in its place */
        const std::string checked =
            map_json(unit_.empty() ? "s" : unit_.c_str());
        csv::column_map m;
        if (!csv::parse_map(checked.data(), checked.size(), m, why)) {
            *why = "the guess made a map core refuses (" + *why + ")";
            return false;
        }
        if (decimal_ == ',') {
            flags_ |= kDecimalComma;
        }
        if (header_row_ > 1) {
            flags_ |= kPreamble;
        }
        std::string& o = out.text;
        o = "{\n\"map\": ";
        out.map_offset = o.size();
        o += map_json(unit_.empty() ? nullptr : unit_.c_str());
        out.map_length = o.size() - out.map_offset;
        o += ",\n\"columns\": [\n";
        uint32_t left_out = 0;
        for (size_t i = 0; i < header_.size(); ++i) {
            o += "  {\"column\": ";
            json_string(o, header_[i]);
            const bool used = i == time_ || used_as_signal(i);
            o += used ? ", \"used\": true" : ", \"used\": false, \"why\": ";
            if (!used) {
                json_string(o, why_[i]);
                ++left_out;
            }
            o += i + 1 < header_.size() ? "},\n" : "}\n";
        }
        o += "],\n\"notes\": [";
        std::vector<std::string> notes = notes_;
        if (notes_dropped_ > 0) {
            notes.push_back("...and " + std::to_string(notes_dropped_) +
                            " more");
        }
        for (size_t i = 0; i < notes.size(); ++i) {
            o += i == 0 ? "\n  " : ",\n  ";
            json_string(o, notes[i]);
        }
        o += notes.empty() ? "]\n}\n" : "\n]\n}\n";
        out.flags = flags_;
        out.columns = static_cast<uint32_t>(signals_.size());
        out.left_out = left_out;
        out.rows_read = rows_read_;
        return true;
    }

    bool used_as_signal(size_t at) const
    {
        for (const signal& s : signals_) {
            if (s.at == at) {
                return true;
            }
        }
        return false;
    }

    static constexpr size_t npos = static_cast<size_t>(-1);

    const unsigned char* p_;
    size_t n_;
    bool cut_;
    const char* given_;
    uint32_t header_row_ = 1;
    char delimiter_ = ',';
    char decimal_ = '.';
    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> rows_;
    std::vector<uint64_t> lines_;  /* each of rows_' first line */
    bool untimed_dropped_ = false; /* rows_ is the rows with a time alone */
    uint32_t rows_read_ = 0;
    bool whole_ = false;           /* the sample read the whole file */
    std::vector<std::string> why_; /* each header column's, when left out */
    std::vector<bool> twice_;
    size_t time_ = npos;
    std::string unit_; /* the map's time unit, "" for none */
    std::vector<signal> signals_;
    std::vector<std::string> missing_;
    std::vector<std::string> notes_;
    size_t notes_dropped_ = 0;
    uint32_t flags_ = 0;
};

/*
 * The guess over a file's first bytes (cut: the file goes on past them,
 * cut at a line feed). unit_given: NULL, "s", "ms" or "us", for a time
 * column of numbers whose header gives none. False with *why.
 */
inline bool guess(const unsigned char* p, size_t n, bool cut,
                  const char* unit_given, result& out, std::string* why)
{
    why->clear();
    guesser g(p, n, cut, unit_given);
    return g.run(out, why);
}

} // namespace ecuio_map_guess

#endif /* ECUIO_CSV_GUESS_H */
