// File/layer: tests/test_signal_file_gauge.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest (test_signals)

#include "test_signal_file_support.h"

namespace {

/* An 8-bit signal in deg C reading -40 to 215, with the JSON after "gauge" */
std::string gauged(const std::string& gauge)
{
    return one(
        R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0},"offset":-40,"unit":"degC","gauge":)" +
        gauge + "}");
}

const ecuio_signal_gauge_t* gauge_of(const File& f, const char* key)
{
    return ecuio_signal_file_gauge(f.get(),
                                   ecuio_signal_file_find(f.get(), key));
}

} // namespace

// A field must end within the 4096 bytes a reply holds; one ending at the
// last byte loads
TEST(SignalFileTest, AFieldEndingPastTheReplyIsRefused)
{
    const auto signal = [](const std::string& kind, const std::string& field) {
        return one(R"({"key":"a","name":"A",)" + kind +
                   R"("read":{"service":"22","number":"D100"},"field":)" +
                   field + "}");
    };
    const std::string enumeration = R"("kind":"enum","values":{"1":"One"},)";
    const std::string text = R"("kind":"text",)";
    for (const std::string& fits :
         {signal("", R"({"byte":4092})"),
          signal("", R"({"byte":4091,"bits":16})"),
          signal("", R"({"byte":4092,"bits":4,"bit":4,"order":"le"})"),
          signal(enumeration, R"({"byte":4092})"),
          signal(text, R"({"byte":4090,"bytes":3})")}) {
        EXPECT_NE(load(fits), nullptr) << ecuio_last_error_detail() << "\n"
                                       << fits;
    }
    for (const std::string& past :
         {signal("", R"({"byte":4092,"bits":16})"),
          signal("", R"({"byte":4092,"bits":4,"bit":6,"order":"le"})"),
          signal(enumeration, R"({"byte":4092,"bits":16})"),
          signal(text, R"({"byte":4090,"bytes":4})")}) {
        const std::string why = refusal(past);
        EXPECT_NE(
            why.find(
                "signal 'a''s field ends past the 4096 bytes a reply holds"),
            std::string::npos)
            << why;
    }
}

// A def's key is the file's own name for it, the inverse of find: each
// own def names its key, and a base set's def (extends), another set's
// def with the same ID, a copy, and NULLs give ""
TEST(SignalFileTest, EachOwnDefNamesItsKeyAndNoOtherDefDoes)
{
    File f = load(kExample);
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    ASSERT_EQ(set->count, obd2->count + 3) << "the file's Boost stands for obd2's";
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t* def = &set->defs[i];
        const char* key = ecuio_signal_file_key(f.get(), def);
        ASSERT_NE(key, nullptr) << def->name;
        if (i < obd2->count - 1) {
            EXPECT_STREQ(key, "") << "the base's " << def->name;
            EXPECT_STREQ(ecuio_signal_file_key(f.get(), &obd2->defs[i]), "")
                << "obd2's own " << def->name;
        } else {
            EXPECT_NE(std::string(key), "") << def->name;
            EXPECT_EQ(ecuio_signal_file_find(f.get(), key), def) << key;
        }
    }
    EXPECT_STREQ(
        ecuio_signal_file_key(f.get(), ecuio_signal_file_find(f.get(), "tft")),
        "tft");
    EXPECT_STREQ(ecuio_signal_file_key(
                     f.get(), ecuio_signal_file_find(f.get(), "boost")),
                 "boost")
        << "a computed def has its key too";

    /* By address, never by ID: a copy and another file's def aren't it */
    const ecuio_signal_def_t copy = *ecuio_signal_file_find(f.get(), "tft");
    EXPECT_STREQ(ecuio_signal_file_key(f.get(), &copy), "");
    File other = load(kExample);
    ASSERT_NE(other, nullptr);
    EXPECT_STREQ(ecuio_signal_file_key(
                     f.get(), ecuio_signal_file_find(other.get(), "tft")),
                 "");

    EXPECT_STREQ(ecuio_signal_file_key(nullptr, &copy), "");
    EXPECT_STREQ(ecuio_signal_file_key(f.get(), nullptr), "");

    /* A file that extends nothing: every def is its own, each keyed */
    File alone = load(one(kNumber));
    ASSERT_NE(alone, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* s = ecuio_signal_file_set(alone.get());
    ASSERT_EQ(s->count, 1u);
    EXPECT_STREQ(ecuio_signal_file_key(alone.get(), &s->defs[0]), "a");
}

