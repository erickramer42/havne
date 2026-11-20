// File/layer: tests/test_export_cancel.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ExportCancelTest, ExportTest (test_core)

#include "test_export_support.h"

namespace {

/* A stop at each ask in turn: CANCELED with its words, no ask after the
 * one that stopped it, and nothing at the name or beside it (each pass's
 * first ask and each file of a copy among them) */
TEST(ExportCancelTest, AStopAtEachAskInTurnLeavesNothingBehind)
{
    Folder f;
    whole_recording(f);
    for (const Kind& k : kKinds) {
        SCOPED_TRACE(k.name);
        Asks all;
        ASSERT_EQ(export_as(f, k, "counted", &all), ECUIO_OK);
        std::error_code ec;
        fs::remove_all(f.path / "counted", ec);
        ASSERT_GE(all.count, 3u);
        for (unsigned long n = 1; n <= all.count; ++n) {
            Asks asks;
            asks.stop_at = n;
            ASSERT_EQ(export_as(f, k, "stopped", &asks), ECUIO_ERR_CANCELED)
                << "at " << n;
            EXPECT_STREQ(ecuio_last_error_detail(), "the export was canceled");
            EXPECT_EQ(asks.count, n) << "asked again after it stopped";
            ASSERT_FALSE(fs::exists(f.path / "stopped")) << "at " << n;
            ASSERT_FALSE(partial_left(f.path, "stopped")) << "at " << n;
        }
    }
    Asks all;
    ecuio_export_estimate_t sized[2] = {};
    ASSERT_EQ(estimate_of(f, &all, sized), ECUIO_OK);
    ASSERT_GE(all.count, 2u);
    for (unsigned long n = 1; n <= all.count; ++n) {
        Asks asks;
        asks.stop_at = n;
        ecuio_export_estimate_t out[2];
        std::memset(out, 0xAB, sizeof out);
        ASSERT_EQ(estimate_of(f, &asks, out), ECUIO_ERR_CANCELED) << n;
        EXPECT_EQ(asks.count, n);
        for (const ecuio_export_estimate_t& e : out) {
            EXPECT_EQ(e.rows | e.bytes | e.columns, 0u) << "at " << n;
        }
    }
}

/* A stopped export's report counts nothing, whatever it had written */
TEST(ExportCancelTest, ACanceledExportsReportIsZeroed)
{
    Folder f;
    whole_recording(f);
    for (const Kind& k : kKinds) {
        SCOPED_TRACE(k.name);
        Asks all;
        ASSERT_EQ(export_as(f, k, "counted", &all), ECUIO_OK);
        std::error_code ec;
        fs::remove_all(f.path / "counted", ec);
        for (unsigned long n = 1; n <= all.count; ++n) {
            Asks asks;
            asks.stop_at = n;
            ecuio_export_report_t report;
            std::memset(&report, 0xAB, sizeof report);
            ASSERT_EQ(export_as(f, k, "stopped", &asks, &report),
                      ECUIO_ERR_CANCELED)
                << "at " << n;
            const unsigned char* b =
                reinterpret_cast<const unsigned char*>(&report);
            for (size_t i = 0; i < sizeof report; ++i) {
                ASSERT_EQ(b[i], 0u) << "byte " << i << " at " << n;
            }
        }
    }
}

/* A recording of a few rows in each file, but kBig in the one named */
constexpr int kBig = 4096;

void rows_recording(const Folder& f, const std::string& big)
{
    const auto rows_of = [&big](const char* name) {
        return big == name ? kBig : 3;
    };
    std::string events = "ecuio session recording, format 3 (ADR-0049)\n"
                         "started  2023-11-14 22:13:20.250 (UTC)\n"
                         "utc  2023-11-14T22:13:20.250Z\n"
                         "profile  raw\n"
                         "frame flags  complete\n"
                         "signal  RPM,number,rpm,0,1\n" +
                         std::string(kColumns) + "\n";
    std::string frames = "host_us,timestamp_us,id,flags,data\n";
    std::string values = "host_us,signal,value,unit\n";
    std::string markers = "host_us,label\n";
    std::string latency = "t_s,ecu,request,answered\n";
    char b[160];
    for (int i = 0; i < rows_of("events.log"); ++i) {
        events += "22:13:23.250  +3.000  asleep  Asleep for 1.0 s: nothing "
                  "was read meanwhile\n";
    }
    for (int i = 0; i < rows_of("frames.csv"); ++i) {
        frames += row(1000000 + 1000u * static_cast<unsigned>(i), 0x123, 0,
                      {0x01, 0x02, 0x03});
    }
    for (int i = 0; i < rows_of("signals.csv"); ++i) {
        std::snprintf(b, sizeof b, "%d,RPM,%d,rpm\n", 1000000 + 1000 * i,
                      800 + i % 100);
        values += b;
    }
    for (int i = 0; i < rows_of("markers.csv"); ++i) {
        std::snprintf(b, sizeof b, "%d,m%d\n", 1000000 + 1000 * i, i);
        markers += b;
    }
    for (int i = 0; i < rows_of("latency.csv"); ++i) {
        std::snprintf(b, sizeof b, "%d.%03d,7E8,01 0C,5\n", 1 + i / 1000,
                      i % 1000);
        latency += b;
    }
    f.write("rec/events.log", events);
    f.write("rec/frames.csv", frames);
    f.write("rec/signals.csv", values);
    f.write("rec/markers.csv", markers);
    f.write("rec/latency.csv", latency);
}

/* Each pass is asked as its rows are read, not only as it begins: a file
 * kBig rows longer adds asks for every pass over it (an ask each 1024
 * rows is the export's own rule; the header promises no interval) */
TEST(ExportCancelTest, EachPassIsAskedAsItsRowsAreRead)
{
    struct Pass {
        const char* kind;
        const char* file;
        unsigned long passes;
    };
    const Pass passes[] = {
        {"pcapng", "frames.csv", 2},  {"pcapng", "signals.csv", 1},
        {"long", "frames.csv", 1},    {"long", "signals.csv", 3},
        {"wide", "frames.csv", 1},    {"wide", "signals.csv", 3},
        {"folder", "frames.csv", 2},  {"folder", "signals.csv", 3},
        {"folder", "events.log", 2},  {"folder", "markers.csv", 2},
        {"folder", "latency.csv", 2}, {"estimate", "signals.csv", 2},
    };
    const auto asks_for = [](const char* kind, const std::string& big) {
        Folder f;
        rows_recording(f, big);
        Asks asks;
        if (std::strcmp(kind, "estimate") == 0) {
            ecuio_export_estimate_t sized[2];
            EXPECT_EQ(estimate_of(f, &asks, sized), ECUIO_OK)
                << ecuio_last_error_detail();
            return asks.count;
        }
        for (const Kind& k : kKinds) {
            if (std::strcmp(k.name, kind) == 0) {
                EXPECT_EQ(export_as(f, k, "out", &asks), ECUIO_OK)
                    << kind << " " << big << ": " << ecuio_last_error_detail();
            }
        }
        return asks.count;
    };
    for (const Pass& p : passes) {
        SCOPED_TRACE(std::string(p.kind) + " " + p.file);
        const unsigned long few = asks_for(p.kind, "");
        const unsigned long many = asks_for(p.kind, p.file);
        EXPECT_GE(many, few + p.passes * (kBig / 1024))
            << "asked " << few << " times with a few rows, " << many << " with "
            << kBig;
    }
}

/* What another thread's stop reaches: the export it asks */
struct ThreadStop {
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> asks{0};
};

int thread_ask(void* user)
{
    auto* s = static_cast<ThreadStop*>(user);
    s->asks.fetch_add(1);
    return s->stop.load() ? 1 : 0;
}

/* A stop set from another thread mid-export: the export returns
 * CANCELED soon after, short of its whole run, nothing left behind */
TEST(ExportCancelTest, AStopFromAnotherThreadEndsALongExport)
{
    Folder f;
    std::string frames = "host_us,timestamp_us,id,flags,data\n";
    for (unsigned i = 0; i < 200000; ++i) {
        frames += row(1000000 + 100u * i, 0x123, 0, {0x01, 0x02, 0x03, 0x04});
    }
    f.write("rec/events.log", events_log());
    f.write("rec/frames.csv", frames);

    Asks whole;
    ASSERT_EQ(export_as(f, kKinds[0], "whole", &whole), ECUIO_OK);

    ThreadStop shared;
    const std::string folder = f.utf8("rec");
    const std::string out = f.utf8("stopped");
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_PCAPNG;
    o.folder_utf8 = folder.c_str();
    o.out_path_utf8 = out.c_str();
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    std::thread exporter([&] {
        rc = ecuio_export_with(&o, thread_ask, &shared, nullptr);
        detail = ecuio_last_error_detail();
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (shared.asks.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stopped = std::chrono::steady_clock::now();
    shared.stop.store(true);
    exporter.join();
    const auto took = std::chrono::steady_clock::now() - stopped;

    EXPECT_EQ(rc, ECUIO_ERR_CANCELED) << detail;
    EXPECT_EQ(detail, "the export was canceled");
    EXPECT_LT(shared.asks.load(), whole.count) << "it ran to its end";
    EXPECT_LT(took, std::chrono::seconds(2));
    EXPECT_FALSE(fs::exists(f.path / "stopped"));
    EXPECT_FALSE(partial_left(f.path, "stopped"));
}

/* The exports' header read puts a focus written on several lines
 * together, a name holding a quote or a comma whole; and passes a line
 * past a row (an older core's focus) by whole, so the signal lines
 * after it still say which signals are text */
TEST(ExportTest, TheHeaderReadJoinsFocusLinesAndPassesALinePastARow)
{
    Folder f;
    const std::string head = "ecuio session recording, format 3 (ADR-0049)\n"
                             "profile  test\n";
    const std::string signals = "signal  RPM,number,rpm,0,1\n"
                                "signal  Words,text,,0,0\n";
    f.write("rec/events.log", head +
                                  "focus  RPM,\"say \"\"hi\"\"\"\n"
                                  "focus  \"a,b\",Words\n" +
                                  signals + kColumns + "\n");
    ecuio_recording_set::header h =
        ecuio_recording_set::read_header(f.utf8("rec"));
    EXPECT_EQ(h.focus,
              (std::vector<std::string>{"RPM", "say \"hi\"", "a,b", "Words"}));

    std::string past = "focus  ";
    while (past.size() <= 40000) {
        past += "\"A name of thirty-one bytes, xx\",";
    }
    f.write("rec/events.log", head + past + "\n" + signals + kColumns + "\n");
    h = ecuio_recording_set::read_header(f.utf8("rec"));
    EXPECT_TRUE(h.focus.empty());
    EXPECT_EQ(h.signals.size(), 2u);
    EXPECT_EQ(h.text_names, std::vector<std::string>{"Words"});

    /* A line one byte past a row's room: what is left of it is its line
     * end alone, which isn't the header's blank last line */
    std::string exact = "focus  ";
    exact.resize(ecuio_recording_rows::kRowMax + 1, 'x');
    f.write("rec/events.log", head + exact + "\n" + signals + kColumns + "\n");
    h = ecuio_recording_set::read_header(f.utf8("rec"));
    EXPECT_TRUE(h.focus.empty());
    EXPECT_EQ(h.signals.size(), 2u);

    /* What is left past two rows' room is no line of its own, though it
     * reads as a focus line */
    std::string caller = "caller  ";
    caller.resize(2 * (ecuio_recording_rows::kRowMax + 1), 'y');
    f.write("rec/events.log",
            head + caller + "focus  Ghost\n" + signals + kColumns + "\n");
    h = ecuio_recording_set::read_header(f.utf8("rec"));
    EXPECT_TRUE(h.focus.empty());
    EXPECT_EQ(h.signals.size(), 2u);
}

} // namespace
