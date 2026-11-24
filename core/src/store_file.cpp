// File/layer: core/src/store_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SecurityStoreTest, ConsentStoreTest (test_core)

#include "store_file.h"

#include "folder_files.h"

#include <cerrno>
#include <cstdio>
#include <memory>
#include <new>

namespace ecuio_store_file {

namespace {

struct file_closer {
    void operator()(FILE* f) const { std::fclose(f); }
};

/* text into a file of this write's own beside path, flushed to the disk
 * and closed, its name in *name; false with *why, the file removed and
 * *name empty. fail is the seam's step. */
bool write_beside(const std::string& path, const std::string& text, int fail,
                  std::string* name, std::string* why)
{
    std::unique_ptr<FILE, file_closer> f(
        ecuio_files::partial_file(path, name, why));
    if (f == nullptr) {
        name->clear(); /* none was made: the name may be another's */
        return false;
    }
    bool ok = fail != 1 &&
              std::fwrite(text.data(), 1, text.size(), f.get()) == text.size();
    ok = ok && fail != 2 && flush_to_disk(f.get());
    ok = (std::fclose(f.release()) == 0) && ok;
    if (!ok) {
        ecuio_files::remove_file(*name);
        name->clear();
        *why = "the disk is full or the file can't be flushed";
    }
    return ok;
}

} // namespace

file_read read_file(const std::string& path, size_t max, int open_errno,
                    std::string* bytes, std::string* why)
{
    bytes->clear();
    bool none = false;
    std::string opened;
    const std::unique_ptr<FILE, file_closer> f(
        open_errno != 0 ? nullptr
                        : ecuio_files::open_existing(path, &none, &opened));
    if (f == nullptr) {
        if (open_errno != 0) {
            none = open_errno == ENOENT;
            opened = ecuio_files::errno_words(open_errno);
        }
        if (none) {
            return file_read::none;
        }
        *why = "can't be opened: " + opened;
        return file_read::failed;
    }
    bytes->resize(max + 1);
    const size_t got = std::fread(&(*bytes)[0], 1, bytes->size(), f.get());
    if (std::ferror(f.get()) != 0) {
        bytes->clear();
        *why = "can't be read";
        return file_read::failed;
    }
    bytes->resize(got);
    return file_read::read;
}

bool flush_to_disk(FILE* f)
{
    if (std::fflush(f) != 0) {
        return false;
    }
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#elif defined(__APPLE__)
    /* fsync leaves the drive's own cache unwritten on macOS; F_FULLFSYNC
     * asks it through, where the file system can */
    return fcntl(fileno(f), F_FULLFSYNC) == 0 || fsync(fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

bool replace_whole(const std::string& path, const std::string& text, int fail,
                   std::atomic<void (*)()>* before_replace, bool* renamed,
                   std::string* why)
{
    std::string name; /* this write's own file, while there is one */
    *renamed = false;
    try {
        if (!write_beside(path, text, fail, &name, why)) {
            return false;
        }
        if (before_replace != nullptr) {
            if (void (*hook)() = before_replace->exchange(nullptr)) {
                hook();
            }
        }
        if (fail == 3) {
            *why = "the disk is full";
        }
        bool replaced =
            fail != 3 && ecuio_files::replace_with(name, path, why, renamed);
        if (fail == 4 && replaced) {
            replaced = false;
            *why = "its folder can't be flushed";
        }
        if (!*renamed) {
            ecuio_files::remove_file(name);
        }
        return replaced;
    } catch (const std::bad_alloc&) {
        if (!*renamed && !name.empty()) {
            ecuio_files::remove_file(name);
        }
        throw;
    }
}

} // namespace ecuio_store_file
