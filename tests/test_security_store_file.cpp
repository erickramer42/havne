// File/layer: tests/test_security_store_file.cpp | test_core, Qt-free
// Testing: UNIT: SecurityStoreTest (security_store.{h,cpp}: the file read,
//          written whole and replaced, a file that can't be opened, a
//          second writer's entries kept, in a folder of the case's own)

#include "security_store.h"

#include "folder_files.h"
#include "open_handles.h"
#include "partial_left.h"
#include "security_store_support.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

using namespace security_store_test;

#ifdef _WIN32
namespace {

/* What replace_with's seam does before each further try: counts it and
 * lets a held file go */
HANDLE g_held = INVALID_HANDLE_VALUE;
int g_retries = 0;

void count_and_let_go()
{
    ++g_retries;
    if (g_held != INVALID_HANDLE_VALUE) {
        CloseHandle(g_held);
        g_held = INVALID_HANDLE_VALUE;
    }
}

/* A file of the folder held open as another program would, sharing reads
 * and writes and no delete: MoveFileExW can't replace it or move it */
HANDLE hold(const Folder& f, const char* name = kName)
{
    return CreateFileW((f.path / name).wstring().c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace
#endif

TEST(SecurityStoreTest, AStoreIsWrittenAndReadBack)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 2, kUtc));
    s.entries.push_back(entry(kRxLong, 0x7D, kStoreMaxFailed, kStoreMaxUtc));
    s.entries.push_back(entry(1, 0x03, 0, kUtc)); /* a 37 on a seed */
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_EQ(f.entries(), 1u) << "the store alone, no partial";
    const std::string text = f.read();
    EXPECT_EQ(text, store_text(s));
    EXPECT_EQ(s.on_disk, text) << "what the store last wrote";
    EXPECT_NE(text.find("\"havne_security_store\": 1"), std::string::npos);
    EXPECT_NE(text.find("\"vehicle\": \"0123456789ABCDEF\""), std::string::npos)
        << text;
    EXPECT_NE(text.find("\"rx\": \"7E8\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"rx\": \"18DAF110\""), std::string::npos) << text;
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    expect_same(back, s);
    EXPECT_EQ(back.on_disk, text) << "what the store last read";
}

TEST(SecurityStoreTest, NoFileIsAnEmptyStore)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 2, kUtc));
    s.on_disk = "another file's";
    std::string why;
    EXPECT_EQ(store_load(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_TRUE(s.entries.empty()) << "emptied first";
    EXPECT_TRUE(s.on_disk.empty());
    EXPECT_TRUE(why.empty());
    EXPECT_EQ(f.entries(), 0u) << "a load makes no file";
}

/* A save replaces the store it finds, whatever that held */
TEST(SecurityStoreTest, ASaveReplacesTheStoreThere)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    s.entries[0].failed = 2;
    s.entries.push_back(entry(kRx, 0x03, 5, kUtc + 9));
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_EQ(f.entries(), 1u);
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    expect_same(back, s);
}

/* An entry with nothing to keep (a right key's) is left out of the file
 * and gives its place to the next one made */
TEST(SecurityStoreTest, AnEntryWithNothingToKeepIsLeftOut)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 3, kUtc));
    s.entries.push_back(entry(kRx, 0x03, 1, kUtc));
    store_unlocked(s.entries[0]);
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_EQ(f.read().find("\"failed\": 0"), std::string::npos)
        << "not in the file";
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    ASSERT_EQ(back.entries.size(), 1u);
    EXPECT_EQ(back.entries[0].level, 0x03);
    store_entry* e = store_entry_for(s, kCar, kRxLong, 0x05);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e, &s.entries[0]) << "the emptied place";
    EXPECT_EQ(e->rx, kRxLong);
    EXPECT_EQ(e->level, 0x05);
    EXPECT_EQ(s.entries.size(), 2u);
    const std::string none = with_entries(
        "{\"rx\": \"7E8\", \"level\": 3, \"failed\": 0, \"last_utc\": 0, "
        "\"pending_utc\": 0, \"vehicle\": \"0123456789ABCDEF\"}, " +
        std::string(kEntry));
    store read;
    ASSERT_EQ(store_read(none.data(), none.size(), &read, &why), store_io::ok)
        << "one in a file reads: " << why;
    ASSERT_EQ(read.entries.size(), 1u) << "and is left out";
    EXPECT_EQ(read.entries[0].failed, 2u);
}

