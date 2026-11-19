// File/layer: core/src/recording_reader.cpp | C++17, Qt-free; the reader
//             (ADR-0049 section 5)
// Testing: UNIT: RecordingTest, RecordingFramesTest, RecordingStatsTest,
//          RecordingStretchesTest, ConditionRecordingTest (test_core)
//
// One pass at open reads events.log's header (the format, the UTC start,
// each signal's kind, decimal places and plot rank) and its cond lines,
// each kept in 40 bytes up to a cap (ADR-0063 section 8), and signals.csv, and
// builds per signal a summary for each second of host_us it has samples
// in, beside each second's byte offset in signals.csv. Queries answer
// from the summaries, and closer than a second a column from the raw
// rows of the seconds they need, which a cache of 8 MiB keeps.
//
// ecuio_recording_open_frames reads frames.csv in signals.csv's place,
// each frame decoded as a session decodes it (frame_decoder.h), raw
// ISO-TP frames reassembled first (isotp.h). Its raw rows are a second's
// frames decoded again, from a mark the open kept at the second's start:
// where a re-read starts (the last row with no message under way, so
// ISO-TP rebuilds the same state) and the decoder's state then.

#include "recording_reader.h"
#include "error_detail.h"
#include "recording_aids.h"
#include "recording_keys.h"
#include "signal_keys.h" /* the find rule */

#include "ecuio/internal.h"

#include <algorithm>
#include <new>

namespace keys = ecuio_recording_keys;
using ecuio_frames_csv::seek_to;

using namespace ecuio_recording_reader;

namespace {

void copy_cut(char* out, size_t cap, const std::string& s)
{
    size_t n = (std::min)(s.size(), cap - 1);
    if (n < s.size()) {
        /* Cut on a character's start */
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
            --n;
        }
    }
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
}

} // namespace

bool ecuio_recording::read_markers(const std::string& folder)
{
    file_ptr f = open_file(folder + "/markers.csv");
    if (!f) {
        return true; /* made by the first mark only */
    }
    line_reader lines(f.get(), 0);
    std::string line;
    bool ended = false;
    bool too_long = false;
    if (!lines.next(line, &ended, &too_long)) {
        return true;
    }
    const size_t bom = rows::bom_length(line.data(), line.size());
    if (!ended || too_long ||
        !rows::equals(line.data() + bom, line.size() - bom,
                      rows::kMarkersHeader)) {
        ecuio_detail_set("markers.csv's header row isn't %s",
                         rows::kMarkersHeader);
        return false;
    }
    uint64_t line_no = 1;
    std::string label;
    while (lines.next(line, &ended, &too_long)) {
        ++line_no;
        if (!ended) {
            ++info.rows_torn;
            break;
        }
        uint64_t host_us = 0;
        const char* why = "the row is longer than a recording's rows can be";
        if (too_long || !rows::parse_marker_row(line.data(), line.size(),
                                                &host_us, label, &why)) {
            ecuio_detail_set("markers.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            return false;
        }
        markers.push_back(marker_entry{host_us, label});
        info.duration_us = (std::max)(info.duration_us, host_us);
    }
    /* Marks from two threads may be stamped out of the order they were
     * queued in */
    std::stable_sort(markers.begin(), markers.end(),
                     [](const marker_entry& a, const marker_entry& b) {
                         return a.host_us < b.host_us;
                     });
    return true;
}

/* Kinds settled, the tables trimmed to the bound */
void ecuio_recording::finish()
{
    if (from_frames) {
        std::vector<signal_entry> kept;
        for (uint32_t& k : def_signal) {
            if (signals[k].pub.samples == 0) {
                k = UINT32_MAX;
                continue;
            }
            kept.push_back(std::move(signals[k]));
            k = static_cast<uint32_t>(kept.size() - 1);
        }
        signals.swap(kept);
        marks.shrink_to_fit();
        lengths.shrink_to_fit();
        latest_pool.shrink_to_fit();
    }
    for (signal_entry& s : signals) {
        if (!s.known_kind) {
            s.pub.flags = s.not_number ? ECUIO_RECORDING_SIGNAL_TEXT : 0;
            s.pub.decimal_places =
                static_cast<uint8_t>((std::min)(s.decimals_seen, 3u));
        }
        if (s.text()) {
            std::vector<number_second>().swap(s.numbers);
            s.texts.shrink_to_fit();
        } else {
            std::vector<text_second>().swap(s.texts);
            s.numbers.shrink_to_fit();
            s.pub.lowest = std::isnan(s.lowest) ? 0.0 : s.lowest;
            s.pub.highest = std::isnan(s.highest) ? 0.0 : s.highest;
        }
    }
    index.shrink_to_fit();
    condition_events.shrink_to_fit();
    info.signal_count = static_cast<uint32_t>(signals.size());
    info.marker_count = static_cast<uint32_t>(markers.size());
}

const index_entry* ecuio_recording::index_of(uint32_t second) const
{
    auto it = std::lower_bound(
        index.begin(), index.end(), second,
        [](const index_entry& e, uint32_t s) { return e.second < s; });
    return (it != index.end() && it->second == second) ? &*it : nullptr;
}

/* Each row of one second, read again from the file: fn(signal, host_us,
 * value text, the row's offset). A row that no longer parses, or isn't
 * the second's, means the file changed after the open. */
/* A second of frames.csv decoded again from its mark: the rows from the
 * mark's rescan to the second's first feed ISO-TP alone, then the
 * second's rows decode, fn(signal, host_us, value text, the row's
 * offset) for each value */
template <typename Fn>
ecuio_status_t ecuio_recording::scan_frames(FILE* f, const index_entry& e,
                                            Fn fn) const
{
    const frame_mark& m = marks[static_cast<size_t>(&e - index.data())];
    if (!seek_to(f, m.rescan)) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    frame_scan scan(&frame_set->set);
    restore(scan, m);
    line_reader lines(f, m.rescan);
    std::string line;
    std::string value;
    bool ended = false;
    bool too_long = false;
    ecuio_frames_csv::row r;
    const char* why = nullptr;
    bool first = true;
    while (lines.offset() < end_offset && lines.next(line, &ended, &too_long)) {
        if (!ended || too_long ||
            !ecuio_frames_csv::parse_row(line.c_str(), &r, &scan.row(), &why)) {
            return ECUIO_ERR_RECORDING_CHANGED;
        }
        const ecuio_frame_t* msg = scan.take(r.host_us);
        if (lines.line_start() < e.offset) {
            continue; /* before the second: ISO-TP's state alone */
        }
        if (r.host_us / kSecondUs != e.second) {
            if (first) {
                return ECUIO_ERR_RECORDING_CHANGED;
            }
            break;
        }
        first = false;
        const uint32_t n = msg != nullptr ? scan.decoder.decode(msg) : 0;
        for (uint32_t k = 0; k < n; ++k) {
            const ecuio_value_t& v = scan.decoder.values[k];
            const auto it = def_of_id.find(v.signal_id);
            if (it == def_of_id.end()) {
                return ECUIO_ERR_RECORDING_CHANGED;
            }
            const ecuio_signal_def_t& d = frame_set->set.defs[it->second];
            value.clear();
            if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
                value.assign(v.text, v.text_len);
            } else {
                /* As the open judged it: not finite is no sample, so a def
                 * with no other value has no signal and isn't a change */
                double x = 0.0;
                rows::append_value(value, v.value);
                if (!rows::parse_number(value.data(), value.size(), &x) ||
                    !std::isfinite(x)) {
                    continue;
                }
            }
            const uint32_t signal = def_signal[it->second];
            if (signal == UINT32_MAX) {
                return ECUIO_ERR_RECORDING_CHANGED; /* none at the open */
            }
            fn(signal, r.host_us, value, lines.line_start());
        }
    }
    return first ? ECUIO_ERR_RECORDING_CHANGED : ECUIO_OK;
}

