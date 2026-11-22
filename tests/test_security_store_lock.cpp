// File/layer: tests/test_security_store_lock.cpp | test_core, Qt-free
// Testing: UNIT: SecurityStoreTest (security_store.cpp's lock across a
//          save, folder_files.h's lock_file: a second save waits for the
//          first's replace, a lock held past the wait refused in words)

#include "security_store.h"

#include "folder_files.h"
#include "partial_left.h"
#include "security_store_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace security_store_test;
using std::chrono::steady_clock;

namespace {

/* The second save, started from the first's seam before its replace */
std::string g_path;
store* g_second_store = nullptr;
std::thread g_second;
store_io g_second_io = store_io::refused;
std::string g_second_why;
std::atomic<bool> g_second_held{false};
std::atomic<bool> g_second_done{false};
std::atomic<int> g_held_tries{0};

void second_finds_it_held()
{
    g_second_held.store(true);
    ++g_held_tries;
}

/* Starts the second save and waits until it finds the lock held or has
 * saved: either way the first's replace comes after the second's look */
void start_second_save()
{
    g_second = std::thread([] {
        g_second_io = store_save(g_path, g_second_store, &g_second_why);
        g_second_done.store(true);
    });
    const auto until = steady_clock::now() + std::chrono::seconds(10);
    while (!g_second_held.load() && !g_second_done.load() &&
           steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/* The seams put back, whatever a case's checks do */
struct Seams {
    Seams() = default;
    ~Seams()
    {
        ecuio_files::lock_test_held.store(nullptr);
        store_test_before_replace.store(nullptr);
        store_test_lock_wait_ms.store(0);
        if (g_second.joinable()) {
            g_second.join();
        }
    }
    Seams(const Seams&) = delete;
    Seams& operator=(const Seams&) = delete;
};

} // namespace

/* Two saves of one file, each with an entry of its own: the second starts
 * while the first holds its file written and not yet put in place, so it
 * waits, then reads the first's file and keeps its entry. A lock taken
 * after the read, or let go before the replace, loses one of the two. */
TEST(SecurityStoreTest, ASecondSaveWaitsForTheFirstsReplace)
{
    Folder f;
    const Seams seams;
    store base;
    base.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &base, &why), store_io::ok) << why;
    store first;
    store second;
    ASSERT_EQ(store_load(f.file(), &first, &why), store_io::ok) << why;
    ASSERT_EQ(store_load(f.file(), &second, &why), store_io::ok) << why;
    first.entries.push_back(entry(kRx, 0x03, 2, kUtc + 10));
    second.entries.push_back(entry(kRxLong, 0x05, 3, kUtc + 20));

    g_path = f.file();
    g_second_store = &second;
    g_second_held.store(false);
    g_second_done.store(false);
    g_second_io = store_io::refused;
    store_test_lock_wait_ms.store(10000);
    ecuio_files::lock_test_held.store(second_finds_it_held);
    store_test_before_replace.store(start_second_save);
    const store_io io = store_save(f.file(), &first, &why);
    ASSERT_TRUE(g_second.joinable()) << "the seam ran";
    g_second.join();
    EXPECT_EQ(io, store_io::ok) << why;
    EXPECT_EQ(g_second_io, store_io::ok) << g_second_why;
    EXPECT_TRUE(g_second_held.load()) << "the second found the lock held";

    store back;
    ASSERT_EQ(store_load(f.file(), &back, &why), store_io::ok) << why;
    EXPECT_EQ(back.entries.size(), 3u);
    EXPECT_NE(store_find(back, kCar, kRx, 0x01), nullptr) << "the file's own";
    ASSERT_NE(store_find(back, kCar, kRx, 0x03), nullptr) << "the first's";
    EXPECT_EQ(store_find(back, kCar, kRx, 0x03)->failed, 2u);
    ASSERT_NE(store_find(back, kCar, kRxLong, 0x05), nullptr) << "the second's";
    EXPECT_EQ(store_find(back, kCar, kRxLong, 0x05)->failed, 3u);
    EXPECT_FALSE(partial_left(f.path, kName));
    EXPECT_EQ(f.entries(), 1u);
}

/* Another holder of the lock, past the shipped wait: the save waits it
 * out, trying again, and is refused in words with the file as it was. The
 * lock file stays beside the store; the holder gone, the save goes
 * through. */
TEST(SecurityStoreTest, ASaveWaitsForAHeldLockThenIsRefusedInWords)
{
    Folder f;
    const Seams seams;
    EXPECT_EQ(kStoreLockWaitMs, 250);
    store s;
    s.entries.push_back(entry(kRx, 0x01, 1, kUtc));
    std::string why;
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_TRUE(fs::exists(f.path / kLockName)) << "left beside the store";
    const std::string before = f.read();

    ecuio_files::file_lock other;
    ASSERT_EQ(ecuio_files::lock_file((f.path / kLockName).generic_u8string(), 0,
                                     &other, &why),
              ecuio_files::lock_got::locked)
        << why;
    s.entries[0].failed = 2;
    g_held_tries.store(0);
    ecuio_files::lock_test_held.store(second_finds_it_held);
    const auto from = steady_clock::now();
    const store_io io = store_save(f.file(), &s, &why);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            steady_clock::now() - from)
                            .count();
    ecuio_files::lock_test_held.store(nullptr);
    EXPECT_EQ(io, store_io::refused);
    EXPECT_EQ(why, "another program is using the security store");
    EXPECT_GE(waited, kStoreLockWaitMs);
    EXPECT_LT(waited, 5000);
    EXPECT_GT(g_held_tries.load(), 1) << "tried again while it waited";
    EXPECT_EQ(f.read(), before);
    EXPECT_EQ(s.on_disk, before) << "the store as it was";
    EXPECT_FALSE(partial_left(f.path, kName));

    /* A lock asked with no wait is refused at once */
    ecuio_files::file_lock third;
    EXPECT_EQ(ecuio_files::lock_file((f.path / kLockName).generic_u8string(), 0,
                                     &third, &why),
              ecuio_files::lock_got::busy);
    other.release();
    ASSERT_EQ(store_save(f.file(), &s, &why), store_io::ok) << why;
    EXPECT_EQ(f.read(), store_text(s));
    EXPECT_EQ(f.entries(), 1u);
}

/* A lock in a folder that isn't there fails in words, and holds nothing */
TEST(SecurityStoreTest, ALockInAFolderThatIsNotThereFailsInWords)
{
    Folder f;
    ecuio_files::file_lock lock;
    std::string why;
    EXPECT_EQ(
        ecuio_files::lock_file((f.path / "none" / kLockName).generic_u8string(),
                               0, &lock, &why),
        ecuio_files::lock_got::failed);
    EXPECT_EQ(why, "the folder it goes in doesn't exist");
    EXPECT_FALSE(lock.locked);
    EXPECT_EQ(f.entries(), 0u);
}
