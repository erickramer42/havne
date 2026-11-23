// File/layer: tests/test_export_range.cpp | test_core, Qt-free, C++17
// Testing: UNIT: FolderExportTest, ScrubFramesTest, ExportTest (test_core)

#include "test_export_support.h"

namespace {

/* A range takes each file's rows and lines in it alone; the end line
 * stays, the header whole */
TEST(FolderExportTest, ARangeTakesEachFilesRowsInItAlone)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f, 0, 1500000, 3500000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files["events.log"],
              std::string(kHead) +
                  export_line("a scrubbed copy", 3, 1, 1, 1, 1, 1, 1) +
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
                  row(1500000, 0x7E8, kWhole, kWholeReply) +
                  row(3000000, 0x123, 0, {0x01, 0x02, 0x03}) +
                  row(3200000, 0x456, 0, kTwelve));
    EXPECT_EQ(e.files["signals.csv"],
              std::string(kBomHeader) + "3000000,RPM,1730,rpm\n");
    EXPECT_EQ(e.files["markers.csv"],
              "host_us,label\n1500000,\"pull, 2nd gear\"\n");
    EXPECT_EQ(e.files["latency.csv"],
              "t_s,ecu,request,answered\n3.000,7E8,01 0C,9\n");
}

/* Raw keeps every row and line as recorded, the user's file still out */
TEST(FolderExportTest, ARawCopyKeepsEveryRowAndLine)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f, ECUIO_EXPORT_RAW);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string source = read_file(f.path / "rec" / "events.log");
    const size_t columns = source.find("columns  ");
    EXPECT_EQ(e.files["events.log"],
              source.substr(0, columns) +
                  export_line("a raw copy, not scrubbed", 5, 0, 3, 0, 2, 0, 1) +
                  source.substr(columns));
    EXPECT_EQ(e.files["frames.csv"], read_file(f.path / "rec" / "frames.csv"));
    EXPECT_EQ(e.files["signals.csv"],
              read_file(f.path / "rec" / "signals.csv"));
    EXPECT_EQ(e.files.count("notes.txt"), 0u);
    EXPECT_EQ(e.report.files_left_out, 1u);
}

/* A mock's label is the user's words; its profile and DID list stay, so
 * the copy decodes as the source did */
TEST(FolderExportTest, AMocksLabelIsLeftOutOfItsUri)
{
    Folder f;
    recording(f, row(1000, 0x7E8, 0, {0x02, 0x41, 0x00}), "",
              "uri  mock://Daily driver?profile=uds-generic&did=D802:text,"
              "D803:u16\nprofile  uds-generic\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_NE(e.files["events.log"].find(
                  "\nuri  mock://<label>?profile=uds-generic&did=D802:text,"
                  "D803:u16\n"),
              std::string::npos)
        << e.files["events.log"];
    EXPECT_EQ(e.files["events.log"].find("Daily"), std::string::npos);
}

/* The session keys are core's own, kept in the copy's URI; a value that
 * isn't plain is dropped with its key */
TEST(FolderExportTest, TheSessionKeysAreKeptInTheUri)
{
    Folder f;
    recording(f, row(1000, 0x7E8, 0, {0x02, 0x41, 0x00}), "",
              "uri  elm327://COM3?profile=uds-generic&did=F40C&session=03&"
              "keepalive_ms=2000&packframes=1\nprofile  uds-generic\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_NE(e.files["events.log"].find(
                  "\nuri  elm327://<address>?profile=uds-generic&did=F40C&"
                  "session=03&keepalive_ms=2000&packframes=1\n"),
              std::string::npos)
        << e.files["events.log"];

    Folder g;
    recording(g, row(1000, 0x7E8, 0, {0x02, 0x41, 0x00}), "",
              "uri  elm327://COM3?profile=uds-generic&session=0 3\n"
              "profile  uds-generic\n");
    FolderExported d(g);
    ASSERT_EQ(d.rc, ECUIO_OK) << d.detail;
    EXPECT_NE(d.files["events.log"].find(
                  "\nuri  elm327://<address>?profile=uds-generic\n"),
              std::string::npos)
        << d.files["events.log"];

    /* J2534's hold for Known Issue #42 (?hold=) is kept too */
    Folder h;
    recording(h, row(1000, 0x7E8, 0, {0x02, 0x41, 0x00}), "",
              "uri  j2534://C:/Vendor/vx.dll?profile=obd2&hold=0\n"
              "profile  obd2\n");
    FolderExported j(h);
    ASSERT_EQ(j.rc, ECUIO_OK) << j.detail;
    EXPECT_NE(j.files["events.log"].find(
                  "\nuri  j2534://<path>?profile=obd2&hold=0\n"),
              std::string::npos)
        << j.files["events.log"];
}

