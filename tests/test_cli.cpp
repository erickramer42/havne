// File/layer: tests/test_cli.cpp | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#include "can_line.h"
#include "clear_run.h"
#include "diagnostics_line.h"
#include "elm327_emulator.h"
#include "elm327_lines.h"
#include "info_lines.h"
#include "jsonl.h"
#include "line_ring.h"
#include "mark_lines.h"
#include "plugin_lines.h"
#include "recording_signals.h"
#include "refused_signals.h"
#include "scan_line.h"
#include "signal_file_lines.h"
#include "sim_line.h"
#include "sleep_watch.h"
#include "timing_line.h"
#include "view_frames.h"
#include "view_lines.h"
#include "vin_line.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string str(const char* s, size_t n = SIZE_MAX, size_t limit = SIZE_MAX)
{
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.string(s, n, limit);
    return std::string(l.data(), l.size());
}

std::string num(double v)
{
    char buf[64];
    cli::Line l(buf, sizeof buf);
    l.number(v);
    return std::string(l.data(), l.size());
}

std::vector<std::string> lines(const char* p, size_t n)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += p[i];
        }
    }
    EXPECT_TRUE(cur.empty()) << "a line without its \\n";
    return out;
}

} // namespace

TEST(CliLineTest, StringsEscapeQuotesBackslashesAndControlCharacters)
{
    EXPECT_EQ(str("plain"), "\"plain\"");
    EXPECT_EQ(str("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(str("\n\r\t\b\f"), "\"\\n\\r\\t\\b\\f\"");
    EXPECT_EQ(str("\x01\x1F"), "\"\\u0001\\u001F\"");
    EXPECT_EQ(str("\x7F"), "\"\x7F\""); // DEL is legal in a JSON string
}

TEST(CliLineTest, StringsStopAtTheirLengthOrANul)
{
    EXPECT_EQ(str("abcdef", 3), "\"abc\"");
    // A fixed char[] field full to its end, with no NUL inside
    const char field[4] = {'R', 'P', 'M', '!'};
    EXPECT_EQ(str(field, sizeof field), "\"RPM!\"");
    EXPECT_EQ(str("ab\0cd", 5), "\"ab\"");
}

TEST(CliLineTest, WellFormedUtf8PassesAndAnythingElseIsReplaced)
{
    EXPECT_EQ(str("\xC2\xB0"
                  "C"),
              "\"\xC2\xB0"
              "C\"");                                           // degree sign
    EXPECT_EQ(str("\xE2\x82\xAC"), "\"\xE2\x82\xAC\"");         // euro, 3 bytes
    EXPECT_EQ(str("\xF0\x9F\x9A\x97"), "\"\xF0\x9F\x9A\x97\""); // 4 bytes
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_EQ(str("\x80"), "\"" + fffd + "\"");            // stray continuation
    EXPECT_EQ(str("\xC0\xAF"), "\"" + fffd + fffd + "\""); // overlong
    EXPECT_EQ(str("\xED\xA0\x80"),
              "\"" + fffd + fffd + fffd + "\""); // a surrogate
    EXPECT_EQ(str("\xF4\x90\x80\x80"),
              "\"" + fffd + fffd + fffd + fffd + "\"");       // past U+10FFFF
    EXPECT_EQ(str("\xE2\x82", 2), "\"" + fffd + fffd + "\""); // cut short
    EXPECT_EQ(str("\xFF"
                  "A"),
              "\"" + fffd + "A\"");
}

TEST(CliLineTest, ALimitCutsAStringAtAWholeCharacter)
{
    char buf[64];
    cli::Line l(buf, sizeof buf);
    // 8 output bytes: the quotes and 6 of content; the euro sign would
    // take the 6th to 8th, so it's left out whole
    const char* s = "abcde\xE2\x82\xAC"
                    "f";
    EXPECT_EQ(l.string(s, SIZE_MAX, 8), 5u);
    EXPECT_EQ(std::string(l.data(), l.size()), "\"abcde\"");
    EXPECT_FALSE(l.full());
    // An escape is never split either
    EXPECT_EQ(str("ab\"", SIZE_MAX, 4), "\"ab\"");
}

TEST(CliLineTest, NumbersReadBackExactlyAndNonFiniteOnesAreNull)
{
    EXPECT_EQ(num(2512.5), "2512.5");
    EXPECT_EQ(num(0.1), "0.1");
    EXPECT_EQ(num(-40), "-40");
    EXPECT_EQ(num(0), "0");
    EXPECT_EQ(num(std::nan("")), "null");
    EXPECT_EQ(num(std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(num(-std::numeric_limits<double>::infinity()), "null");
    for (double v : {1.0 / 3, 2.0 / 3 * 1e-7, 123456789.123456789, 0.1 + 0.2,
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::denorm_min()}) {
        const std::string s = num(v);
        EXPECT_EQ(std::strtod(s.c_str(), nullptr), v) << s;
    }
}

TEST(CliLineTest, HexFormsMatchFramesCsv)
{
    char buf[64];
    cli::Line l(buf, sizeof buf);
    l.hex_id(0x7E8);
    l.ch(' ');
    l.hex_id(0x18DAF110);
    l.ch(' ');
    const uint8_t data[] = {0x41, 0x0C, 0x1A, 0xF8};
    l.hex_bytes(data, sizeof data);
    l.ch(' ');
    l.hex_bytes(data, 0);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "\"0x7E8\" \"0x18DAF110\" \"41 0C 1A F8\" \"\"");
}

/* An ECU that refuses or ignores the list (ADR-0037): each item says
 * which, a refusal with its reply as frames.csv writes bytes */
TEST(CliDiagnosticsTest, RefusalsCarryTheirReplyAndSilenceSaysSo)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    d.rx_id = 0x7E9;
    d.generation = 6;
    d.readiness = ECUIO_DIAG_REFUSED;
    d.readiness_nrc = 0x12;
    d.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_UNANSWERED;
    d.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_REFUSED;
    d.lists[ECUIO_DIAG_PERMANENT].nrc = 0x11;
    d.freeze = ECUIO_DIAG_UNANSWERED;
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    cli::diagnostics_line(l, d);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"diagnostics\",\"rx\":\"0x7E9\",\"generation\":6,"
              "\"readiness\":{\"outcome\":\"refused\",\"reply\":\"7F 01 12\"},"
              "\"cycle\":{\"outcome\":\"unasked\"},"
              "\"stored\":{\"outcome\":\"unanswered\"},"
              "\"pending\":{\"outcome\":\"unasked\"},"
              "\"permanent\":{\"outcome\":\"refused\",\"reply\":\"7F 0A 11\"},"
              "\"freeze\":{\"outcome\":\"unanswered\"}}");
}

/* A compression engine's monitor by its own name, a list longer than
 * the codes held (reported says how long), and an answered freeze frame
 * with none stored (code null) */
TEST(CliDiagnosticsTest, AnswersNameTheirCodesAndMonitors)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
              ECUIO_DIAG_COMPRESSION;
    d.rx_id = 0x7E8;
    d.generation = 9;
    d.readiness = ECUIO_DIAG_ANSWERED;
    d.dtc_count = 0;
    d.monitor_count = 1;
    d.monitors[0].id = 8;
    d.cycle = ECUIO_DIAG_UNSUPPORTED;
    for (ecuio_dtc_list_t& list : d.lists) {
        list.outcome = ECUIO_DIAG_ANSWERED;
    }
    d.lists[ECUIO_DIAG_PENDING].reported = 40;
    d.lists[ECUIO_DIAG_PENDING].held = 2;
    d.lists[ECUIO_DIAG_PENDING].codes[0] = 0x2002;
    d.lists[ECUIO_DIAG_PENDING].codes[1] = 0xC100;
    d.freeze = ECUIO_DIAG_ANSWERED;
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    cli::diagnostics_line(l, d);
    EXPECT_EQ(
        std::string(l.data(), l.size()),
        "{\"type\":\"diagnostics\",\"rx\":\"0x7E8\",\"generation\":9,"
        "\"readiness\":{\"outcome\":\"answered\",\"mil\":false,"
        "\"dtc_count\":0,\"compression\":true,"
        "\"monitors\":[{\"name\":\"NMHC catalyst\",\"complete\":false}]},"
        "\"cycle\":{\"outcome\":\"unsupported\"},"
        "\"stored\":{\"outcome\":\"answered\",\"reported\":0,\"codes\":[],"
        "\"titles\":[]},"
        "\"pending\":{\"outcome\":\"answered\",\"reported\":40,"
        "\"codes\":[\"P2002\",\"U0100\"],"
        "\"titles\":[\"Particulate Filter Efficiency Below Threshold Bank 1\","
        "\"Lost Communication With ECM/PCM A\"]},"
        "\"permanent\":{\"outcome\":\"answered\",\"reported\":0,\"codes\":[],"
        "\"titles\":[]},"
        "\"freeze\":{\"outcome\":\"answered\",\"code\":null,\"values\":[]}}");
}

/* PID 41 answered: each monitor says whether this drive cycle enabled
 * it and whether it completed, beside PID 01's state since the clear */
TEST(CliDiagnosticsTest, MonitorsCarryThisDriveCycle)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
              ECUIO_DIAG_CYCLE;
    d.rx_id = 0x7E8;
    d.generation = 2;
    d.readiness = ECUIO_DIAG_ANSWERED;
    d.cycle = ECUIO_DIAG_ANSWERED;
    d.monitor_count = 2;
    d.monitors[0].id = 0;
    d.monitors[0].flags = ECUIO_MONITOR_COMPLETE | ECUIO_MONITOR_CYCLE_ENABLED;
    d.monitors[1].id = 8;
    d.monitors[1].flags = ECUIO_MONITOR_CYCLE_COMPLETE;
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    cli::diagnostics_line(l, d);
    const std::string line(l.data(), l.size());
    EXPECT_NE(line.find("\"monitors\":["
                        "{\"name\":\"Misfire\",\"complete\":true,"
                        "\"cycle_enabled\":true,\"cycle_complete\":false},"
                        "{\"name\":\"Catalyst\",\"complete\":false,"
                        "\"cycle_enabled\":false,\"cycle_complete\":true}]},"
                        "\"cycle\":{\"outcome\":\"answered\"},"),
              std::string::npos)
        << line;
}

TEST(CliLineTest, KeysTakeCommasAndAFullLineIsRefused)
{
    char buf[64];
    cli::Line l(buf, sizeof buf);
    l.ch('{');
    l.key("a");
    l.u64(1);
    l.key("b");
    l.ch('[');
    l.u64(18446744073709551615ull);
    l.ch(']');
    l.ch('}');
    ASSERT_TRUE(l.finish());
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"a\":1,\"b\":[18446744073709551615]}\n");

    char small[8];
    cli::Line s(small, sizeof small);
    s.raw("{\"type\"");
    s.raw(":1}");
    EXPECT_TRUE(s.full());
    EXPECT_FALSE(s.finish());
}

TEST(CliRingTest, LinesDrainInOrderAcrossTheWrap)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(32));
    std::vector<char> out(ring.capacity());
    std::vector<std::string> got;
    for (int i = 0; i < 20; ++i) {
        const std::string line = "line " + std::to_string(i) + "\n";
        ASSERT_TRUE(
            ring.push(cli::LineKind::Frame, line.data(), line.size(), false));
        if (i % 3 == 2) {
            const size_t n =
                ring.drain(out.data(), std::chrono::milliseconds(0));
            for (auto& l : lines(out.data(), n)) {
                got.push_back(l);
            }
        }
    }
    const size_t n = ring.drain(out.data(), std::chrono::milliseconds(0));
    for (auto& l : lines(out.data(), n)) {
        got.push_back(l);
    }
    ASSERT_EQ(got.size(), 20u);
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(got[i], "line " + std::to_string(i));
    }
}

TEST(CliRingTest, AFullRingDropsAndSaysSoBeforeTheNextLine)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(128));
    std::vector<char> out(ring.capacity());
    const std::string line(40, 'x');
    const std::string l = line + "\n";
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), false));
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), false));
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), false));
    EXPECT_FALSE(ring.push(cli::LineKind::Frame, l.data(), l.size(), false));
    EXPECT_FALSE(ring.push(cli::LineKind::Values, l.data(), l.size(), false));
    EXPECT_FALSE(ring.push(cli::LineKind::Values, l.data(), l.size(), false));
    EXPECT_EQ(ring.dropped_frames(), 1u);
    EXPECT_EQ(ring.dropped_values(), 2u);

    EXPECT_EQ(
        lines(out.data(), ring.drain(out.data(), std::chrono::milliseconds(0)))
            .size(),
        3u);
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), false));
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), false));
    const auto got =
        lines(out.data(), ring.drain(out.data(), std::chrono::milliseconds(0)));
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "{\"type\":\"dropped\",\"frames\":1,\"values\":2}");
    EXPECT_EQ(got[1], line);
    EXPECT_EQ(got[2], line);              // the note goes once
    EXPECT_EQ(ring.dropped_frames(), 1u); // totals stay for the end line
}

TEST(CliRingTest, AWaitingPushGoesInOnceTheRingDrains)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(64));
    std::vector<char> out(ring.capacity());
    const std::string l = std::string(40, 'x') + "\n";
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), true));
    std::atomic<bool> pushed{false};
    std::thread worker([&] {
        pushed.store(ring.push(cli::LineKind::Frame, l.data(), l.size(), true));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load()) << "a waiting push went in with no room";
    EXPECT_EQ(
        lines(out.data(), ring.drain(out.data(), std::chrono::milliseconds(0)))
            .size(),
        1u);
    worker.join();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(
        lines(out.data(), ring.drain(out.data(), std::chrono::milliseconds(0)))
            .size(),
        1u);
    EXPECT_EQ(ring.dropped_frames(), 0u);
}

TEST(CliRingTest, StopEndsAWaitAndDropsItsLine)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(64));
    const std::string l = std::string(40, 'x') + "\n";
    ASSERT_TRUE(ring.push(cli::LineKind::Frame, l.data(), l.size(), true));
    std::atomic<int> result{-1};
    std::thread worker([&] {
        result.store(
            ring.push(cli::LineKind::Values, l.data(), l.size(), true) ? 1 : 0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ring.stop();
    worker.join();
    EXPECT_EQ(result.load(), 0);
    EXPECT_EQ(ring.dropped_values(), 1u);
    // And later pushes no longer wait
    EXPECT_FALSE(ring.push(cli::LineKind::Frame, l.data(), l.size(), true));
}

TEST(CliRingTest, ALineLongerThanTheRingIsDroppedEvenWhenWaiting)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(16));
    const std::string l = std::string(40, 'x') + "\n";
    EXPECT_FALSE(ring.push(cli::LineKind::Frame, l.data(), l.size(), true));
    EXPECT_EQ(ring.dropped_frames(), 1u);
}

TEST(CliRingTest, WakeEndsADrainsWait)
{
    cli::LineRing ring;
    ASSERT_TRUE(ring.init(64));
    std::vector<char> out(ring.capacity());
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ring.wake();
    });
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(ring.drain(out.data(), std::chrono::seconds(10)), 0u);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
    waker.join();
}

// A sleep is a rise of a second or more in one poll (ADR-0033 section 6), told
// once; smaller rises are taken in, so they never add up to a line
/* Standard input's bytes in the pieces a read gives them: a line split
 * across reads is one line, CRLF and LF both end one, an empty line is
 * one (an unlabeled marker), the last without a newline counts at the
 * end, and a line past kMax keeps its first kMax bytes */
TEST(CliMarkLinesTest, TypedBytesBecomeLines)
{
    cli::MarkLines split;
    std::vector<std::string> got;
    const auto out = [&](const std::string& line) { got.push_back(line); };
    const std::string typed = "lap 1\r\n\nsec";
    split.feed(typed.data(), typed.size(), out);
    EXPECT_EQ(got, (std::vector<std::string>{"lap 1", ""}));
    const std::string rest = "ond lap\nlast";
    split.feed(rest.data(), rest.size(), out);
    split.finish(out);
    EXPECT_EQ(got,
              (std::vector<std::string>{"lap 1", "", "second lap", "last"}));

    got.clear();
    const std::string long_line(cli::MarkLines::kMax + 10, 'x');
    split.feed(long_line.data(), long_line.size(), out);
    split.feed("\n", 1, out);
    split.finish(out);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].size(), cli::MarkLines::kMax);
}

TEST(CliSleepTest, ARiseOfASecondInOnePollIsASleep)
{
    cli::SleepWatch w(7000000);
    EXPECT_EQ(w.poll(7000000), 0u);
    EXPECT_EQ(w.poll(105400000), 98400000u);
    EXPECT_EQ(w.poll(105400000), 0u) << "told once";
    EXPECT_EQ(w.poll(106300000), 0u) << "0.9 s";
    EXPECT_EQ(w.poll(106900000), 0u) << "0.6 s more: not 1.5 s";
    EXPECT_EQ(w.poll(107900000), 1000000u) << "a second exactly";
    EXPECT_EQ(w.poll(5), 0u) << "a count that went back";
}

