// File/layer: tests/test_plugin_format3.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest, PluginWrapTest (test_signals)
//
// A plugin against signal-file format 3 (ADR-0065 section 9): a manifest's
// ECU names, which leave a variant's format alone, a variant that holds a
// format 3 file, and the names a wrap and a put carry.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include <cstring>
#include <string>

#include "plugin_wrap_support.h"

using namespace plugin_wrap_test;

namespace {

const char kNamedPcm[] = R"({"id": "pcm", "rx": "7E8", "name": "Engine"})";
const char kOneFile[] = R"({"id": "base", "files": ["a.json"]})";
const char kTwoFiles[] = R"({"id": "base", "files": ["a.json", "b.json"]})";

/* A data file of the given format */
std::string data_of(unsigned format, const std::string& signals,
                    const std::string& head = "")
{
    return "{\"havne_signals\": " + std::to_string(format) + ", " + head +
           (head.empty() ? "" : ", ") + "\"signals\": [" + signals + "]}";
}

/* A signal read by DID on pcm under a name of its own */
std::string named_sig(const std::string& key, const std::string& name,
                      const std::string& did)
{
    return "{\"key\": \"" + key + "\", \"name\": \"" + name +
           "\", \"ecu\": \"pcm\", \"read\": {\"service\": \"22\", "
           "\"number\": \"" +
           did + "\"}, \"field\": {\"byte\": 0}}";
}

std::string refusal_of(const std::string& package)
{
    ecuio_plugin_t* p = nullptr;
    EXPECT_EQ(
        ecuio_plugin_load(package.data(), package.size(), nullptr, nullptr, &p),
        ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(p, nullptr);
    return ecuio_last_error_detail();
}

std::string module_name(const ecuio_signal_file_t* f, uint32_t index)
{
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    EXPECT_EQ(ecuio_signal_file_module(f, index, &m), ECUIO_OK)
        << ecuio_last_error_detail();
    return m.name;
}

/* A written manifest's "name" for the ECU with that id, "" with none */
std::string manifest_name(const std::string& packed_text, const std::string& id)
{
    const Value doc = parsed(packed_text);
    for (const Value& e : doc.find("ecus")->items) {
        if (e.find("id")->text == id) {
            const Value* name = e.find("name");
            return name != nullptr ? name->text : std::string();
        }
    }
    return "no such ecu";
}

} // namespace

// A manifest of either format names its ECUs, and the name changes
// nothing else: the variant keeps its files' format, and its copy reads
// back with the name
TEST(PluginTest, AManifestOfAnyFormatNamesAnEcu)
{
    const std::string one = packed(
        kNamedPcm, kOneFile, "\"a.json\": " + data(sig("oil", "F40D", "pcm")));
    std::string two = one;
    two.replace(two.find("\"havne_plugin\": 1"), 17, "\"havne_plugin\": 2");
    for (const std::string& package : {one, two}) {
        Plugin p = load(package);
        ASSERT_NE(p, nullptr);
        const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p.get(), 0);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(module_name(f, 0), "Engine");
        EXPECT_EQ(ecuio_signal_file_format(f), 1u) << "a name raises no format";

        const std::string copy = written(p.get(), 0);
        EXPECT_TRUE(has(copy, "\"havne_signals\": 1")) << copy.substr(0, 80);
        ecuio_signal_file_t* back = nullptr;
        ASSERT_EQ(ecuio_signal_file_load_with(copy.data(), copy.size(),
                                              ECUIO_SIGNAL_FILE_LOAD_SET,
                                              &back),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(module_name(back, 0), "Engine");
        EXPECT_TRUE(
            same_sets(*ecuio_signal_file_set(back), *ecuio_signal_file_set(f)));
        ecuio_signal_file_free(back);
    }
}

// A manifest's ECU name never moves a format 2 file's signals: its
// replacement of a base def by name stands, in the set and in the copy
TEST(PluginTest, AnEcusNameLeavesAVariantsReplacementByNameAlone)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    Plugin p = load(packed(
        kNamedPcm, kOneFile,
        "\"a.json\": " +
            data_of(2, named_sig("map_kpa", "Intake Manifold Pressure", "D100"),
                    R"("extends": "obd2")")));
    ASSERT_NE(p, nullptr);
    const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p.get(), 0);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(ecuio_signal_file_format(f), 2u);
    EXPECT_EQ(ecuio_signal_file_set(f)->count, obd2->count)
        << "the variant's signal stands in the base def's place";
    const ecuio_signal_def_t* map = ecuio_signal_file_find(f, "map_kpa");
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->id, ECUIO_OBD2_PID_SIG_ID(0x0B, 0));
    EXPECT_EQ(module_name(f, 0), "Engine");
    EXPECT_TRUE(has(written(p.get(), 0), "\"havne_signals\": 2"));
}

