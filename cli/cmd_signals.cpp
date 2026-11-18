// File/layer: cli/cmd_signals.cpp | havne-cli, signals check, signals import
//             and signals expr
// Testing: UNIT: CliGoldenTest, CliUsageTest, CliObdbImportTest,
//          CliPluginMakeTest (ctest); CliSignalFileTest, CliExprTest
//          (test_cli)
//
// Public headers only: what any program linking core can do.

#include "expr_lines.h"
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

namespace {

// What signals expr reads its expression against: a signal file of its
// own, or a plugin's variant
struct ExprFile {
    ecuio_signal_file_t* own = nullptr;
    ecuio_plugin_t* plugin = nullptr;
    const ecuio_signal_file_t* file = nullptr;
    ExprFile() = default;
    ExprFile(const ExprFile&) = delete;
    ExprFile& operator=(const ExprFile&) = delete;
    ~ExprFile()
    {
        ecuio_signal_file_free(own);
        ecuio_plugin_free(plugin);
    }
};

// The file args name, into held. False, the usage line written, for a
// plugin of several variants with none named. A file or package unread
// leaves held->file NULL, with its status and words.
bool load_expr_file(const cli::ExprArgs& args, ExprFile* held,
                    ecuio_status_t* st, std::string* why)
{
    if (args.plugin == nullptr) {
        *st = load_signal_file(args.signals, &held->own, why);
        held->file = held->own;
        return true;
    }
    *st = load_package(args.plugin, &held->plugin, why);
    if (held->plugin == nullptr) {
        return true;
    }
    const ecuio_plugin_info_t info = plugin_info(held->plugin);
    std::string names;
    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const std::string id = variant_of(held->plugin, v);
        names += (v > 0 ? ", " : "") + id;
        if (args.variant != nullptr ? id == args.variant
                                    : info.variant_count == 1) {
            held->file = ecuio_plugin_variant_file(held->plugin, v);
        }
    }
    if (held->file == nullptr) {
        std::fprintf(stderr,
                     "havne-cli: --plugin needs --variant to name one of its "
                     "variants: %s\n",
                     names.c_str());
        return false;
    }
    return true;
}

// An input line for each signal of the file an expression can read
int write_expr_inputs(Output& out, const ecuio_signal_file_t* file)
{
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    for (uint32_t i = 0; i < set.count; ++i) {
        if (cli::expr_reads(file, set.defs[i])) {
            char buf[cli::kExprDefBytes + 64];
            cli::Line l(buf, sizeof buf);
            cli::input_line(l, file, set.defs[i]);
            write_line(out, l);
        }
    }
    return ended(out, kExitOk);
}

// The number defs under a checked program: core's count, then its list
ecuio_status_t expr_leaves(const ecuio_signal_file_t* file,
                           const ecuio_computed_t& program,
                           std::vector<uint16_t>* leaves)
{
    uint32_t count = 0;
    ecuio_status_t st =
        ecuio_signal_file_expr_leaves(file, &program, nullptr, 0, &count);
    if (st == ECUIO_OK && count > 0) {
        leaves->resize(count);
        st = ecuio_signal_file_expr_leaves(file, &program, leaves->data(),
                                           count, &count);
    }
    return st;
}

// Standard error's note for an expression that needs a newer format
// than the file's
void note_format(const ecuio_signal_file_t* file, uint32_t needed)
{
    const uint32_t file_format = ecuio_signal_file_format(file);
    if (needed > file_format) {
        std::fprintf(stderr,
                     "havne-cli: the expression needs \"havne_signals\": %u, "
                     "and the file is %u\n",
                     static_cast<unsigned>(needed),
                     static_cast<unsigned>(file_format));
    }
}

// The value line: the checked program worked out from --value's values,
// one the expression doesn't read said on standard error
int write_expr_value(Output& out, const ecuio_signal_file_t* file,
                     const ecuio_computed_t& program,
                     const std::vector<cli::ExprValue>& values,
                     const std::vector<uint16_t>& leaves)
{
    std::vector<ecuio_eval_input_t> inputs(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        const cli::ExprValue& v = values[i];
        std::memset(&inputs[i], 0, sizeof inputs[i]);
        inputs[i].value = v.value;
        inputs[i].age_us = v.age_us;
        inputs[i].def_id = v.def_id;
        bool read = false;
        for (const uint16_t leaf : leaves) {
            read = read || leaf == v.def_id;
        }
        if (!read) {
            std::fprintf(stderr,
                         "havne-cli: the expression doesn't read '%s': its "
                         "value is passed by\n",
                         v.ref.c_str());
        }
    }
    ecuio_eval_result_t result;
    std::memset(&result, 0, sizeof result);
    result.struct_size = sizeof result;
    const ecuio_status_t st = ecuio_signal_file_evaluate(
        file, &program, inputs.data(), static_cast<uint32_t>(inputs.size()),
        &result);
    if (st != ECUIO_OK) {
        write_error(out, st);
        return ended(out, kExitConnect);
    }
    char buf[cli::kValueLineBytes];
    cli::Line l(buf, sizeof buf);
    cli::value_line(l, file, result);
    write_line(out, l);
    return ended(out, kExitOk);
}

} // namespace