// ---- Reply timing (ADR-0042) ---------------------------------------------

namespace {

// The keys of a flat JSON object, in order
std::vector<std::string> keys_of(const std::string& line)
{
    std::vector<std::string> keys;
    size_t at = 0;
    while ((at = line.find('"', at)) != std::string::npos) {
        const size_t end = line.find('"', at + 1);
        if (end == std::string::npos) {
            break;
        }
        if (end + 1 < line.size() && line[end + 1] == ':') {
            keys.push_back(line.substr(at + 1, end - at - 1));
        }
        at = end + 1;
    }
    return keys;
}

std::string timing_text(const ecuio_request_timing_t& r, uint32_t clock)
{
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::timing_line(l, 12345678, 0x7E8, r, clock);
    return std::string(l.data(), l.size());
}

} // namespace

// A timing line's keys are latency.csv's columns, in their order: one list
// (ECUIO_TIMING_COLUMNS) for both writers
TEST(CliTimingTest, ALinesKeysAreTheCsvsColumns)
{
    ecuio_request_timing_t r{};
    r.length = 2;
    r.request[0] = 0x01;
    r.request[1] = 0x0C;
    std::vector<std::string> columns;
    std::string all = ECUIO_TIMING_COLUMNS;
    for (size_t at = 0; at <= all.size();) {
        const size_t comma = all.find(',', at);
        const size_t end = comma == std::string::npos ? all.size() : comma;
        columns.push_back(all.substr(at, end - at));
        at = end + 1;
    }
    std::vector<std::string> keys =
        keys_of(timing_text(r, ECUIO_TIMING_CLOCK_HOST));
    ASSERT_FALSE(keys.empty());
    EXPECT_EQ(keys.front(), "type");
    keys.erase(keys.begin());
    EXPECT_EQ(keys, columns);
}

// The figures in frames.csv's forms; a quantile with too few samples, or
// none at all, is null
TEST(CliTimingTest, AQuantileWithTooFewSamplesIsNull)
{
    ecuio_request_timing_t r{};
    r.length = 2;
    r.request[0] = 0x01;
    r.request[1] = 0x0C;
    r.answered = 120;
    r.timeouts = 2;
    r.packed = 100;
    r.latency_p50_us = 4100;
    r.latency_p99_us = 6100;
    r.latency_max_us = 7000;
    r.flags = ECUIO_TIMING_LATENCY_P99;
    const std::string line = timing_text(r, ECUIO_TIMING_CLOCK_ADAPTER);
    EXPECT_NE(line.find("\"t_s\":12.346"), std::string::npos) << line;
    EXPECT_NE(line.find("\"ecu\":\"0x7E8\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"request\":\"01 0C\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"latency_p50_us\":4100"), std::string::npos) << line;
    EXPECT_NE(line.find("\"latency_p99_us\":6100"), std::string::npos) << line;
    EXPECT_NE(line.find("\"latency_p999_us\":null"), std::string::npos) << line;
    EXPECT_NE(line.find("\"interval_p50_us\":null"), std::string::npos)
        << "no intervals: " << line;
    EXPECT_NE(line.find("\"interval_clock\":\"adapter\""), std::string::npos)
        << line;
    EXPECT_EQ(line.back(), '}');
}

// A snapshot every 10 s of session time, carrying the rows whose counts
// changed since the one that last carried them
TEST(CliTimingTest, ASnapshotCarriesTheRowsThatChanged)
{
    cli::TimingSnapshots s;
    EXPECT_FALSE(s.due(9999999));
    EXPECT_TRUE(s.due(10000000));
    s.taken(10500000);
    EXPECT_FALSE(s.due(20000000));
    EXPECT_TRUE(s.due(20500000));

    ecuio_request_timing_t r{};
    EXPECT_FALSE(s.changed(0, r)) << "nothing yet";
    r.answered = 3;
    EXPECT_TRUE(s.changed(0, r));
    EXPECT_FALSE(s.changed(0, r)) << "carried once";
    r.timeouts = 1;
    EXPECT_TRUE(s.changed(0, r)) << "a timeout counts";
    EXPECT_TRUE(s.changed(1, r)) << "each row its own";
    EXPECT_FALSE(s.changed(ECUIO_PROFILE_MAX_REQUESTS, r));
}

// A DID scan's progress, from the channel's support report
TEST(CliScanTest, ALineCarriesTheScansCounts)
{
    ecuio_channel_support_t s{};
    s.rx_id = 0x7E8;
    s.scan_total = 1024;
    s.scan_done = 300;
    s.scan_found = 7;
    s.scan_refused = 293;
    s.scan_polled = 7;
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::scan_line(l, s);
    ASSERT_TRUE(l.finish());
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"scan\",\"ecu\":\"0x7E8\",\"total\":1024,"
              "\"done\":300,\"found\":7,\"refused\":293,\"left\":724,"
              "\"polled\":7,\"complete\":false}\n");
}

// While a scan runs, a change is written at most once a second; its end at
// once, and every change on the stream's last read; a channel with no scan
// never
TEST(CliScanTest, AChangeIsWrittenOnceASecondAndTheEndAtOnce)
{
    ecuio_channel_support_t s{};
    s.scan_total = 4;
    cli::ScanSeen seen;
    EXPECT_TRUE(cli::scan_due(s, seen, 0)) << "the first read";
    EXPECT_FALSE(cli::scan_due(s, seen, 2000000)) << "nothing changed";
    s.scan_done = 1;
    EXPECT_TRUE(cli::scan_due(s, seen, 2000000));
    s.scan_done = 2;
    EXPECT_FALSE(cli::scan_due(s, seen, 2500000)) << "within the second";
    EXPECT_TRUE(cli::scan_due(s, seen, 2500000, true)) << "the last read";
    s.scan_done = 3;
    EXPECT_TRUE(cli::scan_due(s, seen, 3500001));
    s.scan_done = 4;
    s.flags = ECUIO_SUPPORT_SCAN_COMPLETE;
    EXPECT_TRUE(cli::scan_due(s, seen, 3500002)) << "the end, at once";
    EXPECT_FALSE(cli::scan_due(s, seen, 9000000));

    ecuio_channel_support_t none{};
    cli::ScanSeen none_seen;
    EXPECT_FALSE(cli::scan_due(none, none_seen, 0));
    EXPECT_FALSE(cli::scan_due(none, none_seen, 0, true));
}

// --until-scanned ends once every channel with a scan has completed it;
// with none scanning there is nothing to wait for
TEST(CliScanTest, UntilScannedWaitsForEveryChannelThatScans)
{
    ecuio_channel_support_t s[3] = {};
    EXPECT_FALSE(cli::scans_any(s, 3));
    EXPECT_FALSE(cli::scans_complete(s, 3)) << "nothing scans";
    s[0].scan_total = 4;
    s[2].scan_total = 2;
    EXPECT_TRUE(cli::scans_any(s, 3));
    s[0].flags = ECUIO_SUPPORT_SCAN_COMPLETE;
    EXPECT_FALSE(cli::scans_complete(s, 3)) << "the third still scans";
    s[2].flags = ECUIO_SUPPORT_SCAN_COMPLETE;
    EXPECT_TRUE(cli::scans_complete(s, 3)) << "the second has no scan";
}

// With ?session=03 the scan line names the session in force (ADR-0060);
// without it, the line is as it was
TEST(CliSessionTest, TheScanLineNamesTheSessionOnlyWhenOneWasAsked)
{
    ecuio_channel_support_t s{};
    s.rx_id = 0x7E8;
    s.scan_total = 2;
    s.scan_done = 2;
    s.scan_found = 1;
    s.scan_refused = 1;
    s.flags = ECUIO_SUPPORT_SCAN_COMPLETE;
    const auto line = [](const ecuio_channel_support_t& r) {
        char buf[512];
        cli::Line l(buf, sizeof buf);
        cli::scan_line(l, r);
        EXPECT_TRUE(l.finish());
        return std::string(l.data(), l.size());
    };
    const std::string plain =
        "{\"type\":\"scan\",\"ecu\":\"0x7E8\",\"total\":2,"
        "\"done\":2,\"found\":1,\"refused\":1,\"left\":0,"
        "\"polled\":0,\"complete\":true";
    EXPECT_EQ(line(s), plain + "}\n");
    s.session_asked = 0x03;
    s.session = 0x03;
    EXPECT_EQ(line(s), plain + ",\"session\":\"03\"}\n");
    s.session = 0x01;
    EXPECT_EQ(line(s), plain + ",\"session\":\"01\"}\n");
}

TEST(CliSessionTest, ASessionLineSaysWhatTheModuleReadsIn)
{
    ecuio_channel_support_t s{};
    s.rx_id = 0x7E9;
    s.generation = 1;
    s.session_asked = 0x03;
    s.session = 0x01;
    s.session_state = ECUIO_SESSION_STATE_REFUSED;
    s.session_nrc = 0x12;
    const auto line = [](const ecuio_channel_support_t& r) {
        char buf[512];
        cli::Line l(buf, sizeof buf);
        cli::session_line(l, r);
        EXPECT_TRUE(l.finish());
        return std::string(l.data(), l.size());
    };
    EXPECT_EQ(line(s),
              "{\"type\":\"session\",\"ecu\":\"0x7E9\",\"asked\":\"03\","
              "\"session\":\"01\",\"state\":\"refused\",\"nrc\":\"12\","
              "\"entries\":0,\"lost\":0}\n");
    s.session = 0x03;
    s.session_state = ECUIO_SESSION_STATE_ENTERED;
    s.session_nrc = 0;
    s.session_entries = 2;
    s.session_lost = 1;
    EXPECT_EQ(line(s),
              "{\"type\":\"session\",\"ecu\":\"0x7E9\",\"asked\":\"03\","
              "\"session\":\"03\",\"state\":\"entered\",\"nrc\":null,"
              "\"entries\":2,\"lost\":1}\n");
    for (unsigned st :
         {ECUIO_SESSION_STATE_NOT_ASKED, ECUIO_SESSION_STATE_ENTERING,
          ECUIO_SESSION_STATE_UNANSWERED, ECUIO_SESSION_STATE_ABANDONED}) {
        s.session_state = static_cast<uint8_t>(st);
        EXPECT_NE(line(s).find(cli::session_state_word(s.session_state)),
                  std::string::npos);
    }
    EXPECT_STREQ(cli::session_state_word(6), "unknown");
}

// A line on each change of state, code or counts; none without a session
// asked or before the channel's first report
TEST(CliSessionTest, ALineIsWrittenOnEachChangeAndOnlyWithASession)
{
    ecuio_channel_support_t s{};
    s.generation = 1;
    cli::SessionSeen seen;
    EXPECT_FALSE(cli::session_due(s, seen)) << "no session asked";
    s.session_asked = 0x03;
    s.generation = 0;
    EXPECT_FALSE(cli::session_due(s, seen)) << "no report yet";
    s.generation = 1;
    s.session_state = ECUIO_SESSION_STATE_ENTERING;
    EXPECT_TRUE(cli::session_due(s, seen));
    EXPECT_FALSE(cli::session_due(s, seen)) << "nothing changed";
    s.session_state = ECUIO_SESSION_STATE_ENTERED;
    s.session_entries = 1;
    EXPECT_TRUE(cli::session_due(s, seen));
    s.generation = 5; /* a scan's progress alone */
    EXPECT_FALSE(cli::session_due(s, seen));
    s.session_lost = 1;
    EXPECT_TRUE(cli::session_due(s, seen));
    s.session_nrc = 0x7F;
    EXPECT_TRUE(cli::session_due(s, seen));
}

/* devices' can line (ADR-0040 section 2): what a connect is judged on,
 * in ip link's words; the driver's modes null when the kernel doesn't
 * say; a vcan's state null and its modes empty */
TEST(CliCanTest, ALineSaysWhatAConnectIsJudgedOn)
{
    ecuio_can_interface_t c{};
    std::memcpy(c.name, "can0", 5);
    std::memcpy(c.kind, "can", 4);
    c.flags = ECUIO_CAN_UP | ECUIO_CAN_MODES_KNOWN;
    c.bitrate = 500000;
    c.state = ECUIO_CAN_STATE_ERROR_PASSIVE;
    c.modes = ECUIO_CAN_MODE_BERR_REPORTING;
    c.modes_supported = ECUIO_CAN_MODE_LOOPBACK | ECUIO_CAN_MODE_LISTEN_ONLY |
                        ECUIO_CAN_MODE_BERR_REPORTING | 0x2000u;
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::can_line(l, c);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"can\",\"name\":\"can0\",\"kind\":\"can\","
              "\"up\":true,\"listen_only\":false,\"fd\":false,"
              "\"bitrate\":500000,\"state\":\"error-passive\","
              "\"modes\":[\"berr-reporting\"],\"modes_supported\":"
              "[\"loopback\",\"listen-only\",\"berr-reporting\",\"0x2000\"],"
              "\"uri\":\"socketcan://can0\"}");

    c.flags = ECUIO_CAN_UP | ECUIO_CAN_LISTEN_ONLY; /* a kernel before 6.0 */
    c.modes = ECUIO_CAN_MODE_LISTEN_ONLY;
    c.modes_supported = 0;
    cli::Line o(buf, sizeof buf);
    cli::can_line(o, c);
    const std::string older(o.data(), o.size());
    EXPECT_NE(
        older.find("\"modes\":[\"listen-only\"],\"modes_supported\":null"),
        std::string::npos)
        << older;

    ecuio_can_interface_t v{};
    std::memcpy(v.name, "vcan0", 6);
    std::memcpy(v.kind, "vcan", 5);
    v.flags = ECUIO_CAN_UP | ECUIO_CAN_FD;
    cli::Line w(buf, sizeof buf);
    cli::can_line(w, v);
    EXPECT_EQ(std::string(w.data(), w.size()),
              "{\"type\":\"can\",\"name\":\"vcan0\",\"kind\":\"vcan\","
              "\"up\":true,\"listen_only\":false,\"fd\":true,\"bitrate\":0,"
              "\"state\":null,\"modes\":[],\"modes_supported\":null,"
              "\"uri\":\"socketcan://vcan0\"}");
}

/* ADR-0043: a VIN's decode, field by field (the VINs made up around FAKE,
 * their check digits by 49 CFR 565.15) */
TEST(CliVinTest, ADecodeCarriesEachField)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::vin_line(l, "1FAFAKE13Y0000001", 2027);
    const std::string source = ecuio_vin_source();
    EXPECT_EQ(
        std::string(l.data(), l.size()),
        "{\"type\":\"vin\",\"vin\":\"1FAFAKE13Y0000001\",\"decoded\":true,"
        "\"model_year\":2000,\"year_earlier\":true,\"make\":\"Ford\","
        "\"wmi\":\"1FA\",\"check_digit\":\"3\",\"check_matches\":true,"
        "\"source\":\"" +
            source + "\"}");

    cli::Line u(buf, sizeof buf);
    cli::vin_line(u, "ZZZFAKE18U0000001", 0);
    EXPECT_EQ(
        std::string(u.data(), u.size()),
        "{\"type\":\"vin\",\"vin\":\"ZZZFAKE18U0000001\",\"decoded\":true,"
        "\"model_year\":null,\"year_earlier\":false,\"make\":null,"
        "\"wmi\":\"ZZZ\",\"check_digit\":\"1\",\"check_matches\":false,"
        "\"source\":\"" +
            source + "\"}");
}

TEST(CliVinTest, NotAVinIsNotDecoded)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::vin_line(l, "FAKEJ2534VIN00001", 0);
    EXPECT_EQ(
        std::string(l.data(), l.size()),
        "{\"type\":\"vin\",\"vin\":\"FAKEJ2534VIN00001\",\"decoded\":false}");
}

/* ADR-0021: the identity line names each item, its request and its
 * outcome, then the VIN core takes from it, decoded */
