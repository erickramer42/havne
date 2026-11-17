// File/layer: tests/test_plugin_least_core.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginWrapTest (test_signals)
//
// PluginWrapTest's cases for a plugin's requires.core (ADR-0064 Q1): the
// least core version the loaders work out for each row of least_core.h,
// and what a wrap, a put and a drop write from it under another core.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "least_core.h" /* core/src: the loaders' least core */
#include "plugin_wrap_support.h"

using namespace plugin_wrap_test;

namespace {

/* Plays core version major.minor.patch for the scope
 * (ecuio_plugin_test_set_core_version), so a case meets a later core */
struct CoreAs {
    CoreAs(uint32_t major, uint32_t minor, uint32_t patch)
    {
        ecuio_plugin_test_set_core_version(1, major, minor, patch);
    }
    ~CoreAs() { ecuio_plugin_test_set_core_version(0, 0, 0, 0); }
    CoreAs(const CoreAs&) = delete;
    CoreAs& operator=(const CoreAs&) = delete;
};

/* The version the plugin rules compare requires.core with */
std::string rules_version()
{
    ecuio_least::version v = {{0, 0, 0}};
    ecuio_plugin_rules::core_version(v.v);
    return ecuio_least::text(v);
}

/* The least core the plugin loader works out for a package it takes */
std::string least_of_package(const std::string& package)
{
    Plugin p = load(package);
    return p ? ecuio_least::text(ecuio_least::of_plugin(p.get()))
             : std::string("refused");
}

/* The least core the signal-file loader works out for a file it takes */
std::string least_of_file(const std::string& bytes)
{
    ecuio_signal_file_t* f = nullptr;
    if (ecuio_signal_file_load(bytes.data(), bytes.size(), &f) != ECUIO_OK) {
        return std::string("refused: ") + ecuio_last_error_detail();
    }
    const std::string v = ecuio_least::text(f->least);
    ecuio_signal_file_free(f);
    return v;
}

/* n signals on pcm, each its own DID from F000 on: n requests */
std::string signals_asking(unsigned n)
{
    std::string out;
    char did[8];
    for (unsigned i = 0; i < n; ++i) {
        std::snprintf(did, sizeof did, "%04X", 0xF000 + i);
        out += (i == 0 ? "" : ", ") + sig("s" + std::to_string(i), did, "pcm");
    }
    return out;
}

/* n ECUs, pcm at 7E8 first, then e1 at 7E9 and on */
std::string ecus(unsigned n)
{
    std::string out = kPcm;
    char rx[8];
    for (unsigned i = 1; i < n; ++i) {
        std::snprintf(rx, sizeof rx, "%X", 0x7E8 + i);
        out += ", {\"id\": \"e" + std::to_string(i) + "\", \"rx\": \"" + rx +
               "\"}";
    }
    return out;
}

/* e1 to e4 at 7E9 to 7EC and none at 7E8: with obd2's engine ECU, five */
const char kFourPastTheBase[] =
    R"({"id": "e1", "rx": "7E9"}, {"id": "e2", "rx": "7EA"}, )"
    R"({"id": "e3", "rx": "7EB"}, {"id": "e4", "rx": "7EC"})";

const char kGauged[] =
    R"({"key": "oil", "name": "Oil", "ecu": "pcm", )"
    R"("read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}, )"
    R"("gauge": {"min": 0, "max": 100}})";

/* A package written by hand around one data file, a.json */
std::string one_file(const std::string& ecu_list, const std::string& data_file,
                     const std::string& more = "")
{
    return packed(ecu_list, R"({"id": "base", "files": ["a.json"]})",
                  "\"a.json\": " + data_file, "1.0.0", more);
}

/* text with its requires.core, 0.1.0 as packed() writes it, made core */
std::string requiring(std::string text, const std::string& core)
{
    const std::string from = "\"requires\": {\"core\": \"0.1.0\"}";
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos) {
        text.replace(at, from.size(),
                     "\"requires\": {\"core\": \"" + core + "\"}");
    }
    return text;
}

} // namespace

