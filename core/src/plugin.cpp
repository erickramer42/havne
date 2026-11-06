// File/layer: core/src/plugin.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest (test_signals); FUZZ: fuzz_plugin_manifest
//
// The plugin loader's data tier (ADR-0052): a manifest (json.h, the
// signal-file loader's rules for unknown keys and words) and its data
// files, each variant's files merged by signal key and built into a set
// by the signal-file loader's own build (signal_file_build.h), so a
// plugin's signals pass exactly the rules a signal file's do.

#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "error_detail.h"
#include "json.h"
#include "profile_hold.h"
#include "signal_file_build.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_sf::expect;
using ecuio_sf::only_keys;
using ecuio_sf::q;
using ecuio_sf::refuse;
using ecuio_sf::refused;
using ecuio_sf::text_of;
using ecuio_sf::whole_of;

struct year_range {
    bool given = false;
    int64_t lo = 0;
    int64_t hi = 0;

    bool covers(uint16_t year) const
    {
        return !given || (year != 0 && year >= lo && year <= hi);
    }
};

struct match_rule {
    std::vector<std::string> wmi;
    year_range years;
};

struct variant {
    std::string id;
    std::string profile;
    year_range years;
    std::unique_ptr<ecuio_signal_file> file;
    bool registered = false;
};

/* A data file of the package, parsed */
struct data_file {
    std::string path;
    const Value* root = nullptr;  /* into the manifest when packed */
    std::unique_ptr<Value> owned; /* a folder's file */
};

} // namespace

struct ecuio_plugin {
    std::string id;
    std::string name;
    std::string version;
    uint32_t tiers = 0;
    std::vector<match_rule> match;
    std::vector<variant> variants;
    std::vector<std::string> fixtures;
};

namespace {

/* [a-z0-9-], lo to hi bytes, a letter or digit at each end */
bool id_ok(const std::string& s, size_t hi)
{
    if (s.empty() || s.size() > hi || s.front() == '-' || s.back() == '-') {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* X.Y.Z, each 0 to 65535 with no leading zero; false otherwise */
bool parse_version(const std::string& s, uint32_t out[3])
{
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return false;
        }
        if (s[i] == '0' && i + 1 < s.size() && s[i + 1] >= '0' &&
            s[i + 1] <= '9') {
            return false;
        }
        uint32_t n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + static_cast<uint32_t>(s[i] - '0');
            if (n > 65535) {
                return false;
            }
            ++i;
        }
        out[part] = n;
        if (part < 2) {
            if (i >= s.size() || s[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    return i == s.size();
}

/* The leading X.Y.Z of core's own version (a suffix after it ignored) */
void core_version(uint32_t out[3])
{
    const std::string v = ecuio_version_string();
    size_t end = 0;
    int dots = 0;
    while (end < v.size() &&
           ((v[end] >= '0' && v[end] <= '9') || v[end] == '.')) {
        if (v[end] == '.' && ++dots == 3) {
            break;
        }
        ++end;
    }
    if (!parse_version(v.substr(0, end), out)) {
        out[0] = out[1] = out[2] = 0;
    }
}

bool older(const uint32_t a[3], const uint32_t b[3])
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i];
        }
    }
    return false;
}

/* Relative, forward slashes, no "." or ".." part, no empty part, 1 to
 * ECUIO_PLUGIN_PATH_MAX printable ASCII bytes but backslash and colon */
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

const std::string& path_of(const Value& v, const std::string& what)
{
    const std::string& p = text_of(v, 1, ECUIO_PLUGIN_PATH_MAX, what);
    if (!path_ok(p)) {
        refuse(v,
               what + " " + q(p) +
                   " isn't a path in the package: relative, forward slashes, "
                   "no \".\" or \"..\" part");
    }
    return p;
}

