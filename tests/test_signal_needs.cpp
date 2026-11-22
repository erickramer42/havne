// File/layer: tests/test_signal_needs.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalNeedsTest (test_signals)
//
// A signal file's "needs" (ADR-0065 section 9, format 3): the keys its
// expressions read that another file provides. Loaded alone, a computed
// signal reading one is judged whole and then left out with a note, and
// so is each signal computed from it.

#include "test_signal_file_support.h"

#include <vector>

namespace {

/* A format 3 file of numbers a to d (named A to D), then the signals
 * given; needs is the list's inside, "" for no "needs" */
std::string needing(const std::string& needs,
                    const std::vector<std::string>& signals,
                    unsigned format = 3)
{
    std::string s = "{\"havne_signals\":" + std::to_string(format) +
                    R"(,"name":"T","source":{"name":"t"},)" +
                    (needs.empty() ? "" : "\"needs\":[" + needs + "],") +
                    R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)";
    for (int i = 0; i < 4; ++i) {
        char one[160];
        std::snprintf(one, sizeof one,
                      "%s{\"key\":\"%c\",\"name\":\"%c\",\"read\":{\"service\":"
                      "\"22\",\"number\":\"D10%d\"},\"field\":{\"byte\":0}}",
                      i == 0 ? "" : ",", "abcd"[i], "ABCD"[i], i);
        s += one;
    }
    for (const std::string& one : signals) {
        s += "," + one;
    }
    return s + "]}";
}

std::string computed(const std::string& key, const std::string& expr,
                     const std::string& more = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"N " + key +
           "\",\"kind\":\"computed\",\"expr\":\"" + expr + "\"" + more + "}";
}

struct Note {
    uint32_t kind;
    std::string text;
};

std::vector<Note> notes_of(const ecuio_signal_file_t* f)
{
    std::vector<Note> out;
    for (uint32_t i = 0; i < ecuio_signal_file_note_count(f); ++i) {
        ecuio_signal_note_t n;
        std::memset(&n, 0xA5, sizeof n);
        n.struct_size = sizeof n;
        EXPECT_EQ(ecuio_signal_file_note(f, i, &n), ECUIO_OK);
        EXPECT_EQ(n.struct_size, sizeof n);
        EXPECT_EQ(n.part, ECUIO_SIGNAL_PART_NONE);
        EXPECT_EQ(n.other_part, ECUIO_SIGNAL_PART_NONE);
        EXPECT_EQ(n.def, UINT32_MAX) << "a signal left out is no def";
        EXPECT_EQ(n.count, 0u);
        for (uint32_t r : n.reserved) {
            EXPECT_EQ(r, 0u);
        }
        const size_t length = strnlen(n.text, sizeof n.text);
        EXPECT_LT(length, sizeof n.text);
        for (size_t at = length; at < sizeof n.text; ++at) {
            EXPECT_EQ(n.text[at], '\0') << "zero past the text";
        }
        out.push_back({n.kind, n.text});
    }
    return out;
}

std::vector<std::string> needs_of(const ecuio_signal_file_t* f)
{
    uint32_t count = 99;
    EXPECT_EQ(ecuio_signal_file_needs(f, nullptr, 0, &count), ECUIO_OK);
    std::vector<ecuio_signal_key_t> keys(count);
    uint32_t again = 0;
    EXPECT_EQ(ecuio_signal_file_needs(f, keys.data(), count, &again), ECUIO_OK);
    EXPECT_EQ(again, count);
    std::vector<std::string> out;
    for (const ecuio_signal_key_t& k : keys) {
        out.emplace_back(k.text);
    }
    return out;
}

} // namespace

