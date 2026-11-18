// File/layer: core/src/health.cpp | C++17, Qt-free; a car's health as one
//             scrubbed report (ADR-0054's appendix "the health report as
//             built")
// Testing: UNIT: HealthTest, HealthCancelTest (test_core)
//
// One model of the facts, built once from the inputs (health.h), then
// written as JSON or text by two writers that read only the model. The
// identification items' text and the VIN never enter the model: the
// vehicle is the VIN's decode, an item its name and outcome. The file is
// then checked for those texts and for the VIN's shape, as an export's
// (scrub.h), under a name of its own beside <out> (folder_files.h's
// partial_name), renamed into place once it passes. A recording's report
// asks the caller's cancel (ecuio_health_export_with) as the frames' pass
// begins and every kCancelRows of its rows, and the reader's open asks it
// as it reads; a stop ends it before anything is written.

#include "health.h"

#include "error_detail.h"
#include "folder_files.h"
#include "frame_reassembly.h"
#include "frame_rows.h"
#include "frames_csv.h" /* read_line */
#include "health_model.h"
#include "health_replies.h"
#include "recording_rows.h" /* parse_number */
#include "recording_set.h"
#include "recording_stretch.h" /* seconds_text */
#include "scrub.h"
#include "session.h"

#include "ecuio/profile.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <unordered_map>
#include <vector>

