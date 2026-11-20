// File/layer: tests/test_export_csv.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvExportTest (test_core)

#include "test_export_support.h"

namespace {

/* By the def first: a text DID's values and a number DID keyed in the
 * identification block (62 F18C) are left out, as the pcapng leaves
 * their frames out; then by kind: a value that isn't a number, and a
 * signal the header declares text even where its value reads as one.
 * Raw keeps every value. */
TEST(CsvExportTest, TextAndIdentificationSignalsAreLeftOut)
{
    const std::string header =
        "uri  elm327://COM3?profile=uds-generic&did=D802:text,D803:u16,"
        "F18C:u16\n"
        "profile  uds-generic\n"
        "signal  Code,text,,0,0\n";
    Folder f;
    recording(f, "",
              "1000,DID D802,ECM-HavneSim,\n2000,DID D803,4660,\n"
              "3000,DID F18C,424242,\n4000,DID D803,4661,\n"
              "5000,Note,hello there,\n6000,Code,123456,\n",
              header);
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes,
              csv_head("a scrubbed export",
                       "2 values of 1 signals, 0.002000 s to 0.004000 s", 1,
                       3) +
                  "0.002000,DID D803,,4660\n"
                  "0.004000,DID D803,,4661\n");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 3u);

    CsvExported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.csv");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_EQ(raw.report.values, 6u);
    EXPECT_EQ(raw.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 0u);
    EXPECT_NE(raw.bytes.find("a raw export, not scrubbed"), std::string::npos);
    EXPECT_NE(raw.bytes.find("0.005000,Note,,hello there\n"),
              std::string::npos);
}

/* The check over the written bytes: a unit echoing an identification
 * reply's value from frames.csv, a name shaped as a VIN */
TEST(CsvExportTest, AnIdentityValueInTheFileFailsTheExport)
{
    {
        SCOPED_TRACE("a calibration ID from the frames, as a unit");
        Folder f;
        recording(f,
                  row(1000, 0x7E8, kWhole,
                      text_bytes({0x49, 0x04, 0x01}, "FAKECAL-01")),
                  "2000,RPM,1726,FAKECAL-01\n");
        CsvExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail,
                  "the check found an identification reply's value in the "
                  "export's file: nothing was written");
        expect_no_file(f, "out.csv");
    }
    {
        SCOPED_TRACE("a VIN-shaped name");
        Folder f;
        recording(f, "", std::string("2000,") + kVin + ",1,\n");
        CsvExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(e.detail.find(kVin), std::string::npos);
        expect_no_file(f, "out.csv");
    }
}

/* A spreadsheet runs a cell that starts with = + - @, a tab or a CR: a
 * name, a unit or a raw text value starting so fails the export (no
 * escape, ADR-0049 section 8); a number never does */
TEST(CsvExportTest, AFormulasStartFailsTheExport)
{
    const auto refused = [](const std::string& signals, uint32_t flags,
                            const char* expected) {
        Folder f;
        recording(f, "", signals);
        CsvExported e(f, flags);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED) << signals;
        EXPECT_EQ(e.detail, expected);
        expect_no_file(f, "out.csv");
    };
    refused("1000,Boost,3,-x\n", 0,
            "the signal 'Boost''s unit starts as a spreadsheet formula does "
            "(=, +, -, @, a tab or a carriage return): nothing was written");
    refused("1000,=SUM(A1),3,kPa\n", 0,
            "the signal '=SUM(A1)''s name starts as a spreadsheet formula "
            "does (=, +, -, @, a tab or a carriage return): nothing was "
            "written");
    refused("1000,Note,@cmd,\n", ECUIO_EXPORT_RAW,
            "the signal 'Note''s text value starts as a spreadsheet formula "
            "does (=, +, -, @, a tab or a carriage return): nothing was "
            "written");

    /* Scrubbed, that text value is left out, so nothing fails */
    Folder f;
    recording(f, "", "1000,Note,@cmd,\n2000,Boost,-3,kPa\n");
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.values, 1u);
    EXPECT_NE(e.bytes.find("0.002000,Boost,kPa,-3\n"), std::string::npos);
}