// signals expr (--signals <file> | --plugin <pkg> [--variant <id>])
// [--key <key>] [<expr> [--value <name>=<n>[@<age s>]]...] (ADR-0062): an
// expression read as a computed signal's "expr" would be in that file,
// or in a plugin's variant (its only one, else --variant's; naming none
// of several is usage, nothing written). With no expression: start and
// an input line for each signal an expression can read. With one: start
// and an expr line, what it reads and the format it needs, or where and
// why it is refused (the words and a caret on standard error too, exit
// 3); with --value a value line, the expression worked out from those
// values. A --value whose name reads no number of the file is usage,
// nothing written. A file or package unread or refused: start and an
// error line, exit 3. No end line (a list, as signals check's). The
// arguments are read here, their bad forms usage
int run_signals_expr(int argc, char** argv)
{
    cli::ExprArgs args;
    std::string why;
    if (!cli::parse_signals_expr(argc, argv, &args, &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    ExprFile held;
    ecuio_status_t st = ECUIO_OK;
    if (!load_expr_file(args, &held, &st, &why)) {
        return kExitUsage;
    }
    const ecuio_signal_file_t* file = held.file;
    if (file != nullptr &&
        !cli::resolve_expr_values(file, &args.values, &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return kExitUsage;
    }
    Output out;
    write_start(out);
    if (file == nullptr) {
        write_error(out, st, why.c_str());
        return ended(out, kExitConnect);
    }
    if (args.expr == nullptr) {
        return write_expr_inputs(out, file);
    }
    ecuio_expr_check_t checked;
    std::memset(&checked, 0, sizeof checked);
    checked.struct_size = sizeof checked;
    st = ecuio_signal_file_check_expr(file, args.key, args.expr,
                                      std::strlen(args.expr), &checked);
    const std::string refused = st != ECUIO_OK ? ecuio_last_error_detail() : "";
    if (st != ECUIO_OK && checked.at == 0) {
        // Not the expression's refusal: the call's own
        write_error(out, st, refused.c_str());
        return ended(out, kExitConnect);
    }
    std::vector<uint16_t> leaves;
    if (st == ECUIO_OK) {
        const ecuio_status_t listed =
            expr_leaves(file, checked.program, &leaves);
        if (listed != ECUIO_OK) {
            write_error(out, listed);
            return ended(out, kExitConnect);
        }
    }
    {
        const auto leaf_count = static_cast<uint32_t>(leaves.size());
        BigLine b(cli::expr_line_bytes(args.expr, args.key, refused.c_str(),
                                       leaf_count));
        cli::expr_line(b.line, file, args.expr, args.key, checked,
                       st != ECUIO_OK ? refused.c_str() : nullptr,
                       leaves.data(), leaf_count);
        write_line(out, b.line);
    }
    if (st != ECUIO_OK) {
        std::fprintf(stderr, "havne-cli: %s\n%s", refused.c_str(),
                     cli::expr_caret(args.expr, checked.at).c_str());
        return ended(out, kExitConnect);
    }
    note_format(file, checked.format);
    if (args.values.empty()) {
        return ended(out, kExitOk);
    }
    return write_expr_value(out, file, checked.program, args.values, leaves);
}

namespace {

// signals import obdb's arguments read and checked, then run_signals_import
int signals_import_obdb(int argc, char** argv)
{
    ImportArgs im;
    im.file = argv[4];
    for (int i = 5; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--out" && has_value) {
            im.out = argv[++i];
        } else if (a == "--year" && has_value) {
            uint64_t year = 0;
            if (!parse_count(argv[++i], year) || year < 1980 || year > 2100) {
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
                std::fprintf(stderr, "havne-cli: --wmi takes manufacturer "
                                     "codes, each 3 or 6 of a VIN's letters "
                                     "and digits, separated by commas\n");
                return kExitUsage;
            }
        } else if (a == "--years" && has_value) {
            if (!cli::parse_model_years(argv[++i], im.years)) {
                std::fprintf(stderr, "havne-cli: --years takes <from>-<to>, "
                                     "two model years from 1980 to 2039\n");
                return kExitUsage;
            }
        } else {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        }
    }
    if (im.out == nullptr || *im.out == '\0') {
        std::fprintf(stderr,
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

} // namespace

// signals' words: check, import obdb or expr
int cmd_signals(int argc, char** argv)
{
    const std::string word = argc > 2 ? argv[2] : "";
    if (word == "check" && argc == 4) {
        return run_signals_check(argv[3]);
    }
    if (word == "import" && argc > 4 && std::string(argv[3]) == "obdb") {
        return signals_import_obdb(argc, argv);
    }
    if (word == "expr") {
        return run_signals_expr(argc - 3, argv + 3);
    }
    std::fprintf(stderr, "havne-cli: signals takes check <file>, import obdb "
                         "<file> --out <file>, or expr (--signals <file> | "
                         "--plugin <pkg>) [<expr>]\n");
    usage(stderr);
    return kExitUsage;
}

} // namespace cmd