/* The one owner's right key reaches the file: a save over the file its
 * store last wrote, or last read, merges nothing back from it */
TEST(SecurityStoreTest, ARightKeysZeroReachesTheFile)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 3, kUtc));
    s.entries.push_back(entry(kRx, 0x03, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    store_unlocked(s.entries[0]);
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    ASSERT_EQ(back.entries.size(), 1u) << "the file's older count is gone";
    EXPECT_EQ(back.entries[0].level, 0x03);
    store_unlocked(back.entries[0]);
    ASSERT_EQ(store_save(f.file(), &back, &why), store_io::ok) << why;
    store none;
    ASSERT_EQ(store_load(f.file(), &none, &why), store_io::ok) << why;
    EXPECT_TRUE(none.entries.empty()) << "after a load as after a save";
}

/* A full store makes no entry: the attempt it would count doesn't start */
TEST(SecurityStoreTest, AFullStoreMakesNoEntry)
{
    Folder f;
    store s;
    for (uint32_t k = 0; k < kStoreMaxEntries; ++k) {
        store_entry* e = store_entry_for(s, kCar, kRxLong - k, 0x7D);
        ASSERT_NE(e, nullptr) << k;
        e->failed = kStoreMaxFailed;
        e->last_utc = kStoreMaxUtc;
        e->pending_utc = kStoreMaxUtc;
    }
    EXPECT_EQ(store_entry_for(s, kCar, kRx, 0x01), nullptr);
    EXPECT_EQ(s.entries.size(), kStoreMaxEntries);
    EXPECT_NE(store_entry_for(s, kCar, kRxLong, 0x7D), nullptr)
        << "one it holds";
    /* The longest entries, as many as a store holds, fit the file */
    const std::string text = store_text(s);
    EXPECT_LE(text.size(), kStoreMaxBytes);
    EXPECT_GT(text.size(), kStoreMaxBytes / 2) << "and the bound isn't idle";
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    EXPECT_EQ(back.entries.size(), kStoreMaxEntries);
    /* One more put in past store_entry_for isn't written: it wouldn't read
     * back. One with nothing to keep doesn't count. */
    const std::string before = f.read();
    s.entries.push_back(entry(kRx, 0x03, 0, 0));
    EXPECT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    s.entries.back().failed = 1;
    EXPECT_EQ(store_save(f.file(), &s, &why), store_io::refused);
    EXPECT_EQ(why, "the security store is full");
    EXPECT_EQ(f.read(), before);
    EXPECT_EQ(f.entries(), 1u);
}

/* An attempt marked and never ended (a crash, a pulled cable) is a failed
 * one: a load counts it once, dated by its mark, and it stays once across
 * loads whether or not a write comes between */
TEST(SecurityStoreTest, APendingAttemptIsCountedOnceAtLoad)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 2, kUtc));
    s.entries.push_back(entry(kRx, 0x03, 0, 0));
    store_attempt_began(s.entries[0], kUtc + 500);
    store_attempt_began(s.entries[1], kUtc + 600);
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_NE(f.read().find("\"pending_utc\": 1795133300"), std::string::npos)
        << "the mark is in the file";
    for (int pass = 0; pass < 2; ++pass) {
        store back;
        ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
        ASSERT_EQ(back.entries.size(), 2u) << pass;
        EXPECT_EQ(back.entries[0].failed, 3u) << pass << ": once";
        EXPECT_EQ(back.entries[0].last_utc, kUtc + 500) << "dated by the mark";
        EXPECT_EQ(back.entries[0].pending_utc, 0) << pass;
        EXPECT_EQ(back.entries[1].failed, 1u) << pass;
        EXPECT_EQ(back.entries[1].last_utc, kUtc + 600) << pass;
    }
    /* With a write between, the count is the file's and the mark is gone:
     * the file is what the store last read, so its write is its own */
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    ASSERT_EQ(store_save(f.file(), &back, &why), store_io::ok) << why;
    EXPECT_EQ(f.read().find("\"pending_utc\": 1795133300"), std::string::npos);
    store again;
    ASSERT_EQ(store_load(f.file(), &again, &why), store_io::ok) << why;
    expect_same(again, back);
    EXPECT_EQ(again.entries[0].failed, 3u) << "still once";
    /* A count at the file's bound stays there */
    EXPECT_EQ(refusal(with_entries(
                  "{\"rx\": \"7E8\", \"level\": 1, \"failed\": 2147483647, "
                  "\"last_utc\": 5, \"pending_utc\": 9, \"vehicle\": "
                  "\"0123456789ABCDEF\"}")),
              "");
    store top;
    const std::string text =
        with_entries("{\"rx\": \"7E8\", \"level\": 1, \"failed\": 2147483647, "
                     "\"last_utc\": 5, \"pending_utc\": 9, \"vehicle\": "
                     "\"0123456789ABCDEF\"}");
    ASSERT_EQ(store_read(text.data(), text.size(), &top, &why), store_io::ok);
    ASSERT_EQ(top.entries.size(), 1u);
    EXPECT_EQ(top.entries[0].failed, kStoreMaxFailed);
    EXPECT_EQ(top.entries[0].last_utc, 9);
}

