// File/layer: tests/test_signal_merged_text.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergedTextTest (test_signals)
//
// A merged set's written form (ADR-0065 section 9), read by the loader
// from texts written by hand, apart from the merge that writes one: the
// parts and their sources, each def's part, the keys a signal replaced,
// the profile's name and every rule of "parts", "part" and "replaced".

#include "test_signal_file_support.h"

#include <ecuio/plugin.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string fixture()
{
    std::ifstream in(std::string(ECUIO_TEST_FIXTURES) +
                         "/signal-files/merged-two-parts.json",
                     std::ios::binary);
    EXPECT_TRUE(in.good());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/* A part's entry with no plugin */
std::string part(const std::string& profile, unsigned format = 3,
                 const std::string& more = "")
{
    return "{\"profile\":\"" + profile + "\",\"name\":\"P " + profile +
           "\",\"format\":" + std::to_string(format) +
           R"(,"source":{"name":"s"})" + more + "}";
}

/* A number read by DID from pcm, of the given part; more goes into it */
std::string sig(const std::string& key, const std::string& number,
                const std::string& more = R"(,"part":0)")
{
    return "{\"key\":\"" + key + "\",\"name\":\"N " + key +
           "\",\"read\":{\"service\":\"22\",\"number\":\"" + number +
           "\"},\"field\":{\"byte\":0}" + more + "}";
}

/* A merged text of the given parts and signals; head is any member more,
 * ending in a comma */
std::string merged(const std::string& parts, const std::string& signals,
                   const std::string& head = "", unsigned format = 3,
                   const std::string& source = R"({"name":"A merge"})")
{
    return "{\"havne_signals\":" + std::to_string(format) +
           R"(,"name":"Together","source":)" + source + "," + head +
           "\"parts\":[" + parts +
           R"(],"ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)" + signals + "]}";
}

const std::string kTwo = part("user.a") + "," + part("user.b");

ecuio_signal_part_t part_at(const ecuio_signal_file_t* f, uint32_t index)
{
    ecuio_signal_part_t p;
    std::memset(&p, 0xA5, sizeof p);
    p.struct_size = sizeof p;
    EXPECT_EQ(ecuio_signal_file_part(f, index, &p), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(p.struct_size, sizeof p);
    for (uint32_t r : p.reserved) {
        EXPECT_EQ(r, 0u);
    }
    return p;
}

ecuio_signal_source_t source_at(const ecuio_signal_file_t* f, uint32_t index)
{
    ecuio_signal_source_t s{};
    s.struct_size = sizeof s;
    EXPECT_EQ(ecuio_signal_file_part_source(f, index, &s), ECUIO_OK)
        << ecuio_last_error_detail();
    return s;
}

std::vector<std::string> replaced_of(const ecuio_signal_file_t* f,
                                     const ecuio_signal_def_t* def)
{
    uint32_t count = 99;
    EXPECT_EQ(ecuio_signal_file_replaced_keys(f, def, nullptr, 0, &count),
              ECUIO_OK);
    std::vector<ecuio_signal_key_t> keys(count);
    uint32_t again = 0;
    EXPECT_EQ(
        ecuio_signal_file_replaced_keys(f, def, keys.data(), count, &again),
        ECUIO_OK);
    EXPECT_EQ(again, count);
    std::vector<std::string> out;
    for (const ecuio_signal_key_t& k : keys) {
        out.emplace_back(k.text);
    }
    return out;
}

} // namespace