namespace ecuio_health {

namespace {

namespace files = ecuio_files;
namespace scrub = ecuio_scrub;

bool refused(const char* fmt, const std::string& a = std::string())
{
    ecuio_detail_set(fmt, a.c_str());
    return false;
}

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};

struct recording_closer {
    void operator()(ecuio_recording_t* r) const { ecuio_recording_close(r); }
};

/* The vehicle's baseline recording as its events.log names it: the
 * header's utc, profile and uri lines (recording_set.h's read_header) and
 * its length, the last timed line's time. Its folder's name is never
 * read into the report: it can hold the vehicle's label. A folder that's
 * gone, or holds no recording's events.log, is named as unread with its
 * why, never its path. */
baseline_facts read_baseline(const std::string& folder)
{
    namespace rows = ecuio_recording_rows;
    baseline_facts b;
    b.named = true;
    if (!files::exists(folder)) {
        b.why = "its folder is gone: it was moved or deleted";
        return b;
    }
    std::string why;
    std::unique_ptr<FILE, file_closer> f(
        files::open_file(folder + "/events.log", false, &why));
    if (!f) {
        b.why = "its folder holds no events.log";
        return b;
    }
    std::vector<char> buf(rows::kRowMax + 2);
    bool too_long = false;
    bool rest_of_long = false; /* the pieces of a line past kRowMax */
    bool first = true;
    uint64_t last_ms = 0;
    while (ecuio_frames_csv::read_line(f.get(), buf, &too_long)) {
        const char* p = buf.data();
        const size_t n = std::strlen(p);
        const bool piece = rest_of_long;
        rest_of_long = too_long;
        if (first) {
            /* the reader's test: a byte order mark skipped, format 2 or
             * 3 by the whole line, format 1 by its ending */
            first = false;
            const size_t bom =
                n >= 3 && std::memcmp(p, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
            const char* line = p + bom;
            const size_t len = n - bom;
            const size_t older = std::strlen(rows::kFormat1End);
            if (too_long || !(rows::equals(line, len, rows::kFormat2) ||
                              rows::equals(line, len, rows::kFormat3) ||
                              rows::equals(line, len, rows::kFormat4) ||
                              (len >= older &&
                               std::memcmp(line + len - older,
                                           rows::kFormat1End, older) == 0))) {
                b.why = "its events.log isn't a recording's";
                return b;
            }
            continue;
        }
        rows::event_line e;
        if (!piece && rows::parse_event_line(p, n, &e)) {
            last_ms = (std::max)(last_ms, e.ms);
            b.has_length = true;
        }
    }
    const ecuio_recording_set::header h =
        ecuio_recording_set::read_header(folder);
    b.read = true;
    b.started_us = h.utc_us;
    b.profile = h.profile;
    b.uri = h.uri;
    b.length_us = last_ms * 1000;
    return b;
}

/* The baseline the options name, NULL or "" none */
baseline_facts baseline_of(const ecuio_health_options_t& o)
{
    if (o.baseline_folder_utf8 == nullptr || o.baseline_folder_utf8[0] == 0) {
        return {};
    }
    return read_baseline(o.baseline_folder_utf8);
}

/* Rows read between two asks of the caller's cancel */
constexpr uint32_t kCancelRows = 1024;

/* The caller's cancel: asked now, or once kCancelRows rows have gone by;
 * once it says stop, every ask says so */
struct canceler {
    ecuio_cancel_t cancel = nullptr;
    void* user = nullptr;
    bool stopped = false;
    uint32_t unasked = 0;

    bool now()
    {
        if (!stopped && cancel != nullptr && cancel(user) != 0) {
            stopped = true;
        }
        return stopped;
    }

    bool row()
    {
        if (stopped || ++unasked >= kCancelRows) {
            unasked = 0;
            return now();
        }
        return false;
    }
};

/* The reader's open asks the same cancel (ecuio_open_progress_t) */
int open_asked(uint64_t, uint64_t, void* user)
{
    return static_cast<canceler*>(user)->now() ? 1 : 0;
}

bool canceled(ecuio_status_t* status)
{
    *status = ECUIO_ERR_CANCELED;
    ecuio_detail_set("the report was canceled");
    return false;
}

/* A recording's report: each ECU's reports from the replies in frames.csv
 * that end in the range (health_replies.h), and each of the set's number
 * defs the value in force at the range's end, aged from it: signals.csv's,
 * else, with frames.csv and a set, the values its frames decode to
 * (ecuio_recording_open_frames). Every identification reply's values go
 * to the check, in the range or not. False with the detail set; *status
 * the failure's. */
bool from_folder(const ecuio_health_options_t& o, ecuio_health_report_t& r,
                 canceler& c, ecuio_status_t* status)
{
    namespace fr = ecuio_frame_reassembly;
    namespace rs = ecuio_recording_set;
    *status = ECUIO_ERR_LOAD_FAILED;
    const std::string folder = o.folder_utf8;
    const bool has_frames = files::exists(folder + "/frames.csv");
    const bool has_values = files::exists(folder + "/signals.csv");
    if (!has_frames && !has_values) {
        return refused("the folder has no frames.csv or signals.csv");
    }
    const rs::header header = rs::read_header(folder);
    rs::set_of set_of;
    const ecuio_signal_set_t* set = set_of.load(folder, header);
    const ecuio_signal_set_t* obd2 = nullptr;
    (void)ecuio_profile_lookup("obd2", &obd2);

    const auto in_range = [&o](uint64_t host_us) {
        return host_us >= o.from_us && (o.to_us == 0 || host_us <= o.to_us);
    };
    ecuio_health_replies::builder replies(obd2);
    std::set<std::string> ident_values;
    uint64_t end_us = 0;
    const auto take = [&](uint32_t id, bool ext, const uint8_t* data, size_t n,
                          uint64_t host_us) {
        if (scrub::identification(data, n)) {
            std::vector<std::string> v;
            scrub::identification_values(data, n, v);
            ident_values.insert(v.begin(), v.end());
        }
        if (in_range(host_us)) {
            replies.take(id, ext, data, n);
        }
    };
    if (has_frames) {
        if (c.now()) {
            return canceled(status);
        }
        ecuio_frame_rows::frame_rows rows;
        if (!rows.open(folder)) {
            return false;
        }
        fr::reassembly iso;
        std::vector<fr::message_end> ends;
        uint64_t index = 0;
        int got = 0;
        while ((got = rows.next()) == 1) {
            if (c.row()) {
                return canceled(status);
            }
            const ecuio_frame_t& f = rows.frame();
            const uint64_t host_us = rows.host_us();
            end_us = host_us;
            const bool ext = (f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
            const bool damaged = (f.flags & (ECUIO_FRAME_FLAG_ERROR |
                                             ECUIO_FRAME_FLAG_TRUNCATED)) != 0;
            /* A message whole: REASSEMBLED, or in a recording older than
             * the flag any row not on the wire (the reader decodes such a
             * row whole too). builder::take lists an ECU only on a reply
             * ID (scrub::reply_id), so another ID's frame that opens like
             * a reply never becomes one; its identity values still go to
             * the check. */
            if (!damaged && f.data_length > 0 &&
                ((f.flags & ECUIO_FRAME_FLAG_REASSEMBLED) != 0 ||
                 !fr::reassembly::on_the_wire(f))) {
                take(f.arbitration_id, ext, f.data, f.data_length, host_us);
            } else if (fr::reassembly::on_the_wire(f)) {
                ends.clear();
                (void)iso.take(fr::seg{index, host_us}, f, ends);
                for (const fr::message_end& e : ends) {
                    if (e.whole) {
                        take(e.id, e.extended, e.data.data(), e.data.size(),
                             e.frames.back().host_us);
                    }
                }
            }
            ++index;
        }
        if (got < 0) {
            return false;
        }
    }
    std::vector<ecuio_channel_diagnostics_t> diags;
    std::vector<ecuio_channel_identity_t> ids;
    replies.finish(diags, ids);

    std::unique_ptr<ecuio_recording_t, recording_closer> rec;
    if (has_values || set != nullptr) {
        // The open's progress asks at its start, as it reads and at its end
        ecuio_recording_t* opened = nullptr;
        const ecuio_status_t st =
            has_values
                ? ecuio_recording_open(folder.c_str(), open_asked, &c, &opened)
                : ecuio_recording_open_frames_with(folder.c_str(), set,
                                                   set_of.file(), open_asked,
                                                   &c, &opened);
        if (st == ECUIO_ERR_CANCELED && c.stopped) {
            return canceled(status);
        }
        if (st != ECUIO_OK) {
            *status = st;
            return false;
        }
        rec.reset(opened);
        ecuio_recording_info_t info{};
        if (ecuio_recording_get_info(rec.get(), &info) == ECUIO_OK) {
            end_us = (std::max)(end_us, info.duration_us);
        }
    }
    if (o.from_us > end_us) {
        /* A range wholly past the end, refused as replay refuses a
         * stretch with no frames */
        char from[24];
        char end[24];
        ecuio_stretch::seconds_text(o.from_us, from, sizeof from);
        ecuio_stretch::seconds_text(end_us, end, sizeof end);
        ecuio_detail_set(
            "the range starts at %s s, after the recording ends at %s s", from,
            end);
        return false;
    }
    const uint64_t to_us = o.to_us != 0 ? (std::min)(o.to_us, end_us) : end_us;

    /* The value in force at to_us of each number def the recording holds
     * a signal of: by its key where the signals have one, else its name */
    std::vector<ecuio_health_value_t> values;
    if (rec && set != nullptr) {
        ecuio_recording_info_t info{};
        (void)ecuio_recording_get_info(rec.get(), &info);
        const bool keyed = info.format == ECUIO_RECORDING_FORMAT_KEYS ||
                           (info.flags & ECUIO_RECORDING_INFO_FRAMES) != 0;
        const std::vector<std::string> row_ids =
            rs::def_ids(set, set_of.file(), keyed);
        std::unordered_map<std::string, uint32_t> by_name;
        for (uint32_t i = 0; i < info.signal_count; ++i) {
            ecuio_recording_signal_t sig{};
            if (ecuio_recording_get_signal(rec.get(), i, &sig) == ECUIO_OK &&
                (sig.flags & ECUIO_RECORDING_SIGNAL_TEXT) == 0) {
                by_name.emplace(
                    keyed
                        ? std::string(sig.key, strnlen(sig.key, sizeof sig.key))
                        : std::string(sig.name,
                                      strnlen(sig.name, sizeof sig.name)),
                    i);
            }
        }
        /* a whole row's room: a value text_at cut to a buffer would read
         * as another number */
        std::vector<char> text(ecuio_recording_rows::kRowMax + 1);
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const auto it = by_name.find(row_ids[i]);
            if (!numeric(d) || it == by_name.end()) {
                continue;
            }
            text[0] = 0;
            uint64_t at = ECUIO_RECORDING_NONE;
            const ecuio_status_t st = ecuio_recording_text_at(
                rec.get(), it->second, to_us, text.data(),
                static_cast<uint32_t>(text.size()), &at);
            if (st != ECUIO_OK) {
                *status = st;
                return false;
            }
            double v = 0;
            if (at == ECUIO_RECORDING_NONE || at < o.from_us ||
                !ecuio_recording_rows::parse_number(
                    text.data(), std::strlen(text.data()), &v)) {
                continue;
            }
            ecuio_health_value_t hv{};
            hv.def_id = d.id;
            hv.value = v;
            hv.age_us = to_us - at;
            values.push_back(hv);
        }
    }

    inputs in;
    in.recording = true;
    in.frames = has_frames;
    in.uri = header.uri;
    in.profile = header.profile;
    in.started_us = header.utc_us;
    in.from_us = o.from_us;
    in.to_us = to_us;
    in.set = set;
    in.diagnostics = diags.data();
    in.identity = ids.data();
    in.channels = static_cast<uint32_t>(diags.size());
    in.values = values.data();
    in.value_count = static_cast<uint32_t>(values.size());
    in.units = o.units;
    in.latest_model_year = o.latest_model_year;
    in.utc_us = o.utc_us;
    in.baseline = baseline_of(o);
    scrub::values identity;
    for (const std::string& v : ident_values) {
        (void)identity.add(v.data(), v.size(),
                           ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION);
    }
    const std::string bytes = write(in, o.format, &r, &identity);
    *status = ECUIO_ERR_LOAD_FAILED;
    return put(o.out_path_utf8, bytes, identity, r);
}

} // namespace

bool put(const std::string& out, const std::string& bytes,
         const ecuio_scrub::values& identity, ecuio_health_report_t& r)
{
    ecuio_scrub::check check(identity);
    check.feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    const uint32_t found = check.finish();
    if (found != 0) {
        r.failed_class = found;
        ecuio_detail_set("the check found %s in the report: nothing was "
                         "written",
                         ecuio_scrub::failed_words(found));
        return false;
    }
    if (files::exists(out)) {
        return refused("the file exists already");
    }
    std::string partial;
    std::string why;
    std::unique_ptr<FILE, file_closer> f(
        files::partial_file(out, &partial, &why));
    if (!f) {
        return refused("the report can't be written: %s", why);
    }
    const bool wrote =
        std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size();
    const int err = errno;
    if (std::fclose(f.release()) != 0 || !wrote) {
        files::remove_file(partial);
        return refused("the report can't be written: %s",
                       files::errno_words(err != 0 ? err : EIO));
    }
    if (put_test_before_rename != nullptr) {
        put_test_before_rename(out);
    }
    if (!files::rename_to(partial, out, false, &why)) {
        files::remove_file(partial);
        return refused("the report can't be put in place: %s", why);
    }
    return true;
}

} // namespace ecuio_health

