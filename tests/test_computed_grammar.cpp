// File/layer: tests/test_computed_grammar.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest (test_signals)
//
// The computed grammar (ADR-0062): format 2's calls, comparisons and if()
// compiled from a signal file, each refusal with its place, a format 1
// file held to format 1 and compiling to the programs it always did, and
// random expression trees run against a walk of the tree.

#include <gtest/gtest.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include "computed.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
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

const double kNone = std::numeric_limits<double>::quiet_NaN();

/* Four numbers, a to d, then each expr a computed signal: e, then e1... */
std::string file_of(int format, const std::vector<std::string>& exprs,
                    const std::string& extra = "")
{
    std::string s = "{\"havne_signals\":" + std::to_string(format) +
                    ",\"name\":\"T\",\"source\":{\"name\":\"t\"},"
                    "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":[";
    const char* leaves = "abcd";
    for (int i = 0; i < 4; ++i) {
        char one[160];
        std::snprintf(
            one, sizeof one,
            "{\"key\":\"%c\",\"name\":\"%c\",\"read\":{\"service\":\"22\","
            "\"number\":\"D10%d\"},\"field\":{\"byte\":0}},",
            leaves[i], leaves[i] - 'a' + 'A', i);
        s += one;
    }
    s += extra;
    for (size_t i = 0; i < exprs.size(); ++i) {
        const std::string key = i == 0 ? "e" : "e" + std::to_string(i);
        s += "{\"key\":\"" + key + "\",\"name\":\"" + key +
             "\",\"kind\":\"computed\",\"expr\":\"" + exprs[i] + "\"}";
        s += i + 1 < exprs.size() ? "," : "";
    }
    return s + "]}";
}

/* The loader's words for an expr, or "" when it loads */
std::string words_of(int format, const std::string& expr)
{
    File f = load(file_of(format, {expr}));
    return f ? std::string() : std::string(ecuio_last_error_detail());
}

/* expr's value at a to d, as bytes in hex, "none", or the loader's words */
std::string value_of(int format, const std::string& expr, double a = 0,
                     double b = 0, double c = 0, double d = 0)
{
    File f = load(file_of(format, {expr}));
    if (!f) {
        return ecuio_last_error_detail();
    }
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f.get(), "e");
    const ecuio_computed_t& p =
        ecuio_signal_file_set(f.get())->computed[def->table_first];
    if (!ecuio_computed_check(p)) {
        return "the checker refuses it";
    }
    const double leaves[4] = {a, b, c, d};
    double in[4] = {};
    for (uint32_t k = 0; k < p.input_count; ++k) {
        in[k] = leaves[p.input_ids[k] - ECUIO_USER_SIG_ID_BASE];
    }
    double out = 0.0;
    if (!ecuio_computed_run(p, in, &out)) {
        return "none";
    }
    uint64_t bits = 0;
    std::memcpy(&bits, &out, sizeof bits);
    char text[24];
    std::snprintf(text, sizeof text, "%016llx",
                  static_cast<unsigned long long>(bits));
    return text;
}

std::string bits(double v)
{
    uint64_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    char text[24];
    std::snprintf(text, sizeof text, "%016llx",
                  static_cast<unsigned long long>(b));
    return text;
}

/* FNV-1a 64 over a program's bytes */
uint64_t hash_of(const ecuio_computed_t& p)
{
    unsigned char raw[sizeof p];
    std::memcpy(raw, &p, sizeof p);
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char c : raw) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace

