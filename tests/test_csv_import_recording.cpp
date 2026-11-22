// File/layer: tests/test_csv_import_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvImportTest (test_core)

#include "test_csv_import_support.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();

namespace {

/* ---- the import ---- */

struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-csv-import");
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    std::string utf8(const std::string& name) const
    {
        return (path / fs::u8path(name)).generic_u8string();
    }
    std::string write(const std::string& name, const std::string& bytes) const
    {
        std::ofstream(path / fs::u8path(name), std::ios::binary) << bytes;
        return utf8(name);
    }
};

std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

struct Imported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_import_report_t report{};
    std::string detail;
    Imported(const std::string& in, const std::string& out,
             const std::string& map)
    {
        ecuio_import_options_t o{};
        o.format = ECUIO_IMPORT_CSV;
        o.in_path_utf8 = in.c_str();
        o.out_folder_utf8 = out.c_str();
        o.column_map_utf8 = map.c_str();
        rc = ecuio_import(&o, &report);
        detail = ecuio_last_error_detail();
    }
};

struct Opened {
    ecuio_recording_t* r = nullptr;
    ecuio_recording_info_t info{};
    explicit Opened(const std::string& folder)
    {
        EXPECT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        if (r != nullptr) {
            EXPECT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
        }
    }
    ~Opened() { ecuio_recording_close(r); }
    ecuio_recording_signal_t signal(uint32_t i) const
    {
        ecuio_recording_signal_t s{};
        EXPECT_EQ(ecuio_recording_get_signal(r, i, &s), ECUIO_OK);
        return s;
    }
    std::string text_at(uint32_t i, uint64_t t) const
    {
        char buf[ECUIO_DECODE_TEXT_MAX + 1] = {};
        uint64_t at = 0;
        EXPECT_EQ(ecuio_recording_text_at(r, i, t, buf, sizeof buf, &at),
                  ECUIO_OK);
        return buf;
    }
};

const std::string kTwoSignals = "Time (s),Engine Speed,Gear,Coolant\n"
                                "0.0,800,N,20\n"
                                "0.5,1210.5,1,21\n"
                                "1.0,1500,\"2, held\",22\n";

TEST(CsvImportTest, ACsvBecomesARecordingTheReaderOpens)
{
    Folder f;
    const std::string in = f.write("log.csv", kTwoSignals);
    const std::string out = f.utf8("rec");
    Imported imp(in, out, map_of("Time (s)", "s", kRpm + "," + kGear));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.lines, 4u);
    EXPECT_EQ(imp.report.values, 6u);
    EXPECT_EQ(imp.report.columns_ignored, 1u);
    EXPECT_EQ(imp.report.frames, 0u);
    EXPECT_EQ(imp.report.utc_start_us, 0u);
    EXPECT_EQ(imp.report.duration_us, 1000000u);
    EXPECT_FALSE(fs::exists(f.path / "rec" / "frames.csv"));
    EXPECT_FALSE(partial_left(f.path, "rec"));

    const std::string events = read_file(f.path / "rec" / "events.log");
    EXPECT_EQ(events.rfind("ecuio session recording, format 3 (ADR-0049)\n", 0),
              0u);
    EXPECT_EQ(events.find("utc  "), std::string::npos) << events;
    EXPECT_EQ(events.find("started  "), std::string::npos) << events;
    EXPECT_NE(
        events.find(
            "import  CSV, its time from the column 'Time (s)' as seconds\n"),
        std::string::npos)
        << events;
    EXPECT_NE(events.find("import  columns ignored: 1 ('Coolant')\n"),
              std::string::npos)
        << events;
    EXPECT_NE(events.find("signal  RPM,number,rpm,2,1,0\n"), std::string::npos)
        << events;
    EXPECT_NE(events.find("signal  Gear,text,,0,0,0\n"), std::string::npos)
        << events;
    EXPECT_NE(
        events.find("00:00:01.000  +1.000  end  import: 0 frames, 6 values, "
                    "0 records dropped, 0 notes dropped\n"),
        std::string::npos)
        << events;
    EXPECT_EQ(events.find("log.csv"),
              std::string::npos); /* never the file's name */

    Opened rec(out);
    ASSERT_NE(rec.r, nullptr);
    EXPECT_EQ(rec.info.signal_count, 2u);
    EXPECT_EQ(rec.info.utc_start_us, 0u);
    EXPECT_EQ(rec.info.duration_us, 1000000u);
    EXPECT_EQ(rec.info.flags & ECUIO_RECORDING_INFO_NO_SUMMARY, 0u);
    const ecuio_recording_signal_t rpm = rec.signal(0);
    EXPECT_STREQ(rpm.name, "RPM");
    EXPECT_STREQ(rpm.unit, "rpm");
    EXPECT_EQ(rpm.samples, 3u);
    EXPECT_EQ(rpm.lowest, 800.0);
    EXPECT_EQ(rpm.highest, 1500.0);
    EXPECT_EQ(rpm.plot_rank, 1u);
    EXPECT_EQ(rec.text_at(0, 600000), "1210.5");
    const ecuio_recording_signal_t gear = rec.signal(1);
    EXPECT_NE(gear.flags & ECUIO_RECORDING_SIGNAL_TEXT, 0u);
    EXPECT_EQ(rec.text_at(1, 1000000), "2, held");
    /* every value at its time, through a query of one column a sample */
    uint32_t ids[1] = {0};
    ecuio_recording_column_t cols[3];
    ASSERT_EQ(ecuio_recording_query(rec.r, ids, 1, 0, 1500000, cols, 3),
              ECUIO_OK);
    EXPECT_EQ(cols[0].lowest, 800.0);
    EXPECT_EQ(cols[1].lowest, 1210.5);
    EXPECT_EQ(cols[2].lowest, 1500.0);
}

