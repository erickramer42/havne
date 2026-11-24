// File/layer: core/src/consent_store.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: ConsentStoreTest (test_core); FUZZ: fuzz_consent_store
//
// The file that keeps a person's consents to plugins that talk to a
// module (ADR-0066's appendix of 2026-11-23): one record for each
// fingerprint consented to, made from the plugin itself. The host names
// the file; core reads and writes it whole.
//
//   {"havne_consent_store": 1,
//    "entries": [{"id": "talker", "version": "1.0.0",
//                 "fingerprint": "<64 hex>", "terms": "<64 hex>",
//                 "publisher": "Made-up Tools", "given_utc": 1795132800}]}
//
// The security store's rules, through its file steps (store_file.h):
//   - Only "no such file" is an empty store; a file that doesn't read, or
//     is there and can't be opened, is refused.
//   - A save holds <store>.lock from its read of the file to the replace
//     and waits up to kStoreLockWaitMs for another's.
//   - A save that finds the file isn't what this store last read or wrote
//     merges by fingerprint, three ways: a record this store gave or
//     withdrew since then is this store's; any other is the file's. So
//     two writers' consents are both kept, and a withdraw stands.
//   - One owner for a path in a process (path_registry.h).
//   - The words name no path.

#ifndef ECUIO_CONSENT_STORE_H
#define ECUIO_CONSENT_STORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecuio_consent {

/* kStoreMaxEntries of the longest record stay under kStoreMaxBytes as
 * store_text writes them (asserted there) */
constexpr size_t kStoreMaxBytes = size_t{192} * 1024;
constexpr size_t kStoreMaxEntries = 256;
/* The last second of the year 9999 */
constexpr int64_t kStoreMaxUtc = 253402300799;
constexpr uint32_t kStoreFormat = 1;
constexpr int kStoreLockWaitMs = 250;

struct record {
    std::string id;      /* the plugin's id */
    std::string version; /* its X.Y.Z */
    uint8_t fingerprint[32] = {};
    uint8_t terms_sha256[32] = {};
    std::string publisher; /* the publisher's name */
    int64_t given_utc = 0; /* 1 to kStoreMaxUtc */
};

struct store {
    std::vector<record> records; /* each fingerprint once */
    std::string on_disk;         /* the file's bytes as last read or written */
    std::vector<record> base;    /* the records then: a merge's base */
};

enum class store_io : uint8_t {
    ok,
    refused,       /* *why says what, in words that name no path */
    out_of_memory, /* *why untouched */
};

/* The record of fingerprint, or nullptr */
const record* store_find(const store& s, const uint8_t fingerprint[32]);

/* A store's text into *out: refused with "line L, column C: <why>" */
store_io store_read(const char* bytes, size_t size, std::vector<record>* out,
                    std::string* why);

/* The records' text, which store_read reads back. Throws bad_alloc. */
std::string store_text(const std::vector<record>& records);

/* The file at path into *out, emptied first; no such file is ok and
 * empty */
store_io store_load(const std::string& path, store* out, std::string* why);

/* *s over the file at path, merged with the file first when it isn't
 * what *s last read or wrote; *s then holds what the file holds. refused:
 * the lock, the file there not read (never overwritten unread), past
 * kStoreMaxEntries, or a step of the write; on any failure the file and
 * *s are as they were, but for the folder's flush after the replace,
 * where the new file stands and *s holds it. */
store_io store_save(const std::string& path, store* s, std::string* why);

/* TEST-ONLY seams, as the security store's: the next save's failing
 * step (1 the write, 2 the flush, 3 the replace, 4 the folder's flush),
 * an errno the next open for reading fails with, the lock's wait in ms
 * (0 kStoreLockWaitMs), and a call just before the next replace */
inline std::atomic<int> store_test_fail_at{0};
inline std::atomic<int> store_test_open_errno{0};
inline std::atomic<int> store_test_lock_wait_ms{0};
inline std::atomic<void (*)()> store_test_before_replace{nullptr};

} // namespace ecuio_consent

#endif /* ECUIO_CONSENT_STORE_H */
