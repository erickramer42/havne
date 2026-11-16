// File/layer: cli/plugin_lines.h | havne-cli, a plugin's lines (ADR-0052)
// Testing: UNIT (tests/test_cli.cpp, CliPluginTest); the commands in the
//          golden runs (tests/golden/cli/plugins-*.jsonl, on
//          plugins/example-sim), CliPluginMakeTest
//
// What `havne-cli plugins list`, `check` and `match` write about a loaded
// package (plugin.h): the package, each variant and its source, each
// value fixture's outcome, the check's summary and the variants a VIN
// proposes, and the rule a fixture passes by; what `plugins wrap`, `put`,
// `drop` and `signals import obdb --plugin` (ADR-0064 Q9) wrote, and the
// forms of their options.
#ifndef HAVNE_CLI_PLUGIN_LINES_H
#define HAVNE_CLI_PLUGIN_LINES_H

#include "jsonl.h"
#include "signal_file_lines.h"

#include "ecuio/plugin.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cli {

// The profile a variant registers as: "plugin.<id>.<variant>"
inline std::string plugin_profile(const char* id, const char* variant)
{
    return std::string("plugin.") + id + "." + variant;
}

// Whether a decoded value passes its fixture: within half the field's
// step (|scale|; an enum's is 1), the half widened by a part in 1e9 of
// the value for the double's own rounding
inline bool fixture_passes(const ecuio_signal_def_t& d, double want, double got)
{
    const double step = d.kind == ECUIO_VALUE_ENUM ? 1.0 : std::fabs(d.scale);
    const double slack = 1e-9 * std::fmax(1.0, std::fabs(want));
    return std::fabs(got - want) <= step / 2 + slack;
}

// A variant's own signals: the defs its files made (they have keys), the
// rest its base set's (signal_file_lines.h's rule)
inline uint32_t own_signals(const ecuio_signal_file_t* file,
                            const ecuio_signal_set_t& set)
{
    uint32_t own = 0;
    for (uint32_t i = 0; i < set.count; ++i) {
        own += own_signal(file, set.defs[i]) ? 1u : 0u;
    }
    return own;
}

// {"type":"plugin", package (its place among the command's packages),
// id, version, name, tiers ["data"], variants [{id, profile}], fixtures
// (a folder's fixture files)}
inline void plugin_line(Line& l, uint32_t package, const ecuio_plugin_t* p,
                        const ecuio_plugin_info_t& info)
{
    l.raw("{\"type\":\"plugin\"");
    l.key("package");
    l.u64(package);
    l.key("id");
    l.string(info.id, std::strlen(info.id));
    l.key("version");
    l.string(info.version, std::strlen(info.version));
    l.key("name");
    l.string(info.name, std::strlen(info.name));
    l.key("tiers");
    l.raw((info.tiers & ECUIO_PLUGIN_TIER_DATA) != 0 ? "[\"data\"]" : "[]");
    l.key("variants");
    l.ch('[');
    for (uint32_t v = 0; v < info.variant_count; ++v) {
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
        ecuio_plugin_variant(p, v, id, sizeof id);
        if (v > 0) {
            l.ch(',');
        }
        l.raw("{\"id\":");
        l.string(id, std::strlen(id));
        l.key("profile");
        const std::string profile = plugin_profile(info.id, id);
        l.string(profile.data(), profile.size());
        l.ch('}');
    }
    l.ch(']');
    l.key("fixtures");
    l.u64(info.fixture_count);
    l.ch('}');
}

// A file's source (ecuio_signal_file_source); every text "" when it
// can't be read
inline ecuio_signal_source_t source_of(const ecuio_signal_file_t* file)
{
    ecuio_signal_source_t s{};
    s.struct_size = sizeof s;
    if (ecuio_signal_file_source(file, &s) != ECUIO_OK) {
        s = ecuio_signal_source_t{};
    }
    return s;
}

// The bytes a variant line needs: its fixed part, its ECUs and its
// source's license
inline size_t variant_line_bytes(const ecuio_signal_set_t& set,
                                 const ecuio_signal_file_t* file)
{
    return 1024 + size_t{set.channel_count} * 64 +
           Line::string_bytes(source_of(file).license);
}

