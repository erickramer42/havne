// File/layer: cli/cmd_plugins.cpp | havne-cli, plugins list, match, check, wrap, put
//             and drop (ADR-0052, ADR-0064)
// Testing: UNIT: CliGoldenTest, CliPluginMakeTest, CliPluginRunTest,
//          CliPluginConversationTest, CliUsageTest (ctest); CliPluginTest
//          (test_cli)
//
// Public headers only: what any program linking core can do.

#include "page_lines.h"
#include "plugin_lines.h"
#include "signal_file_lines.h"

#include "commands.h"
#include "profile_source.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "ecuio/vin.h"

namespace cmd {

namespace {

// The conversation line of a plugin with the tier, after its plugin line
void write_conversation(Output& out, uint32_t package, const ecuio_plugin_t* p)
{
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    if (ecuio_plugin_conversation(p, &c) != ECUIO_OK) {
        return; // a plugin without the tier
    }
    BigLine b(cli::kConversationLineBytes);
    cli::conversation_line(b.line, package, p, c);
    write_line(out, b.line);
}

} // namespace

// plugins list <pkg>... (ADR-0052): start, then a plugin line for each
// package (and a conversation line for one with the tier), or a refused
// line saying why and going on; exit 3 when any was refused. No end line
// (a list)
int run_plugins_list(const std::vector<const char*>& paths)
{
    Output out;
    write_start(out);
    bool refused = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string why;
        ecuio_plugin_t* p = nullptr;
        const ecuio_status_t st = load_package(paths[i], &p, &why);
        const auto index = static_cast<uint32_t>(i);
        BigLine b(4096 + why.size() * 6);
        if (st != ECUIO_OK) {
            refused = true;
            cli::refused_line(b.line, "refused", nullptr, index, st,
                              why.c_str());
        } else {
            cli::plugin_line(b.line, index, p, plugin_info(p));
        }
        write_line(out, b.line);
        if (p != nullptr) {
            write_conversation(out, index, p);
            ecuio_plugin_free(p);
        }
    }
    out.flush();
    if (!out.ok()) {
        return kExitFailed;
    }
    return refused ? kExitConnect : kExitOk;
}

// plugins match <pkg> --vin <VIN> (ADR-0052 section 5): the variants the
// VIN's manufacturer code and model year fall in, a proposal only. The
// VIN is taken as vin takes it, and never written. start, plugin, match;
// a package refused is start and an error line, exit 3
int run_plugins_match(const char* path, const char* typed)
{
    std::string vin;
    for (const char* c = typed; *c != '\0'; ++c) {
        if (*c != ' ') {
            vin += (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A')
                                            : *c;
        }
    }
    ecuio_vin_info_t decoded{};
    if (ecuio_vin_decode(vin.c_str(), latest_model_year(), &decoded) !=
        ECUIO_OK) {
        std::fprintf(stderr,
                     "havne-cli: not a VIN: 49 CFR 565 takes 17 digits and "
                     "capital letters, never I, O or Q\n");
        return kExitUsage;
    }
    Output out;
    write_start(out);
    std::string why;
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t st = load_package(path, &p, &why);
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        return ended(out, kExitConnect);
    }
    const ecuio_plugin_info_t info = plugin_info(p);
    uint32_t indices[ECUIO_PLUGIN_MAX_VARIANTS] = {};
    uint32_t count = 0;
    ecuio_plugin_match(p, &decoded, indices, ECUIO_PLUGIN_MAX_VARIANTS, &count);
    {
        BigLine b(4096);
        cli::plugin_line(b.line, 0, p, info);
        write_line(out, b.line);
    }
    {
        BigLine b(4096);
        cli::match_line(b.line, p, info, decoded, indices,
                        std::min<uint32_t>(count, ECUIO_PLUGIN_MAX_VARIANTS));
        write_line(out, b.line);
    }
    ecuio_plugin_free(p);
    out.flush();
    return ended(out, kExitOk);
}