TEST(SignalFileTest, EachFunctionAndComparisonComputes)
{
    const auto v = [](const std::string& e, double a, double b = 0,
                      double c = 0,
                      double d = 0) { return value_of(2, e, a, b, c, d); };
    EXPECT_EQ(v("min(a, b, c)", 3, -2, 5), bits(-2));
    EXPECT_EQ(v("max(a, b, c, d)", 3, -2, 5, 4), bits(5));
    EXPECT_EQ(v("abs(a - b)", 1, 3), bits(2));
    EXPECT_EQ(v("clamp(a, 0, 100)", 104.5), bits(100));
    EXPECT_EQ(v("clamp(a, 0, 100)", -3), bits(0));
    EXPECT_EQ(v("clamp(a, 0, 100)", 42), bits(42));
    EXPECT_EQ(v("sqrt(a * a + b * b)", 3, 4), bits(5));
    EXPECT_EQ(v("sqrt(a)", -1), "none");
    EXPECT_EQ(v("round(a)", 2.5), bits(3));
    EXPECT_EQ(v("round(a)", -2.5), bits(-3));
    EXPECT_EQ(v("floor(a)", -2.5), bits(-3));
    EXPECT_EQ(v("ceil(a)", -2.5), bits(-2));
    EXPECT_EQ(v("if(a > 0, b * 60 / a, 0)", 800, 12.3), bits(12.3 * 60 / 800));
    EXPECT_EQ(v("if(a > 0, b * 60 / a, 0)", 0, 12.3), bits(0));
    EXPECT_EQ(v("if(a == 0, 0, b / c)", 0, 50, kNone), bits(0))
        << "the branch not taken reads a signal with no value";
    EXPECT_EQ(v("if(all(a > 0, b > 0), 1, 0)", 1, 1), bits(1));
    EXPECT_EQ(v("if(all(a > 0, b > 0, c > 0), 1, 0)", 1, 1, -1), bits(0));
    EXPECT_EQ(v("if(any(a > 0, b > 0, c > 0), 1, 0)", -1, -1, 1), bits(1));
    EXPECT_EQ(v("if(not(a > 0), 1, 0)", -1), bits(1));
    struct Cmp {
        const char* op;
        double below, equal, above; /* 1, 2, 3 against 2 */
    };
    const Cmp cmps[] = {{"<", 1, 0, 0},  {"<=", 1, 1, 0}, {">", 0, 0, 1},
                        {">=", 0, 1, 1}, {"==", 0, 1, 0}, {"!=", 1, 0, 1}};
    for (const Cmp& c : cmps) {
        const std::string e = std::string("if(a ") + c.op + " b, 1, 0)";
        EXPECT_EQ(v(e, 1, 2), bits(c.below)) << c.op;
        EXPECT_EQ(v(e, 2, 2), bits(c.equal)) << c.op;
        EXPECT_EQ(v(e, 3, 2), bits(c.above)) << c.op;
    }
}

// A comparison binds looser than + and -, so each side is a sum
TEST(SignalFileTest, AComparisonBindsLooserThanASum)
{
    const auto v = [](const std::string& e, double a, double b) {
        return value_of(2, e, a, b);
    };
    EXPECT_EQ(v("if(a + 1 > b * 2, 1, 0)", 4, 2), bits(1)) << "5 > 4";
    EXPECT_EQ(v("if(a + 1 > b * 2, 1, 0)", 2, 2), bits(0)) << "3 > 4";
    EXPECT_EQ(v("if(a - b < 0 - 1, 1, 0)", 1, 3), bits(1)) << "-2 < -1";
    EXPECT_EQ(v("if((a > b), a, b)", 1, 3), bits(3))
        << "a comparison in parentheses";
    EXPECT_EQ(v("max (a, b)", 1, 3), bits(3)) << "spaces before a call's '('";
    EXPECT_EQ(v("min(a,b)+max(a,b)", 1, 3), bits(4));
    EXPECT_EQ(v("-max(a, b)", 1, 3), bits(-3));
    EXPECT_EQ(v("if(a>=b,a,b)*2", 5, 3), bits(10));
}

// A call is a name before '(', so a signal keyed as a function's name
// reads as a signal everywhere else
TEST(SignalFileTest, ASignalKeyedAsAFunctionIsStillASignal)
{
    const std::string keyed =
        R"({"key":"min","name":"Min","read":{"service":"22","number":"D104"},"field":{"byte":0}},)"
        R"({"key":"if","name":"If","read":{"service":"22","number":"D105"},"field":{"byte":0}},)";
    for (const int format : {1, 2}) {
        File f = load(file_of(format, {"min + if"}, keyed));
        ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
        const ecuio_computed_t& p = ecuio_signal_file_set(f.get())->computed[0];
        EXPECT_EQ(p.input_count, 2u) << format;
        EXPECT_EQ(p.input_ids[0], ECUIO_USER_SIG_ID_BASE + 4) << format;
        EXPECT_EQ(p.input_ids[1], ECUIO_USER_SIG_ID_BASE + 5) << format;
    }
    File f = load(file_of(2, {"min(min, if)"}, keyed));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_computed_t& p = ecuio_signal_file_set(f.get())->computed[0];
    EXPECT_EQ(p.step_count, 3u);
    EXPECT_EQ(p.steps[2], ECUIO_COMPUTED_MIN)
        << "the call, over the two signals";
}