TEST(CliVinTest, AnIdentityLineCarriesItsItemsAndVin)
{
    ecuio_channel_identity_t id{};
    id.flags = ECUIO_IDENTITY_COMPLETE;
    id.rx_id = 0x7E8;
    id.generation = 4;
    id.items = 3;
    auto item = [&id](uint32_t i, uint8_t a, uint8_t b, uint8_t outcome,
                      const char* text, uint8_t nrc) {
        ecuio_identity_item_t& it = id.list[i];
        it.request[0] = a;
        it.request[1] = b;
        it.length = 2;
        it.outcome = outcome;
        it.nrc = nrc;
        std::snprintf(it.text, sizeof it.text, "%s", text);
        it.text_length = static_cast<uint8_t>(std::strlen(it.text));
    };
    item(0, 0x09, 0x02, ECUIO_IDENTITY_ANSWERED, "1FAFAKE10M0000001", 0);
    item(1, 0x09, 0x04, ECUIO_IDENTITY_REFUSED, "", 0x12);
    item(2, 0x09, 0x06, ECUIO_IDENTITY_UNANSWERED, "", 0);
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    cli::identity_line(l, id, 2027);
    const std::string line(l.data(), l.size());
    EXPECT_EQ(
        line.substr(0, line.find(",\"source\":")),
        "{\"type\":\"identity\",\"rx\":\"0x7E8\",\"generation\":4,\"items\":["
        "{\"name\":\"VIN\",\"request\":\"09 02\",\"outcome\":\"answered\","
        "\"text\":\"1FAFAKE10M0000001\"},"
        "{\"name\":\"CALID\",\"request\":\"09 04\",\"outcome\":\"refused\","
        "\"reply\":\"7F 09 12\"},"
        "{\"name\":\"CVN\",\"request\":\"09 06\",\"outcome\":\"unanswered\"}],"
        "\"vin\":{\"vin\":\"1FAFAKE10M0000001\",\"decoded\":true,"
        "\"model_year\":2021,\"year_earlier\":false,\"make\":\"Ford\","
        "\"wmi\":\"1FA\",\"check_digit\":\"0\",\"check_matches\":true");
    EXPECT_EQ(line.substr(line.size() - 2), "}}");

    // With no VIN among the answers, "vin" is null
    id.list[0].outcome = ECUIO_IDENTITY_UNANSWERED;
    cli::Line n(buf, sizeof buf);
    cli::identity_line(n, id, 2027);
    const std::string none(n.data(), n.size());
    EXPECT_EQ(none.substr(none.size() - 12), ",\"vin\":null}");
}

/* ADR-0044: a code's title, null for one the list lacks (a chassis code,
 * a manufacturer's), the freeze frame's beside its code */
TEST(CliDtcTest, ATitleOrNull)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::dtc_line(l, 0x0301);
    const std::string source = ecuio_dtc_title_source();
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"dtc\",\"code\":\"P0301\","
              "\"title\":\"Cylinder 1 Misfire Detected\","
              "\"source\":\"" +
                  source + "\"}");
    for (uint16_t none : {uint16_t{0x4035}, uint16_t{0x1234}}) {
        cli::Line n(buf, sizeof buf);
        cli::dtc_line(n, none);
        const std::string line(n.data(), n.size());
        EXPECT_NE(line.find(",\"title\":null,"), std::string::npos) << line;
    }
}

TEST(CliDtcTest, TheFreezeFramesCodeCarriesItsTitle)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_FREEZE_FRAME;
    d.freeze = ECUIO_DIAG_ANSWERED;
    d.freeze_dtc = 0x0420;
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    cli::diagnostics_line(l, d);
    const std::string line(l.data(), l.size());
    EXPECT_NE(
        line.find(
            "\"freeze\":{\"outcome\":\"answered\",\"code\":\"P0420\","
            "\"title\":\"Catalyst System Efficiency Below Threshold Bank 1\","
            "\"values\":[]}"),
        std::string::npos)
        << line;
}

TEST(CliInspectionTest, TheVerdictNamesTheIncompleteMonitors)
{
    ecuio_inspection_t i{};
    i.verdict = ECUIO_INSPECTION_NOT_READY;
    i.incomplete = 2;
    i.allowed = 1;
    i.reports = 1;
    i.incomplete_ids = (1u << 8) | (1u << 10);
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::inspection_line(l, i, 2015);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"inspection\",\"verdict\":\"not_ready\","
              "\"model_year\":2015,"
              "\"incomplete\":[\"Catalyst\",\"Evaporative system\"],"
              "\"allowed\":1,\"reports\":1,\"rule\":\"40 CFR 85.2222\"}");
    // no model year: both null; a compression-ignition engine's names
    i.verdict = ECUIO_INSPECTION_YEAR_NEEDED;
    i.diag_flags = ECUIO_DIAG_COMPRESSION;
    i.incomplete_ids = (1u << 8) | (1u << 11);
    i.allowed = 0;
    cli::Line n(buf, sizeof buf);
    cli::inspection_line(n, i, 0);
    EXPECT_EQ(std::string(n.data(), n.size()),
              "{\"type\":\"inspection\",\"verdict\":\"year_needed\","
              "\"model_year\":null,"
              "\"incomplete\":[\"NMHC catalyst\",\"Boost pressure\"],"
              "\"allowed\":null,\"reports\":1,\"rule\":\"40 CFR 85.2222\"}");
    for (uint8_t v = 0; v <= ECUIO_INSPECTION_READING; ++v) {
        EXPECT_STRNE(cli::inspection_word(v), "") << int(v);
    }
    EXPECT_STREQ(cli::inspection_word(ECUIO_INSPECTION_MIL_ON), "mil_on");
    EXPECT_STREQ(cli::inspection_word(ECUIO_INSPECTION_READY), "ready");
}

TEST(CliSimTest, TheLinesCarryTheUriAndTheCounts)
{
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::sim_line(l, "mock://sim?values=canned", "vcan0", false);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"sim\",\"uri\":\"mock://sim?values=canned\","
              "\"on\":\"vcan0\",\"desk_bus\":false}");
    cli::Line d(buf, sizeof buf);
    cli::sim_line(d, "mock://sim", "can0", true);
    EXPECT_EQ(std::string(d.data(), d.size()),
              "{\"type\":\"sim\",\"uri\":\"mock://sim\","
              "\"on\":\"can0\",\"desk_bus\":true}");
    ecuio_sim_stats_t s{};
    s.requests = 10;
    s.replies = 7;
    s.refused = 1;
    s.silent = 2;
    s.late = 1;
    s.bad_sequence = 3;
    s.stalled = 4;
    s.unflowed = 5;
    s.clears = 6;
    s.pending = 8;
    const char* counts =
        "\"requests\":10,\"replies\":7,\"refused\":1,\"silent\":2,\"late\":1,"
        "\"pending\":8,\"bad_sequence\":3,\"stalled\":4,\"unflowed\":5,"
        "\"clears\":6,";
    cli::Line st(buf, sizeof buf);
    cli::sim_stats_line(st, s);
    EXPECT_EQ(std::string(st.data(), st.size()),
              std::string("{\"type\":\"sim_stats\",") + counts +
                  "\"failed\":false,\"yielded\":false}");
    s.flags = ECUIO_SIM_FAILED;
    cli::Line e(buf, sizeof buf);
    cli::sim_end_line(e, "bus-failed", s);
    EXPECT_EQ(std::string(e.data(), e.size()),
              std::string("{\"type\":\"end\",\"reason\":\"bus-failed\",") +
                  counts + "\"failed\":true,\"yielded\":false}");
    s.flags = ECUIO_SIM_YIELDED;
    cli::Line y(buf, sizeof buf);
    cli::sim_end_line(y, "yielded", s);
    EXPECT_EQ(std::string(y.data(), y.size()),
              std::string("{\"type\":\"end\",\"reason\":\"yielded\",") +
                  counts + "\"failed\":false,\"yielded\":true}");
}

TEST(CliViewTest, AlignOnTakesItsFiveFormsAndNothingElse)
{
    cli::AlignOn a;
    ASSERT_TRUE(cli::parse_align_on("rise:RPM:4000", a));
    EXPECT_EQ(a.kind, cli::AlignOn::rise);
    EXPECT_EQ(a.signal, "RPM");
    EXPECT_EQ(a.level, 4000);
    /* The level is the last colon's: a name may hold one */
    ASSERT_TRUE(cli::parse_align_on("fall:Trim:B1:-2.5", a));
    EXPECT_EQ(a.kind, cli::AlignOn::fall);
    EXPECT_EQ(a.signal, "Trim:B1");
    EXPECT_EQ(a.level, -2.5);
    ASSERT_TRUE(cli::parse_align_on("marker:lap: 2", a));
    EXPECT_EQ(a.kind, cli::AlignOn::marker);
    EXPECT_EQ(a.label, "lap: 2");
    ASSERT_TRUE(cli::parse_align_on("engine-start", a));
    EXPECT_EQ(a.kind, cli::AlignOn::engine_start);
    ASSERT_TRUE(cli::parse_align_on("utc", a));
    EXPECT_EQ(a.kind, cli::AlignOn::utc);
    for (const char* bad :
         {"", "UTC", "rise", "rise:RPM", "rise::4000",
          "rise:RPM:", "rise:RPM:fast", "rise:RPM:inf", "rise:RPM:4000x",
          "marker:", "sideways:RPM:1", "Engine-Start", "engine-start:RPM",
          "engine", "engine-start "}) {
        EXPECT_FALSE(cli::parse_align_on(bad, a)) << bad;
    }
    EXPECT_EQ(a.kind, cli::AlignOn::utc) << "a refusal leaves it as it was";
}

TEST(CliViewTest, SecondsAreSignedAndToTheMicrosecond)
{
    int64_t us = 0;
    ASSERT_TRUE(cli::parse_seconds("-5", us));
    EXPECT_EQ(us, -5000000);
    ASSERT_TRUE(cli::parse_seconds("1.0000015", us));
    EXPECT_EQ(us, 1000002);
    ASSERT_TRUE(cli::parse_seconds("0", us));
    EXPECT_EQ(us, 0);
    for (const char* bad : {"", "soon", "5s", "nan", "1e10", "-1e10"}) {
        EXPECT_FALSE(cli::parse_seconds(bad, us)) << bad;
    }
}

TEST(CliViewTest, ASpanIsTwoSecondsInOrder)
{
    cli::Span s;
    ASSERT_TRUE(cli::parse_span("-1.5:2", s));
    EXPECT_EQ(s.from_us, -1500000);
    EXPECT_EQ(s.to_us, 2000000);
    ASSERT_TRUE(cli::parse_span("0:0.000001", s));
    EXPECT_EQ(s.to_us, 1);
    for (const char* bad :
         {"", "5", "5:", ":5", "5:3", "5:5", "a:b", "1:2:3", "1;2"}) {
        EXPECT_FALSE(cli::parse_span(bad, s)) << bad;
    }
}

TEST(CliViewTest, AStatsLineHoldsTheSpanAndNullsWhatIsntThere)
{
    char buf[1024];
    ecuio_recording_signal_t rpm{};
    std::strcpy(rpm.name, "RPM");
    cli::Span span;
    span.from_us = -1000000;
    span.to_us = 2500000;
    ecuio_recording_stats_t st = cli::no_stats();
    st.samples = 3;
    st.first = 812.5;
    st.last = 900;
    st.lowest = 800;
    st.lowest_us = 400000;
    st.highest = HUGE_VAL;
    st.highest_us = 2000000;
    st.mean = HUGE_VAL;
    cli::Line l(buf, sizeof buf);
    cli::stats_line(l, 1, rpm, span, st);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"stats\",\"recording\":1,\"signal\":\"RPM\","
              "\"from_us\":-1000000,\"to_us\":2500000,\"samples\":3,"
              "\"first\":812.5,\"last\":900,\"lowest\":800,"
              "\"lowest_us\":400000,\"highest\":null,\"highest_us\":2000000,"
              "\"mean\":null}");
    cli::Line e(buf, sizeof buf);
    cli::stats_line(e, 0, rpm, span, cli::no_stats());
    EXPECT_EQ(std::string(e.data(), e.size()),
              "{\"type\":\"stats\",\"recording\":0,\"signal\":\"RPM\","
              "\"from_us\":-1000000,\"to_us\":2500000,\"samples\":0,"
              "\"first\":null,\"last\":null,\"lowest\":null,"
              "\"lowest_us\":null,\"highest\":null,\"highest_us\":null,"
              "\"mean\":null}");
}