TEST(CsvExportTest, ARecordingWithoutValuesOrWithABadRowWritesNothing)
{
    Folder f;
    recording(f, row(1000, 0x7E8, 0, {0x02, 0x41, 0x0C}));
    CsvExported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail, "the folder has no signals.csv");
    expect_no_file(f, "out.csv");

    Folder g;
    recording(g, "", "1000,RPM,1726,rpm\nnot a row\n");
    CsvExported bad(g);
    EXPECT_EQ(bad.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(bad.detail.rfind("signals.csv line 3: ", 0), 0u) << bad.detail;
    expect_no_file(g, "out.csv");
}

// ---- the folder export: a scrubbed copy of the recording ----

// ---- The wide CSV (ADR-0056 sections 4 and 5) ----

/* A row a sweep: once every column has a sample newer than the last
 * row, at the completing sample's time in whole ms; the columns every
 * number signal as first met, their decimals the most their values
 * show; CRLF, no comment line, the report's rows and cells */
TEST(CsvExportTest, WideWritesARowPerSweepOverEveryNumberSignal)
{
    Folder f;
    recording(f, "", polled_rows(3));
    fs::remove(f.path / "rec" / "frames.csv");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h),Throttle (%)\r\n"
                       "160,800,0,10.5\r\n"
                       "260,801,0,11.5\r\n"
                       "360,802,1,12.5\r\n");
    EXPECT_EQ(e.report.rows, 3u);
    EXPECT_EQ(e.report.values, 9u);
    EXPECT_EQ(e.report.signal_count, 3u);
    EXPECT_EQ(e.report.frames, 0u);

    /* A range: the rows its samples complete; the time stays the
     * recording's */
    WideExported mid(f, {}, 0, ECUIO_UNITS_METRIC, 0, 200000, 300000,
                     "mid.csv");
    ASSERT_EQ(mid.rc, ECUIO_OK) << mid.detail;
    EXPECT_EQ(mid.bytes, "Time (ms),RPM (rpm),Speed (km/h),Throttle (%)\r\n"
                         "260,801,0,11.5\r\n");
    WideExported none(f, {}, 0, ECUIO_UNITS_METRIC, 0, 9000000, 0, "none.csv");
    ASSERT_EQ(none.rc, ECUIO_OK) << none.detail;
    EXPECT_EQ(none.bytes, "Time (ms)\r\n");
    EXPECT_EQ(none.report.rows, 0u);
}

/* A signal that stops answering leaves the rule after 1 s of silence
 * (3x its usual interval being shorter): the rows go on without it,
 * its cells empty, and it rejoins with its next sample */
TEST(CsvExportTest, WideAColumnThatStopsLeavesTheRuleAndRejoins)
{
    std::string rows;
    for (int t = 100; t <= 3000; t += 100) {
        rows += std::to_string(t) + "000,RPM," + std::to_string(t) + ",rpm\n";
        if (t == 100 || t == 200 || t == 2500) {
            rows += std::to_string(t + 50) + "000,Speed," +
                    std::to_string(t / 100) + ",km/h\n";
        }
    }
    Folder f;
    recording(f, "", rows);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 17u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),Speed (km/h)");
    EXPECT_EQ(got[1], "150,100,1");
    EXPECT_EQ(got[2], "250,200,2");
    /* Speed is quiet from 250 ms: at 1300 ms the gap passes 1 s and the
     * sweeps go on without it */
    EXPECT_EQ(got[3], "1300,1300,") << "the first row after the silence";
    EXPECT_EQ(got[4], "1400,1400,");
    EXPECT_EQ(got[15], "2500,2500,");
    /* Its sample at 2550 ms rejoins; the next RPM completes the row */
    EXPECT_EQ(got[16], "2600,2600,25");
    EXPECT_EQ(e.report.rows, 16u);
    EXPECT_EQ(e.report.values, 16u + 3u);
}

/* A slow signal's own interval sets its silence, 3x it: one every
 * 500 ms leaves the rule 1.5 s after its last, not 1 s */
TEST(CsvExportTest, WideASlowColumnsIntervalSetsItsSilence)
{
    std::string rows;
    for (int t = 100; t <= 4000; t += 100) {
        rows += std::to_string(t) + "000,RPM," + std::to_string(t) + ",rpm\n";
        if (t == 500 || t == 1000 || t == 1500) {
            rows += std::to_string(t + 10) + "000,Slow," +
                    std::to_string(t / 500) + ",\n";
        }
    }
    Folder f;
    recording(f, "", rows);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_GE(got.size(), 5u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),Slow");
    EXPECT_EQ(got[1], "510,500,1");
    EXPECT_EQ(got[2], "1010,1000,2");
    EXPECT_EQ(got[3], "1510,1500,3");
    EXPECT_EQ(got[4], "3100,3100,")
        << "quiet 1.6 s at 3100 ms, past 3x its 500 ms; not at 2600";
    EXPECT_EQ(got.size(), 4u + 10u) << "then a row an RPM to 4000 ms";
}

