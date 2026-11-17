// File/layer: tests/test_signal_expr.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalExprTest (test_signals)
//
// An expression against a loaded file (ADR-0062): the check an editor
// runs and the name an expression reads a def by, each held to what a
// load of the file with the expression in it says.

#include <gtest/gtest.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct FileFree {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};
using File = std::unique_ptr<ecuio_signal_file_t, FileFree>;

File load(const std::string& json)
{
    ecuio_signal_file_t* f = nullptr;
    if (ecuio_signal_file_load(json.data(), json.size(), &f) != ECUIO_OK) {
        EXPECT_EQ(f, nullptr);
    }
    return File(f);
}

/* A computed signal's JSON */
std::string computed(const std::string& key, const std::string& expr,
                     const std::string& more = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"N " + key +
           "\",\"kind\":\"computed\",\"expr\":\"" + expr + "\"" + more + "}";
}

/* Four numbers, a to d (named A to D), then signals as given; head goes
 * before "signals" ("\"extends\":\"obd2\",") */
std::string file_of(int format, const std::vector<std::string>& signals = {},
                    const std::string& head = "")
{
    std::string s = "{\"havne_signals\":" + std::to_string(format) +
                    ",\"name\":\"T\",\"source\":{\"name\":\"t\"}," + head +
                    "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":[";
    const char* leaves = "abcd";
    for (int i = 0; i < 4; ++i) {
        char one[160];
        std::snprintf(
            one, sizeof one,
            "%s{\"key\":\"%c\",\"name\":\"%c\",\"read\":{\"service\":\"22\","
            "\"number\":\"D10%d\"},\"field\":{\"byte\":0}}",
            i == 0 ? "" : ",", leaves[i], leaves[i] - 'a' + 'A', i);
        s += one;
    }
    for (const std::string& one : signals) {
        s += "," + one;
    }
    return s + "]}";
}

/* What a check answered */
struct Checked {
    ecuio_status_t status = ECUIO_OK;
    ecuio_expr_check_t out{};
    std::string detail;
};

Checked check(const ecuio_signal_file_t* f, const char* key,
              const std::string& expr)
{
    Checked c;
    std::memset(&c.out, 0xA5, sizeof c.out);
    c.out.struct_size = sizeof c.out;
    c.status =
        ecuio_signal_file_check_expr(f, key, expr.data(), expr.size(), &c.out);
    c.detail = ecuio_last_error_detail();
    if (c.status == ECUIO_OK || c.out.at != 0) {
        EXPECT_EQ(c.out.struct_size, sizeof c.out);
        EXPECT_EQ(c.out.flags, 0u);
        for (const uint32_t r : c.out.reserved) {
            EXPECT_EQ(r, 0u);
        }
    }
    if (c.status == ECUIO_OK) {
        EXPECT_EQ(c.out.at, 0u) << expr;
        EXPECT_EQ(c.detail, "") << expr;
    } else if (c.out.at != 0) {
        const ecuio_computed_t none{};
        EXPECT_EQ(c.out.format, 0u) << expr;
        EXPECT_EQ(c.out.depth, 0u) << expr;
        EXPECT_EQ(std::memcmp(&c.out.program, &none, sizeof none), 0) << expr;
    }
    return c;
}

/* The words a refused load leaves, without their place in the file */
std::string load_words(const std::string& json)
{
    File f = load(json);
    EXPECT_FALSE(f) << json;
    const std::string detail = ecuio_last_error_detail();
    const size_t colon = detail.find(": ");
    EXPECT_EQ(detail.compare(0, 5, "line "), 0) << detail;
    return colon == std::string::npos ? detail : detail.substr(colon + 2);
}

/* A loader's "expr '...' <words> (at character N)" as the check says it:
 * "the expression <words>", and N into *at */
std::string as_checked(const std::string& words, uint32_t* at)
{
    const size_t open = words.find("expr '");
    const size_t close = words.find("' ", open + 6);
    const size_t place = words.rfind(" (at character ");
    if (open != 0 || close == std::string::npos || place == std::string::npos) {
        ADD_FAILURE() << "not an expr's refusal: " << words;
        return words;
    }
    *at = static_cast<uint32_t>(std::stoul(words.substr(place + 15)));
    return "the expression " + words.substr(close + 2, place - close - 2);
}

const ecuio_computed_t& program_of(const ecuio_signal_file_t* f,
                                   const char* key)
{
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    return ecuio_signal_file_set(f)->computed[def->table_first];
}

/* The name f's expressions read def by, or "!" and the refusal's detail */
std::string ref_of(const ecuio_signal_file_t* f, const ecuio_signal_def_t* def)
{
    char name[ECUIO_EXPR_REF_LEN];
    std::memset(name, 'x', sizeof name);
    if (ecuio_signal_file_expr_ref(f, def, name, sizeof name) != ECUIO_OK) {
        EXPECT_EQ(name[0], '\0');
        return std::string("!") + ecuio_last_error_detail();
    }
    return name;
}

/* The def of f's set an expression reads as name */
const ecuio_signal_def_t* def_read_as(const ecuio_signal_file_t* f,
                                      const std::string& name)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    for (uint32_t i = 0; i < set->count; ++i) {
        if (ref_of(f, &set->defs[i]) == name) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

} // namespace

// A refused expression's words are the loader's, and its place the
// loader's character: every refusal the compiler has, checked as a new
// signal "e" and loaded as one
TEST(SignalExprTest, ARefusedExpressionSaysWhatALoadOfItSays)
{
    const char* const exprs[] = {
        "a +",
        "a + bost",
        "obd2.map - a",
        "a b",
        "a $ b",
        "(a + b",
        "a + .",
        "1.2.3 + a",
        "3 + 4",
        "a > 3000",
        "if(a, 1, 0)",
        "(a > b) + 1",
        "-(a > b)",
        "max(a)",
        "min()",
        "clamp(a, 10, 1)",
        "pow(a, 2)",
        "if(a < b < c, 1, 0)",
        "a = b",
        "a, b",
        "max(a b)",
        "max(a, b",
        "max(a,)",
        "a + t",
        "a + b + c + d + n1",
        "a + b + c + d + a + b + c + d + a + b + c + d + a + b + c + d + "
        "max(a, b)",
        "abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs("
        "a)))))))))))))))))",
        "a+1+2+3+4+5+6+7+8+9+10+11+12+13+14+15+16+17",
        "",
        "   ",
    };
    const std::vector<std::string> more = {
        R"({"key":"t","name":"T","kind":"text","read":{"service":"22",)"
        R"("number":"D110"},"field":{"byte":0}})",
        R"({"key":"n1","name":"N1","read":{"service":"22","number":"D111"},)"
        R"("field":{"byte":0}})",
    };
    File base = load(file_of(2, more));
    ASSERT_TRUE(base) << ecuio_last_error_detail();
    for (const char* expr : exprs) {
        std::vector<std::string> with = more;
        with.push_back(computed("e", expr));
        uint32_t at = 0;
        const std::string want = as_checked(load_words(file_of(2, with)), &at);
        const Checked c = check(base.get(), "e", expr);
        EXPECT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT) << expr;
        EXPECT_EQ(c.detail, want) << expr;
        EXPECT_EQ(c.out.at, at) << expr;
        /* With no key it is the same expression */
        const Checked keyless = check(base.get(), nullptr, expr);
        EXPECT_EQ(keyless.detail, want) << expr;
        EXPECT_EQ(keyless.out.at, at) << expr;
    }
    /* Longer than the bound, at its first character */
    const std::string long_one = "a" + std::string(ECUIO_EXPR_MAX_BYTES, ' ');
    const Checked c = check(base.get(), nullptr, long_one);
    EXPECT_EQ(c.detail, "the expression is longer than 256 characters");
    EXPECT_EQ(c.out.at, 1u);
    EXPECT_EQ(check(base.get(), nullptr,
                    "a" + std::string(ECUIO_EXPR_MAX_BYTES - 1, ' '))
                  .status,
              ECUIO_OK)
        << "256 bytes are taken";
    /* A byte the grammar has no use for, wherever it is: a NUL the length
     * carries, and a character outside ASCII */
    const Checked nul = check(base.get(), nullptr, std::string("a + \0b", 6));
    EXPECT_EQ(nul.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(nul.out.at, 5u) << nul.detail;
    const Checked wide = check(base.get(), nullptr, "a + \xC3\xA9");
    EXPECT_EQ(wide.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(wide.out.at, 5u) << wide.detail;
}

// An expression that passes compiles to the program a load of it gives,
// at the oldest format whose loader takes it
TEST(SignalExprTest, ACheckedExpressionIsTheProgramALoadGives)
{
    struct Case {
        const char* expr;
        uint32_t format;
    };
    const Case cases[] = {
        {"a", 1},
        {"a + b * 2", 1},
        {"(a - b) / (c + 0.5)", 1},
        {"-a + 3", 1},
        {"a + b + c + d", 1},
        {"max(a, b)", 2},
        {"clamp(a, 0, 100)", 2},
        {"if(a > b, c, d)", 2},
        {"if(all(a > 1, not(b <= 2)), sqrt(c), abs(d))", 2},
        {"round(a) + floor(b) + ceil(c)", 2},
        {"x * 2", 2},
        {"if(any(a == b, a != c), min(a, b, c), 0)", 2},
    };
    const std::vector<std::string> more = {computed("x", "a + 1")};
    File base = load(file_of(2, more));
    ASSERT_TRUE(base) << ecuio_last_error_detail();
    for (const Case& c : cases) {
        std::vector<std::string> with = more;
        with.push_back(computed("e", c.expr));
        File loaded = load(file_of(2, with));
        ASSERT_TRUE(loaded) << c.expr << ": " << ecuio_last_error_detail();
        const Checked got = check(base.get(), "e", c.expr);
        ASSERT_EQ(got.status, ECUIO_OK) << c.expr << ": " << got.detail;
        EXPECT_EQ(std::memcmp(&got.out.program, &program_of(loaded.get(), "e"),
                              sizeof(ecuio_computed_t)),
                  0)
            << c.expr;
        EXPECT_EQ(got.out.format, c.format) << c.expr;
        EXPECT_EQ(got.out.depth, std::string(c.expr) == "x * 2" ? 2u : 1u)
            << c.expr;
        /* The format it names is the oldest that loads it */
        EXPECT_EQ(load(file_of(1, {computed("e", c.expr)})) != nullptr,
                  c.format == 1 && std::string(c.expr) != "x * 2")
            << c.expr;
        const Checked keyless = check(base.get(), nullptr, c.expr);
        EXPECT_EQ(std::memcmp(&keyless.out, &got.out, sizeof got.out), 0)
            << c.expr;
    }
    /* A format 1 file's check is not held to format 1: it says 2 */
    File old = load(file_of(1));
    ASSERT_TRUE(old) << ecuio_last_error_detail();
    const Checked got = check(old.get(), nullptr, "max(a, b)");
    EXPECT_EQ(got.status, ECUIO_OK) << got.detail;
    EXPECT_EQ(got.out.format, 2u);
    EXPECT_EQ(check(old.get(), nullptr, "a - b").out.format, 1u);
}

// The key names the signal the expression is for: an edit is judged with
// the expression in that signal's place, as the file would hold it
TEST(SignalExprTest, AnEditIsJudgedInItsSignalsPlace)
{
    const std::vector<std::string> chain = {computed("e", "a + 1"),
                                            computed("e1", "e * 2"),
                                            computed("e2", "e1 + b")};
    File f = load(file_of(2, chain));
    ASSERT_TRUE(f) << ecuio_last_error_detail();

    /* A circle the edit closes, in the loader's words, at the input the
     * circle leaves by */
    Checked c = check(f.get(), "e", "a + e2");
    EXPECT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(c.detail, load_words(file_of(
                            2, {computed("e", "a + e2"), chain[1], chain[2]})));
    EXPECT_EQ(c.detail, "signal 'e' reads 'e2', which reads 'e1', which reads "
                        "'e': computed signals can't read in a circle");
    EXPECT_EQ(c.out.at, 5u);
    /* The loader names the first signal of a circle in the file's order,
     * which need not be the one edited: the place is still its own input */
    c = check(f.get(), "e2", "b + 0 + e2 + e");
    EXPECT_EQ(
        c.detail,
        "signal 'e2' reads 'e2': computed signals can't read in a circle");
    EXPECT_EQ(c.out.at, 9u);
    c = check(f.get(), "e1", "1 + e2");
    EXPECT_EQ(
        c.detail,
        load_words(file_of(2, {chain[0], computed("e1", "1 + e2"), chain[2]})));
    EXPECT_EQ(c.out.at, 5u);
    /* The same expressions for a new signal close nothing */
    EXPECT_EQ(check(f.get(), nullptr, "a + e2").status, ECUIO_OK);
    EXPECT_EQ(check(f.get(), "fresh", "a + e2").out.depth, 4u);
    /* A new signal reading itself */
    c = check(f.get(), "fresh", "a + fresh");
    EXPECT_EQ(c.detail, "signal 'fresh' reads 'fresh': computed signals can't "
                        "read in a circle");
    EXPECT_EQ(c.out.at, 5u);
    /* An edit that breaks the chain it was in passes */
    c = check(f.get(), "e1", "a * 2");
    EXPECT_EQ(c.status, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.out.depth, 1u);
    EXPECT_EQ(c.out.format, 2u) << "e2 reads it, which format 1 can't";

    /* 8 deep is taken; a ninth is refused where the loader refuses it */
    std::vector<std::string> deep = {computed("x", "a + 1"),
                                     computed("e", "a + 1")};
    for (int k = 1; k < 8; ++k) {
        deep.push_back(
            computed("e" + std::to_string(k),
                     (k == 1 ? std::string("e") : "e" + std::to_string(k - 1)) +
                         " + 1"));
    }
    File d = load(file_of(2, deep));
    ASSERT_TRUE(d) << ecuio_last_error_detail();
    c = check(d.get(), "z", "1 + e7");
    EXPECT_EQ(c.detail, "signal 'z' sits 9 computed signals deep: a chain goes "
                        "8 deep at most");
    EXPECT_EQ(c.out.at, 5u);
    std::vector<std::string> with = deep;
    with.push_back(computed("z", "1 + e7"));
    EXPECT_EQ(c.detail, load_words(file_of(2, with)));
    c = check(d.get(), nullptr, "x + e7");
    EXPECT_EQ(c.detail, "the expression sits 9 computed signals deep: a chain "
                        "goes 8 deep at most");
    EXPECT_EQ(c.out.at, 5u) << "its deepest input";
    EXPECT_EQ(check(d.get(), nullptr, "x + e6").out.depth, 8u);
    /* An edit that pushes a reader past the bound: the reader is named */
    c = check(d.get(), "e", "x + 1");
    with = deep;
    with[1] = computed("e", "x + 1");
    EXPECT_EQ(c.detail, load_words(file_of(2, with)));
    EXPECT_EQ(c.detail, "signal 'e7' sits 9 computed signals deep: a chain "
                        "goes 8 deep at most");
    EXPECT_EQ(c.out.at, 1u);

    /* A number turned computed under a reader that keeps an age bound on
     * it, and under one that doesn't */
    const std::string aged = computed("p", "a + b", ",\"max_age\":{\"a\":1}");
    File g = load(file_of(2, {aged}));
    ASSERT_TRUE(g) << ecuio_last_error_detail();
    c = check(g.get(), "a", "b * 2");
    EXPECT_EQ(c.detail, "signal 'p''s max_age names 'a', a computed signal: "
                        "its own inputs carry their ages");
    EXPECT_EQ(c.out.at, 1u);
    c = check(g.get(), "b", "c * 2");
    EXPECT_EQ(c.status, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.out.format, 2u) << "p reads it";
    EXPECT_EQ(check(g.get(), "c", "d * 2").out.format, 1u) << "nothing reads c";
    /* A text signal's place taken by an expression */
    File t = load(file_of(
        1, {R"({"key":"t","name":"T","kind":"text","read":{"service":"22",)"
            R"("number":"D110"},"field":{"byte":0}})"}));
    ASSERT_TRUE(t) << ecuio_last_error_detail();
    EXPECT_EQ(check(t.get(), "t", "a + 1").status, ECUIO_OK);
    EXPECT_EQ(check(t.get(), "t", "t + 1").detail,
              "signal 't' reads 't': computed signals can't read in a circle");
}

// A signal named as a base def stands in its place, so "obd2.<key>" and
// obd2's own Boost read it: the loader's rules for that hold in a check
TEST(SignalExprTest, ASignalInABaseDefsPlaceIsJudgedAsTheLoaderJudgesIt)
{
    const char* const head = "\"extends\":\"obd2\",";
    File plain = load(file_of(2, {}, head));
    ASSERT_TRUE(plain) << ecuio_last_error_detail();
    const ecuio_signal_def_t* map = def_read_as(plain.get(), "obd2.map");
    const ecuio_signal_def_t* baro = def_read_as(plain.get(), "obd2.baro");
    const ecuio_signal_def_t* rpm = def_read_as(plain.get(), "obd2.rpm");
    ASSERT_NE(map, nullptr);
    ASSERT_NE(baro, nullptr);
    ASSERT_NE(rpm, nullptr);
    const auto number = [](const std::string& key, const std::string& name,
                           const char* did) {
        return "{\"key\":\"" + key + "\",\"name\":\"" + name +
               "\",\"read\":{\"service\":\"22\",\"number\":\"" + did +
               "\"},\"field\":{\"byte\":0}}";
    };
    const auto named = [](const std::string& key, const std::string& name,
                          const std::string& expr) {
        return "{\"key\":\"" + key + "\",\"name\":\"" + name +
               "\",\"kind\":\"computed\",\"expr\":\"" + expr + "\"}";
    };

    /* obd2's Boost reads baro with an age bound, which a computed input
     * can't carry */
    const std::string own_baro = number("mybaro", baro->name, "D120");
    File f = load(file_of(2, {own_baro}, head));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    Checked c = check(f.get(), "mybaro", "a * 2");
    EXPECT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(c.detail, load_words(file_of(
                            2, {named("mybaro", baro->name, "a * 2")}, head)));
    EXPECT_NE(c.detail.find("which 'Boost' reads with an age bound"),
              std::string::npos)
        << c.detail;
    EXPECT_EQ(c.out.at, 1u);

    /* MAP it reads with none: a computed signal there is format 2's */
    const std::string own_map = number("mymap", map->name, "D121");
    f = load(file_of(2, {own_map}, head));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    c = check(f.get(), "mymap", "a * 2");
    EXPECT_EQ(c.status, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.out.format, 2u);
    EXPECT_FALSE(load(file_of(1, {named("mymap", map->name, "a * 2")}, head)));
    EXPECT_TRUE(load(file_of(2, {named("mymap", map->name, "a * 2")}, head)))
        << ecuio_last_error_detail();
    /* An expression reading the computed stand-in by obd2's name needs
     * format 2 as well, though its own forms are format 1's */
    f = load(file_of(2, {named("mymap", map->name, "a * 2")}, head));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    c = check(f.get(), nullptr, "obd2.map + 1");
    EXPECT_EQ(c.status, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.out.format, 2u);
    EXPECT_EQ(c.out.depth, 2u);
    EXPECT_EQ(c.out.program.input_ids[0], map->id);

    /* A text signal in a base def's place can't be read by obd2's name */
    const std::string text_rpm = "{\"key\":\"trpm\",\"name\":\"" +
                                 std::string(rpm->name) +
                                 "\",\"kind\":\"text\",\"read\":{\"service\":"
                                 "\"22\",\"number\":\"D122\"},"
                                 "\"field\":{\"byte\":0}}";
    f = load(file_of(2, {text_rpm, named("k", "K", "a + 1")}, head));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    c = check(f.get(), "k", "1 + obd2.rpm");
    EXPECT_EQ(c.detail,
              load_words(file_of(2, {text_rpm, named("k", "K", "1 + obd2.rpm")},
                                 head)));
    EXPECT_NE(
        c.detail.find("which 'K' reads: a computed signal reads a number"),
        std::string::npos)
        << c.detail;
    EXPECT_EQ(c.out.at, 5u);
    c = check(f.get(), nullptr, "obd2.rpm");
    EXPECT_NE(c.detail.find("which the expression reads: a computed signal "
                            "reads a number"),
              std::string::npos)
        << c.detail;

    /* Boost over a stand-in 8 deep would sit 9 deep */
    std::vector<std::string> deep = {named("x", "X", "a + 1"),
                                     named("e", "E", "a + 1")};
    for (int k = 1; k < 7; ++k) {
        const std::string key = "e" + std::to_string(k);
        deep.push_back(
            named(key, k == 6 ? map->name : "N " + key,
                  (k == 1 ? std::string("e") : "e" + std::to_string(k - 1)) +
                      " + 1"));
    }
    f = load(file_of(2, deep, head));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    EXPECT_EQ(check(f.get(), nullptr, "obd2.map").out.depth, 8u);
    c = check(f.get(), "e", "x + 1");
    std::vector<std::string> with = deep;
    with[1] = named("e", "E", "x + 1");
    EXPECT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(c.detail, load_words(file_of(2, with, head)));
    EXPECT_NE(c.detail.find("that puts 'Boost' 9 computed signals deep"),
              std::string::npos)
        << c.detail;
}

// The name an expression reads a def by, for every def of a file: its
// own key, obd2's key for a base def, and compiling the name reads it
TEST(SignalExprTest, EveryDefIsReadByTheNameItIsGiven)
{
    const std::string text =
        R"({"key":"t","name":"T","kind":"text","read":{"service":"22",)"
        R"("number":"D110"},"field":{"byte":0}})";
    File f = load(
        file_of(2, {text, computed("e", "a + 1")}, "\"extends\":\"obd2\","));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    uint32_t named = 0;
    uint32_t unnamed = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const std::string name = ref_of(f.get(), &d);
        const std::string key = ecuio_signal_file_key(f.get(), &d);
        if (name[0] == '!') {
            /* obd2's computed Boost alone has no name */
            EXPECT_TRUE(key.empty()) << d.name;
            EXPECT_NE(d.flags & ECUIO_DEF_FLAG_COMPUTED, 0u) << d.name;
            EXPECT_EQ(name, std::string("!'") + d.name +
                                "' has no name in an expression: obd2's keys "
                                "name its Service 01 fields");
            ++unnamed;
            continue;
        }
        ++named;
        EXPECT_LT(name.size(), size_t{ECUIO_EXPR_REF_LEN});
        if (!key.empty()) {
            EXPECT_EQ(name, key);
        } else {
            EXPECT_EQ(name.compare(0, 5, "obd2."), 0) << name;
        }
        const Checked c = check(f.get(), nullptr, name);
        if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            EXPECT_EQ(c.detail,
                      "the expression reads '" + name + "', which is text");
            continue;
        }
        ASSERT_EQ(c.status, ECUIO_OK) << name << ": " << c.detail;
        EXPECT_EQ(c.out.program.input_count, 1u) << name;
        EXPECT_EQ(c.out.program.input_ids[0], d.id) << name;
    }
    EXPECT_EQ(unnamed, 1u);
    EXPECT_GT(named, 100u);

    /* The short names, and a PID's fields */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const auto id_of = [&](const char* name) {
        const ecuio_signal_def_t* d = def_read_as(f.get(), name);
        return d != nullptr ? d->id : uint16_t{0};
    };
    EXPECT_EQ(id_of("obd2.rpm"), ECUIO_OBD2_SIG_RPM_ID);
    EXPECT_EQ(id_of("obd2.coolant"), ECUIO_OBD2_SIG_COOLANT_TEMP_ID);
    EXPECT_EQ(id_of("obd2.speed"), ECUIO_OBD2_SIG_SPEED_ID);
    EXPECT_EQ(id_of("obd2.pid_14"), ECUIO_OBD2_PID_SIG_ID(0x14, 0));
    EXPECT_EQ(id_of("obd2.pid_14_1"), ECUIO_OBD2_PID_SIG_ID(0x14, 1));
    EXPECT_EQ(id_of("obd2.pid_0c"), 0u) << "a PID with a short name goes by it";
    EXPECT_EQ(ref_of(f.get(), ecuio_signal_file_find(f.get(), "e")), "e");
    EXPECT_EQ(ref_of(f.get(), ecuio_signal_file_find(f.get(), "t")), "t");

    /* Not the file's: the base set's own def, and a copy */
    EXPECT_EQ(ref_of(f.get(), &obd2->defs[0]),
              "!the def isn't one of the file's");
    const ecuio_signal_def_t copy = set->defs[0];
    EXPECT_EQ(ref_of(f.get(), &copy), "!the def isn't one of the file's");
    /* A file that extends nothing names its own alone */
    File alone = load(file_of(1));
    ASSERT_TRUE(alone) << ecuio_last_error_detail();
    EXPECT_EQ(ref_of(alone.get(), ecuio_signal_file_find(alone.get(), "a")),
              "a");
    EXPECT_EQ(ref_of(alone.get(), &set->defs[0]),
              "!the def isn't one of the file's");

    /* The buffer: the name and its NUL, or nothing */
    const ecuio_signal_def_t* map = def_read_as(f.get(), "obd2.map");
    ASSERT_NE(map, nullptr);
    char small[9];
    std::memset(small, 'x', sizeof small);
    EXPECT_EQ(ecuio_signal_file_expr_ref(f.get(), map, small, 8),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the name 'obd2.map' needs a buffer of 9 bytes, and this one "
                 "holds 8");
    EXPECT_EQ(small[0], '\0');
    EXPECT_EQ(small[8], 'x');
    EXPECT_EQ(ecuio_signal_file_expr_ref(f.get(), map, small, 9), ECUIO_OK);
    EXPECT_STREQ(small, "obd2.map");
    EXPECT_EQ(ecuio_signal_file_expr_ref(f.get(), map, small, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_expr_ref(nullptr, map, small, 9),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_expr_ref(f.get(), nullptr, small, 9),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_expr_ref(f.get(), map, nullptr, 9),
              ECUIO_ERR_NULL_ARGUMENT);
}

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
