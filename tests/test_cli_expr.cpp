// File/layer: tests/test_cli_expr.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliExprTest (test_cli)
//
// cli/expr_lines.h: signals expr's arguments, the def a --value's name
// reads, and its lines over a file core's loader read.

#include "expr_lines.h"

#include <gtest/gtest.h>

#include <ecuio/signal_sets.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Args {
    std::vector<std::string> words;
    std::vector<const char*> argv;
    explicit Args(std::vector<std::string> w) : words(std::move(w))
    {
        for (const std::string& s : words) {
            argv.push_back(s.c_str());
        }
    }
    int argc() const { return static_cast<int>(argv.size()); }
};

std::string why_of(std::vector<std::string> w)
{
    Args a(std::move(w));
    cli::ExprArgs args;
    std::string why;
    EXPECT_FALSE(cli::parse_signals_expr(a.argc(), a.argv.data(), &args, &why));
    return why;
}

// A file extending obd2: a number, a text, a computed signal over the
// number and obd2's coolant, and one over that
const char kFile[] =
    "{\"havne_signals\":2,\"name\":\"Expr\",\"extends\":\"obd2\","
    "\"source\":{\"name\":\"t\"},\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],"
    "\"signals\":["
    "{\"key\":\"oil\",\"name\":\"Oil\",\"read\":{\"service\":\"22\","
    "\"number\":\"F40D\"},\"field\":{\"byte\":0},\"unit\":\"kPa\"},"
    "{\"key\":\"label\",\"name\":\"Label\",\"kind\":\"text\",\"read\":"
    "{\"service\":\"22\",\"number\":\"F40E\"},\"field\":{\"byte\":0}},"
    "{\"key\":\"ratio\",\"name\":\"Ratio\",\"kind\":\"computed\","
    "\"expr\":\"oil / obd2.rpm\",\"max_age\":{\"oil\":1}},"
    "{\"key\":\"high\",\"name\":\"High\",\"kind\":\"computed\","
    "\"expr\":\"if(ratio > 1, 1, 0)\"}]}";

