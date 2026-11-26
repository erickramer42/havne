// File/layer: tests/test_signal_expr_names.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalExprTest (test_signals)
//
// The names an expression reads (ecuio_expr_names): by the grammar alone,
// as written, each once in read order, refused in the loader's words.

#include "test_signal_expr_support.h"

namespace {

/* What ecuio_expr_names answered */
struct Names {
    ecuio_status_t status = ECUIO_OK;
    std::vector<std::string> names;
    uint32_t count = 0;
    std::string detail;
};

inline Names names_of(const std::string& expr)
{
    Names n;
    std::vector<ecuio_expr_name_t> out(8);
    std::memset(out.data(), 0xA5, out.size() * sizeof out[0]);
    n.count = 99;
    n.status = ecuio_expr_names(expr.data(), expr.size(), out.data(),
                                static_cast<uint32_t>(out.size()), &n.count);
    n.detail = ecuio_last_error_detail();
    for (uint32_t i = 0; i < n.count && i < out.size(); ++i) {
        const char* text = out[i].text;
        const size_t length = std::strlen(text);
        for (size_t b = length; b < sizeof out[i].text; ++b) {
            EXPECT_EQ(text[b], '\0') << expr << " name " << i << " byte " << b;
        }
        n.names.emplace_back(text, length);
    }
    return n;
}

using List = std::vector<std::string>;

} // namespace

// Each name once, where it is first read: a repeat adds nothing
TEST(SignalExprTest, ExprNamesListsEachNameOnceInReadOrder)
{
    const Names n = names_of("boost*2 + map - boost");
    EXPECT_EQ(n.status, ECUIO_OK) << n.detail;
    EXPECT_EQ(n.count, 2u);
    EXPECT_EQ(n.names, (List{"boost", "map"}));
    EXPECT_EQ(names_of("(c + b) / (a - c) + b").names, (List{"c", "b", "a"}));
}

// A function's name and a number are no signal's
TEST(SignalExprTest, ExprNamesListsNoFunctionNorNumber)
{
    const Names n = names_of("max(a, 3) + abs(b)");
    EXPECT_EQ(n.status, ECUIO_OK) << n.detail;
    EXPECT_EQ(n.names, (List{"a", "b"}));
    EXPECT_EQ(names_of("if(x > 2.5, clamp(y, 0, 10), 1)").names,
              (List{"x", "y"}));
}

// A name stays as written: a second key, a key no signal has, obd2's
// short name and its PID name, both for one field
TEST(SignalExprTest, ExprNamesListsSecondKeysAndObd2NamesAsWritten)
{
    const Names n = names_of("old_boost + bost + obd2.pid_0d + obd2.rpm / 4");
    EXPECT_EQ(n.status, ECUIO_OK) << n.detail;
    EXPECT_EQ(n.names, (List{"old_boost", "bost", "obd2.pid_0d", "obd2.rpm"}));
    EXPECT_EQ(names_of("obd2.map - obd2.pid_0b").names,
              (List{"obd2.map", "obd2.pid_0b"}));
    /* Five keys may be four signals: a file may give one two keys */
    EXPECT_EQ(names_of("a + b + c + d + old_a").names,
              (List{"a", "b", "c", "d", "old_a"}));
    EXPECT_EQ(names_of("obd2 * 2").names, (List{"obd2"}));
    const std::string longest(31, 'k');
    EXPECT_EQ(names_of(longest + " + 1").names, (List{longest}));
}

// What every file's loader refuses is refused, in the loader's words with
// its character, and *count is 0
TEST(SignalExprTest, ExprNamesRefusesWhatTheGrammarRefusesInTheLoadersWords)
{
    /* One file has every name these read, so its check refuses for the
     * grammar alone */
    File f = load(file_of(2, {}, "\"extends\":\"obd2\","));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    for (const std::string expr :
         {"a +", "max(a", "a ** b", "1 + 2", "(a", "a b", "a = b", "foo(a)",
          "a.b + c", "obd2.nope * 2", "a < b", "-", ""}) {
        const Checked c = check(f.get(), nullptr, expr);
        ASSERT_EQ(c.status, ECUIO_ERR_INVALID_ARGUMENT) << expr;
        const Names n = names_of(expr);
        EXPECT_EQ(n.status, ECUIO_ERR_INVALID_ARGUMENT) << expr;
        EXPECT_EQ(n.count, 0u) << expr;
        EXPECT_EQ(n.detail,
                  c.detail + " (at character " + std::to_string(c.out.at) + ")")
            << expr;
    }
    /* A name no key can be, and a fifth obd2 field */
    EXPECT_EQ(names_of("a + " + std::string(32, 'k')).detail,
              "the expression reads '" + std::string(32, 'k') +
                  "', which no signal is (at character 5)");
    const Names five =
        names_of("obd2.rpm + obd2.map + obd2.speed + obd2.load + obd2.iat");
    EXPECT_EQ(five.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(five.count, 0u);
    EXPECT_EQ(five.detail,
              "the expression reads more than 4 signals (at character 48)");
    /* Past the bound, none of its bytes read */
    const std::string at_bound =
        "a" + std::string(ECUIO_EXPR_MAX_BYTES - 1, ' ');
    EXPECT_EQ(names_of(at_bound).status, ECUIO_OK);
    for (const size_t length : {size_t{ECUIO_EXPR_MAX_BYTES} + 1, SIZE_MAX}) {
        uint32_t count = 99;
        EXPECT_EQ(ecuio_expr_names(at_bound.data(), length, nullptr, 0, &count),
                  ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(count, 0u);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "the expression is longer than 256 characters (at "
                     "character 1)");
    }
}

// The serial ports' contract: the count alone, a short array, NULLs
TEST(SignalExprTest, ExprNamesFollowsTheSerialPortsContract)
{
    const std::string expr = "a + b * c";
    uint32_t count = 99;
    EXPECT_EQ(ecuio_expr_names(expr.data(), expr.size(), nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 3u);
    ecuio_expr_name_t two[3];
    std::memset(two, 0xA5, sizeof two);
    count = 99;
    EXPECT_EQ(ecuio_expr_names(expr.data(), expr.size(), two, 2, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_STREQ(two[0].text, "a");
    EXPECT_STREQ(two[1].text, "b");
    EXPECT_EQ(static_cast<unsigned char>(two[2].text[0]), 0xA5u);
    EXPECT_EQ(ecuio_expr_names(expr.data(), expr.size(), two, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_expr_names(expr.data(), expr.size(), nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_expr_names(nullptr, 1, two, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    /* No text at all is an expression that ends where a value should be */
    count = 99;
    EXPECT_EQ(ecuio_expr_names(nullptr, 0, nullptr, 0, &count),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(count, 0u);
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "the expression ends where a value should be (at character 1)");
    /* A success clears what an earlier refusal left */
    EXPECT_EQ(ecuio_expr_names(expr.data(), expr.size(), nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
}