// Every committed format 1 program, and a corpus of format 1 forms, compile
// to the bytes the format 1 compiler gave (d14011e9's, hashed by
// FNV-1a 64), in a file of either format
TEST(SignalFileTest, AFormat1ExpressionCompilesAsItAlwaysHas)
{
    struct Row {
        const char* expr;
        uint64_t hash;
    };
    const Row corpus[] = {
        {"a", 0x5F64AED7788B4004ULL},
        {"a + b", 0x526A0B3EF26CEDE4ULL},
        {"a - b - c", 0x87D63C08510A2ABCULL},
        {"a - (b - c)", 0x67CCDF92C12B3F8EULL},
        {"a * b + c", 0x150B9AFBFD519182ULL},
        {"a + b * c", 0x3A64B0D1B9585042ULL},
        {"(a + b) * c", 0xF4D3DC353C05C70AULL},
        {"a / b / c", 0xAE6EEFEDC2AA955CULL},
        {"a / (b / c)", 0x3721A7ADE6641136ULL},
        {"-a", 0xF6DB723030D66B7AULL},
        {"--a", 0xCF5FE8DA7FEA589FULL},
        {"-(a + b)", 0x88E6851A249BC2B8ULL},
        {"a * -b", 0xA9575F7AB7F7C864ULL},
        {"a - -1", 0x38C1FD2BF439A11CULL},
        {"2 * a + 2", 0x7E4550797BA49263ULL},
        {"0.5 * a - 0.5", 0x19522BD633A7F5EBULL},
        {".5 * a", 0x7F96CAADD94A59BCULL},
        {"a * 1.5 + b * 1.5 + 3", 0x8E4DB3D94E2E8706ULL},
        {"((((a))))", 0x5F64AED7788B4004ULL},
        {"a+b*c-d/a", 0x8F4CE0B751506644ULL},
        {"  a  *\\tb  ", 0x689E15ED54E085B6ULL},
        {"100 - a / 2.55", 0x87DE417FC7E08C4EULL},
        {"a * 0.1 * 0.1", 0xE85C7B42C348A5E8ULL},
        {"(a - b) / (c - d)", 0x08D5349AB7A1FD4FULL},
        {"a + 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15",
         0x5A45183052C6B949ULL},
        {"a + a + a + a + a + a + a + a + a + a + a + a + a + a + a + a + a",
         0xE81C57BB28CADD64ULL},
        {"a - 0 + 0", 0xAFF69FC4C6C42628ULL},
        {"0 - a", 0xF6DB723030D66B7AULL},
        {"3.0 * a + 3", 0xDFD59A3FFC8AF01BULL},
        {"1234567.125 * a", 0x9208E29D19D6C361ULL},
        {"obd2.map - obd2.baro", 0xF5582FC1ECF594CDULL},
        {"(obd2.rpm * obd2.load) / 100", 0x67946EE26567E414ULL},
        {"obd2.pid_14_1 * 2", 0x8E53A1883AE8835EULL},
        {"a + obd2.maf", 0xDFD6AD806518C836ULL},
        {"obd2.rpm / 1000", 0xCD6D3FACB112A97AULL},
        {"(a + b + c + d) / 4", 0x059F5FD06103B520ULL},
    };
    for (const int format : {1, 2}) {
        std::string text = "{\"havne_signals\":" + std::to_string(format) +
                           ",\"name\":\"Corpus\",\"source\":{\"name\":\"t\"},"
                           "\"extends\":\"obd2\",\"signals\":[";
        for (int i = 0; i < 4; ++i) {
            text += "{\"key\":\"" + std::string(1, static_cast<char>('a' + i)) +
                    "\",\"name\":\"" +
                    std::string(1, static_cast<char>('A' + i)) +
                    "\",\"read\":{\"service\":\"22\",\"number\":\"F40" +
                    std::to_string(i) + "\"},\"field\":{\"byte\":0}},";
        }
        for (size_t i = 0; i < sizeof corpus / sizeof corpus[0]; ++i) {
            text += "{\"key\":\"c" + std::to_string(i) + "\",\"name\":\"C" +
                    std::to_string(i) + "\",\"kind\":\"computed\",\"expr\":\"" +
                    corpus[i].expr + "\"},";
        }
        text +=
            R"({"key":"aged","name":"Aged","kind":"computed","expr":"a - obd2.baro",)"
            R"("max_age":{"obd2.baro":30,"a":0.25}}]})";
        File f = load(text);
        ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
        const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
        for (size_t i = 0; i < sizeof corpus / sizeof corpus[0]; ++i) {
            const ecuio_signal_def_t* d = ecuio_signal_file_find(
                f.get(), ("c" + std::to_string(i)).c_str());
            EXPECT_EQ(hash_of(set->computed[d->table_first]), corpus[i].hash)
                << "format " << format << ": " << corpus[i].expr;
        }
        const ecuio_signal_def_t* aged =
            ecuio_signal_file_find(f.get(), "aged");
        EXPECT_EQ(hash_of(set->computed[aged->table_first]),
                  0xB1A8FE0B72372FE3ULL)
            << "format " << format << ", with its ages";
    }
}