TEST(CliViewTest, AStretchLevelIsAKeyAndAValueInItsUnits)
{
    ecuio_stretch_rules_t r = cli::stretch_rules(ECUIO_STRETCH_PULLS);
    EXPECT_EQ(r.kinds, ECUIO_STRETCH_PULLS);
    EXPECT_EQ(r.throttle, UINT32_MAX);
    EXPECT_EQ(r.engine_speed, UINT32_MAX);
    ASSERT_TRUE(cli::parse_stretch_level("pull-throttle=85", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-load=70.5", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-hold=1.5", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-gap=0.75", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("cruise-hold=20", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("cruise-throttle-band=5", true, r));
    EXPECT_EQ(r.pull_level, 85.0);
    EXPECT_EQ(r.pull_load_level, 70.5);
    EXPECT_EQ(r.pull_hold_us, 1500000u);
    EXPECT_EQ(r.pull_gap_us, 750000u);
    EXPECT_EQ(r.cruise_hold_us, 20000000u);
    EXPECT_EQ(r.cruise_throttle_band, 5.0) << "a percentage has no unit system";

    /* Speeds in km/h, or mph under imperial: 40 mph reaches core as km/h */
    ASSERT_TRUE(cli::parse_stretch_level("cruise-floor=40", true, r));
    EXPECT_DOUBLE_EQ(r.cruise_floor, 64.37376);
    ASSERT_TRUE(cli::parse_stretch_level("cruise-speed-band=2", true, r));
    EXPECT_DOUBLE_EQ(r.cruise_speed_band, 3.218688);
    ASSERT_TRUE(cli::parse_stretch_level("cruise-floor=60", false, r));
    EXPECT_EQ(r.cruise_floor, 60.0);
    ASSERT_TRUE(cli::parse_stretch_level("pull-gap=0", false, r));
    EXPECT_EQ(r.pull_gap_us, 0u) << "0 takes core's default";

    for (const char* bad :
         {"pull-gap", "pull-gap=", "pull-gap=-1", "pull-gap=soon", "=1",
          "pull-speed=1", "cruise-floor=1e10"}) {
        EXPECT_FALSE(cli::parse_stretch_level(bad, false, r)) << bad;
    }
}

TEST(CliViewTest, ARoleAndAKindAreOneOfTheirWords)
{
    uint32_t role = 0;
    std::string name;
    ASSERT_TRUE(cli::parse_role("speed=Vehicle Speed", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_VEHICLE_SPEED);
    EXPECT_EQ(name, "Vehicle Speed");
    ASSERT_TRUE(cli::parse_role("throttle=Pedal=D", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_THROTTLE);
    EXPECT_EQ(name, "Pedal=D") << "the name is all after the first =";
    ASSERT_TRUE(cli::parse_role("load=Load", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_ENGINE_LOAD);
    ASSERT_TRUE(cli::parse_role("rpm=RPM", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_ENGINE_SPEED);
    for (const char* bad : {"speed", "speed=", "brake=Pedal", "=RPM"}) {
        EXPECT_FALSE(cli::parse_role(bad, role, name)) << bad;
    }
    ecuio_stretch_rules_t r = cli::stretch_rules(0);
    cli::role_field(r, ECUIO_RECORDING_ROLE_ENGINE_LOAD) = 7;
    EXPECT_EQ(r.engine_load, 7u);
    EXPECT_EQ(r.throttle, UINT32_MAX);

    uint32_t kinds = 0;
    ASSERT_TRUE(cli::parse_stretch_kinds("pulls", kinds));
    EXPECT_EQ(kinds, ECUIO_STRETCH_PULLS);
    ASSERT_TRUE(cli::parse_stretch_kinds("cruises", kinds));
    EXPECT_EQ(kinds, ECUIO_STRETCH_CRUISES);
    ASSERT_TRUE(cli::parse_stretch_kinds("all", kinds));
    EXPECT_EQ(kinds, ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES);
    EXPECT_FALSE(cli::parse_stretch_kinds("pull", kinds));
}

TEST(CliViewTest, TheRolesAndStretchLinesSayWhatWasFoundAndWith)
{
    char buf[1024];
    ecuio_recording_signal_t speed{};
    std::strcpy(speed.name, "Vehicle Speed");
    ecuio_recording_signal_t rpm{};
    std::strcpy(rpm.name, "RPM");
    const ecuio_recording_signal_t* by_role[4] = {nullptr, nullptr, &speed,
                                                  &rpm};
    cli::Line l(buf, sizeof buf);
    cli::roles_line(l, 1, by_role, cli::stretch_rules(ECUIO_STRETCH_PULLS));
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"roles\",\"recording\":1,\"throttle\":null,"
              "\"engine_load\":null,\"vehicle_speed\":\"Vehicle Speed\","
              "\"engine_speed\":\"RPM\",\"cruise_floor\":56.32704,"
              "\"cruise_speed_band\":2}")
        << "the defaults core takes, in km/h";

    ecuio_recording_stretch_t s{};
    s.kind = ECUIO_RECORDING_STRETCH_PULL;
    s.from_us = 14000000;
    s.to_us = 19500000;
    s.start = 40;
    s.end = 113.5;
    s.flags = ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED |
              ECUIO_RECORDING_STRETCH_CUT_END;
    cli::Line sl(buf, sizeof buf);
    cli::stretch_line(sl, 0, s, rpm, -14600000);
    EXPECT_EQ(std::string(sl.data(), sl.size()),
              "{\"type\":\"stretch\",\"recording\":0,\"kind\":\"pull\","
              "\"from_us\":-600000,\"to_us\":4900000,\"length_us\":5500000,"
              "\"signal\":\"RPM\",\"start\":40,\"end\":113.5,"
              "\"by_load\":false,\"by_engine_speed\":true,"
              "\"no_throttle\":false,\"cut_start\":false,\"cut_end\":true}");
    s.kind = ECUIO_RECORDING_STRETCH_CRUISE;
    cli::Line cl(buf, sizeof buf);
    cli::stretch_line(cl, 0, s, speed, 0);
    EXPECT_NE(std::string(cl.data(), cl.size()).find("\"kind\":\"cruise\""),
              std::string::npos);
}

TEST(CliViewTest, TheAxisRoundsOutToWholeColumns)
{
    cli::Axis a;
    ASSERT_TRUE(cli::make_axis(-5000000, 10000000, 15, a));
    EXPECT_EQ(a.column_us, 1000000);
    EXPECT_EQ(a.to_us, 10000000);
    ASSERT_TRUE(cli::make_axis(0, 10, 3, a));
    EXPECT_EQ(a.column_us, 4);
    EXPECT_EQ(a.to_us, 12);
    EXPECT_FALSE(cli::make_axis(5, 5, 3, a));
    EXPECT_FALSE(cli::make_axis(0, 10, 0, a));
}

TEST(CliViewTest, AWindowCoversWhatTheRecordingHoldsOnce)
{
    cli::Axis a;
    cli::Window w;
    /* Unaligned: the whole axis, one query */
    ASSERT_TRUE(cli::make_axis(0, 10000000, 10, a));
    ASSERT_TRUE(cli::window_of(a, 0, w));
    EXPECT_EQ(w.first, 0u);
    EXPECT_EQ(w.count, 10u);
    EXPECT_FALSE(w.partial);
    EXPECT_EQ(w.t0_us, 0u);
    EXPECT_EQ(w.t1_us, 10000000u);

    /* Starting 3.5 s into the axis, wide columns: half a second of column
     * 3 holds no whole second, so the query starts at column 4, which
     * takes second 0 (it starts before the query) */
    ASSERT_TRUE(cli::window_of(a, 3500000, w));
    EXPECT_FALSE(w.partial);
    EXPECT_EQ(w.first, 4u);
    EXPECT_EQ(w.count, 6u);
    EXPECT_EQ(w.t0_us, 500000u);
    /* 3 s columns: the column across its start ends at its last whole
     * second, and the second across the split goes to the next */
    ASSERT_TRUE(cli::make_axis(0, 30000000, 10, a));
    ASSERT_TRUE(cli::window_of(a, 4500000, w));
    EXPECT_TRUE(w.partial);
    EXPECT_EQ(w.first, 1u);
    EXPECT_EQ(w.count, 9u);
    EXPECT_EQ(w.partial_t1_us, 1000000u);
    EXPECT_EQ(w.t0_us, 1500000u);
    EXPECT_EQ(w.t1_us, 25500000u);

    /* Narrow columns: the raw rows, split exactly */
    ASSERT_TRUE(cli::make_axis(0, 1000000, 10, a)); /* 100 ms */
    ASSERT_TRUE(cli::window_of(a, 250000, w));
    EXPECT_TRUE(w.partial);
    EXPECT_EQ(w.first, 2u);
    EXPECT_EQ(w.count, 8u);
    EXPECT_EQ(w.partial_t1_us, 50000u);
    EXPECT_EQ(w.t0_us, 50000u);

    /* On a column boundary: no partial column */
    ASSERT_TRUE(cli::window_of(a, 300000, w));
    EXPECT_FALSE(w.partial);
    EXPECT_EQ(w.first, 3u);
    EXPECT_EQ(w.t0_us, 0u);

    /* The axis ends inside the column across its start */
    ASSERT_TRUE(cli::window_of(a, 950000, w));
    EXPECT_TRUE(w.partial);
    EXPECT_EQ(w.first, 9u);
    EXPECT_EQ(w.count, 1u);
    EXPECT_EQ(w.partial_t1_us, 50000u);

    /* All of the axis before it, or the axis ending where it starts */
    EXPECT_FALSE(cli::window_of(a, 1000000, w));
    EXPECT_FALSE(cli::window_of(a, 5000000, w));
    /* Started before the axis: a later part of it */
    ASSERT_TRUE(cli::window_of(a, -2000000, w));
    EXPECT_EQ(w.first, 0u);
    EXPECT_EQ(w.t0_us, 2000000u);
    EXPECT_EQ(w.t1_us, 3000000u);
}

TEST(CliViewTest, TheLinesCarryWhatTheReaderHolds)
{
    char buf[1024];
    ecuio_recording_info_t info{};
    info.format = ECUIO_RECORDING_FORMAT_OLDER;
    info.flags = ECUIO_RECORDING_INFO_NO_SUMMARY;
    info.duration_us = 30000000;
    info.signal_count = 2;
    info.rows_torn = 1;
    info.records_dropped = 4;
    const ecuio_recording_signals_t no_file{};
    cli::Line o(buf, sizeof buf);
    cli::opened_line(o, 1, info, no_file);
    EXPECT_EQ(std::string(o.data(), o.size()),
              "{\"type\":\"opened\",\"recording\":1,\"format\":\"older\","
              "\"duration_us\":30000000,\"utc_start_us\":null,\"signals\":2,"
              "\"markers\":0,\"summary\":false,\"rows_torn\":1,"
              "\"records_dropped\":4,\"notes_dropped\":0,"
              "\"signal_file\":null}");
    /* The header's file: its hash, there and the same; or there unnamed */
    ecuio_recording_signals_t named{};
    named.flags = ECUIO_RECORDING_SIGNALS_NAMED |
                  ECUIO_RECORDING_SIGNALS_PRESENT |
                  ECUIO_RECORDING_SIGNALS_MATCH;
    std::memset(named.sha256, 'a', 64);
    cli::Line f(buf, sizeof buf);
    cli::opened_line(f, 1, info, named);
    const std::string line(f.data(), f.size());
    EXPECT_NE(line.find(",\"signal_file\":{\"sha256\":\"" +
                        std::string(64, 'a') +
                        "\",\"present\":true,\"matches\":true}}"),
              std::string::npos)
        << line;
    ecuio_recording_signals_t unnamed{};
    unnamed.flags = ECUIO_RECORDING_SIGNALS_PRESENT;
    cli::Line u(buf, sizeof buf);
    cli::opened_line(u, 1, info, unnamed);
    const std::string unnamed_line(u.data(), u.size());
    EXPECT_NE(unnamed_line.find(",\"signal_file\":{\"sha256\":null,\"present\":"
                                "true,\"matches\":false}}"),
              std::string::npos)
        << unnamed_line;
    EXPECT_STREQ(cli::format_word(ECUIO_RECORDING_FORMAT_ENUMS), "core")
        << "format 3 is core's, its enums on their signal lines";
    EXPECT_STREQ(cli::format_word(ECUIO_RECORDING_FORMAT_CORE), "core");
    EXPECT_STREQ(cli::format_word(99), "unknown");

    ecuio_recording_signal_t s{};
    std::strcpy(s.name, "RPM");
    std::strcpy(s.unit, "rpm");
    s.plot_rank = 1;
    s.group = ECUIO_SIGNAL_GROUP_ENGINE;
    s.samples = 2;
    s.lowest = 800;
    s.highest = 2512.5;
    s.first_us = 100;
    s.last_us = 200;
    cli::Line n(buf, sizeof buf);
    cli::signal_line(n, 0, 3, s);
    EXPECT_EQ(
        std::string(n.data(), n.size()),
        "{\"type\":\"signal\",\"recording\":0,\"index\":3,\"name\":\"RPM\","
        "\"unit\":\"rpm\",\"text\":false,\"decimals\":0,\"rank\":1,"
        "\"group\":\"Engine\",\"samples\":2,\"lowest\":800,"
        "\"highest\":2512.5,\"first_us\":100,\"last_us\":200,"
        "\"rate_hz\":10000}");
    ecuio_recording_signal_t none = s;
    none.samples = 0;
    none.first_us = ECUIO_RECORDING_NONE;
    none.last_us = ECUIO_RECORDING_NONE;
    cli::Line e(buf, sizeof buf);
    cli::signal_line(e, 0, 3, none);
    EXPECT_NE(std::string(e.data(), e.size())
                  .find("\"lowest\":null,\"highest\":null,\"first_us\":null,"
                        "\"last_us\":null,\"rate_hz\":null}"),
              std::string::npos);
    ecuio_recording_signal_t vin{};
    std::strcpy(vin.name, "VIN");
    vin.flags = ECUIO_RECORDING_SIGNAL_TEXT;
    vin.samples = 1;
    vin.group = 200; /* a group this core doesn't know: "" */
    cli::Line t(buf, sizeof buf);
    cli::signal_line(t, 0, 4, vin);
    EXPECT_NE(std::string(t.data(), t.size()).find("\"group\":\"\","),
              std::string::npos);
    EXPECT_EQ(std::string(t.data(), t.size()).find("lowest"), std::string::npos)
        << "a text signal has no range";
    EXPECT_NE(std::string(t.data(), t.size()).find("\"rate_hz\":null"),
              std::string::npos)
        << "one sample has no rate";

    /* An enum (format 3): "enum" and its labels after "text", a signal
     * file's "values" shape, then the rest as a number's; a label's
     * quotes escaped, as any string's */
    ecuio_recording_signal_t fuel{};
    std::strcpy(fuel.name, "Fuel");
    fuel.flags = ECUIO_RECORDING_SIGNAL_ENUM;
    fuel.samples = 2;
    fuel.lowest = 1;
    fuel.highest = 4;
    fuel.first_us = 100;
    fuel.last_us = 200;
    ecuio_enum_entry_t labels[2] = {};
    labels[0].raw = 1;
    std::strcpy(labels[0].label, "Open loop, cold");
    labels[1].raw = 4294967295u;
    std::strcpy(labels[1].label, "Say \"fault\"");
    cli::Line en(buf, sizeof buf);
    cli::signal_line(en, 0, 5, fuel, labels, 2);
    EXPECT_EQ(std::string(en.data(), en.size()),
              "{\"type\":\"signal\",\"recording\":0,\"index\":5,\"name\":\"Fuel\","
              "\"unit\":\"\",\"text\":false,\"enum\":true,\"labels\":{\"1\":"
              "\"Open loop, cold\",\"4294967295\":\"Say \\\"fault\\\"\"},"
              "\"decimals\":0,\"rank\":0,\"group\":\"Other\",\"samples\":2,"
              "\"lowest\":1,\"highest\":4,\"first_us\":100,\"last_us\":200,"
              "\"rate_hz\":10000}");
    cli::Line bare(buf, sizeof buf);
    cli::signal_line(bare, 0, 5, fuel);
    EXPECT_NE(std::string(bare.data(), bare.size())
                  .find("\"enum\":true,\"labels\":{},\"decimals\""),
              std::string::npos)
        << "an enum with no labels has an empty object";
    cli::Line number(buf, sizeof buf);
    cli::signal_line(number, 0, 3, s);
    EXPECT_EQ(std::string(number.data(), number.size()).find("enum"),
              std::string::npos)
        << "a number's line is as it was";
    EXPECT_GE(cli::labels_bytes(256), 256u * (12 + 6 * 27))
        << "room for 256 labels, every byte escaped";

    cli::Line al(buf, sizeof buf);
    cli::align_line(al, 1, "rise:RPM:4000", true, 24600000, -24600000);
    EXPECT_EQ(std::string(al.data(), al.size()),
              "{\"type\":\"align\",\"recording\":1,\"on\":\"rise:RPM:4000\","
              "\"found\":true,\"host_us\":24600000,\"offset_us\":-24600000}");
    cli::Line nf(buf, sizeof buf);
    cli::align_line(nf, 0, "marker:pull", false, 0, 0);
    EXPECT_EQ(std::string(nf.data(), nf.size()),
              "{\"type\":\"align\",\"recording\":0,\"on\":\"marker:pull\","
              "\"found\":false,\"host_us\":null,\"offset_us\":null}");

    /* Columns from the third on: the others empty */
    cli::Window w;
    w.first = 2;
    w.count = 2;
    ecuio_recording_column_t cols[2] = {};
    cols[0].samples = 3;
    cols[0].lowest = 1;
    cols[0].highest = 2.5;
    cli::Line c(buf, sizeof buf);
    cli::columns_line(c, 0, s, w, 4, cols);
    EXPECT_EQ(std::string(c.data(), c.size()),
              "{\"type\":\"columns\",\"recording\":0,\"signal\":\"RPM\","
              "\"samples\":[0,0,3,0],\"lowest\":[null,null,1,null],"
              "\"highest\":[null,null,2.5,null]}");
    cli::Line ct(buf, sizeof buf);
    cli::columns_line(ct, 0, vin, w, 2, nullptr);
    EXPECT_EQ(std::string(ct.data(), ct.size()),
              "{\"type\":\"columns\",\"recording\":0,\"signal\":\"VIN\","
              "\"samples\":[0,0]}");

    cli::Line at(buf, sizeof buf);
    cli::at_begin(at, 1, -2000000);
    cli::at_value(at, true, s, 300, "2512.5");
    cli::at_value(at, false, vin, ECUIO_RECORDING_NONE, "");
    cli::at_end(at);
    EXPECT_EQ(std::string(at.data(), at.size()),
              "{\"type\":\"at\",\"recording\":1,\"t_us\":-2000000,\"values\":"
              "[{\"signal\":\"RPM\",\"host_us\":300,\"text\":\"2512.5\"},"
              "{\"signal\":\"VIN\",\"host_us\":null,\"text\":\"\"}]}");
}

TEST(CliInfoTest, RateDropIsAMultipleOverOne)
{
    uint32_t x100 = 0;
    ASSERT_TRUE(cli::parse_rate_drop("4", x100));
    EXPECT_EQ(x100, 400u);
    ASSERT_TRUE(cli::parse_rate_drop("1.5", x100));
    EXPECT_EQ(x100, 150u);
    ASSERT_TRUE(cli::parse_rate_drop("1.01", x100));
    EXPECT_EQ(x100, 101u) << "the check's least";
    for (const char* bad :
         {"1", "1.004", "0.5", "0", "-4", "x", "", "4x", "inf", "1e9"}) {
        EXPECT_FALSE(cli::parse_rate_drop(bad, x100)) << bad;
    }
}

TEST(CliInfoTest, EachKindAndSkipHasItsWord)
{
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_GAP), "gap");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_RATE_DROP), "rate-drop");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_LATE_ID), "late-id");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_OVERFLOW), "overflow");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_ISOTP), "isotp");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_LINK), "link");
    EXPECT_STREQ(cli::check_kind_word(ECUIO_CHECK_ASLEEP), "asleep");
    EXPECT_STREQ(cli::check_kind_word(99), "unknown");
    EXPECT_STREQ(cli::isotp_word(0), "checked");
    EXPECT_STREQ(cli::isotp_word(ECUIO_CHECK_SKIPPED_ISOTP |
                                 ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN),
                 "flags-unknown");
    EXPECT_STREQ(
        cli::isotp_word(ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE),
        "no-wire");
}

