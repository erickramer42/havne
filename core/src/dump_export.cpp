// File/layer: core/src/dump_export.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: DumpExportTest (test_core)
//
// A copy leaving its folder and coming back (ecuio/dump.h): the export's
// two forms, the import, and the one call that removes a file. Each file
// is written whole under a partial name, flushed to the disk, read back,
// then renamed without replacing. Words name no path.
//
// No case on a working disk tells a flush made from one left out: the two
// flushes are pinned through their seam's failures (dump_handle.h).

#include "dump_handle.h"

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"
#include "scrub.h"
#include "sha256.h"
#include "store_file.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using ecuio_dump_file::kHeaderBytes;
using ecuio_dump_handle::copy_text;
using ecuio_dump_handle::hash_file;
using ecuio_dump_handle::kDamagedWords;
using ecuio_dump_handle::kPiece;
using ecuio_dump_handle::open_at;
using ecuio_dump_handle::sums;

namespace files = ecuio_files;

namespace {

/* Whether the seam fails this step, taking the failure if so */
bool seam_fails(int step)
{
    return ecuio_dump_handle::test_fail_step.compare_exchange_strong(step, 0);
}

/* A file of this call's own removed, read-only or not: whether it is
 * gone. Throws nothing. */
bool drop(const std::string& utf8) noexcept
{
    try {
#ifdef _WIN32
        const std::wstring w = files::wide(utf8);
        if (w.empty()) {
            return false;
        }
        SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(w.c_str()) != 0 ||
               GetLastError() == ERROR_FILE_NOT_FOUND;
#else
        return unlink(utf8.c_str()) == 0 || errno == ENOENT;
#endif
    } catch (const std::bad_alloc&) {
        return false; /* left: nothing more can be done without memory */
    }
}

/* The folder holding the file at utf8 flushed, so the file's name is on
 * the disk as its bytes are. Windows keeps a rename in its own journal;
 * a file system that can't flush a folder passes. */
bool flush_folder_of(const std::string& utf8, std::string* why)
{
    if (seam_fails(ecuio_dump_handle::kFailFolderFlush)) {
        *why = "its folder can't be flushed";
        return false;
    }
#ifdef _WIN32
    (void)utf8;
    return true;
#else
    const size_t slash = utf8.find_last_of('/');
    const std::string folder = slash == std::string::npos ? "."
                               : slash == 0 ? "/"
                                            : utf8.substr(0, slash);
    const int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        *why = "its folder can't be flushed: " + files::os_words(errno);
        return false;
    }
    const bool flushed = fsync(fd) == 0 || files::not_supported(errno);
    const int err = errno;
    close(fd);
    if (!flushed) {
        *why = "its folder can't be flushed: " + files::os_words(err);
    }
    return flushed;
#endif
}

/* A file written whole: made under a partial name, flushed to the disk,
 * put in place by a rename that never replaces, and removed on any way
 * out before that */
class whole_file
{
public:
    whole_file() = default;
    ~whole_file()
    {
        (void)close();
        if (!partial_.empty()) {
            (void)drop(partial_);
        }
    }
    whole_file(const whole_file&) = delete;
    whole_file& operator=(const whole_file&) = delete;

    bool begin(const std::string& out, std::string* why)
    {
        out_ = out;
        file_ = files::partial_file(out, &partial_, why);
        if (file_ == nullptr) {
            partial_.clear(); /* not this call's: never removed */
        }
        return file_ != nullptr;
    }
    bool write(const uint8_t* p, size_t n)
    {
        return std::fwrite(p, 1, n, file_) == n;
    }
    /* Flushed through to the disk, then closed */
    bool close()
    {
        if (file_ == nullptr) {
            return true;
        }
        FILE* f = file_;
        file_ = nullptr;
        const bool flushed = !seam_fails(ecuio_dump_handle::kFailFileFlush) &&
                             ecuio_store_file::flush_to_disk(f);
        return std::fclose(f) == 0 && flushed;
    }
    const std::string& partial() const { return partial_; }
    /* Once true the file is at its name, its folder flushed, and no
     * longer removed here; a folder that won't flush, for want of memory
     * too, takes it away again */
    bool place(std::string* why)
    {
        if (!files::rename_to(partial_, out_, false, why)) {
            return false;
        }
        partial_.clear();
        bool flushed = false;
        try {
            flushed = flush_folder_of(out_, why);
        } catch (const std::bad_alloc&) {
            (void)drop(out_);
            throw;
        }
        if (!flushed) {
            (void)drop(out_);
        }
        return flushed;
    }

private:
    std::string out_;
    std::string partial_;
    FILE* file_ = nullptr;
};

