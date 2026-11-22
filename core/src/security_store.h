// File/layer: core/src/security_store.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: SecurityStoreTest (test_core); FUZZ: fuzz_security_store
//
// The file that keeps security-access attempts across sessions (ADR-0066
// section 2.3): for each vehicle, module address and level, how many
// attempts failed since the last right key, the UTC second its delay last
// started, and a mark for an attempt that began and whose end was never
// written. The host names the file; core reads and writes it whole.
//
//   {"havne_security_store": 1,
//    "entries": [{"vehicle": "BA7816BF8F01CFEA", "rx": "7E8", "level": 1,
//                 "failed": 2, "last_utc": 1795132800, "pending_utc": 0}]}
//
// The rules:
//   - A file that isn't there is an empty store. A file that doesn't read
//     is refused, never taken as empty: a count that can't be read must
//     not read as zero. So is one that is there, or may be, and can't be
//     opened: only "no such file" is an empty store.
//   - An entry is one vehicle's: a module's own count lives in that car,
//     so two cars with one address and level share nothing. The file
//     holds 64 bits of the SHA-256 of the identity the host gives
//     (store_vehicle_of), never its text, which can be a label or a VIN.
//     That keeps the text out of a file that may be passed on; it is no
//     secret (a VIN can be found by trying each).
//   - The mark is on the disk before the attempt's seed is asked, so its
//     write never sits inside a key's window. An attempt marked and never
//     ended (a crash, a pulled cable) is a failed attempt: a read counts
//     it once and dates the delay from the mark.
//   - A write goes to a file of its own beside the store, is flushed to
//     the disk, then replaces the store, so a write that fails at any
//     step leaves the store as it was.
//   - A save holds a lock on a file of its own beside the store
//     (<store>.lock, left there) from its read of the store to the
//     replace, so two saves never overlap, in one process or two: the
//     second waits up to kStoreLockWaitMs, then is refused ("another
//     program is using the security store"). A load takes no lock: the
//     replace is one rename, so it reads the old file or the new whole.
//   - One owner for a path: one store, in one process, reads and writes
//     a file. A save that finds the file isn't what this store last read
//     or wrote merges three ways. Each side is judged against itself at
//     that read or write: the store's entries then (a load counts a mark
//     as a failure) and the file's then, marks in place. An entry only
//     one side changed is that side's whole; where both changed one, a
//     count or second only one changed is that side's, the failures each
//     side counted since are added (where one side's went down, the
//     larger stands), and the later second and the later mark stand. So a
//     right key's zero and an ended mark stay as written, two writers'
//     failures are both counted, and a mark a load counted is taken back
//     when its attempt ends well before that store's next save. A
//     count can still come out too high, never too low: once a store has
//     saved another's mark counted, that attempt ending well doesn't take
//     it back, and its failing counts it twice (the price of adding two
//     writers' failures). A store always holds what it last put in the
//     file, a failed save whose new file stood included.
//   - The words name no path (it can hold the account name).

#ifndef ECUIO_SECURITY_STORE_H
#define ECUIO_SECURITY_STORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecuio_security {

/* The file's bounds. kStoreMaxEntries of the longest entry stay under
 * kStoreMaxBytes as store_text writes them (asserted there). */
constexpr size_t kStoreMaxBytes = size_t{64} * 1024;
constexpr size_t kStoreMaxEntries = 256;
constexpr uint32_t kStoreMaxFailed = 0x7FFFFFFF;
/* The last second of the year 9999: a bound for a second read from a file */
constexpr int64_t kStoreMaxUtc = 253402300799;
constexpr uint32_t kStoreFormat = 1;
/* How long a save waits for another's lock on the store */
constexpr int kStoreLockWaitMs = 250;

struct store_entry {
    uint64_t vehicle = 0;    /* store_vehicle_of's */
    uint32_t rx = 0;         /* the module's response ID */
    uint8_t level = 0;       /* the seed request's sub-function */
    uint32_t failed = 0;     /* failed attempts since the last right key */
    int64_t last_utc = 0;    /* the second the delay last started; 0: never */
    int64_t pending_utc = 0; /* the second an attempt began whose end isn't
                                written; 0: none */
};

