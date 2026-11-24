// File/layer: tests/test_cli_files.cpp | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#include "test_cli_support.h"

TEST(CliInfoTest, EachKindAndSkipHasItsWord)
{
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_GAP), "gap");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_RATE_DROP), "rate-drop");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_LATE_ID), "late-id");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_OVERFLOW), "overflow");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_ISOTP), "isotp");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_LINK), "link");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_ASLEEP), "asleep");
    EXPECT_STREQ(cli::check_kind_word(99), "unknown");
    EXPECT_STREQ(cli::isotp_word(0), "checked");
    EXPECT_STREQ(cli::isotp_word(ECUIO_CHECK_SKIPPED_ISOTP |
                                 ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN),
                 "flags-unknown");
    EXPECT_STREQ(
        cli::isotp_word(ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE),
        "no-wire");
}

TEST(CliInfoTest, TheLinesCarryWhatTheCheckFound)
{
    char buf[1024];
    ecuio_check_finding_t drop{};
    drop.kind = ECUIO_CHECK_RATE_DROP;
    drop.arbitration_id = 0x18DAF110;
    drop.flags = ECUIO_CHECK_FINDING_EXTENDED;
    drop.at_us = 7920000;
    drop.length_us = 800000;
    drop.count = 2;
    cli::Line d(buf, sizeof buf);
    cli::finding_line(d, drop);
    EXPECT_EQ(std::string(d.data(), d.size()),
              "{\"type\":\"finding\",\"kind\":\"rate-drop\",\"id\":"
              "\"0x18DAF110\",\"extended\":true,\"at_us\":7920000,"
              "\"length_us\":800000,\"count\":2}");

    /* The whole bus's has no ID, though 0x000 is one */
    ecuio_check_finding_t gap{};
    gap.kind = ECUIO_CHECK_GAP;
    gap.flags = ECUIO_CHECK_FINDING_WHOLE_BUS;
    gap.at_us = 4950000;
    gap.length_us = 1550000;
    cli::Line g(buf, sizeof buf);
    cli::finding_line(g, gap);
    EXPECT_EQ(std::string(g.data(), g.size()),
              "{\"type\":\"finding\",\"kind\":\"gap\",\"id\":null,"
              "\"extended\":false,\"at_us\":4950000,\"length_us\":1550000,"
              "\"count\":0}");
    ecuio_check_finding_t zero{};
    zero.kind = ECUIO_CHECK_LATE_ID;
    zero.count = 1;
    cli::Line z(buf, sizeof buf);
    cli::finding_line(z, zero);
    EXPECT_NE(std::string(z.data(), z.size()).find("\"id\":\"0x0\""),
              std::string::npos);

    /* A link's state, by code and core's words */
    ecuio_check_finding_t link{};
    link.kind = ECUIO_CHECK_LINK;
    link.flags = ECUIO_CHECK_FINDING_WHOLE_BUS;
    link.at_us = 18000000;
    link.count = 1;
    link.code = ECUIO_ERR_VEHICLE_TIMEOUT;
    cli::Line k(buf, sizeof buf);
    cli::finding_line(k, link);
    EXPECT_EQ(std::string(k.data(), k.size()),
              std::string("{\"type\":\"finding\",\"kind\":\"link\",\"id\":null,"
                          "\"extended\":false,\"at_us\":18000000,"
                          "\"length_us\":0,\"count\":1,\"code\":8,"
                          "\"message\":\"") +
                  ecuio_status_string(ECUIO_ERR_VEHICLE_TIMEOUT) + "\"}");

    ecuio_check_summary_t s{};
    s.duration_us = 19950000;
    s.frames = 515;
    s.ids = 3;
    s.torn_rows = 1;
    s.flags = ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE;
    for (uint32_t kind = ECUIO_CHECK_GAP; kind <= ECUIO_CHECK_ASLEEP; ++kind) {
        s.counts[kind] = kind * 10;
    }
    ecuio_check_options_t o{};
    o.gap_us = 1000000;
    o.rate_drop_x100 = 450;
    o.settle_us = 2000000;
    cli::Line m(buf, sizeof buf);
    cli::summary_line(m, s, o);
    EXPECT_EQ(std::string(m.data(), m.size()),
              "{\"type\":\"summary\",\"duration_us\":19950000,\"frames\":515,"
              "\"ids\":3,\"torn_rows\":1,\"isotp\":\"no-wire\",\"findings\":{"
              "\"gap\":10,\"rate-drop\":20,\"late-id\":30,\"overflow\":40,"
              "\"isotp\":50,\"link\":60,\"asleep\":70},\"gap_us\":1000000,"
              "\"rate_drop\":4.5,\"settle_us\":2000000}");

    cli::Line f(buf, sizeof buf);
    cli::open_failed_line(f, 0, ECUIO_ERR_LOAD_FAILED,
                          "the folder has no signals.csv");
    EXPECT_EQ(std::string(f.data(), f.size()),
              std::string("{\"type\":\"open_failed\",\"recording\":0,"
                          "\"code\":7,\"message\":\"") +
                  ecuio_status_string(ECUIO_ERR_LOAD_FAILED) +
                  "\",\"detail\":\"the folder has no signals.csv\"}");
}

