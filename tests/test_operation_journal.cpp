#include "dump_file_support.h"
#include "operation_journal.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

using dump_file_test::Folder;
using ecuio_operation::journal_find;
using ecuio_operation::journal_found;
using ecuio_operation::journal_head;
using ecuio_operation::journal_mark;
using ecuio_operation::journal_state;
using ecuio_operation::journal_writer;

journal_head head_of()
{
    journal_head h;
    h.operation = "flash-code";
    h.fingerprint = std::string(64, 'a');
    h.rx = 0x7E8;
    h.copy = "copy.havnedump";
    h.images = {std::string(64, 'b')};
    h.sectors = {{0x00080000, 4096}, {0x00081000, 4096}};
    h.utc = 1795000000;
    return h;
}

std::string read_all(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool flushed(const journal_writer& j)
{
    for (int i = 0; i < 500 && j.flushed() != j.posted() && !j.failed(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return j.flushed() == j.posted();
}

} // namespace

TEST(OperationJournalTest, TheTextNamesWhatTheFlashWrites)
{
    journal_mark m;
    m.state = journal_state::writing;
    m.written = 1;
    EXPECT_EQ(ecuio_operation::journal_text(head_of(), m),
              "{\n"
              "  \"havne_journal\": 1,\n"
              "  \"state\": \"writing\",\n"
              "  \"operation\": \"flash-code\",\n"
              "  \"plugin\": \"" +
                  std::string(64, 'a') +
                  "\",\n"
                  "  \"rx\": \"7E8\",\n"
                  "  \"copy\": \"copy.havnedump\",\n"
                  "  \"utc\": 1795000000,\n"
                  "  \"images\": [\"" +
                  std::string(64, 'b') +
                  "\"],\n"
                  "  \"sectors\": [\n"
                  "    {\"address\": \"00080000\", \"bytes\": 4096, "
                  "\"written\": true},\n"
                  "    {\"address\": \"00081000\", \"bytes\": 4096, "
                  "\"written\": false}\n"
                  "  ]\n"
                  "}\n");
    journal_head none = head_of();
    none.copy.clear();
    none.sectors.clear();
    const std::string t = ecuio_operation::journal_text(none, journal_mark{});
    EXPECT_NE(t.find("\"copy\": null"), std::string::npos);
    EXPECT_NE(t.find("\"state\": \"ready\""), std::string::npos);
    EXPECT_NE(t.find("\"sectors\": []\n}"), std::string::npos);
}

TEST(OperationJournalTest, ItIsWrittenAheadAndReadsBackUnfinished)
{
    Folder f("-journal");
    const std::string folder = f.path.u8string();
    std::string why;
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::none);
    journal_writer j;
    ASSERT_TRUE(j.open(folder, head_of(), &why)) << why;
    const std::string path = ecuio_operation::journal_path(folder, 0x7E8);
    EXPECT_NE(read_all(path).find("\"state\": \"ready\""), std::string::npos)
        << "on the disk before the start returns";
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::none)
        << "a journal still ready began no write";

    journal_mark m;
    m.state = journal_state::writing;
    j.post(m);
    ASSERT_TRUE(flushed(j));
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::unfinished);
    EXPECT_EQ(why, "the flash journal 000007E8.havnejournal says a flash of "
                   "this module didn't finish: the module may be mid-flash");
    EXPECT_EQ(journal_find(folder, 0x7E9, &why), journal_found::none)
        << "a journal is its module's";

    m.written = 2;
    j.post(m);
    ASSERT_TRUE(flushed(j));
    const std::string text = read_all(path);
    EXPECT_EQ(text.find("\"written\": false"), std::string::npos);
    j.close(false);
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::unfinished)
        << "a close past the floor keeps it";

    journal_writer again;
    ASSERT_TRUE(again.open(folder, head_of(), &why));
    again.close(true);
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::none);
}

TEST(OperationJournalTest, AFailedWriteIsTold)
{
    Folder f("-journal-fail");
    const std::string folder = f.path.u8string();
    std::string why;
    ecuio_operation::journal_test_fail_at = 1;
    journal_writer refused;
    EXPECT_FALSE(refused.open(folder, head_of(), &why));
    EXPECT_EQ(why.rfind("the flash journal can't be written: ", 0), 0u) << why;
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::none);

    journal_writer j;
    ASSERT_TRUE(j.open(folder, head_of(), &why)) << why;
    ecuio_operation::journal_test_fail_at = 2;
    journal_mark m;
    m.state = journal_state::writing;
    j.post(m);
    EXPECT_FALSE(flushed(j));
    EXPECT_TRUE(j.failed());
    EXPECT_EQ(std::string(j.words()).rfind("the flash journal can't be "
                                           "written: ",
                                           0),
              0u);
    j.close(true);

    journal_writer none;
    EXPECT_FALSE(none.open(folder + "/no-such-folder", head_of(), &why));
}

TEST(OperationJournalTest, AJournalNoHavneReadsIsTakenAsMidFlash)
{
    Folder f("-journal-odd");
    const std::string folder = f.path.u8string();
    const std::string path = ecuio_operation::journal_path(folder, 0x7E8);
    for (const char* text :
         {"not json", "{\"havne_journal\": 2, \"state\": \"ready\"}",
          "{\"state\": \"ready\"}", "[1]",
          "{\"havne_journal\": 1, \"state\": 3}"}) {
        std::ofstream(path, std::ios::binary) << text;
        std::string why;
        EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::failed)
            << text;
        EXPECT_EQ(why, "the flash journal 000007E8.havnejournal isn't one "
                       "this Havne reads: the module may be mid-flash");
    }
    std::ofstream(path, std::ios::binary)
        << "{\"havne_journal\": 1, \"state\": \"done\"}";
    std::string why;
    EXPECT_EQ(journal_find(folder, 0x7E8, &why), journal_found::none);
}
