// File/layer: tests/test_cli_transfer.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliTransferTest (test_cli)
//
// cli/transfer_lines.h: export's and import's arguments and their lines.

#include "transfer_lines.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

struct Args {
    std::vector<std::string> words;
    std::vector<char*> argv;
    explicit Args(std::vector<std::string> w) : words(std::move(w))
    {
        for (std::string& s : words) {
            argv.push_back(&s[0]);
        }
    }
    int argc() const { return static_cast<int>(argv.size()); }
};

std::string export_why(std::vector<std::string> w)
{
    Args a(std::move(w));
    cli::ExportArgs ex;
    std::string why;
    EXPECT_FALSE(cli::parse_export(a.argc(), a.argv.data(), ex, &why));
    return why;
}

std::string import_why(std::vector<std::string> w)
{
    Args a(std::move(w));
    cli::ImportArgs im;
    std::string why;
    EXPECT_FALSE(cli::parse_import(a.argc(), a.argv.data(), im, &why));
    return why;
}

TEST(CliTransferTest, ExportTakesARecordingAFileAFormatAndARange)
{
    Args a({"havne-cli", "export", "rec", "out.pcapng", "--format", "pcapng",
            "--from", "1.5", "--to", "3", "--raw"});
    cli::ExportArgs ex;
    std::string why;
    ASSERT_TRUE(cli::parse_export(a.argc(), a.argv.data(), ex, &why)) << why;
    EXPECT_STREQ(ex.options.folder_utf8, "rec");
    EXPECT_STREQ(ex.options.out_path_utf8, "out.pcapng");
    EXPECT_EQ(ex.options.format, ECUIO_EXPORT_PCAPNG);
    EXPECT_EQ(ex.options.flags, ECUIO_EXPORT_RAW);
    EXPECT_EQ(ex.options.from_us, 1500000u);
    EXPECT_EQ(ex.options.to_us, 3000000u);

    Args b({"havne-cli", "export", "--format", "folder", "rec", "copy"});
    cli::ExportArgs plain;
    ASSERT_TRUE(cli::parse_export(b.argc(), b.argv.data(), plain, &why)) << why;
    EXPECT_EQ(plain.options.format, ECUIO_EXPORT_FOLDER);
    EXPECT_EQ(plain.options.flags, 0u);
    EXPECT_EQ(plain.options.from_us, 0u);
    EXPECT_EQ(plain.options.to_us, 0u);
    EXPECT_EQ(plain.options.layout, 0u);
    EXPECT_EQ(plain.options.columns_utf8, nullptr);

    /* The CSV's shape (ADR-0056 section 5): wide by default, its rows,
     * units and columns; long with none of them */
    Args c({"havne-cli", "export", "rec", "out.csv", "--format", "csv",
            "--rows", "250", "--units", "imperial", "--signal", "RPM",
            "--signal", "Speed"});
    cli::ExportArgs wide;
    ASSERT_TRUE(cli::parse_export(c.argc(), c.argv.data(), wide, &why)) << why;
    EXPECT_EQ(wide.options.layout, ECUIO_CSV_WIDE);
    EXPECT_EQ(wide.options.row_ms, 250u);
    EXPECT_EQ(wide.options.units, ECUIO_UNITS_IMPERIAL);
    ASSERT_EQ(wide.options.column_count, 2u);
    EXPECT_STREQ(wide.options.columns_utf8[0], "RPM");
    EXPECT_STREQ(wide.options.columns_utf8[1], "Speed");
    Args d({"havne-cli", "export", "rec", "out.csv", "--format", "csv",
            "--layout", "wide", "--rows", "sweep", "--units", "metric"});
    cli::ExportArgs sweep;
    ASSERT_TRUE(cli::parse_export(d.argc(), d.argv.data(), sweep, &why)) << why;
    EXPECT_EQ(sweep.options.layout, ECUIO_CSV_WIDE);
    EXPECT_EQ(sweep.options.row_ms, 0u);
    EXPECT_EQ(sweep.options.units, ECUIO_UNITS_METRIC);
    EXPECT_EQ(sweep.options.column_count, 0u);
    Args e({"havne-cli", "export", "rec", "out.csv", "--layout", "long",
            "--format", "csv"});
    cli::ExportArgs tall;
    ASSERT_TRUE(cli::parse_export(e.argc(), e.argv.data(), tall, &why)) << why;
    EXPECT_EQ(tall.options.layout, ECUIO_CSV_LONG);
    EXPECT_EQ(tall.options.row_ms, 0u);
}