TEST(CsvImportTest, AStartOrTheFilesOwnTimesGiveTheUtcLine)
{
    Folder f;
    const std::string in = f.write("log.csv", kTwoSignals);
    Imported imp(in, f.utf8("rec"),
                 map_of("Time (s)", "s", kRpm,
                        "\"start\": \"2026-11-05T13:03:22.125Z\""));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.utc_start_us, 1793883802125000u);
    const std::string events = read_file(f.path / "rec" / "events.log");
    EXPECT_NE(events.find("started  2026-11-05 13:03:22.125 (UTC)\nutc  "
                          "2026-11-05T13:03:22.125Z\n"),
              std::string::npos)
        << events;
    EXPECT_NE(events.find("13:03:23.125  +1.000  end  "), std::string::npos)
        << events;
    Opened rec(f.utf8("rec"));
    EXPECT_EQ(rec.info.utc_start_us, 1793883802125000u);
}

TEST(CsvImportTest, ARefusedFileWritesNothing)
{
    Folder f;
    const std::string in =
        f.write("log.csv", kTwoSignals + "1.5,1600,3,23,extra\n");
    Imported imp(in, f.utf8("rec"), map_of("Time (s)", "s", kRpm));
    EXPECT_EQ(imp.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(imp.detail,
              "line 5: the row has 5 fields where the header has 4");
    EXPECT_FALSE(fs::exists(f.path / "rec"));
    EXPECT_FALSE(partial_left(f.path, "rec"));

    Imported bad_map(in, f.utf8("rec"), "{\"havne_column_map\": 1}");
    EXPECT_EQ(bad_map.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(bad_map.detail.rfind("the column map: ", 0), 0u)
        << bad_map.detail;

    fs::create_directories(f.path / "taken");
    const std::string good = f.write("good.csv", kTwoSignals);
    Imported taken(good, f.utf8("taken"), map_of("Time (s)", "s", kRpm));
    EXPECT_EQ(taken.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(taken.detail, "the recording's folder exists already");

    Imported missing(f.utf8("none.csv"), f.utf8("rec"),
                     map_of("Time (s)", "s", kRpm));
    EXPECT_EQ(missing.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(missing.detail.find(f.path.filename().u8string()),
              std::string::npos)
        << missing.detail; /* never the path */
}

TEST(CsvImportTest, ManyIgnoredColumnsAreCountedAndTheFirstListed)
{
    Folder f;
    std::string head = "t,Engine Speed";
    std::string row = "0,1";
    for (int i = 0; i < 40; ++i) {
        head += ",x" + std::to_string(i);
        row += ",0";
    }
    const std::string in = f.write("wide.csv", head + "\n" + row + "\n");
    Imported imp(in, f.utf8("rec"), map_of("t", "s", kRpm));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.columns_ignored, 40u);
    const std::string events = read_file(f.path / "rec" / "events.log");
    EXPECT_NE(events.find("import  columns ignored: 40 ('x0', 'x1', "),
              std::string::npos);
    EXPECT_NE(events.find("'x31' and 8 more)\n"), std::string::npos) << events;
}

/* The import's result and its events.log say how many rows had no time
 * and where the first is; a file with none says nothing, and one where no
 * row has a time is refused with nothing written */
TEST(CsvImportTest, RowsWithNoTimeAreCountedAndNamedInTheLog)
{
    Folder f;
    const std::string map = map_of("Time (s)", "s", kRpm + "," + kGear);
    const std::string in = f.write(
        "log.csv", kTwoSignals + ",1600,3,23\n1.5,1700,3,24\n , , , \n");
    Imported imp(in, f.utf8("rec"), map);
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.rows_no_time, 2u);
    EXPECT_EQ(imp.report.reserved[0], 0u);
    EXPECT_EQ(imp.report.lines, 7u);
    EXPECT_EQ(imp.report.values, 8u);
    EXPECT_EQ(imp.report.duration_us, 1500000u);
    const std::string events = read_file(f.path / "rec" / "events.log");
    EXPECT_NE(events.find("import  columns ignored: 1 ('Coolant')\n"
                          "import  rows left out: 2 with no time (the first "
                          "on line 5)\nprofile  raw\n"),
              std::string::npos)
        << events;
    Opened rec(f.utf8("rec"));
    ASSERT_NE(rec.r, nullptr);
    EXPECT_EQ(rec.info.duration_us, 1500000u);
    const ecuio_recording_signal_t rpm = rec.signal(0);
    EXPECT_EQ(rpm.samples, 4u);
    EXPECT_EQ(rpm.highest, 1700.0); /* the untimed row's 1600 is nowhere */

    Imported full(f.write("full.csv", kTwoSignals), f.utf8("full"), map);
    ASSERT_EQ(full.rc, ECUIO_OK) << full.detail;
    EXPECT_EQ(full.report.rows_no_time, 0u);
    EXPECT_EQ(read_file(f.path / "full" / "events.log").find("rows left out"),
              std::string::npos);

    Imported none(
        f.write("none.csv", "Time (s),Engine Speed,Gear,Coolant\n,800,N,20\n"),
        f.utf8("none"), map);
    EXPECT_EQ(none.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(none.detail, "no row below the header has a time");
    EXPECT_EQ(none.report.rows_no_time, 0u);
    EXPECT_FALSE(fs::exists(f.path / "none"));
    EXPECT_FALSE(partial_left(f.path, "none"));
}

/* Writes its text over the file between the import's passes */
struct Rewritten {
    fs::path file;
    std::string text;
    static void hook(void* user)
    {
        const Rewritten* r = static_cast<const Rewritten*>(user);
        std::ofstream(r->file, std::ios::binary | std::ios::trunc) << r->text;
    }
};

/* The seam set for a scope */
struct BetweenPasses {
    explicit BetweenPasses(Rewritten* r)
    {
        ecuio_import_test_between_passes(&Rewritten::hook, r);
    }
    ~BetweenPasses() { ecuio_import_test_between_passes(nullptr, nullptr); }
    BetweenPasses(const BetweenPasses&) = delete;
    BetweenPasses& operator=(const BetweenPasses&) = delete;
};

/* The second pass must find what the first checked: a row that lost its
 * time (the rows and values the same, one more left out), a cell that
 * gained a value and a row that went are each a file that changed, with
 * nothing left; the same bytes written again import */
TEST(CsvImportTest, AFileThatChangesBetweenItsPassesIsRefused)
{
    Folder f;
    const std::string map = map_of("t", "ms", kRpm);
    /* Longer than a stream's buffer, so the second pass reads the file */
    std::string rest;
    for (int i = 2; i < 2000; ++i) {
        rest += std::to_string(i * 10) + "," + std::to_string(800 + i) + "\n";
    }
    const std::string head = "t,Engine Speed\n0,800\n";
    const std::string start = head + "10,\n" + rest;
    const struct {
        const char* what;
        const char* row;
        bool changed;
    } cases[] = {
        {"the same bytes", "10,\n", false},
        {"a row lost its time", " ,\n", true},
        {"a cell gained a value", "10,801\n", true},
        {"a row went", "", true},
    };
    int n = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.what);
        const std::string name = "rec" + std::to_string(++n);
        const std::string in = f.write("log.csv", start);
        Rewritten r{f.path / "log.csv", head + c.row + rest};
        BetweenPasses seam(&r);
        Imported imp(in, f.utf8(name), map);
        if (!c.changed) {
            EXPECT_EQ(imp.rc, ECUIO_OK) << imp.detail;
            EXPECT_EQ(imp.report.rows_no_time, 0u);
            continue;
        }
        EXPECT_EQ(imp.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(imp.detail, "the file changed while it was read");
        EXPECT_FALSE(fs::exists(f.path / name));
        EXPECT_FALSE(partial_left(f.path, name));
    }
}

/* ---- the made-up loggers ---- */

std::string fixtures()
{
    return ECUIO_IMPORT_FIXTURES;
}

TEST(CsvImportTest, TheMadeUpLoggersFilesImportAndRead)
{
    const std::string dir = fixtures();
    const struct {
        const char* name;
        uint64_t values;
        uint64_t ignored;
        uint64_t utc;
        const char* first_signal;
        double lowest;
        double highest;
    } loggers[] = {
        /* a preamble, ';' with decimal commas, "---" for no reading, no start */
        {"logger-a", 22, 1, 0, "RPM", 780.5, 3420.25},
        /* ISO times with an offset, a text column, quoted cells */
        {"logger-b", 16, 2, 1793883802250000, "Vehicle Speed", 0, 48.5},
    };
    for (const auto& l : loggers) {
        SCOPED_TRACE(l.name);
        const fs::path base = fs::u8path(dir) / "csv" / l.name;
        const std::string map = read_file(base / "map.json");
        Folder f;
        Imported imp((base / "log.csv").generic_u8string(), f.utf8("rec"), map);
        ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
        EXPECT_EQ(imp.report.values, l.values);
        EXPECT_EQ(imp.report.columns_ignored, l.ignored);
        EXPECT_EQ(imp.report.utc_start_us, l.utc);
        Opened rec(f.utf8("rec"));
        ASSERT_NE(rec.r, nullptr);
        const ecuio_recording_signal_t s = rec.signal(0);
        EXPECT_STREQ(s.name, l.first_signal);
        EXPECT_EQ(s.lowest, l.lowest);
        EXPECT_EQ(s.highest, l.highest);
        EXPECT_EQ(rec.info.utc_start_us, l.utc);
    }
}

/* Out of memory at each of the CSV import's allocations in turn: every
 * one returns, and none leaves its folder or a partial */
TEST(CsvImportTest, OutOfMemoryAnywhereLeavesNoFolderBehind)
{
    Folder f;
    const std::string in = f.write("log.csv", kTwoSignals);
    const std::string out = f.utf8("rec");
    const std::string map = map_of("Time (s)", "s", kRpm + "," + kGear);
    ecuio_import_options_t o{};
    o.format = ECUIO_IMPORT_CSV;
    o.in_path_utf8 = in.c_str();
    o.out_folder_utf8 = out.c_str();
    o.column_map_utf8 = map.c_str();
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_import(&o, nullptr), ECUIO_OK);
    const unsigned long news = news_on_this_thread() - before;
    std::error_code ec;
    fs::remove_all(f.path / "rec", ec);
    ASSERT_GT(news, 10u);
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        fail_new_on_this_thread(n);
        const ecuio_status_t rc = ecuio_import(&o, nullptr);
        fail_new_on_this_thread(0);
        if (rc != ECUIO_OK) {
            ++refused;
            ASSERT_TRUE(rc == ECUIO_ERR_OUT_OF_MEMORY ||
                        rc == ECUIO_ERR_LOAD_FAILED)
                << "at " << n << ": " << rc;
            ASSERT_FALSE(fs::exists(f.path / "rec")) << "at " << n;
        }
        ASSERT_FALSE(partial_left(f.path, "rec")) << "at " << n;
        fs::remove_all(f.path / "rec", ec);
    }
    EXPECT_GT(refused, 0u);
}

} // namespace
