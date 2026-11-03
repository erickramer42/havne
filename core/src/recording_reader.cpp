// File/layer: core/src/recording_reader.cpp | C++17, Qt-free; the reader
//             (ADR-0049 section 5)
// Testing: HARNESS: RecordingTest (test_core): folders the recorder wrote
//          (the mock, the replay fixture) and folders written by hand
//          (an older recording, torn and refused rows, markers); the
//          queries against a scan of the rows, from several threads, and
//          the memory bound through the seams in internal.h
//
// One pass at open reads events.log's header (the format, the UTC start,
// each signal's kind, decimal places and plot rank) and signals.csv, and
// builds per signal a summary for each second of host_us it has samples
// in, beside each second's byte offset in signals.csv. Queries answer
// from the summaries, and closer than a second a column from the raw
// rows of the seconds they need, which a cache of 8 MiB keeps.

#include "error_detail.h"
#include "frames_csv.h" /* native_path, file_size, seek_to */
#include "recording_rows.h"

#include "ecuio/internal.h"
#include "ecuio/recording.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace rows = ecuio_recording_rows;
using ecuio_frames_csv::file_size;
using ecuio_frames_csv::seek_to;

namespace {

constexpr uint64_t kSecondUs = 1000000;
constexpr uint64_t kProgressBytes = 4u << 20;
constexpr size_t kCacheBytes = 8u << 20;
constexpr size_t kChunk = 64u << 10;

/* A number signal's second: 32 bytes, the bound's figure */
struct number_second {
    uint32_t second;
    uint32_t count;
    double lowest; /* NaN until a value that isn't NaN */
    double highest;
    double last;
};
static_assert(sizeof(number_second) == 32, "the memory bound's 32 bytes");

/* A text signal's second: 16 bytes, its last row's offset for text_at */
struct text_second {
    uint32_t second;
    uint32_t count;
    uint64_t last_offset;
};
static_assert(sizeof(text_second) == 16, "the memory bound's 16 bytes");

/* A second of signals.csv: where its first row starts */
struct index_entry {
    uint64_t offset;
    uint32_t second;
    uint32_t reserved;
};
static_assert(sizeof(index_entry) == 16, "the memory bound's 16 bytes");

/* A raw row as the cache keeps it */
struct raw_row {
    uint64_t host_us;
    uint32_t signal;
    uint32_t reserved;
    double value; /* 0 for a text signal's */
};

void merge(double& lowest, double& highest, double v)
{
    if (std::isnan(v)) {
        return;
    }
    if (std::isnan(lowest) || v < lowest) {
        lowest = v;
    }
    if (std::isnan(highest) || v > highest) {
        highest = v;
    }
}

struct signal_entry {
    ecuio_recording_signal_t pub{};
    bool known_kind = false; /* from events.log's signal line */
    bool not_number = false; /* inferring: a value that isn't a number */
    unsigned decimals_seen = 0;
    double lowest = NAN;
    double highest = NAN;
    std::vector<number_second> numbers;
    std::vector<text_second> texts;

    bool text() const { return (pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0; }
};

struct marker_entry {
    uint64_t host_us;
    std::string label;
};

/* Lines from a file, their byte offsets counted exactly (a NUL is a byte
 * like any other, so fgets and strlen won't do) */
class line_reader
{
public:
    line_reader(FILE* f, uint64_t start) : f_(f), offset_(start)
    {
        buf_.resize(kChunk);
    }

    /* The next line without its ending. False at the end of the file.
     * *ended is false for the file's last bytes with no newline after
     * them; *too_long for a line past rows::kRowMax (its tail not kept). */
    bool next(std::string& line, bool* ended, bool* too_long)
    {
        line.clear();
        *ended = false;
        *too_long = false;
        start_ = offset_;
        bool any = false;
        for (;;) {
            if (pos_ == len_) {
                len_ = std::fread(buf_.data(), 1, buf_.size(), f_);
                pos_ = 0;
                if (len_ == 0) {
                    return any;
                }
            }
            any = true;
            const char* s = buf_.data() + pos_;
            const auto* nl =
                static_cast<const char*>(std::memchr(s, '\n', len_ - pos_));
            const size_t take =
                nl != nullptr ? static_cast<size_t>(nl - s) : len_ - pos_;
            if (!*too_long) {
                if (line.size() + take > rows::kRowMax + 1) {
                    *too_long = true;
                } else {
                    line.append(s, take);
                }
            }
            pos_ += take;
            offset_ += take;
            if (nl != nullptr) {
                ++pos_;
                ++offset_;
                *ended = true;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return true;
            }
        }
    }

