// File/layer: core/src/export.cpp | C++17, Qt-free; a recording out to
//             another tool's format (ADR-0054 sections 1 and 3)
//             the engine class is export_engine.h's; its parts are in
//             export_pcapng.cpp, export_csv.cpp, export_wide.cpp,
//             export_events.cpp and export_folder.cpp
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)
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
// the CSV's classes; events.log line by line; markers.csv, latency.csv,
// signals.json and conditions.json as they are), each through its own
// file check, under a name of its own beside <out> (folder_files.h's
// partial_name) until every one passes. A range opens with a held cond
// line for each condition ON at its start (ADR-0063 section 8), and with
// the last focus change before it, so a copy says which signals were
// asked.
//
// The caller's cancel (ecuio_export_with) is asked as each pass and each
// file of a copy begins, and every kCancelRows rows; a stop is a failure
// like any other, so the partial goes the same way.

#include "export_engine.h"

#include <new>

namespace ecuio_export_detail {

bool fail(const char* fmt, const std::string& a)
{
    ecuio_detail_set(fmt, a.c_str());
    return false;
}

bool recording_export::run(const std::string& folder, const std::string& out,
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

bool recording_export::estimate(const std::string& folder, const uint32_t* row_ms,
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

bool recording_export::read_header(const std::string& folder)
{
    header_ = rs::read_header(folder);
    text_names_.insert(header_.text_names.begin(),
                       header_.text_names.end());
    return true;
}

bool recording_export::plan_frames(const std::string& folder, bool csv_out) const
{
    return !csv_out || (!raw_ && files::exists(folder + "/frames.csv"));
}

} // namespace ecuio_export_detail

namespace {

/* The most row intervals one ecuio_export_estimate call sizes: each one
 * keeps its own rows engine */
constexpr uint32_t kEstimateCountMax = 64;

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

using namespace ecuio_export_detail;

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
