// File/layer: cli/main.cpp | havne-cli, a command-line client over the C ABI (ADR-0031)
// Testing: UNIT: CliGoldenTest, CliUsageTest, CliConnectTest, CliSignalTest,
//          CliReplayTest, CliRecordTest, CliViewRunTest, CliPathTest,
//          CliTransferRunTest, CliExitTest, CliObdbImportTest, CliLogRunTest,
//          CliPluginMakeTest
//          (ctest);
//          CliTimingTest, CliVinTest, CliSimTest, CliMarkLinesTest,
//          CliViewTest, CliInfoTest, CliSignalFileTest, CliHealthTest,
//          CliPartialTest
//          (test_cli); BENCH
//
// Public headers only: what any program linking core can do.

#include "condition_lines.h"
#include "frames_lines.h"
#include "health_lines.h"
#include "info_lines.h"
#include "log_lines.h"
#include "transfer_lines.h"
#include "view_lines.h"
#include "jsonl.h"
#include "plugin_lines.h"
#include "elm327_lines.h"

#include "commands.h"
#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/obdb.h"
#include "ecuio/recording.h"

using namespace cmd;

namespace {

bool parse_count(const char* text, uint64_t& out);

// --seconds of stream, play and sim: a whole number up to the shared cap
bool parse_run_seconds(const char* text, uint64_t& out)
{
    return parse_count(text, out) && out <= cli::kMaxRunSeconds;
}

bool parse_count(const char* text, uint64_t& out)
{
    if (!text || !*text) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno || *end || v == 0 || *text == '-' || *text == '+') {
        return false;
    }
    out = v;
    return true;
}

