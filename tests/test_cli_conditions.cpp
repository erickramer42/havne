// File/layer: tests/test_cli_conditions.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliConditionTest (test_cli)
//
// cli/condition_lines.h: conditions check's arguments and its lines, over
// sets core's loader read.

#include "condition_lines.h"
#include "line_ring.h"

#include <gtest/gtest.h>

#include <ecuio/profile.h>

#include <chrono>
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
    cli::ConditionsCheckArgs args;
    std::string why;
    EXPECT_FALSE(
        cli::parse_conditions_check(a.argc(), a.argv.data(), &args, &why));
    return why;
}

struct Loaded {
    ecuio_condition_set_t* set = nullptr;
    explicit Loaded(const std::string& json)
    {
        EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Loaded() { ecuio_condition_set_free(set); }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
};

/* Condition i's line, resolved against signals when against is set */
std::string def_line(const Loaded& l, uint32_t i,
                     const ecuio_signal_set_t* signals, bool against,
                     size_t* bound = nullptr)
{
    ecuio_condition_info_t info{};
    ecuio_condition_term_t terms[ECUIO_CONDITION_MAX_TERMS] = {};
    ecuio_condition_binding_t binding{};
    EXPECT_EQ(ecuio_condition_set_get(l.set, i, &info), ECUIO_OK);
    for (uint32_t k = 0; k < info.term_count; ++k) {
        EXPECT_EQ(ecuio_condition_set_get_term(l.set, i, k, &terms[k]),
                  ECUIO_OK);
    }
    EXPECT_EQ(ecuio_condition_set_resolve(l.set, i, signals, nullptr, &binding),
              ECUIO_OK);
    std::vector<char> buf(cli::condition_def_bytes(info));
    cli::Line line(buf.data(), buf.size());
    cli::condition_def_line(line, info, terms, against ? &binding : nullptr,
                            signals);
    EXPECT_FALSE(line.full());
    if (bound != nullptr) {
        *bound = buf.size();
    }
    return std::string(line.data(), line.size());
}

std::string file_of(const std::string& conditions)
{
    return R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
}

const ecuio_signal_set_t* obd2()
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    return set;
}

} // namespace

TEST(CliConditionTest, CheckTakesAFileAndOneSetOfSignals)
{
    {
        Args a({"c.json"});
        cli::ConditionsCheckArgs args;
        std::string why;
        ASSERT_TRUE(
            cli::parse_conditions_check(a.argc(), a.argv.data(), &args, &why))
            << why;
        EXPECT_STREQ(args.file, "c.json");
        EXPECT_EQ(args.signals, nullptr);
        EXPECT_EQ(args.profile, nullptr);
    }
    {
        Args a({"--signals", "s.json", "c.json"});
        cli::ConditionsCheckArgs args;
        std::string why;
        ASSERT_TRUE(
            cli::parse_conditions_check(a.argc(), a.argv.data(), &args, &why))
            << why;
        EXPECT_STREQ(args.file, "c.json");
        EXPECT_STREQ(args.signals, "s.json");
        EXPECT_EQ(args.profile, nullptr);
    }
    {
        Args a({"c.json", "--profile", "obd2"});
        cli::ConditionsCheckArgs args;
        std::string why;
        ASSERT_TRUE(
            cli::parse_conditions_check(a.argc(), a.argv.data(), &args, &why))
            << why;
        EXPECT_STREQ(args.profile, "obd2");
        EXPECT_EQ(args.signals, nullptr);
    }
}

