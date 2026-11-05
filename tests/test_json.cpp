// File/layer: tests/test_json.cpp | test_core, Qt-free, C++17
// Testing: UNIT: JsonTest: core/src/json.h's reader: every type and its
//          position, key order, escapes and UTF-8, numbers (int64's edges,
//          the double range, a comma locale), each refusal with its line,
//          column and words, and the two bounds at their edges
//
// Inputs are written as C++ strings; a non-ASCII byte in one is on
// purpose.

#include <gtest/gtest.h>

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "json.h"

namespace {

using ecuio_json::Error;
using ecuio_json::Limits;
using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;

Value ok(const std::string& text, const Limits& limits = Limits())
{
    Value v;
    Error e;
    EXPECT_TRUE(ecuio_json::parse(text.data(), text.size(), &v, &e, limits))
        << text << "\n  " << e.to_string();
    return v;
}

/* A refusal at line:column whose words hold `words` */
void refused(const std::string& text, uint32_t line, uint32_t column,
             const std::string& words, const Limits& limits = Limits())
{
    Value v;
    v.type = Type::array;   /* reset to null on a refusal */
    Error e;
    EXPECT_FALSE(ecuio_json::parse(text.data(), text.size(), &v, &e, limits))
        << text;
    EXPECT_EQ(v.type, Type::null) << text;
    EXPECT_EQ(e.line, line) << text << "\n  " << e.to_string();
    EXPECT_EQ(e.column, column) << text << "\n  " << e.to_string();
    EXPECT_NE(e.words.find(words), std::string::npos)
        << text << "\n  " << e.to_string();
    EXPECT_EQ(e.words.find('\n'), std::string::npos) << e.words;
}

static_assert(noexcept(ecuio_json::parse(nullptr, 0, nullptr, nullptr)),
              "parse never throws out of core");

}  // namespace

TEST(JsonTest, ReadsEveryType)
{
    const Value v = ok(R"({"n": null, "t": true, "f": false, "i": 12,
        "x": -1.5e2, "s": "text", "a": [1, "two", [], {}], "o": {"k": 0}})");
    ASSERT_EQ(v.type, Type::object);
    ASSERT_EQ(v.members.size(), 8u);
    EXPECT_EQ(v.find("n")->type, Type::null);
    EXPECT_EQ(v.find("t")->type, Type::boolean);
    EXPECT_TRUE(v.find("t")->boolean);
    EXPECT_FALSE(v.find("f")->boolean);
    EXPECT_EQ(v.find("i")->type, Type::number);
    EXPECT_EQ(v.find("i")->whole, 12);
    EXPECT_EQ(v.find("x")->number, -150.0);
    EXPECT_EQ(v.find("s")->text, "text");
    const Value& a = *v.find("a");
    ASSERT_EQ(a.type, Type::array);
    ASSERT_EQ(a.items.size(), 4u);
    EXPECT_EQ(a.items[1].text, "two");
    EXPECT_EQ(a.items[2].type, Type::array);
    EXPECT_TRUE(a.items[2].items.empty());
    EXPECT_EQ(a.items[3].type, Type::object);
    EXPECT_EQ(v.find("o")->find("k")->whole, 0);
    EXPECT_EQ(v.find("missing"), nullptr);
}

TEST(JsonTest, AnyValueIsADocument)
{
    EXPECT_EQ(ok("7").whole, 7);
    EXPECT_EQ(ok("\"s\"").text, "s");
    EXPECT_EQ(ok(" null ").type, Type::null);
    EXPECT_EQ(ok("\r\n\t[ ]\n").type, Type::array);
}

TEST(JsonTest, ObjectsKeepTheirMembersInFileOrder)
{
    const Value v = ok(R"({"zeta": 1, "alpha": 2, "mid": 3})");
    ASSERT_EQ(v.members.size(), 3u);
    EXPECT_EQ(v.members[0].key, "zeta");
    EXPECT_EQ(v.members[1].key, "alpha");
    EXPECT_EQ(v.members[2].key, "mid");
    EXPECT_EQ(v.members[2].value.whole, 3);
}

