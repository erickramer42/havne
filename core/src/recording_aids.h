// File/layer: core/src/recording_aids.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: RecordingAidsTest, RecordingStretchesTest (test_core)
//
// The navigation aids' rules (ADR-0056's appendix "the navigation aids"):
// pulls and cruises found in a recording's samples. Pure: the reader
// (ecuio_recording_stretches) feeds each role signal's samples in time
// order, one more on each side of the range for the cut flags, so the
// rules are tested over plain vectors.

#ifndef ECUIO_RECORDING_AIDS_H
#define ECUIO_RECORDING_AIDS_H

#include "ecuio/recording.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ecuio_aids {

struct sample {
    uint64_t us;
    double value;
};
using samples = std::vector<sample>;

/* A stretch as found: its kind, its times and its measured signal's
 * values (a pull's at its two ends, a cruise's mean speed in both) */
struct stretch {
    uint32_t kind;
    uint64_t from_us;
    uint64_t to_us;
    double start;
    double end;
    uint32_t flags;
};

/* The value in force at t: the last sample at or before it; NaN with
 * none */
inline double in_force(const samples& s, uint64_t t)
{
    auto it =
        std::upper_bound(s.begin(), s.end(), t,
                         [](uint64_t v, const sample& x) { return v < x.us; });
    return it == s.begin() ? NAN : (it - 1)->value;
}

/* The runs of gate's samples at or above level, as [first, last] sample
 * indexes. A dip under the level is kept inside its run when the next
 * sample at or above the level comes less than gap_us after the dip's
 * first sample. */
inline std::vector<std::pair<size_t, size_t>>
runs_at_or_above(const samples& gate, double level, uint64_t gap_us)
{
    std::vector<std::pair<size_t, size_t>> out;
    bool in_run = false;
    bool dipping = false;
    size_t first = 0;
    size_t last = 0;
    uint64_t dip_us = 0;
    for (size_t i = 0; i < gate.size(); ++i) {
        const bool high = gate[i].value >= level; /* NaN is never high */
        if (!in_run) {
            if (high) {
                in_run = true;
                dipping = false;
                first = last = i;
            }
            continue;
        }
        if (high) {
            if (dipping && gate[i].us - dip_us >= gap_us) {
                out.emplace_back(first, last);
                first = i;
            }
            dipping = false;
            last = i;
            continue;
        }
        /* A dip past the gap ends its run at the next sample at the level */
        if (!dipping) {
            dipping = true;
            dip_us = gate[i].us;
        }
    }
    if (in_run) {
        out.emplace_back(first, last);
    }
    return out;
}

struct pull_rules {
    double level;     /* the gate's: throttle or engine load, % */
    uint64_t hold_us; /* the shortest pull */
    uint64_t gap_us;  /* the longest dip kept inside one */
    double tolerance; /* how far under its highest a trimmed pull's
                         measure may sit and still be at it */
    double min_rise;  /* engine speed's least climb in a pull, rpm */
    double min_gain;  /* a speed measure's least gain with no engine
                         speed, in its unit */
};

/* The most a sample of s in (from, to] stands above an earlier one, the
 * value in force at from counted; 0 with no climb. A NaN is passed by. */
inline double largest_climb(const samples& s, uint64_t from, uint64_t to)
{
    double lowest = in_force(s, from);
    double climb = 0.0;
    auto it =
        std::upper_bound(s.begin(), s.end(), from,
                         [](uint64_t v, const sample& x) { return v < x.us; });
    for (; it != s.end() && it->us <= to; ++it) {
        if (std::isnan(it->value)) {
            continue;
        }
        if (std::isnan(lowest) || it->value < lowest) {
            lowest = it->value;
        } else {
            climb = (std::max)(climb, it->value - lowest);
        }
    }
    return climb;
}