/* A log of another format (format 1's, before ADR-0049) is a file the
 * export doesn't know: left out and counted, the frames still copied */
TEST(FolderExportTest, AnEventsLogOfAnotherFormatIsLeftOut)
{
    Folder f;
    recording(f, row(1000, 0x123, 0, {0x01}));
    f.write("rec/events.log", "Havne session recording (ADR-0017)\n"
                              "uri  elm327://COM3\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.size(), 1u);
    EXPECT_EQ(e.files.count("frames.csv"), 1u);
    EXPECT_EQ(e.report.files, 1u);
    EXPECT_EQ(e.report.files_left_out, 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 0u);
}

/* A value the classes don't catch, in any file, fails the whole copy:
 * the recording's VIN typed as a marker's label (another VIN's shape
 * there is masked: test_export_mask.cpp), a calibration ID in
 * signals.json, a DID echoing the VIN across the frames of a segmented
 * reply (which no row holds whole, so the check reassembles it), a VIN's
 * shape in a message delivered whole (each message checked as bytes, not
 * only the rows as text) */
TEST(FolderExportTest, AnIdentityValueInAnyFileFailsTheCopy)
{
    {
        SCOPED_TRACE("the recording's VIN as a marker's label");
        Folder f;
        whole_recording(f);
        f.write("rec/markers.csv",
                std::string("host_us,label\n1500000,") + kVin + "\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        /* the VIN signal's value too, which the check meets first */
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);
        EXPECT_EQ(e.detail, "the check found a text signal's value in the "
                            "copy's markers.csv: nothing was written");
        EXPECT_EQ(e.report.files, 0u);
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    {
        SCOPED_TRACE("a DID echoing the VIN as it crossed the bus");
        Folder f;
        recording(
            f,
            row(1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)) +
                row(2000, 0x7E8, 0,
                    {0x10, 0x14, 0x62, 0xD8, 0x01, 'Z', 'Z', 'Z'}) +
                row(2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}) +
                row(2200, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}) +
                row(2300, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}),
            "2000,RPM,1726,rpm\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail,
                  "the check found an identification reply's value in a "
                  "message the export would write: nothing was written");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    {
        SCOPED_TRACE("a VIN's shape in a message delivered whole, which "
                     "frames.csv holds only as hex");
        Folder f;
        recording(
            f, row(2000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x01}, kVin)),
            "2000,RPM,1726,rpm\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(e.detail,
                  "the check found a VIN-shaped token in a message the export "
                  "would write: nothing was written");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    {
        SCOPED_TRACE("a calibration ID in signals.json");
        Folder f;
        recording(f,
                  row(1000, 0x7E8, kWhole,
                      text_bytes({0x49, 0x04, 0x01}, "FAKECAL-01")),
                  "2000,RPM,1726,rpm\n");
        f.write("rec/signals.json",
                R"({"havne_signals":1,"name":"T","notes":"FAKECAL-01",)"
                R"("source":{"name":"t"},"ecus":[{"id":"pcm","rx":"7E8"}],)"
                R"("signals":[{"key":"a","name":"A","read":{"service":"22",)"
                R"("number":"D100"},"field":{"byte":0}}]})"
                "\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail, "the check found an identification reply's "
                            "value in the copy's signals.json: nothing was "
                            "written");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
}

/* A folder that exists, a source with no recording, a bad row, a name
 * or unit a spreadsheet would run: nothing is written */
