// File/layer: cli/view_lines.h | havne-cli, a recording's view as lines (ADR-0049 section 5)
// Testing: UNIT: CliViewTest (test_cli)
//
// `havne-cli view`'s lines, read through the reader (recording.h): what
// each recording holds, where an --align-on puts it, then each signal's
// lowest, highest, mean and count per column on one axis (the min/max
// shape a chart draws), a later recording's difference from the first's,
// a --span's statistics, and --stretches' pulls and cruises. Times named
// host_us (and a signal's first_us and last_us, a
// stats line's lowest_us and highest_us) are the recording's own; the
// axis's (from_us and to_us, on the axis, stats and stretch lines; an at
// line's t_us) are host_us plus the recording's offset_us, so aligned
// recordings share them.

#ifndef HAVNE_CLI_VIEW_LINES_H
#define HAVNE_CLI_VIEW_LINES_H

#include "jsonl.h"

#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cli {

// --align-on: rise:<signal>:<level>, fall:<signal>:<level>,
// marker:<label>, utc, engine-start (the rpm signal reaching 500
// after being below 100, core's ECUIO_RECORDING_FIND_ENGINE_START) or
// pull:<n>[,<n>...] (the start of each recording's nth pull, 1 the
// first: one number for every recording, or one for each in order). The
// level is the last colon's: a signal's name may hold one.
struct AlignOn {
    enum Kind { none, rise, fall, marker, utc, engine_start, pull };
    Kind kind = none;
    std::string signal;
    std::string label;
    double level = 0;
    std::vector<uint32_t> pulls; // pull: one, or one per recording
};

// The most pulls --align-on pull: counts to
constexpr uint32_t kAlignPullMax = 100000;

// pull:'s list: whole numbers from 1 to kAlignPullMax between commas,
// one or more, nothing else
inline bool parse_pull_list(const std::string& text, std::vector<uint32_t>& out)
{
    std::vector<uint32_t> list;
    uint32_t n = 0;
    size_t digits = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c >= '0' && c <= '9') {
            n = n * 10 + static_cast<uint32_t>(c - '0');
            if (++digits > 6) {
                return false;
            }
        } else if (c == ',' && digits != 0 && n >= 1 && n <= kAlignPullMax) {
            list.push_back(n);
            n = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    out = list;
    return true;
}