/* A file that is there and doesn't read is refused through load too, in
 * words that say what the file is and what removing it costs, and that
 * name no path */
TEST(SecurityStoreTest, AnUnreadableFileIsRefusedNeverTakenAsEmpty)
{
    Folder f;
    const std::string texts[] = {
        "",
        "not json",
        "{\"havne_security_store\": 1}",
        std::string(kStoreMaxBytes + 1, ' '),
    };
    for (const std::string& text : texts) {
        f.write(text);
        store s;
        s.entries.push_back(entry(kRx, 0x01, 2, kUtc));
        std::string why;
        EXPECT_EQ(store_load(f.file(), &s, &why), store_io::refused)
            << text.substr(0, 40);
        EXPECT_TRUE(s.entries.empty());
        EXPECT_EQ(why.rfind("the security store doesn't read (", 0), 0u) << why;
        EXPECT_NE(why.find("removing the file forgets them"), std::string::npos)
            << why;
        EXPECT_EQ(why.find(f.path.filename().generic_u8string()),
                  std::string::npos)
            << "no path: " << why;
        EXPECT_EQ(why.find(kName), std::string::npos) << why;
        EXPECT_EQ(f.read(), text) << "a refused file is left as it is";
    }
    f.write(std::string(kStoreMaxBytes + 1, ' '));
    store s;
    std::string why;
    ASSERT_EQ(store_load(f.file(), &s, &why), store_io::refused);
    EXPECT_NE(why.find("it is longer than 65536 bytes"), std::string::npos)
        << why;
    f.write("{\"havne_security_store\": 1}");
    ASSERT_EQ(store_load(f.file(), &s, &why), store_io::refused);
    EXPECT_NE(why.find("(line 1, column 1: a security store needs "
                       "\"entries\")"),
              std::string::npos)
        << why;
}

/* A write that fails at any step leaves the store as it was, byte for
 * byte, and nothing of its own beside it */
TEST(SecurityStoreTest, AFailedWriteLeavesTheOldStoreWhole)
{
    Folder f;
    store old;
    old.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &old, &why), store_io::ok) << why;
    const std::string before = f.read();
    store next = old;
    next.entries[0].failed = 2;
    for (const int step : {1, 2, 3}) {
        store_test_fail_at.store(step);
        why.clear();
        EXPECT_EQ(store_save(f.file(), &next, &why), store_io::refused) << step;
        EXPECT_EQ(store_test_fail_at.load(), 0) << "the seam is used once";
        EXPECT_EQ(why.rfind("the security store can't be written: ", 0), 0u)
            << step << ": " << why;
        EXPECT_EQ(why.find(kName), std::string::npos) << "no path: " << why;
        EXPECT_EQ(f.read(), before) << step;
        EXPECT_FALSE(partial_left(f.path, kName)) << step;
        EXPECT_EQ(f.entries(), 1u) << step;
    }
    /* With no store there, a failed write leaves none */
    std::error_code ec;
    fs::remove(f.path / kName, ec);
    for (const int step : {1, 2, 3}) {
        store_test_fail_at.store(step);
        EXPECT_EQ(store_save(f.file(), &next, &why), store_io::refused) << step;
        EXPECT_EQ(f.entries(), 0u) << step;
    }
    ASSERT_EQ(store_save(f.file(), &next, &why), store_io::ok)
        << "and the next write goes through: " << why;
    EXPECT_EQ(f.read(), store_text(next));
}

