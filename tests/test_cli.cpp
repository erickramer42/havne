// File/layer: tests/test_cli.cpp | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#include "test_cli_support.h"

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

TEST(CliLineTest, AnExactStringWritesANulAsAnEscape)
{
    char buf[64];
    cli::Line l(buf, sizeof buf);
    EXPECT_EQ(l.string_exact("ab\0cd", 5), 5u);
    EXPECT_EQ(std::string(l.data(), l.size()), "\"ab\\u0000cd\"");
    cli::Line cut(buf, sizeof buf);
    cut.string_exact("ab\0cd", 5, 6);
    EXPECT_EQ(std::string(cut.data(), cut.size()), "\"ab\"")
        << "an escape whole";
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

/* ADR-0027 section 3: the vehicle line comes once core flags the VIN in,
 * the rest of the list still out, and once per new VIN */
TEST(CliVinTest, TheVehicleLineComesOnceTheVinIsIn)
{
    ecuio_channel_identity_t id{};
    id.rx_id = 0x7E8;
    id.items = 2;
    auto item = [&id](uint32_t i, uint8_t a, uint8_t b, uint8_t outcome,
                      const char* text) {
        ecuio_identity_item_t& it = id.list[i];
        it.request[0] = a;
        it.request[1] = b;
        it.length = 2;
        it.outcome = outcome;
        std::snprintf(it.text, sizeof it.text, "%s", text);
        it.text_length = static_cast<uint8_t>(std::strlen(it.text));
    };
    item(0, 0x09, 0x02, ECUIO_IDENTITY_ANSWERED, "1FAFAKE10M0000001");
    item(1, 0x09, 0x04, ECUIO_IDENTITY_UNASKED, "");
    char seen[ECUIO_VIN_TEXT_LEN] = "";
    EXPECT_FALSE(cli::vehicle_due(id, seen)) << "core hasn't flagged it";
    id.flags = ECUIO_IDENTITY_VIN;
    EXPECT_TRUE(cli::vehicle_due(id, seen));
    EXPECT_STREQ(seen, "1FAFAKE10M0000001");
    EXPECT_FALSE(cli::vehicle_due(id, seen)) << "once per VIN";
    id.flags |= ECUIO_IDENTITY_COMPLETE;
    EXPECT_FALSE(cli::vehicle_due(id, seen)) << "nor again once complete";

    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::vehicle_line(l, id, 2027);
    const std::string line(l.data(), l.size());
    EXPECT_EQ(line.substr(0, line.find(",\"source\":")),
              "{\"type\":\"vehicle\",\"rx\":\"0x7E8\","
              "\"vin\":{\"vin\":\"1FAFAKE10M0000001\",\"decoded\":true,"
              "\"model_year\":2021,\"year_earlier\":false,\"make\":\"Ford\","
              "\"wmi\":\"1FA\",\"check_digit\":\"0\",\"check_matches\":true");
    EXPECT_EQ(line.substr(line.size() - 2), "}}");

    std::snprintf(id.list[0].text, sizeof id.list[0].text, "%s",
                  "1FAFAKE13Y0000001");
    EXPECT_TRUE(cli::vehicle_due(id, seen)) << "another VIN is new";
    EXPECT_STREQ(seen, "1FAFAKE13Y0000001");
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
