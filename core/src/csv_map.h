// File/layer: core/src/csv_map.h | internal, not ABI-exported; included by csv_import.h and csv_reader.h
// C++17, Qt-free
// Testing: UNIT: CsvImportTest (test_core); FUZZ: fuzz_csv_import
//
// The CSV import's small parsers and column map (csv_import.h).

#ifndef ECUIO_CSV_MAP_H
#define ECUIO_CSV_MAP_H

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

namespace ecuio_csv_import {

/* A record's bytes (a row, its quoted line breaks included), the
 * columns of a row, the map's signals, its placeholder texts and their
 * length, the preamble's rows and the data rows */
constexpr size_t kRowBytesMax = size_t{64} << 10;
constexpr size_t kColumnsMax = 1024;
constexpr size_t kSignalsMax = 256;
constexpr size_t kMissingMax = 16;
constexpr size_t kMissingTextMax = 32;
constexpr uint32_t kHeaderRowMax = 1000;
constexpr uint64_t kRowsMax = 100000000;
/* A text cell: what a recording's text value holds (the reader's text_at
 * buffer), so a row of signals.csv stays within the reader's row bound */
constexpr size_t kTextMax = ECUIO_DECODE_TEXT_MAX;
/* A row stamped this much before the one before it takes that one's
 * time (a logger's clock stepping back a little, as candump's CPUs' do);
 * further is refused */
constexpr int64_t kReorderUs = 100000;
/* A relative time's bound, about 31 years: past it a double's
 * microseconds stop being whole */
constexpr double kTimeAbsMaxUs = 1e15;
/* The ignored columns events.log names before "and N more" */
constexpr size_t kIgnoredListed = 32;

enum class time_unit { s, ms, us, iso8601, time_of_day };

struct signal_spec {
    std::string column;
    std::string name;
    std::string unit;
    bool text = false;
    uint8_t decimals = 2;
    uint8_t rank = 0;
    uint8_t group = 0;
};

struct column_map {
    uint32_t header_row = 1;
    char delimiter = ',';
    char decimal = '.';
    std::string time_column;
    time_unit unit = time_unit::s;
    bool has_start = false;
    int64_t start_us = 0;      /* UTC, microseconds since 1970 */
    int64_t date_days = 0;     /* time_of_day: days since 1970-01-01 */
    int64_t utc_offset_us = 0; /* time_of_day: local minus UTC */
    std::vector<std::string> missing;
    std::vector<signal_spec> signals;
};

/* ---- small parsers ---- */

/* Strict UTF-8: no overlong form, no surrogate, nothing past U+10FFFF */
inline bool utf8_valid(const char* s, size_t n)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    size_t i = 0;
    while (i < n) {
        const unsigned c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        uint32_t least = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2, cp = c & 0x1F, least = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3, cp = c & 0x0F, least = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4, cp = c & 0x07, least = 0x10000;
        } else {
            return false;
        }
        if (n - i < len) {
            return false;
        }
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

inline bool digits(const char* p, size_t n, uint32_t* out)
{
    if (n == 0) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(p[i] - '0');
    }
    *out = v;
    return true;
}

/* "YYYY-MM-DD" as days since 1970-01-01 (1970 to 9999) */
inline bool parse_date(const char* p, size_t n, int64_t* days)
{
    uint32_t y = 0;
    uint32_t m = 0;
    uint32_t d = 0;
    if (n != 10 || p[4] != '-' || p[7] != '-' || !digits(p, 4, &y) ||
        !digits(p + 5, 2, &m) || !digits(p + 8, 2, &d) || y < 1970 || m < 1 ||
        m > 12 || d < 1) {
        return false;
    }
    static const unsigned kDays[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > kDays[m - 1] + (m == 2 && leap ? 1u : 0u)) {
        return false;
    }
    *days = ecuio_recording_rows::days_from_civil(y, m, d);
    return true;
}

/* "hh:mm:ss" with an optional fraction of 1 to 6 digits ("." or the
 * map's decimal), as microseconds since midnight */
inline bool parse_clock(const char* p, size_t n, char decimal, int64_t* us)
{
    uint32_t h = 0;
    uint32_t mi = 0;
    uint32_t s = 0;
    if (n < 8 || p[2] != ':' || p[5] != ':' || !digits(p, 2, &h) ||
        !digits(p + 3, 2, &mi) || !digits(p + 6, 2, &s) || h > 23 || mi > 59 ||
        s > 59) {
        return false;
    }
    int64_t frac = 0;
    if (n > 8) {
        if ((p[8] != '.' && p[8] != decimal) || n - 9 < 1 || n - 9 > 6) {
            return false;
        }
        uint32_t f = 0;
        if (!digits(p + 9, n - 9, &f)) {
            return false;
        }
        frac = f;
        for (size_t k = n - 9; k < 6; ++k) {
            frac *= 10;
        }
    }
    *us = (int64_t{h} * 3600 + mi * 60 + s) * 1000000 + frac;
    return true;
}

