// File/layer: tests/test_csv_import.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvImportTest (test_core)
//
// csv_import.h's column map, RFC 4180 reader and pass over bytes in
// memory (Pass), and ecuio_import's CSV in as a recording (Imported):
// its signals.csv and events.log read back through ecuio_recording_open,
// the report, each refusal at its line, each bound at its edge, and the
// two made-up loggers' files in tests/fixtures/import/csv/
// (ECUIO_IMPORT_FIXTURES, a compile definition). Each import case writes
// into a folder of its own under TEMP/TMPDIR, removed at the end.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "csv_import.h"
#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "partial_left.h"
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;
namespace csv = ecuio_csv_import;

namespace {

/* ---- in memory ---- */

struct Sample {
    uint64_t us;
    size_t signal;
    bool text;
    double number;
    std::string value;
};

struct Collect {
    std::vector<Sample> got;
    bool number(uint64_t us, size_t s, double v)
    {
        got.push_back({us, s, false, v, {}});
        return true;
    }
    bool text(uint64_t us, size_t s, const std::string& v)
    {
        got.push_back({us, s, true, 0, v});
        return true;
    }
    std::string failure() const { return "never"; }
};

/* The map parsed and the CSV run through it */
struct Pass {
    bool ok = false;
    std::string why;
    csv::column_map map;
    csv::outcome out;
    std::vector<Sample> got;
    Pass(const std::string& map_json, const std::string& text)
    {
        if (!csv::parse_map(map_json.data(), map_json.size(), map, &why)) {
            return;
        }
        csv::memory_source src{
            reinterpret_cast<const unsigned char*>(text.data()), text.size()};
        Collect c;
        ok = csv::run(src, map, c, out, &why);
        got = std::move(c.got);
    }
};

/* A map of a time column and signals, the rest of its keys in extra */
std::string map_of(const std::string& time_column, const std::string& unit,
                   const std::string& signals, const std::string& extra = "")
{
    return "{\"havne_column_map\": 1, \"time\": {\"column\": \"" + time_column +
           "\", \"unit\": \"" + unit + "\"}, \"signals\": [" + signals + "]" +
           (extra.empty() ? "" : ", " + extra) + "}";
}

const std::string kRpm =
    R"({"column": "Engine Speed", "name": "RPM", "unit": "rpm"})";
const std::string kGear =
    R"({"column": "Gear", "name": "Gear", "kind": "text"})";

std::string refused_map(const std::string& map_json)
{
    csv::column_map m;
    std::string why;
    EXPECT_FALSE(csv::parse_map(map_json.data(), map_json.size(), m, &why))
        << map_json;
    EXPECT_EQ(why.rfind("the column map: ", 0), 0u) << why;
    return why;
}

std::string refused(const std::string& map_json, const std::string& text)
{
    Pass p(map_json, text);
    EXPECT_FALSE(p.ok) << text;
    EXPECT_FALSE(p.why.empty());
    return p.why;
}

/* ---- the column map ---- */

TEST(CsvImportTest, AMapReadsEachKey)
{
    const std::string json = R"json({
  "havne_column_map": 1,
  "header_row": 3,
  "delimiter": ";",
  "decimal": ",",
  "time": {"column": "Time (s)", "unit": "s"},
  "start": "2026-11-05T22:03:22.125+09:00",
  "missing": ["---", "NaN"],
  "signals": [
    {"column": "Engine Speed", "name": "RPM", "unit": "rpm", "kind": "number",
     "decimals": 0, "rank": 4, "group": 1},
    {"column": "Gear", "name": "Gear", "kind": "text"},
    {"column": "Load", "name": "Load", "unit": "%"}
  ]
})json";
    csv::column_map m;
    std::string why;
    ASSERT_TRUE(csv::parse_map(json.data(), json.size(), m, &why)) << why;
    EXPECT_EQ(m.header_row, 3u);
    EXPECT_EQ(m.delimiter, ';');
    EXPECT_EQ(m.decimal, ',');
    EXPECT_EQ(m.time_column, "Time (s)");
    EXPECT_EQ(m.unit, csv::time_unit::s);
    ASSERT_TRUE(m.has_start);
    EXPECT_EQ(m.start_us, int64_t{1793883802125000}); /* 13:03:22.125Z */
    EXPECT_EQ(m.missing, (std::vector<std::string>{"---", "NaN"}));
    ASSERT_EQ(m.signals.size(), 3u);
    EXPECT_EQ(m.signals[0].name, "RPM");
    EXPECT_EQ(m.signals[0].decimals, 0u);
    EXPECT_EQ(m.signals[0].rank, 4u);
    EXPECT_EQ(m.signals[0].group, 1u);
    EXPECT_TRUE(m.signals[1].text);
    EXPECT_EQ(m.signals[1].rank, 0u); /* a text never plots */
    EXPECT_EQ(m.signals[1].decimals, 0u);
    EXPECT_EQ(m.signals[2].decimals, 2u);
    EXPECT_EQ(m.signals[2].rank, 3u); /* the map's order */
}

