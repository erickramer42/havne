// File/layer: tests/test_cli_health.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliHealthTest (test_cli)
//
// cli/health_lines.h: health's arguments (a session's and a recording's),
// the live wait's rule (a report counts only once asked), the values'
// quiet, and the lines, which carry counts and never an identity text.

#include "health_lines.h"

#include <gtest/gtest.h>

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

std::string health_why(std::vector<std::string> w)
{
    Args a(std::move(w));
    cli::HealthArgs h;
    std::string why;
    EXPECT_FALSE(cli::parse_health(a.argc(), a.argv.data(), h, &why));
    return why;
}

std::string text_of(cli::Line& l)
{
    EXPECT_TRUE(l.finish());
    return std::string(l.data(), l.size());
}

TEST(CliHealthTest, ASessionTakesItsWaitAndSet)
{
    Args a({"havne-cli", "health", "elm327://COM3?profile=obd2", "car.txt",
            "--format", "text", "--units", "imperial", "--seconds", "45",
            "--signals", "mine.json"});
    cli::HealthArgs h;
    std::string why;
    ASSERT_TRUE(cli::parse_health(a.argc(), a.argv.data(), h, &why)) << why;
    EXPECT_TRUE(h.live);
    EXPECT_EQ(h.options.format, ECUIO_HEALTH_TEXT);
    EXPECT_EQ(h.options.units, ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(h.seconds, 45u);
    EXPECT_STREQ(h.signals, "mine.json");
    EXPECT_EQ(h.options.folder_utf8, nullptr);
    EXPECT_STREQ(h.options.out_path_utf8, "car.txt");

    /* The defaults: metric, clear's 30 s */
    Args b({"havne-cli", "health", "mock://x", "car.json", "--format", "json"});
    cli::HealthArgs d;
    ASSERT_TRUE(cli::parse_health(b.argc(), b.argv.data(), d, &why)) << why;
    EXPECT_EQ(d.options.format, ECUIO_HEALTH_JSON);
    EXPECT_EQ(d.options.units, ECUIO_UNITS_METRIC);
    EXPECT_EQ(d.seconds, cli::kHealthWaitSeconds);
}

TEST(CliHealthTest, ARecordingTakesItsRange)
{
    Args a({"havne-cli", "health", "C:/rec/20261106-142233", "car.json",
            "--format", "json", "--from", "1.5", "--to", "90"});
    cli::HealthArgs h;
    std::string why;
    ASSERT_TRUE(cli::parse_health(a.argc(), a.argv.data(), h, &why)) << why;
    EXPECT_FALSE(h.live);
    EXPECT_STREQ(h.options.folder_utf8, "C:/rec/20261106-142233");
    EXPECT_EQ(h.options.from_us, 1500000u);
    EXPECT_EQ(h.options.to_us, 90000000u);
}

TEST(CliHealthTest, ItsBadFormsAreUsage)
{
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x"}),
              "health needs a URI or a recording's folder, and the file to "
              "write");
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out"}),
              "health needs --format");
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out", "--format",
                          "pdf"}),
              "--format takes json or text");
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out", "--format",
                          "json", "--units", "si"}),
              "--units takes metric or imperial");
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out", "--format",
                          "json", "--from", "1"}),
              "--from and --to take a recording's seconds, not a session's");
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out", "--format",
                          "json", "--to", "5"}),
              "--from and --to take a recording's seconds, not a session's");
    for (const char* flag : {"--seconds", "--signals", "--plugin"}) {
        EXPECT_EQ(health_why({"havne-cli", "health", "rec", "out", "--format",
                              "json", flag, "5"}),
                  "--seconds, --signals and --plugin are a live session's: a "
                  "recording reads with its own set")
            << flag;
    }
    EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out", "--format",
                          "json", "--signals", "a.json", "--plugin", "p"}),
              "--signals and --plugin each give the profile: one at a time");
    for (const char* n : {"0", "x", "1.5", "-3", "86401", ""}) {
        EXPECT_EQ(health_why({"havne-cli", "health", "mock://x", "out",
                              "--format", "json", "--seconds", n}),
                  "--seconds takes a whole number from 1 to 86400")
            << n;
    }
    EXPECT_EQ(health_why({"havne-cli", "health", "rec", "out", "--format",
                          "json", "--from", "5", "--to", "5"}),
              "--to comes after --from");
    EXPECT_EQ(health_why({"havne-cli", "health", "rec", "out", "--format",
                          "json", "--to", "0"}),
              "--to takes seconds after 0");
    EXPECT_EQ(health_why({"havne-cli", "health", "rec", "out", "--format",
                          "json", "--from", "-1"}),
              "--from takes seconds, 0 or more");
    EXPECT_EQ(health_why({"havne-cli", "health", "rec", "out", "more",
                          "--format", "json"}),
              "health takes one session or recording and one file");
    EXPECT_EQ(health_why({"havne-cli", "health", "rec", "out", "--format",
                          "json", "--raw"}),
              "unexpected '--raw'");
}

/* Only an asked report is waited for: diagnostics outside obd2 and the
 * mock are never asked, and an ECU identified with nothing to ask has no
 * items */
