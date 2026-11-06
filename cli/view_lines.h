// File/layer: cli/view_lines.h | havne-cli, a recording's view as lines (ADR-0049 section 5)
// Testing: UNIT (tests/test_cli.cpp, CliViewTest: --align-on's forms and
//          refusals, seconds, the axis and a recording's window on it, each
//          line's fields); the command in the golden runs
//          (tests/golden/cli/view-*.jsonl, on tests/fixtures/recording/)
//
// `havne-cli view`'s lines, read through the reader (recording.h): what
// each recording holds, where an --align-on puts it, then each signal's
// lowest, highest and count per column on one axis, the min/max shape a
// chart draws. Times named host_us (and a signal's first_us and last_us)
// are the recording's own; the axis's (from_us, to_us, an at line's t_us)
// are host_us plus the recording's offset_us, so aligned recordings share
// them.

#ifndef HAVNE_CLI_VIEW_LINES_H
#define HAVNE_CLI_VIEW_LINES_H

#include "jsonl.h"

#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cli {

// --align-on: rise:<signal>:<level>, fall:<signal>:<level>,
// marker:<label> or utc. The level is the last colon's: a signal's name
// may hold one.
struct AlignOn {
    enum Kind { none, rise, fall, marker, utc };
    Kind kind = none;
    std::string signal;
    std::string label;
    double level = 0;
};

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
    } else if (s.rfind("marker:", 0) == 0 && s.size() > 7) {
        a.kind = AlignOn::marker;
        a.label = s.substr(7);
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
// records_dropped, notes_dropped}
inline void opened_line(Line& l, uint32_t recording,
                        const ecuio_recording_info_t& i)
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
        l.ch('{');
        for (uint32_t k = 0; k < label_count; ++k) {
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

// The bytes a columns line can take: a column's three entries at most
// 26 bytes each, and a name escaped
inline size_t columns_line_bytes(uint32_t columns)
{
    return 512 + static_cast<size_t>(columns) * 3 * 27;
}

// One signal's columns on the axis: {"type":"columns", recording, signal,
// samples [], and a number's lowest [] and highest [], null where a column
// is empty}. cols holds w.count columns, the axis's from w.first on; the
// others are empty. NULL cols: every column empty.
inline void columns_line(Line& l, uint32_t recording,
                         const ecuio_recording_signal_t& s, const Window& w,
                         uint32_t columns, const ecuio_recording_column_t* cols)
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

} // namespace cli

#endif // HAVNE_CLI_VIEW_LINES_H
