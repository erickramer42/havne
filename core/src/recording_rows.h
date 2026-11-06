// File/layer: core/src/recording_rows.h | internal, C++17, Qt-free,
//             header-only; ALL platforms
// Testing: UNIT: RecordingRowsTest (test_core): each parser's accepted and
//          refused forms; HARNESS: RecordingTest (test_core) through the
//          reader; FUZZ: fuzz_recording_rows
//
// The rows of a recording as the reader parses them (ADR-0049 section 5):
// signals.csv's and markers.csv's CSV rows, events.log's header lines and
// its activity lines. Pure and allocation-light, so the reader's one pass
// and its fuzz target share one parser. A recording comes from anywhere:
// every field is bounds-checked, and a refusal says why in words that
// never hold the row's text.

#ifndef ECUIO_RECORDING_ROWS_H
#define ECUIO_RECORDING_ROWS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace ecuio_recording_rows {

/* The longest row read, its line ending left out: a text value as long as
 * ECUIO_DECODE_TEXT_MAX with every byte a doubled quote, with room */
constexpr size_t kRowMax = 32768;
constexpr size_t kNameMax = 31; /* ecuio_signal_def_t's name, terminated */
constexpr size_t kUnitMax = 15;

constexpr const char* kSignalsHeader = "host_us,signal,value,unit";
constexpr const char* kMarkersHeader = "host_us,label";
constexpr const char* kFormat2 = "ecuio session recording, format 2 (ADR-0049)";
/* Format 2's shape, its signal lines' kind also "enum" and each enum
 * signal's labels on label lines (ADR-0049's appendix of 2026-11-05) */
constexpr const char* kFormat3 = "ecuio session recording, format 3 (ADR-0049)";
constexpr size_t kLabelMax = 27; /* ECUIO_ENUM_LABEL_MAX */
constexpr const char* kFormat1End = "session recording (ADR-0017)";
/* Format 3's "signal file  signals.json,<SHA-256>" (ADR-0051 section
 * 4): the signal file the recording was decoded with, copied beside it */
constexpr const char* kSignalFileKey = "signal file  ";
constexpr const char* kSignalFileName = "signals.json";
constexpr size_t kSha256Hex = 64;
/* "plugin  <id>,<version>,<variant>" after it (ADR-0052 section 4): the
 * plugin variant that decoded the recording, whose merged set the
 * signal file is */
constexpr const char* kPluginKey = "plugin  ";

/* A UTF-8 byte-order mark's length at the start of a header row, or 0 */
inline size_t bom_length(const char* p, size_t n)
{
    return n >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
                   static_cast<unsigned char>(p[1]) == 0xBB &&
                   static_cast<unsigned char>(p[2]) == 0xBF
               ? 3
               : 0;
}

inline bool equals(const char* p, size_t n, const char* text)
{
    const size_t m = std::strlen(text);
    return n == m && std::memcmp(p, text, n) == 0;
}

inline bool starts_with(const char* p, size_t n, const char* text)
{
    const size_t m = std::strlen(text);
    return n >= m && std::memcmp(p, text, m) == 0;
}