struct Loaded {
    ecuio_signal_file_t* file = nullptr;
    explicit Loaded(const char* json = kFile)
    {
        EXPECT_EQ(ecuio_signal_file_load(json, std::strlen(json), &file),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Loaded() { ecuio_signal_file_free(file); }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;

    const ecuio_signal_set_t& set() const
    {
        return *ecuio_signal_file_set(file);
    }
    // The def named so, its ID as text
    std::string id(const char* name) const
    {
        for (uint32_t i = 0; i < set().count; ++i) {
            if (std::strcmp(set().defs[i].name, name) == 0) {
                return std::to_string(set().defs[i].id);
            }
        }
        ADD_FAILURE() << "no def is named " << name;
        return "?";
    }
};

// What a check of expr answered, and its leaves
struct Checked {
    ecuio_status_t status = ECUIO_OK;
    ecuio_expr_check_t out{};
    std::string detail;
    std::vector<uint16_t> leaves;

    Checked(const Loaded& f, const char* key, const char* expr)
    {
        out.struct_size = sizeof out;
        status = ecuio_signal_file_check_expr(f.file, key, expr,
                                              std::strlen(expr), &out);
        detail = ecuio_last_error_detail();
        if (status == ECUIO_OK) {
            uint32_t count = 0;
            EXPECT_EQ(ecuio_signal_file_expr_leaves(f.file, &out.program,
                                                    nullptr, 0, &count),
                      ECUIO_OK);
            leaves.resize(count);
            EXPECT_EQ(ecuio_signal_file_expr_leaves(
                          f.file, &out.program, leaves.data(), count, &count),
                      ECUIO_OK);
        }
    }
};

std::string expr_line_of(const Loaded& f, const char* key, const char* expr)
{
    const Checked c(f, key, expr);
    std::vector<char> buf(cli::expr_line_bytes(
        expr, key, c.detail.c_str(), static_cast<uint32_t>(c.leaves.size())));
    cli::Line l(buf.data(), buf.size());
    cli::expr_line(l, f.file, expr, key, c.out,
                   c.status == ECUIO_OK ? nullptr : c.detail.c_str(),
                   c.leaves.data(), static_cast<uint32_t>(c.leaves.size()));
    EXPECT_FALSE(l.full()) << expr;
    return std::string(l.data(), l.size());
}

// A value line for expr worked out from the values given
std::string value_line_of(const Loaded& f, const char* expr,
                          std::vector<cli::ExprValue> values)
{
    const Checked c(f, nullptr, expr);
    EXPECT_EQ(c.status, ECUIO_OK) << c.detail;
    std::string why;
    EXPECT_TRUE(cli::resolve_expr_values(f.file, &values, &why)) << why;
    std::vector<ecuio_eval_input_t> inputs(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        inputs[i].value = values[i].value;
        inputs[i].age_us = values[i].age_us;
        inputs[i].def_id = values[i].def_id;
    }
    ecuio_eval_result_t r{};
    r.struct_size = sizeof r;
    EXPECT_EQ(ecuio_signal_file_evaluate(f.file, &c.out.program, inputs.data(),
                                         static_cast<uint32_t>(inputs.size()),
                                         &r),
              ECUIO_OK);
    char buf[cli::kValueLineBytes];
    cli::Line l(buf, sizeof buf);
    cli::value_line(l, f.file, r);
    EXPECT_FALSE(l.full());
    return std::string(l.data(), l.size());
}

cli::ExprValue value(const char* text)
{
    cli::ExprValue v;
    EXPECT_TRUE(cli::parse_expr_value(text, &v)) << text;
    return v;
}

} // namespace

TEST(CliExprTest, SignalsExprTakesAFileAnExpressionAndItsValues)
{
    {
        Args a({"--signals", "f.json"});
        cli::ExprArgs args;
        std::string why;
        ASSERT_TRUE(
            cli::parse_signals_expr(a.argc(), a.argv.data(), &args, &why))
            << why;
        EXPECT_STREQ(args.signals, "f.json");
        EXPECT_EQ(args.plugin, nullptr);
        EXPECT_EQ(args.expr, nullptr);
        EXPECT_EQ(args.key, nullptr);
        EXPECT_TRUE(args.values.empty());
    }
    {
        // The options before and after the expression; a leading minus is
        // the expression's
        Args a({"--value", "oil=1.5", "--plugin", "pkg", "-oil * 2", "--key",
                "boost", "--variant", "base", "--value", "obd2.map=101@2.5"});
        cli::ExprArgs args;
        std::string why;
        ASSERT_TRUE(
            cli::parse_signals_expr(a.argc(), a.argv.data(), &args, &why))
            << why;
        EXPECT_EQ(args.signals, nullptr);
        EXPECT_STREQ(args.plugin, "pkg");
        EXPECT_STREQ(args.variant, "base");
        EXPECT_STREQ(args.key, "boost");
        EXPECT_STREQ(args.expr, "-oil * 2");
        ASSERT_EQ(args.values.size(), 2u);
        EXPECT_EQ(args.values[0].ref, "oil");
        EXPECT_EQ(args.values[0].value, 1.5);
        EXPECT_EQ(args.values[0].age_us, 0u);
        EXPECT_EQ(args.values[1].ref, "obd2.map");
        EXPECT_EQ(args.values[1].value, 101.0);
        EXPECT_EQ(args.values[1].age_us, 2500000u);
    }
    {
        // An expression given empty is still one: core says why it isn't
        Args a({"--signals", "f.json", ""});
        cli::ExprArgs args;
        std::string why;
        ASSERT_TRUE(
            cli::parse_signals_expr(a.argc(), a.argv.data(), &args, &why));
        ASSERT_NE(args.expr, nullptr);
        EXPECT_STREQ(args.expr, "");
    }
}

TEST(CliExprTest, SignalsExprsBadFormsAreUsage)
{
    const char* const source =
        "signals expr takes --signals <file> or --plugin <pkg>";
    EXPECT_EQ(why_of({}), source);
    EXPECT_EQ(why_of({"a + b"}), source);
    EXPECT_EQ(why_of({"--signals", "f.json", "--plugin", "p"}), source);
    EXPECT_EQ(why_of({"--signals"}), "--signals takes one signal file");
    EXPECT_EQ(why_of({"--signals", ""}), "--signals takes one signal file");
    EXPECT_EQ(why_of({"--signals", "a", "--signals", "b"}),
              "--signals takes one signal file");
    EXPECT_EQ(why_of({"--plugin"}), "--plugin takes one package");
    EXPECT_EQ(why_of({"--plugin", "p", "--variant"}),
              "--variant takes one variant's id");
    EXPECT_EQ(why_of({"--signals", "f.json", "--variant", "base"}),
              "--variant names a --plugin's variant");
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "b"}),
              "signals expr takes one expression: quote it whole");
    EXPECT_EQ(why_of({"--signals", "f.json", "--fast"}),
              "unexpected '--fast': an expression that starts with -- goes in "
              "parentheses");
    EXPECT_EQ(why_of({"--signals", "f.json", "--a"}),
              "unexpected '--a': an expression that starts with -- goes in "
              "parentheses");
    /* In parentheses it is the expression */
    Args paren({"--signals", "f.json", "(--a)"});
    cli::ExprArgs taken;
    std::string none;
    EXPECT_TRUE(
        cli::parse_signals_expr(paren.argc(), paren.argv.data(), &taken, &none))
        << none;
    EXPECT_STREQ(taken.expr, "(--a)");
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "--key"}),
              "--key takes one signal's key");
    EXPECT_EQ(why_of({"--signals", "f.json", "--key", "boost"}),
              "--key goes with an expression: the signal it is for");
    const char* const key =
        "--key takes a signal's key: 1 to 31 of a-z, 0-9 and _";
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "--key", "Boost"}), key);
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "--key", "obd2.map"}), key);
    EXPECT_EQ(
        why_of({"--signals", "f.json", "a", "--key", std::string(32, 'k')}),
        key);
    {
        // 31 is a key
        Args a({"--signals", "f.json", "a", "--key", std::string(31, 'k')});
        cli::ExprArgs args;
        std::string why;
        EXPECT_TRUE(
            cli::parse_signals_expr(a.argc(), a.argv.data(), &args, &why))
            << why;
    }
    EXPECT_EQ(why_of({"--signals", "f.json", "--value", "a=1"}),
              "--value goes with an expression");
    const char* const form = "--value takes <name>=<number>[@<age in seconds>]";
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "--value"}), form);
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "--value", "a"}), form);
    EXPECT_EQ(why_of({"--signals", "f.json", "a", "--value", "a=1", "--value",
                      "a=2"}),
              "--value gives 'a' two values");
}