TEST(CliConditionTest, ChecksBadFormsAreUsage)
{
    EXPECT_EQ(why_of({}), "conditions check takes a condition file");
    EXPECT_EQ(why_of({""}), "conditions check takes a condition file");
    EXPECT_EQ(why_of({"--profile", "obd2"}),
              "conditions check takes a condition file");
    EXPECT_EQ(why_of({"a.json", "b.json"}), "conditions check takes one file");
    EXPECT_EQ(why_of({"a.json", "--signals"}),
              "--signals takes one signal file");
    EXPECT_EQ(why_of({"a.json", "--signals", ""}),
              "--signals takes one signal file");
    EXPECT_EQ(why_of({"a.json", "--signals", "s.json", "--signals", "t.json"}),
              "--signals takes one signal file");
    EXPECT_EQ(why_of({"a.json", "--profile"}),
              "--profile takes one profile name");
    EXPECT_EQ(why_of({"a.json", "--profile", "obd2", "--profile", "mock"}),
              "--profile takes one profile name");
    EXPECT_EQ(
        why_of({"a.json", "--signals", "s.json", "--profile", "obd2"}),
        "conditions check takes one of --signals, --profile and --plugin");
    EXPECT_EQ(
        why_of({"a.json", "--plugin", "p", "--profile", "obd2"}),
        "conditions check takes one of --signals, --profile and --plugin");
    EXPECT_EQ(
        why_of({"a.json", "--plugin", "p", "--signals", "s.json"}),
        "conditions check takes one of --signals, --profile and --plugin");
    EXPECT_EQ(why_of({"a.json", "--plugin"}), "--plugin takes one package");
    EXPECT_EQ(why_of({"a.json", "--plugin", "p", "--plugin", "q"}),
              "--plugin takes one package");
    EXPECT_EQ(why_of({"a.json", "--plugin", "p", "--variant"}),
              "--variant takes one variant's id");
    EXPECT_EQ(why_of({"a.json", "--variant", "base"}),
              "--variant names a --plugin's variant");
    EXPECT_EQ(why_of({"a.json", "--profile", "obd2", "--variant", "base"}),
              "--variant names a --plugin's variant");
    EXPECT_EQ(why_of({"a.json", "--package", "p"}), "unexpected '--package'");
    EXPECT_EQ(why_of({"--", "a.json"}), "unexpected '--'");
}

TEST(CliConditionTest, TheSetLineNamesTheChecksInForceAndTheSet)
{
    const auto line = [](const char* name, const ecuio_check_options_t& checks,
                         const char* against) {
        char buf[512];
        cli::Line l(buf, sizeof buf);
        cli::condition_set_line(l, name, 3, checks, against);
        EXPECT_FALSE(l.full());
        return std::string(l.data(), l.size());
    };
    ecuio_check_options_t none{};
    EXPECT_EQ(line("", none, nullptr),
              "{\"type\":\"condition_set\",\"name\":null,\"conditions\":3,"
              "\"gap_us\":1000000,\"rate_drop\":10,\"settle_us\":2000000,"
              "\"against\":null}");
    ecuio_check_options_t own{};
    own.gap_us = 2500000;
    own.rate_drop_x100 = 425;
    own.settle_us = 1;
    EXPECT_EQ(line("Daily \"driver\"", own, "user.daily-driver"),
              "{\"type\":\"condition_set\",\"name\":\"Daily \\\"driver\\\"\","
              "\"conditions\":3,\"gap_us\":2500000,\"rate_drop\":4.25,"
              "\"settle_us\":1,\"against\":\"user.daily-driver\"}");
}

TEST(CliConditionTest, AConditionsLineSaysWhatTheFileWrote)
{
    Loaded l(file_of(
        R"({"key":"hot","name":"Hot","when":{"signal":["obd2.coolant","id:1003"],)"
        R"("unit":"C","above":110.5,"clear":105,"max_age":0.5},"for":2,)"
        R"("clear_for":0.25,"alert":{"level":"danger","sound":true},"mark":true},)"
        R"({"key":"other","name":"Other","enabled":false,"when":{"any":[)"
        R"({"link":"request_failed"},{"check":"late_id"},)"
        R"({"signal":["key:gear"],"differs":2}]},"record":{"before":1.5}})"));
    ASSERT_NE(l.set, nullptr);
    EXPECT_EQ(
        def_line(l, 0, nullptr, false),
        "{\"type\":\"condition_def\",\"key\":\"hot\",\"name\":\"Hot\","
        "\"enabled\":true,\"join\":\"all\",\"terms\":[{\"kind\":\"signal\","
        "\"signal\":[\"obd2.coolant\",\"id:1003\"],\"unit\":\"C\","
        "\"test\":\"above\",\"level\":110.5,\"clear\":105,"
        "\"max_age_us\":500000}],\"for_us\":2000000,"
        "\"clear_for_us\":250000,\"alert\":{\"level\":\"danger\","
        "\"sound\":true,\"banner\":false},\"record\":null,\"mark\":true}");
    EXPECT_EQ(
        def_line(l, 1, nullptr, false),
        "{\"type\":\"condition_def\",\"key\":\"other\",\"name\":\"Other\","
        "\"enabled\":false,\"join\":\"any\",\"terms\":[{\"kind\":\"link\","
        "\"link\":\"request_failed\"},{\"kind\":\"check\","
        "\"check\":\"late_id\"},{\"kind\":\"signal\","
        "\"signal\":[\"key:gear\"],\"unit\":null,\"test\":\"differs\","
        "\"level\":2,\"clear\":null,\"max_age_us\":null}],\"for_us\":0,"
        "\"clear_for_us\":0,\"alert\":null,\"record\":{\"before_us\":1500000,"
        "\"after_us\":0},\"mark\":false}");
}

