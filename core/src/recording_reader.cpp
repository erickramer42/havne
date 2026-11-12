// File/layer: core/src/recording_reader.cpp | C++17, Qt-free; the reader
//             (ADR-0049 section 5)
// Testing: UNIT: RecordingTest, RecordingFramesTest, RecordingStatsTest,
//          RecordingStretchesTest (test_core)
//
// One pass at open reads events.log's header (the format, the UTC start,
// each signal's kind, decimal places and plot rank) and signals.csv, and
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

#include "error_detail.h"
#include "frame_decoder.h"
#include "frames_csv.h" /* native_path, file_size, seek_to, parse_row */
#include "recording_aids.h"
#include "isotp.h"
#include "recording_check.h" /* iso15765_id */
#include "recording_rows.h"
#include "sha256.h"

#include "ecuio/internal.h"
#include "ecuio/recording.h"

#include <algorithm>
#include <array>
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

/* A number signal's second: 40 bytes, the bound's figure */
struct number_second {
    uint32_t second;
    uint32_t count;
    double lowest; /* NaN until a value that isn't NaN */
    double highest;
    double last;
    double sum; /* every value's; NaN when one was NaN (or inf met -inf) */
};
static_assert(sizeof(number_second) == 40, "the memory bound's 40 bytes");

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
    std::vector<ecuio_enum_entry_t> labels; /* an enum's, raw ascending */

    bool text() const { return (pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0; }
    bool enumeration() const
    {
        return (pub.flags & ECUIO_RECORDING_SIGNAL_ENUM) != 0;
    }
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

/* The caller's set, copied: the reader decodes with it after the open
 * returns, when the caller may have freed its own */
struct set_copy {
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    std::vector<ecuio_enum_entry_t> enums;
    std::vector<ecuio_computed_t> computed;
    ecuio_signal_set_t set{};

    explicit set_copy(const ecuio_signal_set_t& s)
        : defs(s.defs, s.defs + (s.defs != nullptr ? s.count : 0)),
          channels(s.channels,
                   s.channels + (s.channels != nullptr ? s.channel_count : 0)),
          enums(s.enums, s.enums + (s.enums != nullptr ? s.enum_count : 0)),
          computed(s.computed,
                   s.computed + (s.computed != nullptr ? s.computed_count : 0))
    {
        set = s;
        set.defs = defs.empty() ? nullptr : defs.data();
        set.channels = channels.empty() ? nullptr : channels.data();
        set.enums = enums.empty() ? nullptr : enums.data();
        set.computed = computed.empty() ? nullptr : computed.data();
    }
    set_copy(const set_copy&) = delete;
    set_copy& operator=(const set_copy&) = delete;
};

/* A second of frames.csv's mark, beside its index entry: the row a re-read
 * starts from, the packed replies' lengths then (an index into the
 * reader's copies; none for all unlearned) and the computed signals'
 * inputs then (their first in the reader's pool; none without computed
 * signals). 16 bytes, the bound's figure. */
struct frame_mark {
    uint64_t rescan;
    uint32_t lengths;
    uint32_t latest;
};
static_assert(sizeof(frame_mark) == 16, "the memory bound's 16 bytes");

constexpr uint32_t kNoMark = UINT32_MAX;

/* A computed input's newest value at a mark: its decode counted back from
 * the mark's (kNoMark's width: none yet). 24 bytes, the bound's figure. */
struct saved_latest {
    double value;
    uint64_t time_us;
    uint64_t age;
};
static_assert(sizeof(saved_latest) == 24, "the memory bound's 24 bytes");

/* A restored decoder counts its decodes from here, so an input's age
 * gives back a decode before it */
constexpr uint64_t kDecodeBase = uint64_t{1} << 40;

/* One pass over frames.csv's rows (the open's, or a re-read's): the
 * session's decode, and core's ISO-TP for frames as they crossed the bus.
 * Rows go into row(); take gives what each one has to decode. */
class frame_scan
{
public:
    explicit frame_scan(const ecuio_signal_set_t* set)
        : iso_(new isotp_rx), row_(new ecuio_frame_t{}),
          whole_(new ecuio_frame_t{})
    {
        iso_->timed = true;
        decoder.prepare(set, false);
    }

    ecuio_frame_decoder decoder;

    ecuio_frame_t& row() { return *row_; }

    /* What the row read at host_us has to decode: itself, the message it
     * completes, or nothing (a frame of a message under way, a Flow
     * Control, a First Frame that tears a message on its ID) */
    const ecuio_frame_t* take(uint64_t host_us)
    {
        const auto now = static_cast<int64_t>(host_us);
        (void)isotp_expire(*iso_, now);
        const ecuio_frame_t& f = *row_;
        if (!on_the_wire(f)) {
            return &f;
        }
        isotp_can c;
        c.id = f.arbitration_id;
        c.len = static_cast<uint8_t>(f.data_length);
        std::memcpy(c.data, f.data, f.data_length);
        if (isotp_kind(c) == isotp_pci::first) {
            /* Two senders on one ID (a second tester on the bus) would
             * splice their messages: both end */
            isotp_rx_slot* open = isotp_rx_find(*iso_, c.id);
            if (open != nullptr) {
                open->active = false;
                ++iso_->dropped;
                return nullptr;
            }
        }
        isotp_message m;
        if (isotp_take(*iso_, c, now, &m) != isotp_result::complete) {
            return nullptr;
        }
        whole_->arbitration_id = f.arbitration_id;
        whole_->flags = f.flags | ECUIO_FRAME_FLAG_REASSEMBLED;
        whole_->timestamp_us = f.timestamp_us;
        std::memcpy(whole_->data, m.data, m.len);
        whole_->data_length = m.len;
        return whole_.get();
    }

    /* No message under way: a pass from the next row rebuilds this state */
    bool idle() const
    {
        for (const isotp_rx_slot& s : iso_->slots) {
            if (s.active) {
                return false;
            }
        }
        return true;
    }

private:
    /* A frame as it crossed the bus on ISO 15765-4's IDs: no adapter
     * reassembled it, and its first nibble is a PCI's. A whole message
     * there starts with its service byte, 0x41 to 0x7F for a reply. */
    static bool on_the_wire(const ecuio_frame_t& f)
    {
        return (f.flags &
                (ECUIO_FRAME_FLAG_REASSEMBLED | ECUIO_FRAME_FLAG_ERROR)) == 0 &&
               f.data_length >= 1 && f.data_length <= 8 &&
               (f.data[0] >> 4) <= 3 &&
               ecuio_check::iso15765_id(f.arbitration_id,
                                        (f.flags & ECUIO_FRAME_FLAG_EXTENDED) !=
                                            0);
    }

    std::unique_ptr<isotp_rx> iso_;
    std::unique_ptr<ecuio_frame_t> row_;
    std::unique_ptr<ecuio_frame_t> whole_;
};

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
    /* signals.csv, or frames.csv for a recording opened through its frames */
    std::string source_path;
    uint64_t end_offset = 0; /* past the source's last row the open read */
    ecuio_recording_info_t info{};
    ecuio_recording_signals_t signal_file{};
    std::vector<signal_entry> signals;
    std::unordered_map<std::string, uint32_t> by_name;
    std::vector<index_entry> index;
    std::vector<marker_entry> markers;

    /* Opened through its frames: the set they decode with, each def's
     * signal (UINT32_MAX for one that never decoded), each def by its id,
     * a mark per index entry, the packed replies' lengths at the marks
     * where they changed, the defs the computed signals read and their
     * newest values at each mark */
    bool from_frames = false;
    std::unique_ptr<set_copy> frame_set;
    std::vector<uint32_t> def_signal;
    std::unordered_map<uint16_t, uint32_t> def_of_id;
    std::vector<frame_mark> marks;
    std::vector<std::array<uint8_t, 256>> lengths;
    std::vector<uint32_t> inputs;
    std::vector<saved_latest> latest_pool;

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
    bool read_label(const char* text, size_t n, const char** why);
    bool read_events(const std::string& folder);
    void read_signal_file(const std::string& folder);
    bool read_signals(ecuio_open_progress_t progress, void* user,
                      bool* canceled);
    void take_set(const ecuio_signal_set_t& set);
    bool read_frames(ecuio_open_progress_t progress, void* user,
                     bool* canceled);
    frame_mark mark(const frame_scan& s, uint64_t rescan);
    void restore(frame_scan& s, const frame_mark& m) const;
    template <typename Fn>
    ecuio_status_t scan_frames(FILE* f, const index_entry& e, Fn fn) const;
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

/* A format 3 label line's text: one label of an enum signal a signal
 * line above declared, raw values ascending as a set's enums are, at
 * most a def's ECUIO_ENUM_MAX_ENTRIES */
bool ecuio_recording::read_label(const char* text, size_t n, const char** why)
{
    rows::header_label hl;
    if (!rows::parse_header_label(text, n, hl, why)) {
        return false;
    }
    const auto it = by_name.find(hl.signal);
    if (it == by_name.end() || !signals[it->second].enumeration()) {
        *why = "the label names no enum signal above it";
        return false;
    }
    std::vector<ecuio_enum_entry_t>& labels = signals[it->second].labels;
    if (!labels.empty() && hl.raw <= labels.back().raw) {
        *why = "the signal's labels aren't in ascending raw order";
        return false;
    }
    if (labels.size() >= ECUIO_ENUM_MAX_ENTRIES) {
        *why = "the signal has more labels than an enum holds";
        return false;
    }
    ecuio_enum_entry_t e{};
    e.raw = hl.raw;
    std::memcpy(e.label, hl.label.data(), hl.label.size());
    labels.push_back(e);
    return true;
}

static_assert(ecuio_recording_rows::kLabelMax == ECUIO_ENUM_LABEL_MAX,
              "a label line's bound is an enum entry's");

/* signals.json beside the recording, whether or not the header names
 * it: its length, and with the header's hash whether its bytes are those.
 * Read in pieces, so a large file costs no memory; one that can't be
 * read counts as absent. */
void ecuio_recording::read_signal_file(const std::string& folder)
{
    file_ptr f = open_file(folder + "/" + rows::kSignalFileName);
    if (!f) {
        return;
    }
    ecuio_sha256::hasher h;
    char buf[16384];
    uint64_t length = 0;
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f.get())) > 0) {
        h.add(buf, got);
        length += got;
    }
    if (std::ferror(f.get()) != 0) {
        return;
    }
    signal_file.flags |= ECUIO_RECORDING_SIGNALS_PRESENT;
    signal_file.length = length;
    uint8_t digest[32];
    h.finish(digest);
    if ((signal_file.flags & ECUIO_RECORDING_SIGNALS_NAMED) != 0 &&
        ecuio_sha256::hex(digest) == signal_file.sha256) {
        signal_file.flags |= ECUIO_RECORDING_SIGNALS_MATCH;
    }
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
            if (rows::equals(p, n, rows::kFormat3)) {
                info.format = ECUIO_RECORDING_FORMAT_ENUMS;
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
            if (info.format != ECUIO_RECORDING_FORMAT_CORE &&
                info.format != ECUIO_RECORDING_FORMAT_ENUMS) {
                continue;
            }
            const char* why = nullptr;
            if (from_frames && (rows::starts_with(p, n, "label  ") ||
                                rows::starts_with(p, n, "signal  "))) {
                /* the caller's set names the signals */
            } else if (info.format == ECUIO_RECORDING_FORMAT_ENUMS &&
                       rows::starts_with(p, n, "label  ")) {
                if (!read_label(p + 7, n - 7, &why)) {
                    ecuio_detail_set("events.log line %llu: %s",
                                     static_cast<unsigned long long>(line_no),
                                     why);
                    return false;
                }
            } else if (rows::starts_with(p, n, "signal  ")) {
                bool ok = rows::parse_header_signal(p + 8, n - 8, hs, &why);
                if (ok && hs.enumeration &&
                    info.format == ECUIO_RECORDING_FORMAT_CORE) {
                    why = "format 2 has no enum signals";
                    ok = false;
                }
                if (!ok) {
                    ecuio_detail_set("events.log line %llu: %s",
                                     static_cast<unsigned long long>(line_no),
                                     why);
                    return false;
                }
                if (by_name.find(hs.name) == by_name.end()) {
                    signal_entry& s = signals[add_signal(hs.name)];
                    std::memcpy(s.pub.unit, hs.unit.data(), hs.unit.size());
                    s.known_kind = true;
                    s.pub.flags = hs.text          ? ECUIO_RECORDING_SIGNAL_TEXT
                                  : hs.enumeration ? ECUIO_RECORDING_SIGNAL_ENUM
                                                   : 0;
                    s.pub.decimal_places = hs.decimal_places;
                    s.pub.plot_rank = hs.plot_rank;
                    s.pub.group = hs.group;
                }
            } else if (info.format == ECUIO_RECORDING_FORMAT_ENUMS &&
                       rows::starts_with(p, n, rows::kSignalFileKey)) {
                const size_t key = std::strlen(rows::kSignalFileKey);
                std::string hex;
                if (!rows::parse_signal_file(p + key, n - key, &hex)) {
                    ecuio_detail_set("events.log line %llu: the signal file "
                                     "line isn't signals.json and its "
                                     "SHA-256",
                                     static_cast<unsigned long long>(line_no));
                    return false;
                }
                std::memcpy(signal_file.sha256, hex.data(), hex.size());
                signal_file.sha256[hex.size()] = '\0';
                signal_file.flags |= ECUIO_RECORDING_SIGNALS_NAMED;
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
    file_ptr f = open_file(source_path);
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
                s->numbers.push_back(
                    number_second{second, 0, NAN, NAN, v, 0.0});
            }
            number_second& ns = s->numbers.back();
            ++ns.count;
            ns.last = v;
            ns.sum += v;
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

/* The set's defs as the signals, in its order, before frames.csv's pass:
 * each def's signal is its index until finish drops those with no sample */
void ecuio_recording::take_set(const ecuio_signal_set_t& set)
{
    frame_set.reset(new set_copy(set));
    const ecuio_signal_set_t& s = frame_set->set;
    for (uint32_t i = 0; i < s.count; ++i) {
        const ecuio_signal_def_t& d = s.defs[i];
        signal_entry e;
        std::memcpy(e.pub.name, d.name, strnlen(d.name, sizeof d.name));
        std::memcpy(e.pub.unit, d.unit, strnlen(d.unit, sizeof d.unit));
        e.pub.first_us = ECUIO_RECORDING_NONE;
        e.pub.last_us = ECUIO_RECORDING_NONE;
        e.known_kind = true;
        const bool text =
            d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX;
        e.pub.flags = text                         ? ECUIO_RECORDING_SIGNAL_TEXT
                      : d.kind == ECUIO_VALUE_ENUM ? ECUIO_RECORDING_SIGNAL_ENUM
                                                   : 0;
        e.pub.decimal_places = d.decimal_places;
        e.pub.plot_rank = d.plot_rank;
        e.pub.group = d.group;
        if (d.kind == ECUIO_VALUE_ENUM && s.enums != nullptr) {
            e.labels.assign(s.enums + d.table_first,
                            s.enums + d.table_first + d.table_count);
        }
        signals.push_back(std::move(e));
        def_signal.push_back(i);
        def_of_id.emplace(d.id, i);
    }
}

/* The decoder's state before a second's first row, and rescan: the row a
 * re-read of the second starts from */
frame_mark ecuio_recording::mark(const frame_scan& s, uint64_t rescan)
{
    frame_mark m{rescan, kNoMark, kNoMark};
    const ecuio_frame_decoder& d = s.decoder;
    if (d.record_frame) {
        static const uint8_t kNone[256] = {};
        const uint8_t* last = lengths.empty() ? kNone : lengths.back().data();
        if (std::memcmp(d.learned_lengths, last, sizeof kNone) != 0) {
            lengths.emplace_back();
            std::memcpy(lengths.back().data(), d.learned_lengths, sizeof kNone);
        }
        if (!lengths.empty()) {
            m.lengths = static_cast<uint32_t>(lengths.size() - 1);
        }
    }
    if (!inputs.empty()) {
        m.latest = static_cast<uint32_t>(latest_pool.size());
        for (const uint32_t j : inputs) {
            const ecuio_frame_decoder::latest_value& l = d.latest[j];
            latest_pool.push_back(saved_latest{
                l.value, l.time_us,
                l.decode == 0 ? UINT64_MAX : d.decodes - l.decode});
        }
    }
    return m;
}

void ecuio_recording::restore(frame_scan& s, const frame_mark& m) const
{
    ecuio_frame_decoder& d = s.decoder;
    if (m.lengths != kNoMark) {
        std::memcpy(d.learned_lengths, lengths[m.lengths].data(),
                    sizeof d.learned_lengths);
    }
    if (m.latest != kNoMark) {
        d.decodes = kDecodeBase;
        for (size_t k = 0; k < inputs.size(); ++k) {
            const saved_latest& v = latest_pool[m.latest + k];
            d.latest[inputs[k]] = {v.value, v.time_us,
                                   v.age == UINT64_MAX ? 0
                                                       : kDecodeBase - v.age};
        }
    }
}

/* frames.csv in signals.csv's place: each row decoded as its session
 * decoded it, each value to signals.csv's text and back, so a number is
 * the double signals.csv would give */
bool ecuio_recording::read_frames(ecuio_open_progress_t progress, void* user,
                                  bool* canceled)
{
    file_ptr f = open_file(source_path);
    if (!f) {
        ecuio_detail_set("the folder has no frames.csv");
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
        ecuio_detail_set("frames.csv has no header row");
        return false;
    }
    const size_t bom = rows::bom_length(line.data(), line.size());
    if (!rows::equals(line.data() + bom, line.size() - bom,
                      ecuio_frames_csv::kHeader)) {
        ecuio_detail_set("frames.csv's header row isn't %s",
                         ecuio_frames_csv::kHeader);
        return false;
    }
    end_offset = lines.offset();
    frame_scan scan(&frame_set->set);
    for (const ecuio_frame_decoder::computed_plan& c : scan.decoder.computed) {
        for (uint32_t k = 0; k < c.program->input_count; ++k) {
            if (std::find(inputs.begin(), inputs.end(), c.inputs[k]) ==
                inputs.end()) {
                inputs.push_back(c.inputs[k]);
            }
        }
    }
    uint64_t rescan = lines.offset();
    uint64_t line_no = 1;
    uint64_t prev_us = 0;
    uint64_t next_progress = kProgressBytes;
    ecuio_frames_csv::row r;
    std::string value;
    while (lines.next(line, &ended, &too_long)) {
        ++line_no;
        if (!ended) {
            ++info.rows_torn; /* section 7: a write cut short */
            break;
        }
        const char* why = nullptr;
        if (too_long) {
            why = "the row is longer than a recording's rows can be";
        } else if (!ecuio_frames_csv::parse_row(line.c_str(), &r, &scan.row(),
                                                &why)) {
            /* why is set */
        } else if (r.host_us < prev_us) {
            why = "host_us is earlier than the row before it";
        } else if (r.host_us / kSecondUs > UINT32_MAX) {
            why = "host_us is past what a recording spans";
        }
        if (why != nullptr) {
            ecuio_detail_set("frames.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            return false;
        }
        prev_us = r.host_us;
        const auto second = static_cast<uint32_t>(r.host_us / kSecondUs);
        const uint64_t at = lines.line_start();
        if (index.empty() || index.back().second != second) {
            index.push_back(index_entry{at, second, 0});
            marks.push_back(mark(scan, rescan));
        }
        end_offset = lines.offset();
        const ecuio_frame_t* msg = scan.take(r.host_us);
        const uint32_t n = msg != nullptr ? scan.decoder.decode(msg) : 0;
        for (uint32_t k = 0; k < n; ++k) {
            const ecuio_value_t& v = scan.decoder.values[k];
            signal_entry& s = signals[def_of_id.at(v.signal_id)];
            double x = 0.0;
            if (!s.text()) {
                value.clear();
                rows::append_value(value, v.value);
                if (!rows::parse_number(value.data(), value.size(), &x) ||
                    !std::isfinite(x)) {
                    continue; /* not finite: no number to plot */
                }
            }
            ecuio_recording_signal_t& pub = s.pub;
            ++pub.samples;
            if (pub.first_us == ECUIO_RECORDING_NONE) {
                pub.first_us = r.host_us;
            }
            pub.last_us = r.host_us;
            if (s.text()) {
                if (s.texts.empty() || s.texts.back().second != second) {
                    /* no row to read back: text_at decodes its second */
                    s.texts.push_back(
                        text_second{second, 1, ECUIO_RECORDING_NONE});
                } else {
                    ++s.texts.back().count;
                }
                continue;
            }
            if (s.numbers.empty() || s.numbers.back().second != second) {
                s.numbers.push_back(number_second{second, 0, NAN, NAN, x, 0.0});
            }
            number_second& ns = s.numbers.back();
            ++ns.count;
            ns.last = x;
            ns.sum += x;
            merge(ns.lowest, ns.highest, x);
            merge(s.lowest, s.highest, x);
        }
        if (scan.idle()) {
            rescan = lines.offset();
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
            const uint32_t signal =
                it != def_of_id.end() ? def_signal[it->second] : UINT32_MAX;
            if (signal == UINT32_MAX) {
                return ECUIO_ERR_RECORDING_CHANGED; /* none at the open */
            }
            value.clear();
            if (signals[signal].text()) {
                value.assign(v.text, v.text_len);
            } else {
                rows::append_value(value, v.value);
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
                /* a frames open dropped a number that isn't finite too */
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
        if (!r->read_events(folder) ||
            !r->read_signals(progress, user_data, &canceled) ||
            !r->read_markers(folder)) {
            return canceled ? ECUIO_ERR_CANCELED : ECUIO_ERR_LOAD_FAILED;
        }
        r->read_signal_file(folder);
        r->finish();
        *out = r.release();
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_recording_open_frames(const char* folder_utf8,
                                           const ecuio_signal_set_t* signal_set,
                                           ecuio_open_progress_t progress,
                                           void* user_data,
                                           ecuio_recording_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (folder_utf8 == nullptr || signal_set == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_detail_clear();
    if (ecuio_validate_signal_set(signal_set, nullptr) != ECUIO_VALIDATE_OK) {
        return ECUIO_ERR_INVALID_ARGUMENT;
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
        r->take_set(*signal_set);
        bool canceled = false;
        if (!r->read_events(folder) ||
            !r->read_frames(progress, user_data, &canceled) ||
            !r->read_markers(folder)) {
            return canceled ? ECUIO_ERR_CANCELED : ECUIO_ERR_LOAD_FAILED;
        }
        r->read_signal_file(folder);
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

/* The first sample of a number signal from from_us on that reaches level
 * (rising: at or above it; falling: at or below) after a sample past
 * hysteresis from it (rising: below level - hysteresis) armed it */
ecuio_status_t find_crossing(const ecuio_recording_t* r, uint32_t signal,
                             bool rising, double level, double hysteresis,
                             uint64_t from_us, uint64_t* at_us)
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
    if (what->kind == ECUIO_RECORDING_FIND_ENGINE_START) {
        /* A NaN is refused too: it equals nothing */
        if (what->signal != 0 || what->direction != 0 || what->level != 0.0 ||
            what->hysteresis != 0.0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        const uint32_t rpm = engine_speed_signal(r);
        if (rpm == UINT32_MAX) {
            return ECUIO_OK;
        }
        return find_crossing(r, rpm, true, ECUIO_ENGINE_START_RPM,
                             ECUIO_ENGINE_START_RPM - ECUIO_ENGINE_STOPPED_RPM,
                             from_us, at_us);
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
    return find_crossing(r, what->signal,
                         what->direction == ECUIO_RECORDING_FIND_RISING,
                         what->level, what->hysteresis, from_us, at_us);
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
                      uint64_t from_us, uint64_t to_us,
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
    if (r == nullptr || rules == nullptr || count == nullptr ||
        (out == nullptr && capacity != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const uint32_t known = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    if (rules->kinds == 0 || (rules->kinds & ~known) != 0 ||
        rules->flags != 0 || from_us >= to_us) {
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
    try {
        ecuio_aids::samples s[kRoles];
        const ecuio_status_t rc = gather(r, signal, from_us, to_us, s);
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
            ecuio_aids::find_pulls(s[gate], s[measure], has_speed, engine_speed,
                                   pull, flags, found);
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