// A variant is read at its newest file's format whole, so one format 3
// file makes it a format 3 set: names repeat across its files, and a
// format 1 or 2 file's signal named as a base def, which would replace
// it were the file read alone, is refused, not moved beside it unsaid
TEST(PluginTest, AFormat3FileMakesItsVariantAFormat3Set)
{
    const std::string a = named_sig("volts_a", "Battery voltage", "D100");
    const std::string b = named_sig("volts_b", "Battery voltage", "D101");
    Plugin p = load(packed(kPcm, kTwoFiles,
                           "\"a.json\": " + data_of(2, a) +
                               ", \"b.json\": " + data_of(3, b)));
    ASSERT_NE(p, nullptr);
    const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p.get(), 0);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(ecuio_signal_file_format(f), 3u);
    EXPECT_NE(ecuio_signal_file_find(f, "volts_a"), nullptr);
    EXPECT_NE(ecuio_signal_file_find(f, "volts_b"), nullptr);
    EXPECT_TRUE(has(written(p.get(), 0), "\"havne_signals\": 3"));

    /* Two files of format 2 still name each signal once */
    EXPECT_TRUE(has(refusal_of(packed(kPcm, kTwoFiles,
                                      "\"a.json\": " + data_of(2, a) +
                                          ", \"b.json\": " + data_of(2, b))),
                    "a name names one signal"))
        << ecuio_last_error_detail();

    const std::string map =
        named_sig("map_kpa", "Intake Manifold Pressure", "D100");
    const std::string extends = R"("extends": "obd2")";
    for (unsigned format : {1u, 2u}) {
        const std::string words =
            refusal_of(packed(kPcm, kTwoFiles,
                              "\"a.json\": " + data_of(format, map, extends) +
                                  ", \"b.json\": " + data_of(3, b)));
        EXPECT_TRUE(has(
            words,
            "signal 'map_kpa' is named as obd2's 'Intake Manifold Pressure', "
            "which a format " +
                std::to_string(format) +
                " file replaces and a format 3 set reads beside it: this "
                "variant holds a format 3 file, so give every file of it "
                "\"havne_signals\": 3"))
            << words;
    }

    /* Every file format 3: the signal is read beside the base def */
    Plugin beside = load(packed(kPcm, kTwoFiles,
                                "\"a.json\": " + data_of(3, map, extends) +
                                    ", \"b.json\": " + data_of(3, b)));
    ASSERT_NE(beside, nullptr);
    const ecuio_signal_file_t* g = ecuio_plugin_variant_file(beside.get(), 0);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(ecuio_signal_file_set(g)->count,
              ecuio_obd2_signal_set()->count + 2);
    EXPECT_EQ(ecuio_signal_file_find(g, "map_kpa")->id, ECUIO_USER_SIG_ID_BASE);
}

// A wrap moves a file's ECUs to the manifest with their names, and a put
// gives a module the file's name when the manifest's entry has none
TEST(PluginWrapTest, AWrapAndAPutCarryAnEcusName)
{
    const std::string named_file =
        file("Named", "By hand", kNamedPcm, sig("oil", "F40D", "pcm"));
    const std::string wrapped = ok(wrap(named_file));
    EXPECT_EQ(manifest_name(wrapped, "pcm"), "Engine");
    Plugin p = load(wrapped);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(module_name(ecuio_plugin_variant_file(p.get(), 0), 0), "Engine");

    /* The manifest's entry lacks a name: the file's is kept, whatever
     * the file calls the module */
    const std::string bare = ok(wrap(file_a()));
    EXPECT_EQ(manifest_name(bare, "pcm"), "");
    const std::string other_id =
        file("Track day", "From a forum",
             R"({"id": "engine", "rx": "7E8", "name": "Engine"})",
             sig("egt", "F40E", "engine"));
    const std::string filled = ok(put(bare, "track", other_id));
    EXPECT_EQ(manifest_name(filled, "pcm"), "Engine");
    EXPECT_EQ(ecus_of(filled), (Strings{"pcm@7E8"}));

    /* The manifest's own name stands */
    const std::string kept =
        ok(put(wrapped, "track",
               file("Track day", "From a forum",
                    R"({"id": "pcm", "rx": "7E8", "name": "Powertrain"})",
                    sig("egt", "F40E", "pcm"))));
    EXPECT_EQ(manifest_name(kept, "pcm"), "Engine");

    /* A new module comes with its name */
    const std::string added =
        ok(put(bare, "track",
               file("Track day", "From a forum",
                    R"({"id": "tcm", "rx": "7E9", "name": "Transmission"})",
                    sig("tft", "F40E", "tcm"))));
    EXPECT_EQ(manifest_name(added, "tcm"), "Transmission");
}

namespace {

std::vector<std::string> needs_of(const ecuio_signal_file_t* f)
{
    uint32_t count = 0;
    EXPECT_EQ(ecuio_signal_file_needs(f, nullptr, 0, &count), ECUIO_OK);
    std::vector<ecuio_signal_key_t> keys(count);
    EXPECT_EQ(ecuio_signal_file_needs(f, keys.data(), count, &count), ECUIO_OK);
    std::vector<std::string> out;
    for (const ecuio_signal_key_t& k : keys) {
        out.emplace_back(k.text);
    }
    return out;
}

const char kMargin[] =
    R"({"key": "margin", "name": "Oil margin", "kind": "computed", )"
    R"("expr": "130 - oil_t"})";

} // namespace

