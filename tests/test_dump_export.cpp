// File/layer: tests/test_dump_export.cpp | C++17, Qt-free, every platform
// Testing: UNIT: DumpExportTest (test_core)
//
// A copy leaving its folder and coming back (ecuio/dump.h): the export's
// two forms, that neither replaces a file nor leaves a partial, the
// bring-back under the copy's own name, the one call that removes, and
// that no other call deletes or changes a copy.

#include "dump_file_support.h"
#include "partial_left.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/* test_core.cpp's, over tests/new_counter.h */
void fail_new_on_this_thread(unsigned long n);

namespace {

using namespace dump_file_test;

const char kDamaged[] =
    "its bytes aren't the ones its header seals: the file is damaged";
const char kVin[] = "ZZZFAKE11M0000001"; /* made up */

/* A case's folders: the copies', one to export into, one elsewhere */
struct Folders {
    Folder root;
    std::string backups;
    std::string out;
    std::string away;

    Folders()
    {
        for (std::string* s : {&backups, &out, &away}) {
            const char* name = s == &backups ? "backups"
                               : s == &out   ? "out"
                                             : "away";
            std::error_code ec;
            fs::create_directories(root.path / name, ec);
            *s = root.at(name);
        }
    }
    /* A read-only copy is removed as any file on POSIX, not on Windows */
    ~Folders()
    {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(root.path, ec)) {
            fs::permissions(e.path(), fs::perms::owner_all,
                            fs::perm_options::add, ec);
        }
    }
    Folders(const Folders&) = delete;
    Folders& operator=(const Folders&) = delete;
};

std::string put(const std::string& folder, const std::string& name,
                const std::string& bytes)
{
    const std::string path = folder + "/" + name;
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    f << bytes;
    return path;
}