TEST(CsvImportTest, EveryMapRefusalSaysWhy)
{
    const struct {
        std::string json;
        const char* words;
    } cases[] = {
        {"[]", "isn't a JSON object"},
        {"{\"havne_column_map\": 1, \"time\": {}, // a comment\n}", "line 1"},
        {map_of("t", "s", kRpm, "\"shade\": 1"), "unknown key 'shade'"},
        {R"({"time": {"column": "t", "unit": "s"}, "signals": [{"column": "a", "name": "A"}]})",
         "\"havne_column_map\": 1"},
        {map_of("t", "s", kRpm, "\"havne_column_map\": 2"),
         "line 1"}, /* a key twice */
        {R"({"havne_column_map": 2, "time": {"column": "t", "unit": "s"}, "signals": [{"column": "a", "name": "A"}]})",
         "reads version 1 only"},
        {map_of("t", "minutes", kRpm), "\"unit\" in time"},
        {map_of("t", "s", kRpm, "\"delimiter\": \"|\""), "\"delimiter\""},
        {map_of("t", "s", kRpm, "\"decimal\": \",\""),
         "a decimal comma needs another delimiter"},
        {map_of("t", "s", kRpm, "\"header_row\": 0"),
         "\"header_row\" is 1 to 1000"},
        {map_of("t", "s", kRpm, "\"header_row\": 1001"),
         "\"header_row\" is 1 to 1000"},
        {map_of("t", "iso8601", kRpm, "\"start\": \"2026-11-05T14:03:22Z\""),
         "\"start\" is for a relative time only"},
        {map_of("t", "s", kRpm, "\"start\": \"2026-11-05 14:03\""),
         "\"start\" is an ISO 8601 time"},
        {map_of("t", "s", kRpm, "\"start\": \"2026-11-05T14:03:22\""),
         "\"start\" is an ISO 8601 time"},
        {R"({"havne_column_map": 1, "time": {"column": "t", "unit": "time-of-day"}, "signals": [{"column": "a", "name": "A"}]})",
         "\"date\" and \"utc_offset\""},
        {R"({"havne_column_map": 1, "time": {"column": "t", "unit": "s", "date": "2026-11-05"}, "signals": [{"column": "a", "name": "A"}]})",
         "\"date\" and \"utc_offset\""},
        {R"({"havne_column_map": 1, "time": {"column": "t", "unit": "time-of-day", "date": "2026-02-30", "utc_offset": "Z"}, "signals": [{"column": "a", "name": "A"}]})",
         "\"date\" in time is YYYY-MM-DD"},
        {R"({"havne_column_map": 1, "time": {"column": "t", "unit": "time-of-day", "date": "2026-11-05", "utc_offset": "+1"}, "signals": [{"column": "a", "name": "A"}]})",
         "\"utc_offset\""},
        {map_of("t", "s", ""), "\"signals\" is a list of 1 to 256"},
        {map_of("t", "s", R"({"column": "a"})"), "\"name\" is 1 to 31 bytes"},
        {map_of(
             "t", "s",
             R"({"column": "a", "name": "0123456789012345678901234567890X"})"),
         "\"name\" is 1 to 31 bytes"},
        {map_of("t", "s",
                R"({"column": "a", "name": "A", "unit": "0123456789abcdefX"})"),
         "\"unit\" is up to 15 bytes"},
        {map_of("t", "s", R"({"column": "a", "name": "A", "kind": "enum"})"),
         "\"kind\" is \"number\" or \"text\""},
        {map_of(
             "t", "s",
             R"({"column": "a", "name": "A", "kind": "text", "decimals": 1})"),
         "\"decimals\" is 0 to 3, for a number"},
        {map_of("t", "s", R"({"column": "a", "name": "A", "decimals": 4})"),
         "\"decimals\" is 0 to 3"},
        {map_of("t", "s", R"({"column": "a", "name": "A", "rank": 256})"),
         "\"rank\" is 0 to 255"},
        {map_of("t", "s", R"({"column": "a", "name": "=1+1"})"),
         "runs as a formula"},
        {map_of("t", "s", R"({"column": "a", "name": "A", "unit": "-x"})"),
         "runs as a formula"},
        {map_of("t", "s", R"({"column": "a", "name": "A\u0007"})"),
         "\"name\" is 1 to 31 bytes"},
        {map_of("t", "s", R"({"column": "a", "name": "A", "x": 1})"),
         "unknown key 'x' in signal 1"},
        {map_of("t", "s", kRpm + "," + R"({"column": "b", "name": "RPM"})"),
         "two signals are named 'RPM'"},
        {map_of("t", "s",
                kRpm + "," + R"({"column": "Engine Speed", "name": "B"})"),
         "two signals read the column 'Engine Speed'"},
        {map_of("t", "s", R"({"column": "t", "name": "T"})"),
         "the time column is a signal's too"},
        {map_of("t", "s", kRpm, "\"missing\": [\"\"]"), "each of \"missing\""},
        {map_of("t", "s", kRpm,
                "\"missing\": [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\","
                "\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\"]"),
         "\"missing\" is a list of up to 16"},
    };
    for (const auto& c : cases) {
        const std::string why = refused_map(c.json);
        EXPECT_NE(why.find(c.words), std::string::npos)
            << "map: " << c.json << "\nwhy: " << why;
    }
}

