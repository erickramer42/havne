// File/layer: tests/test_plugin_put.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginWrapTest (test_signals)
//
// PluginWrapTest's cases for a variant put into a packed plugin or
// dropped from it (ecuio_plugin_put_variant, ecuio_plugin_drop_variant);
// the wrap's are in test_plugin_wrap.cpp.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>

#include <string>
#include <vector>

#include "plugin_wrap_support.h"

using namespace plugin_wrap_test;

// A put adds a variant after the others; the version's minor rises and
// its patch goes to 0, requires.core rises to what reads it, and the id,
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
    EXPECT_EQ(requires_of(g.get()), "0.8.0")
        << "the least core: the file put in keeps its own source";
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

    /* requires.core is raised to the least core that reads what is left */
    const std::string hand = packed(
        kPcm,
        R"({"id": "base", "files": ["a.json"]}, {"id": "v", "files": ["a.json", "b.json"]})",
        "\"a.json\": " + data(sig("oil", "F40D", "pcm")) +
            ", \"b.json\": " + data(sig("egt", "F40E", "pcm")),
        "3.0.9");
    const std::string less = ok(drop(hand, "v"));
    Plugin h = load(less);
    ASSERT_TRUE(h);
    EXPECT_EQ(requires_of(h.get()), "0.5.0");
    EXPECT_STREQ(info_of(h.get()).version, "3.1.0");
    EXPECT_EQ(files_of(less), Strings{"a.json"})
        << "the file base still lists stays";
    EXPECT_EQ(files_of(ok(drop(hand, "base"))), (Strings{"a.json", "b.json"}));
}

// A put and a drop keep the plugin's pages as they were: a page is never
// a variant's file, so neither moves nor reads it
TEST(PluginWrapTest, APutAndADropKeepThePages)
{
    const std::string page =
        R"({"format": 2, "form": "phone", "pages": [{"name": "Engine", "cells": []}]})";
    /* The page sits where the put's file would go, so the file takes the
     * next name */
    const std::string base =
        packed(kPcm, R"({"id": "base", "files": ["a.json"]})",
               "\"a.json\": " + data(sig("oil", "F40D", "pcm")) +
                   ", \"data/track.json\": " + page,
               "1.0.0", R"("pages": ["data/track.json"], )");
    Plugin b = load(base);
    ASSERT_TRUE(b);
    const std::string text = ecuio_plugin_page_text(b.get(), 0);
    const auto keeps = [&](const std::string& made) {
        Plugin p = load(made);
        ASSERT_TRUE(p);
        EXPECT_EQ(info_of(p.get()).page_count, 1u);
        EXPECT_EQ(std::string(ecuio_plugin_page_text(p.get(), 0)), text);
    };
    const std::string two = ok(put(base, "track", file_b()));
    keeps(two);
    EXPECT_EQ(files_of(two),
              (Strings{"a.json", "data/track.json", "data/track-2.json"}));
    const std::string one = ok(drop(two, "base"));
    keeps(one);
    EXPECT_EQ(files_of(one), (Strings{"data/track.json", "data/track-2.json"}));
    const std::string replaced = ok(put(base, "base", file_b()));
    keeps(replaced);
    EXPECT_EQ(files_of(replaced),
              (Strings{"data/track.json", "data/base.json"}));

    /* A file bringing the manifest its first ECU names the plugin's own
     * signals that named none, and leaves the page alone */
    const std::string no_ecus = packed(
        "", R"({"id": "base", "files": ["a.json"]})",
        "\"a.json\": " + data(sig("egt", "F40E", ""), R"("extends": "obd2")") +
            ", \"p.json\": " + page,
        "1.0.0", R"("pages": ["p.json"], )");
    const std::string grown =
        ok(put(no_ecus, "v", file("V", "S", kTcm, sig("tft", "1E1C", ""))));
    EXPECT_EQ(ecus_of(grown), (Strings{"tcm@7E9", "obd2@7E8/7E0"}));
    Plugin g = load(grown);
    ASSERT_TRUE(g);
    EXPECT_EQ(info_of(g.get()).page_count, 1u);
    EXPECT_EQ(std::string(ecuio_plugin_page_text(g.get(), 0)), text);
}