TEST(JsonTest, EveryValueAndKeyKeepsItsLineAndColumn)
{
    const Value v = ok("{\n  \"a\": [10,\n\t\"b\"],\r\n  \"c\":\r{}\n}");
    EXPECT_EQ(v.line, 1u);
    EXPECT_EQ(v.column, 1u);
    const auto& a = v.members[0];
    EXPECT_EQ(a.line, 2u);
    EXPECT_EQ(a.column, 3u);
    EXPECT_EQ(a.value.line, 2u);
    EXPECT_EQ(a.value.column, 8u);
    EXPECT_EQ(a.value.items[0].column, 9u);
    EXPECT_EQ(a.value.items[1].line, 3u);
    EXPECT_EQ(a.value.items[1].column, 2u);   /* a tab is one column */
    const auto& c = v.members[1];
    EXPECT_EQ(c.line, 4u);   /* "\r\n" is one line break */
    EXPECT_EQ(c.column, 3u);
    EXPECT_EQ(c.value.line, 5u);   /* a lone "\r" is one too */
    EXPECT_EQ(c.value.column, 1u);
}

TEST(JsonTest, ColumnsCountCharactersNotBytes)
{
    /* U+00E9 is two bytes, U+20AC three, U+1F600 four: each one column */
    const Value v = ok("[\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\", 1]");
    EXPECT_EQ(v.items[1].column, 9u);
    refused("[\"\xC3\xA9\xE2\x82\xAC\", x]", 1, 8, "'x'");
}

TEST(JsonTest, EscapesAreDecoded)
{
    EXPECT_EQ(ok(R"("\" \\ \/ \b \f \n \r \t")").text,
              "\" \\ / \b \f \n \r \t");
    EXPECT_EQ(ok(R"("\u0041\u00e9\u20AC")").text, "A\xC3\xA9\xE2\x82\xAC");
    EXPECT_EQ(ok(R"("\uD83D\uDE00")").text, "\xF0\x9F\x98\x80");
    EXPECT_EQ(ok(R"("\uDBFF\uDFFF")").text, "\xF4\x8F\xBF\xBF");
    const std::string nul = ok(R"("a\u0000b")").text;
    EXPECT_EQ(nul, std::string("a\0b", 3));   /* kept: JSON allows it */
}

TEST(JsonTest, RawUtf8IsKeptAsWritten)
{
    const std::string s = "\"\xC2\x80 \xDF\xBF \xE0\xA0\x80 \xED\x9F\xBF "
                          "\xEE\x80\x80 \xF0\x90\x80\x80 \xF4\x8F\xBF\xBF "
                          "\xC2\xB0" "C\"";
    EXPECT_EQ(ok(s).text, s.substr(1, s.size() - 2));
}

TEST(JsonTest, IntegersKeepTheirExactValueWithinInt64)
{
    const Value big = ok("9223372036854775807");
    EXPECT_TRUE(big.integer);
    EXPECT_TRUE(big.in_int64);
    EXPECT_EQ(big.whole, INT64_MAX);
    const Value low = ok("-9223372036854775808");
    EXPECT_TRUE(low.in_int64);
    EXPECT_EQ(low.whole, INT64_MIN);
    /* past 2^53, where a double can't hold it */
    EXPECT_EQ(ok("123456789012345679").whole, 123456789012345679LL);

    const Value past = ok("9223372036854775808");
    EXPECT_TRUE(past.integer);
    EXPECT_FALSE(past.in_int64);
    EXPECT_EQ(past.whole, 0);
    EXPECT_EQ(past.number, 9223372036854775808.0);
    EXPECT_FALSE(ok("-9223372036854775809").in_int64);
    EXPECT_FALSE(ok("100000000000000000000000").in_int64);
}

TEST(JsonTest, AFractionOrAnExponentIsNoInteger)
{
    for (const char* text : {"1.0", "1e0", "1E+2", "10e-1", "-0.5"}) {
        const Value v = ok(text);
        EXPECT_FALSE(v.integer) << text;
        EXPECT_FALSE(v.in_int64) << text;
    }
    EXPECT_EQ(ok("1E+2").number, 100.0);
    EXPECT_EQ(ok("10e-1").number, 1.0);
}

/* Exact (one rounding) for up to 15 significant digits and a power of ten
 * within 22, recording_rows.h's parse_number; close past that */