/* A folder that isn't there: no partial can be made, in words */
TEST(SecurityStoreTest, AStoreInAFolderThatIsNotThereIsNotWritten)
{
    Folder f;
    const std::string path = (f.path / "none" / kName).generic_u8string();
    store s;
    s.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    EXPECT_EQ(store_save(path, &s, &why), store_io::refused);
    EXPECT_EQ(why, "the security store can't be written: the folder it goes "
                   "in doesn't exist");
    EXPECT_EQ(store_load(path, &s, &why), store_io::ok)
        << "and reads as no store";
    EXPECT_TRUE(s.entries.empty());
}

/* A store another program holds open: the replace is tried its few
 * times and refused, and the old file stands whole. Elsewhere no file in
 * a folder one may write to refuses a rename, so the case is a folder
 * where the store's name is: it can't be read, so nothing is written over
 * it, and it doesn't load as an empty store */
TEST(SecurityStoreTest, AFailedReplaceLeavesTheOldFileWhole)
{
    Folder f;
    store old;
    old.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &old, &why), store_io::ok) << why;
    const std::string before = f.read();
    store next = old;
    next.entries[0].failed = 2;
#ifdef _WIN32
    HANDLE held = hold(f);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    g_retries = 0;
    ecuio_files::replace_test_before_retry.store(count_and_let_go);
    const store_io io = store_save(f.file(), &next, &why);
    ecuio_files::replace_test_before_retry.store(nullptr);
    CloseHandle(held);
    EXPECT_EQ(io, store_io::refused);
    EXPECT_EQ(g_retries, ecuio_files::kReplaceTries - 1) << "each try made";
    EXPECT_EQ(why.rfind("the security store can't be written: ", 0), 0u) << why;
    EXPECT_EQ(f.read(), before);
    EXPECT_FALSE(partial_left(f.path, kName));
    EXPECT_EQ(next.on_disk, before) << "the store as it was";
#else
    std::error_code ec;
    fs::create_directories(f.path / "taken" / "inside", ec);
    const std::string taken = (f.path / "taken").generic_u8string();
    EXPECT_EQ(store_save(taken, &next, &why), store_io::refused);
    EXPECT_EQ(why, "the security store can't be written: the file there can't "
                   "be read");
    store s;
    EXPECT_EQ(store_load(taken, &s, &why), store_io::refused);
    EXPECT_EQ(why, "the security store can't be read");
    EXPECT_TRUE(fs::exists(f.path / "taken" / "inside")) << "left as it was";
    EXPECT_FALSE(partial_left(f.path, "taken"));
    EXPECT_EQ(f.read(), before) << "and the store beside it";
    EXPECT_EQ(f.entries(), 2u) << "the store and the folder, no partial";
#endif
    ASSERT_EQ(store_save(f.file(), &next, &why), store_io::ok) << why;
    EXPECT_EQ(f.read(), store_text(next));
}

#ifdef _WIN32
/* Another program with the store open for a moment (a scanner, a backup)
 * doesn't fail the save: the move is tried again */