/* How many runs of the bytes fed read as a VIN: the scrub's finder in
 * its masking mode over a scratch copy, its masks counted and dropped */
class vin_runs
{
public:
    vin_runs() : buf_(ecuio_scrub::kMaskReach + kPiece) {}

    /* At most kPiece bytes */
    void feed(const uint8_t* p, size_t n)
    {
        std::memcpy(buf_.data() + held_, p, n);
        check_.feed_masking(buf_.data() + held_, n, held_);
        const size_t all = held_ + n;
        const size_t keep = (std::min)(all, ecuio_scrub::kMaskReach);
        std::memmove(buf_.data(), buf_.data() + (all - keep), keep);
        held_ = keep;
    }
    uint64_t finish()
    {
        (void)check_.finish_masking(buf_.data() + held_, held_);
        return check_.masked();
    }

private:
    ecuio_scrub::values none_;
    ecuio_scrub::check check_{none_, ecuio_scrub::kMask};
    std::vector<uint8_t> buf_;
    size_t held_ = 0;
};

ecuio_status_t refused(std::string* why, std::string words)
{
    *why = std::move(words);
    return ECUIO_ERR_LOAD_FAILED;
}

/* A file being written: what a refusal calls it, who is asked whether to
 * go on, and the piece its bytes pass through */
struct writing {
    const char* what; /* "the copy" or "the export" */
    ecuio_cancel_t cancel;
    void* user;
    uint8_t* piece; /* kPiece bytes */

    bool canceled() const { return cancel != nullptr && cancel(user) != 0; }
};

/* The file at path read whole and held to want, a cancel asked for each
 * piece */
ecuio_status_t read_back(const std::string& path, const uint8_t want[32],
                         const writing& w, std::string* why)
{
    std::string words;
    const std::unique_ptr<FILE, int (*)(FILE*)> f(
        files::open_file(path, false, &words), std::fclose);
    if (f == nullptr) {
        return refused(why,
                       std::string(w.what) + " can't be read back: " + words);
    }
    ecuio_sha256::hasher hash;
    size_t n = 0;
    do {
        if (w.canceled()) {
            return ECUIO_ERR_CANCELED;
        }
        n = std::fread(w.piece, 1, kPiece, f.get());
        hash.add(w.piece, n);
    } while (n == kPiece);
    uint8_t sum[32];
    hash.finish(sum);
    if (std::ferror(f.get()) != 0 || std::memcmp(sum, want, 32) != 0) {
        return refused(why, std::string(w.what) +
                                " didn't read back as it was written");
    }
    return ECUIO_OK;
}

/* A written file's last steps, a copy's and a range's alike: flushed to
 * the disk and closed, read back and held to want, then renamed into
 * place (read-only when a copy is kept there) and its folder flushed */
ecuio_status_t seal(whole_file& file, const uint8_t want[32], bool read_only,
                    const writing& w, std::string* why)
{
    const std::string the(w.what);
    if (!file.close()) {
        return refused(why, the + " can't be flushed to the disk");
    }
    if (void (*hook)(const char*) =
            ecuio_dump_handle::test_before_read_back.exchange(nullptr)) {
        hook(file.partial().c_str());
    }
    const ecuio_status_t back = read_back(file.partial(), want, w, why);
    if (back != ECUIO_OK) {
        return back;
    }
    std::string words;
    if (read_only &&
        !ecuio_dump_handle::set_read_only(file.partial(), &words)) {
        return refused(why, the + " can't be made read-only: " + words);
    }
    if (!file.place(&words)) {
        return refused(why, the + " can't be put in place: " + words);
    }
    return ECUIO_OK;
}

/* bytes of d's file from offset into file, the caller holding d.m; each
 * piece is hashed into sum and handed to each */