extern "C" ecuio_status_t
ecuio_health_export(const ecuio_health_options_t* options,
                    ecuio_health_report_t* report)
{
    return ecuio_health_export_with(options, nullptr, nullptr, report);
}

extern "C" ecuio_status_t
ecuio_health_export_with(const ecuio_health_options_t* options,
                         ecuio_cancel_t cancel, void* user_data,
                         ecuio_health_report_t* report)
{
    if (report != nullptr) {
        *report = ecuio_health_report_t{};
    }
    if (options == nullptr || options->out_path_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const ecuio_health_options_t& o = *options;
    bool reserved = o.reserved0 != 0;
    for (uint64_t v : o.reserved) {
        reserved = reserved || v != 0;
    }
    if ((o.format != ECUIO_HEALTH_JSON && o.format != ECUIO_HEALTH_TEXT) ||
        o.flags != 0 || reserved ||
        (o.units != ECUIO_UNITS_METRIC && o.units != ECUIO_UNITS_IMPERIAL) ||
        ((o.session != nullptr) == (o.folder_utf8 != nullptr)) ||
        (o.values == nullptr && o.value_count != 0) ||
        (o.folder_utf8 != nullptr && o.value_count != 0) ||
        (o.to_us != 0 && o.to_us < o.from_us) ||
        (o.session != nullptr && (o.from_us != 0 || o.to_us != 0))) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < o.value_count; ++i) {
        if (o.values[i].flags != 0 || o.values[i].reserved != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    ecuio_detail_clear();
    ecuio_health_report_t r{};
    bool ok = false;
    ecuio_status_t failed = ECUIO_ERR_LOAD_FAILED;
    /* a report not written counts nothing written; a failed check keeps
     * its class */
    const auto outcome = [&]() {
        if (!ok) {
            const uint32_t failed_class = r.failed_class;
            r = ecuio_health_report_t{};
            r.failed_class = failed_class;
        }
        if (report != nullptr) {
            *report = r;
        }
        return ok ? ECUIO_OK : failed;
    };
    try {
        if (o.folder_utf8 != nullptr) {
            ecuio_health::canceler c;
            c.cancel = cancel;
            c.user = user_data;
            ok = ecuio_health::from_folder(o, r, c, &failed);
            return outcome();
        }
        ecuio_session_info_t info{};
        if (ecuio_session_get_info(o.session, &info) != ECUIO_OK) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        const uint32_t channels = info.module_count;
        std::vector<ecuio_channel_diagnostics_t> diags(channels);
        std::vector<ecuio_channel_identity_t> ids(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            (void)ecuio_session_get_channel_diagnostics(o.session, c,
                                                        &diags[c]);
            (void)ecuio_session_get_channel_identity(o.session, c, &ids[c]);
        }
        ecuio_health::inputs in;
        in.info = &info;
        in.uri = o.session->uri;
        in.set = ecuio_session_signal_set(o.session);
        in.diagnostics = diags.data();
        in.identity = ids.data();
        in.channels = channels;
        in.values = o.values;
        in.value_count = o.value_count;
        in.units = o.units;
        in.latest_model_year = o.latest_model_year;
        in.utc_us = o.utc_us;
        in.baseline = ecuio_health::baseline_of(o);
        ecuio_scrub::values identity;
        const std::string bytes =
            ecuio_health::write(in, o.format, &r, &identity);
        ok = ecuio_health::put(o.out_path_utf8, bytes, identity, r);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return outcome();
}
