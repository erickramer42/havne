// File/layer: core/src/recording_reader_stats.cpp | C++17, Qt-free;
//             stats and step (ADR-0049 section 5)
// Testing: UNIT: RecordingStatsTest, RecordingTest (test_core)

#include "recording_reader.h"

#include <algorithm>
#include <new>

using namespace ecuio_recording_reader;

namespace {

/* A signal's first summary at or after second */
template <typename Second>
typename std::vector<Second>::const_iterator
first_from(const std::vector<Second>& seconds, uint64_t second)
{
    return std::lower_bound(
        seconds.begin(), seconds.end(), second,
        [](const Second& e, uint64_t v) { return e.second < v; });
}

/* fn(row) for each of signal's raw rows in second, through the cache */
template <typename Fn>
ecuio_status_t each_row(const ecuio_recording_t* r, uint32_t second,
                        uint32_t signal, FILE*& f, file_ptr& owner, Fn fn)
{
    const index_entry* e = r->index_of(second);
    if (e == nullptr) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    std::shared_ptr<const std::vector<raw_row>> rows;
    const ecuio_status_t rc = r->rows_of(f, owner, *e, &rows);
    if (rc != ECUIO_OK) {
        return rc;
    }
    for (const raw_row& row : *rows) {
        if (row.signal == signal) {
            fn(row);
        }
    }
    return ECUIO_OK;
}

/* A number signal's value in force at t_us: its last sample at or before
 * it, NaN with none */
ecuio_status_t number_at(const ecuio_recording_t* r, uint32_t signal,
                         uint64_t t_us, FILE*& f, file_ptr& owner, double* out)
{
    *out = NAN;
    const std::vector<number_second>& seconds = r->signals[signal].numbers;
    const uint64_t sec = t_us / kSecondUs;
    auto it = std::upper_bound(
        seconds.begin(), seconds.end(), sec,
        [](uint64_t v, const number_second& e) { return v < e.second; });
    if (it == seconds.begin()) {
        return ECUIO_OK;
    }
    --it;
    if (it->second < sec) {
        *out = it->last;
        return ECUIO_OK;
    }
    /* t's own second may hold the signal's samples only after t */
    bool found = false;
    const ecuio_status_t rc =
        each_row(r, it->second, signal, f, owner, [&](const raw_row& row) {
            if (row.host_us <= t_us) {
                *out = row.value;
                found = true;
            }
        });
    if (rc == ECUIO_OK && !found && it != seconds.begin()) {
        *out = (it - 1)->last;
    }
    return rc;
}

/* A span's statistics as they gather. An extreme a summary gave keeps
 * its second, whose rows give its time once the pass is done. */
struct span_stats {
    uint64_t samples = 0;
    uint64_t valued = 0; /* the samples that aren't NaN */
    double sum = 0.0;
    double lowest = NAN;
    double highest = NAN;
    uint64_t lowest_us = ECUIO_RECORDING_NONE;
    uint64_t highest_us = ECUIO_RECORDING_NONE;
    uint32_t lowest_second = UINT32_MAX;
    uint32_t highest_second = UINT32_MAX;

    void take(uint64_t host_us, double v)
    {
        ++samples;
        if (std::isnan(v)) {
            return;
        }
        ++valued;
        sum += v;
        if (std::isnan(lowest) || v < lowest) {
            lowest = v;
            lowest_us = host_us;
            lowest_second = UINT32_MAX;
        }
        if (std::isnan(highest) || v > highest) {
            highest = v;
            highest_us = host_us;
            highest_second = UINT32_MAX;
        }
    }

