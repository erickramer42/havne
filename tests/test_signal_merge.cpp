// File/layer: tests/test_signal_merge.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergeTest (test_signals)
//
// ecuio_signal_file_merge (ADR-0065 section 9): a later part's signal in
// the place of an earlier one's of its key or its address, the keys it
// keeps, obd2 never replaced, computed signals across parts, and each
// note's sentence. The modules, bounds and refusals are in
// test_signal_merge_bounds.cpp.

#include "test_signal_merge_support.h"

namespace {

const std::string kAlpha =
    doc("Alpha",
        num("a", "D100") + "," + num("b", "D101") + "," + num("c", "D102"));
const std::string kBeta = doc(
    "Beta", num("b", "D201") + "," + num("x", "D100") + "," + num("n", "D300"));

uint16_t input_of(const ecuio_signal_file_t* f, const char* key,
                  uint32_t input = 0)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    EXPECT_NE(def, nullptr) << key;
    if (def == nullptr || (def->flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
        return 0;
    }
    return static_cast<uint16_t>(
        set->computed[def->table_first].input_ids[input]);
}

} // namespace

TEST(MergeTest, ALaterPartsSignalTakesThePlaceOfItsKeyOrItsAddress)
{
    File a = load(kAlpha);
    File b = load(kBeta);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_EQ(m.words, "");

    /* x where a was (its address), Beta's b where Alpha's was (its key),
     * c as it was, n at the end */
    EXPECT_EQ(keys_of(f), (Words{"x", "b", "c", "n"}));
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    ASSERT_EQ(set->count, 4u);
    for (uint32_t i = 0; i < set->count; ++i) {
        EXPECT_EQ(set->defs[i].id, ECUIO_USER_SIG_ID_BASE + i);
    }
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);
    const ecuio_signal_def_t* x = ecuio_signal_file_find(f, "x");
    const ecuio_signal_def_t* b_def = ecuio_signal_file_find(f, "b");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(b_def, nullptr);
    EXPECT_EQ(ecuio_signal_file_find(f, "a"), x) << "the key it replaced";
    EXPECT_EQ(replaced_keys(f, "x"), (Words{"a"}));
    EXPECT_TRUE(replaced_keys(f, "b").empty()) << "its own key, kept once";
    EXPECT_EQ(b_def->mux_key, 0x62D201u) << "Beta's address";
    EXPECT_EQ(ecuio_signal_file_def_part(f, x), 1u);
    EXPECT_EQ(ecuio_signal_file_def_part(f, b_def), 1u);
    EXPECT_EQ(ecuio_signal_file_def_part(f, ecuio_signal_file_find(f, "c")),
              0u);

    /* What the file says of itself */
    EXPECT_EQ(ecuio_signal_file_format(f), 3u);
    EXPECT_STREQ(ecuio_signal_file_name(f), "2 parts read together");
    /* sha256("user.alpha\nuser.beta\n") */
    EXPECT_STREQ(ecuio_signal_file_profile(f), "merged.286425b23d86d98d");
    ecuio_signal_source_t own{};
    own.struct_size = sizeof own;
    ASSERT_EQ(ecuio_signal_file_source(f, &own), ECUIO_OK);
    EXPECT_STREQ(own.name, "A merge of 2 parts");
    EXPECT_EQ(own.flags, ECUIO_SOURCE_MERGED);
    ASSERT_EQ(ecuio_signal_file_part_count(f), 2u);
    ecuio_signal_part_t part{};
    part.struct_size = sizeof part;
    ASSERT_EQ(ecuio_signal_file_part(f, 0, &part), ECUIO_OK);
    EXPECT_STREQ(part.profile, "user.alpha");
    EXPECT_STREQ(part.name, "Alpha");
    EXPECT_EQ(part.signal_count, 1u) << "c alone is left of Alpha";
    EXPECT_EQ(part.flags, 0u);
    ASSERT_EQ(ecuio_signal_file_part(f, 1, &part), ECUIO_OK);
    EXPECT_EQ(part.signal_count, 3u);
    EXPECT_EQ(part.module_count, 1u);
    ecuio_signal_source_t from{};
    from.struct_size = sizeof from;
    ASSERT_EQ(ecuio_signal_file_part_source(f, 1, &from), ECUIO_OK);
    EXPECT_STREQ(from.name, "src Beta");

    /* The notes, in the order the merge made them */
    EXPECT_EQ(sentences(f),
              (Words{"2: 'N b' (Beta) replaces 'N b' (Alpha) by its key 'b'; "
                     "it reads another address",
                     "1: 'N x' (Beta) replaces 'N a' (Alpha): both read PCM "
                     "22 D100, byte 0"}));
    const std::vector<ecuio_signal_note_t> notes = notes_of(f);
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].part, 1u);
    EXPECT_EQ(notes[0].other_part, 0u);
    EXPECT_EQ(notes[0].def, place_of(f, "b"));
    EXPECT_EQ(notes[0].count, 0u);
    EXPECT_EQ(notes[1].def, place_of(f, "x"));
}