TEST(FolderExportTest, ARefusedCopyWritesNothing)
{
    Folder f;
    whole_recording(f);
    fs::create_directories(f.path / "copy");
    FolderExported there(f);
    EXPECT_EQ(there.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(there.detail, "the folder exists already");

    Folder empty;
    FolderExported none(empty);
    EXPECT_EQ(none.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(none.detail, "the folder has no frames.csv or signals.csv");
    EXPECT_FALSE(fs::exists(empty.path / "copy"));

    Folder bad;
    whole_recording(bad);
    bad.write("rec/latency.csv", "t_s,ecu\nsoon,7E8\n");
    FolderExported b(bad);
    EXPECT_EQ(b.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(b.detail, "latency.csv line 2: t_s isn't a time in seconds");
    EXPECT_FALSE(partial_left(bad.path, "copy"));

    Folder formula;
    recording(formula, "", "1000,=RPM,1726,rpm\n");
    FolderExported s(formula);
    EXPECT_EQ(s.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(s.detail.find("starts as a spreadsheet formula"),
              std::string::npos)
        << s.detail;
    FolderExported raw(formula, ECUIO_EXPORT_RAW);
    EXPECT_EQ(raw.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_FALSE(fs::exists(formula.path / "copy"));
}

/* The VIN reply delivered whole, then as it crossed the bus around the
 * tester's request and Flow Control, then a value */
TEST(ScrubFramesTest, AnIdentificationReplyIsLeftOutEveryFrameOfIt)
{
    Source s;
    s.frames = {
        {1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)},
        {2000, 0x7DF, 0, {0x02, 0x09, 0x02, 0, 0, 0, 0, 0}},
        {2100, 0x7E8, 0, {0x10, 0x14, 0x49, 0x02, 0x01, 'Z', 'Z', 'Z'}},
        {2200, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {2300, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}},
        {2400, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}},
        {3000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 1, 0, 1, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 4u);
    EXPECT_EQ(r.report.frames, 3u);
    EXPECT_EQ(r.report.made_up, 0u);
    EXPECT_EQ(r.report.failed_class, 0u);
    EXPECT_EQ(s.calls, 2 * s.frames.size()) << "each frame asked for twice";
}

TEST(ScrubFramesTest, ASegmentNoMessageCanBeAttributedToIsLeftOutOnAReplyId)
{
    Source s;
    s.frames = {
        /* its First Frame came before the ring's oldest frame */
        {1000, 0x7E8, 0, {0x21, 'S', 'E', 'C', 'R', 'E', 'T', '1'}},
        /* a First Frame never finished */
        {2000, 0x7E9, 0, {0x10, 0x0A, 0x62, 0xD8, 0x01, 'A', 'B', 'C'}},
        /* on a request ID: as it is */
        {3000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}},
        /* delivered whole but cut short: a row holds it as it is */
        {4000, 0x7E8, kWhole | ECUIO_FRAME_FLAG_TRUNCATED, {0x62, 0xD8}},
        {5000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 1, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 2u);
    EXPECT_EQ(r.report.frames, 3u);
}

/* A text or hex def of the caller's set: its messages left out, whole
 * or segmented; with no set they stay */
TEST(ScrubFramesTest, ATextDefsMessagesAreLeftOutByTheSetsDef)
{
    const std::string json =
        R"({"havne_signals": 1, "name": "Scrub test", )"
        R"("source": {"name": "test_export"}, )"
        R"("ecus": [{"id": "pcm", "rx": "7E8"}], )"
        R"("signals": [{"key": "ecu_name", "name": "ECU name", "ecu": "pcm", )"
        R"("read": {"service": "22", "number": "D802"}, )"
        R"("field": {"byte": 0}, "kind": "text"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    Source s;
    s.frames = {
        {1000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x02}, "ECM-HavneSim")},
        {2000, 0x7E8, 0, {0x10, 0x0F, 0x62, 0xD8, 0x02, 'E', 'C', 'M'}},
        {2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {2200, 0x7E8, 0, {0x21, '-', 'H', 'a', 'v', 'n', 'e', 'S'}},
        {2300, 0x7E8, 0, {0x22, 'i', 'm', 0, 0, 0, 0, 0}},
        {3000, 0x7E8, kWhole, {0x62, 0xD8, 0x03, 0x01, 0x02}},
    };
    Scrubbed r(s, ecuio_signal_file_set(file));
    EXPECT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 4u);
    EXPECT_EQ(r.report.frames, 2u);

    Scrubbed all(s);
    EXPECT_EQ(all.rc, ECUIO_OK) << all.detail;
    EXPECT_EQ(all.keep, std::vector<uint8_t>(6, 1));
    ecuio_signal_file_free(file);
}

/* The check: a DID echoing the VIN, whole or segmented, and a VIN shape
 * with no identification reply; nothing kept, the class alone named */
TEST(ScrubFramesTest, AnIdentityValueOrAVinShapeInAKeptFrameFailsIt)
{
    const Mem vin_reply{1000, 0x7E8, kWhole,
                        text_bytes({0x49, 0x02, 0x01}, kVin)};
    const Mem echo{2000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x01}, kVin)};
    {
        SCOPED_TRACE("delivered whole");
        Source s;
        s.frames = {vin_reply, echo};
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(r.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(r.report.frames, 0u);
        EXPECT_EQ(r.keep, none_kept(2));
        EXPECT_EQ(r.detail,
                  "the check found an identification reply's value in a "
                  "frame the export would write: nothing was written");
    }
    {
        SCOPED_TRACE("as it crossed the bus, after the reply");
        Source s;
        s.frames = {
            {500, 0x7E8, 0, {0x10, 0x14, 0x62, 0xD8, 0x01, 'Z', 'Z', 'Z'}},
            {600, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
            {700, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}},
            {800, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}},
            vin_reply,
        };
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(r.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(r.keep, none_kept(5));
    }
    {
        SCOPED_TRACE("with no identification reply: the VIN's shape");
        Source s;
        s.frames = {echo};
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(r.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(r.detail,
                  "the check found a VIN-shaped token in a frame the export "
                  "would write: nothing was written");
        EXPECT_EQ(r.detail.find(kVin), std::string::npos);
        EXPECT_EQ(r.keep, none_kept(1));
    }
}

TEST(ScrubFramesTest, ArgumentsACancelAndAFrameTooLongAreRefused)
{
    Source s;
    s.frames = {{1000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
                {2000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF9}}};
    uint8_t keep[2] = {7, 7};
    ecuio_export_report_t r{};
    r.frames = 9;
    EXPECT_EQ(ecuio_scrub_frames(2, nullptr, &s, nullptr, 0, keep, &r),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.frames, 0u);
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 1, keep, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(s.calls, 0u) << "refused before any frame is asked for";
    EXPECT_EQ(ecuio_scrub_frames(0, give, &s, nullptr, 0, nullptr, &r),
              ECUIO_OK);
    EXPECT_EQ(r.frames, 0u);

    s.cancel_at = 3; /* the second pass's second frame */
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, keep, &r),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(keep[0] + keep[1], 0);
    EXPECT_EQ(s.calls, 4u);

    s.cancel_at = SIZE_MAX;
    s.calls = 0;
    s.length_past = ECUIO_FRAME_DATA_MAX + 1;
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, keep, &r),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "frame 0 holds more bytes than a frame can");

    s.length_past = 0;
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, keep, &r), ECUIO_OK);
    EXPECT_EQ(keep[0] + keep[1], 2);
    EXPECT_EQ(r.frames, 2u);
}

