// File/layer: cli/cmd_info.cpp | havne-cli, info (ADR-0049 section 6)
// Testing: UNIT: CliGoldenTest, CliUsageTest (ctest); CliInfoTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "info_lines.h"
#include "view_lines.h"
#include "jsonl.h"

#include "commands.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/recording.h"

namespace cmd {

namespace {

// The check's findings, kept until it ends: an error line then stands
// alone, as a failed open's does
struct Findings {
    std::vector<ecuio_check_finding_t> list;
    bool out_of_memory = false;
};

int keep_finding(const ecuio_check_finding_t* f, void* user)
{
    auto* found = static_cast<Findings*>(user);
    try {
        found->list.push_back(*f);
    } catch (const std::bad_alloc&) {
        found->out_of_memory = true;
        return 1;
    }
    return 0;
}

} // namespace

// A recording checked offline (ADR-0049 section 6): the frames check runs
// whatever the reader makes of the folder (one with frames.csv alone
// still has its frames checked), then the lines: what the reader found,
// each finding, the summary. A failed check is an error line, exit 3.
int run_info(const InfoArgs& args)
{
    Output out;
    std::unique_ptr<ecuio_recording_t, RecordingCloser> r;
    ecuio_recording_t* opened = nullptr;
    const ecuio_status_t open_rc =
        ecuio_recording_open(args.folder, nullptr, nullptr, &opened);
    r.reset(opened);
    const std::string open_detail = ecuio_last_error_detail();

    Findings found;
    ecuio_check_summary_t summary{};
    const ecuio_status_t rc = ecuio_recording_check(
        args.folder, &args.options, nullptr, keep_finding, &found, &summary);
    if (rc != ECUIO_OK) {
        write_start(out);
        if (found.out_of_memory) {
            write_error(out, ECUIO_ERR_OUT_OF_MEMORY,
                        "the findings didn't fit in memory");
        } else {
            write_error(out, rc);
        }
        return ended(out, kExitConnect);
    }

    write_start(out);
    char buf[2048];
    if (open_rc == ECUIO_OK) {
        ecuio_recording_info_t info{};
        ecuio_recording_get_info(r.get(), &info);
        ecuio_recording_signals_t signal_file{};
        ecuio_recording_get_signals(r.get(), &signal_file);
        ecuio_recording_conditions_t conditions{};
        ecuio_recording_get_conditions(r.get(), &conditions);
        cli::Line l(buf, sizeof buf);
        cli::opened_line(l, 0, info, signal_file, conditions);
        write_line(out, l);
        for (uint32_t k = 0; k < info.signal_count; ++k) {
            ecuio_recording_signal_t s{};
            ecuio_recording_get_signal(r.get(), k, &s);
            write_signal_line(out, r.get(), 0, k, s);
        }
        for (uint32_t k = 0; k < info.marker_count; ++k) {
            ecuio_recording_marker_t m{};
            ecuio_recording_get_marker(r.get(), k, &m);
            BigLine b(256 + static_cast<size_t>(m.label_length) * 6);
            cli::marker_line(b.line, 0, k, m);
            write_line(out, b.line);
        }
        write_condition_lines(out, r.get(), 0, info);
    } else {
        BigLine b(1024 + open_detail.size() * 6);
        cli::open_failed_line(b.line, 0, open_rc, open_detail.c_str());
        write_line(out, b.line);
    }
    for (const ecuio_check_finding_t& f : found.list) {
        cli::Line l(buf, sizeof buf);
        cli::finding_line(l, f);
        write_line(out, l);
    }
    cli::Line l(buf, sizeof buf);
    cli::summary_line(l, summary, args.options);
    write_line(out, l);
    return ended(out, kExitOk);
}

// info's arguments read and checked, then run_info
int cmd_info(int argc, char** argv)
{
    InfoArgs info;
    info.options.gap_us = ECUIO_CHECK_DEFAULT_GAP_US;
    info.options.rate_drop_x100 = ECUIO_CHECK_DEFAULT_RATE_DROP_X100;
    info.options.settle_us = ECUIO_CHECK_DEFAULT_SETTLE_US;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if ((a == "--gap" || a == "--settle") && has_value) {
            int64_t us = 0;
            if (!cli::parse_seconds(argv[++i], us) || us <= 0) {
                std::fprintf(stderr,
                             "havne-cli: %s takes seconds, more than 0\n",
                             a.c_str());
                return kExitUsage;
            }
            (a == "--gap" ? info.options.gap_us : info.options.settle_us) =
                static_cast<uint64_t>(us);
        } else if (a == "--rate-drop" && has_value) {
            if (!cli::parse_rate_drop(argv[++i], info.options.rate_drop_x100)) {
                std::fprintf(stderr, "havne-cli: --rate-drop takes a "
                                     "multiple of the median, more than "
                                     "1\n");
                return kExitUsage;
            }
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        } else if (info.folder != nullptr) {
            std::fprintf(stderr, "havne-cli: info takes one recording\n");
            return kExitUsage;
        } else {
            info.folder = argv[i];
        }
    }
    if (info.folder == nullptr) {
        std::fprintf(stderr, "havne-cli: info needs a recording's folder\n");
        usage(stderr);
        return kExitUsage;
    }
    return run_info(info);
}

} // namespace cmd