// The text is the set: read back it gives the same defs, parts and keys,
// and the same parts give the same bytes
TEST(MergeTest, TheTextReadsBackToTheSameSetAndIsTheSameEachTime)
{
    File a = load(kAlpha);
    File b = load(kBeta);
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    size_t length = 0;
    const char* text = ecuio_signal_file_text(m.file.get(), &length);
    ASSERT_GT(length, 0u);
    File back = load_set(std::string(text, length));
    ASSERT_NE(back, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(m.file.get());
    const ecuio_signal_set_t* again = ecuio_signal_file_set(back.get());
    ASSERT_EQ(again->count, set->count);
    EXPECT_EQ(std::memcmp(again->defs, set->defs,
                          set->count * sizeof(ecuio_signal_def_t)),
              0);
    EXPECT_STREQ(ecuio_signal_file_profile(back.get()),
                 ecuio_signal_file_profile(m.file.get()));
    EXPECT_EQ(ecuio_signal_file_part_count(back.get()), 2u);
    EXPECT_EQ(keys_of(back.get()), keys_of(m.file.get()));
    EXPECT_EQ(replaced_keys(back.get(), "x"), (Words{"a"}));
    EXPECT_EQ(ecuio_signal_file_note_count(back.get()), 0u)
        << "the notes are the merge's, not the set's";

    const Merged twice = merge({a.get(), b.get()});
    ASSERT_EQ(twice.status, ECUIO_OK);
    size_t second = 0;
    const char* same = ecuio_signal_file_text(twice.file.get(), &second);
    EXPECT_EQ(std::string(same, second), std::string(text, length));
    EXPECT_EQ(std::string(text, length).find("0x"), std::string::npos)
        << "nothing of a pointer";

    /* It registers under its profile, as any file's set does */
    EXPECT_EQ(ecuio_profile_register(ecuio_signal_file_profile(m.file.get()),
                                     set, nullptr),
              ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister(ecuio_signal_file_profile(m.file.get())),
              ECUIO_OK);
}

TEST(MergeTest, TheCallersLastPartWins)
{
    File a = load(kAlpha);
    File b = load(kBeta);
    const Merged m = merge({b.get(), a.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    /* Alpha's b where Beta's was, a where x was, n as it was, c added */
    EXPECT_EQ(keys_of(f), (Words{"b", "a", "n", "c"}));
    EXPECT_EQ(ecuio_signal_file_find(f, "b")->mux_key, 0x62D101u);
    EXPECT_EQ(ecuio_signal_file_find(f, "x"), ecuio_signal_file_find(f, "a"));
    /* sha256("user.beta\nuser.alpha\n") */
    EXPECT_STREQ(ecuio_signal_file_profile(f), "merged.7491b3db72c74f5f");
}

TEST(MergeTest, ASignalMatchingOneByKeyAndAnotherByAddressReplacesBoth)
{
    File a = load(doc("Alpha", num("a", "D100") + "," + num("b", "D101") + "," +
                                   num("c", "D102")));
    File b = load(doc("Beta", num("a", "D101")));
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_EQ(keys_of(f), (Words{"a", "c"})) << "in the key match's place";
    EXPECT_EQ(replaced_keys(f, "a"), (Words{"b"}));
    EXPECT_EQ(ecuio_signal_file_find(f, "a")->mux_key, 0x62D101u);
    EXPECT_EQ(sentences(f),
              (Words{"2: 'N a' (Beta) replaces 'N a' (Alpha) by its key 'a'; "
                     "it reads another address",
                     "1: 'N a' (Beta) replaces 'N b' (Alpha): both read PCM "
                     "22 D101, byte 0"}));
}

// A signal replaced leaves its key with the one that stands; a later
// part that defines that key afresh takes it
TEST(MergeTest, AnOwnKeyTakesAKeyKeptForAReplacedSignal)
{
    File a = load(doc("Alpha", num("a", "D100")));
    File b = load(doc("Beta", num("x", "D100")));
    File c = load(doc("Gamma", num("a", "D300")));
    const Merged m = merge({a.get(), b.get(), c.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_EQ(keys_of(f), (Words{"x", "a"}));
    EXPECT_TRUE(replaced_keys(f, "x").empty()) << "x lost the key";
    EXPECT_EQ(ecuio_signal_file_find(f, "a")->mux_key, 0x62D300u);
    const Words said = sentences(f);
    ASSERT_EQ(said.size(), 2u);
    EXPECT_EQ(said[1], "3: 'N a' (Gamma) takes the key 'a' from 'N x' (Beta), "
                       "which kept it for a signal it replaced");
    const ecuio_signal_note_t taken = notes_of(f)[1];
    EXPECT_EQ(taken.part, 2u);
    EXPECT_EQ(taken.other_part, 1u);
    EXPECT_EQ(taken.def, place_of(f, "a"));

    /* A note follows its signal to the one that stands there in the end */
    File d = load(doc("Delta", num("y", "D100")));
    const Merged twice = merge({a.get(), b.get(), d.get()});
    ASSERT_EQ(twice.status, ECUIO_OK) << twice.words;
    EXPECT_EQ(keys_of(twice.file.get()), (Words{"y"}));
    EXPECT_EQ(replaced_keys(twice.file.get(), "y"), (Words{"x", "a"}));
    const std::vector<ecuio_signal_note_t> both = notes_of(twice.file.get());
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0].def, place_of(twice.file.get(), "y"));
    EXPECT_EQ(both[0].part, 1u) << "the note is still Beta's over Alpha's";
}

// A signal at the address of the one that kept its key replaces it and
// takes the key with it: one replacement, no key taken from a signal gone
TEST(MergeTest, AKeyKeptByTheSignalItReplacesIsNotTakenFromIt)
{
    File a = load(doc("Alpha", num("a", "D100")));
    File b = load(doc("Beta", num("x", "D100")));
    File c = load(doc("Gamma", num("a", "D100")));
    const Merged m = merge({a.get(), b.get(), c.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_EQ(keys_of(f), (Words{"a"}));
    EXPECT_EQ(replaced_keys(f, "a"), (Words{"x"}));
    EXPECT_EQ(ecuio_signal_file_def_part(f, ecuio_signal_file_find(f, "a")),
              2u);
    EXPECT_EQ(sentences(f),
              (Words{"1: 'N x' (Beta) replaces 'N a' (Alpha): both read PCM "
                     "22 D100, byte 0",
                     "1: 'N a' (Gamma) replaces 'N x' (Beta): both read PCM "
                     "22 D100, byte 0"}));
}

TEST(MergeTest, ANameReplacesNothingAndMayRepeat)
{
    File a = load(doc("Alpha", num("a", "D100", "", "Oil temperature")));
    File b = load(doc("Beta", num("b", "D200", "", "Oil temperature")));
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    EXPECT_EQ(keys_of(m.file.get()), (Words{"a", "b"}));
    EXPECT_EQ(ecuio_signal_file_note_count(m.file.get()), 0u);
}

// obd2's signals are never replaced: not by a signal of an obd2 name, not
// by one at an obd2 address, not by a format 2 part that alone stood in
// an obd2 signal's place
TEST(MergeTest, NoObd2SignalIsReplaced)
{
    /* Alone, Old's "RPM" stands in obd2's place and half reads it */
    File old = load(doc("Old",
                        num("my_rpm", "D100", "", "RPM") + "," +
                            calc("half", "obd2.rpm / 2") + "," +
                            calc("slow", "obd2.speed / 2"),
                        R"("extends":"obd2",)", 2));
    ASSERT_NE(old, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_find(old.get(), "my_rpm")->id,
              ECUIO_OBD2_SIG_RPM_ID);
    File fresh =
        load(doc("Fresh",
                 R"({"key":"pid_rpm","name":"Raw RPM","read":{"service":"01",)"
                 R"("number":"0C"},"field":{"byte":0,"bits":16}},)" +
                     num("named", "D200", "", "Speed"),
                 R"("extends":"obd2",)"));
    ASSERT_NE(fresh, nullptr) << ecuio_last_error_detail();

    const Merged m = merge({old.get(), fresh.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    ASSERT_EQ(set->count, obd2->count + 5);
    EXPECT_EQ(std::memcmp(set->defs, obd2->defs,
                          obd2->count * sizeof(ecuio_signal_def_t)),
              0)
        << "every obd2 def as it is, its ID too";
    EXPECT_EQ(keys_of(f),
              (Words{"my_rpm", "half", "slow", "pid_rpm", "named"}));
    EXPECT_EQ(ecuio_signal_file_find(f, "my_rpm")->id, ECUIO_USER_SIG_ID_BASE);
    /* "obd2.<key>" reads obd2's, whatever its part read alone */
    EXPECT_EQ(input_of(f, "half"), ECUIO_OBD2_SIG_RPM_ID);
    EXPECT_EQ(ecuio_signal_file_def_part(f, &set->defs[0]),
              ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(sentences(f),
              (Words{"4: 'RPM' (Old) is read beside obd2's 'RPM': a merged "
                     "set replaces no obd2 signal; 'C half' reads obd2's"}));
    EXPECT_EQ(notes_of(f)[0].def, place_of(f, "my_rpm"));
    EXPECT_EQ(notes_of(f)[0].part, 0u);
    /* sha256("obd2\nuser.old\nuser.fresh\n"): the base is in the name */
    EXPECT_EQ(
        std::string(ecuio_signal_file_profile(f)).compare(0, 7, "merged."), 0);

    /* Two readers of obd2's are counted */
    File two = load(doc("Old",
                        num("my_rpm", "D100", "", "RPM") + "," +
                            calc("half", "obd2.rpm / 2") + "," +
                            calc("third", "obd2.pid_0c / 3"),
                        R"("extends":"obd2",)", 2));
    ASSERT_NE(two, nullptr) << ecuio_last_error_detail();
    const Merged both = merge({two.get()});
    ASSERT_EQ(both.status, ECUIO_OK) << both.words;
    EXPECT_EQ(sentences(both.file.get()),
              (Words{"4: 'RPM' (Old) is read beside obd2's 'RPM': a merged "
                     "set replaces no obd2 signal; 'C half' and 1 more read "
                     "obd2's"}));
}

// The note of a signal that stood in an obd2 signal's place follows it to
// the signal a later part put there, so what showed the one can show the
// other; with nothing standing there it isn't given
TEST(MergeTest, TheNoteBesideObd2FollowsTheSignalThatStands)
{
    File old =
        load(doc("Old",
                 num("my_rpm", "D100", "", "RPM") + "," + num("keep", "D300") +
                     "," + calc("half", "obd2.rpm / 2"),
                 R"("extends":"obd2",)", 2));
    ASSERT_NE(old, nullptr) << ecuio_last_error_detail();
    File later = load(doc("Later", num("y", "D100")));
    const Merged m = merge({old.get(), later.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_EQ(keys_of(f), (Words{"y", "keep", "half"}));
    EXPECT_EQ(sentences(f),
              (Words{"4: 'N y' (Later) is read beside obd2's 'RPM' in the "
                     "place of 'RPM' (Old): a merged set replaces no obd2 "
                     "signal; 'C half' reads obd2's",
                     "1: 'N y' (Later) replaces 'RPM' (Old): both read PCM 22 "
                     "D100, byte 0"}));
    const ecuio_signal_note_t beside = notes_of(f)[0];
    EXPECT_EQ(beside.def, place_of(f, "y"));
    EXPECT_EQ(beside.part, 0u) << "the note is still Old's";
    EXPECT_EQ(beside.other_part, ECUIO_SIGNAL_PART_NONE);

    /* What took its place is left out: nothing is beside obd2's */
    File unmet = load(doc("Unmet", calc("my_rpm", "130 - oil"),
                          R"("needs":["oil"],)", 3, ""));
    ASSERT_NE(unmet, nullptr) << ecuio_last_error_detail();
    const Merged gone = merge({old.get(), unmet.get()});
    ASSERT_EQ(gone.status, ECUIO_OK) << gone.words;
    EXPECT_EQ(keys_of(gone.file.get()), (Words{"keep", "half"}));
    EXPECT_EQ(sentences(gone.file.get()),
              (Words{"2: 'C my_rpm' (Unmet) replaces 'RPM' (Old) by its key "
                     "'my_rpm'",
                     "8: 'C my_rpm' (Unmet) is left out: it reads 'oil', "
                     "which its part needs and no part gives"}));
}

TEST(MergeTest, AFormat3PartMergedAloneIsItsOwnSet)
{
    const std::string text = doc(
        "Whole",
        num("oil", "4A10",
            R"(,"ecu":"tcm","offset":-40,"unit":"deg C",)"
            R"("gauge":{"min":-40,"max":150})") +
            R"(,{"key":"gear","name":"Gear","kind":"enum","ecu":"tcm",)"
            R"("read":{"service":"22","number":"4A20"},"field":{"byte":0},)"
            R"("values":{"0":"Park","1":"Reverse"}},)"
            R"({"key":"serial","name":"Serial","kind":"text",)"
            R"("read":{"service":"22","number":"4A00"},"field":{"byte":0}},)" +
            calc("margin", "130 - oil", R"(,"max_age":{"oil":5})"),
        R"("extends":"obd2",)", 3,
        R"([{"id":"pcm","rx":"7E8"},{"id":"tcm","name":"Transmission",)"
        R"("rx":"7E9"}])");
    File part = load(text);
    ASSERT_NE(part, nullptr) << ecuio_last_error_detail();
    const Merged m = merge({part.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_set_t* own = ecuio_signal_file_set(part.get());
    const ecuio_signal_set_t* set = ecuio_signal_file_set(m.file.get());
    ASSERT_EQ(set->count, own->count);
    EXPECT_EQ(std::memcmp(set->defs, own->defs,
                          own->count * sizeof(ecuio_signal_def_t)),
              0);
    ASSERT_EQ(set->channel_count, own->channel_count);
    EXPECT_EQ(std::memcmp(set->channels, own->channels,
                          own->channel_count * sizeof(ecuio_channel_decl_t)),
              0);
    ASSERT_EQ(set->enum_count, own->enum_count);
    EXPECT_EQ(std::memcmp(set->enums, own->enums,
                          own->enum_count * sizeof(ecuio_enum_entry_t)),
              0);
    ASSERT_EQ(set->computed_count, own->computed_count);
    EXPECT_EQ(std::memcmp(set->computed, own->computed,
                          own->computed_count * sizeof(ecuio_computed_t)),
              0);
    EXPECT_STREQ(ecuio_signal_file_name(m.file.get()), "1 part read alone");
    EXPECT_EQ(ecuio_signal_file_note_count(m.file.get()), 0u);
    ecuio_signal_module_t module{};
    module.struct_size = sizeof module;
    ASSERT_EQ(ecuio_signal_file_module(m.file.get(), 1, &module), ECUIO_OK);
    EXPECT_STREQ(module.id, "tcm");
    EXPECT_STREQ(module.name, "Transmission");
    EXPECT_EQ(module.part, 0u);
}

// A format 1 or 2 part may read one field as a number and as an enum:
// both stay, and a later part's signal at that address replaces both
TEST(MergeTest, AFormat2PartsTwoAtOneAddressStayTogether)
{
    const std::string pair =
        R"({"key":"gear","name":"Gear","kind":"enum",)"
        R"("read":{"service":"22","number":"4A20"},"field":{"byte":0},)"
        R"("values":{"0":"Park","1":"Drive"}},)" +
        num("gear_raw", "4A20");
    File old = load(doc("Old", pair, "", 2));
    File later = load(doc("Later", num("g2", "4A20")));
    ASSERT_NE(old, nullptr) << ecuio_last_error_detail();

    const Merged alone = merge({old.get()});
    ASSERT_EQ(alone.status, ECUIO_OK) << alone.words;
    EXPECT_EQ(keys_of(alone.file.get()), (Words{"gear", "gear_raw"}));

    const Merged over = merge({old.get(), later.get()});
    ASSERT_EQ(over.status, ECUIO_OK) << over.words;
    EXPECT_EQ(keys_of(over.file.get()), (Words{"g2"}));
    EXPECT_EQ(replaced_keys(over.file.get(), "g2"),
              (Words{"gear", "gear_raw"}));
    EXPECT_EQ(ecuio_signal_file_note_count(over.file.get()), 2u);

    const Merged under = merge({later.get(), old.get()});
    ASSERT_EQ(under.status, ECUIO_OK) << under.words;
    EXPECT_EQ(keys_of(under.file.get()), (Words{"gear", "gear_raw"}))
        << "the pair's first in g2's place, its second kept beside it";
    EXPECT_EQ(replaced_keys(under.file.get(), "gear"), (Words{"g2"}));
}

// A part's license stays with the part, never the whole set's
TEST(MergeTest, EachPartKeepsItsSourceAndItsLicense)
{
    const std::string licensed =
        R"({"havne_signals":2,"name":"Import","source":{"name":"An import",)"
        R"("url":"https://example.org","license":"CC-BY-SA-4.0"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)" +
        num("a", "D100") + "]}";
    File import = load(licensed);
    File mine = load(doc("Mine", num("x", "D100")));
    const Merged m = merge({import.get(), mine.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    ecuio_signal_source_t s{};
    s.struct_size = sizeof s;
    ASSERT_EQ(ecuio_signal_file_part_source(m.file.get(), 0, &s), ECUIO_OK);
    EXPECT_STREQ(s.license, "CC-BY-SA-4.0");
    EXPECT_STREQ(s.url, "https://example.org");
    EXPECT_EQ(s.flags, ECUIO_SOURCE_SHARE_ALIKE);
    ASSERT_EQ(ecuio_signal_file_part_source(m.file.get(), 1, &s), ECUIO_OK);
    EXPECT_EQ(s.flags, 0u);
    ASSERT_EQ(ecuio_signal_file_source(m.file.get(), &s), ECUIO_OK);
    EXPECT_EQ(s.flags, ECUIO_SOURCE_MERGED) << "never share-alike itself";
    EXPECT_STREQ(s.license, "");
    /* The signal that replaced the import's is its own part's */
    EXPECT_EQ(ecuio_signal_file_def_part(
                  m.file.get(), ecuio_signal_file_find(m.file.get(), "a")),
              1u);
    ecuio_signal_part_t part{};
    part.struct_size = sizeof part;
    ASSERT_EQ(ecuio_signal_file_part(m.file.get(), 0, &part), ECUIO_OK);
    EXPECT_EQ(part.format, 2u);
    EXPECT_EQ(part.signal_count, 0u) << "its one signal was replaced";
    EXPECT_EQ(part.module_count, 0u);
}

// One key space across the parts: a need another part defines is met, and
// a key a signal replaced still reads the signal that stands
TEST(MergeTest, AComputedSignalReadsAcrossParts)
{
    File a = load(doc("Alpha", num("oil", "D100")));
    File b = load(doc("Beta", num("oil_t", "D100")));
    const std::string needing =
        doc("Gamma", calc("margin", "130 - oil"), R"("needs":["oil"],)", 3, "");
    File c = load(needing);
    ASSERT_NE(c, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_note_count(c.get()), 1u) << "alone, it waits";

    const Merged met = merge({a.get(), c.get()});
    ASSERT_EQ(met.status, ECUIO_OK) << met.words;
    EXPECT_EQ(keys_of(met.file.get()), (Words{"oil", "margin"}));
    EXPECT_EQ(input_of(met.file.get(), "margin"),
              ecuio_signal_file_find(met.file.get(), "oil")->id);
    EXPECT_EQ(ecuio_signal_file_note_count(met.file.get()), 0u);
    uint32_t needs = 9;
    EXPECT_EQ(ecuio_signal_file_needs(met.file.get(), nullptr, 0, &needs),
              ECUIO_OK);
    EXPECT_EQ(needs, 0u) << "a merged set needs nothing";

    /* Through the key Beta's signal replaced */
    const Merged through = merge({a.get(), b.get(), c.get()});
    ASSERT_EQ(through.status, ECUIO_OK) << through.words;
    EXPECT_EQ(keys_of(through.file.get()), (Words{"oil_t", "margin"}));
    EXPECT_EQ(input_of(through.file.get(), "margin"),
              ecuio_signal_file_find(through.file.get(), "oil_t")->id);
    EXPECT_EQ(sentences(through.file.get()),
              (Words{"1: 'N oil_t' (Beta) replaces 'N oil' (Alpha): both read "
                     "PCM 22 D100, byte 0; 1 computed signal reads 'oil'"}));
}

// What the set can't compute is left out with a note, and what is
// computed from it, never a refusal
TEST(MergeTest, WhatTheSetCannotComputeIsLeftOut)
{
    const std::string needing =
        doc("Beta",
            num("plain", "D200") + "," + calc("margin", "130 - oil") + "," +
                calc("twice", "margin * 2") + "," + calc("thrice", "twice + 1"),
            R"("needs":["oil"],)");
    File b = load(needing);
    ASSERT_NE(b, nullptr) << ecuio_last_error_detail();

    /* A need no part gives */
    const Merged unmet = merge({b.get()});
    ASSERT_EQ(unmet.status, ECUIO_OK) << unmet.words;
    EXPECT_EQ(keys_of(unmet.file.get()), (Words{"plain"}));
    EXPECT_EQ(sentences(unmet.file.get()),
              (Words{"8: 'C margin' (Beta) is left out: it reads 'oil', which "
                     "its part needs and no part gives",
                     "8: 'C twice' (Beta) is left out: it reads 'margin', "
                     "which is left out",
                     "8: 'C thrice' (Beta) is left out: it reads 'twice', "
                     "which is left out"}));
    const ecuio_signal_note_t gone = notes_of(unmet.file.get())[0];
    EXPECT_EQ(gone.def, UINT32_MAX);
    EXPECT_EQ(gone.part, 0u);
    EXPECT_EQ(gone.other_part, ECUIO_SIGNAL_PART_NONE);

    /* A need met by a text */
    File text =
        load(doc("Alpha", R"({"key":"oil","name":"N oil","kind":"text",)"
                          R"("read":{"service":"22","number":"D100"},)"
                          R"("field":{"byte":0}})"));
    ASSERT_NE(text, nullptr) << ecuio_last_error_detail();
    const Merged as_text = merge({text.get(), b.get()});
    ASSERT_EQ(as_text.status, ECUIO_OK) << as_text.words;
    EXPECT_EQ(sentences(as_text.file.get())[0],
              "8: 'C margin' (Beta) is left out: it reads 'oil', which is "
              "text");

    /* A circle through two parts */
    File one = load(doc("One", num("a", "D100") + "," + calc("c1", "c2 + 1"),
                        R"("needs":["c2"],)"));
    File two =
        load(doc("Two", calc("c2", "c1 + 1"), R"("needs":["c1"],)", 3, ""));
    ASSERT_NE(two, nullptr) << ecuio_last_error_detail();
    const Merged circle = merge({one.get(), two.get()});
    ASSERT_EQ(circle.status, ECUIO_OK) << circle.words;
    EXPECT_EQ(keys_of(circle.file.get()), (Words{"a"}));
    EXPECT_EQ(sentences(circle.file.get()),
              (Words{"8: 'C c1' (One) is left out: it is on a circle of "
                     "computed signals, or reads one that is",
                     "8: 'C c2' (Two) is left out: it is on a circle of "
                     "computed signals, or reads one that is"}));

    /* A chain 9 deep: the ninth alone goes */
    std::string chain = num("x", "D100") + "," + calc("c1", "x + 1");
    for (int i = 2; i <= 8; ++i) {
        chain += "," + calc("c" + std::to_string(i),
                            "c" + std::to_string(i - 1) + " + 1");
    }
    File deep = load(doc("Deep", chain));
    ASSERT_NE(deep, nullptr) << ecuio_last_error_detail();
    File ninth =
        load(doc("Ninth", calc("c9", "c8 + 1"), R"("needs":["c8"],)", 3, ""));
    const Merged nine = merge({deep.get(), ninth.get()});
    ASSERT_EQ(nine.status, ECUIO_OK) << nine.words;
    EXPECT_EQ(keys_of(nine.file.get()).size(), 9u);
    EXPECT_EQ(sentences(nine.file.get()),
              (Words{"8: 'C c9' (Ninth) is left out: it sits 9 computed "
                     "signals deep: a chain goes 8 deep at most"}));

    /* An age bound on what a later part made a computed signal */
    File aged =
        load(doc("Aged", num("n", "D100") + "," +
                             calc("r", "n * 2", R"(,"max_age":{"n":5})")));
    File remade =
        load(doc("Remade", num("x", "D200") + "," + calc("n", "x + 1")));
    ASSERT_NE(aged, nullptr) << ecuio_last_error_detail();
    const Merged bound = merge({aged.get(), remade.get()});
    ASSERT_EQ(bound.status, ECUIO_OK) << bound.words;
    EXPECT_EQ(keys_of(bound.file.get()), (Words{"n", "x"}));
    EXPECT_EQ(sentences(bound.file.get()),
              (Words{"2: 'C n' (Remade) replaces 'N n' (Aged) by its key 'n'",
                     "8: 'C r' (Aged) is left out: its max_age names 'n', "
                     "which is a computed signal in the set"}));

    /* With nothing left the merge is refused */
    File only = load(
        doc("Only", calc("margin", "130 - oil"), R"("needs":["oil"],)", 3, ""));
    const Merged none = merge({only.get()});
    EXPECT_EQ(none.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(none.words, "no signal of the 1 part is left to read");
}

// A replacement says what now reads the replaced key, and both units
// when they differ
TEST(MergeTest, AReplacementNamesItsReadersAndBothUnits)
{
    File a =
        load(doc("Alpha", num("p", "D100", R"(,"unit":"psi")") + "," +
                              calc("r", "p * 2") + "," + calc("s", "p + r")));
    File b = load(doc("Beta", num("p", "D200", R"(,"unit":"kPa")")));
    ASSERT_NE(a, nullptr) << ecuio_last_error_detail();
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    EXPECT_EQ(sentences(m.file.get()),
              (Words{"2: 'N p' (Beta) replaces 'N p' (Alpha) by its key 'p'; "
                     "it reads another address; 2 computed signals read 'p'; "
                     "unit 'kPa', was 'psi'"}));

    /* Nothing reads it: the units go unsaid */
    File c = load(doc("Alpha", num("p", "D100", R"(,"unit":"psi")")));
    const Merged quiet = merge({c.get(), b.get()});
    ASSERT_EQ(quiet.status, ECUIO_OK) << quiet.words;
    EXPECT_EQ(sentences(quiet.file.get()),
              (Words{"2: 'N p' (Beta) replaces 'N p' (Alpha) by its key 'p'; "
                     "it reads another address"}));
}

// The longest sentence the bounds allow stays whole inside the note's
// room, and holds no control character
TEST(MergeTest, TheLongestNoteFitsItsRoom)
{
    const std::string long_name(31, 'W');
    const std::string long_key(31, 'k');
    const std::string unit(15, 'u');
    /* 63 bytes of two-byte characters and one more; a line break inside */
    std::string part_a = "A\\n";
    std::string part_b = "B";
    for (int i = 0; i < 30; ++i) {
        part_a += "Å";
        part_b += "Ø";
    }
    const std::string ecus =
        R"([{"id":"m","rx":"7E8","name":")" + std::string(31, 'M') + R"("}])";
    const std::string field = R"(,"unit":")" + unit + R"(")";
    File a = load(doc(part_a,
                      num(long_key, "FFFF", field, long_name) + "," +
                          calc("r", long_key + " * 2"),
                      "", 3, ecus));
    ASSERT_NE(a, nullptr) << ecuio_last_error_detail();
    const std::string other_unit =
        R"(,"unit":")" + std::string(15, 'v') + R"(")";
    File by_key = load(
        doc(part_b, num(long_key, "FFFE", other_unit, long_name), "", 3, ecus));
    File by_address = load(
        doc(part_b, num(std::string(31, 'j'), "FFFF", other_unit, long_name),
            "", 3, ecus));
    for (const ecuio_signal_file_t* later : {by_key.get(), by_address.get()}) {
        ASSERT_NE(later, nullptr) << ecuio_last_error_detail();
        const Merged m = merge({a.get(), later});
        ASSERT_EQ(m.status, ECUIO_OK) << m.words;
        const std::vector<ecuio_signal_note_t> notes = notes_of(m.file.get());
        ASSERT_EQ(notes.size(), 1u);
        const std::string text = notes[0].text;
        EXPECT_LT(text.size(), size_t{ECUIO_NOTE_TEXT_LEN} - 4) << text;
        EXPECT_TRUE(has(text, ("was '" + unit + "'").c_str()))
            << "whole to its last clause: " << text;
        EXPECT_TRUE(has(text, "ÅÅÅ...")) << "a part's name cut at a character";
        for (const char c : text) {
            EXPECT_GE(static_cast<unsigned char>(c), 0x20u) << text;
        }
    }
}
