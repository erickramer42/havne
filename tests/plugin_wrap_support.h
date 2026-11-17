// File/layer: tests/plugin_wrap_support.h | test-only, C++17, Qt-free,
//             header-only
// Testing: UNIT: PluginWrapTest (test_signals)
//
// PluginWrapTest's helpers, shared by test_plugin_wrap.cpp and
// test_plugin_put.cpp: the calls' answers, a plugin loaded and read, and
// the signal files and hand-written packages the cases build.

#ifndef ECUIO_TESTS_PLUGIN_WRAP_SUPPORT_H
#define ECUIO_TESTS_PLUGIN_WRAP_SUPPORT_H

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>

#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"              /* core/src: a written plugin's members */
#include "signal_file_build.h" /* core/src: a variant's bytes */

namespace plugin_wrap_test {

using ecuio_json::Value;

/* A call's answer: the status, the text (freed here) and the detail */
struct Answer {
    ecuio_status_t status = ECUIO_OK;
    std::string text;
    std::string detail;
    bool gave = false; /* *out was set */
};

inline Answer taken(ecuio_status_t status, char* out, size_t length)
{
    Answer a;
    a.status = status;
    a.detail = ecuio_last_error_detail();
    a.gave = out != nullptr;
    if (out != nullptr) {
        EXPECT_EQ(std::strlen(out), length) << "the length is the text's";
        a.text.assign(out, length);
    } else {
        EXPECT_EQ(length, 0u) << "no text, no length";
    }
    ecuio_plugin_text_free(out);
    return a;
}

inline Answer wrap(const std::string& file,
                   const ecuio_plugin_wrap_options_t* options = nullptr)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    const ecuio_status_t s =
        ecuio_plugin_wrap(file.data(), file.size(), options, &out, &length);
    return taken(s, out, length);
}

inline Answer put(const std::string& packed, const char* variant,
                  const std::string& file)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    const ecuio_status_t s =
        ecuio_plugin_put_variant(packed.data(), packed.size(), variant,
                                 file.data(), file.size(), &out, &length);
    return taken(s, out, length);
}

inline Answer drop(const std::string& packed, const char* variant)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    const ecuio_status_t s = ecuio_plugin_drop_variant(
        packed.data(), packed.size(), variant, &out, &length);
    return taken(s, out, length);
}

/* The text of a call that must succeed */
inline std::string ok(const Answer& a)
{
    EXPECT_EQ(a.status, ECUIO_OK) << a.detail;
    EXPECT_TRUE(a.gave);
    return a.text;
}

/* The words of a call that must be refused, with no text given */
inline std::string refused(const Answer& a)
{
    EXPECT_EQ(a.status, ECUIO_ERR_INVALID_ARGUMENT) << a.text.substr(0, 200);
    EXPECT_FALSE(a.gave) << "out is NULL on a refusal";
    return a.detail;
}

struct PluginFree {
    void operator()(ecuio_plugin_t* p) const
    {
        EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);
    }
};
using Plugin = std::unique_ptr<ecuio_plugin_t, PluginFree>;

inline Plugin load(const std::string& packed)
{
    ecuio_plugin_t* p = nullptr;
    EXPECT_EQ(
        ecuio_plugin_load(packed.data(), packed.size(), nullptr, nullptr, &p),
        ECUIO_OK)
        << ecuio_last_error_detail();
    return Plugin(p);
}

inline ecuio_plugin_info_t info_of(const ecuio_plugin_t* p)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    EXPECT_EQ(ecuio_plugin_info(p, &info), ECUIO_OK);
    return info;
}

inline std::vector<std::string> variants_of(const ecuio_plugin_t* p)
{
    std::vector<std::string> ids;
    for (uint32_t i = 0; i < info_of(p).variant_count; ++i) {
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
        EXPECT_EQ(ecuio_plugin_variant(p, i, id, sizeof id), ECUIO_OK);
        ids.emplace_back(id);
    }
    return ids;
}

inline std::string lineage_of(const ecuio_plugin_t* p)
{
    char text[ECUIO_PLUGIN_LINEAGE_LEN + 1] = {'x'};
    EXPECT_EQ(ecuio_plugin_lineage(p, text, sizeof text), ECUIO_OK);
    return text;
}

inline std::string requires_of(const ecuio_plugin_t* p)
{
    uint16_t v[3] = {};
    EXPECT_EQ(ecuio_plugin_requires_core(p, &v[0], &v[1], &v[2]), ECUIO_OK);
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." +
           std::to_string(v[2]);
}

inline std::string core_version()
{
    uint32_t v[3] = {};
    EXPECT_EQ(ecuio_version(&v[0], &v[1], &v[2]), ECUIO_OK);
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." +
           std::to_string(v[2]);
}

/* The response ID variant index's signal key is read from, 0 with none */
inline uint32_t rx_of(const ecuio_plugin_t* p, uint32_t index, const char* key)
{
    const ecuio_signal_def_t* d = ecuio_plugin_variant_find(p, index, key);
    EXPECT_NE(d, nullptr) << key;
    return d != nullptr ? d->message_id : 0;
}