TEST(JsonTest, NumbersReadAsTheirShortestDoubles)
{
    EXPECT_EQ(ok("0.1").number, 0.1);
    EXPECT_EQ(ok("-40").number, -40.0);
    EXPECT_EQ(ok("0.0078125").number, 0.0078125);
    EXPECT_EQ(ok("3.14159265358979").number, 3.14159265358979);
    EXPECT_EQ(ok("1e22").number, 1e22);
    EXPECT_EQ(ok("-1.5e-22").number, -1.5e-22);
    EXPECT_EQ(ok("999999999999999.0").number, 999999999999999.0);
    EXPECT_NEAR(ok("1e308").number / 1e308, 1.0, 1e-14);
    EXPECT_NEAR(ok("4.9e-300").number / 4.9e-300, 1.0, 1e-14);
    EXPECT_EQ(ok("2.5e-3").number, 2.5e-3);
    EXPECT_EQ(ok("1e-400").number, 0.0);   /* underflow reads as zero */
    const Value neg_zero = ok("-0");
    EXPECT_TRUE(neg_zero.in_int64);
    EXPECT_EQ(neg_zero.whole, 0);
    EXPECT_TRUE(std::signbit(neg_zero.number));
}

/* A Qt program sets LC_NUMERIC from the environment, a comma in German */
TEST(JsonTest, NumbersKeepTheirDecimalPointUnderAnyLocale)
{
    const char* before = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = before != nullptr ? before : "C";
    if (std::setlocale(LC_NUMERIC, "de-DE") == nullptr &&
        std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr) {
        GTEST_SKIP() << "no German locale here";
    }
    const Value v = ok("[0.25, 1.5e1]");
    std::setlocale(LC_NUMERIC, saved.c_str());
    EXPECT_EQ(v.items[0].number, 0.25);
    EXPECT_EQ(v.items[1].number, 15.0);
}

TEST(JsonTest, RefusesADuplicateKeyAtItsSecondOccurrence)
{
    refused(R"({"scale": 1, "scale": 2})", 1, 14, "duplicate key 'scale'");
    refused("{\"a\": {\"b\": 1,\n  \"b\": 2}}", 2, 3, "duplicate key 'b'");
    /* the same key spelled with an escape is the same key */
    refused(R"({"ab": 1, "\u0061b": 2})", 1, 11, "duplicate key 'ab'");
    /* the same key in two objects is no duplicate */
    ok(R"([{"k": 1}, {"k": 2}])");
}

TEST(JsonTest, RefusesADuplicateKeyInALargeObject)
{
    std::string text = "{";
    for (int i = 0; i < 1000; ++i) {
        text += "\"k" + std::to_string(i) + "\": " + std::to_string(i) + ",";
    }
    const Value v = ok(text.substr(0, text.size() - 1) + "}");
    EXPECT_EQ(v.members.size(), 1000u);
    EXPECT_EQ(v.members[999].key, "k999");
    const uint32_t column = static_cast<uint32_t>(text.size()) + 1;
    refused(text + "\"k500\": 0}", 1, column, "duplicate key 'k500'");
    refused(text + "\"k0\": 0}", 1, column, "duplicate key 'k0'");
}

TEST(JsonTest, RefusesComments)
{
    refused("{\"a\": 1 // why\n}", 1, 9, "expected ',' or '}'");
    refused("{// why\n\"a\": 1}", 1, 2, "a comment");
    refused("[/* why */ 1]", 1, 2, "a comment");
    refused("[1] // why", 1, 5, "text after the end");
}

TEST(JsonTest, RefusesTrailingCommas)
{
    refused("[1, 2,]", 1, 7, "a trailing comma before ']'");
    refused("{\"a\": 1,\n}", 2, 1, "a trailing comma before '}'");
    refused("[,]", 1, 2, "expected a value, found ','");
    refused("{,}", 1, 2, "expected a key in double quotes, found ','");
    refused("[1,,2]", 1, 4, "expected a value, found ','");
}

TEST(JsonTest, RefusesNanAndInfinity)
{
    refused("NaN", 1, 1, "NaN and Infinity are not JSON numbers");
    refused("[Infinity]", 1, 2, "NaN and Infinity");
    refused("[-Infinity]", 1, 2, "NaN and Infinity");
    refused("nan", 1, 1, "expected a value, found 'n'");
}