namespace {

// A fixture entry and what its run found
struct FixtureRun {
    std::string file;
    ecuio_plugin_value_t value{};
    cli::FixtureResult result = cli::FixtureResult::not_run;
    bool have = false;
    double got = 0;
    std::string why;
};

// The newest decoded value of each signal a round's fixtures name,
// filled on the worker without a lock or a wait: the ids are fixed
// before start, and each slot is an atomic the main thread reads
struct FixtureValues {
    explicit FixtureValues(const std::vector<uint16_t>& signal_ids)
        : ids(signal_ids), values(new std::atomic<double>[signal_ids.size()]),
          have(new std::atomic<bool>[signal_ids.size()])
    {
        for (size_t k = 0; k < ids.size(); ++k) {
            values[k].store(0.0);
            have[k].store(false);
        }
    }

    static void on_values(const ecuio_value_batch_t* batch, void* user)
    {
        auto* self = static_cast<FixtureValues*>(user);
        for (uint32_t i = 0; i < batch->count; ++i) {
            const ecuio_value_t& v = batch->values[i];
            for (size_t k = 0; k < self->ids.size(); ++k) {
                if (self->ids[k] == v.signal_id) {
                    self->values[k].store(v.value);
                    self->have[k].store(true);
                }
            }
        }
    }

    bool all() const
    {
        for (size_t k = 0; k < ids.size(); ++k) {
            if (!have[k].load()) {
                return false;
            }
        }
        return true;
    }