TEST(CsvImportTest, TheMapsBoundsHoldAtTheirEdges)
{
    std::string sigs;
    for (int i = 0; i < 256; ++i) {
        sigs += (i ? "," : "") + std::string("{\"column\": \"c") +
                std::to_string(i) + "\", \"name\": \"s" + std::to_string(i) +
                "\"}";
    }
    csv::column_map m;
    std::string why;
    std::string json = map_of("t", "s", sigs, "\"header_row\": 1000");
    ASSERT_TRUE(csv::parse_map(json.data(), json.size(), m, &why)) << why;
    EXPECT_EQ(m.signals.size(), 256u);
    EXPECT_EQ(m.signals[255].rank, 255u); /* ranks past 255 stay 255 */
    json = map_of("t", "s", sigs + R"(,{"column": "x", "name": "X"})");
    EXPECT_NE(refused_map(json).find("1 to 256"), std::string::npos);
}

/* ---- the reader and the pass ---- */

TEST(CsvImportTest, QuotedFieldsKeepDelimitersQuotesAndLineBreaks)
{
    const std::string text = "Time,Engine Speed,Gear\r\n"
                             "0.0,800,\"N, idle\"\r\n"
                             "0.5,\"1200\",\"say \"\"2\"\"\"\r\n"
                             "1.0,1500,\"two\nlines\"\r\n";
    Pass p(map_of("Time", "s", kRpm + "," + kGear), text);
    ASSERT_TRUE(p.ok) << p.why;
    ASSERT_EQ(p.got.size(), 6u);
    EXPECT_EQ(p.got[1].value, "N, idle");
    EXPECT_EQ(p.got[2].number, 1200.0);
    EXPECT_EQ(p.got[3].value, "say \"2\"");
    EXPECT_EQ(p.got[5].value, "two\nlines");
    EXPECT_EQ(p.got[4].us, 1000000u);
    EXPECT_EQ(p.out.lines, 5u); /* the quoted line break counts */
    EXPECT_EQ(p.out.rows, 3u);
    EXPECT_EQ(p.out.values, 6u);
}

TEST(CsvImportTest, ABomAPreambleBlankLinesAndNoLastLineBreakAreRead)
{
    const std::string text = "\xEF\xBB\xBF"
                             "Logger X v2, \"unbalanced\n"
                             "exported 2026\n"
                             "Time;Engine Speed;Extra\n"
                             "\n"
                             "0,1;800,5;x\n"
                             "0,2;810;y";
    Pass p(
        map_of("Time", "s", kRpm,
               "\"header_row\": 3, \"delimiter\": \";\", \"decimal\": \",\""),
        text);
    ASSERT_TRUE(p.ok) << p.why;
    ASSERT_EQ(p.got.size(), 2u);
    EXPECT_EQ(p.got[0].number, 800.5);
    EXPECT_EQ(p.got[0].us, 0u);
    EXPECT_EQ(p.got[1].us, 100000u);
    EXPECT_EQ(p.out.ignored, (std::vector<std::string>{"Extra"}));
    EXPECT_EQ(p.out.lines, 6u);

    /* The BOM only at the file's start: a header named after one is that
     * name with its bytes */
    Pass bom(map_of("Time", "s", kRpm), "\xEF\xBB\xBFTime,Engine Speed\n1,2\n");
    EXPECT_TRUE(bom.ok) << bom.why;
}

