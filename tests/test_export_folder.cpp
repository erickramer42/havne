// File/layer: tests/test_export_folder.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ExportTest, FolderExportTest (test_core)

#include "test_export_support.h"

namespace {

/* Excel reads a CSV without a byte order mark as ANSI, so the units
 * ("%", "psi", a degree sign) show garbled: both user CSVs start with one,
 * once, and the long one's comment line follows it */
TEST(ExportTest, EachUserCsvStartsWithAByteOrderMark)
{
    Folder f;
    recording(f, "", polled_rows(5));
    const std::string folder = f.utf8("rec");
    for (const uint32_t layout : {ECUIO_CSV_LONG, ECUIO_CSV_WIDE}) {
        for (const uint32_t flags : {0u, uint32_t{ECUIO_EXPORT_RAW}}) {
            SCOPED_TRACE(layout);
            const std::string name =
                "bom" + std::to_string(layout) + std::to_string(flags);
            ecuio_export_options_t o{};
            o.format = ECUIO_EXPORT_CSV;
            o.layout = layout;
            o.flags = flags;
            const std::string out = f.utf8(name);
            o.folder_utf8 = folder.c_str();
            o.out_path_utf8 = out.c_str();
            ecuio_export_report_t r{};
            ASSERT_EQ(ecuio_export(&o, &r), ECUIO_OK)
                << ecuio_last_error_detail();
            const std::string bytes = read_file(f.path / name);
            ASSERT_GT(bytes.size(), 8u);
            EXPECT_EQ(bytes.substr(0, 3), kBom);
            EXPECT_NE(bytes.substr(3, 3), kBom) << "written once";
            EXPECT_EQ(bytes.substr(3, 1), layout == ECUIO_CSV_LONG ? "#" : "T")
                << "the comment line, or the header, right after it";
        }
    }
}

/* The estimate answers several intervals in one pass with what each
 * export then writes: the rows, the bytes exactly, the columns */
TEST(ExportTest, TheEstimateMatchesTheFileForEachInterval)
{
    Folder f;
    recording(f, "", polled_rows(30));
    const std::string folder = f.utf8("rec");
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.folder_utf8 = folder.c_str();
    const uint32_t intervals[3] = {0, 100, 250};
    ecuio_export_estimate_t est[3];
    ASSERT_EQ(ecuio_export_estimate(&o, intervals, 3, est), ECUIO_OK)
        << ecuio_last_error_detail();
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(intervals[i]);
        const std::string name = "est" + std::to_string(i) + ".csv";
        WideExported e(f, {}, intervals[i], ECUIO_UNITS_METRIC, 0, 0, 0, name);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        EXPECT_EQ(est[i].rows, e.report.rows);
        EXPECT_EQ(est[i].bytes, e.bytes.size());
        EXPECT_EQ(est[i].columns, 3u);
        EXPECT_EQ(est[i].flags, 0u);
        EXPECT_GT(est[i].rows, 0u);
    }
    EXPECT_GT(est[1].rows, est[2].rows) << "a 250 ms grid thins the sweeps";

    /* Named columns (one quoted, one the recording lacks), a range and
     * imperial units: the estimate is still the file's */
    Folder g;
    recording(g, "",
              polled_rows(10) + "2000000,\"Boost, abs\",101,kPa\n"
                                "2100000,\"Boost, abs\",102.5,kPa\n");
    const std::string gfolder = g.utf8("rec");
    const char* columns[3] = {"Throttle", "Boost, abs", "Nothing"};
    ecuio_export_options_t n{};
    n.format = ECUIO_EXPORT_CSV;
    n.folder_utf8 = gfolder.c_str();
    n.from_us = 500000;
    n.to_us = 2050000;
    n.units = ECUIO_UNITS_IMPERIAL;
    n.column_count = 3;
    n.columns_utf8 = columns;
    ecuio_export_estimate_t named[3];
    ASSERT_EQ(ecuio_export_estimate(&n, intervals, 3, named), ECUIO_OK)
        << ecuio_last_error_detail();
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(intervals[i]);
        const std::string name = "named" + std::to_string(i) + ".csv";
        WideExported e(g, {"Throttle", "Boost, abs", "Nothing"}, intervals[i],
                       ECUIO_UNITS_IMPERIAL, 0, 500000, 2050000, name);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        EXPECT_EQ(named[i].rows, e.report.rows);
        EXPECT_EQ(named[i].bytes, e.bytes.size());
        EXPECT_EQ(named[i].columns, 3u);
        EXPECT_GT(named[i].rows, 0u);
        EXPECT_EQ(wide_lines(e.bytes)[0],
                  "Time (ms),Throttle (%),\"Boost, abs (psi)\",Nothing");
    }
}

