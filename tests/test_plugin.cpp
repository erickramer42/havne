// File/layer: tests/test_plugin.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest (test_signals)

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include <ecuio/vin.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "signal_file_build.h" /* core/src: a variant's bytes */

#ifndef ECUIO_PLUGINS_DIR
#error                                                                         \
    "tests/CMakeLists.txt defines ECUIO_PLUGINS_DIR, the repository's plugins/"
#endif

namespace {

/* A folder package in memory: manifest.json and the files by path */
struct Folder {
    std::map<std::string, std::string> files;
    std::vector<std::string> asked;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<Folder*>(user);
        self->asked.emplace_back(path);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            return 1;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return 0;
    }
};

struct PluginFree {
    void operator()(ecuio_plugin_t* p) const
    {
        EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);
    }
};
using Plugin = std::unique_ptr<ecuio_plugin_t, PluginFree>;

std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::ostringstream s;
    s << in.rdbuf();
    /* A checkout may carry CRLF; the cases below edit the text by its LF
     * lines, and the loader reads either */
    std::string text = s.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

/* The example plugin's folder, as its files are on disk */
Folder example_folder()
{
    const std::string root = std::string(ECUIO_PLUGINS_DIR) + "/example-sim/";
    Folder f;
    for (const char* p : {"manifest.json", "data/base.json", "data/my2019.json",
                          "fixtures/values.json"}) {
        f.files[p] = slurp(root + p);
    }
    return f;
}

Plugin load_folder(Folder& f, ecuio_status_t* status = nullptr)
{
    ecuio_plugin_t* p = reinterpret_cast<ecuio_plugin_t*>(1);
    const std::string& m = f.files["manifest.json"];
    const ecuio_status_t s =
        ecuio_plugin_load(m.data(), m.size(), &Folder::read, &f, &p);
    if (status != nullptr) {
        *status = s;
    }
    if (s != ECUIO_OK) {
        EXPECT_EQ(p, nullptr) << "out is NULL on a failure";
        return Plugin(nullptr);
    }
    return Plugin(p);
}

Plugin load_packed(const std::string& json, ecuio_status_t* status = nullptr)
{
    ecuio_plugin_t* p = reinterpret_cast<ecuio_plugin_t*>(1);
    const ecuio_status_t s =
        ecuio_plugin_load(json.data(), json.size(), nullptr, nullptr, &p);
    if (status != nullptr) {
        *status = s;
    }
    if (s != ECUIO_OK) {
        EXPECT_EQ(p, nullptr) << "out is NULL on a failure";
        return Plugin(nullptr);
    }
    return Plugin(p);
}

std::string packed_refusal(const std::string& json)
{
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_packed(json, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT) << json;
    return ecuio_last_error_detail();
}

std::string folder_refusal(Folder& f)
{
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_folder(f, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
    return ecuio_last_error_detail();
}

/* A small packed plugin: one data file, one variant; parts replaced by
 * the cases below */
std::string
packed(const std::string& head, const std::string& data,
       const std::string& variants = R"([{"id": "base", "files": ["a.json"]}])",
       const std::string& files_extra = "")
{
    return "{\"havne_plugin\": 1, " + head + (head.empty() ? "" : ", ") +
           "\"variants\": " + variants + ", \"files\": {\"a.json\": " + data +
           files_extra + "}}";
}

const std::string kHead =
    R"("id": "p", "name": "P", "version": "1.0.0", "requires": {"core": "0.1.0"},)"
    R"( "source": {"name": "t"}, "tiers": ["data"],)"
    R"( "ecus": [{"id": "pcm", "rx": "7E8"}])";

const std::string kData =
    R"({"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", "ecu": "pcm",)"
    R"( "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}}]})";

std::string variant_id(const ecuio_plugin_t* p, uint32_t i)
{
    char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
    EXPECT_EQ(ecuio_plugin_variant(p, i, id, sizeof id), ECUIO_OK);
    return id;
}

uint32_t def_index(const ecuio_signal_set_t* set, const ecuio_signal_def_t* d)
{
    return static_cast<uint32_t>(d - set->defs);
}

ecuio_vin_info_t vin_of(const char* wmi, uint16_t year)
{
    ecuio_vin_info_t v{};
    std::strncpy(v.wmi, wmi, sizeof v.wmi - 1);
    v.wmi_length = static_cast<uint8_t>(std::strlen(wmi));
    v.model_year = year;
    return v;
}

} // namespace

