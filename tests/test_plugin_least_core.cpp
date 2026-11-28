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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "least_core.h" /* core/src: the loaders' least core */
#include "request_engine.h" /* core/src: a set's requests counted */
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

/* package as manifest format n, nothing else changed */
std::string of_format(std::string package, char n)
{
    const std::string one = "\"havne_plugin\": 1";
    const size_t at = package.find(one);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos) {
        package[at + one.size() - 1] = n;
    }
    return package;
}

/* A package whose one variant reads a.json, then b.json */
std::string two_files(const std::string& ecu_list, const std::string& a,
                      const std::string& b)
{
    return packed(ecu_list, R"({"id": "base", "files": ["a.json", "b.json"]})",
                  "\"a.json\": " + a + ", \"b.json\": " + b);
}

/* n signals on ecu named <prefix><i>, two to a DID from first on (its
 * bytes 0 and 1): half as many requests */
std::string signals_sharing(unsigned n, const std::string& prefix,
                            const std::string& ecu, unsigned first)
{
    std::string out;
    char did[8];
    for (unsigned i = 0; i < n; ++i) {
        std::snprintf(did, sizeof did, "%04X", first + i / 2);
        const std::string key = prefix + std::to_string(i);
        out += (i == 0 ? "" : ", ") + std::string("{\"key\": \"") + key +
               "\", \"name\": \"N " + key + "\", \"ecu\": \"" + ecu +
               "\", \"read\": {\"service\": \"22\", \"number\": \"" + did +
               "\"}, \"field\": {\"byte\": " + std::to_string(i % 2) + "}}";
    }
    return out;
}

/* An enum signal on pcm, DID did, with an enum's most labels and
 * `chars` bytes of label among them: from one each to the longest each */
std::string enum_of(const std::string& key, unsigned did, unsigned chars)
{
    std::string values;
    for (unsigned raw = 0; raw < ECUIO_ENUM_MAX_ENTRIES; ++raw) {
        const unsigned length = chars / ECUIO_ENUM_MAX_ENTRIES +
                                (raw < chars % ECUIO_ENUM_MAX_ENTRIES ? 1 : 0);
        values += (raw == 0 ? "\"" : ", \"") + std::to_string(raw) + "\": \"" +
                  std::string(length, 'L') + "\"";
    }
    char number[8];
    std::snprintf(number, sizeof number, "%04X", did);
    return "{\"key\": \"" + key + "\", \"name\": \"N " + key +
           "\", \"ecu\": \"pcm\", \"read\": {\"service\": \"22\", "
           "\"number\": \"" +
           number +
           "\"}, \"field\": {\"byte\": 0}, \"kind\": \"enum\", "
           "\"values\": {" +
           values + "}}";
}

/* The requests obd2's set derives, every module's */
uint32_t obd2_requests()
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t c = 0; set != nullptr && c < set->channel_count; ++c) {
        rx[c] = set->channels[c].rx_id;
    }
    ecuio_derived derived;
    EXPECT_EQ(ecuio_engine_plan(set, rx,
                                set != nullptr ? set->channel_count : 0,
                                nullptr, &derived),
              ecuio_plan_result::ok);
    return derived.total;
}

/* The bytes of a loaded package's first variant as one signal file */
size_t copy_bytes(const std::string& package)
{
    Plugin p = load(package);
    size_t length = 0;
    if (p) {
        (void)ecuio_signal_file_text(ecuio_plugin_variant_file(p.get(), 0),
                                     &length);
    }
    return length;
}

/* A package at `version` of two variants, whose first (base, reading
 * a.json then b.json) is written as a copy `bytes` long: enums at their
 * longest, about 12 KB of copy each, then three whose labels take up
 * what those leave. The other variant, track, reads one small file. */
