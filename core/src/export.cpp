// File/layer: core/src/export.cpp | C++17, Qt-free; a recording out to
//             another tool's format (ADR-0054 sections 1 and 3)
// Testing: UNIT: ExportTest (test_core)
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

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"
#include "isotp.h"
#include "pcapng.h"
#include "recording_check.h" /* iso15765_id */
#include "recording_rows.h"
#include "scrub.h"
#include "uds_generic.h"
#include "uri_query.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace csv = ecuio_frames_csv;
namespace files = ecuio_files;
namespace rows = ecuio_recording_rows;
namespace scrub = ecuio_scrub;
namespace pc = ecuio_pcapng;

constexpr size_t kWriteChunk = 1u << 16;
constexpr uint32_t kIdentification = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION;
constexpr uint32_t kUnattributable = ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE;
constexpr uint32_t kTextValues = ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES;

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

struct signal_file_free {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};

bool fail(const char* fmt, const std::string& a = std::string())
{
    ecuio_detail_set(fmt, a.c_str());
    return false;
}

/* frames.csv's rows in order; a torn last row is left out (ADR-0049
 * section 7) */
class frame_rows
{
public:
    frame_rows() : buf_(csv::kLineMax + 2), frame_(new ecuio_frame_t{}) {}

    bool open(const std::string& folder)
    {
        std::string why;
        f_.reset(files::open_file(folder + "/frames.csv", false, &why));
        if (!f_) {
            return fail("the recording's frames.csv can't be opened: %s", why);
        }
        if (!csv::read_header(f_.get(), buf_)) {
            return false;
        }
        line_ = 1;
        prev_ = 0;
        return true;
    }

    /* 1 a row, 0 the end, -1 a row refused (the detail set) */
    int next()
    {
        bool too_long = false;
        bool torn = false;
        if (!csv::read_line(f_.get(), buf_, &too_long, &torn) || torn) {
            return 0;
        }
        ++line_;
        if (too_long) {
            return refuse("the row is longer than a frame's");
        }
        const char* why = nullptr;
        std::memset(frame_.get(), 0, sizeof *frame_);
        if (!csv::parse_row(buf_.data(), &row_, frame_.get(), &why)) {
            return refuse(why);
        }
        if (row_.host_us < prev_) {
            return refuse("host_us is earlier than the row before");
        }
        prev_ = row_.host_us;
        return 1;
    }

    uint64_t host_us() const { return row_.host_us; }
    const ecuio_frame_t& frame() const { return *frame_; }

private:
    int refuse(const char* why)
    {
        ecuio_detail_set("frames.csv line %llu: %s",
                         static_cast<unsigned long long>(line_), why);
        return -1;
    }

    file_ptr f_;
    std::vector<char> buf_;
    std::unique_ptr<ecuio_frame_t> frame_;
    csv::row row_;
    uint64_t line_ = 0;
    uint64_t prev_ = 0;
};

/* A frame by its row's place and time */
struct seg {
    uint64_t index = 0;
    uint64_t host_us = 0;
};

/* A message's end: whole (reassembled from its frames) or torn (its frames
 * no message uses) */
struct message_end {
    bool whole = false;
    uint32_t id = 0;
    bool extended = false;
    std::vector<seg> frames;
    std::vector<uint8_t> data;
};

/* ISO-TP messages from frames as they crossed the bus, each with the rows
 * that carried it: the reader's rules (two First Frames on one ID end
 * both, N_Cr on the rows' host_us) */
class reassembly
{
public:
    reassembly() : iso_(new isotp_rx) { iso_->timed = true; }

    /* A frame as it crossed the bus on ISO 15765-4's IDs: no adapter
     * reassembled it, and its first nibble is a PCI's */
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

