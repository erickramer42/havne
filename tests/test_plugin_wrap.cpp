// File/layer: tests/test_plugin_wrap.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginWrapTest (test_signals)

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>
#include <ecuio/vin.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"              /* core/src: a written plugin's members */
#include "sha256.h"            /* core/src: a lineage's hash */
#include "signal_file_build.h" /* core/src: a variant's bytes */

#ifndef ECUIO_TEST_FIXTURES
#error "tests/CMakeLists.txt defines ECUIO_TEST_FIXTURES, tests/fixtures"
#endif

namespace {

using ecuio_json::Value;

/* A call's answer: the status, the text (freed here) and the detail */
struct Answer {
    ecuio_status_t status = ECUIO_OK;
    std::string text;
    std::string detail;
    bool gave = false; /* *out was set */
};

Answer taken(ecuio_status_t status, char* out, size_t length)
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

Answer wrap(const std::string& file,
            const ecuio_plugin_wrap_options_t* options = nullptr)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    const ecuio_status_t s =
        ecuio_plugin_wrap(file.data(), file.size(), options, &out, &length);
    return taken(s, out, length);
}

Answer put(const std::string& packed, const char* variant,
           const std::string& file)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    const ecuio_status_t s =
        ecuio_plugin_put_variant(packed.data(), packed.size(), variant,
                                 file.data(), file.size(), &out, &length);
    return taken(s, out, length);
}

Answer drop(const std::string& packed, const char* variant)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    const ecuio_status_t s = ecuio_plugin_drop_variant(
        packed.data(), packed.size(), variant, &out, &length);
    return taken(s, out, length);
}

/* The text of a call that must succeed */
std::string ok(const Answer& a)
{
    EXPECT_EQ(a.status, ECUIO_OK) << a.detail;
    EXPECT_TRUE(a.gave);
    return a.text;
}

/* The words of a call that must be refused, with no text given */
std::string refused(const Answer& a)
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

Plugin load(const std::string& packed)
{
    ecuio_plugin_t* p = nullptr;
    EXPECT_EQ(
        ecuio_plugin_load(packed.data(), packed.size(), nullptr, nullptr, &p),
        ECUIO_OK)
        << ecuio_last_error_detail();
    return Plugin(p);
}

ecuio_plugin_info_t info_of(const ecuio_plugin_t* p)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    EXPECT_EQ(ecuio_plugin_info(p, &info), ECUIO_OK);
    return info;
}

std::vector<std::string> variants_of(const ecuio_plugin_t* p)
{
    std::vector<std::string> ids;
    for (uint32_t i = 0; i < info_of(p).variant_count; ++i) {
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
        EXPECT_EQ(ecuio_plugin_variant(p, i, id, sizeof id), ECUIO_OK);
        ids.emplace_back(id);
    }
    return ids;
}

std::string lineage_of(const ecuio_plugin_t* p)
{
    char text[ECUIO_PLUGIN_LINEAGE_LEN + 1] = {'x'};
    EXPECT_EQ(ecuio_plugin_lineage(p, text, sizeof text), ECUIO_OK);
    return text;
}

std::string requires_of(const ecuio_plugin_t* p)
{
    uint16_t v[3] = {};
    EXPECT_EQ(ecuio_plugin_requires_core(p, &v[0], &v[1], &v[2]), ECUIO_OK);
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." +
           std::to_string(v[2]);
}

std::string core_version()
{
    uint32_t v[3] = {};
    EXPECT_EQ(ecuio_version(&v[0], &v[1], &v[2]), ECUIO_OK);
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." +
           std::to_string(v[2]);
}

/* The response ID variant index's signal key is read from, 0 with none */
uint32_t rx_of(const ecuio_plugin_t* p, uint32_t index, const char* key)
{
    const ecuio_signal_def_t* d = ecuio_plugin_variant_find(p, index, key);
    EXPECT_NE(d, nullptr) << key;
    return d != nullptr ? d->message_id : 0;
}

/* Variant index's signal file as a recording copies it */
std::string written(const ecuio_plugin_t* p, uint32_t index)
{
    const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p, index);
    EXPECT_NE(f, nullptr);
    return f != nullptr ? ecuio_sf::bytes_of(*f) : std::string();
}

Value parsed(const std::string& text)
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
std::vector<std::string> ecus_of(const std::string& packed)
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

std::vector<std::string> files_of(const std::string& packed)
{
    std::vector<std::string> out;
    const Value doc = parsed(packed);
    for (const ecuio_json::Member& m : doc.find("files")->members) {
        out.push_back(m.key);
    }
    return out;
}

std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

using Strings = std::vector<std::string>;

/* A signal read by DID from ecu ("" names none) */
std::string sig(const std::string& key, const std::string& did,
                const std::string& ecu)
{
    return "{\"key\": \"" + key + "\", \"name\": \"N " + key + "\"" +
           (ecu.empty() ? "" : ", \"ecu\": \"" + ecu + "\"") +
           ", \"read\": {\"service\": \"22\", \"number\": \"" + did +
           "\"}, \"field\": {\"byte\": 0}}";
}

/* A signal file: ecus and signals as JSON lists' insides, head for any
 * member more ("extends", "notes") */
