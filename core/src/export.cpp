// File/layer: core/src/export.cpp | C++17, Qt-free; a recording out to
//             another tool's format (ADR-0054 sections 1 and 3)
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest (test_core)
//
// ecuio_export: frames.csv read twice. The first pass classifies every
// frame (scrub.h), the frames of ISO-TP messages as they crossed the bus
// reassembled first with core's ISO-TP, as the reader's frames path does
// (ADR-0049 section 5): an identification reply, a message a text or hex
// def of the recording's set applies to (its signals.json, uds-generic's
// did= list from its URI, or its profile by name), a segment no message
// can be attributed to. It collects the recording's identity values and
// counts what the export leaves out. The second checks each message the
// export writes as it was before any segmenting, then writes it: a frame
// as it arrived, or a message an adapter delivered whole re-segmented
// into the frames a sender would have sent, each marked as made up. The
// written bytes are checked as they go out, and the file is renamed into
// place only when both checks pass.
//
// The CSV export (ADR-0054's appendix "the CSV export as built"): the
// same first pass for the identity values, then signals.csv read twice,
// to count and then to write each value a scrubbed export keeps, one row
// a sample (time_s,signal,unit,value), through the same file check.
//
// The folder export (ADR-0054's appendix "the folder export as built"): a
// scrubbed copy of the recording, each file it knows rewritten by the
// classes (frames.csv by the plan, kept rows as recorded; signals.csv by
// the CSV's classes; events.log line by line; markers.csv, latency.csv
// and signals.json as they are), each through its own file check, under
// a name of its own beside <out> (folder_files.h's partial_name) until
// every one passes.
//
// The caller's cancel (ecuio_export_with) is asked as each pass and each
// file of a copy begins, and every kCancelRows rows; a stop is a failure
// like any other, so the partial goes the same way.

#include "error_detail.h"
#include "folder_files.h"
#include "frame_reassembly.h"
#include "frame_rows.h"
#include "frames_csv.h"
#include "isotp.h"
#include "json.h" /* quoted */
#include "pcapng.h"
#include "recording_check.h" /* iso15765_id */
#include "recording_rows.h"
#include "recording_set.h"
#include "request_timing.h" /* timing_hist: a column's usual interval */
#include "scrub.h"

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace csv = ecuio_frames_csv;
namespace files = ecuio_files;
namespace rows = ecuio_recording_rows;
namespace scrub = ecuio_scrub;
namespace pc = ecuio_pcapng;
namespace rs = ecuio_recording_set;

constexpr size_t kWriteChunk = 1u << 16;
constexpr uint32_t kIdentification = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION;
constexpr uint32_t kUnattributable = ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE;
constexpr uint32_t kTextValues = ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES;
/* The wide CSV's silence (ADR-0056 section 4): a column with no sample
 * for this long, or 3x its usual interval if longer, leaves the rule */
/* The most columns a wide CSV has: past it no spreadsheet opens the
 * file, and every sample costs a step per column */
constexpr size_t kWideColumnsMax = 16384;
/* The most row intervals one ecuio_export_estimate call sizes: each one
 * keeps its own rows engine */
constexpr uint32_t kEstimateCountMax = 64;
/* Rows read between two asks of the caller's cancel */
constexpr uint32_t kCancelRows = 1024;
constexpr uint64_t kSilenceUs = 1000000;
constexpr uint64_t kSilenceTimes = 3;

/* A number to d decimal places (0 to 3) with a '.', whatever the
 * locale: rounded half away from zero, no "-0.0"; not finite, "" (no
 * cell). At 9e15 or more scaled, %.12g as signals.csv writes it, so a cell
 * is never empty for being large */
std::string fixed_text(double v, int d)
{
    static const double kPow10[] = {1.0, 10.0, 100.0, 1000.0};
    if (!std::isfinite(v) || d < 0 || d > 3) {
        return std::string();
    }
    const double scaled = std::round(v * kPow10[d]);
    if (std::fabs(scaled) >= 9.0e15) {
        std::string as_recorded;
        rows::append_value(as_recorded, v);
        return as_recorded;
    }
    const long long n = static_cast<long long>(scaled);
    const unsigned long long a = n < 0
                                     ? 0ull - static_cast<unsigned long long>(n)
                                     : static_cast<unsigned long long>(n);
    std::string digits = std::to_string(a);
    if (d > 0) {
        while (digits.size() < static_cast<size_t>(d) + 1) {
            digits.insert(digits.begin(), '0');
        }
        digits.insert(digits.size() - static_cast<size_t>(d), 1, '.');
    }
    return n < 0 ? "-" + digits : digits;
}

/* The digits after a recorded value's point, capped at 3; an exponent
 * form counts as 3 */
uint8_t decimals_of(const std::string& value)
{
    if (value.find_first_of("eE") != std::string::npos) {
        return 3;
    }
    const size_t dot = value.find('.');
    if (dot == std::string::npos) {
        return 0;
    }
    const size_t after = value.size() - dot - 1;
    return static_cast<uint8_t>(after > 3 ? 3 : after);
}

const char* const kMadeUp =
    "made up by Havne: the adapter delivered this frame's message whole";
const char* const kMadeUpFlow =
    "made up by Havne: the Flow Control a tester sends for this message";

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};
using file_ptr = std::unique_ptr<FILE, file_closer>;

bool fail(const char* fmt, const std::string& a = std::string())
{
    ecuio_detail_set(fmt, a.c_str());
    return false;
}

using ecuio_frame_rows::frame_rows;

namespace fr = ecuio_frame_reassembly;
using fr::any_id_messages;
using fr::joined;
using fr::message_end;
using fr::other_id_identification;
using fr::reassembly;
using fr::seg;

/* The ID a re-segmented reply's Flow Control goes on: the physical
 * request ID paired with it (isotp_flow_control_id), 0 for none */
uint32_t flow_id(uint32_t id, bool extended)
{
    const uint32_t fc =
        isotp_flow_control_id(id, extended ? 0x18DB33F1u : 0x7DFu);
    if (extended) {
        return fc != 0x18DB33F1u ? fc : 0;
    }
    return id >= 9 && id <= 0x7FFu ? fc : 0;
}

/* The frames a message of len bytes re-segments into, its Flow Control
 * among them */
uint64_t segments(uint32_t id, bool extended, uint16_t len)
{
    if (len <= 7) {
        return 1;
    }
    return 1 + (flow_id(id, extended) != 0 ? 1 : 0) +
           (static_cast<uint64_t>(len) - 6 + 6) / 7;
}

/* text as ecuio_decode_text writes a hex value, two hex digits a byte with
 * one space between (either case), into *bytes; false for anything else */
bool hex_pairs(const std::string& text, std::string* bytes)
{
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    if (text.size() < 2 || (text.size() + 1) % 3 != 0) {
        return false;
    }
    std::string out;
    for (size_t i = 0; i < text.size(); i += 3) {
        const int hi = digit(text[i]);
        const int lo = digit(text[i + 1]);
        if (hi < 0 || lo < 0 || (i + 2 < text.size() && text[i + 2] != ' ')) {
            return false;
        }
        out += static_cast<char>(hi << 4 | lo);
    }
    *bytes = std::move(out);
    return true;
}