// A URI's profile= is the last one in its query, as core reads it, and
// only that key: one that ends in profile isn't it
TEST(CliSignalFileTest, AUrisProfileIsItsLastProfileKey)
{
    std::string p;
    EXPECT_FALSE(cli::uri_profile("mock://x", &p));
    EXPECT_FALSE(cli::uri_profile("mock://x?myprofile=a", &p));
    ASSERT_TRUE(cli::uri_profile("mock://x?profile=obd2", &p));
    EXPECT_EQ(p, "obd2");
    ASSERT_TRUE(
        cli::uri_profile("replay://a?speed=max&profile=a&profile=b", &p));
    EXPECT_EQ(p, "b");
    ASSERT_TRUE(cli::uri_profile("mock://x?profile=", &p));
    EXPECT_EQ(p, "");
}

// A loaded file's profile is added to a URI that names none, kept where
// the URI names it, and a URI naming another is refused
TEST(CliSignalFileTest, TheFilesProfileIsTheUrisWhenItNamesNone)
{
    std::string out;
    ASSERT_TRUE(cli::uri_for_profile("mock://x", "user.a", &out));
    EXPECT_EQ(out, "mock://x?profile=user.a");
    ASSERT_TRUE(cli::uri_for_profile("replay://p?speed=max", "user.a", &out));
    EXPECT_EQ(out, "replay://p?speed=max&profile=user.a");
    ASSERT_TRUE(
        cli::uri_for_profile("mock://x?profile=user.a", "user.a", &out));
    EXPECT_EQ(out, "mock://x?profile=user.a");
    EXPECT_FALSE(cli::uri_for_profile("mock://x?profile=obd2", "user.a", &out));
    EXPECT_FALSE(cli::uri_for_profile("mock://x?profile=", "user.a", &out));
}

// The kind words are a signal file's, and a unit line is a file's own
// signal's unit that never converts
TEST(CliSignalFileTest, KindsAndUnitsAreTheFilesWords)
{
    ecuio_signal_def_t d{};
    d.kind = ECUIO_VALUE_UINT;
    EXPECT_STREQ(cli::kind_word(d), "number");
    d.kind = ECUIO_VALUE_INT;
    EXPECT_STREQ(cli::kind_word(d), "number");
    d.kind = ECUIO_VALUE_TEXT;
    EXPECT_STREQ(cli::kind_word(d), "text");
    d.kind = ECUIO_VALUE_HEX;
    EXPECT_STREQ(cli::kind_word(d), "hex");
    d.kind = ECUIO_VALUE_ENUM;
    EXPECT_STREQ(cli::kind_word(d), "enum");
    d.kind = ECUIO_VALUE_UINT;
    d.flags = ECUIO_DEF_FLAG_COMPUTED;
    EXPECT_STREQ(cli::kind_word(d), "computed");

    ecuio_signal_def_t u{};
    std::strcpy(u.unit, "km/h");
    EXPECT_TRUE(cli::unit_converts(u));
    EXPECT_FALSE(cli::unit_kept(u, true));
    std::strcpy(u.unit, "kph");
    EXPECT_FALSE(cli::unit_converts(u));
    EXPECT_TRUE(cli::unit_kept(u, true));
    std::strcpy(u.unit, "%");
    EXPECT_TRUE(cli::unit_kept(u, true));
    u.unit[0] = '\0';
    EXPECT_FALSE(cli::unit_kept(u, true)) << "no unit, nothing to convert";
    std::strcpy(u.unit, "%");
    EXPECT_FALSE(cli::unit_kept(u, false)) << "the base set's, not the file's";
}