// {"type":"variant", id, profile, signals (its own), base (its base
// set's), ecus [{rx, tx}], license (its source's, null with none),
// share_alike (the license is Creative Commons' share-alike)}
inline void variant_line(Line& l, const char* id, const std::string& profile,
                         const ecuio_signal_file_t* file,
                         const ecuio_signal_set_t& set)
{
    const uint32_t own = own_signals(file, set);
    const ecuio_signal_source_t source = source_of(file);
    l.raw("{\"type\":\"variant\"");
    l.key("id");
    l.string(id, std::strlen(id));
    l.key("profile");
    l.string(profile.data(), profile.size());
    l.key("signals");
    l.u64(own);
    l.key("base");
    l.u64(set.count - own);
    l.key("ecus");
    l.ch('[');
    for (uint32_t i = 0; i < set.channel_count; ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.raw("{\"rx\":");
        l.hex_id(set.channels[i].rx_id);
        l.key("tx");
        l.hex_id(set.channels[i].tx_id);
        l.ch('}');
    }
    l.ch(']');
    l.key("license");
    if (source.license[0] != '\0') {
        l.string(source.license, std::strlen(source.license));
    } else {
        l.raw("null");
    }
    l.key("share_alike");
    l.raw((source.flags & ECUIO_SOURCE_SHARE_ALIKE) != 0 ? "true" : "false");
    l.ch('}');
}

// The words for a share-alike variant, as the app's Export note gives
// them (ADR-0064 Q7)
inline std::string share_alike_words(const char* variant, const char* license)
{
    return std::string("variant '") + variant + "' holds data under " +
           license +
           ", a share-alike license: whoever passes the plugin on keeps "
           "that license and the variant's source with it";
}

// The bytes a share_alike line needs
inline size_t share_alike_line_bytes(const char* license)
{
    return 1024 + 2 * Line::string_bytes(license);
}

// {"type":"share_alike", variant, license, words}, after a share-alike
// variant's line
inline void share_alike_line(Line& l, const char* variant, const char* license)
{
    l.raw("{\"type\":\"share_alike\"");
    l.key("variant");
    l.string(variant, std::strlen(variant));
    l.key("license");
    l.string(license, std::strlen(license));
    l.key("words");
    const std::string words = share_alike_words(variant, license);
    l.string(words.c_str(), words.size());
    l.ch('}');
}

// A refusal of the command's package number package, or of a fixture
// file: {"type":type, package or file, code, message, detail}
inline void refused_line(Line& l, const char* type, const char* file,
                         uint32_t package, ecuio_status_t code,
                         const char* detail)
{
    l.raw("{\"type\":");
    l.string(type, std::strlen(type));
    if (file != nullptr) {
        l.key("file");
        l.string(file, std::strlen(file));
    } else {
        l.key("package");
        l.u64(package);
    }
    l.key("code");
    l.u64(static_cast<uint32_t>(code));
    l.key("message");
    l.string(ecuio_status_string(code), SIZE_MAX);
    l.key("detail");
    l.string(detail, std::strlen(detail));
    l.ch('}');
}

// The words for a fixture's outcome
enum class FixtureResult { passed, failed, not_run };

inline const char* fixture_word(FixtureResult r)
{
    switch (r) {
    case FixtureResult::passed:
        return "passed";
    case FixtureResult::failed:
        return "failed";
    default:
        return "not-run";
    }
}

// {"type":"fixture", file, line, variant, signal, value, tolerance (half
// the field's step), result, got (the decoded value, null with none),
// why (a failed or unrun fixture's reason, else absent)}
inline void fixture_line(Line& l, const char* file, const char* variant,
                         const ecuio_plugin_value_t& v, FixtureResult result,
                         bool have, double got, const char* why)
{
    const double step =
        v.def->kind == ECUIO_VALUE_ENUM ? 1.0 : std::fabs(v.def->scale);
    l.raw("{\"type\":\"fixture\"");
    l.key("file");
    l.string(file, std::strlen(file));
    l.key("line");
    l.u64(v.line);
    l.key("variant");
    l.string(variant, std::strlen(variant));
    l.key("signal");
    l.string(v.signal, std::strlen(v.signal));
    l.key("value");
    l.number(v.value);
    l.key("tolerance");
    l.number(step / 2);
    l.key("result");
    l.string(fixture_word(result), SIZE_MAX);
    l.key("got");
    if (have) {
        l.number(got);
    } else {
        l.raw("null");
    }
    if (why != nullptr) {
        l.key("why");
        l.string(why, std::strlen(why));
    }
    l.ch('}');
}

// The oldest core that reads a signal file's "gauge" (ADR-0051)
constexpr uint16_t kGaugeCore[3] = {0, 7, 2};

// The oldest core that reads a format 2 signal file (ADR-0062)
constexpr uint16_t kFormat2Core[3] = {0, 8, 0};

