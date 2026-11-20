// File/layer: tests/test_csv_guess_rows.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CsvGuessTest (test_core)

#include "test_csv_guess_support.h"

namespace {

/* The guess's map over the same bytes, as the import runs it */
struct Ran {
    bool ok = false;
    std::string why;
    csv::outcome out;
    Ran(const Guessed& g, const std::string& text)
    {
        csv::column_map m;
        const std::string mt = g.map_text();
        if (!csv::parse_map(mt.data(), mt.size(), m, &why)) {
            return;
        }
        csv::memory_source src{
            reinterpret_cast<const unsigned char*>(text.data()), text.size()};
        csv::counting_sink count;
        ok = csv::run(src, m, count, out, &why);
    }
};

/* A row with no time: the import leaves it out, so the guess says how
 * many of the rows it read are such, decides every signal without them,
 * and its map imports the file with the same rows left out */
TEST(CsvGuessTest, ARowWithNoTimeIsNotedAndDecidesNoSignal)
{
    /* A's text, S's two values and all of E's are in rows with no time */
    const std::string six = "time (s),A,B,S,E\n0,1,a,,\n1,2,b,7,\n2,3,c,,\n"
                            "3,4,d,,\n4,5,e,8,\n";
    const std::string text = six + ",x,f,9,1\n5,6,g,,\n , , ,9 , 2\n";
    Guessed g(text);
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.r.rows_read, 8u);
    EXPECT_TRUE(g.noted("2 of the 8 rows read have no time (the first on line "
                        "7): the import leaves such a row out"))
        << notes_of(g);
    EXPECT_EQ(g.sig("A", "kind"), "number");
    EXPECT_EQ(g.sig("B", "kind"), "text");
    /* Past the drop a count is of the rows with a time, and says so */
    EXPECT_TRUE(g.noted("'S' holds a value in 2 of the 6 rows with a time: a "
                        "wide export by sweep follows its rate; pick a row "
                        "interval"))
        << notes_of(g);
    EXPECT_EQ(g.column("E"), "it is empty in every row with a time");
    Guessed longer(text, nullptr, true);
    ASSERT_TRUE(longer.ok) << longer.why;
    EXPECT_EQ(longer.column("E"),
              "it is empty in the first 6 rows with a time: a later value is "
              "imported only if it is put back");
    Guessed lone("time (s),A,E\n0,1,\n,2,\n", nullptr, true);
    ASSERT_TRUE(lone.ok) << lone.why;
    EXPECT_EQ(lone.column("E"),
              "it is empty in the one row with a time: a later value is "
              "imported only if it is put back");
    const Ran ran(g, text);
    ASSERT_TRUE(ran.ok) << ran.why;
    EXPECT_EQ(ran.out.rows_no_time, 2u);
    EXPECT_EQ(ran.out.first_no_time, 7u);
    EXPECT_EQ(ran.out.values, 6u * 2u + 2u);

    /* One row: its line */
    Guessed one(six + ",9,z,,\n");
    ASSERT_TRUE(one.ok) << one.why;
    EXPECT_TRUE(one.noted("1 of the 6 rows read has no time (line 7): the "
                          "import leaves it out"))
        << notes_of(one);
    /* None: no note, and the counts are of the rows read */
    Guessed none(six);
    ASSERT_TRUE(none.ok) << none.why;
    EXPECT_FALSE(none.noted("no time")) << notes_of(none);
    EXPECT_TRUE(none.noted("'S' holds a value in 2 of 5 rows read: "))
        << notes_of(none);
    EXPECT_EQ(none.column("E"), "it is empty in every row");
    /* "Rows read" is every row read, one of another field count too */
    Guessed mixed(six + "9,9,9,9,9,9\n,9,z,,\n");
    ASSERT_TRUE(mixed.ok) << mixed.why;
    EXPECT_EQ(mixed.r.rows_read, 7u);
    EXPECT_TRUE(mixed.noted("1 of the 7 rows read has no time (line 8): the "
                            "import leaves it out"))
        << notes_of(mixed);
    EXPECT_TRUE(mixed.noted("'S' holds a value in 2 of the 5 rows with a time"))
        << notes_of(mixed);