template <typename Fn>
ecuio_status_t ecuio_recording::scan_second(FILE* f, const index_entry& e,
                                            Fn fn) const
{
    if (from_frames) {
        return scan_frames(f, e, fn);
    }
    if (!seek_to(f, e.offset)) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    line_reader lines(f, e.offset);
    std::string line;
    bool ended = false;
    bool too_long = false;
    rows::signal_row r;
    const char* why = nullptr;
    bool first = true;
    while (lines.offset() < end_offset && lines.next(line, &ended, &too_long)) {
        if (!ended || too_long ||
            !rows::parse_signal_row(line.data(), line.size(), r, &why)) {
            return ECUIO_ERR_RECORDING_CHANGED;
        }
        const uint64_t second = r.host_us / kSecondUs;
        if (second != e.second) {
            if (first) {
                return ECUIO_ERR_RECORDING_CHANGED;
            }
            break;
        }
        first = false;
        auto it = by_name.find(r.name);
        if (it == by_name.end()) {
            return ECUIO_ERR_RECORDING_CHANGED;
        }
        fn(it->second, r.host_us, r.value, lines.line_start());
    }
    return first ? ECUIO_ERR_RECORDING_CHANGED : ECUIO_OK;
}

/* The row at offset, which must be signal's: its value and time */
ecuio_status_t ecuio_recording::row_at(FILE* f, uint64_t offset,
                                       uint32_t signal, std::string* value,
                                       uint64_t* host_us) const
{
    if (offset >= end_offset || !seek_to(f, offset)) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    line_reader lines(f, offset);
    std::string line;
    bool ended = false;
    bool too_long = false;
    rows::signal_row r;
    const char* why = nullptr;
    if (!lines.next(line, &ended, &too_long) || !ended || too_long ||
        !rows::parse_signal_row(line.data(), line.size(), r, &why)) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    auto it = by_name.find(r.name);
    if (it == by_name.end() || it->second != signal) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    *value = r.value;
    *host_us = r.host_us;
    return ECUIO_OK;
}

/* A second's raw rows, from the cache or read and kept there. f is
 * opened on the first read a query needs. */
ecuio_status_t
ecuio_recording::rows_of(FILE*& f, file_ptr& owner, const index_entry& e,
                         std::shared_ptr<const std::vector<raw_row>>* out) const
{
    {
        std::lock_guard<std::mutex> g(cache_m);
        auto it = cached.find(e.second);
        if (it != cached.end()) {
            lru.splice(lru.begin(), lru, it->second);
            *out = it->second->rows;
            return ECUIO_OK;
        }
    }
    if (f == nullptr) {
        owner = open_file(source_path);
        if (!owner) {
            return ECUIO_ERR_RECORDING_CHANGED;
        }
        f = owner.get();
    }
    auto parsed = std::make_shared<std::vector<raw_row>>();
    bool bad = false;
    const ecuio_status_t rc = scan_second(
        f, e,
        [&](uint32_t signal, uint64_t host_us, const std::string& value,
            uint64_t) {
            double v = 0.0;
            if (!signals[signal].text() &&
                !rows::parse_number(value.data(), value.size(), &v)) {
                /* signals.csv's row lost its number: the file changed */
                bad = !from_frames;
                return;
            }
            parsed->push_back(raw_row{host_us, signal, 0, v});
        });
    if (rc != ECUIO_OK || bad) {
        return ECUIO_ERR_RECORDING_CHANGED;
    }
    *out = parsed;
    const size_t bytes = parsed->capacity() * sizeof(raw_row);
    std::lock_guard<std::mutex> g(cache_m);
    if (cached.find(e.second) == cached.end()) {
        lru.push_front(block{e.second, parsed});
        cached.emplace(e.second, lru.begin());
        cache_bytes += bytes;
        evict();
    }
    return ECUIO_OK;
}

