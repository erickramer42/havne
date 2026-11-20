// File/layer: tests/test_plugin_support.h | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest (test_signals)

#ifndef ECUIO_TEST_PLUGIN_SUPPORT_H
#define ECUIO_TEST_PLUGIN_SUPPORT_H

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

namespace plugin_test {
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

inline std::string slurp(const std::string& path)
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
inline Folder example_folder()
{
    const std::string root = std::string(ECUIO_PLUGINS_DIR) + "/example-sim/";
    Folder f;
    for (const char* p : {"manifest.json", "data/base.json", "data/my2019.json",
                          "fixtures/values.json"}) {
        f.files[p] = slurp(root + p);
    }
    return f;
}

inline Plugin load_folder(Folder& f, ecuio_status_t* status = nullptr)
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

inline Plugin load_packed(const std::string& json,
                          ecuio_status_t* status = nullptr)
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

inline std::string packed_refusal(const std::string& json)
{
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_packed(json, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT) << json;
    return ecuio_last_error_detail();
}

inline std::string folder_refusal(Folder& f)
{
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_folder(f, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
    return ecuio_last_error_detail();
}

/* A small packed plugin: one data file, one variant; parts replaced by
 * the cases below */
inline std::string
packed(const std::string& head, const std::string& data,
       const std::string& variants = R"([{"id": "base", "files": ["a.json"]}])",
       const std::string& files_extra = "")
{
    return "{\"havne_plugin\": 1, " + head + (head.empty() ? "" : ", ") +
           "\"variants\": " + variants + ", \"files\": {\"a.json\": " + data +
           files_extra + "}}";
}

inline const std::string kHead =
    R"("id": "p", "name": "P", "version": "1.0.0", "requires": {"core": "0.1.0"},)"
    R"( "source": {"name": "t"}, "tiers": ["data"],)"
    R"( "ecus": [{"id": "pcm", "rx": "7E8"}])";

inline const std::string kData =
    R"({"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", "ecu": "pcm",)"
    R"( "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}}]})";

inline std::string variant_id(const ecuio_plugin_t* p, uint32_t i)
{
    char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
    EXPECT_EQ(ecuio_plugin_variant(p, i, id, sizeof id), ECUIO_OK);
    return id;
}

inline uint32_t def_index(const ecuio_signal_set_t* set,
                          const ecuio_signal_def_t* d)
{
    return static_cast<uint32_t>(d - set->defs);
}

inline ecuio_vin_info_t vin_of(const char* wmi, uint16_t year)
{
    ecuio_vin_info_t v{};
    std::strncpy(v.wmi, wmi, sizeof v.wmi - 1);
    v.wmi_length = static_cast<uint8_t>(std::strlen(wmi));
    v.model_year = year;
    return v;
}
} // namespace plugin_test

using namespace plugin_test;

#endif /* ECUIO_TEST_PLUGIN_SUPPORT_H */