// Each line says what the loader read, in the file's own terms: a reply's
// echo split back into service and number, the byte after it, the field,
// an enum's labels, a computed signal's inputs with the stale age where
// the file gives none
TEST(CliSignalFileTest, TheLinesSayWhatTheLoaderRead)
{
    const std::string json =
        "{\"havne_signals\":1,\"name\":\"Unit test\","
        "\"source\":{\"name\":\"test\"},"
        "\"ecus\":[{\"id\":\"tcm\",\"rx\":\"7E9\"}],\"signals\":["
        "{\"key\":\"temp\",\"name\":\"Temp\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},"
        "\"field\":{\"byte\":1,\"bits\":16,\"signed\":true},"
        "\"scale\":0.5,\"offset\":-40,\"unit\":\"kph\",\"decimals\":1,"
        "\"group\":\"engine\",\"plot\":2},"
        "{\"key\":\"gear\",\"name\":\"Gear\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"01\",\"number\":\"A4\"},"
        "\"field\":{\"byte\":0,\"bits\":4,\"bit\":2,\"order\":\"le\"},"
        "\"kind\":\"enum\",\"values\":{\"1\":\"First\",\"2\":\"Second\"},"
        "\"group\":\"vehicle\"},"
        "{\"key\":\"part\",\"name\":\"Part\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"22\",\"number\":\"F187\"},"
        "\"field\":{\"byte\":0,\"bytes\":5},\"kind\":\"hex\"},"
        "{\"key\":\"half\",\"name\":\"Half\",\"kind\":\"computed\","
        "\"expr\":\"temp / 2\",\"unit\":\"kph\"}]}";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    ASSERT_EQ(set.count, 4u);
    char buf[4096];
    auto text = [&](auto write) {
        cli::Line l(buf, sizeof buf);
        write(l);
        return std::string(l.data(), l.size());
    };
    EXPECT_EQ(text([&](cli::Line& l) {
                  cli::signal_file_line(l, file, set);
              }),
              "{\"type\":\"signal_file\",\"name\":\"Unit test\","
              "\"profile\":\"user.unit-test\",\"format\":1,"
              "\"signals\":4,\"base\":0,"
              "\"ecus\":[{\"rx\":\"0x7E9\",\"tx\":\"0x7E1\"}]}");
    const std::string other = ecuio_signal_group_name(ECUIO_SIGNAL_GROUP_OTHER);
    const std::string expected[] = {
        "{\"type\":\"signal_def\",\"id\":30000,\"name\":\"Temp\","
        "\"kind\":\"number\",\"unit\":\"kph\",\"decimals\":1,"
        "\"group\":\"Engine\",\"rank\":2,\"rx\":\"0x7E9\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},\"byte\":1,"
        "\"bits\":16,\"order\":\"be\",\"signed\":true,\"scale\":0.5,"
        "\"offset\":-40}",
        "{\"type\":\"signal_def\",\"id\":30001,\"name\":\"Gear\","
        "\"kind\":\"enum\",\"unit\":\"\",\"decimals\":0,"
        "\"group\":\"Vehicle\",\"rank\":0,\"rx\":\"0x7E9\","
        "\"read\":{\"service\":\"01\",\"number\":\"A4\"},\"byte\":0,"
        "\"bits\":4,\"bit\":2,\"order\":\"le\",\"signed\":false,"
        "\"labels\":{\"1\":\"First\",\"2\":\"Second\"}}",
        "{\"type\":\"signal_def\",\"id\":30002,\"name\":\"Part\","
        "\"kind\":\"hex\",\"unit\":\"\",\"decimals\":0,\"group\":\"" +
            other +
            "\",\"rank\":0,\"rx\":\"0x7E9\","
            "\"read\":{\"service\":\"22\",\"number\":\"F187\"},\"byte\":0,"
            "\"bytes\":5}",
        "{\"type\":\"signal_def\",\"id\":30003,\"name\":\"Half\","
        "\"kind\":\"computed\",\"unit\":\"kph\",\"decimals\":0,\"group\":\"" +
            other +
            "\",\"rank\":0,\"expr\":\"temp / 2\","
            "\"inputs\":[{\"id\":30000,\"max_age_us\":3000000}]}",
    };
    for (uint32_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_TRUE(cli::own_signal(file, set.defs[i]));
        EXPECT_LE(expected[i].size(), cli::def_line_bytes(set, set.defs[i]));
        EXPECT_EQ(text([&](cli::Line& l) {
                      cli::def_line(l, set, set.defs[i], nullptr,
                                    ecuio_signal_file_expr(file, &set.defs[i]));
                  }),
                  expected[i]);
    }
    EXPECT_EQ(text([&](cli::Line& l) { cli::unit_line(l, set.defs[0]); }),
              "{\"type\":\"unit\",\"id\":30000,\"name\":\"Temp\","
              "\"unit\":\"kph\",\"converts\":false}");
    uint32_t n = 0;
    EXPECT_EQ(cli::def_labels(set, set.defs[0], &n), nullptr);
    EXPECT_EQ(n, 0u) << "a number has no labels";
    ASSERT_NE(cli::def_labels(set, set.defs[1], &n), nullptr);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(cli::def_program(set, set.defs[0]), nullptr);
    EXPECT_NE(cli::def_program(set, set.defs[3]), nullptr);
    ecuio_signal_file_free(file);
}