std::string copy_sized(size_t bytes, const std::string& version = "1.0.0")
{
    const unsigned least = ECUIO_ENUM_MAX_ENTRIES;
    const unsigned most = ECUIO_ENUM_MAX_ENTRIES * ECUIO_ENUM_LABEL_MAX;
    const auto with = [&](unsigned whole, size_t spare) {
        std::string a;
        std::string b;
        for (unsigned i = 0; i < whole; ++i) {
            /* Two digits in every key, so each is as long as the next */
            std::string& to = i % 2 == 0 ? a : b;
            to += (to.empty() ? "" : ", ") +
                  enum_of("w" + std::to_string(10 + i), 0xE000 + i, most);
        }
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned more =
                static_cast<unsigned>(std::min<size_t>(spare, most - least));
            spare -= more;
            b += ", " +
                 enum_of("x" + std::to_string(i), 0xEF00 + i, least + more);
        }
        EXPECT_EQ(spare, 0u) << "three enums' labels don't take up the rest";
        /* data by its whole name: a bare call on a string that isn't
         * const finds std::data */
        return packed(kPcm,
                      R"({"id": "base", "files": ["a.json", "b.json"]}, )"
                      R"({"id": "track", "files": ["c.json"]})",
                      "\"a.json\": " + plugin_wrap_test::data(a) +
                          ", \"b.json\": " + plugin_wrap_test::data(b) +
                          ", \"c.json\": " + data(sig("egt", "F40E", "pcm")),
                      version);
    };
    /* The copy grows by one size for each whole enum: measured, since
     * the size is the writer's */
    const size_t two = copy_bytes(with(2, 0));
    const size_t each = (copy_bytes(with(4, 0)) - two) / 2;
    if (two == 0 || each == 0 || bytes < two) {
        ADD_FAILURE() << "no copy of " << bytes << " bytes from " << two
                      << " and " << each << " for each enum";
        return std::string();
    }
    const size_t whole = 2 + (bytes - two) / each;
    EXPECT_LE(whole, 90u) << "a key past two digits";
    return with(static_cast<unsigned>(whole), (bytes - two) % each);
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
        {"plugin format 2", of_format(one_file(kPcm, data(oil)), '2'), "0.9.0"},
        {"plugin format 3", of_format(one_file(kPcm, data(oil)), '3'), "0.9.1"},
        {"signal-file format 3",
         one_file(kPcm, "{\"havne_signals\": 3, \"signals\": [" + oil + "]}"),
         "0.9.1"},
        {"an ECU's name",
         one_file(R"({"id": "pcm", "rx": "7E8", "name": "Engine"})", data(oil)),
         "0.9.1"},
        {"a page",
         packed(
             kPcm, R"({"id": "base", "files": ["a.json"]})",
             "\"a.json\": " + data(oil) +
                 R"(, "p.json": {"format": 2, "form": "phone", "pages": [{}]})",
             "1.0.0", R"("pages": ["p.json"], )"),
         "0.9.0"},
    };
    for (const row& r : rows) {
        EXPECT_EQ(least_of_package(r.package), r.least) << r.what;
    }

    /* A signal file alone, through the same build */
    EXPECT_EQ(least_of_file(file("F", "S", kPcm, oil)), "0.5.0");
    EXPECT_EQ(least_of_file(file("F", "S", kPcm, kGauged)), "0.7.2");
    EXPECT_EQ(least_of_file(file("F", "S", ecus(5), oil)), "0.9.0");
    EXPECT_EQ(
        least_of_file(file(
            "F", "S", R"({"id": "pcm", "rx": "7E8", "name": "Engine"})", oil)),
        "0.9.1");
    EXPECT_EQ(least_of_file(file(
                  "F", "S",
                  R"({"id": "pcm", "rx": "7E8", "session": "extended"})", oil)),
              "0.9.1");
    std::string format3 = file("F", "S", kPcm, oil);
    format3.replace(format3.find("\"havne_signals\": 1"), 18,
                    "\"havne_signals\": 3");
    EXPECT_EQ(least_of_file(format3), "0.9.1");
    EXPECT_EQ(least_of_file(file("F", "S", kFourPastTheBase,
                                 sig("oil", "F40D", "e1"), extends)),
              "0.9.0");
}