template <typename Each>
ecuio_status_t copy_bytes(ecuio_dump& d, uint64_t offset, uint64_t bytes,
                          whole_file& file, const writing& w,
                          ecuio_sha256::hasher& sum, Each each,
                          std::string* why)
{
    if (!ecuio_frames_csv::seek_to(d.file, offset)) {
        return refused(why, "the file no longer reads");
    }
    for (uint64_t at = 0; at < bytes;) {
        if (w.canceled()) {
            return ECUIO_ERR_CANCELED;
        }
        const size_t n =
            static_cast<size_t>((std::min)(uint64_t{kPiece}, bytes - at));
        if (std::fread(w.piece, 1, n, d.file) != n) {
            return refused(why, "the file no longer reads");
        }
        if (!file.write(w.piece, n)) {
            return refused(why, std::string(w.what) + " can't be written: " +
                                    files::errno_words(errno));
        }
        sum.add(w.piece, n);
        each(w.piece, n);
        at += n;
    }
    return ECUIO_OK;
}

/* d's whole file copied to out, the caller holding d.m: the bytes held
 * to want as they are written and again as they read back */
ecuio_status_t copy_whole(ecuio_dump& d, const std::string& out,
                          const uint8_t want[32], ecuio_cancel_t cancel,
                          void* user, bool read_only, std::string* why)
{
    const std::unique_ptr<uint8_t[]> piece(new (std::nothrow) uint8_t[kPiece]);
    if (piece == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    const writing w{"the copy", cancel, user, piece.get()};
    whole_file file;
    std::string words;
    if (!file.begin(out, &words)) {
        return refused(why, "the copy can't be written: " + words);
    }
    ecuio_sha256::hasher hash;
    const ecuio_status_t s = copy_bytes(
        d, 0, d.file_bytes, file, w, hash, [](const uint8_t*, size_t) {}, why);
    if (s != ECUIO_OK) {
        return s;
    }
    uint8_t sum[32];
    hash.finish(sum);
    if (std::memcmp(sum, want, 32) != 0) {
        return refused(why, "the file changed since it was checked");
    }
    return seal(file, want, read_only, w, why);
}

/* The files an image export has put in place: removed again unless the
 * whole export is kept */
struct placed_files {
    std::vector<std::string> paths;
    bool keep = false;

    placed_files() = default;
    ~placed_files() { (void)undo(); }
    placed_files(const placed_files&) = delete;
    placed_files& operator=(const placed_files&) = delete;

    /* How many couldn't be removed (a file another program holds open) */
    uint32_t undo() noexcept
    {
        uint32_t left = 0;
        if (!keep) {
            for (const std::string& p : paths) {
                left += drop(p) ? 0u : 1u;
            }
            paths.clear();
        }
        return left;
    }
};

/* <yyyymmdd>-<tx>-<first 12 hex of the copy's checksum>-<address>.bin */
std::string image_name(const ecuio_dump& d, const uint8_t file_sum[32],
                       uint64_t address)
{
    const std::string copy =
        ecuio_dump_file::name_of(d.r.utc, d.r.tx_id, file_sum);
    constexpr size_t hash_and_suffix = 64 + sizeof ecuio_dump_file::kSuffix - 1;
    char tail[16];
    std::snprintf(tail, sizeof tail, "-%08X.bin",
                  static_cast<unsigned>(address));
    return copy.substr(0, copy.size() - hash_and_suffix + 12) + tail;
}

/* One range's bytes from d's file into a file of their own at out, the
 * caller holding d.m: held to their own checksum as they read back, the
 * image's hash and the VIN count fed as they go */
ecuio_status_t write_range(ecuio_dump& d, const ecuio_dump_file::range& g,
                           const std::string& out, const writing& w,
                           ecuio_sha256::hasher& image, uint64_t* runs,
                           std::string* why)
{
    whole_file file;
    std::string words;
    if (!file.begin(out, &words)) {
        return refused(why, "the export can't be written: " + words);
    }
    vin_runs found;
    ecuio_sha256::hasher own;
    const ecuio_status_t s = copy_bytes(
        d, kHeaderBytes + g.offset, g.bytes, file, w, own,
        [&image, &found](const uint8_t* p, size_t n) {
            image.add(p, n);
            found.feed(p, n);
        },
        why);
    if (s != ECUIO_OK) {
        return s;
    }
    uint8_t sum[32];
    own.finish(sum);
    const ecuio_status_t sealed = seal(file, sum, false, w, why);
    if (sealed == ECUIO_OK) {
        *runs += found.finish();
    }
    return sealed;
}

/* Every range written and put in place, in order, and the bytes written
 * held to the header's checksum */
ecuio_status_t write_ranges(ecuio_dump& d, const std::string& folder,
                            const uint8_t file_sum[32], const writing& w,
                            placed_files& placed, uint64_t* runs,
                            std::string* why)
{
    ecuio_sha256::hasher image;
    for (const ecuio_dump_file::range& g : d.r.ranges) {
        std::string out = folder + "/" + image_name(d, file_sum, g.address);
        const ecuio_status_t s = write_range(d, g, out, w, image, runs, why);
        if (s != ECUIO_OK) {
            return s;
        }
        placed.paths.push_back(std::move(out)); /* reserved: no allocation */
    }
    uint8_t sum[32];
    image.finish(sum);
    if (std::memcmp(sum, d.h.image_sha256, 32) != 0) {
        return refused(why, "the file changed since it was checked");
    }
    return ECUIO_OK;
}

/* The image as one file for each range, the caller holding d.m: all of
 * them or none. A failure that can't remove a file it put in place says
 * how many it left, in result->files and a refusal's words. */
ecuio_status_t export_image(ecuio_dump& d, const std::string& folder,
                            const uint8_t file_sum[32], ecuio_cancel_t cancel,
                            void* user, ecuio_dump_export_result_t* result,
                            std::string* why)
{
    const std::unique_ptr<uint8_t[]> piece(new (std::nothrow) uint8_t[kPiece]);
    if (piece == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    const writing w{"the export", cancel, user, piece.get()};
    placed_files placed;
    placed.paths.reserve(d.r.ranges.size());
    uint64_t runs = 0;
    const ecuio_status_t s =
        write_ranges(d, folder, file_sum, w, placed, &runs, why);
    if (s != ECUIO_OK) {
        const uint32_t left = placed.undo();
        if (left != 0 && result != nullptr) {
            result->files = left;
        }
        if (left != 0 && s == ECUIO_ERR_LOAD_FAILED) {
            *why += "; " + std::to_string(left) +
                    " of its files couldn't be removed again";
        }
        return s;
    }
    if (result != nullptr) {
        result->files = static_cast<uint32_t>(placed.paths.size());
        result->vin_runs =
            static_cast<uint32_t>((std::min)(runs, uint64_t{UINT32_MAX}));
    }
    placed.keep = true;
    return ECUIO_OK;
}

/* The whole export, d.m held throughout */
ecuio_status_t export_locked(ecuio_dump& d, uint32_t form,
                             const std::string& folder, ecuio_cancel_t cancel,
                             void* user, ecuio_dump_export_result_t* result,
                             std::string* why)
{
    sums got;
    const ecuio_status_t read = hash_file(d, cancel, user, &got, why);
    if (read != ECUIO_OK) {
        return read;
    }
    if (!got.sound) {
        return refused(why, kDamagedWords);
    }
    if (form == ECUIO_DUMP_EXPORT_IMAGE) {
        return export_image(d, folder, got.file, cancel, user, result, why);
    }
    const std::string out =
        folder + "/" + ecuio_dump_file::name_of(d.r.utc, d.r.tx_id, got.file);
    const ecuio_status_t s =
        copy_whole(d, out, got.file, cancel, user, false, why);
    if (s == ECUIO_OK && result != nullptr) {
        result->files = 1;
    }
    return s;
}

/* A name ecuio_dump_list shows, and nothing a file system reads as more
 * than a name in the folder: no separator, no colon (a drive, or on
 * Windows another file's stream) and no control character */
bool removable_name(const std::string& name)
{
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || u < 0x20 || u == 0x7F) {
            return false;
        }
    }
    return ecuio_dump_handle::listed(name);
}

