// File/layer: core/src/plugin_manifest.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest, PluginWrapTest, PluginConversationTest
//          (test_signals); FUZZ: fuzz_plugin_manifest
//
// The plugin loader's reading (ADR-0052): the manifest's keys, each group
// in a function of its own (json.h, the signal-file loader's rules for
// unknown keys and words), and the package's data files, a packed one by
// its name and a folder's through the host's callback. The conversation
// tier's group is plugin_conversation.cpp's.

#include "plugin_loader.h"

#include "ecuio/internal.h"
#include "ecuio/profile.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_plugin_loader {

bool path_ok(const std::string& p)
{
    if (p.empty() || p.size() > ECUIO_PLUGIN_PATH_MAX || p.front() == '/') {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            const std::string part = p.substr(start, i - start);
            if (part.empty() || part == "." || part == "..") {
                return false;
            }
            start = i + 1;
            continue;
        }
        const char c = p[i];
        if (c < 0x21 || c > 0x7E || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

const std::string& path_of(const Value& v, std::string_view what)
{
    const std::string& p = text_of(v, 1, ECUIO_PLUGIN_PATH_MAX, what);
    if (!path_ok(p)) {
        refuse(v,
               std::string(what) + " " + q(p) +
                   " isn't a path in the package: relative, forward slashes, "
                   "no \".\" or \"..\" part");
    }
    return p;
}

namespace {

year_range years_of(const Value& v, const std::string& what)
{
    expect(v, Type::array, what);
    if (v.items.size() != 2) {
        refuse(v, what + " takes two model years, the first and the last");
    }
    year_range r;
    r.given = true;
    r.lo = whole_of(v.items[0], 1980, 2039, what + "'s first year");
    r.hi = whole_of(v.items[1], 1980, 2039, what + "'s last year");
    if (r.lo > r.hi) {
        refuse(v, what + " runs from " + std::to_string(r.lo) + " back to " +
                      std::to_string(r.hi));
    }
    return r;
}

/* Whether v is at, or holds, target */
bool holds(const Value& v, const Value* target)
{
    if (&v == target) {
        return true;
    }
    for (const Value& item : v.items) {
        if (holds(item, target)) {
            return true;
        }
    }
    for (const Member& m : v.members) {
        if (holds(m.value, target)) {
            return true;
        }
    }
    return false;
}

} // namespace

placed placed_json(const std::string& prefix, const ecuio_json::Error& error)
{
    placed p{prefix + error.to_string()};
    p.out_of_memory = error.out_of_memory;
    return p;
}

std::unique_ptr<ecuio_plugin> loader::load()
{
    ecuio_json::Limits limits;
    limits.max_bytes =
        packed() ? ECUIO_PLUGIN_PACKAGE_MAX : ECUIO_PLUGIN_MANIFEST_MAX;
    ecuio_json::Error error;
    if (!ecuio_json::parse(bytes_ != nullptr ? bytes_ : "", length_, &manifest_,
                           &error, limits)) {
        throw placed_json(where_manifest(), error);
    }
    total_ = length_;
    std::unique_ptr<ecuio_plugin> p(new ecuio_plugin());
    try {
        read_manifest(*p);
    } catch (const refused& r) {
        throw placed{place(r)};
    }
    return p;
}

std::string loader::where_manifest() const
{
    return packed() ? std::string() : std::string("manifest.json, ");
}

/* "<file>, line L, column C: why" for a folder, the line alone for
 * a packed package, whose lines are all one file's */
std::string loader::place(const refused& r) const
{
    std::string where = where_manifest();
    if (!packed()) {
        for (const data_file& f : files_) {
            if (f.owned != nullptr && holds(*f.owned, r.at)) {
                where = f.path + ", ";
                break;
            }
        }
        for (size_t i = 0; i < page_values_.size(); ++i) {
            if (holds(page_values_[i], r.at)) {
                where = page_paths_[i] + ", ";
                break;
            }
        }
    }
    return where + "line " + std::to_string(r.line) + ", column " +
           std::to_string(r.column) + ": " + r.words;
}

/* The manifest's keys in their order, then each variant built */
void loader::read_manifest(ecuio_plugin& p)
{
    const Value& root = manifest_;
    read_format(root, p);
    read_identity(root, p);
    read_requires(root, p);
    read_origin(root, p);
    read_tiers(root, p);
    read_match(root, p);
    read_fixtures(root, p);
    read_files(root, p);
    read_pages(root, p);
    const std::vector<std::vector<const data_file*>> lists =
        read_variants(root, p);
    const Value* variants = root.find("variants");

    /* A packed file no variant reads is a mistake, not a spare */
    for (const data_file& f : files_) {
        bool used = false;
        for (const auto& l : lists) {
            for (const data_file* c : l) {
                used = used || c == &f;
            }
        }
        if (!used) {
            refuse(*f.root, "file " + q(f.path) + " is in no variant");
        }
    }

    for (data_file& f : files_) {
        check_data_file(f);
    }
    for (size_t i = 0; i < p.variants.size(); ++i) {
        build_variant(p, variants->items[i], lists[i], p.variants[i]);
        ecuio_least::raise(p.least, p.variants[i].file->least);
    }
    /* After the variants: the manifest's "ecus" are judged by then */
    read_conversation(root, p);
}

/* "havne_plugin", first, and the keys a manifest may hold */
void loader::read_format(const Value& root, ecuio_plugin& p)
{
    expect(root, Type::object, "the manifest");
    if (root.members.empty() || root.members.front().key != "havne_plugin") {
        refuse(root, "a plugin's manifest starts with \"havne_plugin\"");
    }
    const Value& format = root.members.front().value;
    expect(format, Type::number, "\"havne_plugin\"");
    if (!format.integer || format.whole < ECUIO_PLUGIN_FORMAT ||
        format.whole > ECUIO_PLUGIN_FORMAT_MAX) {
        refuse(format, "\"havne_plugin\" is format " +
                           (format.integer ? std::to_string(format.whole)
                                           : std::string("?")) +
                           ", and this Havne reads formats 1 and 2");
    }
    p.format = static_cast<uint32_t>(format.whole);
    ecuio_least::raise(p.least, ecuio_least::kPluginFormatSince[p.format]);
    only_keys(root,
              {"havne_plugin", "id", "name", "version", "requires", "lineage",
               "source", "tiers", "match", "ecus", "variants", "fixtures",
               "files", "conversation", "pages"},
              "the manifest");
}

/* "id", "name" and "version" */
void loader::read_identity(const Value& root, ecuio_plugin& p)
{
    const Value* id = root.find("id");
    if (id == nullptr) {
        refuse(root, "the manifest needs an \"id\"");
    }
    p.id = text_of(*id, 1, ECUIO_PLUGIN_ID_MAX, "\"id\"");
    if (!id_ok(p.id, ECUIO_PLUGIN_ID_MAX)) {
        refuse(*id, "\"id\" " + q(p.id) +
                        " takes 1 to 31 of a-z, 0-9 and -, a letter or digit "
                        "at each end");
    }
    const Value* name = root.find("name");
    if (name == nullptr) {
        refuse(root, "the manifest needs a \"name\"");
    }
    p.name = text_of(*name, 1, 63, "\"name\"");
    /* Each variant's signal file takes its profile name from it */
    const std::string profile = ecuio_sf::profile_of(p.name);
    if (profile.empty()) {
        refuse(*name, "\"name\" " + q(p.name) +
                          " holds no letter or digit, which a variant's "
                          "signal file makes its profile name from");
    }
    if (profile.size() > ECUIO_PROFILE_NAME_MAX) {
        refuse(*name, "\"name\" makes a variant's signal file a profile "
                      "name longer than " +
                          std::to_string(ECUIO_PROFILE_NAME_MAX) +
                          " characters");
    }

    const Value* version = root.find("version");
    if (version == nullptr) {
        refuse(root,
               "the manifest needs a \"version\": the plugin's own X.Y.Z");
    }
    p.version = text_of(*version, 1, 15, "\"version\"");
    uint32_t own[3];
    if (!parse_version(p.version, own)) {
        refuse(*version, "\"version\" " + q(p.version) + " isn't X.Y.Z");
    }
}

/* "requires": the oldest core that reads the plugin */
void loader::read_requires(const Value& root, ecuio_plugin& p)
{
    const Value* requires_v = root.find("requires");
    if (requires_v == nullptr) {
        refuse(root, "the manifest needs \"requires\": the oldest core that "
                     "reads it");
    }
    expect(*requires_v, Type::object, "\"requires\"");
    only_keys(*requires_v, {"core"}, "\"requires\"");
    const Value* needs = requires_v->find("core");
    if (needs == nullptr) {
        refuse(*requires_v, "\"requires\" needs \"core\"");
    }
    const std::string needed = text_of(*needs, 1, 15, "\"requires\"'s core");
    uint32_t want[3];
    if (!parse_version(needed, want)) {
        refuse(*needs, "\"requires\"'s core " + q(needed) + " isn't X.Y.Z");
    }
    for (int i = 0; i < 3; ++i) {
        p.requires_core[i] = static_cast<uint16_t>(want[i]);
    }
    uint32_t have[3];
    core_version(have);
    if (older(have, want)) {
        refuse(*needs, "the plugin needs core " + needed +
                           " or later, and this "
                           "is " +
                           ecuio_version_string());
    }
}

/* "lineage" and "source": where the plugin comes from */
void loader::read_origin(const Value& root, ecuio_plugin& p)
{
    if (const Value* lineage = root.find("lineage")) {
        expect(*lineage, Type::string, "\"lineage\"");
        if (!ecuio_plugin_rules::lineage_ok(lineage->text,
                                            ECUIO_PLUGIN_LINEAGE_LEN)) {
            refuse(*lineage, "\"lineage\" " + q(lineage->text) +
                                 " takes 32 of 0-9 and a-f");
        }
        p.lineage = lineage->text;
        ecuio_least::raise(p.least, ecuio_least::kLineage);
    }

    const Value* source = root.find("source");
    if (source == nullptr) {
        refuse(root,
               "the manifest needs a \"source\": where its signals come from");
    }
    ecuio_sf::read_source(*source);
}

void loader::read_tiers(const Value& root, ecuio_plugin& p)
{
    const Value* tiers = root.find("tiers");
    if (tiers == nullptr) {
        refuse(root, "the manifest needs \"tiers\": [\"data\"]");
    }
    expect(*tiers, Type::array, "\"tiers\"");
    if (tiers->items.empty()) {
        refuse(*tiers, "\"tiers\" lists none");
    }
    for (const Value& t : tiers->items) {
        expect(t, Type::string, "a tier");
        if (t.text != "data" && t.text != "conversation") {
            refuse(t, "tier " + q(t.text) +
                          (p.format < 2
                               ? " isn't one format 1 takes: only \"data\""
                               : " isn't one this Havne runs: \"data\" "
                                 "and \"conversation\""));
        }
        const bool data = t.text == "data";
        if (!data && p.format < 2) {
            refuse(t, "tier \"conversation\" is format 2's: its manifest "
                      "starts with \"havne_plugin\": 2");
        }
        const uint32_t bit =
            data ? ECUIO_PLUGIN_TIER_DATA : ECUIO_PLUGIN_TIER_CONVERSATION;
        if ((p.tiers & bit) != 0) {
            refuse(t, "tier " + q(t.text) + " is listed twice");
        }
        p.tiers |= bit;
    }
    /* The variants are data, so the tier is every plugin's */
    if ((p.tiers & ECUIO_PLUGIN_TIER_DATA) == 0) {
        refuse(*tiers, "\"tiers\" lists \"data\" too: a plugin's variants "
                       "are signal files");
    }
}

void loader::read_match(const Value& root, ecuio_plugin& p)
{
    const Value* match = root.find("match");
    if (match == nullptr) {
        return;
    }
    expect(*match, Type::array, "\"match\"");
    if (match->items.size() > 32) {
        refuse(*match, "\"match\" lists more than 32");
    }
    for (const Value& m : match->items) {
        expect(m, Type::object, "a match");
        only_keys(m, {"wmi", "years"}, "a match");
        const Value* wmi = m.find("wmi");
        if (wmi == nullptr) {
            refuse(m, "a match needs \"wmi\": the manufacturer codes");
        }
        expect(*wmi, Type::array, "a match's wmi");
        if (wmi->items.empty() || wmi->items.size() > 64) {
            refuse(*wmi, "a match's wmi lists 1 to 64 codes");
        }
        match_rule rule;
        for (const Value& w : wmi->items) {
            const std::string code = text_of(w, 1, 6, "a manufacturer code");
            if (!wmi_ok(code)) {
                refuse(w, "manufacturer code " + q(code) +
                              " takes 3 or 6 of a VIN's letters and digits, in "
                              "upper case");
            }
            rule.wmi.push_back(code);
        }
        if (const Value* years = m.find("years")) {
            rule.years = years_of(*years, "a match's years");
        }
        p.match.push_back(std::move(rule));
    }
}

/* "fixtures": a folder's fixture files, each once */
void loader::read_fixtures(const Value& root, ecuio_plugin& p)
{
    if (const Value* fixtures = root.find("fixtures")) {
        if (packed()) {
            refuse(*fixtures, "a packed plugin carries no \"fixtures\": they "
                              "run from its folder");
        }
        expect(*fixtures, Type::array, "\"fixtures\"");
        if (fixtures->items.size() > ECUIO_PLUGIN_MAX_FIXTURES) {
            refuse(*fixtures, "\"fixtures\" lists more than 16");
        }
        for (const Value& f : fixtures->items) {
            const std::string path = path_of(f, "a fixture");
            for (const std::string& known : p.fixtures) {
                if (known == path) {
                    refuse(f, "fixture " + q(path) + " is listed twice");
                }
            }
            p.fixtures.push_back(path);
        }
    }
}

/* "files": a packed plugin's files by their paths: a data file, or with
 * a "kind" a binary one (format 2) */
void loader::read_files(const Value& root, const ecuio_plugin& p)
{
    const Value* files = root.find("files");
    if (files != nullptr && !packed()) {
        refuse(*files,
               "\"files\" is a packed plugin's: a folder's manifest names "
               "its files in its variants");
    }
    if (packed()) {
        if (files == nullptr) {
            refuse(root,
                   "a packed plugin needs \"files\": each data file by its "
                   "path");
        }
        expect(*files, Type::object, "\"files\"");
        /* The pages' room beside the data and binary files' 64 */
        if (files->members.size() >
            ECUIO_PLUGIN_MAX_FILES + ECUIO_PLUGIN_MAX_PAGES) {
            refuse(*files, "\"files\" holds more than 80");
        }
        for (const Member& m : files->members) {
            if (!path_ok(m.key)) {
                refuse(m, "file " + q(m.key) +
                              " isn't a path in the package: relative, forward "
                              "slashes, no \".\" or \"..\" part");
            }
            if (m.value.type == Type::object &&
                m.value.find("kind") != nullptr) {
                if (p.format < 2) {
                    refuse(m, "file " + q(m.key) +
                                  " is a binary file, which format 2 "
                                  "carries: \"havne_plugin\": 2");
                }
                binary_file b;
                b.path = m.key;
                b.root = &m.value;
                binaries_.push_back(std::move(b));
                continue;
            }
            data_file f;
            f.path = m.key;
            f.root = &m.value;
            files_.push_back(std::move(f));
        }
    }
}

/* "variants": each one's id, years and files, into p.variants; the
 * files each names, in its order */
std::vector<std::vector<const data_file*>>
loader::read_variants(const Value& root, ecuio_plugin& p)
{
    const Value* variants = root.find("variants");
    if (variants == nullptr) {
        refuse(root, "the manifest needs \"variants\"");
    }
    expect(*variants, Type::array, "\"variants\"");
    if (variants->items.empty()) {
        refuse(*variants, "\"variants\" lists none");
    }
    if (variants->items.size() > ECUIO_PLUGIN_MAX_VARIANTS) {
        refuse(*variants, "\"variants\" lists more than 32");
    }

    /* Each variant's files, read once each */
    std::vector<std::vector<const data_file*>> lists;
    for (const Value& v : variants->items) {
        expect(v, Type::object, "a variant");
        only_keys(v, {"id", "years", "files"}, "a variant");
        const Value* vid = v.find("id");
        if (vid == nullptr) {
            refuse(v, "a variant needs an \"id\"");
        }
        variant out;
        out.id =
            text_of(*vid, 1, ECUIO_PLUGIN_VARIANT_ID_MAX, "a variant's id");
        if (!id_ok(out.id, ECUIO_PLUGIN_VARIANT_ID_MAX)) {
            refuse(*vid,
                   "variant " + q(out.id) +
                       " takes 1 to 24 of a-z, 0-9 and -, a letter or digit "
                       "at each end");
        }
        for (const variant& known : p.variants) {
            if (known.id == out.id) {
                refuse(*vid, "variant " + q(out.id) + " is given twice");
            }
        }
        const std::string what = "variant " + q(out.id);
        if (const Value* years = v.find("years")) {
            out.years = years_of(*years, what + "'s years");
        }
        const Value* list = v.find("files");
        if (list == nullptr) {
            refuse(v, what + " needs \"files\"");
        }
        expect(*list, Type::array, what + "'s files");
        if (list->items.empty()) {
            refuse(*list, what + " lists no files");
        }
        if (list->items.size() > ECUIO_PLUGIN_MAX_FILES) {
            refuse(*list, what + " lists more than 64 files");
        }
        std::vector<const data_file*> chosen;
        for (const Value& f : list->items) {
            const std::string path = path_of(f, what + "'s file");
            for (const data_file* c : chosen) {
                if (c->path == path) {
                    refuse(f, what + " lists " + q(path) + " twice");
                }
            }
            chosen.push_back(file_at(f, path));
        }
        out.profile = "plugin." + p.id + "." + out.id;
        p.variants.push_back(std::move(out));
        lists.push_back(std::move(chosen));
    }
    return lists;
}

/* The package's file at path: a packed one by name, a folder's read
 * through the host's callback the first time it's named */
const data_file* loader::file_at(const Value& at, const std::string& path)
{
    for (const data_file& f : files_) {
        if (f.path == path) {
            return &f;
        }
    }
    if (is_page(path)) {
        refuse(at, "file " + q(path) + " is a page, which no variant reads");
    }
    if (packed()) {
        refuse(at, "file " + q(path) + " isn't in \"files\"");
    }
    if (files_.size() >= ECUIO_PLUGIN_MAX_FILES) {
        refuse(at, "the variants name more than 64 files");
    }
    const char* bytes = nullptr;
    size_t length = 0;
    if (read_(user_, path.c_str(), &bytes, &length) != 0 ||
        (bytes == nullptr && length > 0)) {
        refuse(at, "file " + q(path) + " couldn't be read");
    }
    if (length > ECUIO_SIGNAL_FILE_MAX_BYTES) {
        refuse(at, "file " + q(path) + " is over 1 MiB");
    }
    total_ += length;
    if (total_ > ECUIO_PLUGIN_PACKAGE_MAX) {
        refuse(at, "the package is over 16 MiB");
    }
    data_file f;
    f.path = path;
    f.owned.reset(new Value());
    ecuio_json::Error error;
    ecuio_json::Limits limits;
    limits.max_bytes = ECUIO_SIGNAL_FILE_MAX_BYTES;
    if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, f.owned.get(),
                           &error, limits)) {
        throw placed_json(path + ", ", error);
    }
    f.root = f.owned.get();
    files_.push_back(std::move(f));
    return &files_.back();
}

