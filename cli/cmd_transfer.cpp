// File/layer: cli/cmd_transfer.cpp | havne-cli, frames, and what export and import
//             ask of core (ADR-0054, ADR-0049)
// Testing: UNIT: CliGoldenTest, CliTransferRunTest (ctest);
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

} // namespace cmd