TEST(CliConditionTest, AgainstASetEachTermSaysWhatItTookAndTheConditionWhyNot)
{
    Loaded l(file_of(
        R"({"key":"revs","name":"Revs","when":{"signal":["id:1001","obd2.rpm"],)"
        R"("unit":"RPM","below":500}},)"
        R"({"key":"psi","name":"Psi","when":{"all":[{"link":"timeout"},)"
        R"({"signal":["obd2.map"],"unit":"psi","above":20},)"
        R"({"signal":["key:none"],"equals":1}]}},)"
        R"({"key":"off","name":"Off","enabled":false,"when":{"check":"gap"}})"));
    ASSERT_NE(l.set, nullptr);
    EXPECT_EQ(
        def_line(l, 0, obd2(), true),
        "{\"type\":\"condition_def\",\"key\":\"revs\",\"name\":\"Revs\","
        "\"enabled\":true,\"join\":\"all\",\"terms\":[{\"kind\":\"signal\","
        "\"signal\":[\"id:1001\",\"obd2.rpm\"],\"unit\":\"RPM\","
        "\"test\":\"below\",\"level\":500,\"clear\":null,"
        "\"max_age_us\":null,\"resolved\":{\"reference\":\"obd2.rpm\","
        "\"id\":7003,\"name\":\"RPM\",\"unit\":\"RPM\"}}],\"for_us\":0,"
        "\"clear_for_us\":0,\"alert\":null,\"record\":null,\"mark\":false,"
        "\"available\":true,\"why\":null,\"why_term\":null}");
    const std::string psi = def_line(l, 1, obd2(), true);
    EXPECT_NE(psi.find("{\"kind\":\"link\",\"link\":\"timeout\"},"),
              std::string::npos)
        << "a link term has nothing to resolve: " << psi;
    EXPECT_NE(
        psi.find("\"resolved\":{\"reference\":\"obd2.map\",\"id\":11088,"
                 "\"name\":\"Intake Manifold Pressure\",\"unit\":\"kPa\"}"),
        std::string::npos)
        << "the def a term can't be judged on is still named: " << psi;
    EXPECT_NE(
        psi.find("\"signal\":[\"key:none\"],\"unit\":null,\"test\":\"equals\","
                 "\"level\":1,\"clear\":null,\"max_age_us\":null,"
                 "\"resolved\":null}"),
        std::string::npos)
        << psi;
    EXPECT_NE(psi.find("\"available\":false,\"why\":\"unit\",\"why_term\":1}"),
              std::string::npos)
        << psi;
    const std::string off = def_line(l, 2, obd2(), true);
    EXPECT_NE(
        off.find("\"available\":false,\"why\":\"disabled\",\"why_term\":null}"),
        std::string::npos)
        << off;
    /* Against no signals at all (the raw profile) a signal term has none */
    const std::string raw = def_line(l, 0, nullptr, true);
    EXPECT_NE(raw.find("\"resolved\":null}],"), std::string::npos) << raw;
    EXPECT_NE(
        raw.find("\"available\":false,\"why\":\"no_signal\",\"why_term\":0}"),
        std::string::npos)
        << raw;
}

