// File/layer: tests/test_export_mask.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvExportTest, FolderExportTest (test_core)
//
// A VIN-shaped token in text a user wrote (a signal's name, a marker's
// label, a set's or a condition file's name) is masked by a scrubbed CSV
// and a folder copy, counted, and never fails the export: in every row, over
// several write chunks, in the wide CSV's head; the folder copy's JSON files
// masked as its CSVs are, their hashes in events.log the copy's own, a note
// beside them, and the copy opens. The recording's own VIN in a name, and an
// identification value a mask would make, still fail it; a raw export masks
// nothing. The VINs are made up.

#include "test_export_support.h"

namespace {

/* A plugin-style name holding a VIN inside a longer run */
const std::string kName = std::string("PN_") + kVin + "x";
const std::string kMaskedName = "PN_" + std::string(17, '*') + "x";

std::string masked_vin(std::string s)
{
    for (size_t at = s.find(kVin); at != std::string::npos;
         at = s.find(kVin, at)) {
        s.replace(at, 17, std::string(17, '*'));
    }
    return s;
}

TEST(CsvExportTest, AVinShapedNameIsMaskedInEveryRow)
{
    Folder f;
    recording(f, "",
              "1000," + kName + ",5,rpm\n2000,RPM,1726,rpm\n3000," + kName +
                  ",6,rpm\n4000," + std::string(kVin) + ",7,\n");
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(unbommed(e.bytes),
              csv_head("a scrubbed export",
                       "4 values of 3 signals, 0.001000 s to 0.004000 s", 0,
                       0) +
                  "0.001000," + kMaskedName + ",rpm,5\n" +
                  "0.002000,RPM,rpm,1726\n" + "0.003000," + kMaskedName +
                  ",rpm,6\n" + "0.004000," + std::string(17, '*') + ",,7\n");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 3u);
    EXPECT_EQ(e.report.failed_class, 0u);
    EXPECT_EQ(e.bytes.find(kVin), std::string::npos);

    /* the same export again: the same bytes */
    CsvExported again(f, 0, 0, 0, "again.csv");
    ASSERT_EQ(again.rc, ECUIO_OK) << again.detail;
    EXPECT_EQ(again.bytes, e.bytes);

    CsvExported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.csv");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_NE(raw.bytes.find(kName), std::string::npos);
    EXPECT_EQ(raw.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 0u);
}

/* A file of several 64 KiB write chunks: the name masked in every row,
 * every row counted */
TEST(CsvExportTest, ANameInRowsOverSeveralWriteChunksIsMaskedInEach)
{
    Folder f;
    std::string rows;
    const int n = 5000;
    for (int i = 0; i < n; ++i) {
        rows += std::to_string(1000 + i) + "," + kName + "," +
                std::to_string(i % 97) + ",rpm\n";
    }
    recording(f, "", rows);
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    ASSERT_GT(e.bytes.size(), size_t{2} << 16);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED],
              static_cast<uint64_t>(n));
    EXPECT_EQ(e.bytes.find(kVin), std::string::npos);
    size_t masked = 0;
    for (size_t at = e.bytes.find(kMaskedName); at != std::string::npos;
         at = e.bytes.find(kMaskedName, at + 1)) {
        ++masked;
    }
    EXPECT_EQ(masked, static_cast<size_t>(n));
}

TEST(CsvExportTest, WideAVinShapedNameIsMaskedInItsHead)
{
    Folder f;
    recording(f, "",
              "1000," + kName + ",5,rpm\n1500,RPM,1726,rpm\n2000," + kName +
                  ",6,rpm\n2500,RPM,1730,rpm\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> lines = wide_lines(e.bytes);
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines[0], "Time (ms)," + kMaskedName + " (rpm),RPM (rpm)");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 1u);
    EXPECT_EQ(e.bytes.find(kVin), std::string::npos);
}

/* The recording's own VIN is its identity value, and fails the export
 * wherever it stands: a mask never hides one */
TEST(CsvExportTest, TheRecordingsOwnVinInANameStillFails)
{
    Folder f;
    recording(f, row(1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)),
              "2000," + kName + ",5,rpm\n");
    CsvExported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(e.detail, "the check found an identification reply's value in "
                        "the export's file: nothing was written");
    expect_no_file(f, "out.csv");
}