TEST(CliTransferTest, ExportsBadFormsAreUsage)
{
    EXPECT_EQ(export_why({"havne-cli", "export", "rec"}),
              "export needs a recording's folder and the file to write");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out"}),
              "export needs --format");
    EXPECT_EQ(
        export_why({"havne-cli", "export", "rec", "out", "--format", "blf"}),
        "--format takes folder, csv or pcapng");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "x", "--format",
                          "csv"}),
              "export takes one recording and one file");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--from", "-1"}),
              "--from takes seconds, 0 or more");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--to", "0"}),
              "--to takes seconds after 0");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--from", "5", "--to", "5"}),
              "--to comes after --from");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--scrub"}),
              "unexpected '--scrub'");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--layout", "tall"}),
              "--layout takes wide or long");
    for (const char* bad : {"0", "1.5", "86400001", "ms", ""}) {
        EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                              "csv", "--rows", bad}),
                  "--rows takes sweep or a number of milliseconds, 1 to "
                  "86400000")
            << bad;
    }
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--units", "si"}),
              "--units takes metric or imperial");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--signal", ""}),
              "--signal takes a signal's name");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "pcapng", "--rows", "100"}),
              "--layout, --rows, --units and --signal go with --format csv");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--layout",
                          "wide", "--format", "folder"}),
              "--layout, --rows, --units and --signal go with --format csv");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--layout", "long", "--signal", "RPM"}),
              "--rows, --units and --signal go with the wide layout");
    EXPECT_EQ(export_why({"havne-cli", "export", "rec", "out", "--format",
                          "csv", "--layout", "long", "--units", "metric"}),
              "--rows, --units and --signal go with the wide layout");
}

TEST(CliTransferTest, ImportTakesACandumpLogAFolderAndAnInterface)
{
    Args a({"havne-cli", "import", "candump", "bus.log", "rec", "--interface",
            "can1"});
    cli::ImportArgs im;
    std::string why;
    ASSERT_TRUE(cli::parse_import(a.argc(), a.argv.data(), im, &why)) << why;
    EXPECT_EQ(im.options.format, ECUIO_IMPORT_CANDUMP);
    EXPECT_STREQ(im.options.in_path_utf8, "bus.log");
    EXPECT_STREQ(im.options.out_folder_utf8, "rec");
    EXPECT_STREQ(im.options.interface_utf8, "can1");
    EXPECT_EQ(im.map_path, nullptr);
    EXPECT_EQ(import_why({"havne-cli", "import", "bus.log", "rec"}),
              "import takes candump or csv");
    EXPECT_EQ(import_why({"havne-cli", "import"}),
              "import takes candump or csv");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "bus.log"}),
              "import needs the log and the folder to make");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "a", "b", "c"}),
              "import takes one log and one folder");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "a", "b", "--map",
                          "m.json"}),
              "unexpected '--map'");
}

TEST(CliTransferTest, ImportTakesACsvAFolderAndItsMap)
{
    Args a(
        {"havne-cli", "import", "csv", "log.csv", "rec", "--map", "map.json"});
    cli::ImportArgs im;
    std::string why;
    ASSERT_TRUE(cli::parse_import(a.argc(), a.argv.data(), im, &why)) << why;
    EXPECT_EQ(im.options.format, ECUIO_IMPORT_CSV);
    EXPECT_STREQ(im.options.in_path_utf8, "log.csv");
    EXPECT_STREQ(im.options.out_folder_utf8, "rec");
    EXPECT_STREQ(im.map_path, "map.json");
    EXPECT_EQ(im.options.interface_utf8, nullptr);
    EXPECT_EQ(im.options.column_map_utf8, nullptr); // main reads the file
    EXPECT_EQ(import_why(
                  {"havne-cli", "import", "csv", "log.csv", "--map", "m.json"}),
              "import needs the file and the folder to make");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "a", "b", "c", "--map",
                          "m.json"}),
              "import takes one file and one folder");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "a", "b", "--map"}),
              "unexpected '--map'");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "a", "b", "--map",
                          "m.json", "--interface", "can0"}),
              "unexpected '--interface'");
}

std::string text(const cli::Line& l)
{
    return std::string(l.data(), l.size());
}