TEST(CliHealthTest, OnlyAnAskedReportIsWaitedFor)
{
    ecuio_channel_diagnostics_t d{};
    ecuio_channel_identity_t id{};
    EXPECT_TRUE(cli::reports_settled(d, id));
    d.flags = ECUIO_DIAG_ASKED;
    EXPECT_FALSE(cli::reports_settled(d, id));
    d.flags |= ECUIO_DIAG_COMPLETE;
    EXPECT_TRUE(cli::reports_settled(d, id));
    id.items = 3;
    EXPECT_FALSE(cli::reports_settled(d, id));
    id.flags = ECUIO_IDENTITY_COMPLETE;
    EXPECT_TRUE(cli::reports_settled(d, id));
    d.flags = ECUIO_DIAG_ASKED;
    EXPECT_FALSE(cli::reports_settled(d, id));
}

TEST(CliHealthTest, TheShownDefsAreTheRankedAndTheSinceClearedPids)
{
    ecuio_signal_def_t d{};
    d.kind = ECUIO_VALUE_UINT;
    EXPECT_FALSE(cli::shown_def(d));
    d.plot_rank = 4;
    EXPECT_TRUE(cli::shown_def(d));
    d.kind = ECUIO_VALUE_TEXT;
    EXPECT_FALSE(cli::shown_def(d));
    d.kind = ECUIO_VALUE_UINT;
    d.plot_rank = 0;
    d.mux_length = 2;
    for (uint32_t pid : {0x21u, 0x30u, 0x31u, 0x4Du, 0x4Eu}) {
        d.mux_key = 0x4100u | pid;
        EXPECT_TRUE(cli::shown_def(d)) << pid;
    }
    d.mux_key = 0x410Cu;
    EXPECT_FALSE(cli::shown_def(d));
    d.mux_key = 0x6231u; /* a DID's 62, not 41 */
    EXPECT_FALSE(cli::shown_def(d));
}

/* An ECU's channel whose bitmaps list pids (and cover through 0x60) */
ecuio_channel_support_t support_of(uint32_t rx, std::vector<uint32_t> pids)
{
    ecuio_channel_support_t s{};
    s.rx_id = rx;
    s.generation = 3;
    s.pids_through = 0x60;
    for (uint32_t p : pids) {
        s.pid_bitmap[(p - 1) / 8] |=
            static_cast<uint8_t>(0x80u >> ((p - 1) % 8));
    }
    return s;
}

ecuio_signal_def_t pid_def(uint32_t rx, uint32_t pid)
{
    ecuio_signal_def_t d{};
    d.kind = ECUIO_VALUE_UINT;
    d.message_id = rx;
    d.mux_length = 2;
    d.mux_key = 0x4100u | pid;
    return d;
}

/* A since-cleared PID the bitmaps list is waited for until it has a
 * value; one they clear, or no bitmap covers, or a dropped request, or a
 * def that isn't Service 01's, is left to the quiet rule */
TEST(CliHealthTest, ASupportedPidKeepsTheWaitUntilItsFirstValue)
{
    const ecuio_channel_support_t s[2] = {support_of(0x7E8, {0x0C, 0x31, 0x4D}),
                                          support_of(0x7E9, {0x21})};
    EXPECT_TRUE(cli::awaited(pid_def(0x7E8, 0x31), false, s, 2));
    EXPECT_TRUE(cli::awaited(pid_def(0x7E8, 0x4D), false, s, 2));
    EXPECT_TRUE(cli::awaited(pid_def(0x7E9, 0x21), false, s, 2));
    EXPECT_FALSE(cli::awaited(pid_def(0x7E8, 0x31), true, s, 2)); /* has one */
    EXPECT_FALSE(
        cli::awaited(pid_def(0x7E8, 0x21), false, s, 2)); /* bit clear */
    EXPECT_FALSE(
        cli::awaited(pid_def(0x7EA, 0x31), false, s, 2)); /* no channel */
    EXPECT_FALSE(
        cli::awaited(pid_def(0x7E8, 0x0C), false, s, 2)); /* not shown */

    ecuio_signal_def_t ranked = pid_def(0x7E8, 0x0C);
    ranked.plot_rank = 1; /* shown: RPM, listed */
    EXPECT_TRUE(cli::awaited(ranked, false, s, 2));

    ecuio_channel_support_t short_chain = s[0];
    short_chain.pids_through = 0x40; /* 4D past what was answered */
    EXPECT_FALSE(cli::awaited(pid_def(0x7E8, 0x4D), false, &short_chain, 1));
    EXPECT_TRUE(cli::awaited(pid_def(0x7E8, 0x31), false, &short_chain, 1));

    ecuio_channel_support_t unlearned = s[0];
    unlearned.generation = 0;
    EXPECT_FALSE(cli::awaited(pid_def(0x7E8, 0x31), false, &unlearned, 1));

    ecuio_channel_support_t refused = s[0];
    refused.dropped = 1;
    refused.dropped_list[0].request[0] = 0x01;
    refused.dropped_list[0].request[1] = 0x31;
    refused.dropped_list[0].length = 2;
    refused.dropped_list[0].reason = ECUIO_DROP_NOT_SUPPORTED;
    EXPECT_FALSE(cli::awaited(pid_def(0x7E8, 0x31), false, &refused, 1));
    EXPECT_TRUE(cli::awaited(pid_def(0x7E8, 0x4D), false, &refused, 1));

    ecuio_signal_def_t did = pid_def(0x7E8, 0x31);
    did.mux_length = 3;
    did.mux_key = 0x62F190u; /* a DID's reply, ranked */
    did.plot_rank = 2;
    EXPECT_FALSE(cli::awaited(did, false, s, 2));
}

