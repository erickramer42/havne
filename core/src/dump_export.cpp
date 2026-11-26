// File/layer: core/src/dump_export.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: DumpExportTest (test_core)
//
// A copy leaving its folder and coming back (ecuio/dump.h): the export's
// two forms, the import, and the one call that removes a file. Each file
// is written whole under a partial name, then renamed without replacing.
// Words name no path.

#include "dump_handle.h"

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"
#include "scrub.h"
#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifndef _WIN32
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

/* A file of this call's own removed, read-only or not; throws nothing */
void drop(const std::string& utf8) noexcept
{
    try {
#ifdef _WIN32
        const std::wstring w = files::wide(utf8);
        if (!w.empty()) {
            SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(w.c_str());
        }
#else
        unlink(utf8.c_str());
#endif
    } catch (const std::bad_alloc&) {
        (void)utf8; /* left: nothing more can be done without memory */
    }
}

/* A file written whole: made under a partial name, put in place by a
 * rename that never replaces, and removed on any way out before that */
class whole_file
{
public:
    whole_file() = default;
    ~whole_file()
    {
        (void)close();
        if (!partial_.empty()) {
            drop(partial_);
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
    bool close()
    {
        if (file_ == nullptr) {
            return true;
        }
        FILE* f = file_;
        file_ = nullptr;
        const bool flushed = std::fflush(f) == 0;
        return std::fclose(f) == 0 && flushed;
    }
    const std::string& partial() const { return partial_; }
    /* Once true the file is at its name, and no longer removed here */
    bool place(std::string* why)
    {
        if (!files::rename_to(partial_, out_, false, why)) {
            return false;
        }
        partial_.clear();
        return true;
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

/* The file at path read whole and held to want, a cancel asked for each
 * piece */
ecuio_status_t read_back(const std::string& path, const uint8_t want[32],
                         ecuio_cancel_t cancel, void* user, uint8_t* piece,
                         std::string* why)
{
    std::string words;
    const std::unique_ptr<FILE, int (*)(FILE*)> f(
        files::open_file(path, false, &words), std::fclose);
    if (f == nullptr) {
        return refused(why, "the copy can't be read back: " + words);
    }
    ecuio_sha256::hasher hash;
    size_t n = 0;
    do {
        if (cancel != nullptr && cancel(user) != 0) {
            return ECUIO_ERR_CANCELED;
        }
        n = std::fread(piece, 1, kPiece, f.get());
        hash.add(piece, n);
    } while (n == kPiece);
    uint8_t sum[32];
    hash.finish(sum);
    if (std::ferror(f.get()) != 0 || std::memcmp(sum, want, 32) != 0) {
        return refused(why, "the copy didn't read back as it was written");
    }
    return ECUIO_OK;
}

/* d's whole file copied to out, the caller holding d.m: the bytes held
 * to want as they are written and again as the disk gives them back,
 * then renamed into place, read-only when a copy is kept there */
ecuio_status_t copy_whole(ecuio_dump& d, const std::string& out,
                          const uint8_t want[32], ecuio_cancel_t cancel,
                          void* user, bool read_only, std::string* why)
{
    const std::unique_ptr<uint8_t[]> piece(new (std::nothrow) uint8_t[kPiece]);
    if (piece == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    whole_file file;
    std::string words;
    if (!file.begin(out, &words)) {
        return refused(why, "the copy can't be written: " + words);
    }
    if (!ecuio_frames_csv::seek_to(d.file, 0)) {
        return refused(why, "the file no longer reads");
    }
    ecuio_sha256::hasher hash;
    for (uint64_t at = 0; at < d.file_bytes;) {
        if (cancel != nullptr && cancel(user) != 0) {
            return ECUIO_ERR_CANCELED;
        }
        const size_t want_n = static_cast<size_t>(
            (std::min)(uint64_t{kPiece}, d.file_bytes - at));
        if (std::fread(piece.get(), 1, want_n, d.file) != want_n) {
            return refused(why, "the file no longer reads");
        }
        if (!file.write(piece.get(), want_n)) {
            return refused(why, "the copy can't be written: " +
                                    files::errno_words(errno));
        }
        hash.add(piece.get(), want_n);
        at += want_n;
    }
    uint8_t sum[32];
    hash.finish(sum);
    if (std::memcmp(sum, want, 32) != 0) {
        return refused(why, "the file changed since it was checked");
    }
    if (!file.close()) {
        return refused(why, "the copy can't be written: " +
                                files::errno_words(errno));
    }
    const ecuio_status_t back =
        read_back(file.partial(), want, cancel, user, piece.get(), why);
    if (back != ECUIO_OK) {
        return back;
    }
    if (read_only &&
        !ecuio_dump_handle::set_read_only(file.partial(), &words)) {
        return refused(why, "the copy can't be made read-only: " + words);
    }
    if (!file.place(&words)) {
        return refused(why, "the copy can't be put in place: " + words);
    }
    return ECUIO_OK;
}

/* The files an image export has put in place: removed again unless the
 * whole export is kept */
struct placed_files {
    std::vector<std::string> paths;
    bool keep = false;

    placed_files() = default;
    ~placed_files()
    {
        if (!keep) {
            for (const std::string& p : paths) {
                drop(p);
            }
        }
    }
    placed_files(const placed_files&) = delete;
    placed_files& operator=(const placed_files&) = delete;
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
 * caller holding d.m; the image's hash and the VIN count fed as it goes */
ecuio_status_t write_range(ecuio_dump& d, const ecuio_dump_file::range& g,
                           const std::string& out, ecuio_cancel_t cancel,
                           void* user, uint8_t* piece,
                           ecuio_sha256::hasher& image, uint64_t* runs,
                           std::string* why)
{
    whole_file file;
    std::string words;
    if (!file.begin(out, &words)) {
        return refused(why, "the export can't be written: " + words);
    }
    if (!ecuio_frames_csv::seek_to(d.file, kHeaderBytes + g.offset)) {
        return refused(why, "the file no longer reads");
    }
    vin_runs found;
    for (uint64_t at = 0; at < g.bytes;) {
        if (cancel != nullptr && cancel(user) != 0) {
            return ECUIO_ERR_CANCELED;
        }
        const size_t n =
            static_cast<size_t>((std::min)(uint64_t{kPiece}, g.bytes - at));
        if (std::fread(piece, 1, n, d.file) != n) {
            return refused(why, "the file no longer reads");
        }
        if (!file.write(piece, n)) {
            return refused(why, "the export can't be written: " +
                                    files::errno_words(errno));
        }
        image.add(piece, n);
        found.feed(piece, n);
        at += n;
    }
    if (!file.close()) {
        return refused(why, "the export can't be written: " +
                                files::errno_words(errno));
    }
    if (!file.place(&words)) {
        return refused(why, "the export can't be put in place: " + words);
    }
    *runs += found.finish();
    return ECUIO_OK;
}

/* The image as one file for each range, the caller holding d.m: all of
 * them or none, and the bytes written held to the header's checksum */
ecuio_status_t export_image(ecuio_dump& d, const std::string& folder,
                            const uint8_t file_sum[32], ecuio_cancel_t cancel,
                            void* user, ecuio_dump_export_result_t* result,
                            std::string* why)
{
    const std::unique_ptr<uint8_t[]> piece(new (std::nothrow) uint8_t[kPiece]);
    if (piece == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    placed_files placed;
    placed.paths.reserve(d.r.ranges.size());
    ecuio_sha256::hasher image;
    uint64_t runs = 0;
    for (const ecuio_dump_file::range& g : d.r.ranges) {
        std::string out = folder + "/" + image_name(d, file_sum, g.address);
        const ecuio_status_t s = write_range(d, g, out, cancel, user,
                                             piece.get(), image, &runs, why);
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
    placed.keep = true;
    if (result != nullptr) {
        result->files = static_cast<uint32_t>(placed.paths.size());
        result->vin_runs =
            static_cast<uint32_t>((std::min)(runs, uint64_t{UINT32_MAX}));
    }
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

/* A name ecuio_dump_list shows: a copy's suffix or a partial's, and no
 * way out of the folder */
bool removable_name(const std::string& name)
{
    return name.find_first_of("/\\") == std::string::npos &&
           ecuio_dump_handle::listed(name);
}

/* The file at utf8 removed, read-only or not; a failure leaves it as it
 * was. LOAD_FAILED with *why. */
ecuio_status_t remove_at(const std::string& utf8, std::string* why)
{
#ifdef _WIN32
    const std::wstring w = files::wide(utf8);
    const DWORD was =
        w.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(w.c_str());
    if (was == INVALID_FILE_ATTRIBUTES) {
        return refused(why, "there's no such file");
    }
    if ((was & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return refused(why, "it isn't a file");
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
    if (S_ISDIR(st.st_mode)) {
        return refused(why, "it isn't a file");
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