    const std::vector<uint16_t> ids;
    std::unique_ptr<std::atomic<double>[]> values;
    std::unique_ptr<std::atomic<bool>[]> have;
};

// How long a variant's fixtures wait for their values: the probe's walk
// and the rotation over the base set's PIDs share the bus with them
constexpr int kFixtureWaitMs = 20000;

// Runs one round of a variant's fixtures, each signal at most once in it,
// against the simulated ECU on interface on: the sim answers the
// variant's DIDs with the fixtures' values, a socketcan:// session
// decodes them through the variant's own profile, and each must come back
// within half its field's step. ECUIO_OK, or the status and why of a sim
// or session that wouldn't start
ecuio_status_t run_fixture_round(const std::string& profile, const char* on,
                                 const std::vector<FixtureRun*>& mine,
                                 std::string* why)
{
    const std::string sim_uri = "mock://check?profile=" + profile;
    ecuio_sim_t* sim = nullptr;
    ecuio_status_t st = ecuio_sim_create(sim_uri.c_str(), on, &sim);
    if (st != ECUIO_OK) {
        *why = ecuio_last_error_detail();
        return st;
    }
    std::vector<FixtureRun*> asked;
    std::vector<uint16_t> ids;
    for (FixtureRun* r : mine) {
        if (ecuio_sim_set_value(sim, r->value.def->id, r->value.value) !=
            ECUIO_OK) {
            r->result = cli::FixtureResult::failed;
            r->why = "the simulated ECU can't answer it: it answers a "
                     "Service 22 DID's field, and only a value the field "
                     "holds";
            continue;
        }
        asked.push_back(r);
        ids.push_back(r->value.def->id);
    }
    FixtureValues got(ids);
    if (!asked.empty()) {
        const std::string uri = std::string("socketcan://") + on +
                                "?profile=" + profile +
                                "&identify=0&diagnose=0";
        ecuio_session_t* s = nullptr;
        st = ecuio_session_create(uri.c_str(), &s);
        if (st == ECUIO_OK) {
            st = ecuio_register_values_callback(s, &FixtureValues::on_values,
                                                &got);
        }
        if (st == ECUIO_OK) {
            st = ecuio_session_start(s);
        }
        if (st != ECUIO_OK) {
            *why = ecuio_last_error_detail();
            ecuio_session_destroy(s);
            ecuio_sim_destroy(sim);
            return st;
        }
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(kFixtureWaitMs);
        while (!got.all() && !g_interrupted.load() &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ecuio_session_close(s);
        ecuio_session_destroy(s);
    }
    ecuio_sim_destroy(sim);
    for (size_t k = 0; k < asked.size(); ++k) {
        FixtureRun& r = *asked[k];
        r.have = got.have[k].load();
        r.got = got.values[k].load();
        if (!r.have) {
            r.result = cli::FixtureResult::failed;
            r.why = g_interrupted.load()
                        ? "interrupted before its value came"
                        : "no value came within " +
                              std::to_string(kFixtureWaitMs / 1000) + " s";
        } else if (cli::fixture_passes(*r.value.def, r.value.value, r.got)) {
            r.result = cli::FixtureResult::passed;
        } else {
            r.result = cli::FixtureResult::failed;
            r.why = "the decoded value is past half the field's step";
        }
    }
    return ECUIO_OK;
}

// Runs variant index's fixtures (the runs whose value.variant is index)
// in rounds, a signal given in two fixture files being in a later round
// than its first: the simulated ECU answers one value per signal at a time
ecuio_status_t run_variant_fixtures(const std::string& profile, uint32_t index,
                                    const char* on,
                                    std::vector<FixtureRun>& runs,
                                    std::string* why)
{
    std::vector<FixtureRun*> left;
    for (FixtureRun& r : runs) {
        if (r.value.variant == index) {
            left.push_back(&r);
        }
    }
    while (!left.empty()) {
        std::vector<FixtureRun*> round;
        std::vector<FixtureRun*> later;
        for (FixtureRun* r : left) {
            const bool taken =
                std::any_of(round.begin(), round.end(), [r](FixtureRun* o) {
                    return o->value.def == r->value.def;
                });
            (taken ? later : round).push_back(r);
        }
        const ecuio_status_t st = run_fixture_round(profile, on, round, why);
        if (st != ECUIO_OK) {
            return st;
        }
        left.swap(later);
    }
    return ECUIO_OK;
}

// What a plugin's variants use that an older core reads otherwise
struct VariantUses {
    bool gauge = false;
    bool format2 = false;
};

// For each variant a variant line, a share_alike line when its license is
// share-alike, and its own signals' signal_def lines
VariantUses write_variant_lines(Output& out, const ecuio_plugin_t* p,
                                const ecuio_plugin_info_t& info)
{
    VariantUses uses;
    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(p, v);
        const std::string id = variant_of(p, v);
        uses.format2 =
            uses.format2 ||
            ecuio_signal_file_format(ecuio_plugin_variant_file(p, v)) >= 2;
        {
            BigLine b(
                cli::variant_line_bytes(set, ecuio_plugin_variant_file(p, v)));
            cli::variant_line(b.line, id.c_str(),
                              cli::plugin_profile(info.id, id.c_str()),
                              ecuio_plugin_variant_file(p, v), set);
            write_line(out, b.line);
        }
        const ecuio_signal_source_t source =
            cli::source_of(ecuio_plugin_variant_file(p, v));
        if ((source.flags & ECUIO_SOURCE_SHARE_ALIKE) != 0) {
            BigLine b(cli::share_alike_line_bytes(source.license));
            cli::share_alike_line(b.line, id.c_str(), source.license);
            write_line(out, b.line);
        }
        for (uint32_t i = 0; i < set.count; ++i) {
            if (cli::own_signal(ecuio_plugin_variant_file(p, v), set.defs[i])) {
                const ecuio_signal_gauge_t* gauge = ecuio_signal_file_gauge(
                    ecuio_plugin_variant_file(p, v), &set.defs[i]);
                uses.gauge = uses.gauge || gauge != nullptr;
                BigLine b(cli::def_line_bytes(set, set.defs[i]));
                cli::def_line(b.line, set, set.defs[i], gauge,
                              ecuio_signal_file_expr(
                                  ecuio_plugin_variant_file(p, v), &set.defs[i]));
                write_line(out, b.line);
            }
        }
    }
    return uses;
}

// A no_gauge note for each number or computed signal of a variant's own
// that has none, so a plugin's author sets the range the Dashboard would
// guess
void write_no_gauge_notes(Output& out, const ecuio_plugin_t* p,
                          const ecuio_plugin_info_t& info)
{
    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const ecuio_signal_file_t* file = ecuio_plugin_variant_file(p, v);
        const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(p, v);
        const std::string id = variant_of(p, v);
        const std::vector<ecuio_signal_key_t> keys = cli::set_keys(&set, file);
        for (uint32_t i = 0; i < set.count; ++i) {
            if (cli::lacks_gauge(file, set.defs[i],
                                 ecuio_signal_file_gauge(file, &set.defs[i]))) {
                BigLine b(cli::no_gauge_line_bytes());
                cli::no_gauge_line(b.line, id.c_str(), set.defs[i],
                                   i < keys.size() ? keys[i].text : nullptr);
                write_line(out, b.line);
            }
        }
    }
}