bool re_segmentable(const ecuio_frame_t& f)
{
    return (f.flags & ECUIO_FRAME_FLAG_TRUNCATED) == 0 && f.data_length >= 1 &&
           f.data_length <= ECUIO_ISOTP_MAX;
}

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
             std::string* partial)
    {
        if (folder_) {
            return run_folder(folder, out);
        }
        const bool csv_out = o_.format == ECUIO_EXPORT_CSV;
        if (!read_header(folder) ||
            (!raw_ && (!read_text_values(folder) || !text_defs(folder))) ||
            (plan_frames(folder, csv_out) && !plan(folder))) {
            return false;
        }
        if (csv_out) {
            /* The plan's frame counts are the pcapng's; the CSV counts
             * values */
            r_ = ecuio_export_report_t{};
            if (wide_ ? !wide_survey(folder) : !values(folder, false)) {
                return false;
            }
        }
        if (files::exists(out)) {
            return fail("the file exists already");
        }
        std::string why;
        /* the name goes in *partial before the file is made */
        file_.reset(files::partial_file(out, partial, &why));
        if (!file_) {
            partial->clear();
            return fail("the export can't be written: %s", why);
        }
        if (!(csv_out ? (wide_ ? write_wide(folder) : write_csv(folder))
                      : write(folder))) {
            return false;
        }
        if (std::fclose(file_.release()) != 0) {
            return fail("the export can't be written: %s",
                        files::errno_words(errno));
        }
        if (!files::rename_to(*partial, out, false, &why)) {
            return fail("the export can't be put in place: %s", why);
        }
        partial->clear();
        return true;
    }

    /* The wide CSV's size for each interval, nothing written
     * (ecuio_export_estimate) */
    bool estimate(const std::string& folder, const uint32_t* row_ms,
                  uint32_t count, ecuio_export_estimate_t* out)
    {
        if (!read_header(folder) || (!raw_ && !text_defs(folder)) ||
            !wide_survey(folder)) {
            return false;
        }
        std::vector<wide_rows> engines(count);
        for (uint32_t i = 0; i < count; ++i) {
            engines[i].row_ms = row_ms[i];
        }
        if (!wide_walk(folder, false, &engines)) {
            return false;
        }
        const size_t head = wide_head().size();
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = ecuio_export_estimate_t{};
            out[i].rows = engines[i].rows;
            out[i].bytes = head + engines[i].bytes;
            out[i].columns = static_cast<uint32_t>(columns_.size());
        }
        return true;
    }

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
    bool read_header(const std::string& folder)
    {
        header_ = rs::read_header(folder);
        text_names_.insert(header_.text_names.begin(),
                           header_.text_names.end());
        return true;
    }

    /* The plan's pass over frames.csv: always for the pcapng; for the
     * CSV only scrubbed, for the identity values the check searches for,
     * and only when the recording holds frames */
    bool plan_frames(const std::string& folder, bool csv_out) const
    {
        return !csv_out || (!raw_ && files::exists(folder + "/frames.csv"));
    }

    /* A def keyed as an identification reply (49 xx, 62 F180-F1FF) at the
     * message's start, judged by the classifier the frames go through */
    static bool identification_def(const ecuio_signal_def_t& d)
    {
        if (d.mux_length == 0 || d.mux_length > 3 || d.mux_offset != 0) {
            return false;
        }
        uint8_t key[3] = {};
        for (uint8_t i = 0; i < d.mux_length; ++i) {
            key[i] = static_cast<uint8_t>(
                d.mux_key >>
                (8u * static_cast<unsigned>(d.mux_length - 1 - i)));
        }
        return scrub::identification(key, d.mux_length);
    }

    /* The recording's text and hex defs (ADR-0054 section 1): its own
     * signals.json, else uds-generic's set from its URI's did= list, else
     * its profile by name. None found leaves the value search alone to
     * catch a text value. A signals.json that doesn't load refuses the
     * export: the set it names may have text defs no other set has. */
    bool text_defs(const std::string& folder)
    {
        const ecuio_signal_set_t* set = set_.load(folder, header_);
        if (set_.own_refused()) {
            return fail("the recording's signals.json doesn't load (%s): a "
                        "scrubbed export can't tell which signals are text",
                        set_.own_why());
        }
        if (set != nullptr) {
            for (uint32_t i = 0; i < set->count; ++i) {
                const ecuio_signal_def_t& d = set->defs[i];
                if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
                    continue;
                }
                if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
                    text_.push_back(&d);
                    text_names_.insert(d.name);
                } else if (identification_def(d)) {
                    ident_names_.insert(d.name);
                }
            }
        }
        return true;
    }

    /* A message a text or hex def applies to */
    bool text_message(uint32_t id, uint32_t flags, const uint8_t* data,
                      size_t n)
    {
        if (text_.empty() || n > ECUIO_FRAME_DATA_MAX) {
            return false;
        }
        probe_->arbitration_id = id;
        probe_->flags = flags;
        probe_->data_length = static_cast<uint16_t>(n);
        std::memcpy(probe_->data, data, n);
        for (const ecuio_signal_def_t* d : text_) {
            if (ecuio_def_applies_to_frame(d, probe_.get()) != 0) {
                return true;
            }
        }
        return false;
    }

    /* Each text value signals.csv holds: what a number can't be */
    bool read_text_values(const std::string& folder)
    {
        if (canceled(true)) {
            return false;
        }
        std::string why;
        file_ptr f(files::open_file(folder + "/signals.csv", false, &why));
        if (!f) {
            return true;
        }
        std::vector<char> buf(rows::kRowMax + 2);
        bool too_long = false;
        bool torn = false;
        uint64_t line = 0;
        rows::signal_row row;
        std::unordered_set<std::string> seen;
        while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
            if (canceled()) {
                return false;
            }
            ++line;
            if (line == 1) {
                continue; /* the header */
            }
            const char* bad = nullptr;
            if (too_long ||
                !rows::parse_signal_row(buf.data(), std::strlen(buf.data()),
                                        row, &bad)) {
                ecuio_detail_set("signals.csv line %llu: %s",
                                 static_cast<unsigned long long>(line),
                                 too_long ? "the row is too long" : bad);
                return false;
            }
            double v = 0.0;
            if (row.value.size() < scrub::kValueMin ||
                rows::parse_number(row.value.data(), row.value.size(), &v) ||
                !seen.insert(row.value).second) {
                continue;
            }
            /* A hex def's value is its bytes as spaced pairs: searched as
             * those bytes, so the shortest value and the padding split
             * count bytes, not characters (the bytes' forms include the
             * spaced text) */
            std::string bytes;
            const std::string& value =
                hex_pairs(row.value, &bytes) ? bytes : row.value;
            if (!values_.add(value.data(), value.size(), kTextValues)) {
                return fail("the recording holds more text values than the "
                            "export's check can take");
            }
        }
        return true;
    }

    void left_out(const seg& s, uint32_t cls, bool counted)
    {
        dropped_.push_back(s.index);
        if (in_range(s.host_us)) {
            ++r_.left_out[cls];
            if (counted) {
                --r_.frames;
            }
        }
    }

    bool add_identity(const uint8_t* msg, size_t n)
    {
        std::vector<std::string> v;
        scrub::identification_values(msg, n, v);
        for (const std::string& s : v) {
            if (!values_.add(s.data(), s.size(), kIdentification)) {
                return fail("the recording holds more identity values than "
                            "the export's check can take");
            }
        }
        return true;
    }

    /* The first pass: each frame's fate, the counts, the identity values */
    bool plan(const std::string& folder)
    {
        if (canceled(true)) {
            return false;
        }
        frame_rows rows_in;
        if (!rows_in.open(folder)) {
            return false;
        }
        reassembly rs;
        other_id_identification others;
        std::vector<uint8_t> whole;
        std::vector<message_end> ends;
        uint64_t index = 0;
        int got = 0;
        while ((got = rows_in.next()) == 1) {
            if (canceled()) {
                return false;
            }
            const ecuio_frame_t& f = rows_in.frame();
            const seg s{index++, rows_in.host_us()};
            ends.clear();
            if (reassembly::on_the_wire(f)) {
                if (in_range(s.host_us)) {
                    ++r_.frames;
                }
                (void)rs.take(s, f, ends);
                if (!judge(ends)) {
                    return false;
                }
                continue;
            }
            if ((f.flags & ECUIO_FRAME_FLAG_REASSEMBLED) != 0) {
                if (!folder_ && !re_segmentable(f)) {
                    left_out(s, kUnattributable, false);
                } else if (!raw_ &&
                           scrub::identification(f.data, f.data_length)) {
                    if (!add_identity(f.data, f.data_length)) {
                        return false;
                    }
                    left_out(s, kIdentification, false);
                } else if (!raw_ && text_message(f.arbitration_id, f.flags,
                                                 f.data, f.data_length)) {
                    left_out(s, kTextValues, false);
                } else if (folder_ && in_range(s.host_us)) {
                    ++r_.frames; /* kept whole, one row */
                } else if (in_range(s.host_us)) {
                    const uint64_t n =
                        segments(f.arbitration_id,
                                 (f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0,
                                 static_cast<uint16_t>(f.data_length));
                    r_.frames += n;
                    r_.made_up += n;
                }
                continue;
            }
            uint8_t bytes[pc::kFdBytes];
            uint32_t n = 0;
            /* a frame no CAN frame can be, or a reply whose ISO-TP form
             * isn't read (CAN FD's) */
            if ((!folder_ && !pc::socketcan(f, bytes, &n)) ||
                (!raw_ && reassembly::unread_reply(f))) {
                left_out(s, kUnattributable, false);
            } else if (!raw_ && others.take(f, &whole)) {
                if (!whole.empty() &&
                    !add_identity(whole.data(), whole.size())) {
                    return false;
                }
                left_out(s, kIdentification, false);
            } else if (!raw_ && text_message(f.arbitration_id, f.flags, f.data,
                                             f.data_length)) {
                left_out(s, kTextValues, false);
            } else if (in_range(s.host_us)) {
                ++r_.frames;
            }
        }
        if (got < 0) {
            return false;
        }
        ends.clear();
        rs.finish(ends);
        if (!judge(ends)) {
            return false;
        }
        std::vector<std::vector<uint8_t>> short_replies;
        others.finish(short_replies);
        for (const std::vector<uint8_t>& m : short_replies) {
            if (!add_identity(m.data(), m.size())) {
                return false;
            }
        }
        std::sort(dropped_.begin(), dropped_.end());
        return true;
    }

    /* Messages reassembled from the wire: an identification reply's
     * frames left out, a text or hex def's, a torn one's on a reply ID */
    bool judge(std::vector<message_end>& ends)
    {
        for (message_end& e : ends) {
            if (raw_) {
                continue;
            }
            if (e.whole) {
                if (scrub::identification(e.data.data(), e.data.size())) {
                    if (!add_identity(e.data.data(), e.data.size())) {
                        return false;
                    }
                    for (const seg& s : e.frames) {
                        left_out(s, kIdentification, true);
                    }
                } else if (text_message(
                               e.id,
                               (e.extended ? ECUIO_FRAME_FLAG_EXTENDED : 0u) |
                                   ECUIO_FRAME_FLAG_REASSEMBLED,
                               e.data.data(), e.data.size())) {
                    for (const seg& s : e.frames) {
                        left_out(s, kTextValues, true);
                    }
                }
            } else if (scrub::segment_reply_id(e.id, e.extended)) {
                for (const seg& s : e.frames) {
                    left_out(s, kUnattributable, true);
                }
            }
        }
        return true;
    }

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

    bool flush(bool all)
    {
        if (buf_.empty() || (!all && buf_.size() < kWriteChunk)) {
            return true;
        }
        if (!raw_) {
            file_check_->feed(reinterpret_cast<const uint8_t*>(buf_.data()),
                              buf_.size());
        }
        if (std::fwrite(buf_.data(), 1, buf_.size(), file_.get()) !=
            buf_.size()) {
            return fail("the export can't be written: %s",
                        files::errno_words(errno));
        }
        buf_.clear();
        return true;
    }

    void put(const ecuio_frame_t& f, uint64_t host_us,
             const std::string* comment)
    {
        uint8_t bytes[pc::kFdBytes];
        uint32_t n = 0;
        if (!pc::socketcan(f, bytes, &n)) {
            return; /* the plan left it out */
        }
        const uint32_t dir =
            (f.flags & ECUIO_FRAME_FLAG_TX_ECHO) != 0 ? pc::kOutbound : 0;
        pc::enhanced_packet(buf_, header_.utc_us + host_us, bytes, n, comment,
                            dir);
        ++written_;
    }

    void put_can(const isotp_can& c, uint32_t flags, uint64_t host_us,
                 const std::string& comment)
    {
        std::memset(made_.get(), 0, offsetof(ecuio_frame_t, data) + 8);
        made_->arbitration_id = c.id;
        made_->flags = flags;
        made_->data_length = c.len;
        std::memcpy(made_->data, c.data, c.len);
        put(*made_, host_us, &comment);
    }

    /* A message an adapter delivered whole, as its sender sent it */
    void re_segment(const ecuio_frame_t& f, uint64_t host_us)
    {
        const uint32_t flags = f.flags & ECUIO_FRAME_FLAG_EXTENDED;
        const bool ext = flags != 0;
        *tx_ = isotp_tx{};
        isotp_can c;
        (void)isotp_tx_start(*tx_, f.arbitration_id, f.data,
                             static_cast<uint16_t>(f.data_length), 0, &c);
        put_can(c, flags, host_us, made_up_);
        if (f.data_length <= 7) {
            return;
        }
        const uint32_t fc = flow_id(f.arbitration_id, ext);
        const isotp_can flow = isotp_flow_control(fc);
        if (fc != 0) {
            put_can(flow, fc > 0x7FFu ? ECUIO_FRAME_FLAG_EXTENDED : 0u, host_us,
                    made_up_flow_);
        }
        (void)isotp_tx_flow(*tx_, flow, 0);
        while (isotp_tx_next(*tx_, 0, &c) == isotp_tx_step::frame) {
            put_can(c, flags, host_us, made_up_);
        }
    }

    std::string section_comment() const
    {
        char buf[384];
        std::snprintf(
            buf, sizeof buf,
            "Havne %s, %s (ADR-0054): %llu frames, %llu of them made up from "
            "messages an adapter delivered whole; left out: %llu frames "
            "of identification replies, %llu of text signals' messages, "
            "%llu segments of messages that couldn't be reassembled",
            ecuio_version_string(),
            raw_ ? "a raw export, not scrubbed" : "a scrubbed export",
            static_cast<unsigned long long>(r_.frames),
            static_cast<unsigned long long>(r_.made_up),
            static_cast<unsigned long long>(r_.left_out[kIdentification]),
            static_cast<unsigned long long>(r_.left_out[kTextValues]),
            static_cast<unsigned long long>(r_.left_out[kUnattributable]));
        return buf;
    }

    /* The second pass */
    bool write(const std::string& folder)
    {
        if (canceled(true)) {
            return false;
        }
        file_check_.reset(new scrub::check(values_));
        made_up_ = kMadeUp;
        made_up_flow_ = kMadeUpFlow;
        pc::section_header(buf_, section_comment(),
                           std::string("Havne ") + ecuio_version_string());
        pc::interface_block(buf_);
        frame_rows rows_in;
        if (!rows_in.open(folder)) {
            return false;
        }
        reassembly rs;
        any_id_messages any;
        std::vector<message_end> ends;
        uint64_t index = 0;
        int got = 0;
        while ((got = rows_in.next()) == 1) {
            if (canceled()) {
                return false;
            }
            const ecuio_frame_t& f = rows_in.frame();
            const seg s{index++, rows_in.host_us()};
            const bool wire = reassembly::on_the_wire(f);
            if (wire) {
                ends.clear();
                (void)rs.take(s, f, ends);
                if (!check_ends(ends)) {
                    return false;
                }
            }
            const bool written = in_range(s.host_us) && !is_dropped(s.index);
            if (!wire && !check_joined(any, &f, written)) {
                return false;
            }
            if (!written) {
                continue;
            }
            if (!wire && !check_message(f.data, f.data_length)) {
                return false;
            }
            if ((f.flags & ECUIO_FRAME_FLAG_REASSEMBLED) != 0) {
                re_segment(f, s.host_us);
            } else {
                put(f, s.host_us, nullptr);
            }
            if (!flush(false)) {
                return false;
            }
        }
        if (got < 0) {
            return false;
        }
        ends.clear();
        rs.finish(ends);
        if (!check_ends(ends) || !check_joined(any, nullptr, false) ||
            !flush(true)) {
            return false;
        }
        if (written_ != r_.frames) {
            return fail("the recording changed while it was exported");
        }
        if (!raw_) {
            const uint32_t found = file_check_->finish();
            if (found != 0) {
                r_.failed_class = found;
                return fail("the check found %s in the export's file: nothing "
                            "was written",
                            scrub::failed_words(found));
            }
        }
        return true;
    }

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
    static bool formula_start(const std::string& s)
    {
        return !s.empty() && std::strchr("=+-@\t\r", s[0]) != nullptr;
    }

    /* signals.csv in host_us order, each value a scrubbed export keeps:
     * counted (write false: the values, the signals, the times written)
     * or written. The same decisions both times. */
    bool values(const std::string& folder, bool write)
    {
        if (canceled(true)) {
            return false;
        }
        std::string why;
        file_ptr f(files::open_file(folder + "/signals.csv", false, &why));
        if (!f) {
            return fail("the folder has no signals.csv");
        }
        std::vector<char> buf(rows::kRowMax + 2);
        bool too_long = false;
        bool torn = false;
        uint64_t line = 0;
        rows::signal_row row;
        std::unordered_set<std::string> signals;
        while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
            if (canceled()) {
                return false;
            }
            ++line;
            if (line == 1) {
                if (write && folder_) {
                    buf_ += buf.data(); /* the header, its BOM with it */
                    buf_ += '\n';
                }
                continue;
            }
            const char* bad = nullptr;
            if (too_long ||
                !rows::parse_signal_row(buf.data(), std::strlen(buf.data()),
                                        row, &bad)) {
                ecuio_detail_set("signals.csv line %llu: %s",
                                 static_cast<unsigned long long>(line),
                                 too_long ? "the row is too long" : bad);
                return false;
            }
            if (!in_range(row.host_us)) {
                continue;
            }
            if (!raw_ && ident_names_.count(row.name) != 0) {
                r_.left_out[kIdentification] += write ? 0u : 1u;
                continue;
            }
            double v = 0.0;
            const bool number =
                rows::parse_number(row.value.data(), row.value.size(), &v);
            if (!raw_ && (!number || text_names_.count(row.name) != 0)) {
                r_.left_out[kTextValues] += write ? 0u : 1u;
                continue;
            }
            if (!write && !check_formula(row, number)) {
                return false;
            }
            if (!write) {
                ++r_.values;
                signals.insert(row.name);
                first_us_ = r_.values == 1 ? row.host_us : first_us_;
                last_us_ = row.host_us;
                continue;
            }
            if (folder_) {
                buf_ += buf.data();
                buf_ += '\n';
            } else {
                put_row(row);
            }
            ++written_;
            if (!flush(false)) {
                return false;
            }
        }
        if (!write) {
            r_.signal_count = signals.size();
        }
        return true;
    }

    /* A name, a unit or a raw text value that a spreadsheet would run */
    static bool check_formula(const rows::signal_row& row, bool number)
    {
        const char* what = nullptr;
        if (formula_start(row.name)) {
            what = "name";
        } else if (formula_start(row.unit)) {
            what = "unit";
        } else if (!number && formula_start(row.value)) {
            what = "text value";
        }
        if (what == nullptr) {
            return true;
        }
        ecuio_detail_set("the signal %s's %s starts as a spreadsheet formula "
                         "does (=, +, -, @, a tab or a carriage return): "
                         "nothing was written",
                         ecuio_json::quoted(row.name).c_str(), what);
        return false;
    }

    /* One CSV field, RFC 4180's quotes only where it holds a comma, a
     * quote or a line end */
    void put_field(const std::string& s)
    {
        if (s.find_first_of(",\"\r\n") == std::string::npos) {
            buf_ += s;
            return;
        }
        buf_ += '"';
        for (char c : s) {
            buf_ += c;
            if (c == '"') {
                buf_ += '"';
            }
        }
        buf_ += '"';
    }

    static std::string seconds(uint64_t us)
    {
        char b[32];
        std::snprintf(b, sizeof b, "%llu.%06llu",
                      static_cast<unsigned long long>(us / 1000000u),
                      static_cast<unsigned long long>(us % 1000000u));
        return b;
    }

    void put_row(const rows::signal_row& row)
    {
        buf_ += seconds(row.host_us);
        buf_ += ',';
        put_field(row.name);
        buf_ += ',';
        put_field(row.unit);
        buf_ += ',';
        put_field(row.value);
        buf_ += '\n';
    }

    std::string csv_comment() const
    {
        const std::string span = r_.values == 0
                                     ? std::string()
                                     : ", " + seconds(first_us_) + " s to " +
                                           seconds(last_us_) + " s";
        char buf[384];
        std::snprintf(
            buf, sizeof buf,
            "# Havne %s, %s (ADR-0054): %llu values of %llu signals%s; left "
            "out: %llu values of identification signals, %llu of text "
            "signals\n",
            ecuio_version_string(),
            raw_ ? "a raw export, not scrubbed" : "a scrubbed export",
            static_cast<unsigned long long>(r_.values),
            static_cast<unsigned long long>(r_.signal_count), span.c_str(),
            static_cast<unsigned long long>(r_.left_out[kIdentification]),
            static_cast<unsigned long long>(r_.left_out[kTextValues]));
        return buf;
    }

    /* The CSV's second pass */
    bool write_csv(const std::string& folder)
    {
        file_check_.reset(new scrub::check(values_));
        buf_ = csv_comment();
        buf_ += "time_s,signal,unit,value\n";
        if (!values(folder, true) || !flush(true)) {
            return false;
        }
        if (written_ != r_.values) {
            return fail("the recording changed while it was exported");
        }
        if (!raw_) {
            const uint32_t found = file_check_->finish();
            if (found != 0) {
                r_.failed_class = found;
                return fail("the check found %s in the export's file: nothing "
                            "was written",
                            scrub::failed_words(found));
            }
        }
        return true;
    }

    // ---- The wide CSV (ADR-0056 sections 4 and 5) ----

    struct wide_column {
        std::string name;
        std::string unit;     /* the header's, else the first row's */
        std::string head;     /* "Name (unit)" in the export's units, a field */
        uint8_t decimals = 0; /* the header's; else the most its values show */
        bool in_header = false;
        uint64_t samples = 0;
        uint64_t last_us = 0;
        timing_hist deltas; /* between its samples */
        uint64_t silence_us = kSilenceUs;
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
             * than the last row, at the completing sample's time */
            for (size_t j = 0; j < cols.size(); ++j) {
                if (in_rule[j] != 0 && fresh[j] == 0) {
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

    /* The refusal for a wide CSV with more columns than kWideColumnsMax,
     * given as soon as one past it is met */
    static bool too_many_columns()
    {
        ecuio_detail_set("the wide CSV is limited to %u columns: pick the "
                         "columns to keep",
                         static_cast<unsigned>(kWideColumnsMax));
        return false;
    }

    /* A column by name, in order of arrival; its header line's unit
     * and decimals when the header has it */
    size_t add_column(const std::string& name)
    {
        wide_column c;
        c.name = name;
        for (const rows::header_signal& hs : header_.signals) {
            if (hs.name == name) {
                c.in_header = true;
                c.unit = hs.unit;
                c.decimals = hs.decimal_places;
                break;
            }
        }
        columns_.push_back(c);
        index_[name] = columns_.size() - 1;
        return columns_.size() - 1;
    }

    /* A column's name and unit, as check_formula judges a row's */
    bool check_column(const wide_column& c)
    {
        rows::signal_row row;
        row.name = c.name;
        row.unit = c.unit;
        row.value = "0";
        return check_formula(row, true);
    }

    static std::string csv_field(const std::string& s)
    {
        if (s.find_first_of(",\"\r\n") == std::string::npos) {
            return s;
        }
        std::string out = "\"";
        for (char ch : s) {
            out += ch;
            if (ch == '"') {
                out += '"';
            }
        }
        out += '"';
        return out;
    }

    /* The first pass: the columns (named, the header's focus, or every
     * number signal met) with their samples, interval, unit and decimals;
     * the values left out counted */
    bool wide_survey(const std::string& folder)
    {
        if (o_.column_count > kWideColumnsMax) {
            return too_many_columns();
        }
        std::vector<std::string> picked;
        picked.reserve(o_.column_count);
        for (uint32_t i = 0; i < o_.column_count; ++i) {
            picked.emplace_back(o_.columns_utf8[i]);
        }
        /* The header's focus line is the recording's, so a pick it can't
         * give is a bad recording (LOAD_FAILED), not the caller's */
        const bool from_header = picked.empty();
        if (from_header) {
            picked = header_.focus;
        }
        const auto reject = [this, from_header](const char* fmt,
                                                const std::string& a) {
            return from_header ? fail(fmt, a) : refuse(fmt, a);
        };
        fixed_columns_ = !picked.empty();
        for (const std::string& name : picked) {
            if (index_.count(name) != 0) {
                return reject("the column %s is named twice",
                              ecuio_json::quoted(name));
            }
            if (!raw_ && text_names_.count(name) != 0) {
                return reject("the signal %s is a text signal, never a column",
                              ecuio_json::quoted(name));
            }
            if (!raw_ && ident_names_.count(name) != 0) {
                return reject("the signal %s is an identification signal, "
                              "never a column",
                              ecuio_json::quoted(name));
            }
            add_column(name);
        }
        if (!fixed_columns_) {
            /* The header's number signals first, in its order */
            for (const rows::header_signal& hs : header_.signals) {
                if (!hs.text && index_.count(hs.name) == 0 &&
                    (raw_ || (text_names_.count(hs.name) == 0 &&
                              ident_names_.count(hs.name) == 0))) {
                    if (columns_.size() >= kWideColumnsMax) {
                        return too_many_columns();
                    }
                    add_column(hs.name);
                }
            }
        }
        if (!wide_walk(folder, true, nullptr)) {
            return false;
        }
        if (!fixed_columns_) {
            /* A header signal with no sample in range isn't a column */
            std::vector<wide_column> kept;
            index_.clear();
            for (const wide_column& c : columns_) {
                if (c.samples > 0) {
                    index_[c.name] = kept.size();
                    kept.push_back(c);
                }
            }
            columns_.swap(kept);
        }
        for (wide_column& c : columns_) {
            if (!check_column(c)) {
                return false;
            }
            const uint64_t usual =
                c.deltas.count > 0 ? c.deltas.quantile(0.5) : 0;
            c.silence_us = (std::max)(kSilenceUs, kSilenceTimes * usual);
            std::string unit = c.unit;
            ecuio_display_value_t dv{};
            if (ecuio_display_convert(c.unit.c_str(), c.decimals, 0.0, o_.units,
                                      &dv) == ECUIO_DISPLAY_OK) {
                unit = dv.unit;
            }
            c.head =
                csv_field(unit.empty() ? c.name : c.name + " (" + unit + ")");
        }
        r_.signal_count = columns_.size();
        return true;
    }

    std::string wide_head() const
    {
        std::string h = "Time (ms)";
        for (const wide_column& c : columns_) {
            h += ',';
            h += c.head;
        }
        h += "\r\n";
        return h;
    }

    /* A cell: the value in the export's units, to the places shown */
    std::string cell_text(const wide_column& c, double v) const
    {
        ecuio_display_value_t dv{};
        if (ecuio_display_convert(c.unit.c_str(), c.decimals, v, o_.units,
                                  &dv) != ECUIO_DISPLAY_OK) {
            return fixed_text(v, c.decimals);
        }
        return fixed_text(dv.value, dv.decimal_places);
    }

    /* signals.csv's number rows in range, by column: the survey counts
     * and classifies, the second pass feeds each interval's rows */
    bool wide_walk(const std::string& folder, bool survey,
                   std::vector<wide_rows>* engines)
    {
        if (canceled(true)) {
            return false;
        }
        std::string why;
        file_ptr f(files::open_file(folder + "/signals.csv", false, &why));
        if (!f) {
            return fail("the folder has no signals.csv");
        }
        if (!survey) {
            for (wide_rows& e : *engines) {
                e.start(columns_, first_us_);
            }
        }
        std::vector<char> buf(rows::kRowMax + 2);
        bool too_long = false;
        bool torn = false;
        uint64_t line = 0;
        rows::signal_row row;
        std::string text;
        while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
            if (canceled()) {
                return false;
            }
            ++line;
            if (line == 1) {
                continue;
            }
            const char* bad = nullptr;
            if (too_long ||
                !rows::parse_signal_row(buf.data(), std::strlen(buf.data()),
                                        row, &bad)) {
                ecuio_detail_set("signals.csv line %llu: %s",
                                 static_cast<unsigned long long>(line),
                                 too_long ? "the row is too long" : bad);
                return false;
            }
            if (!in_range(row.host_us)) {
                continue;
            }
            if (!raw_ && ident_names_.count(row.name) != 0) {
                r_.left_out[kIdentification] += survey ? 1u : 0u;
                continue;
            }
            double v = 0.0;
            const bool number =
                rows::parse_number(row.value.data(), row.value.size(), &v);
            if (!number || (!raw_ && text_names_.count(row.name) != 0)) {
                r_.left_out[kTextValues] += survey ? 1u : 0u;
                continue;
            }
            auto it = index_.find(row.name);
            if (it == index_.end()) {
                if (fixed_columns_ || !survey) {
                    continue;
                }
                if (columns_.size() >= kWideColumnsMax) {
                    return too_many_columns();
                }
                add_column(row.name);
                it = index_.find(row.name);
            }
            wide_column& c = columns_[it->second];
            if (survey) {
                if (c.samples == 0 && c.unit.empty()) {
                    c.unit = row.unit;
                }
                if (c.samples > 0 && row.host_us >= c.last_us) {
                    c.deltas.add(static_cast<int64_t>(row.host_us - c.last_us));
                }
                if (!c.in_header) {
                    c.decimals = (std::max)(c.decimals, decimals_of(row.value));
                }
                c.last_us = row.host_us;
                ++c.samples;
                first_us_ = surveyed_ == 0 ? row.host_us : first_us_;
                last_us_ = row.host_us;
                ++surveyed_;
                continue;
            }
            ++seen_;
            text = cell_text(c, v);
            for (wide_rows& e : *engines) {
                e.sample(it->second, row.host_us, text, columns_);
                if (e.sink != nullptr && !flush(false)) {
                    return false;
                }
            }
        }
        if (!survey) {
            for (wide_rows& e : *engines) {
                e.finish(last_us_, columns_);
            }
        }
        return true;
    }

    /* The wide CSV's second pass, written and checked as the long's */
    bool write_wide(const std::string& folder)
    {
        file_check_.reset(new scrub::check(values_));
        buf_ = wide_head();
        std::vector<wide_rows> engines(1);
        engines[0].row_ms = o_.row_ms;
        engines[0].sink = &buf_;
        if (!wide_walk(folder, false, &engines) || !flush(true)) {
            return false;
        }
        if (seen_ != surveyed_) {
            return fail("the recording changed while it was exported");
        }
        r_.values = engines[0].cells;
        r_.rows = engines[0].rows;
        if (!raw_) {
            const uint32_t found = file_check_->finish();
            if (found != 0) {
                r_.failed_class = found;
                return fail("the check found %s in the export's file: nothing "
                            "was written",
                            scrub::failed_words(found));
            }
        }
        return true;
    }

    // ---- The folder export ----

    /* A caller line's text a scrubbed copy keeps, rewritten; false to
     * leave it out: the front end's name and version, the surrogate's
     * frame logging, and the vehicle's label as <vehicle> */
    static bool kept_caller(const std::string& text, std::string* out)
    {
        if (rows::starts_with(text.data(), text.size(), "Havne ") ||
            rows::starts_with(text.data(), text.size(), "havne-cli ") ||
            rows::starts_with(text.data(), text.size(),
                              "surrogate frame logging  ")) {
            *out = text;
            return true;
        }
        if (rows::starts_with(text.data(), text.size(), "vehicle  ")) {
            *out = "vehicle  <vehicle>";
            return true;
        }
        return false;
    }

    /* events.log, header line by line, then the activity lines by kind,
     * into events_head_ and events_body_; the lines left out counted */
    bool plan_events(const std::string& folder)
    {
        if (canceled(true)) {
            return false;
        }
        std::string why;
        file_ptr f(files::open_file(folder + "/events.log", false, &why));
        if (!f) {
            return true;
        }
        std::vector<char> buf(rows::kRowMax + 2);
        bool too_long = false;
        bool header = true;
        bool first = true;
        while (csv::read_line(f.get(), buf, &too_long)) {
            if (canceled()) {
                return false;
            }
            if (too_long) {
                return fail("events.log holds a line longer than a row's");
            }
            const std::string line = buf.data();
            if (first) {
                first = false;
                if (line != rows::kFormat2 && line != rows::kFormat3) {
                    /* Another format's log: a file the export doesn't
                     * know */
                    ++r_.files_left_out;
                    return true;
                }
                has_events_ = true;
                events_head_.push_back(line);
                continue;
            }
            if (header) {
                if (line.empty()) {
                    header = false;
                } else {
                    head_line(line);
                }
                continue;
            }
            if (!line.empty()) {
                activity_line(line);
            }
        }
        return true;
    }

    void head_line(const std::string& line)
    {
        const char* p = line.data();
        const size_t n = line.size();
        if (raw_) {
            events_head_.push_back(line);
            return;
        }
        static const char* const kKept[] = {
            "started  ",     "utc  ",     "core  ",     "profile  ",
            "frame flags  ", "focus  ",   "link  ",     "signal  ",
            "label  ",       "columns  ", "min sweep  "};
        for (const char* k : kKept) {
            if (rows::starts_with(p, n, k)) {
                events_head_.push_back(line);
                return;
            }
        }
        if (rows::starts_with(p, n, rows::kSignalFileKey) ||
            rows::starts_with(p, n, rows::kPluginKey)) {
            events_head_.push_back(line);
            return;
        }
        if (rows::starts_with(p, n, "uri  ")) {
            events_head_.push_back("uri  " + scrub::uri(line.substr(5)));
            return;
        }
        std::string text;
        if (rows::starts_with(p, n, "caller  ") &&
            kept_caller(line.substr(8), &text)) {
            events_head_.push_back("caller  " + text);
            return;
        }
        ++events_left_;
    }

    void activity_line(const std::string& line)
    {
        rows::event_line e;
        const char* text = nullptr;
        size_t text_len = 0;
        size_t kind_len = 0;
        if (!rows::parse_event_line(line.data(), line.size(), &e) ||
            !rows::split_kind(e.rest, e.rest_len, &text, &text_len,
                              &kind_len)) {
            ++events_left_;
            return;
        }
        const std::string kind(e.rest, kind_len);
        const std::string head(line.data(),
                               static_cast<size_t>(e.rest - line.data()));
        if (kind == "end") {
            uint64_t records = 0;
            uint64_t notes = 0;
            if (raw_) {
                events_body_.push_back(line);
                return;
            }
            if (!rows::parse_end(text, text_len, &records, &notes)) {
                ++events_left_;
                return;
            }
            /* The counts from the last ": ", the why the caller's words */
            const std::string all(text, text_len);
            const size_t at = all.rfind(": ");
            events_body_.push_back(head + "end  <why>" + all.substr(at));
            return;
        }
        if (!in_range(e.ms * 1000)) {
            return;
        }
        if (raw_ || kind == "link" || kind == "drop" || kind == "asleep") {
            events_body_.push_back(line);
            return;
        }
        ++events_left_; /* a note, the caller's text, or a kind unknown */
    }

    /* markers.csv or latency.csv: the header and the rows in the range,
     * as they are */
    bool plan_rows(const std::string& folder, const char* name, bool markers,
                   std::vector<std::string>& out, bool* present)
    {
        if (canceled(true)) {
            return false;
        }
        std::string why;
        file_ptr f(files::open_file(folder + "/" + name, false, &why));
        *present = static_cast<bool>(f);
        if (!f) {
            return true;
        }
        std::vector<char> buf(rows::kRowMax + 2);
        bool too_long = false;
        bool torn = false;
        uint64_t line = 0;
        std::string field;
        while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
            if (canceled()) {
                return false;
            }
            ++line;
            if (line == 1) {
                out.emplace_back(buf.data());
                continue;
            }
            const size_t n = std::strlen(buf.data());
            uint64_t host_us = 0;
            const char* bad = "the row is too long";
            bool ok = !too_long;
            if (ok && markers) {
                ok = rows::parse_marker_row(buf.data(), n, &host_us, field,
                                            &bad);
            } else if (ok) {
                /* t_s, seconds to the ms, then the counts */
                const char* comma =
                    static_cast<const char*>(std::memchr(buf.data(), ',', n));
                double t = 0.0;
                ok = comma != nullptr &&
                     rows::parse_number(buf.data(),
                                        static_cast<size_t>(comma - buf.data()),
                                        &t) &&
                     t >= 0.0 && t < 1e12;
                bad = "t_s isn't a time in seconds";
                host_us = static_cast<uint64_t>(std::llround(t * 1e6));
            }
            if (!ok) {
                ecuio_detail_set("%s line %llu: %s", name,
                                 static_cast<unsigned long long>(line), bad);
                return false;
            }
            if (in_range(host_us)) {
                out.emplace_back(buf.data());
            }
        }
        return true;
    }

    /* The files a folder holds that the export doesn't know */
    void count_unknown(const std::string& folder)
    {
        static const char* const kKnown[] = {"events.log",  "frames.csv",
                                             "signals.csv", "markers.csv",
                                             "latency.csv", "signals.json"};
        std::vector<std::string> names;
        if (!files::list_folder(folder, &names)) {
            return;
        }
        for (const std::string& name : names) {
            bool known = false;
            for (const char* k : kKnown) {
                known = known || name == k;
            }
            r_.files_left_out += known ? 0u : 1u;
        }
    }

    std::string export_line() const
    {
        const uint64_t vi =
            r_.left_out[kIdentification] - frames_left_[kIdentification];
        const uint64_t vt =
            r_.left_out[kTextValues] - frames_left_[kTextValues];
        char buf[1024];
        std::snprintf(
            buf, sizeof buf,
            "export  %s (ADR-0054), Havne %s: frames.csv %llu frames (left "
            "out: %llu of identification replies, %llu of text signals' "
            "messages, %llu segments of messages that couldn't be "
            "reassembled); signals.csv %llu values (left out: %llu of "
            "identification signals, %llu of text signals); markers.csv %llu "
            "markers, their labels as typed; events.log %llu lines left out "
            "(the caller's notes, its other lines and lines of a kind the "
            "export doesn't know); other files left out: %u",
            raw_ ? "a raw copy, not scrubbed" : "a scrubbed copy",
            ecuio_version_string(), static_cast<unsigned long long>(r_.frames),
            static_cast<unsigned long long>(frames_left_[kIdentification]),
            static_cast<unsigned long long>(frames_left_[kTextValues]),
            static_cast<unsigned long long>(frames_left_[kUnattributable]),
            static_cast<unsigned long long>(r_.values),
            static_cast<unsigned long long>(vi),
            static_cast<unsigned long long>(vt),
            static_cast<unsigned long long>(
                markers_.empty() ? 0 : markers_.size() - 1),
            static_cast<unsigned long long>(events_left_), r_.files_left_out);
        return buf;
    }

    /* A file of the copy: written under partial, checked as it goes */
    bool begin_file(const std::string& partial, const char* name)
    {
        if (canceled(true)) {
            return false;
        }
        std::string why;
        /* listed before it's made, so a failure past it (bad_alloc
         * included) still removes it with the partial folder */
        written_paths_.push_back(partial + "/" + name);
        file_.reset(files::open_file(written_paths_.back(), true, &why));
        if (!file_) {
            written_paths_.pop_back();
            return fail("the export can't be written: %s", why);
        }
        file_check_.reset(new scrub::check(values_));
        buf_.clear();
        return true;
    }

    bool end_file(const char* name)
    {
        if (!flush(true)) {
            return false;
        }
        if (std::fclose(file_.release()) != 0) {
            return fail("the export can't be written: %s",
                        files::errno_words(errno));
        }
        if (!raw_) {
            const uint32_t found = file_check_->finish();
            if (found != 0) {
                r_.failed_class = found;
                ecuio_detail_set("the check found %s in the copy's %s: "
                                 "nothing was written",
                                 scrub::failed_words(found), name);
                return false;
            }
        }
        ++r_.files;
        return true;
    }

    bool put_lines(const std::vector<std::string>& lines)
    {
        for (const std::string& l : lines) {
            if (canceled()) {
                return false;
            }
            buf_ += l;
            buf_ += '\n';
            if (!flush(false)) {
                return false;
            }
        }
        return true;
    }

    /* frames.csv's kept rows as recorded, each message a kept row
     * carries checked before segmenting, as the pcapng's second pass */
    bool write_frames(const std::string& folder)
    {
        if (canceled(true)) {
            return false;
        }
        buf_ += "host_us,timestamp_us,id,flags,data\n";
        frame_rows rows_in;
        if (!rows_in.open(folder)) {
            return false;
        }
        reassembly rs;
        any_id_messages any;
        std::vector<message_end> ends;
        uint64_t index = 0;
        int got = 0;
        written_ = 0;
        while ((got = rows_in.next()) == 1) {
            if (canceled()) {
                return false;
            }
            const ecuio_frame_t& f = rows_in.frame();
            const seg s{index++, rows_in.host_us()};
            const bool wire = reassembly::on_the_wire(f);
            if (wire) {
                ends.clear();
                (void)rs.take(s, f, ends);
                if (!check_ends(ends)) {
                    return false;
                }
            }
            const bool written = in_range(s.host_us) && !is_dropped(s.index);
            if (!wire && !check_joined(any, &f, written)) {
                return false;
            }
            if (!written) {
                continue;
            }
            if (!wire && !check_message(f.data, f.data_length)) {
                return false;
            }
            buf_ += rows_in.line();
            buf_ += '\n';
            ++written_;
            if (!flush(false)) {
                return false;
            }
        }
        if (got < 0) {
            return false;
        }
        ends.clear();
        rs.finish(ends);
        if (!check_ends(ends) || !check_joined(any, nullptr, false)) {
            return false;
        }
        if (written_ != r_.frames) {
            return fail("the recording changed while it was exported");
        }
        return true;
    }

    bool copy_signal_file(const std::string& folder)
    {
        std::string why;
        file_ptr f(files::open_file(folder + "/signals.json", false, &why));
        if (!f) {
            return true;
        }
        if (!begin_file(partial_, "signals.json")) {
            return false;
        }
        char chunk[1u << 14];
        size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof chunk, f.get())) > 0) {
            buf_.append(chunk, n);
            if (!flush(false)) {
                return false;
            }
        }
        return end_file("signals.json");
    }

    bool write_folder(const std::string& folder)
    {
        if (has_events_) {
            std::vector<std::string> head;
            bool placed = false;
            for (const std::string& l : events_head_) {
                if (!placed &&
                    rows::starts_with(l.data(), l.size(), "columns  ")) {
                    head.push_back(export_line());
                    placed = true;
                }
                head.push_back(l);
            }
            if (!placed) {
                head.push_back(export_line());
            }
            if (!begin_file(partial_, "events.log") || !put_lines(head)) {
                return false;
            }
            buf_ += '\n';
            if (!put_lines(events_body_) || !end_file("events.log")) {
                return false;
            }
        }
        if (has_frames_ && (!begin_file(partial_, "frames.csv") ||
                            !write_frames(folder) || !end_file("frames.csv"))) {
            return false;
        }
        if (has_values_) {
            written_ = 0;
            if (!begin_file(partial_, "signals.csv") || !values(folder, true) ||
                !end_file("signals.csv")) {
                return false;
            }
            if (written_ != r_.values) {
                return fail("the recording changed while it was exported");
            }
        }
        if (has_markers_ &&
            (!begin_file(partial_, "markers.csv") || !put_lines(markers_) ||
             !end_file("markers.csv"))) {
            return false;
        }
        if (has_latency_ &&
            (!begin_file(partial_, "latency.csv") || !put_lines(latency_) ||
             !end_file("latency.csv"))) {
            return false;
        }
        return copy_signal_file(folder);
    }

    /* The folder export's passes: every count first, then each file */
    bool run_folder(const std::string& folder, const std::string& out)
    {
        has_frames_ = files::exists(folder + "/frames.csv");
        has_values_ = files::exists(folder + "/signals.csv");
        if (!has_frames_ && !has_values_) {
            return fail("the folder has no frames.csv or signals.csv");
        }
        if (!read_header(folder) ||
            (!raw_ && (!read_text_values(folder) || !text_defs(folder))) ||
            (has_frames_ && !plan(folder))) {
            return false;
        }
        for (uint32_t c = 0; c < ECUIO_EXPORT_LEFT_OUT_COUNT; ++c) {
            frames_left_[c] = r_.left_out[c];
        }
        if ((has_values_ && !values(folder, false)) || !plan_events(folder) ||
            !plan_rows(folder, "markers.csv", true, markers_, &has_markers_) ||
            !plan_rows(folder, "latency.csv", false, latency_, &has_latency_)) {
            return false;
        }
        r_.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY] += events_left_;
        count_unknown(folder);
        if (files::exists(out)) {
            return fail("the folder exists already");
        }
        std::string why;
        if (!files::partial_folder(out, &partial_, &why)) {
            partial_.clear();
            return fail("the export can't be written: %s", why);
        }
        bool ok = false;
        try {
            ok = write_folder(folder);
            file_.reset();
            if (ok && !files::rename_to(partial_, out, false, &why)) {
                ok = fail("the export can't be put in place: %s", why);
            }
        } catch (const std::bad_alloc&) {
            remove_partial();
            throw;
        }
        if (!ok) {
            remove_partial();
        }
        return ok;
    }

    /* The partial folder and the files written into it */
    void remove_partial()
    {
        file_.reset();
        for (const std::string& path : written_paths_) {
            files::remove_file(path);
        }
        files::remove_folder(partial_);
        r_.files = 0;
    }

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
    /* The folder export's */
    std::string partial_;
    std::vector<std::string> written_paths_;
    uint64_t frames_left_[ECUIO_EXPORT_LEFT_OUT_COUNT] = {};
    bool has_frames_ = false;
    bool has_values_ = false;
    bool has_events_ = false;
    bool has_markers_ = false;
    bool has_latency_ = false;
    std::vector<std::string> events_head_;
    std::vector<std::string> events_body_;
    std::vector<std::string> markers_;
    std::vector<std::string> latency_;
    uint64_t events_left_ = 0;
    std::string made_up_;
    std::string made_up_flow_;
    std::vector<joined> joins_;
    uint64_t written_ = 0;
};

} // namespace