TEST(CliConditionTest, EachWordIsTheFilesOwn)
{
    EXPECT_STREQ(cli::term_test_word(ECUIO_TERM_ABOVE), "above");
    EXPECT_STREQ(cli::term_test_word(ECUIO_TERM_BELOW), "below");
    EXPECT_STREQ(cli::term_test_word(ECUIO_TERM_EQUALS), "equals");
    EXPECT_STREQ(cli::term_test_word(ECUIO_TERM_DIFFERS), "differs");
    EXPECT_STREQ(cli::term_test_word(0), "unknown");
    EXPECT_STREQ(cli::term_link_word(ECUIO_TERM_LINK_NOT_OK), "not_ok");
    EXPECT_STREQ(cli::term_link_word(ECUIO_TERM_LINK_TIMEOUT), "timeout");
    EXPECT_STREQ(cli::term_link_word(ECUIO_TERM_LINK_REQUEST_FAILED),
                 "request_failed");
    EXPECT_STREQ(cli::term_link_word(ECUIO_TERM_LINK_HARD_FAILURE),
                 "hard_failure");
    EXPECT_STREQ(cli::term_link_word(5), "unknown");
    EXPECT_STREQ(cli::term_check_word(ECUIO_CHECK_GAP), "gap");
    EXPECT_STREQ(cli::term_check_word(ECUIO_CHECK_RATE_DROP), "rate_drop");
    EXPECT_STREQ(cli::term_check_word(ECUIO_CHECK_LATE_ID), "late_id");
    EXPECT_STREQ(cli::term_check_word(ECUIO_CHECK_OVERFLOW), "overflow");
    EXPECT_STREQ(cli::term_check_word(ECUIO_CHECK_ISOTP), "isotp");
    EXPECT_STREQ(cli::term_check_word(ECUIO_CHECK_LINK), "unknown");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_DISABLED),
                 "disabled");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_NO_SIGNAL),
                 "no_signal");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_UNIT), "unit");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_NOT_A_NUMBER),
                 "not_a_number");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_ENUM_ORDER),
                 "enum_order");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_NO_LINK),
                 "no_link");
    EXPECT_STREQ(cli::condition_why_word(ECUIO_CONDITION_WHY_NO_CHECK),
                 "no_check");
    EXPECT_STREQ(cli::condition_why_word(99), "unknown");
}

TEST(CliConditionTest, CheckTakesAPluginAndItsVariant)
{
    Args a({"c.json", "--plugin", "pkg", "--variant", "my2019"});
    cli::ConditionsCheckArgs args;
    std::string why;
    ASSERT_TRUE(
        cli::parse_conditions_check(a.argc(), a.argv.data(), &args, &why))
        << why;
    EXPECT_STREQ(args.plugin, "pkg");
    EXPECT_STREQ(args.variant, "my2019");
    EXPECT_EQ(args.signals, nullptr);
    EXPECT_EQ(args.profile, nullptr);
}