/* "Z", "+hh:mm" or "-hh:mm" as microseconds local minus UTC */
inline bool parse_offset(const char* p, size_t n, int64_t* us)
{
    if (n == 1 && p[0] == 'Z') {
        *us = 0;
        return true;
    }
    uint32_t h = 0;
    uint32_t m = 0;
    if (n != 6 || (p[0] != '+' && p[0] != '-') || p[3] != ':' ||
        !digits(p + 1, 2, &h) || !digits(p + 4, 2, &m) || h > 14 || m > 59) {
        return false;
    }
    const int64_t v = (int64_t{h} * 60 + m) * 60 * 1000000;
    *us = p[0] == '-' ? -v : v;
    return true;
}

/* "YYYY-MM-DDThh:mm:ss[.ffffff](Z|+hh:mm|-hh:mm)", 'T' or a space
 * between, as UTC microseconds since 1970 */
inline bool parse_iso(const char* p, size_t n, char decimal, int64_t* us)
{
    if (n < 20 || (p[10] != 'T' && p[10] != ' ')) {
        return false;
    }
    int64_t days = 0;
    if (!parse_date(p, 10, &days)) {
        return false;
    }
    size_t zone = n - 1;
    if (p[n - 1] != 'Z') {
        if (n < 25) {
            return false;
        }
        zone = n - 6;
    }
    int64_t clock = 0;
    int64_t offset = 0;
    if (!parse_clock(p + 11, zone - 11, decimal, &clock) ||
        !parse_offset(p + zone, n - zone, &offset)) {
        return false;
    }
    *us = days * 86400 * 1000000 + clock - offset;
    return *us >= 0;
}

/* A number cell, ASCII spaces around it allowed, its decimal mark the
 * map's; finite only */