    /* A first column that rises around an empty cell is the time; one
     * that falls across it is none */
    const std::string clock = "Clock,RPM\n0,800\n,850\n1,900\n";
    Guessed first(clock, "s");
    ASSERT_TRUE(first.ok) << first.why;
    EXPECT_EQ(first.time("column"), "Clock");
    EXPECT_TRUE(first.noted("1 of the 3 rows read has no time (line 3)"))
        << notes_of(first);
    const Ran first_ran(first, clock);
    ASSERT_TRUE(first_ran.ok) << first_ran.why;
    EXPECT_EQ(first_ran.out.rows_no_time, 1u);
    EXPECT_NE(refused("Clock,RPM\n1,800\n,850\n0,900\n")
                  .find("no column reads as the time"),
              std::string::npos);
    /* A first column filled in half the rows or fewer is a sparse counter,
     * not the time: taking it would leave half the file out */
    EXPECT_NE(refused("Counter,RPM\n1,800\n,810\n,820\n2,830\n")
                  .find("no column reads as the time"),
              std::string::npos);
    Guessed most("Counter,RPM\n1,800\n,810\n2,820\n,830\n3,840\n", "s");
    ASSERT_TRUE(most.ok) << most.why;
    EXPECT_EQ(most.time("column"), "Counter");
    /* A column named as the time is the time however sparse, and noted */
    Guessed named("time,RPM\n1,800\n,810\n,820\n2,830\n");
    ASSERT_TRUE(named.ok) << named.why;
    EXPECT_TRUE(named.noted("2 of the 4 rows read have no time"))
        << notes_of(named);

    /* No row with a time: the import's refusal when the sample is the
     * whole file, a note when the file goes on past it */
    const std::string empty = "time,rpm\n,800\n,900\n";
    EXPECT_EQ(refused(empty), "no row below the header has a time");
    Guessed more(empty, nullptr, true);
    ASSERT_TRUE(more.ok) << more.why;
    EXPECT_TRUE(more.noted("the time column 'time' is empty in the rows read"))
        << notes_of(more);
    EXPECT_FALSE(more.noted("the import leaves")) << notes_of(more);
    EXPECT_EQ(more.sig("rpm", "kind"), "number");
    /* None named and the first column empty throughout: no time to name */
    EXPECT_NE(
        refused("Clock,rpm\n,800\n,900\n").find("no column reads as the time"),
        std::string::npos);
}

/* A row with no time is left out by the import unread, so its text can't
 * refuse a header the rows with a time hold up, however narrow the file */
TEST(CsvGuessTest, ARowWithNoTimeHoldingATextKeepsItsHeader)
{
    const std::string narrow = "time (s),RPM,Gear\n0,800,N\n,810,N\n1,820,N\n";
    Guessed g(narrow);
    ASSERT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.number("header_row"), 1);
    EXPECT_TRUE(g.noted("1 of the 3 rows read has no time (line 3)"))
        << notes_of(g);
    const Ran ran(g, narrow);
    ASSERT_TRUE(ran.ok) << ran.why;
    EXPECT_EQ(ran.out.rows_no_time, 1u);
    /* A time cell of spaces is empty after the trim, here as in the import */
    const std::string spaced =
        "time (s),RPM,Gear\n0,800,N\n  ,810,N\n1,820,N\n";
    Guessed blank(spaced);
    ASSERT_TRUE(blank.ok) << blank.why;
    const Ran blank_ran(blank, spaced);
    ASSERT_TRUE(blank_ran.ok) << blank_ran.why;
    EXPECT_EQ(blank_ran.out.rows_no_time, 1u);
    /* The time named past the first column is the cell judged empty */
    const std::string named = "RPM,Gear,t\n800,N,0\n810,N,\n820,N,1\n";
    Guessed later(named, "s");
    ASSERT_TRUE(later.ok) << later.why;
    EXPECT_EQ(later.time("column"), "t");
    EXPECT_TRUE(later.noted("1 of the 3 rows read has no time (line 3)"))
        << notes_of(later);
    /* A time column's name twice is no time: the next named one is */
    Guessed once("Gear,t,t,time\nN,1,1,0\nN,x,5,\nN,2,2,1\n", "s");
    ASSERT_TRUE(once.ok) << once.why;
    EXPECT_EQ(once.time("column"), "time");
    /* A row with a time still refuses as before, and so does a text in a
     * cell beside an empty one that isn't the time */
    EXPECT_FALSE(refused("time (s),RPM,Gear\n0,800,N\n1,N,N\n").empty());
    EXPECT_FALSE(refused("RPM,Gear,t\n800,N,0\n,N,1\n").empty());
    /* Rows with no time alone hold up no header */
    EXPECT_FALSE(refused("time (s),Gear,Mode\n,N,idle\n,1,run\n").empty());
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

} // namespace
