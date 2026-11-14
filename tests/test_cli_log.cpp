// File/layer: tests/test_cli_log.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliLogTest, CliPickTest (test_cli)
//
// cli/log_lines.h: the log command's arguments, the picks resolved
// against a set, its lines and the log line's ticker; cli/default_pick.h
// (CliPickTest), the default pick --pick ranked and the app share.

#include "default_pick.h"
#include "log_lines.h"

#include "ecuio/profile.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Args {
    std::vector<std::string> words;
    std::vector<char*> argv;
    explicit Args(std::vector<std::string> w) : words(std::move(w))
    {
        for (std::string& s : words) {
            argv.push_back(&s[0]);
        }
    }
    int argc() const { return static_cast<int>(argv.size()); }
};

std::string why_of(std::vector<std::string> w)
{
    Args a(std::move(w));
    cli::LogArgs la;
    std::string why;
    EXPECT_FALSE(cli::parse_log(a.argc(), a.argv.data(), la, &why));
    return why;
}

/* A set of three number signals and one text, by hand */
struct Set {
    ecuio_signal_def_t defs[4];
    ecuio_signal_set_t set;
    Set()
    {
        std::memset(defs, 0, sizeof defs);
        std::memset(&set, 0, sizeof set);
        const char* names[] = {"RPM", "Vehicle Speed", "Coolant Temp", "VIN"};
        for (uint16_t i = 0; i < 4; ++i) {
            defs[i].id = static_cast<uint16_t>(10 + i);
            std::strncpy(defs[i].name, names[i], sizeof defs[i].name - 1);
            defs[i].kind = static_cast<uint8_t>(i == 3 ? ECUIO_VALUE_TEXT
                                                       : ECUIO_VALUE_UINT);
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.defs = defs;
        set.count = 4;
    }
};

template <typename F> std::string text(F fill)
{
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    fill(l);
    EXPECT_TRUE(l.finish());
    std::string s(l.data(), l.size());
    if (!s.empty() && s.back() == '\n') {
        s.pop_back(); /* finish ends the line */
    }
    return s;
}

} // namespace

/* --min-sweep: taken with a --signal, its bad forms and a time with
 * nothing to pace are usage */
TEST(CliLogTest, AMinimumTimeBetweenSweepsIsTakenWithPicks)
{
    {
        Args a({"havne-cli", "log", "mock://x", "out", "--signal", "RPM",
                "--min-sweep", "250"});
        cli::LogArgs la;
        std::string why;
        ASSERT_TRUE(cli::parse_log(a.argc(), a.argv.data(), la, &why)) << why;
        EXPECT_EQ(la.min_sweep_ms, 250U);
    }
    {
        Args a({"havne-cli", "log", "mock://x", "out", "--signal", "RPM",
                "--min-sweep", "60000"});
        cli::LogArgs la;
        std::string why;
        ASSERT_TRUE(cli::parse_log(a.argc(), a.argv.data(), la, &why)) << why;
        EXPECT_EQ(la.min_sweep_ms, 60000U);
    }
    const std::string range =
        "--min-sweep takes a whole number of ms from 1 to 60000";
    for (const char* bad : {"0", "60001", "-1", "1.5", "x"}) {
        EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "out", "--signal",
                          "RPM", "--min-sweep", bad}),
                  range)
            << bad;
    }
    EXPECT_EQ(
        why_of({"havne-cli", "log", "mock://x", "out", "--min-sweep", "250"}),
        "--min-sweep goes with --signal or --pick ranked: it paces the "
        "picked signals");
    {
        Args a({"havne-cli", "log", "mock://x", "out", "--pick", "ranked",
                "--min-sweep", "250"});
        cli::LogArgs la;
        std::string why;
        ASSERT_TRUE(cli::parse_log(a.argc(), a.argv.data(), la, &why)) << why;
        EXPECT_EQ(la.min_sweep_ms, 250U);
    }
}

/* --pick takes "ranked" alone, with or without --signal */
TEST(CliLogTest, PickTakesRankedAlone)
{
    Args a({"havne-cli", "log", "mock://x", "out", "--pick", "ranked",
            "--signal", "RPM"});
    cli::LogArgs la;
    std::string why;
    ASSERT_TRUE(cli::parse_log(a.argc(), a.argv.data(), la, &why)) << why;
    EXPECT_TRUE(la.pick_ranked);
    ASSERT_EQ(la.names.size(), 1U);
    cli::LogArgs plain;
    Args b({"havne-cli", "log", "mock://x", "out"});
    ASSERT_TRUE(cli::parse_log(b.argc(), b.argv.data(), plain, &why)) << why;
    EXPECT_FALSE(plain.pick_ranked);
    const std::string words =
        "--pick takes 'ranked': the profile's ranked signals";
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "out", "--pick", "all"}),
              words);
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "out", "--pick"}), words);
    EXPECT_TRUE(cli::parse_pick("ranked"));
    EXPECT_FALSE(cli::parse_pick("Ranked"));
    EXPECT_FALSE(cli::parse_pick(nullptr));
}