// ---- a message torn every way, and frames no message reads ----

/* A serial's reply (62 F1 8C, 33 bytes) as it crossed the bus, a Single
 * Frame of length 0 (malformed) between its first Consecutive Frame and
 * its second */
std::vector<Mem> serial_with_a_malformed_frame()
{
    const std::vector<uint8_t> msg =
        text_bytes({0x62, 0xF1, 0x8C}, "SERIALFAKE-0123456789ABCDEFGHI");
    auto cf = [&msg](uint8_t sn, size_t at) {
        std::vector<uint8_t> d{static_cast<uint8_t>(0x20 | sn)};
        for (size_t k = 0; k < 7; ++k) {
            d.push_back(at + k < msg.size() ? msg[at + k] : 0);
        }
        return d;
    };
    return {
        {1000,
         0x7E8,
         0,
         {0x10, 0x21, msg[0], msg[1], msg[2], msg[3], msg[4], msg[5]}},
        {2000, 0x7E8, 0, cf(1, 6)},
        {3000, 0x7E8, 0, {0, 0, 0, 0, 0, 0, 0, 0}},
        {4000, 0x7E8, 0, cf(2, 13)},
        {5000, 0x7E8, 0, cf(3, 20)},
        {6000, 0x7E8, 0, cf(4, 27)},
        {7000, 0x7E0, 0, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}},
    };
}

/* The malformed frame ends the message under way, so every frame of it
 * is torn: none of them pends into a message no frame list holds */