TEST(CliConditionTest, TheStreamsLinesSayTheSetAndEachEvent)
{
    Loaded l(R"({"havne_conditions":1,"name":"Desk","conditions":[)"
             R"({"key":"hot","name":"Hot","when":{"link":"timeout"},)"
             R"("alert":{"level":"danger"},"mark":true},)"
             R"({"key":"revs","name":"Revs","when":{"signal":["obd2.rpm"],)"
             R"("unit":"RPM","above":1},"record":{}}]})");
    ASSERT_NE(l.set, nullptr);
    ecuio_condition_info_t infos[2] = {};
    ASSERT_EQ(ecuio_condition_set_get(l.set, 0, &infos[0]), ECUIO_OK);
    ASSERT_EQ(ecuio_condition_set_get(l.set, 1, &infos[1]), ECUIO_OK);
    ecuio_condition_state_t states[2] = {};
    states[0].flags = ECUIO_CONDITION_UNAVAILABLE;
    states[0].why = ECUIO_CONDITION_WHY_NO_LINK;
    states[1].flags = ECUIO_CONDITION_STALE;
    std::vector<char> buf(cli::conditions_line_bytes("Desk", 2));
    cli::Line line(buf.data(), buf.size());
    cli::conditions_line(line, "Desk", 3, infos, states, 2);
    EXPECT_FALSE(line.full());
    EXPECT_EQ(std::string(line.data(), line.size()),
              "{\"type\":\"conditions\",\"name\":\"Desk\",\"generation\":3,"
              "\"conditions\":[{\"key\":\"hot\",\"alert\":\"danger\","
              "\"record\":false,\"mark\":true,\"available\":false,"
              "\"why\":\"no_link\"},{\"key\":\"revs\",\"alert\":null,"
              "\"record\":true,\"mark\":false,\"available\":true,"
              "\"why\":null}]}");
    char small[512];
    cli::Line none(small, sizeof small);
    cli::conditions_line(none, "", 1, infos, states, 0);
    EXPECT_EQ(std::string(none.data(), none.size()),
              "{\"type\":\"conditions\",\"name\":null,\"generation\":1,"
              "\"conditions\":[]}");

    ecuio_condition_event_t e{};
    e.timestamp_us = 12431000;
    e.condition = 1;
    e.state = ECUIO_CONDITION_ON;
    e.flags = ECUIO_CONDITION_FIRED;
    e.term = 2;
    e.value = 112.5;
    char one[cli::kConditionLineBytes];
    cli::Line event(one, sizeof one);
    cli::condition_event_line(event, e, "revs");
    EXPECT_EQ(std::string(event.data(), event.size()),
              "{\"type\":\"condition\",\"t_us\":12431000,\"key\":\"revs\","
              "\"state\":\"on\",\"fired\":true,\"stale\":false,\"value\":112.5,"
              "\"term\":2}");
    e.state = ECUIO_CONDITION_OFF;
    e.flags = ECUIO_CONDITION_STALE;
    /* The longest key, every byte a character to escape, still fits */
    const std::string key(ECUIO_CONDITION_KEY_MAX, '\x01');
    cli::Line longest(one, sizeof one);
    cli::condition_event_line(longest, e, key.c_str());
    EXPECT_FALSE(longest.full());
    EXPECT_NE(std::string(longest.data(), longest.size())
                  .find("\"state\":\"off\",\"fired\":false,\"stale\":true"),
              std::string::npos);
}

/* The longest line a file can ask for: four terms of eight references,
 * each the longest a reference can be and the last the one taken, the
 * unit, the condition's name and the resolved def's name and unit all
 * control characters (six bytes each on the line). It fits its bound,
 * and needs more than one term's share of it. */
TEST(CliConditionTest, TheFullestConditionFitsItsLine)
{
    const std::string key31(31, 'k');
    std::string unit;
    for (int i = 0; i < 15; ++i) {
        unit += "\\u0001";
    }
    std::string terms;
    for (int t = 0; t < 4; ++t) {
        std::string refs;
        for (int r = 0; r < 7; ++r) {
            std::string key(31, static_cast<char>('a' + t));
            key[30] = static_cast<char>('a' + r);
            refs += "\"key:" + key + "\",";
        }
        refs += "\"id:1999" + std::to_string(t) + "\"";
        terms += std::string(t == 0 ? "" : ",") + R"({"signal":[)" + refs +
                 R"(],"unit":")" + unit +
                 R"(","above":-1.2345678901234567e-300,)"
                 R"("clear":-1.2345678901234567e+300,"max_age":59.999999})";
    }
    std::string name;
    for (int i = 0; i < 40; ++i) {
        name += "\\u0001";
    }
    /* Four defs with the longest name and unit, each a control character */
    ecuio_signal_def_t defs[4] = {};
    for (int t = 0; t < 4; ++t) {
        defs[t].id = static_cast<uint16_t>(19990 + t);
        defs[t].kind = ECUIO_VALUE_UINT;
        std::memset(defs[t].name, 1, sizeof defs[t].name - 1);
        std::memset(defs[t].unit, 1, sizeof defs[t].unit - 1);
    }
    ecuio_signal_set_t four{};
    four.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    four.defs = defs;
    four.count = 4;
    Loaded l(
        file_of(R"({"key":")" + key31 + R"(","name":")" + name +
                R"(","when":{"any":[)" + terms +
                R"(]},"for":599.999999,"clear_for":599.999999,)"
                R"("alert":{"level":"warning","sound":true,"banner":true},)"
                R"("record":{"before":59.999999,"after":599.999999},)"
                R"("mark":true})"));
    ASSERT_NE(l.set, nullptr);
    size_t bound = 0;
    const std::string line = def_line(l, 0, &four, true, &bound);
    EXPECT_LE(line.size() + 1, bound) << "the line and its line end";
    EXPECT_GT(line.size(), bound - 3 * cli::condition_term_bytes())
        << "the bound counts every term";
    EXPECT_NE(
        line.find("\"resolved\":{\"reference\":\"id:19993\",\"id\":19993,"),
        std::string::npos)
        << line;
    EXPECT_NE(line.find("\"available\":true"), std::string::npos) << line;
    EXPECT_EQ(line.back(), '}');
}

