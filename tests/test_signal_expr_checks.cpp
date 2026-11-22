// File/layer: tests/test_signal_expr_checks.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalExprTest (test_signals)

#include "test_signal_expr_support.h"

#include "signal_expr.h" /* core/src: the grammar's format */

TEST(SignalExprTest, ACheckRefusesItsArgumentsApartFromTheExpression)
{
    File f = load(file_of(2));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    ecuio_expr_check_t out;
    std::memset(&out, 0xA5, sizeof out);
    const ecuio_expr_check_t before = out;
    EXPECT_EQ(ecuio_signal_file_check_expr(nullptr, nullptr, "a", 1, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), nullptr, "a", 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), nullptr, nullptr, 1, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(std::memcmp(&out, &before, sizeof out), 0);
    /* A struct smaller than this core's: nothing written, at left alone */
    out.struct_size = sizeof out - 1;
    const ecuio_expr_check_t small = out;
    EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), nullptr, "a", 1, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the check's struct_size is 247, under its 248 bytes");
    EXPECT_EQ(std::memcmp(&out, &small, sizeof out), 0);
    /* A key that could be no signal's */
    out = ecuio_expr_check_t{};
    out.struct_size = sizeof out;
    for (const char* key :
         {"Bad", "a b", "a.b", "k2345678901234567890123456789012"}) {
        EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), key, "a", 1, &out),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << key;
        EXPECT_EQ(out.at, 0u) << key;
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("takes 1 to 31 of a-z, 0-9 and _"),
                  std::string::npos)
            << ecuio_last_error_detail();
    }
    /* A larger struct, a later core's caller: this core's bytes filled,
     * the rest left */
    struct Grown {
        ecuio_expr_check_t check;
        uint32_t later[4];
    } grown;
    std::memset(&grown, 0xA5, sizeof grown);
    grown.check.struct_size = sizeof grown;
    EXPECT_EQ(
        ecuio_signal_file_check_expr(f.get(), "", "a + b", 5, &grown.check),
        ECUIO_OK);
    EXPECT_EQ(grown.check.struct_size, sizeof(ecuio_expr_check_t));
    EXPECT_EQ(grown.check.program.input_count, 2u);
    EXPECT_EQ(grown.later[0], 0xA5A5A5A5u);
    /* No expression at all: a NULL with no length is the empty one */
    EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), nullptr, nullptr, 0,
                                           &grown.check),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(grown.check.at, 1u);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the expression ends where a value should be");
}

// A byte that is not UTF-8 (a load never meets one: the JSON reader refuses it
// first) is quoted alone, as '?'
TEST(SignalExprTest, AnInvalidByteIsQuotedAsAQuestionMark)
{
    File f = load(file_of(2));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const struct {
        std::string expr;
        std::string words;
    } cases[] = {
        {"a + \xFF", "has '?' where a value should be"},
        {"a \xFF", "has '?' where an operator or the end should be"},
        {"max(a \xFF)", "has '?' where ',' or ')' should be"},
        {"a + \xC3", "has '?' where a value should be"},
    };
    for (const auto& c : cases) {
        ecuio_expr_check_t out{};
        out.struct_size = sizeof out;
        EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), "", c.expr.data(),
                                               c.expr.size(), &out),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << c.expr;
        EXPECT_NE(std::string(ecuio_last_error_detail()).find(c.words),
                  std::string::npos)
            << ecuio_last_error_detail();
    }
}

