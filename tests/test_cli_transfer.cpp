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
    EXPECT_EQ(import_why({"havne-cli", "import", "bus.log", "rec"}),
              "import takes candump");
    EXPECT_EQ(import_why({"havne-cli", "import"}), "import takes candump");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "bus.log"}),
              "import needs the log and the folder to make");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "a", "b", "c"}),
              "import takes one log and one folder");
    EXPECT_EQ(import_why({"havne-cli", "import", "candump", "a", "b", "--map",
                          "m.json"}),
              "unexpected '--map'");
}

std::string text(const cli::Line& l)
{
    return std::string(l.data(), l.size());
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
    v.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES] = 2;
    cli::Line cl(buf, sizeof buf);
    cli::exported_line(cl, csv, v);
    EXPECT_EQ(text(cl),
              "{\"type\":\"exported\",\"format\":\"csv\",\"file\":"
              "\"out.csv\",\"scrubbed\":true,\"frames\":0,\"made_up\":0,"
              "\"values\":40,\"signals\":3,\"left_out\":{\"identification\":0,"
              "\"unattributable\":0,\"text_values\":2,\"activity\":0}}");

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
}

} // namespace