struct store {
    std::vector<store_entry> entries; /* each (vehicle, rx, level) once */
    std::string on_disk; /* the file's bytes as this store last read or
                            wrote them, "" for no file: store_load's and
                            store_save's to set */
    std::vector<store_entry> base; /* the entries as that read or write
                                      left them: a merge's base */
};

enum class store_io : uint8_t {
    ok,
    refused,       /* *why says what, in words that name no path */
    out_of_memory, /* *why untouched */
};

/* The number the file keeps for a vehicle's identity (the host's key for
 * it, or what else names the car): the first 8 bytes of its SHA-256.
 * Allocates nothing. */
uint64_t store_vehicle_of(const std::string& identity);

/* The entry of (vehicle, rx, level), or nullptr */
store_entry* store_find(store& s, uint64_t vehicle, uint32_t rx, uint8_t level);

/* The entry of (vehicle, rx, level), made when the store has none.
 * nullptr when the store is full (kStoreMaxEntries, each with something
 * to keep): the attempt it would count must not start. Throws
 * std::bad_alloc. Making one moves the others, and so does a save: no
 * pointer to an entry outlives the next call of either. */
store_entry* store_entry_for(store& s, uint64_t vehicle, uint32_t rx,
                             uint8_t level);

/* An attempt begins at utc: the mark. Write the store before the seed. */
void store_attempt_began(store_entry& e, int64_t utc);

/* The attempt ended at utc. failed: it counts one. delay: the delay runs
 * from now (a failure's always does; a seed's 7F 27 36 or 37 without
 * one). */
void store_attempt_ended(store_entry& e, bool failed, bool delay, int64_t utc);

/* The right key: nothing is left to keep, and the next write drops the
 * entry */
void store_unlocked(store_entry& e);

/* A store's text into out->entries: store_load's inside, for bytes from
 * anywhere. An entry with a mark comes out counted once, its delay dated
 * from the mark and the mark gone; one with nothing to keep is left out.
 * refused with "line L, column C: <why>". */
store_io store_read(const char* bytes, size_t size, store* out,
                    std::string* why);

/* The store's text, entries with nothing to keep left out: what
 * store_read reads back to the same entries. Throws std::bad_alloc. */
std::string store_text(const store& s);

/* The file at path (UTF-8) into *out, emptied first. No such file: ok and
 * empty. refused: the file can't be opened or read, is longer than
 * kStoreMaxBytes, or doesn't read as a store. */
store_io store_load(const std::string& path, store* out, std::string* why);

/* *s over the file at path, or beside nothing when there is none; merged
 * with the file first when the file isn't what *s last read or wrote
 * (the rules above), and *s then holds the merged entries. refused: the
 * lock can't be taken or another holds it past kStoreLockWaitMs, the
 * file there can't be read or doesn't read as a store (it is never
 * overwritten unread), the entries are past kStoreMaxEntries, or a step
 * of the write failed. On any failure the file and *s are as they were
 * and nothing of this write is left, with one exception the words name:
 * the folder's own flush failing after the replace (POSIX), where the new
 * file stands and *s holds it, as after a save that went through. */
store_io store_save(const std::string& path, store* s, std::string* why);

/* TEST-ONLY: the step of the next store_save that fails as a full disk
 * does, once: 1 the write, 2 the flush to the disk, 3 the replace, 4 the
 * folder's flush after the replace (the new file stands); 0 none. */
inline std::atomic<int> store_test_fail_at{0};

/* TEST-ONLY: an errno the next open of the file for reading (a load's, a
 * save's look at what is there) fails with, once; 0 none. */
inline std::atomic<int> store_test_open_errno{0};

/* TEST-ONLY: the lock's wait in ms; 0 kStoreLockWaitMs */
inline std::atomic<int> store_test_lock_wait_ms{0};

/* TEST-ONLY: called once, by the next save, with its lock held and its
 * file written, just before the replace */
inline std::atomic<void (*)()> store_test_before_replace{nullptr};

} // namespace ecuio_security

#endif /* ECUIO_SECURITY_STORE_H */