    /* Row s's frame f, which on_the_wire takes: the messages it ends go in
     * out. False for a frame that stands alone (a Flow Control). */
    bool take(const seg& s, const ecuio_frame_t& f,
              std::vector<message_end>& out)
    {
        const auto now = static_cast<int64_t>(s.host_us);
        if (isotp_expire(*iso_, now) > 0) {
            reconcile(out);
        }
        isotp_can c;
        c.id = f.arbitration_id;
        c.len = static_cast<uint8_t>(f.data_length);
        std::memcpy(c.data, f.data, f.data_length);
        const bool ext = (f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
        const isotp_pci kind = isotp_kind(c);
        if (kind == isotp_pci::first) {
            isotp_rx_slot* open = isotp_rx_find(*iso_, c.id);
            if (open != nullptr) {
                open->active = false;
                ++iso_->dropped;
                torn(c.id, ext, &s, out);
                return true;
            }
        }
        isotp_message m;
        switch (isotp_take(*iso_, c, now, &m)) {
        case isotp_result::complete: {
            if (kind == isotp_pci::single) {
                torn(c.id, ext, nullptr, out); /* one it ended, if any */
            }
            message_end e;
            e.whole = true;
            e.id = c.id;
            e.extended = ext;
            e.frames = take_held(c.id);
            e.frames.push_back(s);
            e.data.assign(m.data, m.data + m.len);
            out.push_back(std::move(e));
            break;
        }
        case isotp_result::first:
            held_.push_back(held{c.id, ext, {s}});
            break;
        case isotp_result::pending:
            for (held& h : held_) {
                if (h.id == c.id) {
                    h.frames.push_back(s);
                }
            }
            break;
        case isotp_result::flow:
            return false;
        case isotp_result::malformed:
        case isotp_result::dropped:
            torn(c.id, ext, &s, out);
            break;
        }
        reconcile(out);
        return true;
    }

    /* The messages still under way at the end are torn */
    void finish(std::vector<message_end>& out)
    {
        for (held& h : held_) {
            message_end e;
            e.id = h.id;
            e.extended = h.extended;
            e.frames = std::move(h.frames);
            out.push_back(std::move(e));
        }
        held_.clear();
    }

private:
    struct held {
        uint32_t id = 0;
        bool extended = false;
        std::vector<seg> frames;
    };

    std::vector<seg> take_held(uint32_t id)
    {
        for (size_t i = 0; i < held_.size(); ++i) {
            if (held_[i].id == id) {
                std::vector<seg> f = std::move(held_[i].frames);
                held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
                return f;
            }
        }
        return {};
    }

    /* id's message under way, and the frame last when given, as torn */
    void torn(uint32_t id, bool ext, const seg* last,
              std::vector<message_end>& out)
    {
        message_end e;
        e.id = id;
        e.extended = ext;
        e.frames = take_held(id);
        if (last != nullptr) {
            e.frames.push_back(*last);
        }
        if (!e.frames.empty()) {
            out.push_back(std::move(e));
        }
    }

    /* A message ISO-TP gave up on (N_Cr, or evicted for a newer one) */
    void reconcile(std::vector<message_end>& out)
    {
        for (size_t i = 0; i < held_.size();) {
            if (isotp_rx_find(*iso_, held_[i].id) == nullptr) {
                message_end e;
                e.id = held_[i].id;
                e.extended = held_[i].extended;
                e.frames = std::move(held_[i].frames);
                out.push_back(std::move(e));
                held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    std::unique_ptr<isotp_rx> iso_;
    std::vector<held> held_;
};

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

bool re_segmentable(const ecuio_frame_t& f)
{
    return (f.flags & ECUIO_FRAME_FLAG_TRUNCATED) == 0 && f.data_length >= 1 &&
           f.data_length <= ECUIO_ISOTP_MAX;
}

/* The export's two passes */
class pcapng_export
{
public:
    pcapng_export(const ecuio_export_options_t& o, ecuio_export_report_t& r)
        : o_(o), r_(r), raw_((o.flags & ECUIO_EXPORT_RAW) != 0),
          made_(new ecuio_frame_t{}), probe_(new ecuio_frame_t{}),
          tx_(new isotp_tx)
    {}

    bool run(const std::string& folder, const std::string& out, bool* made)
    {
        if (!read_header(folder) ||
            (!raw_ && (!read_text_values(folder) || !text_defs(folder))) ||
            !plan(folder)) {
            return false;
        }
        if (files::exists(out)) {
            return fail("the file exists already");
        }
        const std::string partial = out + ".partial";
        std::string why;
        file_.reset(files::open_file(partial, true, &why));
        if (!file_) {
            return fail("the export can't be written: %s", why);
        }
        *made = true;
        if (!write(folder)) {
            return false;
        }
        if (std::fclose(file_.release()) != 0) {
            return fail("the export can't be written: %s",
                        files::errno_words(errno));
        }
        if (!files::rename_to(partial, out, false, &why)) {
            return fail("the export can't be put in place: %s", why);
        }
        *made = false;
        return true;
    }

private:
    bool in_range(uint64_t host_us) const
    {
        return host_us >= o_.from_us && (o_.to_us == 0 || host_us <= o_.to_us);
    }

    /* events.log's header: the utc line, the recording's start (none
     * gives 0), its profile and URI */
    bool read_header(const std::string& folder)
    {
        std::string why;
        file_ptr f(files::open_file(folder + "/events.log", false, &why));
        if (!f) {
            return true;
        }
        std::vector<char> buf(rows::kRowMax + 2);
        bool too_long = false;
        while (csv::read_line(f.get(), buf, &too_long)) {
            const size_t n = std::strlen(buf.data());
            if (too_long || n == 0) {
                break;
            }
            if (rows::starts_with(buf.data(), n, "utc  ")) {
                (void)rows::parse_utc(buf.data() + 5, n - 5, &utc_us_);
            } else if (rows::starts_with(buf.data(), n, "profile  ")) {
                profile_.assign(buf.data() + 9, n - 9);
            } else if (rows::starts_with(buf.data(), n, "uri  ")) {
                uri_.assign(buf.data() + 5, n - 5);
            }
        }
        return true;
    }

    /* The recording's text and hex defs (ADR-0054 section 1): its own
     * signals.json, else uds-generic's set from its URI's did= list, else
     * its profile by name. None found leaves the value search alone to
     * catch a text value. */
    bool text_defs(const std::string& folder)
    {
        const ecuio_signal_set_t* set = nullptr;
        std::string why;
        file_ptr f(files::open_file(folder + "/signals.json", false, &why));
        if (f) {
            std::string bytes(ECUIO_SIGNAL_FILE_MAX_BYTES, '\0');
            bytes.resize(std::fread(&bytes[0], 1, bytes.size(), f.get()));
            ecuio_signal_file_t* loaded = nullptr;
            if (ecuio_signal_file_load(bytes.data(), bytes.size(), &loaded) ==
                ECUIO_OK) {
                file_set_.reset(loaded);
                set = ecuio_signal_file_set(loaded);
            }
        }
        if (set == nullptr && profile_ == ECUIO_UDS_GENERIC_NAME) {
            const size_t scheme = uri_.find("://");
            ecuio_uri_query query;
            if (scheme != std::string::npos &&
                ecuio_uri::split(ecuio_uri::query_of(uri_.c_str() + scheme + 3),
                                 &query) == ecuio_uri::split_result::ok) {
                uds_.reset(new ecuio_uds_session);
                if (ecuio_uds_build(query, uds_.get()) == ECUIO_OK) {
                    set = &uds_->set;
                }
            }
        } else if (set == nullptr && !profile_.empty()) {
            (void)ecuio_profile_lookup(profile_.c_str(), &set);
        }
        ecuio_detail_clear();
        if (set != nullptr) {
            for (uint32_t i = 0; i < set->count; ++i) {
                const ecuio_signal_def_t& d = set->defs[i];
                if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 &&
                    (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX)) {
                    text_.push_back(&d);
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
            if (!values_.add(row.value.data(), row.value.size(), kTextValues)) {
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
        frame_rows rows_in;
        if (!rows_in.open(folder)) {
            return false;
        }
        reassembly rs;
        std::vector<message_end> ends;
        uint64_t index = 0;
        int got = 0;
        while ((got = rows_in.next()) == 1) {
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
                if (!re_segmentable(f)) {
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
            if (!pc::socketcan(f, bytes, &n)) {
                left_out(s, kUnattributable, false);
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
            } else if (scrub::reply_id(e.id, e.extended)) {
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
        pc::enhanced_packet(buf_, utc_us_ + host_us, bytes, n, comment, dir);
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
        std::vector<message_end> ends;
        uint64_t index = 0;
        int got = 0;
        while ((got = rows_in.next()) == 1) {
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
            if (!in_range(s.host_us) || is_dropped(s.index)) {
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
        if (!check_ends(ends) || !flush(true)) {
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

    const ecuio_export_options_t& o_;
    ecuio_export_report_t& r_;
    const bool raw_;
    uint64_t utc_us_ = 0;
    scrub::values values_;
    std::vector<uint64_t> dropped_;
    file_ptr file_;
    std::string buf_;
    std::unique_ptr<scrub::check> file_check_;
    std::unique_ptr<ecuio_frame_t> made_;
    std::unique_ptr<ecuio_frame_t> probe_;
    std::unique_ptr<isotp_tx> tx_;
    std::string profile_;
    std::string uri_;
    std::unique_ptr<ecuio_signal_file_t, signal_file_free> file_set_;
    std::unique_ptr<ecuio_uds_session> uds_;
    std::vector<const ecuio_signal_def_t*> text_;
    std::string made_up_;
    std::string made_up_flow_;
    uint64_t written_ = 0;
};

} // namespace

extern "C" ecuio_status_t ecuio_export(const ecuio_export_options_t* options,
                                       ecuio_export_report_t* report)
{
    if (report != nullptr) {
        *report = ecuio_export_report_t{};
    }
    if (options == nullptr || options->folder_utf8 == nullptr ||
        options->out_path_utf8 == nullptr) {
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
        break;
    case ECUIO_EXPORT_FOLDER:
    case ECUIO_EXPORT_CSV:
        return ECUIO_ERR_NOT_IMPLEMENTED;
    default:
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    std::string folder = options->folder_utf8;
    while (folder.size() > 1 &&
           (folder.back() == '/' || folder.back() == '\\')) {
        folder.pop_back();
    }
    const std::string out = options->out_path_utf8;
    ecuio_export_report_t r{};
    bool made = false;
    bool ok = false;
    try {
        pcapng_export e(*options, r);
        ok = e.run(folder, out, &made);
    } catch (const std::bad_alloc&) {
        if (made) {
            files::remove_file(out + ".partial");
        }
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (made) {
        files::remove_file(out + ".partial");
    }
    if (report != nullptr) {
        *report = r;
    }
    return ok ? ECUIO_OK : ECUIO_ERR_LOAD_FAILED;
}
