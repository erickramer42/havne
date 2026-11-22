// File/layer: cli/cmd_transfer.cpp | havne-cli, frames, and what export and import
//             ask of core (ADR-0054, ADR-0049)
// Testing: UNIT: CliGoldenTest, CliTransferRunTest, CliUsageTest (ctest);
//          CliTransferTest, CliFramesTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "frames_lines.h"
#include "transfer_lines.h"
#include "jsonl.h"

#include "commands.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/recording.h"

namespace cmd {

// The CSV's column map guessed by core (ecuio_import_csv_guess): its whole
// text, asked its size first, then read; a file grown between the two
// calls is read again, a few times at most. A failure's words in *why
ecuio_status_t guess_map(const cli::ImportArgs& im, std::string* text,
                         ecuio_csv_guess_t* guess, std::string* why)
{
    size_t needed = 0;
    ecuio_status_t rc =
        ecuio_import_csv_guess(im.in, im.time_unit, nullptr, 0, &needed, guess);
    for (int tries = 0; rc == ECUIO_OK && tries < 4; ++tries) {
        std::vector<char> buf(needed);
        size_t now = 0;
        rc = ecuio_import_csv_guess(im.in, im.time_unit, buf.data(), buf.size(),
                                    &now, guess);
        if (rc == ECUIO_OK && now <= buf.size()) {
            text->assign(buf.data(), now - 1);
            return ECUIO_OK;
        }
        needed = now;
    }
    if (rc == ECUIO_OK) {
        *why = "the file changed while it was read";
        return ECUIO_ERR_LOAD_FAILED;
    }
    *why = ecuio_last_error_detail();
    return rc;
}

// `import csv <file> --print-map`: the guessed map's JSON alone on stdout
// (core's slice, byte for byte, and a line end), to save and pass with
// --map; no lines. A refusal is stderr's, exit 3; a map whose time has no
// unit is printed with a stderr line saying so, for its unit to be added
int print_guessed_map(const cli::ImportArgs& im)
{
    std::string text;
    std::string why;
    ecuio_csv_guess_t guess{};
    if (guess_map(im, &text, &guess, &why) != ECUIO_OK) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return kExitConnect;
    }
    const std::string map = text.substr(guess.map_offset, guess.map_length);
    if (std::fwrite(map.data(), 1, map.size(), stdout) != map.size() ||
        std::fputc('\n', stdout) == EOF || std::fflush(stdout) != 0) {
        return 1;
    }
    if ((guess.flags & ECUIO_CSV_GUESS_TIME_UNIT_KNOWN) == 0) {
        std::fprintf(stderr, "%s",
                     (guess.flags & ECUIO_CSV_GUESS_TIME_NUMBERS) != 0
                         ? "havne-cli: the map's time has no unit: add one, "
                           "or give --time-unit s, ms or us\n"
                         : "havne-cli: the map's time has no unit: its "
                           "cells need one with the zone (or the date and "
                           "offset)\n");
    }
    return kExitOk;
}

// The named columns the recording has no values for, by the reader's
// signal list (ecuio_recording_open lists the signals with a sample; a
// pick the recording never answered isn't among them). False when an
// interrupt stopped the open; a recording the reader refuses has none
bool missing_columns(const cli::ExportArgs& ex, bool* asked,
                     std::vector<const char*>* missing)
{
    missing->clear();
    ecuio_recording_t* r = nullptr;
    const ecuio_status_t opened =
        ecuio_recording_open(ex.folder, open_interrupted, asked, &r);
    if (opened == ECUIO_ERR_CANCELED) {
        return false;
    }
    if (opened != ECUIO_OK || r == nullptr) {
        return true;
    }
    std::vector<std::string> recorded;
    ecuio_recording_info_t info;
    std::memset(&info, 0, sizeof info);
    if (ecuio_recording_get_info(r, &info) == ECUIO_OK) {
        for (uint32_t i = 0; i < info.signal_count; ++i) {
            ecuio_recording_signal_t s;
            std::memset(&s, 0, sizeof s);
            if (ecuio_recording_get_signal(r, i, &s) == ECUIO_OK) {
                recorded.emplace_back(s.name, strnlen(s.name, sizeof s.name));
                recorded.emplace_back(s.key, strnlen(s.key, sizeof s.key));
            }
        }
    }
    ecuio_recording_close(r);
    *missing = cli::missing_names(ex.signals, recorded);
    return true;
}

