// File/layer: cli/cmd_check.cpp | havne-cli, conditions check (ADR-0063 section 10)
//             and layout check (ADR-0057)
// Testing: UNIT: CliGoldenTest, CliUsageTest (ctest); CliConditionTest,
//          CliLayoutTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "condition_lines.h"
#include "jsonl.h"
#include "layout_check.h"
#include "plugin_lines.h"

#include "commands.h"
#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "ecuio/conditions.h"

namespace cmd {

// conditions check <file> [--signals <file> | --profile <name> |
// --plugin <pkg> [--variant <id>]] (ADR-0063 section 10): what the loader
// read, or why it refused the file, and with a set of signals how each
// condition's references meet it: a signal file's, a registered
// profile's, or a plugin variant's (its only one, else --variant's;
// naming none of several is usage, nothing written). start,
// condition_set, a condition_def line each; a file refused or unread, a
// signal file or a package likewise, or a profile not registered: start
// and an error line, exit 3. No end line (a list, as signals check's)
int run_conditions_check(const cli::ConditionsCheckArgs& args)
{
    ecuio_plugin_t* plugin = nullptr;
    uint32_t variant = 0;
    std::string plugin_why;
    std::string plugin_profile;
    ecuio_status_t plugin_st = ECUIO_OK;
    if (args.plugin != nullptr) {
        plugin_st = load_package(args.plugin, &plugin, &plugin_why);
    }
    if (plugin != nullptr) {
        const ecuio_plugin_info_t info = plugin_info(plugin);
        std::string names;
        bool chosen = false;
        for (uint32_t v = 0; v < info.variant_count; ++v) {
            const std::string id = variant_of(plugin, v);
            names += (v > 0 ? ", " : "") + id;
            if (args.variant != nullptr ? id == args.variant
                                        : info.variant_count == 1) {
                chosen = true;
                variant = v;
                plugin_profile = cli::plugin_profile(info.id, id.c_str());
            }
        }
        if (!chosen) {
            std::fprintf(stderr,
                         "havne-cli: --plugin needs --variant to name one of "
                         "its variants: %s\n",
                         names.c_str());
            ecuio_plugin_free(plugin);
            return kExitUsage;
        }
    }
    Output out;
    write_start(out);
    std::string why;
    std::string bytes;
    if (!read_whole(args.file, ECUIO_CONDITION_FILE_MAX_BYTES, "condition file",
                    &bytes, &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        ecuio_plugin_free(plugin);
        return ended(out, kExitConnect);
    }
    ecuio_condition_set_t* set = nullptr;
    ecuio_status_t st =
        ecuio_condition_set_load(bytes.data(), bytes.size(), &set);
    if (st != ECUIO_OK) {
        write_error(out, st);
        ecuio_plugin_free(plugin);
        return ended(out, kExitConnect);
    }
    ecuio_signal_file_t* own_file = nullptr;
    const ecuio_signal_file_t* file = nullptr;
    const ecuio_signal_set_t* signals = nullptr;
    const char* against = nullptr;
    if (args.signals != nullptr) {
        st = load_signal_file(args.signals, &own_file, &why);
        if (st == ECUIO_OK) {
            file = own_file;
            signals = ecuio_signal_file_set(file);
            against = ecuio_signal_file_profile(file);
        }
    } else if (args.profile != nullptr) {
        st = ecuio_profile_lookup(args.profile, &signals);
        why = std::string("no profile is named '") + args.profile + "'";
        against = args.profile;
    } else if (args.plugin != nullptr) {
        st = plugin_st;
        why = plugin_why;
        if (plugin != nullptr) {
            file = ecuio_plugin_variant_file(plugin, variant);
            signals = ecuio_plugin_variant_set(plugin, variant);
            against = plugin_profile.c_str();
        }
    }
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        ecuio_condition_set_free(set);
        ecuio_plugin_free(plugin);
        return ended(out, kExitConnect);
    }
    ecuio_check_options_t checks;
    std::memset(&checks, 0, sizeof checks);
    ecuio_condition_set_checks(set, &checks);
    const char* name = ecuio_condition_set_name(set);
    const uint32_t count = ecuio_condition_set_count(set);
    {
        BigLine b(512 + cli::Line::string_bytes(name) +
                  (against != nullptr ? cli::Line::string_bytes(against) : 0));
        cli::condition_set_line(b.line, name, count, checks, against);
        write_line(out, b.line);
    }
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_condition_info_t info;
        ecuio_condition_term_t terms[ECUIO_CONDITION_MAX_TERMS];
        ecuio_condition_binding_t binding;
        if (ecuio_condition_set_get(set, i, &info) != ECUIO_OK) {
            continue;
        }
        const bool bound = against != nullptr;
        st = cli::read_terms(set, i, info, terms, signals, file,
                             bound ? &binding : nullptr);
        if (st != ECUIO_OK) {
            write_error(out, st);
            ecuio_signal_file_free(own_file);
            ecuio_plugin_free(plugin);
            ecuio_condition_set_free(set);
            return ended(out, kExitConnect);
        }
        BigLine b(cli::condition_def_bytes(info));
        cli::condition_def_line(b.line, info, terms, bound ? &binding : nullptr,
                                signals);
        write_line(out, b.line);
    }
    ecuio_signal_file_free(own_file);
    ecuio_plugin_free(plugin);
    ecuio_condition_set_free(set);
    return ended(out, kExitOk);
}

// layout check <file> [--form phone|desktop]: whether a dashboard layout
// file reads, and what loading it would drop (cli/layout_check.h, held
// equal to the app's DashboardBook by tests/data/layouts). start, then a
// layout line; exit 0 when it reads (with pages or cells left out too), 1
// when it is refused, 3 when the file can't be read, passes the bound or
// isn't UTF-8 text
int run_layout_check(const char* path, const std::string& form)
{
    Output out;
    write_start(out);
    std::string bytes;
    std::string why;
    if (!read_whole(path, cli::layout::kFileMaxBytes, "layout file", &bytes,
                    &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        return ended(out, kExitConnect);
    }
    // The app's bound counts the whole file, a BOM included, before it reads
    if (bytes.size() > static_cast<size_t>(cli::layout::kFileMaxBytes)) {
        const std::string words = "the file is over " +
                                  std::to_string(cli::layout::kFileMaxBytes) +
                                  " bytes, a layout file's most";
        write_error(out, ECUIO_ERR_LOAD_FAILED, words.c_str());
        return ended(out, kExitConnect);
    }
    if (!cli::layout::valid_utf8(bytes)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED,
                    "the layout file isn't UTF-8 text");
        return ended(out, kExitConnect);
    }
    cli::layout::strip_bom(&bytes);
    const cli::layout::Verdict v = cli::layout::check(bytes, form);
    BigLine b(cli::layout::layout_line_bytes(v));
    cli::layout::layout_line(b.line, v);
    write_line(out, b.line);
    return ended(out, v.read ? kExitOk : kExitFailed);
}

} // namespace cmd
