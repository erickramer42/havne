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
//
// The parts by what they do: recording_reader.h the types and the
// struct; this file the open, the getters, the markers, finish and the
// close; recording_reader_events.cpp events.log's pass;
// recording_reader_source.cpp the source's (signals.csv or frames.csv);
// recording_reader_rows.cpp the raw rows, their cache, query and
// text_at; recording_reader_stats.cpp stats and step;
// recording_reader_search.cpp find, the roles and the stretches. No call
// made for each row crosses a file (no build has link-time optimization).

#include "recording_reader.h"
#include "error_detail.h"
#include "recording_keys.h"
#include "signal_keys.h" /* the find rule */

#include "ecuio/internal.h"

#include <algorithm>
#include <new>

namespace keys = ecuio_recording_keys;

using namespace ecuio_recording_reader;

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
