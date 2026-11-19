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
#include "key_field.h"
#include "view_args.h"
#include "view_axis.h"

#include "ecuio/conditions.h"
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

// The opened line's format: core's for formats 2, 3 and 4 alike (format 3
// keeps format 2's shape, its enums seen on their signal lines, and format
// 4 keeps a signal under its key), so schema 1's word holds
inline const char* format_word(uint32_t format)
{
    switch (format) {
    case ECUIO_RECORDING_FORMAT_CORE:
    case ECUIO_RECORDING_FORMAT_ENUMS:
    case ECUIO_RECORDING_FORMAT_KEYS:
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
// records_dropped, notes_dropped, signal_file, conditions}; signal_file
// is null when the header names none and the folder holds none, else
// {sha256 (the header's, or null), present, matches} (ADR-0051 section
// 4); conditions is null when the header names no set, the folder holds
// none and events.log has no cond line, else {sha256|null, present,
// matches, events (every cond line that parses), lines_skipped, cut (more
// than the reader keeps), events_lost (the events the recorder had no
// room for and never wrote)} (ADR-0063 section 8); and with view --frames,
// frames: the profile its frames decoded through, or "signals.json" for
// the recording's own file
inline void opened_line(Line& l, uint32_t recording,
                        const ecuio_recording_info_t& i,
                        const ecuio_recording_signals_t& sf,
                        const ecuio_recording_conditions_t& cs,
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
    l.key("conditions");
    if (cs.flags == 0 && i.condition_events == 0 && cs.lines_skipped == 0 &&
        cs.events_lost == 0) {
        l.raw("null");
    } else {
        l.ch('{');
        l.key("sha256");
        if ((cs.flags & ECUIO_RECORDING_CONDITIONS_NAMED) != 0) {
            l.string(cs.sha256, sizeof cs.sha256);
        } else {
            l.raw("null");
        }
        l.key("present");
        l.raw((cs.flags & ECUIO_RECORDING_CONDITIONS_PRESENT) != 0 ? "true"
                                                                   : "false");
        l.key("matches");
        l.raw((cs.flags & ECUIO_RECORDING_CONDITIONS_MATCH) != 0 ? "true"
                                                                 : "false");
        l.key("events");
        l.u64(i.condition_events);
        l.key("lines_skipped");
        l.u64(cs.lines_skipped);
        l.key("cut");
        l.raw((i.flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT) != 0 ? "true"
                                                                   : "false");
        l.key("events_lost");
        l.u64(cs.events_lost);
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

// A signal: {"type":"signal", recording, index, name, key, unit, text, then
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
    key_field(l, s.key, sizeof s.key);
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

// One cond line of a recording: {"type":"condition", recording, host_us,
// t_us, key, state (on|off), fired, stale, held, value, term}: stream's
// condition line with the recording's time and held added. Fits a buffer
// of kRecordedConditionLineBytes.
constexpr size_t kRecordedConditionLineBytes =
    320 + 6 * sizeof(ecuio_recording_condition_event_t{}.key);

inline void recorded_condition_line(Line& l, uint32_t recording,
                                    const ecuio_recording_condition_event_t& e)
{
    l.raw("{\"type\":\"condition\"");
    l.key("recording");
    l.u64(recording);
    l.key("host_us");
    l.u64(e.host_us);
    l.key("t_us");
    l.u64(e.timestamp_us);
    l.key("key");
    l.string(e.key, sizeof e.key);
    l.key("state");
    l.raw(e.state == ECUIO_CONDITION_ON ? "\"on\"" : "\"off\"");
    l.key("fired");
    l.raw((e.flags & ECUIO_CONDITION_FIRED) != 0 ? "true" : "false");
    l.key("stale");
    l.raw((e.flags & ECUIO_CONDITION_STALE) != 0 ? "true" : "false");
    l.key("held");
    l.raw((e.flags & ECUIO_CONDITION_HELD) != 0 ? "true" : "false");
    l.key("value");
    l.number(e.value);
    l.key("term");
    l.u64(e.term);
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

// The bytes a columns line can take: a column's five entries at most
// 26 bytes each, a name escaped and a key
inline size_t columns_line_bytes(uint32_t columns)
{
    return 512 + kKeyFieldBytes + static_cast<size_t>(columns) * 5 * 27;
}

// One signal's columns on the axis: {"type":"columns", recording, signal,
// key, samples [], and a number's lowest [], highest [] and mean [], null
// where a column is empty; then, for a recording after the first aligned
// one that shows the signal's pair (cli/signal_pairs.h) as a number in
// the same unit, delta_from (that first's place) and delta [], its mean
// less the first's, null where either has no mean}. A column's numbers are all
// over one set of samples: the ones a query counts there, or under
// columns_own_window the ones in the column's own microseconds; the last
// column's include a sample exactly at the axis's end. A
// column one microsecond wide holding three or more unlike samples has no
// mean (the reader's statistics take no span of one instant).
// cols holds w.count columns, the axis's from w.first on; the
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
    key_field(l, s.key, sizeof s.key);
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
// {"type":"at", recording, t_us, values [{signal, key, host_us|null, text}]},
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
    key_field(l, s.key, sizeof s.key);
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
// {"type":"stats", recording, signal, key, from_us, to_us, samples, first,
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
    key_field(l, s.key, sizeof s.key);
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

// A failed read of an open recording, as its error line's detail: the
// changed file's words for that code, NULL (core's own detail) otherwise
inline const char* read_error_detail(ecuio_status_t rc)
{
    return rc == ECUIO_ERR_RECORDING_CHANGED
               ? "the recording changed while it was read"
               : nullptr;
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
// cruise_speed_band, pull_gain, pull_end lift|top-speed}. by_role holds
// the four in that order.
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
    l.key("pull_end");
    l.raw((r.flags & ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED) != 0
              ? "\"top-speed\""
              : "\"lift\"");
    l.ch('}');
}

inline void flag(Line& l, const char* key, uint32_t flags, uint32_t bit)
{
    l.key(key);
    l.raw((flags & bit) != 0 ? "true" : "false");
}

// A stretch found: {"type":"stretch", recording, kind pull|cruise,
// from_us, to_us (on the axis), length_us, signal (the one measured) and
// its key, start, end (a pull's two ends, a cruise's mean in both), by_load,
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
    key_field(l, measured.key, sizeof measured.key);
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