TEST(SignalFileTest, AGaugeIsKeptAsTheFileWroteIt)
{
    File f = load(
        gauged(R"({"min":40.5,"max":130,"warn_low":60,"warn_high":110.25})"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_gauge_t* g = gauge_of(f, "a");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->min, 40.5);
    EXPECT_EQ(g->max, 130.0);
    EXPECT_EQ(g->warn_low, 60.0);
    EXPECT_EQ(g->warn_high, 110.25);
    EXPECT_EQ(g->flags, ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH);
    EXPECT_EQ(g->reserved[0] | g->reserved[1] | g->reserved[2], 0u);
}

TEST(SignalFileTest, AGaugesLevelsAreOptionalAndEachHasItsFlag)
{
    File none = load(gauged(R"({"min":0,"max":100})"));
    ASSERT_TRUE(none) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(none, "a"), nullptr);
    EXPECT_EQ(gauge_of(none, "a")->flags, 0u);
    EXPECT_EQ(gauge_of(none, "a")->warn_low, 0.0);
    EXPECT_EQ(gauge_of(none, "a")->warn_high, 0.0);

    File low = load(gauged(R"({"min":0,"max":100,"warn_low":10})"));
    ASSERT_TRUE(low) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(low, "a"), nullptr);
    EXPECT_EQ(gauge_of(low, "a")->flags, ECUIO_GAUGE_WARN_LOW);
    EXPECT_EQ(gauge_of(low, "a")->warn_low, 10.0);
    EXPECT_EQ(gauge_of(low, "a")->warn_high, 0.0);

    File high = load(gauged(R"({"min":0,"max":100,"warn_high":90})"));
    ASSERT_TRUE(high) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(high, "a"), nullptr);
    EXPECT_EQ(gauge_of(high, "a")->flags, ECUIO_GAUGE_WARN_HIGH);
    EXPECT_EQ(gauge_of(high, "a")->warn_high, 90.0);
    EXPECT_EQ(gauge_of(high, "a")->warn_low, 0.0);
}

TEST(SignalFileTest, AGaugeTakesWhatADialCanShow)
{
    const char* const kept[] = {
        R"({"min":0,"max":100,"warn_low":0,"warn_high":100})", /* the ends */
        R"({"min":-100,"max":500})", /* wider than the field */
        R"({"min":-100,"max":-40})", /* meets the field's lowest */
        R"({"min":215,"max":300})",  /* meets its highest */
        R"({"min":200,"max":300})",  /* overlaps it */
        R"({"min":0,"max":100,"notes":"from the gauge cluster","x-tool":[1]})",
    };
    for (const char* gauge : kept) {
        File f = load(gauged(gauge));
        EXPECT_TRUE(f) << gauge << ": " << ecuio_last_error_detail();
    }
}

/* The field's reach follows its scale's sign and its own: a negative
 * scale reads its span backwards (8 bits at -0.5 from 100: -27.5 to 100)
 * and a signed field reads below zero (8 signed bits: -128 to 127) */
TEST(SignalFileTest, AFieldsReachFollowsItsScaleAndItsSign)
{
    struct Case {
        const char* field;
        const char* gauge;
        bool kept;
    };
    const char* const backwards =
        R"("field":{"byte":0},"scale":-0.5,"offset":100)";
    const char* const signed8 = R"("field":{"byte":0,"signed":true})";
    const Case cases[] = {
        {backwards, R"({"min":-27.5,"max":-10})", true},  /* its lowest */
        {backwards, R"({"min":100,"max":200})", true},    /* its highest */
        {backwards, R"({"min":-40,"max":0})", true},      /* overlapping */
        {backwards, R"({"min":-60,"max":-28})", false},   /* under it */
        {backwards, R"({"min":100.5,"max":200})", false}, /* over it */
        {signed8, R"({"min":-128,"max":-100})", true},    /* its lowest */
        {signed8, R"({"min":120,"max":200})", true},      /* overlapping */
        {signed8, R"({"min":-200,"max":-129})", false},   /* under it */
        {signed8, R"({"min":128,"max":200})", false},     /* over it */
    };
    for (const Case& k : cases) {
        File f = load(one(std::string(R"({"key":"a","name":"A","read":)"
                                      R"({"service":"22","number":"D100"},)") +
                          k.field + R"(,"gauge":)" + k.gauge + "}"));
        EXPECT_EQ(static_cast<bool>(f), k.kept)
            << k.field << " " << k.gauge << ": " << ecuio_last_error_detail();
        if (!k.kept) {
            EXPECT_NE(std::string(ecuio_last_error_detail())
                          .find("but its field reads only"),
                      std::string::npos)
                << ecuio_last_error_detail();
        }
    }
}