TEST(CliExprTest, AValueIsANameANumberAndAnAge)
{
    cli::ExprValue v = value("a=1");
    EXPECT_EQ(v.ref, "a");
    EXPECT_EQ(v.value, 1.0);
    EXPECT_EQ(v.age_us, 0u);
    v = value("obd2.pid_14_1=-2.5e1@0.25");
    EXPECT_EQ(v.ref, "obd2.pid_14_1");
    EXPECT_EQ(v.value, -25.0);
    EXPECT_EQ(v.age_us, 250000u);
    // A day is the oldest
    EXPECT_EQ(value("a=0@86400").age_us, 86400000000ull);
    for (const char* bad : {"", "a", "=1", "a=", "a=x", "a=nan", "a=inf",
                            "a=1e999", "a=1 ", "a=1@", "a=1@x", "a=1@-0.5",
                            "a=1@86400.000001", "a=1@2@3", "a=1=2", "a=@1"}) {
        cli::ExprValue none;
        EXPECT_FALSE(cli::parse_expr_value(bad, &none)) << bad;
    }
}

TEST(CliExprTest, AValuesNameIsReadAsTheExpressionReadsIt)
{
    const Loaded f;
    std::string why;
    {
        std::vector<cli::ExprValue> values = {value("oil=3"),
                                              value("obd2.pid_0c=900")};
        ASSERT_TRUE(cli::resolve_expr_values(f.file, &values, &why)) << why;
        EXPECT_EQ(std::to_string(values[0].def_id), f.id("Oil"));
        EXPECT_EQ(values[1].def_id, ECUIO_OBD2_SIG_RPM_ID);
    }
    const auto refused = [&](std::vector<const char*> texts) {
        std::vector<cli::ExprValue> values;
        for (const char* t : texts) {
            values.push_back(value(t));
        }
        std::string words;
        EXPECT_FALSE(cli::resolve_expr_values(f.file, &values, &words));
        return words;
    };
    EXPECT_EQ(refused({"bost=1"}),
              "--value bost: the expression reads 'bost', which no signal is");
    EXPECT_EQ(refused({"label=1"}),
              "--value label: the expression reads 'label', which is text");
    EXPECT_EQ(refused({"oil=1", "ratio=2"}),
              "--value ratio: the signal is computed: give the signals it "
              "reads their values");
    EXPECT_EQ(refused({"oil+1=2"}),
              "--value oil+1: a value goes to a signal by its name");
    // Two spellings of one def
    EXPECT_EQ(refused({"obd2.rpm=900", "oil=1", "obd2.pid_0c=901"}),
              "--value gives one signal two values, as 'obd2.rpm' and "
              "'obd2.pid_0c'");
}

