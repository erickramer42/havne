// File/layer: fuzz/fuzz_recording.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (rows, a folder, the stretches' rules)

#include "fuzz_recording_common.h"
#include "fuzz_recording_lines.h"
#include "fuzz_recording_conditions.h"
#include "fuzz_recording_query.h"
#include "fuzz_recording_reads.h"

namespace {

/* --- The stretches' rules (recording_aids.h), an input starting 'S' ---
 * After it: a byte of switches, ten of rules, two of range, three
 * sample counts, then two bytes a sample (a time step and a value) for
 * the gate, the speed and the engine speed in turn. Times never go back,
 * as the reader feeds them; values reach NaN, both infinities, -0.0, the
 * double's edges, the floor and the level themselves. */

double rule_value(uint8_t b)
{
    /* What the reader passes on: 0 or more, never NaN, inf allowed */
    static const double kRules[] = {0.0,
                                    0.5,
                                    1.0,
                                    2.0,
                                    3.0,
                                    50.0,
                                    56.32704,
                                    80.0,
                                    100.0,
                                    1e300,
                                    INFINITY,
                                    4.9e-324,
                                    2.2250738585072014e-308,
                                    750.0,
                                    10.0};
    return kRules[b % (sizeof kRules / sizeof kRules[0])];
}

uint64_t rule_us(uint8_t b)
{
    static const uint64_t kUs[] = {
        0, 1, 1000, 100000, 500000, 1000000, 10000000, UINT64_MAX, 1500000};
    return kUs[b % (sizeof kUs / sizeof kUs[0])];
}

double sample_value(uint8_t b)
{
    static const double kEdges[] = {NAN,
                                    INFINITY,
                                    -INFINITY,
                                    -0.0,
                                    1.7976931348623157e308,
                                    -1.7976931348623157e308,
                                    2.2250738585072014e-308,
                                    4.9e-324,
                                    56.32704,
                                    80.0,
                                    1e-300,
                                    -1.0,
                                    100.0,
                                    0.0,
                                    79.999999999999986,
                                    1e300};
    return b >= 240 ? kEdges[b - 240] : b * 0.5;
}

uint64_t step_us(uint8_t b)
{
    if (b < 200) {
        return b * 10000ull; /* 0 to 2 s */
    }
    return b < 255 ? (b - 200) * 1000000ull : (1ull << 62);
}

uint64_t saturated(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

bool high(double v, double level)
{
    return v >= level;
}

/* The largest climb the slow way: every pair of samples in (from, to]
 * and the one in force at from, the later above the earlier */
double climb_by_pairs(const ecuio_aids::samples& s, uint64_t from, uint64_t to)
{
    double best = 0.0;
    size_t first = 0;
    for (size_t i = 0; i < s.size() && s[i].us <= from; ++i) {
        first = i; /* the one in force at from */
    }
    for (size_t i = first; i < s.size() && s[i].us <= to; ++i) {
        for (size_t j = i + 1; j < s.size() && s[j].us <= to; ++j) {
            if (s[j].value - s[i].value > best) { /* false for a NaN */
                best = s[j].value - s[i].value;
            }
        }
    }
    return best;
}

/* What a pull's rules ask of it, worked out apart from pull_rises */
bool rises(const ecuio_aids::stretch& x, bool trim,
           const ecuio_aids::samples* engine_speed,
           const ecuio_aids::pull_rules& rules)
{
    if (std::isnan(x.start) || std::isnan(x.end) ||
        (trim && !(x.end > x.start))) {
        return false;
    }
    if (engine_speed != nullptr) {
        return climb_by_pairs(*engine_speed, x.from_us, x.to_us) >=
               rules.min_rise;
    }
    return x.end - x.start >= rules.min_gain;
}

void stretches(const uint8_t* data, size_t size)
{
    namespace aids = ecuio_aids;
    if (size < 16) {
        return;
    }
    const uint8_t switches = data[0];
    const aids::pull_rules pull{
        rule_value(data[1]), rule_us(data[2]),
        rule_us(data[3]),    (switches & 4) != 0 ? rule_value(data[4]) : 0.0,
        rule_value(data[9]), rule_value(data[10])};
    const aids::cruise_rules cruise{rule_us(data[5]), rule_value(data[6]),
                                    rule_value(data[7]), rule_value(data[8])};
    const bool trim = (switches & 1) != 0;
    const bool throttled = (switches & 2) != 0;
    const bool top_speed_end = (switches & 32) != 0;
    const uint32_t flags_in =
        (switches & 8) != 0 ? ECUIO_RECORDING_STRETCH_BY_LOAD : 0u;
    const uint8_t range[2] = {data[11], data[12]};
    size_t at = 16;
    aids::samples s[3];
    for (int k = 0; k < 3; ++k) {
        uint64_t t = 0;
        for (size_t n = 0; n < data[13 + k] && at + 1 < size; ++n, at += 2) {
            t = saturated(t, step_us(data[at]));
            s[k].push_back(aids::sample{t, sample_value(data[at + 1])});
        }
    }
    const aids::samples& gate = s[0];
    const aids::samples& speed = s[1];
    const aids::samples& measure = trim ? s[1] : s[2];
    /* The recording's engine speed, or none: the gain floor then */
    const aids::samples* engine_speed = (switches & 16) != 0 ? &s[2] : nullptr;

    /* Runs: in range, in order, apart, each end at the level, and every
     * sample at the level inside one */
    const auto runs = aids::runs_at_or_above(gate, pull.level, pull.gap_us);
    size_t covered = 0;
    for (size_t r = 0; r < runs.size(); ++r) {
        const auto& run = runs[r];
        if (run.first > run.second || run.second >= gate.size() ||
            (r > 0 && run.first <= runs[r - 1].second) ||
            !high(gate[run.first].value, pull.level) ||
            !high(gate[run.second].value, pull.level)) {
            abort();
        }
    }
    for (size_t i = 0; i < gate.size(); ++i) {
        if (!high(gate[i].value, pull.level)) {
            continue;
        }
        while (covered < runs.size() && runs[covered].second < i) {
            ++covered;
        }
        if (covered == runs.size() || runs[covered].first > i) {
            abort();
        }
    }

    /* Pulls: as long as the hold, risen as the rules ask, from a run's
     * first sample, in time order, the flags as given */
    std::vector<aids::stretch> pulls;
    aids::find_pulls(gate, measure, trim, top_speed_end, engine_speed, pull,
                     flags_in, pulls);
    for (size_t p = 0; p < pulls.size(); ++p) {
        const aids::stretch& x = pulls[p];
        if (x.kind != ECUIO_RECORDING_STRETCH_PULL || x.from_us > x.to_us ||
            x.to_us - x.from_us < pull.hold_us ||
            !rises(x, trim, engine_speed, pull) || x.flags != flags_in ||
            (p > 0 && x.from_us < pulls[p - 1].from_us)) {
            abort();
        }
        /* Runs may share a first time (a step of 0), so any run counts */
        bool within_a_run = false;
        bool ends_at_the_lift = false;
        for (const auto& run : runs) {
            if (gate[run.first].us == x.from_us &&
                x.to_us <= gate[run.second].us) {
                within_a_run = true;
                ends_at_the_lift =
                    ends_at_the_lift || gate[run.second].us == x.to_us;
            }
        }
        /* The lift ends a pull unless top speed does, and then only with
         * a speed measure, never past the lift */
        if (!within_a_run || (!(top_speed_end && trim) && !ends_at_the_lift)) {
            abort();
        }
    }

    /* Cruises: as long as the hold, one after another, a mean in both
     * ends at the floor or above (a mean of values at or above it) */
    std::vector<aids::stretch> cruises;
    aids::find_cruises(speed, throttled ? &gate : nullptr, cruise, flags_in,
                       cruises);
    for (size_t c = 0; c < cruises.size(); ++c) {
        const aids::stretch& x = cruises[c];
        if (x.kind != ECUIO_RECORDING_STRETCH_CRUISE || x.from_us > x.to_us ||
            x.to_us - x.from_us < cruise.hold_us || x.flags != flags_in ||
            !same_bits(x.start, x.end) || std::isnan(x.start) ||
            x.start < cruise.floor * (1 - 1e-12) ||
            (c > 0 && x.from_us < cruises[c - 1].to_us)) {
            abort();
        }
    }

    /* The cut: within the range, sorted, the cut flagged at its edge,
     * still the hold long, and every stretch inside the range kept */
    uint64_t span = 0;
    for (const auto& v : s) {
        span = (std::max)(span, v.empty() ? 0 : v.back().us);
    }
    const uint64_t from_us = span / 256 * range[0];
    const uint64_t to_us = saturated(from_us, span / 256 * range[1] + 1);
    std::vector<aids::stretch> all = pulls;
    all.insert(all.end(), cruises.begin(), cruises.end());
    size_t inside = 0;
    for (const aids::stretch& x : all) {
        inside += x.from_us >= from_us && x.to_us <= to_us ? 1 : 0;
    }
    aids::cut_to(all, from_us, to_us, pull, trim, engine_speed, cruise.hold_us,
                 measure, speed);
    size_t untouched = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        const aids::stretch& x = all[i];
        const bool is_pull = x.kind == ECUIO_RECORDING_STRETCH_PULL;
        if (x.from_us < from_us || x.to_us > to_us || x.from_us > x.to_us ||
            (i > 0 && x.from_us < all[i - 1].from_us) ||
            x.to_us - x.from_us < (is_pull ? pull.hold_us : cruise.hold_us) ||
            ((x.flags & ECUIO_RECORDING_STRETCH_CUT_START) != 0 &&
             x.from_us != from_us) ||
            ((x.flags & ECUIO_RECORDING_STRETCH_CUT_END) != 0 &&
             x.to_us != to_us) ||
            (is_pull && !rises(x, trim, engine_speed, pull))) {
            abort();
        }
        const uint32_t cuts =
            ECUIO_RECORDING_STRETCH_CUT_START | ECUIO_RECORDING_STRETCH_CUT_END;
        untouched += (x.flags & cuts) == 0 ? 1 : 0;
    }
    if (untouched != inside) {
        abort();
    }

    /* Pure: the same input, the same stretches */
    std::vector<aids::stretch> again;
    aids::find_pulls(gate, measure, trim, top_speed_end, engine_speed, pull,
                     flags_in, again);
    aids::find_cruises(speed, throttled ? &gate : nullptr, cruise, flags_in,
                       again);
    aids::cut_to(again, from_us, to_us, pull, trim, engine_speed,
                 cruise.hold_us, measure, speed);
    if (again.size() != all.size()) {
        abort();
    }
    for (size_t i = 0; i < all.size(); ++i) {
        if (again[i].from_us != all[i].from_us ||
            again[i].to_us != all[i].to_us || again[i].flags != all[i].flags ||
            !same_bits(again[i].start, all[i].start) ||
            !same_bits(again[i].end, all[i].end)) {
            abort();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 262144) {
        return 0;
    }
    if (data[0] == 'S') {
        stretches(data + 1, size - 1);
        return 0;
    }
    /* 'F' a folder; '3' or '4' a folder whose events.log starts with
     * that format's first line, so its header's label lines (and format
     * 4's keyed signal lines) are reached */
    const bool format3 = data[0] == '3' || data[0] == '4';
    if (data[0] != 'F' && !format3) {
        each_line(data + 1, size - 1);
        return 0;
    }
    /* events.log, signals.csv, markers.csv, frames.csv, conditions.json:
     * absent past the cuts given */
    const uint8_t* parts[5] = {};
    size_t lens[5] = {};
    size_t count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= size && count < 5; ++i) {
        if (i == size || data[i] == 0x1E) {
            parts[count] = data + start;
            lens[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    std::string events;
    if (format3 && count > 0) {
        events = std::string(data[0] == '4' ? rows::kFormat4 : rows::kFormat3) +
                 "\n" +
                 std::string(reinterpret_cast<const char*>(parts[0]), lens[0]);
        parts[0] = reinterpret_cast<const uint8_t*>(events.data());
        lens[0] = events.size();
    }
    write_file("events.log", parts[0], lens[0], count > 0);
    write_file("signals.csv", parts[1], lens[1], count > 1);
    write_file("markers.csv", parts[2], lens[2], count > 2);
    write_file("frames.csv", parts[3], lens[3], count > 3);
    write_file("conditions.json", parts[4], lens[4], count > 4);
    g_want = want_conditions(reinterpret_cast<const char*>(parts[0]), lens[0],
                             count > 0,
                             reinterpret_cast<const char*>(parts[4]), lens[4],
                             count > 4);
    /* signals.csv, then the frames through obd2 (packed replies split),
     * the mock's set (a computed signal) and a float, each twice alike;
     * every open that succeeds reads the same conditions from events.log */
    const ecuio_signal_set_t* sets[] = {nullptr, ecuio_obd2_signal_set(),
                                        ecuio_mock_signal_set(), float_set()};
    opened first_ok;
    bool any_ok = false;
    for (const ecuio_signal_set_t* set : sets) {
        const opened a = open_and_query(set);
        const opened b = open_and_query(set);
        if (a.rc != b.rc || a.detail != b.detail ||
            memcmp(&a.info, &b.info, sizeof a.info) != 0 ||
            !same_conditions(a, b)) {
            abort();
        }
        if (a.rc != ECUIO_OK) {
            continue;
        }
        if (any_ok &&
            (!same_conditions(a, first_ok) ||
             a.info.condition_events != first_ok.info.condition_events)) {
            abort();
        }
        if (!any_ok) {
            first_ok = a;
            any_ok = true;
        }
    }
    const checked x = check_folder(0);
    const checked y = check_folder(0);
    if (x.rc != y.rc || x.detail != y.detail ||
        memcmp(&x.summary, &y.summary, sizeof x.summary) != 0) {
        abort();
    }
    if (x.rc == ECUIO_OK) {
        if (x.last_end > x.summary.duration_us) {
            abort();
        }
        if (x.seen > 0) {
            const checked z = check_folder(1);
            if (z.rc != ECUIO_ERR_CANCELED || z.seen != 1) {
                abort();
            }
        }
    }
    read_frames_back(data, size);
    return 0;
}