TEST(SecurityStoreTest, AStoreInUseForAMomentIsStillReplaced)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    s.entries[0].failed = 2;
    g_held = hold(f);
    ASSERT_NE(g_held, INVALID_HANDLE_VALUE);
    g_retries = 0;
    ecuio_files::replace_test_before_retry.store(count_and_let_go);
    const store_io io = store_save(f.file(), &s, &why);
    ecuio_files::replace_test_before_retry.store(nullptr);
    if (g_held != INVALID_HANDLE_VALUE) {
        CloseHandle(g_held);
        g_held = INVALID_HANDLE_VALUE;
    }
    EXPECT_EQ(io, store_io::ok) << why;
    EXPECT_EQ(g_retries, 1) << "the second try went through";
    EXPECT_EQ(f.read(), store_text(s));
    EXPECT_FALSE(partial_left(f.path, kName));
}

/* A held store is Windows' access denied. The file to move held open is
 * its other refusal, a sharing violation, and is tried again as well; a
 * failure of another kind (no file to move) is tried once */
TEST(SecurityStoreTest, AMoveIsTriedAgainOnlyForAFileInUse)
{
    Folder f;
    f.write("old");
    {
        std::ofstream o(f.path / "from", std::ios::binary);
        o << "new";
    }
    const std::string from = (f.path / "from").generic_u8string();
    g_held = hold(f, "from");
    ASSERT_NE(g_held, INVALID_HANDLE_VALUE);
    g_retries = 0;
    ecuio_files::replace_test_before_retry.store(count_and_let_go);
    std::string why;
    const bool moved = ecuio_files::replace_with(from, f.file(), &why);
    const int for_a_file_in_use = g_retries;
    g_retries = 0;
    std::string none;
    const bool again = ecuio_files::replace_with(from, f.file(), &none);
    ecuio_files::replace_test_before_retry.store(nullptr);
    if (g_held != INVALID_HANDLE_VALUE) {
        CloseHandle(g_held);
        g_held = INVALID_HANDLE_VALUE;
    }
    EXPECT_TRUE(moved) << why;
    EXPECT_EQ(for_a_file_in_use, 1) << "the second try went through";
    EXPECT_EQ(f.read(), "new");
    EXPECT_FALSE(again) << "nothing is left to move";
    EXPECT_FALSE(none.empty());
    EXPECT_EQ(g_retries, 0) << "and that is tried once";
}
#endif

/* replace_with by itself: a file over a file, over nothing, from a name
 * that isn't there, and never over a folder */
TEST(SecurityStoreTest, ReplaceWithPutsOneFileOverAnother)
{
    Folder f;
    const auto put = [&](const char* name, const char* text) {
        std::ofstream o(f.path / name, std::ios::binary | std::ios::trunc);
        o << text;
    };
    const auto get = [&](const char* name) {
        std::ifstream i(f.path / name, std::ios::binary);
        std::ostringstream s;
        s << i.rdbuf();
        return s.str();
    };
    const std::string a = (f.path / "a").generic_u8string();
    const std::string b = (f.path / "b").generic_u8string();
    put("a", "new");
    put("b", "old");
    std::string why;
    ASSERT_TRUE(ecuio_files::replace_with(a, b, &why)) << why;
    EXPECT_EQ(get("b"), "new");
    EXPECT_FALSE(fs::exists(f.path / "a"));
    put("a", "newer");
    std::error_code ec;
    fs::remove(f.path / "b", ec);
    ASSERT_TRUE(ecuio_files::replace_with(a, b, &why)) << why << ": over none";
    EXPECT_EQ(get("b"), "newer");
    EXPECT_FALSE(ecuio_files::replace_with(a, b, &why)) << "nothing to move";
    EXPECT_FALSE(why.empty());
    EXPECT_EQ(get("b"), "newer") << "what was there stands";
    put("a", "kept");
    fs::create_directories(f.path / "dir" / "inside", ec);
    const std::string dir = (f.path / "dir").generic_u8string();
    EXPECT_FALSE(ecuio_files::replace_with(a, dir, &why)) << "a folder";
    EXPECT_EQ(get("a"), "kept") << "the file stays where it was";
    EXPECT_TRUE(fs::exists(f.path / "dir" / "inside"));
}

/* Out of memory at each allocation of a load and of a save in turn: each
 * returns, a load holds a whole store or none, a save leaves the old file
 * and its store as they were or the new file whole, never a partial, and
 * no handle stays open. A save runs twice: with the file as its store
 * left it, and with a file it finds changed and merges (the store's
 * entries are the larger, so both write the same text) */