TEST(SignalFileTest, AFormat1FileRefusesEachFormat2Form)
{
    struct Case {
        const char* expr;
        const char* words;
    };
    const Case cases[] = {
        {"max(a, b)",
         "uses max(), which needs \"havne_signals\": 2 (at character 1)"},
        {"a + min(a, b)",
         "uses min(), which needs \"havne_signals\": 2 (at character 5)"},
        {"abs(a)", "uses abs()"},
        {"clamp(a, 0, 1)", "uses clamp()"},
        {"sqrt(a)", "uses sqrt()"},
        {"round(a)", "uses round()"},
        {"floor(a)", "uses floor()"},
        {"ceil(a)", "uses ceil()"},
        {"if(a > 0, 1, 0)", "uses if()"},
        {"all(a > 0, b > 0)", "uses all()"},
        {"any(a > 0, b > 0)", "uses any()"},
        {"not(a > 0)", "uses not()"},
        {"a < b",
         "uses '<', which needs \"havne_signals\": 2 (at character 3)"},
        {"a <= b", "uses '<='"},
        {"a > b", "uses '>'"},
        {"a >= b", "uses '>='"},
        {"a == b", "uses '=='"},
        {"a != b", "uses '!='"},
    };
    for (const Case& c : cases) {
        const std::string got = words_of(1, c.expr);
        EXPECT_NE(got.find(c.words), std::string::npos)
            << c.expr << "\n  got: " << got << "\n  want: " << c.words;
        EXPECT_EQ(words_of(2, c.expr).find("havne_signals"), std::string::npos)
            << c.expr << " in format 2: " << words_of(2, c.expr);
    }
    EXPECT_NE(words_of(1, "(a < b) + 1").find("uses '<'"), std::string::npos)
        << "inside parentheses too";
    EXPECT_NE(
        words_of(1, "pow(a, 2)").find("calls pow(), which isn't a function"),
        std::string::npos)
        << "a name that is no function says so in either format";
    EXPECT_NE(words_of(1, "pow(a, 2)")
                  .find("and a format 1 file calls none (functions need "
                        "\"havne_signals\": 2) (at character 1)"),
              std::string::npos)
        << "format 1 names the format, not format 2's list: "
        << words_of(1, "pow(a, 2)");
    EXPECT_EQ(words_of(1, "pow(a, 2)").find("min, max"), std::string::npos);
    EXPECT_EQ(words_of(2, "pow(a, 2)").find("format 1"), std::string::npos)
        << "format 2 lists its functions: " << words_of(2, "pow(a, 2)");
    EXPECT_NE(words_of(1, "a = b")
                  .find("has '=' where an operator or the end should be"),
              std::string::npos)
        << "format 1's own words for what it always refused";
    EXPECT_NE(words_of(1, "a, b").find("has ',' where an operator"),
              std::string::npos);
    EXPECT_NE(words_of(1, "a+a+a+a+a+a+a+a+a+a+a+a+a+a+a+a+a+a")
                  .find("has more than 16 operations"),
              std::string::npos);
}