// A def's gauge field: nothing for a def with none, else min and max and
// only the levels given (cli::gauge_field)
// A def that replaced a base one by name has the base's ID (7002 here), so
// the ID range no longer says whose it is: the file's key does, and the
// line and the count follow it
TEST(CliSignalFileTest, AReplacingDefIsTheFilesOwnWhateverItsId)
{
    const std::string json =
        "{\"havne_signals\":1,\"name\":\"Replaces\","
        "\"source\":{\"name\":\"test\"},\"extends\":\"obd2\",\"signals\":["
        "{\"key\":\"temp\",\"name\":\"Coolant Temp\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},"
        "\"field\":{\"byte\":1},\"unit\":\"kph\"}]}";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    const ecuio_signal_def_t* temp = ecuio_signal_file_find(file, "temp");
    ASSERT_NE(temp, nullptr);
    ASSERT_LT(temp->id, ECUIO_USER_SIG_ID_BASE) << "the base def's ID";
    EXPECT_TRUE(cli::own_signal(file, *temp));
    for (uint32_t i = 0; i < set.count; ++i) {
        if (&set.defs[i] != temp) {
            EXPECT_FALSE(cli::own_signal(file, set.defs[i])) << set.defs[i].name;
        }
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::signal_file_line(l, file, set);
    const std::string line(l.data(), l.size());
    EXPECT_NE(line.find("\"signals\":1,"), std::string::npos) << line;
    ecuio_signal_file_free(file);
}

TEST(CliSignalFileTest, AGaugeFieldHoldsOnlyTheLevelsGiven)
{
    const auto field = [](const ecuio_signal_gauge_t* g) {
        char buf[cli::kGaugeBytes + 16];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"id\":1");
        cli::gauge_field(l, g);
        l.ch('}');
        return std::string(l.data(), l.size());
    };
    EXPECT_EQ(field(nullptr), "{\"id\":1}");
    ecuio_signal_gauge_t g{};
    g.min = -40;
    g.max = 130.5;
    g.warn_low = 60;
    g.warn_high = 110;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5}}")
        << "a level whose flag is clear isn't given";
    g.flags = ECUIO_GAUGE_WARN_LOW;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5,"
                         "\"warn_low\":60}}");
    g.flags = ECUIO_GAUGE_WARN_HIGH;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5,"
                         "\"warn_high\":110}}");
    g.flags = ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5,"
                         "\"warn_low\":60,\"warn_high\":110}}");
}

// An import's notes and summary are lines of their own, each kind a word
TEST(CliSignalFileTest, AnImportsNotesAreLinesOfTheirOwn)
{
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_CONVERTED), "converted");
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_CHANGED), "changed");
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_SKIPPED), "skipped");
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_FILE), "file");
    EXPECT_STREQ(cli::note_word(9), "unknown");
    ecuio_obdb_note_t n{};
    n.what = ECUIO_OBDB_NOTE_CHANGED;
    n.line = 12;
    n.signal = "TCM_TFT";
    n.key = "tcm_tft";
    n.field = "max";
    n.text = "OBDb holds the value within 0 to 215";
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::import_note_line(l, n);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"import_note\",\"what\":\"changed\",\"line\":12,"
              "\"signal\":\"TCM_TFT\",\"key\":\"tcm_tft\",\"field\":\"max\","
              "\"text\":\"OBDb holds the value within 0 to 215\"}");
    EXPECT_LE(l.size(), cli::note_line_bytes(n));
    cli::Line s(buf, sizeof buf);
    cli::imported_line(s, 7, 6, 5);
    EXPECT_EQ(std::string(s.data(), s.size()),
              "{\"type\":\"imported\",\"converted\":7,\"changed\":6,"
              "\"skipped\":5}");
}