TEST(SignalNeedsTest, ANeededKeyLeavesItsReaderOutAlone)
{
    File f = load(
        needing(R"("x")", {computed("w", "a + x"), computed("k", "a * 2")}));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_find(f.get(), "w"), nullptr)
        << "left out: no part gives x";
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    ASSERT_EQ(set->count, 5u);
    EXPECT_EQ(set->computed_count, 1u);
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);

    /* The kept signals' IDs follow their place among the kept, so the
     * set is the one the file without the signal gives */
    const ecuio_signal_def_t* k = ecuio_signal_file_find(f.get(), "k");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->id, ECUIO_USER_SIG_ID_BASE + 4) << "not its written place, 5";
    File without = load(needing("", {computed("k", "a * 2")}));
    ASSERT_NE(without, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* other = ecuio_signal_file_set(without.get());
    ASSERT_EQ(other->count, set->count);
    EXPECT_EQ(
        std::memcmp(other->defs, set->defs, sizeof *set->defs * set->count), 0);
    EXPECT_EQ(
        std::memcmp(other->computed, set->computed, sizeof *set->computed), 0);

    const std::vector<Note> notes = notes_of(f.get());
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].kind, ECUIO_NOTE_SIGNAL_LEFT_OUT);
    EXPECT_EQ(notes[0].text, "'N w' is left out: it reads 'x', which this file "
                             "needs from another");
    EXPECT_EQ(needs_of(f.get()), (std::vector<std::string>{"x"}));

    /* A file with no needs has neither */
    EXPECT_EQ(ecuio_signal_file_note_count(without.get()), 0u);
    EXPECT_TRUE(needs_of(without.get()).empty());
}

TEST(SignalNeedsTest, WhatIsComputedFromItIsLeftOutToo)
{
    File f = load(
        needing(R"("x", "y")", {computed("g", "e + 1"), computed("e", "w * 2"),
                                computed("w", "a + x"), computed("k", "a + 1"),
                                computed("m", "k + y + w")}));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    for (const char* gone : {"w", "e", "g", "m"}) {
        EXPECT_EQ(ecuio_signal_file_find(f.get(), gone), nullptr) << gone;
    }
    const ecuio_signal_def_t* k = ecuio_signal_file_find(f.get(), "k");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->id, ECUIO_USER_SIG_ID_BASE + 4);
    EXPECT_EQ(ecuio_signal_file_set(f.get())->count, 5u);

    /* Those that read a need, in the file's order, then what reads them */
    const std::vector<Note> notes = notes_of(f.get());
    ASSERT_EQ(notes.size(), 4u);
    EXPECT_EQ(notes[0].text, "'N w' is left out: it reads 'x', which this file "
                             "needs from another");
    EXPECT_EQ(notes[1].text, "'N m' is left out: it reads 'y', which this file "
                             "needs from another");
    EXPECT_EQ(notes[2].text,
              "'N e' is left out: it reads 'w', which is left out");
    EXPECT_EQ(notes[3].text,
              "'N g' is left out: it reads 'e', which is left out");
    for (const Note& n : notes) {
        EXPECT_EQ(n.kind, ECUIO_NOTE_SIGNAL_LEFT_OUT);
    }
    EXPECT_EQ(needs_of(f.get()), (std::vector<std::string>{"x", "y"}));
}

TEST(SignalNeedsTest, AFileOfOnlyWaitingSignalsLoadsWithNone)
{
    const std::string only =
        R"({"havne_signals":3,"name":"Mine","source":{"name":"t"},)"
        R"("needs":["oil_t"],"signals":[)" +
        computed("margin", "130 - oil_t") + "]}";
    File f = load(only);
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    EXPECT_EQ(set->count, 0u);
    EXPECT_EQ(set->computed_count, 0u);
    EXPECT_EQ(ecuio_signal_file_note_count(f.get()), 1u);
    EXPECT_STREQ(ecuio_signal_file_profile(f.get()), "user.mine");
    size_t length = 0;
    const char* text = ecuio_signal_file_text(f.get(), &length);
    EXPECT_EQ(std::string(text, length), only)
        << "its text is what it was read from, the waiting signal in it";

    /* Over obd2 the base stands alone */
    File over =
        load(R"({"havne_signals":3,"name":"Mine","source":{"name":"t"},)"
             R"("extends":"obd2","needs":["oil_t"],"signals":[)" +
             computed("margin", "130 - oil_t") + "]}");
    ASSERT_NE(over, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_set(over.get())->count,
              ecuio_obd2_signal_set()->count);

    /* A list written empty is still refused */
    EXPECT_NE(
        refusal(R"({"havne_signals":3,"name":"Mine","source":{"name":"t"},)"
                R"("signals":[]})")
            .find("\"signals\" lists none"),
        std::string::npos);
}