// A variant is one set: what one of its files needs and another gives is
// met there, and only what none of them gives is the variant's need
TEST(PluginTest, AVariantsFilesMeetEachOthersNeeds)
{
    const std::string needing = data_of(3, kMargin, R"("needs": ["oil_t"])");
    const std::string giving =
        data_of(3, named_sig("oil_t", "Oil temp", "D100"));
    const std::string package = packed(
        kPcm,
        R"({"id": "both", "files": ["a.json", "b.json"]}, )"
        R"({"id": "alone", "files": ["a.json", "c.json"]})",
        "\"a.json\": " + needing + ", \"b.json\": " + giving +
            ", \"c.json\": " +
            data_of(3,
                    R"({"key": "twice", "name": "Twice", "kind": "computed", )"
                    R"("expr": "oil_t * 2"})",
                    R"("needs": ["oil_t"])"));
    Plugin p = load(package);
    ASSERT_NE(p, nullptr);

    const ecuio_signal_file_t* both = ecuio_plugin_variant_file(p.get(), 0);
    ASSERT_NE(both, nullptr);
    EXPECT_NE(ecuio_signal_file_find(both, "margin"), nullptr)
        << "its need is met by the variant's other file";
    EXPECT_EQ(ecuio_signal_file_note_count(both), 0u);
    EXPECT_TRUE(needs_of(both).empty());
    EXPECT_FALSE(has(written(p.get(), 0), "\"needs\""));

    /* Without the file that gives it, both readers wait, and the two
     * files' one need is the variant's, once */
    const ecuio_signal_file_t* alone = ecuio_plugin_variant_file(p.get(), 1);
    ASSERT_NE(alone, nullptr);
    EXPECT_EQ(ecuio_signal_file_find(alone, "margin"), nullptr);
    EXPECT_EQ(ecuio_signal_file_find(alone, "twice"), nullptr);
    EXPECT_EQ(ecuio_signal_file_set(alone)->count, 0u);
    EXPECT_EQ(ecuio_signal_file_note_count(alone), 2u);
    EXPECT_EQ(needs_of(alone), (std::vector<std::string>{"oil_t"}));

    /* Its copy holds the need and the waiting signals, and reads back so */
    const std::string copy = written(p.get(), 1);
    const Value doc = parsed(copy);
    ASSERT_NE(doc.find("needs"), nullptr) << copy.substr(0, 300);
    ASSERT_EQ(doc.find("needs")->items.size(), 1u);
    EXPECT_EQ(doc.find("needs")->items[0].text, "oil_t");
    EXPECT_EQ(doc.find("signals")->items.size(), 2u);
    ecuio_signal_file_t* back = nullptr;
    ASSERT_EQ(ecuio_signal_file_load_with(copy.data(), copy.size(),
                                          ECUIO_SIGNAL_FILE_LOAD_SET, &back),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_note_count(back), 2u);
    EXPECT_EQ(needs_of(back), (std::vector<std::string>{"oil_t"}));
    ecuio_signal_file_free(back);

    /* A data file takes "needs" only in format 3 */
    EXPECT_TRUE(
        has(refusal_of(packed(
                kPcm, kOneFile,
                "\"a.json\": " + data_of(2, kMargin, R"("needs": ["oil_t"])"))),
            "\"needs\" is signal-file format 3's, and this file is format 2"))
        << ecuio_last_error_detail();
}

// A wrap keeps a file's needs in its data file, and a put carries them
TEST(PluginWrapTest, AWrapAndAPutCarryNeeds)
{
    std::string needing = file("Mine", "By hand", kPcm,
                               sig("oil", "F40D", "pcm") + ", " + kMargin,
                               R"("needs": ["oil_t"])");
    needing.replace(needing.find("\"havne_signals\": 1"), 18,
                    "\"havne_signals\": 3");
    const std::string wrapped = ok(wrap(needing));
    Plugin p = load(wrapped);
    ASSERT_NE(p, nullptr);
    const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p.get(), 0);
    EXPECT_EQ(needs_of(f), (std::vector<std::string>{"oil_t"}));
    EXPECT_EQ(ecuio_signal_file_find(f, "margin"), nullptr);
    EXPECT_NE(ecuio_signal_file_find(f, "oil"), nullptr);

    const std::string added = ok(put(ok(wrap(file_a())), "mine", needing));
    Plugin q = load(added);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(variants_of(q.get()), (Strings{"base", "mine"}));
    EXPECT_EQ(needs_of(ecuio_plugin_variant_file(q.get(), 1)),
              (std::vector<std::string>{"oil_t"}));
}