namespace {

// A packed plugin of two variants, the second for 2019 on
const char kPacked[] =
    R"({"havne_plugin": 1, "id": "p", "name": "P", "version": "1.2.3",)"
    R"( "requires": {"core": "0.1.0"}, "source": {"name": "t"},)"
    R"( "tiers": ["data"], "match": [{"wmi": ["ZZZ"]}],)"
    R"( "ecus": [{"id": "pcm", "rx": "7E8"}],)"
    R"( "variants": [{"id": "base", "files": ["a.json"]},)"
    R"( {"id": "late", "years": [2019, 2030], "files": ["a.json"]}],)"
    R"( "files": {"a.json": {"havne_signals": 1, "signals": [{"key": "oil",)"
    R"( "name": "Oil", "ecu": "pcm", "read": {"service": "22",)"
    R"( "number": "F40D"}, "field": {"byte": 0}, "scale": 0.25}]}}})";

struct Packed {
    ecuio_plugin_t* p = nullptr;
    Packed()
    {
        EXPECT_EQ(ecuio_plugin_load(kPacked, sizeof kPacked - 1, nullptr,
                                    nullptr, &p),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Packed() { ecuio_plugin_free(p); }
    ecuio_plugin_info_t info() const
    {
        ecuio_plugin_info_t i{};
        i.struct_size = sizeof i;
        ecuio_plugin_info(p, &i);
        return i;
    }
};

} // namespace

// A fixture passes within half its field's step, an enum's step being 1,
// with only the double's own rounding as slack
TEST(CliPluginTest, AFixturePassesWithinHalfItsFieldsStep)
{
    ecuio_signal_def_t d{};
    d.kind = ECUIO_VALUE_UINT;
    d.scale = 0.1;
    EXPECT_TRUE(cli::fixture_passes(d, 81.3, 81.3));
    EXPECT_TRUE(cli::fixture_passes(d, 81.3, 81.35));
    EXPECT_TRUE(cli::fixture_passes(d, 81.3, 81.25));
    EXPECT_FALSE(cli::fixture_passes(d, 81.3, 81.36));
    EXPECT_FALSE(cli::fixture_passes(d, 81.3, 81.2));
    d.scale = -2;
    EXPECT_TRUE(cli::fixture_passes(d, 10, 11)) << "the step's size, |scale|";
    EXPECT_FALSE(cli::fixture_passes(d, 10, 11.01));
    d.kind = ECUIO_VALUE_ENUM;
    d.scale = 1;
    EXPECT_TRUE(cli::fixture_passes(d, 3, 3));
    EXPECT_FALSE(cli::fixture_passes(d, 3, 4));
    EXPECT_FALSE(cli::fixture_passes(d, 3, 2));
    EXPECT_EQ(cli::plugin_profile("example-sim", "my2019"),
              "plugin.example-sim.my2019");
}

// The plugin line names each variant's profile; the match line the
// variants proposed, and a VIN with no model year's null
TEST(CliPluginTest, ThePluginAndMatchLinesNameTheProfiles)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    char buf[2048];
    cli::Line l(buf, sizeof buf);
    cli::plugin_line(l, 2, pk.p, pk.info());
    ecuio_plugin_fingerprint_t print{};
    print.struct_size = sizeof print;
    ASSERT_EQ(ecuio_plugin_fingerprint(pk.p, &print), ECUIO_OK);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"plugin\",\"package\":2,\"id\":\"p\",\"version\":"
              "\"1.2.3\",\"name\":\"P\",\"tiers\":[\"data\"],\"variants\":["
              "{\"id\":\"base\",\"profile\":\"plugin.p.base\"},{\"id\":"
              "\"late\",\"profile\":\"plugin.p.late\"}],\"fixtures\":0,"
              "\"fingerprint\":\"" +
                  cli::hex_text(print.fingerprint, 32) +
                  "\",\"publisher\":null}\n");

    ecuio_vin_info_t vin{};
    std::strcpy(vin.wmi, "ZZZ");
    vin.model_year = 2018;
    uint32_t idx[4] = {};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_plugin_match(pk.p, &vin, idx, 4, &n), ECUIO_OK);
    cli::Line m(buf, sizeof buf);
    cli::match_line(m, pk.p, pk.info(), vin, idx, n);
    EXPECT_EQ(text_of(m), "{\"type\":\"match\",\"wmi\":\"ZZZ\",\"model_year\":"
                          "2018,\"variants\":[{\"id\":\"base\",\"profile\":"
                          "\"plugin.p.base\"}]}\n");
    vin.model_year = 0;
    ASSERT_EQ(ecuio_plugin_match(pk.p, &vin, idx, 4, &n), ECUIO_OK);
    cli::Line z(buf, sizeof buf);
    cli::match_line(z, pk.p, pk.info(), vin, idx, n);
    EXPECT_NE(text_of(z).find("\"model_year\":null"), std::string::npos);
}

// A variant line counts its own signals apart from its base set's, and
// lists its ECUs
TEST(CliPluginTest, AVariantLineCountsItsOwnSignals)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(pk.p, 0);
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::variant_line(l, "base", "plugin.p.base",
                      ecuio_plugin_variant_file(pk.p, 0), set);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"variant\",\"id\":\"base\",\"profile\":"
              "\"plugin.p.base\",\"signals\":1,\"base\":0,\"ecus\":[{\"rx\":"
              "\"0x7E8\",\"tx\":\"0x7E0\"}],\"license\":null,"
              "\"share_alike\":false}\n");
}

