// File/layer: core/src/export_folder.cpp | C++17, Qt-free; the folder export's copy
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)

#include "export_engine.h"

#include <new>

namespace ecuio_export_detail {

void recording_export::count_unknown(const std::string& folder)
{
    static const char* const kKnown[] = {
        "events.log",  "frames.csv",   "signals.csv",    "markers.csv",
        "latency.csv", "signals.json", "conditions.json"};
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

std::string recording_export::export_line() const
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

bool recording_export::begin_file(const std::string& partial, const char* name)
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

bool recording_export::end_file(const char* name)
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

bool recording_export::put_lines(const std::vector<std::string>& lines)
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

bool recording_export::write_frames(const std::string& folder)
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

bool recording_export::copy_file(const std::string& folder, const char* name)
{
    std::string why;
    file_ptr f(files::open_file(folder + "/" + name, false, &why));
    if (!f) {
        return true;
    }
    if (!begin_file(partial_, name)) {
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
    return end_file(name);
}

bool recording_export::write_folder(const std::string& folder)
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
    return copy_file(folder, rows::kSignalFileName) &&
           copy_file(folder, rows::kConditionsName);
}

bool recording_export::run_folder(const std::string& folder, const std::string& out)
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

void recording_export::remove_partial()
{
    file_.reset();
    for (const std::string& path : written_paths_) {
        files::remove_file(path);
    }
    files::remove_folder(partial_);
    r_.files = 0;
}

} // namespace ecuio_export_detail
