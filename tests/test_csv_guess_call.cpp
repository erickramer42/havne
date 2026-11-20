// File/layer: tests/test_csv_guess_call.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvGuessTest (test_core)

#include "test_csv_guess_support.h"

namespace {

/* ---- ecuio_import_csv_guess ---- */

struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-csv-guess");
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

/* The call with room to spare: its text, report and detail */
struct Called {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    std::string text;
    size_t needed = 0;
    ecuio_csv_guess_t report{};
    std::string detail;
    explicit Called(const std::string& path, const char* unit = nullptr)
    {
        std::vector<char> buf(4u << 20);
        rc = ecuio_import_csv_guess(path.c_str(), unit, buf.data(), buf.size(),
                                    &needed, &report);
        detail = ecuio_last_error_detail();
        if (rc == ECUIO_OK) {
            text.assign(buf.data());
        }
    }
    std::string map() const
    {
        return text.substr(report.map_offset, report.map_length);
    }
};

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

TEST(CsvGuessTest, TheCallsArgumentsAndRoom)
{
    Folder f;
    const std::string in = f.write("log.csv", kSimple);
    size_t needed = 99;
    ecuio_csv_guess_t report{};
    report.flags = 7;
    EXPECT_EQ(
        ecuio_import_csv_guess(nullptr, nullptr, nullptr, 0, &needed, &report),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(needed, 0u);
    EXPECT_EQ(report.flags, 0u);
    EXPECT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, nullptr, 0, nullptr,
                                     nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, nullptr, 8, &needed,
                                     nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    for (const char* bad : {"", "S", "sec", "ms ", "iso8601", "h"}) {
        EXPECT_EQ(ecuio_import_csv_guess(in.c_str(), bad, nullptr, 0, &needed,
                                         nullptr),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << bad;
    }
    /* Each refusal blanks a buffer it's given, the arguments' too */
    std::vector<char> blank(64, 'x');
    EXPECT_EQ(ecuio_import_csv_guess(in.c_str(), "h", blank.data(),
                                     blank.size(), &needed, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(blank[0], '\0');
    blank.assign(64, 'x');
    EXPECT_EQ(ecuio_import_csv_guess(nullptr, nullptr, blank.data(),
                                     blank.size(), &needed, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(blank[0], '\0');
    blank.assign(64, 'x');
    EXPECT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, blank.data(),
                                     blank.size(), nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(blank[0], '\0');
    /* Asked the size, then a buffer one short, then one just right */
    ASSERT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, nullptr, 0, &needed,
                                     &report),
              ECUIO_OK);
    const Called whole(in);
    ASSERT_EQ(whole.rc, ECUIO_OK) << whole.detail;
    EXPECT_EQ(needed, whole.text.size() + 1);
    EXPECT_EQ(report.columns, 2u);
    std::vector<char> room(needed - 1, 'x');
    size_t again = 0;
    ASSERT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, room.data(),
                                     room.size(), &again, nullptr),
              ECUIO_OK);
    EXPECT_EQ(again, needed);
    EXPECT_EQ(room[0], '\0');
    room.assign(needed, 'x');
    ASSERT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, room.data(),
                                     room.size(), &again, nullptr),
              ECUIO_OK);
    EXPECT_EQ(std::string(room.data()), whole.text);
}

TEST(CsvGuessTest, TheReportMatchesTheText)
{
    Folder f;
    const std::string in =
        f.write("log.csv", "pre\ntime,A,,B\n0,1,,x\n1,2,,y\n");
    const Called c(in, "ms");
    ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.report.flags, ECUIO_CSV_GUESS_TIME_UNIT_KNOWN |
                                  ECUIO_CSV_GUESS_PREAMBLE |
                                  ECUIO_CSV_GUESS_TIME_NUMBERS);
    EXPECT_EQ(c.report.columns, 2u);
    EXPECT_EQ(c.report.left_out, 1u);
    EXPECT_EQ(c.report.rows_read, 2u);
    for (uint64_t r : c.report.reserved) {
        EXPECT_EQ(r, 0u);
    }
    EXPECT_EQ(c.text.compare(c.report.map_offset, 1, "{"), 0);
    EXPECT_EQ(c.text[c.report.map_offset + c.report.map_length - 1], '}');
    EXPECT_EQ(c.text.compare(c.report.map_offset + c.report.map_length, 13,
                             ",\n\"columns\": "),
              0)
        << c.text;
}