void ecuio_recording::evict() const
{
    while (cache_bytes > cache_limit && !lru.empty()) {
        const block& old = lru.back();
        cache_bytes -= old.rows->capacity() * sizeof(raw_row);
        cached.erase(old.second);
        lru.pop_back();
    }
}

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

ecuio_status_t ecuio_recording_open(const char* folder_utf8,
                                    ecuio_open_progress_t progress,
                                    void* user_data, ecuio_recording_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (folder_utf8 == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_detail_clear();
    try {
        std::unique_ptr<ecuio_recording> r(new ecuio_recording);
        std::string folder = folder_utf8;
        while (folder.size() > 1 &&
               (folder.back() == '/' || folder.back() == '\\')) {
            folder.pop_back();
        }
        r->source_path = folder + "/signals.csv";
        bool canceled = false;
        r->open_progress = progress;
        r->open_user = user_data;
        if (!r->read_events(folder) ||
            !r->read_signals(progress, user_data, &canceled) ||
            !r->read_markers(folder)) {
            return canceled || r->open_canceled ? ECUIO_ERR_CANCELED
                                                : ECUIO_ERR_LOAD_FAILED;
        }
        r->read_copies(folder);
        r->finish();
        *out = r.release();
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_recording_find_signal(const ecuio_recording_t* r,
                                           const char* text_utf8,
                                           uint32_t* index)
{
    if (r == nullptr || text_utf8 == nullptr || index == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    return ecuio_keys::find(
        static_cast<uint32_t>(r->signals.size()),
        [r](uint32_t i) { return ecuio_keys::view_of(r->signals[i].pub.key); },
        [r](uint32_t i) { return ecuio_keys::view_of(r->signals[i].pub.name); },
        text_utf8, index);
}

ecuio_status_t ecuio_recording_open_frames_with(
    const char* folder_utf8, const ecuio_signal_set_t* signal_set,
    const ecuio_signal_file_t* file, ecuio_open_progress_t progress,
    void* user_data, ecuio_recording_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (folder_utf8 == nullptr || signal_set == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_detail_clear();
    const int valid = ecuio_validate_signal_set(signal_set, nullptr);
    if (valid == ECUIO_VALIDATE_ERR_OUT_OF_MEMORY) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (valid != ECUIO_VALIDATE_OK) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    std::vector<ecuio_signal_key_t> set_keys;
    const ecuio_status_t keyed = keys::set_keys(signal_set, file, &set_keys);
    if (keyed != ECUIO_OK) {
        return keyed;
    }
    try {
        std::unique_ptr<ecuio_recording> r(new ecuio_recording);
        std::string folder = folder_utf8;
        while (folder.size() > 1 &&
               (folder.back() == '/' || folder.back() == '\\')) {
            folder.pop_back();
        }
        r->from_frames = true;
        r->info.flags |= ECUIO_RECORDING_INFO_FRAMES;
        r->source_path = folder + "/frames.csv";
        r->take_set(*signal_set, set_keys);
        bool canceled = false;
        r->open_progress = progress;
        r->open_user = user_data;
        if (!r->read_events(folder) ||
            !r->read_frames(progress, user_data, &canceled) ||
            !r->read_markers(folder)) {
            return canceled || r->open_canceled ? ECUIO_ERR_CANCELED
                                                : ECUIO_ERR_LOAD_FAILED;
        }
        r->read_copies(folder);
        r->finish();
        *out = r.release();
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_recording_get_info(const ecuio_recording_t* r,
                                        ecuio_recording_info_t* out)
{
    if (r == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = r->info;
    return ECUIO_OK;
}

ecuio_status_t ecuio_recording_get_signals(const ecuio_recording_t* r,
                                           ecuio_recording_signals_t* out)
{
    if (r == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = r->signal_file;
    return ECUIO_OK;
}

ecuio_status_t ecuio_recording_get_conditions(const ecuio_recording_t* r,
                                              ecuio_recording_conditions_t* out)
{
    if (r == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = r->conditions;
    return ECUIO_OK;
}

ecuio_status_t
ecuio_recording_get_condition_event(const ecuio_recording_t* r, uint64_t index,
                                    ecuio_recording_condition_event_t* out)
{
    if (r == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= r->condition_events.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const condition_entry& e = r->condition_events[static_cast<size_t>(index)];
    *out = ecuio_recording_condition_event_t{};
    out->host_us = e.host_us;
    out->timestamp_us = e.t_us;
    out->value = e.value;
    out->state = e.state;
    out->flags = e.flags;
    out->term = e.term;
    std::memcpy(out->key, r->condition_keys[e.key], sizeof out->key);
    return ECUIO_OK;
}

ecuio_status_t ecuio_recording_get_signal(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_signal_t* out)
{
    if (r == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= r->signals.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out = r->signals[index].pub;
    return ECUIO_OK;
}

ecuio_status_t ecuio_recording_get_marker(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_marker_t* out)
{
    if (r == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= r->markers.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const marker_entry& m = r->markers[index];
    *out = ecuio_recording_marker_t{};
    out->host_us = m.host_us;
    out->label_length = static_cast<uint32_t>(m.label.size());
    out->label_utf8 = m.label.c_str();
    return ECUIO_OK;
}

ecuio_status_t ecuio_recording_get_labels(const ecuio_recording_t* r,
                                          uint32_t signal,
                                          ecuio_enum_entry_t* entries,
                                          uint32_t capacity, uint32_t* count)
{
    if (r == nullptr || count == nullptr ||
        (entries == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (signal >= r->signals.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const std::vector<ecuio_enum_entry_t>& labels = r->signals[signal].labels;
    *count = static_cast<uint32_t>(labels.size());
    const size_t n = (std::min)(labels.size(), size_t{capacity});
    for (size_t i = 0; i < n; ++i) {
        entries[i] = labels[i];
    }
    return ECUIO_OK;
}

const char* ecuio_recording_enum_label(const ecuio_recording_t* r,
                                       uint32_t signal, uint32_t raw)
{
    if (r == nullptr || signal >= r->signals.size()) {
        return nullptr;
    }
    const std::vector<ecuio_enum_entry_t>& labels = r->signals[signal].labels;
    const auto it = std::lower_bound(
        labels.begin(), labels.end(), raw,
        [](const ecuio_enum_entry_t& e, uint32_t v) { return e.raw < v; });
    return it != labels.end() && it->raw == raw ? it->label : nullptr;
}

ecuio_status_t
ecuio_recording_query(const ecuio_recording_t* r, const uint32_t* signal_ids,
                      uint32_t signal_count, uint64_t t0_us, uint64_t t1_us,
                      ecuio_recording_column_t* columns, uint32_t column_count)
{
    if (r == nullptr || signal_ids == nullptr || columns == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (signal_count == 0 || column_count == 0 || t0_us >= t1_us) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < signal_count; ++i) {
        if (signal_ids[i] >= r->signals.size()) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    const uint64_t span = t1_us - t0_us;
    auto column_of = [&](uint64_t t) {
        const double c = static_cast<double>(t - t0_us) * column_count /
                         static_cast<double>(span);
        return (std::min)(static_cast<uint32_t>(c), column_count - 1);
    };
    try {
        /* NaN until a value, so a column's first sets its range */
        std::vector<double> lo(static_cast<size_t>(signal_count) * column_count,
                               NAN);
        std::vector<double> hi(lo.size(), NAN);
        std::vector<uint64_t> count(lo.size(), 0);

        if (span / column_count >= kSecondUs) {
            /* Wide columns: the summaries, each second in the column it
             * starts in */
            for (uint32_t q = 0; q < signal_count; ++q) {
                const signal_entry& s = r->signals[signal_ids[q]];
                const auto first_second = static_cast<uint32_t>(
                    (std::min<uint64_t>)(t0_us / kSecondUs, UINT32_MAX));
                const size_t base = static_cast<size_t>(q) * column_count;
                auto place = [&](uint32_t second, uint32_t n) {
                    const uint64_t start = second * kSecondUs;
                    if (start >= t1_us) {
                        return false;
                    }
                    const uint32_t c = column_of((std::max)(start, t0_us));
                    count[base + c] += n;
                    return true;
                };
                if (s.text()) {
                    auto it = std::lower_bound(
                        s.texts.begin(), s.texts.end(), first_second,
                        [](const text_second& e, uint32_t v) {
                            return e.second < v;
                        });
                    for (; it != s.texts.end() && place(it->second, it->count);
                         ++it) {
                    }
                } else {
                    auto it = std::lower_bound(
                        s.numbers.begin(), s.numbers.end(), first_second,
                        [](const number_second& e, uint32_t v) {
                            return e.second < v;
                        });
                    for (; it != s.numbers.end(); ++it) {
                        if (!place(it->second, it->count)) {
                            break;
                        }
                        const uint32_t c = column_of(
                            (std::max)(it->second * kSecondUs, t0_us));
                        merge(lo[base + c], hi[base + c], it->lowest);
                        merge(lo[base + c], hi[base + c], it->highest);
                    }
                }
            }
        } else {
            /* Narrow columns: the raw rows of the seconds the range
             * covers, once for every signal in the query */
            std::vector<int32_t> slot(r->signals.size(), -1);
            for (uint32_t q = 0; q < signal_count; ++q) {
                if (slot[signal_ids[q]] < 0) {
                    slot[signal_ids[q]] = static_cast<int32_t>(q);
                }
            }
            const uint64_t s0 = t0_us / kSecondUs;
            const uint64_t s1 = (t1_us - 1) / kSecondUs;
            auto it = std::lower_bound(
                r->index.begin(), r->index.end(), s0,
                [](const index_entry& e, uint64_t v) { return e.second < v; });
            FILE* f = nullptr;
            file_ptr owner;
            for (; it != r->index.end() && it->second <= s1; ++it) {
                std::shared_ptr<const std::vector<raw_row>> rows;
                const ecuio_status_t rc = r->rows_of(f, owner, *it, &rows);
                if (rc != ECUIO_OK) {
                    return rc;
                }
                for (const raw_row& row : *rows) {
                    if (row.host_us < t0_us || row.host_us >= t1_us ||
                        slot[row.signal] < 0) {
                        continue;
                    }
                    const uint32_t c = column_of(row.host_us);
                    /* Every query slot naming this signal */
                    for (uint32_t q = static_cast<uint32_t>(slot[row.signal]);
                         q < signal_count; ++q) {
                        if (signal_ids[q] != row.signal) {
                            continue;
                        }
                        const size_t k =
                            static_cast<size_t>(q) * column_count + c;
                        ++count[k];
                        if (!r->signals[row.signal].text()) {
                            merge(lo[k], hi[k], row.value);
                        }
                    }
                }
            }
        }
        for (size_t k = 0; k < lo.size(); ++k) {
            ecuio_recording_column_t& col = columns[k];
            col = ecuio_recording_column_t{};
            col.samples = static_cast<uint32_t>(
                (std::min<uint64_t>)(count[k], UINT32_MAX));
            col.lowest = std::isnan(lo[k]) ? 0.0 : lo[k];
            col.highest = std::isnan(hi[k]) ? 0.0 : hi[k];
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_recording_text_at(const ecuio_recording_t* r,
                                       uint32_t signal, uint64_t t_us,
                                       char* utf8, uint32_t capacity,
                                       uint64_t* at_us)
{
    if (r == nullptr || utf8 == nullptr || at_us == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (signal >= r->signals.size() || capacity == 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    utf8[0] = '\0';
    *at_us = ECUIO_RECORDING_NONE;
    const signal_entry& s = r->signals[signal];
    const auto sec = static_cast<uint32_t>(
        (std::min<uint64_t>)(t_us / kSecondUs, UINT32_MAX));
    /* The signal's last two seconds at or before t's, newest first, and a
     * text second's last row: t's own second may hold the signal's
     * samples only after t, and then the one before it has the value in
     * force, which is that second's last */
    struct candidate {
        uint32_t second;
        uint64_t last_offset; /* ECUIO_RECORDING_NONE for a number's */
    };
    candidate seconds[2];
    size_t found_seconds = 0;
    if (s.text()) {
        auto it = std::upper_bound(
            s.texts.begin(), s.texts.end(), sec,
            [](uint32_t v, const text_second& e) { return v < e.second; });
        for (; it != s.texts.begin() && found_seconds < 2; --it) {
            seconds[found_seconds++] = {(it - 1)->second,
                                        (it - 1)->last_offset};
        }
    } else {
        auto it = std::upper_bound(
            s.numbers.begin(), s.numbers.end(), sec,
            [](uint32_t v, const number_second& e) { return v < e.second; });
        for (; it != s.numbers.begin() && found_seconds < 2; --it) {
            seconds[found_seconds++] = {(it - 1)->second, ECUIO_RECORDING_NONE};
        }
    }
    if (found_seconds == 0) {
        return ECUIO_OK;
    }
    try {
        file_ptr f = open_file(r->source_path);
        if (!f) {
            return ECUIO_ERR_RECORDING_CHANGED;
        }
        std::string found;
        uint64_t found_us = ECUIO_RECORDING_NONE;
        for (size_t k = 0; k < found_seconds; ++k) {
            const uint32_t second = seconds[k].second;
            if (second < sec &&
                seconds[k].last_offset != ECUIO_RECORDING_NONE) {
                /* A whole second before t's: its last row is the one */
                const ecuio_status_t rc = r->row_at(
                    f.get(), seconds[k].last_offset, signal, &found, &found_us);
                if (rc != ECUIO_OK) {
                    return rc;
                }
                break;
            }
            const index_entry* e = r->index_of(second);
            if (e == nullptr) {
                return ECUIO_ERR_RECORDING_CHANGED;
            }
            const ecuio_status_t rc =
                r->scan_second(f.get(), *e,
                               [&](uint32_t sig, uint64_t host_us,
                                   const std::string& value, uint64_t) {
                                   if (sig == signal && host_us <= t_us) {
                                       found = value;
                                       found_us = host_us;
                                   }
                               });
            if (rc != ECUIO_OK) {
                return rc;
            }
            if (found_us != ECUIO_RECORDING_NONE) {
                break;
            }
        }
        if (found_us != ECUIO_RECORDING_NONE) {
            copy_cut(utf8, capacity, found);
            *at_us = found_us;
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

namespace {

/* A query's stop: asked before each second's rows */
struct query_stop {
    ecuio_cancel_t cancel = nullptr;
    void* user = nullptr;

    bool asked() const { return cancel != nullptr && cancel(user) != 0; }
};

ecuio_status_t canceled()
{
    ecuio_detail_set("the search was canceled");
    return ECUIO_ERR_CANCELED;
}

/* The first sample of a number signal from from_us on that reaches level
 * (rising: at or above it; falling: at or below) after a sample past
 * hysteresis from it (rising: below level - hysteresis) armed it */
ecuio_status_t find_crossing(const ecuio_recording_t* r, uint32_t signal,
                             bool rising, double level, double hysteresis,
                             uint64_t from_us, const query_stop& stop,
                             uint64_t* at_us)
{
    const signal_entry& s = r->signals[signal];
    const double arm_at = rising ? level - hysteresis : level + hysteresis;
    auto arms = [&](double v) { return rising ? v < arm_at : v > arm_at; };
    auto fires = [&](double v) { return rising ? v >= level : v <= level; };
    bool armed = false;
    try {
        const auto first_second = static_cast<uint32_t>(
            (std::min<uint64_t>)(from_us / kSecondUs, UINT32_MAX));
        auto it = std::lower_bound(
            s.numbers.begin(), s.numbers.end(), first_second,
            [](const number_second& e, uint32_t v) { return e.second < v; });
        FILE* f = nullptr;
        file_ptr owner;
        for (; it != s.numbers.end(); ++it) {
            /* A second that can neither arm nor fire is passed by its
             * summary: its range says so for every sample in it */
            if (!std::isnan(it->lowest)) {
                if (!armed && !arms(rising ? it->lowest : it->highest)) {
                    continue;
                }
                if (armed && !fires(rising ? it->highest : it->lowest)) {
                    continue;
                }
            }
            if (stop.asked()) {
                return canceled();
            }
            const index_entry* e = r->index_of(it->second);
            if (e == nullptr) {
                return ECUIO_ERR_RECORDING_CHANGED;
            }
            std::shared_ptr<const std::vector<raw_row>> rows;
            const ecuio_status_t rc = r->rows_of(f, owner, *e, &rows);
            if (rc != ECUIO_OK) {
                return rc;
            }
            for (const raw_row& row : *rows) {
                if (row.signal != signal || row.host_us < from_us) {
                    continue;
                }
                if (armed && fires(row.value)) {
                    *at_us = row.host_us;
                    return ECUIO_OK;
                }
                if (arms(row.value)) {
                    armed = true;
                }
            }
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

/* "rpm" in any ASCII case, the whole unit */
bool is_rpm(const char* unit)
{
    static const char kRpm[] = "rpm";
    for (size_t i = 0; i < sizeof kRpm; ++i) {
        char c = unit[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kRpm[i]) {
            return false;
        }
    }
    return true;
}

char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/* Whether text is word (lower case), ASCII case ignored */
bool is_word(const char* text, const char* word)
{
    for (; *word != '\0'; ++text, ++word) {
        if (ascii_lower(*text) != *word) {
            return false;
        }
    }
    return *text == '\0';
}

/* Whether text holds word (lower case), ASCII case ignored */
bool holds(const char* text, const char* word)
{
    const size_t n = std::strlen(word);
    for (const char* p = text; *p != '\0'; ++p) {
        size_t k = 0;
        while (k < n && p[k] != '\0' && ascii_lower(p[k]) == word[k]) {
            ++k;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

/* Whether a signal can play role, by its unit and name */
bool plays(const signal_entry& s, uint32_t role)
{
    if (s.text()) {
        return false;
    }
    const char* unit = s.pub.unit;
    const char* name = s.pub.name;
    switch (role) {
    case ECUIO_RECORDING_ROLE_ENGINE_SPEED:
        return is_rpm(unit);
    case ECUIO_RECORDING_ROLE_VEHICLE_SPEED:
        return (is_word(unit, "km/h") || is_word(unit, "mph")) &&
               holds(name, "speed");
    case ECUIO_RECORDING_ROLE_THROTTLE:
        return is_word(unit, "%") &&
               (holds(name, "throttle") || holds(name, "pedal")) &&
               !holds(name, "relative") && !holds(name, "commanded");
    case ECUIO_RECORDING_ROLE_ENGINE_LOAD:
        return is_word(unit, "%") && holds(name, "load");
    default:
        return false;
    }
}

/* The signal playing role: the lowest nonzero rank among several, else
 * the first; UINT32_MAX with none */
uint32_t role_signal(const ecuio_recording_t* r, uint32_t role)
{
    uint32_t best = UINT32_MAX;
    for (uint32_t k = 0; k < r->signals.size(); ++k) {
        const signal_entry& s = r->signals[k];
        if (!plays(s, role)) {
            continue;
        }
        const uint8_t rank = s.pub.plot_rank;
        if (best == UINT32_MAX ||
            (rank != 0 && (r->signals[best].pub.plot_rank == 0 ||
                           rank < r->signals[best].pub.plot_rank))) {
            best = k;
        }
    }
    return best;
}

/* The engine-speed signal: a number signal in rpm */
uint32_t engine_speed_signal(const ecuio_recording_t* r)
{
    return role_signal(r, ECUIO_RECORDING_ROLE_ENGINE_SPEED);
}

} // namespace

ecuio_status_t ecuio_recording_find(const ecuio_recording_t* r,
                                    const ecuio_recording_find_t* what,
                                    uint64_t from_us, uint64_t* at_us)
{
    return ecuio_recording_find_with(r, what, from_us, nullptr, nullptr, at_us);
}

ecuio_status_t ecuio_recording_find_with(const ecuio_recording_t* r,
                                         const ecuio_recording_find_t* what,
                                         uint64_t from_us,
                                         ecuio_cancel_t cancel, void* user_data,
                                         uint64_t* at_us)
{
    if (r == nullptr || what == nullptr || at_us == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    for (uint64_t v : what->reserved) {
        if (v != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    if (what->flags != 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *at_us = ECUIO_RECORDING_NONE;
    const query_stop stop{cancel, user_data};
    if (what->kind == ECUIO_RECORDING_FIND_MARKER) {
        if (what->label_utf8 == nullptr) {
            return ECUIO_ERR_NULL_ARGUMENT;
        }
        if (stop.asked()) {
            return canceled();
        }
        for (const marker_entry& m : r->markers) {
            if (m.host_us >= from_us && m.label == what->label_utf8) {
                *at_us = m.host_us;
                break;
            }
        }
        return ECUIO_OK;
    }
    if (what->kind == ECUIO_RECORDING_FIND_ENGINE_START) {
        /* A NaN is refused too: it equals nothing */
        if (what->signal != 0 || what->direction != 0 || what->level != 0.0 ||
            what->hysteresis != 0.0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        if (stop.asked()) {
            return canceled();
        }
        const uint32_t rpm = engine_speed_signal(r);
        if (rpm == UINT32_MAX) {
            return ECUIO_OK;
        }
        return find_crossing(r, rpm, true, ECUIO_ENGINE_START_RPM,
                             ECUIO_ENGINE_START_RPM - ECUIO_ENGINE_STOPPED_RPM,
                             from_us, stop, at_us);
    }
    if (what->kind != ECUIO_RECORDING_FIND_THRESHOLD ||
        what->signal >= r->signals.size() ||
        (what->direction != ECUIO_RECORDING_FIND_RISING &&
         what->direction != ECUIO_RECORDING_FIND_FALLING) ||
        !(what->hysteresis >= 0.0) || std::isnan(what->level)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (r->signals[what->signal].text()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (stop.asked()) {
        return canceled();
    }
    return find_crossing(r, what->signal,
                         what->direction == ECUIO_RECORDING_FIND_RISING,
                         what->level, what->hysteresis, from_us, stop, at_us);
}

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

ecuio_status_t ecuio_recording_role_signal(const ecuio_recording_t* r,
                                           uint32_t role, uint32_t* index)
{
    if (r == nullptr || index == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (role < ECUIO_RECORDING_ROLE_ENGINE_SPEED ||
        role > ECUIO_RECORDING_ROLE_ENGINE_LOAD) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *index = role_signal(r, role);
    return ECUIO_OK;
}

namespace {

/* How far under its highest a pull's speed may sit and be at it */
constexpr double kPullTolerance = 0.0;

enum : size_t { kThrottle, kLoad, kSpeed, kRpm, kRoles };

/* A rule's level or band: its default for 0; false for a negative or NaN */
bool level_of(double given, double fallback, double* out)
{
    if (std::isnan(given) || given < 0.0) {
        return false;
    }
    *out = given == 0.0 ? fallback : given;
    return true;
}

uint64_t time_of(uint64_t given, uint64_t fallback)
{
    return given == 0 ? fallback : given;
}

/* A role's signal: the rules' index, or core's pick for UINT32_MAX;
 * false for an index past the signals or on a text signal */
bool role_of(const ecuio_recording_t* r, uint32_t given, uint32_t role,
             uint32_t* out)
{
    if (given == UINT32_MAX) {
        *out = role_signal(r, role);
        return true;
    }
    if (given >= r->signals.size() || r->signals[given].text()) {
        return false;
    }
    *out = given;
    return true;
}

/* Each role signal's samples in [from_us, to_us] and the one on each side
 * of it, in one pass over the rows */
ecuio_status_t gather(const ecuio_recording_t* r, const uint32_t* signal,
                      uint64_t from_us, uint64_t to_us, const query_stop& stop,
                      ecuio_aids::samples* out)
{
    uint64_t lo[kRoles];
    uint64_t hi[kRoles];
    uint64_t first = UINT64_MAX;
    uint64_t last = 0;
    for (size_t k = 0; k < kRoles; ++k) {
        lo[k] = from_us;
        hi[k] = to_us;
        if (signal[k] == UINT32_MAX) {
            continue;
        }
        uint64_t at = ECUIO_RECORDING_NONE;
        ecuio_status_t rc = ecuio_recording_step(
            r, signal[k], from_us, ECUIO_RECORDING_STEP_BACKWARD, &at);
        if (rc != ECUIO_OK) {
            return rc;
        }
        if (at != ECUIO_RECORDING_NONE) {
            lo[k] = at;
        }
        rc = ecuio_recording_step(r, signal[k], to_us,
                                  ECUIO_RECORDING_STEP_FORWARD, &at);
        if (rc != ECUIO_OK) {
            return rc;
        }
        if (at != ECUIO_RECORDING_NONE) {
            hi[k] = at;
        }
        first = (std::min)(first, lo[k]);
        last = (std::max)(last, hi[k]);
    }
    if (first == UINT64_MAX) {
        return ECUIO_OK;
    }
    auto it = std::lower_bound(
        r->index.begin(), r->index.end(), first / kSecondUs,
        [](const index_entry& e, uint64_t v) { return e.second < v; });
    FILE* f = nullptr;
    file_ptr owner;
    for (; it != r->index.end() && it->second <= last / kSecondUs; ++it) {
        if (stop.asked()) {
            return canceled();
        }
        std::shared_ptr<const std::vector<raw_row>> rows;
        const ecuio_status_t rc = r->rows_of(f, owner, *it, &rows);
        if (rc != ECUIO_OK) {
            return rc;
        }
        for (const raw_row& row : *rows) {
            for (size_t k = 0; k < kRoles; ++k) {
                if (row.signal == signal[k] && row.host_us >= lo[k] &&
                    row.host_us <= hi[k]) {
                    out[k].push_back(
                        ecuio_aids::sample{row.host_us, row.value});
                }
            }
        }
    }
    return ECUIO_OK;
}

} // namespace

ecuio_status_t ecuio_recording_stretches(const ecuio_recording_t* r,
                                         const ecuio_stretch_rules_t* rules,
                                         uint64_t from_us, uint64_t to_us,
                                         ecuio_recording_stretch_t* out,
                                         uint32_t capacity, uint32_t* count)
{
    return ecuio_recording_stretches_with(r, rules, from_us, to_us, nullptr,
                                          nullptr, out, capacity, count);
}

ecuio_status_t ecuio_recording_stretches_with(
    const ecuio_recording_t* r, const ecuio_stretch_rules_t* rules,
    uint64_t from_us, uint64_t to_us, ecuio_cancel_t cancel, void* user_data,
    ecuio_recording_stretch_t* out, uint32_t capacity, uint32_t* count)
{
    if (r == nullptr || rules == nullptr || count == nullptr ||
        (out == nullptr && capacity != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const uint32_t known = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    if (rules->kinds == 0 || (rules->kinds & ~known) != 0 ||
        (rules->flags & ~ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED) != 0 ||
        from_us >= to_us) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint64_t v : rules->reserved) {
        if (v != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    double pull_level = 0;
    double load_level = 0;
    double speed_band = 0;
    double throttle_band = 0;
    double floor = 0;
    double min_rise = 0;
    double min_gain = 0;
    uint32_t signal[kRoles] = {};
    if (!level_of(rules->pull_level, ECUIO_STRETCH_DEFAULT_PULL_LEVEL,
                  &pull_level) ||
        !level_of(rules->pull_min_rise, ECUIO_STRETCH_DEFAULT_PULL_RISE,
                  &min_rise) ||
        !level_of(rules->pull_min_gain, ECUIO_STRETCH_DEFAULT_PULL_GAIN,
                  &min_gain) ||
        !level_of(rules->pull_load_level, ECUIO_STRETCH_DEFAULT_PULL_LOAD_LEVEL,
                  &load_level) ||
        !level_of(rules->cruise_speed_band,
                  ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND, &speed_band) ||
        !level_of(rules->cruise_throttle_band,
                  ECUIO_STRETCH_DEFAULT_CRUISE_THROTTLE_BAND, &throttle_band) ||
        !level_of(rules->cruise_floor, ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR,
                  &floor) ||
        !role_of(r, rules->throttle, ECUIO_RECORDING_ROLE_THROTTLE,
                 &signal[kThrottle]) ||
        !role_of(r, rules->engine_load, ECUIO_RECORDING_ROLE_ENGINE_LOAD,
                 &signal[kLoad]) ||
        !role_of(r, rules->vehicle_speed, ECUIO_RECORDING_ROLE_VEHICLE_SPEED,
                 &signal[kSpeed]) ||
        !role_of(r, rules->engine_speed, ECUIO_RECORDING_ROLE_ENGINE_SPEED,
                 &signal[kRpm])) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *count = 0;
    const uint64_t pull_hold =
        time_of(rules->pull_hold_us, ECUIO_STRETCH_DEFAULT_PULL_HOLD_US);
    const uint64_t cruise_hold =
        time_of(rules->cruise_hold_us, ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US);
    const query_stop stop{cancel, user_data};
    if (stop.asked()) {
        return canceled();
    }
    try {
        ecuio_aids::samples s[kRoles];
        const ecuio_status_t rc = gather(r, signal, from_us, to_us, stop, s);
        if (rc != ECUIO_OK) {
            return rc;
        }
        const bool has_throttle = signal[kThrottle] != UINT32_MAX;
        const bool has_speed = signal[kSpeed] != UINT32_MAX;
        const size_t gate = has_throttle ? kThrottle : kLoad;
        const size_t measure = has_speed ? kSpeed : kRpm;
        const ecuio_aids::samples none;
        const ecuio_aids::samples* engine_speed =
            signal[kRpm] != UINT32_MAX ? &s[kRpm] : nullptr;
        /* The rules' km/h in an mph signal's own unit, by the display's
         * table */
        double per_kmh = 1.0;
        ecuio_display_value_t mph{};
        if (has_speed && is_word(r->signals[signal[kSpeed]].pub.unit, "mph") &&
            ecuio_display_convert("km/h", 0, 1.0, ECUIO_UNITS_IMPERIAL, &mph) ==
                ECUIO_DISPLAY_OK) {
            per_kmh = mph.value;
        }
        const ecuio_aids::pull_rules pull{
            has_throttle ? pull_level : load_level,
            pull_hold,
            time_of(rules->pull_gap_us, ECUIO_STRETCH_DEFAULT_PULL_GAP_US),
            kPullTolerance,
            min_rise,
            min_gain * per_kmh};
        std::vector<ecuio_aids::stretch> found;
        if ((rules->kinds & ECUIO_STRETCH_PULLS) != 0 &&
            signal[gate] != UINT32_MAX && signal[measure] != UINT32_MAX) {
            const uint32_t flags =
                (has_throttle ? 0u : ECUIO_RECORDING_STRETCH_BY_LOAD) |
                (has_speed ? 0u : ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
            ecuio_aids::find_pulls(
                s[gate], s[measure], has_speed,
                (rules->flags & ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED) != 0,
                engine_speed, pull, flags, found);
        }
        if ((rules->kinds & ECUIO_STRETCH_CRUISES) != 0 && has_speed) {
            const ecuio_aids::cruise_rules cruise{
                cruise_hold, speed_band * per_kmh, throttle_band,
                floor * per_kmh};
            ecuio_aids::find_cruises(
                s[kSpeed], has_throttle ? &s[kThrottle] : nullptr, cruise,
                has_throttle ? 0u : ECUIO_RECORDING_STRETCH_NO_THROTTLE, found);
        }
        ecuio_aids::cut_to(
            found, from_us, to_us, pull, has_speed, engine_speed, cruise_hold,
            signal[measure] != UINT32_MAX ? s[measure] : none, s[kSpeed]);
        *count =
            static_cast<uint32_t>((std::min<size_t>)(found.size(), UINT32_MAX));
        for (uint32_t i = 0; i < capacity && i < *count; ++i) {
            const ecuio_aids::stretch& f = found[i];
            ecuio_recording_stretch_t& o = out[i];
            o = ecuio_recording_stretch_t{};
            o.kind = f.kind;
            o.signal = f.kind == ECUIO_RECORDING_STRETCH_PULL ? signal[measure]
                                                              : signal[kSpeed];
            o.from_us = f.from_us;
            o.to_us = f.to_us;
            o.start = f.start;
            o.end = f.end;
            o.flags = f.flags;
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void ecuio_recording_close(ecuio_recording_t* r)
{
    delete r;
}

uint64_t ecuio_recording_test_table_bytes(const ecuio_recording_t* r)
{
    if (r == nullptr) {
        return 0;
    }
    uint64_t n = r->index.capacity() * sizeof(index_entry) +
                 r->marks.capacity() * sizeof(frame_mark) +
                 r->lengths.capacity() * 256 +
                 r->latest_pool.capacity() * sizeof(saved_latest);
    for (const signal_entry& s : r->signals) {
        n += s.numbers.capacity() * sizeof(number_second) +
             s.texts.capacity() * sizeof(text_second);
    }
    return n;
}

void ecuio_recording_test_set_cache_limit(ecuio_recording_t* r, uint64_t bytes)
{
    if (r != nullptr) {
        std::lock_guard<std::mutex> g(r->cache_m);
        r->cache_limit = static_cast<size_t>(bytes);
        r->evict();
    }
}

uint64_t ecuio_recording_test_cache_bytes(const ecuio_recording_t* r)
{
    if (r == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> g(r->cache_m);
    return r->cache_bytes;
}

} // extern "C"