std::string file(const std::string& name, const std::string& source,
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
std::string file_a()
{
    return file("Daily driver extras", "By hand", kPcm,
                sig("oil", "F40D", "pcm"));
}

std::string file_b()
{
    return file("Track day", "From a forum", kPcm, sig("egt", "F40E", "pcm"));
}

/* A packed plugin written by hand: its ECUs, variants and files */
std::string packed(const std::string& ecus, const std::string& variants,
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
std::string data(const std::string& signals, const std::string& head = "")
{
    return "{\"havne_signals\": 1, " + head + (head.empty() ? "" : ", ") +
           "\"signals\": [" + signals + "]}";
}

bool same_sets(const ecuio_signal_set_t& a, const ecuio_signal_set_t& b)
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

ecuio_plugin_wrap_options_t options()
{
    ecuio_plugin_wrap_options_t o{};
    o.struct_size = sizeof o;
    return o;
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

// The committed signal-file fixture wraps to a plugin whose one variant
// decodes as the file alone does: the same tables and the same keys
TEST(PluginWrapTest, AFileWrapsToAPluginThatDecodesAsTheFileAlone)
{
    const std::string bytes = slurp(std::string(ECUIO_TEST_FIXTURES) +
                                    "/signal-files/daily-driver-extras.json");
    const std::string text = ok(wrap(bytes));
    Plugin p = load(text);
    ASSERT_TRUE(p);
    const ecuio_plugin_info_t info = info_of(p.get());
    EXPECT_STREQ(info.id, "daily-driver-extras");
    EXPECT_STREQ(info.name, "Daily driver extras");
    EXPECT_STREQ(info.version, "1.0.0");
    EXPECT_EQ(variants_of(p.get()), Strings{"base"});
    EXPECT_EQ(requires_of(p.get()), core_version());

    ecuio_signal_file_t* alone = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(bytes.data(), bytes.size(), &alone),
              ECUIO_OK);
    const ecuio_signal_set_t* a = ecuio_signal_file_set(alone);
    const ecuio_signal_set_t* b = ecuio_plugin_variant_set(p.get(), 0);
    EXPECT_TRUE(same_sets(*a, *b));
    const ecuio_signal_file_t* variant = ecuio_plugin_variant_file(p.get(), 0);
    EXPECT_STREQ(ecuio_signal_file_profile(variant),
                 "plugin.daily-driver-extras.base");
    EXPECT_STREQ(ecuio_signal_file_extends(variant), "obd2");
    uint32_t own = 0;
    for (uint32_t i = 0; i < a->count; ++i) {
        const std::string key = ecuio_signal_file_key(alone, &a->defs[i]);
        if (key.empty()) {
            continue;
        }
        ++own;
        const ecuio_signal_def_t* d =
            ecuio_plugin_variant_find(p.get(), 0, key.c_str());
        ASSERT_NE(d, nullptr) << key;
        EXPECT_EQ(d - b->defs, static_cast<std::ptrdiff_t>(i)) << key;
    }
    EXPECT_GT(own, 0u);
    ecuio_signal_file_free(alone);
}

// The bytes of a wrap are a function of the input alone, the same on
// every run and platform: this text is what every build must write.
// The input has CRLF line ends, a tab, UTF-8 beyond ASCII, an escape
// and numbers of each kind the writer tells apart.
TEST(PluginWrapTest, AWrapsBytesAreTheSameEverywhere)
{
    const std::string in =
        "{\r\n\t\"havne_signals\": 1,\r\n"
        "  \"name\": \"Caf\xC3\xA9 \\u00e9 \\\"daily\\\"\",\r\n"
        "  \"source\": {\"name\": \"By hand\", \"license\": \"MIT\"},\r\n"
        "  \"x-numbers\": [0.1, 1e21, -0, 1.0, 12345678901234567890, 1E2, "
        "0.30000000000000004],\r\n"
        "  \"ecus\": [{\"id\": \"pcm\", \"rx\": \"7e8\"}],\r\n"
        "  \"signals\": [{\"key\": \"oil\", \"name\": \"Oil\", \"ecu\": "
        "\"pcm\",\r\n"
        "    \"read\": {\"service\": \"22\", \"number\": \"F40D\"},\r\n"
        "    \"field\": {\"byte\": 0}, \"scale\": 0.5, \"offset\": -40}]\r\n"
        "}\r\n";
    const std::string want =
        "{\n"
        "  \"havne_plugin\": 1,\n"
        "  \"id\": \"caf-daily\",\n"
        "  \"name\": \"Caf\xC3\xA9 \xC3\xA9 \\\"daily\\\"\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"requires\": {\n"
        "    \"core\": \"" +
        core_version() +
        "\"\n"
        "  },\n"
        "  \"lineage\": \"@LINEAGE@\",\n"
        "  \"source\": {\n"
        "    \"name\": \"By hand\",\n"
        "    \"license\": \"MIT\"\n"
        "  },\n"
        "  \"tiers\": [\n"
        "    \"data\"\n"
        "  ],\n"
        "  \"ecus\": [\n"
        "    {\n"
        "      \"id\": \"pcm\",\n"
        "      \"rx\": \"7e8\"\n"
        "    }\n"
        "  ],\n"
        "  \"variants\": [\n"
        "    {\n"
        "      \"id\": \"base\",\n"
        "      \"files\": [\n"
        "        \"data/base.json\"\n"
        "      ]\n"
        "    }\n"
        "  ],\n"
        "  \"files\": {\n"
        "    \"data/base.json\": {\n"
        "      \"havne_signals\": 1,\n"
        "      \"name\": \"Caf\xC3\xA9 \xC3\xA9 \\\"daily\\\"\",\n"
        "      \"x-numbers\": [\n"
        "        0.1,\n"
        "        1e+21,\n"
        "        -0,\n"
        "        1.0,\n"
        "        12345678901234567168,\n"
        "        100.0,\n"
        "        0.30000000000000004\n"
        "      ],\n"
        "      \"signals\": [\n"
        "        {\n"
        "          \"key\": \"oil\",\n"
        "          \"name\": \"Oil\",\n"
        "          \"ecu\": \"pcm\",\n"
        "          \"read\": {\n"
        "            \"service\": \"22\",\n"
        "            \"number\": \"F40D\"\n"
        "          },\n"
        "          \"field\": {\n"
        "            \"byte\": 0\n"
        "          },\n"
        "          \"scale\": 0.5,\n"
        "          \"offset\": -40\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n";
    auto with = [&want](const std::string& lineage) {
        std::string w = want;
        w.replace(w.find("@LINEAGE@"), 9, lineage);
        return w;
    };

    const std::string text = ok(wrap(in));
    /* The first 32 hex digits of the SHA-256 of the bytes given, pinned
     * so a build that hashes anything else differs */
    EXPECT_EQ(text, with("06e18defa83820f32c3918d85cb9e3d0"));
    EXPECT_EQ(ok(wrap(in)), text) << "a second wrap gives the same bytes";
    EXPECT_FALSE(has(text, "\r"));

    /* The same file with LF line ends: only its hash differs */
    std::string lf = in;
    lf.erase(std::remove(lf.begin(), lf.end(), '\r'), lf.end());
    EXPECT_EQ(ok(wrap(lf)),
              with(ecuio_sha256::hex_of(lf.data(), lf.size()).substr(0, 32)));
    EXPECT_NE(ok(wrap(lf)), text);
}

// The file's source and ECUs move to the manifest; every other member
// stays in the data file, as it was read and in its order
TEST(PluginWrapTest, TheSourceAndEcusMoveToTheManifest)
{
    const std::string in =
        file("Daily driver extras", "By hand", std::string(kPcm) + ", " + kTcm,
             sig("oil", "F40D", "pcm") + ", " + sig("tft", "1E1C", "tcm"),
             R"("notes": "kept", "x-tool": {"n": 1}, "extends": "obd2")");
    const std::string text = ok(wrap(in));
    const Value doc = parsed(text);
    Strings keys;
    for (const ecuio_json::Member& m : doc.members) {
        keys.push_back(m.key);
    }
    EXPECT_EQ(keys, (Strings{"havne_plugin", "id", "name", "version",
                             "requires", "lineage", "source", "tiers", "ecus",
                             "variants", "files"}));
    EXPECT_EQ(doc.find("source")->find("name")->text, "By hand");
    EXPECT_EQ(ecus_of(text), (Strings{"pcm@7E8", "tcm@7E9"}));
    EXPECT_EQ(doc.find("match"), nullptr) << "no match unless asked";

    const Value& data_file = *doc.find("files")->find("data/base.json");
    keys.clear();
    for (const ecuio_json::Member& m : data_file.members) {
        keys.push_back(m.key);
    }
    EXPECT_EQ(keys, (Strings{"havne_signals", "name", "notes", "x-tool",
                             "extends", "signals"}));
    EXPECT_EQ(data_file.find("notes")->text, "kept");
    EXPECT_EQ(data_file.find("x-tool")->find("n")->whole, 1);

    Plugin p = load(text);
    ASSERT_TRUE(p);
    EXPECT_EQ(rx_of(p.get(), 0, "oil"), 0x7E8u);
    EXPECT_EQ(rx_of(p.get(), 0, "tft"), 0x7E9u);
    EXPECT_TRUE(has(written(p.get(), 0), "\"name\": \"By hand\""));

    /* A file with no ECUs makes a manifest with none */
    const std::string bare = ok(wrap(
        file("Bare", "S", "", sig("oil", "F40D", ""), R"("extends": "obd2")")));
    EXPECT_EQ(parsed(bare).find("ecus"), nullptr);
    Plugin b = load(bare);
    ASSERT_TRUE(b);
    EXPECT_EQ(rx_of(b.get(), 0, "oil"), 0x7E8u);
}

// The id, the variant and the match come from the options when given
TEST(PluginWrapTest, TheOptionsNameTheIdTheVariantAndTheMatch)
{
    ecuio_plugin_wrap_options_t o = options();
    o.id = "my-car-2";
    o.variant = "my2019";
    o.wmi = "ZZZ,ZZ9ABC";
    o.years[0] = 2015;
    o.years[1] = 2030;
    const std::string text = ok(wrap(file_a(), &o));
    Plugin p = load(text);
    ASSERT_TRUE(p);
    EXPECT_STREQ(info_of(p.get()).id, "my-car-2");
    EXPECT_STREQ(info_of(p.get()).name, "Daily driver extras");
    EXPECT_EQ(variants_of(p.get()), Strings{"my2019"});
    EXPECT_EQ(files_of(text), Strings{"data/my2019.json"});
    const Value doc = parsed(text);
    const Value& rule = doc.find("match")->items.at(0);
    ASSERT_EQ(rule.find("wmi")->items.size(), 2u);
    EXPECT_EQ(rule.find("wmi")->items[0].text, "ZZZ");
    EXPECT_EQ(rule.find("wmi")->items[1].text, "ZZ9ABC");
    EXPECT_EQ(rule.find("years")->items.at(0).whole, 2015);
    EXPECT_EQ(rule.find("years")->items.at(1).whole, 2030);

    auto matches = [&p](const char* wmi, uint16_t year) {
        ecuio_vin_info_t v{};
        std::strncpy(v.wmi, wmi, sizeof v.wmi - 1);
        v.wmi_length = static_cast<uint8_t>(std::strlen(wmi));
        v.model_year = year;
        uint32_t n = 99;
        EXPECT_EQ(ecuio_plugin_match(p.get(), &v, nullptr, 0, &n), ECUIO_OK);
        return n;
    };
    EXPECT_EQ(matches("ZZZ", 2021), 1u);
    EXPECT_EQ(matches("ZZ9ABC", 2015), 1u);
    EXPECT_EQ(matches("ZZZ", 2031), 0u);
    EXPECT_EQ(matches("ZZY", 2021), 0u);

    /* Codes alone: a match of any year */
    ecuio_plugin_wrap_options_t codes = options();
    codes.wmi = "ZZZ";
    const std::string any = ok(wrap(file_a(), &codes));
    EXPECT_EQ(parsed(any).find("match")->items.at(0).find("years"), nullptr);

    /* Every default: the same bytes as no options at all */
    const ecuio_plugin_wrap_options_t none = options();
    EXPECT_EQ(ok(wrap(file_a(), &none)), ok(wrap(file_a())));
    Plugin d = load(ok(wrap(file_a())));
    uint32_t n = 99;
    ecuio_vin_info_t v{};
    std::strncpy(v.wmi, "ZZZ", sizeof v.wmi - 1);
    v.wmi_length = 3;
    v.model_year = 2021;
    EXPECT_EQ(ecuio_plugin_match(d.get(), &v, nullptr, 0, &n), ECUIO_OK);
    EXPECT_EQ(n, 0u);

    /* An options struct from a later header, longer than this one: what
     * lies past the struct is never read, and the flag bit a later field
     * comes with is what this core refuses */
    struct Later {
        ecuio_plugin_wrap_options_t known;
        uint32_t newer[2];
    };
    Later later{};
    later.known = options();
    later.known.struct_size = sizeof later;
    later.newer[0] = 0xFFFFFFFFu;
    later.newer[1] = 7;
    EXPECT_EQ(ok(wrap(file_a(), &later.known)), ok(wrap(file_a())));
    later.known.flags = 1;
    EXPECT_EQ(refused(wrap(file_a(), &later.known)),
              "the options' flags and reserved words are zero");
}

// The generated id is the file's name as user.<slug>'s slug, cut to the
// id's 31 bytes with no '-' left at its end
TEST(PluginWrapTest, TheGeneratedIdIsTheNamesSlugCutToFit)
{
    auto id_for = [](const std::string& name) {
        Plugin p =
            load(ok(wrap(file(name, "S", kPcm, sig("oil", "F40D", "pcm")))));
        return p ? std::string(info_of(p.get()).id) : std::string();
    };
    EXPECT_EQ(id_for("Daily driver (extras)!"), "daily-driver-extras");
    EXPECT_EQ(id_for("  X  "), "x");
    /* 31 bytes exactly, then 32 cut to 31 */
    EXPECT_EQ(id_for("Abcdefghij Abcdefghij Abcdefghi"),
              "abcdefghij-abcdefghij-abcdefghi");
    EXPECT_EQ(id_for("Abcdefghij Abcdefghij AbcdefghiX"),
              "abcdefghij-abcdefghij-abcdefghi");
    /* The cut lands on a '-': it goes too */
    EXPECT_EQ(id_for("Abcdefghij Abcdefghij Abcdefgh Xyz"),
              "abcdefghij-abcdefghij-abcdefgh");
}

// A lineage is the start of the SHA-256 of the bytes wrapped; the loader
// takes one of 32 lower-case hex digits and refuses any other
TEST(PluginWrapTest, ALineageIsTheStartOfTheWrappedFilesHash)
{
    const std::string a = file_a();
    Plugin p = load(ok(wrap(a)));
    ASSERT_TRUE(p);
    const std::string want =
        ecuio_sha256::hex_of(a.data(), a.size()).substr(0, 32);
    EXPECT_EQ(lineage_of(p.get()), want);
    EXPECT_EQ(want.size(), size_t{ECUIO_PLUGIN_LINEAGE_LEN});
    /* The options change the plugin, never where it comes from */
    ecuio_plugin_wrap_options_t o = options();
    o.id = "other";
    Plugin named = load(ok(wrap(a, &o)));
    EXPECT_EQ(lineage_of(named.get()), want);
    Plugin other = load(ok(wrap(file_b())));
    EXPECT_NE(lineage_of(other.get()), want);

    /* A manifest with none */
    const std::string hand =
        packed(kPcm, R"({"id": "base", "files": ["a.json"]})",
               "\"a.json\": " + data(sig("oil", "F40D", "pcm")));
    Plugin none = load(hand);
    ASSERT_TRUE(none);
    EXPECT_EQ(lineage_of(none.get()), "");

    char buffer[ECUIO_PLUGIN_LINEAGE_LEN + 1] = {};
    EXPECT_EQ(ecuio_plugin_lineage(nullptr, buffer, sizeof buffer),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_lineage(p.get(), nullptr, sizeof buffer),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_lineage(p.get(), buffer, ECUIO_PLUGIN_LINEAGE_LEN),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "no room for the NUL";
    EXPECT_EQ(ecuio_plugin_lineage(none.get(), buffer, 1), ECUIO_OK);
    EXPECT_EQ(ecuio_plugin_lineage(none.get(), buffer, 0),
              ECUIO_ERR_INVALID_ARGUMENT);

    auto lineage_refusal = [](const std::string& value) {
        const std::string text =
            packed(kPcm, R"({"id": "base", "files": ["a.json"]})",
                   "\"a.json\": " + data(sig("oil", "F40D", "pcm")), "1.0.0",
                   "\"lineage\": " + value + ", ");
        ecuio_plugin_t* p = nullptr;
        EXPECT_EQ(
            ecuio_plugin_load(text.data(), text.size(), nullptr, nullptr, &p),
            ECUIO_ERR_INVALID_ARGUMENT)
            << value;
        return std::string(ecuio_last_error_detail());
    };
    const std::string good(32, 'a');
    Plugin taken_p =
        load(packed(kPcm, R"({"id": "base", "files": ["a.json"]})",
                    "\"a.json\": " + data(sig("oil", "F40D", "pcm")), "1.0.0",
                    "\"lineage\": \"0123456789abcdef0123456789abcdef\", "));
    ASSERT_TRUE(taken_p);
    EXPECT_EQ(lineage_of(taken_p.get()), "0123456789abcdef0123456789abcdef");
    for (const std::string& bad :
         {"\"" + std::string(31, 'a') + "\"",
          "\"" + std::string(33, 'a') + "\"",
          "\"" + std::string(31, 'a') + "A\"",
          "\"" + std::string(31, 'a') + "g\"",
          "\"" + std::string(31, 'a') + "/\"",
          "\"" + std::string(31, 'a') + ":\"",
          "\"" + std::string(31, 'a') + "`\"", std::string("\"\"")}) {
        EXPECT_TRUE(has(lineage_refusal(bad), "\"lineage\" ") &&
                    has(lineage_refusal(bad), " takes 32 of 0-9 and a-f"))
            << bad << ": " << lineage_refusal(bad);
    }
    EXPECT_TRUE(
        has(lineage_refusal("7"), "\"lineage\" is a number, not a string"));
}

// A put adds a variant after the others; the version's minor rises and
// its patch goes to 0, requires.core becomes this core's, and the id,
// the name, the lineage and the source stay
TEST(PluginWrapTest, APutAddsAVariantAndRaisesTheMinor)
{
    const std::string one = ok(wrap(file_a()));
    const Answer added = put(one, "track", file_b());
    const std::string two = ok(added);
    EXPECT_EQ(added.detail, "") << "nothing to say when no ECU moved";
    Plugin p = load(two);
    ASSERT_TRUE(p);
    EXPECT_EQ(variants_of(p.get()), (Strings{"base", "track"}));
    EXPECT_STREQ(info_of(p.get()).version, "1.1.0");
    EXPECT_STREQ(info_of(p.get()).id, "daily-driver-extras");
    EXPECT_STREQ(info_of(p.get()).name, "Daily driver extras");
    Plugin before = load(one);
    EXPECT_EQ(lineage_of(p.get()), lineage_of(before.get()));
    EXPECT_EQ(files_of(two), (Strings{"data/base.json", "data/track.json"}));
    EXPECT_EQ(ecus_of(two), Strings{"pcm@7E8"});
    EXPECT_EQ(parsed(two).find("source")->find("name")->text, "By hand");
    EXPECT_NE(ecuio_plugin_variant_find(p.get(), 0, "oil"), nullptr);
    EXPECT_EQ(ecuio_plugin_variant_find(p.get(), 0, "egt"), nullptr);
    EXPECT_NE(ecuio_plugin_variant_find(p.get(), 1, "egt"), nullptr);
    EXPECT_EQ(ecuio_plugin_variant_find(p.get(), 1, "oil"), nullptr);

    /* The same call again gives the same bytes; a third variant goes on */
    EXPECT_EQ(ok(put(one, "track", file_b())), two);
    Plugin three = load(ok(put(two, "wet", file_b())));
    ASSERT_TRUE(three);
    EXPECT_EQ(variants_of(three.get()), (Strings{"base", "track", "wet"}));
    EXPECT_STREQ(info_of(three.get()).version, "1.2.0");

    /* A plugin written by hand: its patch goes, its requires rises, and
     * with no lineage it gets none */
    const std::string hand =
        packed(kPcm, R"({"id": "base", "files": ["a.json"]})",
               "\"a.json\": " + data(sig("oil", "F40D", "pcm")), "2.5.7");
    const std::string grown = ok(put(hand, "track", file_b()));
    Plugin g = load(grown);
    ASSERT_TRUE(g);
    EXPECT_STREQ(info_of(g.get()).version, "2.6.0");
    EXPECT_EQ(requires_of(g.get()), core_version());
    EXPECT_NE(core_version(), "0.1.0");
    EXPECT_EQ(lineage_of(g.get()), "");
    EXPECT_EQ(parsed(grown).find("lineage"), nullptr);
    EXPECT_EQ(files_of(grown), (Strings{"a.json", "data/track.json"}));
}

// A put of a variant the plugin has replaces it in its place: its years
// stay, its files go unless another variant lists them
TEST(PluginWrapTest, APutReplacesAVariantInItsPlace)
{
    const std::string one = ok(put(ok(wrap(file_a())), "track", file_b()));
    const std::string swapped =
        ok(put(one, "base",
               file("Other", "S2", kPcm, sig("boost_raw", "F411", "pcm"))));
    Plugin p = load(swapped);
    ASSERT_TRUE(p);
    EXPECT_EQ(variants_of(p.get()), (Strings{"base", "track"}));
    EXPECT_STREQ(info_of(p.get()).version, "1.2.0");
    EXPECT_STREQ(info_of(p.get()).name, "Daily driver extras");
    EXPECT_EQ(ecuio_plugin_variant_find(p.get(), 0, "oil"), nullptr);
    EXPECT_NE(ecuio_plugin_variant_find(p.get(), 0, "boost_raw"), nullptr);
    EXPECT_NE(ecuio_plugin_variant_find(p.get(), 1, "egt"), nullptr);
    EXPECT_EQ(files_of(swapped),
              (Strings{"data/track.json", "data/base.json"}));

    /* A file two variants list stays for the other, and the new file
     * takes the first path free: "data/base.json" is a file that stays */
    const std::string hand = packed(
        kPcm,
        R"({"id": "base", "years": [2015, 2018], "notes": "n", "files": ["data/base.json"]}, )"
        R"({"id": "my2019", "files": ["data/base.json", "b.json"]})",
        "\"data/base.json\": " + data(sig("oil", "F40D", "pcm")) +
            ", \"b.json\": " + data(sig("egt", "F40E", "pcm")));
    const std::string put_base = ok(put(hand, "base", file_b()));
    EXPECT_EQ(files_of(put_base),
              (Strings{"data/base.json", "b.json", "data/base-2.json"}));
    const Value doc = parsed(put_base);
    const Value& entry = doc.find("variants")->items.at(0);
    EXPECT_EQ(entry.find("id")->text, "base");
    EXPECT_EQ(entry.find("years")->items.at(0).whole, 2015);
    EXPECT_EQ(entry.find("notes")->text, "n");
    ASSERT_EQ(entry.find("files")->items.size(), 1u);
    EXPECT_EQ(entry.find("files")->items[0].text, "data/base-2.json");
    Plugin h = load(put_base);
    ASSERT_TRUE(h);
    EXPECT_EQ(ecuio_plugin_variant_find(h.get(), 0, "oil"), nullptr);
    EXPECT_NE(ecuio_plugin_variant_find(h.get(), 1, "oil"), nullptr)
        << "my2019 still reads the file it shared";

    /* The other variant's own file goes with it; the shared one stays */
    const std::string put_2019 = ok(put(hand, "my2019", file_b()));
    EXPECT_EQ(files_of(put_2019),
              (Strings{"data/base.json", "data/my2019.json"}));
}

// With no variant named, a put takes the file's name as a slug, cut to
// a variant id's 24 bytes
TEST(PluginWrapTest, APutWithNoVariantNamedTakesTheFilesNameAsItsId)
{
    const std::string one = ok(wrap(file_a()));
    Plugin p = load(ok(put(one, nullptr, file_b())));
    ASSERT_TRUE(p);
    EXPECT_EQ(variants_of(p.get()), (Strings{"base", "track-day"}));
    Plugin cut = load(ok(put(one, nullptr,
                             file("Abcdefghij Abcdefghij Ab Xyz", "S", kPcm,
                                  sig("egt", "F40E", "pcm")))));
    ASSERT_TRUE(cut);
    EXPECT_EQ(variants_of(cut.get()),
              (Strings{"base", "abcdefghij-abcdefghij-ab"}));
    /* A name that is the variant's replaces it */
    Plugin same = load(ok(
        put(one, nullptr, file("Base", "S", kPcm, sig("egt", "F40E", "pcm")))));
    ASSERT_TRUE(same);
    EXPECT_EQ(variants_of(same.get()), Strings{"base"});
}

// A data file's own source names its variant in a recording's copy; a
// variant whose files have none keeps the manifest's
TEST(PluginWrapTest, ADataFilesOwnSourceNamesItsVariant)
{
    const std::string two = ok(put(ok(wrap(file_a())), "track", file_b()));
    const Value doc = parsed(two);
    EXPECT_EQ(doc.find("files")->find("data/base.json")->find("source"),
              nullptr)
        << "the wrapped file's source moved to the manifest";
    EXPECT_EQ(doc.find("files")
                  ->find("data/track.json")
                  ->find("source")
                  ->find("name")
                  ->text,
              "From a forum");
    Plugin p = load(two);
    ASSERT_TRUE(p);
    EXPECT_TRUE(has(written(p.get(), 0), "\"name\": \"By hand\""));
    EXPECT_FALSE(has(written(p.get(), 0), "From a forum"));
    EXPECT_TRUE(has(written(p.get(), 1), "\"name\": \"From a forum\""));
    EXPECT_FALSE(has(written(p.get(), 1), "By hand"));

    /* A variant of several files: the first that has a source */
    const std::string src1 = R"("source": {"name": "First"})";
    const std::string src2 = R"("source": {"name": "Second"})";
    auto written_for = [&](const std::string& head1, const std::string& head2) {
        Plugin h = load(packed(
            kPcm, R"({"id": "base", "files": ["a.json", "b.json"]})",
            "\"a.json\": " + data(sig("oil", "F40D", "pcm"), head1) +
                ", \"b.json\": " + data(sig("egt", "F40E", "pcm"), head2)));
        return h ? written(h.get(), 0) : std::string();
    };
    EXPECT_TRUE(has(written_for("", ""), "\"name\": \"M\""));
    EXPECT_TRUE(has(written_for(src1, ""), "\"name\": \"First\""));
    EXPECT_TRUE(has(written_for("", src2), "\"name\": \"Second\""));
    EXPECT_TRUE(has(written_for(src1, src2), "\"name\": \"First\""));
    EXPECT_FALSE(has(written_for(src1, src2), "Second"));
    EXPECT_FALSE(has(written_for(src1, ""), "\"name\": \"M\""));
}

// The file's ECUs join the manifest's by their addresses: one the
// manifest has is that ECU whatever its id, any other is added, renamed
// when the manifest has its id elsewhere, and the user is told
TEST(PluginWrapTest, APutsEcusJoinTheManifestsByAddress)
{
    const std::string one = ok(wrap(file_a())); /* pcm@7E8 */

    /* The manifest's ECU under another id: its signals name the
     * manifest's id */
    const Answer alias = put(one, "v",
                             file("V", "S", R"({"id": "ecm", "rx": "7E8"})",
                                  sig("egt", "F40E", "ecm")));
    const std::string aliased = ok(alias);
    EXPECT_EQ(alias.detail, "the file's ecu 'ecm' is the plugin's 'pcm'");
    EXPECT_EQ(ecus_of(aliased), Strings{"pcm@7E8"});
    Plugin a = load(aliased);
    ASSERT_TRUE(a);
    EXPECT_EQ(rx_of(a.get(), 1, "egt"), 0x7E8u);

    /* The manifest's id at other addresses: renamed, its signals with it */
    const Answer clash = put(one, "v",
                             file("V", "S", R"({"id": "pcm", "rx": "7E9"})",
                                  sig("egt", "F40E", "pcm")));
    const std::string renamed = ok(clash);
    EXPECT_EQ(clash.detail, "the file's ecu 'pcm' is 'pcm_2' in the plugin, "
                            "whose 'pcm' has other addresses");
    EXPECT_EQ(ecus_of(renamed), (Strings{"pcm@7E8", "pcm_2@7E9"}));
    Plugin r = load(renamed);
    ASSERT_TRUE(r);
    EXPECT_EQ(rx_of(r.get(), 0, "oil"), 0x7E8u);
    EXPECT_EQ(rx_of(r.get(), 1, "egt"), 0x7E9u);

    /* The same file again, as another variant: pcm_2 is found by its
     * addresses, and no pcm_3 is made */
    const Answer again = put(renamed, "w",
                             file("W", "S", R"({"id": "pcm", "rx": "7E9"})",
                                  sig("egt", "F40E", "pcm")));
    EXPECT_EQ(ecus_of(ok(again)), (Strings{"pcm@7E8", "pcm_2@7E9"}));
    EXPECT_EQ(again.detail, "the file's ecu 'pcm' is the plugin's 'pcm_2'");

    /* The same rx with another tx is another ECU, which the loader then
     * refuses beside the first */
    const std::string other_tx =
        refused(put(one, "v",
                    file("V", "S", R"({"id": "ecm", "rx": "7E8", "tx": "7E1"})",
                         sig("egt", "F40E", "ecm"))));
    EXPECT_EQ(other_tx, "ecu 'ecm' shares its rx with another ECU but not its "
                        "tx; the file can be a plugin of its own");

    /* The same tx with another rx is another ECU too, added under its
     * own id */
    const Answer other_rx =
        put(one, "v",
            file("V", "S", R"({"id": "aux", "rx": "7EC", "tx": "7E0"})",
                 sig("egt", "F40E", "aux")));
    EXPECT_EQ(ecus_of(ok(other_rx)), (Strings{"pcm@7E8", "aux@7EC/7E0"}));
    EXPECT_EQ(other_rx.detail, "");
    Plugin x = load(other_rx.text);
    ASSERT_TRUE(x);
    EXPECT_EQ(rx_of(x.get(), 1, "egt"), 0x7ECu);

    /* A tx written out and one left to the standard are one address */
    const Answer spelled =
        put(one, "v",
            file("V", "S", R"({"id": "pcm", "rx": "7e8", "tx": "7E0"})",
                 sig("egt", "F40E", "pcm")));
    EXPECT_EQ(ecus_of(ok(spelled)), Strings{"pcm@7E8"});
    EXPECT_EQ(spelled.detail, "");

    /* Two of the file's at once: each takes its own way, and a new id
     * the manifest lacks is kept */
    const Answer both = put(
        one, "v",
        file(
            "V", "S",
            R"({"id": "pcm", "rx": "7EA"}, {"id": "tcm", "rx": "7E9", "x-n": 1})",
            sig("egt", "F40E", "pcm") + ", " + sig("tft", "1E1C", "tcm")));
    const std::string mixed = ok(both);
    EXPECT_EQ(ecus_of(mixed), (Strings{"pcm@7E8", "pcm_2@7EA", "tcm@7E9"}));
    EXPECT_EQ(parsed(mixed).find("ecus")->items.at(2).find("x-n")->whole, 1)
        << "an added ECU keeps its other members";
    EXPECT_EQ(both.detail, "the file's ecu 'pcm' is 'pcm_2' in the plugin, "
                           "whose 'pcm' has other addresses");
    Plugin m = load(mixed);
    ASSERT_TRUE(m);
    EXPECT_EQ(rx_of(m.get(), 1, "egt"), 0x7EAu);
    EXPECT_EQ(rx_of(m.get(), 1, "tft"), 0x7E9u);

    /* The next free number, and an id cut so its 31 bytes hold it */
    const std::string long_id(31, 'e');
    const std::string crowded = ok(
        put(ok(wrap(file(
                "C", "S",
                R"({"id": "pcm", "rx": "7E8"}, {"id": "pcm_2", "rx": "7E9"}, )"
                "{\"id\": \"" +
                    long_id + "\", \"rx\": \"7EA\"}",
                sig("oil", "F40D", "pcm")))),
            "v",
            file("V", "S", "{\"id\": \"pcm\", \"rx\": \"7EB\"}",
                 sig("egt", "F40E", "pcm"))));
    EXPECT_EQ(ecus_of(crowded).back(), "pcm_3@7EB");
    const Answer cut = put(
        ok(wrap(file("C", "S", "{\"id\": \"" + long_id + "\", \"rx\": \"7EA\"}",
                     sig("oil", "F40D", long_id)))),
        "v",
        file("V", "S", "{\"id\": \"" + long_id + "\", \"rx\": \"7EB\"}",
             sig("egt", "F40E", long_id)));
    EXPECT_EQ(ecus_of(ok(cut)).back(), std::string(29, 'e') + "_2@7EB");
}

// Notes past what the detail holds end "and N more", never cut mid-note
TEST(PluginWrapTest, PutsNotesPastTheDetailEndWithHowManyMore)
{
    const std::string a(31, 'a');
    const std::string b(31, 'b');
    const std::string c(31, 'c');
    const std::string d(31, 'd');
    const auto ecu = [](const std::string& id, const char* rx) {
        return "{\"id\": \"" + id + "\", \"rx\": \"" + rx + "\"}";
    };
    const std::string plugin =
        ok(wrap(file("P", "S", ecu(a, "7E8") + ", " + ecu(b, "7E9"),
                     sig("oil", "F40D", a) + ", " + sig("egt", "F40E", b))));

    /* c and d at the plugin's addresses, a and b at others: four notes */
    const Answer full =
        put(plugin, "v",
            file("V", "S",
                 ecu(c, "7E8") + ", " + ecu(d, "7E9") + ", " + ecu(a, "7EA") +
                     ", " + ecu(b, "7EB"),
                 sig("s1", "F401", c) + ", " + sig("s2", "F402", d) + ", " +
                     sig("s3", "F403", a) + ", " + sig("s4", "F404", b)));
    ok(full);
    const auto q = [](const std::string& id) { return "'" + id + "'"; };
    const std::string notes[] = {
        "the file's ecu " + q(c) + " is the plugin's " + q(a),
        "the file's ecu " + q(d) + " is the plugin's " + q(b),
        "the file's ecu " + q(a) + " is " + q(std::string(29, 'a') + "_2") +
            " in the plugin, whose " + q(a) + " has other addresses",
        "the file's ecu " + q(b) + " is " + q(std::string(29, 'b') + "_2") +
            " in the plugin, whose " + q(b) + " has other addresses",
    };
    const std::string all =
        notes[0] + "; " + notes[1] + "; " + notes[2] + "; " + notes[3];
    ASSERT_GT(all.size(), 511u) << "the four notes overflow the detail";
    EXPECT_EQ(full.detail,
              notes[0] + "; " + notes[1] + "; " + notes[2] + "; and 1 more");
}

// A signal that names no ECU reads the file's first; in a plugin it
// would fall to the manifest's first, so a put names its ECU whenever
// that is another one
TEST(PluginWrapTest, ASignalNamingNoEcuKeepsTheEcuItRead)
{
    /* The file's first ECU isn't the manifest's first */
    const std::string one = ok(wrap(file_a())); /* pcm@7E8 */
    const std::string named = ok(
        put(one, "v",
            file("V", "S", kTcm,
                 sig("tft", "1E1C", "") + ", " + sig("gear", "1E1D", "tcm"))));
    Plugin p = load(named);
    ASSERT_TRUE(p);
    EXPECT_EQ(rx_of(p.get(), 1, "tft"), 0x7E9u);
    EXPECT_EQ(rx_of(p.get(), 1, "gear"), 0x7E9u);
    EXPECT_EQ(rx_of(p.get(), 0, "oil"), 0x7E8u);

    /* The file's first ECU is the manifest's first: nothing to write */
    const std::string plain =
        ok(put(one, "v", file("V", "S", kPcm, sig("egt", "F40E", ""))));
    const Value plain_doc = parsed(plain);
    const Value& s = plain_doc
                         .find("files")
                         ->find("data/v.json")
                         ->find("signals")
                         ->items.at(0);
    EXPECT_EQ(s.find("ecu"), nullptr);
    Plugin q = load(plain);
    ASSERT_TRUE(q);
    EXPECT_EQ(rx_of(q.get(), 1, "egt"), 0x7E8u);

    /* A file with no ECUs reads the set it extends; in a manifest whose
     * first ECU is another, its signals name that set's ECU, added as
     * "obd2" */
    const std::string tcm_first =
        ok(wrap(file("T", "S", kTcm, sig("tft", "1E1C", "tcm"))));
    const std::string bare =
        file("B", "S", "", sig("egt", "F40E", ""), R"("extends": "obd2")");
    const std::string joined = ok(put(tcm_first, "v", bare));
    EXPECT_EQ(ecus_of(joined), (Strings{"tcm@7E9", "obd2@7E8/7E0"}));
    Plugin j = load(joined);
    ASSERT_TRUE(j);
    EXPECT_EQ(rx_of(j.get(), 1, "egt"), 0x7E8u);
    EXPECT_EQ(rx_of(j.get(), 0, "tft"), 0x7E9u);

    /* The manifest has that ECU under an id of its own, not first */
    const std::string with_pcm = ok(wrap(file(
        "T", "S", std::string(kTcm) + ", " + kPcm, sig("tft", "1E1C", "tcm"))));
    const std::string reused = ok(put(with_pcm, "v", bare));
    EXPECT_EQ(ecus_of(reused), (Strings{"tcm@7E9", "pcm@7E8"}));
    Plugin u = load(reused);
    ASSERT_TRUE(u);
    EXPECT_EQ(rx_of(u.get(), 1, "egt"), 0x7E8u);

    /* The manifest has "obd2" for another ECU: the next free id */
    const std::string taken_id =
        ok(wrap(file("T", "S", R"({"id": "obd2", "rx": "7E9"})",
                     sig("tft", "1E1C", "obd2"))));
    EXPECT_EQ(ecus_of(ok(put(taken_id, "v", bare))),
              (Strings{"obd2@7E9", "obd2_2@7E8/7E0"}));

    /* A plugin with no ECUs whose signals read the set it extends, and
     * a file that brings the manifest its first ECU: the plugin's own
     * signals are named too */
    const std::string no_ecus = ok(wrap(bare));
    ASSERT_TRUE(ecus_of(no_ecus).empty());
    const std::string grown =
        ok(put(no_ecus, "v", file("V", "S", kTcm, sig("tft", "1E1C", ""))));
    EXPECT_EQ(ecus_of(grown), (Strings{"tcm@7E9", "obd2@7E8/7E0"}));
    Plugin g = load(grown);
    ASSERT_TRUE(g);
    EXPECT_EQ(rx_of(g.get(), 0, "egt"), 0x7E8u);
    EXPECT_EQ(rx_of(g.get(), 1, "tft"), 0x7E9u);

    /* Neither has an ECU: nothing is added */
    const std::string still_none = ok(put(no_ecus, "v", bare));
    EXPECT_TRUE(ecus_of(still_none).empty());
    EXPECT_EQ(parsed(still_none).find("ecus"), nullptr);

    /* A computed signal names no ECU and is given none */
    const std::string computed = ok(put(
        one, "v",
        file(
            "V", "S", kTcm,
            sig("tft", "1E1C", "") +
                R"(, {"key": "twice", "name": "Twice", "kind": "computed", "expr": "tft * 2"})")));
    const Value computed_doc = parsed(computed);
    const Value& c = computed_doc
                         .find("files")
                         ->find("data/v.json")
                         ->find("signals")
                         ->items.at(1);
    EXPECT_EQ(c.find("ecu"), nullptr);
}

// An ECU that only a replaced or dropped variant's files read leaves
// the manifest with them; one another file reads stays
TEST(PluginWrapTest, AnEcuOnlyARemovedVariantReadLeavesTheManifest)
{
    /* base reads pcm@7E8; its replacement brings pcm at another
     * address: the old one is gone first, so nothing is renamed */
    const std::string one = ok(wrap(file_a()));
    const Answer moved = put(one, "base",
                             file("V", "S", R"({"id": "pcm", "rx": "7E9"})",
                                  sig("egt", "F40E", "pcm")));
    EXPECT_EQ(ecus_of(ok(moved)), Strings{"pcm@7E9"});
    EXPECT_EQ(moved.detail, "");

    /* Two variants, each with an ECU of its own and one they share */
    const std::string shared = std::string(kPcm) + ", " + kTcm;
    const std::string two = ok(put(
        ok(wrap(file("A", "S", shared,
                     sig("oil", "F40D", "pcm") + ", " +
                         sig("tft", "1E1C", "tcm")))),
        "v",
        file("V", "S", std::string(kPcm) + R"(, {"id": "abs", "rx": "7EA"})",
             sig("egt", "F40E", "pcm") + ", " + sig("yaw", "2B00", "abs"))));
    EXPECT_EQ(ecus_of(two), (Strings{"pcm@7E8", "tcm@7E9", "abs@7EA"}));
    EXPECT_EQ(ecus_of(ok(drop(two, "v"))), (Strings{"pcm@7E8", "tcm@7E9"}));
    EXPECT_EQ(ecus_of(ok(drop(two, "base"))), (Strings{"pcm@7E8", "abs@7EA"}));

    /* A signal naming no ECU reads the first, which then stays */
    const std::string unnamed_reader =
        ok(put(ok(wrap(file("A", "S", kPcm, sig("oil", "F40D", "pcm")))), "v",
               file("V", "S", kPcm, sig("egt", "F40E", ""))));
    EXPECT_EQ(ecus_of(ok(drop(unnamed_reader, "base"))), Strings{"pcm@7E8"});
    Plugin kept = load(ok(drop(unnamed_reader, "base")));
    ASSERT_TRUE(kept);
    EXPECT_EQ(rx_of(kept.get(), 0, "egt"), 0x7E8u);

    /* An ECU nothing read before the call isn't the call's to remove */
    const std::string spare = packed(
        std::string(kPcm) + ", " + kTcm,
        R"({"id": "base", "files": ["a.json"]}, {"id": "v", "files": ["b.json"]})",
        "\"a.json\": " + data(sig("oil", "F40D", "pcm")) +
            ", \"b.json\": " + data(sig("egt", "F40E", "pcm")));
    EXPECT_EQ(ecus_of(ok(drop(spare, "v"))), (Strings{"pcm@7E8", "tcm@7E9"}));
}

// A drop takes the variant and its files out and raises the version;
// requires.core, the lineage and the rest stay as they were
TEST(PluginWrapTest, ADropTakesAVariantOutAndRaisesTheMinor)
{
    const std::string two = ok(put(ok(wrap(file_a())), "track", file_b()));
    const Answer d = drop(two, "base");
    const std::string one = ok(d);
    EXPECT_EQ(d.detail, "");
    Plugin p = load(one);
    ASSERT_TRUE(p);
    EXPECT_EQ(variants_of(p.get()), Strings{"track"});
    EXPECT_STREQ(info_of(p.get()).version, "1.2.0");
    EXPECT_EQ(files_of(one), Strings{"data/track.json"});
    Plugin before = load(two);
    EXPECT_EQ(lineage_of(p.get()), lineage_of(before.get()));
    EXPECT_EQ(ok(drop(two, "base")), one) << "the same bytes again";
    Plugin other = load(ok(drop(two, "track")));
    ASSERT_TRUE(other);
    EXPECT_EQ(variants_of(other.get()), Strings{"base"});

    /* requires.core is the plugin's own: a drop adds nothing to read */
    const std::string hand = packed(
        kPcm,
        R"({"id": "base", "files": ["a.json"]}, {"id": "v", "files": ["a.json", "b.json"]})",
        "\"a.json\": " + data(sig("oil", "F40D", "pcm")) +
            ", \"b.json\": " + data(sig("egt", "F40E", "pcm")),
        "3.0.9");
    const std::string less = ok(drop(hand, "v"));
    Plugin h = load(less);
    ASSERT_TRUE(h);
    EXPECT_EQ(requires_of(h.get()), "0.1.0");
    EXPECT_STREQ(info_of(h.get()).version, "3.1.0");
    EXPECT_EQ(files_of(less), Strings{"a.json"})
        << "the file base still lists stays";
    EXPECT_EQ(files_of(ok(drop(hand, "base"))), (Strings{"a.json", "b.json"}));
}

// Each refusal says why, and gives no text
TEST(PluginWrapTest, EveryRefusalSaysWhy)
{
    const std::string a = file_a();
    const std::string one = ok(wrap(a));

    /* The file, in the signal-file loader's words and the file's lines */
    EXPECT_EQ(refused(wrap("{\"havne_signals\": 1,\n \"name\": 7}")),
              "line 2, column 10: \"name\" is a number, not a string");
    EXPECT_FALSE(refused(wrap("")).empty());
    EXPECT_EQ(refused(put(one, "v", "{\"havne_signals\": 1,\n \"name\": 7}")),
              "line 2, column 10: \"name\" is a number, not a string");
    /* A packed plugin given as the file, and a file as the plugin */
    EXPECT_TRUE(has(refused(wrap(one)), "a signal file starts with "
                                        "\"havne_signals\""));
    EXPECT_EQ(refused(put(a, "v", a)),
              "the plugin, line 1, column 1: a plugin's manifest starts with "
              "\"havne_plugin\"");
    EXPECT_EQ(refused(drop(a, "v")),
              "line 1, column 1: a plugin's manifest starts with "
              "\"havne_plugin\"");

    /* The options */
    ecuio_plugin_wrap_options_t o = options();
    o.struct_size = sizeof o - 1;
    EXPECT_EQ(refused(wrap(a, &o)),
              "the options' struct_size is smaller than the options");
    o = options();
    o.struct_size = 0;
    EXPECT_FALSE(refused(wrap(a, &o)).empty());
    o = options();
    o.flags = 1;
    EXPECT_EQ(refused(wrap(a, &o)),
              "the options' flags and reserved words are zero");
    for (size_t i = 0; i < 6; ++i) {
        o = options();
        o.reserved[i] = 1;
        EXPECT_EQ(refused(wrap(a, &o)),
                  "the options' flags and reserved words are zero")
            << i;
    }
    for (const char* bad : {"", "-a", "a-", "A", "a_b", "a b",
                            "abcdefghijabcdefghijabcdefghijab"}) {
        o = options();
        o.id = bad;
        EXPECT_EQ(refused(wrap(a, &o)),
                  "the plugin's id '" + std::string(bad) +
                      "' takes 1 to 31 of a-z, 0-9 and -, a letter or digit "
                      "at each end")
            << bad;
    }
    for (const char* bad :
         {"", "a/../b", "Base", "abcdefghijabcdefghijabcde"}) {
        o = options();
        o.variant = bad;
        const std::string why =
            "the variant's id '" + std::string(bad) +
            "' takes 1 to 24 of a-z, 0-9 and -, a letter or digit at each end";
        EXPECT_EQ(refused(wrap(a, &o)), why) << bad;
        EXPECT_EQ(refused(put(one, bad, file_b())), why) << bad;
    }
    o = options();
    o.id = "abcdefghijabcdefghijabcdefghija"; /* 31: the most */
    o.variant = "abcdefghijabcdefghijabcd";   /* 24: the most */
    EXPECT_FALSE(ok(wrap(a, &o)).empty());

    /* The match: years only with codes, and the loader's rules for both */
    for (int which = 0; which < 2; ++which) {
        o = options();
        o.years[which] = 2020;
        EXPECT_EQ(refused(wrap(a, &o)),
                  "model years go with manufacturer codes: a match needs its "
                  "wmi")
            << which;
        o.wmi = "ZZZ";
        EXPECT_TRUE(
            has(refused(wrap(a, &o)), "takes a whole number from 1980 to 2039"))
            << which;
    }
    o = options();
    o.wmi = "ZZZ";
    o.years[0] = 2020;
    o.years[1] = 2010;
    EXPECT_EQ(refused(wrap(a, &o)),
              "a match's years runs from 2020 back to 2010");
    for (const char* bad : {"", "zzz", "ZZ", "ZZZ,", "ZZZ, ZZY", "ZZI"}) {
        o = options();
        o.wmi = bad;
        const std::string why = refused(wrap(a, &o));
        EXPECT_TRUE(has(why, "manufacturer code")) << bad << ": " << why;
        EXPECT_FALSE(has(why, "line ")) << "no place in a text never seen";
    }

    /* A drop: a variant the plugin lacks, and its only one */
    const std::string two = ok(put(one, "track", file_b()));
    EXPECT_EQ(refused(drop(two, "wet")), "the plugin has no variant 'wet'");
    EXPECT_EQ(refused(drop(one, "base")),
              "variant 'base' is the plugin's only one, and a plugin keeps "
              "one");
    EXPECT_EQ(refused(drop(one, "wet")), "the plugin has no variant 'wet'");

    /* A version with no minor left */
    const std::string top = packed(
        kPcm,
        R"({"id": "base", "files": ["a.json"]}, {"id": "v", "files": ["a.json"]})",
        "\"a.json\": " + data(sig("oil", "F40D", "pcm")), "1.65535.0");
    EXPECT_EQ(refused(put(top, "w", file_b())),
              "version 1.65535.0 has no minor left to raise: 65535 is the "
              "most");
    EXPECT_EQ(refused(drop(top, "v")),
              "version 1.65535.0 has no minor left to raise: 65535 is the "
              "most");
    const std::string near_top =
        packed(kPcm, R"({"id": "base", "files": ["a.json"]})",
               "\"a.json\": " + data(sig("oil", "F40D", "pcm")), "1.65534.3");
    Plugin last = load(ok(put(near_top, "w", file_b())));
    ASSERT_TRUE(last);
    EXPECT_STREQ(info_of(last.get()).version, "1.65535.0");
}

// A put the plugin can't hold is refused in the loader's words, with no
// place, and says the file can be a plugin of its own when it can
TEST(PluginWrapTest, APutThePluginCantHoldSuggestsAPluginOfItsOwn)
{
    const std::string advice = "; the file can be a plugin of its own";

    /* One ECU past what a session reads */
    std::string ecus;
    std::string signals;
    for (unsigned i = 1; i <= ECUIO_MAX_CHANNELS; ++i) {
        char ecu[80];
        std::snprintf(ecu, sizeof ecu,
                      "%s{\"id\": \"e%u\", \"rx\": \"%03X\", \"tx\": \"%03X\"}",
                      i > 1 ? ", " : "", i, 0x700u + i, 0x600u + i);
        ecus += ecu;
        char did[8];
        std::snprintf(did, sizeof did, "%04X", 0xF400u + i);
        signals += std::string(i > 1 ? ", " : "") +
                   sig("a" + std::to_string(i), did, "e" + std::to_string(i));
    }
    const std::string full = ok(wrap(file("Full", "S", ecus, signals)));
    const std::string past =
        refused(put(full, "v",
                    file("V", "S", R"({"id": "e33", "rx": "7EC"})",
                         sig("egt", "F40E", "e33"))));
    EXPECT_EQ(past, "ecu 'e33' is past the 32 ECUs one session reads, the "
                    "base's counted" +
                        advice);

    /* Two ID widths */
    const std::string wide =
        refused(put(ok(wrap(file_a())), "v",
                    file("V", "S", R"({"id": "wide", "rx": "18DAF110"})",
                         sig("egt", "F40E", "wide"))));
    EXPECT_TRUE(has(wide, "one session carries one width")) << wide;
    EXPECT_TRUE(has(wide, advice)) << wide;
    EXPECT_FALSE(has(wide, "line ")) << wide;

    /* One variant more than a plugin holds */
    std::string many = ok(wrap(file_a()));
    for (int i = 1; i < 32; ++i) {
        many = ok(put(many, ("v" + std::to_string(i)).c_str(), file_b()));
    }
    Plugin m = load(many);
    ASSERT_TRUE(m);
    EXPECT_EQ(info_of(m.get()).variant_count, 32u);
    EXPECT_EQ(refused(put(many, "v32", file_b())),
              "\"variants\" lists more than 32" + advice);
    /* Replacing one of the 32 still fits */
    EXPECT_FALSE(ok(put(many, "v31", file_a())).empty());

    /* A file that a wrap refuses too gets no such advice: written out,
     * its signals are past a signal file's 1 MiB */
    std::string padding = "[";
    for (int i = 0; i < 150000; ++i) {
        padding += i == 0 ? "1" : ",1";
    }
    padding += "]";
    const std::string heavy = file(
        "Heavy", "S", kPcm,
        R"({"key": "oil", "name": "Oil", "ecu": "pcm", "x-pad": )" + padding +
            R"(, "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}})");
    ASSERT_LT(heavy.size(), size_t{ECUIO_SIGNAL_FILE_MAX_BYTES});
    const std::string alone = refused(wrap(heavy));
    EXPECT_TRUE(has(alone, "variant 'base''s files don't make one signal file"))
        << alone;
    const std::string into = refused(put(ok(wrap(file_a())), "v", heavy));
    EXPECT_TRUE(has(into, "variant 'v''s files don't make one signal file"))
        << into;
    EXPECT_FALSE(has(into, advice)) << into;
}

// NULLs are refused before anything is read, the inputs' bounds are the
// loaders', and a refusal leaves no text
TEST(PluginWrapTest, TheCallsRefuseNullsAndInputsPastTheirBounds)
{
    const std::string a = file_a();
    const std::string one = ok(wrap(a));
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;

    EXPECT_EQ(ecuio_plugin_wrap(a.data(), a.size(), nullptr, nullptr, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(length, 0u);
    length = 77;
    EXPECT_EQ(ecuio_plugin_wrap(nullptr, 5, nullptr, &out, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(length, 0u);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    /* No bytes at all is a file refused, not a NULL */
    out = reinterpret_cast<char*>(1);
    EXPECT_EQ(ecuio_plugin_wrap(nullptr, 0, nullptr, &out, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);

    out = reinterpret_cast<char*>(1);
    EXPECT_EQ(ecuio_plugin_put_variant(nullptr, 5, "v", a.data(), a.size(),
                                       &out, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    out = reinterpret_cast<char*>(1);
    EXPECT_EQ(ecuio_plugin_put_variant(one.data(), one.size(), "v", nullptr, 5,
                                       &out, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(ecuio_plugin_put_variant(one.data(), one.size(), "v", a.data(),
                                       a.size(), nullptr, &length),
              ECUIO_ERR_NULL_ARGUMENT);

    out = reinterpret_cast<char*>(1);
    EXPECT_EQ(ecuio_plugin_drop_variant(nullptr, 5, "v", &out, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    out = reinterpret_cast<char*>(1);
    EXPECT_EQ(ecuio_plugin_drop_variant(one.data(), one.size(), nullptr, &out,
                                        &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(ecuio_plugin_drop_variant(one.data(), one.size(), "base", nullptr,
                                        &length),
              ECUIO_ERR_NULL_ARGUMENT);

    /* A length is optional; the text ends in a NUL either way */
    out = nullptr;
    ASSERT_EQ(ecuio_plugin_wrap(a.data(), a.size(), nullptr, &out, nullptr),
              ECUIO_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(std::string(out), one);
    ecuio_plugin_text_free(out);
    ecuio_plugin_text_free(nullptr);

    /* A signal file one byte past its bound, padded with spaces */
    std::string big = a;
    big.append(ECUIO_SIGNAL_FILE_MAX_BYTES + 1 - big.size(), ' ');
    EXPECT_FALSE(refused(wrap(big)).empty());
    EXPECT_FALSE(refused(put(one, "v", big)).empty());
    big.pop_back();
    EXPECT_EQ(ok(wrap(big)).size(), one.size()) << "at the bound it is taken";

    /* A packed plugin one byte past its bound */
    std::string huge = one;
    huge.append(ECUIO_PLUGIN_PACKAGE_MAX + 1 - huge.size(), ' ');
    EXPECT_TRUE(has(refused(put(huge, "v", a)), "the plugin, "));
    EXPECT_FALSE(refused(drop(huge, "base")).empty());
}

namespace {

ecuio_signal_source_t variant_source(const ecuio_plugin_t* p, uint32_t index)
{
    ecuio_signal_source_t s{};
    s.struct_size = sizeof s;
    EXPECT_EQ(ecuio_signal_file_source(ecuio_plugin_variant_file(p, index), &s),
              ECUIO_OK);
    return s;
}

} // namespace

// A variant's source is its written file's: the first of its files that
// has one, else the manifest's; a share-alike file put into a plugin
// marks its own variant alone
TEST(PluginWrapTest, EachVariantGivesItsOwnSourceElseTheManifests)
{
    const std::string src1 =
        R"("source": {"name": "First", "license": "CC-BY-SA-4.0"})";
    const std::string src2 = R"("source": {"name": "Second", "url": "u"})";
    auto source_for = [&](const std::string& head1, const std::string& head2) {
        Plugin h = load(packed(
            kPcm, R"({"id": "base", "files": ["a.json", "b.json"]})",
            "\"a.json\": " + data(sig("oil", "F40D", "pcm"), head1) +
                ", \"b.json\": " + data(sig("egt", "F40E", "pcm"), head2)));
        return h ? variant_source(h.get(), 0) : ecuio_signal_source_t{};
    };
    EXPECT_STREQ(source_for("", "").name, "M");
    EXPECT_STREQ(source_for(src1, "").name, "First");
    EXPECT_EQ(source_for(src1, "").flags, ECUIO_SOURCE_SHARE_ALIKE);
    EXPECT_STREQ(source_for("", src2).name, "Second");
    EXPECT_STREQ(source_for("", src2).url, "u");
    EXPECT_EQ(source_for("", src2).flags, 0u);
    EXPECT_STREQ(source_for(src1, src2).name, "First");
    EXPECT_STREQ(source_for(src1, src2).url, "") << "one block, never mixed";

    const std::string obdb =
        file("Converted", "OBDb", kPcm, sig("egt", "F40E", "pcm"), "");
    const std::string licensed =
        obdb.substr(0, obdb.find("\"name\": \"OBDb\"")) +
        R"("name": "OBDb", "license": "CC BY-SA 4.0")" +
        obdb.substr(obdb.find("\"name\": \"OBDb\"") + 14);
    const std::string two = ok(put(ok(wrap(file_a())), "obdb", licensed));
    Plugin p = load(two);
    ASSERT_TRUE(p);
    EXPECT_STREQ(variant_source(p.get(), 0).name, "By hand");
    EXPECT_EQ(variant_source(p.get(), 0).flags, 0u);
    EXPECT_STREQ(variant_source(p.get(), 1).name, "OBDb");
    EXPECT_STREQ(variant_source(p.get(), 1).license, "CC BY-SA 4.0");
    EXPECT_EQ(variant_source(p.get(), 1).flags, ECUIO_SOURCE_SHARE_ALIKE);

    Plugin w = load(ok(wrap(licensed)));
    ASSERT_TRUE(w);
    EXPECT_EQ(variant_source(w.get(), 0).flags, ECUIO_SOURCE_SHARE_ALIKE)
        << "a wrapped file's source, moved to the manifest, still its own";
}

// A variant's text is its written file, which reads back as its set and
// goes into another plugin as the same variant (Copy to my plugin)
TEST(PluginWrapTest, AVariantsTextReadsBackAsItsSetAndMovesAcross)
{
    Plugin p = load(ok(put(ok(wrap(file_a())), "track", file_b())));
    ASSERT_TRUE(p);
    for (uint32_t v = 0; v < 2; ++v) {
        const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p.get(), v);
        size_t length = 0;
        const char* text = ecuio_signal_file_text(f, &length);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(std::string(text, length), written(p.get(), v));
        ecuio_signal_file_t* back = nullptr;
        ASSERT_EQ(ecuio_signal_file_load(text, length, &back), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_TRUE(same_sets(*ecuio_signal_file_set(back),
                              *ecuio_plugin_variant_set(p.get(), v)));
        ecuio_signal_file_free(back);
    }

    size_t length = 0;
    const char* track =
        ecuio_signal_file_text(ecuio_plugin_variant_file(p.get(), 1), &length);
    Plugin mine = load(ok(put(ok(wrap(file_a(), nullptr)), "copied",
                              std::string(track, length))));
    ASSERT_TRUE(mine);
    EXPECT_TRUE(same_sets(*ecuio_plugin_variant_set(mine.get(), 1),
                          *ecuio_plugin_variant_set(p.get(), 1)));
    EXPECT_STREQ(variant_source(mine.get(), 1).name, "From a forum")
        << "the copy keeps the source";
}