TEST(SignalFileTest, EveryGaugeRefusalSaysWhy)
{
    struct Case {
        const char* gauge;
        const char* words;
    };
    const Case cases[] = {
        {"[]", "signal 'a''s gauge is a list, not an object"},
        {R"({"max":1})", "gauge needs \"min\" and \"max\""},
        {R"({"min":0})", "gauge needs \"min\" and \"max\""},
        {R"({"min":"0","max":1})", "gauge min is a string, not a number"},
        {R"({"min":0,"max":null})", "gauge max is"},
        {R"({"min":0,"max":1,"warn_low":true})", "gauge warn_low is"},
        {R"({"min":0,"max":1,"warn_high":[]})", "gauge warn_high is a list"},
        {R"({"min":0,"max":1,"high":1})",
         "unknown key 'high' in signal 'a''s gauge"},
        {R"({"min":5,"max":5})", "gauge max 5 isn't above its min 5"},
        {R"({"min":9,"max":5})", "gauge max 5 isn't above its min 9"},
        {R"({"min":0,"max":10,"warn_low":-0.5})",
         "gauge warn_low -0.5 is outside its range, 0 to 10"},
        {R"({"min":0,"max":10,"warn_low":10.5})",
         "gauge warn_low 10.5 is outside"},
        {R"({"min":0,"max":10,"warn_high":-0.5})",
         "gauge warn_high -0.5 is outside"},
        {R"({"min":0,"max":10,"warn_high":10.5})",
         "gauge warn_high 10.5 is outside"},
        {R"({"min":0,"max":10,"warn_low":5,"warn_high":5})",
         "gauge warn_high 5 isn't above its warn_low 5"},
        {R"({"min":0,"max":10,"warn_low":6,"warn_high":5})",
         "gauge warn_high 5 isn't above its warn_low 6"},
        {R"({"min":250,"max":400})",
         "gauge runs 250 to 400, but its field reads only -40 to 215: "
         "is the gauge in another unit than 'degC'?"},
        {R"({"min":-100,"max":-41})", "but its field reads only -40 to 215"},
    };
    for (const Case& c : cases) {
        const std::string words = refusal(gauged(c.gauge));
        EXPECT_NE(words.find("line 1, column"), std::string::npos) << words;
        EXPECT_NE(words.find(c.words), std::string::npos)
            << c.gauge << ": " << words;
    }
}

TEST(SignalFileTest, ARangeTheFieldCantReachSaysSoWithNoUnitToName)
{
    const std::string words = refusal(
        one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
            R"("field":{"byte":0},"gauge":{"min":300,"max":400}})"));
    EXPECT_NE(words.find("in another unit than the signal's?"),
              std::string::npos)
        << words;
}

TEST(SignalFileTest, ANumberPastTheDoubleRangeIsNoGauge)
{
    EXPECT_NE(refusal(gauged(R"({"min":0,"max":1e999})")).find("line 1"),
              std::string::npos);
    EXPECT_NE(refusal(gauged(R"({"min":-1e999,"max":0})")).find("line 1"),
              std::string::npos);
    EXPECT_NE(refusal(gauged(R"({"min":0,"max":NaN})")).find("line 1"),
              std::string::npos);
}

