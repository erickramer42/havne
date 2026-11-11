// File/layer: cli/plugin_lines.h | havne-cli, a plugin's lines (ADR-0052)
// Testing: UNIT (tests/test_cli.cpp, CliPluginTest); the commands in the
//          golden runs (tests/golden/cli/plugins-*.jsonl, on
//          plugins/example-sim)
//
// What `havne-cli plugins list`, `check` and `match` write about a loaded
// package (plugin.h): the package, each variant, each value fixture's
// outcome, the check's summary and the variants a VIN proposes, and the
// rule a fixture passes by.
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

// {"type":"variant", id, profile, signals (its own), base (its base
// set's), ecus [{rx, tx}]}
inline void variant_line(Line& l, const char* id, const std::string& profile,
                         const ecuio_signal_file_t* file,
                         const ecuio_signal_set_t& set)
{
    const uint32_t own = own_signals(file, set);
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
    l.raw("]}");
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

// Whether a plugin whose data files use "gauge" (uses_gauge) declares
// a requires.core (ecuio_plugin_requires_core's X, Y, Z) older than
// kGaugeCore: an older core passes the manifest's check, then refuses
// those files
inline bool gauge_needs_newer_core(const uint16_t declared[3], bool uses_gauge)
{
    if (!uses_gauge) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (declared[i] != kGaugeCore[i]) {
            return declared[i] < kGaugeCore[i];
        }
    }
    return false;
}

// {"type":"requires_core", declared, needs, warning}, written before
// checked when gauge_needs_newer_core
inline void requires_core_line(Line& l, const uint16_t declared[3])
{
    char version[24];
    std::snprintf(version, sizeof version, "%u.%u.%u", unsigned{declared[0]},
                  unsigned{declared[1]}, unsigned{declared[2]});
    l.raw("{\"type\":\"requires_core\"");
    l.key("declared");
    l.string(version, std::strlen(version));
    std::snprintf(version, sizeof version, "%u.%u.%u", unsigned{kGaugeCore[0]},
                  unsigned{kGaugeCore[1]}, unsigned{kGaugeCore[2]});
    l.key("needs");
    l.string(version, std::strlen(version));
    l.key("warning");
    const std::string why =
        std::string("the data files use \"gauge\", which core reads from ") +
        version + " on: raise the manifest's \"requires\" core to " + version +
        ", so an older Havne names that version instead of refusing a "
        "signal file";
    l.string(why.c_str(), why.size());
    l.ch('}');
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

} // namespace cli

#endif /* HAVNE_CLI_PLUGIN_LINES_H */
