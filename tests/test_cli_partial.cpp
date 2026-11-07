// File/layer: tests/test_cli_partial.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliPartialTest (test_cli)
//
// cli/partial_file.h: a partial of the call's own, created exclusively,
// that leaves another writer's partial alone, and the refusal once every
// name is taken. Each case works in a folder of its own under TEMP/TMPDIR,
// removed at the end.

#include "partial_file.h"

#include "temp_name.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Folder {
    fs::path root;
    Folder() : root(fs::temp_directory_path() / temp_name("havne-cli-partial"))
    {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string path(const char* name) const
    {
        return (root / name).generic_u8string();
    }
};

void put(const std::string& name, const std::string& text)
{
    std::ofstream(fs::u8path(name), std::ios::binary) << text;
}

std::string read(const std::string& name)
{
    std::ifstream in(fs::u8path(name), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

TEST(CliPartialTest, ItsNameIsTheProcesssAndTheTrysOwn)
{
    EXPECT_EQ(cli::partial_name("C:/out/car.json", 4242, 0),
              "C:/out/car.json.partial-4242-0");
    EXPECT_EQ(cli::partial_name("car.json", 7, 15), "car.json.partial-7-15");
}

// Another writer's partial at the first name is left as it was, and the
// call's own goes to the next
TEST(CliPartialTest, AnotherWritersPartialIsLeftAlone)
{
    Folder f;
    const std::string out = f.path("car.json");
    const unsigned long pid = cli::process_id();
    const std::string theirs = cli::partial_name(out, pid, 0);
    put(theirs, "the other writer's");
    std::string name;
    int e = 0;
    FILE* mine = cli::create_partial(out, pid, &name, &e);
    ASSERT_NE(mine, nullptr) << e;
    EXPECT_EQ(name, cli::partial_name(out, pid, 1));
    EXPECT_EQ(std::fputs("mine", mine) >= 0, true);
    EXPECT_EQ(std::fclose(mine), 0);
    EXPECT_EQ(read(theirs), "the other writer's");
    EXPECT_EQ(read(name), "mine");
}

// A non-ASCII folder: the name is UTF-8 and reaches the file system so
TEST(CliPartialTest, ANonAsciiPathMakesItsPartial)
{
    Folder f;
    const std::string out = f.path("bil-\xC3\xB8l.json"); /* o with stroke */
    std::string name;
    int e = 0;
    FILE* mine = cli::create_partial(out, 9, &name, &e);
    ASSERT_NE(mine, nullptr) << e;
    EXPECT_EQ(std::fclose(mine), 0);
    EXPECT_TRUE(fs::exists(fs::u8path(cli::partial_name(out, 9, 0))));
}

// Every name taken: refused with EEXIST, nothing made, nothing touched
TEST(CliPartialTest, EveryNameTakenIsRefused)
{
    Folder f;
    const std::string out = f.path("car.json");
    for (unsigned n = 0; n < cli::kPartialTries; ++n) {
        put(cli::partial_name(out, 77, n), std::to_string(n));
    }
    std::string name;
    int e = 0;
    EXPECT_EQ(cli::create_partial(out, 77, &name, &e), nullptr);
    EXPECT_EQ(e, EEXIST);
    for (unsigned n = 0; n < cli::kPartialTries; ++n) {
        EXPECT_EQ(read(cli::partial_name(out, 77, n)), std::to_string(n));
    }
    EXPECT_FALSE(
        fs::exists(fs::u8path(cli::partial_name(out, 77, cli::kPartialTries))));
}

// A folder that isn't there: refused at once with its own error, not
// tried kPartialTries times as if taken
TEST(CliPartialTest, AMissingFolderIsRefusedAtOnce)
{
    Folder f;
    const std::string out = f.path("missing/car.json");
    std::string name;
    int e = 0;
    EXPECT_EQ(cli::create_partial(out, 5, &name, &e), nullptr);
    EXPECT_EQ(e, ENOENT);
    EXPECT_EQ(name, cli::partial_name(out, 5, 0));
}

} // namespace
