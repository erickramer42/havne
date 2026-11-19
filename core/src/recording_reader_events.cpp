// File/layer: core/src/recording_reader_events.cpp | C++17, Qt-free;
//             events.log's pass (ADR-0049 section 5, ADR-0063 section 8)
// Testing: UNIT: RecordingTest, ConditionRecordingTest, RecordingKeysTest
//          (test_core)

#include "recording_reader.h"
#include "error_detail.h"
#include "recording_keys.h"
#include "sha256.h"

#include <algorithm>

namespace keys = ecuio_recording_keys;
using ecuio_frames_csv::file_size;

using namespace ecuio_recording_reader;

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

namespace {

/* A copy beside the recording (signals.json, conditions.json), whether or
 * not the header names it: its length, and with the header's hash whether
 * its bytes are those. Read in pieces, so a large file costs no memory;
 * one that can't be read counts as absent, and one past most bytes (its
 * loader refuses it) is present and never read. */
void read_copy(const std::string& path, uint64_t most, const char* sha256,
               uint32_t* flags, uint64_t* length)
{
    file_ptr f = open_file(path);
    if (!f) {
        return;
    }
    const uint64_t size = file_size(f.get());
    if (size > most) {
        *flags |= ECUIO_RECORDING_SIGNALS_PRESENT;
        *length = size;
        return;
    }
    ecuio_sha256::hasher h;
    char buf[16384];
    uint64_t total = 0;
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f.get())) > 0) {
        h.add(buf, got);
        total += got;
    }
    if (std::ferror(f.get()) != 0) {
        return;
    }
    *flags |= ECUIO_RECORDING_SIGNALS_PRESENT;
    *length = total;
    uint8_t digest[32];
    h.finish(digest);
    if ((*flags & ECUIO_RECORDING_SIGNALS_NAMED) != 0 &&
        ecuio_sha256::hex(digest) == sha256) {
        *flags |= ECUIO_RECORDING_SIGNALS_MATCH;
    }
}

} // namespace

void ecuio_recording::read_copies(const std::string& folder)
{
    read_copy(folder + "/" + rows::kSignalFileName, ECUIO_SIGNAL_SET_MAX_BYTES,
              signal_file.sha256, &signal_file.flags, &signal_file.length);
    read_copy(folder + "/" + rows::kConditionsName,
              ECUIO_CONDITION_FILE_MAX_BYTES, conditions.sha256,
              &conditions.flags, &conditions.length);
}

/* A cond line counted, and kept up to the cap; false for a key past a
 * set's most, which no writer makes */