// Each fixture file's entries as runs not yet run, or a fixture_refused
// line for a file that can't be read or that core refused
std::vector<FixtureRun> read_fixture_runs(Output& out, const char* path,
                                          const ecuio_plugin_t* p,
                                          const ecuio_plugin_info_t& info,
                                          cli::FixtureCounts& counts)
{
    std::vector<FixtureRun> runs;
    std::string why;
    for (uint32_t f = 0; f < info.fixture_count; ++f) {
        char name[ECUIO_PLUGIN_PATH_MAX + 1] = {};
        ecuio_plugin_fixture(p, f, name, sizeof name);
        std::string bytes;
        ecuio_status_t st = read_whole((std::string(path) + "/" + name).c_str(),
                                       ECUIO_SIGNAL_FILE_MAX_BYTES,
                                       "fixture file", &bytes, &why)
                                ? ECUIO_OK
                                : ECUIO_ERR_LOAD_FAILED;
        uint32_t count = 0;
        if (st == ECUIO_OK) {
            st = ecuio_plugin_values(p, bytes.data(), bytes.size(), nullptr, 0,
                                     &count);
            why = ecuio_last_error_detail();
        }
        std::vector<ecuio_plugin_value_t> values(count);
        if (st == ECUIO_OK) {
            st = ecuio_plugin_values(p, bytes.data(), bytes.size(),
                                     values.data(), count, &count);
            why = ecuio_last_error_detail();
        }
        if (st != ECUIO_OK) {
            ++counts.files_refused;
            BigLine b(1024 + why.size() * 6);
            cli::refused_line(b.line, "fixture_refused", name, 0, st,
                              why.c_str());
            write_line(out, b.line);
            continue;
        }
        for (const ecuio_plugin_value_t& v : values) {
            FixtureRun r;
            r.file = name;
            r.value = v;
            r.why = "no --on interface: the simulated ECU answers on one";
            runs.push_back(r);
        }
    }
    return runs;
}

// Every variant's runs, through the simulated ECU on interface on
ecuio_status_t run_fixtures(ecuio_plugin_t* p, const ecuio_plugin_info_t& info,
                            const char* on, std::vector<FixtureRun>& runs,
                            std::string* why)
{
    ecuio_status_t st = register_variants(p, why);
    for (uint32_t v = 0; st == ECUIO_OK && v < info.variant_count; ++v) {
        const std::string profile =
            cli::plugin_profile(info.id, variant_of(p, v).c_str());
        st = run_variant_fixtures(profile, v, on, runs, why);
    }
    return st;
}

// A fixture line for each run, counted
void write_fixture_lines(Output& out, const ecuio_plugin_t* p,
                         const std::vector<FixtureRun>& runs,
                         cli::FixtureCounts& counts)
{
    for (const FixtureRun& r : runs) {
        switch (r.result) {
        case cli::FixtureResult::passed:
            ++counts.passed;
            break;
        case cli::FixtureResult::failed:
            ++counts.failed;
            break;
        default:
            ++counts.not_run;
        }
        const std::string variant = variant_of(p, r.value.variant);
        BigLine b(1024 + r.why.size() * 6);
        cli::fixture_line(
            b.line, r.file.c_str(), variant.c_str(), r.value, r.result, r.have,
            r.got,
            r.result == cli::FixtureResult::passed ? nullptr : r.why.c_str());
        write_line(out, b.line);
    }
}

// A page line for each of the plugin's pages; false when one fails
bool write_page_lines(Output& out, const ecuio_plugin_t* p,
                      const ecuio_plugin_info_t& info)
{
    bool all = true;
    std::vector<std::pair<std::string, std::string>> seen;
    for (uint32_t i = 0; i < info.page_count; ++i) {
        const cli::PageCheck c =
            cli::check_page(p, info.variant_count, i, &seen);
        all = all && c.words.empty();
        BigLine b(cli::page_line_bytes(c));
        cli::page_line(b.line, c);
        write_line(out, b.line);
    }
    return all;
}

