// File/layer: tests/test_csv_guess.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvGuessTest (test_core)
//
// csv_guess.h's column map guessed from a CSV's header (ADR-0054's
// appendix "a column map guessed from the header") over bytes in memory
// (Guessed), each rule, note and refusal in its words, and
// ecuio_import_csv_guess over files: its contract, the map's slice taken
// by ecuio_import as it is, and the two made-up loggers' files
// (tests/fixtures/import/csv/, ECUIO_IMPORT_FIXTURES) guessed, imported
// and read. Each file case writes into a folder of its own under
// TEMP/TMPDIR, removed at the end.

#include "test_csv_guess_support.h"

namespace {

/* The map parsed by the import's own parser */
bool imports_map(const std::string& map_text, std::string* why)
{
    csv::column_map m;
    return csv::parse_map(map_text.data(), map_text.size(), m, why);
}

/* ---- the map ---- */

TEST(CsvGuessTest, ASimpleFileGivesItsMap)
{
    Guessed g(kSimple);
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.number("havne_column_map"), 1);
    EXPECT_EQ(g.number("header_row"), 1);
    EXPECT_EQ(g.key("delimiter"), ",");
    EXPECT_EQ(g.key("decimal"), ".");
    EXPECT_EQ(g.time("column"), "Time (s)");
    EXPECT_EQ(g.time("unit"), "s");
    ASSERT_EQ(g.signals().size(), 2u);
    EXPECT_EQ(g.sig("Speed (km/h)", "name"), "Speed");
    EXPECT_EQ(g.sig("Speed (km/h)", "unit"), "km/h");
    EXPECT_EQ(g.sig("Speed (km/h)", "kind"), "number");
    EXPECT_EQ(g.sig("Speed (km/h)", "decimals"), "2");
    EXPECT_EQ(g.sig("Gear", "kind"), "text");
    EXPECT_EQ(g.sig("Gear", "unit"), "");
    EXPECT_EQ(g.sig("Gear", "decimals"), "");
    EXPECT_EQ(g.column("Time (s)"), "used");
    EXPECT_EQ(g.column("Gear"), "used");
    EXPECT_EQ(g.r.flags, guess::kTimeUnitKnown | guess::kTimeNumbers);
    EXPECT_EQ(g.r.columns, 2u);
    EXPECT_EQ(g.r.left_out, 0u);
    EXPECT_EQ(g.r.rows_read, 3u);
    EXPECT_TRUE(g.noted("'Time (s)' is the time, in seconds by its header"))
        << notes_of(g);
}

/* The map's slice is complete JSON the import takes, its keys at two
 * spaces, and the whole text is the same for the same bytes */
TEST(CsvGuessTest, TheMapsSliceIsAMapTheImportTakes)
{
    Guessed g(kSimple);
    ASSERT_TRUE(g.ok) << g.why;
    const std::string m = g.map_text();
    EXPECT_EQ(m.front(), '{');
    EXPECT_EQ(m.back(), '}');
    EXPECT_EQ(m.compare(0, 24, "{\n  \"havne_column_map\": "), 0) << m;
    std::string why;
    EXPECT_TRUE(imports_map(m, &why)) << why;
    json::Value alone;
    json::Error e;
    EXPECT_TRUE(json::parse(m.data(), m.size(), &alone, &e)) << e.to_string();
    EXPECT_EQ(g.r.text.compare(0, 9, "{\n\"map\": "), 0) << g.r.text;
    Guessed again(kSimple);
    EXPECT_EQ(again.r.text, g.r.text);
}

TEST(CsvGuessTest, EachDelimiterIsFound)
{
    Guessed semi("Time [s];A;B\n0;1;2\n1;2;3\n");
    ASSERT_TRUE(semi.ok) << semi.why;
    EXPECT_EQ(semi.key("delimiter"), ";");
    Guessed tab("Time [s]\tA\tB\n0\t1\t2\n1\t2\t3\n");
    ASSERT_TRUE(tab.ok) << tab.why;
    EXPECT_EQ(tab.key("delimiter"), "\t");
    EXPECT_NE(tab.map_text().find("\"delimiter\": \"\\t\""), std::string::npos)
        << tab.map_text();
    /* Both split it alike: the comma first */
    Guessed both("Time [s],A;x,B\n0,1;1,2\n1,2;2,3\n");
    ASSERT_TRUE(both.ok) << both.why;
    EXPECT_EQ(both.key("delimiter"), ",");
}