// A finite decimal, the whole text
inline bool parse_decimal(const char* text, double& out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text, &end);
    if (errno != 0 || *end != '\0' || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

inline bool parse_align_on(const char* text, AlignOn& out)
{
    if (text == nullptr) {
        return false;
    }
    const std::string s = text;
    AlignOn a;
    if (s == "utc") {
        a.kind = AlignOn::utc;
    } else if (s == "engine-start") {
        a.kind = AlignOn::engine_start;
    } else if (s.rfind("marker:", 0) == 0 && s.size() > 7) {
        a.kind = AlignOn::marker;
        a.label = s.substr(7);
    } else if (s.rfind("pull:", 0) == 0) {
        a.kind = AlignOn::pull;
        if (!parse_pull_list(s.substr(5), a.pulls)) {
            return false;
        }
    } else if (s.rfind("rise:", 0) == 0 || s.rfind("fall:", 0) == 0) {
        a.kind = s[0] == 'r' ? AlignOn::rise : AlignOn::fall;
        const std::string rest = s.substr(5);
        const size_t colon = rest.rfind(':');
        if (colon == std::string::npos || colon == 0 ||
            !parse_decimal(rest.c_str() + colon + 1, a.level)) {
            return false;
        }
        a.signal = rest.substr(0, colon);
    } else {
        return false;
    }
    out = a;
    return true;
}

// Seconds, signed (an aligned axis runs before its zero), to the
// microsecond; at most a billion either way
inline bool parse_seconds(const char* text, int64_t& us)
{
    double s = 0;
    if (!parse_decimal(text, s) || std::fabs(s) > 1e9) {
        return false;
    }
    us = static_cast<int64_t>(std::llround(s * 1e6));
    return true;
}

// --span A:B, seconds on the axis as --at takes them, A before B
struct Span {
    int64_t from_us = 0;
    int64_t to_us = 0;
};

inline bool parse_span(const char* text, Span& out)
{
    const char* colon = std::strchr(text, ':');
    if (colon == nullptr || std::strchr(colon + 1, ':') != nullptr) {
        return false;
    }
    const std::string from(text, static_cast<size_t>(colon - text));
    return parse_seconds(from.c_str(), out.from_us) &&
           parse_seconds(colon + 1, out.to_us) && out.from_us < out.to_us;
}

// The axis: `columns` columns of column_us each from from_us, to_us
// rounded out to a whole column, so every recording's columns start on
// the same microseconds
struct Axis {
    int64_t from_us = 0;
    int64_t to_us = 0;
    int64_t column_us = 0;
    uint32_t columns = 0;
};

inline bool make_axis(int64_t from_us, int64_t to_us, uint32_t columns,
                      Axis& out)
{
    if (columns == 0 || to_us <= from_us) {
        return false;
    }
    const int64_t span = to_us - from_us;
    const int64_t n = columns;
    out.from_us = from_us;
    out.column_us = (span + n - 1) / n;
    out.to_us = from_us + out.column_us * n;
    out.columns = columns;
    return true;
}

// A recording's part of the axis: `count` columns from axis column
// `first`, over its own time; the ones before are empty. The reader takes
// no time before the recording's start (host_us 0), so a column across it
// is a query of its own, [0, partial_t1_us), and the rest, `count` less
// that one, is a query over [t0_us, t1_us). Columns of a second or more
// place whole seconds (each in the column it starts in, the first column
// taking one that starts before it), so there the partial column ends at
// the last whole second and the second across the split goes to the
// next: none is lost or counted twice, and aligned recordings agree to
// the second, as the reader's columns are. False when none of the axis
// falls in the recording.
struct Window {
    uint32_t first = 0;
    uint32_t count = 0;
    bool partial = false;
    uint64_t partial_t1_us = 0;
    uint64_t t0_us = 0;
    uint64_t t1_us = 0;
};

inline bool window_of(const Axis& a, int64_t offset_us, Window& out)
{
    const int64_t t0 = a.from_us - offset_us;
    const int64_t t1 = a.to_us - offset_us;
    if (t1 <= 0) {
        return false;
    }
    /* The first column starting at host_us 0 or later; t1 > 0 bounds it
     * by the column count */
    const int64_t skip = t0 < 0 ? (-t0 + a.column_us - 1) / a.column_us : 0;
    Window w;
    w.first = static_cast<uint32_t>(skip);
    w.count = a.columns - w.first;
    w.t0_us = static_cast<uint64_t>(t0 + skip * a.column_us);
    w.t1_us = static_cast<uint64_t>(t1);
    if (skip > 0 && w.t0_us > 0) {
        uint64_t p = w.t0_us;
        if (w.count == 0) {
            p = w.t1_us; /* the axis ends inside it: no rest */
        } else if (a.column_us >= 1000000) {
            p = w.t0_us / 1000000 * 1000000;
        }
        if (p > 0) {
            w.partial = true;
            w.partial_t1_us = p;
            w.first -= 1;
            w.count += 1;
        }
    }
    if (w.count == 0) {
        return false;
    }
    out = w;
    return true;
}

// The opened line's format: core's for formats 2 and 3 alike (format 3
// keeps format 2's shape, its enums seen on their signal lines), so schema
// 1's word holds
inline const char* format_word(uint32_t format)
{
    switch (format) {
    case ECUIO_RECORDING_FORMAT_CORE:
    case ECUIO_RECORDING_FORMAT_ENUMS:
        return "core";
    case ECUIO_RECORDING_FORMAT_OLDER:
        return "older";
    default:
        return "unknown";
    }
}

inline void us_or_null(Line& l, uint64_t us)
{
    if (us == ECUIO_RECORDING_NONE) {
        l.raw("null");
    } else {
        l.u64(us);
    }
}

inline void signed_us(Line& l, int64_t us)
{
    l.number(static_cast<double>(us));
}

// What a recording holds: {"type":"opened", recording, format,
// duration_us, utc_start_us|null, signals, markers, summary, rows_torn,
// records_dropped, notes_dropped, signal_file}; signal_file is null when
// the header names none and the folder holds none, else {sha256 (the
// header's, or null), present, matches} (ADR-0051 section 4); and with
// view --frames, frames: the profile its frames decoded through, or
// "signals.json" for the recording's own file
inline void opened_line(Line& l, uint32_t recording,
                        const ecuio_recording_info_t& i,
                        const ecuio_recording_signals_t& sf,
                        const char* frames = nullptr)
{
    l.raw("{\"type\":\"opened\"");
    l.key("recording");
    l.u64(recording);
    l.key("format");
    l.string(format_word(i.format), SIZE_MAX);
    l.key("duration_us");
    l.u64(i.duration_us);
    l.key("utc_start_us");
    if (i.utc_start_us == 0) {
        l.raw("null");
    } else {
        l.u64(i.utc_start_us);
    }
    l.key("signals");
    l.u64(i.signal_count);
    l.key("markers");
    l.u64(i.marker_count);
    l.key("summary");
    l.raw((i.flags & ECUIO_RECORDING_INFO_NO_SUMMARY) ? "false" : "true");
    l.key("rows_torn");
    l.u64(i.rows_torn);
    l.key("records_dropped");
    l.u64(i.records_dropped);
    l.key("notes_dropped");
    l.u64(i.notes_dropped);
    l.key("signal_file");
    if (sf.flags == 0) {
        l.raw("null");
    } else {
        l.ch('{');
        l.key("sha256");
        if ((sf.flags & ECUIO_RECORDING_SIGNALS_NAMED) != 0) {
            l.string(sf.sha256, sizeof sf.sha256);
        } else {
            l.raw("null");
        }
        l.key("present");
        l.raw((sf.flags & ECUIO_RECORDING_SIGNALS_PRESENT) != 0 ? "true"
                                                                : "false");
        l.key("matches");
        l.raw((sf.flags & ECUIO_RECORDING_SIGNALS_MATCH) != 0 ? "true"
                                                              : "false");
        l.ch('}');
    }
    if (frames != nullptr) {
        l.key("frames");
        l.string(frames, SIZE_MAX);
    }
    l.ch('}');
}

// A signal's samples a second from its first to its last, or null with
// fewer than two or none apart
inline void rate_hz(Line& l, const ecuio_recording_signal_t& s)
{
    if (s.samples < 2 || s.first_us == ECUIO_RECORDING_NONE ||
        s.last_us == ECUIO_RECORDING_NONE || s.last_us <= s.first_us) {
        l.raw("null");
        return;
    }
    l.number(static_cast<double>(s.samples - 1) * 1e6 /
             static_cast<double>(s.last_us - s.first_us));
}

// The bytes an enum signal's labels add to its line at most: each a
// key of up to 10 digits and a label of 27 bytes, every byte escaped
inline size_t labels_bytes(uint32_t count)
{
    return static_cast<size_t>(count) * (16 + 6 * ECUIO_ENUM_LABEL_MAX);
}

// An enum's labels as an object, {"<raw>": label, ...} in raw order, as a
// signal file's "values" (ADR-0051)
inline void labels_object(Line& l, const ecuio_enum_entry_t* labels,
                          uint32_t count)
{
    l.ch('{');
    for (uint32_t k = 0; k < count; ++k) {
        char raw[16];
        std::snprintf(raw, sizeof raw, "%u",
                      static_cast<unsigned>(labels[k].raw));
        if (k > 0) {
            l.ch(',');
        }
        l.string(raw, sizeof raw);
        l.ch(':');
        l.string(labels[k].label, sizeof labels[k].label);
    }
    l.ch('}');
}

// A signal: {"type":"signal", recording, index, name, unit, text, then
// for an enum (format 3) "enum":true and "labels" {"<raw>": label} in
// raw order, as a signal file's "values" (ADR-0051), then decimals, rank,
// group (its name; "" for a code this core doesn't know), samples, lowest
// and highest (a number's, an enum's too; null with no samples),
// first_us|null, last_us|null, rate_hz|null}
inline void signal_line(Line& l, uint32_t recording, uint32_t index,
                        const ecuio_recording_signal_t& s,
                        const ecuio_enum_entry_t* labels = nullptr,
                        uint32_t label_count = 0)
{
    const bool text = (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
    l.raw("{\"type\":\"signal\"");
    l.key("recording");
    l.u64(recording);
    l.key("index");
    l.u64(index);
    l.key("name");
    l.string(s.name, sizeof s.name);
    l.key("unit");
    l.string(s.unit, sizeof s.unit);
    l.key("text");
    l.raw(text ? "true" : "false");
    if ((s.flags & ECUIO_RECORDING_SIGNAL_ENUM) != 0) {
        l.key("enum");
        l.raw("true");
        l.key("labels");
        labels_object(l, labels, label_count);
    }
    l.key("decimals");
    l.u64(s.decimal_places);
    l.key("rank");
    l.u64(s.plot_rank);
    l.key("group");
    const char* group = ecuio_signal_group_name(s.group);
    l.string(group, std::strlen(group));
    l.key("samples");
    l.u64(s.samples);
    if (!text) {
        l.key("lowest");
        if (s.samples == 0) {
            l.raw("null");
        } else {
            l.number(s.lowest);
        }
        l.key("highest");
        if (s.samples == 0) {
            l.raw("null");
        } else {
            l.number(s.highest);
        }
    }
    l.key("first_us");
    us_or_null(l, s.first_us);
    l.key("last_us");
    us_or_null(l, s.last_us);
    l.key("rate_hz");
    rate_hz(l, s);
    l.ch('}');
}

// {"type":"marker", recording, index, host_us, label}
inline void marker_line(Line& l, uint32_t recording, uint32_t index,
                        const ecuio_recording_marker_t& m)
{
    l.raw("{\"type\":\"marker\"");
    l.key("recording");
    l.u64(recording);
    l.key("index");
    l.u64(index);
    l.key("host_us");
    l.u64(m.host_us);
    l.key("label");
    l.string(m.label_utf8, m.label_length);
    l.ch('}');
}

// Where --align-on put a recording: {"type":"align", recording, on,
// found, host_us (the event's; null for utc or when not found),
// offset_us (null when not found)}. A recording not found has no
// columns or at line.
inline void align_line(Line& l, uint32_t recording, const char* on, bool found,
                       uint64_t host_us, int64_t offset_us)
{
    l.raw("{\"type\":\"align\"");
    l.key("recording");
    l.u64(recording);
    l.key("on");
    l.string(on, SIZE_MAX, 256);
    l.key("found");
    l.raw(found ? "true" : "false");
    l.key("host_us");
    us_or_null(l, found ? host_us : ECUIO_RECORDING_NONE);
    l.key("offset_us");
    if (found) {
        signed_us(l, offset_us);
    } else {
        l.raw("null");
    }
    l.ch('}');
}

// The closed span of a recording's time whose samples the reader counts
// in column c of a query of n columns over [t0_us, t1_us), the span a
// whole number of columns as view's queries are: the column's own
// microseconds, or with columns of a second or more the whole seconds
// the reader places there (each in the column it starts in, the first
// taking the second across t0_us, the last second whole though it runs
// past t1_us). False for a column no second starts in.
inline bool column_span(uint64_t t0_us, uint64_t t1_us, uint32_t n, uint32_t c,
                        uint64_t& from_us, uint64_t& to_us)
{
    const uint64_t second = 1000000;
    const uint64_t width = (t1_us - t0_us) / n;
    const uint64_t start = t0_us + c * width;
    if (width < second) {
        from_us = start;
        to_us = start + width - 1;
        return true;
    }
    const uint64_t first =
        c == 0 ? t0_us / second : (start + second - 1) / second;
    const uint64_t past = (std::min)((start + width + second - 1) / second,
                                     (t1_us + second - 1) / second);
    if (first >= past) {
        return false;
    }
    from_us = first * second;
    to_us = past * second - 1;
    return true;
}

// A column's mean where its count and range alone give it: none for an
// empty column (NaN), the value of one sample or of samples all alike,
// the middle of two. False where the samples themselves must be read.
inline bool mean_of_column(const ecuio_recording_column_t& col, double& mean)
{
    if (col.samples == 0) {
        mean = NAN;
    } else if (col.samples == 1 || col.lowest == col.highest) {
        mean = col.lowest;
    } else if (col.samples == 2) {
        mean = (col.lowest + col.highest) / 2;
    } else {
        return false;
    }
    return true;
}

// What a delta compares: a signal's name and its unit, the unit's ASCII
// case ignored ("RPM" is "rpm"), so a speed in km/h is never taken from
// one in mph
inline std::string delta_key(const ecuio_recording_signal_t& s)
{
    std::string key;
    for (size_t i = 0; i < sizeof s.name && s.name[i] != '\0'; ++i) {
        key += s.name[i];
    }
    key += '\n';
    for (size_t i = 0; i < sizeof s.unit && s.unit[i] != '\0'; ++i) {
        const char c = s.unit[i];
        key += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return key;
}

// {"type":"axis", from_us, to_us, columns, column_us}
inline void axis_line(Line& l, const Axis& a)
{
    l.raw("{\"type\":\"axis\"");
    l.key("from_us");
    signed_us(l, a.from_us);
    l.key("to_us");
    signed_us(l, a.to_us);
    l.key("columns");
    l.u64(a.columns);
    l.key("column_us");
    l.u64(static_cast<uint64_t>(a.column_us));
    l.ch('}');
}

// The bytes a columns line can take: a column's five entries at most
// 26 bytes each, and a name escaped
inline size_t columns_line_bytes(uint32_t columns)
{
    return 512 + static_cast<size_t>(columns) * 5 * 27;
}

// One signal's columns on the axis: {"type":"columns", recording, signal,
// samples [], and a number's lowest [], highest [] and mean [], null
// where a column is empty; then, for a recording after the first aligned
// one that shows the signal as a number in the same unit (delta_key),
// delta_from (that first's place) and delta [], its mean less the
// first's, null where either column is empty}. A column one microsecond
// wide holding three or more unlike samples has no mean (the reader's
// statistics take no span of one instant). cols holds w.count columns, the axis's from w.first on; the
// others are empty. NULL cols: every column empty. means and delta hold
// the axis's columns each, NaN for none; NULL delta: no delta keys.
inline void columns_line(Line& l, uint32_t recording,
                         const ecuio_recording_signal_t& s, const Window& w,
                         uint32_t columns, const ecuio_recording_column_t* cols,
                         const double* means = nullptr,
                         const double* delta = nullptr, uint32_t delta_from = 0)
{
    const bool text = (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
    auto at = [&](uint32_t c) -> const ecuio_recording_column_t* {
        if (cols == nullptr || c < w.first || c - w.first >= w.count) {
            return nullptr;
        }
        const ecuio_recording_column_t* p = &cols[c - w.first];
        return p->samples == 0 ? nullptr : p;
    };
    l.raw("{\"type\":\"columns\"");
    l.key("recording");
    l.u64(recording);
    l.key("signal");
    l.string(s.name, sizeof s.name);
    l.key("samples");
    l.ch('[');
    for (uint32_t c = 0; c < columns; ++c) {
        if (c != 0) {
            l.ch(',');
        }
        const ecuio_recording_column_t* p = at(c);
        l.u64(p != nullptr ? p->samples : 0);
    }
    l.ch(']');
    for (int pass = 0; pass < 2 && !text; ++pass) {
        l.key(pass == 0 ? "lowest" : "highest");
        l.ch('[');
        for (uint32_t c = 0; c < columns; ++c) {
            if (c != 0) {
                l.ch(',');
            }
            const ecuio_recording_column_t* p = at(c);
            if (p == nullptr) {
                l.raw("null");
            } else {
                l.number(pass == 0 ? p->lowest : p->highest);
            }
        }
        l.ch(']');
    }
    const auto numbers = [&](const char* key, const double* values) {
        l.key(key);
        l.ch('[');
        for (uint32_t c = 0; c < columns; ++c) {
            if (c != 0) {
                l.ch(',');
            }
            l.number(values != nullptr ? values[c] : NAN);
        }
        l.ch(']');
    };
    if (!text) {
        numbers("mean", means);
        if (delta != nullptr) {
            l.key("delta_from");
            l.u64(delta_from);
            numbers("delta", delta);
        }
    }
    l.ch('}');
}

// The values in force at a time on the axis (the cursor's readout):
// {"type":"at", recording, t_us, values [{signal, host_us|null, text}]},
// each the last sample at or before it, as signals.csv holds it; before
// a signal's first, null and "". Built in three calls.
inline void at_begin(Line& l, uint32_t recording, int64_t t_us)
{
    l.raw("{\"type\":\"at\"");
    l.key("recording");
    l.u64(recording);
    l.key("t_us");
    signed_us(l, t_us);
    l.key("values");
    l.ch('[');
}

inline void at_value(Line& l, bool first, const ecuio_recording_signal_t& s,
                     uint64_t host_us, const char* text)
{
    if (!first) {
        l.ch(',');
    }
    l.raw("{\"signal\":");
    l.string(s.name, sizeof s.name);
    l.key("host_us");
    us_or_null(l, host_us);
    l.key("text");
    l.string(text, SIZE_MAX);
    l.ch('}');
}

inline void at_end(Line& l)
{
    l.raw("]}");
}

// A span's statistics for one signal (ecuio_recording_stats):
// {"type":"stats", recording, signal, from_us, to_us, samples, first,
// last, lowest, lowest_us, highest, highest_us, mean}; from_us and to_us
// the span on the axis, both ends counted; the values as signals.csv
// holds them, null with none (and for an inf, which JSON can't hold); a
// text signal's samples alone, the rest null
inline void stats_line(Line& l, uint32_t recording,
                       const ecuio_recording_signal_t& s, const Span& span,
                       const ecuio_recording_stats_t& st)
{
    l.raw("{\"type\":\"stats\"");
    l.key("recording");
    l.u64(recording);
    l.key("signal");
    l.string(s.name, sizeof s.name);
    l.key("from_us");
    signed_us(l, span.from_us);
    l.key("to_us");
    signed_us(l, span.to_us);
    l.key("samples");
    l.u64(st.samples);
    l.key("first");
    l.number(st.first);
    l.key("last");
    l.number(st.last);
    l.key("lowest");
    l.number(st.lowest);
    l.key("lowest_us");
    us_or_null(l, st.lowest_us);
    l.key("highest");
    l.number(st.highest);
    l.key("highest_us");
    us_or_null(l, st.highest_us);
    l.key("mean");
    l.number(st.mean);
    l.ch('}');
}

// A span's statistics with nothing in it: before a recording's start
inline ecuio_recording_stats_t no_stats()
{
    ecuio_recording_stats_t st{};
    st.first = st.last = st.lowest = st.highest = st.mean = NAN;
    st.lowest_us = st.highest_us = ECUIO_RECORDING_NONE;
    return st;
}

// --- --stretches (ADR-0056's appendix "the navigation aids") -----------

// km/h in a mile per hour, by definition
constexpr double kKmhPerMph = 1.609344;

// --stretches' word: pulls, cruises or all
inline bool parse_stretch_kinds(const char* word, uint32_t& kinds)
{
    if (std::strcmp(word, "pulls") == 0) {
        kinds = ECUIO_STRETCH_PULLS;
    } else if (std::strcmp(word, "cruises") == 0) {
        kinds = ECUIO_STRETCH_CRUISES;
    } else if (std::strcmp(word, "all") == 0) {
        kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    } else {
        return false;
    }
    return true;
}

// The rules core's defaults fill, kinds as given, every role core's pick
inline ecuio_stretch_rules_t stretch_rules(uint32_t kinds)
{
    ecuio_stretch_rules_t r{};
    r.kinds = kinds;
    r.throttle = r.engine_load = r.vehicle_speed = r.engine_speed = UINT32_MAX;
    return r;
}

// --stretch-level <key>=<value>: pull-throttle and pull-load in %,
// pull-hold, pull-gap and cruise-hold in seconds, pull-rise in rpm,
// pull-gain, cruise-speed-band and cruise-floor in km/h (mph with
// imperial, converted), and cruise-throttle-band in %; 0 or more, 0 the
// default
inline bool parse_stretch_level(const char* text, bool imperial,
                                ecuio_stretch_rules_t& r)
{
    const char* eq = std::strchr(text, '=');
    double v = 0;
    if (eq == nullptr || !parse_decimal(eq + 1, v) || v < 0 || v > 1e9) {
        return false;
    }
    const std::string key(text, static_cast<size_t>(eq - text));
    const double speed = imperial ? v * kKmhPerMph : v;
    const auto us = static_cast<uint64_t>(std::llround(v * 1e6));
    if (key == "pull-throttle") {
        r.pull_level = v;
    } else if (key == "pull-load") {
        r.pull_load_level = v;
    } else if (key == "pull-hold") {
        r.pull_hold_us = us;
    } else if (key == "pull-gap") {
        r.pull_gap_us = us;
    } else if (key == "pull-rise") {
        r.pull_min_rise = v;
    } else if (key == "pull-gain") {
        r.pull_min_gain = speed;
    } else if (key == "cruise-hold") {
        r.cruise_hold_us = us;
    } else if (key == "cruise-speed-band") {
        r.cruise_speed_band = speed;
    } else if (key == "cruise-throttle-band") {
        r.cruise_throttle_band = v;
    } else if (key == "cruise-floor") {
        r.cruise_floor = speed;
    } else {
        return false;
    }
    return true;
}

// --role <role>=<name>: throttle, load, speed or rpm, and a signal's name
inline bool parse_role(const char* text, uint32_t& role, std::string& name)
{
    const char* eq = std::strchr(text, '=');
    if (eq == nullptr || eq[1] == '\0') {
        return false;
    }
    const std::string key(text, static_cast<size_t>(eq - text));
    if (key == "throttle") {
        role = ECUIO_RECORDING_ROLE_THROTTLE;
    } else if (key == "load") {
        role = ECUIO_RECORDING_ROLE_ENGINE_LOAD;
    } else if (key == "speed") {
        role = ECUIO_RECORDING_ROLE_VEHICLE_SPEED;
    } else if (key == "rpm") {
        role = ECUIO_RECORDING_ROLE_ENGINE_SPEED;
    } else {
        return false;
    }
    name = eq + 1;
    return true;
}

// The rules' field for a role
inline uint32_t& role_field(ecuio_stretch_rules_t& r, uint32_t role)
{
    switch (role) {
    case ECUIO_RECORDING_ROLE_THROTTLE:
        return r.throttle;
    case ECUIO_RECORDING_ROLE_ENGINE_LOAD:
        return r.engine_load;
    case ECUIO_RECORDING_ROLE_VEHICLE_SPEED:
        return r.vehicle_speed;
    default:
        return r.engine_speed;
    }
}

inline void name_or_null(Line& l, const ecuio_recording_signal_t* s)
{
    if (s == nullptr) {
        l.raw("null");
    } else {
        l.string(s->name, sizeof s->name);
    }
}

// Each role's signal for a recording, and the speeds the stretches were
// sought with, in km/h: {"type":"roles", recording, throttle, engine_load,
// vehicle_speed, engine_speed (names, or null for none), cruise_floor,
// cruise_speed_band, pull_gain}. by_role holds the four in that order.
inline void roles_line(Line& l, uint32_t recording,
                       const ecuio_recording_signal_t* const by_role[4],
                       const ecuio_stretch_rules_t& r)
{
    l.raw("{\"type\":\"roles\"");
    l.key("recording");
    l.u64(recording);
    l.key("throttle");
    name_or_null(l, by_role[0]);
    l.key("engine_load");
    name_or_null(l, by_role[1]);
    l.key("vehicle_speed");
    name_or_null(l, by_role[2]);
    l.key("engine_speed");
    name_or_null(l, by_role[3]);
    l.key("cruise_floor");
    l.number(r.cruise_floor != 0 ? r.cruise_floor
                                 : ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR);
    l.key("cruise_speed_band");
    l.number(r.cruise_speed_band != 0
                 ? r.cruise_speed_band
                 : ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND);
    l.key("pull_gain");
    l.number(r.pull_min_gain != 0 ? r.pull_min_gain
                                  : ECUIO_STRETCH_DEFAULT_PULL_GAIN);
    l.ch('}');
}

inline void flag(Line& l, const char* key, uint32_t flags, uint32_t bit)
{
    l.key(key);
    l.raw((flags & bit) != 0 ? "true" : "false");
}

// A stretch found: {"type":"stretch", recording, kind pull|cruise,
// from_us, to_us (on the axis), length_us, signal (the one measured),
// start, end (a pull's two ends, a cruise's mean in both), by_load,
// by_engine_speed, no_throttle, cut_start, cut_end}
inline void stretch_line(Line& l, uint32_t recording,
                         const ecuio_recording_stretch_t& s,
                         const ecuio_recording_signal_t& measured,
                         int64_t offset_us)
{
    l.raw("{\"type\":\"stretch\"");
    l.key("recording");
    l.u64(recording);
    l.key("kind");
    l.raw(s.kind == ECUIO_RECORDING_STRETCH_PULL ? "\"pull\"" : "\"cruise\"");
    l.key("from_us");
    signed_us(l, static_cast<int64_t>(s.from_us) + offset_us);
    l.key("to_us");
    signed_us(l, static_cast<int64_t>(s.to_us) + offset_us);
    l.key("length_us");
    l.u64(s.to_us - s.from_us);
    l.key("signal");
    l.string(measured.name, sizeof measured.name);
    l.key("start");
    l.number(s.start);
    l.key("end");
    l.number(s.end);
    flag(l, "by_load", s.flags, ECUIO_RECORDING_STRETCH_BY_LOAD);
    flag(l, "by_engine_speed", s.flags,
         ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
    flag(l, "no_throttle", s.flags, ECUIO_RECORDING_STRETCH_NO_THROTTLE);
    flag(l, "cut_start", s.flags, ECUIO_RECORDING_STRETCH_CUT_START);
    flag(l, "cut_end", s.flags, ECUIO_RECORDING_STRETCH_CUT_END);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_VIEW_LINES_H
