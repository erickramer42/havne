// File/layer: cli/summary_lines.h | havne-cli, log's closing summary
// Testing: UNIT: CliSummaryTest (test_cli); CliLogRunTest (ctest)
//
// What a log reached (ADR-0056 section 7): values a second, sweeps a
// second, frames a second and each signal's interval, counted on core's
// worker from the values and frame callbacks into tables sized before the
// start, then built after the close joins the worker as one interval line
// for each signal and a summary line. Public headers only.
//
// The signals counted: the picks, when there are any (a passive bus or the
// mock decodes signals nobody picked), else every def of the set.
//
// The window: from the session time of the last first value, the moment
// every signal that answered at all had answered, to the last value. A
// first value starts a new epoch: the counts before it are set aside and
// each signal's tally restarts at its next value, so the connect, the
// supported-PID walk, identification and a scan stay out. An interval
// that spans the window's start is not counted. A late first value (a
// slow DID, a scan finding one late) shortens the window, which the
// summary says with short_window. An armed log's summary covers the
// whole session, not each window. Times are the batches' timestamp_us,
// the clock frames.csv's timestamp_us gives.
//
// Interval percentiles read at a bucket's top: eighth octaves from 1 ms
// to 2^26 us (67 s), so at most 12.5% above the true value and never
// above the maximum; the minimum and maximum are exact.

#ifndef HAVNE_CLI_SUMMARY_LINES_H
#define HAVNE_CLI_SUMMARY_LINES_H

#include "jsonl.h"
#include "key_field.h"

#include "ecuio/profile.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cli {

inline constexpr uint32_t kIntervalSubs = 8;      /* buckets an octave */
inline constexpr uint32_t kIntervalLowShift = 10;  /* 2^10 us, about 1 ms */
inline constexpr uint32_t kIntervalHighShift = 26; /* 2^26 us, about 67 s */
inline constexpr uint32_t kIntervalBuckets =
    (kIntervalHighShift - kIntervalLowShift) * kIntervalSubs;
inline constexpr uint16_t kNoSlot = 0xFFFF;

// An interval's bucket: below 2^10 us the first, from 2^26 us the last.
// The interval is held to that range first, so the octave is 10 to 25
inline uint32_t interval_bucket(uint64_t us)
{
    const uint64_t v =
        std::min(std::max(us, uint64_t{1} << kIntervalLowShift),
                 (uint64_t{1} << kIntervalHighShift) - 1);
    uint32_t e = kIntervalHighShift - 1;
    while (e > kIntervalLowShift && (v >> e) == 0) {
        --e;
    }
    const uint32_t sub =
        static_cast<uint32_t>(v >> (e - 3)) & (kIntervalSubs - 1);
    return (e - kIntervalLowShift) * kIntervalSubs + sub;
}

// A bucket's top, the first interval past it, in us
inline uint64_t interval_bucket_top(uint32_t i)
{
    const uint32_t e = kIntervalLowShift + i / kIntervalSubs;
    return uint64_t{kIntervalSubs + i % kIntervalSubs + 1} << (e - 3);
}

// One signal's tally since its epoch's start
struct SignalTally {
    uint64_t values = 0; /* in the window */
    uint64_t intervals = 0;
    uint64_t last_us = 0;
    uint64_t min_us = 0;
    uint64_t max_us = 0;
    uint32_t epoch = 0; /* 0: never answered */
    uint32_t buckets[kIntervalBuckets] = {};
};

// The tallies of a set's defs, the table from a def's id to its slot built
// before the start: the worker's calls allocate nothing. With picks, only
// the picks' ids have a slot
class SummaryTally
{
public:
    explicit SummaryTally(const ecuio_signal_set_t* set,
                          const std::vector<uint16_t>& picks = {})
        : m_slot(size_t{0xFFFF} + 1, kNoSlot)
    {
        const uint32_t n = set != nullptr && set->defs != nullptr
                               ? std::min<uint32_t>(set->count, kNoSlot)
                               : 0;
        m_tally.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            const uint16_t id = set->defs[i].id;
            if (picks.empty() ||
                std::find(picks.begin(), picks.end(), id) != picks.end()) {
                m_slot[id] = static_cast<uint16_t>(i);
            }
        }
    }

    // Core's worker: one frame's values. A frame holding a first value
    // (a pack brings several) starts the window before any of its values
    // is counted, so all of them are in it
    void on_batch(const ecuio_value_batch_t& b)
    {
        bool first = false;
        for (uint32_t i = 0; i < b.count && !first; ++i) {
            const uint16_t slot = m_slot[b.values[i].signal_id];
            first = slot != kNoSlot && m_tally[slot].epoch == 0;
        }
        if (first) {
            start_window(b.timestamp_us);
        }
        for (uint32_t i = 0; i < b.count; ++i) {
            const uint16_t slot = m_slot[b.values[i].signal_id];
            if (slot != kNoSlot) {
                on_value(m_tally[slot], b.timestamp_us);
            }
        }
    }

    // Core's worker: one frame, values or not
    void on_frame() { ++m_frames; }

    const std::vector<SignalTally>& tallies() const { return m_tally; }
    const SignalTally* tally_of(uint16_t id) const
    {
        return m_slot[id] == kNoSlot ? nullptr : &m_tally[m_slot[id]];
    }
    uint32_t epoch() const { return m_epoch; }
    uint64_t first_us() const { return m_first_us; }
    uint64_t from_us() const { return m_from_us; }
    uint64_t to_us() const { return m_to_us; }
    uint64_t window_values() const { return m_values - m_values_at; }
    uint64_t window_frames() const { return m_frames - m_frames_at; }

