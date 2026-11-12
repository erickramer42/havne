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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "csv_guess.h"
#include "csv_import.h"
#include "json.h"
#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;
namespace csv = ecuio_csv_import;
namespace guess = ecuio_map_guess;
namespace json = ecuio_json;

namespace {

/* ---- in memory ---- */

/* The guess over text, its JSON parsed */
struct Guessed {
    bool ok = false;
    std::string why;
    guess::result r;
    json::Value v;
    explicit Guessed(const std::string& text, const char* unit = nullptr,
                     bool cut = false)
    {
        ok = guess::guess(reinterpret_cast<const unsigned char*>(text.data()),
                          text.size(), cut, unit, r, &why);
        if (!ok) {
            return;
        }
        json::Error e;
        EXPECT_TRUE(json::parse(r.text.data(), r.text.size(), &v, &e))
            << e.to_string() << "\n"
            << r.text;
    }
    const json::Value& map() const { return *v.find("map"); }
    std::string map_text() const
    {
        return r.text.substr(r.map_offset, r.map_length);
    }
    /* A map key's text, or "" */
    std::string key(const char* k) const
    {
        const json::Value* x = map().find(k);
        return x != nullptr && x->type == json::Type::string ? x->text : "";
    }
    int64_t number(const char* k) const
    {
        const json::Value* x = map().find(k);
        return x != nullptr ? x->whole : -1;
    }
    std::string time(const char* k) const
    {
        const json::Value* t = map().find("time");
        const json::Value* x = t != nullptr ? t->find(k) : nullptr;
        return x != nullptr ? x->text : "";
    }
    bool time_has_unit() const { return map().find("time")->find("unit"); }
    const std::vector<json::Value>& signals() const
    {
        return map().find("signals")->items;
    }
    /* A signal's key by its column, or "-" for no signal, "" for none */
    std::string sig(const std::string& column, const char* k) const
    {
        for (const json::Value& s : signals()) {
            if (s.find("column")->text == column) {
                const json::Value* x = s.find(k);
                if (x == nullptr) {
                    return "";
                }
                return x->type == json::Type::string ? x->text
                                                     : std::to_string(x->whole);
            }
        }
        return "-";
    }
    std::vector<std::string> missing() const
    {
        std::vector<std::string> out;
        if (const json::Value* m = map().find("missing")) {
            for (const json::Value& x : m->items) {
                out.push_back(x.text);
            }
        }
        return out;
    }
    /* The columns list's why for a header text, "used" when used */
    std::string column(const std::string& c) const
    {
        for (const json::Value& x : v.find("columns")->items) {
            if (x.find("column")->text == c) {
                return x.find("used")->boolean ? "used" : x.find("why")->text;
            }
        }
        return "not listed";
    }
    std::vector<std::string> notes() const
    {
        std::vector<std::string> out;
        for (const json::Value& x : v.find("notes")->items) {
            out.push_back(x.text);
        }
        return out;
    }
    bool noted(const std::string& words) const
    {
        for (const std::string& n : notes()) {
            if (n.find(words) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

std::string refused(const std::string& text, const char* unit = nullptr)
{
    Guessed g(text, unit);
    EXPECT_FALSE(g.ok) << g.r.text;
    EXPECT_FALSE(g.why.empty());
    return g.why;
}

std::string notes_of(const Guessed& g)
{
    std::string all;
    for (const std::string& n : g.notes()) {
        all += n + "\n";
    }
    return all;
}

/* The map parsed by the import's own parser */
bool imports_map(const std::string& map_text, std::string* why)
{
    csv::column_map m;
    return csv::parse_map(map_text.data(), map_text.size(), m, why);
}

const std::string kSimple = "Time (s),Speed (km/h),Gear\n"
                            "0,0.0,N\n"
                            "0.5,1.25,1\n"
                            "1.0,2.5,1\n";

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

/* Each row below a header holds mostly numbers, so a preamble line of the
 * data's field count is never taken for it */
TEST(CsvGuessTest, APreambleLineOfTheDatasShapeIsNoHeader)
{
    Guessed g("Logger,X1\ntime,rpm\n0,800\n1,900\n");
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.number("header_row"), 2);
    EXPECT_EQ(g.time("column"), "time");
    /* A row of empty cells says nothing either way */
    Guessed blank("time,rpm\n,\n0,800\n1,900\n");
    ASSERT_TRUE(blank.ok) << blank.why;
    EXPECT_EQ(blank.number("header_row"), 1);
    /* A line of which exactly half the cells are numbers isn't a header
     * (more than half must not be) */
    EXPECT_FALSE(refused("2024,Notes\n0,800\n1,900\n").empty());
}

TEST(CsvGuessTest, AQuoteTheReadsEndCutShortEndsTheSample)
{
    const std::string text = "time (s),A\n0,1\n1,\"open";
    Guessed cut(text, nullptr, true);
    ASSERT_TRUE(cut.ok) << cut.why;
    EXPECT_EQ(cut.r.rows_read, 1u);
    /* The whole file: the reader's refusal, at its line */
    EXPECT_EQ(refused(text), "line 3: a quoted field is never closed");
}

TEST(CsvGuessTest, EachRefusalSaysWhy)
{
    EXPECT_EQ(refused(""), "the file is empty");
    const std::string no_header =
        "no row within the file's first 1000 reads as a header with rows of "
        "numbers below it, split by a comma, a semicolon or a tab";
    EXPECT_EQ(refused("1,2\n3,4\n"), no_header);
    EXPECT_EQ(refused("a,b\nc,d\n"), no_header);
    EXPECT_EQ(refused("only\n1\n"), no_header);
    EXPECT_EQ(refused("time (s),A\n"), no_header);
    EXPECT_EQ(refused("time (s),A\n0,\n1,\n"),
              "no column besides the time holds values in the rows read");
    EXPECT_NE(
        refused("A,B,C\nx,1,2\ny,2,3\n").find("no column reads as the time"),
        std::string::npos);
    /* A header past the 1000th line isn't looked for */
    std::string far(1000, '\n');
    EXPECT_EQ(refused(far + "time (s),A\n0,1\n1,2\n"), no_header);
    Guessed near(std::string(999, '\n') + "time (s),A\n0,1\n1,2\n");
    ASSERT_TRUE(near.ok) << near.why;
    EXPECT_EQ(near.number("header_row"), 1000);
    /* A preamble line the import refuses: in the import's words, as a map
     * with that header_row gets them */
    const std::string rows = "time (s),A\n0,1\n1,2\n";
    EXPECT_EQ(refused(std::string(70000, 'p') + "\n" + rows),
              "line 1: the row is longer than 64 KiB");
    EXPECT_EQ(refused("pre\nam" + std::string(1, '\0') + "ble\n" + rows),
              "line 2: the file holds a NUL byte");
}

/* Rows that run past the header check's 64 KiB window: the row the window
 * ends inside isn't one of another shape */
TEST(CsvGuessTest, RowsPastTheCheckWindowKeepTheirHeader)
{
    /* A wide text cell in the middle, so the cut row has fewer fields */
    std::string wide = "time,Note,rpm\n";
    for (int i = 0; i < 10; ++i) {
        wide += std::to_string(i) + "," + std::string(15000, 'n') + ",800\n";
    }
    Guessed g(wide);
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.number("header_row"), 1);
    /* A thousand columns of numbers */
    std::string many = "time";
    for (int c = 1; c < 1000; ++c) {
        many += ",c" + std::to_string(c);
    }
    many += "\n";
    for (int i = 0; i < 10; ++i) {
        many += std::to_string(i);
        for (int c = 1; c < 1000; ++c) {
            many += ",1234567.891234";
        }
        many += "\n";
    }
    Guessed m(many);
    ASSERT_TRUE(m.ok) << m.why;
    EXPECT_EQ(m.number("header_row"), 1);
    /* A row whose line feed is the window's last byte is whole */
    const std::string head = "time,Note,rpm\n";
    const std::string exact =
        head + "0," +
        std::string(guess::kCheckBytes - head.size() - 2 - 5, 'n') +
        ",800\n";
    ASSERT_EQ(exact.size(), guess::kCheckBytes);
    Guessed at_end(exact + "1,x,900\n");
    ASSERT_TRUE(at_end.ok) << at_end.why;
    EXPECT_EQ(at_end.number("header_row"), 1);
    /* The window ending inside the first row leaves no row to judge by */
    EXPECT_EQ(refused("time," + std::string(40000, 'N') + ",rpm\n0," +
                      std::string(30000, 'n') + ",800\n1,x,900\n"),
              "no row within the file's first 1000 reads as a header with "
              "rows of numbers below it, split by a comma, a semicolon or a "
              "tab");
}

/* A guessed map the import takes, its slice imported over the same bytes */
TEST(CsvGuessTest, EveryGuessedMapImportsItsOwnBytes)
{
    const std::string texts[] = {
        kSimple,
        "\xEF\xBB\xBF"
        "Time [s];A [V];B\r\n0,5;1,25;x\r\n1,0;2;y\r\n",
        "pre\n\"time\",\"A, quoted\"\n\"0\",\"1\"\n\"1\",\"2\"\n",
        "timestamp\tA\n2026-01-01T00:00:00Z\t1\n2026-01-01T00:00:01Z\t2\n",
    };
    for (const std::string& t : texts) {
        SCOPED_TRACE(t);
        Guessed g(t, "s");
        ASSERT_TRUE(g.ok) << g.why;
        csv::column_map m;
        std::string why;
        const std::string mt = g.map_text();
        ASSERT_TRUE(csv::parse_map(mt.data(), mt.size(), m, &why)) << why;
        csv::memory_source src{reinterpret_cast<const unsigned char*>(t.data()),
                               t.size()};
        csv::counting_sink count;
        csv::outcome out;
        EXPECT_TRUE(csv::run(src, m, count, out, &why)) << why;
        EXPECT_GT(out.values, 0u);
    }
}

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