TEST(PluginTest, TheExamplePluginLoadsWithItsVariants)
{
    Folder f = example_folder();
    Plugin p = load_folder(f);
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();

    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_plugin_info(p.get(), &info), ECUIO_OK);
    EXPECT_STREQ(info.id, "example-sim");
    EXPECT_STREQ(info.version, "1.0.0");
    EXPECT_STREQ(info.name, "Example: the simulated car");
    EXPECT_EQ(info.tiers, ECUIO_PLUGIN_TIER_DATA);
    EXPECT_EQ(info.variant_count, 2u);
    EXPECT_EQ(info.fixture_count, 1u);
    EXPECT_EQ(variant_id(p.get(), 0), "base");
    EXPECT_EQ(variant_id(p.get(), 1), "my2019");
    char fixture[ECUIO_PLUGIN_PATH_MAX + 1] = {};
    ASSERT_EQ(ecuio_plugin_fixture(p.get(), 0, fixture, sizeof fixture),
              ECUIO_OK);
    EXPECT_STREQ(fixture, "fixtures/values.json");

    /* The data files are read once each, never the fixtures */
    EXPECT_EQ(f.asked,
              (std::vector<std::string>{"data/base.json", "data/my2019.json"}));

    /* base: obd2 and its five; my2019 replaces two in their places and
     * adds one */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const ecuio_signal_set_t* base = ecuio_plugin_variant_set(p.get(), 0);
    const ecuio_signal_set_t* late = ecuio_plugin_variant_set(p.get(), 1);
    ASSERT_NE(base, nullptr);
    ASSERT_NE(late, nullptr);
    EXPECT_EQ(base->count, obd2->count + 5);
    EXPECT_EQ(late->count, obd2->count + 6);
    EXPECT_EQ(base->channel_count, 2u);
    const ecuio_signal_def_t* t0 =
        ecuio_plugin_variant_find(p.get(), 0, "trans_temp");
    const ecuio_signal_def_t* t1 =
        ecuio_plugin_variant_find(p.get(), 1, "trans_temp");
    ASSERT_NE(t0, nullptr);
    ASSERT_NE(t1, nullptr);
    EXPECT_DOUBLE_EQ(t0->scale, 0.1);
    EXPECT_DOUBLE_EQ(t1->scale, 0.01);
    EXPECT_EQ(def_index(base, t0), def_index(late, t1))
        << "replaced in its place";
    EXPECT_EQ(ecuio_plugin_variant_find(p.get(), 0, "clutch_slip"), nullptr);
    ASSERT_NE(ecuio_plugin_variant_find(p.get(), 1, "clutch_slip"), nullptr);
    EXPECT_EQ(
        def_index(late, ecuio_plugin_variant_find(p.get(), 1, "clutch_slip")),
        late->count - 1)
        << "added after the rest";
    const ecuio_signal_def_t* gear =
        ecuio_plugin_variant_find(p.get(), 1, "gear");
    ASSERT_NE(gear, nullptr);
    EXPECT_STREQ(ecuio_enum_label(late, gear, 7), "Seventh");
    EXPECT_EQ(ecuio_enum_label(
                  base, ecuio_plugin_variant_find(p.get(), 0, "gear"), 7),
              nullptr);
    EXPECT_NE(ecuio_plugin_variant_find(p.get(), 1, "boost"), nullptr)
        << "the first file's own signals stay";

    /* Registered, each variant is a profile a URI names */
    char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
    ASSERT_EQ(ecuio_plugin_register(p.get(), 1, name, sizeof name), ECUIO_OK);
    EXPECT_STREQ(name, "plugin.example-sim.my2019");
    const ecuio_signal_set_t* found = nullptr;
    ASSERT_EQ(ecuio_profile_lookup(name, &found), ECUIO_OK);
    EXPECT_EQ(found, late);
}