/* Each signal's samples as signals.csv rows in time order: {ms, name,
 * value, unit} */
struct Sample {
    int ms;
    const char* name;
    int value;
    const char* unit;
};
std::string sample_rows(std::vector<Sample> samples)
{
    std::stable_sort(
        samples.begin(), samples.end(),
        [](const Sample& a, const Sample& b) { return a.ms < b.ms; });
    std::string s;
    for (const Sample& x : samples) {
        s += std::to_string(x.ms) + "000," + x.name + "," +
             std::to_string(x.value) + "," + x.unit + "\n";
    }
    return s;
}

/* A column far sparser than the rest (its usual interval over 16x the
 * median of the others') paces no sweep: a row for each sweep of the
 * others, its newest value held in each, empty before its first sample
 * and once quiet past its silence (3x its own 2 s) */
TEST(CsvExportTest, WideAFarSparserColumnPacesNoSweep)
{
    /* polled_rows' rows with the counter's sorted in by host_us */
    const std::string polled = polled_rows(120);
    std::vector<std::string> lines = {
        "1050000,Counter,1,\n", "3050000,Counter,2,\n", "5050000,Counter,3,\n"};
    for (size_t at = 0; at < polled.size();) {
        const size_t nl = polled.find('\n', at);
        lines.push_back(polled.substr(at, nl + 1 - at));
        at = nl + 1;
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const std::string& a, const std::string& b) {
                         return std::stoull(a) < std::stoull(b);
                     });
    std::string rows;
    for (const std::string& l : lines) {
        rows += l;
    }
    Folder f;
    recording(f, "", rows);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 1u + 120u) << e.bytes.substr(0, 400);
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),Speed (km/h),Throttle (%),Counter");
    EXPECT_EQ(got[1], "160,800,0,10.5,") << "before its first sample";
    EXPECT_EQ(got[9], "960,808,4,18.5,");
    EXPECT_EQ(got[10], "1060,809,4,19.5,1") << "its sample rides the next row";
    EXPECT_EQ(got[30], "3060,829,14,39.5,2");
    EXPECT_EQ(got[109], "10960,908,54,118.5,3") << "held within its silence";
    EXPECT_EQ(got[110], "11060,909,54,119.5,") << "6 s quiet: past it";
    EXPECT_EQ(got[120], "12060,919,59,129.5,");
    EXPECT_EQ(e.report.rows, 120u);
    EXPECT_EQ(e.report.values, 3u * 120u + 100u);

    /* The estimate runs the same rule: its rows and bytes are the file's */
    const uint32_t sweep = 0;
    ecuio_export_estimate_t size{};
    ecuio_export_options_t o{};
    const std::string folder = f.utf8("rec");
    o.format = ECUIO_EXPORT_CSV;
    o.folder_utf8 = folder.c_str();
    ASSERT_EQ(ecuio_export_estimate(&o, &sweep, 1, &size), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(size.rows, 120u);
    EXPECT_EQ(size.bytes, e.bytes.size());
}

/* The bound: a column at 16x the others' interval still sets the pace; at
 * 17x it leaves the rule */