TEST(ScrubFramesTest, AMalformedFrameInAMessageTearsAllOfIt)
{
    Source s;
    s.frames = serial_with_a_malformed_frame();
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 6u);
    EXPECT_EQ(r.report.frames, 1u);
}

TEST(FolderExportTest, AMalformedFrameInAMessageTearsAllOfIt)
{
    Folder f;
    std::string rows;
    for (const Mem& m : serial_with_a_malformed_frame()) {
        rows += row(m.t, m.id, m.flags, m.data);
    }
    recording(f, rows);
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files["frames.csv"],
              "host_us,timestamp_us,id,flags,data\n" +
                  row(7000, 0x7E0, 0, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}));
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 6u);
}

/* Torn segments on any 18DA ID: a reply to a tester at F9, and one at F1
 * (whose First Frames came before the frames began) */
TEST(ScrubFramesTest, ATornSegmentOnAny18DaIdIsLeftOut)
{
    const uint32_t x = ECUIO_FRAME_FLAG_EXTENDED;
    Source s;
    s.frames = {
        {1000, 0x18DAF910, x, text_bytes({0x21}, "FAKE11M")},
        {2000, 0x18DAF910, x, text_bytes({0x22}, "0000001")},
        {3000, 0x18DAF110, x, text_bytes({0x21}, "FAKE11M")},
        {4000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 3u);
}

TEST(ExportTest, ATornSegmentOnAny18DaIdIsLeftOut)
{
    const uint32_t x = ECUIO_FRAME_FLAG_EXTENDED;
    Folder f;
    recording(f, row(1000, 0x18DAF910, x, text_bytes({0x21}, "FAKE11M")) +
                     row(2000, 0x18DAF910, x, text_bytes({0x22}, "0000001")) +
                     row(3000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 2u);
    ASSERT_EQ(e.pcap.packets.size(), 1u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x7E0u);
    EXPECT_EQ(e.bytes.find("FAKE11M"), std::string::npos);
}

/* A CAN FD frame on a reply ID, longer than classic ISO-TP's: its ISO-TP
 * form isn't read, so it's left out; one on another ID is kept */
TEST(ScrubFramesTest, AnFdFrameOnAReplyIdIsLeftOut)
{
    const uint32_t fd = ECUIO_FRAME_FLAG_FD;
    Source s;
    s.frames = {
        {1000, 0x7E8, fd,
         text_bytes({0x00, 0x13, 0x49, 0x04, 0x01}, "CALIDFAKE0000001")},
        {2000, 0x18DAF110, fd | ECUIO_FRAME_FLAG_EXTENDED,
         text_bytes({0x00, 0x13, 0x49, 0x04, 0x01}, "CALIDFAKE0000002")},
        {3000, 0x123, fd, std::vector<uint8_t>(12, 0x55)},
        {4000, 0x7E8, fd, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 2u);
}

TEST(ExportTest, AnFdFrameOnAReplyIdIsLeftOut)
{
    Folder f;
    recording(
        f, row(1000, 0x7E8, ECUIO_FRAME_FLAG_FD,
               text_bytes({0x00, 0x13, 0x49, 0x04, 0x01}, "CALIDFAKE0000001")) +
               row(2000, 0x123, ECUIO_FRAME_FLAG_FD,
                   std::vector<uint8_t>(12, 0x55)));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 1u);
    ASSERT_EQ(e.pcap.packets.size(), 1u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x123u);
    EXPECT_EQ(e.bytes.find("CALIDFAKE"), std::string::npos);
    /* raw keeps it */
    Exported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.pcapng");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_EQ(raw.pcap.packets.size(), 2u);
}

/* A hex def's value is searched as its bytes: three zero bytes are too
 * short to search for, so a Flow Control's zero padding doesn't fail the
 * copy; six bytes are searched, in every form */
TEST(FolderExportTest, AHexValueIsSearchedAsItsBytes)
{
    const std::string header = "profile  raw\nsignal  DID D100,text,,0,0\n";
    {
        Folder f;
        recording(f, row(2000, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}),
                  "1000,DID D100,00 00 00,\n", header);
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_OK) << e.detail;
    }
    {
        Folder f;
        recording(f,
                  row(2000, 0x7E0, 0,
                      {0x07, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x00}),
                  "1000,DID D100,de ad be ef 01 02,\n", header);
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);
    }
}

} // namespace
