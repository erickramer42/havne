// File/layer: cli/cmd_view_args.cpp | havne-cli, view's arguments read and checked
// Testing: UNIT: CliUsageTest, CliViewRunTest (ctest)
//
// Public headers only: what any program linking core can do.

#include "view_lines.h"

#include "commands.h"
#include "common.h"

#include "ecuio/recording.h"

namespace cmd {

// view's arguments read and checked, then run_view
int cmd_view(int argc, char** argv)
{
    ViewArgs view;
    std::vector<const char*> levels; // --stretch-level, read once --units is
    bool imperial = false;
    bool has_units = false;
    bool has_pull_end = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--signal" && has_value) {
            view.signals.emplace_back(argv[++i]);
        } else if ((a == "--from" || a == "--to" || a == "--at") && has_value) {
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
                std::fprintf(stderr,
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
                std::fprintf(stderr, "havne-cli: --columns takes 1 to 10000\n");
                return kExitUsage;
            }
            view.columns = static_cast<uint32_t>(n);
        } else if (a == "--align-on" && has_value) {
            view.align_text = argv[++i];
            if (!cli::parse_align_on(view.align_text, view.align)) {
                std::fprintf(stderr, "havne-cli: --align-on takes "
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
        std::fprintf(stderr, "havne-cli: view needs a recording's folder\n");
        usage(stderr);
        return kExitUsage;
    }
    const bool aligns_pulls = view.align.kind == cli::AlignOn::pull;
    if (!view.stretches && !aligns_pulls &&
        (!levels.empty() || !view.roles.empty() || has_units || has_pull_end)) {
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
                stderr, "havne-cli: --stretch-level takes <key>=<value>, 0 or "
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

} // namespace cmd