int run(int argc, char** argv)
{
#ifdef _WIN32
    // JSON lines end in \n on every platform
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        usage(stderr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        usage(stdout);
        return kExitOk;
    }
    if (command == "--version") {
        std::printf("havne-cli %s\n", ecuio_version_string());
        return kExitOk;
    }
    if (command == "devices" && argc == 2) {
        return run_devices();
    }
    if (command == "vin" && argc == 3) {
        return run_vin(argv[2]);
    }
    if (command == "dtc" && argc == 3) {
        return run_dtc(argv[2]);
    }
    if (command == "view") {
        ViewArgs view;
        std::vector<const char*>
            levels; // --stretch-level, read once --units is
        bool imperial = false;
        bool has_units = false;
        bool has_pull_end = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const bool has_value = i + 1 < argc;
            if (a == "--signal" && has_value) {
                view.signals.emplace_back(argv[++i]);
            } else if ((a == "--from" || a == "--to" || a == "--at") &&
                       has_value) {
                int64_t us = 0;
                if (!cli::parse_seconds(argv[++i], us)) {
                    std::fprintf(stderr, "havne-cli: %s takes seconds\n",
                                 a.c_str());
                    return kExitUsage;
                }
                if (a == "--from") {
                    view.from_us = us;
                    view.has_from = true;
                } else if (a == "--to") {
                    view.to_us = us;
                    view.has_to = true;
                } else {
                    view.at_us = us;
                    view.has_at = true;
                }
            } else if (a == "--stretches") {
                view.stretches = true;
                uint32_t kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
                if (has_value && cli::parse_stretch_kinds(argv[i + 1], kinds)) {
                    ++i;
                }
                view.rules.kinds = kinds;
            } else if (a == "--stretch-level" && has_value) {
                levels.push_back(argv[++i]);
            } else if (a == "--pull-end" && has_value) {
                if (!cli::parse_pull_end(argv[++i], view.rules.flags)) {
                    std::fprintf(stderr, "havne-cli: --pull-end takes lift or "
                                         "top-speed\n");
                    return kExitUsage;
                }
                has_pull_end = true;
            } else if (a == "--role" && has_value) {
                uint32_t role = 0;
                std::string name;
                if (!cli::parse_role(argv[++i], role, name)) {
                    std::fprintf(stderr,
                                 "havne-cli: --role takes throttle=, load=, "
                                 "speed= or rpm= and a signal's name\n");
                    return kExitUsage;
                }
                view.roles.emplace_back(role, name);
            } else if (a == "--units" && has_value) {
                const std::string u = argv[++i];
                if (u != "metric" && u != "imperial") {
                    std::fprintf(
                        stderr,
                        "havne-cli: --units takes metric or imperial\n");
                    return kExitUsage;
                }
                imperial = u == "imperial";
                has_units = true;
            } else if (a == "--span" && has_value) {
                cli::Span span;
                if (!cli::parse_span(argv[++i], span)) {
                    std::fprintf(stderr, "havne-cli: --span takes A:B, two "
                                         "seconds with A before B\n");
                    return kExitUsage;
                }
                view.spans.push_back(span);
            } else if (a == "--columns" && has_value) {
                uint64_t n = 0;
                if (!parse_count(argv[++i], n) || n > 10000) {
                    std::fprintf(stderr,
                                 "havne-cli: --columns takes 1 to 10000\n");
                    return kExitUsage;
                }
                view.columns = static_cast<uint32_t>(n);
            } else if (a == "--align-on" && has_value) {
                view.align_text = argv[++i];
                if (!cli::parse_align_on(view.align_text, view.align)) {
                    std::fprintf(stderr,
                                 "havne-cli: --align-on takes "
                                 "rise:<signal>:<level>, "
                                 "fall:<signal>:<level>, marker:<label>, "
                                 "utc, engine-start or pull:<n>[,<n>...], "
                                 "pulls counted from 1\n");
                    return kExitUsage;
                }
            } else if (a == "--hysteresis" && has_value) {
                if (!cli::parse_decimal(argv[++i], view.hysteresis) ||
                    view.hysteresis < 0) {
                    std::fprintf(stderr,
                                 "havne-cli: --hysteresis takes 0 or more\n");
                    return kExitUsage;
                }
                view.has_hysteresis = true;
            } else if (a == "--frames") {
                view.frames = true;
            } else if (a == "--profile" && has_value) {
                view.profile = argv[++i];
            } else if (a == "--signals" && has_value) {
                view.signals_path = argv[++i];
            } else if (a == "--plugin" && has_value) {
                view.plugin_path = argv[++i];
            } else if (a.rfind("--", 0) == 0) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                view.folders.push_back(argv[i]);
            }
        }
        if (view.folders.empty()) {
            std::fprintf(stderr,
                         "havne-cli: view needs a recording's folder\n");
            usage(stderr);
            return kExitUsage;
        }
        const bool aligns_pulls = view.align.kind == cli::AlignOn::pull;
        if (!view.stretches && !aligns_pulls &&
            (!levels.empty() || !view.roles.empty() || has_units ||
             has_pull_end)) {
            std::fprintf(stderr, "havne-cli: --stretch-level, --role, "
                                 "--pull-end and --units go with --stretches "
                                 "or --align-on pull:\n");
            return kExitUsage;
        }
        if (aligns_pulls && view.align.pulls.size() != 1 &&
            view.align.pulls.size() != view.folders.size()) {
            std::fprintf(stderr,
                         "havne-cli: --align-on pull: takes one pull for "
                         "every recording, or one for each of the %u\n",
                         static_cast<unsigned>(view.folders.size()));
            return kExitUsage;
        }
        for (const char* level : levels) {
            if (!cli::parse_stretch_level(level, imperial, view.rules)) {
                std::fprintf(
                    stderr,
                    "havne-cli: --stretch-level takes <key>=<value>, 0 or "
                    "more: pull-throttle, pull-load, pull-hold, pull-gap, "
                    "pull-rise, pull-gain, cruise-hold, cruise-speed-band, "
                    "cruise-throttle-band or cruise-floor\n");
                return kExitUsage;
            }
        }
        if (view.has_hysteresis && view.align.kind != cli::AlignOn::rise &&
            view.align.kind != cli::AlignOn::fall) {
            std::fprintf(stderr, "havne-cli: --hysteresis goes with "
                                 "--align-on rise: or fall:\n");
            return kExitUsage;
        }
        if (view.has_from && view.has_to && view.from_us >= view.to_us) {
            std::fprintf(stderr, "havne-cli: --from must come before --to\n");
            return kExitUsage;
        }
        if (!view.frames &&
            (!view.profile.empty() || view.signals_path != nullptr ||
             view.plugin_path != nullptr)) {
            std::fprintf(stderr, "havne-cli: --profile, --signals and "
                                 "--plugin go with --frames\n");
            return kExitUsage;
        }
        if (view.signals_path != nullptr && view.plugin_path != nullptr) {
            std::fprintf(stderr, "havne-cli: --signals or --plugin, not "
                                 "both\n");
            return kExitUsage;
        }
        return run_view(view);
    }
    if (command == "info") {
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
                if (!cli::parse_rate_drop(argv[++i],
                                          info.options.rate_drop_x100)) {
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
            std::fprintf(stderr,
                         "havne-cli: info needs a recording's folder\n");
            usage(stderr);
            return kExitUsage;
        }
        return run_info(info);
    }
    if (command == "health") {
        cli::HealthArgs h;
        std::string why;
        if (!cli::parse_health(argc, argv, h, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        if (!h.live) {
            return run_health_recording(h);
        }
        const char* uri = h.source;
        std::string opened;
        if (h.plugin != nullptr) {
            const int rc = use_plugin(h.plugin, uri);
            if (rc >= 0) {
                return rc;
            }
        } else if (h.signals != nullptr) {
            const int rc = use_signal_file(h.signals, uri, &opened);
            if (rc >= 0) {
                return rc;
            }
            uri = opened.c_str();
        } else {
            const int rc = use_recording_signals(uri, &opened);
            if (rc >= 0) {
                return rc;
            }
            if (!opened.empty()) {
                uri = opened.c_str();
            }
        }
        return run_health_session(h, uri);
    }
    if (command == "frames") {
        cli::FramesArgs fr;
        std::string why;
        if (!cli::parse_frames(argc, argv, fr, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        return run_frames(fr);
    }
    if (command == "export") {
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
        const auto say_missing = [&missing] {
            for (const char* name : missing) {
                std::fprintf(stderr,
                             "havne-cli: the recording has no values for "
                             "'%s': its column is empty\n",
                             name);
            }
            return missing.size();
        };
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
            const std::vector<uint32_t> shown = cli::estimate_intervals(ex);
            const std::vector<uint32_t> rows = cli::with_sweep(shown);
            std::vector<ecuio_export_estimate_t> sized(rows.size());
            const ecuio_status_t sizing = ecuio_export_estimate_with(
                &ex.options, rows.data(), static_cast<uint32_t>(rows.size()),
                export_interrupted, &asked, sized.data());
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
                say_missing(),
                cli::sweep_rows_of(rows.data(), sized.data(), sized.size()));
            write_line(out, sizes.line);
            return ended(out, kExitOk);
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
        const bool no_fewer = cli::no_fewer_than_sweep(ex.options.row_ms,
                                                       report.rows, sweep_rows);
        cli::exported_line(l, ex, report, say_missing(), no_fewer);
        write_line(out, l);
        if (no_fewer) {
            std::fprintf(stderr,
                         "havne-cli: every %u ms gives no fewer rows than "
                         "every sweep: the recording is slower than that\n",
                         static_cast<unsigned>(ex.options.row_ms));
        }
        return ended(out, kExitOk);
    }
    if (command == "import") {
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
        // A CSV's column map: core takes its text, up to the JSON reader's
        // 1 MiB, and refuses a larger one in its own words
        std::string map;
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
            map = text.substr(guess.map_offset, guess.map_length);
            im.options.column_map_utf8 = map.c_str();
        } else if (im.map_path != nullptr) {
            if (!read_whole(im.map_path, size_t{1} << 20, "column map", &map,
                            &why)) {
                second_interrupt_over();
                write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
                return ended(out, kExitConnect);
            }
            im.options.column_map_utf8 = map.c_str();
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
    if (command == "play") {
#if HAVNE_CLI_PLAY
        PlayArgs play;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--to" && i + 1 < argc) {
                play.to = argv[++i];
            } else if ((a == "--count" || a == "--seconds") && i + 1 < argc) {
                const bool is_count = a == "--count";
                if (!(is_count ? parse_count(argv[++i], play.count)
                               : parse_run_seconds(argv[++i], play.seconds))) {
                    std::fprintf(stderr, "havne-cli: %s\n",
                                 is_count
                                     ? "--count takes a whole number above 0"
                                     : cli::run_seconds_why());
                    return kExitUsage;
                }
            } else if (a.rfind("--", 0) == 0 || play.uri) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                play.uri = argv[i];
            }
        }
        if (!play.uri || !play.to) {
            std::fprintf(stderr,
                         "havne-cli: play needs a URI and --to <interface>\n");
            usage(stderr);
            return kExitUsage;
        }
        return run_play(play);
#else
        std::fprintf(stderr, "havne-cli: play needs Linux, where a CAN "
                             "interface is a network device\n");
        return kExitUsage;
#endif
    }
    if (command == "clear") {
        const char* uri = nullptr;
        bool yes = false;
        bool running = false;
        bool moving = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--yes") {
                yes = true;
            } else if (a == "--engine-running") {
                running = true;
            } else if (a == "--moving") {
                moving = true;
            } else if (a.rfind("--", 0) == 0 || uri != nullptr) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                uri = argv[i];
            }
        }
        if (uri == nullptr) {
            std::fprintf(stderr, "havne-cli: clear needs a URI\n");
            usage(stderr);
            return kExitUsage;
        }
        if (!yes) {
            std::fprintf(stderr,
                         "havne-cli: clear erases the ECUs' trouble codes, "
                         "freeze frame and readiness: it needs --yes\n");
            return kExitUsage;
        }
        if (moving && !running) {
            std::fprintf(stderr, "havne-cli: --moving is a word on top of "
                                 "--engine-running's\n");
            return kExitUsage;
        }
        return run_clear_command(uri,
                                 (running ? ECUIO_CLEAR_ALLOW_RUNNING : 0u) |
                                     (moving ? ECUIO_CLEAR_ALLOW_MOVING : 0u));
    }
    if (command == "sim") {
        SimArgs sim;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--on" && i + 1 < argc) {
                sim.on = argv[++i];
            } else if (a == "--elm327" && i + 1 < argc) {
                sim.elm327 = argv[++i];
                if (!cli::parse_elm327_address(sim.elm327, &sim.elm327_at)) {
                    std::fprintf(stderr,
                                 "havne-cli: --elm327 takes tcp:<port> or "
                                 "tcp:<a.b.c.d>:<port>, a port 0-65535\n");
                    return kExitUsage;
                }
            } else if (a == "--desk-bus") {
                sim.desk_bus = true;
            } else if (a == "--signals" && i + 1 < argc) {
                sim.signals = argv[++i];
            } else if (a == "--plugin" && i + 1 < argc) {
                sim.plugin = argv[++i];
            } else if (a == "--seconds" && i + 1 < argc) {
                if (!parse_run_seconds(argv[++i], sim.seconds)) {
                    std::fprintf(stderr, "havne-cli: %s\n",
                                 cli::run_seconds_why());
                    return kExitUsage;
                }
            } else if (a.rfind("--", 0) == 0 || sim.uri) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                sim.uri = argv[i];
            }
        }
        if (!sim.uri || (sim.on == nullptr) == (sim.elm327 == nullptr)) {
            std::fprintf(stderr, "havne-cli: sim needs a URI and either --on "
                                 "<interface> or --elm327 tcp:<port>\n");
            usage(stderr);
            return kExitUsage;
        }
        if (sim.elm327 != nullptr && sim.desk_bus) {
            std::fprintf(stderr, "havne-cli: --desk-bus is for --on: the "
                                 "ELM327 emulator is on no bus\n");
            usage(stderr);
            return kExitUsage;
        }
        if (sim.signals != nullptr && sim.plugin != nullptr) {
            std::fprintf(stderr, "havne-cli: --signals and --plugin each "
                                 "give the profile: one at a time\n");
            usage(stderr);
            return kExitUsage;
        }
        if (sim.plugin != nullptr) {
            const int rc = use_plugin(sim.plugin, sim.uri);
            if (rc >= 0) {
                return rc;
            }
        }
        std::string opened;
        if (sim.signals != nullptr) {
            const int rc = use_signal_file(sim.signals, sim.uri, &opened);
            if (rc >= 0) {
                return rc;
            }
            sim.uri = opened.c_str();
        }
        return sim.elm327 != nullptr ? run_sim_elm327(sim) : run_sim(sim);
    }
    if (command == "plugins") {
        const std::string word = argc > 2 ? argv[2] : "";
        if (word == "list" && argc > 3) {
            std::vector<const char*> paths;
            for (int i = 3; i < argc; ++i) {
                if (argv[i][0] == '-' && argv[i][1] == '-') {
                    std::fprintf(stderr, "havne-cli: unexpected '%s'\n",
                                 argv[i]);
                    usage(stderr);
                    return kExitUsage;
                }
                paths.push_back(argv[i]);
            }
            return run_plugins_list(paths);
        }
        const bool package =
            argc > 3 && std::string(argv[3]).rfind("--", 0) != 0;
        if (word == "check" && package && (argc == 4 || argc == 6)) {
            const char* on = nullptr;
            if (argc == 6) {
                if (std::string(argv[4]) != "--on" || *argv[5] == '\0') {
                    std::fprintf(stderr, "havne-cli: plugins check takes "
                                         "<pkg> [--on <vcan>]\n");
                    usage(stderr);
                    return kExitUsage;
                }
                on = argv[5];
            }
            return run_plugins_check(argv[3], on);
        }
        if (word == "match" && package && argc == 6 &&
            std::string(argv[4]) == "--vin") {
            return run_plugins_match(argv[3], argv[5]);
        }
        const bool two =
            package && argc > 4 && std::string(argv[4]).rfind("--", 0) != 0;
        if ((word == "wrap" && package) || (word == "put" && two) ||
            (word == "drop" && package)) {
            PluginMakeArgs make;
            if (word == "wrap") {
                make.file = argv[3];
            } else {
                make.package = argv[3];
                make.file = word == "put" ? argv[4] : nullptr;
            }
            std::string why;
            if (!parse_plugin_make(word, word == "put" ? 5 : 4, argc, argv,
                                   &make, &why)) {
                std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
                usage(stderr);
                return kExitUsage;
            }
            return word == "wrap"  ? run_plugins_wrap(make)
                   : word == "put" ? run_plugins_put(make)
                                   : run_plugins_drop(make);
        }
        std::fprintf(stderr,
                     "havne-cli: plugins takes list <pkg>..., check <pkg> "
                     "[--on <vcan>], match <pkg> --vin <VIN>, wrap "
                     "<signal-file> --out <pkg>, put <pkg> <signal-file> "
                     "--out <pkg>, or drop <pkg> --variant <id> --out "
                     "<pkg>\n");
        usage(stderr);
        return kExitUsage;
    }
    if (command == "conditions") {
        cli::ConditionsCheckArgs check;
        std::string why = "conditions takes check <file> [--signals <file> | "
                          "--profile <name> | --plugin <pkg> [--variant <id>]]";
        if (argc > 2 && std::string(argv[2]) == "check" &&
            cli::parse_conditions_check(argc - 3, argv + 3, &check, &why)) {
            return run_conditions_check(check);
        }
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    if (command == "layout") {
        const std::string word = argc > 2 ? argv[2] : "";
        if (word == "check" && argc == 4) {
            return run_layout_check(argv[3], "");
        }
        if (word == "check" && argc == 6 && std::string(argv[4]) == "--form" &&
            (std::string(argv[5]) == "phone" ||
             std::string(argv[5]) == "desktop")) {
            return run_layout_check(argv[3], argv[5]);
        }
        usage(stderr);
        return kExitUsage;
    }
    if (command == "signals") {
        const std::string word = argc > 2 ? argv[2] : "";
        if (word == "check" && argc == 4) {
            return run_signals_check(argv[3]);
        }
        if (word == "import" && argc > 4 && std::string(argv[3]) == "obdb") {
            ImportArgs im;
            im.file = argv[4];
            for (int i = 5; i < argc; ++i) {
                const std::string a = argv[i];
                const bool has_value = i + 1 < argc;
                if (a == "--out" && has_value) {
                    im.out = argv[++i];
                } else if (a == "--year" && has_value) {
                    uint64_t year = 0;
                    if (!parse_count(argv[++i], year) || year < 1980 ||
                        year > 2100) {
                        std::fprintf(stderr, "havne-cli: --year takes a model "
                                             "year, 1980 to 2100\n");
                        return kExitUsage;
                    }
                    im.options.model_year = static_cast<uint32_t>(year);
                } else if (a == "--name" && has_value) {
                    im.options.name = argv[++i];
                } else if (a == "--url" && has_value) {
                    im.options.url = argv[++i];
                } else if (a == "--commit" && has_value) {
                    im.options.commit = argv[++i];
                } else if (a == "--alone") {
                    im.options.flags |= ECUIO_OBDB_ALONE;
                } else if (a == "--plugin") {
                    im.plugin = true;
                } else if (a == "--wmi" && has_value) {
                    if (!cli::parse_wmi(argv[++i], &im.wmi)) {
                        std::fprintf(stderr,
                                     "havne-cli: --wmi takes manufacturer "
                                     "codes, each 3 or 6 of a VIN's letters "
                                     "and digits, separated by commas\n");
                        return kExitUsage;
                    }
                } else if (a == "--years" && has_value) {
                    if (!cli::parse_model_years(argv[++i], im.years)) {
                        std::fprintf(stderr,
                                     "havne-cli: --years takes <from>-<to>, "
                                     "two model years from 1980 to 2039\n");
                        return kExitUsage;
                    }
                } else {
                    std::fprintf(stderr, "havne-cli: unexpected '%s'\n",
                                 argv[i]);
                    usage(stderr);
                    return kExitUsage;
                }
            }
            if (im.out == nullptr || *im.out == '\0') {
                std::fprintf(
                    stderr,
                    "havne-cli: signals import obdb needs --out <file>\n");
                usage(stderr);
                return kExitUsage;
            }
            if (!im.plugin && (!im.wmi.empty() || im.years[0] != 0)) {
                std::fprintf(stderr, "havne-cli: --wmi and --years go with "
                                     "--plugin: a match is a plugin's\n");
                usage(stderr);
                return kExitUsage;
            }
            if (im.years[0] != 0 && im.wmi.empty()) {
                std::fprintf(stderr, "havne-cli: --years goes with --wmi: a "
                                     "match needs its manufacturer codes\n");
                usage(stderr);
                return kExitUsage;
            }
            return run_signals_import(im);
        }
        if (word == "expr") {
            return run_signals_expr(argc - 3, argv + 3);
        }
        std::fprintf(stderr,
                     "havne-cli: signals takes check <file>, import obdb "
                     "<file> --out <file>, or expr (--signals <file> | "
                     "--plugin <pkg>) [<expr>]\n");
        usage(stderr);
        return kExitUsage;
    }
    if (command == "log") {
        cli::LogArgs la;
        std::string why;
        if (!cli::parse_log(argc, argv, la, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        std::string opened;
        const int rc =
            take_profile_source(la.signals, la.plugin, &la.uri, &opened);
        if (rc >= 0) {
            return rc;
        }
        return run_log(la);
    }
    if (command != "stream") {
        usage(stderr);
        return kExitUsage;
    }
    StreamArgs args;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--values") {
            args.values = true;
        } else if (a == "--until-scanned") {
            args.until_scanned = true;
        } else if (a == "--count" && i + 1 < argc) {
            if (!parse_count(argv[++i], args.count)) {
                std::fprintf(
                    stderr,
                    "havne-cli: --count takes a whole number above 0\n");
                return kExitUsage;
            }
        } else if (a == "--seconds" && i + 1 < argc) {
            if (!parse_run_seconds(argv[++i], args.seconds)) {
                std::fprintf(stderr, "havne-cli: %s\n", cli::run_seconds_why());
                return kExitUsage;
            }
        } else if (a == "--record" && i + 1 < argc) {
            args.record = argv[++i];
            if (*args.record == '\0') {
                std::fprintf(stderr, "havne-cli: --record takes a folder\n");
                return kExitUsage;
            }
        } else if (a == "--label" && i + 1 < argc) {
            args.label = argv[++i];
        } else if (a == "--signals" && i + 1 < argc) {
            args.signals = argv[++i];
        } else if (a == "--plugin" && i + 1 < argc) {
            args.plugin = argv[++i];
        } else if (a == "--conditions" && i + 1 < argc &&
                   args.conditions == nullptr && *argv[i + 1] != '\0') {
            args.conditions = argv[++i];
        } else if (a == "--conditions") {
            std::fprintf(stderr,
                         "havne-cli: --conditions takes one condition file\n");
            return kExitUsage;
        } else if (a == "--pick") {
            if (i + 1 >= argc || !cli::parse_pick(argv[++i])) {
                std::fprintf(stderr, "havne-cli: %s\n", cli::pick_why());
                return kExitUsage;
            }
            args.pick_ranked = true;
        } else if (a.rfind("--", 0) == 0 || args.uri) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        } else {
            args.uri = argv[i];
        }
    }
    if (!args.uri) {
        std::fprintf(stderr, "havne-cli: stream needs a URI\n");
        usage(stderr);
        return kExitUsage;
    }
    if (args.label && !args.record) {
        std::fprintf(stderr, "havne-cli: --label names a --record's folder\n");
        usage(stderr);
        return kExitUsage;
    }
    if (args.signals != nullptr && args.plugin != nullptr) {
        std::fprintf(stderr, "havne-cli: --signals and --plugin each give "
                             "the profile: one at a time\n");
        usage(stderr);
        return kExitUsage;
    }
    std::string opened;
    const int rc =
        take_profile_source(args.signals, args.plugin, &args.uri, &opened);
    if (rc >= 0) {
        return rc;
    }
    return run_stream(args);
}

} // namespace

#ifdef _WIN32
// Windows hands the arguments over as UTF-16 here; main's char** would be
// the ANSI code page's, where a path with a character outside it (an
// accented user name's folder in another code page, Greek, CJK) comes as
// '?'. Each is UTF-8 from here on, as core's paths are.
int wmain(int argc, wchar_t** wargv)
{
    try {
        std::vector<std::string> args(static_cast<size_t>(argc));
        std::vector<char*> argv(static_cast<size_t>(argc) + 1, nullptr);
        for (int i = 0; i < argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr,
                                              0, nullptr, nullptr);
            if (n > 0) {
                args[i].resize(static_cast<size_t>(n));
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &args[i][0], n,
                                    nullptr, nullptr);
                args[i].resize(static_cast<size_t>(n - 1));
            }
            argv[i] = &args[i][0];
        }
        return run(argc, argv.data());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "havne-cli: %s\n", e.what());
        return kExitFailed;
    }
}
#else
int main(int argc, char** argv)
{
    // What throws (an allocation, a thread that won't start) ends as the
    // ring's out of memory does: a CLI failure with its reason, not
    // std::terminate
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "havne-cli: %s\n", e.what());
        return kExitFailed;
    }
}
#endif