/* The wide CSV has 16384 columns at most, found or named: the refusal
 * names the limit, comes from the survey (a few seconds for 16384
 * columns, not minutes) and leaves no file */
TEST(ExportTest, TheWideCsvRefusesMoreThanTheColumnLimit)
{
    const auto names = [](size_t n) {
        std::string rows;
        for (size_t i = 0; i < n; ++i) {
            rows += std::to_string(1000 * (i + 1)) + ",S" + std::to_string(i) +
                    ",1,\n";
        }
        return rows;
    };
    Folder at;
    recording(at, "", names(16384));
    WideExported ok(at);
    ASSERT_EQ(ok.rc, ECUIO_OK) << ok.detail;
    EXPECT_EQ(ok.report.signal_count, 16384u);
    Folder over;
    recording(over, "", names(16385));
    WideExported found(over);
    EXPECT_EQ(found.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(found.detail,
              "the wide CSV is limited to 16384 columns: pick the columns to "
              "keep");
    EXPECT_FALSE(fs::exists(over.path / "wide.csv"));
    EXPECT_FALSE(partial_left(over.path, "wide.csv"));

    /* The estimate refuses it the same way */
    {
        const std::string folder = over.utf8("rec");
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.folder_utf8 = folder.c_str();
        const uint32_t ms = 0;
        ecuio_export_estimate_t est;
        EXPECT_EQ(ecuio_export_estimate(&o, &ms, 1, &est),
                  ECUIO_ERR_LOAD_FAILED);
    }

    /* Declared in events.log's header: the same limit */
    {
        std::string header = "profile  obd2\n";
        for (size_t i = 0; i < 16385; ++i) {
            header += "signal  H" + std::to_string(i) + ",number,,0,0,0\n";
        }
        Folder declared;
        recording(declared, "", "1000,H0,1,\n", header);
        WideExported d(declared);
        EXPECT_EQ(d.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_NE(d.detail.find("limited to 16384 columns"), std::string::npos)
            << d.detail;
    }

    /* Named: the limit holds for the picks, which the survey reads first */
    std::vector<std::string> store;
    for (size_t i = 0; i < 16385; ++i) {
        store.push_back("S" + std::to_string(i));
    }
    std::vector<const char*> picks;
    for (size_t i = 0; i < 16384; ++i) {
        picks.push_back(store[i].c_str());
    }
    Folder few;
    recording(few, "", names(3));
    WideExported named(few, picks, 0, ECUIO_UNITS_METRIC, 0, 0, 0, "n.csv");
    ASSERT_EQ(named.rc, ECUIO_OK) << named.detail;
    picks.push_back(store[16384].c_str());
    WideExported named_over(few, picks, 0, ECUIO_UNITS_METRIC, 0, 0, 0,
                            "o.csv");
    EXPECT_EQ(named_over.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(named_over.detail.find("limited to 16384 columns"),
              std::string::npos)
        << named_over.detail;
    EXPECT_FALSE(fs::exists(few.path / "o.csv"));
}

/* The estimate takes 64 intervals in a call, no more, and refuses the
 * 65th before it allocates or reads anything */
TEST(ExportTest, TheEstimateRefusesMoreThanSixtyFourIntervals)
{
    Folder f;
    recording(f, "", polled_rows(5));
    const std::string folder = f.utf8("rec");
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.folder_utf8 = folder.c_str();
    std::vector<uint32_t> ms(65, 100);
    std::vector<ecuio_export_estimate_t> est(65);
    EXPECT_EQ(ecuio_export_estimate(&o, ms.data(), 64, est.data()), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_GT(est[63].rows, 0u);
    est[0].rows = 7;
    EXPECT_EQ(ecuio_export_estimate(&o, ms.data(), 65, est.data()),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(est[0].rows, 7u) << "refused before out was touched";
    /* A count no array could back: refused before any allocation */
    EXPECT_EQ(ecuio_export_estimate(&o, ms.data(), 0xFFFFFFFFu, est.data()),
              ECUIO_ERR_INVALID_ARGUMENT);
}

/* Each file rewritten by its classes: the URI's port and a key that
 * isn't core's out, the vehicle's label, the caller's other line and
 * its note out, the VIN reply's frame and the VIN's value out, the
 * kept rows as recorded (a message delivered whole one row, a frame the
 * pcapng couldn't carry, signals.csv's byte order mark), the user's
 * file out and counted, the export's own line before the columns */
TEST(FolderExportTest, EachFileIsCopiedThroughItsClasses)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.size(), 5u);
    EXPECT_EQ(e.files["events.log"],
              std::string(kHead) +
                  export_line("a scrubbed copy", 4, 1, 2, 1, 2, 2, 1) +
                  kColumns +
                  "\n"
                  "22:13:22.250  +2.000  link  vehicle not responding "
                  "(state 8, 1 episode)\n"
                  "22:13:23.250  +3.000  asleep  Asleep for 1.0 s: nothing "
                  "was read meanwhile\n"
                  "22:13:24.250  +4.000  end  <why>: 3 frames, 2 values, 0 "
                  "records dropped, 0 notes dropped\n");
    EXPECT_EQ(e.files["frames.csv"],
              "host_us,timestamp_us,id,flags,data\n" +
                  row(1000000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}) +
                  row(1500000, 0x7E8, kWhole, kWholeReply) +
                  row(3000000, 0x123, 0, {0x01, 0x02, 0x03}) +
                  row(3200000, 0x456, 0, kTwelve));
    EXPECT_EQ(e.files["signals.csv"],
              std::string(kBomHeader) +
                  "1000000,RPM,1726,rpm\n3000000,RPM,1730,rpm\n");
    EXPECT_EQ(e.files["markers.csv"],
              "host_us,label\n1500000,\"pull, 2nd gear\"\n9000000,late\n");
    EXPECT_EQ(e.files["latency.csv"],
              "t_s,ecu,request,answered\n1.000,7E8,01 0C,5\n3.000,7E8,01 "
              "0C,9\n");
    EXPECT_EQ(e.report.frames, 4u);
    EXPECT_EQ(e.report.made_up, 0u);
    EXPECT_EQ(e.report.values, 2u);
    EXPECT_EQ(e.report.signal_count, 1u);
    EXPECT_EQ(e.report.files, 5u);
    EXPECT_EQ(e.report.files_left_out, 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 2u);
    EXPECT_FALSE(partial_left(f.path, "copy"));

    /* The copy is a recording: exported again as CSV, its values are
     * the source's */
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.layout = ECUIO_CSV_LONG;
    const std::string copy = f.utf8("copy");
    const std::string out = f.utf8("copy.csv");
    o.folder_utf8 = copy.c_str();
    o.out_path_utf8 = out.c_str();
    ecuio_export_report_t r{};
    ASSERT_EQ(ecuio_export(&o, &r), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_NE(read_file(f.path / "copy.csv")
                  .find("time_s,signal,unit,value\n1.000000,RPM,rpm,1726\n"),
              std::string::npos);
    /* And wide, the default: the copied focus line picks the columns */
    const std::string wide = f.utf8("copy-wide.csv");
    o.layout = ECUIO_CSV_WIDE;
    o.out_path_utf8 = wide.c_str();
    ASSERT_EQ(ecuio_export(&o, &r), ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<std::string> wl =
        wide_lines(read_file(f.path / "copy-wide.csv"));
    ASSERT_EQ(wl.size(), 3u);
    EXPECT_EQ(wl[0], "Time (ms),RPM (rpm)");
    EXPECT_EQ(wl[1], "1000,1726");
    EXPECT_EQ(wl[2], "3000,1730");
    EXPECT_EQ(r.rows, 2u);
}

/* The header's pick line, a front end's default pick, is copied as the
 * focus line is */
TEST(FolderExportTest, APickLineIsKept)
{
    Folder f;
    recording(f, "", polled_rows(1), "profile  raw\npick  Speed,RPM\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_NE(e.files["events.log"].find("\npick  Speed,RPM\n"),
              std::string::npos)
        << e.files["events.log"];
}

/* An armed recorder's window names its trigger in the header */
TEST(FolderExportTest, AWindowsTriggerLineIsKept)
{
    Folder f;
    recording(f, "", polled_rows(1), "profile  raw\ntrigger  high,2000 ms\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_NE(e.files["events.log"].find("\ntrigger  high,2000 ms\n"),
              std::string::npos)
        << e.files["events.log"];
}

const char* const kConditionsJson =
    "{\"havne_conditions\":1,\"conditions\":[]}\n";

/* A format 3 recording of a session's conditions (ADR-0063 section 8):
 * its copy and header line, cond lines of four keys (one stale; the last
 * before 3.5 s at 3,499,600 us, which its +S.sss rounds to 3.500, its
 * session time 100 us off the others' offset), a link line, a lost
 * events line and the end */
void conditions_recording(const Folder& f, const std::string& extra = "")
{
    const std::string sha =
        ecuio_sha256::hex_of(kConditionsJson, std::strlen(kConditionsJson));
    f.write("rec/events.log",
            std::string("ecuio session recording, format 3 (ADR-0049)\n"
                        "started  2023-11-14 22:13:20.250 (UTC)\n"
                        "utc  2023-11-14T22:13:20.250Z\n"
                        "profile  raw\n"
                        "frame flags  complete\n"
                        "conditions  conditions.json,") +
                sha + "\n" + kColumns +
                "\n"
                "22:13:21.250  +1.000  cond  "
                "coolant_high,on,fired,112,0,1000000,950000\n"
                "22:13:21.750  +1.500  cond  "
                "boost_low,on,fired,-3.5,1,1500000,1450000\n"
                "22:13:22.250  +2.000  cond  "
                "boost_low,on,stale,-3.5,1,2000000,1950000\n"
                "22:13:22.750  +2.500  cond  "
                "coolant_high,off,,98,0,2500000,2450000\n"
                "22:13:23.250  +3.000  cond  knock,on,fired,4,0,3000000,"
                "2950000\n"
                "22:13:23.250  +3.000  cond  knock,off,,0,0,3000200,2950200\n"
                "22:13:23.749  +3.500  cond  idle,on,fired,750,0,3499600,"
                "3449700\n"
                "22:13:24.000  +3.750  link  vehicle not responding (state "
                "8, 1 episode)\n"
                "22:13:24.250  +4.000  cond  "
                "coolant_high,on,fired,113,0,4000000,3950000\n" +
                extra +
                "22:13:24.500  +4.250  drop  the recording lost 2 condition "
                "events: the disk fell behind\n"
                "22:13:25.250  +5.000  cond  boost_low,off,,1,1,5000000,"
                "4950000\n"
                "22:13:25.250  +5.000  end  Disconnected by the user: 5 "
                "frames, 1 values, 0 records dropped, 0 notes dropped\n");
    f.write("rec/frames.csv", "host_us,timestamp_us,id,flags,data\n" +
                                  row(1000000, 0x123, 0, {0x01}) +
                                  row(4000000, 0x123, 0, {0x02}));
    f.write("rec/signals.csv",
            std::string(kBomHeader) + "4000000,Coolant,113,C\n");
    f.write("rec/conditions.json", kConditionsJson);
}

/* The body of a copy's events.log: what follows its blank line */
std::string body_of(const std::string& events)
{
    const size_t at = events.find("\n\n");
    return at == std::string::npos ? std::string() : events.substr(at + 2);
}

/* conditions.json is copied whole and its header line kept, never counted
 * as a file the export doesn't know; every cond line is kept, scrubbed or
 * raw, and the lost events' drop line with them */
TEST(FolderExportTest, TheConditionsCopyAndItsLinesAreKept)
{
    Folder f;
    conditions_recording(f);
    const std::string source = read_file(f.path / "rec" / "events.log");
    for (const uint32_t flags : {0u, static_cast<uint32_t>(ECUIO_EXPORT_RAW)}) {
        FolderExported e(f, flags, 0, 0, flags == 0 ? "copy" : "raw");
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        EXPECT_EQ(e.files["conditions.json"], kConditionsJson) << flags;
        EXPECT_EQ(e.report.files, 4u) << flags;
        EXPECT_EQ(e.report.files_left_out, 0u) << flags;
        EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 0u)
            << flags;
        const std::string& events = e.files["events.log"];
        EXPECT_NE(events.find("\nconditions  conditions.json,"),
                  std::string::npos)
            << events;
        std::string want = body_of(source);
        if (flags == 0) {
            want.replace(want.find("Disconnected by the user"),
                         std::strlen("Disconnected by the user"), "<why>");
        }
        EXPECT_EQ(body_of(events), want) << flags;
    }
}

/* A range opens with a held line for each condition ON at its start, from
 * the lines before it by their own host_us (stale kept, fired dropped),
 * at the range's start in each of its times, ahead of the range's lines;
 * a line past the range is left out, and the copy reads them back */
TEST(FolderExportTest, ARangeOpensWithAHeldLineForEachConditionOnAtItsStart)
{
    Folder f;
    conditions_recording(f);
    FolderExported e(f, 0, 3500000, 4500000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(body_of(e.files["events.log"]),
              "22:13:23.750  +3.500  cond  "
              "boost_low,on,stale held,-3.5,1,3500000,3450100\n"
              "22:13:23.750  +3.500  cond  idle,on,held,750,0,3500000,3450100\n"
              "22:13:24.000  +3.750  link  vehicle not responding (state 8, "
              "1 episode)\n"
              "22:13:24.250  +4.000  cond  "
              "coolant_high,on,fired,113,0,4000000,3950000\n"
              "22:13:24.500  +4.250  drop  the recording lost 2 condition "
              "events: the disk fell behind\n"
              "22:13:25.250  +5.000  end  <why>: 5 frames, 1 values, 0 "
              "records dropped, 0 notes dropped\n");

    ecuio_recording_t* r = nullptr;
    const std::string copy = f.utf8("copy");
    ASSERT_EQ(ecuio_recording_open(copy.c_str(), nullptr, nullptr, &r),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_conditions_t c{};
    ASSERT_EQ(ecuio_recording_get_conditions(r, &c), ECUIO_OK);
    EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_NAMED |
                           ECUIO_RECORDING_CONDITIONS_PRESENT |
                           ECUIO_RECORDING_CONDITIONS_MATCH);
    ecuio_recording_info_t info{};
    ASSERT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_EQ(info.condition_events, 3u);
    ecuio_recording_condition_event_t ev{};
    ASSERT_EQ(ecuio_recording_get_condition_event(r, 0, &ev), ECUIO_OK);
    EXPECT_STREQ(ev.key, "boost_low");
    EXPECT_EQ(ev.flags, ECUIO_CONDITION_HELD | ECUIO_CONDITION_STALE);
    EXPECT_EQ(ev.host_us, 3500000u);
    EXPECT_EQ(ev.timestamp_us, 3450100u);
    ecuio_recording_close(r);

    /* A range holding no line but the end: the held lines go before it */
    FolderExported late(f, 0, 5000001, 0, "late");
    ASSERT_EQ(late.rc, ECUIO_OK) << late.detail;
    EXPECT_EQ(body_of(late.files["events.log"]),
              "22:13:25.250  +5.000  cond  "
              "coolant_high,on,held,113,0,5000001,4950001\n"
              "22:13:25.250  +5.000  cond  idle,on,held,750,0,5000001,4950001\n"
              "22:13:25.250  +5.000  end  <why>: 5 frames, 1 values, 0 "
              "records dropped, 0 notes dropped\n");

    /* The held line's +S.sss rounds half up, as the recorder's prefix */
    FolderExported half(f, 0, 3500500, 4500000, "half");
    ASSERT_EQ(half.rc, ECUIO_OK) << half.detail;
    EXPECT_EQ(body_of(half.files["events.log"])
                  .rfind("22:13:23.750  +3.501  cond  "
                         "boost_low,on,stale held,-3.5,1,3500500,3450600\n",
                         0),
              0u)
        << half.files["events.log"];

    /* From 0, or from before every cond line: nothing held */
    FolderExported early(f, 0, 900000, 0, "early");
    ASSERT_EQ(early.rc, ECUIO_OK) << early.detail;
    EXPECT_EQ(body_of(early.files["events.log"]).find(" held,"),
              std::string::npos);
}

/* A scrubbed copy writes a cond line again from its fields, so a flag word
 * or field the export doesn't know stays out, and leaves out a cond line
 * that doesn't parse; a raw copy keeps both as they are */
TEST(FolderExportTest, AScrubbedCopyKeepsOnlyACondLinesKnownFields)
{
    Folder f;
    const std::string odd = "22:13:24.300  +4.050  cond  "
                            "knock,on,fired blink,5,0,4050000,4000000,"
                            "the user's words\n";
    const std::string bad =
        "22:13:24.400  +4.150  cond  Knock,on,fired,6,0,4150000,4100000\n";
    /* In the range by its +4.500 prefix, past it by its own host_us */
    const std::string past =
        "22:13:24.750  +4.500  cond  knock,off,,0,0,4500400,4450400\n";
    conditions_recording(f, odd + bad + past);
    FolderExported e(f, 0, 4000000, 4500000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string body = body_of(e.files["events.log"]);
    EXPECT_NE(body.find("22:13:24.300  +4.050  cond  "
                        "knock,on,fired,5,0,4050000,4000000\n"),
              std::string::npos)
        << body;
    EXPECT_EQ(body.find("blink"), std::string::npos) << body;
    EXPECT_EQ(body.find("Knock"), std::string::npos) << body;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 1u);
    /* A line at the range's start is in it, not held before it */
    EXPECT_NE(
        body.find("  cond  coolant_high,on,fired,113,0,4000000,3950000\n"),
        std::string::npos)
        << body;
    EXPECT_EQ(body.find("coolant_high,on,held"), std::string::npos) << body;
    EXPECT_EQ(body.find("4500400"), std::string::npos) << body;

    FolderExported raw(f, ECUIO_EXPORT_RAW, 4000000, 4500000, "raw");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    const std::string kept = body_of(raw.files["events.log"]);
    EXPECT_NE(kept.find(odd), std::string::npos) << kept;
    EXPECT_NE(kept.find(bad), std::string::npos) << kept;
    EXPECT_EQ(kept.find(past), std::string::npos) << kept;
}

/* A held line's clock carries on through midnight */
TEST(FolderExportTest, AHeldLinesClockWrapsAtMidnight)
{
    Folder f;
    f.write("rec/events.log",
            std::string("ecuio session recording, format 3 (ADR-0049)\n"
                        "profile  raw\n") +
                kColumns +
                "\n"
                "23:59:59.900  +1.000  cond  late,on,fired,1,0,1000000,"
                "990000\n"
                "00:00:01.000  +2.100  end  done: 0 frames, 1 values, 0 "
                "records dropped, 0 notes dropped\n");
    f.write("rec/signals.csv", std::string(kBomHeader) + "1500000,A,1,\n");
    FolderExported e(f, 0, 1200000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(body_of(e.files["events.log"]),
              "00:00:00.100  +1.200  cond  late,on,held,1,0,1200000,1190000\n"
              "00:00:01.000  +2.100  end  <why>: 0 frames, 1 values, 0 "
              "records dropped, 0 notes dropped\n");
}

/* Cond lines are format 3's alone, as the reader takes them: in a format
 * 2 log one is a kind the export doesn't know, left out of a scrubbed copy
 * and counted, kept by a raw one, and never held */
TEST(FolderExportTest, ACondLineInFormat2IsAKindTheExportDoesntKnow)
{
    Folder f;
    conditions_recording(f);
    std::string events = read_file(f.path / "rec" / "events.log");
    events.replace(0, events.find('\n'),
                   "ecuio session recording, format 2 (ADR-0049)");
    f.write("rec/events.log", events);
    FolderExported e(f, 0, 3500000, 4500000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string body = body_of(e.files["events.log"]);
    EXPECT_EQ(body.find("  cond  "), std::string::npos) << body;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 2u)
        << "idle's line at +3.500 and coolant_high's at +4.000";

    FolderExported raw(f, ECUIO_EXPORT_RAW, 3500000, 4500000, "raw");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    const std::string kept = body_of(raw.files["events.log"]);
    EXPECT_EQ(kept.find(" held,"), std::string::npos) << kept;
    EXPECT_NE(kept.find("cond  coolant_high,on,fired,113,"), std::string::npos)
        << kept;
}

/* A scrubbed copy keeps a copy's header line only as the name and hash
 * the readers take, and an activity line only when its clock's places are
 * digits: text anywhere else there is unchecked. A raw copy keeps the
 * header lines as they are. */
TEST(FolderExportTest, AScrubbedCopyKeepsOnlyHeaderLinesAndClocksThatRead)
{
    Folder f;
    conditions_recording(f, "2x:13:24.350  +4.100  cond  "
                            "knock,on,fired,7,0,4100000,4050000\n"
                            "22:13:24.360  +4.110  link  vehicle not "
                            "responding (state 8, 1 episode)\n");
    std::string events = read_file(f.path / "rec" / "events.log");
    const std::string good = "conditions  conditions.json,";
    const size_t at = events.find(good);
    ASSERT_NE(at, std::string::npos);
    const std::string kept_line = events.substr(at, events.find('\n', at) - at);
    events.insert(at, "conditions  conditions.json,the user's words\n"
                      "signal file  signals.json," +
                          std::string(64, '0') + ",more\n");
    f.write("rec/events.log", events);

    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string& out = e.files["events.log"];
    EXPECT_NE(out.find("\n" + kept_line + "\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("the user's words"), std::string::npos) << out;
    EXPECT_EQ(out.find(",more"), std::string::npos) << out;
    EXPECT_EQ(out.find("2x:13"), std::string::npos) << out;
    EXPECT_NE(out.find("22:13:24.360  +4.110  link  "), std::string::npos)
        << out;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 3u)
        << "two header lines and the line whose clock doesn't read";

    FolderExported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_NE(raw.files["events.log"].find("the user's words"),
              std::string::npos);
}

/* A scrubbed copy keeps the modules' lines (ADR-0065 step 5): the header's
 * module not open at the start and the timed change after it */
TEST(FolderExportTest, AScrubbedCopyKeepsTheModuleLines)
{
    Folder f;
    conditions_recording(f, "22:13:24.360  +4.110  module  0x7E9,open,read\n");
    std::string events = read_file(f.path / "rec" / "events.log");
    const size_t at = events.find("conditions  conditions.json,");
    ASSERT_NE(at, std::string::npos);
    events.insert(at, "module  0x7E9,closed\n");
    f.write("rec/events.log", events);

    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string& out = e.files["events.log"];
    EXPECT_NE(out.find("\nmodule  0x7E9,closed\n"), std::string::npos) << out;
    EXPECT_NE(out.find("22:13:24.360  +4.110  module  0x7E9,open,read"),
              std::string::npos)
        << out;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 0u);
}

/* A range's held lines take a set's most keys, as the reader does: with
 * coolant_high and boost_low first, k61 is the 64th key before the range
 * and held, k62 the 65th and not */
TEST(FolderExportTest, ARangeHoldsNoKeyPastASetsMost)
{
    Folder f;
    std::string lines;
    for (int k = 0; k < 63; ++k) {
        char line[96];
        std::snprintf(line, sizeof line,
                      "22:13:21.250  +1.000  cond  k%02d,on,fired,1,0,%d,%d\n",
                      k, 1000000 + k, 950000 + k);
        lines += line;
    }
    conditions_recording(f, lines);
    FolderExported e(f, 0, 2000000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string body = body_of(e.files["events.log"]);
    EXPECT_NE(body.find("  cond  k61,on,held,"), std::string::npos) << body;
    EXPECT_EQ(body.find("  cond  k62,on,held,"), std::string::npos) << body;
}

/* A focus change's lines, a pick's and an ended focus's, are kept in
 * the range as the link lines are, scrubbed or raw; the notes beside
 * them are not. A range opens with the last change before it, its lines
 * at the range's start, ahead of the range's own */
TEST(FolderExportTest, AFocusChangesLinesAreKeptInTheRange)
{
    Folder f;
    recording(f, "", polled_rows(1));
    const std::string pick =
        "22:13:21.250  +1.000  pick  2 signals: Speed,RPM\n";
    const std::string more = "22:13:21.250  +1.000  pick  more: \"a,b\"\n";
    const std::string none =
        "22:13:22.250  +2.000  focus  none: every signal asked\n";
    const std::string note = "22:13:22.250  +2.000  note  Asking 2 signals\n";
    const std::string link = "22:13:22.850  +2.600  link  vehicle not "
                             "responding (state 8, 1 episode)\n";
    const std::string end = "22:13:24.250  +4.000  end  Disconnected by the "
                            "user: 3 frames, 2 values, 0 records dropped, 0 "
                            "notes dropped\n";
    f.write("rec/events.log",
            events_log() + pick + more + none + note + link + end);
    FolderExported whole(f);
    ASSERT_EQ(whole.rc, ECUIO_OK) << whole.detail;
    const std::string copied = whole.files["events.log"];
    EXPECT_NE(copied.find(pick + more + none), std::string::npos) << copied;
    EXPECT_EQ(copied.find("note  Asking"), std::string::npos) << copied;

    const std::string held =
        "22:13:21.750  +1.500  pick  2 signals: Speed,RPM\n"
        "22:13:21.750  +1.500  pick  more: \"a,b\"\n";
    for (uint32_t flags : {0u, static_cast<uint32_t>(ECUIO_EXPORT_RAW)}) {
        FolderExported ranged(f, flags, 1500000, 2500000,
                              flags == 0 ? "ranged" : "ranged-raw");
        ASSERT_EQ(ranged.rc, ECUIO_OK) << ranged.detail;
        const std::string in_range = ranged.files["events.log"];
        EXPECT_NE(in_range.find(held + none), std::string::npos) << in_range;
        EXPECT_EQ(in_range.find("+1.000  pick"), std::string::npos) << in_range;
    }

    /* The last change before the range is the one held, ahead of a link
     * line or the end line that opens the range */
    FolderExported late(f, 0, 2500400, 2800000, "late");
    ASSERT_EQ(late.rc, ECUIO_OK) << late.detail;
    const std::string after = late.files["events.log"];
    EXPECT_NE(after.find("\n22:13:22.750  +2.500  focus  none: every signal "
                         "asked\n" +
                         link),
              std::string::npos)
        << after;
    EXPECT_EQ(after.find("  pick  "), std::string::npos) << after;
    EXPECT_EQ(after.find("+2.000  focus"), std::string::npos) << after;
    FolderExported last(f, 0, 3000000, 0, "last");
    ASSERT_EQ(last.rc, ECUIO_OK) << last.detail;
    EXPECT_NE(last.files["events.log"].find(
                  "\n22:13:23.250  +3.000  focus  none: every signal asked\n"
                  "22:13:24.250  +4.000  end  <why>: "),
              std::string::npos)
        << last.files["events.log"];

    /* A range before any change holds nothing */
    FolderExported early(f, 0, 500000, 900000, "early");
    ASSERT_EQ(early.rc, ECUIO_OK) << early.detail;
    EXPECT_EQ(early.files["events.log"].find("  pick  "), std::string::npos);
    EXPECT_EQ(early.files["events.log"].find("  focus  "), std::string::npos);
}

} // namespace
