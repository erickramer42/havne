// File/layer: core/src/export_pcapng.cpp | C++17, Qt-free; a recording's frames out as pcapng
//             and the export's first pass
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)

#include "export_engine.h"

#include "pcapng.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ecuio_export_detail {

namespace pc = ecuio_pcapng;

namespace {

const char* const kMadeUp =
    "made up by Havne: the adapter delivered this frame's message whole";
const char* const kMadeUpFlow =
    "made up by Havne: the Flow Control a tester sends for this message";

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

} // namespace

bool recording_export::identification_def(const ecuio_signal_def_t& d)
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

bool recording_export::text_defs(const std::string& folder)
{
    const ecuio_signal_set_t* set = set_.load(folder, header_);
    if (set_.own_refused()) {
        return fail("the recording's signals.json doesn't load (%s): a "
                    "scrubbed export can't tell which signals are text",
                    set_.own_why());
    }
    if (set != nullptr) {
        const std::vector<std::string> ids =
            rs::def_ids(set, set_.file(), header_.keyed);
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
                continue;
            }
            if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
                text_.push_back(&d);
                text_names_.insert(ids[i]);
            } else if (identification_def(d)) {
                ident_names_.insert(ids[i]);
            }
        }
    }
    return true;
}

bool recording_export::text_message(uint32_t id, uint32_t flags, const uint8_t* data,
                                    size_t n)
{
    return fr::text_message(*probe_, text_, id, flags, data, n);
}

bool recording_export::read_text_values(const std::string& folder)
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

void recording_export::left_out(const seg& s, uint32_t cls, bool counted)
{
    dropped_.push_back(s.index);
    if (in_range(s.host_us)) {
        ++r_.left_out[cls];
        if (counted && r_.frames > 0) {
            --r_.frames; /* never wraps, whatever a join names */
        }
    }
}

bool recording_export::add_identity(const uint8_t* msg, size_t n)
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