// Every refusal the grammar adds, its words and its place in the expr
TEST(SignalFileTest, EveryGrammarRefusalSaysWhereAndWhy)
{
    struct Case {
        const char* expr;
        const char* words;
    };
    const Case cases[] = {
        {"if(a, 1, 0)",
         "gives if() a number where a comparison should be (at character 4)"},
        {"a > 3000", "is a comparison where a number should be: if(a > 3000, "
                     "1, 0) makes one (at character 1)"},
        {"(a > b) + 1",
         "adds a comparison where a number should be (at character 2)"},
        {"1 + (a > b)",
         "adds a comparison where a number should be (at character 6)"},
        {"(a > b) - 1", "subtracts a comparison"},
        {"(a > b) * 2", "multiplies a comparison"},
        {"a / (a > b)", "divides a comparison"},
        {"-(a > b)",
         "negates a comparison where a number should be (at character 3)"},
        {"if((a > b) < c, 1, 0)",
         "compares a comparison where a number should be (at character 5)"},
        {"if(a < (b > c), 1, 0)", "compares a comparison"},
        {"max(a, b > c)",
         "gives max() a comparison where a number should be (at character 8)"},
        {"if(a > b, a > b, 0)",
         "gives if() a comparison where a number should be (at character 11)"},
        {"if(a > b, 1, a > b)",
         "gives if() a comparison where a number should be (at character 14)"},
        {"if(all(a, b), 1, 0)",
         "gives all() a number where a comparison should be (at character 8)"},
        {"if(any(a > b, c), 1, 0)",
         "gives any() a number where a comparison should be (at character 15)"},
        {"if(not(a), 1, 0)",
         "gives not() a number where a comparison should be (at character 8)"},
        {"max(a)",
         "gives max() 1 value where it takes 2 or more (at character 1)"},
        {"min()", "gives min() 0 values where it takes 2 or more"},
        {"abs(a, b)", "gives abs() 2 values where it takes 1"},
        {"clamp(a, 1)", "gives clamp() 2 values where it takes 3"},
        {"if(a > b, 1)",
         "gives if() 2 parts where it takes 3 (at character 1)"},
        {"if(all(a > b), 1, 0)",
         "gives all() 1 part where it takes 2 or more (at character 4)"},
        {"if(not(a > b, a > c), 1, 0)", "gives not() 2 parts where it takes 1"},
        {"pow(a, 2)",
         "calls pow(), which isn't a function: min, max, abs, clamp, sqrt, "
         "round, floor, ceil, if, all, any or not (at character 1)"},
        {"clamp(a, 10, 1)",
         "gives clamp() a low of 10 above its high of 1 (at character 10)"},
        {"clamp(a, -1, -5)", "gives clamp() a low of -1 above its high of -5"},
        {"if(a < b < c, 1, 0)", "has '<' beside a comparison: all() or any() "
                                "joins two (at character 10)"},
        {"if(a == b != c, 1, 0)", "has '!=' beside a comparison"},
        {"a = b", "has '=' where '==' should be (at character 3)"},
        {"if(a = b, 1, 0)", "has '=' where '==' should be (at character 6)"},
        {"a, b", "has ',' outside a call (at character 2)"},
        {"(a, b)", "has ',' outside a call (at character 3)"},
        {"max(a b)", "has 'b' where ',' or ')' should be (at character 7)"},
        {"max(a, b", "has no ')' to close its max( (at character 9)"},
        {"max(a,)", "has ')' where a value should be"},
        {"a + b + c + d + a + b + c + d + a + b + c + d + a + b + c + d + "
         "max(a, b)",
         "is longer than a program's 33 steps"},
        {"abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs(abs("
         "a)))))))))))))))))",
         "nests deeper than 16"},
    };
    for (const Case& c : cases) {
        const std::string got = words_of(2, c.expr);
        EXPECT_NE(got.find(c.words), std::string::npos)
            << c.expr << "\n  got: " << got << "\n  want: " << c.words;
        EXPECT_EQ(got.compare(0, 5, "line "), 0) << got;
    }

    /* The whole line: the expr's place in the file, the expr, the place in it */
    const std::string text =
        "{\n  \"havne_signals\": 2,\n  \"name\": \"T\",\n"
        "  \"source\": {\"name\": \"t\"},\n"
        "  \"ecus\": [{\"id\": \"pcm\", \"rx\": \"7E8\"}],\n"
        "  \"signals\": [\n"
        "    {\"key\": \"rpm\", \"name\": \"RPM\", \"read\": {\"service\": "
        "\"01\", \"number\": \"0C\"}, \"field\": {\"byte\": 0}},\n"
        "    {\"key\": \"e\", \"name\": \"E\", \"kind\": \"computed\",\n"
        "     \"expr\": \"if(rpm, 1, 0)\"}\n  ]\n}";
    EXPECT_EQ(load(text), nullptr);
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "line 9, column 14: expr 'if(rpm, 1, 0)' gives if() a number where "
        "a comparison should be (at character 4)");

    /* A computed input is format 2's: refused in format 1, read in 2 */
    File one = load(file_of(1, {"a + 1", "e * 2"}));
    EXPECT_EQ(one, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail())
                  .find("expr 'e * 2' reads 'e', a computed signal, which "
                        "needs \"havne_signals\": 2 (at character 1)"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_NE(load(file_of(2, {"a + 1", "e * 2"})), nullptr)
        << ecuio_last_error_detail();
}

TEST(SignalFileTest, TheFormatAndEachExpressionReadBack)
{
    for (const int format : {1, 2}) {
        File f = load(file_of(format, {"a  +  b", "(a - b)*2"}));
        ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_signal_file_format(f.get()),
                  static_cast<uint32_t>(format));
        EXPECT_STREQ(ecuio_signal_file_expr(
                         f.get(), ecuio_signal_file_find(f.get(), "e")),
                     "a  +  b")
            << "as written";
        EXPECT_STREQ(ecuio_signal_file_expr(
                         f.get(), ecuio_signal_file_find(f.get(), "e1")),
                     "(a - b)*2");
        EXPECT_STREQ(ecuio_signal_file_expr(
                         f.get(), ecuio_signal_file_find(f.get(), "a")),
                     "")
            << "a number";
        ecuio_signal_def_t copy = *ecuio_signal_file_find(f.get(), "e");
        EXPECT_STREQ(ecuio_signal_file_expr(f.get(), &copy), "")
            << "a copy is no def of the file";
        EXPECT_STREQ(ecuio_signal_file_expr(f.get(), nullptr), "");
        EXPECT_STREQ(ecuio_signal_file_expr(
                         nullptr, ecuio_signal_file_find(f.get(), "e")),
                     "");
    }
    EXPECT_EQ(ecuio_signal_file_format(nullptr), 0u);

    File base = load(
        R"({"havne_signals":2,"name":"T","source":{"name":"t"},"extends":"obd2",)"
        R"("signals":[{"key":"b","name":"Boost2","kind":"computed","expr":"obd2.map - obd2.baro"}]})");
    ASSERT_NE(base, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(base.get());
    EXPECT_STREQ(ecuio_signal_file_expr(base.get(), &set->defs[0]), "")
        << "a base def";
    EXPECT_STREQ(ecuio_signal_file_expr(
                     base.get(), ecuio_signal_file_find(base.get(), "b")),
                 "obd2.map - obd2.baro");
}