std::vector<std::string> names_in(const std::string& folder)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(folder), ec)) {
        names.push_back(e.path().filename().generic_u8string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool read_only(const std::string& path)
{
    const fs::perms p = fs::status(fs::u8path(path)).permissions();
    return (p & (fs::perms::owner_write | fs::perms::group_write |
                 fs::perms::others_write)) == fs::perms::none;
}

/* A file of two ranges, 100000 and 50000 bytes: several pieces a read */
std::string big_file()
{
    ecuio_dump_file::record r = record_of();
    r.ranges[0].bytes = 100000;
    r.ranges[1].address = 0x00040000;
    r.ranges[1].bytes = 50000;
    r.ranges[1].offset = 100000;
    const Bytes image = image_of(150000);
    return ecuio_dump_file::build(r, image.data(), image.size());
}

ecuio_dump_export_result_t fresh_result()
{
    ecuio_dump_export_result_t r;
    std::memset(&r, 0xA5, sizeof r);
    r.struct_size = sizeof r;
    return r;
}

/* The image's files' names for a copy's name: its date, <tx> and first
 * 12 hex, then each start address */
std::string bin_name(const std::string& copy, const char* address)
{
    return copy.substr(0, copy.find('-', 9) + 13) + "-" + address + ".bin";
}

struct Asks {
    int asked = 0;
    int stop_at = -1; /* the ask, from 0, that answers stop */
};

int ask(void* user)
{
    Asks* a = static_cast<Asks*>(user);
    return a->asked++ == a->stop_at ? 1 : 0;
}

TEST(DumpExportTest, RestoreIsTheFileByteForByteAndVerifies)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string name = name_for(bytes);
    const std::string path = put(f.backups, name, bytes);
    {
        const Dump d = open_dump(path);
        ASSERT_NE(d, nullptr);
        ecuio_dump_export_result_t result = fresh_result();
        ASSERT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                    f.out.c_str(), nullptr, nullptr, &result),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(result.files, 1u);
        EXPECT_EQ(result.vin_runs, 0u);
        EXPECT_EQ(result.flags, 0u);
        EXPECT_EQ(result.reserved[3], 0u);
    }
    EXPECT_EQ(names_in(f.out), std::vector<std::string>{name});
    EXPECT_EQ(read_bytes(f.out + "/" + name), bytes);
    const Dump back = open_dump(f.out + "/" + name);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(ecuio_dump_verify(back.get(), nullptr, nullptr, nullptr, 0),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(read_bytes(path), bytes) << "the copy itself is untouched";

    /* A sound file under another name leaves under its own; no result
     * asked for */
    const Dump renamed = open_dump(put(f.away, "holiday.havnedump", bytes));
    ASSERT_NE(renamed, nullptr);
    EXPECT_EQ(ecuio_dump_export(renamed.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                f.away.c_str(), nullptr, nullptr, nullptr),
              ECUIO_OK);
    EXPECT_EQ(names_in(f.away),
              (std::vector<std::string>{name, "holiday.havnedump"}));
}

TEST(DumpExportTest, TheImageFormWritesOneBinForEachRange)
{
    Folders f;
    ecuio_dump_file::record r = record_of();
    Bytes image = image_of(kImageBytes);
    /* A VIN alone between bytes that are no letters or digits in the
     * first range. Ending the second, one whose check digit doesn't
     * hold: a VIN by its shape alone, which only the range's end shows */
    image[99] = 0x00;
    std::memcpy(&image[100], kVin, 17);
    image[117] = 0x00;
    image[kImageBytes - 18] = 0xFF;
    std::memcpy(&image[kImageBytes - 17], "ZZZFAKE12M0000001", 17);
    const std::string bytes =
        ecuio_dump_file::build(r, image.data(), image.size());
    const std::string name = name_for(bytes);
    const Dump d = open_dump(put(f.backups, name, bytes));
    ASSERT_NE(d, nullptr);
    ecuio_dump_export_result_t result = fresh_result();
    ASSERT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_IMAGE, f.out.c_str(),
                                nullptr, nullptr, &result),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(result.files, 2u);
    EXPECT_EQ(result.vin_runs, 2u);
    const std::string first = bin_name(name, "00010000");
    const std::string second = bin_name(name, "00020000");
    EXPECT_EQ(first, name.substr(0, 25) + "-00010000.bin");
    EXPECT_EQ(names_in(f.out), (std::vector<std::string>{first, second}));
    const std::string whole(image.begin(), image.end());
    EXPECT_EQ(read_bytes(f.out + "/" + first), whole.substr(0, 512));
    EXPECT_EQ(read_bytes(f.out + "/" + second), whole.substr(512));

    /* An image holding none: the count is 0 */
    const std::string plain = file_of(record_of());
    const Dump p = open_dump(put(f.backups, name_for(plain), plain));
    ASSERT_NE(p, nullptr);
    result = fresh_result();
    ASSERT_EQ(ecuio_dump_export(p.get(), ECUIO_DUMP_EXPORT_IMAGE,
                                f.away.c_str(), nullptr, nullptr, &result),
              ECUIO_OK);
    EXPECT_EQ(result.files, 2u);
    EXPECT_EQ(result.vin_runs, 0u);
}

/* A VIN that two pieces of a range share is one run, and the count
 * goes on after it */
TEST(DumpExportTest, AVinAcrossTwoPiecesIsCounted)
{
    Folders f;
    ecuio_dump_file::record r = record_of();
    r.ranges[0].bytes = 100000;
    r.ranges[1].address = 0x00040000;
    r.ranges[1].bytes = 50000;
    r.ranges[1].offset = 100000;
    Bytes image = image_of(150000);
    const size_t at = 65536 - 9; /* a piece is 64 KiB */
    image[at - 1] = 0x00;
    std::memcpy(&image[at], kVin, 17);
    image[at + 17] = 0x00;
    image[89999] = 0x00;
    std::memcpy(&image[90000], kVin, 17);
    image[90017] = 0x00;
    const std::string bytes =
        ecuio_dump_file::build(r, image.data(), image.size());
    const Dump d = open_dump(put(f.backups, name_for(bytes), bytes));
    ASSERT_NE(d, nullptr);
    ecuio_dump_export_result_t result = fresh_result();
    ASSERT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_IMAGE, f.out.c_str(),
                                nullptr, nullptr, &result),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(result.vin_runs, 2u);
    const std::string whole(image.begin(), image.end());
    const std::vector<std::string> names = names_in(f.out);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(read_bytes(f.out + "/" + names[0]), whole.substr(0, 100000))
        << "the count masks nothing";
    EXPECT_EQ(read_bytes(f.out + "/" + names[1]), whole.substr(100000));
}