TEST(PluginTest, APackedPluginReadsAsItsFolderDoes)
{
    Folder f = example_folder();
    std::string manifest = f.files["manifest.json"];
    /* The packed form: no fixtures, the files inlined */
    const size_t fx = manifest.find(",\n  \"fixtures\"");
    ASSERT_NE(fx, std::string::npos);
    manifest =
        manifest.substr(0, fx) +
        ",\n  \"files\": {\"data/base.json\": " + f.files["data/base.json"] +
        ", \"data/my2019.json\": " + f.files["data/my2019.json"] + "}\n}\n";
    Plugin packed_p = load_packed(manifest);
    ASSERT_NE(packed_p, nullptr) << ecuio_last_error_detail();
    Plugin folder_p = load_folder(f);
    ASSERT_NE(folder_p, nullptr);
    for (uint32_t v = 0; v < 2; ++v) {
        const ecuio_signal_set_t* a =
            ecuio_plugin_variant_set(packed_p.get(), v);
        const ecuio_signal_set_t* b =
            ecuio_plugin_variant_set(folder_p.get(), v);
        ASSERT_EQ(a->count, b->count);
        EXPECT_EQ(std::memcmp(a->defs, b->defs,
                              sizeof(ecuio_signal_def_t) * a->count),
                  0);
        EXPECT_EQ(a->enum_count, b->enum_count);
        EXPECT_EQ(a->computed_count, b->computed_count);
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_plugin_info(packed_p.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.fixture_count, 0u);
}

TEST(PluginTest, ARefusalNamesTheFileAndThePlace)
{
    /* A folder's data file: its path, then its own line and column */
    Folder f = example_folder();
    f.files["data/my2019.json"] =
        "{\n  \"havne_signals\": 1,\n  \"signals\": [\n"
        "    {\"key\": \"Bad\"}\n  ]\n}\n";
    EXPECT_EQ(folder_refusal(f).rfind(
                  "data/my2019.json, line 4, column 13: key 'Bad'", 0),
              0u)
        << ecuio_last_error_detail();

    /* Its JSON's own refusals too */
    f = example_folder();
    f.files["data/base.json"] = "{\"havne_signals\": 1,}";
    EXPECT_EQ(folder_refusal(f).rfind("data/base.json, line 1, column ", 0), 0u)
        << ecuio_last_error_detail();

    /* The manifest's, by name */
    f = example_folder();
    f.files["manifest.json"].replace(f.files["manifest.json"].find("\"1.0.0\""),
                                     7, "\"1.0\"");
    EXPECT_EQ(folder_refusal(f).rfind(
                  "manifest.json, line 5, column 14: \"version\"", 0),
              0u)
        << ecuio_last_error_detail();

    /* A file the host can't give, at the path that names it */
    f = example_folder();
    f.files.erase("data/my2019.json");
    EXPECT_EQ(folder_refusal(f), "manifest.json, line 17, column 74: file "
                                 "'data/my2019.json' couldn't be read");

    /* An ECU a data file names that the manifest lacks: the data file's */
    f = example_folder();
    std::string& m = f.files["manifest.json"];
    m.erase(m.find(",\n    { \"id\": \"tcm\""),
            std::strlen(",\n    { \"id\": \"tcm\", \"rx\": \"7E9\" }"));
    EXPECT_EQ(folder_refusal(f).rfind("data/base.json, line ", 0), 0u)
        << ecuio_last_error_detail();

    /* Packed: one file, so the line and column alone */
    const std::string got =
        packed_refusal(packed(kHead, R"({"havne_signals": 2})"));
    EXPECT_EQ(got.rfind("line 1, column ", 0), 0u) << got;
    EXPECT_NE(got.find("format 2"), std::string::npos) << got;
}

TEST(PluginTest, EveryManifestRefusalSaysWhy)
{
    struct Case {
        std::string json;
        const char* words;
    };
    const std::string v1 = R"([{"id": "base", "files": ["a.json"]}])";
    const std::vector<Case> cases = {
        {"{\"id\": \"p\"}", "starts with \"havne_plugin\""},
        {"{\"havne_plugin\": 2}", "format 2, and this Havne reads format 1"},
        {packed(kHead + ", \"tint\": 1", kData),
         "unknown key 'tint' in the manifest"},
        {packed(R"("name": "P")", kData), "needs an \"id\""},
        {packed(R"("id": "Upper", "name": "P")", kData),
         "\"id\" 'Upper' takes 1 to 31"},
        {packed(R"("id": "-p", "name": "P")", kData),
         "a letter or digit at each end"},
        {packed(R"("id": "p", "name": "P", "version": "1.0")", kData),
         "isn't X.Y.Z"},
        {packed(R"("id": "p", "name": "P", "version": "1.02.0")", kData),
         "isn't X.Y.Z"},
        {packed(R"("id": "p", "name": "P", "version": "1.0.0")", kData),
         "needs \"requires\""},
        {packed(
             R"("id": "p", "name": "P", "version": "1.0.0", "requires": {"core": "99.0.0"})",
             kData),
         "needs core 99.0.0 or later"},
        {packed(
             R"("id": "p", "name": "P", "version": "1.0.0", "requires": {"core": "0.1.0"})",
             kData),
         "needs a \"source\""},
        {packed(
             R"("id": "p", "name": "P", "version": "1.0.0", "requires": {"core": "0.1.0"}, "source": {"name": "t"}, "tiers": ["data", "conversation"])",
             kData),
         "tier 'conversation' isn't one this Havne runs"},
        {packed(
             R"("id": "p", "name": "P", "version": "1.0.0", "requires": {"core": "0.1.0"}, "source": {"name": "t"}, "tiers": [])",
             kData),
         "\"tiers\" lists none"},
        {packed(kHead + R"(, "match": [{"wmi": ["zzz"]}])", kData),
         "manufacturer code 'zzz'"},
        {packed(kHead + R"(, "match": [{"wmi": ["ZZI"]}])", kData),
         "manufacturer code 'ZZI'"},
        {packed(kHead +
                    R"(, "match": [{"wmi": ["ZZZ"], "years": [2020, 2019]}])",
                kData),
         "runs from 2020 back to 2019"},
        {packed(kHead +
                    R"(, "match": [{"wmi": ["ZZZ"], "years": [1979, 2019]}])",
                kData),
         "first year takes a whole number from 1980 to 2039"},
        {packed(kHead, kData, "[]"), "\"variants\" lists none"},
        {packed(
             kHead, kData,
             R"([{"id": "base", "files": ["a.json"]}, {"id": "base", "files": ["a.json"]}])"),
         "variant 'base' is given twice"},
        {packed(
             kHead, kData,
             R"([{"id": "a-very-long-variant-name-x", "files": ["a.json"]}])"),
         "a variant's id is 26 bytes"},
        {packed(kHead, kData, R"([{"id": "base", "files": ["../a.json"]}])"),
         "isn't a path in the package"},
        {packed(kHead, kData, R"([{"id": "base", "files": ["/a.json"]}])"),
         "isn't a path in the package"},
        {packed(kHead, kData,
                R"([{"id": "base", "files": ["a.json", "a.json"]}])"),
         "lists 'a.json' twice"},
        {packed(kHead, kData, R"([{"id": "base", "files": ["b.json"]}])"),
         "file 'b.json' isn't in \"files\""},
        {packed(kHead, kData, v1, ", \"b.json\": " + kData),
         "file 'b.json' is in no variant"},
        {packed(kHead + R"(, "fixtures": ["f.json"])", kData),
         "a packed plugin carries no \"fixtures\""},
        {"{\"havne_plugin\": 1, " + kHead + ", \"variants\": " + v1 + "}",
         "needs \"files\""},
        {packed(kHead, R"({"havne_signals": 1, "ecus": [], "signals": []})"),
         "declares no \"ecus\": it names the manifest's by id"},
        {packed(
             kHead,
             R"({"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", "ecu": "pcm", "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}}, {"key": "oil", "name": "Oil 2", "ecu": "pcm", "read": {"service": "22", "number": "F40E"}, "field": {"byte": 0}}]})"),
         "key 'oil' is given twice"},
        {packed(
             kHead, kData, R"([{"id": "base", "files": ["a.json", "b.json"]}])",
             R"(, "b.json": {"havne_signals": 1, "extends": "obd2", "signals": []})"),
         "only a variant's first file extends a set, and this is file 2 of "
         "variant 'base'"},
        {packed(kHead, R"({"havne_signals": 1, "signals": []})"),
         "\"signals\" lists none"},
    };
    for (const Case& c : cases) {
        const std::string got = packed_refusal(c.json);
        EXPECT_NE(got.find(c.words), std::string::npos)
            << "json: " << c.json << "\n  got: " << got
            << "\n  want: " << c.words;
        EXPECT_EQ(got.compare(0, 5, "line "), 0) << got;
    }

    /* A folder's manifest takes no "files" */
    Folder f = example_folder();
    std::string& m = f.files["manifest.json"];
    m.insert(m.rfind('}'), ", \"files\": {}");
    EXPECT_NE(folder_refusal(f).find("\"files\" is a packed plugin's"),
              std::string::npos)
        << ecuio_last_error_detail();
}