TEST(CliExprTest, TheExprLineSaysWhatTheExpressionReads)
{
    const Loaded f;
    const std::string oil = "{\"ref\":\"oil\",\"id\":" + f.id("Oil") +
                            ",\"name\":\"Oil\",\"unit\":\"kPa\","
                            "\"kind\":\"number\"}";
    const std::string rpm = "{\"ref\":\"obd2.rpm\",\"id\":7003,"
                            "\"name\":\"RPM\",\"unit\":\"RPM\","
                            "\"kind\":\"number\"}";
    const std::string high = "{\"ref\":\"high\",\"id\":" + f.id("High") +
                             ",\"name\":\"High\",\"unit\":\"\","
                             "\"kind\":\"computed\"}";
    // A chain three deep: the inputs as written, the leaves under them
    // each once in the order they are first read
    EXPECT_EQ(expr_line_of(f, nullptr, "high + obd2.rpm - oil"),
              "{\"type\":\"expr\",\"expr\":\"high + obd2.rpm - oil\","
              "\"key\":null,\"ok\":true,\"at\":null,\"detail\":null,"
              "\"format\":2,\"file_format\":2,\"depth\":3,\"inputs\":[" +
                  high + "," + rpm + "," + oil + "],\"leaves\":[" + oil + "," +
                  rpm + "]}");
    // Plain arithmetic is format 1's, whatever the file's; the key is the
    // signal it is for
    EXPECT_EQ(expr_line_of(f, "high", "oil * 2"),
              "{\"type\":\"expr\",\"expr\":\"oil * 2\",\"key\":\"high\","
              "\"ok\":true,\"at\":null,\"detail\":null,\"format\":1,"
              "\"file_format\":2,\"depth\":1,\"inputs\":[" +
                  oil + "],\"leaves\":[" + oil + "]}");
    // In ratio's place it is read by high, a chain, which format 2 brought
    const std::string read = expr_line_of(f, "ratio", "oil * 2");
    EXPECT_NE(read.find("\"key\":\"ratio\",\"ok\":true,\"at\":null,"
                        "\"detail\":null,\"format\":2,"),
              std::string::npos)
        << read;
}

TEST(CliExprTest, ARefusedExpressionsLineSaysWhereAndWhy)
{
    const Loaded f;
    EXPECT_EQ(expr_line_of(f, nullptr, "oil + bost"),
              "{\"type\":\"expr\",\"expr\":\"oil + bost\",\"key\":null,"
              "\"ok\":false,\"at\":7,\"detail\":\"the expression reads "
              "'bost', which no signal is\",\"format\":null,"
              "\"file_format\":2,\"depth\":null,\"inputs\":[],"
              "\"leaves\":[]}");
    // An edit that would read itself, through the signal that reads it
    const std::string circle = expr_line_of(f, "ratio", "high * 2");
    EXPECT_NE(circle.find("\"key\":\"ratio\",\"ok\":false,\"at\":1,"),
              std::string::npos)
        << circle;
    // The longest expression, every byte escaped, still fits its line
    const std::string quotes(ECUIO_EXPR_MAX_BYTES, '"');
    const std::string line = expr_line_of(f, nullptr, quotes.c_str());
    EXPECT_NE(line.find("\"ok\":false,\"at\":1,"), std::string::npos) << line;
}

