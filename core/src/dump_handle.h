// File/layer: core/src/dump_handle.h | internal, C++17, Qt-free
// Testing: UNIT: DumpFileTest, DumpExportTest (test_core)
//
// An opened dump file (ecuio/dump.h's handle) and what the calls on one
// share: dump_file.cpp reads it, dump_export.cpp copies it.

#ifndef ECUIO_DUMP_HANDLE_H
#define ECUIO_DUMP_HANDLE_H

#include "dump_file.h"
#include "ecuio/recording.h" /* ecuio_cancel_t */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

struct ecuio_dump {
    FILE* file = nullptr;
    std::string name; /* the file's own, without its folder */
    uint64_t file_bytes = 0;
    ecuio_dump_file::header h;
    ecuio_dump_file::record r;
    std::mutex m; /* the file's position is one call's at a time */

    ecuio_dump() = default;
    ~ecuio_dump()
    {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
    ecuio_dump(const ecuio_dump&) = delete;
    ecuio_dump& operator=(const ecuio_dump&) = delete;
};

namespace ecuio_dump_handle {

/* A read's piece, and how often a cancel is asked: once for each */
constexpr size_t kPiece = 64 * 1024;

/* What a file whose checksums don't hold is told */
constexpr char kDamagedWords[] =
    "its bytes aren't the ones its header seals: the file is damaged";

/* What a whole read found; sound says the two inner checksums hold */
struct sums {
    bool sound = false;
    uint8_t file[32] = {};
};

/* s into dst, cut to cap - 1 bytes, with its NUL */
void copy_text(char* dst, size_t cap, const std::string& s);

/* The file at path as a handle in *out, with no detail set: LOAD_FAILED
 * with *why, or OUT_OF_MEMORY. *file_bytes, when given, is the file's
 * size wherever it could be opened. */
ecuio_status_t open_at(const std::string& path,
                       std::unique_ptr<ecuio_dump>* out, std::string* why,
                       uint64_t* file_bytes = nullptr);

/* The whole file read again, the caller holding d.m: LOAD_FAILED with
 * *why when it no longer reads or isn't the file that was opened,
 * CANCELED, OUT_OF_MEMORY */
ecuio_status_t hash_file(ecuio_dump& d, ecuio_cancel_t cancel, void* user,
                         sums* out, std::string* why);

/* Whether a folder's list shows a file of this name: a copy's suffix,
 * or a partial's */
bool listed(const std::string& name);

/* The file at utf8 set read-only, as a copy is kept; false with *why */
bool set_read_only(const std::string& utf8, std::string* why);

/* TEST-ONLY: the step of a file's writing that fails next, once, as a
 * disk that won't flush fails it; 0 none */
constexpr int kFailFileFlush = 1;   /* the file's own flush */
constexpr int kFailFolderFlush = 2; /* its folder's, after the rename */
inline std::atomic<int> test_fail_step{0};

/* TEST-ONLY: taken and called once with a written file's partial name,
 * the file flushed and closed, before it is read back */
inline std::atomic<void (*)(const char*)> test_before_read_back{nullptr};

} // namespace ecuio_dump_handle

#endif /* ECUIO_DUMP_HANDLE_H */
