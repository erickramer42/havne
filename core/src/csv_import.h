// File/layer: core/src/csv_import.h | C++17, Qt-free; another logger's CSV
//             as a recording's values (ADR-0054 section 5)
// Testing: UNIT: CsvImportTest (test_core); FUZZ: fuzz_csv_import
//
// The CSV import's rules, apart from the files: the column map (JSON,
// through core's strict parser), an RFC 4180 reader over any byte source,
// and the pass that turns each row into its values with their times, so
// import.cpp runs it over a file twice (check, then write) and the fuzz
// harness and the tests over bytes in memory. Strict and bounded: every
// refusal names its line ("line 12: ..."), never the file's path, and
// quotes the input only through ecuio_json::quoted.

#ifndef ECUIO_CSV_IMPORT_H
#define ECUIO_CSV_IMPORT_H

#include "json.h"
#include "recording_rows.h" /* parse_number, kNameMax, kUnitMax */

#include "ecuio/signals.h" /* ECUIO_DECODE_TEXT_MAX */

#include <cmath>
#include <initializer_list>
#include <new>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "csv_map.h"
#include "csv_reader.h"

namespace ecuio_csv_import {

/* ---- the pass ---- */

/* Bytes in memory as a Source */
struct memory_source {
    const unsigned char* p;
    size_t n;
    size_t at = 0;
    int get() { return at < n ? p[at++] : -1; }
};

/* What a pass found */
struct outcome {
    uint64_t lines = 0;               /* physical lines read */
    uint64_t rows = 0;                /* data rows */
    uint64_t values = 0;              /* samples handed on */
    uint64_t rows_no_time = 0;        /* rows left out: an empty time cell */
    uint64_t first_no_time = 0;       /* the first such row's line */
    std::vector<std::string> ignored; /* header columns not taken, in order */
    bool has_utc = false;
    uint64_t utc_start_us = 0; /* the first row's UTC time */
    uint64_t duration_us = 0;  /* the last row's time since the first */
};

inline std::string line_words(uint64_t line, const std::string& words)
{
    char buf[32];
    std::snprintf(buf, sizeof buf,
                  "line %llu: ", static_cast<unsigned long long>(line));
    return buf + words;
}

/* A time cell as microseconds (UTC since 1970 for the absolute forms);
 * *clock_us the time of day for time_of_day */
inline bool parse_time(const std::string& cell, const column_map& m,
                       int64_t* us, int64_t* clock_us)
{
    switch (m.unit) {
    case time_unit::s:
    case time_unit::ms:
    case time_unit::us: {
        double v = 0;
        if (!parse_cell_number(cell, m.decimal, &v)) {
            return false;
        }
        const double scale = m.unit == time_unit::s    ? 1e6
                             : m.unit == time_unit::ms ? 1e3
                                                       : 1.0;
        const double t = v * scale;
        if (!(std::fabs(t) <= kTimeAbsMaxUs)) {
            return false;
        }
        *us = std::llround(t);
        return true;
    }
    case time_unit::iso8601:
        return parse_iso(cell.data(), cell.size(), m.decimal, us);
    case time_unit::time_of_day:
        return parse_clock(cell.data(), cell.size(), m.decimal, clock_us);
    }
    return false;
}

inline const char* time_form(time_unit u)
{
    switch (u) {
    case time_unit::s:
        return "a number of seconds";
    case time_unit::ms:
        return "a number of milliseconds";
    case time_unit::us:
        return "a number of microseconds";
    case time_unit::iso8601:
        return "an ISO 8601 time with its offset";
    case time_unit::time_of_day:
        return "hh:mm:ss with an optional fraction";
    }
    return "";
}

/*
 * Reads src through the map: the preamble's rows skipped, the header
 * matched by name, then each row's time and its signals' cells; a row
 * whose time cell is empty is left out whole and counted. Sink:
 * bool number(uint64_t host_us, size_t signal, double v) and
 * bool text(uint64_t host_us, size_t signal, const std::string& v), each
 * false to stop (the pass then returns false with *why from
 * sink.failure()). host_us counts from the first row's time and never
 * goes back. False with *why ("line N: ..." where a line is at fault).
 */
template <class Source, class Sink>
bool run(Source& src, const column_map& m, Sink& sink, outcome& out,
         std::string* why)
{
    out = outcome{};
    why->clear();
    reader<Source> r(src, m.delimiter);
    for (uint32_t i = 1; i < m.header_row; ++i) {
        if (!r.skip_line(why)) {
            if (why->empty()) {
                *why = "the file ends before its header row";
            } else {
                *why = line_words(r.lines() + 1, *why);
            }
            return false;
        }
    }
    std::vector<std::string> header;
    uint64_t line_no = 0;
    const int got = r.next(header, &line_no, why);
    if (got <= 0) {
        if (got == 0) {
            *why = "the file ends before its header row";
        }
        return false;
    }
    r.trim_padding(true);
    /* Each column the map reads, by the header's text */
    auto index_of = [&](const std::string& name, size_t* at) {
        size_t found = 0;
        for (size_t i = 0; i < header.size(); ++i) {
            if (header[i] == name) {
                *at = i;
                ++found;
            }
        }
        if (found != 1) {
            *why = line_words(
                line_no, (found == 0 ? "the header has no column "
                                     : "the header names a column twice: ") +
                             ecuio_json::quoted(name));
            return false;
        }
        return true;
    };
    size_t time_at = 0;
    if (!index_of(m.time_column, &time_at)) {
        return false;
    }
    std::vector<size_t> at(m.signals.size());
    std::vector<bool> taken(header.size(), false);
    taken[time_at] = true;
    for (size_t s = 0; s < m.signals.size(); ++s) {
        if (!index_of(m.signals[s].column, &at[s])) {
            return false;
        }
        taken[at[s]] = true;
    }
    for (size_t i = 0; i < header.size(); ++i) {
        if (!taken[i]) {
            out.ignored.push_back(header[i]);
        }
    }

    std::vector<std::string> row;
    bool any = false;
    int64_t first = 0;
    int64_t prev = 0;
    int64_t day = 0;         /* time_of_day: days past the map's date */
    int64_t prev_clock = -1; /* time_of_day: the row before's */
    for (;;) {
        const int k = r.next(row, &line_no, why);
        if (k < 0) {
            return false;
        }
        if (k == 0) {
            break;
        }
        if (++out.rows > kRowsMax) {
            *why = "the file holds more rows than an import takes";
            return false;
        }
        if (row.size() != header.size()) {
            *why = line_words(line_no, "the row has " +
                                           std::to_string(row.size()) +
                                           " fields where the header has " +
                                           std::to_string(header.size()));
            return false;
        }
        const std::string& cell = row[time_at];
        if (cell.empty()) {
            /* No time to put its values at: the row is left out whole */
            if (out.rows_no_time++ == 0) {
                out.first_no_time = line_no;
            }
            continue;
        }
        int64_t t = 0;
        int64_t clock = 0;
        if (!parse_time(cell, m, &t, &clock)) {
            *why = line_words(line_no, std::string("its time isn't ") +
                                           time_form(m.unit));
            return false;
        }
        if (m.unit == time_unit::time_of_day) {
            /* Past midnight: a time of day more than 12 hours before the
             * row before's is the next day's */
            if (prev_clock >= 0 &&
                clock < prev_clock - int64_t{43200} * 1000000) {
                ++day;
            }
            prev_clock = clock;
            t = (m.date_days + day) * 86400 * 1000000 + clock - m.utc_offset_us;
            if (t < 0) {
                *why = line_words(line_no, "its time is before 1970");
                return false;
            }
        }
        if (any && t < prev) {
            if (prev - t > kReorderUs) {
                *why = line_words(line_no, "its time is more than 0.1 s before "
                                           "the row before it");
                return false;
            }
            t = prev;
        }
        if (!any) {
            first = t;
            any = true;
        }
        prev = t;
        const auto host = static_cast<uint64_t>(t - first);
        for (size_t s = 0; s < m.signals.size(); ++s) {
            const std::string& v = row[at[s]];
            if (v.empty()) {
                continue;
            }
            bool placeholder = false;
            for (const std::string& x : m.missing) {
                placeholder = placeholder || x == v;
            }
            if (placeholder) {
                continue;
            }
            bool ok = false;
            if (m.signals[s].text) {
                if (v.size() > kTextMax) {
                    *why = line_words(
                        line_no, "column " +
                                     ecuio_json::quoted(m.signals[s].column) +
                                     " holds more text than a recording "
                                     "keeps");
                    return false;
                }
                ok = sink.text(host, s, v);
            } else {
                double x = 0;
                if (!parse_cell_number(v, m.decimal, &x)) {
                    *why = line_words(
                        line_no, "column " +
                                     ecuio_json::quoted(m.signals[s].column) +
                                     " isn't a number");
                    return false;
                }
                ok = sink.number(host, s, x);
            }
            if (!ok) {
                *why = sink.failure();
                return false;
            }
            ++out.values;
        }
    }
    out.lines = r.lines();
    if (!any) {
        *why = out.rows_no_time > 0 ? "no row below the header has a time"
                                    : "the file holds no rows after its header";
        return false;
    }
    out.duration_us = static_cast<uint64_t>(prev - first);
    if (m.unit == time_unit::iso8601 || m.unit == time_unit::time_of_day) {
        out.has_utc = true;
        out.utc_start_us = static_cast<uint64_t>(first);
    } else if (m.has_start) {
        out.has_utc = true;
        out.utc_start_us = static_cast<uint64_t>(m.start_us);
    }
    return true;
}

/* A Sink that only counts */
struct counting_sink {
    bool number(uint64_t, size_t, double) { return true; }
    bool text(uint64_t, size_t, const std::string&) { return true; }
    std::string failure() const { return {}; }
};

} // namespace ecuio_csv_import

#endif /* ECUIO_CSV_IMPORT_H */