/* The focus line says a default pick after the signals, before any
 * minimum */
TEST(CliLogTest, TheFocusLineNamesAPick)
{
    Set s;
    const std::vector<uint16_t> ids = {10, 11};
    EXPECT_EQ(
        text([&](cli::Line& l) { cli::focus_line(l, &s.set, ids, 250, true); }),
        "{\"type\":\"focus\",\"signals\":[{\"id\":10,\"name\":\"RPM\"},"
        "{\"id\":11,\"name\":\"Vehicle Speed\"}],\"pick\":\"ranked\","
        "\"min_sweep_ms\":250}");
    EXPECT_EQ(
        text([&](cli::Line& l) { cli::focus_line(l, &s.set, {}, 0, true); }),
        "{\"type\":\"focus\",\"signals\":[],\"pick\":\"ranked\"}");
}

/* The default pick (cli/default_pick.h): a set's ranked number defs and
 * the ids added, in the set's order, each once; a text's rank doesn't
 * count, an id the set lacks is passed by, and a set that ranks none is
 * asked whole (an empty pick) */
TEST(CliPickTest, ThePickIsTheRankedDefsAndTheAddedInTheSetsOrder)
{
    Set s;
    EXPECT_FALSE(cli::set_ranks(&s.set));
    EXPECT_FALSE(cli::set_ranks(nullptr));
    EXPECT_TRUE(cli::default_pick(&s.set, {11, 13}).empty())
        << "a set that ranks none is asked whole";

    s.defs[3].plot_rank = 1; /* the text: no rank of the pick's */
    EXPECT_FALSE(cli::set_ranks(&s.set));
    EXPECT_FALSE(cli::ranked_def(s.defs[3]));

    s.defs[2].plot_rank = 2; /* Coolant Temp */
    EXPECT_TRUE(cli::set_ranks(&s.set));
    EXPECT_EQ(cli::default_pick(&s.set, {}), std::vector<uint16_t>{12});
    EXPECT_EQ(cli::default_pick(&s.set, {13, 99, 10, 12, 10}),
              (std::vector<uint16_t>{10, 12, 13}));
    EXPECT_TRUE(cli::default_pick(nullptr, {10}).empty());
}

/* obd2's pick with nothing added is obd2's ranked signals */
TEST(CliPickTest, Obd2sPickIsItsRankedSignals)
{
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const std::vector<uint16_t> pick = cli::default_pick(obd2, {});
    EXPECT_EQ(pick.size(), 14U);
    for (uint16_t id : pick) {
        bool ranked = false;
        for (uint32_t i = 0; i < obd2->count; ++i) {
            ranked = ranked ||
                     (obd2->defs[i].id == id && obd2->defs[i].plot_rank != 0);
        }
        EXPECT_TRUE(ranked) << id;
    }
}

/* The focus line says the minimum only when one is set, after the picks,
 * so a log with none keeps its line */
TEST(CliLogTest, TheFocusLineNamesAMinimumOnlyWhenSet)
{
    Set s;
    const std::vector<uint16_t> ids = {11, 10};
    EXPECT_EQ(text([&](cli::Line& l) { cli::focus_line(l, &s.set, ids); }),
              "{\"type\":\"focus\",\"signals\":[{\"id\":11,\"name\":"
              "\"Vehicle Speed\"},{\"id\":10,\"name\":\"RPM\"}]}");
    EXPECT_EQ(text([&](cli::Line& l) { cli::focus_line(l, &s.set, ids, 250); }),
              "{\"type\":\"focus\",\"signals\":[{\"id\":11,\"name\":"
              "\"Vehicle Speed\"},{\"id\":10,\"name\":\"RPM\"}],"
              "\"min_sweep_ms\":250}");
}

TEST(CliLogTest, TheUriTheFolderAndEachOptionAreTaken)
{
    Args a({"havne-cli", "log", "mock://x", "out", "--signal", "RPM", "--label",
            "lap", "--seconds", "30", "--signal", "Vehicle Speed", "--plugin",
            "pkg"});
    cli::LogArgs la;
    std::string why;
    ASSERT_TRUE(cli::parse_log(a.argc(), a.argv.data(), la, &why)) << why;
    EXPECT_STREQ(la.uri, "mock://x");
    EXPECT_STREQ(la.folder, "out");
    EXPECT_STREQ(la.label, "lap");
    EXPECT_EQ(la.seconds, 30U);
    EXPECT_STREQ(la.plugin, "pkg");
    EXPECT_EQ(la.signals, nullptr);
    ASSERT_EQ(la.names.size(), 2U);
    EXPECT_STREQ(la.names[0], "RPM");
    EXPECT_STREQ(la.names[1], "Vehicle Speed");
    /* The options may come before the words */
    Args b({"havne-cli", "log", "--signals", "s.json", "mock://y", "--seconds",
            "1", "root"});
    cli::LogArgs lb;
    ASSERT_TRUE(cli::parse_log(b.argc(), b.argv.data(), lb, &why)) << why;
    EXPECT_STREQ(lb.uri, "mock://y");
    EXPECT_STREQ(lb.folder, "root");
    EXPECT_STREQ(lb.signals, "s.json");
    EXPECT_EQ(lb.seconds, 1U);
    EXPECT_TRUE(lb.names.empty());
}