// Whether a declared requires.core (ecuio_plugin_requires_core's X, Y, Z)
// is older than need: an older core passes the manifest's check, then
// refuses the data files that use what need introduced
inline bool core_older_than(const uint16_t declared[3], const uint16_t need[3])
{
    for (int i = 0; i < 3; ++i) {
        if (declared[i] != need[i]) {
            return declared[i] < need[i];
        }
    }
    return false;
}

// Whether a plugin whose data files use "gauge" (uses_gauge) declares
// a requires.core older than kGaugeCore
inline bool gauge_needs_newer_core(const uint16_t declared[3], bool uses_gauge)
{
    return uses_gauge && core_older_than(declared, kGaugeCore);
}

// Whether a plugin with a data file of format 2 (uses_format2) declares a
// requires.core older than kFormat2Core
inline bool format2_needs_newer_core(const uint16_t declared[3],
                                     bool uses_format2)
{
    return uses_format2 && core_older_than(declared, kFormat2Core);
}

// {"type":"requires_core", declared, needs, warning}, written before
// checked when a feature the data files use (what, as the warning says it:
// "gauge" in quotes, say) needs a core newer than declared's: need
inline void requires_core_line(Line& l, const uint16_t declared[3],
                               const uint16_t need[3], const char* what)
{
    char version[24];
    std::snprintf(version, sizeof version, "%u.%u.%u", unsigned{declared[0]},
                  unsigned{declared[1]}, unsigned{declared[2]});
    l.raw("{\"type\":\"requires_core\"");
    l.key("declared");
    l.string(version, std::strlen(version));
    std::snprintf(version, sizeof version, "%u.%u.%u", unsigned{need[0]},
                  unsigned{need[1]}, unsigned{need[2]});
    l.key("needs");
    l.string(version, std::strlen(version));
    l.key("warning");
    const std::string why =
        std::string("the data files use ") + what +
        ", which core reads from " + version +
        " on: raise the manifest's \"requires\" core to " + version +
        ", so an older Havne names that version instead of refusing a "
        "signal file";
    l.string(why.c_str(), why.size());
    l.ch('}');
}

// The gauge's warning
inline void requires_core_line(Line& l, const uint16_t declared[3])
{
    requires_core_line(l, declared, kGaugeCore, "\"gauge\"");
}

// The check's counts
struct FixtureCounts {
    uint64_t passed = 0;
    uint64_t failed = 0;
    uint64_t not_run = 0;
    uint64_t files_refused = 0;
};

// {"type":"checked", fixtures, passed, failed, not_run, files_refused,
// on (the interface, null with none), and warning when the plugin has no
// fixtures (ADR-0052 section 8: required in the repository, a warning
// for a user's own)}
inline void checked_line(Line& l, const FixtureCounts& c, const char* on,
                         uint32_t fixture_files)
{
    l.raw("{\"type\":\"checked\"");
    l.key("fixtures");
    l.u64(c.passed + c.failed + c.not_run);
    l.key("passed");
    l.u64(c.passed);
    l.key("failed");
    l.u64(c.failed);
    l.key("not_run");
    l.u64(c.not_run);
    l.key("files_refused");
    l.u64(c.files_refused);
    l.key("on");
    if (on != nullptr) {
        l.string(on, std::strlen(on));
    } else {
        l.raw("null");
    }
    if (fixture_files == 0) {
        l.key("warning");
        l.raw("\"the plugin has no fixtures: its values are unchecked\"");
    }
    l.ch('}');
}

// {"type":"match", wmi, model_year (null with none), variants [{id,
// profile}]} for the variants ecuio_plugin_match proposed, by index
inline void match_line(Line& l, const ecuio_plugin_t* p,
                       const ecuio_plugin_info_t& info,
                       const ecuio_vin_info_t& vin, const uint32_t* indices,
                       uint32_t count)
{
    l.raw("{\"type\":\"match\"");
    l.key("wmi");
    l.string(vin.wmi, std::strlen(vin.wmi));
    l.key("model_year");
    if (vin.model_year != 0) {
        l.u64(vin.model_year);
    } else {
        l.raw("null");
    }
    l.key("variants");
    l.ch('[');
    for (uint32_t i = 0; i < count; ++i) {
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
        ecuio_plugin_variant(p, indices[i], id, sizeof id);
        if (i > 0) {
            l.ch(',');
        }
        l.raw("{\"id\":");
        l.string(id, std::strlen(id));
        l.key("profile");
        const std::string profile = plugin_profile(info.id, id);
        l.string(profile.data(), profile.size());
        l.ch('}');
    }
    l.raw("]}");
}