/* The file at utf8 removed, read-only or not; a failure leaves it as it
 * was. Only a plain file: a folder and a link (whose target's read-only
 * state a remove must never touch) are refused. LOAD_FAILED with *why. */
ecuio_status_t remove_at(const std::string& utf8, std::string* why)
{
#ifdef _WIN32
    const std::wstring w = files::wide(utf8);
    const DWORD was =
        w.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(w.c_str());
    if (was == INVALID_FILE_ATTRIBUTES) {
        return refused(why, "there's no such file");
    }
    if ((was & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
        0) {
        return refused(why, "it isn't a plain file");
    }
    if ((was & FILE_ATTRIBUTE_READONLY) != 0) {
        const DWORD rest = was & ~DWORD{FILE_ATTRIBUTE_READONLY};
        SetFileAttributesW(w.c_str(), rest != 0 ? rest : FILE_ATTRIBUTE_NORMAL);
    }
    if (DeleteFileW(w.c_str())) {
        return ECUIO_OK;
    }
    const DWORD code = GetLastError();
    SetFileAttributesW(w.c_str(), was);
    return refused(why, "the file can't be removed: " + files::os_words(code));
#else
    struct stat st{};
    if (lstat(utf8.c_str(), &st) != 0) {
        return refused(why, errno == ENOENT
                                ? std::string("there's no such file")
                                : "the file can't be removed: " +
                                      files::os_words(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return refused(why, "it isn't a plain file");
    }
    if (unlink(utf8.c_str()) == 0) {
        return ECUIO_OK;
    }
    return refused(why, "the file can't be removed: " + files::os_words(errno));
#endif
}

} // namespace

namespace ecuio_dump_handle {

bool set_read_only(const std::string& utf8, std::string* why)
{
#ifdef _WIN32
    const std::wstring w = files::wide(utf8);
    if (!w.empty() && SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_READONLY)) {
        return true;
    }
    *why = files::os_words(GetLastError());
    return false;
#else
    if (chmod(utf8.c_str(), S_IRUSR | S_IRGRP | S_IROTH) == 0) {
        return true;
    }
    *why = files::os_words(errno);
    return false;
#endif
}

} // namespace ecuio_dump_handle

extern "C" {

ecuio_status_t ecuio_dump_export(const ecuio_dump_t* dump, uint32_t form,
                                 const char* folder_utf8, ecuio_cancel_t cancel,
                                 void* user_data,
                                 ecuio_dump_export_result_t* result)
{
    if (dump == nullptr || folder_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if ((form != ECUIO_DUMP_EXPORT_RESTORE &&
         form != ECUIO_DUMP_EXPORT_IMAGE) ||
        (result != nullptr && result->struct_size < sizeof *result)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    if (result != nullptr) {
        std::memset(result, 0, sizeof *result);
        result->struct_size = sizeof *result;
    }
    auto* d = const_cast<ecuio_dump*>(dump);
    try {
        std::string why;
        ecuio_status_t s = ECUIO_OK;
        {
            const std::lock_guard<std::mutex> lock(d->m);
            s = export_locked(*d, form, folder_utf8, cancel, user_data, result,
                              &why);
        }
        if (s == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("%s", why.c_str());
        }
        return s;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_dump_import(const char* path_utf8, const char* folder_utf8,
                                 char* name_out, size_t name_capacity)
{
    if (path_utf8 == nullptr || folder_utf8 == nullptr ||
        (name_out == nullptr && name_capacity != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (name_capacity != 0 && name_capacity < ECUIO_DUMP_NAME_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (name_out != nullptr) {
        name_out[0] = '\0';
    }
    ecuio_detail_clear();
    try {
        std::unique_ptr<ecuio_dump> d;
        std::string why;
        ecuio_status_t s = open_at(path_utf8, &d, &why);
        sums got;
        if (s == ECUIO_OK) {
            const std::lock_guard<std::mutex> lock(d->m);
            s = hash_file(*d, nullptr, nullptr, &got, &why);
            if (s == ECUIO_OK && !got.sound) {
                s = refused(&why, kDamagedWords);
            }
            if (s == ECUIO_OK) {
                const std::string name =
                    ecuio_dump_file::name_of(d->r.utc, d->r.tx_id, got.file);
                if (name_out != nullptr) {
                    copy_text(name_out, name_capacity, name);
                }
                s = copy_whole(*d, std::string(folder_utf8) + "/" + name,
                               got.file, nullptr, nullptr, true, &why);
            }
        }
        if (s == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("%s", why.c_str());
        }
        return s;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_dump_remove(const char* folder_utf8, const char* name)
{
    if (folder_utf8 == nullptr || name == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_detail_clear();
    try {
        const std::string leaf(name);
        if (!removable_name(leaf)) {
            ecuio_detail_set("the name isn't a dump file's");
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::string why;
        const ecuio_status_t s =
            remove_at(std::string(folder_utf8) + "/" + leaf, &why);
        if (s == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("%s", why.c_str());
        }
        return s;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
