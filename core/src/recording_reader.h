// File/layer: core/src/recording_reader.h | internal, C++17, Qt-free; the
//             reader's types (ADR-0049 section 5)
// Testing: UNIT: RecordingTest, RecordingFramesTest, RecordingStatsTest,
//          RecordingStretchesTest, ConditionRecordingTest (test_core)
//
// What the reader's sources share: the per-second structs and their
// bound's figures, the line reader, the frames pass's scan and mark, and
// struct ecuio_recording, whose members the reader's sources define.
// Everything but the struct is in namespace ecuio_recording_reader.

#ifndef ECUIO_RECORDING_READER_H
#define ECUIO_RECORDING_READER_H

#include "frame_decoder.h"
#include "frames_csv.h" /* native_path, file_size, seek_to, parse_row */
#include "isotp.h"
#include "recording_check.h" /* iso15765_id */
#include "recording_rows.h"

#include "ecuio/conditions.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ecuio_recording_reader {

namespace rows = ecuio_recording_rows;

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

inline void merge(double& lowest, double& highest, double v)
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

/* A cond line, its key by its place in the key table */
struct condition_entry {
    uint64_t host_us;
    uint64_t t_us;
    double value;
    uint32_t term;
    uint32_t key;
    uint8_t state;
    uint8_t flags;
};
static_assert(sizeof(condition_entry) == 40, "the cap's 40 bytes");
static_assert(ecuio_recording_rows::kCondStale == ECUIO_CONDITION_STALE &&
                  ecuio_recording_rows::kCondFired == ECUIO_CONDITION_FIRED &&
                  ecuio_recording_rows::kCondHeld == ECUIO_CONDITION_HELD,
              "a cond line's flag bits are the event's");
static_assert(ecuio_recording_rows::kConditionKeyMax == ECUIO_CONDITION_KEY_MAX,
              "a cond line's key is a condition's");
static_assert(
    ECUIO_RECORDING_CONDITIONS_NAMED == ECUIO_RECORDING_SIGNALS_NAMED &&
        ECUIO_RECORDING_CONDITIONS_PRESENT == ECUIO_RECORDING_SIGNALS_PRESENT &&
        ECUIO_RECORDING_CONDITIONS_MATCH == ECUIO_RECORDING_SIGNALS_MATCH,
    "a copy's flags are read alike");

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
                const size_t room = rows::kRowMax + 1 - line.size();
                *too_long = take > room;
                line.append(s, *too_long ? room : take);
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

inline file_ptr open_file(const std::string& utf8)
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

} // namespace ecuio_recording_reader

/* The names the struct's members use */
using ecuio_recording_reader::condition_entry;
using ecuio_recording_reader::file_ptr;
using ecuio_recording_reader::frame_mark;
using ecuio_recording_reader::frame_scan;
using ecuio_recording_reader::index_entry;
using ecuio_recording_reader::kCacheBytes;
using ecuio_recording_reader::marker_entry;
using ecuio_recording_reader::raw_row;
using ecuio_recording_reader::saved_latest;
using ecuio_recording_reader::set_copy;
using ecuio_recording_reader::signal_entry;

struct ecuio_recording {
    /* signals.csv, or frames.csv for a recording opened through its frames */
    std::string source_path;
    uint64_t end_offset = 0; /* past the source's last row the open read */
    ecuio_recording_info_t info{};
    ecuio_recording_signals_t signal_file{};
    ecuio_recording_conditions_t conditions{};
    std::vector<signal_entry> signals;
    /* By what signals.csv's signal cell holds: the key in format 4, the
     * name before */
    std::unordered_map<std::string, uint32_t> by_name;
    std::vector<index_entry> index;
    std::vector<marker_entry> markers;
    /* The cond lines kept, and their keys, each once: a set's most, so
     * a lookup is a short scan that allocates nothing */
    std::vector<condition_entry> condition_events;
    char condition_keys[ECUIO_CONDITION_MAX_CONDITIONS]
                       [ECUIO_CONDITION_KEY_MAX + 1] = {};
    uint32_t condition_key_count = 0;

    /* Opened through its frames: the set they decode with, each def's
     * signal (UINT32_MAX for one that never decoded), each def by its id,
     * a mark per index entry, the packed replies' lengths at the marks
     * where they changed, the number defs the computed signals read and
     * their newest values at each mark */
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
    /* The open's progress, asked by the events pass too: at the data
     * file's start, every kProgressBytes of events.log */
    ecuio_open_progress_t open_progress = nullptr;
    void* open_user = nullptr;
    bool open_canceled = false;

    uint32_t add_signal(const std::string& id, const std::string& name);
    bool read_label(const char* text, size_t n, const char** why);
    bool read_events(const std::string& folder);
    bool take_condition(const ecuio_recording_rows::cond_line& c);
    void read_copies(const std::string& folder);
    bool read_signals(ecuio_open_progress_t progress, void* user,
                      bool* canceled);
    void take_set(const ecuio_signal_set_t& set,
                  const std::vector<ecuio_signal_key_t>& set_keys);
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

#endif /* ECUIO_RECORDING_READER_H */