TEST(DumpExportTest, AnExportNeverReplacesAFile)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string name = name_for(bytes);
    const Dump d = open_dump(put(f.backups, name, bytes));
    ASSERT_NE(d, nullptr);
    put(f.out, name, "mine");
    ecuio_dump_export_result_t result = fresh_result();
    EXPECT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                f.out.c_str(), nullptr, nullptr, &result),
              ECUIO_ERR_LOAD_FAILED);
    const std::string words = ecuio_last_error_detail();
    EXPECT_EQ(words.rfind("the copy can't be put in place: ", 0), 0u) << words;
    EXPECT_EQ(words.find(f.out), std::string::npos) << "no path in the words";
    EXPECT_EQ(result.files, 0u);
    EXPECT_EQ(result.struct_size, sizeof result);
    EXPECT_EQ(read_bytes(f.out + "/" + name), "mine");
    EXPECT_EQ(names_in(f.out), std::vector<std::string>{name});

    /* The second range's name taken: the first range's file isn't left */
    const std::string second = bin_name(name, "00020000");
    put(f.away, second, "mine too");
    result = fresh_result();
    EXPECT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_IMAGE,
                                f.away.c_str(), nullptr, nullptr, &result),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail())
                  .rfind("the export can't be put in place: ", 0),
              0u)
        << ecuio_last_error_detail();
    EXPECT_EQ(result.files, 0u);
    EXPECT_EQ(result.vin_runs, 0u);
    EXPECT_EQ(names_in(f.away), std::vector<std::string>{second});
    EXPECT_EQ(read_bytes(f.away + "/" + second), "mine too");
}

/* A stop at each ask in turn: nothing at any name, and no partial */
TEST(DumpExportTest, ACancelAnywhereLeavesNothing)
{
    Folders f;
    const std::string bytes = big_file();
    const Dump d = open_dump(put(f.backups, name_for(bytes), bytes));
    ASSERT_NE(d, nullptr);
    for (const uint32_t form :
         {ECUIO_DUMP_EXPORT_RESTORE, ECUIO_DUMP_EXPORT_IMAGE}) {
        Asks count;
        ASSERT_EQ(ecuio_dump_export(d.get(), form, f.away.c_str(), ask, &count,
                                    nullptr),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        /* The check reads 3 pieces; a restore's copy and its read back 3
         * each, an image's two ranges 2 and 1 */
        EXPECT_EQ(count.asked, form == ECUIO_DUMP_EXPORT_RESTORE ? 9 : 6);
        for (int stop = 0; stop < count.asked; ++stop) {
            Asks a;
            a.stop_at = stop;
            ecuio_dump_export_result_t result = fresh_result();
            ASSERT_EQ(ecuio_dump_export(d.get(), form, f.out.c_str(), ask, &a,
                                        &result),
                      ECUIO_ERR_CANCELED)
                << "form " << form << ", ask " << stop;
            EXPECT_EQ(a.asked, stop + 1) << "nothing is read after a stop";
            EXPECT_EQ(result.files, 0u);
            ASSERT_EQ(names_in(f.out), std::vector<std::string>{})
                << "form " << form << ", ask " << stop;
        }
    }
}

struct Swap {
    int asked = 0;
    int at = 0; /* the ask, from 0, at which the file is written again */
    std::string path;
    std::string bytes;
};

int swap_at(void* user)
{
    Swap* s = static_cast<Swap*>(user);
    if (s->asked++ == s->at) {
        std::ofstream f(fs::u8path(s->path),
                        std::ios::binary | std::ios::trunc);
        f << s->bytes;
    }
    return 0;
}

/* The bytes written are held to the checksum the check found: a file
 * that changes after it is refused, whatever was already put in place */
TEST(DumpExportTest, AFileChangedAfterTheCheckIsNotExported)
{
    Folders f;
    const std::string bytes = big_file();
    const std::string path = put(f.backups, name_for(bytes), bytes);
    const Dump d = open_dump(path);
    ASSERT_NE(d, nullptr);
    std::string other = bytes;
    const size_t last = 128 + 150000 - 1; /* the image's last byte */
    other[last] = static_cast<char>(other[last] ^ 0x40);
    for (const uint32_t form :
         {ECUIO_DUMP_EXPORT_RESTORE, ECUIO_DUMP_EXPORT_IMAGE}) {
        Swap s;
        s.at = 3; /* the check asked 3 times: the first piece written */
        s.path = path;
        s.bytes = other;
        EXPECT_EQ(ecuio_dump_export(d.get(), form, f.out.c_str(), swap_at, &s,
                                    nullptr),
                  ECUIO_ERR_LOAD_FAILED)
            << form;
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "the file changed since it was checked");
        EXPECT_EQ(names_in(f.out), std::vector<std::string>{}) << form;
        put(f.backups, name_for(bytes), bytes);
    }
}

