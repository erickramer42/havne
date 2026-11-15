// File/layer: cli/cmd_signals.cpp | havne-cli, signals check and signals import
// Testing: UNIT: CliGoldenTest, CliObdbImportTest, CliPluginMakeTest
//          (ctest); CliSignalFileTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "jsonl.h"
#include "plugin_lines.h"
#include "signal_file_lines.h"

#include "commands.h"
#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/obdb.h"
#include "ecuio/plugin.h"
#include "ecuio/signals.h"

namespace cmd {

namespace {

// signals check's lines for a loaded file: signal_file, a signal_def line
// for each of the file's own signals, then a unit line for each unit that
// never converts
void write_check_lines(Output& out, const ecuio_signal_file_t* file)
{
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    {
        BigLine b(1024 + size_t{set.channel_count} * 64);
        cli::signal_file_line(b.line, file, set);
        write_line(out, b.line);
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        if (cli::own_signal(file, set.defs[i])) {
            BigLine b(cli::def_line_bytes(set, set.defs[i]));
            cli::def_line(b.line, set, set.defs[i],
                          ecuio_signal_file_gauge(file, &set.defs[i]),
                          ecuio_signal_file_expr(file, &set.defs[i]));
            write_line(out, b.line);
        }
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        if (cli::unit_kept(set.defs[i], cli::own_signal(file, set.defs[i]))) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::unit_line(l, set.defs[i]);
            write_line(out, l);
        }
    }
}

} // namespace

// signals check <file> (ADR-0051 section 3): what the loader read, or why
// it refused the file. start, signal_file, a signal_def line for each of
// the file's own signals, then a unit line for each unit that never
// converts; refused, start and an error line, exit 3. No end line (a
// list, as view's)
int run_signals_check(const char* path)
{
    Output out;
    write_start(out);
    std::string why;
    ecuio_signal_file_t* file = nullptr;
    const ecuio_status_t st = load_signal_file(path, &file, &why);
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        out.flush();
        return kExitConnect;
    }
    write_check_lines(out, file);
    ecuio_signal_file_free(file);
    out.flush();
    return ended(out, kExitOk);
}

namespace {

// What an import's note callback shares: the output and each kind's count
struct ImportNotes {
    Output* out = nullptr;
    uint64_t counts[4] = {};
};

int write_import_note(const ecuio_obdb_note_t* n, void* user)
{
    auto* notes = static_cast<ImportNotes*>(user);
    if (n->what < 4) {
        ++notes->counts[n->what];
    }
    BigLine b(cli::note_line_bytes(*n));
    cli::import_note_line(b.line, *n);
    write_line(*notes->out, b.line);
    return 0;
}

// signals import obdb ... --plugin (ADR-0064 Q3, Q9): the converted file
// wrapped as a plugin of one variant, "my<year>" for --year and "base"
// without one, a match only from --wmi and --years; --out takes the
// plugin. imported, plugin, written, then signals check's lines for the
// variant's file. A wrap refused: its error line, exit 3
int import_as_plugin(Output& out, const ImportArgs& args,
                     const std::string& converted, const ImportNotes& notes)
{
    const std::string variant =
        cli::typed_year_variant(args.options.model_year);
    ecuio_plugin_wrap_options_t o{};
    o.struct_size = sizeof o;
    o.variant = variant.empty() ? nullptr : variant.c_str();
    o.wmi = args.wmi.empty() ? nullptr : args.wmi.c_str();
    o.years[0] = args.years[0];
    o.years[1] = args.years[1];
    char* text = nullptr;
    size_t length = 0;
    const ecuio_status_t st = ecuio_plugin_wrap(
        converted.data(), converted.size(), &o, &text, &length);
    const MadeText m = made_text(st, text, length);
    if (m.status != ECUIO_OK) {
        write_error(out, m.status, m.words.c_str());
        out.flush();
        return kExitConnect;
    }
    if (!write_made_file(out, args.out, m)) {
        out.flush();
        return kExitFailed;
    }
    {
        char buf[256];
        cli::Line l(buf, sizeof buf);
        cli::imported_line(l, notes.counts[ECUIO_OBDB_NOTE_CONVERTED],
                           notes.counts[ECUIO_OBDB_NOTE_CHANGED],
                           notes.counts[ECUIO_OBDB_NOTE_SKIPPED]);
        write_line(out, l);
    }
    write_made_lines(out, m, cli::Written::wrapped,
                     variant.empty() ? std::string("base") : variant);
    ecuio_plugin_t* p = nullptr;
    if (ecuio_plugin_load(m.text.data(), m.text.size(), nullptr, nullptr, &p) ==
        ECUIO_OK) {
        write_check_lines(out, ecuio_plugin_variant_file(p, 0));
        ecuio_plugin_free(p);
    }
    out.flush();
    return ended(out, kExitOk);
}

} // namespace

// signals import obdb <file> --out <file> (ADR-0051 section 7, obdb.h):
// start, an import_note line for each of core's notes, the converted file
// written to --out whole or not at all, imported with the counts, then
// signals check's lines for the file written. A set that can't be read or
// isn't OBDb's: start and an error line, exit 3; an --out that can't be
// written: its error line, exit 1. No end line
int run_signals_import(const ImportArgs& args)
{
    Output out;
    write_start(out);
    std::string bytes;
    std::string why;
    if (!read_whole(args.file, ECUIO_OBDB_MAX_BYTES, "OBDb file", &bytes,
                    &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        out.flush();
        return kExitConnect;
    }
    ImportNotes notes;
    notes.out = &out;
    char* json = nullptr;
    size_t length = 0;
    const ecuio_status_t st =
        ecuio_obdb_import(bytes.data(), bytes.size(), &args.options,
                          write_import_note, &notes, &json, &length);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        return kExitConnect;
    }
    const std::string text(json, length);
    ecuio_obdb_free(json);
    if (args.plugin) {
        return import_as_plugin(out, args, text, notes);
    }
    if (!write_whole(args.out, text, &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        out.flush();
        return kExitFailed;
    }
    {
        char buf[256];
        cli::Line l(buf, sizeof buf);
        cli::imported_line(l, notes.counts[ECUIO_OBDB_NOTE_CONVERTED],
                           notes.counts[ECUIO_OBDB_NOTE_CHANGED],
                           notes.counts[ECUIO_OBDB_NOTE_SKIPPED]);
        write_line(out, l);
    }
    ecuio_signal_file_t* file = nullptr;
    if (ecuio_signal_file_load(text.data(), text.size(), &file) == ECUIO_OK) {
        write_check_lines(out, file);
        ecuio_signal_file_free(file);
    }
    out.flush();
    return ended(out, kExitOk);
}

} // namespace cmd