TEST(MergedTextTest, AMergedTextReadsBackItsPartsAndKeys)
{
    File f = load(fixture());
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_format(f.get()), 3u);
    EXPECT_STREQ(ecuio_signal_file_name(f.get()), "2 parts read together");
    /* sha256("obd2\nplugin.example-import.base\nuser.my-car-signals\n") */
    EXPECT_STREQ(ecuio_signal_file_profile(f.get()), "merged.42999fb6840c3b07");
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    ASSERT_EQ(set->count, obd2->count + 5);
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);

    ASSERT_EQ(ecuio_signal_file_part_count(f.get()), 2u);
    const ecuio_signal_part_t import = part_at(f.get(), 0);
    EXPECT_EQ(import.flags, ECUIO_PART_PLUGIN);
    EXPECT_EQ(import.format, 2u);
    EXPECT_EQ(import.signal_count, 3u) << "gear, gear_raw and line_p";
    EXPECT_EQ(import.module_count, 2u) << "tcm and pcm";
    EXPECT_STREQ(import.profile, "plugin.example-import.base");
    EXPECT_STREQ(import.name, "Example import");
    EXPECT_STREQ(import.plugin_id, "example-import");
    EXPECT_STREQ(import.plugin_version, "1.2.0");
    EXPECT_STREQ(import.plugin_variant, "base");
    const ecuio_signal_part_t mine = part_at(f.get(), 1);
    EXPECT_EQ(mine.flags, 0u) << "a file of its own, no plugin's";
    EXPECT_EQ(mine.format, 3u);
    EXPECT_EQ(mine.signal_count, 2u) << "oil_temp and the computed oil_margin";
    EXPECT_EQ(mine.module_count, 1u) << "a computed signal reads no module";
    EXPECT_STREQ(mine.profile, "user.my-car-signals");
    EXPECT_STREQ(mine.name, "My car signals");
    EXPECT_STREQ(mine.plugin_id, "");
    EXPECT_STREQ(mine.plugin_version, "");
    EXPECT_STREQ(mine.plugin_variant, "");

    /* Each part keeps its source and its license; the file's own says
     * only that it is a merge, and is never share-alike */
    const ecuio_signal_source_t from = source_at(f.get(), 0);
    EXPECT_STREQ(from.name, "A made-up import");
    EXPECT_STREQ(from.url, "https://example.org/signals");
    EXPECT_STREQ(from.license, "CC-BY-SA-4.0");
    EXPECT_EQ(from.flags, ECUIO_SOURCE_SHARE_ALIKE);
    const ecuio_signal_source_t hand = source_at(f.get(), 1);
    EXPECT_STREQ(hand.name, "Written by hand");
    EXPECT_STREQ(hand.license, "");
    EXPECT_EQ(hand.flags, 0u);
    ecuio_signal_source_t own{};
    own.struct_size = sizeof own;
    ASSERT_EQ(ecuio_signal_file_source(f.get(), &own), ECUIO_OK);
    EXPECT_STREQ(own.name, "A merge of 2 parts");
    EXPECT_EQ(own.flags, ECUIO_SOURCE_MERGED);

    /* A def's part: the replacing signal's is its own, a base def's none */
    const ecuio_signal_def_t* oil = ecuio_signal_file_find(f.get(), "oil_temp");
    const ecuio_signal_def_t* gear = ecuio_signal_file_find(f.get(), "gear");
    ASSERT_NE(oil, nullptr);
    ASSERT_NE(gear, nullptr);
    EXPECT_EQ(ecuio_signal_file_def_part(f.get(), oil), 1u);
    EXPECT_EQ(ecuio_signal_file_def_part(f.get(), gear), 0u);
    EXPECT_EQ(ecuio_signal_file_def_part(f.get(), &set->defs[0]),
              ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(oil->id, ECUIO_USER_SIG_ID_BASE);
    EXPECT_EQ(gear->id, ECUIO_USER_SIG_ID_BASE + 1);

    /* The key it replaced finds it; its own key is still its key */
    EXPECT_EQ(ecuio_signal_file_find(f.get(), "oil_t"), oil);
    EXPECT_STREQ(ecuio_signal_file_key(f.get(), oil), "oil_temp");
    EXPECT_EQ(replaced_of(f.get(), oil), (std::vector<std::string>{"oil_t"}));
    EXPECT_TRUE(replaced_of(f.get(), gear).empty());
    EXPECT_TRUE(replaced_of(f.get(), &set->defs[0]).empty());

    /* An expression written against the replaced key reads what stands */
    const ecuio_signal_def_t* margin =
        ecuio_signal_file_find(f.get(), "oil_margin");
    ASSERT_NE(margin, nullptr);
    const ecuio_computed_t& p = set->computed[margin->table_first];
    ASSERT_EQ(p.input_count, 1);
    EXPECT_EQ(p.input_ids[0], oil->id);
    /* As does one checked against the file */
    ecuio_expr_check_t check;
    std::memset(&check, 0, sizeof check);
    check.struct_size = sizeof check;
    const std::string expr = "oil_t * 2";
    ASSERT_EQ(ecuio_signal_file_check_expr(f.get(), nullptr, expr.data(),
                                           expr.size(), &check),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(check.program.input_count, 1);
    EXPECT_EQ(check.program.input_ids[0], oil->id);

    /* A module's entry names the part whose id stands for it */
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    ASSERT_EQ(ecuio_signal_file_module(f.get(), 1, &m), ECUIO_OK);
    EXPECT_STREQ(m.id, "tcm");
    EXPECT_STREQ(m.name, "Transmission");
    EXPECT_EQ(m.part, 0u);
}

TEST(MergedTextTest, TheProfileIsTheHashOfTheBaseAndThePartsInOrder)
{
    const std::string a_then_b = sig("x", "D100");
    const struct {
        std::string json;
        const char* profile; /* each worked out apart from core */
    } cases[] = {
        /* sha256("user.a\nuser.b\n") */
        {merged(kTwo, a_then_b), "merged.9ff663623bbd2f34"},
        /* sha256("obd2\nuser.a\nuser.b\n") */
        {merged(kTwo, a_then_b, R"("extends":"obd2",)"),
         "merged.5c169dc54bb67570"},
        /* sha256("obd2\nuser.b\nuser.a\n") */
        {merged(part("user.b") + "," + part("user.a"), a_then_b,
                R"("extends":"obd2",)"),
         "merged.8e77046ac51ceaad"},
        /* sha256("user.a\n") */
        {merged(part("user.a"), a_then_b), "merged.01667a2a5c272063"},
    };
    for (const auto& c : cases) {
        File f = load(c.json);
        ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
        EXPECT_STREQ(ecuio_signal_file_profile(f.get()), c.profile);
        /* It registers under the name, as any file's profile does */
        EXPECT_EQ(ecuio_profile_register(ecuio_signal_file_profile(f.get()),
                                         ecuio_signal_file_set(f.get()),
                                         nullptr),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_profile_unregister(ecuio_signal_file_profile(f.get())),
                  ECUIO_OK);
    }
}

TEST(MergedTextTest, EveryRuleOfTheWrittenFormIsRefusedInItsWords)
{
    const std::string x = sig("x", "D100");
    const std::string plugin_of = R"(,"plugin":)";
    std::string many = part("user.p0");
    for (int i = 1; i < 17; ++i) {
        many += "," + part("user.p" + std::to_string(i));
    }
    const struct {
        std::string json;
        const char* words;
    } cases[] = {
        /* "parts" itself */
        {merged(kTwo, x, "", 2),
         "\"parts\" is signal-file format 3's, and this file is format 2"},
        {merged("", x), "\"parts\" lists 0: a merged set has 1 to 16"},
        {merged(many, x), "\"parts\" lists 17: a merged set has 1 to 16"},
        {merged(kTwo, x, R"("needs":["y"],)"), "a merged set has no \"needs\""},
        {merged("7", x), "part 0 is a number, not an object"},
        /* A part */
        {merged(R"({"name":"P","format":3,"source":{"name":"s"}})", x),
         "part 0 needs a \"profile\""},
        {merged(part("User.A"), x),
         "part 0's profile 'User.A' takes 1 to 63 of a-z, 0-9, '.', '_' and "
         "'-'"},
        {merged(part("user.a") + "," + part("user.a"), x),
         "part 1's profile 'user.a' is another part's too"},
        {merged(part("merged.0123456789abcdef"), x),
         "part 0 is a merged set: a merged set's parts are signal files and "
         "plugin variants"},
        {merged(R"({"profile":"user.a","format":3,"source":{"name":"s"}})", x),
         "part 0 needs a \"name\""},
        {merged(
             R"({"profile":"user.a","name":"","format":3,"source":{"name":"s"}})",
             x),
         "part 0's name is 0 bytes; it takes 1 to 63"},
        {merged(R"({"profile":"user.a","name":"P","source":{"name":"s"}})", x),
         "part 0 needs a \"format\""},
        {merged(part("user.a", 0), x),
         "part 0's format takes a whole number from 1 to 3"},
        {merged(part("user.a", 4), x),
         "part 0's format takes a whole number from 1 to 3"},
        {merged(R"({"profile":"user.a","name":"P","format":3})", x),
         "part 0 needs a \"source\""},
        {merged(R"({"profile":"user.a","name":"P","format":3,"source":{}})", x),
         "\"source\" needs a \"name\""},
        {merged(part("user.a", 3, R"(,"owner":"me")"), x),
         "unknown key 'owner' in a part"},
        {merged(part("user.a", 3,
                     plugin_of + R"({"version":"1.0.0","variant":"base"})"),
                x),
         "part 0's plugin needs a \"id\""},
        {merged(part("user.a", 3,
                     plugin_of +
                         R"({"id":"-p","version":"1.0.0","variant":"base"})"),
                x),
         "part 0's plugin's id '-p' is no plugin's id"},
        {merged(
             part("user.a", 3,
                  plugin_of + R"({"id":"p","version":"1.0","variant":"base"})"),
             x),
         "part 0's plugin's version '1.0' isn't three numbers as 1.2.3"},
        {merged(part("user.a", 3,
                     plugin_of +
                         R"({"id":"p","version":"1.0.0","variant":"Base"})"),
                x),
         "part 0's plugin's variant 'Base' is no variant's id"},
        {merged(
             part("user.a", 3,
                  plugin_of +
                      R"({"id":"p","version":"1.0.0","variant":"base","x":1})"),
             x),
         "unknown key 'x' in a part's plugin"},
        /* The file's own source says only that it is a merge */
        {merged(kTwo, x, "", 3,
                R"({"name":"A merge","url":"https://example.org"})"),
         "a merged set's \"source\" holds its \"name\" alone, and this one "
         "has \"url\": each part keeps its own source"},
        {merged(kTwo, x, "", 3,
                R"({"name":"A merge","license":"CC-BY-SA-4.0"})"),
         "and this one has \"license\""},
        /* A signal's part */
        {merged(kTwo, sig("x", "D100", "")),
         "signal 'x' needs a \"part\": each signal of a merged set names the "
         "part it came from"},
        {merged(kTwo, sig("x", "D100", R"(,"part":2)")),
         "signal 'x''s part takes a whole number from 0 to 1"},
        {merged(kTwo, sig("x", "D100", R"(,"part":-1)")),
         "signal 'x''s part takes a whole number from 0 to 1"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0.5)")),
         "signal 'x''s part takes a whole number from 0 to 1"},
        /* The keys it replaced */
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":"y")")),
         "signal 'x''s replaced is a string, not a list"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":["Y"])")),
         "signal 'x''s replaced key 'Y' takes 1 to 31 of a-z, 0-9 and _"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":[7])")),
         "signal 'x''s replaced key is a number, not a string"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":["x"])")),
         "signal 'x''s replaced key 'x' is a key of the file already"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":["y"])") + "," +
                          sig("y", "D101")),
         "signal 'x''s replaced key 'y' is a key of the file already"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":["y","y"])")),
         "signal 'x''s replaced key 'y' is a key of the file already"},
        {merged(kTwo, sig("x", "D100", R"(,"part":0,"replaced":["y"])") + "," +
                          sig("z", "D101", R"(,"part":1,"replaced":["y"])")),
         "signal 'z''s replaced key 'y' is a key of the file already"},
        /* One address has one signal across parts, and in a format 3 part */
        {merged(kTwo,
                sig("x", "D100") + "," + sig("y", "D100", R"(,"part":1)")),
         "signal 'y' reads the address signal 'x' reads (7E8:62D100:3.0:8)"},
        {merged(kTwo, sig("x", "D100") + "," + sig("y", "D100")),
         "signal 'y' reads the address signal 'x' reads (7E8:62D100:3.0:8)"},
    };
    for (const auto& c : cases) {
        const std::string words = refusal(c.json);
        EXPECT_NE(words.find(c.words), std::string::npos)
            << c.words << "\n  got: " << words;
    }

    /* One format 1 or 2 part's own two at one address stay, as alone */
    const std::string old_part = part("user.a", 2) + "," + part("user.b");
    EXPECT_NE(load(merged(old_part, sig("x", "D100") + "," + sig("y", "D100"))),
              nullptr)
        << ecuio_last_error_detail();
    EXPECT_NE(refusal(merged(old_part, sig("x", "D100") + "," +
                                           sig("y", "D100", R"(,"part":1)")))
                  .find("signal 'y' reads the address signal 'x' reads"),
              std::string::npos);
    /* The later of two parts being format 2 keeps no other part's */
    EXPECT_NE(refusal(merged(part("user.a") + "," + part("user.b", 2),
                             sig("x", "D100") + "," +
                                 sig("y", "D100", R"(,"part":1)")))
                  .find("signal 'y' reads the address signal 'x' reads"),
              std::string::npos);
}