// Made-up expressions, most of them wrong somewhere: a check and a load
// of the file with the expression in it agree on each, in its words, its
// character and its program
TEST(SignalExprTest, MadeUpExpressionsCheckAsTheyLoad)
{
    const char* const parts[] = {
        "a",    "b",    "c",        "d",         "x",      "y",     "e",
        "t",    "nope", "obd2.rpm", "obd2.nope", "1",      "2.5",   "0",
        ".",    "+",    "-",        "*",         "/",      "(",     ")",
        ",",    " ",    "<",        "<=",        ">",      "==",    "!=",
        "=",    "max(", "min(",     "abs(",      "clamp(", "sqrt(", "if(",
        "all(", "any(", "not(",     "pow(",      "round(", "A",     "$",
    };
    /* Every other one is values joined by operators, so many compile */
    const char* const values[] = {
        "a",
        "b",
        "c",
        "d",
        "x",
        "y",
        "obd2.rpm",
        "1",
        "2.5",
        "max(a, b)",
        "abs(c)",
        "if(a > b, c, d)",
        "clamp(d, 0, 9)",
        "(a - b)",
        "-c",
        "t",
        "nope",
        "e",
        "(a > b)",
    };
    const char* const joins[] = {" + ", " - ", " * ", " / ", "+", " "};
    const std::string text =
        R"({"key":"t","name":"T","kind":"text","read":{"service":"22",)"
        R"("number":"D110"},"field":{"byte":0}})";
    const std::vector<std::string> more = {text, computed("x", "a + 1"),
                                           computed("y", "x * b")};
    std::mt19937 rng(20261116u);
    uint32_t passed = 0;
    uint32_t refused = 0;
    for (const bool obd2 : {false, true}) {
        const std::string head = obd2 ? "\"extends\":\"obd2\"," : "";
        File base = load(file_of(2, more, head));
        ASSERT_TRUE(base) << ecuio_last_error_detail();
        for (int n = 0; n < 1500; ++n) {
            std::string expr;
            const int count = 1 + static_cast<int>(rng() % 9);
            for (int k = 0; k < count && n % 2 == 0; ++k) {
                expr += parts[rng() % (sizeof parts / sizeof parts[0])];
            }
            for (int k = 0; k < (count + 1) / 2 && n % 2 != 0; ++k) {
                expr += k == 0 ? "" : joins[rng() % 6];
                expr += values[rng() % (sizeof values / sizeof values[0])];
            }
            std::vector<std::string> with = more;
            with.push_back(computed("e", expr));
            File loaded = load(file_of(2, with, head));
            const std::string said = loaded ? "" : ecuio_last_error_detail();
            const Checked c = check(base.get(), "e", expr);
            ASSERT_EQ(c.status == ECUIO_OK, loaded != nullptr)
                << expr << "\n  load: " << said << "\n  check: " << c.detail;
            if (loaded) {
                ++passed;
                EXPECT_EQ(std::memcmp(&c.out.program,
                                      &program_of(loaded.get(), "e"),
                                      sizeof(ecuio_computed_t)),
                          0)
                    << expr;
                /* Format 1 holds no chain, so its file is without y,
                 * which reads x: an expression reading either is format
                 * 2's, and the file without y refuses one reading y */
                EXPECT_EQ(load(file_of(1, {text, more[1], with.back()},
                                       head)) != nullptr,
                          c.out.format == 1)
                    << expr;
                continue;
            }
            ++refused;
            const std::string words = said.substr(said.find(": ") + 2);
            if (words.compare(0, 6, "expr '") == 0) {
                uint32_t at = 0;
                const size_t place = words.rfind(" (at character ");
                ASSERT_NE(place, std::string::npos) << words;
                at =
                    static_cast<uint32_t>(std::stoul(words.substr(place + 15)));
                const std::string tail =
                    " " + c.detail.substr(std::strlen("the expression ")) +
                    words.substr(place);
                EXPECT_EQ(c.detail.compare(0, 15, "the expression "), 0)
                    << c.detail;
                ASSERT_GE(words.size(), tail.size()) << expr;
                EXPECT_EQ(words.substr(words.size() - tail.size()), tail)
                    << expr << "\n  check: " << c.detail;
                EXPECT_EQ(c.out.at, at) << expr;
            } else {
                EXPECT_EQ(c.detail, words) << expr;
            }
        }
    }
    /* Both ways were met often enough to mean something */
    EXPECT_GT(passed, 200u);
    EXPECT_GT(refused, 1000u);
}