/* A data file is a signal file that names the manifest's ECUs */
void loader::check_data_file(data_file& f)
{
    const Value& root = *f.root;
    expect(root, Type::object, "a data file");
    f.format = ecuio_sf::format_of(root, "a data file");
    if (const Value* ecus = root.find("ecus")) {
        refuse(*ecus, "a plugin's data file declares no \"ecus\": it names the "
                      "manifest's by id");
    }
    only_keys(root, {"havne_signals", "name", "source", "extends", "signals"},
              "a data file");
    if (const Value* name = root.find("name")) {
        text_of(*name, 1, 63, "\"name\"");
    }
    if (const Value* source = root.find("source")) {
        ecuio_sf::read_source(*source);
    }
    const Value* signals = root.find("signals");
    if (signals == nullptr) {
        refuse(root, "a data file needs \"signals\"");
    }
    expect(*signals, Type::array, "\"signals\"");
    /* A data file's own bound; the variant's merged list has a set's */
    if (signals->items.size() > ECUIO_SIGNAL_FILE_MAX_SIGNALS) {
        refuse(*signals, "\"signals\" lists " +
                             std::to_string(signals->items.size()) +
                             ", more than " +
                             std::to_string(ECUIO_SIGNAL_FILE_MAX_SIGNALS));
    }
}

} // namespace ecuio_plugin_loader