// Each row of least_core.h, met by the loader that reads it: a package
// with nothing past the first plugins needs 0.5.0, and each feature
// alone raises it to its own row, a bound's at the bound and not below
TEST(PluginWrapTest, EachRowsInputNeedsItsCore)
{
    const std::string oil = sig("oil", "F40D", "pcm");
    const std::string speed =
        R"({"key": "spd", "name": "Vehicle Speed", "ecu": "pcm", )"
        R"("read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}})";
    const std::string extends = R"("extends": "obd2")";
    struct row {
        const char* what;
        std::string package;
        const char* least;
    };
    const row rows[] = {
        {"a package", one_file(kPcm, data(oil)), "0.5.0"},
        {"a gauge", one_file(kPcm, data(kGauged)), "0.7.2"},
        {"a file extending obd2 that replaces nothing",
         one_file(kPcm, data(oil, extends)), "0.5.0"},
        {"a base def replaced by name", one_file(kPcm, data(speed, extends)),
         "0.7.3"},
        {"256 requests", one_file(kPcm, data(signals_asking(256))), "0.5.0"},
        {"257 requests", one_file(kPcm, data(signals_asking(257))), "0.8.0"},
        {"format 2",
         one_file(kPcm, "{\"havne_signals\": 2, \"signals\": [" + oil + "]}"),
         "0.8.0"},
        {"a lineage",
         one_file(kPcm, data(oil),
                  "\"lineage\": \"" + std::string(32, 'a') + "\", "),
         "0.8.0"},
        {"a data file's own source",
         one_file(kPcm, data(oil, R"("source": {"name": "S"})")), "0.8.0"},
        {"a data file's source the manifest's",
         one_file(kPcm, data(oil, R"("source": {"name": "M"})")), "0.5.0"},
        {"4 ECUs", one_file(ecus(4), data(oil)), "0.5.0"},
        {"5 ECUs", one_file(ecus(5), data(oil)), "0.9.0"},
        {"4 ECUs and the base's",
         one_file(kFourPastTheBase, data(sig("oil", "F40D", "e1"), extends)),
         "0.9.0"},
    };
    for (const row& r : rows) {
        EXPECT_EQ(least_of_package(r.package), r.least) << r.what;
    }

    /* A signal file alone, through the same build */
    EXPECT_EQ(least_of_file(file("F", "S", kPcm, oil)), "0.5.0");
    EXPECT_EQ(least_of_file(file("F", "S", kPcm, kGauged)), "0.7.2");
    EXPECT_EQ(least_of_file(file("F", "S", ecus(5), oil)), "0.9.0");
    EXPECT_EQ(least_of_file(file("F", "S", kFourPastTheBase,
                                 sig("oil", "F40D", "e1"), extends)),
              "0.9.0");
}

// A wrap names the least core that reads it, never the core it ran
// under: the same file wraps to the same bytes under a later core, and
// a row of a release to come is cut to this core's version, so a build
// before that release still takes what it wrote
TEST(PluginWrapTest, AWrapRequiresTheLeastCoreThatReadsIt)
{
    const std::string six = file("Six", "S", ecus(6), sig("oil", "F40D", "e5"));
    const std::string own = core_version();
    const std::string here = ok(wrap(file_a()));
    {
        CoreAs before(0, 8, 0);
        Plugin cut = load(ok(wrap(six)));
        ASSERT_TRUE(cut);
        EXPECT_EQ(requires_of(cut.get()), "0.8.0")
            << "a build before 0.9.0 writes its own version";
    }
    /* Last, a version no core is, so a seam left on shows after it */
    {
        CoreAs later(9, 9, 9);
        EXPECT_EQ(rules_version(), "9.9.9");
        EXPECT_EQ(core_version(), own) << "ecuio_version isn't the seam's";
        const std::string there = ok(wrap(file_a()));
        EXPECT_EQ(there, here) << "the same bytes under a later core";
        Plugin p = load(there);
        ASSERT_TRUE(p);
        EXPECT_EQ(requires_of(p.get()), "0.8.0");

        Plugin whole = load(ok(wrap(six)));
        ASSERT_TRUE(whole);
        EXPECT_EQ(requires_of(whole.get()), "0.9.0") << "past 4 ECUs";
    }
    EXPECT_EQ(rules_version(), own) << "the seam gave the core back";
}

// A put keeps a requires.core above what its contents need (the
// author's word) and raises one below it; a drop leaves it, above or
// below what the rest needs
TEST(PluginWrapTest, APutKeepsAHigherRequiresAndRaisesALowerOne)
{
    CoreAs later(0, 9, 3);
    const std::string oil = data(sig("oil", "F40D", "pcm"));
    const auto put_into = [&](const std::string& core,
                              const std::string& ecu_list,
                              const std::string& in) {
        Plugin p = load(
            ok(put(requiring(one_file(ecu_list, oil), core), "track", in)));
        return p ? requires_of(p.get()) : std::string("refused");
    };
    EXPECT_EQ(put_into("0.1.0", kPcm, file_b()), "0.8.0")
        << "raised: the file put in keeps a source of its own";
    EXPECT_EQ(put_into("0.1.0", kPcm,
                       file("Plain", "M", kPcm, sig("egt", "F40E", "pcm"))),
              "0.5.0")
        << "a source the manifest's asks nothing more";
    EXPECT_EQ(put_into("0.1.0", kPcm, file("Gauged", "M", kPcm, kGauged)),
              "0.7.2")
        << "raised to the gauge's";
    EXPECT_EQ(put_into("0.9.1", kPcm, file_b()), "0.9.1") << "kept";
    EXPECT_EQ(put_into("0.8.0", kPcm,
                       file("Five", "S", ecus(5), sig("egt", "F40E", "e4"))),
              "0.9.0")
        << "raised past 4 ECUs";

    const std::string two_variants =
        packed(kPcm,
               R"({"id": "base", "files": ["a.json"]}, )"
               R"({"id": "track", "files": ["b.json"]})",
               "\"a.json\": " + oil +
                   ", \"b.json\": " + data(sig("egt", "F40E", "pcm")),
               "1.0.0", "\"lineage\": \"" + std::string(32, 'b') + "\", ");
    for (const char* stored : {"0.9.2", "0.1.0"}) {
        Plugin dropped =
            load(ok(drop(requiring(two_variants, stored), "track")));
        ASSERT_TRUE(dropped);
        EXPECT_EQ(requires_of(dropped.get()), stored)
            << "a drop leaves it, the lineage's 0.8.0 notwithstanding";
    }
}