/* A decimal count, the whole of [p, p + n); false past UINT64_MAX */
inline bool parse_count(const char* p, size_t n, uint64_t* out)
{
    if (n == 0) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        const auto d = static_cast<uint64_t>(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* A number as "%.12g" writes it (and "inf", "-inf", "nan"), the whole of
 * [p, p + n), with a '.' whatever the locale: strtod reads the process's,
 * which a Qt program sets from the environment. Exact for up to 15
 * significant digits and a power of ten within 22, which covers every
 * value the writer formats; close otherwise. */
inline bool parse_number(const char* p, size_t n, double* out)
{
    size_t i = 0;
    bool negative = false;
    if (i < n && (p[i] == '-' || p[i] == '+')) {
        negative = p[i] == '-';
        ++i;
    }
    if (n - i == 3 && (std::memcmp(p + i, "inf", 3) == 0 ||
                       std::memcmp(p + i, "nan", 3) == 0)) {
        const bool inf = p[i] == 'i';
        const double v = inf ? std::numeric_limits<double>::infinity()
                             : std::numeric_limits<double>::quiet_NaN();
        *out = negative ? -v : v;
        return true;
    }
    uint64_t mantissa = 0;
    int digits = 0;
    int exponent = 0;
    bool any = false;
    for (; i < n && p[i] >= '0' && p[i] <= '9'; ++i) {
        any = true;
        if (digits < 19) {
            mantissa = mantissa * 10 + static_cast<uint64_t>(p[i] - '0');
            if (mantissa != 0) {
                ++digits;
            }
        } else {
            ++exponent;
        }
    }
    if (i < n && p[i] == '.') {
        ++i;
        for (; i < n && p[i] >= '0' && p[i] <= '9'; ++i) {
            any = true;
            if (digits < 19) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(p[i] - '0');
                if (mantissa != 0) {
                    ++digits;
                }
                --exponent;
            }
        }
    }
    if (!any) {
        return false;
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        ++i;
        bool neg_exp = false;
        if (i < n && (p[i] == '-' || p[i] == '+')) {
            neg_exp = p[i] == '-';
            ++i;
        }
        if (i == n) {
            return false;
        }
        int e = 0;
        for (; i < n && p[i] >= '0' && p[i] <= '9'; ++i) {
            if (e < 100000) {
                e = e * 10 + (p[i] - '0');
            }
        }
        exponent += neg_exp ? -e : e;
    }
    if (i != n) {
        return false;
    }
    static const double kPow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    double v = static_cast<double>(mantissa);
    if (v != 0.0) {
        int e = exponent;
        while (e > 22) {
            v *= 1e22;
            e -= 22;
        }
        while (e < -22) {
            v /= 1e22;
            e += 22;
        }
        v = e >= 0 ? v * kPow10[e] : v / kPow10[-e];
    }
    *out = negative ? -v : v;
    return true;
}

/* The decimal places a number's text shows: its digits after the point,
 * before any exponent */
inline unsigned decimals_shown(const char* p, size_t n)
{
    const void* dot = std::memchr(p, '.', n);
    if (dot == nullptr) {
        return 0;
    }
    unsigned d = 0;
    for (size_t i = static_cast<size_t>(static_cast<const char*>(dot) - p) + 1;
         i < n && p[i] >= '0' && p[i] <= '9'; ++i) {
        ++d;
    }
    return d;
}

/*
 * One CSV field from [*p, end): quoted ("a ""b"", c") or not, into out
 * (assigned, so its capacity is reused row to row). *p moves past the
 * field and its comma; *last says no comma followed. False for a quote
 * left open or text after a closing quote.
 */
inline bool next_field(const char*& p, const char* end, std::string& out,
                       bool* last)
{
    out.clear();
    if (p < end && *p == '"') {
        ++p;
        for (;;) {
            if (p == end) {
                return false;
            }
            if (*p == '"') {
                if (p + 1 < end && p[1] == '"') {
                    out += '"';
                    p += 2;
                    continue;
                }
                ++p;
                break;
            }
            out += *p++;
        }
        if (p < end && *p != ',') {
            return false;
        }
    } else {
        const char* start = p;
        while (p < end && *p != ',') {
            ++p;
        }
        out.append(start, static_cast<size_t>(p - start));
    }
    *last = p == end;
    if (p < end) {
        ++p;
    }
    return true;
}

/* A row of signals.csv, its strings reused row to row */
struct signal_row {
    uint64_t host_us = 0;
    std::string name;
    std::string value;
    std::string unit;
};

/* A NUL inside a row: a C caller would read the name or label short of
 * what the reader holds, so no row may carry one. An empty row may come
 * with no buffer at all (an empty vector's data()), which memchr can't
 * take even for no bytes. */
inline bool holds_nul(const char* p, size_t n, const char** why)
{
    if (n != 0 && std::memchr(p, '\0', n) != nullptr) {
        *why = "the row holds a NUL byte";
        return true;
    }
    return false;
}

/* host_us,signal,value,unit; *why on failure */
inline bool parse_signal_row(const char* line, size_t n, signal_row& r,
                             const char** why)
{
    if (holds_nul(line, n, why)) {
        return false;
    }
    const char* p = line;
    const char* end = line + n;
    bool last = false;
    std::string& scratch = r.unit; /* host_us's text, before the unit's */
    if (!next_field(p, end, scratch, &last) || last ||
        !parse_count(scratch.data(), scratch.size(), &r.host_us)) {
        *why = "host_us isn't a decimal count";
        return false;
    }
    if (!next_field(p, end, r.name, &last) || last) {
        *why = "the row isn't host_us,signal,value,unit";
        return false;
    }
    if (r.name.empty() || r.name.size() > kNameMax) {
        *why = "the signal's name is empty or longer than 31 bytes";
        return false;
    }
    if (!next_field(p, end, r.value, &last) || last) {
        *why = "the row isn't host_us,signal,value,unit";
        return false;
    }
    if (!next_field(p, end, r.unit, &last) || !last) {
        *why = "the row isn't host_us,signal,value,unit";
        return false;
    }
    if (r.unit.size() > kUnitMax) {
        *why = "the unit is longer than 15 bytes";
        return false;
    }
    return true;
}

/* A row of markers.csv: host_us,label */
inline bool parse_marker_row(const char* line, size_t n, uint64_t* host_us,
                             std::string& label, const char** why)
{
    if (holds_nul(line, n, why)) {
        return false;
    }
    const char* p = line;
    const char* end = line + n;
    bool last = false;
    if (!next_field(p, end, label, &last) || last ||
        !parse_count(label.data(), label.size(), host_us)) {
        *why = "host_us isn't a decimal count";
        return false;
    }
    if (!next_field(p, end, label, &last) || !last) {
        *why = "the row isn't host_us,label";
        return false;
    }
    return true;
}

/* events.log's "signal  <name>,<number|text|enum>,<unit>,<decimals>,
 * <rank>" and then ",<group>" (ADR-0050), which a recording from before
 * groups leaves out; "enum" is format 3's, a number shown as a word */
struct header_signal {
    std::string name;
    std::string unit;
    bool text = false;
    bool enumeration = false;
    uint8_t decimal_places = 0;
    uint8_t plot_rank = 0;
    uint8_t group = 0;
};

inline bool parse_header_signal(const char* text, size_t n, header_signal& s,
                                const char** why)
{
    if (holds_nul(text, n, why)) {
        return false;
    }
    const char* p = text;
    const char* end = text + n;
    bool last = false;
    std::string field;
    *why = "the signal line isn't name,kind,unit,decimals,rank[,group]";
    if (!next_field(p, end, s.name, &last) || last) {
        return false;
    }
    if (s.name.empty() || s.name.size() > kNameMax) {
        *why = "the signal's name is empty or longer than 31 bytes";
        return false;
    }
    if (!next_field(p, end, field, &last) || last) {
        return false;
    }
    s.text = field == "text";
    s.enumeration = field == "enum";
    if (!s.text && !s.enumeration && field != "number") {
        *why = "the signal's kind isn't number, text or enum";
        return false;
    }
    if (!next_field(p, end, s.unit, &last) || last) {
        return false;
    }
    if (s.unit.size() > kUnitMax) {
        *why = "the unit is longer than 15 bytes";
        return false;
    }
    uint64_t v = 0;
    if (!next_field(p, end, field, &last) || last ||
        !parse_count(field.data(), field.size(), &v) || v > 3) {
        *why = "the signal's decimal places aren't 0 to 3";
        return false;
    }
    s.decimal_places = static_cast<uint8_t>(v);
    if (!next_field(p, end, field, &last) ||
        !parse_count(field.data(), field.size(), &v) || v > 255) {
        *why = "the signal's plot rank isn't 0 to 255";
        return false;
    }
    s.plot_rank = static_cast<uint8_t>(v);
    if (last) {
        return true;
    }
    if (!next_field(p, end, field, &last) || !last ||
        !parse_count(field.data(), field.size(), &v) || v > 255) {
        *why = "the signal's group isn't 0 to 255";
        return false;
    }
    s.group = static_cast<uint8_t>(v);
    return true;
}

/* events.log's "label  <signal>,<raw>,<label>" (format 3): one of an enum
 * signal's labels, the signal and the label CSV-quoted as a signal line's
 * name is, the raw value a decimal uint32_t */
struct header_label {
    std::string signal;
    uint32_t raw = 0;
    std::string label;
};

inline bool parse_header_label(const char* text, size_t n, header_label& l,
                               const char** why)
{
    if (holds_nul(text, n, why)) {
        return false;
    }
    const char* p = text;
    const char* end = text + n;
    bool last = false;
    std::string field;
    *why = "the label line isn't signal,raw,label";
    if (!next_field(p, end, l.signal, &last) || last) {
        return false;
    }
    if (l.signal.empty() || l.signal.size() > kNameMax) {
        *why = "the label's signal name is empty or longer than 31 bytes";
        return false;
    }
    if (!next_field(p, end, field, &last) || last) {
        return false;
    }
    uint64_t v = 0;
    if (!parse_count(field.data(), field.size(), &v) || v > UINT32_MAX) {
        *why = "the label's raw value isn't 0 to 4294967295";
        return false;
    }
    l.raw = static_cast<uint32_t>(v);
    if (!next_field(p, end, l.label, &last) || !last) {
        return false;
    }
    if (l.label.empty() || l.label.size() > kLabelMax) {
        *why = "the label is empty or longer than 27 bytes";
        return false;
    }
    return true;
}

/* Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil) */
inline int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/* A signal file line's value, "signals.json," and 64 lowercase hex
 * digits: the digits into hex */
inline bool parse_signal_file(const char* p, size_t n, std::string* hex)
{
    const size_t name = std::strlen(kSignalFileName);
    if (n != name + 1 + kSha256Hex ||
        std::memcmp(p, kSignalFileName, name) != 0 || p[name] != ',') {
        return false;
    }
    const char* digits = p + name + 1;
    for (size_t i = 0; i < kSha256Hex; ++i) {
        const char c = digits[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    hex->assign(digits, kSha256Hex);
    return true;
}

/* "yyyy-MM-ddTHH:mm:ss.zzzZ" as microseconds since 1970 UTC */
inline bool parse_utc(const char* p, size_t n, uint64_t* out)
{
    if (n != 24 || p[4] != '-' || p[7] != '-' || p[10] != 'T' || p[13] != ':' ||
        p[16] != ':' || p[19] != '.' || p[23] != 'Z') {
        return false;
    }
    uint64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    if (!parse_count(p, 4, &y) || !parse_count(p + 5, 2, &mo) ||
        !parse_count(p + 8, 2, &d) || !parse_count(p + 11, 2, &h) ||
        !parse_count(p + 14, 2, &mi) || !parse_count(p + 17, 2, &s) ||
        !parse_count(p + 20, 3, &ms)) {
        return false;
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 ||
        s > 60) {
        return false;
    }
    const int64_t days =
        days_from_civil(static_cast<int64_t>(y), static_cast<unsigned>(mo),
                        static_cast<unsigned>(d));
    if (days < 0) {
        return false;
    }
    const uint64_t secs =
        static_cast<uint64_t>(days) * 86400 + h * 3600 + mi * 60 + s;
    *out = secs * 1000000 + ms * 1000;
    return true;
}

/* An activity line, "HH:mm:ss.zzz  +S.sss  <rest>": its seconds since the
 * start in ms (few enough that the microseconds fit), and the rest
 * (format 2: "<kind>  <text>") */
struct event_line {
    uint64_t ms = 0;
    const char* rest = nullptr;
    size_t rest_len = 0;
};

inline bool parse_event_line(const char* p, size_t n, event_line* e)
{
    /* "HH:mm:ss.zzz" then two spaces and '+' */
    if (n < 15 || p[2] != ':' || p[5] != ':' || p[8] != '.' || p[12] != ' ' ||
        p[13] != ' ' || p[14] != '+') {
        return false;
    }
    size_t i = 15;
    const size_t whole = i;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        ++i;
    }
    uint64_t s = 0;
    uint64_t ms = 0;
    if (i == whole || i + 4 > n || p[i] != '.' ||
        !parse_count(p + whole, i - whole, &s) ||
        !parse_count(p + i + 1, 3, &ms) || s > UINT64_MAX / 1000000 - 1) {
        return false;
    }
    i += 4;
    if (i + 2 > n || p[i] != ' ' || p[i + 1] != ' ') {
        return false;
    }
    e->ms = s * 1000 + ms;
    e->rest = p + i + 2;
    e->rest_len = n - i - 2;
    return true;
}

/* A format 2 line's kind and text: "<kind>  <text>" */
inline bool split_kind(const char* p, size_t n, const char** text,
                       size_t* text_len, size_t* kind_len)
{
    for (size_t i = 0; i + 1 < n; ++i) {
        if (p[i] == ' ' && p[i + 1] == ' ') {
            *kind_len = i;
            *text = p + i + 2;
            *text_len = n - i - 2;
            return i > 0;
        }
        if (p[i] < 'a' || p[i] > 'z') {
            return false;
        }
    }
    return false;
}

/* The summary's counts: "<why>: N frames, M values, R records dropped,
 * K notes dropped", read from its end, since the why may hold ": " */
inline bool parse_end(const char* p, size_t n, uint64_t* records,
                      uint64_t* notes)
{
    /* The text after the last ": " */
    size_t at = n;
    for (size_t i = n; i >= 2; --i) {
        if (p[i - 2] == ':' && p[i - 1] == ' ') {
            at = i;
            break;
        }
    }
    if (at == n) {
        return false;
    }
    const char* q = p + at;
    const char* end = p + n;
    uint64_t v[4] = {};
    const char* const words[4] = {" frames, ", " values, ",
                                  " records dropped, ", " notes dropped"};
    for (int k = 0; k < 4; ++k) {
        const char* start = q;
        while (q < end && *q >= '0' && *q <= '9') {
            ++q;
        }
        const size_t len = std::strlen(words[k]);
        if (!parse_count(start, static_cast<size_t>(q - start), &v[k]) ||
            static_cast<size_t>(end - q) < len ||
            std::memcmp(q, words[k], len) != 0) {
            return false;
        }
        q += len;
    }
    if (q != end) {
        return false;
    }
    *records = v[2];
    *notes = v[3];
    return true;
}

/* A drop line's count: "the recording dropped N frame[s]: ..." */
inline bool parse_drop(const char* p, size_t n, uint64_t* frames)
{
    const char* const lead = "the recording dropped ";
    const size_t len = std::strlen(lead);
    if (n <= len || std::memcmp(p, lead, len) != 0) {
        return false;
    }
    size_t i = len;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        ++i;
    }
    return parse_count(p + len, i - len, frames) && i < n && p[i] == ' ';
}

} // namespace ecuio_recording_rows

#endif /* ECUIO_RECORDING_ROWS_H */
