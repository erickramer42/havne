// File/layer: tests/test_security_store_owner.cpp | test_core, Qt-free
// Testing: UNIT: SecurityStoreTest (security_store_owner.{h,cpp} and
//          path_registry.h: one owner for each path, each step saved at
//          once and answered as found again, a full store refused in
//          words, out of memory at an open and at a mark)

#include "security_store_owner.h"

#include "folder_files.h"
#include "open_handles.h"
#include "partial_left.h"
#include "security_store_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

using namespace security_store_test;

namespace {

const store_key kKey{kCar, kRx, 0x01};
const store_key kOtherKey{kCar, kRxLong, 0x03};
constexpr uint32_t kSteps = 10; /* each thread's attempts */

/* A second step started from inside the first's save */
store_owner* g_owner = nullptr;
std::thread g_other;
store_io g_other_io = store_io::refused;
std::string g_other_why;
std::atomic<bool> g_other_done{false};
std::atomic<bool> g_other_at_lock{false};
std::atomic<bool> g_done_while_saving{false};

void other_reaches_the_lock()
{
    g_other_at_lock.store(true);
}

/* A negative check: it waits 200 ms, so under load a mutant may pass, the
 * fixed code never fails */
void step_from_another_thread()
{
    g_other = std::thread([] {
        g_other_io =
            g_owner->ended(kOtherKey, true, false, kUtc, nullptr, &g_other_why);
        g_other_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_done_while_saving.store(g_other_done.load());
}

/* What the file holds for key once loaded: a mark there counts once */
store_entry loaded(const Folder& f, const store_key& key)
{
    store s;
    std::string why;
    EXPECT_EQ(store_load(f.file(), &s, &why), store_io::ok) << why;
    const store_entry* e = store_find(s, key.vehicle, key.rx, key.level);
    return e != nullptr ? *e : store_entry{};
}

} // namespace

/* Two opens of a path share its owner, another path has its own, and the
 * last release ends it: the path's next open reads the file it left */
TEST(SecurityStoreTest, TwoOpensOfAPathShareOneOwner)
{
    Folder f;
    const size_t before = store_owners_open();
    store_ref a;
    store_ref b;
    std::string why;
    ASSERT_EQ(store_open(f.file(), &a, &why), store_io::ok) << why;
    ASSERT_EQ(store_open(f.file(), &b, &why), store_io::ok) << why;
    ASSERT_TRUE(a);
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(store_owners_open(), before + 1);
    store_entry after;
    ASSERT_EQ(a->began(kKey, kUtc, &after, &why), store_io::ok) << why;
    EXPECT_EQ(b->counts(kKey).pending_utc, kUtc) << "one store";

    store_ref c;
    const std::string other = (f.path / "other.json").generic_u8string();
    ASSERT_EQ(store_open(other, &c, &why), store_io::ok) << why;
    EXPECT_NE(c.get(), a.get());
    EXPECT_EQ(c->counts(kKey).pending_utc, 0) << "a store of its own";
    EXPECT_EQ(store_owners_open(), before + 2);
    a.reset();
    EXPECT_EQ(store_owners_open(), before + 2) << "b still holds it";
    b.reset();
    EXPECT_EQ(store_owners_open(), before + 1);
    store_ref moved = std::move(c);
    EXPECT_FALSE(c);
    moved.reset();
    EXPECT_EQ(store_owners_open(), before);

    store_ref d;
    ASSERT_EQ(store_open(f.file(), &d, &why), store_io::ok) << why;
    EXPECT_EQ(d->counts(kKey).failed, 1u) << "the mark counted once";
    EXPECT_EQ(d->counts(kKey).pending_utc, 0);
}

/* Each step is on the disk when it returns, and answers the entry */
TEST(SecurityStoreTest, AnOwnersStepsAreSavedAtOnce)
{
    Folder f;
    store_ref r;
    std::string why;
    ASSERT_EQ(store_open(f.file(), &r, &why), store_io::ok) << why;
    store_entry after;
    ASSERT_EQ(r->began(kKey, kUtc, &after, &why), store_io::ok) << why;
    EXPECT_EQ(after.pending_utc, kUtc);
    EXPECT_EQ(after.rx, kRx);
    EXPECT_NE(f.read().find("\"pending_utc\": 1795132800"), std::string::npos)
        << "the mark is in the file";
    ASSERT_EQ(r->ended(kKey, true, false, kUtc + 3, &after, &why), store_io::ok)
        << why;
    EXPECT_EQ(after.failed, 1u);
    EXPECT_EQ(after.last_utc, kUtc + 3);
    EXPECT_EQ(after.pending_utc, 0);
    EXPECT_EQ(loaded(f, kKey).failed, 1u) << "and its end";
    EXPECT_EQ(loaded(f, kKey).last_utc, kUtc + 3);
    ASSERT_EQ(r->unlocked(kKey, &after, &why), store_io::ok) << why;
    EXPECT_EQ(after.failed, 0u);
    EXPECT_EQ(after.last_utc, 0);
    EXPECT_EQ(f.read().find("\"rx\": \"7E8\""), std::string::npos)
        << "the right key's entry leaves the file";
    const std::string left = f.read();
    ASSERT_EQ(r->unlocked(kKey, nullptr, &why), store_io::ok)
        << "a key with no entry";
    EXPECT_EQ(f.read(), left);
}

/* Another program wrote the file meanwhile: the step's save merges, and
 * it answers the larger count at once */
TEST(SecurityStoreTest, AnOwnersStepAnswersTheMergesLargerCounts)
{
    Folder f;
    store_ref r;
    std::string why;
    ASSERT_EQ(store_open(f.file(), &r, &why), store_io::ok) << why;
    f.write(with_entries(kEntry)); /* kKey's: 2 failed at kUtc */
    store_entry after;
    ASSERT_EQ(r->began(kKey, kUtc + 9, &after, &why), store_io::ok) << why;
    EXPECT_EQ(after.failed, 2u) << "the file's count";
    EXPECT_EQ(after.last_utc, kUtc);
    EXPECT_EQ(after.pending_utc, kUtc + 9);
    EXPECT_EQ(r->counts(kKey).failed, 2u);
    /* And again before the attempt's end: its answer is the second merge's */
    f.write(entry_with("\"failed\": 2", "\"failed\": 6"));
    ASSERT_EQ(r->ended(kKey, false, false, kUtc + 10, &after, &why),
              store_io::ok)
        << why;
    EXPECT_EQ(after.failed, 6u);
    EXPECT_EQ(after.pending_utc, 0);
}

/* A full store refuses an entry it hasn't room for, in words, with the
 * file as it was; an entry it holds still takes its steps */
TEST(SecurityStoreTest, AFullStoreRefusesANewEntryInWords)
{
    Folder f;
    store full;
    for (uint32_t k = 0; k < kStoreMaxEntries; ++k) {
        store_entry* e = store_entry_for(full, kOtherCar, kRxLong - k, 0x7D);
        ASSERT_NE(e, nullptr) << k;
        e->failed = 1;
    }
    f.write(store_text(full));
    const std::string before = f.read();
    store_ref r;
    std::string why;
    ASSERT_EQ(store_open(f.file(), &r, &why), store_io::ok) << why;
    store_entry after;
    after.failed = 9;
    EXPECT_EQ(r->began(kKey, kUtc, &after, &why), store_io::refused);
    EXPECT_EQ(why, "the security store holds 256 entries and has no room");
    EXPECT_EQ(after.failed, 0u);
    EXPECT_EQ(after.pending_utc, 0);
    why.clear();
    EXPECT_EQ(r->ended(kKey, true, false, kUtc, &after, &why),
              store_io::refused);
    EXPECT_EQ(why, "the security store holds 256 entries and has no room");
    EXPECT_EQ(f.read(), before);
    const store_key held{kOtherCar, kRxLong, 0x7D};
    ASSERT_EQ(r->began(held, kUtc, &after, &why), store_io::ok) << why;
    EXPECT_EQ(after.failed, 1u);
    EXPECT_EQ(after.pending_utc, kUtc);
}

/* A mark whose save is refused is taken back: the attempt doesn't start,
 * and the next save leaves no mark of it, a new file that stood after a
 * failed folder flush included */
TEST(SecurityStoreTest, ARefusedMarkIsTakenBack)
{
    Folder f;
    store_ref r;
    std::string why;
    ASSERT_EQ(store_open(f.file(), &r, &why), store_io::ok) << why;
    for (const int step : {1, 4}) {
        store_test_fail_at.store(step);
        store_entry after;
        EXPECT_EQ(r->began(kKey, kUtc, &after, &why), store_io::refused)
            << step;
        EXPECT_EQ(after.pending_utc, 0) << step;
        EXPECT_EQ(r->counts(kKey).pending_utc, 0) << step;
        ASSERT_EQ(r->began(kOtherKey, kUtc + step, &after, &why), store_io::ok)
            << step << ": " << why;
        EXPECT_EQ(loaded(f, kKey).failed, 0u) << step << ": no mark of it";
        ASSERT_EQ(r->ended(kOtherKey, false, false, kUtc + 5, nullptr, &why),
                  store_io::ok)
            << why;
    }
    EXPECT_FALSE(partial_left(f.path, kName));
}

/* A file that doesn't read opens no owner, and the next open reads it
 * again */
TEST(SecurityStoreTest, AFileThatDoesntReadOpensNoOwner)
{
    Folder f;
    f.write("not json");
    const size_t before = store_owners_open();
    for (int pass = 0; pass < 2; ++pass) {
        store_ref r;
        std::string why;
        EXPECT_EQ(store_open(f.file(), &r, &why), store_io::refused) << pass;
        EXPECT_EQ(why.rfind("the security store doesn't read (", 0), 0u) << why;
        EXPECT_FALSE(r);
        EXPECT_EQ(store_owners_open(), before);
    }
    EXPECT_EQ(f.read(), "not json");
}

/* Two callers' steps through one owner, at once from two threads: each
 * one's every step is counted */
TEST(SecurityStoreTest, TwoThreadsStepThroughOneOwner)
{
    Folder f;
    const auto steps = [&f](const store_key& key, store_io* io) {
        store_ref r;
        std::string why;
        *io = store_open(f.file(), &r, &why);
        for (uint32_t k = 0; k < kSteps && *io == store_io::ok; ++k) {
            *io = r->began(key, kUtc + k, nullptr, &why);
            if (*io == store_io::ok) {
                *io = r->ended(key, true, false, kUtc + k, nullptr, &why);
            }
        }
    };
    store_io one = store_io::refused;
    store_io two = store_io::refused;
    std::thread a(steps, kKey, &one);
    std::thread b(steps, kOtherKey, &two);
    a.join();
    b.join();
    EXPECT_EQ(one, store_io::ok);
    EXPECT_EQ(two, store_io::ok);
    EXPECT_EQ(loaded(f, kKey).failed, kSteps);
    EXPECT_EQ(loaded(f, kOtherKey).failed, kSteps);
}

/* A step waits for another's save to end: the second never reaches the
 * file's lock, nor ends, while the first holds its file written and not
 * yet in place, then takes its turn */
TEST(SecurityStoreTest, AnOwnersStepWaitsForAnothersSave)
{
    Folder f;
    store_ref r;
    std::string why;
    ASSERT_EQ(store_open(f.file(), &r, &why), store_io::ok) << why;
    g_owner = r.get();
    g_other_io = store_io::refused;
    g_other_done.store(false);
    g_other_at_lock.store(false);
    g_done_while_saving.store(false);
    ecuio_files::lock_test_held.store(other_reaches_the_lock);
    store_test_before_replace.store(step_from_another_thread);
    const store_io io = r->began(kKey, kUtc, nullptr, &why);
    const bool at_lock = g_other_at_lock.load();
    store_test_before_replace.store(nullptr);
    if (g_other.joinable()) {
        g_other.join();
    }
    ecuio_files::lock_test_held.store(nullptr);
    EXPECT_EQ(io, store_io::ok) << why;
    EXPECT_FALSE(at_lock) << "the second step went past the owner's mutex";
    EXPECT_FALSE(g_done_while_saving.load());
    EXPECT_EQ(g_other_io, store_io::ok) << g_other_why;
    EXPECT_EQ(loaded(f, kKey).failed, 1u) << "the first's mark";
    EXPECT_EQ(loaded(f, kOtherKey).failed, 1u) << "the second's end";
}

/* Out of memory at each allocation of an open, then of a mark: an open
 * answers an owner or none, with nothing left open; a mark is in the file
 * and the store, or in neither, never a partial, no handle left open */
TEST(SecurityStoreTest, OutOfMemoryAtAnOpenOrAMarkLeavesNothingHalfDone)
{
    Folder f;
    store first;
    for (uint32_t k = 0; k < 8; ++k) {
        first.entries.push_back(entry(kRx + k, 0x03, k + 1, kUtc + k));
    }
    std::string why;
    /* Named before a failure is armed: the name's own string allocates */
    const std::string path = f.file();
    ASSERT_EQ(store_save(path, &first, &why), store_io::ok) << why;
    const std::string before = f.read();
    const size_t open_before = store_owners_open();
    const unsigned long handles = open_handles();

    unsigned long from = news_on_this_thread();
    {
        store_ref r;
        ASSERT_EQ(store_open(path, &r, &why), store_io::ok) << why;
    }
    const unsigned long open_news = news_on_this_thread() - from;
    ASSERT_GT(open_news, 5u);
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= open_news; ++n) {
        store_ref r;
        fail_new_on_this_thread(n);
        const store_io io = store_open(path, &r, &why);
        fail_new_on_this_thread(0);
        if (io == store_io::ok) {
            ASSERT_TRUE(r) << n;
            ASSERT_EQ(r->counts(store_key{kCar, kRx + 7, 0x03}).failed, 8u)
                << n;
        } else {
            ++refused;
            ASSERT_EQ(io, store_io::out_of_memory) << n << ": " << why;
            ASSERT_FALSE(r) << n;
            ASSERT_EQ(store_owners_open(), open_before) << n;
        }
    }
    EXPECT_GT(refused, 0u);

    {
        store_ref r;
        ASSERT_EQ(store_open(path, &r, &why), store_io::ok) << why;
        from = news_on_this_thread();
        ASSERT_EQ(r->began(kKey, kUtc, nullptr, &why), store_io::ok) << why;
    }
    const unsigned long mark_news = news_on_this_thread() - from;
    ASSERT_GT(mark_news, 5u);
    refused = 0;
    for (unsigned long n = 1; n <= mark_news; ++n) {
        f.write(before);
        store_ref r;
        ASSERT_EQ(store_open(path, &r, &why), store_io::ok) << why;
        store_entry after;
        fail_new_on_this_thread(n);
        const store_io io = r->began(kKey, kUtc, &after, &why);
        fail_new_on_this_thread(0);
        if (io == store_io::ok) {
            ASSERT_EQ(after.pending_utc, kUtc) << n;
            ASSERT_EQ(loaded(f, kKey).failed, 1u) << n << ": the mark's";
        } else {
            ++refused;
            ASSERT_EQ(io, store_io::out_of_memory) << n << ": " << why;
            ASSERT_EQ(after.pending_utc, 0) << n;
            ASSERT_EQ(r->counts(kKey).pending_utc, 0) << n;
            ASSERT_EQ(f.read(), before) << n;
        }
        ASSERT_FALSE(partial_left(f.path, kName)) << n;
    }
    EXPECT_GT(refused, 0u);
    EXPECT_EQ(store_owners_open(), open_before);
    EXPECT_EQ(open_handles(), handles) << "a file left open";
}