TEST(CliExprTest, TheInputLinesAreWhatAnExpressionCanRead)
{
    const Loaded f;
    std::vector<std::string> lines;
    for (uint32_t i = 0; i < f.set().count; ++i) {
        const ecuio_signal_def_t& d = f.set().defs[i];
        if (!cli::expr_reads(f.file, d)) {
            // A text, and obd2's computed Boost, which has no name there
            EXPECT_TRUE(std::strcmp(d.name, "Label") == 0 ||
                        d.id == ECUIO_OBD2_SIG_BOOST_ID)
                << d.name;
            continue;
        }
        char buf[cli::kExprDefBytes + 64];
        cli::Line l(buf, sizeof buf);
        cli::input_line(l, f.file, d);
        EXPECT_FALSE(l.full());
        lines.emplace_back(l.data(), l.size());
    }
    const auto has = [&](const std::string& line) {
        for (const std::string& one : lines) {
            if (one == line) {
                return true;
            }
        }
        return false;
    };
    EXPECT_EQ(lines.size(), size_t{f.set().count} - 2);
    EXPECT_TRUE(has("{\"type\":\"input\",\"ref\":\"obd2.rpm\",\"id\":7003,"
                    "\"name\":\"RPM\",\"unit\":\"RPM\",\"kind\":\"number\","
                    "\"own\":false}"));
    EXPECT_TRUE(
        has("{\"type\":\"input\",\"ref\":\"oil\",\"id\":" + f.id("Oil") +
            ",\"name\":\"Oil\",\"unit\":\"kPa\",\"kind\":\"number\","
            "\"own\":true}"));
    EXPECT_TRUE(
        has("{\"type\":\"input\",\"ref\":\"ratio\",\"id\":" + f.id("Ratio") +
            ",\"name\":\"Ratio\",\"unit\":\"\",\"kind\":\"computed\","
            "\"own\":true}"));
}

TEST(CliExprTest, TheValueLineSaysTheValueOrWhyThereIsNone)
{
    const Loaded f;
    EXPECT_EQ(
        value_line_of(f, "ratio * 4", {value("oil=3"), value("obd2.rpm=1.5")}),
        "{\"type\":\"value\",\"value\":8,\"why\":null,\"leaf\":null,"
        "\"bound_us\":null}");
    EXPECT_EQ(value_line_of(f, "ratio * 4", {value("oil=3")}),
              "{\"type\":\"value\",\"value\":null,\"why\":\"missing\","
              "\"leaf\":\"obd2.rpm\",\"bound_us\":null}");
    // ratio reads oil within a second, the file's max_age
    EXPECT_EQ(value_line_of(f, "ratio * 4",
                            {value("oil=3@1.5"), value("obd2.rpm=1.5")}),
              "{\"type\":\"value\",\"value\":null,\"why\":\"stale\","
              "\"leaf\":\"oil\",\"bound_us\":1000000}");
    EXPECT_EQ(
        value_line_of(f, "ratio * 4", {value("oil=3"), value("obd2.rpm=0")}),
        "{\"type\":\"value\",\"value\":null,\"why\":\"no_number\","
        "\"leaf\":null,\"bound_us\":null}");
    // A why this CLI doesn't know
    ecuio_eval_result_t later{};
    later.why = 9;
    later.leaf = 7003;
    later.bound_us = 5;
    char buf[cli::kValueLineBytes];
    cli::Line l(buf, sizeof buf);
    cli::value_line(l, f.file, later);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"value\",\"value\":null,\"why\":\"unknown\","
              "\"leaf\":null,\"bound_us\":null}");
}

TEST(CliExprTest, ACaretSitsUnderTheRefusedCharacter)
{
    EXPECT_EQ(cli::expr_caret("oil + bost", 7), "  oil + bost\n        ^\n");
    EXPECT_EQ(cli::expr_caret("oil +", 1), "  oil +\n  ^\n");
    // The end's next column, where a value should be
    EXPECT_EQ(cli::expr_caret("oil +", 6), "  oil +\n       ^\n");
    EXPECT_EQ(cli::expr_caret("oil +", 7), "");
    EXPECT_EQ(cli::expr_caret("oil +", 0), "");
    EXPECT_EQ(cli::expr_caret("", 1), "  \n  ^\n");
    // A column is a character only through printable ASCII
    EXPECT_EQ(cli::expr_caret("oil\t+ bost", 7), "");
    EXPECT_EQ(cli::expr_caret("oil \x7F bost", 7), "");
    EXPECT_EQ(cli::expr_caret("\xC3\xA5 + bost", 6), "");
    EXPECT_EQ(cli::expr_caret("oil + \xC3\xA5", 7),
              "  oil + \xC3\xA5\n        ^\n");
}