TEST(CsvGuessTest, APreambleIsLeftOutAndSaidSo)
{
    Guessed g("Made-up logger, export 2\n"
              "no car\n"
              "\n"
              "Time [s];RPM [rpm]\n"
              "0;800\n"
              "1;900\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.number("header_row"), 4);
    EXPECT_EQ(g.r.flags & guess::kPreamble, guess::kPreamble);
    EXPECT_TRUE(g.noted("the header is line 4: the lines above it are left "
                        "out"))
        << notes_of(g);
    /* The import reads it so */
    std::string why;
    ASSERT_TRUE(imports_map(g.map_text(), &why)) << why;
}

TEST(CsvGuessTest, ADecimalCommaOnlyWithAnotherDelimiter)
{
    Guessed comma("Time [s];RPM\n0,25;800,5\n0,50;812\n");
    ASSERT_TRUE(comma.ok) << comma.why;
    EXPECT_EQ(comma.key("decimal"), ",");
    EXPECT_EQ(comma.r.flags & guess::kDecimalComma, guess::kDecimalComma);
    EXPECT_EQ(comma.sig("RPM", "decimals"), "1");
    /* A point anywhere keeps the point */
    Guessed point("Time [s];RPM;Load\n0,25;800,5;1.5\n0,50;812;2\n");
    ASSERT_TRUE(point.ok) << point.why;
    EXPECT_EQ(point.key("decimal"), ".");
    /* A comma delimiter never makes a decimal comma */
    Guessed none("Time [s],RPM\n0,800\n1,812\n");
    ASSERT_TRUE(none.ok) << none.why;
    EXPECT_EQ(none.key("decimal"), ".");
}

/* ---- the time ---- */

TEST(CsvGuessTest, TheTimeColumnIsNamedSoOrTheFirstThatRises)
{
    for (const char* name : {"time", "TIME", "Timestamp", "t", "elapsed",
                             "Time Stamp", "Elapsed [ms]"}) {
        SCOPED_TRACE(name);
        Guessed g(std::string("RPM,") + name + "\n800,0\n900,5\n");
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.time("column"), name);
    }
    /* None named: the first column, rising (a repeat allowed) */
    Guessed first("Clock,RPM\n0,800\n0,810\n1,900\n");
    ASSERT_TRUE(first.ok) << first.why;
    EXPECT_EQ(first.time("column"), "Clock");
    EXPECT_FALSE(first.time_has_unit()) << "only a time name reads as ms";
    EXPECT_EQ(first.r.flags & guess::kTimeNumbers, guess::kTimeNumbers);
    EXPECT_TRUE(first.noted("'Clock' is the time, its unit not known: say s, "
                            "ms or us"))
        << notes_of(first);
    /* One that falls, or never moves, is no time */
    EXPECT_NE(refused("Clock,RPM\n1,800\n0,900\n")
                  .find("no column reads as the time"),
              std::string::npos);
    /* Falling once is enough, whatever rises after it */
    EXPECT_NE(refused("Clock,RPM\n1,800\n0,900\n2,950\n")
                  .find("no column reads as the time"),
              std::string::npos);
    EXPECT_NE(refused("Clock,RPM\n5,800\n5,900\n")
                  .find("no column reads as the time"),
              std::string::npos);
    /* A time name twice can't be the map's column */
    Guessed twice("time,time,Clock\n0,0,1\n1,1,2\n");
    EXPECT_FALSE(twice.ok);
}