TEST(CliInfoTest, TheLinesCarryWhatTheCheckFound)
{
    char buf[1024];
    ecuio_check_finding_t drop{};
    drop.kind = ECUIO_CHECK_RATE_DROP;
    drop.arbitration_id = 0x18DAF110;
    drop.flags = ECUIO_CHECK_FINDING_EXTENDED;
    drop.at_us = 7920000;
    drop.length_us = 800000;
    drop.count = 2;
    cli::Line d(buf, sizeof buf);
    cli::finding_line(d, drop);
    EXPECT_EQ(std::string(d.data(), d.size()),
              "{\"type\":\"finding\",\"kind\":\"rate-drop\",\"id\":"
              "\"0x18DAF110\",\"extended\":true,\"at_us\":7920000,"
              "\"length_us\":800000,\"count\":2}");

    /* The whole bus's has no ID, though 0x000 is one */
    ecuio_check_finding_t gap{};
    gap.kind = ECUIO_CHECK_GAP;
    gap.flags = ECUIO_CHECK_FINDING_WHOLE_BUS;
    gap.at_us = 4950000;
    gap.length_us = 1550000;
    cli::Line g(buf, sizeof buf);
    cli::finding_line(g, gap);
    EXPECT_EQ(std::string(g.data(), g.size()),
              "{\"type\":\"finding\",\"kind\":\"gap\",\"id\":null,"
              "\"extended\":false,\"at_us\":4950000,\"length_us\":1550000,"
              "\"count\":0}");
    ecuio_check_finding_t zero{};
    zero.kind = ECUIO_CHECK_LATE_ID;
    zero.count = 1;
    cli::Line z(buf, sizeof buf);
    cli::finding_line(z, zero);
    EXPECT_NE(std::string(z.data(), z.size()).find("\"id\":\"0x0\""),
              std::string::npos);

    /* A link's state, by code and core's words */
    ecuio_check_finding_t link{};
    link.kind = ECUIO_CHECK_LINK;
    link.flags = ECUIO_CHECK_FINDING_WHOLE_BUS;
    link.at_us = 18000000;
    link.count = 1;
    link.code = ECUIO_ERR_VEHICLE_TIMEOUT;
    cli::Line k(buf, sizeof buf);
    cli::finding_line(k, link);
    EXPECT_EQ(std::string(k.data(), k.size()),
              std::string("{\"type\":\"finding\",\"kind\":\"link\",\"id\":null,"
                          "\"extended\":false,\"at_us\":18000000,"
                          "\"length_us\":0,\"count\":1,\"code\":8,"
                          "\"message\":\"") +
                  ecuio_status_string(ECUIO_ERR_VEHICLE_TIMEOUT) + "\"}");

    ecuio_check_summary_t s{};
    s.duration_us = 19950000;
    s.frames = 515;
    s.ids = 3;
    s.torn_rows = 1;
    s.flags = ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE;
    for (uint32_t kind = ECUIO_CHECK_GAP; kind <= ECUIO_CHECK_ASLEEP; ++kind) {
        s.counts[kind] = kind * 10;
    }
    ecuio_check_options_t o{};
    o.gap_us = 1000000;
    o.rate_drop_x100 = 450;
    o.settle_us = 2000000;
    cli::Line m(buf, sizeof buf);
    cli::summary_line(m, s, o);
    EXPECT_EQ(std::string(m.data(), m.size()),
              "{\"type\":\"summary\",\"duration_us\":19950000,\"frames\":515,"
              "\"ids\":3,\"torn_rows\":1,\"isotp\":\"no-wire\",\"findings\":{"
              "\"gap\":10,\"rate-drop\":20,\"late-id\":30,\"overflow\":40,"
              "\"isotp\":50,\"link\":60,\"asleep\":70},\"gap_us\":1000000,"
              "\"rate_drop\":4.5,\"settle_us\":2000000}");

    cli::Line f(buf, sizeof buf);
    cli::open_failed_line(f, 0, ECUIO_ERR_LOAD_FAILED,
                          "the folder has no signals.csv");
    EXPECT_EQ(std::string(f.data(), f.size()),
              std::string("{\"type\":\"open_failed\",\"recording\":0,"
                          "\"code\":7,\"message\":\"") +
                  ecuio_status_string(ECUIO_ERR_LOAD_FAILED) +
                  "\",\"detail\":\"the folder has no signals.csv\"}");
}

// A URI's profile= is the last one in its query, as core reads it, and
// only that key: one that ends in profile isn't it
TEST(CliSignalFileTest, AUrisProfileIsItsLastProfileKey)
{
    std::string p;
    EXPECT_FALSE(cli::uri_profile("mock://x", &p));
    EXPECT_FALSE(cli::uri_profile("mock://x?myprofile=a", &p));
    ASSERT_TRUE(cli::uri_profile("mock://x?profile=obd2", &p));
    EXPECT_EQ(p, "obd2");
    ASSERT_TRUE(
        cli::uri_profile("replay://a?speed=max&profile=a&profile=b", &p));
    EXPECT_EQ(p, "b");
    ASSERT_TRUE(cli::uri_profile("mock://x?profile=", &p));
    EXPECT_EQ(p, "");
}

// A loaded file's profile is added to a URI that names none, kept where
// the URI names it, and a URI naming another is refused
TEST(CliSignalFileTest, TheFilesProfileIsTheUrisWhenItNamesNone)
{
    std::string out;
    ASSERT_TRUE(cli::uri_for_profile("mock://x", "user.a", &out));
    EXPECT_EQ(out, "mock://x?profile=user.a");
    ASSERT_TRUE(cli::uri_for_profile("replay://p?speed=max", "user.a", &out));
    EXPECT_EQ(out, "replay://p?speed=max&profile=user.a");
    ASSERT_TRUE(
        cli::uri_for_profile("mock://x?profile=user.a", "user.a", &out));
    EXPECT_EQ(out, "mock://x?profile=user.a");
    EXPECT_FALSE(cli::uri_for_profile("mock://x?profile=obd2", "user.a", &out));
    EXPECT_FALSE(cli::uri_for_profile("mock://x?profile=", "user.a", &out));
}

// The kind words are a signal file's, and a unit line is a file's own
// signal's unit that never converts
TEST(CliSignalFileTest, KindsAndUnitsAreTheFilesWords)
{
    ecuio_signal_def_t d{};
    d.kind = ECUIO_VALUE_UINT;
    EXPECT_STREQ(cli::kind_word(d), "number");
    d.kind = ECUIO_VALUE_INT;
    EXPECT_STREQ(cli::kind_word(d), "number");
    d.kind = ECUIO_VALUE_TEXT;
    EXPECT_STREQ(cli::kind_word(d), "text");
    d.kind = ECUIO_VALUE_HEX;
    EXPECT_STREQ(cli::kind_word(d), "hex");
    d.kind = ECUIO_VALUE_ENUM;
    EXPECT_STREQ(cli::kind_word(d), "enum");
    d.kind = ECUIO_VALUE_UINT;
    d.flags = ECUIO_DEF_FLAG_COMPUTED;
    EXPECT_STREQ(cli::kind_word(d), "computed");

    ecuio_signal_def_t u{};
    std::strcpy(u.unit, "km/h");
    EXPECT_TRUE(cli::unit_converts(u));
    EXPECT_FALSE(cli::unit_kept(u, true));
    std::strcpy(u.unit, "kph");
    EXPECT_FALSE(cli::unit_converts(u));
    EXPECT_TRUE(cli::unit_kept(u, true));
    std::strcpy(u.unit, "%");
    EXPECT_TRUE(cli::unit_kept(u, true));
    u.unit[0] = '\0';
    EXPECT_FALSE(cli::unit_kept(u, true)) << "no unit, nothing to convert";
    std::strcpy(u.unit, "%");
    EXPECT_FALSE(cli::unit_kept(u, false)) << "the base set's, not the file's";
}

// Each line says what the loader read, in the file's own terms: a reply's
// echo split back into service and number, the byte after it, the field,
// an enum's labels, a computed signal's inputs with the stale age where
// the file gives none
TEST(CliSignalFileTest, TheLinesSayWhatTheLoaderRead)
{
    const std::string json =
        "{\"havne_signals\":1,\"name\":\"Unit test\","
        "\"source\":{\"name\":\"test\"},"
        "\"ecus\":[{\"id\":\"tcm\",\"rx\":\"7E9\"}],\"signals\":["
        "{\"key\":\"temp\",\"name\":\"Temp\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},"
        "\"field\":{\"byte\":1,\"bits\":16,\"signed\":true},"
        "\"scale\":0.5,\"offset\":-40,\"unit\":\"kph\",\"decimals\":1,"
        "\"group\":\"engine\",\"plot\":2},"
        "{\"key\":\"gear\",\"name\":\"Gear\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"01\",\"number\":\"A4\"},"
        "\"field\":{\"byte\":0,\"bits\":4,\"bit\":2,\"order\":\"le\"},"
        "\"kind\":\"enum\",\"values\":{\"1\":\"First\",\"2\":\"Second\"},"
        "\"group\":\"vehicle\"},"
        "{\"key\":\"part\",\"name\":\"Part\",\"ecu\":\"tcm\","
        "\"read\":{\"service\":\"22\",\"number\":\"F187\"},"
        "\"field\":{\"byte\":0,\"bytes\":5},\"kind\":\"hex\"},"
        "{\"key\":\"half\",\"name\":\"Half\",\"kind\":\"computed\","
        "\"expr\":\"temp / 2\",\"unit\":\"kph\"}]}";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    ASSERT_EQ(set.count, 4u);
    char buf[4096];
    auto text = [&](auto write) {
        cli::Line l(buf, sizeof buf);
        write(l);
        return std::string(l.data(), l.size());
    };
    EXPECT_EQ(text([&](cli::Line& l) {
                  cli::signal_file_line(l, file, set);
              }),
              "{\"type\":\"signal_file\",\"name\":\"Unit test\","
              "\"profile\":\"user.unit-test\",\"signals\":4,\"base\":0,"
              "\"ecus\":[{\"rx\":\"0x7E9\",\"tx\":\"0x7E1\"}]}");
    const std::string other = ecuio_signal_group_name(ECUIO_SIGNAL_GROUP_OTHER);
    const std::string expected[] = {
        "{\"type\":\"signal_def\",\"id\":30000,\"name\":\"Temp\","
        "\"kind\":\"number\",\"unit\":\"kph\",\"decimals\":1,"
        "\"group\":\"Engine\",\"rank\":2,\"rx\":\"0x7E9\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},\"byte\":1,"
        "\"bits\":16,\"order\":\"be\",\"signed\":true,\"scale\":0.5,"
        "\"offset\":-40}",
        "{\"type\":\"signal_def\",\"id\":30001,\"name\":\"Gear\","
        "\"kind\":\"enum\",\"unit\":\"\",\"decimals\":0,"
        "\"group\":\"Vehicle\",\"rank\":0,\"rx\":\"0x7E9\","
        "\"read\":{\"service\":\"01\",\"number\":\"A4\"},\"byte\":0,"
        "\"bits\":4,\"bit\":2,\"order\":\"le\",\"signed\":false,"
        "\"labels\":{\"1\":\"First\",\"2\":\"Second\"}}",
        "{\"type\":\"signal_def\",\"id\":30002,\"name\":\"Part\","
        "\"kind\":\"hex\",\"unit\":\"\",\"decimals\":0,\"group\":\"" +
            other +
            "\",\"rank\":0,\"rx\":\"0x7E9\","
            "\"read\":{\"service\":\"22\",\"number\":\"F187\"},\"byte\":0,"
            "\"bytes\":5}",
        "{\"type\":\"signal_def\",\"id\":30003,\"name\":\"Half\","
        "\"kind\":\"computed\",\"unit\":\"kph\",\"decimals\":0,\"group\":\"" +
            other +
            "\",\"rank\":0,\"inputs\":[{\"id\":30000,\"max_age_us\":3000000}]}",
    };
    for (uint32_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_TRUE(cli::own_signal(file, set.defs[i]));
        EXPECT_LE(expected[i].size(), cli::def_line_bytes(set, set.defs[i]));
        EXPECT_EQ(text([&](cli::Line& l) {
                      cli::def_line(l, set, set.defs[i], nullptr);
                  }),
                  expected[i]);
    }
    EXPECT_EQ(text([&](cli::Line& l) { cli::unit_line(l, set.defs[0]); }),
              "{\"type\":\"unit\",\"id\":30000,\"name\":\"Temp\","
              "\"unit\":\"kph\",\"converts\":false}");
    uint32_t n = 0;
    EXPECT_EQ(cli::def_labels(set, set.defs[0], &n), nullptr);
    EXPECT_EQ(n, 0u) << "a number has no labels";
    ASSERT_NE(cli::def_labels(set, set.defs[1], &n), nullptr);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(cli::def_program(set, set.defs[0]), nullptr);
    EXPECT_NE(cli::def_program(set, set.defs[3]), nullptr);
    ecuio_signal_file_free(file);
}

// A def's gauge field: nothing for a def with none, else min and max and
// only the levels given (cli::gauge_field)
// A def that replaced a base one by name has the base's ID (7002 here), so
// the ID range no longer says whose it is: the file's key does, and the
// line and the count follow it
TEST(CliSignalFileTest, AReplacingDefIsTheFilesOwnWhateverItsId)
{
    const std::string json =
        "{\"havne_signals\":1,\"name\":\"Replaces\","
        "\"source\":{\"name\":\"test\"},\"extends\":\"obd2\",\"signals\":["
        "{\"key\":\"temp\",\"name\":\"Coolant Temp\","
        "\"read\":{\"service\":\"22\",\"number\":\"1E1C\"},"
        "\"field\":{\"byte\":1},\"unit\":\"kph\"}]}";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    const ecuio_signal_def_t* temp = ecuio_signal_file_find(file, "temp");
    ASSERT_NE(temp, nullptr);
    ASSERT_LT(temp->id, ECUIO_USER_SIG_ID_BASE) << "the base def's ID";
    EXPECT_TRUE(cli::own_signal(file, *temp));
    for (uint32_t i = 0; i < set.count; ++i) {
        if (&set.defs[i] != temp) {
            EXPECT_FALSE(cli::own_signal(file, set.defs[i])) << set.defs[i].name;
        }
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::signal_file_line(l, file, set);
    const std::string line(l.data(), l.size());
    EXPECT_NE(line.find("\"signals\":1,"), std::string::npos) << line;
    ecuio_signal_file_free(file);
}

TEST(CliSignalFileTest, AGaugeFieldHoldsOnlyTheLevelsGiven)
{
    const auto field = [](const ecuio_signal_gauge_t* g) {
        char buf[cli::kGaugeBytes + 16];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"id\":1");
        cli::gauge_field(l, g);
        l.ch('}');
        return std::string(l.data(), l.size());
    };
    EXPECT_EQ(field(nullptr), "{\"id\":1}");
    ecuio_signal_gauge_t g{};
    g.min = -40;
    g.max = 130.5;
    g.warn_low = 60;
    g.warn_high = 110;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5}}")
        << "a level whose flag is clear isn't given";
    g.flags = ECUIO_GAUGE_WARN_LOW;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5,"
                         "\"warn_low\":60}}");
    g.flags = ECUIO_GAUGE_WARN_HIGH;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5,"
                         "\"warn_high\":110}}");
    g.flags = ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH;
    EXPECT_EQ(field(&g), "{\"id\":1,\"gauge\":{\"min\":-40,\"max\":130.5,"
                         "\"warn_low\":60,\"warn_high\":110}}");
}

// An import's notes and summary are lines of their own, each kind a word
TEST(CliSignalFileTest, AnImportsNotesAreLinesOfTheirOwn)
{
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_CONVERTED), "converted");
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_CHANGED), "changed");
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_SKIPPED), "skipped");
    EXPECT_STREQ(cli::note_word(ECUIO_OBDB_NOTE_FILE), "file");
    EXPECT_STREQ(cli::note_word(9), "unknown");
    ecuio_obdb_note_t n{};
    n.what = ECUIO_OBDB_NOTE_CHANGED;
    n.line = 12;
    n.signal = "TCM_TFT";
    n.key = "tcm_tft";
    n.field = "max";
    n.text = "OBDb holds the value within 0 to 215";
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::import_note_line(l, n);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"import_note\",\"what\":\"changed\",\"line\":12,"
              "\"signal\":\"TCM_TFT\",\"key\":\"tcm_tft\",\"field\":\"max\","
              "\"text\":\"OBDb holds the value within 0 to 215\"}");
    EXPECT_LE(l.size(), cli::note_line_bytes(n));
    cli::Line s(buf, sizeof buf);
    cli::imported_line(s, 7, 6, 5);
    EXPECT_EQ(std::string(s.data(), s.size()),
              "{\"type\":\"imported\",\"converted\":7,\"changed\":6,"
              "\"skipped\":5}");
}