    /* A whole second whose sum isn't NaN, so none of its values is */
    void take(const number_second& ns)
    {
        samples += ns.count;
        valued += ns.count;
        sum += ns.sum;
        if (std::isnan(lowest) || ns.lowest < lowest) {
            lowest = ns.lowest;
            lowest_us = ECUIO_RECORDING_NONE;
            lowest_second = ns.second;
        }
        if (std::isnan(highest) || ns.highest > highest) {
            highest = ns.highest;
            highest_us = ECUIO_RECORDING_NONE;
            highest_second = ns.second;
        }
    }
};

/* The time of the first of signal's samples in second equal to value */
ecuio_status_t first_at(const ecuio_recording_t* r, uint32_t signal,
                        uint32_t second, double value, FILE*& f,
                        file_ptr& owner, uint64_t* at_us)
{
    return each_row(r, second, signal, f, owner, [&](const raw_row& row) {
        if (*at_us == ECUIO_RECORDING_NONE && row.value == value) {
            *at_us = row.host_us;
        }
    });
}

ecuio_status_t number_stats(const ecuio_recording_t* r, uint32_t signal,
                            uint64_t t0_us, uint64_t t1_us, FILE*& f,
                            file_ptr& owner, ecuio_recording_stats_t* out)
{
    const std::vector<number_second>& seconds = r->signals[signal].numbers;
    span_stats acc;
    const uint64_t last_second = t1_us / kSecondUs;
    for (auto it = first_from(seconds, t0_us / kSecondUs);
         it != seconds.end() && it->second <= last_second; ++it) {
        const uint64_t start = uint64_t{it->second} * kSecondUs;
        if (start >= t0_us && start + kSecondUs - 1 <= t1_us &&
            !std::isnan(it->sum)) {
            acc.take(*it);
            continue;
        }
        const ecuio_status_t rc =
            each_row(r, it->second, signal, f, owner, [&](const raw_row& row) {
                if (row.host_us >= t0_us && row.host_us <= t1_us) {
                    acc.take(row.host_us, row.value);
                }
            });
        if (rc != ECUIO_OK) {
            return rc;
        }
    }
    if (acc.lowest_second != UINT32_MAX) {
        const ecuio_status_t rc = first_at(
            r, signal, acc.lowest_second, acc.lowest, f, owner, &acc.lowest_us);
        if (rc != ECUIO_OK) {
            return rc;
        }
    }
    if (acc.highest_second != UINT32_MAX) {
        const ecuio_status_t rc =
            first_at(r, signal, acc.highest_second, acc.highest, f, owner,
                     &acc.highest_us);
        if (rc != ECUIO_OK) {
            return rc;
        }
    }
    out->samples = acc.samples;
    out->lowest = acc.lowest;
    out->highest = acc.highest;
    out->lowest_us = acc.lowest_us;
    out->highest_us = acc.highest_us;
    out->mean =
        acc.valued != 0 ? acc.sum / static_cast<double>(acc.valued) : NAN;
    const ecuio_status_t rc =
        number_at(r, signal, t0_us, f, owner, &out->first);
    if (rc != ECUIO_OK) {
        return rc;
    }
    return number_at(r, signal, t1_us, f, owner, &out->last);
}

ecuio_status_t text_stats(const ecuio_recording_t* r, uint32_t signal,
                          uint64_t t0_us, uint64_t t1_us, FILE*& f,
                          file_ptr& owner, ecuio_recording_stats_t* out)
{
    const std::vector<text_second>& seconds = r->signals[signal].texts;
    const uint64_t last_second = t1_us / kSecondUs;
    for (auto it = first_from(seconds, t0_us / kSecondUs);
         it != seconds.end() && it->second <= last_second; ++it) {
        const uint64_t start = uint64_t{it->second} * kSecondUs;
        if (start >= t0_us && start + kSecondUs - 1 <= t1_us) {
            out->samples += it->count;
            continue;
        }
        const ecuio_status_t rc =
            each_row(r, it->second, signal, f, owner, [&](const raw_row& row) {
                if (row.host_us >= t0_us && row.host_us <= t1_us) {
                    ++out->samples;
                }
            });
        if (rc != ECUIO_OK) {
            return rc;
        }
    }
    return ECUIO_OK;
}

/* The first of signal's samples strictly after t_us, or strictly before
 * it, through its seconds */
template <typename Second>
ecuio_status_t step_through(const ecuio_recording_t* r,
                            const std::vector<Second>& seconds, uint32_t signal,
                            uint64_t t_us, bool forward, uint64_t* at_us)
{
    FILE* f = nullptr;
    file_ptr owner;
    const uint64_t sec = t_us / kSecondUs;
    if (forward) {
        for (auto it = first_from(seconds, sec); it != seconds.end(); ++it) {
            const ecuio_status_t rc = each_row(
                r, it->second, signal, f, owner, [&](const raw_row& row) {
                    if (*at_us == ECUIO_RECORDING_NONE && row.host_us > t_us) {
                        *at_us = row.host_us;
                    }
                });
            if (rc != ECUIO_OK || *at_us != ECUIO_RECORDING_NONE) {
                return rc;
            }
        }
        return ECUIO_OK;
    }
    auto it = std::upper_bound(
        seconds.begin(), seconds.end(), sec,
        [](uint64_t v, const Second& e) { return v < e.second; });
    while (it != seconds.begin()) {
        --it;
        const ecuio_status_t rc =
            each_row(r, it->second, signal, f, owner, [&](const raw_row& row) {
                if (row.host_us < t_us) {
                    *at_us = row.host_us;
                }
            });
        if (rc != ECUIO_OK || *at_us != ECUIO_RECORDING_NONE) {
            return rc;
        }
    }
    return ECUIO_OK;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_recording_stats(const ecuio_recording_t* r,
                                     const uint32_t* signal_ids,
                                     uint32_t signal_count, uint64_t t0_us,
                                     uint64_t t1_us,
                                     ecuio_recording_stats_t* out)
{
    if (r == nullptr || signal_ids == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (signal_count == 0 || t0_us >= t1_us) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < signal_count; ++i) {
        if (signal_ids[i] >= r->signals.size()) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    try {
        FILE* f = nullptr;
        file_ptr owner;
        for (uint32_t i = 0; i < signal_count; ++i) {
            ecuio_recording_stats_t& st = out[i];
            st = ecuio_recording_stats_t{};
            st.first = st.last = st.lowest = st.highest = st.mean = NAN;
            st.lowest_us = st.highest_us = ECUIO_RECORDING_NONE;
            const uint32_t signal = signal_ids[i];
            const ecuio_status_t rc =
                r->signals[signal].text()
                    ? text_stats(r, signal, t0_us, t1_us, f, owner, &st)
                    : number_stats(r, signal, t0_us, t1_us, f, owner, &st);
            if (rc != ECUIO_OK) {
                return rc;
            }
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_recording_step(const ecuio_recording_t* r, uint32_t signal,
                                    uint64_t t_us, uint32_t direction,
                                    uint64_t* at_us)
{
    if (r == nullptr || at_us == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (signal >= r->signals.size() ||
        (direction != ECUIO_RECORDING_STEP_FORWARD &&
         direction != ECUIO_RECORDING_STEP_BACKWARD)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *at_us = ECUIO_RECORDING_NONE;
    const bool forward = direction == ECUIO_RECORDING_STEP_FORWARD;
    try {
        const signal_entry& s = r->signals[signal];
        return s.text()
                   ? step_through(r, s.texts, signal, t_us, forward, at_us)
                   : step_through(r, s.numbers, signal, t_us, forward, at_us);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
