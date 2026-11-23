// File/layer: core/src/export_engine.h | internal, C++17, Qt-free;
//             the recording export's engine class
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)
//
// One class holds an export's state across its passes; its members are
// defined in export.cpp (the run, the entry points), export_pcapng.cpp
// (the plan and the pcapng write), export_csv.cpp, export_wide.cpp,
// export_events.cpp and export_folder.cpp (the folder copy).

#ifndef ECUIO_EXPORT_ENGINE_H
#define ECUIO_EXPORT_ENGINE_H

#include "error_detail.h"
#include "folder_files.h"
#include "frame_reassembly.h"
#include "frame_rows.h"
#include "frames_csv.h"
#include "isotp.h"
#include "recording_rows.h"
#include "recording_set.h"
#include "request_timing.h" /* timing_hist: a column's usual interval */
#include "scrub.h"

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ecuio_export_detail {

namespace csv = ecuio_frames_csv;
namespace files = ecuio_files;
namespace rows = ecuio_recording_rows;
namespace scrub = ecuio_scrub;
namespace rs = ecuio_recording_set;

constexpr size_t kWriteChunk = 1u << 16;
constexpr uint32_t kIdentification = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION;
constexpr uint32_t kUnattributable = ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE;
constexpr uint32_t kTextValues = ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES;
constexpr uint32_t kVinShaped = ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED;
constexpr uint32_t kVinMasked = ECUIO_EXPORT_LEFT_OUT_VIN_MASKED;
/* A folder copy's note on its masks, beside the files it explains */
constexpr const char* kMaskNoteName = "masks.txt";
constexpr const char* kUtf8Bom = "\xEF\xBB\xBF"; /* the user CSVs start with one */
/* The wide CSV's silence (ADR-0056 section 4): a column with no sample
 * for this long, or 3x its usual interval if longer, leaves the rule */
/* The most columns a wide CSV has: past it no spreadsheet opens the
 * file, and every sample costs a step per column */
constexpr size_t kWideColumnsMax = 16384;
/* Rows read between two asks of the caller's cancel */
constexpr uint32_t kCancelRows = 1024;
constexpr uint64_t kSilenceUs = 1000000;
constexpr uint64_t kSilenceTimes = 3;

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};
using file_ptr = std::unique_ptr<FILE, file_closer>;

/* Sets the error detail from a printf-style format; false, so a refusal
 * reads "return fail(...)" */
bool fail(const char* fmt, const std::string& a = std::string());

using ecuio_frame_rows::frame_rows;

namespace fr = ecuio_frame_reassembly;
using fr::any_id_messages;
using fr::joined;
using fr::message_end;
using fr::other_id_identification;
using fr::reassembly;
using fr::seg;

/* A copy's JSON file (signals.json, conditions.json) as a scrubbed copy
 * writes it, masked whole in memory so events.log can name its hash */
struct masked_copy {
    bool present = false;
    uint64_t masks = 0;
    std::string bytes;
    std::string source_sha; /* the recording's file's */
    std::string sha;        /* the copy's */
};

struct wide_column {
    std::string name;     /* as signals.csv names it: a key, or a name */
    std::string shown;    /* its name for the head cell */
    std::string unit;     /* the header's, else the first row's */
    std::string head;     /* "Name (unit)" in the export's units, a field */
    uint8_t decimals = 0; /* the header's; else the most its values show */
    bool in_header = false;
    uint64_t samples = 0;
    uint64_t last_us = 0;
    timing_hist deltas; /* between its samples */
    uint64_t usual_us = 0; /* the median of deltas; 0 with none */
    uint64_t silence_us = kSilenceUs;
    bool sparse = false; /* far sparser than the rest: paces no sweep */
};

/* One row interval's rows over the columns, fed the samples in time
 * order: the sweep rule (row_ms 0) or a grid; sink takes each row's
 * text when set, bytes counts it either way */
struct wide_rows {
    uint32_t row_ms = 0;
    uint64_t next_grid_us = 0;
    std::vector<std::string> cell; /* the newest value; "" none */
    std::vector<uint8_t> fresh;    /* a sample since the last row */
    std::vector<uint8_t> in_rule;
    std::vector<uint64_t> last_us;
    uint64_t rows = 0;
    uint64_t cells = 0;
    uint64_t bytes = 0;
    std::string* sink = nullptr;
    std::string line;