// A number or computed signal of the variant's own with no gauge gets a
// note; a gauge, a text or hex or enum value, or a signal the base set holds
// gets none
TEST(CliPluginTest, ANumberWithNoGaugeGetsANoteAndNothingElseDoes)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    const ecuio_signal_file_t* file = ecuio_plugin_variant_file(pk.p, 0);
    const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(pk.p, 0);
    const ecuio_signal_def_t& own = set.defs[set.count - 1];
    ASSERT_TRUE(cli::own_signal(file, own));
    ASSERT_STREQ(cli::kind_word(own), "number");
    EXPECT_TRUE(cli::lacks_gauge(file, own, nullptr));
    const ecuio_signal_gauge_t gauge = {};
    EXPECT_FALSE(cli::lacks_gauge(file, own, &gauge)) << "it has a gauge";
    ecuio_signal_def_t d = own;
    for (const int kind : {static_cast<int>(ECUIO_VALUE_TEXT),
                           static_cast<int>(ECUIO_VALUE_HEX),
                           static_cast<int>(ECUIO_VALUE_ENUM)}) {
        d.kind = static_cast<uint8_t>(kind);
        EXPECT_FALSE(cli::takes_gauge(d)) << "not a number";
    }
    d = own;
    d.flags |= ECUIO_DEF_FLAG_COMPUTED;
    EXPECT_TRUE(cli::takes_gauge(d)) << "computed counts";
    std::vector<char> buf(cli::no_gauge_line_bytes());
    cli::Line l(buf.data(), buf.size());
    cli::no_gauge_line(l, "base", d);
    const std::string line = text_of(l);
    EXPECT_EQ(line.rfind("{\"type\":\"no_gauge\",\"variant\":\"base\",\"id\":", 0),
              0u)
        << line;
    EXPECT_NE(line.find("\"kind\":\"computed\""), std::string::npos) << line;
    EXPECT_NE(line.find("no gauge block"), std::string::npos) << line;
    EXPECT_NE(line.find("\"key\":null"), std::string::npos)
        << "a key field: " << line;
    cli::Line keyed(buf.data(), buf.size());
    cli::no_gauge_line(keyed, "base", d, "boost");
    EXPECT_NE(text_of(keyed).find("\"key\":\"boost\""), std::string::npos)
        << text_of(keyed);
}

// A variant's license and whether it's share-alike come from its written
// file's source, the data file's own before the manifest's; the
// share_alike line names the variant, the license and what passing the
// plugin on keeps
TEST(CliPluginTest, AVariantLineNamesItsLicenseAndShareAlike)
{
    std::string packed = kPacked;
    const std::string data = "{\"havne_signals\": 1, \"signals\"";
    ASSERT_NE(packed.find(data), std::string::npos);
    packed.replace(packed.find(data), data.size(),
                   "{\"havne_signals\": 1, \"source\": {\"name\": \"OBDb\", "
                   "\"license\": \"CC-BY-SA-4.0\"}, \"signals\"");
    ecuio_plugin_t* p = nullptr;
    ASSERT_EQ(
        ecuio_plugin_load(packed.data(), packed.size(), nullptr, nullptr, &p),
        ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_file_t* file = ecuio_plugin_variant_file(p, 0);
    const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(p, 0);
    std::vector<char> buf(cli::variant_line_bytes(set, file));
    cli::Line l(buf.data(), buf.size());
    cli::variant_line(l, "base", "plugin.p.base", file, set);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"variant\",\"id\":\"base\",\"profile\":"
              "\"plugin.p.base\",\"signals\":1,\"base\":0,\"ecus\":[{\"rx\":"
              "\"0x7E8\",\"tx\":\"0x7E0\"}],\"license\":\"CC-BY-SA-4.0\","
              "\"share_alike\":true}\n");
    const ecuio_signal_source_t s = cli::source_of(file);
    std::vector<char> sbuf(cli::share_alike_line_bytes(s.license));
    cli::Line sl(sbuf.data(), sbuf.size());
    cli::share_alike_line(sl, "base", s.license);
    EXPECT_EQ(text_of(sl),
              "{\"type\":\"share_alike\",\"variant\":\"base\",\"license\":"
              "\"CC-BY-SA-4.0\",\"words\":\"variant 'base' holds data under "
              "CC-BY-SA-4.0, a share-alike license: whoever passes the plugin "
              "on keeps that license and the variant's source with it\"}\n");
    ecuio_plugin_free(p);

    /* A license at its bound fits the line it's sized for */
    const std::string longest(ECUIO_SOURCE_TEXT_MAX, '"');
    std::vector<char> big(cli::share_alike_line_bytes(longest.c_str()));
    cli::Line bl(big.data(), big.size());
    cli::share_alike_line(bl, "base", longest.c_str());
    EXPECT_TRUE(bl.finish());
    EXPECT_STREQ(cli::source_of(nullptr).license, "") << "none read, none";
}

// The written line: the action, the variant, the version, the lineage
// (null with none), the file's bytes and core's notes (null with none)
TEST(CliPluginTest, AWrittenLineSaysWhatTheCommandDid)
{
    char buf[2048];
    cli::Line l(buf, sizeof buf);
    cli::written_line(l, cli::Written::replaced, "base", "1.2.0",
                      "0123456789abcdef0123456789abcdef", 4290,
                      "the file's ecu 'tcm' is 'tcm_2' in the plugin");
    EXPECT_EQ(text_of(l),
              "{\"type\":\"written\",\"action\":\"replaced\",\"variant\":"
              "\"base\",\"version\":\"1.2.0\",\"lineage\":"
              "\"0123456789abcdef0123456789abcdef\",\"bytes\":4290,\"notes\":"
              "\"the file's ecu 'tcm' is 'tcm_2' in the plugin\"}\n");
    cli::Line n(buf, sizeof buf);
    cli::written_line(n, cli::Written::dropped, "late", "1.0.0", "", 10, "");
    EXPECT_EQ(text_of(n),
              "{\"type\":\"written\",\"action\":\"dropped\",\"variant\":"
              "\"late\",\"version\":\"1.0.0\",\"lineage\":null,\"bytes\":10,"
              "\"notes\":null}\n");
    EXPECT_STREQ(cli::written_word(cli::Written::wrapped), "wrapped");
    EXPECT_STREQ(cli::written_word(cli::Written::added), "added");
}