private:
    void on_value(SignalTally& t, uint64_t ts)
    {
        /* Each channel's clock is monotonic, not one against another */
        m_first_us = m_values == 0 ? ts : std::min(m_first_us, ts);
        m_to_us = std::max(m_to_us, ts);
        ++m_values;
        if (t.epoch != m_epoch) {
            restart(t, ts);
            return;
        }
        const uint64_t gap = ts > t.last_us ? ts - t.last_us : 0;
        ++t.buckets[interval_bucket(gap)];
        t.min_us = t.intervals == 0 ? gap : std::min(t.min_us, gap);
        t.max_us = std::max(t.max_us, gap);
        ++t.intervals;
        ++t.values;
        t.last_us = ts;
    }

    // The window starts again at ts, the frame bringing it counted in it
    // (the frame callback runs before the values)
    void start_window(uint64_t ts)
    {
        ++m_epoch;
        m_from_us = ts;
        m_values_at = m_values;
        m_frames_at = m_frames > 0 ? m_frames - 1 : 0;
    }

    void restart(SignalTally& t, uint64_t ts)
    {
        std::memset(t.buckets, 0, sizeof t.buckets);
        t.values = 1;
        t.intervals = 0;
        t.min_us = 0;
        t.max_us = 0;
        t.last_us = ts;
        t.epoch = m_epoch;
    }

    std::vector<uint16_t> m_slot;
    std::vector<SignalTally> m_tally;
    uint32_t m_epoch = 0;
    uint64_t m_values = 0;
    uint64_t m_values_at = 0;
    uint64_t m_frames = 0;
    uint64_t m_frames_at = 0;
    uint64_t m_first_us = 0;
    uint64_t m_from_us = 0;
    uint64_t m_to_us = 0;
};

// The interval at quantile q (0.5, 0.99) of a tally with intervals: the
// bucket's top holding that rank, held to the exact maximum
inline uint64_t interval_at(const SignalTally& t, double q)
{
    const uint64_t rank = std::max<uint64_t>(
        1,
        static_cast<uint64_t>(std::ceil(q * static_cast<double>(t.intervals))));
    uint64_t seen = 0;
    for (uint32_t i = 0; i < kIntervalBuckets; ++i) {
        seen += t.buckets[i];
        if (seen >= rank) {
            return std::min(interval_bucket_top(i), t.max_us);
        }
    }
    return t.max_us;
}

// Whether a tally belongs to the window (it answered since its start)
inline bool in_window(const SignalTally& t, uint32_t epoch)
{
    return epoch != 0 && t.epoch == epoch;
}

inline void ms_field(Line& l, const char* name, uint64_t us, bool known)
{
    l.key(name);
    if (known) {
        l.number(static_cast<double>(us) / 1000.0);
    } else {
        l.raw("null");
    }
}

// A rate or a time in seconds, to a thousandth
inline void thousandths(Line& l, double v)
{
    l.number(std::isfinite(v) ? std::round(v * 1000.0) / 1000.0 : v);
}

inline size_t interval_line_bytes()
{
    return 256 + sizeof(ecuio_signal_def_t::name) * 6 + kKeyFieldBytes;
}

// {"type":"interval", signal, key, values, p50_ms, p99_ms, min_ms,
// max_ms}: one signal's figures in the window; the times null for a
// signal with one value in it
inline void interval_line(Line& l, const ecuio_signal_def_t& d, const char* key,
                          size_t key_size, const SignalTally& t)
{
    const bool known = t.intervals > 0;
    l.raw("{\"type\":\"interval\"");
    l.key("signal");
    l.string(d.name, sizeof d.name);
    key_field(l, key, key_size);
    l.key("values");
    l.u64(t.values);
    ms_field(l, "p50_ms", known ? interval_at(t, 0.5) : 0, known);
    ms_field(l, "p99_ms", known ? interval_at(t, 0.99) : 0, known);
    ms_field(l, "min_ms", t.min_us, known);
    ms_field(l, "max_ms", t.max_us, known);
    l.ch('}');
}