namespace {

/* A random expression tree, its text, and a walk of it by ADR-0062's
 * rules written apart from core: no value passes through a step, if()
 * ignores the branch it doesn't take, all() and any() settle first */
struct Node {
    enum Kind { leaf, constant, binary, negate, call, compare } kind = leaf;
    int index = 0;    /* leaf: a to d */
    double value = 0; /* constant */
    char op = '+';    /* binary */
    std::string name; /* call; compare's operator */
    std::vector<Node> parts;
    bool truth = false;
};

struct Value {
    bool has = false;
    double v = 0;
};

class Trees
{
public:
    explicit Trees(uint32_t seed) : rng_(seed) {}

    Node number(int depth)
    {
        if (depth <= 0 || pick(4) == 0) {
            Node n;
            if (pick(10) < 7) {
                n.kind = Node::leaf;
                n.index = static_cast<int>(pick(4));
            } else {
                n.kind = Node::constant;
                n.value =
                    static_cast<double>(pick(21)) + (pick(2) == 0 ? 0.0 : 0.5);
            }
            return n;
        }
        Node n;
        switch (pick(7)) {
        case 0:
        case 1: {
            n.kind = Node::binary;
            n.op = "+-*/"[pick(4)];
            n.parts = {number(depth - 1), number(depth - 1)};
            return n;
        }
        case 2:
            n.kind = Node::negate;
            n.parts = {number(depth - 1)};
            return n;
        case 3: {
            n.kind = Node::call;
            n.name = pick(2) == 0 ? "min" : "max";
            const uint32_t count = 2 + pick(2);
            for (uint32_t i = 0; i < count; ++i) {
                n.parts.push_back(number(depth - 1));
            }
            return n;
        }
        case 4: {
            static const char* const kOne[] = {"abs", "sqrt", "round", "floor",
                                               "ceil"};
            n.kind = Node::call;
            n.name = kOne[pick(5)];
            n.parts = {number(depth - 1)};
            return n;
        }
        case 5:
            n.kind = Node::call;
            n.name = "clamp";
            n.parts = {number(depth - 1), number(depth - 1), number(depth - 1)};
            return n;
        default:
            n.kind = Node::call;
            n.name = "if";
            n.parts = {truth(depth - 1), number(depth - 1), number(depth - 1)};
            return n;
        }
    }

    Node truth(int depth)
    {
        Node n;
        n.truth = true;
        const uint32_t k = depth <= 0 ? 0 : pick(6);
        if (k <= 2) {
            static const char* const kOps[] = {"<",  "<=", ">",
                                               ">=", "==", "!="};
            n.kind = Node::compare;
            n.name = kOps[pick(6)];
            n.parts = {number(depth - 1), number(depth - 1)};
        } else if (k <= 4) {
            n.kind = Node::call;
            n.name = pick(2) == 0 ? "all" : "any";
            const uint32_t count = 2 + pick(2);
            for (uint32_t i = 0; i < count; ++i) {
                n.parts.push_back(truth(depth - 1));
            }
        } else {
            n.kind = Node::call;
            n.name = "not";
            n.parts = {truth(depth - 1)};
        }
        return n;
    }