    /* A column with samples in range starts in the rule; one with none
     * (a named pick never answered) starts outside it and holds no
     * row up */
    void start(const std::vector<wide_column>& cols, uint64_t t0)
    {
        const size_t n = cols.size();
        cell.assign(n, std::string());
        fresh.assign(n, 0);
        in_rule.assign(n, 0);
        for (size_t k = 0; k < n; ++k) {
            in_rule[k] = cols[k].samples > 0 ? 1 : 0;
        }
        last_us.assign(n, t0);
        if (row_ms > 0) {
            const uint64_t step = uint64_t{row_ms} * 1000;
            next_grid_us = (t0 / step) * step;
        }
    }

    /* At time t: a column quiet past its silence leaves the rule,
     * and emit writes no cell for it until its next sample */
    void silence(uint64_t t, const std::vector<wide_column>& cols)
    {
        for (size_t k = 0; k < cols.size(); ++k) {
            if (in_rule[k] != 0 && t > last_us[k] &&
                t - last_us[k] > cols[k].silence_us) {
                in_rule[k] = 0;
                fresh[k] = 0;
            }
        }
    }

    void emit(uint64_t ms)
    {
        line = std::to_string(ms);
        for (size_t k = 0; k < cell.size(); ++k) {
            line += ',';
            if (in_rule[k] != 0 && !cell[k].empty()) {
                line += cell[k];
                ++cells;
            }
            fresh[k] = 0;
        }
        line += "\r\n";
        bytes += line.size();
        ++rows;
        if (sink != nullptr) {
            *sink += line;
        }
    }

    /* The grid rows due before t (or at it, when inclusive), each with
     * the newest values, skipped with no new cell. Nothing is new
     * until the next sample, so steps are skipped in one move (a gap
     * of hours at --rows 1 costs no loop); silence is judged at the
     * step that writes, at or after every step skipped */
    void grid_to(uint64_t t, bool inclusive,
                 const std::vector<wide_column>& cols)
    {
        const uint64_t step = uint64_t{row_ms} * 1000;
        while (inclusive ? next_grid_us <= t : next_grid_us < t) {
            silence(next_grid_us, cols);
            bool any = false;
            for (uint8_t f : fresh) {
                any = any || f != 0;
            }
            if (any) {
                emit(next_grid_us / 1000);
                next_grid_us += step;
                continue;
            }
            const uint64_t last =
                inclusive ? (t / step) * step : ((t - 1) / step) * step;
            next_grid_us = last > next_grid_us ? last : next_grid_us + step;
        }
    }

    void sample(size_t k, uint64_t t, const std::string& text,
                const std::vector<wide_column>& cols)
    {
        if (row_ms > 0) {
            grid_to(t, false, cols);
        }
        silence(t, cols);
        cell[k] = text;
        fresh[k] = 1;
        in_rule[k] = 1;
        last_us[k] = t;
        if (row_ms > 0) {
            return;
        }
        /* A row once every column in the rule has a sample newer
         * than the last row, at the completing sample's time; a
         * sparse column's cell rides along */
        for (size_t j = 0; j < cols.size(); ++j) {
            if (in_rule[j] != 0 && !cols[j].sparse && fresh[j] == 0) {
                return;
            }
        }
        emit((t + 500) / 1000);
    }

    /* The grid's last row: the step at or after the last sample, so
     * the newest values are in */
    void finish(uint64_t last_t, const std::vector<wide_column>& cols)
    {
        if (row_ms > 0) {
            const uint64_t step = uint64_t{row_ms} * 1000;
            grid_to(((last_t + step - 1) / step) * step, true, cols);
        }
    }
};