TEST(SignalNeedsTest, AKeyNeitherDefinedNorNeededIsRefused)
{
    EXPECT_NE(refusal(needing(R"("x")", {computed("w", "a + x + bost")}))
                  .find("expr 'a + x + bost' reads 'bost', which no signal is "
                        "(at character 9)"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_NE(refusal(needing("", {computed("w", "a + x")}))
                  .find("reads 'x', which no signal is"),
              std::string::npos)
        << ecuio_last_error_detail();
}

TEST(SignalNeedsTest, EachNeedIsAKeyAnotherFileGivesAndThisOneReads)
{
    const std::string reads_x = computed("w", "a + x");
    const struct {
        std::string json;
        const char* words;
    } cases[] = {
        {needing(R"("x", "a")", {reads_x}),
         "\"needs\" lists 'a', which the file defines: a need is a key "
         "another file provides"},
        {needing(R"("x", "w")", {reads_x}),
         "\"needs\" lists 'w', which the file defines"},
        {needing(R"("x", "y")", {reads_x}),
         "\"needs\" lists 'y', which no expression of the file reads"},
        {needing(R"("x", "x")", {reads_x}), "\"needs\" lists 'x' twice"},
        {needing(R"("X")", {reads_x}),
         "\"needs\" lists 'X', which is no key: 1 to 31 of a-z, 0-9 and _"},
        {needing(R"("obd2.map")", {reads_x}),
         "\"needs\" lists 'obd2.map', which is no key"},
        {needing(R"("")", {reads_x}), "\"needs\" lists '', which is no key"},
        {needing(R"(7)", {reads_x}), "a need is a number, not a string"},
        {needing(R"("x")", {reads_x}, 2),
         "\"needs\" is signal-file format 3's, and this file is format 2"},
        {needing(R"("x")", {reads_x}, 1),
         "\"needs\" is signal-file format 3's, and this file is format 1"},
    };
    for (const auto& c : cases) {
        const std::string words = refusal(c.json);
        EXPECT_NE(words.find(c.words), std::string::npos)
            << c.words << "\n  got: " << words;
        EXPECT_EQ(words.rfind("line 1, column ", 0), 0u) << words;
    }
    std::string not_a_list = needing(R"("x")", {reads_x});
    not_a_list.replace(not_a_list.find("[\"x\"]"), 5, "\"x\"");
    EXPECT_NE(refusal(not_a_list).find("\"needs\" is a string, not a list"),
              std::string::npos)
        << ecuio_last_error_detail();
}

// A signal that waits is still read whole, so what is wrong with it is
// said when its file is loaded, not when another file meets its need
TEST(SignalNeedsTest, AWaitingSignalIsStillJudgedWhole)
{
    const struct {
        std::string signal;
        const char* words;
    } cases[] = {
        {computed("w", "a + x +"), "ends where a value should be"},
        {computed("w", "x + nope"), "reads 'nope', which no signal is"},
        {computed("w", "a + b + c2 + d + x"), "reads 'c2', which no signal is"},
        {computed("w", "a + b + d + x + y"), "reads more than 4 signals"},
        {computed("w", "a + x", R"(,"unti":"kPa")"), "unknown key 'unti'"},
        {computed("w", "a + x", R"(,"unit":"=kPa")"),
         "starts as a spreadsheet formula would"},
        {computed("w", "a + x", R"(,"gauge":{"min":5,"max":1})"),
         "isn't above its min"},
        {computed("w", "a + x", R"(,"max_age":{"b":5})"),
         "max_age names 'b', which its expr doesn't read"},
        {computed("w", "a + x", R"(,"max_age":{"x":500})"),
         "max_age for 'x' takes 0.1 to 60 seconds"},
    };
    for (const auto& c : cases) {
        const std::string words =
            refusal(needing(R"("x", "y")", {c.signal, computed("z", "y")}));
        EXPECT_NE(words.find(c.words), std::string::npos)
            << c.words << "\n  got: " << words;
    }

    /* A circle through a waiting signal is the file's own fault */
    EXPECT_NE(refusal(needing(R"("x")",
                              {computed("w", "e + x"), computed("e", "w + 1")}))
                  .find("computed signals can't read in a circle"),
              std::string::npos)
        << ecuio_last_error_detail();

    /* An age bound on a needed key is taken: the other file's signal
     * is judged when the two are read together */
    File aged = load(
        needing(R"("x")", {computed("w", "a + x", R"(,"max_age":{"x":5})")}));
    EXPECT_NE(aged, nullptr) << ecuio_last_error_detail();
}

TEST(SignalNeedsTest, TheGettersFollowTheirContracts)
{
    File f = load(needing(R"("x", "yy")", {computed("w", "a + x + yy")}));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();

    /* Needs: the serial ports' contract */
    uint32_t count = 99;
    EXPECT_EQ(ecuio_signal_file_needs(f.get(), nullptr, 0, &count), ECUIO_OK);
    EXPECT_EQ(count, 2u);
    ecuio_signal_key_t keys[3];
    std::memset(keys, 0x5A, sizeof keys);
    count = 99;
    EXPECT_EQ(ecuio_signal_file_needs(f.get(), keys, 1, &count), ECUIO_OK);
    EXPECT_EQ(count, 2u) << "all found, the first capacity copied";
    EXPECT_STREQ(keys[0].text, "x");
    for (size_t i = 1; i < sizeof keys[0].text; ++i) {
        EXPECT_EQ(keys[0].text[i], '\0') << "zero from the NUL to byte 32";
    }
    EXPECT_EQ(keys[1].text[0], 0x5A) << "past the capacity: untouched";
    EXPECT_EQ(ecuio_signal_file_needs(f.get(), keys, 3, &count), ECUIO_OK);
    EXPECT_STREQ(keys[1].text, "yy");
    EXPECT_EQ(keys[2].text[0], 0x5A);
    EXPECT_EQ(ecuio_signal_file_needs(nullptr, keys, 3, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_needs(f.get(), keys, 3, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_needs(f.get(), nullptr, 3, &count),
              ECUIO_ERR_NULL_ARGUMENT);

    /* Notes: a getter by index over a sized struct */
    EXPECT_EQ(ecuio_signal_file_note_count(nullptr), 0u);
    ASSERT_EQ(ecuio_signal_file_note_count(f.get()), 1u);
    ecuio_signal_note_t note;
    std::memset(&note, 0x5A, sizeof note);
    note.struct_size = sizeof note;
    const ecuio_signal_note_t untouched = note;
    EXPECT_EQ(ecuio_signal_file_note(f.get(), 1, &note),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file has 1 notes, and index 1 is none");
    EXPECT_EQ(ecuio_signal_file_note(f.get(), UINT32_MAX, &note),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&note, &untouched, sizeof note), 0);
    EXPECT_EQ(ecuio_signal_file_note(nullptr, 0, &note),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_note(f.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    note.struct_size = sizeof note - 1;
    EXPECT_EQ(ecuio_signal_file_note(f.get(), 0, &note),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the note's struct_size is 423, under its 424 bytes");
    note.struct_size = sizeof note;
    EXPECT_EQ(ecuio_signal_file_note(f.get(), 0, &note), ECUIO_OK);
    EXPECT_EQ(note.kind, ECUIO_NOTE_SIGNAL_LEFT_OUT);
}