TEST(CsvImportTest, EveryReaderRefusalNamesItsLine)
{
    const std::string m = map_of("Time", "s", kRpm);
    const struct {
        std::string text;
        const char* words;
    } cases[] = {
        {"Time,Engine Speed\n0,800\n1,900,3\n",
         "line 3: the row has 3 fields where the header has 2"},
        {"Time,Engine Speed,Load\n0,800,10\n1,900\n",
         "line 3: the row has 2 fields where the header has 3"},
        {"Time,Engine Speed\n0,80\"0\n", "line 2: a quote inside a field"},
        {"Time,Engine Speed\n0,\"800\"x\n",
         "line 2: text after a quoted field's closing quote"},
        {"Time,Engine Speed\n0,\"800\n1,2\n",
         "line 2: a quoted field is never closed"},
        {"Time,Engine Speed\n0,800\r1,2\n",
         "line 2: a carriage return without a line feed"},
        {std::string("Time,Engine Speed\n0,8\0"
                     "00\n",
                     25),
         "line 2: the row holds a NUL byte"},
        {"Time,Engine Speed\n0,\xC3\x28\n",
         "line 2: the row isn't valid UTF-8"},
        {"Time,Engine Speed\n0,\xE0\x80\xAF\n",
         "line 2: the row isn't valid UTF-8"},
        {"Time,Engine Speed\n0,\xED\xA0\x80\n",
         "line 2: the row isn't valid UTF-8"},
        {"Time,Speed\n0,800\n",
         "line 1: the header has no column 'Engine Speed'"},
        {"Time,Engine Speed,Engine Speed\n0,1,2\n",
         "line 1: the header names a column twice: 'Engine Speed'"},
        {"Time,Engine Speed\n", "the file holds no rows after its header"},
        {"", "the file ends before its header row"},
        {"Time,Engine Speed\n,800\n", "line 2: the row has no time"},
        {"Time,Engine Speed\nzero,800\n",
         "line 2: its time isn't a number of seconds"},
        {"Time,Engine Speed\n0,eight\n",
         "line 2: column 'Engine Speed' isn't a number"},
        {"Time,Engine Speed\n0,nan\n",
         "line 2: column 'Engine Speed' isn't a number"},
        {"Time,Engine Speed\n0,1e999\n",
         "line 2: column 'Engine Speed' isn't a number"},
        {"Time,Engine Speed\n2,800\n1.8,800\n",
         "line 3: its time is more than 0.1 s before the row before it"},
        {"Time,Engine Speed\n1e10,800\n", "line 2: its time isn't"},
    };
    for (const auto& c : cases) {
        const std::string why = refused(m, c.text);
        EXPECT_NE(why.find(c.words), std::string::npos)
            << "text: " << c.text << "\nwhy: " << why;
    }
}

TEST(CsvImportTest, ATimeALittleBackTakesTheRowBeforesTime)
{
    Pass p(map_of("Time", "ms", kRpm),
           "Time,Engine Speed\n1000,1\n950,2\n900.5,3\n1200,4\n");
    ASSERT_TRUE(p.ok) << p.why;
    ASSERT_EQ(p.got.size(), 4u);
    EXPECT_EQ(p.got[1].us, 0u);
    EXPECT_EQ(p.got[2].us, 0u); /* 99.5 ms back from 1000: still 1000 */
    EXPECT_EQ(p.got[3].us, 200000u);
    /* exactly 100 ms back is taken, a microsecond more refused */
    EXPECT_TRUE(
        Pass(map_of("Time", "us", kRpm), "Time,Engine Speed\n100000,1\n0,2\n")
            .ok);
    EXPECT_FALSE(
        Pass(map_of("Time", "us", kRpm), "Time,Engine Speed\n100001,1\n0,2\n")
            .ok);
}