TEST(CsvExportTest, WideTheSparseBoundIsSixteenTimesTheOthersInterval)
{
    const auto rows_with = [](int gap_ms) {
        std::vector<Sample> s;
        for (int t = 100; t <= 12000; t += 100) {
            s.push_back({t, "RPM", t, "rpm"});
        }
        for (int i = 0; i < 4; ++i) {
            s.push_back({50 + gap_ms * i, "Slow", i, ""});
        }
        return sample_rows(s);
    };
    {
        SCOPED_TRACE("16x");
        Folder f;
        recording(f, "", rows_with(1600));
        WideExported e(f);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        const std::vector<std::string> got = wide_lines(e.bytes);
        ASSERT_GE(got.size(), 5u) << e.bytes;
        EXPECT_EQ(got[0], "Time (ms),Slow,RPM (rpm)");
        EXPECT_EQ(got[1], "100,0,100") << "RPM's first completes the row";
        EXPECT_EQ(got[2], "1650,1,1600") << "then the rows wait for Slow";
        EXPECT_EQ(got[3], "3250,2,3200");
        EXPECT_EQ(got[4], "4850,3,4800");
        /* Slow quiet past 3x 1.6 s at 9700 ms: the rows go on without it */
        ASSERT_EQ(got.size(), 5u + 24u) << e.bytes;
        EXPECT_EQ(got[5], "9700,,9700");
        EXPECT_EQ(got[28], "12000,,12000");
    }
    {
        SCOPED_TRACE("17x");
        Folder f;
        recording(f, "", rows_with(1700));
        WideExported e(f);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        const std::vector<std::string> got = wide_lines(e.bytes);
        ASSERT_EQ(got.size(), 1u + 120u) << e.bytes.substr(0, 400);
        EXPECT_EQ(got[1], "100,0,100");
        EXPECT_EQ(got[17], "1700,0,1700") << "held, not waited for";
        EXPECT_EQ(got[18], "1800,1,1800");
        EXPECT_EQ(got[102], "10200,3,10200");
        EXPECT_EQ(got[103], "10300,,10300") << "quiet past 3x 1.7 s";
        EXPECT_EQ(e.report.rows, 120u);
    }
}

/* Every column as sparse as the rest: none leaves the rule, so the rows
 * follow them all */
TEST(CsvExportTest, WideColumnsSparseAlikeAllKeepTheRule)
{
    std::vector<Sample> s;
    for (int i = 0; i < 4; ++i) {
        s.push_back({2000 * i, "A", i, ""});
        s.push_back({2000 * i + 500, "B", i, ""});
        s.push_back({2000 * i + 900, "C", i, ""});
    }
    Folder f;
    recording(f, "", sample_rows(s));
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),A,B,C\r\n"
                       "900,0,0,0\r\n"
                       "2900,1,1,1\r\n"
                       "4900,2,2,2\r\n"
                       "6900,3,3,3\r\n");
}

/* Two columns far sparser than one: each is judged against the lower
 * median of the others' intervals (the fast one's), so both leave the
 * rule and the fast one's samples make the rows */
TEST(CsvExportTest, WideTwoSparseColumnsBesideOneLeaveTheRuleBoth)
{
    std::vector<Sample> s;
    for (int t = 100; t <= 6000; t += 100) {
        s.push_back({t, "RPM", t, "rpm"});
    }
    for (int i = 0; i < 3; ++i) {
        s.push_back({1050 + 2000 * i, "A", i, ""});
        s.push_back({1070 + 2000 * i, "B", i, ""});
    }
    Folder f;
    recording(f, "", sample_rows(s));
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 1u + 60u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),A,B");
    EXPECT_EQ(got[10], "1000,1000,,");
    EXPECT_EQ(got[11], "1100,1100,0,0");
    EXPECT_EQ(got[60], "6000,6000,2,2");
}

/* A column with one sample in range has no interval of its own: beside a
 * column with one it is sparse, holding no row up, its value held for its
 * 1 s silence */
TEST(CsvExportTest, WideAOneSampleColumnHoldsNoRowUp)
{
    std::vector<Sample> s;
    for (int t = 100; t <= 3000; t += 100) {
        s.push_back({t, "RPM", t, "rpm"});
    }
    s.push_back({1050, "One", 7, ""});
    Folder f;
    recording(f, "", sample_rows(s));
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 1u + 30u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),One");
    EXPECT_EQ(got[1], "100,100,") << "no wait for its sample";
    EXPECT_EQ(got[10], "1000,1000,");
    EXPECT_EQ(got[11], "1100,1100,7");
    EXPECT_EQ(got[20], "2000,2000,7") << "held within its silence";
    EXPECT_EQ(got[21], "2100,2100,") << "1 s quiet: past it";
    EXPECT_EQ(e.report.rows, 30u);
}

/* A one-sample column has no interval to count among the others: beside
 * it, a column at 16x the rest's still paces the rows */