TEST(CliTransferTest, TheNamedColumnsTheRecordingLacksAreListedInOrder)
{
    const std::vector<std::string> recorded = {"RPM", "Speed", "Boost, abs"};
    const std::vector<const char*> named = {"Nothing", "Speed", "rpm",
                                            "Boost, abs", "RPM"};
    const std::vector<const char*> missing =
        cli::missing_names(named, recorded);
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_STREQ(missing[0], "Nothing");
    EXPECT_STREQ(missing[1], "rpm") << "names are matched whole";
    EXPECT_TRUE(cli::missing_names({}, recorded).empty());
    EXPECT_EQ(cli::missing_names({"RPM"}, {}).size(), 1u);
}

TEST(CliTransferTest, TheLinesSayWhatCoreReports)
{
    Args a({"havne-cli", "export", "rec", "out.pcapng", "--format", "pcapng"});
    cli::ExportArgs ex;
    std::string why;
    ASSERT_TRUE(cli::parse_export(a.argc(), a.argv.data(), ex, &why));
    ecuio_export_report_t r{};
    r.frames = 12;
    r.made_up = 4;
    r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 3;
    r.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE] = 1;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::exported_line(l, ex, r);
    EXPECT_EQ(text(l),
              "{\"type\":\"exported\",\"format\":\"pcapng\",\"file\":"
              "\"out.pcapng\",\"scrubbed\":true,\"frames\":12,\"made_up\":4,"
              "\"left_out\":{\"identification\":3,\"unattributable\":1,"
              "\"text_values\":0,\"activity\":0}}");

    Args c({"havne-cli", "export", "rec", "out.csv", "--format", "csv"});
    cli::ExportArgs csv;
    ASSERT_TRUE(cli::parse_export(c.argc(), c.argv.data(), csv, &why));
    ecuio_export_report_t v{};
    v.values = 40;
    v.signal_count = 3;
    v.rows = 14;
    v.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES] = 2;
    cli::Line cl(buf, sizeof buf);
    cli::exported_line(cl, csv, v);
    EXPECT_EQ(text(cl),
              "{\"type\":\"exported\",\"format\":\"csv\",\"file\":"
              "\"out.csv\",\"scrubbed\":true,\"frames\":0,\"made_up\":0,"
              "\"values\":40,\"signals\":3,\"layout\":\"wide\",\"rows\":14,"
              "\"row_ms\":0,\"missing\":0,\"left_out\":{\"identification\":0,"
              "\"unattributable\":0,\"text_values\":2,\"activity\":0}}");
    cli::Line ml(buf, sizeof buf);
    cli::exported_line(ml, csv, v, 2);
    EXPECT_NE(text(ml).find("\"row_ms\":0,\"missing\":2,"), std::string::npos)
        << text(ml);
    Args g({"havne-cli", "export", "rec", "grid.csv", "--format", "csv",
            "--rows", "100"});
    cli::ExportArgs grid;
    ASSERT_TRUE(cli::parse_export(g.argc(), g.argv.data(), grid, &why));
    cli::Line gl(buf, sizeof buf);
    cli::exported_line(gl, grid, v);
    EXPECT_NE(text(gl).find("\"layout\":\"wide\",\"rows\":14,\"row_ms\":100,"),
              std::string::npos)
        << text(gl);
    Args t({"havne-cli", "export", "rec", "long.csv", "--format", "csv",
            "--layout", "long"});
    cli::ExportArgs tall;
    ASSERT_TRUE(cli::parse_export(t.argc(), t.argv.data(), tall, &why));
    cli::Line tl(buf, sizeof buf);
    cli::exported_line(tl, tall, v);
    EXPECT_NE(text(tl).find("\"layout\":\"long\",\"rows\":14,\"row_ms\":0,"),
              std::string::npos)
        << text(tl);

    Args d({"havne-cli", "export", "rec", "copy", "--format", "folder"});
    cli::ExportArgs folder;
    ASSERT_TRUE(cli::parse_export(d.argc(), d.argv.data(), folder, &why));
    ecuio_export_report_t copied{};
    copied.frames = 9;
    copied.values = 40;
    copied.signal_count = 3;
    copied.files = 4;
    copied.files_left_out = 1;
    copied.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY] = 2;
    cli::Line fl(buf, sizeof buf);
    cli::exported_line(fl, folder, copied);
    EXPECT_EQ(text(fl),
              "{\"type\":\"exported\",\"format\":\"folder\",\"file\":"
              "\"copy\",\"scrubbed\":true,\"frames\":9,\"made_up\":0,"
              "\"values\":40,\"signals\":3,\"files\":4,\"files_left_out\":1,"
              "\"left_out\":{\"identification\":0,\"unattributable\":0,"
              "\"text_values\":0,\"activity\":2}}");

    r.failed_class = ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1;
    cli::Line f(buf, sizeof buf);
    cli::export_failed_line(f, r);
    EXPECT_EQ(text(f), "{\"type\":\"export_failed\",\"class\":\"vin_shaped\"}");

    Args b({"havne-cli", "import", "candump", "bus.log", "rec"});
    cli::ImportArgs im;
    ASSERT_TRUE(cli::parse_import(b.argc(), b.argv.data(), im, &why));
    ecuio_import_report_t ir{};
    ir.lines = 7;
    ir.frames = 6;
    ir.error_frames = 1;
    ir.interfaces = 1;
    ir.utc_start_us = 1700000000250000u;
    ir.duration_us = 105000;
    cli::Line i(buf, sizeof buf);
    cli::imported_line(i, im, ir);
    EXPECT_EQ(
        text(i),
        "{\"type\":\"imported\",\"format\":\"candump\",\"folder\":\"rec\","
        "\"interface\":null,\"lines\":7,\"frames\":6,\"error_frames\":1,"
        "\"interfaces\":1,\"utc_start_us\":1700000000250000,"
        "\"duration_us\":105000}");

    Args s({"havne-cli", "import", "csv", "log.csv", "rec", "--map", "m.json"});
    cli::ImportArgs csv_im;
    ASSERT_TRUE(cli::parse_import(s.argc(), s.argv.data(), csv_im, &why));
    ecuio_import_report_t cr{};
    cr.lines = 11;
    cr.values = 22;
    cr.columns_ignored = 1;
    cr.duration_us = 3500000;
    cli::Line c0(buf, sizeof buf);
    cli::imported_line(c0, csv_im, cr);
    EXPECT_EQ(text(c0),
              "{\"type\":\"imported\",\"format\":\"csv\",\"folder\":\"rec\","
              "\"lines\":11,\"values\":22,\"columns_ignored\":1,"
              "\"utc_start_us\":null,\"duration_us\":3500000}");
    cr.utc_start_us = 1793883802250000u;
    cli::Line c1(buf, sizeof buf);
    cli::imported_line(c1, csv_im, cr);
    EXPECT_EQ(text(c1),
              "{\"type\":\"imported\",\"format\":\"csv\",\"folder\":\"rec\","
              "\"lines\":11,\"values\":22,\"columns_ignored\":1,"
              "\"utc_start_us\":1793883802250000,\"duration_us\":3500000}");
}

