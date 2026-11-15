// File/layer: tests/test_cli_layout.cpp | gtest, havne-cli's layout check
// Testing: UNIT: CliLayoutTest (test_cli)
//
// cli/layout_check.h against the corpus in tests/data/layouts, which the
// app's tst_dashboardbook reads too: each <case>.json with the verdicts
// of <case>.expect.json, one for each form it is read as. A rule changed
// in DashboardBook or in the port fails the other side's reading of the
// same case.

#include "layout_check.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using cli::layout::File;
using cli::layout::Json;
using cli::layout::Verdict;

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string text_of(const Json* v)
{
    return v != nullptr && v->is_string() ? cli::layout::utf8_of(v->str) : "";
}

std::vector<std::string> case_names()
{
    std::vector<std::string> names;
    const std::string suffix = ".expect.json";
    for (const auto& e :
         std::filesystem::directory_iterator(ECUIO_LAYOUT_CORPUS)) {
        const std::string f = e.path().filename().string();
        if (f.size() > suffix.size() &&
            f.compare(f.size() - suffix.size(), suffix.size(), suffix) == 0) {
            names.push_back(f.substr(0, f.size() - suffix.size()));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string read_case(const std::string& name)
{
    std::string bytes =
        read_file(std::filesystem::path(ECUIO_LAYOUT_CORPUS) / (name + ".json"));
    cli::layout::strip_bom(&bytes);
    return bytes;
}

Verdict checked(const std::string& text, const char* as)
{
    return cli::layout::check(text, as);
}

} // namespace

TEST(CliLayoutTest, TheCorpusIsThere)
{
    EXPECT_GE(case_names().size(), 80u);
}

// The corpus is the contract with DashboardBook: every verdict it holds
TEST(CliLayoutTest, EveryCaseReadsToItsVerdicts)
{
    for (const std::string& name : case_names()) {
        const std::string text = read_case(name);
        ASSERT_TRUE(cli::layout::valid_utf8(text)) << name;
        Json runs;
        const std::string expected = read_file(
            std::filesystem::path(ECUIO_LAYOUT_CORPUS) / (name + ".expect.json"));
        ASSERT_TRUE(cli::layout::Parser(expected).parse(&runs)) << name;
        ASSERT_TRUE(runs.is_array() && !runs.arr.empty()) << name;
        for (const Json& run : runs.arr) {
            const std::string as = text_of(run.get("as"));
            const std::string where = name + " as " + as;
            const Verdict v = checked(text, as.c_str());
            EXPECT_EQ(v.problem, text_of(run.get("problem"))) << where;
            EXPECT_EQ(v.form, text_of(run.get("form"))) << where;
            if (!v.read) {
                continue;
            }
            EXPECT_EQ(v.pages, static_cast<int>(run.get("pages")->num)) << where;
            EXPECT_EQ(v.cells, static_cast<int>(run.get("cells")->num)) << where;
            const Json* dropped = run.get("dropped");
            EXPECT_EQ(v.dropped_pages,
                      dropped != nullptr
                          ? static_cast<int>(dropped->get("pages")->num)
                          : 0)
                << where;
            EXPECT_EQ(v.dropped_cells,
                      dropped != nullptr
                          ? static_cast<int>(dropped->get("cells")->num)
                          : 0)
                << where;
            const Json* files = run.get("files");
            ASSERT_EQ(v.files.size(), files->arr.size()) << where;
            for (size_t i = 0; i < v.files.size(); ++i) {
                EXPECT_EQ(v.files[i].name, text_of(files->arr[i].get("name")))
                    << where;
                EXPECT_EQ(v.files[i].sha256, text_of(files->arr[i].get("sha256")))
                    << where;
            }
            const Json* notes = run.get("notes");
            ASSERT_EQ(v.notes.size(), notes->arr.size()) << where;
            for (size_t i = 0; i < v.notes.size(); ++i) {
                EXPECT_EQ(v.notes[i], text_of(&notes->arr[i])) << where;
            }
            const Json* first = run.get("first_cell");
            if (first != nullptr) {
                EXPECT_TRUE(v.has_first) << where;
                EXPECT_EQ(v.first_w, static_cast<int>(first->get("w")->num)) << where;
                EXPECT_EQ(v.first_h, static_cast<int>(first->get("h")->num)) << where;
            }
        }
    }
}

// What the app's reader has no say in: a file read with no form named
TEST(CliLayoutTest, NoFormAsksTheFileOrAssumesDesktopAndSaysSo)
{
    const std::string none = read_case("no-form");
    Verdict v = checked(none, "");
    EXPECT_EQ(v.as, "desktop");
    ASSERT_EQ(v.notes.size(), 2u);
    EXPECT_EQ(v.notes[0], "no-form");
    EXPECT_EQ(v.notes[1], "assumed-desktop");

    v = checked(read_case("phone-ok"), "");
    EXPECT_EQ(v.as, "phone");
    EXPECT_EQ(v.problem, "");
    EXPECT_TRUE(v.notes.empty());

    // Only a format 1 file's upgrade depends on the form read as
    const std::string f1 = read_case("format1-no-form");
    v = checked(f1, "");
    EXPECT_EQ(v.as, "desktop");
    EXPECT_EQ(v.first_w, 2);
    v = checked(f1, "phone");
    EXPECT_EQ(v.first_w, 6);
    EXPECT_EQ(v.first_h, 12);
}

TEST(CliLayoutTest, AnEmptyFileIsNothingToRead)
{
    EXPECT_EQ(checked("", "desktop").problem, "none");
}

// With no form named, a file refused before its form is read is read as
// desktop, as a readable file naming no form is
TEST(CliLayoutTest, ARefusalBeforeTheFormIsReadIsAsDesktop)
{
    EXPECT_EQ(checked("", "").as, "desktop");
    EXPECT_EQ(checked("[", "").as, "desktop");
    EXPECT_EQ(checked(std::string(cli::layout::kFileMaxBytes + 1, ' '), "").as,
              "desktop");
    EXPECT_EQ(checked("[", "phone").as, "phone");
}

// The app's bound on what it reads: past it the file is refused at once
TEST(CliLayoutTest, ATextPastTheFilesBoundIsDamaged)
{
    const std::string over(cli::layout::kFileMaxBytes + 1, ' ');
    EXPECT_EQ(checked(over, "desktop").problem, "damaged");
    const std::string at(cli::layout::kFileMaxBytes, ' ');
    EXPECT_EQ(checked(at, "desktop").problem, "damaged"); // blank: not JSON
}

TEST(CliLayoutTest, Utf8IsReadStrictlyAndABomIsSkipped)
{
    EXPECT_TRUE(cli::layout::valid_utf8("caf\xC3\xA9 \xF0\x9F\x98\x80"));
    EXPECT_FALSE(cli::layout::valid_utf8("\xE9"));              // Latin-1
    EXPECT_FALSE(cli::layout::valid_utf8("\xC0\xAF"));          // overlong
    EXPECT_FALSE(cli::layout::valid_utf8("\xED\xA0\x80"));      // a surrogate
    EXPECT_FALSE(cli::layout::valid_utf8("\xF4\x90\x80\x80"));  // past U+10FFFF
    EXPECT_FALSE(cli::layout::valid_utf8("\xE2\x82"));          // cut short
    std::string with_bom = "\xEF\xBB\xBF{}";
    cli::layout::strip_bom(&with_bom);
    EXPECT_EQ(with_bom, "{}");
    std::string twice = "\xEF\xBB\xBF\xEF\xBB\xBF{}";
    cli::layout::strip_bom(&twice);
    EXPECT_EQ(twice, "\xEF\xBB\xBF{}"); // one BOM, never two
}

// JSON.stringify's numbers: the size of a stored text follows them
TEST(CliLayoutTest, NumbersAreWrittenAsJavaScriptWritesThem)
{
    using cli::layout::js_number;
    EXPECT_EQ(js_number(0), "0");
    EXPECT_EQ(js_number(-0.0), "0");
    EXPECT_EQ(js_number(2), "2");
    EXPECT_EQ(js_number(-12), "-12");
    EXPECT_EQ(js_number(0.5), "0.5");
    EXPECT_EQ(js_number(161.5565), "161.5565");
    EXPECT_EQ(js_number(100000), "100000");
    EXPECT_EQ(js_number(1e21), "1e+21");
    EXPECT_EQ(js_number(123456789012345680000.0), "123456789012345680000");
    EXPECT_EQ(js_number(1e-6), "0.000001");
    EXPECT_EQ(js_number(1e-7), "1e-7");
    EXPECT_EQ(js_number(1.5e-7), "1.5e-7");
    EXPECT_EQ(js_number(0.1 + 0.2), "0.30000000000000004");
}

// A lone surrogate is a character to count and, as DashboardBook's
// utf8Bytes counts it, three bytes
TEST(CliLayoutTest, StringsAreCountedAndStoredAsTheAppDoes)
{
    using cli::layout::code_points;
    EXPECT_EQ(code_points(u"\xD83D\xDE00"), 1u);
    EXPECT_EQ(code_points(u"\xD83D"), 1u);
    EXPECT_EQ(code_points(u"\xDE00\xD83D"), 2u);
    std::string out;
    cli::layout::write_string(&out, u"a\xD83D" u"b\"\\\n\x01");
    EXPECT_EQ(out, "\"a\xEF\xBF\xBD"
                   "b\\\"\\\\\\n\\u0001\"");
    out.clear();
    cli::layout::write_string(&out, u"\xD83D\xDE00");
    EXPECT_EQ(out, "\"\xF0\x9F\x98\x80\"");
}

TEST(CliLayoutTest, TheLineSaysWhatTheFileIs)
{
    const Verdict v = checked(read_case("desktop-ok-with-files"), "desktop");
    std::vector<char> buf(cli::layout::layout_line_bytes(v));
    cli::Line l(buf.data(), buf.size());
    cli::layout::layout_line(l, v);
    ASSERT_TRUE(l.finish());
    const std::string line(l.data(), l.size());
    EXPECT_NE(line.find("\"ok\":true"), std::string::npos) << line;
    EXPECT_NE(line.find("\"problem\":\"\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"needs_files\":[{\"name\":\"Boost.json\""),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("\"words\":null"), std::string::npos) << line;
}

// A file's name is written whole, a NUL among its bytes too
TEST(CliLayoutTest, AFilesNameIsWrittenWithItsLength)
{
    Verdict v = checked(read_case("desktop-ok-with-files"), "desktop");
    ASSERT_FALSE(v.files.empty());
    v.files[0].name = std::string("a\0b.json", 8);
    std::vector<char> buf(cli::layout::layout_line_bytes(v));
    cli::Line l(buf.data(), buf.size());
    cli::layout::layout_line(l, v);
    ASSERT_TRUE(l.finish());
    const std::string line(l.data(), l.size());
    EXPECT_NE(line.find("{\"name\":\"a\\u0000b.json\""), std::string::npos)
        << line;

    // The line's room holds every name at its longest, each byte a NUL's
    // six-byte escape
    v.files.resize(cli::layout::kMaxFiles, v.files[0]);
    for (File& f : v.files) {
        f.name = std::string(cli::layout::kMaxFileName, '\0');
    }
    std::vector<char> tight(cli::layout::layout_line_bytes(v));
    cli::Line t(tight.data(), tight.size());
    cli::layout::layout_line(t, v);
    EXPECT_TRUE(t.finish());
}