// The summary line's figures, from the tallies once the worker is done
struct SummaryFigures {
    uint32_t signals = 0; /* answered in the window */
    uint32_t single = 0;  /* of those, one value only */
    uint32_t silent = 0;  /* picks that never answered */
    uint64_t median_p50_us = 0;
    uint64_t worst_p50_us = 0;
    uint64_t worst_p99_us = 0;
    int64_t worst = -1; /* the worst P50's slot */
};

inline SummaryFigures summary_figures(const SummaryTally& s,
                                      const std::vector<uint16_t>& picks)
{
    SummaryFigures f;
    for (uint16_t id : picks) {
        const SignalTally* t = s.tally_of(id);
        if (t != nullptr && t->epoch == 0) {
            ++f.silent;
        }
    }
    std::vector<uint64_t> p50s;
    const std::vector<SignalTally>& all = s.tallies();
    for (size_t i = 0; i < all.size(); ++i) {
        const SignalTally& t = all[i];
        if (!in_window(t, s.epoch())) {
            continue;
        }
        ++f.signals;
        if (t.intervals == 0) {
            ++f.single;
            continue;
        }
        const uint64_t p50 = interval_at(t, 0.5);
        p50s.push_back(p50);
        if (f.worst < 0 || p50 > f.worst_p50_us) {
            f.worst_p50_us = p50;
            f.worst = static_cast<int64_t>(i);
        }
        f.worst_p99_us = std::max(f.worst_p99_us, interval_at(t, 0.99));
    }
    if (!p50s.empty()) {
        std::sort(p50s.begin(), p50s.end());
        f.median_p50_us = p50s[(p50s.size() - 1) / 2];
    }
    return f;
}

inline size_t summary_line_bytes()
{
    return 640 + sizeof(ecuio_signal_def_t::name) * 6 + kKeyFieldBytes;
}

// {"type":"summary", from_s, span_s, from_us, to_us, short_window,
// signals, single, silent, values, values_per_s, sweeps_per_s,
// frames_per_s, p50_ms, worst_p50_ms, worst, worst_p99_ms}: from_s is the
// window's start in seconds after the first value, from_us and to_us its
// ends on frames.csv's timestamp_us clock; short_window when it is under
// half of the first value to the last; sweeps_per_s is values_per_s over
// the signals answering; p50_ms the median of their P50s; single the
// signals with one value in it, left out of the interval figures. silent,
// only with picks: the picks that never answered. A null rate for a
// window of no time
inline void summary_line(Line& l, const ecuio_signal_set_t* set,
                         const std::vector<ecuio_signal_key_t>& keys,
                         const std::vector<uint16_t>& picks,
                         const SummaryTally& s)
{
    const SummaryFigures f = summary_figures(s, picks);
    const uint64_t span = s.to_us() - s.from_us();
    const uint64_t whole = s.to_us() - s.first_us();
    const double secs = static_cast<double>(span) / 1e6;
    const auto rate = [&](const char* name, double n) {
        l.key(name);
        thousandths(l, span > 0 ? n / secs : NAN);
    };
    l.raw("{\"type\":\"summary\"");
    l.key("from_s");
    thousandths(l, static_cast<double>(s.from_us() - s.first_us()) / 1e6);
    l.key("span_s");
    thousandths(l, secs);
    l.key("from_us");
    l.u64(s.from_us());
    l.key("to_us");
    l.u64(s.to_us());
    if (s.epoch() != 0 && span * 2 < whole) {
        l.key("short_window");
        l.raw("true");
    }
    l.key("signals");
    l.u64(f.signals);
    l.key("single");
    l.u64(f.single);
    if (!picks.empty()) {
        l.key("silent");
        l.u64(f.silent);
    }
    l.key("values");
    l.u64(s.window_values());
    rate("values_per_s", static_cast<double>(s.window_values()));
    rate("sweeps_per_s",
         f.signals > 0 ? static_cast<double>(s.window_values()) / f.signals
                       : NAN);
    rate("frames_per_s", static_cast<double>(s.window_frames()));
    const bool known = f.worst >= 0;
    ms_field(l, "p50_ms", f.median_p50_us, known);
    ms_field(l, "worst_p50_ms", f.worst_p50_us, known);
    l.key("worst");
    if (known) {
        const auto w = static_cast<size_t>(f.worst);
        l.raw("{");
        l.raw("\"signal\":");
        l.string(set->defs[w].name, sizeof set->defs[w].name);
        key_field(l, w < keys.size() ? keys[w].text : nullptr,
                  sizeof(ecuio_signal_key_t::text));
        l.ch('}');
    } else {
        l.raw("null");
    }
    ms_field(l, "worst_p99_ms", f.worst_p99_us, known);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_SUMMARY_LINES_H