// With no --map the map is guessed: --time-unit gives a plain time its
// unit, --print-map prints the guess and takes no folder
TEST(CliTransferTest, ImportGuessesACsvsMapWithoutOne)
{
    Args a(
        {"havne-cli", "import", "csv", "log.csv", "rec", "--time-unit", "ms"});
    cli::ImportArgs im;
    std::string why;
    ASSERT_TRUE(cli::parse_import(a.argc(), a.argv.data(), im, &why)) << why;
    EXPECT_EQ(im.map_path, nullptr);
    EXPECT_STREQ(im.time_unit, "ms");
    EXPECT_FALSE(im.print_map);
    EXPECT_STREQ(im.options.out_folder_utf8, "rec");
    Args b({"havne-cli", "import", "csv", "--print-map", "log.csv"});
    cli::ImportArgs printed;
    ASSERT_TRUE(cli::parse_import(b.argc(), b.argv.data(), printed, &why))
        << why;
    EXPECT_TRUE(printed.print_map);
    EXPECT_EQ(printed.time_unit, nullptr);
    EXPECT_STREQ(printed.options.in_path_utf8, "log.csv");
    EXPECT_EQ(printed.folder, nullptr);
    for (const char* unit : {"s", "ms", "us"}) {
        Args c({"havne-cli", "import", "csv", "log.csv", "rec", "--time-unit",
                unit});
        cli::ImportArgs each;
        ASSERT_TRUE(cli::parse_import(c.argc(), c.argv.data(), each, &why))
            << unit;
        EXPECT_STREQ(each.time_unit, unit);
    }
    for (const char* bad : {"S", "sec", "h", ""}) {
        EXPECT_EQ(import_why({"havne-cli", "import", "csv", "log.csv", "rec",
                              "--time-unit", bad}),
                  "--time-unit takes s, ms or us")
            << bad;
    }
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "log.csv", "rec",
                          "--print-map"}),
              "import csv --print-map takes one file and no folder");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "log.csv",
                          "--print-map", "rec"}),
              "import csv --print-map takes one file and no folder");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "log.csv", "rec",
                          "--map", "m.json", "--time-unit", "s"}),
              "--time-unit is a guessed map's: a map given with --map names "
              "its own");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "log.csv",
                          "--print-map", "--map", "m.json"}),
              "--print-map prints a guessed map: give no --map");
    EXPECT_EQ(import_why({"havne-cli", "import", "csv", "--print-map"}),
              "import needs the file and the folder to make");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "bus.log", "rec",
                          "--time-unit", "s"}),
              "unexpected '--time-unit'");
    EXPECT_EQ(import_why(
                  {"havne-cli", "import", "candump", "bus.log", "--print-map"}),
              "unexpected '--print-map'");
}