/* Variant index's signal file as a recording copies it */
inline std::string written(const ecuio_plugin_t* p, uint32_t index)
{
    const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p, index);
    EXPECT_NE(f, nullptr);
    return f != nullptr ? ecuio_sf::bytes_of(*f) : std::string();
}

inline Value parsed(const std::string& text)
{
    Value root;
    ecuio_json::Error error;
    ecuio_json::Limits limits;
    limits.max_bytes = ECUIO_PLUGIN_PACKAGE_MAX;
    EXPECT_TRUE(
        ecuio_json::parse(text.data(), text.size(), &root, &error, limits))
        << error.to_string();
    return root;
}

/* A written manifest's ECU ids, in order, each as "<id>@<rx>[/<tx>]" */
inline std::vector<std::string> ecus_of(const std::string& packed)
{
    std::vector<std::string> out;
    const Value doc = parsed(packed);
    if (const Value* ecus = doc.find("ecus")) {
        for (const Value& e : ecus->items) {
            std::string s = e.find("id")->text + "@" + e.find("rx")->text;
            if (const Value* tx = e.find("tx")) {
                s += "/" + tx->text;
            }
            out.push_back(s);
        }
    }
    return out;
}

inline std::vector<std::string> files_of(const std::string& packed)
{
    std::vector<std::string> out;
    const Value doc = parsed(packed);
    for (const ecuio_json::Member& m : doc.find("files")->members) {
        out.push_back(m.key);
    }
    return out;
}

inline std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

using Strings = std::vector<std::string>;

/* A signal read by DID from ecu ("" names none) */
inline std::string sig(const std::string& key, const std::string& did,
                       const std::string& ecu)
{
    return "{\"key\": \"" + key + "\", \"name\": \"N " + key + "\"" +
           (ecu.empty() ? "" : ", \"ecu\": \"" + ecu + "\"") +
           ", \"read\": {\"service\": \"22\", \"number\": \"" + did +
           "\"}, \"field\": {\"byte\": 0}}";
}

/* A signal file: ecus and signals as JSON lists' insides, head for any
 * member more ("extends", "notes") */
inline std::string file(const std::string& name, const std::string& source,
                        const std::string& ecus, const std::string& signals,
                        const std::string& head = "")
{
    return "{\"havne_signals\": 1, \"name\": \"" + name +
           "\", \"source\": {\"name\": \"" + source + "\"}, " + head +
           (head.empty() ? "" : ", ") +
           (ecus.empty() ? "" : "\"ecus\": [" + ecus + "], ") +
           "\"signals\": [" + signals + "]}";
}

const char kPcm[] = R"({"id": "pcm", "rx": "7E8"})";
const char kTcm[] = R"({"id": "tcm", "rx": "7E9"})";

/* One ECU, one signal: what most cases start from */
inline std::string file_a()
{
    return file("Daily driver extras", "By hand", kPcm,
                sig("oil", "F40D", "pcm"));
}

inline std::string file_b()
{
    return file("Track day", "From a forum", kPcm, sig("egt", "F40E", "pcm"));
}

/* A packed plugin written by hand: its ECUs, variants and files */
inline std::string packed(const std::string& ecus, const std::string& variants,
                          const std::string& files,
                          const std::string& version = "1.0.0",
                          const std::string& more = "")
{
    return std::string(R"({"havne_plugin": 1, "id": "p", "name": "P", )") +
           "\"version\": \"" + version +
           R"(", "requires": {"core": "0.1.0"}, "source": {"name": "M"}, )" +
           R"("tiers": ["data"], )" + more +
           (ecus.empty() ? "" : "\"ecus\": [" + ecus + "], ") +
           "\"variants\": [" + variants + "], \"files\": {" + files + "}}";
}

/* A data file of a hand-written package */
inline std::string data(const std::string& signals,
                        const std::string& head = "")
{
    return "{\"havne_signals\": 1, " + head + (head.empty() ? "" : ", ") +
           "\"signals\": [" + signals + "]}";
}

inline bool same_sets(const ecuio_signal_set_t& a, const ecuio_signal_set_t& b)
{
    auto same = [](const void* x, const void* y, size_t n) {
        return n == 0 || std::memcmp(x, y, n) == 0;
    };
    return a.count == b.count && a.channel_count == b.channel_count &&
           a.enum_count == b.enum_count &&
           a.computed_count == b.computed_count &&
           same(a.defs, b.defs, sizeof *a.defs * a.count) &&
           same(a.channels, b.channels, sizeof *a.channels * a.channel_count) &&
           same(a.enums, b.enums, sizeof *a.enums * a.enum_count) &&
           same(a.computed, b.computed, sizeof *a.computed * a.computed_count);
}

inline ecuio_plugin_wrap_options_t options()
{
    ecuio_plugin_wrap_options_t o{};
    o.struct_size = sizeof o;
    return o;
}

inline bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace plugin_wrap_test

#endif /* ECUIO_TESTS_PLUGIN_WRAP_SUPPORT_H */