TEST(SignalFileTest, OnlyNumbersAndComputedSignalsTakeAGauge)
{
    const char* const kGauge = R"("gauge":{"min":0,"max":1})";
    const std::string read =
        R"("read":{"service":"22","number":"D100"},"field":{"byte":0},)";
    for (const std::string& s :
         {R"({"key":"a","name":"A",)" + read + R"("kind":"text",)" + kGauge +
              "}",
          R"({"key":"a","name":"A",)" + read + R"("kind":"hex",)" + kGauge +
              "}",
          R"({"key":"a","name":"A",)" + read +
              R"("kind":"enum","values":{"1":"On"},)" + kGauge + "}"}) {
        const std::string words = refusal(one(s));
        EXPECT_NE(words.find("unknown key 'gauge' in signal 'a'"),
                  std::string::npos)
            << words;
    }

    File f =
        load(one(std::string(kNumber) + "," +
                 R"({"key":"b","name":"B","kind":"computed","expr":"a * 2",)" +
                 R"("gauge":{"min":-5,"max":5000,"warn_high":4000}})"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(f, "b"), nullptr);
    EXPECT_EQ(gauge_of(f, "b")->min, -5.0)
        << "a computed signal has no field to judge";
    EXPECT_EQ(gauge_of(f, "b")->warn_high, 4000.0);
    EXPECT_EQ(gauge_of(f, "a"), nullptr);
}

TEST(SignalFileTest, AGaugeIsTheDefsOwnAndNoOtherDefs)
{
    File f = load(
        R"json({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2",
      "signals":[
        {"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},
         "gauge":{"min":0,"max":1}},
        {"key":"b","name":"B","read":{"service":"22","number":"D101"},"field":{"byte":0},
         "gauge":{"min":2,"max":3}},
        {"key":"c","name":"C","read":{"service":"22","number":"D102"},"field":{"byte":0}}
      ]})json");
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(f, "a"), nullptr);
    ASSERT_NE(gauge_of(f, "b"), nullptr);
    EXPECT_EQ(gauge_of(f, "a")->min, 0.0);
    EXPECT_EQ(gauge_of(f, "b")->min, 2.0) << "each def's own";
    EXPECT_EQ(gauge_of(f, "c"), nullptr);

    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < obd2->count; ++i) {
        EXPECT_EQ(ecuio_signal_file_gauge(f.get(), &set->defs[i]), nullptr)
            << "the base set's " << set->defs[i].name;
        EXPECT_EQ(ecuio_signal_file_gauge(f.get(), &obd2->defs[i]), nullptr);
    }
    ecuio_signal_def_t copy = *ecuio_signal_file_find(f.get(), "a");
    EXPECT_EQ(ecuio_signal_file_gauge(f.get(), &copy), nullptr)
        << "a copy isn't the file's";
    File same = load(
        R"({"havne_signals":1,"name":"T","source":{"name":"t"},"ecus":[{"id":"p","rx":"7E8"}],
      "signals":[{"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},
      "gauge":{"min":0,"max":1}}]})");
    ASSERT_TRUE(same) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_gauge(f.get(),
                                      ecuio_signal_file_find(same.get(), "a")),
              nullptr)
        << "another file's def";
    EXPECT_EQ(ecuio_signal_file_gauge(nullptr, &copy), nullptr);
    EXPECT_EQ(ecuio_signal_file_gauge(f.get(), nullptr), nullptr);
}

TEST(SignalFileTest, AGaugeLeavesTheDefAndTheRestOfTheSetAlone)
{
    File with = load(gauged(R"({"min":0,"max":100,"warn_high":90})"));
    File without = load(
        one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
            R"("field":{"byte":0},"offset":-40,"unit":"degC"})"));
    ASSERT_TRUE(with) << ecuio_last_error_detail();
    ASSERT_TRUE(without) << ecuio_last_error_detail();
    const ecuio_signal_set_t* a = ecuio_signal_file_set(with.get());
    const ecuio_signal_set_t* b = ecuio_signal_file_set(without.get());
    ASSERT_EQ(a->count, b->count);
    EXPECT_EQ(std::memcmp(a->defs, b->defs, sizeof *a->defs * a->count), 0)
        << "a recording's decode never depends on a gauge";
}

TEST(SignalFileTest, AToolsOwnXGaugeStaysIgnored)
{
    File f = load(one(
        R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0},"x-gauge":{"min":9,"max":1,"warn_low":"no"}})"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    EXPECT_EQ(gauge_of(f, "a"), nullptr);
}

