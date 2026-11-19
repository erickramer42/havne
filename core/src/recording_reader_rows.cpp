// File/layer: core/src/recording_reader_rows.cpp | C++17, Qt-free;
//             the raw rows, their cache, query and text_at (ADR-0049
//             section 5)
// Testing: UNIT: RecordingTest, RecordingFramesTest (test_core)

#include "recording_reader.h"

#include <algorithm>
#include <new>

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

extern "C" {

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

} // extern "C"