TEST(CliLogTest, TheSecondsCapIsTakenAndOneMoreIsNot)
{
    Args b({"havne-cli", "log", "mock://y", "root", "--seconds", "31622400"});
    cli::LogArgs a;
    std::string why;
    ASSERT_TRUE(cli::parse_log(b.argc(), b.argv.data(), a, &why)) << why;
    EXPECT_EQ(a.seconds, 31622400U);
    EXPECT_EQ(a.seconds, cli::kMaxRunSeconds);
}

TEST(CliLogTest, BadArgumentsSayWhy)
{
    EXPECT_EQ(why_of({"havne-cli", "log"}),
              "log needs a URI and a folder for the recording");
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x"}),
              "log needs a URI and a folder for the recording");
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", ""}),
              "log takes a folder");
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "b"}),
              "log takes a URI and a folder");
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--signal", ""}),
              "--signal takes a signal's name");
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--signal"}),
              "unexpected '--signal'");
    const std::string seconds_why =
        "--seconds takes a whole number from 1 to 31622400";
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--seconds", "0"}),
              seconds_why);
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--seconds", "2s"}),
              seconds_why);
    // The cap is a year and a day of seconds; one more, or a number that
    // would wrap seconds * 1e6, is refused
    EXPECT_EQ(
        why_of({"havne-cli", "log", "mock://x", "a", "--seconds", "31622401"}),
        seconds_why);
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--seconds",
                      "18446744073710"}),
              seconds_why);
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--layout", "wide"}),
              "unexpected '--layout'");
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--signals", "s",
                      "--plugin", "p"}),
              "--signals and --plugin each give the profile: one at a time");
    const std::string conditions_why = "--conditions takes one condition file";
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--conditions"}),
              conditions_why);
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--conditions", ""}),
              conditions_why);
    EXPECT_EQ(why_of({"havne-cli", "log", "mock://x", "a", "--conditions",
                      "c.json", "--conditions", "d.json"}),
              conditions_why);
}

TEST(CliLogTest, AConditionFileIsTakenBeforeOrAfterTheWords)
{
    Args a({"havne-cli", "log", "--conditions", "c.json", "mock://x", "out"});
    cli::LogArgs la;
    std::string why;
    ASSERT_TRUE(cli::parse_log(a.argc(), a.argv.data(), la, &why)) << why;
    EXPECT_STREQ(la.conditions, "c.json");
    EXPECT_STREQ(la.uri, "mock://x");
    EXPECT_STREQ(la.folder, "out");
    Args b({"havne-cli", "log", "mock://x", "out", "--conditions", "c.json"});
    cli::LogArgs lb;
    ASSERT_TRUE(cli::parse_log(b.argc(), b.argv.data(), lb, &why)) << why;
    EXPECT_STREQ(lb.conditions, "c.json");
    Args c({"havne-cli", "log", "mock://x", "out"});
    cli::LogArgs lc;
    ASSERT_TRUE(cli::parse_log(c.argc(), c.argv.data(), lc, &why)) << why;
    EXPECT_EQ(lc.conditions, nullptr);
}

TEST(CliLogTest, ThePicksResolveInOrderAndATypoOrARepeatIsRefused)
{
    Set s;
    std::vector<uint16_t> ids;
    std::string why;
    ASSERT_TRUE(cli::resolve_focus(&s.set, {"Coolant Temp", "RPM"}, ids, &why))
        << why;
    EXPECT_EQ(ids, (std::vector<uint16_t>{12, 10}));
    ASSERT_TRUE(cli::resolve_focus(&s.set, {}, ids, &why)) << why;
    EXPECT_TRUE(ids.empty());
    /* A text signal is a pick like any other: the focus is core's call */
    ASSERT_TRUE(cli::resolve_focus(&s.set, {"VIN"}, ids, &why)) << why;
    EXPECT_EQ(ids, (std::vector<uint16_t>{13}));

    EXPECT_FALSE(cli::resolve_focus(&s.set, {"RPM", "Rpm"}, ids, &why));
    EXPECT_EQ(why, "no signal named 'Rpm' in the session's profile");
    EXPECT_FALSE(
        cli::resolve_focus(&s.set, {"RPM", "Vehicle Speed", "RPM"}, ids, &why));
    EXPECT_EQ(why, "--signal names 'RPM' twice");
    EXPECT_FALSE(cli::resolve_focus(nullptr, {"RPM"}, ids, &why));
    EXPECT_EQ(why, "no signal named 'RPM' in the session's profile");
}