/* Whether [from, to] rises as a pull must, *start and *end the measure in
 * force at its ends: a speed measure (trim) ends above its start; engine
 * speed climbs min_rise inside it, or with no engine speed the measure
 * gains min_gain. A NaN at either end is no rise. */
inline bool pull_rises(const samples& measure, bool trim,
                       const samples* engine_speed, const pull_rules& rules,
                       uint64_t from, uint64_t to, double* start, double* end)
{
    *start = in_force(measure, from);
    *end = in_force(measure, to);
    if (std::isnan(*start) || std::isnan(*end) || (trim && !(*end > *start))) {
        return false;
    }
    if (engine_speed != nullptr) {
        return largest_climb(*engine_speed, from, to) >= rules.min_rise;
    }
    return *end - *start >= rules.min_gain;
}

/* Pulls: gate's runs at or above the level that rise as pull_rises asks.
 * With trim (a vehicle-speed measure) a run ends at the measure's first
 * sample within tolerance of its highest in the run, so the limiter's
 * flat speed ends a pull; an engine-speed measure keeps the whole run,
 * since an upshift's drop would end it at the first shift. engine_speed
 * is NULL for a recording with none. */
inline void find_pulls(const samples& gate, const samples& measure, bool trim,
                       const samples* engine_speed, const pull_rules& rules,
                       uint32_t flags, std::vector<stretch>& out)
{
    for (const auto& run : runs_at_or_above(gate, rules.level, rules.gap_us)) {
        const uint64_t from = gate[run.first].us;
        uint64_t to = gate[run.second].us;
        if (trim) {
            auto lo = std::lower_bound(
                measure.begin(), measure.end(), from,
                [](const sample& x, uint64_t v) { return x.us < v; });
            auto hi = std::upper_bound(
                measure.begin(), measure.end(), to,
                [](uint64_t v, const sample& x) { return v < x.us; });
            double best = NAN;
            for (auto it = lo; it != hi; ++it) {
                if (!std::isnan(it->value) &&
                    (std::isnan(best) || it->value > best)) {
                    best = it->value;
                }
            }
            if (!std::isnan(best)) {
                for (auto it = lo; it != hi; ++it) {
                    if (it->value >= best - rules.tolerance) {
                        to = it->us;
                        break;
                    }
                }
            }
        }
        if (to - from < rules.hold_us) {
            continue;
        }
        double start = NAN;
        double end = NAN;
        if (!pull_rises(measure, trim, engine_speed, rules, from, to, &start,
                        &end)) {
            continue;
        }
        out.push_back(
            stretch{ECUIO_RECORDING_STRETCH_PULL, from, to, start, end, flags});
    }
}

struct cruise_rules {
    uint64_t hold_us;     /* the shortest cruise */
    double speed_band;    /* in the speed signal's unit */
    double throttle_band; /* % */
    double floor;         /* in the speed signal's unit */
};

/* Cruises: runs of samples from a speed sample at or above the floor, in
 * which speed stays within speed_band and throttle (when there is one)
 * within throttle_band. The two signals' samples are taken in time
 * order; a run ends at the first sample that would put a band past its
 * width or speed under the floor, and the next may start at that speed
 * sample (a throttle sample's break waits for the next speed sample). A
 * run's throttle band starts from the throttle in force where it starts.
 * Its value is the mean of its speed samples. */