// What a set holds past one file's bounds (ADR-0065) needs the core
// that reads it, each bound alone: a variant's signals together, its
// requests in all, and its written copy
TEST(PluginWrapTest, ASetPastAFilesBoundsNeedsItsCore)
{
    /* 1,024 signals on 512 requests, and one more signal on a request
     * the first file has */
    const std::string most = data(
        signals_sharing(ECUIO_SIGNAL_FILE_MAX_SIGNALS, "a", "pcm", 0xE000));
    EXPECT_EQ(least_of_package(one_file(kPcm, most)), "0.8.0");
    EXPECT_EQ(least_of_package(one_file(
                  kPcm, data(signals_sharing(ECUIO_SIGNAL_FILE_MAX_SIGNALS, "a",
                                             "pcm", 0xE000),
                             R"("extends": "obd2")"))),
              "0.8.0")
        << "the file's own signals are judged, the base's not counted";
    EXPECT_EQ(least_of_package(
                  two_files(kPcm, most,
                            R"({"havne_signals": 1, "signals": [{"key": "b0", )"
                            R"("name": "N b0", "ecu": "pcm", )"
                            R"("read": {"service": "22", "number": "E000"}, )"
                            R"("field": {"byte": 2}}]})")),
              "0.9.0");

    /* obd2's requests and a second ECU's make 1,024 in all, then one more:
     * the signals stay within a file's */
    const uint32_t base = obd2_requests();
    ASSERT_GT(base, 0u);
    ASSERT_LT(base, 512u);
    const std::string e1 = R"({"id": "e1", "rx": "7E9"})";
    const std::string extends = R"("extends": "obd2")";
    const auto on_e1 = [](uint32_t n) {
        std::string list;
        char did[8];
        for (uint32_t i = 0; i < n; ++i) {
            std::snprintf(did, sizeof did, "%04X", 0xE000 + i);
            list +=
                (i == 0 ? "" : ", ") + sig("s" + std::to_string(i), did, "e1");
        }
        return list;
    };
    /* 1,024: what a set could ask in all before ADR-0065, written out so
     * the row's own bound is what is judged */
    const uint32_t fits = 1024 - base;
    EXPECT_EQ(least_of_package(one_file(e1, data(on_e1(fits), extends))),
              "0.8.0");
    EXPECT_EQ(least_of_package(one_file(e1, data(on_e1(fits + 1), extends))),
              "0.9.0");
    EXPECT_EQ(least_of_file(file("F", "S", e1, on_e1(fits + 1), extends)),
              "0.9.0");

    /* Two files of long enums, far fewer than 256 requests: a copy of
     * 1 MiB to the byte (a file's most before ADR-0065, written out),
     * and one byte more */
    const size_t file_most = 1048576;
    const std::string full = copy_sized(file_most);
    const std::string past = copy_sized(file_most + 1);
    EXPECT_EQ(copy_bytes(full), file_most);
    EXPECT_EQ(copy_bytes(past), file_most + 1);
    EXPECT_EQ(least_of_package(full), "0.5.0");
    EXPECT_EQ(least_of_package(past), "0.9.0");
}

// A drop judges its result as a put does. Its version is in each
// variant's written copy, so one a digit longer carries a copy at a
// file's most bytes past them, and the result needs the core that
// reads such a set
TEST(PluginWrapTest, ADropsLongerVersionCanNeedALaterCore)
{
    CoreAs later(0, 9, 3);
    const size_t file_most = 1048576;
    /* "1.9.0" becomes "1.10.0" */
    const std::string room =
        ok(drop(copy_sized(file_most - 1, "1.9.0"), "track"));
    EXPECT_EQ(copy_bytes(room), file_most);
    Plugin within = load(room);
    ASSERT_TRUE(within);
    EXPECT_EQ(requires_of(within.get()), "0.5.0");

    const std::string none = ok(drop(copy_sized(file_most, "1.9.0"), "track"));
    EXPECT_EQ(copy_bytes(none), file_most + 1);
    Plugin grown = load(none);
    ASSERT_TRUE(grown);
    EXPECT_EQ(requires_of(grown.get()), "0.9.0");
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

// A put and a drop keep a requires.core above what the result needs
// (the author's word) and raise one below it, so neither writes a
// plugin that names a core too old to read it
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
    const auto drop_from = [&](const std::string& core) {
        Plugin p = load(ok(drop(requiring(two_variants, core), "track")));
        return p ? requires_of(p.get()) : std::string("refused");
    };
    EXPECT_EQ(drop_from("0.9.2"), "0.9.2") << "kept";
    EXPECT_EQ(drop_from("0.1.0"), "0.8.0") << "raised to the lineage's";
}
