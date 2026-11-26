// File/layer: core/src/plugin_merge.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest, PluginWrapTest (test_signals); HARNESS: RecorderTest
//          (test_core); FUZZ: fuzz_plugin_manifest
//
// The plugin loader's variants (ADR-0052): each variant's files merged by
// signal key and built into a set by the signal-file loader's own build
// (signal_file_build.h), so a plugin's signals pass exactly the rules a
// signal file's do, and the one signal file a recording copies for it.

#include "plugin_loader.h"

#include "ecuio/internal.h"
#include "error_detail.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecuio_plugin_loader {

namespace {

/* Whether a data file's own source says what the manifest's does, which
 * an older core takes for the variant whatever the file holds */
bool same_source(const Value& manifest, const ecuio_signal_source_t& own)
{
    ecuio_signal_source_t m{};
    ecuio_sf::take_source(manifest, m);
    return std::strcmp(m.name, own.name) == 0 &&
           std::strcmp(m.url, own.url) == 0 &&
           std::strcmp(m.commit, own.commit) == 0 &&
           std::strcmp(m.license, own.license) == 0 &&
           std::strcmp(m.modified, own.modified) == 0;
}

/* What one file needs and another of the variant gives (slot_of holds
 * the variant's keys) is met in the variant; what two need is one need */
void meet_needs(ecuio_sf::parts& parts,
                const std::unordered_map<std::string, size_t>& slot_of)
{
    std::vector<std::pair<std::string, const Value*>> unmet;
    for (const auto& need : parts.needs) {
        const bool again =
            std::any_of(unmet.begin(), unmet.end(), [&need](const auto& u) {
                return u.first == need.first;
            });
        if (!again && slot_of.count(need.first) == 0) {
            unmet.push_back(need);
        }
    }
    parts.needs.swap(unmet);
}

} // namespace

/* The variant's files' signals merged by key, a later file's
 * replacing an earlier one's in its place, then built */
void loader::build_variant(const ecuio_plugin& p, const Value& at,
                           const std::vector<const data_file*>& list,
                           variant& out)
{
    ecuio_sf::parts parts;
    parts.at = &at;
    parts.name = p.name;
    parts.profile = out.profile;
    parts.ecus = manifest_.find("ecus");
    parts.signals_at = &at;
    parts.max_signals = ECUIO_SIGNAL_SET_MAX_SIGNALS;
    /* The first of the variant's files that has a source of its own
     * names where the variant comes from, else the manifest's does */
    const Value* source = manifest_.find("source");
    bool own_source = false;
    std::unordered_map<std::string, size_t> slot_of;
    for (size_t n = 0; n < list.size(); ++n) {
        const Value& root = *list[n]->root;
        parts.format = std::max(parts.format, list[n]->format);
        ecuio_sf::read_needs(root, list[n]->format, &parts.needs);
        if (const Value* own = root.find("source")) {
            if (!own_source) {
                source = own;
                own_source = true;
            }
        }
        if (const Value* extends = root.find("extends")) {
            if (n != 0) {
                refuse(*extends,
                       "only a variant's first file extends a set, and "
                       "this is file " +
                           std::to_string(n + 1) + " of variant " + q(out.id));
            }
            parts.extends = extends;
        }
        std::vector<std::string> mine;
        for (const Value& s : root.find("signals")->items) {
            const Value* key = s.type == Type::object ? s.find("key") : nullptr;
            if (key == nullptr || key->type != Type::string) {
                parts.signals.push_back(&s); /* build says why */
                parts.formats.push_back(list[n]->format);
                continue;
            }
            for (const std::string& k : mine) {
                if (k == key->text) {
                    refuse(*key, "key " + q(k) + " is given twice");
                }
            }
            mine.push_back(key->text);
            auto it = slot_of.find(key->text);
            if (it != slot_of.end()) {
                parts.signals[it->second] = &s;
                parts.formats[it->second] = list[n]->format;
            } else {
                slot_of.emplace(key->text, parts.signals.size());
                parts.signals.push_back(&s);
                parts.formats.push_back(list[n]->format);
            }
        }
    }
    meet_needs(parts, slot_of);
    out.file.reset(new ecuio_signal_file());
    ecuio_sf::build(parts, *out.file);
    if (source != nullptr) {
        ecuio_sf::take_source(*source, out.file->source);
    }
    if (own_source &&
        !same_source(*manifest_.find("source"), out.file->source)) {
        ecuio_least::raise(out.file->least, ecuio_least::kOwnSource);
    }
    out.file->plugin = p.id + "," + p.version + "," + out.id;
    std::string why;
    out.file->bytes = variant_json(p, out, parts, source, &why);
    if (out.file->bytes.empty()) {
        refuse(at, "variant " + q(out.id) +
                       "'s files don't make one signal file, which a "
                       "recording copies: " +
                       why);
    }
    if (out.file->bytes.size() > ECUIO_SIGNAL_FILE_MAX_BYTES) {
        ecuio_least::raise(out.file->least, ecuio_least::kSetBytes);
    }
}