// An OBDb plugin's variant is my<year> only for a year typed, base (core's
// default, "") for none
TEST(CliPluginTest, OnlyATypedYearNamesAVariant)
{
    EXPECT_EQ(cli::typed_year_variant(2019), "my2019");
    EXPECT_EQ(cli::typed_year_variant(0), "");
}

// --years is two four-digit model years from 1980 to 2039, the first at
// or before the second; --wmi codes of 3 or 6 of a VIN's letters and
// digits, lower case taken as upper
TEST(CliPluginTest, TheMatchOptionsTakeTheirFormsOnly)
{
    uint32_t y[2] = {7, 7};
    EXPECT_TRUE(cli::parse_model_years("2015-2020", y));
    EXPECT_EQ(y[0], 2015u);
    EXPECT_EQ(y[1], 2020u);
    EXPECT_TRUE(cli::parse_model_years("1980-2039", y));
    EXPECT_TRUE(cli::parse_model_years("2019-2019", y));
    for (const char* bad :
         {"2020-2015", "1979-1990", "2015-2040", "2015", "2015-", "-2015",
          "2015--2020", "15-20", "2015-202x", "2015 2020", "02015-2020", ""}) {
        y[0] = y[1] = 7;
        EXPECT_FALSE(cli::parse_model_years(bad, y)) << bad;
        EXPECT_EQ(y[0], 7u) << "nothing written for " << bad;
    }

    std::string codes = "kept";
    EXPECT_TRUE(cli::parse_wmi("zzz", &codes));
    EXPECT_EQ(codes, "ZZZ");
    EXPECT_TRUE(cli::parse_wmi("ZZZ,zz9abc,1HG", &codes));
    EXPECT_EQ(codes, "ZZZ,ZZ9ABC,1HG");
    for (const char* bad : {"", "ZZ", "ZZZZ", "ZZZZZZZ", "ZZI", "ZOZ", "QZZ",
                            "ZZZ,", ",ZZZ", "ZZZ,,ZZZ", "ZZ-", "ZZZ ZZZ"}) {
        codes = "kept";
        EXPECT_FALSE(cli::parse_wmi(bad, &codes)) << bad;
        EXPECT_EQ(codes, "kept") << "nothing written for " << bad;
    }
}

// With no --variant, put names the variant core gave: the file's profile
// less "user.", cut to the variant id's bound with no '-' at its end
TEST(CliPluginTest, APutsUnnamedVariantIsTheOneCoreAdds)
{
    EXPECT_EQ(cli::variant_slug("user.gauged-extras"), "gauged-extras");
    for (const char* name : {"Gauged extras", "A name longer than twenty-four",
                             "Twenty three characters plus", "x"}) {
        const std::string file =
            std::string("{\"havne_signals\": 1, \"name\": \"") + name +
            "\", \"source\": {\"name\": \"t\"}, \"ecus\": [{\"id\": \"pcm\", "
            "\"rx\": \"7E8\"}], \"signals\": [{\"key\": \"oil\", \"name\": "
            "\"Oil\", \"read\": {\"service\": \"22\", \"number\": \"F40E\"}, "
            "\"field\": {\"byte\": 0}}]}";
        ecuio_signal_file_t* f = nullptr;
        ASSERT_EQ(ecuio_signal_file_load(file.data(), file.size(), &f),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        const std::string mine =
            cli::variant_slug(ecuio_signal_file_profile(f));
        ecuio_signal_file_free(f);
        char* out = nullptr;
        size_t length = 0;
        ASSERT_EQ(ecuio_plugin_put_variant(kPacked, sizeof kPacked - 1, nullptr,
                                           file.data(), file.size(), &out,
                                           &length),
                  ECUIO_OK)
            << name << ": " << ecuio_last_error_detail();
        ecuio_plugin_t* p = nullptr;
        ASSERT_EQ(ecuio_plugin_load(out, length, nullptr, nullptr, &p),
                  ECUIO_OK);
        ecuio_plugin_text_free(out);
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
        ASSERT_EQ(ecuio_plugin_variant(p, 2, id, sizeof id), ECUIO_OK);
        ecuio_plugin_free(p);
        EXPECT_EQ(mine, id) << name;
    }
}

// A fixture line: its place, the value and tolerance, the result, what
// came back (null with nothing) and why when it didn't pass
TEST(CliPluginTest, AFixtureLineSaysWhatCameBack)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    const char file[] =
        R"({"havne_fixtures": 1, "values": [{"variant": "late", "signal": "oil", "value": 2.5}]})";
    ecuio_plugin_value_t v{};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_plugin_values(pk.p, file, sizeof file - 1, &v, 1, &n),
              ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    cli::Line a(buf, sizeof buf);
    cli::fixture_line(a, "f.json", "late", v, cli::FixtureResult::passed, true,
                      2.5, nullptr);
    EXPECT_EQ(text_of(a),
              "{\"type\":\"fixture\",\"file\":\"f.json\",\"line\":1,"
              "\"variant\":\"late\",\"signal\":\"oil\",\"value\":2.5,"
              "\"tolerance\":0.125,\"result\":\"passed\",\"got\":2.5}\n");
    cli::Line b(buf, sizeof buf);
    cli::fixture_line(b, "f.json", "late", v, cli::FixtureResult::not_run,
                      false, 0, "no --on");
    EXPECT_EQ(text_of(b),
              "{\"type\":\"fixture\",\"file\":\"f.json\",\"line\":1,"
              "\"variant\":\"late\",\"signal\":\"oil\",\"value\":2.5,"
              "\"tolerance\":0.125,\"result\":\"not-run\",\"got\":null,"
              "\"why\":\"no --on\"}\n");
    EXPECT_STREQ(cli::fixture_word(cli::FixtureResult::failed), "failed");
}