TEST(PluginTest, TheVinProposesByMakerAndYear)
{
    Folder f = example_folder();
    Plugin p = load_folder(f);
    ASSERT_NE(p, nullptr);
    uint32_t idx[4] = {};
    uint32_t n = 99;

    ecuio_vin_info_t vin = vin_of("ZZZ", 2021);
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, idx, 4, &n), ECUIO_OK);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(idx[0], 0u);
    EXPECT_EQ(idx[1], 1u);

    vin = vin_of("ZZZ", 2016); /* before my2019's years */
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, idx, 4, &n), ECUIO_OK);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(idx[0], 0u);

    vin = vin_of("ZZZ", 2014); /* before the match's */
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, idx, 4, &n), ECUIO_OK);
    EXPECT_EQ(n, 0u);

    vin = vin_of("ZZZ", 0); /* no year: no range covers it */
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, idx, 4, &n), ECUIO_OK);
    EXPECT_EQ(n, 0u);

    vin = vin_of("ZZY", 2021);
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, idx, 4, &n), ECUIO_OK);
    EXPECT_EQ(n, 0u);

    /* The count alone, then the first only */
    vin = vin_of("ZZZ", 2021);
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, nullptr, 0, &n), ECUIO_OK);
    EXPECT_EQ(n, 2u);
    idx[0] = 77;
    idx[1] = 77;
    ASSERT_EQ(ecuio_plugin_match(p.get(), &vin, idx, 1, &n), ECUIO_OK);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(idx[0], 0u);
    EXPECT_EQ(idx[1], 77u) << "nothing past cap";
    EXPECT_EQ(ecuio_plugin_match(p.get(), &vin, nullptr, 1, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_match(p.get(), nullptr, idx, 1, &n),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(PluginTest, TheFreeIsRefusedWhileASessionDecodesWithItsSet)
{
    Folder f = example_folder();
    ecuio_plugin_t* p = nullptr;
    const std::string& m = f.files["manifest.json"];
    ASSERT_EQ(ecuio_plugin_load(m.data(), m.size(), &Folder::read, &f, &p),
              ECUIO_OK);
    char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
    ASSERT_EQ(ecuio_plugin_register(p, 0, name, sizeof name), ECUIO_OK);
    ASSERT_STREQ(name, "plugin.example-sim.base");

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(
                  "mock://plugin?profile=plugin.example-sim.base", &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_signal_set(s), ecuio_plugin_variant_set(p, 0));
    EXPECT_EQ(ecuio_plugin_free(p), ECUIO_ERR_IN_USE);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("plugin example-sim"),
              std::string::npos);
    const ecuio_signal_set_t* still = nullptr;
    EXPECT_EQ(ecuio_profile_lookup(name, &still), ECUIO_OK)
        << "nothing changed";

    /* Closed is not gone: the session still holds the set */
    ASSERT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_plugin_free(p), ECUIO_ERR_IN_USE);
    ASSERT_EQ(ecuio_session_destroy(s), ECUIO_OK);
    ASSERT_EQ(ecuio_plugin_free(p), ECUIO_OK);
    EXPECT_EQ(ecuio_profile_lookup(name, &still), ECUIO_ERR_UNKNOWN_PROFILE);

    /* A refused create holds nothing either */
    ASSERT_EQ(ecuio_plugin_load(m.data(), m.size(), &Folder::read, &f, &p),
              ECUIO_OK);
    ASSERT_EQ(ecuio_plugin_register(p, 0, nullptr, 0), ECUIO_OK);
    s = nullptr;
    EXPECT_NE(ecuio_session_create(
                  "mock://x?profile=plugin.example-sim.base&nope=1", &s),
              ECUIO_OK);
    EXPECT_EQ(s, nullptr);
    EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);
}