namespace {

std::string text_of(cli::Line& l)
{
    EXPECT_TRUE(l.finish());
    return std::string(l.data(), l.size());
}

// A packed plugin of two variants, the second for 2019 on
const char kPacked[] =
    R"({"havne_plugin": 1, "id": "p", "name": "P", "version": "1.2.3",)"
    R"( "requires": {"core": "0.1.0"}, "source": {"name": "t"},)"
    R"( "tiers": ["data"], "match": [{"wmi": ["ZZZ"]}],)"
    R"( "ecus": [{"id": "pcm", "rx": "7E8"}],)"
    R"( "variants": [{"id": "base", "files": ["a.json"]},)"
    R"( {"id": "late", "years": [2019, 2030], "files": ["a.json"]}],)"
    R"( "files": {"a.json": {"havne_signals": 1, "signals": [{"key": "oil",)"
    R"( "name": "Oil", "ecu": "pcm", "read": {"service": "22",)"
    R"( "number": "F40D"}, "field": {"byte": 0}, "scale": 0.25}]}}})";

struct Packed {
    ecuio_plugin_t* p = nullptr;
    Packed()
    {
        EXPECT_EQ(ecuio_plugin_load(kPacked, sizeof kPacked - 1, nullptr,
                                    nullptr, &p),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Packed() { ecuio_plugin_free(p); }
    ecuio_plugin_info_t info() const
    {
        ecuio_plugin_info_t i{};
        i.struct_size = sizeof i;
        ecuio_plugin_info(p, &i);
        return i;
    }
};

} // namespace

// A fixture passes within half its field's step, an enum's step being 1,
// with only the double's own rounding as slack
TEST(CliPluginTest, AFixturePassesWithinHalfItsFieldsStep)
{
    ecuio_signal_def_t d{};
    d.kind = ECUIO_VALUE_UINT;
    d.scale = 0.1;
    EXPECT_TRUE(cli::fixture_passes(d, 81.3, 81.3));
    EXPECT_TRUE(cli::fixture_passes(d, 81.3, 81.35));
    EXPECT_TRUE(cli::fixture_passes(d, 81.3, 81.25));
    EXPECT_FALSE(cli::fixture_passes(d, 81.3, 81.36));
    EXPECT_FALSE(cli::fixture_passes(d, 81.3, 81.2));
    d.scale = -2;
    EXPECT_TRUE(cli::fixture_passes(d, 10, 11)) << "the step's size, |scale|";
    EXPECT_FALSE(cli::fixture_passes(d, 10, 11.01));
    d.kind = ECUIO_VALUE_ENUM;
    d.scale = 1;
    EXPECT_TRUE(cli::fixture_passes(d, 3, 3));
    EXPECT_FALSE(cli::fixture_passes(d, 3, 4));
    EXPECT_FALSE(cli::fixture_passes(d, 3, 2));
    EXPECT_EQ(cli::plugin_profile("example-sim", "my2019"),
              "plugin.example-sim.my2019");
}

// The plugin line names each variant's profile; the match line the
// variants proposed, and a VIN with no model year's null
TEST(CliPluginTest, ThePluginAndMatchLinesNameTheProfiles)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    char buf[2048];
    cli::Line l(buf, sizeof buf);
    cli::plugin_line(l, 2, pk.p, pk.info());
    EXPECT_EQ(text_of(l),
              "{\"type\":\"plugin\",\"package\":2,\"id\":\"p\",\"version\":"
              "\"1.2.3\",\"name\":\"P\",\"tiers\":[\"data\"],\"variants\":["
              "{\"id\":\"base\",\"profile\":\"plugin.p.base\"},{\"id\":"
              "\"late\",\"profile\":\"plugin.p.late\"}],\"fixtures\":0}\n");

    ecuio_vin_info_t vin{};
    std::strcpy(vin.wmi, "ZZZ");
    vin.model_year = 2018;
    uint32_t idx[4] = {};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_plugin_match(pk.p, &vin, idx, 4, &n), ECUIO_OK);
    cli::Line m(buf, sizeof buf);
    cli::match_line(m, pk.p, pk.info(), vin, idx, n);
    EXPECT_EQ(text_of(m), "{\"type\":\"match\",\"wmi\":\"ZZZ\",\"model_year\":"
                          "2018,\"variants\":[{\"id\":\"base\",\"profile\":"
                          "\"plugin.p.base\"}]}\n");
    vin.model_year = 0;
    ASSERT_EQ(ecuio_plugin_match(pk.p, &vin, idx, 4, &n), ECUIO_OK);
    cli::Line z(buf, sizeof buf);
    cli::match_line(z, pk.p, pk.info(), vin, idx, n);
    EXPECT_NE(text_of(z).find("\"model_year\":null"), std::string::npos);
}

// A variant line counts its own signals apart from its base set's, and
// lists its ECUs
TEST(CliPluginTest, AVariantLineCountsItsOwnSignals)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(pk.p, 0);
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::variant_line(l, "base", "plugin.p.base",
                      ecuio_plugin_variant_file(pk.p, 0), set);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"variant\",\"id\":\"base\",\"profile\":"
              "\"plugin.p.base\",\"signals\":1,\"base\":0,\"ecus\":[{\"rx\":"
              "\"0x7E8\",\"tx\":\"0x7E0\"}]}\n");
}

// A fixture line: its place, the value and tolerance, the result, what
// came back (null with nothing) and why when it didn't pass
TEST(CliPluginTest, AFixtureLineSaysWhatCameBack)
{
    Packed pk;
    ASSERT_NE(pk.p, nullptr);
    const char file[] =
        R"({"havne_fixtures": 1, "values": [{"variant": "late", "signal": "oil", "value": 2.5}]})";
    ecuio_plugin_value_t v{};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_plugin_values(pk.p, file, sizeof file - 1, &v, 1, &n),
              ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    cli::Line a(buf, sizeof buf);
    cli::fixture_line(a, "f.json", "late", v, cli::FixtureResult::passed, true,
                      2.5, nullptr);
    EXPECT_EQ(text_of(a),
              "{\"type\":\"fixture\",\"file\":\"f.json\",\"line\":1,"
              "\"variant\":\"late\",\"signal\":\"oil\",\"value\":2.5,"
              "\"tolerance\":0.125,\"result\":\"passed\",\"got\":2.5}\n");
    cli::Line b(buf, sizeof buf);
    cli::fixture_line(b, "f.json", "late", v, cli::FixtureResult::not_run,
                      false, 0, "no --on");
    EXPECT_EQ(text_of(b),
              "{\"type\":\"fixture\",\"file\":\"f.json\",\"line\":1,"
              "\"variant\":\"late\",\"signal\":\"oil\",\"value\":2.5,"
              "\"tolerance\":0.125,\"result\":\"not-run\",\"got\":null,"
              "\"why\":\"no --on\"}\n");
    EXPECT_STREQ(cli::fixture_word(cli::FixtureResult::failed), "failed");
}

// The checked line counts each result, names the interface or null, and
// warns of a plugin with no fixtures only; a refusal names its package or
// its file
TEST(CliPluginTest, TheCheckedAndRefusedLinesSayWhatHappened)
{
    cli::FixtureCounts c;
    c.passed = 6;
    c.failed = 1;
    c.files_refused = 1;
    char buf[1024];
    cli::Line a(buf, sizeof buf);
    cli::checked_line(a, c, "vcan0", 2);
    EXPECT_EQ(text_of(a),
              "{\"type\":\"checked\",\"fixtures\":7,\"passed\":6,\"failed\":1,"
              "\"not_run\":0,\"files_refused\":1,\"on\":\"vcan0\"}\n");
    cli::Line b(buf, sizeof buf);
    cli::checked_line(b, cli::FixtureCounts{}, nullptr, 0);
    EXPECT_EQ(text_of(b),
              "{\"type\":\"checked\",\"fixtures\":0,\"passed\":0,\"failed\":0,"
              "\"not_run\":0,\"files_refused\":0,\"on\":null,\"warning\":"
              "\"the plugin has no fixtures: its values are unchecked\"}\n");
    cli::Line r(buf, sizeof buf);
    cli::refused_line(r, "refused", nullptr, 3, ECUIO_ERR_LOAD_FAILED, "why");
    EXPECT_NE(text_of(r).find("\"package\":3,\"code\":7"), std::string::npos);
    cli::Line f(buf, sizeof buf);
    cli::refused_line(f, "fixture_refused", "a.json", 0,
                      ECUIO_ERR_INVALID_ARGUMENT, "why");
    const std::string ft = text_of(f);
    EXPECT_NE(ft.find("\"file\":\"a.json\",\"code\":11"), std::string::npos);
    EXPECT_EQ(ft.find("package"), std::string::npos);
}

// A plugin whose data files use "gauge" is warned of a requires.core
// below 0.7.2 and of no other; one with no gauge never is
TEST(CliPluginTest, AGaugeWantsTheCoreThatReadsIt)
{
    struct Version {
        uint16_t part[3];
    };
    const auto older = [](Version v, bool uses_gauge) {
        return cli::gauge_needs_newer_core(v.part, uses_gauge);
    };
    EXPECT_FALSE(older({{0, 7, 2}}, true));
    EXPECT_FALSE(older({{0, 7, 3}}, true));
    EXPECT_FALSE(older({{0, 8, 0}}, true));
    EXPECT_FALSE(older({{1, 0, 0}}, true));
    EXPECT_TRUE(older({{0, 7, 1}}, true));
    EXPECT_TRUE(older({{0, 6, 9}}, true));
    EXPECT_TRUE(older({{0, 1, 0}}, true));
    EXPECT_FALSE(older({{0, 1, 0}}, false));

    char buf[1024];
    cli::Line l(buf, sizeof buf);
    const uint16_t declared[3] = {0, 7, 1};
    cli::requires_core_line(l, declared);
    EXPECT_EQ(text_of(l),
              "{\"type\":\"requires_core\",\"declared\":\"0.7.1\",\"needs\":"
              "\"0.7.2\",\"warning\":\"the data files use \\\"gauge\\\", which "
              "core reads from 0.7.2 on: raise the manifest's \\\"requires\\\" "
              "core to 0.7.2, so an older Havne names that version instead of "
              "refusing a signal file\"}\n");
}

// ---- A replay's own signal file (cli/recording_signals.h, ADR-0051
// section 4) ----

/* The header's profile and signal file lines, up to its blank line; a
 * hash that isn't 64 lowercase hex digits names no file */
TEST(CliRecordingSignalsTest, TheHeaderNamesTheProfileAndTheFile)
{
    const std::string sha(64, 'a');
    cli::RecordedSignals r;
    EXPECT_TRUE(r.take("ecuio session recording, format 3 (ADR-0049)"));
    EXPECT_TRUE(r.take("profile  user.extras\r"));
    EXPECT_TRUE(r.take("signal  A,number,,0,0,0"));
    EXPECT_TRUE(r.take("signal file  signals.json," + sha));
    EXPECT_FALSE(r.take(""));
    EXPECT_FALSE(r.take("profile  after.the.header"));
    EXPECT_EQ(r.profile, "user.extras");
    EXPECT_EQ(r.sha256, sha);
    EXPECT_TRUE(r.names_file());

    cli::RecordedSignals upper;
    upper.take("profile  user.extras");
    upper.take("signal file  signals.json," + std::string(64, 'A'));
    EXPECT_FALSE(upper.names_file()) << "the recorder writes lower case";
    cli::RecordedSignals short_hash;
    short_hash.take("profile  user.extras");
    short_hash.take("signal file  signals.json," + std::string(63, 'a'));
    EXPECT_FALSE(short_hash.names_file());
    cli::RecordedSignals none;
    none.take("profile  obd2");
    EXPECT_FALSE(none.names_file());
}

/* The folder: the path up to the query, a frames.csv's own folder, any
 * scheme's case; another scheme none */
TEST(CliRecordingSignalsTest, AReplayUrisFolder)
{
    std::string folder;
    ASSERT_TRUE(
        cli::replay_folder("replay://C:/r/20261106-0900?speed=max", &folder));
    EXPECT_EQ(folder, "C:/r/20261106-0900");
    ASSERT_TRUE(cli::replay_folder("REPLAY:///data/r/frames.csv", &folder));
    EXPECT_EQ(folder, "/data/r");
    // Another .csv is a frames file of its own: its folder's events.log
    // and signals.json aren't its, as core's source_frame_flags says
    EXPECT_FALSE(cli::replay_folder("replay:///data/r/other.csv", &folder));
    EXPECT_FALSE(cli::replay_folder("replay:///data/r/frames.CSV", &folder));
    EXPECT_FALSE(cli::replay_folder("replay://frames.csv", &folder));
    ASSERT_TRUE(cli::replay_folder("replay:///data/r/", &folder));
    EXPECT_EQ(folder, "/data/r");
    EXPECT_FALSE(cli::replay_folder("mock://demo", &folder));
    EXPECT_FALSE(cli::replay_folder("replay://", &folder));
    EXPECT_FALSE(cli::replay_folder("replay://?profile=obd2", &folder));
}

/* One profile=, the rest of the query kept in order; the copy's own
 * name from the hash */
TEST(CliRecordingSignalsTest, TheUriWithAnotherProfile)
{
    EXPECT_EQ(cli::with_profile("replay://f", "rec.0123"),
              "replay://f?profile=rec.0123");
    EXPECT_EQ(cli::with_profile("replay://f?profile=user.a&speed=max", "rec.1"),
              "replay://f?speed=max&profile=rec.1");
    EXPECT_EQ(
        cli::with_profile("replay://f?speed=2&profile=a&loop=1&profile=b", "c"),
        "replay://f?speed=2&loop=1&profile=c");
    // An empty pair is kept, so core refuses the query as it would have
    EXPECT_EQ(cli::with_profile("replay://f?", "c"), "replay://f?&profile=c");
    EXPECT_EQ(cli::with_profile("replay://f?speed=2&&loop=1", "c"),
              "replay://f?speed=2&&loop=1&profile=c");
    EXPECT_EQ(cli::with_profile("replay://f?profile=a", "c"),
              "replay://f?profile=c");
    EXPECT_EQ(cli::own_profile(std::string(16, '0') + std::string(48, 'f')),
              "rec.0000000000000000");
}

// ---- The signals an ECU refuses (cli/refused_signals.h, ADR-0051) ----

namespace {

// A def keyed at the reply's start: key's bytes, first most significant
ecuio_signal_def_t keyed(uint16_t id, uint32_t rx, uint8_t length, uint32_t key)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_offset = 0;
    d.mux_length = length;
    d.mux_key = key;
    return d;
}

ecuio_dropped_request_t dropped(uint8_t service, uint16_t key, uint8_t length,
                                uint8_t reason, uint8_t nrc)
{
    ecuio_dropped_request_t d{};
    d.request[0] = service;
    if (length == 2) {
        d.request[1] = static_cast<uint8_t>(key);
    } else {
        d.request[1] = static_cast<uint8_t>(key >> 8);
        d.request[2] = static_cast<uint8_t>(key);
    }
    d.length = length;
    d.reason = reason;
    d.nrc = nrc;
    return d;
}

ecuio_channel_support_t
report(uint32_t rx, std::initializer_list<ecuio_dropped_request_t> drops)
{
    ecuio_channel_support_t s{};
    s.rx_id = rx;
    s.generation = 1;
    for (const ecuio_dropped_request_t& d : drops) {
        s.dropped_list[s.dropped++] = d;
    }
    return s;
}

std::vector<uint16_t> ids_of(const std::vector<cli::RefusedSignal>& r)
{
    std::vector<uint16_t> ids;
    for (const cli::RefusedSignal& s : r) {
        ids.push_back(s.id);
    }
    return ids;
}

} // namespace

/* A refused request leaves its own signals without values: the defs on
 * its channel's reply ID keyed by its reply (62 <DID>, 41 <PID>), each
 * once, in the set's order; another ECU's, another key's, an unkeyed or
 * a computed def keep theirs, and a PID its bitmap left out is
 * unsupported, not refused */
TEST(CliRefusedTest, ARefusedRequestIsItsOwnSignalsAlone)
{
    std::vector<ecuio_signal_def_t> defs = {
        keyed(1, 0x7E8, 3, 0x62F1FE), // refused
        keyed(2, 0x7E8, 3, 0x621E1C), // answered
        keyed(3, 0x7E9, 3, 0x62F1FE), // the same DID on another ECU
        keyed(4, 0x7E8, 2, 0x410C),   // refused by its bitmap
        keyed(5, 0x7E8, 0, 0),        // unkeyed on the ID
        keyed(6, 0x7E8, 3, 0x62F1FE), // a second field of the refused reply
        keyed(7, 0x7E8, 2, 0x410D),   // refused by a negative reply
        keyed(8, 0x7E8, 3, 0x62F1FF), // refused for its access
    };
    ecuio_signal_def_t computed = keyed(9, 0, 0, 0);
    computed.flags = ECUIO_DEF_FLAG_COMPUTED;
    defs.push_back(computed);
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(defs.size());
    set.defs = defs.data();
    const ecuio_channel_support_t reports[2] = {
        report(0x7E8, {dropped(0x22, 0xF1FE, 3, ECUIO_DROP_NOT_SUPPORTED, 0x31),
                       dropped(0x01, 0x0C, 2, ECUIO_DROP_BITMAP, 0),
                       dropped(0x01, 0x0D, 2, ECUIO_DROP_SERVICE, 0x11),
                       dropped(0x22, 0xF1FF, 3, ECUIO_DROP_ACCESS, 0x33)}),
        report(0x7E9, {}),
    };
    const std::vector<cli::RefusedSignal> r =
        cli::refused_signals(&set, reports, 2);
    EXPECT_EQ(ids_of(r), (std::vector<uint16_t>{1, 6, 7, 8}));
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0].request[0], 0x22);
    EXPECT_EQ(r[0].request[1], 0xF1);
    EXPECT_EQ(r[0].request[2], 0xFE);
    EXPECT_EQ(r[0].length, 3);
    EXPECT_EQ(r[0].nrc, 0x31);
    EXPECT_EQ(r[2].nrc, 0x11);
    EXPECT_EQ(r[3].nrc, 0x33);
    EXPECT_EQ(ids_of(cli::refused_signals(&set, reports, 1)),
              (std::vector<uint16_t>{1, 6, 7, 8}))
        << "the channels counted";
    EXPECT_EQ(ids_of(cli::refused_signals(&set, reports + 1, 1)),
              std::vector<uint16_t>{})
        << "the other ECU refused nothing";
}