TEST(DumpExportTest, ADamagedCopyIsNeitherExportedNorBroughtBack)
{
    Folders f;
    std::string bytes = file_of(record_of());
    bytes[128 + 40] = static_cast<char>(bytes[128 + 40] ^ 0x01);
    const std::string path = put(f.away, "damaged.havnedump", bytes);
    const Dump d = open_dump(path); /* the open reads no image byte */
    ASSERT_NE(d, nullptr);
    for (const uint32_t form :
         {ECUIO_DUMP_EXPORT_RESTORE, ECUIO_DUMP_EXPORT_IMAGE}) {
        EXPECT_EQ(ecuio_dump_export(d.get(), form, f.out.c_str(), nullptr,
                                    nullptr, nullptr),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), kDamaged);
    }
    EXPECT_EQ(names_in(f.out), std::vector<std::string>{});
    char name[ECUIO_DUMP_NAME_MAX];
    std::memset(name, 'x', sizeof name);
    EXPECT_EQ(
        ecuio_dump_import(path.c_str(), f.backups.c_str(), name, sizeof name),
        ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), kDamaged);
    EXPECT_STREQ(name, "") << "a damaged file is given no name";
    EXPECT_EQ(names_in(f.backups), std::vector<std::string>{});
}

TEST(DumpExportTest, TheExportChecksItsArguments)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const Dump d = open_dump(put(f.backups, name_for(bytes), bytes));
    ASSERT_NE(d, nullptr);
    ecuio_dump_export_result_t result = fresh_result();
    EXPECT_EQ(ecuio_dump_export(nullptr, ECUIO_DUMP_EXPORT_RESTORE,
                                f.out.c_str(), nullptr, nullptr, &result),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE, nullptr,
                                nullptr, nullptr, &result),
              ECUIO_ERR_NULL_ARGUMENT);
    for (const uint32_t form : {0u, 3u, 0xFFFFFFFFu}) {
        EXPECT_EQ(ecuio_dump_export(d.get(), form, f.out.c_str(), nullptr,
                                    nullptr, &result),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << form;
    }
    result.struct_size = sizeof result - 1;
    EXPECT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                f.out.c_str(), nullptr, nullptr, &result),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(result.files, 0xA5A5A5A5u) << "a refused result is untouched";
    EXPECT_EQ(names_in(f.out), std::vector<std::string>{});
    /* A folder that isn't there */
    const std::string none = f.root.at("none");
    for (const uint32_t form :
         {ECUIO_DUMP_EXPORT_RESTORE, ECUIO_DUMP_EXPORT_IMAGE}) {
        EXPECT_EQ(ecuio_dump_export(d.get(), form, none.c_str(), nullptr,
                                    nullptr, nullptr),
                  ECUIO_ERR_LOAD_FAILED);
        const std::string words = ecuio_last_error_detail();
        EXPECT_EQ(words.rfind(form == ECUIO_DUMP_EXPORT_RESTORE
                                  ? "the copy can't be written: "
                                  : "the export can't be written: ",
                              0),
                  0u)
            << words;
        EXPECT_EQ(words.find("none"), std::string::npos) << words;
    }
}