/* An export's passes: the plan over frames.csv, then the format's */
class recording_export
{
public:
    recording_export(const ecuio_export_options_t& o, ecuio_export_report_t& r)
        : o_(o), r_(r), raw_((o.flags & ECUIO_EXPORT_RAW) != 0),
          folder_(o.format == ECUIO_EXPORT_FOLDER),
          wide_(o.format == ECUIO_EXPORT_CSV && o.layout == ECUIO_CSV_WIDE),
          made_(new ecuio_frame_t{}), probe_(new ecuio_frame_t{}),
          tx_(new isotp_tx)
    {}

    /* The status a failed run or estimate ends in: LOAD_FAILED, or
     * INVALID_ARGUMENT for a column the recording can't give */
    ecuio_status_t status() const { return status_; }

    void set_cancel(ecuio_cancel_t cancel, void* user_data)
    {
        cancel_ = cancel;
        cancel_user_ = user_data;
    }

    /* *partial names the file made under a name of its own (pcapng, CSV)
     * while it isn't in place, so the caller removes it on any failure */
    bool run(const std::string& folder, const std::string& out,
             std::string* partial);

    /* The wide CSV's size for each interval, nothing written
     * (ecuio_export_estimate) */
    bool estimate(const std::string& folder, const uint32_t* row_ms,
                  uint32_t count, ecuio_export_estimate_t* out);
private:
    /* The caller's cancel: asked now, or once kCancelRows rows have gone
     * by; true when it stops the export */
    bool canceled(bool now = false)
    {
        if (cancel_ == nullptr || (!now && ++unasked_ < kCancelRows)) {
            return false;
        }
        unasked_ = 0;
        if (cancel_(cancel_user_) == 0) {
            return false;
        }
        status_ = ECUIO_ERR_CANCELED;
        ecuio_detail_set("the export was canceled");
        return true;
    }

    bool in_range(uint64_t host_us) const
    {
        return host_us >= o_.from_us && (o_.to_us == 0 || host_us <= o_.to_us);
    }

    /* A refusal that is the caller's, not the recording's */
    bool refuse(const char* fmt, const std::string& a = std::string())
    {
        status_ = ECUIO_ERR_INVALID_ARGUMENT;
        return fail(fmt, a);
    }

    /* events.log's header: the utc line, the recording's start (none
     * gives 0), its profile and URI, and each signal's declared kind */
    bool read_header(const std::string& folder);

    /* The plan's pass over frames.csv: always for the pcapng; for the
     * CSV only scrubbed, for the identity values the check searches for,
     * and only when the recording holds frames */
    bool plan_frames(const std::string& folder, bool csv_out) const;

    /* A def keyed as an identification reply (49 xx, 62 F180-F1FF) at the
     * message's start, judged by the classifier the frames go through */
    static bool identification_def(const ecuio_signal_def_t& d);

    /* The recording's text and hex defs (ADR-0054 section 1): its own
     * signals.json, else uds-generic's set from its URI's did= list, else
     * its profile by name. None found leaves the value search alone to
     * catch a text value. A signals.json that doesn't load refuses the
     * export: the set it names may have text defs no other set has. */
    bool text_defs(const std::string& folder);

    /* A message a text or hex def applies to */
    bool text_message(uint32_t id, uint32_t flags, const uint8_t* data,
                      size_t n);

    /* Each text value signals.csv holds: what a number can't be */
    bool read_text_values(const std::string& folder);

    void left_out(const seg& s, uint32_t cls, bool counted);

    bool add_identity(const uint8_t* msg, size_t n);

    /* The first pass: each frame's fate, the counts, the identity values */
    bool plan(const std::string& folder);

    /* Messages reassembled from the wire: an identification reply's
     * frames left out, a text or hex def's, a torn one's on a reply ID */
    bool judge(std::vector<message_end>& ends);

    bool is_dropped(uint64_t index) const
    {
        return std::binary_search(dropped_.begin(), dropped_.end(), index);
    }

    /* The second check, over a message before any segmenting */
    bool check_message(const uint8_t* p, size_t n)
    {
        if (raw_) {
            return true;
        }
        const uint32_t found = scrub::check_bytes(values_, p, n);
        if (found != 0) {
            r_.failed_class = found;
            return fail("the check found %s in a message the export would "
                        "write: nothing was written",
                        scrub::failed_words(found));
        }
        return true;
    }