// A requires_core line for each thing the plugin uses (its variants' and
// its pages) that the manifest's requires.core is too old to read
void write_requires_core_lines(Output& out, const ecuio_plugin_t* p,
                               const VariantUses& uses, uint32_t page_count)
{
    uint16_t declared[3] = {};
    if (ecuio_plugin_requires_core(p, &declared[0], &declared[1],
                                   &declared[2]) == ECUIO_OK) {
        if (cli::gauge_needs_newer_core(declared, uses.gauge)) {
            BigLine b(512);
            cli::requires_core_line(b.line, declared);
            write_line(out, b.line);
        }
        if (cli::format2_needs_newer_core(declared, uses.format2)) {
            BigLine b(512);
            cli::requires_core_line(b.line, declared, cli::kFormat2Core,
                                    "\"havne_signals\": 2");
            write_line(out, b.line);
        }
        if (cli::pages_need_newer_core(declared, page_count)) {
            BigLine b(1024);
            cli::pages_core_line(b.line, declared);
            write_line(out, b.line);
        }
    }
}

} // namespace

// plugins check <pkg> [--on <vcan>] (ADR-0052 section 8): loads the
// package as a session would, then reads its value fixtures. start,
// plugin, a conversation line for a plugin with the tier, for each
// variant a variant line (its source's license), a
// share_alike line when that license is share-alike, and its own signals'
// signal_def lines, then a no_gauge note for each of its own number or
// computed signals with no gauge (exit stays 0), a page line for each
// Dashboard page, a fixture_refused line for each fixture file that can't
// be read or that core refused, a fixture line for each entry (with --on,
// run through the simulated ECU on that interface, Linux's vcan; else not
// run), then checked. A package refused is start and an error line, exit
// 3, as is a sim or session that won't start; a fixture failed, a file
// refused or a page failed, exit 1. No end line
int run_plugins_check(const char* path, const char* on)
{
    Output out;
    write_start(out);
    std::string why;
    ecuio_plugin_t* p = nullptr;
    ecuio_status_t st = load_package(path, &p, &why);
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        return ended(out, kExitConnect);
    }
    const ecuio_plugin_info_t info = plugin_info(p);
    {
        BigLine b(4096);
        cli::plugin_line(b.line, 0, p, info);
        write_line(out, b.line);
    }
    write_conversation(out, 0, p);
    const VariantUses uses = write_variant_lines(out, p, info);
    write_no_gauge_notes(out, p, info);
    const bool pages_ok = write_page_lines(out, p, info);

    cli::FixtureCounts counts;
    std::vector<FixtureRun> runs =
        read_fixture_runs(out, path, p, info, counts);
    if (on != nullptr && !runs.empty()) {
        st = run_fixtures(p, info, on, runs, &why);
        if (st != ECUIO_OK) {
            write_error(out, st, why.c_str());
            ecuio_plugin_free(p);
            return ended(out, kExitConnect);
        }
    }
    write_fixture_lines(out, p, runs, counts);
    write_requires_core_lines(out, p, uses, info.page_count);
    {
        BigLine b(512 + (on != nullptr ? std::strlen(on) * 6 : 0));
        cli::checked_line(b.line, counts, on, info.fixture_count);
        write_line(out, b.line);
    }
    ecuio_plugin_free(p);
    out.flush();
    if (!out.ok()) {
        return kExitFailed;
    }
    return counts.failed > 0 || counts.files_refused > 0 || !pages_ok
               ? kExitFailed
               : kExitOk;
}

MadeText made_text(ecuio_status_t status, char* text, size_t length)
{
    MadeText m;
    m.status = status;
    m.words = ecuio_last_error_detail();
    if (text != nullptr) {
        m.text.assign(text, length);
    }
    ecuio_plugin_text_free(text);
    return m;
}

// Writes a made plugin to path whole: true, or its error line and false
bool write_made_file(Output& out, const char* path, const MadeText& m)
{
    std::string why;
    if (!write_whole(path, m.text, &why, "the plugin")) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        return false;
    }
    return true;
}

// The plugin line of the plugin written, then the written line
void write_made_lines(Output& out, const MadeText& m, cli::Written action,
                      const std::string& variant)
{
    ecuio_plugin_t* p = nullptr;
    if (ecuio_plugin_load(m.text.data(), m.text.size(), nullptr, nullptr, &p) !=
        ECUIO_OK) {
        return; /* never: core loads what it gives before giving it */
    }
    const ecuio_plugin_info_t info = plugin_info(p);
    {
        BigLine b(4096);
        cli::plugin_line(b.line, 0, p, info);
        write_line(out, b.line);
    }
    char lineage[ECUIO_PLUGIN_LINEAGE_LEN + 1] = {};
    ecuio_plugin_lineage(p, lineage, sizeof lineage);
    BigLine b(cli::written_line_bytes(m.words));
    cli::written_line(b.line, action, variant.c_str(), info.version, lineage,
                      m.text.size(), m.words);
    write_line(out, b.line);
    ecuio_plugin_free(p);
}