bool recording_export::plan_whole(const ecuio_frame_t& f, const seg& s)
{
    if (!folder_ && !re_segmentable(f)) {
        left_out(s, kUnattributable, false);
    } else if (!raw_ && scrub::identification(f.data, f.data_length)) {
        if (!add_identity(f.data, f.data_length)) {
            return false;
        }
        left_out(s, kIdentification, false);
    } else if (!raw_ &&
               text_message(f.arbitration_id, f.flags, f.data, f.data_length)) {
        left_out(s, kTextValues, false);
    } else if (!raw_ && vin_window(f.data, f.data_length)) {
        left_out(s, kVinShaped, false);
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
    return true;
}

bool recording_export::plan_other(const ecuio_frame_t& f, const seg& s,
                                  other_id_identification& others,
                                  std::vector<uint8_t>& whole)
{
    uint8_t bytes[pc::kFdBytes];
    uint32_t n = 0;
    /* a frame no CAN frame can be, or a reply whose ISO-TP form isn't
     * read (CAN FD's) */
    if ((!folder_ && !pc::socketcan(f, bytes, &n)) ||
        (!raw_ && reassembly::unread_reply(f))) {
        left_out(s, kUnattributable, false);
    } else if (!raw_ && others.take(f, &whole)) {
        if (!whole.empty() && !add_identity(whole.data(), whole.size())) {
            return false;
        }
        left_out(s, kIdentification, false);
    } else if (!raw_ &&
               text_message(f.arbitration_id, f.flags, f.data, f.data_length)) {
        left_out(s, kTextValues, false);
    } else if (!raw_ && vin_window(f.data, f.data_length)) {
        left_out(s, kVinShaped, false); /* a CAN FD frame's payload */
    } else if (in_range(s.host_us)) {
        ++r_.frames;
    }
    return true;
}

bool recording_export::vin_window(const uint8_t* p, size_t n)
{
    if (!scrub::check::window_in(p, n)) {
        return false;
    }
    vin_messages_.emplace_back(p, p + n);
    return true;
}

void recording_export::vin_joins(std::vector<joined>& joins)
{
    for (joined& j : joins) {
        if (scrub::check::window_in(j.data.data(), j.data.size())) {
            vin_joined_.push_back(std::move(j));
        }
    }
}

bool recording_export::vin_end()
{
    for (const std::vector<uint8_t>& m : vin_messages_) {
        const uint32_t found = values_.find(m.data(), m.size());
        if (found != 0) {
            r_.failed_class = found;
            return fail("the check found %s in a message the export would "
                        "write: nothing was written",
                        scrub::failed_words(found));
        }
    }
    return vin_joined_end();
}

bool recording_export::vin_joined_end()
{
    std::sort(dropped_.begin(), dropped_.end());
    std::vector<seg> out;
    for (const joined& j : vin_joined_) {
        bool kept = false;
        for (const seg& s : j.frames) {
            kept = kept || !is_dropped(s.index);
        }
        if (!kept) {
            continue; /* another class left it out */
        }
        const uint32_t found = values_.find(j.data.data(), j.data.size());
        if (found != 0) {
            r_.failed_class = found;
            return fail("the check found %s in a message the export would "
                        "write: nothing was written",
                        scrub::failed_words(found));
        }
        out.insert(out.end(), j.frames.begin(), j.frames.end());
    }
    std::sort(out.begin(), out.end(),
              [](const seg& a, const seg& b) { return a.index < b.index; });
    const auto planned = static_cast<std::ptrdiff_t>(dropped_.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if ((i > 0 && out[i - 1].index == out[i].index) ||
            std::binary_search(dropped_.begin(), dropped_.begin() + planned,
                               out[i].index)) {
            continue;
        }
        left_out(out[i], kVinShaped, true);
    }
    return true;
}

bool recording_export::plan(const std::string& folder)
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
    any_id_messages any;
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
        if (!raw_ && !reassembly::on_the_wire(f) && fr::tp_readable(f)) {
            joins_.clear();
            any.take(f, true, joins_, &s);
            vin_joins(joins_);
        }
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
        const bool ok = (f.flags & ECUIO_FRAME_FLAG_REASSEMBLED) != 0
                            ? plan_whole(f, s)
                            : plan_other(f, s, others, whole);
        if (!ok) {
            return false;
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
    joins_.clear();
    any.finish(joins_);
    vin_joins(joins_);
    if (!vin_end()) {
        return false;
    }
    std::sort(dropped_.begin(), dropped_.end());
    return true;
}

bool recording_export::judge(std::vector<message_end>& ends)
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
            } else if (vin_window(e.data.data(), e.data.size())) {
                for (const seg& s : e.frames) {
                    left_out(s, kVinShaped, true);
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

void recording_export::put(const ecuio_frame_t& f, uint64_t host_us,
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

void recording_export::put_can(const isotp_can& c, uint32_t flags, uint64_t host_us,
                               const std::string& comment)
{
    std::memset(made_.get(), 0, offsetof(ecuio_frame_t, data) + 8);
    made_->arbitration_id = c.id;
    made_->flags = flags;
    made_->data_length = c.len;
    std::memcpy(made_->data, c.data, c.len);
    put(*made_, host_us, &comment);
}

void recording_export::re_segment(const ecuio_frame_t& f, uint64_t host_us)
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

std::string recording_export::section_comment() const
{
    char buf[384];
    std::snprintf(
        buf, sizeof buf,
        "Havne %s, %s (ADR-0054): %llu frames, %llu of them made up from "
        "messages an adapter delivered whole; left out: %llu frames "
        "of identification replies, %llu of text signals' messages, "
        "%llu segments of messages that couldn't be reassembled, %llu "
        "frames of messages holding a VIN-shaped token",
        ecuio_version_string(),
        raw_ ? "a raw export, not scrubbed" : "a scrubbed export",
        static_cast<unsigned long long>(r_.frames),
        static_cast<unsigned long long>(r_.made_up),
        static_cast<unsigned long long>(r_.left_out[kIdentification]),
        static_cast<unsigned long long>(r_.left_out[kTextValues]),
        static_cast<unsigned long long>(r_.left_out[kUnattributable]),
        static_cast<unsigned long long>(r_.left_out[kVinShaped]));
    return buf;
}

bool recording_export::write(const std::string& folder)
{
    if (canceled(true)) {
        return false;
    }
    start_check(scrub::kRefuse);
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

} // namespace ecuio_export_detail