namespace {

/* The options every call checks before anything is read; an estimate
 * needs no file to write and only the wide CSV */
ecuio_status_t check_options(const ecuio_export_options_t* options,
                             bool estimate)
{
    if (options == nullptr || options->folder_utf8 == nullptr ||
        (!estimate && options->out_path_utf8 == nullptr)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    for (uint64_t v : options->reserved) {
        if (v != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    if ((options->flags & ~ECUIO_EXPORT_RAW) != 0 ||
        (options->to_us != 0 && options->to_us < options->from_us)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    switch (options->format) {
    case ECUIO_EXPORT_PCAPNG:
    case ECUIO_EXPORT_CSV:
    case ECUIO_EXPORT_FOLDER:
        break;
    default:
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const bool wide = options->format == ECUIO_EXPORT_CSV &&
                      options->layout == ECUIO_CSV_WIDE;
    if (options->layout > ECUIO_CSV_LONG ||
        (options->format != ECUIO_EXPORT_CSV && options->layout != 0)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (!wide) {
        /* The shape fields are the wide CSV's alone */
        if (options->row_ms != 0 || options->units != 0 ||
            options->column_count != 0 || options->columns_utf8 != nullptr ||
            estimate) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        return ECUIO_OK;
    }
    if (options->units > ECUIO_UNITS_IMPERIAL) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (options->column_count > 0 && options->columns_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    for (uint32_t i = 0; i < options->column_count; ++i) {
        if (options->columns_utf8[i] == nullptr) {
            return ECUIO_ERR_NULL_ARGUMENT;
        }
        if (options->columns_utf8[i][0] == '\0') {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    return ECUIO_OK;
}

/* The folder's path with no trailing separators */
std::string folder_of(const char* utf8)
{
    std::string folder = utf8;
    while (folder.size() > 1 &&
           (folder.back() == '/' || folder.back() == '\\')) {
        folder.pop_back();
    }
    return folder;
}

} // namespace

extern "C" ecuio_status_t ecuio_export(const ecuio_export_options_t* options,
                                       ecuio_export_report_t* report)
{
    return ecuio_export_with(options, nullptr, nullptr, report);
}

extern "C" ecuio_status_t
ecuio_export_with(const ecuio_export_options_t* options, ecuio_cancel_t cancel,
                  void* user_data, ecuio_export_report_t* report)
{
    if (report != nullptr) {
        *report = ecuio_export_report_t{};
    }
    const ecuio_status_t checked = check_options(options, false);
    if (checked != ECUIO_OK) {
        return checked;
    }
    ecuio_detail_clear();
    ecuio_export_report_t r{};
    std::string out;     /* these two allocate nothing until the try */
    std::string partial; /* the pcapng's or CSV's, while not in place */
    bool ok = false;
    ecuio_status_t failed = ECUIO_ERR_LOAD_FAILED;
    try {
        const std::string folder = folder_of(options->folder_utf8);
        out = options->out_path_utf8;
        {
            /* closed before its partial is removed: Windows removes no
             * file still open */
            recording_export e(*options, r);
            e.set_cancel(cancel, user_data);
            ok = e.run(folder, out, &partial);
            failed = e.status();
        }
        if (!partial.empty()) {
            files::remove_file(partial);
        }
    } catch (const std::bad_alloc&) {
        try {
            if (!partial.empty()) {
                files::remove_file(partial);
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): out of memory
                        // again; the partial stays
        }
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (!ok) {
        const uint32_t failed_class = r.failed_class;
        r = ecuio_export_report_t{};
        r.failed_class = failed_class;
    }
    if (report != nullptr) {
        *report = r;
    }
    return ok ? ECUIO_OK : failed;
}

extern "C" ecuio_status_t
ecuio_export_estimate(const ecuio_export_options_t* options,
                      const uint32_t* row_ms, uint32_t count,
                      ecuio_export_estimate_t* out)
{
    return ecuio_export_estimate_with(options, row_ms, count, nullptr, nullptr,
                                      out);
}

extern "C" ecuio_status_t
ecuio_export_estimate_with(const ecuio_export_options_t* options,
                           const uint32_t* row_ms, uint32_t count,
                           ecuio_cancel_t cancel, void* user_data,
                           ecuio_export_estimate_t* out)
{
    if (row_ms == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const ecuio_status_t checked = check_options(options, true);
    if (checked != ECUIO_OK) {
        return checked;
    }
    if (count == 0 || count > kEstimateCountMax) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = ecuio_export_estimate_t{};
    }
    ecuio_detail_clear();
    ecuio_export_report_t r{};
    try {
        recording_export e(*options, r);
        e.set_cancel(cancel, user_data);
        if (!e.estimate(folder_of(options->folder_utf8), row_ms, count, out)) {
            return e.status();
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}