// The keys of a merged set are refused in a file that is none, each in
// words that name the merged set
TEST(MergedTextTest, AFileWithNoPartsTakesNoneOfItsKeys)
{
    const std::string head =
        R"({"havne_signals":3,"name":"T","source":{"name":"t"},)";
    const std::string ecus = R"("ecus":[{"id":"pcm","rx":"7E8"}],)";
    const struct {
        std::string json;
        const char* words;
    } cases[] = {
        {head + ecus + "\"signals\":[" + sig("x", "D100") + "]}",
         "signal 'x''s \"part\" is a merged set's: a file with no \"parts\" "
         "takes neither \"part\" nor \"replaced\""},
        {head + ecus + "\"signals\":[" +
             sig("x", "D100", R"(,"replaced":["y"])") + "]}",
         "signal 'x''s \"replaced\" is a merged set's"},
        {head + R"("ecus":[{"id":"pcm","rx":"7E8","part":0}],"signals":[)" +
             sig("x", "D100", "") + "]}",
         "ecu 'pcm''s \"part\" is a merged set's: a file with no \"parts\" "
         "takes none"},
    };
    for (const auto& c : cases) {
        const std::string words = refusal(c.json);
        EXPECT_NE(words.find(c.words), std::string::npos)
            << c.words << "\n  got: " << words;
    }
    /* An ECU's part is one of the parts */
    std::string past = merged(kTwo, sig("x", "D100"));
    past.replace(past.find(R"({"id":"pcm","rx":"7E8"})"), 23,
                 R"({"id":"pcm","rx":"7E8","part":2})");
    EXPECT_NE(
        refusal(past).find("ecu 'pcm''s part takes a whole number from 0 to 1"),
        std::string::npos)
        << ecuio_last_error_detail();

    /* A file that is none has no part, no part for a def, no second key */
    File plain =
        load(head + ecus + "\"signals\":[" + sig("x", "D100", "") + "]}");
    ASSERT_NE(plain, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_part_count(plain.get()), 0u);
    const ecuio_signal_def_t* def = ecuio_signal_file_find(plain.get(), "x");
    EXPECT_EQ(ecuio_signal_file_def_part(plain.get(), def),
              ECUIO_SIGNAL_PART_NONE);
    EXPECT_TRUE(replaced_of(plain.get(), def).empty());
    ecuio_signal_source_t own{};
    own.struct_size = sizeof own;
    ASSERT_EQ(ecuio_signal_file_source(plain.get(), &own), ECUIO_OK);
    EXPECT_EQ(own.flags, 0u);
}