TEST(CsvImportTest, EachTimeFormReadsToItsMicroseconds)
{
    Pass s(map_of("t", "s", kRpm), "t,Engine Speed\n-1.5,1\n0.000001,2\n");
    ASSERT_TRUE(s.ok) << s.why;
    EXPECT_EQ(s.got[1].us, 1500001u);
    EXPECT_FALSE(s.out.has_utc); /* no start: no UTC time */

    Pass start(
        map_of("t", "s", kRpm, "\"start\": \"2026-11-05T22:03:22.125+09:00\""),
        "t,Engine Speed\n12.5,1\n13.5,2\n");
    ASSERT_TRUE(start.ok) << start.why;
    EXPECT_TRUE(start.out.has_utc);
    EXPECT_EQ(start.out.utc_start_us, 1793883802125000u); /* the first row's */
    EXPECT_EQ(start.out.duration_us, 1000000u);

    Pass iso(map_of("t", "iso8601", kRpm),
             "t,Engine Speed\n2026-11-05T13:03:22.125Z,1\n"
             "2026-11-05 23:03:22.6+10:00,2\n2026-11-05T08:03:23-05:00,3\n");
    ASSERT_TRUE(iso.ok) << iso.why;
    EXPECT_EQ(iso.out.utc_start_us, 1793883802125000u);
    EXPECT_EQ(iso.got[1].us, 475000u);
    EXPECT_EQ(iso.got[2].us, 875000u);

    const std::string tod_map =
        R"({"havne_column_map": 1, "decimal": ",", "delimiter": ";",
        "time": {"column": "t", "unit": "time-of-day", "date": "2026-11-05", "utc_offset": "-05:00"},
        "signals": [{"column": "Engine Speed", "name": "RPM"}]})";
    Pass tod(tod_map, "t;Engine Speed\n23:59:59,5;1\n00:00:00,25;2\n");
    ASSERT_TRUE(tod.ok) << tod.why;
    /* 2026-11-05 23:59:59.5 at -05:00 is 04:59:59.5Z on the 6th; past midnight the next day */
    EXPECT_EQ(tod.out.utc_start_us, 1793941199500000u);
    EXPECT_EQ(tod.got[1].us, 750000u);

    for (const char* bad :
         {"2026-11-05T13:03:22", "2026-11-05T13:03:22.1234567Z",
          "2026-13-05T13:03:22Z", "1969-12-31T23:59:59Z",
          "2026-11-05T24:00:00Z", "2026-11-05T13:03:22+15:00"}) {
        EXPECT_FALSE(Pass(map_of("t", "iso8601", kRpm),
                          std::string("t,Engine Speed\n") + bad + ",1\n")
                         .ok)
            << bad;
    }
}

TEST(CsvImportTest, EmptyCellsAndTheMapsMissingTextsAreNoSample)
{
    Pass p(
        map_of("t", "s", kRpm + "," + kGear, "\"missing\": [\"---\", \"NaN\"]"),
        "t,Engine Speed,Gear\n0,,N\n1,---,---\n2,NaN,3\n3,900,\n");
    ASSERT_TRUE(p.ok) << p.why;
    ASSERT_EQ(p.got.size(), 3u);
    EXPECT_EQ(p.got[0].value, "N");
    EXPECT_EQ(p.got[1].value, "3");
    EXPECT_EQ(p.got[2].number, 900.0);
    /* another placeholder is refused */
    EXPECT_NE(refused(map_of("t", "s", kRpm), "t,Engine Speed\n0,n/a\n")
                  .find("isn't a number"),
              std::string::npos);
}

/* A logger that pads each field with a space writes an empty one as " ":
 * below the header an unquoted cell's spaces and tabs around it are
 * padding, so " " is no sample (never a refusal or a zero), " N" is "N"
 * and " ---" a placeholder; a quoted cell keeps its text exactly, and
 * padding may stand around its quotes; a time of spaces alone is none */