    uint64_t line_start() const { return start_; }
    uint64_t offset() const { return offset_; }

private:
    FILE* f_;
    std::vector<char> buf_;
    size_t pos_ = 0;
    size_t len_ = 0;
    uint64_t offset_;
    uint64_t start_ = 0;
};

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};
using file_ptr = std::unique_ptr<FILE, file_closer>;

file_ptr open_file(const std::string& utf8)
{
    ecuio_frames_csv::native_path p;
    if (!p.set(utf8)) {
        return nullptr;
    }
    return file_ptr(p.open());
}

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

struct ecuio_recording {
    std::string signals_path;
    uint64_t end_offset = 0; /* past signals.csv's last row the open read */
    ecuio_recording_info_t info{};
    std::vector<signal_entry> signals;
    std::unordered_map<std::string, uint32_t> by_name;
    std::vector<index_entry> index;
    std::vector<marker_entry> markers;

    /* The raw rows' cache: the seconds parsed last, newest first */
    struct block {
        uint32_t second;
        std::shared_ptr<const std::vector<raw_row>> rows;
    };
    mutable std::mutex cache_m;
    mutable std::list<block> lru;
    mutable std::unordered_map<uint32_t, std::list<block>::iterator> cached;
    mutable size_t cache_bytes = 0;
    size_t cache_limit = kCacheBytes; /* a test's seam lowers it */

    uint32_t add_signal(const std::string& name);
    bool read_events(const std::string& folder);
    bool read_signals(ecuio_open_progress_t progress, void* user,
                      bool* canceled);
    bool read_markers(const std::string& folder);
    void finish();