TEST(CsvExportTest, WideAOneSampleColumnLeavesTheBoundAsItIs)
{
    std::vector<Sample> s;
    for (int t = 100; t <= 12000; t += 100) {
        s.push_back({t, "RPM", t, "rpm"});
    }
    for (int i = 0; i < 4; ++i) {
        s.push_back({50 + 1600 * i, "Slow", i, ""});
    }
    s.push_back({3000, "One", 7, ""});
    Folder f;
    recording(f, "", sample_rows(s));
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 5u + 24u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),Slow,RPM (rpm),One");
    EXPECT_EQ(got[1], "100,0,100,");
    EXPECT_EQ(got[2], "1650,1,1600,") << "the rows wait for Slow";
    EXPECT_EQ(got[3], "3250,2,3200,7");
    EXPECT_EQ(got[4], "4850,3,4800,");
    EXPECT_EQ(got[5], "9700,,9700,");
}

/* The others gone quiet past their silence leave the sparse column alone
 * in the rule: each of its samples is then a row of its own */
TEST(CsvExportTest, WideASparseColumnMakesTheRowsOnceTheRestStop)
{
    std::vector<Sample> s;
    for (int t = 100; t <= 2000; t += 100) {
        s.push_back({t, "RPM", t, "rpm"});
    }
    for (int i = 0; i < 4; ++i) {
        s.push_back({1050 + 2000 * i, "Counter", i + 1, ""});
    }
    Folder f;
    recording(f, "", sample_rows(s));
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 1u + 20u + 3u) << e.bytes;
    EXPECT_EQ(got[10], "1000,1000,");
    EXPECT_EQ(got[11], "1100,1100,1");
    EXPECT_EQ(got[20], "2000,2000,1");
    /* RPM quiet past 1 s at 3050 ms: the counter's samples are the rows */
    EXPECT_EQ(got[21], "3050,,2");
    EXPECT_EQ(got[22], "5050,,3");
    EXPECT_EQ(got[23], "7050,,4");
}

/* A grid: a row at each interval from the recording's zero with the
 * newest values at its time, held; a step with no new cell skipped;
 * the last values on the step after the last sample */
TEST(CsvExportTest, WideAGridHoldsTheNewestValuesAndSkipsAnEmptyStep)
{
    Folder f;
    recording(f, "",
              "50000,RPM,800,rpm\n60000,Speed,3,km/h\n120000,RPM,810,rpm\n"
              "530000,RPM,820,rpm\n");
    WideExported e(f, {}, 100);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h)\r\n"
                       "100,800,3\r\n"
                       "200,810,3\r\n"
                       "600,820,3\r\n");
    EXPECT_EQ(e.report.rows, 3u);
    EXPECT_EQ(e.report.values, 6u);

    /* A --from past the zero keeps the grid on the zero's multiples */
    WideExported late(f, {}, 100, ECUIO_UNITS_METRIC, 0, 55000, 0, "late.csv");
    ASSERT_EQ(late.rc, ECUIO_OK) << late.detail;
    EXPECT_EQ(late.bytes, "Time (ms),Speed (km/h),RPM (rpm)\r\n"
                          "100,3,\r\n"
                          "200,3,810\r\n"
                          "600,3,820\r\n");
}

/* Each cell in its signal's decimal places from the header's signal
 * line, converted to the units asked for (one more place where the
 * conversion needs it), the head cell in the unit shown */
TEST(CsvExportTest, WideASweepRowsTimeIsTheCompletingSamplesToTheNearestMs)
{
    Folder f;
    recording(f, "",
              "100600,RPM,800,rpm\n100700,Speed,3,km/h\n"
              "200000,RPM,801,rpm\n200500,Speed,4,km/h\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h)\r\n"
                       "101,800,3\r\n"
                       "201,801,4\r\n");
}

TEST(CsvExportTest, WideAGridAppliesTheSilenceAtTheStepItWrites)
{
    /* Speed answers once; RPM's sample at 1050 ms finds it 990 ms quiet
     * (in the rule), the grid's step at 1100 ms finds it past 1 s: the
     * step writes RPM alone */
    Folder f;
    recording(f, "",
              "50000,RPM,800,rpm\n60000,Speed,3,km/h\n"
              "1050000,RPM,810,rpm\n1250000,RPM,820,rpm\n");
    WideExported e(f, {}, 100);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h)\r\n"
                       "100,800,3\r\n"
                       "1100,810,\r\n"
                       "1300,820,\r\n");
    EXPECT_EQ(e.report.values, 4u);
}