inline void find_cruises(const samples& speed, const samples* throttle,
                         const cruise_rules& rules, uint32_t flags,
                         std::vector<stretch>& out)
{
    bool open = false;
    uint64_t from = 0;
    uint64_t last = 0;
    double smin = 0;
    double smax = 0;
    double ssum = 0;
    uint64_t sn = 0;
    double tmin = NAN;
    double tmax = NAN;
    const auto close = [&] {
        if (open && last - from >= rules.hold_us && sn != 0) {
            const double mean = ssum / static_cast<double>(sn);
            out.push_back(stretch{ECUIO_RECORDING_STRETCH_CRUISE, from, last,
                                  mean, mean, flags});
        }
        open = false;
    };
    const auto start_at = [&](const sample& x) {
        open = true;
        from = last = x.us;
        smin = smax = ssum = x.value;
        sn = 1;
        tmin = tmax = throttle != nullptr ? in_force(*throttle, x.us) : NAN;
    };
    size_t i = 0;
    size_t j = 0;
    const size_t tn = throttle != nullptr ? throttle->size() : 0;
    while (i < speed.size() || j < tn) {
        /* At one time, the speed sample first */
        const bool take_speed =
            j >= tn || (i < speed.size() && speed[i].us <= (*throttle)[j].us);
        if (take_speed) {
            const sample& x = speed[i++];
            if (!(x.value >= rules.floor)) { /* NaN is under it too */
                close();
                continue;
            }
            if (!open) {
                start_at(x);
                continue;
            }
            const double lo = (std::min)(smin, x.value);
            const double hi = (std::max)(smax, x.value);
            if (hi - lo > rules.speed_band) {
                close();
                start_at(x);
                continue;
            }
            smin = lo;
            smax = hi;
            ssum += x.value;
            ++sn;
            last = x.us;
            continue;
        }
        const sample& y = (*throttle)[j++];
        if (!open || std::isnan(y.value)) {
            continue;
        }
        if (std::isnan(tmin)) {
            tmin = tmax = y.value;
            last = y.us;
            continue;
        }
        const double lo = (std::min)(tmin, y.value);
        const double hi = (std::max)(tmax, y.value);
        if (hi - lo > rules.throttle_band) {
            close();
            continue;
        }
        tmin = lo;
        tmax = hi;
        last = y.us;
    }
    close();
}

/* The stretches cut to [from_us, to_us], which the samples fed reach
 * past by a sample on each side: one reaching past an edge is cut there
 * and flagged CUT_START or CUT_END, its values taken again over what is
 * left (a pull's ends, a cruise's mean), and dropped when what is left is
 * shorter than its hold or, for a pull, no longer rises as pull_rises
 * asks (trim and engine_speed as find_pulls took them) */
inline void cut_to(std::vector<stretch>& found, uint64_t from_us,
                   uint64_t to_us, const pull_rules& pull_rule, bool trim,
                   const samples* engine_speed, uint64_t cruise_hold_us,
                   const samples& pull_measure, const samples& speed)
{
    std::vector<stretch> kept;
    for (stretch s : found) {
        if (s.to_us < from_us || s.from_us > to_us) {
            continue;
        }
        const bool cut = s.from_us < from_us || s.to_us > to_us;
        if (s.from_us < from_us) {
            s.from_us = from_us;
            s.flags |= ECUIO_RECORDING_STRETCH_CUT_START;
        }
        if (s.to_us > to_us) {
            s.to_us = to_us;
            s.flags |= ECUIO_RECORDING_STRETCH_CUT_END;
        }
        const bool pull = s.kind == ECUIO_RECORDING_STRETCH_PULL;
        if (s.to_us - s.from_us < (pull ? pull_rule.hold_us : cruise_hold_us)) {
            continue;
        }
        if (cut && pull) {
            if (!pull_rises(pull_measure, trim, engine_speed, pull_rule,
                            s.from_us, s.to_us, &s.start, &s.end)) {
                continue;
            }
        } else if (cut) {
            double sum = 0;
            uint64_t n = 0;
            for (const sample& x : speed) {
                if (x.us >= s.from_us && x.us <= s.to_us) {
                    sum += x.value;
                    ++n;
                }
            }
            if (n == 0) {
                continue;
            }
            s.start = s.end = sum / static_cast<double>(n);
        }
        kept.push_back(s);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const stretch& a, const stretch& b) {
                         return a.from_us < b.from_us;
                     });
    found.swap(kept);
}

} // namespace ecuio_aids

#endif