/* A report that has learned nothing refuses nothing, only the drops the
 * report lists count, and no set or no reports refuse nothing */
TEST(CliRefusedTest, OnlyALearnedReportsListedDropsRefuse)
{
    std::vector<ecuio_signal_def_t> defs;
    for (uint16_t did = 0; did < 10; ++did) {
        defs.push_back(
            keyed(static_cast<uint16_t>(100 + did), 0x7E8, 3, 0x62F100u + did));
    }
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(defs.size());
    set.defs = defs.data();
    ecuio_channel_support_t s = report(0x7E8, {});
    for (uint16_t did = 0; did < ECUIO_SUPPORT_MAX_DROPPED; ++did) {
        s.dropped_list[did] = dropped(0x22, static_cast<uint16_t>(0xF100 + did),
                                      3, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    }
    s.dropped = 10; // two past the list: core keeps the first eight
    EXPECT_EQ(cli::refused_signals(&set, &s, 1).size(),
              size_t{ECUIO_SUPPORT_MAX_DROPPED});
    s.dropped = 3;
    EXPECT_EQ(ids_of(cli::refused_signals(&set, &s, 1)),
              (std::vector<uint16_t>{100, 101, 102}))
        << "a listed slot past the count isn't a drop";
    s.generation = 0;
    EXPECT_TRUE(cli::refused_signals(&set, &s, 1).empty());
    s.generation = 1;
    EXPECT_TRUE(cli::refused_signals(nullptr, &s, 1).empty());
    EXPECT_TRUE(cli::refused_signals(&set, nullptr, 1).empty());
}

/* The refused line: each signal's id, its request and the reply in
 * frames.csv's form, the code by name, null for one with none; an
 * emptied list is [] */
TEST(CliRefusedTest, TheRefusedLineNamesEachSignalsReply)
{
    cli::RefusedSignal a;
    a.id = 30001;
    a.request[0] = 0x22;
    a.request[1] = 0xF1;
    a.request[2] = 0xFE;
    a.length = 3;
    a.nrc = 0x31;
    cli::RefusedSignal b = a;
    b.id = 12;
    b.request[0] = 0x01;
    b.request[1] = 0x0D;
    b.length = 2;
    b.nrc = 0x99;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::refused_line(l, {a, b});
    EXPECT_EQ(
        std::string(l.data(), l.size()),
        "{\"type\":\"refused\",\"signals\":[{\"id\":30001,\"request\":\"22 F1 "
        "FE\","
        "\"reply\":\"7F 22 31\",\"nrc\":\"requestOutOfRange\"},{\"id\":12,"
        "\"request\":\"01 0D\",\"reply\":\"7F 01 99\",\"nrc\":null}]}");
    cli::Line e(buf, sizeof buf);
    cli::refused_line(e, {});
    EXPECT_EQ(std::string(e.data(), e.size()),
              "{\"type\":\"refused\",\"signals\":[]}");
    EXPECT_LE(std::string(l.data(), l.size()).size(),
              cli::refused_line_bytes(2));
}

/* Past a report's list, refused_unlisted counts the refusals: the drops
 * past the list less the requests the bitmaps left out (one a Service 01
 * PID among the set's defs on the channel, a PID's second def not
 * counted again). A list that ends in a bitmap drop, or has room, named
 * every refusal; a report that learned nothing counts none. */
TEST(CliRefusedTest, RefusalsPastTheListAreCounted)
{
    std::vector<ecuio_signal_def_t> defs;
    for (uint16_t did = 0; did < 12; ++did) {
        defs.push_back(
            keyed(static_cast<uint16_t>(100 + did), 0x7E8, 3, 0x62F100u + did));
    }
    defs.push_back(keyed(200, 0x7E8, 2, 0x4104)); // left out by its bitmap
    defs.push_back(keyed(201, 0x7E8, 2, 0x4104)); // its second field
    defs.push_back(keyed(202, 0x7E8, 2, 0x4105)); // left out by its bitmap
    defs.push_back(keyed(203, 0x7E8, 2, 0x410C)); // supported
    defs.push_back(keyed(204, 0x7E8, 2, 0x4161)); // its bitmap unanswered
    defs.push_back(keyed(205, 0x7E9, 2, 0x4106)); // another ECU's
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(defs.size());
    set.defs = defs.data();

    ecuio_channel_support_t s = report(0x7E8, {});
    s.ranges = 0x1; // the bitmap at 0x00 answered: PIDs 0x01 to 0x20
    s.pid_bitmap[(0x0C - 1) / 8] |= 0x80u >> ((0x0C - 1) % 8);
    for (uint16_t did = 0; did < ECUIO_SUPPORT_MAX_DROPPED; ++did) {
        s.dropped_list[did] = dropped(0x22, static_cast<uint16_t>(0xF100 + did),
                                      3, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    }
    // the twelve DIDs refused and two PIDs left out: four refusals past
    s.dropped = 12 + 2;
    std::vector<cli::RefusedMore> more = cli::refused_unlisted(&set, &s, 1);
    ASSERT_EQ(more.size(), 1u);
    EXPECT_EQ(more[0].rx_id, 0x7E8u);
    EXPECT_EQ(more[0].count, 4u);

    s.dropped = 8 + 2; // only the bitmap's two past the list
    EXPECT_TRUE(cli::refused_unlisted(&set, &s, 1).empty());
    s.dropped = 12 + 2;
    s.dropped_list[ECUIO_SUPPORT_MAX_DROPPED - 1] =
        dropped(0x01, 0x04, 2, ECUIO_DROP_BITMAP, 0);
    EXPECT_TRUE(cli::refused_unlisted(&set, &s, 1).empty())
        << "a list ending in a bitmap drop named every refusal";
    s.dropped_list[ECUIO_SUPPORT_MAX_DROPPED - 1] =
        dropped(0x22, 0xF107, 3, ECUIO_DROP_ACCESS, 0x33);
    s.generation = 0;
    EXPECT_TRUE(cli::refused_unlisted(&set, &s, 1).empty());
    s.generation = 1;
    EXPECT_TRUE(cli::refused_unlisted(nullptr, &s, 1).empty());
    EXPECT_EQ(cli::refused_unlisted(&set, &s, 1).size(), 1u);

    cli::RefusedSignal a;
    a.id = 100;
    a.request[0] = 0x22;
    a.request[1] = 0xF1;
    a.request[2] = 0x00;
    a.length = 3;
    a.nrc = 0x31;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::refused_line(l, {a}, more);
    const std::string line(l.data(), l.size());
    EXPECT_EQ(line,
              "{\"type\":\"refused\",\"signals\":[{\"id\":100,\"request\":"
              "\"22 F1 00\",\"reply\":\"7F 22 31\",\"nrc\":"
              "\"requestOutOfRange\"}],\"more\":[{\"ecu\":\"0x7E8\","
              "\"refused\":4}]}");
    EXPECT_LE(line.size(), cli::refused_line_bytes(1, 1));
}

/* The code's names, ISO 14229-1's, and "" past them */
TEST(CliRefusedTest, ANegativeCodeHasItsStandardName)
{
    EXPECT_STREQ(cli::nrc_name(0x11), "serviceNotSupported");
    EXPECT_STREQ(cli::nrc_name(0x12), "subFunctionNotSupported");
    EXPECT_STREQ(cli::nrc_name(0x22), "conditionsNotCorrect");
    EXPECT_STREQ(cli::nrc_name(0x31), "requestOutOfRange");
    EXPECT_STREQ(cli::nrc_name(0x33), "securityAccessDenied");
    EXPECT_STREQ(cli::nrc_name(0x36), "exceedNumberOfAttempts");
    EXPECT_STREQ(cli::nrc_name(0x7F), "serviceNotSupportedInActiveSession");
    EXPECT_STREQ(cli::nrc_name(0x00), "");
    EXPECT_STREQ(cli::nrc_name(0x99), "");
}

/* ---------- havne-cli clear (ADR-0038's appendix of 2026-11-06) ---------- */

namespace {

std::string clear_text(const ecuio_channel_diagnostics_t& d,
                       const char* word = nullptr)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::clear_line(l, d, word);
    return std::string(l.data(), l.size());
}

/* A mock session started, destroyed at the end */
struct MockSession {
    ecuio_session_t* s = nullptr;
    explicit MockSession(const char* uri)
    {
        EXPECT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    }
    ~MockSession() { ecuio_session_destroy(s); }
    MockSession(const MockSession&) = delete;
    MockSession& operator=(const MockSession&) = delete;
};

/* Each line's type, and the lines, as run_clear wrote them */
struct Written {
    std::vector<std::string> lines;
    bool flush_ok = true;
    int flushes = 0;
    cli::ClearIo io()
    {
        cli::ClearIo io;
        io.write = [this](cli::Line& l) {
            lines.emplace_back(l.data(), l.size());
        };
        io.flush = [this] {
            ++flushes;
            return flush_ok;
        };
        io.stop = [] { return false; };
        return io;
    }
    std::vector<std::string> types() const
    {
        std::vector<std::string> t;
        for (const std::string& l : lines) {
            const size_t a = l.find("\"type\":\"") + 8;
            t.push_back(l.substr(a, l.find('"', a) - a));
        }
        return t;
    }
};

} // namespace

TEST(CliClearTest, TheOutcomesInWords)
{
    const char* const words[] = {
        "none",           "cleared",        "refused",
        "unanswered",     "engine-running", "no-engine-speed",
        "report-changed", "moving",         "no-vehicle-speed"};
    for (uint8_t i = 0; i < 9; ++i) {
        EXPECT_STREQ(cli::clear_outcome_word(i), words[i]) << int{i};
    }
    EXPECT_STREQ(cli::clear_outcome_word(9), "unknown");
    EXPECT_TRUE(cli::clear_reads_again(ECUIO_CLEAR_CLEARED));
    EXPECT_TRUE(cli::clear_reads_again(ECUIO_CLEAR_UNANSWERED));
    EXPECT_FALSE(cli::clear_reads_again(ECUIO_CLEAR_REFUSED));
    EXPECT_FALSE(cli::clear_reads_again(ECUIO_CLEAR_ENGINE_RUNNING));
}

TEST(CliClearTest, TheLineNamesTheReplyAndWhatWasRead)
{
    ecuio_channel_diagnostics_t d{};
    d.rx_id = 0x7E8;
    d.clear = ECUIO_CLEAR_REFUSED;
    d.clear_nrc = 0x22;
    d.clear_rpm = 812;
    d.flags = ECUIO_DIAG_CLEAR_RPM;
    EXPECT_EQ(clear_text(d), "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                             "\"outcome\":\"refused\",\"reply\":\"7F 04 22\","
                             "\"rpm\":812,\"kmh\":null}");
    d.clear = ECUIO_CLEAR_NO_ENGINE_SPEED;
    d.clear_nrc = 0x12;
    d.flags = 0;
    EXPECT_EQ(clear_text(d), "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                             "\"outcome\":\"no-engine-speed\","
                             "\"reply\":\"7F 01 12\",\"rpm\":null,"
                             "\"kmh\":null}")
        << "01 0C's refusal";
    d.clear = ECUIO_CLEAR_CLEARED;
    d.clear_nrc = 0;
    d.clear_rpm = 0;
    d.clear_kmh = 30;
    d.flags = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;
    EXPECT_EQ(clear_text(d), "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                             "\"outcome\":\"cleared\",\"rpm\":0,\"kmh\":30}")
        << "a 0 read is a 0, not a null";
    d.clear_nrc = 0x22;
    EXPECT_EQ(clear_text(d, "not-read"),
              "{\"type\":\"clear\",\"rx\":\"0x7E8\",\"outcome\":\"not-read\","
              "\"rpm\":null,\"kmh\":null}")
        << "the run's own word: nothing of the report's";

    cli::ClearResult r;
    r.reason = "not-cleared";
    r.cleared = 1;
    r.not_cleared = 2;
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::clear_end_line(l, r);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"end\",\"reason\":\"not-cleared\",\"cleared\":1,"
              "\"not_cleared\":2}");
}

// The worker takes a clear by moving the generation on (outcome none,
// clearing) and settles it by moving it on again with an outcome
TEST(CliClearTest, AClearSettlesPastItsGeneration)
{
    ecuio_channel_diagnostics_t d{};
    d.generation = 5;
    d.clear = ECUIO_CLEAR_CLEARED; /* an earlier clear's */
    EXPECT_FALSE(cli::clear_settled(d, 5)) << "not taken yet";
    d.generation = 6;
    d.clear = ECUIO_CLEAR_NONE;
    d.flags = ECUIO_DIAG_CLEARING;
    EXPECT_FALSE(cli::clear_settled(d, 5)) << "under way";
    d.generation = 7;
    d.flags = 0;
    EXPECT_FALSE(cli::clear_settled(d, 5)) << "no outcome";
    d.clear = ECUIO_CLEAR_ENGINE_RUNNING;
    EXPECT_TRUE(cli::clear_settled(d, 5));
    d.clear = ECUIO_CLEAR_CLEARED;
    d.generation = 8; /* the read after, started */
    EXPECT_TRUE(cli::clear_settled(d, 5));
}

// The report first, flushed, then the clear, then the read after a 44
TEST(CliClearTest, TheReportComesFirstAndTheReadAfter)
{
    MockSession m("mock://clear");
    ASSERT_NE(m.s, nullptr);
    Written w;
    const cli::ClearResult r =
        cli::run_clear(m.s, 1, 0, w.io(), std::chrono::seconds(10));
    EXPECT_STREQ(r.reason, "cleared");
    EXPECT_EQ(r.cleared, 1u);
    EXPECT_EQ(r.not_cleared, 0u);
    ASSERT_EQ(w.types(), (std::vector<std::string>{"diagnostics", "clear",
                                                   "diagnostics"}));
    EXPECT_NE(w.lines[0].find("\"P0301\""), std::string::npos) << w.lines[0];
    EXPECT_EQ(w.lines[1], "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                          "\"outcome\":\"cleared\",\"rpm\":0,\"kmh\":null}");
    EXPECT_EQ(w.lines[2].find("\"P0301\""), std::string::npos) << w.lines[2];
    EXPECT_GE(w.flushes, 1);
}