bool ecuio_recording::take_condition(const rows::cond_line& c)
{
    uint32_t key = condition_key_count;
    if (!condition_events.empty() &&
        std::strcmp(condition_keys[condition_events.back().key], c.key) == 0) {
        key = condition_events.back().key; /* a flapping condition's */
    } else {
        for (uint32_t k = 0; k < condition_key_count; ++k) {
            if (std::strcmp(condition_keys[k], c.key) == 0) {
                key = k;
                break;
            }
        }
    }
    if (key == condition_key_count) {
        if (condition_key_count == ECUIO_CONDITION_MAX_CONDITIONS) {
            return false;
        }
        std::memcpy(condition_keys[key], c.key, std::strlen(c.key) + 1);
        ++condition_key_count;
    }
    if (++info.condition_events > ECUIO_RECORDING_MAX_CONDITION_EVENTS) {
        info.flags |= ECUIO_RECORDING_INFO_CONDITIONS_CUT;
        return true;
    }
    condition_events.push_back(condition_entry{
        c.host_us, c.t_us, c.value, c.term, key,
        static_cast<uint8_t>(c.on ? ECUIO_CONDITION_ON : ECUIO_CONDITION_OFF),
        static_cast<uint8_t>(c.flags)});
    return true;
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
    rows::cond_line cond;
    /* A format 3 activity line of kind cond, by its time and kind alone */
    const auto cond_kind = [this, &in_header](const std::string& l) {
        rows::event_line e;
        const char* text = nullptr;
        size_t text_len = 0;
        size_t kind_len = 0;
        return keys::has_enums(info.format) && !in_header &&
               rows::parse_event_line(l.data(), l.size(), &e) &&
               rows::split_kind(e.rest, e.rest_len, &text, &text_len,
                                &kind_len) &&
               rows::equals(e.rest, kind_len, "cond");
    };
    const auto skip_condition = [this] {
        if (conditions.lines_skipped < UINT32_MAX) {
            ++conditions.lines_skipped;
        }
    };
    uint64_t data_bytes = 0;
    if (open_progress != nullptr) {
        file_ptr data = open_file(source_path);
        data_bytes = data ? file_size(data.get()) : 0;
    }
    uint64_t next_progress = kProgressBytes;
    while (lines.next(line, &ended, &too_long)) {
        if (open_progress != nullptr && lines.offset() >= next_progress) {
            next_progress = lines.offset() + kProgressBytes;
            if (open_progress(0, data_bytes, open_user) != 0) {
                open_canceled = true;
                return false;
            }
        }
        ++line_no;
        if (!ended || too_long) {
            /* A torn last line, or one no writer makes: a cond line's is
             * counted as one that doesn't parse */
            if (line_no > 1 && cond_kind(line)) {
                skip_condition();
            }
            continue;
        }
        const char* p = line.data();
        size_t n = line.size();
        if (line_no == 1) {
            const size_t bom = rows::bom_length(p, n);
            p += bom;
            n -= bom;
            const size_t m = std::strlen(rows::kFormat1End);
            if (keys::format_of(p, n) != ECUIO_RECORDING_FORMAT_UNKNOWN) {
                info.format = keys::format_of(p, n);
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
            if (!keys::has_header(info.format)) {
                continue;
            }
            const char* why = nullptr;
            if (from_frames && (rows::starts_with(p, n, "label  ") ||
                                rows::starts_with(p, n, "signal  "))) {
                /* the caller's set names the signals */
            } else if (keys::has_enums(info.format) &&
                       rows::starts_with(p, n, "label  ")) {
                if (!read_label(p + 7, n - 7, &why)) {
                    ecuio_detail_set("events.log line %llu: %s",
                                     static_cast<unsigned long long>(line_no),
                                     why);
                    return false;
                }
            } else if (rows::starts_with(p, n, "signal  ")) {
                std::string id; /* what its rows are under */
                bool ok = keys::parse_signal_line(info.format, p + 8, n - 8,
                                                  &id, hs, &why);
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
                if (by_name.find(id) == by_name.end()) {
                    signal_entry& s = signals[add_signal(id, hs.name)];
                    std::memcpy(s.pub.unit, hs.unit.data(), hs.unit.size());
                    s.known_kind = true;
                    s.pub.flags = hs.text          ? ECUIO_RECORDING_SIGNAL_TEXT
                                  : hs.enumeration ? ECUIO_RECORDING_SIGNAL_ENUM
                                                   : 0;
                    s.pub.decimal_places = hs.decimal_places;
                    s.pub.plot_rank = hs.plot_rank;
                    s.pub.group = hs.group;
                }
            } else if (keys::has_enums(info.format) &&
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
            } else if (keys::has_enums(info.format) &&
                       rows::starts_with(p, n, rows::kConditionsKey)) {
                /* Evidence beside the data: a line that isn't the copy's
                 * names none, and the open goes on */
                const size_t key = std::strlen(rows::kConditionsKey);
                std::string hex;
                if (rows::parse_named_file(p + key, n - key,
                                           rows::kConditionsName, &hex)) {
                    std::memcpy(conditions.sha256, hex.data(), hex.size());
                    conditions.sha256[hex.size()] = '\0';
                    conditions.flags |= ECUIO_RECORDING_CONDITIONS_NAMED;
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
        } else if (rows::equals(e.rest, kind_len, "drop")) {
            if (rows::parse_drop(text, text_len, &count)) {
                drop_frames += count;
            } else if (keys::has_enums(info.format) &&
                       rows::parse_lost(text, text_len, &count)) {
                conditions.events_lost +=
                    (std::min)(count, UINT64_MAX - conditions.events_lost);
            }
        } else if (keys::has_enums(info.format) &&
                   rows::equals(e.rest, kind_len, "cond")) {
            if (!(rows::parse_cond_line(text, text_len, &cond) &&
                  take_condition(cond))) {
                skip_condition();
            }
        }
    }
    if (summary) {
        info.flags &= ~ECUIO_RECORDING_INFO_NO_SUMMARY;
    } else {
        info.records_dropped = drop_frames;
    }
    return true;
}