/* An identification value a mask would make (stars where a VIN stood)
 * fails the export too: the check reads the bytes as written */
TEST(CsvExportTest, AnIdentityValueAMaskWouldMakeFails)
{
    Folder f;
    recording(f,
              row(1000, 0x7E8, kWhole,
                  text_bytes({0x62, 0xF1, 0x8C}, kMaskedName)),
              "2000," + kName + ",5,rpm\n");
    CsvExported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(e.detail, "the check found an identification reply's value in "
                        "the export's file: nothing was written");
    expect_no_file(f, "out.csv");
}

std::string signals_json(const std::string& name)
{
    return R"({"havne_signals":1,"name":"T","source":{"name":"t"},)"
           R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[{"key":"a",)"
           R"("name":")" +
           name +
           R"(","read":{"service":"22","number":"D100"},)"
           R"("field":{"byte":0}}]})"
           "\n";
}

std::string conditions_json(const std::string& name)
{
    return R"({"havne_conditions":1,"name":"Pulls )" + name +
           R"(","conditions":[]})"
           "\n";
}

std::string sha_of(const std::string& s)
{
    return ecuio_sha256::hex_of(s.data(), s.size());
}

/* A recording whose signal file, condition file, signals.csv, events.log
 * and markers.csv each hold a VIN-shaped token of a user's text */
void named_recording(const Folder& f)
{
    const std::string signals = signals_json(kName);
    const std::string conditions = conditions_json(kVin);
    f.write("rec/events.log",
            "ecuio session recording, format 3 (ADR-0049)\n"
            "started  2023-11-14 22:13:20.250 (UTC)\n"
            "utc  2023-11-14T22:13:20.250Z\n"
            "profile  raw\n"
            "frame flags  complete\n"
            "signal file  signals.json," +
                sha_of(signals) + "\nconditions  conditions.json," +
                sha_of(conditions) + "\nsignal  " + kName +
                ",number,,0,0\n" + kColumns +
                "\n"
                "22:13:24.250  +4.000  end  Disconnected by the user: 1 "
                "frames, 2 values, 0 records dropped, 0 notes dropped\n");
    f.write("rec/frames.csv", "host_us,timestamp_us,id,flags,data\n" +
                                  row(1000000, 0x123, 0, {0x01}));
    f.write("rec/signals.csv", std::string(kBomHeader) + "1000000," + kName +
                                   ",5,\n2000000," + kName + ",6,\n");
    f.write("rec/markers.csv", "host_us,label\n1500000," +
                                   std::string(kVin) + "\n");
    f.write("rec/signals.json", signals);
    f.write("rec/conditions.json", conditions);
}

/* Every file of a user's text masked alike, so the copy's names agree;
 * its JSON files' hashes in events.log are the copy's own, so it opens
 * with them; the note says what the masks are; exported again, the copy
 * masks nothing more and keeps the note */