/* 3 or 6 of the VIN alphabet (no I, O or Q), as ecuio_vin_info_t.wmi */
bool wmi_ok(const std::string& s)
{
    if (s.size() != 3 && s.size() != 6) {
        return false;
    }
    for (char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter =
            c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q';
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

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

/* Thrown with the words already placed: a file the reader couldn't
 * give, a data file's JSON refused */
struct placed {
    std::string words;
};

class loader
{
public:
    loader(const char* bytes, size_t length, ecuio_plugin_read_t read,
           void* user)
        : bytes_(bytes), length_(length), read_(read), user_(user)
    {}

    std::unique_ptr<ecuio_plugin> load()
    {
        ecuio_json::Limits limits;
        limits.max_bytes =
            packed() ? ECUIO_PLUGIN_PACKAGE_MAX : ECUIO_PLUGIN_MANIFEST_MAX;
        ecuio_json::Error error;
        if (!ecuio_json::parse(bytes_ != nullptr ? bytes_ : "", length_,
                               &manifest_, &error, limits)) {
            throw placed{where_manifest() + error.to_string()};
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

private:
    bool packed() const { return read_ == nullptr; }

    std::string where_manifest() const
    {
        return packed() ? std::string() : std::string("manifest.json, ");
    }

    /* "<file>, line L, column C: why" for a folder, the line alone for
     * a packed package, whose lines are all one file's */
    std::string place(const refused& r) const
    {
        std::string where = where_manifest();
        if (!packed()) {
            for (const data_file& f : files_) {
                if (f.owned != nullptr && holds(*f.owned, r.at)) {
                    where = f.path + ", ";
                    break;
                }
            }
        }
        return where + "line " + std::to_string(r.line) + ", column " +
               std::to_string(r.column) + ": " + r.words;
    }

    void read_manifest(ecuio_plugin& p)
    {
        const Value& root = manifest_;
        expect(root, Type::object, "the manifest");
        if (root.members.empty() ||
            root.members.front().key != "havne_plugin") {
            refuse(root, "a plugin's manifest starts with \"havne_plugin\"");
        }
        const Value& format = root.members.front().value;
        expect(format, Type::number, "\"havne_plugin\"");
        if (!format.integer || format.whole != ECUIO_PLUGIN_FORMAT) {
            refuse(format, "\"havne_plugin\" is format " +
                               (format.integer ? std::to_string(format.whole)
                                               : std::string("?")) +
                               ", and this Havne reads format 1");
        }
        only_keys(root,
                  {"havne_plugin", "id", "name", "version", "requires",
                   "source", "tiers", "match", "ecus", "variants", "fixtures",
                   "files"},
                  "the manifest");

        const Value* id = root.find("id");
        if (id == nullptr) {
            refuse(root, "the manifest needs an \"id\"");
        }
        p.id = text_of(*id, 1, ECUIO_PLUGIN_ID_MAX, "\"id\"");
        if (!id_ok(p.id, ECUIO_PLUGIN_ID_MAX)) {
            refuse(*id,
                   "\"id\" " + q(p.id) +
                       " takes 1 to 31 of a-z, 0-9 and -, a letter or digit "
                       "at each end");
        }
        const Value* name = root.find("name");
        if (name == nullptr) {
            refuse(root, "the manifest needs a \"name\"");
        }
        p.name = text_of(*name, 1, 63, "\"name\"");

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

        const Value* requires_v = root.find("requires");
        if (requires_v == nullptr) {
            refuse(root,
                   "the manifest needs \"requires\": the oldest core that "
                   "reads it");
        }
        expect(*requires_v, Type::object, "\"requires\"");
        only_keys(*requires_v, {"core"}, "\"requires\"");
        const Value* needs = requires_v->find("core");
        if (needs == nullptr) {
            refuse(*requires_v, "\"requires\" needs \"core\"");
        }
        const std::string needed =
            text_of(*needs, 1, 15, "\"requires\"'s core");
        uint32_t want[3];
        if (!parse_version(needed, want)) {
            refuse(*needs, "\"requires\"'s core " + q(needed) + " isn't X.Y.Z");
        }
        uint32_t have[3];
        core_version(have);
        if (older(have, want)) {
            refuse(*needs, "the plugin needs core " + needed +
                               " or later, and this "
                               "is " +
                               ecuio_version_string());
        }

        const Value* source = root.find("source");
        if (source == nullptr) {
            refuse(
                root,
                "the manifest needs a \"source\": where its signals come from");
        }
        ecuio_sf::read_source(*source);

        read_tiers(root, p);
        read_match(root, p);

        if (const Value* fixtures = root.find("fixtures")) {
            if (packed()) {
                refuse(*fixtures,
                       "a packed plugin carries no \"fixtures\": they "
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
            if (files->members.size() > ECUIO_PLUGIN_MAX_FILES) {
                refuse(*files, "\"files\" holds more than 64");
            }
            for (const Member& m : files->members) {
                if (!path_ok(m.key)) {
                    refuse(
                        m,
                        "file " + q(m.key) +
                            " isn't a path in the package: relative, forward "
                            "slashes, no \".\" or \"..\" part");
                }
                data_file f;
                f.path = m.key;
                f.root = &m.value;
                files_.push_back(std::move(f));
            }
        }

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
                refuse(
                    *vid,
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

        for (const data_file& f : files_) {
            check_data_file(f);
        }
        for (size_t i = 0; i < p.variants.size(); ++i) {
            build_variant(p, variants->items[i], lists[i], p.variants[i]);
        }
    }

    void read_tiers(const Value& root, ecuio_plugin& p)
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
            if (t.text != "data") {
                refuse(t, "tier " + q(t.text) +
                              " isn't one this Havne runs: only \"data\"");
            }
            if ((p.tiers & ECUIO_PLUGIN_TIER_DATA) != 0) {
                refuse(t, "tier \"data\" is listed twice");
            }
            p.tiers |= ECUIO_PLUGIN_TIER_DATA;
        }
    }

    void read_match(const Value& root, ecuio_plugin& p)
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
                const std::string code =
                    text_of(w, 1, 6, "a manufacturer code");
                if (!wmi_ok(code)) {
                    refuse(
                        w,
                        "manufacturer code " + q(code) +
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

    /* The package's file at path: a packed one by name, a folder's read
     * through the host's callback the first time it's named */
    const data_file* file_at(const Value& at, const std::string& path)
    {
        for (const data_file& f : files_) {
            if (f.path == path) {
                return &f;
            }
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
        if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length,
                               f.owned.get(), &error, limits)) {
            throw placed{path + ", " + error.to_string()};
        }
        f.root = f.owned.get();
        files_.push_back(std::move(f));
        return &files_.back();
    }

    /* A data file is a signal file that names the manifest's ECUs */
    static void check_data_file(const data_file& f)
    {
        const Value& root = *f.root;
        expect(root, Type::object, "a data file");
        if (root.members.empty() ||
            root.members.front().key != "havne_signals") {
            refuse(root, "a data file starts with \"havne_signals\"");
        }
        const Value& version = root.members.front().value;
        expect(version, Type::number, "\"havne_signals\"");
        if (!version.integer || version.whole != ECUIO_SIGNAL_FILE_FORMAT) {
            refuse(version, "\"havne_signals\" is format " +
                                (version.integer ? std::to_string(version.whole)
                                                 : std::string("?")) +
                                ", and this Havne reads format 1");
        }
        if (const Value* ecus = root.find("ecus")) {
            refuse(*ecus,
                   "a plugin's data file declares no \"ecus\": it names the "
                   "manifest's by id");
        }
        only_keys(root,
                  {"havne_signals", "name", "source", "extends", "signals"},
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
    }

    /* The variant's files' signals merged by key, a later file's
     * replacing an earlier one's in its place, then built */
    void build_variant(const ecuio_plugin& p, const Value& at,
                       const std::vector<const data_file*>& list, variant& out)
    {
        ecuio_sf::parts parts;
        parts.at = &at;
        parts.name = p.name;
        parts.profile = out.profile;
        parts.ecus = manifest_.find("ecus");
        parts.signals_at = &at;
        std::unordered_map<std::string, size_t> slot_of;
        for (size_t n = 0; n < list.size(); ++n) {
            const Value& root = *list[n]->root;
            if (const Value* extends = root.find("extends")) {
                if (n != 0) {
                    refuse(*extends,
                           "only a variant's first file extends a set, and "
                           "this is file " +
                               std::to_string(n + 1) + " of variant " +
                               q(out.id));
                }
                parts.extends = extends;
            }
            std::vector<std::string> mine;
            for (const Value& s : root.find("signals")->items) {
                const Value* key =
                    s.type == Type::object ? s.find("key") : nullptr;
                if (key == nullptr || key->type != Type::string) {
                    parts.signals.push_back(&s); /* build says why */
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
                } else {
                    slot_of.emplace(key->text, parts.signals.size());
                    parts.signals.push_back(&s);
                }
            }
        }
        out.file.reset(new ecuio_signal_file());
        ecuio_sf::build(parts, *out.file);
    }

    const char* bytes_;
    size_t length_;
    ecuio_plugin_read_t read_;
    void* user_;
    size_t total_ = 0;
    Value manifest_;
    std::deque<data_file> files_; /* a deque: chosen lists point into it */
};

bool copy_out(const std::string& s, char* out, size_t cap)
{
    if (out == nullptr || cap < s.size() + 1) {
        return false;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return true;
}

void copy_field(char* dst, size_t cap, const std::string& s)
{
    size_t n = 0;
    for (; n + 1 < cap && n < s.size(); ++n) {
        dst[n] = s[n];
    }
    dst[n] = '\0';
}

/* A value fixture file's entries, or a placed thrown with its words
 * ("line L, column C: why"); std::bad_alloc too */
std::vector<ecuio_plugin_value_t> read_values(const ecuio_plugin& p,
                                              const char* bytes, size_t length)
{
    ecuio_json::Limits limits;
    limits.max_bytes = ECUIO_SIGNAL_FILE_MAX_BYTES;
    ecuio_json::Error error;
    Value root;
    if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root, &error,
                           limits)) {
        throw placed{error.to_string()};
    }
    std::vector<ecuio_plugin_value_t> out;
    try {
        expect(root, Type::object, "a fixture file");
        if (root.members.empty() ||
            root.members.front().key != "havne_fixtures") {
            refuse(root, "a fixture file starts with \"havne_fixtures\"");
        }
        const Value& format = root.members.front().value;
        expect(format, Type::number, "\"havne_fixtures\"");
        if (!format.integer || format.whole != ECUIO_PLUGIN_FIXTURES_FORMAT) {
            refuse(format, "\"havne_fixtures\" is format " +
                               (format.integer ? std::to_string(format.whole)
                                               : std::string("?")) +
                               ", and this Havne reads format 1");
        }
        only_keys(root, {"havne_fixtures", "values"}, "a fixture file");
        const Value* list = root.find("values");
        if (list == nullptr) {
            refuse(root, "a fixture file needs \"values\"");
        }
        expect(*list, Type::array, "\"values\"");
        if (list->items.empty() ||
            list->items.size() > ECUIO_PLUGIN_MAX_VALUES) {
            refuse(*list, "\"values\" lists 1 to 4096 entries");
        }
        std::vector<std::pair<uint32_t, uint16_t>> seen;
        for (const Value& e : list->items) {
            expect(e, Type::object, "a value fixture");
            only_keys(e, {"variant", "signal", "value"}, "a value fixture");
            const Value* variant = e.find("variant");
            const Value* signal = e.find("signal");
            const Value* value = e.find("value");
            if (variant == nullptr || signal == nullptr || value == nullptr) {
                refuse(e, "a value fixture needs \"variant\", \"signal\" "
                          "and \"value\"");
            }
            const std::string id = text_of(
                *variant, 1, ECUIO_PLUGIN_VARIANT_ID_MAX, "\"variant\"");
            uint32_t index = 0;
            while (index < p.variants.size() && p.variants[index].id != id) {
                ++index;
            }
            if (index == p.variants.size()) {
                refuse(*variant, "the plugin has no variant " + q(id));
            }
            const std::string key = text_of(*signal, 1, 31, "\"signal\"");
            const ecuio_signal_def_t* def = ecuio_signal_file_find(
                p.variants[index].file.get(), key.c_str());
            if (def == nullptr) {
                refuse(*signal,
                       "variant " + q(id) + " has no signal " + q(key));
            }
            if ((def->flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ||
                def->kind == ECUIO_VALUE_TEXT || def->kind == ECUIO_VALUE_HEX) {
                refuse(*signal, q(key) +
                                    " isn't a number or an enum: a value "
                                    "fixture checks a field the simulated ECU "
                                    "answers");
            }
            for (const auto& s : seen) {
                if (s.first == index && s.second == def->id) {
                    refuse(*signal,
                           q(key) + " of variant " + q(id) + " is given twice");
                }
            }
            seen.emplace_back(index, def->id);
            expect(*value, Type::number, "\"value\"");
            ecuio_plugin_value_t v{};
            v.value = value->number;
            v.variant = index;
            v.line = e.line;
            copy_field(v.signal, sizeof v.signal, key);
            v.def = def;
            out.push_back(v);
        }
    } catch (const refused& r) {
        throw placed{"line " + std::to_string(r.line) + ", column " +
                     std::to_string(r.column) + ": " + r.words};
    }
    return out;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_plugin_load(const char* manifest, size_t length,
                                 ecuio_plugin_read_t read, void* user,
                                 ecuio_plugin_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (manifest == nullptr && length > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        loader l(manifest, length, read, user);
        std::unique_ptr<ecuio_plugin> p = l.load();
        *out = p.release();
        return ECUIO_OK;
    } catch (const placed& r) {
        ecuio_detail_set("%s", r.words.c_str());
        return r.words.find("out of memory") != std::string::npos
                   ? ECUIO_ERR_OUT_OF_MEMORY
                   : ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the plugin couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

ecuio_status_t ecuio_plugin_info(const ecuio_plugin_t* plugin,
                                 ecuio_plugin_info_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof(ecuio_plugin_info_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof(ecuio_plugin_info_t);
    info.tiers = plugin->tiers;
    copy_field(info.id, sizeof info.id, plugin->id);
    copy_field(info.version, sizeof info.version, plugin->version);
    copy_field(info.name, sizeof info.name, plugin->name);
    info.variant_count = static_cast<uint32_t>(plugin->variants.size());
    info.fixture_count = static_cast<uint32_t>(plugin->fixtures.size());
    *out = info;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_variant(const ecuio_plugin_t* plugin,
                                    uint32_t index, char* id, size_t cap)
{
    if (plugin == nullptr || id == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= plugin->variants.size() ||
        !copy_out(plugin->variants[index].id, id, cap)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_fixture(const ecuio_plugin_t* plugin,
                                    uint32_t index, char* path, size_t cap)
{
    if (plugin == nullptr || path == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= plugin->fixtures.size() ||
        !copy_out(plugin->fixtures[index], path, cap)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

const ecuio_signal_set_t* ecuio_plugin_variant_set(const ecuio_plugin_t* plugin,
                                                   uint32_t index)
{
    if (plugin == nullptr || index >= plugin->variants.size()) {
        return nullptr;
    }
    return ecuio_signal_file_set(plugin->variants[index].file.get());
}

const ecuio_signal_def_t*
ecuio_plugin_variant_find(const ecuio_plugin_t* plugin, uint32_t index,
                          const char* key)
{
    if (plugin == nullptr || index >= plugin->variants.size()) {
        return nullptr;
    }
    return ecuio_signal_file_find(plugin->variants[index].file.get(), key);
}

ecuio_status_t ecuio_plugin_values(const ecuio_plugin_t* plugin,
                                   const char* bytes, size_t length,
                                   ecuio_plugin_value_t* values, uint32_t cap,
                                   uint32_t* count)
{
    ecuio_detail_clear();
    if (plugin == nullptr || count == nullptr ||
        (bytes == nullptr && length > 0) || (values == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        const std::vector<ecuio_plugin_value_t> read =
            read_values(*plugin, bytes, length);
        for (size_t i = 0; i < read.size() && i < cap; ++i) {
            values[i] = read[i];
        }
        *count = static_cast<uint32_t>(read.size());
        return ECUIO_OK;
    } catch (const placed& r) {
        ecuio_detail_set("%s", r.words.c_str());
        return r.words.find("out of memory") != std::string::npos
                   ? ECUIO_ERR_OUT_OF_MEMORY
                   : ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the fixture file couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

ecuio_status_t ecuio_plugin_match(const ecuio_plugin_t* plugin,
                                  const ecuio_vin_info_t* vin,
                                  uint32_t* indices, uint32_t cap,
                                  uint32_t* count)
{
    if (plugin == nullptr || vin == nullptr || count == nullptr ||
        (indices == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    char code[ECUIO_VIN_WMI_LEN + 1] = {};
    std::memcpy(code, vin->wmi, ECUIO_VIN_WMI_LEN);
    const std::string wmi(code);
    uint32_t n = 0;
    for (size_t v = 0; v < plugin->variants.size(); ++v) {
        bool proposed = false;
        for (const match_rule& rule : plugin->match) {
            bool maker = false;
            for (const std::string& w : rule.wmi) {
                maker = maker || w == wmi;
            }
            proposed =
                proposed || (maker && rule.years.covers(vin->model_year));
        }
        if (proposed && plugin->variants[v].years.covers(vin->model_year)) {
            if (n < cap) {
                indices[n] = static_cast<uint32_t>(v);
            }
            ++n;
        }
    }
    *count = n;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_register(ecuio_plugin_t* plugin, uint32_t index,
                                     char* name, size_t cap)
{
    if (plugin == nullptr || (name == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= plugin->variants.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    variant& v = plugin->variants[index];
    if (name != nullptr && cap < v.profile.size() + 1) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_status_t rc = ecuio_profile_register(
        v.profile.c_str(), ecuio_signal_file_set(v.file.get()), nullptr);
    if (rc != ECUIO_OK) {
        return rc;
    }
    v.registered = true;
    if (name != nullptr) {
        copy_out(v.profile, name, cap);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_free(ecuio_plugin_t* plugin)
{
    if (plugin == nullptr) {
        return ECUIO_OK;
    }
    try {
        std::vector<const char*> names;
        std::vector<const ecuio_signal_set_t*> sets;
        for (const variant& v : plugin->variants) {
            if (v.registered) {
                names.push_back(v.profile.c_str());
                sets.push_back(ecuio_signal_file_set(v.file.get()));
            }
        }
        const ecuio_status_t rc = ecuio_profile_unregister_unused(
            names.data(), sets.data(), names.size());
        if (rc != ECUIO_OK) {
            if (rc == ECUIO_ERR_IN_USE) {
                ecuio_detail_set(
                    "a session or a simulated ECU still decodes with one "
                    "of plugin %s's profiles",
                    plugin->id.c_str());
            }
            return rc;
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    delete plugin;
    return ECUIO_OK;
}

} // extern "C"