// The guess's text on one line, its keys the line's own: every line
// break and the indent after it go, a string's spaces stay
TEST(CliTransferTest, AGuessIsOneLine)
{
    /* Spaces inside a string stay, an escaped quote doesn't end one */
    const std::string guess_text =
        "{\n\"map\": {\n  \"havne_column_map\": 1,\n"
        "  \"signals\": [\n    {\"column\": \"Load  [%]\""
        ", \"name\": \"a\\nb, \\\"c d\\\": e\"}\n  ]\n},\n"
        "\"columns\": [\n  {\"column\": \"x\", "
        "\"used\": true}\n],\n\"notes\": [\"a, b: c\"]\n}\n";
    EXPECT_EQ(cli::one_line(guess_text),
              "{\"map\":{\"havne_column_map\":1,\"signals\":[{\"column\":"
              "\"Load  [%]\",\"name\":\"a\\nb, \\\"c d\\\": e\"}]},"
              "\"columns\":[{\"column\":\"x\",\"used\":true}],"
              "\"notes\":[\"a, b: c\"]}");
    std::vector<char> buf(cli::guessed_line_bytes(guess_text));
    cli::Line l(buf.data(), buf.size());
    cli::guessed_line(l, guess_text);
    ASSERT_TRUE(l.finish());
    EXPECT_EQ(text(l).rfind("{\"type\":\"guessed\",\"map\":{", 0), 0u)
        << text(l);
    EXPECT_EQ(text(l).find('\n'), text(l).size() - 1);
}

// Paths at their longest escape (every byte a control character's six)
// still fit the lines they size, so a line is never cut or dropped
TEST(CliTransferTest, LongPathsFitTheirLines)
{
    const std::string path(4000, '\x01');
    std::string escaped;
    for (size_t i = 0; i < path.size(); ++i) {
        escaped += "\\u0001";
    }
    cli::ExportArgs ex;
    ex.folder = "rec";
    ex.out = path.c_str();
    ex.format_word = "folder";
    ex.options.format = ECUIO_EXPORT_FOLDER;
    ecuio_export_report_t r{};
    std::vector<char> eb(cli::exported_line_bytes(ex));
    cli::Line e(eb.data(), eb.size());
    cli::exported_line(e, ex, r);
    ASSERT_TRUE(e.finish());
    EXPECT_NE(text(e).find("\"file\":\"" + escaped + "\""), std::string::npos);
    EXPECT_NE(text(e).find("\"activity\":0}}"), std::string::npos);

    cli::ImportArgs im;
    im.folder = path.c_str();
    im.interface_name = path.c_str();
    im.options.format = ECUIO_IMPORT_CANDUMP;
    ecuio_import_report_t ir{};
    std::vector<char> ib(cli::imported_line_bytes(im));
    cli::Line i(ib.data(), ib.size());
    cli::imported_line(i, im, ir);
    ASSERT_TRUE(i.finish());
    EXPECT_NE(text(i).find("\"folder\":\"" + escaped + "\",\"interface\":\"" +
                           escaped + "\""),
              std::string::npos);
    EXPECT_NE(text(i).find("\"duration_us\":0}"), std::string::npos);
}

} // namespace