    uint32_t pick(uint32_t n) { return static_cast<uint32_t>(rng_() % n); }

private:
    std::mt19937 rng_;
};

std::string text_of(const Node& n)
{
    switch (n.kind) {
    case Node::leaf:
        return std::string(1, static_cast<char>('a' + n.index));
    case Node::constant: {
        char t[32];
        std::snprintf(t, sizeof t,
                      n.value == std::floor(n.value) ? "%.0f" : "%.1f",
                      n.value);
        return t;
    }
    case Node::binary:
        return "(" + text_of(n.parts[0]) + " " + n.op + " " +
               text_of(n.parts[1]) + ")";
    case Node::negate:
        return "-" + text_of(n.parts[0]);
    case Node::compare:
        return "(" + text_of(n.parts[0]) + " " + n.name + " " +
               text_of(n.parts[1]) + ")";
    default: {
        std::string s = n.name + "(";
        for (size_t i = 0; i < n.parts.size(); ++i) {
            s += (i == 0 ? "" : ", ") + text_of(n.parts[i]);
        }
        return s + ")";
    }
    }
}

Value number_value(double v)
{
    return {std::isfinite(v), v};
}

Value walk(const Node& n, const double leaves[4])
{
    switch (n.kind) {
    case Node::leaf:
        return {!std::isnan(leaves[n.index]), leaves[n.index]};
    case Node::constant:
        return {true, n.value};
    case Node::binary: {
        const Value a = walk(n.parts[0], leaves);
        const Value b = walk(n.parts[1], leaves);
        if (!a.has || !b.has) {
            return {};
        }
        return number_value(n.op == '+'   ? a.v + b.v
                            : n.op == '-' ? a.v - b.v
                            : n.op == '*' ? a.v * b.v
                                          : a.v / b.v);
    }
    case Node::negate: {
        /* Compiled as 0 - x: -0 of +0 is +0 */
        const Value x = walk(n.parts[0], leaves);
        return x.has ? number_value(0.0 - x.v) : Value{};
    }
    case Node::compare: {
        const Value a = walk(n.parts[0], leaves);
        const Value b = walk(n.parts[1], leaves);
        if (!a.has || !b.has) {
            return {};
        }
        const std::string& op = n.name;
        const bool r = op == "<"    ? a.v < b.v
                       : op == "<=" ? a.v <= b.v
                       : op == ">"  ? a.v > b.v
                       : op == ">=" ? a.v >= b.v
                       : op == "==" ? a.v == b.v
                                    : a.v != b.v;
        return {true, r ? 1.0 : 0.0};
    }
    default:
        break;
    }
    const std::string& f = n.name;
    if (f == "if") {
        const Value p = walk(n.parts[0], leaves);
        if (!p.has) {
            return {};
        }
        return walk(n.parts[p.v != 0.0 ? 1 : 2], leaves);
    }
    if (f == "all" || f == "any") {
        const double settles = f == "all" ? 0.0 : 1.0;
        Value r = walk(n.parts[0], leaves);
        for (size_t i = 1; i < n.parts.size(); ++i) {
            const Value q = walk(n.parts[i], leaves);
            if ((r.has && r.v == settles) || (q.has && q.v == settles)) {
                r = {true, settles};
            } else if (r.has && q.has) {
                r = {true, 1.0 - settles};
            } else {
                r = {};
            }
        }
        return r;
    }
    if (f == "not") {
        const Value p = walk(n.parts[0], leaves);
        return p.has ? Value{true, p.v == 0.0 ? 1.0 : 0.0} : Value{};
    }
    if (f == "min" || f == "max" || f == "clamp") {
        std::vector<Value> v;
        for (const Node& p : n.parts) {
            v.push_back(walk(p, leaves));
        }
        const auto pick = [](const Value& a, const Value& b, bool smaller) {
            if (!a.has || !b.has) {
                return Value{};
            }
            return number_value(smaller ? (b.v < a.v ? b.v : a.v)
                                        : (b.v > a.v ? b.v : a.v));
        };
        if (f == "clamp") {
            return pick(pick(v[0], v[1], false), v[2], true);
        }
        Value r = v[0];
        for (size_t i = 1; i < v.size(); ++i) {
            r = pick(r, v[i], f == "min");
        }
        return r;
    }
    const Value x = walk(n.parts[0], leaves);
    if (!x.has) {
        return {};
    }
    if (f == "abs") {
        return number_value(std::fabs(x.v));
    }
    if (f == "sqrt") {
        return x.v < 0 ? Value{} : number_value(std::sqrt(x.v));
    }
    if (f == "round") {
        return number_value(std::round(x.v));
    }
    if (f == "floor") {
        return number_value(std::floor(x.v));
    }
    return number_value(std::ceil(x.v));
}

} // namespace