namespace {

// A recording's frames read back as it recorded them (ADR-0049's
// appendix "frames read back"): a frame line each, written from core's
// callback on this thread, then the end line. Ctrl+C or a failed write
// stops the read; a refused row is an error line, exit 3, the rows
// before it already out.
struct FramesOut {
    Output* out = nullptr;
    char buf[kLineBytes];
};

int on_frame_row(void* user, const ecuio_frame_t* f, uint64_t host_us)
{
    auto* w = static_cast<FramesOut*>(user);
    cli::Line l(w->buf, sizeof w->buf);
    cli::frame_row_line(l, *f, host_us);
    write_line(*w->out, l);
    return w->out->failed || g_interrupted.load() ? 1 : 0;
}

} // namespace

int run_frames(const cli::FramesArgs& args)
{
    Output out;
    write_start(out);
    install_handlers();
    auto w = std::make_unique<FramesOut>();
    w->out = &out;
    ecuio_frames_read_t summary{};
    const ecuio_status_t rc = ecuio_recording_read_frames(
        args.folder, args.from_us, args.to_us, on_frame_row, w.get(), &summary);
    if (out.failed) {
        return ended(out, kExitOk); /* a closed pipe stays quiet */
    }
    if (rc != ECUIO_OK && rc != ECUIO_ERR_CANCELED) {
        write_error(out, rc);
        return ended(out, kExitConnect);
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::frames_end_line(l, rc == ECUIO_OK ? "read" : "interrupted", summary);
    write_line(out, l);
    return ended(out, kExitOk);
}

// frames' arguments, then run_frames
int cmd_frames(int argc, char** argv)
{
    cli::FramesArgs fr;
    std::string why;
    if (!cli::parse_frames(argc, argv, fr, &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    return run_frames(fr);
}

namespace {

// Each named column the recording has no values for, said on stderr; their
// count, for the result line
size_t say_missing(const std::vector<const char*>& missing)
{
    for (const char* name : missing) {
        std::fprintf(stderr,
                     "havne-cli: the recording has no values for "
                     "'%s': its column is empty\n",
                     name);
    }
    return missing.size();
}

// --estimate: the wide CSV's size at each interval, no file written
int export_estimate(Output& out, const cli::ExportArgs& ex, bool* asked,
                    const std::vector<const char*>& missing)
{
    const std::vector<uint32_t> shown = cli::estimate_intervals(ex);
    const std::vector<uint32_t> rows = cli::with_sweep(shown);
    std::vector<ecuio_export_estimate_t> sized(rows.size());
    const ecuio_status_t sizing = ecuio_export_estimate_with(
        &ex.options, rows.data(), static_cast<uint32_t>(rows.size()),
        export_interrupted, asked, sized.data());
    second_interrupt_over("estimate");
    if (sizing == ECUIO_ERR_CANCELED) {
        return ended_interrupted(out);
    }
    if (sizing != ECUIO_OK) {
        write_error(out, sizing);
        return ended(out, kExitConnect);
    }
    BigLine sizes(1024);
    cli::estimate_line(
        sizes.line, rows.data(), sized.data(), shown.size(),
        say_missing(missing),
        cli::sweep_rows_of(rows.data(), sized.data(), sized.size()));
    write_line(out, sizes.line);
    return ended(out, kExitOk);
}

} // namespace

// export: its arguments, the column check, then the estimate or the file
int cmd_export(int argc, char** argv)
{
    cli::ExportArgs ex;
    std::string why;
    if (!cli::parse_export(argc, argv, ex, &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    // A named column the recording has no values for stays empty
    // (a focus pick that never answered is a column of a log): each is
    // said on stderr once the export is done, so a typo isn't silent,
    // and counted. The check runs first, so an interrupt during it
    // leaves nothing written
    std::vector<const char*> missing;
    Output out;
    install_handlers(true);
    write_start(out);
    out.flush(); // a second interrupt leaves no half line behind
    bool asked = false;
    if (ex.options.format == ECUIO_EXPORT_CSV &&
        ex.options.layout == ECUIO_CSV_WIDE && !ex.signals.empty() &&
        !missing_columns(ex, &asked, &missing)) {
        return ended_interrupted(out, "column check");
    }
    if (ex.estimate) {
        return export_estimate(out, ex, &asked, missing);
    }
    // An interval is judged against the sweep's rows, sized first so an
    // interrupt here leaves nothing written: one pass more
    uint64_t sweep_rows = UINT64_MAX;
    if (ex.options.format == ECUIO_EXPORT_CSV &&
        ex.options.layout == ECUIO_CSV_WIDE && ex.options.row_ms > 0) {
        const uint32_t sweep = 0;
        ecuio_export_estimate_t sized{};
        const ecuio_status_t sizing = ecuio_export_estimate_with(
            &ex.options, &sweep, 1, interval_interrupted, &asked, &sized);
        if (sizing == ECUIO_ERR_CANCELED) {
            return ended_interrupted(out);
        }
        if (sizing == ECUIO_OK) {
            sweep_rows = sized.rows;
        }
    }
    ecuio_export_report_t report{};
    const ecuio_status_t rc =
        ecuio_export_with(&ex.options, export_interrupted, &asked, &report);
    second_interrupt_over("export");
    if (rc == ECUIO_ERR_CANCELED) {
        return ended_interrupted(out);
    }
    BigLine b(cli::exported_line_bytes(ex));
    cli::Line& l = b.line;
    if (rc != ECUIO_OK) {
        if (report.failed_class != 0) {
            cli::export_failed_line(l, report);
            write_line(out, l);
        }
        write_error(out, rc);
        return ended(out, kExitConnect);
    }
    const bool no_fewer =
        cli::no_fewer_than_sweep(ex.options.row_ms, report.rows, sweep_rows);
    cli::exported_line(l, ex, report, say_missing(missing), no_fewer);
    write_line(out, l);
    if (no_fewer) {
        std::fprintf(stderr,
                     "havne-cli: every %u ms gives no fewer rows than "
                     "every sweep: the recording is slower than that\n",
                     static_cast<unsigned>(ex.options.row_ms));
    }
    return ended(out, kExitOk);
}

namespace {

// A CSV's column map: core takes its text, up to the JSON reader's
// 1 MiB, and refuses a larger one in its own words. -1 to go on, else the
// exit code; im's options point into map
int take_column_map(Output& out, cli::ImportArgs& im, std::string* map)
{
    std::string why;
    if (im.options.format == ECUIO_IMPORT_CSV && im.map_path == nullptr) {
        // None given: guessed from the file, the guess written first,
        // so its notes come before an import's words
        std::string text;
        ecuio_csv_guess_t guess{};
        const ecuio_status_t grc = guess_map(im, &text, &guess, &why);
        if (grc != ECUIO_OK) {
            second_interrupt_over();
            write_error(out, grc, why.c_str());
            return ended(out, kExitConnect);
        }
        {
            BigLine g(cli::guessed_line_bytes(text));
            cli::guessed_line(g.line, text);
            write_line(out, g.line);
            out.flush();
        }
        if ((guess.flags & ECUIO_CSV_GUESS_TIME_UNIT_KNOWN) == 0) {
            // --time-unit gives a unit only to plain numbers
            second_interrupt_over();
            write_error(out, ECUIO_ERR_LOAD_FAILED,
                        (guess.flags & ECUIO_CSV_GUESS_TIME_NUMBERS) != 0
                            ? "the guessed map's time has no unit: give "
                              "--time-unit s, ms or us, or a map with "
                              "--map"
                            : "the guessed map's time has no unit a "
                              "guess can give: its cells need a map "
                              "with the zone (or the date and offset), "
                              "given with --map");
            return ended(out, kExitConnect);
        }
        *map = text.substr(guess.map_offset, guess.map_length);
        im.options.column_map_utf8 = map->c_str();
    } else if (im.map_path != nullptr) {
        if (!read_whole(im.map_path, size_t{1} << 20, "column map", map,
                        &why)) {
            second_interrupt_over();
            write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
            return ended(out, kExitConnect);
        }
        im.options.column_map_utf8 = map->c_str();
    }
    return -1;
}

} // namespace

// import: its arguments, the CSV's column map, then the recording
int cmd_import(int argc, char** argv)
{
    cli::ImportArgs im;
    std::string why;
    if (!cli::parse_import(argc, argv, im, &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    if (im.print_map) {
        return print_guessed_map(im);
    }
    Output out;
    // Before the guess, so a Ctrl+C during it stops the import at its
    // first ask
    install_handlers(true);
    write_start(out);
    out.flush(); // a second interrupt leaves no half line behind
    std::string map;
    const int mapped = take_column_map(out, im, &map);
    if (mapped >= 0) {
        return mapped;
    }
    ecuio_import_report_t report{};
    bool asked = false;
    const ecuio_status_t rc =
        ecuio_import_with(&im.options, import_interrupted, &asked, &report);
    second_interrupt_over("import");
    if (rc == ECUIO_ERR_CANCELED) {
        return ended_interrupted(out);
    }
    if (rc != ECUIO_OK) {
        write_error(out, rc);
        return ended(out, kExitConnect);
    }
    BigLine b(cli::imported_line_bytes(im));
    cli::Line& l = b.line;
    cli::imported_line(l, im, report);
    write_line(out, l);
    if (report.rows_no_time > 0) {
        // Left out and counted, never silent
        std::fprintf(stderr, "havne-cli: %llu %s with no time left out\n",
                     static_cast<unsigned long long>(report.rows_no_time),
                     report.rows_no_time == 1 ? "row" : "rows");
    }
    return ended(out, kExitOk);
}

} // namespace cmd