// A max_age on a replaced key is judged by the signal that stands for it
TEST(MergedTextTest, AnAgeBoundOnAReplacedKeyIsJudgedByWhatStands)
{
    const std::string computed_c =
        R"({"key":"c","part":1,"replaced":["old"],"name":"C","kind":"computed",)"
        R"("expr":"x + 1"})";
    const std::string reader =
        R"({"key":"r","part":0,"name":"R","kind":"computed","expr":"old * 2",)"
        R"("max_age":{"old":5}})";
    EXPECT_NE(refusal(merged(kTwo, sig("x", "D100") + "," + computed_c + "," +
                                       reader))
                  .find("signal 'r''s max_age names 'old', a computed signal"),
              std::string::npos)
        << ecuio_last_error_detail();
    /* Over a number it is taken */
    EXPECT_NE(
        load(merged(kTwo, sig("x", "D100", R"(,"part":1,"replaced":["old"])") +
                              "," + reader)),
        nullptr)
        << ecuio_last_error_detail();
}

TEST(MergedTextTest, ThePlainLoadHoldsAMergedTextToAFilesBounds)
{
    /* The fixture, and any small merged text, is a file like another */
    EXPECT_NE(load(fixture()), nullptr) << ecuio_last_error_detail();

    std::string signals;
    for (unsigned i = 0; i <= ECUIO_SIGNAL_FILE_MAX_SIGNALS; ++i) {
        char one[160];
        std::snprintf(one, sizeof one,
                      "%s{\"key\":\"s%u\",\"name\":\"S\",\"part\":0,\"read\":"
                      "{\"service\":\"22\",\"number\":\"%04X\"},\"field\":"
                      "{\"byte\":%u}}",
                      i == 0 ? "" : ",", i, 0xD000u + i / 4, i % 4);
        signals += one;
    }
    const std::string big = merged(kTwo, signals);
    EXPECT_NE(refusal(big).find("\"signals\" lists 1025, more than 1024"),
              std::string::npos);
    ecuio_signal_file_t* as_set = nullptr;
    EXPECT_EQ(ecuio_signal_file_load_with(big.data(), big.size(),
                                          ECUIO_SIGNAL_FILE_LOAD_SET, &as_set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_part_count(as_set), 2u);
    ecuio_signal_file_free(as_set);
}

// Of two at one address, which only a format 1 or 2 part gives, the
// earlier keeps the address key and the later goes by its author's
TEST(SignalKeysTest, AMergedSetsPairAtOneAddressKeepsTheirOrder)
{
    File f = load(fixture());
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    std::vector<ecuio_signal_key_t> keys(set->count);
    uint32_t count = 0;
    ASSERT_EQ(
        ecuio_signal_set_keys(set, f.get(), keys.data(), set->count, &count),
        ECUIO_OK);
    const ecuio_signal_def_t* gear = ecuio_signal_file_find(f.get(), "gear");
    const ecuio_signal_def_t* raw = ecuio_signal_file_find(f.get(), "gear_raw");
    EXPECT_STREQ(keys[gear - set->defs].text, "7E9:624A20:3.0:8");
    EXPECT_STREQ(keys[raw - set->defs].text, "gear_raw");
    /* A computed signal goes by its own key, never one it replaced */
    const ecuio_signal_def_t* oil = ecuio_signal_file_find(f.get(), "oil_t");
    EXPECT_STREQ(keys[oil - set->defs].text, "7E9:624A10:3.0:8");
}

TEST(MergedTextTest, TheGettersFollowTheirContracts)
{
    File f = load(fixture());
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_part_count(nullptr), 0u);

    /* A part, and its source: getters by index over a sized struct */
    ecuio_signal_part_t p;
    std::memset(&p, 0x5A, sizeof p);
    p.struct_size = sizeof p;
    const ecuio_signal_part_t untouched = p;
    EXPECT_EQ(ecuio_signal_file_part(f.get(), 2, &p),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file has 2 parts, and index 2 is none");
    EXPECT_EQ(ecuio_signal_file_part(f.get(), UINT32_MAX, &p),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&p, &untouched, sizeof p), 0);
    EXPECT_EQ(ecuio_signal_file_part(nullptr, 0, &p), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_part(f.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    p.struct_size = sizeof p - 1;
    EXPECT_EQ(ecuio_signal_file_part(f.get(), 0, &p),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the part's struct_size is 247, under its 248 bytes");

    ecuio_signal_source_t s;
    std::memset(&s, 0x5A, sizeof s);
    s.struct_size = sizeof s;
    const ecuio_signal_source_t kept = s;
    EXPECT_EQ(ecuio_signal_file_part_source(f.get(), 2, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&s, &kept, sizeof s), 0);
    EXPECT_EQ(ecuio_signal_file_part_source(nullptr, 0, &s),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_part_source(f.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    s.struct_size = sizeof s - 1;
    EXPECT_EQ(ecuio_signal_file_part_source(f.get(), 0, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    s.struct_size = sizeof s;
    EXPECT_EQ(ecuio_signal_file_part_source(f.get(), 0, &s), ECUIO_OK);
    EXPECT_EQ(s.struct_size, sizeof s);

    /* A def's part: none for NULL, a copy or another set's def */
    const ecuio_signal_def_t* oil = ecuio_signal_file_find(f.get(), "oil_temp");
    ASSERT_NE(oil, nullptr);
    const ecuio_signal_def_t copy = *oil;
    EXPECT_EQ(ecuio_signal_file_def_part(nullptr, oil), ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(ecuio_signal_file_def_part(f.get(), nullptr),
              ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(ecuio_signal_file_def_part(f.get(), &copy),
              ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(
        ecuio_signal_file_def_part(f.get(), &ecuio_obd2_signal_set()->defs[0]),
        ECUIO_SIGNAL_PART_NONE);

    /* The replaced keys: the serial ports' contract */
    uint32_t count = 99;
    ecuio_signal_key_t keys[2];
    std::memset(keys, 0x5A, sizeof keys);
    EXPECT_EQ(ecuio_signal_file_replaced_keys(f.get(), oil, keys, 2, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_STREQ(keys[0].text, "oil_t");
    for (size_t i = 5; i < sizeof keys[0].text; ++i) {
        EXPECT_EQ(keys[0].text[i], '\0') << "zero from the NUL to byte 32";
    }
    EXPECT_EQ(keys[1].text[0], 0x5A);
    EXPECT_EQ(ecuio_signal_file_replaced_keys(f.get(), &copy, keys, 2, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "a copy is no def of the file's";
    EXPECT_EQ(ecuio_signal_file_replaced_keys(nullptr, oil, keys, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_signal_file_replaced_keys(f.get(), nullptr, keys, 2, &count),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_replaced_keys(f.get(), oil, keys, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_replaced_keys(f.get(), oil, nullptr, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);
}

// A merged set is what several plugins read together make, never one to
// import: a plugin's data file, a wrap and a put each refuse it
TEST(MergedTextTest, AMergedSetIsNeverImported)
{
    const std::string text = fixture();
    char* out = reinterpret_cast<char*>(1);
    size_t length = 77;
    EXPECT_EQ(
        ecuio_plugin_wrap(text.data(), text.size(), nullptr, &out, &length),
        ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file is a merged set, which isn't imported: import its "
                 "plugins");

    const std::string plain =
        R"({"havne_signals":1,"name":"Plain","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)" +
        sig("x", "D100", "") + "]}";
    char* packed = nullptr;
    size_t packed_length = 0;
    ASSERT_EQ(ecuio_plugin_wrap(plain.data(), plain.size(), nullptr, &packed,
                                &packed_length),
              ECUIO_OK)
        << ecuio_last_error_detail();
    out = reinterpret_cast<char*>(1);
    EXPECT_EQ(ecuio_plugin_put_variant(packed, packed_length, "more",
                                       text.data(), text.size(), &out, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file is a merged set, which isn't imported: import its "
                 "plugins");
    ecuio_plugin_text_free(packed);

    /* As a plugin's data file */
    const std::string package =
        R"({"havne_plugin":1,"id":"p","name":"P","version":"1.0.0",)"
        R"("requires":{"core":"0.1.0"},"source":{"name":"t"},"tiers":["data"],)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],)"
        R"("variants":[{"id":"base","files":["a.json"]}],"files":{"a.json":)"
        R"({"havne_signals":3,"parts":[],"signals":[)" +
        sig("x", "D100") + "]}}}";
    ecuio_plugin_t* plugin = reinterpret_cast<ecuio_plugin_t*>(1);
    EXPECT_EQ(ecuio_plugin_load(package.data(), package.size(), nullptr,
                                nullptr, &plugin),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(plugin, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail())
                  .find("a plugin's data file is no merged set: a merged set "
                        "isn't imported, its plugins are"),
              std::string::npos)
        << ecuio_last_error_detail();
}