/* The variant as one signal file, what a recording copies as
 * signals.json: the plugin's name, the variant's source, the
 * manifest's ECUs, the first file's extends, the merged signals in
 * their order. Empty, with why, when the text doesn't read back to
 * the set just built, so a recording never holds a copy that decodes
 * otherwise. */
std::string loader::variant_json(const ecuio_plugin& p, const variant& v,
                                 const ecuio_sf::parts& parts,
                                 const Value* source, std::string* why) const
{
    Value doc;
    doc.type = Type::object;
    auto add = [&doc](const char* key, Value value) {
        Member m;
        m.key = key;
        m.value = std::move(value);
        doc.members.push_back(std::move(m));
    };
    Value format;
    format.type = Type::number;
    format.integer = true;
    format.in_int64 = true;
    /* The newest of the variant's files: each signal reads alike in
     * it, since no format 1 expression changes its meaning in 2 */
    format.whole = parts.format;
    format.number = parts.format;
    add("havne_signals", format);
    Value text;
    text.type = Type::string;
    text.text = p.name;
    add("name", text);
    text.text = "Plugin " + p.id + " " + p.version + ", variant " + v.id +
                ": its files' signals merged";
    add("notes", text);
    if (source != nullptr) {
        add("source", *source);
    }
    if (parts.extends != nullptr) {
        add("extends", *parts.extends);
    }
    if (!parts.needs.empty()) {
        Value needs;
        needs.type = Type::array;
        for (const auto& need : parts.needs) {
            needs.items.push_back(*need.second);
        }
        add("needs", std::move(needs));
    }
    if (parts.ecus != nullptr) {
        add("ecus", *parts.ecus);
    }
    Value list;
    list.type = Type::array;
    for (const Value* s : parts.signals) {
        list.items.push_back(*s);
    }
    add("signals", std::move(list));
    std::string json = ecuio_json::write(doc);

    ecuio_signal_file_t* back = nullptr;
    const ecuio_status_t status = ecuio_signal_file_load_with(
        json.data(), json.size(), ECUIO_SIGNAL_FILE_LOAD_SET, &back);
    if (status == ECUIO_ERR_OUT_OF_MEMORY) {
        ecuio_detail_clear();
        throw std::bad_alloc(); /* the load's answer, never a refusal's words */
    }
    const bool read = status == ECUIO_OK;
    const bool same = read && same_set(back->set, v.file->set);
    if (!read) {
        /* The loader's words, past the place in the written text */
        const std::string words = ecuio_last_error_detail();
        const size_t colon = words.find(": ");
        *why = colon == std::string::npos ? words : words.substr(colon + 2);
    } else if (!same) {
        *why = "it reads back as another set";
    }
    ecuio_signal_file_free(back);
    ecuio_detail_clear();
    return same ? json : std::string();
}

/* Two sets decode alike: every count and table equal, byte for byte */
bool loader::same_set(const ecuio_signal_set_t& a, const ecuio_signal_set_t& b)
{
    auto same = [](const void* x, const void* y, size_t n) {
        return n == 0 || std::memcmp(x, y, n) == 0;
    };
    return a.layout_version == b.layout_version && a.count == b.count &&
           a.channel_count == b.channel_count && a.enum_count == b.enum_count &&
           a.computed_count == b.computed_count &&
           same(a.defs, b.defs, sizeof *a.defs * a.count) &&
           same(a.channels, b.channels, sizeof *a.channels * a.channel_count) &&
           same(a.enums, b.enums, sizeof *a.enums * a.enum_count) &&
           same(a.computed, b.computed, sizeof *a.computed * a.computed_count);
}

} // namespace ecuio_plugin_loader