TEST(SecurityStoreTest, OutOfMemoryAnywhereLeavesTheStoreWhole)
{
    Folder f;
    store old;
    for (uint32_t k = 0; k < 8; ++k) {
        old.entries.push_back(entry(kRx + k, 0x01, k + 1, kUtc + k));
    }
    std::string why;
    /* Named before a failure is armed: the name's own string allocates */
    const std::string path = f.file();
    ASSERT_EQ(store_save(path, &old, &why), store_io::ok) << why;
    const std::string before = f.read();
    store next = old;
    next.entries[0].failed = 99;
    const std::string after = store_text(next);

    unsigned long from = news_on_this_thread();
    store got;
    ASSERT_EQ(store_load(path, &got, &why), store_io::ok) << why;
    const unsigned long load_news = news_on_this_thread() - from;
    ASSERT_GT(load_news, 5u);
    const unsigned long handles = open_handles();
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= load_news; ++n) {
        store s;
        fail_new_on_this_thread(n);
        const store_io io = store_load(path, &s, &why);
        fail_new_on_this_thread(0);
        if (io == store_io::ok) {
            expect_same(s, old);
            ASSERT_EQ(s.on_disk, before) << n;
        } else {
            ++refused;
            ASSERT_EQ(io, store_io::out_of_memory) << "at " << n << ": " << why;
            ASSERT_TRUE(s.entries.empty()) << n;
            ASSERT_TRUE(s.on_disk.empty()) << n;
        }
    }
    EXPECT_GT(refused, 0u);

    for (const bool merges : {false, true}) {
        store first = next;
        if (merges) {
            first.on_disk.clear();
        }
        from = news_on_this_thread();
        ASSERT_EQ(store_save(path, &first, &why), store_io::ok) << why;
        const unsigned long save_news = news_on_this_thread() - from;
        ASSERT_GT(save_news, 5u);
        ASSERT_EQ(f.read(), after) << merges;
        refused = 0;
        for (unsigned long n = 1; n <= save_news; ++n) {
            f.write(before);
            store s = next;
            if (merges) {
                s.on_disk.clear();
            }
            fail_new_on_this_thread(n);
            const store_io io = store_save(path, &s, &why);
            fail_new_on_this_thread(0);
            if (io == store_io::ok) {
                ASSERT_EQ(f.read(), after) << merges << " at " << n;
                ASSERT_EQ(s.on_disk, after) << n;
            } else {
                ++refused;
                ASSERT_EQ(io, store_io::out_of_memory)
                    << merges << " at " << n << ": " << why;
                ASSERT_EQ(f.read(), before) << merges << " at " << n;
                expect_same(s, next);
            }
            ASSERT_FALSE(partial_left(f.path, kName)) << n;
        }
        EXPECT_GT(refused, 0u) << merges;
        f.write(before);
    }
    EXPECT_EQ(open_handles(), handles) << "a file left open";
}

/* Only "no such file" is an empty store. A file that is there, or may be,
 * and can't be opened is refused by a load, and a save writes nothing
 * over what it couldn't look at */