namespace {

// A packed plugin's bytes for put and drop: true, or its error line (a
// folder, or a file that can't be read) and false
bool read_packed(Output& out, const char* path, std::string* bytes)
{
    std::string why;
    if (is_folder(path)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED,
                    "the plugin is a folder: put and drop take a packed "
                    "plugin, one file");
        return false;
    }
    if (!read_whole(path, ECUIO_PLUGIN_PACKAGE_MAX, "plugin", bytes, &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        return false;
    }
    return true;
}

// A signal file's bytes for wrap and put: true, or its error line and
// false
bool read_signal_bytes(Output& out, const char* path, std::string* bytes)
{
    std::string why;
    if (!read_whole(path, ECUIO_SIGNAL_FILE_MAX_BYTES, "signal file", bytes,
                    &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        return false;
    }
    return true;
}

// What a made plugin's command ends with: a refusal's error line (exit
// 3), an --out that can't be written (exit 1), else the plugin and
// written lines (exit 0)
int end_made(Output& out, const char* path, const MadeText& m,
             cli::Written action, const std::string& variant)
{
    if (m.status != ECUIO_OK) {
        write_error(out, m.status, m.words.c_str());
        return ended(out, kExitConnect);
    }
    if (!write_made_file(out, path, m)) {
        return ended(out, kExitFailed);
    }
    write_made_lines(out, m, action, variant);
    return ended(out, kExitOk);
}

} // namespace

// plugins wrap <signal-file> --out <pkg> [--id <id>] [--variant <id>]: the
// file as a packed plugin of one variant (ecuio_plugin_wrap). start, then
// plugin and written; a file unread or refused, start and an error line,
// exit 3; an --out that can't be written, exit 1. No end line
int run_plugins_wrap(const PluginMakeArgs& args)
{
    Output out;
    write_start(out);
    std::string file;
    if (!read_signal_bytes(out, args.file, &file)) {
        return ended(out, kExitConnect);
    }
    ecuio_plugin_wrap_options_t o{};
    o.struct_size = sizeof o;
    o.id = args.id;
    o.variant = args.variant;
    char* text = nullptr;
    size_t length = 0;
    const ecuio_status_t st =
        ecuio_plugin_wrap(file.data(), file.size(), &o, &text, &length);
    return end_made(out, args.out, made_text(st, text, length),
                    cli::Written::wrapped,
                    args.variant != nullptr ? args.variant : "base");
}

// plugins put <pkg> <signal-file> [--variant <id>] --out <pkg>: the
// plugin with the file as a variant, added or put in the place of the
// one of that id (ecuio_plugin_put_variant; with no --variant, the id
// core gives, the file's name as a slug). As wrap's, the written line
// saying added or replaced and which of the file's ECUs took another id
int run_plugins_put(const PluginMakeArgs& args)
{
    Output out;
    write_start(out);
    std::string packed;
    std::string file;
    if (!read_packed(out, args.package, &packed) ||
        !read_signal_bytes(out, args.file, &file)) {
        return ended(out, kExitConnect);
    }
    char* text = nullptr;
    size_t length = 0;
    const ecuio_status_t st =
        ecuio_plugin_put_variant(packed.data(), packed.size(), args.variant,
                                 file.data(), file.size(), &text, &length);
    const MadeText m = made_text(st, text, length);
    std::string variant = args.variant != nullptr ? args.variant : "";
    bool replaced = false;
    if (m.status == ECUIO_OK) {
        if (variant.empty()) {
            ecuio_signal_file_t* f = nullptr;
            if (ecuio_signal_file_load(file.data(), file.size(), &f) ==
                ECUIO_OK) {
                variant = cli::variant_slug(ecuio_signal_file_profile(f));
            }
            ecuio_signal_file_free(f);
        }
        ecuio_plugin_t* before = nullptr;
        if (ecuio_plugin_load(packed.data(), packed.size(), nullptr, nullptr,
                              &before) == ECUIO_OK) {
            const ecuio_plugin_info_t info = plugin_info(before);
            for (uint32_t v = 0; v < info.variant_count; ++v) {
                replaced = replaced || variant_of(before, v) == variant;
            }
            ecuio_plugin_free(before);
        }
    }
    return end_made(out, args.out, m,
                    replaced ? cli::Written::replaced : cli::Written::added,
                    variant);
}

// plugins drop <pkg> --variant <id> --out <pkg>: the plugin without that
// variant (ecuio_plugin_drop_variant; never its last). As wrap's
int run_plugins_drop(const PluginMakeArgs& args)
{
    Output out;
    write_start(out);
    std::string packed;
    if (!read_packed(out, args.package, &packed)) {
        return ended(out, kExitConnect);
    }
    char* text = nullptr;
    size_t length = 0;
    const ecuio_status_t st = ecuio_plugin_drop_variant(
        packed.data(), packed.size(), args.variant, &text, &length);
    return end_made(out, args.out, made_text(st, text, length),
                    cli::Written::dropped, args.variant);
}

// The options after plugins wrap's, put's and drop's inputs: --out
// always, --variant (drop's needed), --id (wrap's alone). false with the
// usage words
bool parse_plugin_make(const std::string& word, int first, int argc,
                       char** argv, PluginMakeArgs* args, std::string* why)
{
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc && *argv[i + 1] != '\0';
        if (a == "--out" && has_value && args->out == nullptr) {
            args->out = argv[++i];
        } else if (a == "--variant" && has_value && args->variant == nullptr) {
            args->variant = argv[++i];
        } else if (a == "--id" && has_value && word == "wrap" &&
                   args->id == nullptr) {
            args->id = argv[++i];
        } else {
            *why = "unexpected '" + a + "'";
            return false;
        }
    }
    if (args->out == nullptr) {
        *why = "plugins " + word + " needs --out <pkg>";
        return false;
    }
    if (word == "drop" && args->variant == nullptr) {
        *why = "plugins drop needs --variant <id>";
        return false;
    }
    return true;
}