// What a command that writes a packed plugin did
enum class Written { wrapped, added, replaced, dropped };

inline const char* written_word(Written w)
{
    switch (w) {
    case Written::wrapped:
        return "wrapped";
    case Written::added:
        return "added";
    case Written::replaced:
        return "replaced";
    default:
        return "dropped";
    }
}

// The bytes a written line needs: its fixed part and core's notes
inline size_t written_line_bytes(const std::string& notes)
{
    return 1024 + Line::string_bytes(notes.c_str());
}

// {"type":"written", action, variant (the one wrapped, put or dropped),
// version, lineage (null with none), bytes (the file's), notes (which of
// the file's ECUs took another id, in core's words; null with none)},
// after the plugin line of the file --out holds
inline void written_line(Line& l, Written action, const char* variant,
                         const char* version, const char* lineage,
                         uint64_t bytes, const std::string& notes)
{
    l.raw("{\"type\":\"written\"");
    l.key("action");
    l.string(written_word(action), SIZE_MAX);
    l.key("variant");
    l.string(variant, std::strlen(variant));
    l.key("version");
    l.string(version, std::strlen(version));
    l.key("lineage");
    if (lineage[0] != '\0') {
        l.string(lineage, std::strlen(lineage));
    } else {
        l.raw("null");
    }
    l.key("bytes");
    l.u64(bytes);
    l.key("notes");
    if (!notes.empty()) {
        l.string(notes.c_str(), notes.size());
    } else {
        l.raw("null");
    }
    l.ch('}');
}

// The variant id core's ecuio_plugin_put_variant gives a file when none
// is named: the file's profile (ecuio_signal_file_profile, "user.<slug>")
// without "user.", cut to ECUIO_PLUGIN_VARIANT_ID_MAX bytes with no '-'
// left at its end
inline std::string variant_slug(const char* profile)
{
    std::string s = profile;
    if (s.compare(0, 5, "user.") == 0) {
        s.erase(0, 5);
    }
    if (s.size() > ECUIO_PLUGIN_VARIANT_ID_MAX) {
        s.resize(ECUIO_PLUGIN_VARIANT_ID_MAX);
    }
    while (!s.empty() && s.back() == '-') {
        s.pop_back();
    }
    return s;
}

// The variant an OBDb set converted to a plugin becomes (ADR-0064 Q2):
// "my<year>" for a model year the user typed, "" (core's "base") for
// none. A year decoded from a VIN never names a variant, and the CLI has
// no VIN to decode one from.
inline std::string typed_year_variant(uint32_t typed_year)
{
    return typed_year != 0 ? "my" + std::to_string(typed_year) : std::string();
}

// --years' form, "<from>-<to>": two model years of four digits, 1980 to
// 2039 (a match's bounds, plugin.h), from at or before to
inline bool parse_model_years(const char* text, uint32_t years[2])
{
    const std::string t = text;
    if (t.size() != 9 || t[4] != '-') {
        return false;
    }
    uint32_t got[2] = {};
    for (size_t n = 0; n < 2; ++n) {
        for (size_t i = 0; i < 4; ++i) {
            const char c = t[n * 5 + i];
            if (c < '0' || c > '9') {
                return false;
            }
            got[n] = got[n] * 10 + static_cast<uint32_t>(c - '0');
        }
        if (got[n] < 1980 || got[n] > 2039) {
            return false;
        }
    }
    if (got[0] > got[1]) {
        return false;
    }
    years[0] = got[0];
    years[1] = got[1];
    return true;
}

// --wmi's form: manufacturer codes separated by commas, each 3 or 6 of a
// VIN's letters and digits (no I, O or Q), lower case taken as upper as
// `vin` takes a VIN. The codes go upper-cased into *codes.
inline bool parse_wmi(const char* text, std::string* codes)
{
    std::string out;
    size_t run = 0;
    for (const char* p = text;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (run != 3 && run != 6) {
                return false;
            }
            if (*p == '\0') {
                break;
            }
            out += ',';
            run = 0;
            continue;
        }
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        const bool digit = c >= '0' && c <= '9';
        const bool letter =
            c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q';
        if (!digit && !letter) {
            return false;
        }
        out += c;
        ++run;
    }
    *codes = out;
    return true;
}

} // namespace cli

#endif /* HAVNE_CLI_PLUGIN_LINES_H */