TEST(SecurityStoreTest, AFileThatCantBeOpenedIsRefusedNeverTakenAsEmpty)
{
    Folder f;
    store kept;
    kept.entries.push_back(entry(kRx, 0x01, 3, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &kept, &why), store_io::ok) << why;
    const std::string before = f.read();
    for (const int err : {EACCES, EIO, ENOTDIR}) {
        store s;
        s.entries.push_back(entry(kRx, 0x03, 1, kUtc));
        store_test_open_errno.store(err);
        why.clear();
        EXPECT_EQ(store_load(f.file(), &s, &why), store_io::refused) << err;
        EXPECT_EQ(store_test_open_errno.load(), 0) << "the seam is used once";
        EXPECT_TRUE(s.entries.empty()) << err;
        EXPECT_EQ(why.rfind("the security store can't be opened: ", 0), 0u)
            << why;
        EXPECT_EQ(why.find(kName), std::string::npos) << "no path: " << why;
        store next = kept;
        next.entries[0].failed = 1;
        store_test_open_errno.store(err);
        EXPECT_EQ(store_save(f.file(), &next, &why), store_io::refused) << err;
        EXPECT_EQ(why.rfind("the security store can't be written: the file "
                            "there can't be opened: ",
                            0),
                  0u)
            << why;
        EXPECT_EQ(f.read(), before) << err;
        EXPECT_FALSE(partial_left(f.path, kName)) << err;
    }
    /* The one errno that is no file */
    store none;
    store_test_open_errno.store(ENOENT);
    EXPECT_EQ(store_load(f.file(), &none, &why), store_io::ok) << why;
    EXPECT_TRUE(none.entries.empty());
    /* And a real one, each platform's own */
    store s;
    why.clear();
#ifdef _WIN32
    /* Another program holds the store open and shares nothing */
    HANDLE held =
        CreateFileW((f.path / kName).wstring().c_str(), GENERIC_READ, 0,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    EXPECT_EQ(store_load(f.file(), &s, &why), store_io::refused)
        << "a file in use isn't no file";
    CloseHandle(held);
#else
    /* A name longer than a file system takes isn't "no such file" */
    const std::string longer =
        (f.path / std::string(300, 'a') / kName).generic_u8string();
    EXPECT_EQ(store_load(longer, &s, &why), store_io::refused);
#endif
    EXPECT_EQ(why.rfind("the security store can't be opened: ", 0), 0u) << why;
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    expect_same(back, kept);
}

/* One owner for a path is the rule, and a second writer's entries are
 * still kept. A save that finds the file isn't what its store last read
 * or wrote merges: each of the file's entries is kept, and where both
 * hold one, the larger count, the later second and the later mark stand */
TEST(SecurityStoreTest, ASaveKeepsWhatAnotherWriterWrote)
{
    Folder f;
    store first;
    first.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    first.entries.push_back(entry(kRx, 0x07, 1, kUtc + 5));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &first, &why), store_io::ok) << why;
    store a;
    store b;
    ASSERT_EQ(store_load(f.file(), &a, &why), store_io::ok) << why;
    ASSERT_EQ(store_load(f.file(), &b, &why), store_io::ok) << why;
    /* a writes: a failure on level 1, a later second and a mark on level
     * 7, and a module of its own */
    store_attempt_ended(*store_find(a, kCar, kRx, 0x01), true, false,
                        kUtc + 50);
    store_find(a, kCar, kRx, 0x07)->last_utc = kUtc + 10;
    store_attempt_began(*store_find(a, kCar, kRx, 0x07), kUtc + 80);
    a.entries.push_back(entry(kRxLong, 0x05, 4, kUtc + 60));
    ASSERT_EQ(store_save(f.file(), &a, &why), store_io::ok) << why;
    /* b, behind, holds the other half of each */
    store_find(b, kCar, kRx, 0x01)->last_utc = kUtc + 90;
    store_attempt_began(*store_find(b, kCar, kRx, 0x01), kUtc + 95);
    store_find(b, kCar, kRx, 0x07)->failed = 5;
    store_attempt_began(*store_find(b, kCar, kRx, 0x07), kUtc + 20);
    b.entries.push_back(entry(kRx, 0x03, 7, kUtc + 70));
    ASSERT_EQ(store_save(f.file(), &b, &why), store_io::ok) << why;
    ASSERT_EQ(b.entries.size(), 4u) << "the store holds what it wrote";
    const store_entry* one = store_find(b, kCar, kRx, 0x01);
    EXPECT_EQ(one->failed, 2u) << "the file's larger count";
    EXPECT_EQ(one->last_utc, kUtc + 90) << "its own later second";
    EXPECT_EQ(one->pending_utc, kUtc + 95) << "its own mark";
    const store_entry* seven = store_find(b, kCar, kRx, 0x07);
    EXPECT_EQ(seven->failed, 5u) << "its own larger count";
    EXPECT_EQ(seven->last_utc, kUtc + 10) << "the file's later second";
    EXPECT_EQ(seven->pending_utc, kUtc + 80) << "the file's later mark";
    ASSERT_NE(store_find(b, kCar, kRxLong, 0x05), nullptr)
        << "the other writer's entry is kept";
    EXPECT_EQ(store_find(b, kCar, kRxLong, 0x05)->failed, 4u);
    EXPECT_EQ(store_find(b, kCar, kRx, 0x03)->failed, 7u);
    EXPECT_EQ(f.read(), store_text(b));
    EXPECT_EQ(b.on_disk, f.read());
    /* The file is what b wrote now, so its next save is its own: a right
     * key's entry leaves the file */
    store_unlocked(*store_find(b, kCar, kRx, 0x01));
    ASSERT_EQ(store_save(f.file(), &b, &why), store_io::ok) << why;
    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    EXPECT_EQ(store_find(back, kCar, kRx, 0x01), nullptr);
    EXPECT_EQ(back.entries.size(), 3u);
    /* a, behind now, still holds that entry, and its save brings the
     * count back: too high for b's right key, never too low */
    ASSERT_EQ(store_save(f.file(), &a, &why), store_io::ok) << why;
    EXPECT_EQ(store_find(a, kCar, kRx, 0x01)->failed, 2u);
    EXPECT_EQ(a.entries.size(), 4u);
    EXPECT_EQ(f.read(), store_text(a));
}