TEST(DumpExportTest, AFileBroughtBackLandsUnderItsOwnName)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string own = name_for(bytes);
    const std::string path = put(f.away, "holiday.havnedump", bytes);
    char name[ECUIO_DUMP_NAME_MAX];
    std::memset(name, 'x', sizeof name);
    ASSERT_EQ(
        ecuio_dump_import(path.c_str(), f.backups.c_str(), name, sizeof name),
        ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(own, name);
    EXPECT_EQ(names_in(f.backups), std::vector<std::string>{own});
    const std::string landed = f.backups + "/" + own;
    EXPECT_EQ(read_bytes(landed), bytes);
    EXPECT_TRUE(read_only(landed)) << "kept as a copy is";
    EXPECT_EQ(read_bytes(path), bytes) << "the file given stays";
    {
        const Dump d = open_dump(landed);
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(ecuio_dump_verify(d.get(), nullptr, nullptr, nullptr, 0),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }

    /* Again: the name is taken, and it is still told */
    std::memset(name, 'x', sizeof name);
    EXPECT_EQ(
        ecuio_dump_import(path.c_str(), f.backups.c_str(), name, sizeof name),
        ECUIO_ERR_LOAD_FAILED);
    const std::string words = ecuio_last_error_detail();
    EXPECT_EQ(words.rfind("the copy can't be put in place: ", 0), 0u) << words;
    EXPECT_EQ(words.find(f.backups), std::string::npos);
    EXPECT_EQ(own, name);
    EXPECT_EQ(names_in(f.backups), std::vector<std::string>{own});
    EXPECT_EQ(read_bytes(landed), bytes);

    /* What is no dump file, the arguments, and no name asked for */
    EXPECT_EQ(ecuio_dump_import(put(f.away, "x.havnedump", "nothing").c_str(),
                                f.out.c_str(), name, sizeof name),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "it is shorter than a dump's header");
    EXPECT_STREQ(name, "");
    EXPECT_EQ(ecuio_dump_import(nullptr, f.out.c_str(), name, sizeof name),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_import(path.c_str(), nullptr, name, sizeof name),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_import(path.c_str(), f.out.c_str(), nullptr, 96),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_import(path.c_str(), f.out.c_str(), name,
                                ECUIO_DUMP_NAME_MAX - 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(names_in(f.out), std::vector<std::string>{});
    EXPECT_EQ(ecuio_dump_import(path.c_str(), f.out.c_str(), nullptr, 0),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(names_in(f.out), std::vector<std::string>{own});
}

TEST(DumpExportTest, RemoveTakesOneFileByItsWholeName)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string copy = name_for(bytes);
    ASSERT_EQ(ecuio_dump_import(put(f.away, "a.havnedump", bytes).c_str(),
                                f.backups.c_str(), nullptr, 0),
              ECUIO_OK);
    ASSERT_TRUE(read_only(f.backups + "/" + copy));
    const std::string other_bytes = big_file();
    const std::string other = name_for(other_bytes);
    put(f.backups, other, other_bytes);
    const std::string partial = copy + ".partial-12-3";
    put(f.backups, partial, bytes.substr(0, 300));
    put(f.backups, "junk.havnedump", "junk");
    put(f.backups, "notes.txt", "mine");
    std::error_code ec;
    fs::create_directories(fs::u8path(f.backups + "/dir.havnedump"), ec);
    const std::vector<std::string> all = names_in(f.backups);
    ASSERT_EQ(all.size(), 6u);

    /* Names the list never shows, and names that reach out of the
     * folder: refused, and nothing is touched */
    for (const std::string& name :
         {std::string("notes.txt"), std::string(""), other.substr(0, 40),
          other.substr(0, other.size() - 1), "../backups/" + other,
          "..\\backups\\" + other, "sub/" + other, f.backups + "/" + other}) {
        EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), name.c_str()),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << name;
        EXPECT_STREQ(ecuio_last_error_detail(), "the name isn't a dump file's");
    }
    EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), "none.havnedump"),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), "there's no such file");
    EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), "dir.havnedump"),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), "it isn't a file");
    EXPECT_EQ(ecuio_dump_remove(nullptr, copy.c_str()),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(names_in(f.backups), all);

    /* A read-only copy, an unfinished partial and an invalid file */
    for (const std::string& name :
         {copy, partial, std::string("junk.havnedump")}) {
        EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), name.c_str()), ECUIO_OK)
            << name << ": " << ecuio_last_error_detail();
        EXPECT_FALSE(fs::exists(fs::u8path(f.backups + "/" + name))) << name;
    }
    EXPECT_EQ(names_in(f.backups),
              (std::vector<std::string>{other, "dir.havnedump", "notes.txt"}));
    EXPECT_EQ(read_bytes(f.backups + "/" + other), other_bytes);
    EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), copy.c_str()),
              ECUIO_ERR_LOAD_FAILED)
        << "removed once";
}