// A plugin variant's file is checked and named as a file loaded alone
TEST(SignalExprTest, AVariantsFileIsCheckedTheSame)
{
    const std::string text =
        file_of(2, {computed("e", "a + obd2.rpm")}, "\"extends\":\"obd2\",");
    char* packed = nullptr;
    size_t length = 0;
    ASSERT_EQ(
        ecuio_plugin_wrap(text.data(), text.size(), nullptr, &packed, &length),
        ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_plugin_t* plugin = nullptr;
    const ecuio_status_t st =
        ecuio_plugin_load(packed, length, nullptr, nullptr, &plugin);
    ecuio_plugin_text_free(packed);
    ASSERT_EQ(st, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_signal_file_t* variant = ecuio_plugin_variant_file(plugin, 0);
    ASSERT_NE(variant, nullptr);
    Checked c = check(variant, "e", "e + 1");
    EXPECT_EQ(c.detail,
              "signal 'e' reads 'e': computed signals can't read in a circle");
    c = check(variant, nullptr, "max(e, obd2.map) - a");
    EXPECT_EQ(c.status, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.out.depth, 2u);
    EXPECT_EQ(ref_of(variant, ecuio_signal_file_find(variant, "e")), "e");
    EXPECT_NE(def_read_as(variant, "obd2.rpm"), nullptr);
    EXPECT_EQ(ecuio_plugin_free(plugin), ECUIO_OK);
}

// The calls only read the file: two threads over one file agree with one
TEST(SignalExprTest, TwoThreadsCheckOneFileAtOnce)
{
    File f = load(file_of(2, {computed("e", "a + 1"), computed("e1", "e * 2")},
                          "\"extends\":\"obd2\","));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const Checked ok = check(f.get(), "e", "max(a, obd2.rpm) + b");
    const Checked circle = check(f.get(), "e", "e1 + 1");
    ASSERT_EQ(ok.status, ECUIO_OK) << ok.detail;
    std::atomic<int> wrong{0};
    const auto run = [&] {
        for (int i = 0; i < 300; ++i) {
            const Checked a = check(f.get(), "e", "max(a, obd2.rpm) + b");
            const Checked b = check(f.get(), "e", "e1 + 1");
            if (std::memcmp(&a.out, &ok.out, sizeof ok.out) != 0 ||
                b.detail != circle.detail || b.out.at != circle.out.at) {
                ++wrong;
            }
            const ecuio_signal_def_t& d = set->defs[i % set->count];
            char name[ECUIO_EXPR_REF_LEN];
            const ecuio_status_t st =
                ecuio_signal_file_expr_ref(f.get(), &d, name, sizeof name);
            if (st == ECUIO_OK && (d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 &&
                d.kind != ECUIO_VALUE_TEXT && d.kind != ECUIO_VALUE_HEX) {
                const Checked c = check(f.get(), nullptr, name);
                if (c.status != ECUIO_OK ||
                    c.out.program.input_ids[0] != d.id) {
                    ++wrong;
                }
            }
        }
    };
    std::thread one(run);
    std::thread two(run);
    one.join();
    two.join();
    EXPECT_EQ(wrong.load(), 0);
}

// A length past the bound is refused before a byte is read, whatever it
// claims: the buffer here ends at the bound
TEST(SignalExprTest, ALengthPastTheBoundIsRefusedBeforeAByteIsRead)
{
    File f = load(file_of(2));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const std::string at_bound =
        "a" + std::string(ECUIO_EXPR_MAX_BYTES - 1, ' ');
    EXPECT_EQ(check(f.get(), nullptr, at_bound).status, ECUIO_OK);
    for (const size_t length :
         {size_t{ECUIO_EXPR_MAX_BYTES} + 1, size_t{1} << 31, SIZE_MAX}) {
        ecuio_expr_check_t out;
        std::memset(&out, 0xA5, sizeof out);
        out.struct_size = sizeof out;
        EXPECT_EQ(ecuio_signal_file_check_expr(f.get(), nullptr,
                                               at_bound.data(), length, &out),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << length;
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "the expression is longer than 256 characters");
        EXPECT_EQ(out.at, 1u) << length;
        EXPECT_EQ(out.format, 0u) << length;
        EXPECT_EQ(out.program.input_count, 0u) << length;
    }
    /* The loader's compiler keeps the same bound, in the same words */
    File holds = load(file_of(2, {computed("e", at_bound)}));
    EXPECT_TRUE(holds) << ecuio_last_error_detail();
    EXPECT_NE(load_words(file_of(2, {computed("e", at_bound + " ")}))
                  .find("is longer than 256 characters"),
              std::string::npos);
}

// A key that starts with a digit is a number to an expression, so the
// signal has no name there, unless it stands in a base def's place and
// goes by obd2's name
TEST(SignalExprTest, AKeyStartingWithADigitIsNoNameInAnExpression)
{
    const char* const head = "\"extends\":\"obd2\",";
    File plain = load(file_of(2, {}, head));
    ASSERT_TRUE(plain) << ecuio_last_error_detail();
    const ecuio_signal_def_t* map = def_read_as(plain.get(), "obd2.map");
    ASSERT_NE(map, nullptr);
    const auto number = [](const std::string& key, const std::string& name,
                           const char* did) {
        return "{\"key\":\"" + key + "\",\"name\":\"" + name +
               "\",\"read\":{\"service\":\"22\",\"number\":\"" + did +
               "\"},\"field\":{\"byte\":0}}";
    };
    File f = load(file_of(
        2,
        {number("2nd", "Second", "D120"), number("123", "Digits", "D121"),
         number("0th", "Zeroth", "D125"), number("9map", map->name, "D122"),
         number("_x", "Under", "D123"), number("x9", "Later", "D124")},
        head));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const auto def = [&](const char* key) {
        return ecuio_signal_file_find(f.get(), key);
    };
    const struct {
        const char* key;
        const char* name;
    } unnamed[] = {{"2nd", "Second"}, {"123", "Digits"}, {"0th", "Zeroth"}};
    for (const auto& u : unnamed) {
        EXPECT_EQ(ref_of(f.get(), def(u.key)),
                  "!'" + std::string(u.name) +
                      "' has no name in an expression: its key '" + u.key +
                      "' starts with a digit, which an expression reads as a "
                      "number");
    }
    /* The loader takes such a key, and no expression reads it */
    EXPECT_EQ(check(f.get(), nullptr, "2nd").status,
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(check(f.get(), nullptr, "123").detail,
              "the expression reads no signal: a constant is no signal of "
              "its own");
    /* One in a base def's place goes by obd2's name, which reads it */
    ASSERT_NE(def("9map"), nullptr);
    EXPECT_EQ(def("9map")->id, map->id);
    EXPECT_EQ(ref_of(f.get(), def("9map")), "obd2.map");
    Checked c = check(f.get(), nullptr, "obd2.map");
    ASSERT_EQ(c.status, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.out.program.input_ids[0], def("9map")->id);
    /* A digit after the first character, or an underscore first, is a name */
    for (const char* key : {"_x", "x9"}) {
        EXPECT_EQ(ref_of(f.get(), def(key)), key);
        c = check(f.get(), nullptr, key);
        ASSERT_EQ(c.status, ECUIO_OK) << key << ": " << c.detail;
        EXPECT_EQ(c.out.program.input_ids[0], def(key)->id) << key;
    }
    /* In a file that extends nothing it has no other name */
    File alone = load(file_of(2, {number("2nd", "Second", "D120")}));
    ASSERT_TRUE(alone) << ecuio_last_error_detail();
    EXPECT_EQ(ref_of(alone.get(), ecuio_signal_file_find(alone.get(), "2nd")),
              "!'Second' has no name in an expression: its key '2nd' starts "
              "with a digit, which an expression reads as a number");
}

// A reader's age bound on a signal in a base def's place is worded by the
// name the reader gave it, as the loader words it: the signal's key, or
// obd2's name for it
TEST(SignalExprTest, AnAgeBoundOnAStandInIsWordedByTheNameItIsReadBy)
{
    const char* const head = "\"extends\":\"obd2\",";
    File plain = load(file_of(2, {}, head));
    ASSERT_TRUE(plain) << ecuio_last_error_detail();
    const ecuio_signal_def_t* map = def_read_as(plain.get(), "obd2.map");
    ASSERT_NE(map, nullptr);
    const std::string named =
        "{\"key\":\"mymap\",\"name\":\"" + std::string(map->name) + "\",";
    const std::string number = named + "\"read\":{\"service\":\"22\","
                                       "\"number\":\"D121\"},"
                                       "\"field\":{\"byte\":0}}";
    const std::string turned = named + "\"kind\":\"computed\","
                                       "\"expr\":\"a * 2\"}";
    const std::string other = computed("q", "b + 1");
    const struct {
        const char* read_as;
        const char* words;
    } readers[] = {
        {"mymap", "signal 'p''s max_age names 'mymap', a computed signal"},
        {"obd2.map", "which 'N p' reads with an age bound"},
    };
    for (const auto& r : readers) {
        const std::string name = r.read_as;
        const std::string p =
            computed("p", name + " * 2", ",\"max_age\":{\"" + name + "\":1}");
        File f = load(file_of(2, {number, other, p}, head));
        ASSERT_TRUE(f) << name << ": " << ecuio_last_error_detail();
        const Checked c = check(f.get(), "mymap", "a * 2");
        EXPECT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT) << name;
        EXPECT_EQ(c.detail, load_words(file_of(2, {turned, other, p}, head)))
            << name;
        EXPECT_NE(c.detail.find(r.words), std::string::npos) << c.detail;
        EXPECT_EQ(c.out.at, 1u) << name;
    }
}

// A check's format is the grammar's, which format 2 changed last: a
// form's file that uses nothing of a later format is never raised to it
TEST(SignalExprTest, ACheckAnswersNoFormatPastTheGrammars)
{
    EXPECT_EQ(ecuio_sf::kGrammarFormat, 2u);
    EXPECT_GT(ECUIO_SIGNAL_FILE_FORMAT, ecuio_sf::kGrammarFormat);
    for (int format = 1; format <= static_cast<int>(ECUIO_SIGNAL_FILE_FORMAT);
         ++format) {
        File f = load(file_of(format));
        ASSERT_TRUE(f) << format << ": " << ecuio_last_error_detail();
        const Checked plain = check(f.get(), nullptr, "a + b");
        EXPECT_EQ(plain.status, ECUIO_OK) << plain.detail;
        EXPECT_EQ(plain.out.format, 1u) << "in a format " << format << " file";
        const Checked call = check(f.get(), nullptr, "max(a, b)");
        EXPECT_EQ(call.status, ECUIO_OK) << call.detail;
        EXPECT_EQ(call.out.format, 2u) << "in a format " << format << " file";
    }
}