TEST(JsonTest, RefusesNumbersOutsideTheGrammar)
{
    refused("01", 1, 1, "a number with a leading zero");
    refused("[-01]", 1, 2, "a number with a leading zero");
    refused("00", 1, 1, "a number with a leading zero");
    refused("+1", 1, 1, "expected a value, found '+'");
    refused(".5", 1, 1, "expected a value, found '.'");
    refused("1.", 1, 3, "expected a digit after '.'");
    refused("1.e5", 1, 3, "expected a digit after '.'");
    refused("-", 1, 2, "expected a digit after '-', found the end");
    refused("-a", 1, 2, "expected a digit after '-'");
    refused("1e", 1, 3, "expected a digit in the exponent");
    refused("1e+", 1, 4, "expected a digit in the exponent");
    refused("0x10", 1, 2, "text after the end of the value: 'x'");
    refused("1e309", 1, 1, "past the range of a double");
    refused("[-1e400]", 1, 2, "past the range of a double");
    refused(std::string(400, '9'), 1, 1, "past the range of a double");
}

TEST(JsonTest, RefusesInvalidUtf8)
{
    struct Case {
        const char* bytes;
        const char* why;
    };
    const Case cases[] = {
        {"\xC0\x80", "overlong NUL"},
        {"\xC1\xBF", "overlong 2-byte"},
        {"\xE0\x80\x80", "overlong 3-byte"},
        {"\xE0\x9F\xBF", "overlong 3-byte, top"},
        {"\xF0\x80\x80\x80", "overlong 4-byte"},
        {"\xF0\x8F\xBF\xBF", "overlong 4-byte, top"},
        {"\xED\xA0\x80", "an encoded high surrogate"},
        {"\xED\xBF\xBF", "an encoded low surrogate"},
        {"\xF4\x90\x80\x80", "past U+10FFFF"},
        {"\xF5\x80\x80\x80", "a lead byte past F4"},
        {"\xFF", "never in UTF-8"},
        {"\x80", "a stray continuation byte"},
        {"\xC3", "cut before its continuation"},
        {"\xE2\x82", "cut 3-byte"},
        {"\xF0\x9F\x98", "cut 4-byte"},
        {"\xC3\x28", "a lead byte before ASCII"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.why);
        refused(std::string("[\"ab") + c.bytes + "\"]", 1, 5, "invalid UTF-8");
    }
    /* outside a string, any byte past ASCII is no token */
    refused("[\xC3\xA9]", 1, 2, "the character U+00E9");
    refused("[\xFF]", 1, 2, "the byte 0xFF, not UTF-8");
}

TEST(JsonTest, RefusesLoneSurrogatesInEscapes)
{
    refused(R"("\uD800")", 1, 2, "a lone surrogate");
    refused(R"("x\uDC00")", 1, 3, "a lone surrogate");
    refused(R"("\uD83D\u0041")", 1, 2, "a lone surrogate");
    refused(R"("\uD83D\uD83D")", 1, 2, "a lone surrogate");
    refused(R"("\uD83Dx")", 1, 2, "a lone surrogate");
    refused(R"("\uDE00\uD83D")", 1, 2, "a lone surrogate");
}

TEST(JsonTest, RefusesBadEscapesAndRawControlCharacters)
{
    refused(R"("\x41")", 1, 2, "an unknown escape: \\x");
    refused(R"("\U0041")", 1, 2, "an unknown escape: \\U");
    refused(R"("\u12G4")", 1, 2, "\\u takes four hex digits");
    refused(R"("\u12")", 1, 2, "\\u takes four hex digits");
    refused("\"a\tb\"", 1, 3, "the control character 0x09 in a string");
    refused("\"a\nb\"", 1, 3, "the control character 0x0A in a string");
    refused(std::string("\"a\0b\"", 5), 1, 3, "0x00 in a string");
}