inline bool parse_cell_number(const std::string& cell, char decimal,
                              double* out)
{
    size_t a = 0;
    size_t b = cell.size();
    while (a < b && cell[a] == ' ') {
        ++a;
    }
    while (b > a && cell[b - 1] == ' ') {
        --b;
    }
    if (a == b || b - a > 64) {
        return false;
    }
    char buf[65];
    size_t k = 0;
    for (size_t i = a; i < b; ++i) {
        char c = cell[i];
        if (decimal == ',') {
            if (c == '.') {
                return false; /* a grouping mark, or a point by mistake */
            }
            if (c == ',') {
                c = '.';
            }
        }
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
              c == 'e' || c == 'E')) {
            return false; /* parse_number's inf and nan too */
        }
        buf[k++] = c;
    }
    double v = 0;
    if (!ecuio_recording_rows::parse_number(buf, k, &v) || !std::isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

/* ---- the column map ---- */

inline bool map_fail(std::string* why, const std::string& words)
{
    *why = "the column map: " + words;
    return false;
}

inline bool only_keys(const ecuio_json::Value& o,
                      std::initializer_list<const char*> keys,
                      const char* where, std::string* why)
{
    for (const ecuio_json::Member& m : o.members) {
        bool known = false;
        for (const char* k : keys) {
            known = known || m.key == k;
        }
        if (!known) {
            return map_fail(why, std::string("unknown key ") +
                                     ecuio_json::quoted(m.key) + where);
        }
    }
    return true;
}

inline bool small_int(const ecuio_json::Value* v, int64_t lo, int64_t hi,
                      int64_t* out)
{
    if (v == nullptr || v->type != ecuio_json::Type::number || !v->in_int64 ||
        v->whole < lo || v->whole > hi) {
        return false;
    }
    *out = v->whole;
    return true;
}

/* A name or unit: valid UTF-8 (the parser's), no control character */
inline bool plain_text(const std::string& s)
{
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

inline bool parse_signal(const ecuio_json::Value& s, size_t index,
                         signal_spec& out, std::string* why)
{
    namespace json = ecuio_json;
    const std::string where = " in signal " + std::to_string(index + 1);
    if (s.type != json::Type::object) {
        return map_fail(why, "signal " + std::to_string(index + 1) +
                                 " isn't an object");
    }
    if (!only_keys(
            s, {"column", "name", "unit", "kind", "decimals", "rank", "group"},
            where.c_str(), why)) {
        return false;
    }
    const json::Value* column = s.find("column");
    const json::Value* name = s.find("name");
    const json::Value* unit = s.find("unit");
    const json::Value* kind = s.find("kind");
    if (column == nullptr || column->type != json::Type::string) {
        return map_fail(why, "\"column\" is a header's text" + where);
    }
    if (name == nullptr || name->type != json::Type::string ||
        name->text.empty() ||
        name->text.size() > ecuio_recording_rows::kNameMax ||
        !plain_text(name->text)) {
        return map_fail(why, "\"name\" is 1 to 31 bytes of text" + where);
    }
    if (unit != nullptr &&
        (unit->type != json::Type::string ||
         unit->text.size() > ecuio_recording_rows::kUnitMax ||
         !plain_text(unit->text))) {
        return map_fail(why, "\"unit\" is up to 15 bytes of text" + where);
    }
    if (kind != nullptr && (kind->type != json::Type::string ||
                            (kind->text != "number" && kind->text != "text"))) {
        return map_fail(why, "\"kind\" is \"number\" or \"text\"" + where);
    }
    /* The validator's rule for a def's name and unit (ADR-0049 section
     * 8): a spreadsheet runs a cell starting so as a formula */
    auto formula = [](const std::string& t) {
        return !t.empty() && (t[0] == '=' || t[0] == '+' || t[0] == '-' ||
                              t[0] == '@' || t[0] == '\t' || t[0] == '\r');
    };
    if (formula(name->text) || (unit != nullptr && formula(unit->text))) {
        return map_fail(why, "a name or unit can't start with =, +, -, @, a "
                             "tab or a carriage return, which a spreadsheet "
                             "runs as a formula" +
                                 where);
    }
    out.column = column->text;
    out.name = name->text;
    out.unit = unit != nullptr ? unit->text : std::string();
    out.text = kind != nullptr && kind->text == "text";
    out.decimals = out.text ? 0 : 2; /* a text shows no decimals */
    int64_t v = 0;
    if (const json::Value* d = s.find("decimals")) {
        if (out.text || !small_int(d, 0, 3, &v)) {
            return map_fail(why,
                            "\"decimals\" is 0 to 3, for a number" + where);
        }
        out.decimals = static_cast<uint8_t>(v);
    }
    /* A number plots in the map's order unless its rank says otherwise;
     * a text never plots */
    out.rank =
        out.text ? 0 : static_cast<uint8_t>(index + 1 > 255 ? 255 : index + 1);
    if (const json::Value* r = s.find("rank")) {
        if (!small_int(r, 0, 255, &v)) {
            return map_fail(why, "\"rank\" is 0 to 255" + where);
        }
        out.rank = static_cast<uint8_t>(v);
    }
    if (const json::Value* g = s.find("group")) {
        if (!small_int(g, 0, 255, &v)) {
            return map_fail(why, "\"group\" is 0 to 255" + where);
        }
        out.group = static_cast<uint8_t>(v);
    }
    return true;
}

/* The map's text: false with *why ("the column map: ...") */
inline bool parse_map(const char* text, size_t n, column_map& m,
                      std::string* why)
{
    namespace json = ecuio_json;
    m = column_map{};
    json::Value root;
    json::Error error;
    if (!json::parse(text, n, &root, &error)) {
        if (error.out_of_memory) {
            throw std::bad_alloc();
        }
        return map_fail(why, error.to_string());
    }
    if (root.type != json::Type::object) {
        return map_fail(why, "it isn't a JSON object");
    }
    if (!only_keys(root,
                   {"havne_column_map", "header_row", "delimiter", "decimal",
                    "time", "start", "missing", "signals"},
                   "", why)) {
        return false;
    }
    int64_t v = 0;
    if (!small_int(root.find("havne_column_map"), 1, 1, &v)) {
        return map_fail(why, "\"havne_column_map\": 1 is its first key's "
                             "value, and this core reads version 1 only");
    }
    if (const json::Value* h = root.find("header_row")) {
        if (!small_int(h, 1, kHeaderRowMax, &v)) {
            return map_fail(why, "\"header_row\" is 1 to 1000");
        }
        m.header_row = static_cast<uint32_t>(v);
    }
    if (const json::Value* d = root.find("delimiter")) {
        if (d->type != json::Type::string ||
            (d->text != "," && d->text != ";" && d->text != "\t")) {
            return map_fail(why, "\"delimiter\" is \",\", \";\" or a tab");
        }
        m.delimiter = d->text[0];
    }
    if (const json::Value* d = root.find("decimal")) {
        if (d->type != json::Type::string ||
            (d->text != "." && d->text != ",")) {
            return map_fail(why, "\"decimal\" is \".\" or \",\"");
        }
        m.decimal = d->text[0];
    }
    if (m.decimal == ',' && m.delimiter == ',') {
        return map_fail(why, "a decimal comma needs another delimiter");
    }
    const json::Value* t = root.find("time");
    if (t == nullptr || t->type != json::Type::object) {
        return map_fail(why, "\"time\" is an object naming the time column");
    }
    if (!only_keys(*t, {"column", "unit", "date", "utc_offset"}, " in time",
                   why)) {
        return false;
    }
    const json::Value* tc = t->find("column");
    const json::Value* tu = t->find("unit");
    if (tc == nullptr || tc->type != json::Type::string) {
        return map_fail(why, "\"column\" in time is a header's text");
    }
    m.time_column = tc->text;
    const std::string unit =
        tu != nullptr && tu->type == json::Type::string ? tu->text : "";
    if (unit == "s") {
        m.unit = time_unit::s;
    } else if (unit == "ms") {
        m.unit = time_unit::ms;
    } else if (unit == "us") {
        m.unit = time_unit::us;
    } else if (unit == "iso8601") {
        m.unit = time_unit::iso8601;
    } else if (unit == "time-of-day") {
        m.unit = time_unit::time_of_day;
    } else {
        return map_fail(why, "\"unit\" in time is \"s\", \"ms\", \"us\", "
                             "\"iso8601\" or \"time-of-day\"");
    }
    const bool tod = m.unit == time_unit::time_of_day;
    const json::Value* date = t->find("date");
    const json::Value* off = t->find("utc_offset");
    if (tod != (date != nullptr) || tod != (off != nullptr)) {
        return map_fail(why, "\"date\" and \"utc_offset\" in time come with "
                             "\"time-of-day\", and only with it");
    }
    if (tod) {
        if (date->type != json::Type::string ||
            !parse_date(date->text.data(), date->text.size(), &m.date_days)) {
            return map_fail(why, "\"date\" in time is YYYY-MM-DD");
        }
        if (off->type != json::Type::string ||
            !parse_offset(off->text.data(), off->text.size(),
                          &m.utc_offset_us)) {
            return map_fail(why, "\"utc_offset\" in time is \"Z\" or "
                                 "+hh:mm or -hh:mm");
        }
    }
    if (const json::Value* s = root.find("start")) {
        if (m.unit != time_unit::s && m.unit != time_unit::ms &&
            m.unit != time_unit::us) {
            return map_fail(why, "\"start\" is for a relative time only: an "
                                 "ISO 8601 time or a time of day carries its "
                                 "own");
        }
        if (s->type != json::Type::string ||
            !parse_iso(s->text.data(), s->text.size(), '.', &m.start_us)) {
            return map_fail(why, "\"start\" is an ISO 8601 time with its "
                                 "offset, as 2026-11-05T22:03:22.125+09:00");
        }
        m.has_start = true;
    }
    if (const json::Value* mi = root.find("missing")) {
        if (mi->type != json::Type::array || mi->items.size() > kMissingMax) {
            return map_fail(why, "\"missing\" is a list of up to 16 texts");
        }
        for (const json::Value& x : mi->items) {
            if (x.type != json::Type::string || x.text.empty() ||
                x.text.size() > kMissingTextMax) {
                return map_fail(why, "each of \"missing\" is 1 to 32 bytes "
                                     "of text");
            }
            m.missing.push_back(x.text);
        }
    }
    const json::Value* sigs = root.find("signals");
    if (sigs == nullptr || sigs->type != json::Type::array ||
        sigs->items.empty() || sigs->items.size() > kSignalsMax) {
        return map_fail(why, "\"signals\" is a list of 1 to 256 signals");
    }
    for (size_t i = 0; i < sigs->items.size(); ++i) {
        signal_spec s;
        if (!parse_signal(sigs->items[i], i, s, why)) {
            return false;
        }
        for (const signal_spec& o : m.signals) {
            if (o.name == s.name) {
                return map_fail(why, "two signals are named " +
                                         json::quoted(s.name));
            }
            if (o.column == s.column) {
                return map_fail(why, "two signals read the column " +
                                         json::quoted(s.column));
            }
        }
        if (s.column == m.time_column) {
            return map_fail(why, "the time column is a signal's too");
        }
        m.signals.push_back(std::move(s));
    }
    return true;
}

} // namespace ecuio_csv_import

#endif /* ECUIO_CSV_MAP_H */