TEST(CliLogTest, TheLinesSayThePicksTheReadoutAndTheEnd)
{
    Set s;
    EXPECT_EQ(text([&](cli::Line& l) { cli::focus_line(l, &s.set, {12, 10}); }),
              "{\"type\":\"focus\",\"signals\":[{\"id\":12,\"name\":\"Coolant "
              "Temp\"},{\"id\":10,\"name\":\"RPM\"}]}");
    EXPECT_EQ(text([&](cli::Line& l) { cli::focus_line(l, &s.set, {}); }),
              "{\"type\":\"focus\",\"signals\":[]}");
    EXPECT_LE(std::strlen("{\"type\":\"focus\",\"signals\":[]}"),
              cli::focus_line_bytes(0));

    cli::LogReadout r;
    r.seconds = 20;
    r.bytes = 123456;
    r.values = 4000;
    EXPECT_EQ(text([&](cli::Line& l) { cli::log_line(l, r); }),
              "{\"type\":\"log\",\"seconds\":20,\"bytes\":123456,\"values\":"
              "4000}");
    const std::string folder(1000, 'f');
    const std::string end = text([&](cli::Line& l) {
        cli::log_end_line(l, "seconds", folder.c_str(), r);
    });
    EXPECT_EQ(end, "{\"type\":\"end\",\"reason\":\"seconds\",\"folder\":\"" +
                       folder +
                       "\",\"seconds\":20,\"bytes\":123456,\"values\":4000}");
    EXPECT_LE(end.size(), cli::log_end_line_bytes(folder.c_str()));
    // The recording's own note names no folder
    EXPECT_EQ(text([&](cli::Line& l) {
                  cli::log_end_line(l, "seconds", nullptr, r);
              }),
              "{\"type\":\"end\",\"reason\":\"seconds\",\"seconds\":20,"
              "\"bytes\":123456,\"values\":4000}");
}

TEST(CliLogTest,
     TheLoopEndsOnAnInterruptAFailedRecordingAReplaysEndOrTheSeconds)
{
    EXPECT_EQ(cli::log_end_reason(false, false, false, false), nullptr);
    EXPECT_STREQ(cli::log_end_reason(true, false, false, false), "interrupted");
    EXPECT_STREQ(cli::log_end_reason(false, true, false, false),
                 "recording-failed");
    EXPECT_STREQ(cli::log_end_reason(false, false, true, false), "replay-end");
    EXPECT_STREQ(cli::log_end_reason(false, false, false, true), "seconds");
    /* The order when several hold at once */
    EXPECT_STREQ(cli::log_end_reason(true, true, true, true), "interrupted");
    EXPECT_STREQ(cli::log_end_reason(false, true, true, true),
                 "recording-failed");
    EXPECT_STREQ(cli::log_end_reason(false, false, true, true), "replay-end");
}

TEST(CliLogTest, ALogLineIsDueEachPeriodAndOnceAfterAStall)
{
    cli::LogTicker t(1000, 10);
    EXPECT_FALSE(t.due(1000));
    EXPECT_FALSE(t.due(1009));
    EXPECT_TRUE(t.due(1010));
    EXPECT_FALSE(t.due(1015));
    EXPECT_FALSE(t.due(1019));
    EXPECT_TRUE(t.due(1021));
    /* Three periods missed: one line, the next due a period on */
    EXPECT_TRUE(t.due(1065));
    EXPECT_FALSE(t.due(1069));
    EXPECT_TRUE(t.due(1070));
    /* A clock before the start owes nothing */
    cli::LogTicker u(1000, 10);
    EXPECT_FALSE(u.due(500));
    EXPECT_TRUE(u.due(1010));
}

TEST(CliLogTest, TheRecordingsBytesCountEachOfItsFiles)
{
    const std::map<std::string, uint64_t> sizes = {
        {"f/frames.csv", 1},       {"f/signals.csv", 2},
        {"f/events.log", 4},       {"f/markers.csv", 8},
        {"f/latency.csv", 16},     {"f/signals.json", 32},
        {"f/conditions.json", 64}, {"f/notes.txt", 128}};
    EXPECT_EQ(cli::recording_bytes("f",
                                   [&](const std::string& path) {
                                       const auto it = sizes.find(path);
                                       return it == sizes.end() ? uint64_t{0}
                                                                : it->second;
                                   }),
              127u)
        << "each of a recording's files, conditions.json among them, and "
           "nothing else";
}