// Output that fails before the clear asks none: the report the ECU holds
// is the one nobody kept
TEST(CliClearTest, AFailedOutputAsksNoClear)
{
    MockSession m("mock://clear");
    ASSERT_NE(m.s, nullptr);
    ecuio_channel_diagnostics_t before{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(m.s, 0, &before), ECUIO_OK);
    Written w;
    w.flush_ok = false;
    const cli::ClearResult r =
        cli::run_clear(m.s, 1, 0, w.io(), std::chrono::seconds(10));
    EXPECT_STREQ(r.reason, "output-failed");
    EXPECT_EQ(r.cleared, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ecuio_channel_diagnostics_t after{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(m.s, 0, &after), ECUIO_OK);
    EXPECT_EQ(after.generation, before.generation) << "no clear was taken";
    EXPECT_EQ(after.clear, ECUIO_CLEAR_NONE);
    EXPECT_EQ(w.types(), (std::vector<std::string>{"diagnostics"}));
}

// A session that reads no diagnostics has nothing to save: no clear
TEST(CliClearTest, NothingReadIsNothingCleared)
{
    MockSession m("mock://clear?diagnose=0");
    ASSERT_NE(m.s, nullptr);
    Written w;
    const cli::ClearResult r =
        cli::run_clear(m.s, 1, 0, w.io(), std::chrono::milliseconds(300));
    EXPECT_STREQ(r.reason, "nothing-read");
    EXPECT_EQ(r.not_cleared, 1u);
    ASSERT_EQ(w.lines.size(), 1u);
    EXPECT_NE(w.lines[0].find("\"outcome\":\"not-read\""), std::string::npos);
}

// view --frames' choice of a set (cli/view_frames.h)
TEST(CliViewFramesTest, TheGivenProfileThenTheOwnFileThenTheRecordedOne)
{
    cli::RecordedSignals rec;
    rec.profile = "user.extras";
    rec.sha256 = std::string(64, 'a');
    cli::FramesSource s = cli::frames_source("obd2", rec, true);
    EXPECT_FALSE(s.own_file);
    EXPECT_EQ(s.profile, "obd2") << "--profile first";
    s = cli::frames_source("", rec, true);
    EXPECT_TRUE(s.own_file) << "the own file while it matches";
    EXPECT_TRUE(s.profile.empty());
    s = cli::frames_source("", rec, false);
    EXPECT_FALSE(s.own_file);
    EXPECT_EQ(s.profile, "user.extras") << "a changed copy: the recorded name";
    rec.sha256.clear();
    rec.profile = "obd2";
    s = cli::frames_source("", rec, true);
    EXPECT_FALSE(s.own_file) << "no file named, none to match";
    EXPECT_EQ(s.profile, "obd2");
}

TEST(CliViewFramesTest, ARecordingWithNoSignalsOfItsOwnSaysWhatToGive)
{
    cli::RecordedSignals rec;
    cli::FramesSource s = cli::frames_source("", rec, false);
    EXPECT_TRUE(s.profile.empty());
    EXPECT_FALSE(s.own_file);
    EXPECT_EQ(s.why,
              "its events.log names no profile: name one with --profile");
    rec.profile = "raw";
    s = cli::frames_source("", rec, false);
    EXPECT_TRUE(s.profile.empty());
    EXPECT_EQ(s.why, "it was recorded under raw, which decodes nothing: name a "
                     "profile with --profile");
    rec.profile = "uds-generic";
    s = cli::frames_source("", rec, false);
    EXPECT_TRUE(s.profile.empty());
    EXPECT_EQ(s.why, "uds-generic's signals come from its session's did= "
                     "list: give a --signals file");
    EXPECT_EQ(cli::frames_source("raw", rec, false).profile, "raw")
        << "a given one is looked up, and refused there";
}

TEST(CliViewFramesTest, AProfileWithNothingToDecodeIsRefused)
{
    EXPECT_EQ(cli::profile_refusal("obd2", true, 120), "");
    EXPECT_EQ(cli::profile_refusal("user.gone", false, 0),
              "no profile 'user.gone' is loaded: give its --signals file or "
              "--plugin");
    EXPECT_EQ(cli::profile_refusal("raw", true, 0),
              "the profile 'raw' has no signals to decode with");
}

/* ---------- The ELM327 emulator (ADR-0055) ---------- */

namespace {

/* The emulator before an unbound sim, at times the test gives */
struct Emu {
    ecuio_sim_t* sim = nullptr;
    cli::Elm327Emulator* emu = nullptr;
    int64_t t = 1000000; /* the next command's time, us */

    explicit Emu(const char* uri = "mock://emu?values=canned")
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << ecuio_last_error_detail();
        emu = new cli::Elm327Emulator(sim);
    }
    ~Emu()
    {
        delete emu;
        ecuio_sim_destroy(sim);
    }
    Emu(const Emu&) = delete;
    Emu& operator=(const Emu&) = delete;

    /* What's due by at */
    std::string heard(int64_t at)
    {
        std::string s;
        uint8_t buf[256];
        for (;;) {
            const size_t n = emu->to_host(buf, sizeof buf, at);
            if (n == 0) {
                return s;
            }
            s.append(reinterpret_cast<const char*>(buf), n);
        }
    }

    /* cmd and its CR at t, and everything it answers within 10 s; t
     * moves past it */
    std::string say(const std::string& cmd)
    {
        const std::string line = cmd + "\r";
        emu->from_host(reinterpret_cast<const uint8_t*>(line.data()),
                       line.size(), t);
        const std::string got = heard(t + 10000000);
        t += 20000000;
        return got;
    }

    /* cmd at t, without reading it: heard() then takes it at its times */
    void send(const std::string& cmd)
    {
        const std::string line = cmd + "\r";
        emu->from_host(reinterpret_cast<const uint8_t*>(line.data()),
                       line.size(), t);
    }

    /* The power-on echo off, line feeds off */
    void quiet() { (void)say("ATE0"); }
};

/* AT ST's default wait, 0x32 x 4.096 ms */
constexpr int64_t kWait = 0x32 * 4096;

} // namespace

TEST(CliElm327Test, TheResetAndTheSettings)
{
    Emu e;
    EXPECT_EQ(e.say("ATZ"), "ATZ\r\r\rELM327 v1.4b\r\r>") << "echo on";
    EXPECT_EQ(e.say("at e0"), "at e0\rOK\r\r>") << "lower case, spaces";
    EXPECT_EQ(e.say("ATI"), "ELM327 v1.4b\r\r>");
    EXPECT_EQ(e.say(""), "ELM327 v1.4b\r\r>") << "a CR alone repeats it";
    EXPECT_EQ(e.say("AT@1"), "OBDII to RS232 Interpreter\r\r>");
    EXPECT_EQ(e.say("ATRV"), "13.8V\r\r>");
    EXPECT_EQ(e.say("ATL1"), "OK\r\n\r\n>");
    EXPECT_EQ(e.say("ATL0"), "OK\r\r>");
    EXPECT_EQ(e.say("ATXYZ"), "?\r\r>");
    EXPECT_EQ(e.say("STI"), "?\r\r>") << "a plain ELM327";
    EXPECT_EQ(e.say("ATBRD10"), "?\r\r>") << "no baud change over TCP";
    EXPECT_EQ(e.say("0G"), "?\r\r>") << "not hex";
    EXPECT_EQ(e.say("0102030405060708"), "?\r\r>")
        << "8 bytes: past a Single Frame";
    EXPECT_EQ(e.say("ATD"), "OK\r\r>");
    EXPECT_EQ(e.say("ATI"), "ATI\rELM327 v1.4b\r\r>")
        << "the defaults: echo on";
    e.emu->power_on();
    EXPECT_EQ(e.say("ATI"), "ATI\rELM327 v1.4b\r\r>");
    EXPECT_EQ(e.emu->commands, 16u);
}

TEST(CliElm327Test, TheSearchAndTheProtocols)
{
    Emu e;
    e.quiet();
    EXPECT_EQ(e.say("ATDPN"), "0\r\r>");
    EXPECT_EQ(e.say("ATDP"), "AUTO\r\r>");
    /* the reply prints as it comes, the prompt after the adapter waited
     * for more */
    const int64_t at = e.t;
    e.send("0100");
    EXPECT_EQ(e.heard(at), "SEARCHING...\r41 00 9F FF 9F E3\r");
    EXPECT_EQ(e.heard(at + kWait - 1), "");
    EXPECT_EQ(e.heard(at + kWait), "\r>");
    e.t += 20000000;
    EXPECT_EQ(e.say("0100"), "41 00 9F FF 9F E3\r\r>") << "searched once";
    EXPECT_EQ(e.say("ATDPN"), "A6\r\r>");
    EXPECT_EQ(e.say("ATDP"), "AUTO, ISO 15765-4 (CAN 11/500)\r\r>");
    EXPECT_EQ(e.say("ATSP6"), "OK\r\r>");
    EXPECT_EQ(e.say("ATDPN"), "6\r\r>");
    EXPECT_EQ(e.say("010C"), "41 0C 1F 40\r\r>") << "the canned 2000 rpm";
    EXPECT_EQ(e.say("ATSP7"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "NO DATA\r\r>") << "no 29-bit ECU";
    EXPECT_EQ(e.say("ATSP8"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "CAN ERROR\r\r>")
        << "250 kbaud on a 500 kbaud bus";
    EXPECT_EQ(e.say("ATTP3"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "UNABLE TO CONNECT\r\r>");
    EXPECT_EQ(e.say("ATSPA6"), "OK\r\r>");
    EXPECT_EQ(e.say("0100"), "SEARCHING...\r41 00 9F FF 9F E3\r\r>");
    EXPECT_EQ(e.emu->requests, 7u);
}

TEST(CliElm327Test, HeadersSpacesAndTheDlc)
{
    Emu e;
    e.quiet();
    (void)e.say("ATSP6");
    EXPECT_EQ(e.say("ATH1"), "OK\r\r>");
    EXPECT_EQ(e.say("010C"), "7E8 04 41 0C 1F 40 00 00 00\r\r>")
        << "padded with zeros, as the vcan sim does";
    (void)e.say("ATD1");
    EXPECT_EQ(e.say("010C"), "7E8 8 04 41 0C 1F 40 00 00 00\r\r>");
    (void)e.say("ATS0");
    EXPECT_EQ(e.say("010C"), "7E8804410C1F40000000\r\r>");
    (void)e.say("ATD0");
    (void)e.say("ATS1");
    EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                             "7E8 21 46 41 4B 45 31 31 4D\r"
                             "7E8 22 30 30 30 30 30 30 31\r\r>");
    (void)e.say("ATH0");
    EXPECT_EQ(e.say("0902"),
              "014\r0: 49 02 01 5A 5A 5A\r1: 46 41 4B 45 31 31 4D\r"
              "2: 30 30 30 30 30 30 31\r\r>");
    (void)e.say("ATS0");
    EXPECT_EQ(e.say("03"), "008\r0:430303010420\r1:C100\r\r>")
        << "a long message's last line holds its own bytes alone";
}

TEST(CliElm327Test, TheCountDigitEndsAtItsLine)
{
    Emu e;
    e.quiet();
    (void)e.say("ATSP6");
    (void)e.say("ATH1");
    const int64_t at = e.t;
    e.send("09021");
    EXPECT_EQ(e.heard(at), "7E8 10 14 49 02 01 5A 5A 5A\r\r>")
        << "the first line, the prompt at once";
    e.t += 20000000;
    EXPECT_EQ(e.say("09022"),
              "7E8 10 14 49 02 01 5A 5A 5A\r7E8 21 46 41 4B 45 31 31 4D\r\r>");
    EXPECT_EQ(e.say("0902G"), "?\r\r>");
}

TEST(CliElm327Test, TheHeaderAndTheReceiveFilterPickTheEcu)
{
    Emu e("mock://emu?modules=2");
    e.quiet();
    (void)e.say("ATSP6");
    (void)e.say("ATH1");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>")
        << "functional: the engine ECU's refusal is never sent";
    EXPECT_EQ(e.say("ATSH7E0"), "OK\r\r>");
    EXPECT_EQ(e.say("22D101"), "7E8 03 7F 22 31 00 00 00 00\r\r>");
    EXPECT_EQ(e.say("ATSH7E1"), "OK\r\r>");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>");
    (void)e.say("ATSH7DF");
    EXPECT_EQ(e.say("ATCRA7E8"), "OK\r\r>");
    EXPECT_EQ(e.say("22D101"), "NO DATA\r\r>") << "the filter keeps 0x7E9 out";
    (void)e.say("ATCRA");
    (void)e.say("ATCF7E0");
    (void)e.say("ATCM7F0");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>");
    (void)e.say("ATCM7FF");
    EXPECT_EQ(e.say("22D101"), "NO DATA\r\r>");
    (void)e.say("ATAR");
    EXPECT_EQ(e.say("22D101"), "7E9 04 62 D1 01 03 00 00 00\r\r>");
}

TEST(CliElm327Test, FaultsAsAnAdapterSeesThem)
{
    {
        /* 7F 01 78 at once; the adapter waits P2* for the reply 300 ms on */
        Emu e("mock://emu?values=canned&faults=pending:1");
        e.quiet();
        (void)e.say("ATSP6");
        const int64_t at = e.t;
        e.send("010C");
        EXPECT_EQ(e.heard(at), "7F 01 78\r");
        EXPECT_EQ(e.heard(at + 299999), "");
        EXPECT_EQ(e.heard(at + 300000), "41 0C 1F 40\r");
        EXPECT_EQ(e.heard(at + 300000 + kWait), "\r>");
    }
    {
        /* 130 ms late: inside the default wait, past AT ST 19's */
        Emu e("mock://emu?values=canned&faults=late:1");
        e.quiet();
        (void)e.say("ATSP6");
        const int64_t at = e.t;
        e.send("010C");
        EXPECT_EQ(e.heard(at + 129999), "");
        EXPECT_EQ(e.heard(at + 130000), "41 0C 1F 40\r");
        EXPECT_EQ(e.heard(at + 130000 + kWait), "\r>");
        e.t += 20000000;
        EXPECT_EQ(e.say("ATST19"), "OK\r\r>");
        const int64_t at2 = e.t;
        e.send("010C");
        EXPECT_EQ(e.heard(at2 + 0x19 * 4096), "NO DATA\r\r>") << "lost";
    }
    {
        /* the last Consecutive Frame 160 ms after the one before */
        Emu e("mock://emu?values=canned&faults=stalled:1");
        e.quiet();
        (void)e.say("ATSP6");
        (void)e.say("ATH1");
        EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                                 "7E8 21 46 41 4B 45 31 31 4D\r"
                                 "7E8 22 30 30 30 30 30 30 31\r\r>");
        (void)e.say("ATST19");
        EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                                 "7E8 21 46 41 4B 45 31 31 4D\r\r>")
            << "past the shorter wait, the last frame is lost";
    }
    {
        Emu e("mock://emu?values=canned&faults=bad_sequence:1");
        e.quiet();
        (void)e.say("ATSP6");
        (void)e.say("ATH1");
        EXPECT_EQ(e.say("0902"), "7E8 10 14 49 02 01 5A 5A 5A\r"
                                 "7E8 21 46 41 4B 45 31 31 4D\r"
                                 "7E8 23 30 30 30 30 30 30 31\r\r>")
            << "the last frame's number skips one";
    }
    {
        Emu e("mock://emu?values=canned&faults=silent:1");
        e.quiet();
        (void)e.say("ATSP6");
        EXPECT_EQ(e.say("010C"), "NO DATA\r\r>");
    }
}

TEST(CliElm327Test, ResponsesOffAndTheMonitor)
{
    Emu e;
    e.quiet();
    (void)e.say("ATSP6");
    EXPECT_EQ(e.say("ATR0"), "OK\r\r>");
    EXPECT_EQ(e.say("04"), ">") << "sent, nothing printed";
    ecuio_sim_stats_t s{};
    ASSERT_EQ(ecuio_sim_get_stats(e.sim, &s), ECUIO_OK);
    EXPECT_EQ(s.clears, 1u);
    (void)e.say("ATR1");
    EXPECT_EQ(e.say("ATMA"), "") << "nothing else on the bus";
    EXPECT_TRUE(e.emu->monitoring());
    const uint8_t space = ' ';
    e.emu->from_host(&space, 1, e.t);
    EXPECT_EQ(e.heard(e.t), "STOPPED\r\r>") << "any byte stops it";
    EXPECT_FALSE(e.emu->monitoring());
    EXPECT_EQ(e.say("ATMA"), "");
    EXPECT_EQ(e.say("x"), "STOPPED\r\r>");
    EXPECT_TRUE(e.emu->monitoring())
        << "the CR after the stop is a CR alone: it repeats AT MA";
}

TEST(CliElm327Test, AddressesAndLines)
{
    cli::Elm327Address a;
    ASSERT_TRUE(cli::parse_elm327_address("tcp:35000", &a));
    EXPECT_EQ(a.ip, 0x7F000001u);
    EXPECT_EQ(a.port, 35000);
    ASSERT_TRUE(cli::parse_elm327_address("tcp:0", &a));
    EXPECT_EQ(a.port, 0);
    ASSERT_TRUE(cli::parse_elm327_address("tcp:192.168.4.1:65535", &a));
    EXPECT_EQ(a.ip, 0xC0A80401u);
    EXPECT_EQ(a.port, 65535);
    ASSERT_TRUE(cli::parse_elm327_address("tcp:0.0.0.0:23", &a));
    EXPECT_EQ(a.ip, 0u);
    for (const char* bad :
         {"35000", "udp:35000", "tcp:", "tcp:65536", "tcp:035000", "tcp:-1",
          "tcp:1.2.3:5", "tcp:1.2.3.4.5:5", "tcp:256.0.0.1:5", "tcp:01.2.3.4:5",
          "tcp:localhost:5", "tcp:1.2.3.4:", "tcp::5", "tcp:1..3.4:5",
          "tcp:[::1]:5"}) {
        EXPECT_FALSE(cli::parse_elm327_address(bad, &a)) << bad;
    }
    EXPECT_EQ(cli::address_text(0x7F000001u, 35000), "127.0.0.1:35000");

    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::elm327_line(l, "mock://emu?modules=2", "127.0.0.1:35000");
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"elm327\",\"uri\":\"mock://emu?modules=2\","
              "\"listen\":\"127.0.0.1:35000\"}");
    cli::Line c(buf, sizeof buf);
    cli::client_line(c, "refused", "127.0.0.1:50123");
    EXPECT_EQ(std::string(c.data(), c.size()),
              "{\"type\":\"client\",\"event\":\"refused\","
              "\"peer\":\"127.0.0.1:50123\"}");
}