    const index_entry* index_of(uint32_t second) const;
    template <typename Fn>
    ecuio_status_t scan_second(FILE* f, const index_entry& e, Fn fn) const;
    ecuio_status_t row_at(FILE* f, uint64_t offset, uint32_t signal,
                          std::string* value, uint64_t* host_us) const;
    void evict() const; /* under cache_m */
    ecuio_status_t
    rows_of(FILE*& f, file_ptr& owner, const index_entry& e,
            std::shared_ptr<const std::vector<raw_row>>* out) const;
};

uint32_t ecuio_recording::add_signal(const std::string& name)
{
    auto it = by_name.find(name);
    if (it != by_name.end()) {
        return it->second;
    }
    signal_entry s;
    std::memcpy(s.pub.name, name.data(), name.size());
    s.pub.first_us = ECUIO_RECORDING_NONE;
    s.pub.last_us = ECUIO_RECORDING_NONE;
    signals.push_back(std::move(s));
    const auto i = static_cast<uint32_t>(signals.size() - 1);
    by_name.emplace(name, i);
    return i;
}

/* events.log: the format, the header's signals and UTC start, the
 * summary or the drop lines, and the latest line's time. A folder with
 * no events.log opens as an unknown format with no summary. */
bool ecuio_recording::read_events(const std::string& folder)
{
    info.flags |= ECUIO_RECORDING_INFO_NO_SUMMARY;
    file_ptr f = open_file(folder + "/events.log");
    if (!f) {
        return true;
    }
    line_reader lines(f.get(), 0);
    std::string line;
    bool ended = false;
    bool too_long = false;
    uint64_t line_no = 0;
    bool in_header = false;
    uint64_t drop_frames = 0;
    bool summary = false;
    rows::header_signal hs;
    while (lines.next(line, &ended, &too_long)) {
        ++line_no;
        if (!ended || too_long) {
            continue; /* a torn last line, or one no writer makes */
        }
        const char* p = line.data();
        size_t n = line.size();
        if (line_no == 1) {
            const size_t bom = rows::bom_length(p, n);
            p += bom;
            n -= bom;
            const size_t m = std::strlen(rows::kFormat1End);
            if (rows::equals(p, n, rows::kFormat2)) {
                info.format = ECUIO_RECORDING_FORMAT_CORE;
                in_header = true;
                continue;
            }
            if (n >= m && std::memcmp(p + n - m, rows::kFormat1End, m) == 0) {
                info.format = ECUIO_RECORDING_FORMAT_OLDER;
                in_header = true;
                continue;
            }
        }
        if (in_header) {
            if (n == 0) {
                in_header = false;
                continue;
            }
            if (info.format != ECUIO_RECORDING_FORMAT_CORE) {
                continue;
            }
            const char* why = nullptr;
            if (rows::starts_with(p, n, "signal  ")) {
                if (!rows::parse_header_signal(p + 8, n - 8, hs, &why)) {
                    ecuio_detail_set("events.log line %llu: %s",
                                     static_cast<unsigned long long>(line_no),
                                     why);
                    return false;
                }
                if (by_name.find(hs.name) == by_name.end()) {
                    signal_entry& s = signals[add_signal(hs.name)];
                    std::memcpy(s.pub.unit, hs.unit.data(), hs.unit.size());
                    s.known_kind = true;
                    s.pub.flags = hs.text ? ECUIO_RECORDING_SIGNAL_TEXT : 0;
                    s.pub.decimal_places = hs.decimal_places;
                    s.pub.plot_rank = hs.plot_rank;
                    s.pub.group = hs.group;
                }
            } else if (rows::starts_with(p, n, "utc  ")) {
                if (!rows::parse_utc(p + 5, n - 5, &info.utc_start_us)) {
                    ecuio_detail_set("events.log line %llu: the utc line "
                                     "isn't yyyy-MM-ddTHH:mm:ss.zzzZ",
                                     static_cast<unsigned long long>(line_no));
                    return false;
                }
            }
            continue;
        }
        rows::event_line e;
        if (!rows::parse_event_line(p, n, &e)) {
            /* ADR-0017's summary is an untimed "end" line */
            if (info.format == ECUIO_RECORDING_FORMAT_OLDER &&
                rows::starts_with(p, n, "end ")) {
                summary = true;
            }
            continue;
        }
        info.duration_us = (std::max)(info.duration_us, e.ms * 1000);
        const char* text = nullptr;
        size_t text_len = 0;
        size_t kind_len = 0;
        if (info.format == ECUIO_RECORDING_FORMAT_OLDER ||
            !rows::split_kind(e.rest, e.rest_len, &text, &text_len,
                              &kind_len)) {
            continue;
        }
        uint64_t count = 0;
        if (rows::equals(e.rest, kind_len, "end")) {
            uint64_t records = 0;
            uint64_t notes = 0;
            if (rows::parse_end(text, text_len, &records, &notes)) {
                summary = true;
                info.records_dropped = records;
                info.notes_dropped = notes;
            }
        } else if (rows::equals(e.rest, kind_len, "drop") &&
                   rows::parse_drop(text, text_len, &count)) {
            drop_frames += count;
        }
    }
    if (summary) {
        info.flags &= ~ECUIO_RECORDING_INFO_NO_SUMMARY;
    } else {
        info.records_dropped = drop_frames;
    }
    return true;
}

bool ecuio_recording::read_signals(ecuio_open_progress_t progress, void* user,
                                   bool* canceled)
{
    file_ptr f = open_file(signals_path);
    if (!f) {
        ecuio_detail_set("the folder has no signals.csv");
        return false;
    }
    const uint64_t total = file_size(f.get());
    if (progress != nullptr && progress(0, total, user) != 0) {
        *canceled = true;
        return false;
    }
    line_reader lines(f.get(), 0);
    std::string line;
    bool ended = false;
    bool too_long = false;
    if (!lines.next(line, &ended, &too_long) || !ended || too_long) {
        ecuio_detail_set("signals.csv has no header row");
        return false;
    }
    const size_t bom = rows::bom_length(line.data(), line.size());
    if (!rows::equals(line.data() + bom, line.size() - bom,
                      rows::kSignalsHeader)) {
        ecuio_detail_set("signals.csv's header row isn't %s",
                         rows::kSignalsHeader);
        return false;
    }
    end_offset = lines.offset();
    uint64_t line_no = 1;
    uint64_t prev_us = 0;
    uint64_t next_progress = kProgressBytes;
    rows::signal_row r;
    while (lines.next(line, &ended, &too_long)) {
        ++line_no;
        if (!ended) {
            ++info.rows_torn; /* section 7: a write cut short */
            break;
        }
        const char* why = nullptr;
        if (too_long) {
            why = "the row is longer than a recording's rows can be";
        } else if (!rows::parse_signal_row(line.data(), line.size(), r, &why)) {
            /* why is set */
        } else if (r.host_us < prev_us) {
            why = "host_us is earlier than the row before it";
        } else if (r.host_us / kSecondUs > UINT32_MAX) {
            why = "host_us is past what a recording spans";
        }
        signal_entry* s = nullptr;
        double v = 0.0;
        bool number = false;
        if (why == nullptr) {
            s = &signals[add_signal(r.name)];
            if (!s->known_kind && s->pub.unit[0] == '\0') {
                std::memcpy(s->pub.unit, r.unit.data(), r.unit.size());
            }
            number = rows::parse_number(r.value.data(), r.value.size(), &v);
            if (s->known_kind && !s->text() && !number) {
                why = "the value isn't a number";
            }
        }
        if (why != nullptr) {
            ecuio_detail_set("signals.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            return false;
        }
        prev_us = r.host_us;
        const auto second = static_cast<uint32_t>(r.host_us / kSecondUs);
        const uint64_t at = lines.line_start();
        if (index.empty() || index.back().second != second) {
            index.push_back(index_entry{at, second, 0});
        }
        end_offset = lines.offset();

        ecuio_recording_signal_t& pub = s->pub;
        ++pub.samples;
        if (pub.first_us == ECUIO_RECORDING_NONE) {
            pub.first_us = r.host_us;
        }
        pub.last_us = r.host_us;
        const bool track_text = !s->known_kind || s->text();
        const bool track_number = s->known_kind ? !s->text() : !s->not_number;
        if (!s->known_kind) {
            if (!number) {
                s->not_number = true;
            } else {
                s->decimals_seen =
                    (std::max)(s->decimals_seen,
                               rows::decimals_shown(r.value.data(),
                                                    r.value.size()));
            }
        }
        if (track_text) {
            if (s->texts.empty() || s->texts.back().second != second) {
                s->texts.push_back(text_second{second, 1, at});
            } else {
                ++s->texts.back().count;
                s->texts.back().last_offset = at;
            }
        }
        if (track_number && number) {
            if (s->numbers.empty() || s->numbers.back().second != second) {
                s->numbers.push_back(number_second{second, 0, NAN, NAN, v});
            }
            number_second& ns = s->numbers.back();
            ++ns.count;
            ns.last = v;
            merge(ns.lowest, ns.highest, v);
            merge(s->lowest, s->highest, v);
        }
        if (progress != nullptr && lines.offset() >= next_progress) {
            next_progress = lines.offset() + kProgressBytes;
            if (progress(lines.offset(), total, user) != 0) {
                *canceled = true;
                return false;
            }
        }
    }
    info.duration_us = (std::max)(info.duration_us, prev_us);
    if (progress != nullptr && progress(total, total, user) != 0) {
        *canceled = true;
        return false;
    }
    return true;
}

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
template <typename Fn>
ecuio_status_t ecuio_recording::scan_second(FILE* f, const index_entry& e,
                                            Fn fn) const
{
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
        owner = open_file(signals_path);
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
                bad = true;
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
        r->signals_path = folder + "/signals.csv";
        bool canceled = false;
        if (!r->read_events(folder) ||
            !r->read_signals(progress, user_data, &canceled) ||
            !r->read_markers(folder)) {
            return canceled ? ECUIO_ERR_CANCELED : ECUIO_ERR_LOAD_FAILED;
        }
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
        file_ptr f = open_file(r->signals_path);
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

ecuio_status_t ecuio_recording_find(const ecuio_recording_t* r,
                                    const ecuio_recording_find_t* what,
                                    uint64_t from_us, uint64_t* at_us)
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
    if (what->kind == ECUIO_RECORDING_FIND_MARKER) {
        if (what->label_utf8 == nullptr) {
            return ECUIO_ERR_NULL_ARGUMENT;
        }
        for (const marker_entry& m : r->markers) {
            if (m.host_us >= from_us && m.label == what->label_utf8) {
                *at_us = m.host_us;
                break;
            }
        }
        return ECUIO_OK;
    }
    if (what->kind != ECUIO_RECORDING_FIND_THRESHOLD ||
        what->signal >= r->signals.size() ||
        (what->direction != ECUIO_RECORDING_FIND_RISING &&
         what->direction != ECUIO_RECORDING_FIND_FALLING) ||
        !(what->hysteresis >= 0.0) || std::isnan(what->level)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const signal_entry& s = r->signals[what->signal];
    if (s.text()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const bool rising = what->direction == ECUIO_RECORDING_FIND_RISING;
    const double level = what->level;
    const double arm_at =
        rising ? level - what->hysteresis : level + what->hysteresis;
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
                if (row.signal != what->signal || row.host_us < from_us) {
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

void ecuio_recording_close(ecuio_recording_t* r)
{
    delete r;
}

uint64_t ecuio_recording_test_table_bytes(const ecuio_recording_t* r)
{
    if (r == nullptr) {
        return 0;
    }
    uint64_t n = r->index.capacity() * sizeof(index_entry);
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
