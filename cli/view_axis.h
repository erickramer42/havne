// File/layer: cli/view_axis.h | havne-cli, `view`'s axis and columns (ADR-0049 section 5)
// Testing: UNIT: CliViewTest (test_cli)
//
// The axis columns lie on, a recording's part of it (Window) and the column
// and span statistics arithmetic over the reader's results. No lines are
// written here (view_lines.h).

#ifndef HAVNE_CLI_VIEW_AXIS_H
#define HAVNE_CLI_VIEW_AXIS_H

#include "ecuio/recording.h"

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

// The axis: `columns` columns of column_us each from from_us, to_us
// rounded out to a whole column, so every recording's columns start on
// the same microseconds
struct Axis {
    int64_t from_us = 0;
    int64_t to_us = 0;
    int64_t column_us = 0;
    uint32_t columns = 0;
};

// The axis: columns of column_us from from_us, each holding its own
// microseconds and the last the axis's last one, to_us, too, so a sample
// exactly at the end is counted once
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

// Whether a view's columns each hold the column's own microseconds, not
// the whole seconds a query places there: an aligned view (any
// --align-on) in columns of a second or more, where two recordings'
// offsets a part of a second apart would put one axis column on two
// windows
inline bool columns_own_window(bool aligned_view, const Axis& a)
{
    return aligned_view && a.column_us >= 1000000;
}

// A span's statistics as the column a query over it would give
inline ecuio_recording_column_t
column_of_stats(const ecuio_recording_stats_t& st)
{
    ecuio_recording_column_t col{};
    col.samples =
        static_cast<uint32_t>((std::min<uint64_t>)(st.samples, UINT32_MAX));
    col.lowest = st.lowest;
    col.highest = st.highest;
    return col;
}

// Whether the window's queries leave the axis's last microsecond out of
// the last column: a half-open query does, except one in columns of a
// second or more that ends inside a second, which takes that second whole
inline bool query_leaves_end(const Window& w, const Axis& a)
{
    const bool rest = w.count > (w.partial ? 1u : 0u);
    const uint64_t width =
        rest ? static_cast<uint64_t>(a.column_us) : w.partial_t1_us;
    return width < 1000000 || w.t1_us % 1000000 == 0;
}

// One more column's samples counted into a column
inline void add_column(ecuio_recording_column_t& into,
                       const ecuio_recording_column_t& more)
{
    if (more.samples == 0) {
        return;
    }
    if (into.samples == 0) {
        into = more;
        return;
    }
    into.samples = static_cast<uint32_t>((
        std::min<uint64_t>)(uint64_t{into.samples} + more.samples, UINT32_MAX));
    into.lowest = std::fmin(into.lowest, more.lowest);
    into.highest = std::fmax(into.highest, more.highest);
}

// Axis column c in a recording's own time, cut at its start: the closed
// span [from_us, to_us], the last column's taking the axis's last
// microsecond. False for a column that ends before the start.
inline bool own_span(const Axis& a, int64_t offset_us, uint32_t c,
                     uint64_t& from_us, uint64_t& to_us)
{
    const int64_t lo =
        a.from_us + static_cast<int64_t>(c) * a.column_us - offset_us;
    const int64_t hi = lo + a.column_us - (c + 1 == a.columns ? 0 : 1);
    if (hi < 0) {
        return false;
    }
    from_us = static_cast<uint64_t>((std::max<int64_t>)(lo, 0));
    to_us = static_cast<uint64_t>(hi);
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

// A span's statistics with nothing in it: before a recording's start
inline ecuio_recording_stats_t no_stats()
{
    ecuio_recording_stats_t st{};
    st.first = st.last = st.lowest = st.highest = st.mean = NAN;
    st.lowest_us = st.highest_us = ECUIO_RECORDING_NONE;
    return st;
}

// The statistics of one microsecond, a span ecuio_recording_stats
// refuses, from that microsecond's column: the count and the extremes
// (their time at_us), a mean where mean_of_column gives one, and first
// and last only where the samples are alike, their order being untold
inline ecuio_recording_stats_t
stats_of_instant(const ecuio_recording_column_t& col, bool text, uint64_t at_us)
{
    ecuio_recording_stats_t st = no_stats();
    st.samples = col.samples;
    if (text || col.samples == 0) {
        return st;
    }
    st.lowest = col.lowest;
    st.highest = col.highest;
    st.lowest_us = st.highest_us = at_us;
    double mean = NAN;
    if (mean_of_column(col, mean)) {
        st.mean = mean;
    }
    if (col.lowest == col.highest) {
        st.first = st.last = col.lowest;
    }
    return st;
}

} // namespace cli

#endif // HAVNE_CLI_VIEW_AXIS_H