namespace {

/* A file with a source block of the given members */
std::string sourced(const std::string& members)
{
    return R"({"havne_signals":1,"name":"Sourced","source":{)" + members +
           R"(},"ecus":[{"id":"pcm","rx":"7E8"}],"signals":[{"key":"a",)"
           R"("name":"A",)"
           R"("read":{"service":"22","number":"D100"},"field":{"byte":0}}]})";
}

ecuio_signal_source_t source_of(const File& f)
{
    ecuio_signal_source_t s{};
    s.struct_size = sizeof s;
    EXPECT_EQ(ecuio_signal_file_source(f.get(), &s), ECUIO_OK);
    return s;
}

} // namespace

// The source as the file wrote it: every text, "" for one not given,
// the rest of each field zero, and struct_size core's own (what it filled)
TEST(SignalFileTest, ItsSourceIsGivenAsTheFileWroteIt)
{
    const std::string name(ECUIO_SOURCE_TEXT_MAX, 'n');
    File all = load(sourced(R"("name":")" + name +
                            R"(","url":"https://example.com/set",)"
                            R"("commit":"0123abc","license":"MIT",)"
                            R"("modified":"converted by hand")"));
    ASSERT_TRUE(all) << ecuio_last_error_detail();
    struct Bigger {
        ecuio_signal_source_t s;
        uint32_t after;
    } big;
    std::memset(&big, 0x5A, sizeof big);
    big.s.struct_size = sizeof big;
    ASSERT_EQ(ecuio_signal_file_source(all.get(), &big.s), ECUIO_OK);
    EXPECT_EQ(big.s.struct_size, sizeof(ecuio_signal_source_t))
        << "core's own size, what it filled";
    EXPECT_EQ(big.after, 0x5A5A5A5Au) << "nothing past the struct written";
    EXPECT_EQ(std::string(big.s.name), name) << "a text at its bound whole";
    EXPECT_STREQ(big.s.url, "https://example.com/set");
    EXPECT_STREQ(big.s.commit, "0123abc");
    EXPECT_STREQ(big.s.license, "MIT");
    EXPECT_STREQ(big.s.modified, "converted by hand");
    EXPECT_EQ(big.s.flags, 0u);
    for (uint32_t r : big.s.reserved) {
        EXPECT_EQ(r, 0u);
    }
    for (size_t i = std::strlen(big.s.url); i < ECUIO_SOURCE_TEXT_LEN; ++i) {
        ASSERT_EQ(big.s.url[i], '\0') << "zero past the text, at " << i;
    }

    File one = load(sourced(R"("name":"Only a name")"));
    ASSERT_TRUE(one) << ecuio_last_error_detail();
    const ecuio_signal_source_t s = source_of(one);
    EXPECT_STREQ(s.name, "Only a name");
    EXPECT_STREQ(s.url, "");
    EXPECT_STREQ(s.commit, "");
    EXPECT_STREQ(s.license, "");
    EXPECT_STREQ(s.modified, "");
}

