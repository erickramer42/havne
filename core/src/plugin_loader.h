// File/layer: core/src/plugin_loader.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginTest, PluginWrapTest, PluginConversationTest
//          (test_signals); FUZZ: fuzz_plugin_manifest
//
// The plugin loader's parts (ADR-0052): what a loaded plugin holds, and
// the loader that reads a manifest into one. plugin_manifest.cpp reads
// the manifest's keys and the package's files, plugin_conversation.cpp
// the conversation tier's keys and its module, plugin_merge.cpp merges a
// variant's files into its set, and plugin.cpp holds the C API.

#ifndef ECUIO_PLUGIN_LOADER_H
#define ECUIO_PLUGIN_LOADER_H

#include "ecuio/plugin.h"
#include "json.h"
#include "least_core.h"
#include "plugin_rules.h"
#include "signal_file_build.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_plugin_loader {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_plugin_rules::core_version;
using ecuio_plugin_rules::id_ok;
using ecuio_plugin_rules::older;
using ecuio_plugin_rules::parse_version;
using ecuio_plugin_rules::wmi_ok;
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
    uint32_t format = 0;          /* its "havne_signals", once checked */
};

/* A packed package's binary file: a "files" entry with a "kind" */
struct binary_file {
    std::string path;
    const Value* root = nullptr; /* into the manifest */
    bool read = false;           /* something in the manifest names it */
};

/* An ECU the conversation's module talks to: the manifest's, by its id */
struct conversation_ecu {
    std::string id;
    uint32_t rx = 0;
    uint32_t tx = 0;
};

/* The conversation tier's part of a plugin (ADR-0066), as its manifest
 * declares it and its module's bytes, checked */
struct conversation {
    std::vector<uint8_t> code;
    uint8_t sha256[32] = {};
    uint32_t budget = ECUIO_PLUGIN_BUDGET_MAX;
    std::vector<conversation_ecu> ecus;
    std::vector<ecuio_plugin_service_t> services;
    std::vector<ecuio_plugin_security_level_t> levels;
};

} // namespace ecuio_plugin_loader

struct ecuio_plugin {
    std::string id;
    std::string name;
    std::string version;
    uint16_t requires_core[3] = {};
    /* The least core that reads the manifest and every variant's files */
    ecuio_least::version least = ecuio_least::kPackage;
    std::string lineage; /* "" for none */
    uint32_t format = ECUIO_PLUGIN_FORMAT; /* its "havne_plugin" */
    uint32_t tiers = 0;
    /* NULL without the conversation tier */
    std::unique_ptr<ecuio_plugin_loader::conversation> conversation;
    std::vector<ecuio_plugin_loader::match_rule> match;
    std::vector<ecuio_plugin_loader::variant> variants;
    std::vector<std::string> fixtures;
};

namespace ecuio_plugin_loader {

/* Thrown with the words already placed: a file the reader couldn't
 * give, a data file's JSON refused */
struct placed {
    std::string words;
    bool out_of_memory = false; /* the reader ran out, whatever the words */
};

/* A JSON refusal, placed after prefix: out of memory when the reader ran
 * out, never because the input's own words say so */
placed placed_json(const std::string& prefix, const ecuio_json::Error& error);

/* Whether p is a path in a package: relative, forward slashes, no "."
 * or ".." part, no empty part, 1 to ECUIO_PLUGIN_PATH_MAX printable
 * ASCII bytes but backslash and colon. path_of reads one from v (what a
 * view, as text_of's). */
bool path_ok(const std::string& p);
const std::string& path_of(const Value& v, std::string_view what);

class loader
{
public:
    loader(const char* bytes, size_t length, ecuio_plugin_read_t read,
           void* user)
        : bytes_(bytes), length_(length), read_(read), user_(user)
    {}

    std::unique_ptr<ecuio_plugin> load();

private:
    bool packed() const { return read_ == nullptr; }

    std::string where_manifest() const;
    std::string place(const refused& r) const;
    void read_manifest(ecuio_plugin& p);
    static void read_format(const Value& root, ecuio_plugin& p);
    static void read_identity(const Value& root, ecuio_plugin& p);
    static void read_requires(const Value& root, ecuio_plugin& p);
    static void read_origin(const Value& root, ecuio_plugin& p);
    void read_tiers(const Value& root, ecuio_plugin& p);
    void read_match(const Value& root, ecuio_plugin& p);
    void read_fixtures(const Value& root, ecuio_plugin& p);
    void read_files(const Value& root, const ecuio_plugin& p);
    void read_conversation(const Value& root, ecuio_plugin& p);
    void read_code(const Value& code, conversation& c);
    void packed_code(const Value& file, const std::string& path,
                     conversation& c);
    void folder_code(const Value& file, const std::string& path,
                     conversation& c);
    std::vector<std::vector<const data_file*>> read_variants(const Value& root,
                                                             ecuio_plugin& p);
    const data_file* file_at(const Value& at, const std::string& path);
    static void check_data_file(data_file& f);
    void build_variant(const ecuio_plugin& p, const Value& at,
                       const std::vector<const data_file*>& list, variant& out);
    std::string variant_json(const ecuio_plugin& p, const variant& v,
                             const ecuio_sf::parts& parts, const Value* source,
                             std::string* why) const;
    static bool same_set(const ecuio_signal_set_t& a,
                         const ecuio_signal_set_t& b);

    const char* bytes_;
    size_t length_;
    ecuio_plugin_read_t read_;
    void* user_;
    size_t total_ = 0;
    Value manifest_;
    std::deque<data_file> files_; /* a deque: chosen lists point into it */
    std::vector<binary_file> binaries_; /* a packed package's */
};

} // namespace ecuio_plugin_loader

#endif /* ECUIO_PLUGIN_LOADER_H */