TEST(PluginTest, TheFreeLeavesANameRegisteredSinceToAnotherSet)
{
    Folder f = example_folder();
    ecuio_plugin_t* p = nullptr;
    const std::string& m = f.files["manifest.json"];
    ASSERT_EQ(ecuio_plugin_load(m.data(), m.size(), &Folder::read, &f, &p),
              ECUIO_OK);
    ASSERT_EQ(ecuio_plugin_register(p, 0, nullptr, 0), ECUIO_OK);
    ASSERT_EQ(ecuio_plugin_register(p, 1, nullptr, 0), ECUIO_OK);
    /* Another plugin of the id took one name since */
    ASSERT_EQ(ecuio_profile_register("plugin.example-sim.base",
                                     ecuio_mock_signal_set(), nullptr),
              ECUIO_OK);
    ASSERT_EQ(ecuio_plugin_free(p), ECUIO_OK);
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("plugin.example-sim.base", &set), ECUIO_OK);
    EXPECT_EQ(set, ecuio_mock_signal_set());
    EXPECT_EQ(ecuio_profile_lookup("plugin.example-sim.my2019", &set),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(ecuio_profile_unregister("plugin.example-sim.base"), ECUIO_OK);
}

// A manifest whose own words say "out of memory" is refused as any
// other is, never reported as memory run out
TEST(PluginTest, AnOutOfMemoryInTheWordsIsStillARefusal)
{
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_packed(
        packed(R"("id": "out of memory", "name": "P", "version": "1.0.0",)"
               R"( "requires": {"core": "0.1.0"}, "source": {"name": "t"},)"
               R"( "tiers": ["data"], "ecus": [{"id": "pcm", "rx": "7E8"}])",
               kData),
        &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("out of memory"),
              std::string::npos)
        << ecuio_last_error_detail();
}