#ifdef _WIN32
/* Windows removes no file a handle holds: the refusal leaves the copy
 * read-only as it was */
TEST(DumpExportTest, ARemoveThatFailsLeavesTheCopyAsItWas)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string copy = name_for(bytes);
    ASSERT_EQ(ecuio_dump_import(put(f.away, "a.havnedump", bytes).c_str(),
                                f.backups.c_str(), nullptr, 0),
              ECUIO_OK);
    const std::string path = f.backups + "/" + copy;
    {
        const Dump held = open_dump(path);
        ASSERT_NE(held, nullptr);
        EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), copy.c_str()),
                  ECUIO_ERR_LOAD_FAILED);
        const std::string words = ecuio_last_error_detail();
        EXPECT_EQ(words.rfind("the file can't be removed: ", 0), 0u) << words;
        EXPECT_EQ(words.find(f.backups), std::string::npos);
        EXPECT_TRUE(read_only(path));
        EXPECT_EQ(read_bytes(path), bytes);
    }
    EXPECT_EQ(ecuio_dump_remove(f.backups.c_str(), copy.c_str()), ECUIO_OK);
}
#endif

/* Every dump call but remove, run on a kept copy: it is still there,
 * read-only, with its bytes */
TEST(DumpExportTest, NoOtherCallRemovesOrChangesACopy)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string copy = name_for(bytes);
    ASSERT_EQ(ecuio_dump_import(put(f.away, "a.havnedump", bytes).c_str(),
                                f.backups.c_str(), nullptr, 0),
              ECUIO_OK);
    const std::string path = f.backups + "/" + copy;
    {
        const Dump d = open_dump(path);
        ASSERT_NE(d, nullptr);
        ecuio_dump_info_t info{};
        info.struct_size = sizeof info;
        EXPECT_EQ(ecuio_dump_get_info(d.get(), &info), ECUIO_OK);
        ecuio_dump_range_t range{};
        range.struct_size = sizeof range;
        EXPECT_EQ(ecuio_dump_get_range(d.get(), 0, &range), ECUIO_OK);
        ecuio_dump_ident_t ident{};
        ident.struct_size = sizeof ident;
        EXPECT_EQ(ecuio_dump_get_ident(d.get(), 0, &ident), ECUIO_OK);
        ecuio_dump_calibration_t calibration{};
        calibration.struct_size = sizeof calibration;
        EXPECT_EQ(ecuio_dump_get_calibration(d.get(), 0, &calibration),
                  ECUIO_OK);
        uint8_t piece[64];
        size_t n = 0;
        EXPECT_EQ(ecuio_dump_read(d.get(), 0, piece, sizeof piece, &n),
                  ECUIO_OK);
        char name[ECUIO_DUMP_NAME_MAX];
        EXPECT_EQ(
            ecuio_dump_verify(d.get(), nullptr, nullptr, name, sizeof name),
            ECUIO_OK);
        ecuio_dump_entry_t list[2];
        EXPECT_EQ(ecuio_dump_list(f.backups.c_str(), list, 2, &n), ECUIO_OK);
        EXPECT_EQ(n, 1u);
        /* Exported both ways elsewhere, and into its own folder, where
         * its name is taken */
        for (const uint32_t form :
             {ECUIO_DUMP_EXPORT_RESTORE, ECUIO_DUMP_EXPORT_IMAGE}) {
            EXPECT_EQ(ecuio_dump_export(d.get(), form, f.out.c_str(), nullptr,
                                        nullptr, nullptr),
                      ECUIO_OK);
        }
        EXPECT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                    f.backups.c_str(), nullptr, nullptr,
                                    nullptr),
                  ECUIO_ERR_LOAD_FAILED);
        Asks stop;
        stop.stop_at = 1;
        EXPECT_EQ(ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                    f.backups.c_str(), ask, &stop, nullptr),
                  ECUIO_ERR_CANCELED);
    }
    /* Brought back over itself, from its own folder and from elsewhere */
    EXPECT_EQ(ecuio_dump_import(path.c_str(), f.backups.c_str(), nullptr, 0),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(ecuio_dump_import((f.out + "/" + copy).c_str(), f.backups.c_str(),
                                nullptr, 0),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(names_in(f.backups), std::vector<std::string>{copy});
    EXPECT_EQ(read_bytes(path), bytes);
    EXPECT_TRUE(read_only(path));
}

/* Each allocation of a clean export, bring-back and remove fails in
 * turn: OK or OUT_OF_MEMORY, nothing thrown, and a failure leaves no
 * file and no partial */
TEST(DumpExportTest, EveryCallSurvivesEveryAllocationFailing)
{
    Folders f;
    const std::string bytes = file_of(record_of());
    const std::string copy = name_for(bytes);
    const std::string path = put(f.backups, copy, bytes);
    const Dump d = open_dump(path);
    ASSERT_NE(d, nullptr);
    unsigned long refused = 0;
    for (const int call : {0, 1, 2, 3}) {
        for (unsigned long n = 1;; ++n) {
            ecuio_status_t s = ECUIO_OK;
            fail_new_on_this_thread(n);
            if (call == 0) {
                s = ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_RESTORE,
                                      f.out.c_str(), nullptr, nullptr, nullptr);
            } else if (call == 1) {
                s = ecuio_dump_export(d.get(), ECUIO_DUMP_EXPORT_IMAGE,
                                      f.out.c_str(), nullptr, nullptr, nullptr);
            } else if (call == 2) {
                s = ecuio_dump_import(path.c_str(), f.out.c_str(), nullptr, 0);
            } else {
                s = ecuio_dump_remove(f.out.c_str(), copy.c_str());
            }
            fail_new_on_this_thread(0);
            ASSERT_TRUE(s == ECUIO_OK || s == ECUIO_ERR_OUT_OF_MEMORY)
                << "call " << call << ", allocation " << n << ": " << s << " "
                << ecuio_last_error_detail();
            if (s == ECUIO_OK) {
                break;
            }
            ++refused;
            if (call == 3) {
                ASSERT_EQ(names_in(f.out), std::vector<std::string>{copy})
                    << "a remove that failed left the file";
            } else {
                ASSERT_EQ(names_in(f.out), std::vector<std::string>{})
                    << "call " << call << ", allocation " << n;
            }
            ASSERT_LT(n, 5000u) << "the loop should end";
        }
        /* What each clean call left, cleared for the next but the last
         * bring-back's, which the remove takes */
        const std::vector<std::string> left = names_in(f.out);
        ASSERT_EQ(left.size(), call == 1 ? 2u : call == 3 ? 0u : 1u) << call;
        if (call < 2) {
            for (const std::string& name : left) {
                fs::remove(fs::u8path(f.out + "/" + name));
            }
        }
    }
    EXPECT_GT(refused, 12u) << "the calls do allocate";
    EXPECT_EQ(read_bytes(path), bytes);
}

} // namespace