TEST(CsvGuessTest, ATimeUnitComesFromTheHeaderOrTheCaller)
{
    const struct {
        const char* header;
        const char* unit;
    } cases[] = {
        {"Time (s)", "s"},          {"Time [s]", "s"},
        {"Time [sec]", "s"},        {"Time (secs)", "s"},
        {"Time (seconds)", "s"},    {"Time (S)", "s"},
        {"Time (ms)", "ms"},        {"Time [msec]", "ms"},
        {"Time (us)", "us"},        {"Time [usec]", "us"},
        {"Time (\xC2\xB5s)", "us"}, {"Time (\xCE\xBCs)", "us"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.header);
        Guessed g(std::string(c.header) + ",A\n0,1\n1,2\n");
        ASSERT_TRUE(g.ok) << g.why;
        EXPECT_EQ(g.time("unit"), c.unit);
        EXPECT_EQ(g.r.flags & guess::kTimeUnitKnown, guess::kTimeUnitKnown);
    }
    /* None: plain numbers read as ms, a logger's convention, the flag set
     * and a note saying so; a unit given would change it */
    Guessed none("time,A\n0,1\n1,2\n");
    ASSERT_TRUE(none.ok) << none.why;
    EXPECT_EQ(none.time("unit"), "ms");
    EXPECT_EQ(none.r.flags & guess::kTimeUnitKnown, guess::kTimeUnitKnown);
    EXPECT_EQ(none.r.flags & guess::kTimeNumbers, guess::kTimeNumbers);
    EXPECT_TRUE(none.noted("the time column 'time' has no unit: read as ms; "
                           "change it if it isn't"))
        << notes_of(none);
    std::string why;
    EXPECT_TRUE(imports_map(none.map_text(), &why)) << why;
    /* The caller's unit, over the header's too */
    Guessed given("time,A\n0,1\n1,2\n", "ms");
    ASSERT_TRUE(given.ok) << given.why;
    EXPECT_EQ(given.time("unit"), "ms");
    EXPECT_TRUE(given.noted("in milliseconds as given")) << notes_of(given);
    Guessed over("Time (s),A\n0,1\n1,2\n", "us");
    ASSERT_TRUE(over.ok) << over.why;
    EXPECT_EQ(over.time("unit"), "us");
}

TEST(CsvGuessTest, TimesThatArentNumbersSayWhatTheyAre)
{
    /* ISO 8601 with an offset, the caller's unit set aside */
    Guessed iso("timestamp,A\n2026-11-05T22:03:22.250+09:00,1\n"
                "2026-11-05T22:03:22.750+09:00,2\n",
                "s");
    ASSERT_TRUE(iso.ok) << iso.why;
    EXPECT_EQ(iso.time("unit"), "iso8601");
    EXPECT_EQ(iso.r.flags & guess::kTimeNumbers, 0u);
    EXPECT_TRUE(iso.noted("as ISO 8601 times with their offset: the time "
                          "unit given isn't used"))
        << notes_of(iso);
    /* Without one: no unit, and why */
    Guessed zoneless("timestamp,A\n2026-11-05T22:03:22.250,1\n"
                     "2026-11-05 22:03:22.750,2\n");
    ASSERT_TRUE(zoneless.ok) << zoneless.why;
    EXPECT_FALSE(zoneless.time_has_unit());
    /* No unit given would do: a map is needed */
    EXPECT_EQ(zoneless.r.flags & guess::kTimeNumbers, 0u);
    EXPECT_TRUE(zoneless.noted("holds ISO 8601 times without a UTC offset, "
                               "which the import needs in each"))
        << notes_of(zoneless);
    /* A time of day */
    Guessed clock("time,A\n22:03:22.250,1\n22:03:23,2\n", "s");
    ASSERT_TRUE(clock.ok) << clock.why;
    EXPECT_FALSE(clock.time_has_unit());
    EXPECT_EQ(clock.r.flags & guess::kTimeNumbers, 0u);
    EXPECT_TRUE(clock.noted("holds times of day, which need a date and a UTC "
                            "offset"))
        << notes_of(clock);
}

/* ---- the signals ---- */