namespace {

// plugins list's packages, an option refused
int plugins_list(int argc, char** argv)
{
    std::vector<const char*> paths;
    for (int i = 3; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        }
        paths.push_back(argv[i]);
    }
    return run_plugins_list(paths);
}

// plugins check's package, and --on <vcan> read and checked
int plugins_check(int argc, char** argv)
{
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

// wrap, put and drop: their words through parse_plugin_make, then the run
int plugins_make(const std::string& word, int argc, char** argv)
{
    PluginMakeArgs make;
    if (word == "wrap") {
        make.file = argv[3];
    } else {
        make.package = argv[3];
        make.file = word == "put" ? argv[4] : nullptr;
    }
    std::string why;
    if (!parse_plugin_make(word, word == "put" ? 5 : 4, argc, argv, &make,
                           &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    return word == "wrap"  ? run_plugins_wrap(make)
           : word == "put" ? run_plugins_put(make)
                           : run_plugins_drop(make);
}

} // namespace

// plugins' words, then each one's run
int cmd_plugins(int argc, char** argv)
{
    const std::string word = argc > 2 ? argv[2] : "";
    if (word == "list" && argc > 3) {
        return plugins_list(argc, argv);
    }
    const bool package = argc > 3 && std::string(argv[3]).rfind("--", 0) != 0;
    if (word == "check" && package && (argc == 4 || argc == 6)) {
        return plugins_check(argc, argv);
    }
    if (word == "match" && package && argc == 6 &&
        std::string(argv[4]) == "--vin") {
        return run_plugins_match(argv[3], argv[5]);
    }
    const bool two =
        package && argc > 4 && std::string(argv[4]).rfind("--", 0) != 0;
    if ((word == "wrap" && package) || (word == "put" && two) ||
        (word == "drop" && package)) {
        return plugins_make(word, argc, argv);
    }
    std::fprintf(stderr, "havne-cli: plugins takes list <pkg>..., check <pkg> "
                         "[--on <vcan>], match <pkg> --vin <VIN>, wrap "
                         "<signal-file> --out <pkg>, put <pkg> <signal-file> "
                         "--out <pkg>, or drop <pkg> --variant <id> --out "
                         "<pkg>\n");
    usage(stderr);
    return kExitUsage;
}

} // namespace cmd