    /* The file's check: scrub::kMask for text a user wrote (names,
     * labels, markers), kRefuse for bytes core writes */
    void start_check(uint32_t mode)
    {
        file_check_.reset(new scrub::check(values_, mode));
        masking_ = (mode & scrub::kMask) != 0;
        fed_ = 0;
    }

    /* buf_ to the file, checked: masking, the last kMaskReach bytes wait
     * for the next chunk, so a mask the next finds can reach them; all
     * ends the file */
    bool flush(bool all)
    {
        if ((buf_.empty() && !all) || (!all && buf_.size() < kWriteChunk)) {
            return true;
        }
        size_t keep = 0;
        if (!raw_ && masking_) {
            auto* b = reinterpret_cast<uint8_t*>(&buf_[0]);
            file_check_->feed_masking(b + fed_, buf_.size() - fed_, fed_);
            if (all) {
                (void)file_check_->finish_masking(b + buf_.size(), buf_.size());
                r_.left_out[kVinMasked] += file_check_->masked();
            } else {
                /* a guard: each masking writer flushes at a line's end */
                keep = (std::min)(scrub::kMaskReach, buf_.size());
            }
            file_check_->written(b, buf_.size() - keep);
        } else if (!raw_) {
            file_check_->feed(reinterpret_cast<const uint8_t*>(buf_.data()),
                              buf_.size());
        }
        const size_t n = buf_.size() - keep;
        if (std::fwrite(buf_.data(), 1, n, file_.get()) != n) {
            return fail("the export can't be written: %s",
                        files::errno_words(errno));
        }
        buf_.erase(0, n);
        fed_ = keep;
        return true;
    }

    void put(const ecuio_frame_t& f, uint64_t host_us,
             const std::string* comment);

    void put_can(const isotp_can& c, uint32_t flags, uint64_t host_us,
                 const std::string& comment);

    /* A message an adapter delivered whole, as its sender sent it */
    void re_segment(const ecuio_frame_t& f, uint64_t host_us);

    std::string section_comment() const;

    /* The second pass */
    bool write(const std::string& folder);

    /* The whole messages a written frame carries, each checked once */
    bool check_ends(const std::vector<message_end>& ends)
    {
        for (const message_end& e : ends) {
            if (!e.whole || e.frames.empty() || is_dropped(e.frames[0].index)) {
                continue;
            }
            bool written = false;
            for (const seg& s : e.frames) {
                written = written || in_range(s.host_us);
            }
            if (written && !check_message(e.data.data(), e.data.size())) {
                return false;
            }
        }
        return true;
    }

    /* The messages any ISO-TP-shaped frames join (any_id_messages) that
     * a written frame carries, each checked whole: frame f's (f nullptr:
     * those still open at the end) */
    bool check_joined(any_id_messages& any, const ecuio_frame_t* f,
                      bool written)
    {
        if (raw_ || (f != nullptr && !fr::tp_readable(*f))) {
            return true;
        }
        joins_.clear();
        if (f != nullptr) {
            any.take(*f, written, joins_);
        } else {
            any.finish(joins_);
        }
        for (const joined& j : joins_) {
            if (j.written && !check_message(j.data.data(), j.data.size())) {
                return false;
            }
        }
        return true;
    }

    /* A spreadsheet runs a cell starting with one of these as a formula
     * (ADR-0049 section 8). A number is exempt: one parse_number takes
     * whole, such as -12.5, is read as a number. */
    static bool formula_start(const std::string& s);

    /* signals.csv in host_us order, each value a scrubbed export keeps:
     * counted (write false: the values, the signals, the times written)
     * or written. The same decisions both times. */
    bool values(const std::string& folder, bool write);

    /* A name, a key, a unit or a raw text value that a spreadsheet would
     * run */
    bool check_formula(const rows::signal_row& row, bool number) const;

    /* One CSV field, RFC 4180's quotes only where it holds a comma, a
     * quote or a line end */
    void put_field(const std::string& s);

    static std::string seconds(uint64_t us);

    void put_row(const rows::signal_row& row);

    std::string csv_comment() const;

    /* The CSV's second pass */
    bool write_csv(const std::string& folder);