TEST(CsvGuessTest, NamesAreCutAndNumberedAndLongUnitsDropped)
{
    const std::string long_name(40, 'x');
    /* 30 bytes then a two-byte character: the cut keeps whole ones */
    const std::string wide = std::string(30, 'y') + "\xC3\xA9" + "zz";
    Guessed g("time (s),RPM,RPM (rpm),RPM [x]," + long_name + "," + wide +
              ",Boost (a unit far too long)\n"
              "0,1,2,3,4,5,6\n1,2,3,4,5,6,7\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.sig("RPM", "name"), "RPM");
    EXPECT_EQ(g.sig("RPM (rpm)", "name"), "RPM 2");
    EXPECT_EQ(g.sig("RPM [x]", "name"), "RPM 3");
    EXPECT_EQ(g.sig(long_name, "name"), std::string(31, 'x'));
    EXPECT_EQ(g.sig(wide, "name"), std::string(30, 'y'));
    EXPECT_EQ(g.sig("Boost (a unit far too long)", "unit"), "");
    EXPECT_TRUE(g.noted("'RPM (rpm)' is named 'RPM 2': another column has its "
                        "name"))
        << notes_of(g);
    EXPECT_TRUE(g.noted("is named '" + std::string(29, 'x') +
                        "...': a name is 31 bytes at most") ||
                g.noted(": a name is 31 bytes at most"))
        << notes_of(g);
    EXPECT_TRUE(g.noted("'Boost (a unit far too long)''s unit is left out: it "
                        "is longer than 15 bytes"))
        << notes_of(g);
    /* A name cut and then repeated keeps its suffix inside 31 bytes */
    Guessed cut("time (s)," + long_name + "," + long_name +
                "y\n0,1,2\n1,2,3\n");
    ASSERT_TRUE(cut.ok) << cut.why;
    EXPECT_EQ(cut.sig(long_name + "y", "name"), std::string(29, 'x') + " 2");
    std::string why;
    EXPECT_TRUE(imports_map(g.map_text(), &why)) << why;
}

TEST(CsvGuessTest, AColumnTheMapCantTakeIsLeftOutWithWhy)
{
    const std::string long_text(csv::kTextMax + 1, 'q');
    Guessed g("time (s),=SUM,(V),Dup,Dup,Empty,Long,Unit (-x),A,\n"
              "0,1,2,3,4,," +
              long_text +
              ",x,1,\n"
              "1,2,3,4,5,,z,y,2,\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.column("=SUM"), "its name starts as a spreadsheet formula "
                                "does");
    EXPECT_EQ(g.column("(V)"), "its header has no name before its unit");
    EXPECT_EQ(g.column("Dup"), "the header names it twice");
    EXPECT_EQ(g.column("Empty"), "it is empty in every row");
    EXPECT_EQ(g.column("Long"), "it holds more text than a recording keeps");
    EXPECT_EQ(g.column(""), "its header cell is empty");
    EXPECT_EQ(g.column("A"), "used");
    EXPECT_EQ(g.column("Unit (-x)"), "used");
    /* A unit a map refuses is dropped, the column kept */
    EXPECT_EQ(g.sig("Unit (-x)", "unit"), "");
    EXPECT_EQ(g.sig("Unit (-x)", "name"), "Unit");
    EXPECT_TRUE(g.noted("'Unit (-x)''s unit is left out: a map refuses it"))
        << notes_of(g);
    EXPECT_EQ(g.r.left_out, 7u);
    EXPECT_EQ(g.r.columns, 2u);
    EXPECT_TRUE(g.noted("'=SUM' is left out: its name starts as a "
                        "spreadsheet formula does"))
        << notes_of(g);
    std::string why;
    EXPECT_TRUE(imports_map(g.map_text(), &why)) << why;
}

TEST(CsvGuessTest, PlaceholdersInNumbersHoldNoSample)
{
    Guessed g("time (s),A,B,C,D\n"
              "0,1.5,---,N/A,1\n"
              "1,NaN,2,x,2\n"
              "2,2.125,--,y,3\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.sig("A", "kind"), "number");
    EXPECT_EQ(g.sig("A", "decimals"), "3");
    EXPECT_EQ(g.sig("B", "kind"), "number");
    EXPECT_EQ(g.sig("C", "kind"), "text");
    /* In a column of numbers only, in the list's order */
    const std::vector<std::string> want = {"---", "--", "NaN"};
    EXPECT_EQ(g.missing(), want);
    EXPECT_TRUE(g.noted("'---', '--', 'NaN' in a column of numbers hold no "
                        "sample"))
        << notes_of(g);
    /* A column of placeholders alone is empty */
    Guessed only("time (s),A,B\n0,---,1\n1,NaN,2\n");
    ASSERT_TRUE(only.ok) << only.why;
    EXPECT_EQ(only.column("A"), "it is empty in every row");
}

/* A logger that pads each field with a space writes an empty one as " ":
 * blank, never text, wherever the guess decides (the header's rows, the
 * time, a column's kind), and a padded placeholder is still one */
TEST(CsvGuessTest, ACellOfSpacesIsEmpty)
{
    /* Half of each row blank: with " " counted as text no header passes */
    Guessed g("Time (ms), A, B, C, D, E\n"
              "0, , , 1, OL, \n"
              "250, , 3, 2, , x\n"
              "500,  , , 3, CL, \n"
              "750, \t, ---, 4, , \n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.sig(" B", "kind"), "number") << g.r.text;
    EXPECT_EQ(g.sig(" C", "kind"), "number");
    EXPECT_EQ(g.sig(" D", "kind"), "text");
    EXPECT_EQ(g.missing(), std::vector<std::string>{"---"});
    /* A number column with a value in under half the rows is said to set
     * a sweep's pace; a full one isn't */
    EXPECT_TRUE(g.noted("'B' holds a value in 1 of 4 rows read: a wide "
                        "export by sweep follows its rate; pick a row "
                        "interval"))
        << notes_of(g);
    EXPECT_FALSE(g.noted("'C' holds")) << notes_of(g);
    EXPECT_EQ(g.sig(" E", "kind"), "text");
    EXPECT_FALSE(g.noted("'E' holds")) << "a text column: " << notes_of(g);
    Guessed half("Time (ms), A\n0, 1\n1, \n");
    ASSERT_TRUE(half.ok) << half.why;
    EXPECT_FALSE(half.noted("holds a value")) << "one in two is not under half";
    /* A value in under one row in kSparseTimes (16) is far sparser: a
     * sweep holds it in the others' rows; one in 16 still sets the pace */
    const auto one_in = [](int rows) {
        std::string t = "Time (ms), A, B\n0, 1, 1\n";
        for (int i = 1; i < rows; ++i) {
            t += std::to_string(i) + ", " + std::to_string(i) + ", \n";
        }
        return t;
    };
    Guessed far(one_in(17));
    ASSERT_TRUE(far.ok) << far.why;
    EXPECT_TRUE(far.noted("'B' holds a value in 1 of 17 rows read: a wide "
                          "export by sweep holds its newest value in the "
                          "rows the other columns set"))
        << notes_of(far);
    Guessed close(one_in(16));
    ASSERT_TRUE(close.ok) << close.why;
    EXPECT_TRUE(close.noted("'B' holds a value in 1 of 16 rows read: a wide "
                            "export by sweep follows its rate; pick a row "
                            "interval"))
        << notes_of(close);
    EXPECT_EQ(g.column(" A"), "it is empty in every row");
    EXPECT_TRUE(g.noted("'A' is left out: it is empty in every row"))
        << "the header's text without its padding: " << notes_of(g);
    std::string why;
    EXPECT_TRUE(imports_map(g.map_text(), &why)) << why;
    /* The time column's blanks are skipped as empty ones are */
    Guessed t("Time, A\n0, 1\n , 2\n500, 3\n");
    ASSERT_TRUE(t.ok) << t.why;
    EXPECT_EQ(t.time("unit"), "ms");
    EXPECT_EQ(t.r.flags & guess::kTimeNumbers, guess::kTimeNumbers);
    /* One row read of a file cut after it */
    Guessed one("Time (s), A, B\n0, , 1\n", nullptr, true);
    ASSERT_TRUE(one.ok) << one.why;
    EXPECT_EQ(one.column(" A"),
              "it is empty in the one row read: a later value is imported "
              "only if it is put back");
}

/* The sample's end: a column empty in the rows read may hold a value
 * later, unless the rows read were the whole file */
TEST(CsvGuessTest, AnEmptyColumnSaysWhetherTheFileWentOn)
{
    std::string text = "Time (ms),A,B\n";
    for (int i = 0; i < 250; ++i) {
        text += std::to_string(i) + ",1,\n";
    }
    Guessed sampled(text);
    ASSERT_TRUE(sampled.ok) << sampled.why;
    EXPECT_EQ(sampled.column("B"),
              "it is empty in the first 200 rows read: a later value is "
              "imported only if it is put back");
    Guessed cut("Time (ms),A,B\n0,1,\n1,2,\n", nullptr, true);
    ASSERT_TRUE(cut.ok) << cut.why;
    EXPECT_EQ(cut.column("B"),
              "it is empty in the first 2 rows read: a later value is "
              "imported only if it is put back");
    Guessed whole("Time (ms),A,B\n0,1,\n1,2,\n");
    ASSERT_TRUE(whole.ok) << whole.why;
    EXPECT_EQ(whole.column("B"), "it is empty in every row");
}

/* A header cell of spaces alone is an empty one, never a unit's leftover */
TEST(CsvGuessTest, AHeaderCellOfSpacesIsEmpty)
{
    Guessed g("Time (ms), A, \n0, 1, \n1, 2, \n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.column(" "), "its header cell is empty");
    EXPECT_TRUE(g.noted("'' is left out: its header cell is empty"))
        << notes_of(g);
    EXPECT_EQ(g.r.left_out, 1u);
}

/* Padding around a header's quotes is taken, the quoted text exact */
TEST(CsvGuessTest, AQuotedHeaderCellMayBePadded)
{
    Guessed g("Time, \"Engine speed (rpm)\" \n0, 800\n263, 900\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.sig("Engine speed (rpm)", "name"), "Engine speed");
    EXPECT_EQ(g.sig("Engine speed (rpm)", "unit"), "rpm");
    EXPECT_EQ(g.time("unit"), "ms");
    std::string why;
    EXPECT_TRUE(imports_map(g.map_text(), &why)) << why;
}

/* A time column named so, of whole numbers with no unit, reads as ms, a
 * header's unit or a given one winning; a counter or a fraction has none */
TEST(CsvGuessTest, ABareNumericTimeIsMilliseconds)
{
    Guessed bare("Time, A\n0, 1\n263, 2\n573, 3\n");
    ASSERT_TRUE(bare.ok) << bare.why;
    EXPECT_EQ(bare.time("unit"), "ms");
    EXPECT_TRUE(bare.noted("the time column 'Time' has no unit: read as ms; "
                           "change it if it isn't"))
        << notes_of(bare);
    Guessed header("Time (s), A\n0, 1\n0.25, 2\n");
    ASSERT_TRUE(header.ok) << header.why;
    EXPECT_EQ(header.time("unit"), "s");
    EXPECT_FALSE(header.noted("has no unit")) << notes_of(header);
    Guessed given("Time, A\n0, 1\n263, 2\n", "s");
    ASSERT_TRUE(given.ok) << given.why;
    EXPECT_EQ(given.time("unit"), "s");
    EXPECT_FALSE(given.noted("has no unit")) << notes_of(given);
    /* Not numbers: no unit is guessed */
    Guessed clock("Time, A\n12:00:00, 1\n12:00:01, 2\n");
    ASSERT_TRUE(clock.ok) << clock.why;
    EXPECT_FALSE(clock.time_has_unit());
    /* A first column that only rises may count rows: no unit */
    Guessed counter("Sample,RPM\n1,800\n2,900\n");
    ASSERT_TRUE(counter.ok) << counter.why;
    EXPECT_EQ(counter.time("column"), "Sample");
    EXPECT_FALSE(counter.time_has_unit());
    EXPECT_FALSE(counter.noted("has no unit")) << notes_of(counter);
    /* A fraction may be seconds: no unit, so it can't import 1000x short */
    Guessed fraction("Time, A\n0.000, 1\n0.250, 2\n0.500, 3\n");
    ASSERT_TRUE(fraction.ok) << fraction.why;
    EXPECT_FALSE(fraction.time_has_unit());
    EXPECT_EQ(fraction.r.flags & guess::kTimeNumbers, guess::kTimeNumbers);
    EXPECT_TRUE(fraction.noted("'Time' is the time, its unit not known"))
        << notes_of(fraction);
    Guessed one_fraction("Time, A\n0, 1\n250, 2\n500.5, 3\n");
    ASSERT_TRUE(one_fraction.ok) << one_fraction.why;
    EXPECT_FALSE(one_fraction.time_has_unit()) << "every cell whole, or none";
    /* A padded name is still a time's name, its note the trimmed text */
    Guessed padded(" Time , A\n0, 1\n263, 2\n");
    ASSERT_TRUE(padded.ok) << padded.why;
    EXPECT_EQ(padded.time("unit"), "ms");
    EXPECT_TRUE(padded.noted("the time column 'Time' has no unit"))
        << notes_of(padded);
}

TEST(CsvGuessTest, AMapTakesTwoHundredFiftySixSignals)
{
    std::string head = "time (s)";
    std::string row0 = "0";
    std::string row1 = "1";
    for (int i = 0; i < 300; ++i) {
        head += ",c" + std::to_string(i);
        row0 += ",1";
        row1 += ",2";
    }
    Guessed g(head + "\n" + row0 + "\n" + row1 + "\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.signals().size(), 256u);
    EXPECT_EQ(g.r.left_out, 44u);
    EXPECT_EQ(g.column("c256"), "a map takes 256 signals");
    EXPECT_TRUE(g.noted("44 more columns are left out: a map takes 256 "
                        "signals"))
        << notes_of(g);
    std::string why;
    EXPECT_TRUE(imports_map(g.map_text(), &why)) << why;
}

TEST(CsvGuessTest, NotesPastSixtyFourAreCounted)
{
    std::string head = "time (s)";
    std::string row = "0";
    for (int i = 0; i < 70; ++i) {
        head += ",=f" + std::to_string(i); /* each a note */
        row += ",1";
    }
    head += ",A";
    Guessed g(head + ",\n" + row + ",1,\n" + "1" + row.substr(1) + ",2,\n");
    ASSERT_TRUE(g.ok) << g.why;
    const std::vector<std::string> n = g.notes();
    ASSERT_EQ(n.size(), guess::kNotesMax + 1);
    EXPECT_EQ(n.back(), "...and 8 more");
}

/* ---- the sample ---- */

TEST(CsvGuessTest, TheSampleIsTwoHundredRowsOrSixtyFourKibibytes)
{
    std::string text = "time (s),A\n";
    for (int i = 0; i < 500; ++i) {
        text += std::to_string(i) + ",1\n";
    }
    /* Text far down is never read */
    text += "500,late text\n";
    Guessed g(text);
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.r.rows_read, guess::kSampleRows);
    EXPECT_EQ(g.sig("A", "kind"), "number");
    std::string wide = "time (s),A,Note\n";
    const std::string cell(1000, 'n');
    for (int i = 0; i < 150; ++i) {
        wide += std::to_string(i) + ",1," + cell + "\n";
    }
    Guessed bytes(wide);
    ASSERT_TRUE(bytes.ok) << bytes.why;
    EXPECT_GT(bytes.r.rows_read, 60u);
    EXPECT_LT(bytes.r.rows_read, 70u); /* 64 KiB of 1 KB rows */
}

TEST(CsvGuessTest, ARowOfAnotherShapeIsNoted)
{
    /* Past the rows the header is judged on */
    const std::string five = "time (s),A\n0,1\n1,2\n2,3\n3,4\n4,5\n";
    Guessed g(five + "5,6,7\n6,7\n8\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.r.rows_read, 8u);
    EXPECT_TRUE(g.noted("line 7 and 1 more row read have another number of "
                        "fields than the header: the import refuses such a "
                        "row"))
        << notes_of(g);
    Guessed three(five + "5,6,7\n6,7\n8\n9,9,9\n");
    ASSERT_TRUE(three.ok) << three.why;
    EXPECT_TRUE(three.noted("line 7 and 2 more rows read have another number "
                            "of fields than the header"))
        << notes_of(three);
    Guessed one(five + "5,6,7\n");
    ASSERT_TRUE(one.ok) << one.why;
    EXPECT_TRUE(one.noted("line 7: the row has another number of fields than "
                          "the header: the import refuses it"))
        << notes_of(one);
    /* Among them, the line is no header */
    EXPECT_EQ(
        refused("time (s),A\n0,1\n1,2,3\n2,3\n").rfind("no row within", 0), 0u);
}

} // namespace