TEST(JsonTest, RefusesAByteOrderMark)
{
    refused("\xEF\xBB\xBF{}", 1, 1, "byte order mark");
    /* inside the text, U+FEFF is a character like any other */
    EXPECT_EQ(ok("\"\xEF\xBB\xBF\"").text, "\xEF\xBB\xBF");
    refused("[\xEF\xBB\xBF]", 1, 2, "the character U+FEFF");
}

TEST(JsonTest, RefusesWhatIsNoJsonAtAll)
{
    refused("", 1, 1, "expected a value, found the end of the file");
    refused(" \n ", 2, 2, "expected a value, found the end of the file");
    refused("{'a': 1}", 1, 2, "a single quote");
    refused("['a']", 1, 2, "a single quote");
    refused("{a: 1}", 1, 2, "expected a key in double quotes, found 'a'");
    refused("{\"a\" 1}", 1, 6, "expected ':' after the key, found '1'");
    refused("[1 2]", 1, 4, "expected ',' or ']', found '2'");
    refused("{\"a\": 1 \"b\": 2}", 1, 9, "expected ',' or '}', found '\"'");
    refused("tru", 1, 1, "expected a value, found 't'");
    refused("True", 1, 1, "expected a value, found 'T'");
    refused("nulls", 1, 5, "text after the end of the value: 's'");
    refused("{} {}", 1, 4, "text after the end");
    refused("[1]\x01", 1, 4, "the control character 0x01");
    refused("\f[]", 1, 1, "the control character 0x0C");
}

TEST(JsonTest, AnInputEndingEarlySaysWhatWasOpen)
{
    refused("{\n  \"a\": [1,", 2, 11, "inside an array opened at line 2, column 8");
    refused("{\"a\": 1", 1, 8, "inside an object opened at line 1, column 1");
    refused("{\"a\":", 1, 6, "inside an object opened at line 1, column 1");
    refused("[\"abc", 1, 6, "inside a string opened at line 1, column 2");
    refused("\"ab\\", 1, 5, "inside an escape");
}

TEST(JsonTest, NestingStopsAtTheDepthBound)
{
    const unsigned d = ecuio_json::kMaxDepth;
    ok(std::string(d, '[') + std::string(d, ']'));
    refused(std::string(d + 1, '[') + std::string(d + 1, ']'), 1, d + 1,
            "nested deeper than 32 arrays and objects");
    std::string objects;
    for (unsigned i = 0; i <= d; ++i) {
        objects += "{\"k\":";
    }
    refused(objects + "0" + std::string(d + 1, '}'), 1, 5 * d + 1,
            "nested deeper than 32");

    Limits two;
    two.max_depth = 2;
    ok("[[1], {\"a\": 2}]", two);
    refused("[[[]]]", 1, 3, "nested deeper than 2", two);
    /* depth falls again when a container closes */
    ok("[[], [], [[]]]", Limits());
    refused("[[], [[[]]]]", 1, 7, "nested deeper than 2", two);
}

TEST(JsonTest, InputStopsAtTheSizeBound)
{
    Limits small;
    small.max_bytes = 8;
    ok("[1,2,3] ", small);
    refused("[1,2,3]  ", 0, 0, "the input is 9 bytes, past the limit of 8",
            small);

    /* the default: 1 MiB, whitespace counting too */
    std::string doc(ecuio_json::kMaxBytes, ' ');
    doc[0] = '0';
    EXPECT_EQ(ok(doc).whole, 0);
    doc += ' ';
    refused(doc, 0, 0, "past the limit of 1048576");
}

TEST(JsonTest, TheLargestInputParses)
{
    /* the densest input within the bound: half a million values */
    std::string text = "[";
    while (text.size() + 4 <= ecuio_json::kMaxBytes) {
        text += "0,";
    }
    text += "0]";
    ASSERT_LE(text.size(), ecuio_json::kMaxBytes);
    const Value v = ok(text);
    EXPECT_EQ(v.items.size(), (text.size() - 1) / 2);
    EXPECT_EQ(v.items.back().column, text.size() - 1);
}

TEST(JsonTest, AnErrorNamesItsPlace)
{
    Value v;
    Error e;
    const std::string text = "{\n  \"a\": 1,\n  \"a\": 2\n}";
    ASSERT_FALSE(ecuio_json::parse(text.data(), text.size(), &v, &e));
    EXPECT_EQ(e.to_string(), "line 3, column 3: duplicate key 'a'");
    Error whole;
    whole.words = "the input is 2 bytes";
    EXPECT_EQ(whole.to_string(), "the input is 2 bytes");
}