    // ---- The wide CSV (ADR-0056 sections 4 and 5) ----

    /* The refusal for a wide CSV with more columns than kWideColumnsMax,
     * given as soon as one past it is met */
    static bool too_many_columns();

    /* A column by name, in order of arrival; its header line's unit
     * and decimals when the header has it */
    size_t add_column(const std::string& name);

    /* A column's name and unit, as check_formula judges a row's */
    bool check_column(const wide_column& c);

    static std::string csv_field(const std::string& s);

    /* The first pass: the columns (named, the header's focus, or every
     * number signal met) with their samples, interval, unit and decimals;
     * the values left out counted */
    bool wide_survey(const std::string& folder);

    /* A column is sparse, pacing no sweep, when its interval is over
     * rows::kSparseTimes the lower median of the others' intervals. One
     * sample in range gives no interval: its stand-in is the span of the
     * values exported, which held no other, and it isn't among the others.
     * With no other interval to compare, none is sparse */
    void mark_sparse();

    std::string wide_head() const;

    /* A cell: the value in the export's units, to the places shown */
    std::string cell_text(const wide_column& c, double v) const;

    /* signals.csv's number rows in range, by column: the survey counts
     * and classifies, the second pass feeds each interval's rows */
    bool wide_walk(const std::string& folder, bool survey,
                   std::vector<wide_rows>* engines);

    /* The wide CSV's second pass, written and checked as the long's */
    bool write_wide(const std::string& folder);

    // ---- The folder export ----

    /* A caller line's text a scrubbed copy keeps, rewritten; false to
     * leave it out: the front end's name and version, the surrogate's
     * frame logging, and the vehicle's label as <vehicle> */
    static bool kept_caller(const std::string& text, std::string* out);

    /* events.log, header line by line, then the activity lines by kind,
     * into events_head_ and events_body_; the lines left out counted */
    bool plan_events(const std::string& folder);

    void head_line(const std::string& line);

    void activity_line(const std::string& line);

    /* Where the range's lines start in events_body_: the held lines go
     * there */
    void range_starts();

    /* A format 3 cond line, in the range by its own host_us: a scrubbed
     * copy writes it again from its fields, so a word or field the export
     * doesn't know stays out, and leaves out one that doesn't parse. One
     * before the range sets the state its held lines start from. */
    void cond_activity(const std::string& line, const std::string& head,
                       const char* text, size_t text_len, uint64_t ms);

    /* Each key's last line before the range, up to a set's most keys (a
     * line naming another is one no writer makes, as the reader holds),
     * and the latest line's clock and times */
    void hold_before(const rows::cond_line& c, const std::string& line);

    /* A focus or pick line, in the range by its time. Before the range,
     * the last change's lines are held for the range's start ("more: "
     * lines go with the line before them, at its time). */
    void focus_activity(const std::string& line, const rows::event_line& e,
                        const char* text, size_t text_len);

    /* "HH:mm:ss.zzz" moved on by ms, through midnight; as it was when it
     * doesn't read */
    static std::string clock_after(const std::string& clock, uint64_t ms);

    /* A range that starts after a cond line opens with a held line for
     * each condition then ON (ADR-0063 section 8), at the range's start:
     * the state, stale and value the key's last line gave, the session
     * time and clock the latest line's carried on to the start (the clock
     * within a millisecond of the recorder's) */
    void put_held();

    /* A range that starts after a focus change opens with the focus then
     * in force: the last change's lines again, at the range's start, the
     * clock carried on from theirs (within a millisecond of the
     * recorder's) */
    void put_focus_held();

    /* markers.csv or latency.csv: the header and the rows in the range,
     * as they are */
    bool plan_rows(const std::string& folder, const char* name, bool markers,
                   std::vector<std::string>& out, bool* present);

    /* The files a folder holds that the export doesn't know */
    void count_unknown(const std::string& folder);

    std::string export_line() const;

    /* A file of the copy: written under partial, checked as it goes */
    bool begin_file(const std::string& partial, const char* name,
                    uint32_t mode);

    bool end_file(const char* name);

    bool put_lines(const std::vector<std::string>& lines);