TEST(CsvExportTest, WideAGridJumpsAGapWithNothingNew)
{
    /* Two samples 1e9 s apart at --rows 1: a step at a time would be
     * 1e12 steps; the file is two rows and the export is quick */
    Folder f;
    recording(f, "", "0,RPM,800,rpm\n1000000000000000,RPM,801,rpm\n");
    const auto begun = std::chrono::steady_clock::now();
    WideExported e(f, {}, 1);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm)\r\n"
                       "0,800\r\n"
                       "1000000000000,801\r\n");
    EXPECT_EQ(e.report.rows, 2u);
    EXPECT_LT(std::chrono::steady_clock::now() - begun,
              std::chrono::seconds(5));
}

TEST(CsvExportTest, WideAValuePastTheFixedDigitsIsWrittenAsRecorded)
{
    Folder f;
    recording(f, "", "100000,RPM,1e17,rpm\n200000,RPM,-2.5e16,rpm\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm)\r\n"
                       "100,1e+17\r\n"
                       "200,-2.5e+16\r\n");
    EXPECT_EQ(e.report.values, 2u);
}

TEST(CsvExportTest, WideCellsFollowTheHeadersDecimalsAndTheUnits)
{
    const std::string header = "profile  obd2\n"
                               "signal  Speed,number,km/h,0,1,0\n"
                               "signal  Boost,number,kPa,1,2,0\n";
    Folder f;
    recording(f, "", "1000,Speed,100,km/h\n2000,Boost,101.3,kPa\n", header);
    WideExported metric(f);
    ASSERT_EQ(metric.rc, ECUIO_OK) << metric.detail;
    EXPECT_EQ(metric.bytes, "Time (ms),Speed (km/h),Boost (kPa)\r\n"
                            "2,100,101.3\r\n");
    WideExported imperial(f, {}, 0, ECUIO_UNITS_IMPERIAL, 0, 0, 0,
                          "imperial.csv");
    ASSERT_EQ(imperial.rc, ECUIO_OK) << imperial.detail;
    EXPECT_EQ(imperial.bytes, "Time (ms),Speed (mph),Boost (psi)\r\n"
                              "2,62,14.69\r\n");
}

/* The columns named, in their order: one the recording lacks is empty,
 * and leaves the rule after 1 s so the rows go on; a text or
 * identification signal, or a name twice, is refused */
