// File/layer: core/src/signal_file_merged.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergedTextTest (test_signals); FUZZ: fuzz_signal_file,
//          fuzz_signal_merge
//
// A merged set's written form (ADR-0065 section 9), read by the loader: a
// format 3 file's "parts", each signal's "part" and the keys it
// "replaced". ecuio_signal_file_merge writes this text and a recording
// copies it, so a copy read back names the same parts, gives each signal
// its part and still answers a key a later part's signal took the place
// of. A file with no "parts" takes none of these keys.

#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "json.h"
#include "plugin_rules.h"
#include "sha256.h"
#include "signal_file_build.h"
#include "signal_file_parts.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ecuio_sf {

using ecuio_json::Type;
using ecuio_json::Value;

namespace {

/* The registry's grammar for a profile's name (profile.h) */
bool profile_name_ok(const std::string& s)
{
    if (s.empty() || s.size() > ECUIO_PROFILE_NAME_MAX) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

const Value& member(const Value& of, const char* key, const std::string& what)
{
    const Value* v = of.find(key);
    if (v == nullptr) {
        refuse(of, what + " needs a \"" + key + "\"");
    }
    return *v;
}

/* A part's "plugin": the id, version and variant its recording line holds */
void read_plugin(const Value& plugin, const std::string& what,
                 ecuio_signal_file::part& out)
{
    expect(plugin, Type::object, what + "'s plugin");
    only_keys(plugin, {"id", "version", "variant"}, "a part's plugin");
    const std::string who = what + "'s plugin";
    const Value& id = member(plugin, "id", who);
    const Value& version = member(plugin, "version", who);
    const Value& variant = member(plugin, "variant", who);
    out.plugin_id = text_of(id, 1, ECUIO_PLUGIN_ID_MAX, who + "'s id");
    out.plugin_version = text_of(version, 1, 15, who + "'s version");
    out.plugin_variant =
        text_of(variant, 1, ECUIO_PLUGIN_VARIANT_ID_MAX, who + "'s variant");
    uint32_t n[3] = {0, 0, 0};
    if (!ecuio_plugin_rules::id_ok(out.plugin_id, ECUIO_PLUGIN_ID_MAX)) {
        refuse(id, who + "'s id " + q(out.plugin_id) + " is no plugin's id");
    }
    if (!ecuio_plugin_rules::parse_version(out.plugin_version, n)) {
        refuse(version, who + "'s version " + q(out.plugin_version) +
                            " isn't three numbers as 1.2.3");
    }
    if (!ecuio_plugin_rules::id_ok(out.plugin_variant,
                                   ECUIO_PLUGIN_VARIANT_ID_MAX)) {
        refuse(variant, who + "'s variant " + q(out.plugin_variant) +
                            " is no variant's id");
    }
    out.has_plugin = true;
}

/* One entry of "parts" */
ecuio_signal_file::part read_part(const Value& v, size_t index)
{
    const std::string what = "part " + std::to_string(index);
    expect(v, Type::object, what);
    only_keys(v, {"profile", "name", "format", "plugin", "source"}, "a part");
    ecuio_signal_file::part out;
    const Value& profile = member(v, "profile", what);
    out.profile =
        text_of(profile, 1, ECUIO_PROFILE_NAME_MAX, what + "'s profile");
    if (!profile_name_ok(out.profile)) {
        refuse(profile, what + "'s profile " + q(out.profile) +
                            " takes 1 to 63 of a-z, 0-9, '.', '_' and '-'");
    }
    if (out.profile.compare(0, 7, "merged.") == 0) {
        refuse(profile, what + " is a merged set: a merged set's parts are "
                               "signal files and plugin variants");
    }
    out.name = text_of(member(v, "name", what), 1, 63, what + "'s name");
    out.format = static_cast<uint32_t>(
        whole_of(member(v, "format", what), ECUIO_SIGNAL_FILE_FORMAT_OLDEST,
                 ECUIO_SIGNAL_FILE_FORMAT, what + "'s format"));
    const Value& source = member(v, "source", what);
    read_source(source);
    take_source(source, out.source);
    if (const Value* plugin = v.find("plugin")) {
        read_plugin(*plugin, what, out);
    }
    return out;
}

/* A merged set's "base_modules" (modules, NULL for none): obd2's channels
 * first, as "extends" gives them, with none of its defs. The set may then
 * hold no signal, since the modules read the emissions codes. */
void read_base_modules(const Value* modules, const Value* extends, parts& p,
                       ecuio_signal_file& f)
{
    if (modules == nullptr) {
        return;
    }
    expect(*modules, Type::string, "\"base_modules\"");
    if (modules->text != "obd2") {
        refuse(*modules, "\"base_modules\" " + q(modules->text) +
                             " isn't a set whose modules a merged set reads: "
                             "only \"obd2\"");
    }
    if (extends != nullptr) {
        refuse(*modules, "a merged set takes \"extends\" or "
                         "\"base_modules\", not both");
    }
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    f.channels.assign(obd2->channels, obd2->channels + obd2->channel_count);
    p.may_be_empty = true;
}

} // namespace

void read_merged(const Value& root, uint32_t format, parts& p,
                 ecuio_signal_file& f)
{
    const Value* list = root.find("parts");
    const Value* modules = root.find("base_modules");
    if (list == nullptr) {
        if (modules != nullptr) {
            refuse(*modules, "\"base_modules\" is a merged set's: a file with "
                             "no \"parts\" takes none");
        }
        return;
    }
    if (format < 3) {
        refuse(*list, "\"parts\" is signal-file format 3's, and this file is "
                      "format " +
                          std::to_string(format));
    }
    if (const Value* needs = root.find("needs")) {
        refuse(*needs, "a merged set has no \"needs\": each part's are met in "
                       "it, or the signals that wait are left out");
    }
    expect(*list, Type::array, "\"parts\"");
    const Value* extends = root.find("extends");
    read_base_modules(modules, extends, p, f);
    if ((list->items.empty() && modules == nullptr) ||
        list->items.size() > ECUIO_MERGE_MAX_PARTS) {
        refuse(*list, "\"parts\" lists " + std::to_string(list->items.size()) +
                          ": a merged set has 1 to " +
                          std::to_string(ECUIO_MERGE_MAX_PARTS) +
                          " (0 with \"base_modules\")");
    }
    /* The profile's name: the set under the parts, then each part's
     * profile, in merge order */
    std::string named;
    if (modules != nullptr) {
        named = "obd2 modules\n";
    } else if (extends != nullptr && extends->type == Type::string &&
               extends->text == "obd2") {
        named = "obd2\n";
    }
    std::unordered_set<std::string> profiles;
    for (size_t i = 0; i < list->items.size(); ++i) {
        f.parts.push_back(read_part(list->items[i], i));
        if (!profiles.insert(f.parts.back().profile).second) {
            refuse(list->items[i], "part " + std::to_string(i) + "'s profile " +
                                       q(f.parts.back().profile) +
                                       " is another part's too");
        }
        named += f.parts.back().profile + "\n";
    }
    /* The file's own source says only that it is a merge: a license is
     * its part's, never the whole set's */
    const Value* source = root.find("source");
    for (const char* k : {"url", "commit", "license", "modified"}) {
        if (const Value* more = source != nullptr ? source->find(k) : nullptr) {
            refuse(*more, std::string("a merged set's \"source\" holds its "
                                      "\"name\" alone, and this one has \"") +
                              k + "\": each part keeps its own source");
        }
    }
    f.source.flags = ECUIO_SOURCE_MERGED;
    p.profile = "merged." +
                ecuio_sha256::hex_of(named.data(), named.size()).substr(0, 16);
}

void read_merged_keys(const std::vector<entry>& entries,
                      std::unordered_map<std::string, size_t>& at_key,
                      size_t first_own, ecuio_signal_file& f)
{
    const bool merged = !f.parts.empty();
    if (merged) {
        f.def_part.assign(first_own + entries.size(), ECUIO_SIGNAL_PART_NONE);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const Value& s = *entries[i].v;
        const std::string what = "signal " + q(entries[i].key);
        const Value* part = s.find("part");
        const Value* replaced = s.find("replaced");
        if (!merged) {
            for (const Value* v : {part, replaced}) {
                if (v != nullptr) {
                    refuse(*v, what + "'s \"" +
                                   (v == part ? "part" : "replaced") +
                                   "\" is a merged set's: a file with no "
                                   "\"parts\" takes neither \"part\" nor "
                                   "\"replaced\"");
                }
            }
            continue;
        }
        if (part == nullptr) {
            refuse(s, what + " needs a \"part\": each signal of a merged set "
                             "names the part it came from");
        }
        f.def_part[first_own + i] = static_cast<uint32_t>(
            whole_of(*part, 0, static_cast<int64_t>(f.parts.size()) - 1,
                     what + "'s part"));
        if (replaced == nullptr) {
            continue;
        }
        expect(*replaced, Type::array, what + "'s replaced");
        for (const Value& k : replaced->items) {
            expect(k, Type::string, what + "'s replaced key");
            if (!key_ok(k.text)) {
                refuse(k, what + "'s replaced key " + q(k.text) +
                              " takes 1 to 31 of a-z, 0-9 and _");
            }
            /* One key space: a signal's own key and the keys it replaced */
            if (!at_key.emplace(k.text, i).second) {
                refuse(k, what + "'s replaced key " + q(k.text) +
                              " is a key of the file already");
            }
            if (f.second_keys.size() >= ECUIO_SIGNAL_SET_MAX_SIGNALS) {
                refuse(k, "the file's signals replaced more than " +
                              std::to_string(ECUIO_SIGNAL_SET_MAX_SIGNALS) +
                              " keys");
            }
            f.second_keys.emplace_back(k.text,
                                       static_cast<uint32_t>(first_own + i));
        }
    }
}

} // namespace ecuio_sf
