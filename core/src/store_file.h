// File/layer: core/src/store_file.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: SecurityStoreTest, ConsentStoreTest (test_core)
//
// The file steps of a store core owns and rewrites whole (core.md's
// exception to a file core writes whole): the security store and the
// consent store. A read takes the file's bytes up to a bound, "no such
// file" alone reading as none; a write goes to a file of its own beside
// the store, is flushed to the disk and then replaces the store, so a
// write that fails at any step leaves the store as it was. The words
// name no path.

#ifndef ECUIO_STORE_FILE_H
#define ECUIO_STORE_FILE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ecuio_store_file {

enum class file_read : uint8_t {
    none,   /* no such file */
    read,   /* its bytes are in *bytes */
    failed, /* it is there, or may be, and can't be opened or read */
};

/* The file's bytes into *bytes, max + 1 at most, so a longer file shows
 * as one; failed with *why, words that follow the store's name.
 * open_errno: an errno the open fails with instead (a test seam), 0 none;
 * ENOENT reads as no file. */
file_read read_file(const std::string& path, size_t max, int open_errno,
                    std::string* bytes, std::string* why);

/* f's bytes down to the disk itself */
bool flush_to_disk(FILE* f);

/* text over the file at path: written to a file of its own beside it,
 * flushed, then put over it (ecuio_files::replace_with). fail is a test
 * seam's step that fails as a full disk does: 1 the write, 2 the flush,
 * 3 the replace, 4 the folder's flush after the replace (the new file
 * stands); 0 none. before_replace, when it holds a function, is taken and
 * called once the file beside is written. false with *why; *renamed says
 * whether the new file stands. Nothing of the write is left behind it
 * when the replace didn't go through, bad_alloc (rethrown) included. */
bool replace_whole(const std::string& path, const std::string& text, int fail,
                   std::atomic<void (*)()>* before_replace, bool* renamed,
                   std::string* why);

} // namespace ecuio_store_file

#endif /* ECUIO_STORE_FILE_H */