TEST(CsvImportTest, PaddingAroundACellIsDropped)
{
    Pass p(map_of("t", "ms", kRpm + "," + kGear, "\"missing\": [\"---\"]"),
           "t,Engine Speed,Gear\n0, 800, N\n250, , 1\n500,  \t, \t\n"
           "750, ---, 2 \n1000, 900, \" CL \" \n1250, 950,\"\"\n");
    ASSERT_TRUE(p.ok) << p.why;
    ASSERT_EQ(p.got.size(), 7u);
    EXPECT_EQ(p.got[0].number, 800.0);
    EXPECT_EQ(p.got[1].value, "N");
    EXPECT_EQ(p.got[2].value, "1");
    EXPECT_EQ(p.got[3].value, "2");
    EXPECT_EQ(p.got[4].number, 900.0);
    EXPECT_EQ(p.got[4].us, 1000000u);
    EXPECT_EQ(p.got[5].value, " CL ") << "a quoted cell is kept exactly";
    EXPECT_EQ(p.got[6].number, 950.0);
    EXPECT_EQ(p.out.values, 7u);
    EXPECT_EQ(
        refused(map_of("t", "ms", kRpm), "t,Engine Speed\n0,800\n  ,900\n"),
        "line 3: the row has no time");
    /* Padding is spaces and tabs alone */
    EXPECT_EQ(refused(map_of("t", "ms", kGear), "t,Gear\n0, \"N\" x\n"),
              "line 2: text after a quoted field's closing quote");
    EXPECT_EQ(refused(map_of("t", "ms", kGear), "t,Gear\n0, a\"N\"\n"),
              "line 2: a quote inside a field that isn't quoted");
    /* A quoted space is text, so a number column refuses it */
    EXPECT_NE(refused(map_of("t", "ms", kRpm), "t,Engine Speed\n0,\" \"\n")
                  .find("isn't a number"),
              std::string::npos);
    /* The header keeps its text: a map names a padded column as it is */
    Pass header(
        map_of("t", "ms", R"({"column": " Engine Speed", "name": "RPM"})"),
        "t, Engine Speed\n0, 800\n");
    ASSERT_TRUE(header.ok) << header.why;
    ASSERT_EQ(header.got.size(), 1u);
    EXPECT_EQ(header.got[0].number, 800.0);
    /* Padding around a header's quotes is taken; the quoted text names it */
    Pass quoted(
        map_of("t", "ms",
               R"json({"column": "Engine speed (rpm)", "name": "RPM"})json"),
        "t, \"Engine speed (rpm)\" \n0, 800\n");
    ASSERT_TRUE(quoted.ok) << quoted.why;
    ASSERT_EQ(quoted.got.size(), 1u);
    EXPECT_EQ(quoted.got[0].number, 800.0);
}

TEST(CsvImportTest, ADecimalCommaReadsAndAPointIsRefused)
{
    const std::string m =
        map_of("t", "s", kRpm, "\"delimiter\": \"\\t\", \"decimal\": \",\"");
    Pass p(m, "t\tEngine Speed\n0,25\t 1234,5 \n");
    ASSERT_TRUE(p.ok) << p.why;
    EXPECT_EQ(p.got[0].number, 1234.5);
    EXPECT_NE(
        refused(m, "t\tEngine Speed\n0\t1.234,5\n").find("isn't a number"),
        std::string::npos);
}

TEST(CsvImportTest, TheReadersBoundsHoldAtTheirEdges)
{
    /* 1024 columns read, 1025 refused */
    auto wide = [](size_t n) {
        std::string head = "t";
        std::string row = "0";
        for (size_t i = 1; i < n; ++i) {
            head += ",c" + std::to_string(i);
            row += ",1";
        }
        return head + "\n" + row + "\n";
    };
    const std::string m = map_of("t", "s", R"({"column": "c1", "name": "C"})");
    EXPECT_TRUE(Pass(m, wide(1024)).ok);
    EXPECT_NE(refused(m, wide(1025))
                  .find("line 1: the row has more than 1024 columns"),
              std::string::npos);

    /* a row of 64 KiB read, a byte more refused */
    const std::string head = "t,c1\n";
    const std::string row_at =
        "0,\"" + std::string(csv::kRowBytesMax - 5, 'a') + "\"\n";
    const std::string mt =
        map_of("t", "s", R"({"column": "c1", "name": "C", "kind": "text"})");
    /* the text bound comes first for a long text cell: under it, the row's */
    EXPECT_NE(refused(mt, head + row_at)
                  .find("holds more text than a recording keeps"),
              std::string::npos);
    const std::string ignored_wide =
        "t,c1,pad\n0,x,\"" + std::string(csv::kRowBytesMax - 7, 'b') + "\"\n";
    EXPECT_TRUE(Pass(mt, ignored_wide).ok) << Pass(mt, ignored_wide).why;
    const std::string too_wide =
        "t,c1,pad\n0,x,\"" + std::string(csv::kRowBytesMax - 6, 'b') + "\"\n";
    EXPECT_NE(
        refused(mt, too_wide).find("line 2: the row is longer than 64 KiB"),
        std::string::npos);

    /* a text cell of the recording's bound kept, a byte more refused */
    EXPECT_TRUE(
        Pass(mt, head + "0," + std::string(csv::kTextMax, 'c') + "\n").ok);
    EXPECT_FALSE(
        Pass(mt, head + "0," + std::string(csv::kTextMax + 1, 'c') + "\n").ok);
}

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