// Seeded random trees of every form, compiled and run, against the walk:
// the same value to the bit, or no value for both
TEST(SignalFileTest, RandomTreesComputeAsAWalkOfThem)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double inputs[][4] = {
        {1, 2, 3, 4},           {0, -0.0, 7.5, -3},
        {kNone, 2, kNone, 0.5}, {1e300, 1e300, -1e300, 2},
        {-1, -2.5, 2.5, 0},     {kNone, kNone, kNone, kNone},
        {inf, 1, -inf, 3},
    };
    uint32_t compiled = 0;
    uint32_t compared = 0;
    for (uint32_t seed = 1; seed <= 2000; ++seed) {
        Trees t(seed);
        const Node tree = t.number(1 + static_cast<int>(t.pick(4)));
        const std::string expr = text_of(tree);
        File f = load(file_of(2, {expr}));
        if (!f) {
            /* Only the program's bounds, and clamp()'s constants, may
             * refuse a tree */
            const std::string why = ecuio_last_error_detail();
            const bool bound =
                why.find("33 steps") != std::string::npos ||
                why.find("16 constants") != std::string::npos ||
                why.find("nests deeper") != std::string::npos ||
                why.find("256 characters") != std::string::npos ||
                why.find("reads no signal") != std::string::npos ||
                why.find("above its high") != std::string::npos;
            EXPECT_TRUE(bound) << expr << ": " << why;
            continue;
        }
        ++compiled;
        const ecuio_computed_t& p = ecuio_signal_file_set(f.get())->computed[0];
        ASSERT_TRUE(ecuio_computed_check(p)) << expr;
        for (const auto& leaves : inputs) {
            double in[4] = {};
            for (uint32_t k = 0; k < p.input_count; ++k) {
                in[k] = leaves[p.input_ids[k] - ECUIO_USER_SIG_ID_BASE];
            }
            double out = 0.0;
            const bool ran = ecuio_computed_run(p, in, &out);
            const Value want = walk(tree, leaves);
            const bool want_has = want.has && std::isfinite(want.v);
            ASSERT_EQ(ran, want_has)
                << expr << " at " << leaves[0] << ", " << leaves[1] << ", "
                << leaves[2] << ", " << leaves[3];
            if (ran) {
                ASSERT_EQ(bits(out), bits(want.v)) << expr;
            }
            ++compared;
        }
    }
    EXPECT_GE(compiled, 1000u) << "most trees fit a program";
    EXPECT_GE(compared, 7000u);
}

namespace {

/* line 1, column C: the place of expr's value in a one-line file */
std::string at_expr(const std::string& text, const std::string& expr)
{
    const size_t at = text.find("\"expr\":\"" + expr + "\"");
    return "line 1, column " + std::to_string(at + 8) + ": ";
}

} // namespace

/* ADR-0062: a computed signal reads another in format 2; a circle, a
 * chain past 8 deep and an age bound on a computed input are refused at
 * the first signal of each in the file's order, at its expr */
TEST(SignalFileTest, AChainLoadsAndItsRefusalsNameTheSignal)
{
    const std::string chain = file_of(2, {"a + 1", "e * 2", "e1 + b"});
    File f = load(chain);
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_def_t* e = ecuio_signal_file_find(f.get(), "e");
    const ecuio_signal_def_t* e2 = ecuio_signal_file_find(f.get(), "e2");
    EXPECT_EQ(set->computed[e2->table_first].input_ids[0],
              ecuio_signal_file_find(f.get(), "e1")->id);
    EXPECT_EQ(set->computed[e2->table_first].max_age_us[0], 0u);
    EXPECT_NE(e, nullptr);

    const auto refused = [](const std::string& text) {
        File g = load(text);
        EXPECT_FALSE(g) << text;
        return std::string(ecuio_last_error_detail());
    };
    std::string text = file_of(2, {"e1 + 1", "e * 2"});
    EXPECT_EQ(refused(text),
              at_expr(text, "e1 + 1") +
                  "signal 'e' reads 'e1', which reads 'e': computed signals "
                  "can't read in a circle");
    text = file_of(2, {"e + 1"});
    EXPECT_EQ(refused(text),
              at_expr(text, "e + 1") +
                  "signal 'e' reads 'e': computed signals can't read in a "
                  "circle");
    /* e reads the circle e1, e2, e3 without being on it: e1 is named */
    text = file_of(2, {"e1 + a", "e2 + 1", "e3 * 2", "e1 - 1"});
    EXPECT_EQ(refused(text),
              at_expr(text, "e2 + 1") +
                  "signal 'e1' reads 'e2', which reads 'e3', which reads "
                  "'e1': computed signals can't read in a circle");

    /* 8 deep loads; 9 deep is refused at the signal 9 deep */
    std::vector<std::string> deep = {"a + 1"};
    for (int k = 1; k < 8; ++k) {
        deep.push_back(
            (k == 1 ? std::string("e") : "e" + std::to_string(k - 1)) + " + 1");
    }
    EXPECT_TRUE(load(file_of(2, deep))) << ecuio_last_error_detail();
    deep.push_back("e7 + 1");
    text = file_of(2, deep);
    EXPECT_EQ(refused(text),
              at_expr(text, "e7 + 1") +
                  "signal 'e8' sits 9 computed signals deep: a chain goes 8 "
                  "deep at most");

    /* A computed input's ages are its own inputs' */
    text = file_of(2, {"a + 1"},
                   R"({"key":"p","name":"P","kind":"computed","expr":"e + a",)"
                   R"("max_age":{"a":1,"e":1}},)");
    const std::string got = refused(text);
    EXPECT_NE(got.find("signal 'p''s max_age names 'e', a computed signal: its "
                       "own inputs carry their ages"),
              std::string::npos)
        << got;
}