TEST(JsonTest, NoInputIsARefusalNotACrash)
{
    Value v;
    Error e;
    EXPECT_FALSE(ecuio_json::parse(nullptr, 1, &v, &e));
    EXPECT_EQ(e.line, 0u);
    EXPECT_EQ(e.words, "no input");
    EXPECT_FALSE(ecuio_json::parse(nullptr, 0, &v, &e));
    EXPECT_EQ(e.line, 1u);
    EXPECT_FALSE(ecuio_json::parse("1", 1, nullptr, &e));
    EXPECT_FALSE(ecuio_json::parse("1", 1, &v, nullptr));
}

TEST(JsonTest, ASecondParseStartsFresh)
{
    Value v;
    Error e;
    const std::string bad = "[1,]";
    const std::string good = "{\"a\": 1}";
    ASSERT_FALSE(ecuio_json::parse(bad.data(), bad.size(), &v, &e));
    ASSERT_TRUE(ecuio_json::parse(good.data(), good.size(), &v, &e));
    EXPECT_EQ(v.members.size(), 1u);
    EXPECT_EQ(e.line, 0u);
    EXPECT_TRUE(e.words.empty());
    ASSERT_TRUE(ecuio_json::parse(good.data(), good.size(), &v, &e));
    EXPECT_EQ(v.members.size(), 1u);
}

TEST(JsonTest, QuotedTextStaysOneShortLine)
{
    EXPECT_EQ(ecuio_json::quoted("scael"), "'scael'");
    EXPECT_EQ(ecuio_json::quoted("a\nb\tc\x7F"), "'a?b?c?'");
    EXPECT_EQ(ecuio_json::quoted("\xC2\xB0" "C"), "'\xC2\xB0" "C'");
    EXPECT_EQ(ecuio_json::quoted(std::string(40, 'x')),
              "'" + std::string(32, 'x') + "...'");
    /* 32 characters of two bytes each, cut at a whole one */
    std::string degrees;
    for (int i = 0; i < 33; ++i) {
        degrees += "\xC2\xB0";
    }
    EXPECT_EQ(ecuio_json::quoted(degrees),
              "'" + degrees.substr(0, 64) + "...'");
    EXPECT_EQ(ecuio_json::quoted(std::string(32, 'x')),
              "'" + std::string(32, 'x') + "'");
    EXPECT_EQ(ecuio_json::quoted("\xFF"), "'?'");
    /* a duplicate key's words go through it */
    refused("{\"a\\nb\": 1, \"a\\nb\": 2}", 1, 13, "duplicate key 'a?b'");
}

TEST(JsonTest, ReadsASignalFileShapedDocument)
{
    const std::string text = R"({
  "havne_signals": 1,
  "name": "Daily driver extras",
  "source": { "name": "Written by hand" },
  "ecus": [ { "id": "tcm", "rx": "7E9" } ],
  "signals": [
    {
      "key": "tft",
      "read": { "service": "22", "number": "1E1C" },
      "field": { "byte": 0, "bits": 16, "order": "be", "signed": false },
      "scale": 1, "offset": -40,
      "unit": "°C", "decimals": 0,
      "values": { "1": "Open loop, cold", "2": "Closed loop" }
    }
  ]
})";
    const Value v = ok(text);
    EXPECT_EQ(v.find("havne_signals")->whole, 1);
    const Value& sig = v.find("signals")->items[0];
    EXPECT_EQ(sig.line, 7u);
    EXPECT_EQ(sig.column, 5u);
    EXPECT_EQ(sig.find("unit")->text, "\xC2\xB0" "C");
    EXPECT_EQ(sig.find("offset")->whole, -40);
    EXPECT_FALSE(sig.find("field")->find("signed")->boolean);
    const Member& scale = sig.members[3];
    EXPECT_EQ(scale.key, "scale");
    EXPECT_EQ(scale.line, 11u);
    EXPECT_EQ(scale.column, 7u);
    EXPECT_EQ(sig.find("values")->members[1].value.text, "Closed loop");
}