/* The slice alone, as the CLI prints and passes it, imports the file */
TEST(CsvGuessTest, TheSliceImportsTheFile)
{
    Folder f;
    const std::string in = f.write("log.csv", kSimple);
    const Called c(in);
    ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
    Imported imp(in, f.utf8("rec"), c.map());
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.values, 6u);
    EXPECT_EQ(imp.report.columns_ignored, 0u);
}

TEST(CsvGuessTest, AFileThatCantBeGuessedSaysWhyWithoutItsPath)
{
    Folder f;
    const std::string gone = f.utf8("no-such-file.csv");
    const Called missing(gone);
    EXPECT_EQ(missing.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(missing.detail.rfind("the file can't be opened: ", 0), 0u)
        << missing.detail;
    EXPECT_EQ(missing.detail.find("no-such-file"), std::string::npos);
    EXPECT_EQ(missing.needed, 0u);
    const Called numbers(f.write("n.csv", "1,2\n3,4\n"));
    EXPECT_EQ(numbers.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(numbers.detail.rfind("no row within the file's first 1000", 0),
              0u)
        << numbers.detail;
}

/* A file past what the guess reads, its header near the search's end so
 * that the read's end falls inside its sample's last row (*whole the rows
 * before it), then more rows. A wide header and a long last row reach it
 * within the bounds: the search's 1 MiB, the sample's 64 KiB, a row's */
std::string near_the_reads_end(bool quoted, uint32_t* whole)
{
    std::string text;
    const std::string pre(1100, 'p');
    while (text.size() + pre.size() + 1 < guess::kSearchBytes - 2048) {
        text += pre + "\n";
    }
    text += "time," + std::string(30000, 'N') + ",rpm\n";
    const size_t start = text.size();
    const std::string note(380, 'n');
    uint32_t i = 0;
    while (text.size() - start + note.size() + 16 < guess::kSampleBytes) {
        text += std::to_string(i++) + "," + note + ",800\n";
    }
    *whole = i;
    /* The note in the middle, so the row cut short has fewer fields; or
     * quoted, with line feeds, so the cut lands inside the quote */
    std::string last_note;
    if (quoted) {
        last_note = "\"";
        while (last_note.size() < 60000) {
            last_note += std::string(99, 'q') + "\n";
        }
        last_note += "\"";
    } else {
        last_note = std::string(60000, 'q');
    }
    const size_t last_at = text.size();
    text += std::to_string(i++) + "," + last_note + ",800\n";
    EXPECT_LT(last_at, guess::kReadMax);
    EXPECT_GT(text.size(), guess::kReadMax);
    while (text.size() < guess::kReadMax + 4096) {
        text += std::to_string(i++) + "," + note + ",800\n";
    }
    return text;
}

/* The C call reads a byte past what the guess reads and cuts at the last
 * line feed before it: a row the cut leaves open is no row */
TEST(CsvGuessTest, ALongFileIsCutAtALineFeed)
{
    Folder f;
    uint32_t whole = 0;
    const Called plain(f.write("long.csv", near_the_reads_end(false, &whole)));
    ASSERT_EQ(plain.rc, ECUIO_OK) << plain.detail;
    EXPECT_EQ(plain.report.rows_read, whole);
    EXPECT_EQ(plain.text.find("another number of fields"), std::string::npos)
        << plain.text;
    /* Cut inside a quoted field: the read's end, so the sample ends there */
    const Called quoted(
        f.write("quoted.csv", near_the_reads_end(true, &whole)));
    ASSERT_EQ(quoted.rc, ECUIO_OK) << quoted.detail;
    EXPECT_EQ(quoted.report.rows_read, whole);
    /* No line end in all the read holds: said so, not "empty" */
    const Called one_line(
        f.write("one-line.csv", std::string(guess::kReadMax + 1, 'a')));
    EXPECT_EQ(one_line.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(one_line.detail, "the file's first 1152 KiB hold no line end");
}

/* ---- the made-up loggers ---- */

std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/* A committed text file, CRs gone (a checkout may add them) */
std::string read_text(const fs::path& p)
{
    std::string t = read_file(p);
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    return t;
}

/* Each guessed as its own map reads it, and imported; its map's slice is
 * guessed-map.json byte for byte, which `havne-cli import csv --print-map`
 * prints (CliTransferRunTest.csv-guess) */
TEST(CsvGuessTest, TheMadeUpLoggersAreGuessedAndImported)
{
    const fs::path dir = fs::u8path(ECUIO_IMPORT_FIXTURES) / "csv";
    {
        SCOPED_TRACE("logger-a");
        const std::string in =
            (dir / "logger-a" / "log.csv").generic_u8string();
        Guessed g(read_file(dir / "logger-a" / "log.csv"));
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.number("header_row"), 3);
        EXPECT_EQ(g.key("delimiter"), ";");
        EXPECT_EQ(g.key("decimal"), ",");
        EXPECT_EQ(g.time("column"), "Time [s]");
        EXPECT_EQ(g.time("unit"), "s");
        EXPECT_EQ(g.missing(), std::vector<std::string>{"---"});
        EXPECT_EQ(g.sig("Engine Speed [rpm]", "name"), "Engine Speed");
        EXPECT_EQ(g.sig("Engine Speed [rpm]", "unit"), "rpm");
        EXPECT_EQ(g.sig("Engine Speed [rpm]", "decimals"), "2");
        EXPECT_EQ(g.sig("Load [%]", "unit"), "%");
        EXPECT_EQ(g.sig("Lambda", "kind"), "number");
        EXPECT_EQ(g.sig("Note", "kind"), "text");
        EXPECT_TRUE(g.noted("'---' in a column of numbers holds no sample"))
            << notes_of(g);
        Folder f;
        const Called c(in);
        ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
        EXPECT_EQ(c.map(), g.map_text());
        EXPECT_EQ(c.map() + "\n",
                  read_text(dir / "logger-a" / "guessed-map.json"));
        Imported imp(in, f.utf8("rec"), c.map());
        ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
        /* the fixture map's 22 values, and Note's three */
        EXPECT_EQ(imp.report.values, 25u);
        EXPECT_EQ(imp.report.columns_ignored, 0u);
    }
    {
        SCOPED_TRACE("logger-b");
        const std::string in =
            (dir / "logger-b" / "log.csv").generic_u8string();
        Guessed g(read_file(dir / "logger-b" / "log.csv"));
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.number("header_row"), 1);
        EXPECT_EQ(g.time("column"), "timestamp");
        EXPECT_EQ(g.time("unit"), "iso8601");
        EXPECT_EQ(g.sig("speed_kmh", "kind"), "number");
        EXPECT_EQ(g.sig("gear", "kind"), "text");
        EXPECT_EQ(g.sig("throttle", "kind"), "number");
        EXPECT_EQ(g.sig("comment", "kind"), "text");
        Folder f;
        const Called c(in);
        ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
        EXPECT_EQ(c.map() + "\n",
                  read_text(dir / "logger-b" / "guessed-map.json"));
        Imported imp(in, f.utf8("rec"), c.map());
        ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
        EXPECT_EQ(imp.report.utc_start_us, 1793883802250000u);
    }
    {
        /* A tuner log's shape: "Name (Unit)", a space after each comma, a
         * trailing comma, a bare ms Time, a sparse counter of " " blanks */
        SCOPED_TRACE("logger-c");
        const std::string in =
            (dir / "logger-c" / "log.csv").generic_u8string();
        Guessed g(read_file(dir / "logger-c" / "log.csv"));
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.time("column"), "Time");
        EXPECT_EQ(g.time("unit"), "ms");
        EXPECT_TRUE(g.noted("the time column 'Time' has no unit: read as ms"))
            << notes_of(g);
        EXPECT_EQ(g.sig(" Flux counter", "kind"), "number");
        EXPECT_TRUE(g.noted("'Flux counter' holds a value in 3 of 200 rows "
                            "read: a wide export by sweep holds its newest "
                            "value in the rows the other columns set"))
            << notes_of(g);
        EXPECT_EQ(g.sig(" Stage", "kind"), "number");
        EXPECT_EQ(g.sig(" Stage", "unit"), "");
        EXPECT_EQ(g.sig(" Mode (OL/CL)", "kind"), "text");
        EXPECT_EQ(g.column(""), "its header cell is empty");
        EXPECT_EQ(g.sig(" Throttle angle (%)", "name"), "Throttle angle");
        EXPECT_EQ(g.sig(" throttle angle (ratio)", "name"), "throttle angle");
        EXPECT_EQ(
            g.sig(" Intake manifold absolute pressure sensor A (kPa)", "name"),
            "Intake manifold absolute pressu");
        EXPECT_EQ(
            g.sig(" Intake manifold absolute pressure sensor B (kPa)", "name"),
            "Intake manifold absolute pres 2");
        EXPECT_EQ(g.r.columns, 15u);
        Folder f;
        const Called c(in);
        ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
        EXPECT_EQ(c.map() + "\n",
                  read_text(dir / "logger-c" / "guessed-map.json"));
        Imported imp(in, f.utf8("rec"), c.map());
        ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
        /* 420 rows of 14 full columns, and the counter's 6 values */
        EXPECT_EQ(imp.report.values, 420u * 14u + 6u);
        EXPECT_EQ(imp.report.columns_ignored, 1u);
        EXPECT_EQ(imp.report.duration_us, 121521000u);
    }
    {
        /* A row with no time in the middle, holding values, and a last
         * row of padding alone: left out by the import, said by the guess */
        SCOPED_TRACE("logger-d");
        const std::string in =
            (dir / "logger-d" / "log.csv").generic_u8string();
        Guessed g(read_file(dir / "logger-d" / "log.csv"));
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.time("column"), "Time (ms)");
        EXPECT_EQ(g.time("unit"), "ms");
        EXPECT_TRUE(g.noted("2 of the 10 rows read have no time (the first on "
                            "line 5): the import leaves such a row out"))
            << notes_of(g);
        EXPECT_EQ(g.sig(" Engine speed (rpm)", "kind"), "number");
        EXPECT_EQ(g.sig(" Gizmo state", "kind"), "text");
        EXPECT_EQ(g.r.columns, 3u);
        Folder f;
        const Called c(in);
        ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
        EXPECT_EQ(c.map() + "\n",
                  read_text(dir / "logger-d" / "guessed-map.json"));
        Imported imp(in, f.utf8("rec"), c.map());
        ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
        EXPECT_EQ(imp.report.rows_no_time, 2u);
        EXPECT_EQ(imp.report.lines, 11u);
        /* 8 rows with a time, 3 columns each */
        EXPECT_EQ(imp.report.values, 8u * 3u);
        EXPECT_EQ(imp.report.duration_us, 1740000u);
        const std::string events = read_file(f.path / "rec" / "events.log");
        EXPECT_NE(events.find("import  rows left out: 2 with no time (the "
                              "first on line 5)\n"),
                  std::string::npos)
            << events;
    }
    {
        /* Three columns, a row with no time and a text among the rows
         * the header is judged by */
        SCOPED_TRACE("logger-e");
        const std::string in =
            (dir / "logger-e" / "log.csv").generic_u8string();
        Guessed g(read_file(dir / "logger-e" / "log.csv"));
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.number("header_row"), 1);
        EXPECT_EQ(g.time("column"), "Time (s)");
        EXPECT_EQ(g.time("unit"), "s");
        EXPECT_TRUE(g.noted("1 of the 7 rows read has no time (line 4): the "
                            "import leaves it out"))
            << notes_of(g);
        EXPECT_EQ(g.sig("Gear", "kind"), "text");
        Folder f;
        const Called c(in);
        ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
        EXPECT_EQ(c.map() + "\n",
                  read_text(dir / "logger-e" / "guessed-map.json"));
        Imported imp(in, f.utf8("rec"), c.map());
        ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
        EXPECT_EQ(imp.report.rows_no_time, 1u);
        /* 6 rows with a time, 2 columns each */
        EXPECT_EQ(imp.report.values, 6u * 2u);
        EXPECT_EQ(imp.report.duration_us, 2500000u);
    }
}

/* Out of memory at each of the guess's allocations in turn: every one
 * returns OK or OUT_OF_MEMORY */
TEST(CsvGuessTest, OutOfMemoryAnywhereReturns)
{
    Folder f;
    const std::string in = f.write("log.csv", kSimple);
    size_t needed = 0;
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_import_csv_guess(in.c_str(), nullptr, nullptr, 0, &needed,
                                     nullptr),
              ECUIO_OK);
    const unsigned long news = news_on_this_thread() - before;
    ASSERT_GT(news, 10u);
    unsigned long refused_count = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        fail_new_on_this_thread(n);
        const ecuio_status_t rc = ecuio_import_csv_guess(
            in.c_str(), nullptr, nullptr, 0, &needed, nullptr);
        fail_new_on_this_thread(0);
        ASSERT_TRUE(rc == ECUIO_OK || rc == ECUIO_ERR_OUT_OF_MEMORY)
            << "at " << n << ": " << rc;
        refused_count += rc == ECUIO_ERR_OUT_OF_MEMORY ? 1 : 0;
    }
    EXPECT_GT(refused_count, 0u);
}

} // namespace