    /* frames.csv's kept rows as recorded, each message a kept row
     * carries checked before segmenting, as the pcapng's second pass */
    bool write_frames(const std::string& folder);

    /* A file as it is, when the folder has it: a raw copy's JSON files,
     * a masked copy's note */
    bool copy_file(const std::string& folder, const char* name);

    /* signals.json or conditions.json, up to max bytes, masked whole
     * (scrub::kJson); past max it's left out. A masked file that no longer
     * loads where the recording's did refuses the copy. */
    bool mask_copy(const std::string& folder, const char* name, size_t max,
                   masked_copy* out);

    /* A masked copy as planned, under a check that refuses */
    bool write_copy(const char* name, const masked_copy& c);

    /* masks.txt: what the masks are, when the copy holds any or the
     * recording was a masked copy itself */
    bool write_note();

    bool write_folder(const std::string& folder);

    /* The folder export's passes: every count first, then each file */
    bool run_folder(const std::string& folder, const std::string& out);

    /* The partial folder and the files written into it */
    void remove_partial();

    const ecuio_export_options_t& o_;
    ecuio_export_report_t& r_;
    const bool raw_;
    const bool folder_;
    const bool wide_;
    scrub::values values_;
    std::vector<uint64_t> dropped_;
    file_ptr file_;
    std::string buf_;
    std::unique_ptr<scrub::check> file_check_;
    bool masking_ = false;
    size_t fed_ = 0; /* buf_'s bytes the check has, held for its masks */
    std::unique_ptr<ecuio_frame_t> made_;
    std::unique_ptr<ecuio_frame_t> probe_;
    std::unique_ptr<isotp_tx> tx_;
    rs::header header_;
    rs::set_of set_;
    std::vector<const ecuio_signal_def_t*> text_;
    /* Signals whose values a scrubbed CSV leaves out: text or hex by the
     * def or the header, and numbers keyed as identification replies */
    std::unordered_set<std::string> text_names_;
    std::unordered_set<std::string> ident_names_;
    uint64_t first_us_ = 0;
    uint64_t last_us_ = 0;
    /* The wide CSV's */
    std::vector<wide_column> columns_;
    std::unordered_map<std::string, size_t> index_;
    bool fixed_columns_ = false;
    uint64_t surveyed_ = 0;
    uint64_t seen_ = 0;
    ecuio_status_t status_ = ECUIO_ERR_LOAD_FAILED;
    ecuio_cancel_t cancel_ = nullptr;
    void* cancel_user_ = nullptr;
    uint32_t unasked_ = 0;
    uint32_t held_keys_ = 0; /* the folder export's, beside held_ */
    /* The folder export's */
    std::string partial_;
    std::vector<std::string> written_paths_;
    uint64_t frames_left_[ECUIO_EXPORT_LEFT_OUT_COUNT] = {};
    bool has_frames_ = false;
    bool has_values_ = false;
    bool has_events_ = false;
    bool format3_ = false; /* format 3 or later, which have cond lines */
    bool has_markers_ = false;
    bool has_latency_ = false;
    bool had_note_ = false; /* the recording's own masks.txt */
    masked_copy signals_copy_;
    masked_copy conditions_copy_;
    bool held_seen_ = false; /* a cond line before the range */
    std::vector<std::string> events_head_;
    std::vector<std::string> events_body_;
    std::vector<std::string> markers_;
    std::vector<std::string> latency_;
    uint64_t events_left_ = 0;
    /* A range's held lines: each key's last cond line before it */
    rows::cond_line held_[ECUIO_CONDITION_MAX_CONDITIONS];
    rows::cond_line held_last_;
    std::string held_clock_;
    /* A range's held lines: the last focus change before it, each line
     * from its kind word on, and that change's clock and time */
    std::vector<std::string> focus_held_;
    std::string focus_held_clock_;
    uint64_t focus_held_ms_ = 0;
    size_t held_at_ = SIZE_MAX;
    std::string made_up_;
    std::string made_up_flow_;
    std::vector<joined> joins_;
    uint64_t written_ = 0;
};

} // namespace ecuio_export_detail

#endif // ECUIO_EXPORT_ENGINE_H
