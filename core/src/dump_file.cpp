// File/layer: core/src/dump_file.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: DumpFileTest (test_core); FUZZ: fuzz_dump_file
//
// A dump file on the disk (ecuio/dump.h): opened to read, its structure
// and record judged by dump_file.h's calls, its image read as a stream.
// No call here opens a copy for writing. Words name no path.

#include "dump_handle.h"

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"
#include "sha256.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using ecuio_dump_file::judged;
using ecuio_dump_file::kHeaderBytes;
using ecuio_dump_file::record;

namespace {

std::string leaf_of(const std::string& path)
{
    const size_t at = path.find_last_of("/\\");
    return at == std::string::npos ? path : path.substr(at + 1);
}

ecuio_status_t status_of(judged j)
{
    return j == judged::ok              ? ECUIO_OK
           : j == judged::out_of_memory ? ECUIO_ERR_OUT_OF_MEMORY
                                        : ECUIO_ERR_LOAD_FAILED;
}

} // namespace

namespace ecuio_dump_handle {

void copy_text(char* dst, size_t cap, const std::string& s)
{
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

ecuio_status_t open_at(const std::string& path,
                       std::unique_ptr<ecuio_dump>* out, std::string* why,
                       uint64_t* file_bytes)
{
    std::unique_ptr<ecuio_dump> d(new (std::nothrow) ecuio_dump);
    if (d == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    std::string words;
    d->file = ecuio_files::open_file(path, false, &words);
    if (d->file == nullptr) {
        *why = "the file can't be opened: " + words;
        return ECUIO_ERR_LOAD_FAILED;
    }
    /* Unbuffered: a later read is the disk's bytes, never an earlier
     * read's kept in a buffer (glibc keeps one across a seek inside it) */
    std::setvbuf(d->file, nullptr, _IONBF, 0);
    d->file_bytes = ecuio_frames_csv::file_size(d->file);
    if (file_bytes != nullptr) {
        *file_bytes = d->file_bytes;
    }
    if (d->file_bytes < kHeaderBytes) {
        *why = "it is shorter than a dump's header";
        return ECUIO_ERR_LOAD_FAILED;
    }
    uint8_t head[kHeaderBytes];
    if (std::fread(head, 1, sizeof head, d->file) != sizeof head) {
        *why = "its header can't be read";
        return ECUIO_ERR_LOAD_FAILED;
    }
    judged j = ecuio_dump_file::read_header(head, d->file_bytes, &d->h, why);
    if (j != judged::ok) {
        return status_of(j);
    }
    /* The record's length is judged: at most ECUIO_DUMP_RECORD_MAX */
    std::string text(d->h.record_bytes, '\0');
    if (!ecuio_frames_csv::seek_to(d->file, kHeaderBytes + d->h.image_bytes) ||
        std::fread(&text[0], 1, text.size(), d->file) != text.size()) {
        *why = "its record can't be read";
        return ECUIO_ERR_LOAD_FAILED;
    }
    j = ecuio_dump_file::read_record(text.data(), text.size(), d->h, &d->r,
                                     why);
    if (j != judged::ok) {
        return status_of(j);
    }
    d->name = leaf_of(path);
    *out = std::move(d);
    return ECUIO_OK;
}

ecuio_status_t hash_file(ecuio_dump& d, ecuio_cancel_t cancel, void* user,
                         sums* out, std::string* why)
{
    std::unique_ptr<uint8_t[]> piece(new (std::nothrow) uint8_t[kPiece]);
    if (piece == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (ecuio_frames_csv::file_size(d.file) != d.file_bytes) {
        *why = "the file changed since it was opened";
        return ECUIO_ERR_LOAD_FAILED;
    }
    ecuio_sha256::hasher whole;
    ecuio_sha256::hasher image;
    ecuio_sha256::hasher text;
    const uint64_t image_end = kHeaderBytes + d.h.image_bytes;
    uint64_t at = 0;
    while (at < d.file_bytes) {
        if (cancel != nullptr && cancel(user) != 0) {
            return ECUIO_ERR_CANCELED;
        }
        const size_t want = static_cast<size_t>(
            (std::min)(uint64_t{kPiece}, d.file_bytes - at));
        if (std::fread(piece.get(), 1, want, d.file) != want) {
            *why = "the file no longer reads";
            return ECUIO_ERR_LOAD_FAILED;
        }
        whole.add(piece.get(), want);
        /* The piece's part in the header, the image and the record */
        const uint64_t end = at + want;
        if (at < kHeaderBytes) {
            uint8_t head[kHeaderBytes];
            ecuio_dump_file::write_header(d.h, head);
            if (std::memcmp(piece.get(), head, kHeaderBytes) != 0) {
                *why = "the file changed since it was opened";
                return ECUIO_ERR_LOAD_FAILED;
            }
        }
        const uint64_t from = (std::max)(at, uint64_t{kHeaderBytes});
        if (from < (std::min)(end, image_end)) {
            image.add(piece.get() + (from - at),
                      static_cast<size_t>((std::min)(end, image_end) - from));
        }
        if (end > image_end) {
            const uint64_t first = (std::max)(at, image_end);
            text.add(piece.get() + (first - at),
                     static_cast<size_t>(end - first));
        }
        at = end;
    }
    /* A file longer than when the read began was written to meanwhile */
    if (ecuio_frames_csv::file_size(d.file) != d.file_bytes) {
        *why = "the file changed since it was opened";
        return ECUIO_ERR_LOAD_FAILED;
    }
    uint8_t sum[32];
    image.finish(sum);
    out->sound = std::memcmp(sum, d.h.image_sha256, 32) == 0;
    text.finish(sum);
    out->sound = out->sound && std::memcmp(sum, d.h.record_sha256, 32) == 0;
    whole.finish(out->file);
    return ECUIO_OK;
}

bool listed(const std::string& name)
{
    constexpr size_t suffix = sizeof ecuio_dump_file::kSuffix - 1;
    return ecuio_dump_file::partial_name(name) ||
           (name.size() > suffix &&
            name.compare(name.size() - suffix, suffix,
                         ecuio_dump_file::kSuffix) == 0);
}

} // namespace ecuio_dump_handle

namespace {

using ecuio_dump_handle::copy_text;
using ecuio_dump_handle::hash_file;
using ecuio_dump_handle::listed;
using ecuio_dump_handle::open_at;
using ecuio_dump_handle::sums;

template <typename T> ecuio_status_t sized(const ecuio_dump* d, const T* out)
{
    if (d == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    return out->struct_size < sizeof(T) ? ECUIO_ERR_INVALID_ARGUMENT : ECUIO_OK;
}

void fill_info(const ecuio_dump& d, ecuio_dump_info_t* out)
{
    const record& r = d.r;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->flags = (r.has_volts ? ECUIO_DUMP_HAS_VOLTS : 0u) |
                 (r.has_volts_min ? ECUIO_DUMP_HAS_VOLTS_MIN : 0u) |
                 (r.has_rpm ? ECUIO_DUMP_HAS_RPM : 0u) |
                 (r.has_kmh ? ECUIO_DUMP_HAS_KMH : 0u) |
                 (r.parked_tick ? ECUIO_DUMP_PARKED_TICK : 0u) |
                 (!r.vin.empty() || !r.calids.empty() || !r.cvns.empty() ||
                          !r.ecu_name.empty()
                      ? ECUIO_DUMP_HAS_OBD
                      : 0u);
    out->format = d.h.format;
    out->kind = r.kind;
    out->image_bytes = d.h.image_bytes;
    out->utc = static_cast<uint64_t>(r.utc);
    std::memcpy(out->image_sha256, d.h.image_sha256, 32);
    std::memcpy(out->record_sha256, d.h.record_sha256, 32);
    out->rx_id = r.rx_id;
    out->tx_id = r.tx_id;
    out->range_count = static_cast<uint32_t>(r.ranges.size());
    out->ident_count = static_cast<uint32_t>(r.idents.size());
    out->calibration_count =
        static_cast<uint32_t>((std::max)(r.calids.size(), r.cvns.size()));
    out->passes = r.passes;
    out->seconds = r.seconds;
    out->volts_mv = r.volts_mv;
    out->volts_min_mv = r.volts_min_mv;
    out->rpm = r.rpm;
    out->kmh = r.kmh;
    out->unlocks = r.unlocks;
    out->writes_before = r.writes_before;
    copy_text(out->core, sizeof out->core, r.core);
    copy_text(out->plugin_id, sizeof out->plugin_id, r.plugin_id);
    copy_text(out->plugin_version, sizeof out->plugin_version,
              r.plugin_version);
    copy_text(out->operation, sizeof out->operation, r.operation);
    std::memcpy(out->module_sha256, r.module_sha256, 32);
    std::memcpy(out->fingerprint, r.fingerprint, 32);
    copy_text(out->scheme, sizeof out->scheme, r.scheme);
    copy_text(out->adapter, sizeof out->adapter, r.adapter);
    copy_text(out->firmware, sizeof out->firmware, r.firmware);
    copy_text(out->vin, sizeof out->vin, r.vin);
    copy_text(out->ecu_name, sizeof out->ecu_name, r.ecu_name);
}

static_assert(
    ecuio_dump_file::kNameMax < sizeof(ecuio_dump_range_t{}.name) &&
        ecuio_dump_file::kNameMax < sizeof(ecuio_dump_info_t{}.operation) &&
        ecuio_dump_file::kTextMax < sizeof(ecuio_dump_info_t{}.adapter) &&
        ecuio_dump_file::kSchemeMax < sizeof(ecuio_dump_info_t{}.scheme) &&
        ecuio_dump_file::kCalidMax < sizeof(ecuio_dump_calibration_t{}.calid) &&
        ecuio_dump_file::kEcuNameMax < sizeof(ecuio_dump_info_t{}.ecu_name),
    "each of the record's texts fits its field with its NUL");

/* One listed file's entry: opened as a copy is, the checksums left */
ecuio_status_t entry_of(const std::string& folder, const std::string& name,
                        ecuio_dump_entry_t* e)
{
    std::memset(e, 0, sizeof *e);
    if (name.size() >= sizeof e->name) {
        /* cut between characters, so the name stays UTF-8 */
        size_t n = sizeof e->name - 1;
        while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) {
            --n;
        }
        e->flags |= ECUIO_DUMP_ENTRY_NAME_CUT;
        copy_text(e->name, n + 1, name);
    } else {
        copy_text(e->name, sizeof e->name, name);
    }
    std::unique_ptr<ecuio_dump> d;
    std::string why;
    uint64_t bytes = 0;
    const ecuio_status_t opened =
        open_at(folder + "/" + name, &d, &why, &bytes);
    e->file_bytes = bytes;
    if (opened == ECUIO_ERR_OUT_OF_MEMORY) {
        return opened;
    }
    if (ecuio_dump_file::partial_name(name)) {
        e->state = ECUIO_DUMP_STATE_UNFINISHED;
        copy_text(e->words, sizeof e->words,
                  "a write that didn't finish left it");
        return ECUIO_OK;
    }
    if (opened != ECUIO_OK) {
        e->state = ECUIO_DUMP_STATE_INVALID;
        copy_text(e->words, sizeof e->words, why);
        return ECUIO_OK;
    }
    e->kind = d->r.kind;
    e->writes_before = d->r.writes_before;
    e->utc = static_cast<uint64_t>(d->r.utc);
    e->image_bytes = d->h.image_bytes;
    e->rx_id = d->r.rx_id;
    e->tx_id = d->r.tx_id;
    uint8_t hash[32];
    if (ecuio_dump_file::name_hash(name, hash)) {
        e->state = ECUIO_DUMP_STATE_COPY;
    } else {
        e->state = ECUIO_DUMP_STATE_INVALID;
        copy_text(e->words, sizeof e->words,
                  "it reads as a copy, but its name isn't a copy's");
    }
    return ECUIO_OK;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_dump_open(const char* path_utf8, ecuio_dump_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (path_utf8 == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_detail_clear();
    try {
        std::unique_ptr<ecuio_dump> d;
        std::string why;
        const ecuio_status_t s = open_at(path_utf8, &d, &why);
        if (s == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("%s", why.c_str());
        }
        if (s == ECUIO_OK) {
            *out = d.release();
        }
        return s;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void ecuio_dump_close(ecuio_dump_t* dump)
{
    delete dump;
}

ecuio_status_t ecuio_dump_get_info(const ecuio_dump_t* dump,
                                   ecuio_dump_info_t* out)
{
    const ecuio_status_t s = sized(dump, out);
    if (s == ECUIO_OK) {
        fill_info(*dump, out);
    }
    return s;
}

ecuio_status_t ecuio_dump_get_range(const ecuio_dump_t* dump, uint32_t index,
                                    ecuio_dump_range_t* out)
{
    const ecuio_status_t s = sized(dump, out);
    if (s != ECUIO_OK) {
        return s;
    }
    if (index >= dump->r.ranges.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_dump_file::range& g = dump->r.ranges[index];
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->step = g.step;
    out->session = g.session;
    out->address = g.address;
    out->bytes = g.bytes;
    out->offset = g.offset;
    copy_text(out->name, sizeof out->name, g.name);
    return ECUIO_OK;
}

ecuio_status_t ecuio_dump_get_ident(const ecuio_dump_t* dump, uint32_t index,
                                    ecuio_dump_ident_t* out)
{
    const ecuio_status_t s = sized(dump, out);
    if (s != ECUIO_OK) {
        return s;
    }
    if (index >= dump->r.idents.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_dump_file::ident& d = dump->r.idents[index];
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->did = d.did;
    out->role = d.role;
    out->length = static_cast<uint32_t>(d.value.size());
    std::memcpy(out->value, d.value.data(), d.value.size());
    return ECUIO_OK;
}

ecuio_status_t ecuio_dump_get_calibration(const ecuio_dump_t* dump,
                                          uint32_t index,
                                          ecuio_dump_calibration_t* out)
{
    const ecuio_status_t s = sized(dump, out);
    if (s != ECUIO_OK) {
        return s;
    }
    const record& r = dump->r;
    if (index >= (std::max)(r.calids.size(), r.cvns.size())) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    if (index < r.calids.size()) {
        out->flags |= ECUIO_DUMP_HAS_CALID;
        copy_text(out->calid, sizeof out->calid, r.calids[index]);
    }
    if (index < r.cvns.size()) {
        out->flags |= ECUIO_DUMP_HAS_CVN;
        std::memcpy(out->cvn, r.cvns[index].data(), 4);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_dump_read(const ecuio_dump_t* dump, uint64_t offset,
                               uint8_t* out, size_t capacity, size_t* count)
{
    if (dump == nullptr || out == nullptr || count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    if (offset >= dump->h.image_bytes) {
        return ECUIO_OK;
    }
    auto* d = const_cast<ecuio_dump*>(dump);
    const size_t want = static_cast<size_t>(
        (std::min)(uint64_t{capacity}, dump->h.image_bytes - offset));
    const std::lock_guard<std::mutex> lock(d->m);
    if (!ecuio_frames_csv::seek_to(d->file, kHeaderBytes + offset) ||
        std::fread(out, 1, want, d->file) != want) {
        ecuio_detail_set("the file no longer reads");
        return ECUIO_ERR_LOAD_FAILED;
    }
    *count = want;
    return ECUIO_OK;
}

ecuio_status_t ecuio_dump_verify(const ecuio_dump_t* dump,
                                 ecuio_cancel_t cancel, void* user_data,
                                 char* name_out, size_t name_capacity)
{
    if (dump == nullptr || (name_out == nullptr && name_capacity != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (name_capacity != 0 && name_capacity < ECUIO_DUMP_NAME_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (name_out != nullptr) {
        name_out[0] = '\0';
    }
    ecuio_detail_clear();
    auto* d = const_cast<ecuio_dump*>(dump);
    try {
        sums got;
        std::string why;
        ecuio_status_t s = ECUIO_OK;
        {
            const std::lock_guard<std::mutex> lock(d->m);
            s = hash_file(*d, cancel, user_data, &got, &why);
        }
        if (s == ECUIO_OK && !got.sound) {
            why = ecuio_dump_handle::kDamagedWords;
            s = ECUIO_ERR_LOAD_FAILED;
        }
        if (s == ECUIO_OK) {
            const std::string name =
                ecuio_dump_file::name_of(d->r.utc, d->r.tx_id, got.file);
            if (name_out != nullptr) {
                copy_text(name_out, name_capacity, name);
            }
            uint8_t named[32];
            if (!ecuio_dump_file::name_hash(d->name, named) ||
                std::memcmp(named, got.file, 32) != 0) {
                why = "it is sound, but its name isn't its checksum's";
                s = ECUIO_ERR_LOAD_FAILED;
            }
        }
        if (s == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("%s", why.c_str());
        }
        return s;
    } catch (const std::bad_alloc&) {
        if (name_out != nullptr) {
            name_out[0] = '\0';
        }
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_dump_list(const char* folder_utf8, ecuio_dump_entry_t* out,
                               size_t capacity, size_t* count)
{
    if (folder_utf8 == nullptr || count == nullptr ||
        (out == nullptr && capacity != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    ecuio_detail_clear();
    try {
        const std::string folder(folder_utf8);
        std::vector<std::string> names;
        if (!ecuio_files::list_folder(folder, &names)) {
            ecuio_detail_set("the folder can't be read");
            return ECUIO_ERR_LOAD_FAILED;
        }
        names.erase(
            std::remove_if(names.begin(), names.end(),
                           [](const std::string& n) { return !listed(n); }),
            names.end());
        std::sort(names.begin(), names.end());
        for (size_t i = 0; i < names.size() && i < capacity; ++i) {
            const ecuio_status_t s = entry_of(folder, names[i], &out[i]);
            if (s != ECUIO_OK) {
                return s;
            }
        }
        *count = names.size();
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