TEST(FolderExportTest, AMaskedCopyOpensAndItsNamesAgree)
{
    Folder f;
    named_recording(f);
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.size(), 7u);
    for (const auto& file : e.files) {
        EXPECT_EQ(file.second.find(kVin), std::string::npos) << file.first;
    }
    /* events.log's signal line, two rows, a label, the two JSON names */
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 6u);
    EXPECT_EQ(e.report.files, 7u);
    EXPECT_EQ(e.files["signals.csv"], std::string(kBomHeader) + "1000000," +
                                          kMaskedName + ",5,\n2000000," +
                                          kMaskedName + ",6,\n");
    EXPECT_EQ(e.files["markers.csv"],
              "host_us,label\n1500000," + std::string(17, '*') + "\n");
    EXPECT_EQ(e.files["signals.json"], signals_json(kMaskedName));
    EXPECT_EQ(e.files["conditions.json"], masked_vin(conditions_json(kVin)));
    const std::string& events = e.files["events.log"];
    EXPECT_NE(events.find("\nsignal  " + kMaskedName + ",number,,0,0\n"),
              std::string::npos)
        << events;
    EXPECT_NE(events.find("\nsignal file  signals.json," +
                          sha_of(e.files["signals.json"]) + "\n"),
              std::string::npos)
        << events;
    EXPECT_NE(events.find("\nconditions  conditions.json," +
                          sha_of(e.files["conditions.json"]) + "\n"),
              std::string::npos)
        << events;
    const std::string& note = e.files["masks.txt"];
    EXPECT_EQ(note.rfind(std::string("Havne ") + ecuio_version_string() +
                             ", a scrubbed copy (ADR-0054): 6 VIN-shaped "
                             "tokens masked, 0 frames of messages holding "
                             "one left out.\n",
                         0),
              0u)
        << note;
    EXPECT_EQ(note.find("was a masked copy itself"), std::string::npos);

    const std::string copy = f.utf8("copy");
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(copy.c_str(), nullptr, nullptr, &r),
              ECUIO_OK);
    ecuio_recording_signals_t sig{};
    ecuio_recording_conditions_t cond{};
    EXPECT_EQ(ecuio_recording_get_signals(r, &sig), ECUIO_OK);
    EXPECT_EQ(ecuio_recording_get_conditions(r, &cond), ECUIO_OK);
    ecuio_recording_close(r);
    EXPECT_NE(sig.flags & ECUIO_RECORDING_SIGNALS_MATCH, 0u);
    EXPECT_NE(cond.flags & ECUIO_RECORDING_CONDITIONS_MATCH, 0u);
    ecuio_signal_file_t* loaded = nullptr;
    ASSERT_EQ(ecuio_signal_file_load_with(e.files["signals.json"].data(),
                                          e.files["signals.json"].size(),
                                          ECUIO_SIGNAL_FILE_LOAD_SET, &loaded),
              ECUIO_OK);
    EXPECT_EQ(ecuio_signal_file_set(loaded)->defs[0].name, kMaskedName);
    ecuio_signal_file_free(loaded);

    /* the copy exported again: nothing more to mask, the note kept */
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_FOLDER;
    const std::string again = f.utf8("again");
    o.folder_utf8 = copy.c_str();
    o.out_path_utf8 = again.c_str();
    ecuio_export_report_t ra{};
    ASSERT_EQ(ecuio_export(&o, &ra), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(ra.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 0u);
    EXPECT_EQ(ra.files_left_out, 0u);
    EXPECT_EQ(read_file(f.path / "again" / "signals.json"),
              e.files["signals.json"]);
    EXPECT_NE(read_file(f.path / "again" / "masks.txt")
                  .find("was a masked copy itself"),
              std::string::npos);
}

TEST(FolderExportTest, ARawCopyMasksNothing)
{
    Folder f;
    named_recording(f);
    FolderExported e(f, ECUIO_EXPORT_RAW);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.count("masks.txt"), 0u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 0u);
    EXPECT_EQ(e.files["signals.json"], signals_json(kName));
    EXPECT_NE(e.files["signals.csv"].find(kName), std::string::npos);
}

/* A copy with nothing to mask has no note */
TEST(FolderExportTest, ACopyWithNoMaskHasNoNote)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.count("masks.txt"), 0u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_VIN_MASKED], 0u);
}

/* A signal file whose masks would stop it loading refuses the copy: an
 * escape of a letter reads as its bytes, so a VIN glued to it masks the
 * escape's digits too */
TEST(FolderExportTest, ASignalFileThatWouldNotLoadMaskedRefusesTheCopy)
{
    Folder f;
    named_recording(f);
    const std::string signals = signals_json(std::string(1, '\\') + "u0041" + kVin);
    ecuio_signal_file_t* loaded = nullptr;
    ASSERT_EQ(ecuio_signal_file_load_with(signals.data(), signals.size(),
                                          ECUIO_SIGNAL_FILE_LOAD_SET, &loaded),
              ECUIO_OK);
    ecuio_signal_file_free(loaded);
    f.write("rec/signals.json", signals);
    FolderExported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail, "the copy's signals.json doesn't load with its "
                        "VIN-shaped tokens masked: nothing was written");
    EXPECT_FALSE(fs::exists(f.path / "copy"));
    EXPECT_FALSE(partial_left(f.path, "copy"));
}

} // namespace