/* The wait ends once settled, quiet and owed nothing, or at the deadline
 * whatever is still owed */
TEST(CliHealthTest, TheDeadlineEndsTheWaitWhateverIsOwed)
{
    EXPECT_TRUE(cli::health_wait_over(true, true, 0, 10, 100));
    EXPECT_FALSE(cli::health_wait_over(true, true, 2, 10, 100));
    EXPECT_FALSE(cli::health_wait_over(true, false, 0, 10, 100));
    EXPECT_FALSE(cli::health_wait_over(false, true, 0, 10, 100));
    EXPECT_TRUE(cli::health_wait_over(true, true, 2, 100, 100));
    EXPECT_TRUE(cli::health_wait_over(false, false, 5, 101, 100));
}

/* The values' wait ends once the count stops growing for the quiet time,
 * counted again from each rise */
TEST(CliHealthTest, TheValuesWaitEndsOnQuiet)
{
    cli::ValuesQuiet q;
    EXPECT_FALSE(q.quiet(0, 1000000, 2000000));
    EXPECT_FALSE(q.quiet(0, 2900000, 2000000));
    EXPECT_FALSE(q.quiet(3, 3000000, 2000000));
    EXPECT_FALSE(q.quiet(3, 4900000, 2000000));
    EXPECT_TRUE(q.quiet(3, 5000000, 2000000));
    cli::ValuesQuiet none;
    EXPECT_FALSE(none.quiet(0, 0, 2000000));
    EXPECT_TRUE(none.quiet(0, 2000000, 2000000));
}

TEST(CliHealthTest, TheLinesCarryCountsAndNoIdentity)
{
    Args a({"havne-cli", "health", "mock://x", "car.json", "--format", "json"});
    cli::HealthArgs h;
    std::string why;
    ASSERT_TRUE(cli::parse_health(a.argc(), a.argv.data(), h, &why)) << why;
    ecuio_health_report_t r{};
    r.ecus = 2;
    r.codes = 3;
    r.values = 14;
    r.since_cleared = 5;
    r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 2;
    r.flags = ECUIO_HEALTH_VIN_DECODED;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::health_line(l, h, r, false);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"health\",\"format\":\"json\",\"file\":\"car.json\","
              "\"source\":\"session\",\"ecus\":2,\"codes\":3,\"values\":14,"
              "\"since_cleared\":5,\"left_out\":{\"identification\":2},"
              "\"vin_decoded\":true,\"settled\":false}\n");

    Args b({"havne-cli", "health", "rec", "car.txt", "--format", "text"});
    cli::HealthArgs rec;
    ASSERT_TRUE(cli::parse_health(b.argc(), b.argv.data(), rec, &why)) << why;
    ecuio_health_report_t none{};
    cli::Line m(buf, sizeof buf);
    cli::health_line(m, rec, none, true);
    EXPECT_EQ(text_of(m),
              "{\"type\":\"health\",\"format\":\"text\",\"file\":\"car.txt\","
              "\"source\":\"recording\",\"ecus\":0,\"codes\":0,\"values\":0,"
              "\"since_cleared\":0,\"left_out\":{\"identification\":0},"
              "\"vin_decoded\":false}\n");

    ecuio_health_report_t failed{};
    failed.failed_class = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1;
    cli::Line f(buf, sizeof buf);
    cli::health_failed_line(f, failed);
    EXPECT_EQ(text_of(f),
              "{\"type\":\"health_failed\",\"class\":\"identification\"}\n");
}

// A path at its longest escape (every byte a control character's six)
// still fits the line it sizes, so the line is never cut or dropped
TEST(CliHealthTest, ALongPathFitsItsLine)
{
    const std::string path(4000, '\x01');
    std::string escaped;
    for (size_t i = 0; i < path.size(); ++i) {
        escaped += "\\u0001";
    }
    cli::HealthArgs h;
    h.out = path.c_str();
    h.format_word = "json";
    h.live = true;
    ecuio_health_report_t r{};
    r.ecus = UINT32_MAX;
    std::vector<char> buf(cli::health_line_bytes(h));
    cli::Line l(buf.data(), buf.size());
    cli::health_line(l, h, r, true);
    const std::string t = text_of(l);
    EXPECT_NE(t.find("\"file\":\"" + escaped + "\""), std::string::npos);
    EXPECT_NE(t.find("\"settled\":true}"), std::string::npos);
}

} // namespace