/* A file that changed under a store and doesn't read is never written
 * over: an unreadable count isn't zero. A store that never read the file
 * merges with it, and one the merge would fill past its bound isn't
 * written */
TEST(SecurityStoreTest, AChangedFileIsNeverWrittenOverUnread)
{
    Folder f;
    store s;
    s.entries.push_back(entry(kRx, 0x03, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    const std::string texts[] = {
        "",
        "not json",
        with_entries("7"),
        std::string(kStoreMaxBytes + 1, ' '),
    };
    for (const std::string& text : texts) {
        f.write(text);
        s.entries[0].failed = 2;
        why.clear();
        EXPECT_EQ(store_save(f.file(), &s, &why), store_io::refused)
            << text.substr(0, 40);
        EXPECT_EQ(why.rfind("the security store can't be written: the file "
                            "there ",
                            0),
                  0u)
            << why;
        EXPECT_EQ(f.read(), text) << "left as it is";
        EXPECT_FALSE(partial_left(f.path, kName));
        EXPECT_EQ(f.entries(), 1u);
    }
    /* Nor by a store that never read a file: an empty file is no store */
    f.write("");
    store blank;
    blank.entries.push_back(entry(kRx, 0x03, 1, kUtc));
    EXPECT_EQ(store_save(f.file(), &blank, &why), store_io::refused);
    EXPECT_EQ(f.read(), "");
    /* kEntry's is level 1 with 2 failed; the store's own is level 3 */
    f.write(with_entries(kEntry));
    store fresh;
    fresh.entries.push_back(entry(kRx, 0x03, 1, kUtc));
    ASSERT_EQ(store_save(f.file(), &fresh, &why), store_io::ok) << why;
    ASSERT_EQ(fresh.entries.size(), 2u) << "the file's entry is kept";
    EXPECT_EQ(store_find(fresh, kCar, kRx, 0x01)->failed, 2u);
    const std::string two = f.read();
    store full;
    for (uint32_t k = 0; k < kStoreMaxEntries; ++k) {
        store_entry* e = store_entry_for(full, kOtherCar, kRxLong - k, 0x7D);
        ASSERT_NE(e, nullptr) << k;
        e->failed = 1;
    }
    EXPECT_EQ(store_save(f.file(), &full, &why), store_io::refused);
    EXPECT_EQ(why, "the security store is full");
    EXPECT_EQ(f.read(), two);
    EXPECT_EQ(full.entries.size(), kStoreMaxEntries) << "the store as it was";
    /* A file removed is forgotten: the store's own entries are written */
    std::error_code ec;
    fs::remove(f.path / kName, ec);
    ASSERT_EQ(store_save(f.file(), &fresh, &why), store_io::ok) << why;
    EXPECT_EQ(f.read(), two);
}