/* A frame's events are each a line, or each counted: never neither */
TEST(CliConditionTest, AFramesEventsAreWrittenOrCounted)
{
    ecuio_condition_event_t events[3] = {};
    events[0].condition = 1;
    events[0].state = ECUIO_CONDITION_ON;
    events[0].flags = ECUIO_CONDITION_FIRED;
    events[1].condition = 0;
    events[2].condition = 1;
    const std::vector<std::string> keys = {"hot", "revs"};
    std::vector<std::string> lines;
    int refuse = -1; /* the push to refuse, counted from 0 */
    const auto push = [&lines, &refuse](const char* line, size_t n) {
        if (static_cast<int>(lines.size()) == refuse) {
            refuse = -2;
            return false;
        }
        lines.emplace_back(line, n);
        return true;
    };
    const auto keys_written = [&lines] {
        std::string out;
        for (const std::string& l : lines) {
            const size_t at = l.find("\"key\":\"") + 7;
            out += l.substr(at, l.find('"', at) - at) + " ";
        }
        lines.clear();
        return out;
    };

    EXPECT_EQ(cli::write_condition_events(events, 3, keys, true, false, push),
              0u);
    EXPECT_EQ(keys_written(), "revs hot revs ");
    /* The frame's own line was dropped: its events go with it, counted */
    EXPECT_EQ(cli::write_condition_events(events, 3, keys, false, false, push),
              3u);
    EXPECT_EQ(keys_written(), "");
    /* Past --count the stream has ended: nothing written, nothing lost */
    EXPECT_EQ(cli::write_condition_events(events, 3, keys, false, true, push),
              0u);
    EXPECT_EQ(cli::write_condition_events(events, 3, keys, true, true, push),
              0u);
    EXPECT_EQ(keys_written(), "");
    /* A line the ring has no room for */
    refuse = 1;
    EXPECT_EQ(cli::write_condition_events(events, 3, keys, true, false, push),
              1u);
    EXPECT_EQ(keys_written(), "revs revs ");
    /* An event of a condition the stream has no key for */
    events[1].condition = 2;
    EXPECT_EQ(cli::write_condition_events(events, 3, keys, true, false, push),
              1u);
    EXPECT_EQ(keys_written(), "revs revs ");
    EXPECT_EQ(cli::write_condition_events(events, 0, keys, false, false, push),
              0u);
}

/* The ring counts a dropped frame and a dropped values line, and leaves
 * a condition line's drop to its caller */
TEST(CliConditionTest, TheRingLeavesAConditionLinesDropToItsCaller)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(8));
    const char line[] = "0123456789\n";
    EXPECT_FALSE(ring.push(cli::LineKind::Condition, line, 11, false));
    EXPECT_EQ(ring.dropped_frames(), 0u);
    EXPECT_EQ(ring.dropped_values(), 0u);
    EXPECT_FALSE(ring.push(cli::LineKind::Values, line, 11, false));
    EXPECT_EQ(ring.dropped_values(), 1u);
    char out[8];
    EXPECT_EQ(ring.drain(out, std::chrono::milliseconds(0)), 0u)
        << "no dropped line is owed for the condition line";
}