TEST(PluginTest, TheAccessorsRefuseWhatTheyCantFill)
{
    Folder f = example_folder();
    Plugin p = load_folder(f);
    ASSERT_NE(p, nullptr);
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info - 4;
    EXPECT_EQ(ecuio_plugin_info(p.get(), &info), ECUIO_ERR_INVALID_ARGUMENT);
    char small[4] = {};
    EXPECT_EQ(ecuio_plugin_variant(p.get(), 1, small, sizeof small),
              ECUIO_ERR_INVALID_ARGUMENT);
    char id[8] = {};
    EXPECT_EQ(ecuio_plugin_variant(p.get(), 1, id, 6),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "\"my2019\" and its NUL take 7";
    EXPECT_EQ(ecuio_plugin_variant(p.get(), 1, id, 7), ECUIO_OK);
    EXPECT_STREQ(id, "my2019");
    EXPECT_EQ(ecuio_plugin_variant(p.get(), 2, id, sizeof id),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_fixture(p.get(), 1, id, sizeof id),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_variant_set(p.get(), 2), nullptr);
    EXPECT_EQ(ecuio_plugin_variant_find(p.get(), 0, "nope"), nullptr);
    char name[8] = {};
    EXPECT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "too small for the profile's name";
    EXPECT_EQ(ecuio_plugin_register(p.get(), 2, nullptr, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_free(nullptr), ECUIO_OK);
    ecuio_plugin_t* out = nullptr;
    EXPECT_EQ(ecuio_plugin_load(nullptr, 3, nullptr, nullptr, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_load("{}", 2, nullptr, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

namespace {

/* A fixture file's refusal against plugin p: INVALID_ARGUMENT, the count
 * untouched, and the detail */
std::string values_refusal(const ecuio_plugin_t* p, const std::string& json)
{
    uint32_t count = 77;
    EXPECT_EQ(
        ecuio_plugin_values(p, json.data(), json.size(), nullptr, 0, &count),
        ECUIO_ERR_INVALID_ARGUMENT)
        << json;
    EXPECT_EQ(count, 77u) << "the count is untouched on a refusal";
    return ecuio_last_error_detail();
}

} // namespace

TEST(PluginTest, TheExamplesFixturesReadAsItsVariantsSignals)
{
    Folder f = example_folder();
    Plugin p = load_folder(f);
    ASSERT_NE(p, nullptr);
    const std::string& text = f.files["fixtures/values.json"];
    uint32_t count = 0;
    ASSERT_EQ(ecuio_plugin_values(p.get(), text.data(), text.size(), nullptr, 0,
                                  &count),
              ECUIO_OK);
    ASSERT_EQ(count, 7u);
    std::vector<ecuio_plugin_value_t> v(count);
    uint32_t again = 0;
    ASSERT_EQ(ecuio_plugin_values(p.get(), text.data(), text.size(), v.data(),
                                  2, &again),
              ECUIO_OK);
    EXPECT_EQ(again, 7u) << "every entry is counted, the first cap copied";
    ASSERT_EQ(ecuio_plugin_values(p.get(), text.data(), text.size(), v.data(),
                                  count, &count),
              ECUIO_OK);
    EXPECT_EQ(v[0].variant, 0u);
    EXPECT_EQ(v[0].def, ecuio_plugin_variant_find(p.get(), 0, "oil_temp"));
    EXPECT_EQ(v[0].value, 104.0);
    EXPECT_EQ(v[0].line, 4u);
    EXPECT_STREQ(v[0].signal, "oil_temp");
    EXPECT_EQ(v[4].variant, 1u);
    EXPECT_EQ(v[4].def, ecuio_plugin_variant_find(p.get(), 1, "trans_temp"))
        << "my2019's own trans_temp, not base's";
    EXPECT_NE(v[4].def, v[2].def);
    EXPECT_EQ(v[4].value, 81.37);
    EXPECT_EQ(v[6].def, ecuio_plugin_variant_find(p.get(), 1, "clutch_slip"));
    EXPECT_EQ(v[6].value, -120.0);
    for (const ecuio_plugin_value_t& e : v) {
        for (uint32_t r : e.reserved) {
            EXPECT_EQ(r, 0u);
        }
    }
}

TEST(PluginTest, EveryFixtureRefusalSaysWhy)
{
    Folder f = example_folder();
    Plugin p = load_folder(f);
    ASSERT_NE(p, nullptr);
    const std::string head = R"({"havne_fixtures": 1, "values": )";
    const struct {
        std::string json;
        std::string words;
    } cases[] = {
        {"[]", "line 1, column 1: "},
        {R"({"values": []})", "starts with \"havne_fixtures\""},
        {R"({"havne_fixtures": 2, "values": []})", "is format 2"},
        {R"({"havne_fixtures": 1})", "needs \"values\""},
        {R"({"havne_fixtures": 1, "values": [], "tint": 1})",
         "unknown key 'tint'"},
        {head + "[]}", "lists 1 to 4096 entries"},
        {head + R"([{"variant": "base", "signal": "oil_temp"}]})",
         "needs \"variant\", \"signal\" and \"value\""},
        {head +
             R"([{"variant": "base", "signal": "oil_temp", "value": 1, "at": 2}]})",
         "unknown key 'at'"},
        {head + R"([{"variant": "my2020", "signal": "oil_temp", "value": 1}]})",
         "no variant 'my2020'"},
        /* The words name it, and it's still a refusal, not memory */
        {head +
             R"([{"variant": "out of memory", "signal": "oil_temp", "value": 1}]})",
         "no variant 'out of memory'"},
        {head +
             R"([{"variant": "base", "signal": "clutch_slip", "value": 1}]})",
         "variant 'base' has no signal 'clutch_slip'"},
        {head + R"([{"variant": "base", "signal": "boost", "value": 1}]})",
         "'boost' isn't a number or an enum"},
        {head + R"([{"variant": "base", "signal": "oil_temp", "value": "1"}]})",
         "\"value\""},
        {head + R"([{"variant": "base", "signal": "oil_temp", "value": 1},)"
                R"( {"variant": "base", "signal": "oil_temp", "value": 2}]})",
         "line 1, column 119: 'oil_temp' of variant 'base' is given twice"},
    };
    for (const auto& c : cases) {
        const std::string why = values_refusal(p.get(), c.json);
        EXPECT_NE(why.find(c.words), std::string::npos)
            << c.json << "\n  said: " << why;
        EXPECT_EQ(why.find('\n'), std::string::npos) << why;
    }
    /* The same signal key in two variants is two fixtures */
    const std::string both =
        head + R"([{"variant": "base", "signal": "gear", "value": 1},)"
               R"( {"variant": "my2019", "signal": "gear", "value": 1}]})";
    uint32_t count = 0;
    EXPECT_EQ(ecuio_plugin_values(p.get(), both.data(), both.size(), nullptr, 0,
                                  &count),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(count, 2u);

    /* A text signal is refused as a computed one is */
    Plugin text(load_packed(packed(
        kHead,
        R"({"havne_signals": 1, "signals": [{"key": "part", "name": "Part",)"
        R"( "kind": "text", "ecu": "pcm", "read": {"service": "22",)"
        R"( "number": "F187"}, "field": {"byte": 0}}]})")));
    ASSERT_NE(text, nullptr) << ecuio_last_error_detail();
    EXPECT_NE(
        values_refusal(
            text.get(),
            head + R"([{"variant": "base", "signal": "part", "value": 1}]})")
            .find("'part' isn't a number or an enum"),
        std::string::npos);

    EXPECT_EQ(ecuio_plugin_values(nullptr, "{}", 2, nullptr, 0, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_values(p.get(), nullptr, 2, nullptr, 0, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_values(p.get(), "{}", 2, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_values(p.get(), "{}", 2, nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}
/* Each variant is one signal file: its merged signals written as the
 * file a recording copies, which reads back to the variant's own set,
 * with the plugin's id, version and variant for the recording's header
 * (ADR-0052 section 4) */
TEST(PluginTest, AVariantIsOneSignalFileThatReadsBackAsItsSet)
{
    Folder f = example_folder();
    Plugin p = load_folder(f);
    ASSERT_TRUE(p);
    EXPECT_EQ(ecuio_plugin_variant_file(nullptr, 0), nullptr);
    EXPECT_EQ(ecuio_plugin_variant_file(p.get(), 2), nullptr);
    for (uint32_t i = 0; i < 2; ++i) {
        const std::string id = variant_id(p.get(), i);
        const ecuio_signal_file_t* file = ecuio_plugin_variant_file(p.get(), i);
        ASSERT_NE(file, nullptr) << id;
        EXPECT_EQ(ecuio_signal_file_set(file),
                  ecuio_plugin_variant_set(p.get(), i));
        EXPECT_STREQ(ecuio_signal_file_profile(file),
                     ("plugin.example-sim." + id).c_str());
        EXPECT_STREQ(ecuio_signal_file_extends(file), "obd2");
        EXPECT_EQ(file->plugin, "example-sim,1.0.0," + id);

        const std::string& json = ecuio_sf::bytes_of(*file);
        ASSERT_FALSE(json.empty()) << id << ": the text reads back";
        EXPECT_NE(json.find("\"notes\": \"Plugin example-sim 1.0.0, variant " +
                            id + ": its files' signals merged\""),
                  std::string::npos)
            << json.substr(0, 300);
        ecuio_signal_file_t* back = nullptr;
        ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &back),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        const ecuio_signal_set_t& a = *ecuio_signal_file_set(back);
        const ecuio_signal_set_t& b = *ecuio_signal_file_set(file);
        ASSERT_EQ(a.count, b.count);
        ASSERT_EQ(a.channel_count, b.channel_count);
        ASSERT_EQ(a.enum_count, b.enum_count);
        ASSERT_EQ(a.computed_count, b.computed_count);
        EXPECT_EQ(std::memcmp(a.defs, b.defs, sizeof *a.defs * a.count), 0);
        EXPECT_EQ(std::memcmp(a.channels, b.channels,
                              sizeof *a.channels * a.channel_count),
                  0);
        EXPECT_EQ(std::memcmp(a.enums, b.enums, sizeof *a.enums * a.enum_count),
                  0);
        /* The later file's signal in the earlier's place */
        const ecuio_signal_def_t* t =
            ecuio_signal_file_find(back, "trans_temp");
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->scale, i == 0 ? 0.1 : 0.01);
        ecuio_signal_file_free(back);
    }
}

// A name a variant's signal file can't make a profile name from is
// refused at the manifest, where it stands
TEST(PluginTest, ANameThatMakesNoProfileIsRefused)
{
    for (const std::string& name : {std::string("!!!"), std::string(60, 'a')}) {
        std::string head = kHead;
        head.replace(head.find("\"name\": \"P\""), 11,
                     "\"name\": \"" + name + "\"");
        const std::string why = packed_refusal(packed(head, kData));
        EXPECT_NE(why.find("\"name\""), std::string::npos) << why;
        EXPECT_NE(why.find("profile name"), std::string::npos) << why;
        EXPECT_EQ(why.find("variant 'base'"), std::string::npos)
            << "refused at the name, not by a variant's copy: " << why;
    }
}

// A variant whose merged files a recording can't copy as one signal file
// (past 1 MiB written out) is refused, never loaded with no copy
TEST(PluginTest, AVariantThatMakesNoOneSignalFileIsRefused)
{
    const std::string pad(600 * 1024, 'x');
    const auto file = [&pad](const char* key, const char* did) {
        return std::string(R"({"havne_signals": 1, "signals": [{"key": ")") +
               key + R"(", "name": "N", "ecu": "pcm", "x-pad": ")" + pad +
               R"(", "read": {"service": "22", "number": ")" + did +
               R"("}, "field": {"byte": 0}}]})";
    };
    const std::string why = packed_refusal(
        packed(kHead, file("one", "F40D"),
               R"([{"id": "base", "files": ["a.json", "b.json"]}])",
               ", \"b.json\": " + file("two", "F40E")));
    EXPECT_NE(why.find("variant 'base''s files don't make one signal file"),
              std::string::npos)
        << why;
}

// A scale and offset in their shortest 17-digit text (a tool's, as
// Python writes a double) load, and the variant reads back as its set:
// each number read is the double nearest it, which the writer's 17
// digits give back
TEST(PluginTest, ASeventeenDigitScaleReadsBackAsItsSet)
{
    const std::string data =
        R"({"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", "ecu": "pcm",)"
        R"( "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0, "bits": 16},)"
        R"( "scale": 1.0212934580499022e-07, "offset": -40.000000000000014}]})";
    ecuio_status_t s = ECUIO_ERR_INVALID_ARGUMENT;
    Plugin p = load_packed(packed(kHead, data), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_signal_file_t* file = ecuio_plugin_variant_file(p.get(), 0);
    ASSERT_NE(file, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->count, 1u);
    EXPECT_EQ(set->defs[0].scale,
              std::strtod("1.0212934580499022e-07", nullptr));
    EXPECT_EQ(set->defs[0].offset, std::strtod("-40.000000000000014", nullptr));
}

// A session on a variant's set registered under the caller's own name
// keeps the plugin from being freed; once it's gone, the free takes that
// name too, so none is left pointing at a freed set
TEST(PluginTest, AFreeWaitsForASessionOnASetRegisteredByHand)
{
    Plugin p = load_packed(packed(kHead, kData));
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_profile_register(
                  "by-hand", ecuio_plugin_variant_set(p.get(), 0), nullptr),
              ECUIO_OK);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=by-hand", &s), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_plugin_free(p.get()), ECUIO_ERR_IN_USE);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_plugin_free(p.release()), ECUIO_OK);
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("by-hand", &set), ECUIO_ERR_UNKNOWN_PROFILE);
}