// The checked line counts each result, names the interface or null, and
// warns of a plugin with no fixtures only; a refusal names its package or
// its file
TEST(CliPluginTest, TheCheckedAndRefusedLinesSayWhatHappened)
{
    cli::FixtureCounts c;
    c.passed = 6;
    c.failed = 1;
    c.files_refused = 1;
    char buf[1024];
    cli::Line a(buf, sizeof buf);
    cli::checked_line(a, c, "vcan0", 2);
    EXPECT_EQ(text_of(a),
              "{\"type\":\"checked\",\"fixtures\":7,\"passed\":6,\"failed\":1,"
              "\"not_run\":0,\"files_refused\":1,\"on\":\"vcan0\"}\n");
    cli::Line b(buf, sizeof buf);
    cli::checked_line(b, cli::FixtureCounts{}, nullptr, 0);
    EXPECT_EQ(text_of(b),
              "{\"type\":\"checked\",\"fixtures\":0,\"passed\":0,\"failed\":0,"
              "\"not_run\":0,\"files_refused\":0,\"on\":null,\"warning\":"
              "\"the plugin has no fixtures: its values are unchecked\"}\n");
    cli::Line r(buf, sizeof buf);
    cli::refused_line(r, "refused", nullptr, 3, ECUIO_ERR_LOAD_FAILED, "why");
    EXPECT_NE(text_of(r).find("\"package\":3,\"code\":7"), std::string::npos);
    cli::Line f(buf, sizeof buf);
    cli::refused_line(f, "fixture_refused", "a.json", 0,
                      ECUIO_ERR_INVALID_ARGUMENT, "why");
    const std::string ft = text_of(f);
    EXPECT_NE(ft.find("\"file\":\"a.json\",\"code\":11"), std::string::npos);
    EXPECT_EQ(ft.find("package"), std::string::npos);
}

// A plugin whose data files use "gauge" is warned of a requires.core
// below 0.7.2 and of no other; one with no gauge never is
TEST(CliPluginTest, AGaugeWantsTheCoreThatReadsIt)
{
    struct Version {
        uint16_t part[3];
    };
    const auto older = [](Version v, bool uses_gauge) {
        return cli::gauge_needs_newer_core(v.part, uses_gauge);
    };
    EXPECT_FALSE(older({{0, 7, 2}}, true));
    EXPECT_FALSE(older({{0, 7, 3}}, true));
    EXPECT_FALSE(older({{0, 8, 0}}, true));
    EXPECT_FALSE(older({{1, 0, 0}}, true));
    EXPECT_TRUE(older({{0, 7, 1}}, true));
    EXPECT_TRUE(older({{0, 6, 9}}, true));
    EXPECT_TRUE(older({{0, 1, 0}}, true));
    EXPECT_FALSE(older({{0, 1, 0}}, false));

    char buf[1024];
    cli::Line l(buf, sizeof buf);
    const uint16_t declared[3] = {0, 7, 1};
    cli::requires_core_line(l, declared);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"requires_core\",\"declared\":\"0.7.1\",\"needs\":"
              "\"0.7.2\",\"warning\":\"the data files use \\\"gauge\\\", which "
              "core reads from 0.7.2 on: raise the manifest's \\\"requires\\\" "
              "core to 0.7.2, so an older Havne names that version instead of "
              "refusing a signal file\"}\n");
}