TEST(CsvExportTest, WideNamedColumnsComeInTheirOrder)
{
    Folder f;
    recording(f, "", polled_rows(12));
    WideExported e(f, {"Speed", "RPM", "Nothing"});
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_GE(got.size(), 2u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),Speed (km/h),RPM (rpm),Nothing");
    EXPECT_EQ(got[1], "130,0,800,")
        << "the first sweep: Nothing has no sample, so it holds no row up";
    EXPECT_EQ(got.size(), 13u) << "a row a sweep";
    EXPECT_EQ(e.report.signal_count, 3u);

    const std::string header =
        "uri  mock://x?profile=uds-generic&did=D802:text,"
        "F18C:u16\n"
        "profile  uds-generic\n";
    Folder g;
    recording(g, "",
              "1000,DID D802,ECM,\n2000,DID F18C,4242,\n3000,RPM,1,rpm\n",
              header);
    WideExported text(g, {"DID D802"});
    EXPECT_EQ(text.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(text.detail.find("text signal"), std::string::npos)
        << text.detail;
    WideExported ident(g, {"DID F18C"});
    EXPECT_EQ(ident.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(ident.detail.find("identification signal"), std::string::npos)
        << ident.detail;
    WideExported twice(g, {"RPM", "RPM"});
    EXPECT_EQ(twice.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(twice.detail.find("named twice"), std::string::npos)
        << twice.detail;
    EXPECT_FALSE(fs::exists(g.path / "wide.csv"));
}

/* No columns named: the header's focus line picks them, in its order;
 * without one, every number signal */
TEST(CsvExportTest, WideTakesTheFocusLinesColumnsWhenNoneAreNamed)
{
    Folder f;
    recording(f, "", polled_rows(2), "profile  raw\nfocus  Speed,RPM\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),Speed (km/h),RPM (rpm)\r\n"
                       "130,0,800\r\n"
                       "230,0,801\r\n");
    WideExported all(f, {"Throttle"}, 0, ECUIO_UNITS_METRIC, 0, 0, 0,
                     "all.csv");
    ASSERT_EQ(all.rc, ECUIO_OK) << all.detail;
    EXPECT_EQ(all.bytes, "Time (ms),Throttle (%)\r\n160,10.5\r\n260,11.5\r\n");
}

/* A front end's default pick names no columns: a recording with a pick
 * line exports as one with no line, every number signal */
TEST(CsvExportTest, WideTakesNoColumnsFromAPickLine)
{
    /* One folder at a time: a Folder is named for the process */
    std::string picked;
    {
        Folder f;
        recording(f, "", polled_rows(2), "profile  raw\npick  Speed,RPM\n");
        WideExported p(f);
        ASSERT_EQ(p.rc, ECUIO_OK) << p.detail;
        picked = p.bytes;
    }
    Folder f;
    recording(f, "", polled_rows(2), "profile  raw\n");
    WideExported plain(f);
    ASSERT_EQ(plain.rc, ECUIO_OK) << plain.detail;
    EXPECT_EQ(picked, plain.bytes);
    EXPECT_NE(picked.rfind("Time (ms),Speed (km/h),RPM (rpm)\r\n", 0), 0u)
        << "not the pick's columns: " << picked;
}

/* A focus line that names a column twice, or a text or identification
 * signal, is the recording's fault: LOAD_FAILED with the reason, not the
 * caller's INVALID_ARGUMENT, and no file */
TEST(CsvExportTest, WideAFocusLineThatCannotBeColumnsIsABadRecording)
{
    const std::string rows =
        "1000,DID D802,ECM,\n2000,DID F18C,4242,\n3000,RPM,1,rpm\n";
    const struct {
        const char* focus;
        const char* reason;
    } cases[] = {{"RPM,RPM", "named twice"},
                 {"DID D802", "text signal"},
                 {"DID F18C", "identification signal"}};
    for (const auto& c : cases) {
        Folder g;
        recording(g, "", rows,
                  std::string("uri  mock://x?profile=uds-generic&did="
                              "D802:text,F18C:u16\n"
                              "profile  uds-generic\n"
                              "focus  ") +
                      c.focus + "\n");
        WideExported e(g);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED) << c.focus;
        EXPECT_NE(e.detail.find(c.reason), std::string::npos) << e.detail;
        EXPECT_FALSE(fs::exists(g.path / "wide.csv")) << c.focus;
    }
}

/* Text and identification signals are never columns, scrubbed, and
 * their values are counted left out as the long form counts them; the
 * header's number signals come first, in its order, with no sample
 * dropped */
TEST(CsvExportTest, WideTextAndIdentificationSignalsAreNeverColumns)
{
    const std::string header =
        "uri  elm327://COM3?profile=uds-generic&did=D802:text,D803:u16,"
        "F18C:u16\n"
        "profile  uds-generic\n"
        "signal  Code,text,,0,0\n"
        "signal  Quiet,number,,0,0\n"
        "signal  DID D803,number,,0,0\n";
    Folder f;
    recording(f, "",
              "1000,DID D802,ECM-HavneSim,\n2000,DID D803,4660,\n"
              "3000,DID F18C,424242,\n4000,DID D803,4661,\n"
              "5000,Note,hello there,\n6000,Code,123456,\n7000,Extra,7,\n",
              header);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    /* Extra, in the rule from the start, holds the rows until its first
     * sample, which completes the one row */
    EXPECT_EQ(e.bytes, "Time (ms),DID D803,Extra\r\n7,4661,7\r\n");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 3u);
}

/* A name holding a comma is quoted in the head cell; a formula's start
 * in a name fails the export, leaving no file */
TEST(CsvExportTest, WideQuotesAHeadAndRefusesAFormula)
{
    Folder f;
    recording(f, "",
              "1000,\"Trim, bank 1\",3.1,%\n2000,\"Trim, bank 1\",3.2,%\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),\"Trim, bank 1 (%)\"\r\n1,3.1\r\n2,3.2\r\n");
    Folder g;
    recording(g, "", "1000,=RPM,5,rpm\n");
    WideExported bad(g);
    EXPECT_EQ(bad.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(bad.detail.find("formula"), std::string::npos) << bad.detail;
    EXPECT_FALSE(fs::exists(g.path / "wide.csv"));
}

} // namespace