TEST(SignalFileTest, ItsSourceRefusesWhatItCantFill)
{
    File f = load(sourced(R"("name":"S")"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    ecuio_signal_source_t s{};
    s.struct_size = sizeof s;
    /* Each refusal says why: no earlier call's words are left to read */
    const auto stale = [] {
        ecuio_signal_file_t* bad = nullptr;
        EXPECT_EQ(ecuio_signal_file_load("{", 1, &bad),
                  ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_STRNE(ecuio_last_error_detail(), "");
    };
    stale();
    EXPECT_EQ(ecuio_signal_file_source(nullptr, &s), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the source needs a file and a struct to fill");
    stale();
    EXPECT_EQ(ecuio_signal_file_source(f.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the source needs a file and a struct to fill");
    std::memset(&s, 0x5A, sizeof s);
    s.struct_size = sizeof s - 1;
    stale();
    EXPECT_EQ(ecuio_signal_file_source(f.get(), &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the source's struct_size is " + std::to_string(sizeof s - 1) +
                  ", under its " + std::to_string(sizeof s) + " bytes");
    EXPECT_EQ(static_cast<unsigned char>(s.name[0]), 0x5Au) << "untouched";
    EXPECT_EQ(s.struct_size, sizeof s - 1) << "untouched";
}

// Creative Commons' share-alike licenses, as SPDX writes them in any
// case or with spaces; any other license, or a name inside a longer
// word, isn't
TEST(SignalFileTest, AShareAlikeLicenseIsKnownInEachSpelling)
{
    const struct {
        const char* license;
        bool share_alike;
    } cases[] = {
        {"CC-BY-SA-4.0", true},
        {"cc-by-sa-3.0", true},
        {"CC BY-SA 4.0", true},
        {"CC-BY-SA", true},
        {"CC-BY-NC-SA-4.0", true},
        {"MIT OR CC-BY-SA-4.0", true},
        {"(CC-BY-SA-2.0-UK)", true},
        {"CC-BY-SA-4.0+", true},
        {"CC-BY-4.0", false},
        {"CC-BY-NC-4.0", false},
        {"MIT", false},
        {"XCC-BY-SA-4.0", false},
        {"1CC-BY-SA", false},
        {"NOT-CC-BY-SA-4.0", false},
        {"CC-BY-SAX", false},
        {"CC-BY-SA4", false},
    };
    for (const auto& c : cases) {
        File f = load(sourced(std::string(R"("name":"S","license":")") +
                              c.license + "\""));
        ASSERT_TRUE(f) << ecuio_last_error_detail();
        EXPECT_EQ(source_of(f).flags,
                  c.share_alike ? ECUIO_SOURCE_SHARE_ALIKE : 0u)
            << c.license;
    }
    File none = load(sourced(R"("name":"S")"));
    ASSERT_TRUE(none);
    EXPECT_EQ(source_of(none).flags, 0u) << "no license, none";
}

// A byte past ASCII is no letter, whatever the locale: a Qt or Python
// host sets one, and in Latin-1 the micro sign's 0xB5 is a letter
TEST(SignalFileTest, AShareAlikeLicenseIsReadAsAsciiInAnyLocale)
{
    const auto flags_of = [](const char* license) {
        File f = load(
            sourced(std::string(R"("name":"S","license":")") + license + "\""));
        EXPECT_TRUE(f) << ecuio_last_error_detail();
        return f ? source_of(f).flags : 0xFFu;
    };
    const char* micro_before = "\xC2\xB5"
                               "CC-BY-SA-4.0";
    const char* micro_inside = "CC-BY-SA\xC2\xB5";
    EXPECT_EQ(flags_of(micro_before), ECUIO_SOURCE_SHARE_ALIKE);
    EXPECT_EQ(flags_of(micro_inside), ECUIO_SOURCE_SHARE_ALIKE);

    const char* before = std::setlocale(LC_CTYPE, nullptr);
    const std::string saved = before != nullptr ? before : "C";
    if (std::setlocale(LC_CTYPE, ".1252") != nullptr ||
        std::setlocale(LC_CTYPE, "en_US.ISO-8859-1") != nullptr ||
        std::setlocale(LC_CTYPE, "de_DE.ISO-8859-1") != nullptr) {
        const bool letter = std::isalnum(0xB5) != 0;
        const uint32_t first = flags_of(micro_before);
        const uint32_t last = flags_of(micro_inside);
        std::setlocale(LC_CTYPE, saved.c_str());
        EXPECT_TRUE(letter) << "the case needs 0xB5 a letter in this locale";
        EXPECT_EQ(first, ECUIO_SOURCE_SHARE_ALIKE) << "under Latin-1";
        EXPECT_EQ(last, ECUIO_SOURCE_SHARE_ALIKE) << "under Latin-1";
    }
}

// A file's text is the bytes it was read from, spacing and line ends
// as they were, borrowed for the file's life
TEST(SignalFileTest, ItsTextIsTheBytesItWasReadFrom)
{
    const std::string bytes = "{ \"havne_signals\":1,\r\n  \"name\":\"T\","
                              "\"source\":{\"name\":\"S\"},\r\n\"ecus\":[{"
                              "\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":["
                              "{\"key\":\"a\",\"name\":\"A\",\"read\":{"
                              "\"service\":\"22\",\"number\":\"D100\"},"
                              "\"field\":{\"byte\":0}}]  }\n";
    File f = load(bytes);
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    size_t length = 99;
    const char* text = ecuio_signal_file_text(f.get(), &length);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(length, bytes.size());
    EXPECT_EQ(std::string(text, length), bytes);
    EXPECT_EQ(text[length], '\0');
    EXPECT_EQ(ecuio_signal_file_text(f.get(), nullptr), text)
        << "the same text each call, a NULL length skipped";

    length = 99;
    EXPECT_STREQ(ecuio_signal_file_text(nullptr, &length), "");
    EXPECT_EQ(length, 0u);
}
