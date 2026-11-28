// File/layer: core/src/signal_file_readers.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileFormat3Test, SignalNeedsTest, MergedTextTest
//          (test_signals), c_abi_merge
//
// What a loaded signal file says of how its set is made up (signals.h,
// ADR-0065 section 9): its modules as its "ecus" entries name them, a
// merged set's parts, each def's part and the keys it replaced, the keys
// a file needs from another, and the notes its load left. Each call only
// reads, so any thread may make it while the file lives.

#include "ecuio/signals.h"
#include "error_detail.h"
#include "signal_file_build.h"

#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

std::mutex& files_mutex()
{
    static std::mutex m;
    return m;
}

std::vector<const ecuio_signal_file*>& files()
{
    static std::vector<const ecuio_signal_file*> all;
    return all;
}

} // namespace

void ecuio_sf::keep_file(const ecuio_signal_file* f)
{
    const std::lock_guard<std::mutex> lock(files_mutex());
    files().push_back(f);
}

void ecuio_sf::forget_file(const ecuio_signal_file* f) noexcept
{
    const std::lock_guard<std::mutex> lock(files_mutex());
    std::vector<const ecuio_signal_file*>& all = files();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == f) {
            all[i] = all.back();
            all.pop_back();
            return;
        }
    }
}

const ecuio_signal_file*
ecuio_sf::file_of_set(const ecuio_signal_set_t* set) noexcept
{
    const std::lock_guard<std::mutex> lock(files_mutex());
    for (const ecuio_signal_file* f : files()) {
        if (&f->set == set) {
            return f;
        }
    }
    return nullptr;
}

namespace {

/* Whether a struct_size holds a struct of size bytes; the detail names
 * the struct as what when it doesn't */
bool sized(uint32_t struct_size, size_t size, const char* what)
{
    if (struct_size >= size) {
        return true;
    }
    ecuio_detail_set("the %s's struct_size is %u, under its %u bytes", what,
                     static_cast<unsigned>(struct_size),
                     static_cast<unsigned>(size));
    return false;
}

/* The part at index, or NULL with the detail set: out's size is judged
 * first, as each getter by index does */
const ecuio_signal_file::part* part_at(const ecuio_signal_file_t* file,
                                       uint32_t index, bool has_out,
                                       uint32_t struct_size, size_t size,
                                       ecuio_status_t* status)
{
    ecuio_detail_clear();
    if (file == nullptr || !has_out) {
        ecuio_detail_set("the part needs a file and a struct to fill");
        *status = ECUIO_ERR_NULL_ARGUMENT;
        return nullptr;
    }
    *status = ECUIO_ERR_INVALID_ARGUMENT;
    if (!sized(struct_size, size, "part")) {
        return nullptr;
    }
    if (index >= file->parts.size()) {
        ecuio_detail_set("the file has %u parts, and index %u is none",
                         static_cast<unsigned>(file->parts.size()),
                         static_cast<unsigned>(index));
        return nullptr;
    }
    *status = ECUIO_OK;
    return &file->parts[index];
}

/* def's index among the file's defs, or the count for one not the file's */
size_t def_index(const ecuio_signal_file_t* file, const ecuio_signal_def_t* def)
{
    const ecuio_signal_def_t* first = file->defs.data();
    return def >= first && def < first + file->defs.size()
               ? static_cast<size_t>(def - first)
               : file->defs.size();
}

} // namespace

extern "C" {

uint32_t ecuio_signal_file_part_count(const ecuio_signal_file_t* file)
{
    return file != nullptr ? static_cast<uint32_t>(file->parts.size()) : 0;
}

ecuio_status_t ecuio_signal_file_part(const ecuio_signal_file_t* file,
                                      uint32_t index, ecuio_signal_part_t* out)
{
    ecuio_status_t status = ECUIO_OK;
    const ecuio_signal_file::part* p = part_at(
        file, index, out != nullptr, out != nullptr ? out->struct_size : 0,
        sizeof(ecuio_signal_part_t), &status);
    if (p == nullptr) {
        return status;
    }
    ecuio_signal_part_t filled{};
    filled.struct_size = sizeof(ecuio_signal_part_t);
    filled.flags = p->has_plugin ? ECUIO_PART_PLUGIN : 0u;
    filled.format = p->format;
    /* Its signals in the set, and the modules they read */
    uint32_t modules[ECUIO_MAX_CHANNELS] = {};
    for (size_t d = 0; d < file->def_part.size(); ++d) {
        if (file->def_part[d] != index) {
            continue;
        }
        ++filled.signal_count;
        if ((file->defs[d].flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue;
        }
        uint32_t m = 0;
        while (m < filled.module_count &&
               modules[m] != file->defs[d].message_id) {
            ++m;
        }
        if (m == filled.module_count && m < ECUIO_MAX_CHANNELS) {
            modules[filled.module_count++] = file->defs[d].message_id;
        }
    }
    ecuio_sf::copy_text(filled.profile, sizeof filled.profile, p->profile);
    ecuio_sf::copy_text(filled.name, sizeof filled.name, p->name);
    ecuio_sf::copy_text(filled.plugin_id, sizeof filled.plugin_id,
                        p->plugin_id);
    ecuio_sf::copy_text(filled.plugin_version, sizeof filled.plugin_version,
                        p->plugin_version);
    ecuio_sf::copy_text(filled.plugin_variant, sizeof filled.plugin_variant,
                        p->plugin_variant);
    std::memcpy(out, &filled, sizeof filled);
    return ECUIO_OK;
}

ecuio_status_t ecuio_signal_file_part_source(const ecuio_signal_file_t* file,
                                             uint32_t index,
                                             ecuio_signal_source_t* out)
{
    ecuio_status_t status = ECUIO_OK;
    const ecuio_signal_file::part* p = part_at(
        file, index, out != nullptr, out != nullptr ? out->struct_size : 0,
        sizeof(ecuio_signal_source_t), &status);
    if (p == nullptr) {
        return status;
    }
    std::memcpy(out, &p->source, sizeof p->source);
    out->struct_size = sizeof(ecuio_signal_source_t);
    return ECUIO_OK;
}

uint32_t ecuio_signal_file_def_part(const ecuio_signal_file_t* file,
                                    const ecuio_signal_def_t* def)
{
    if (file == nullptr || def == nullptr) {
        return ECUIO_SIGNAL_PART_NONE;
    }
    const size_t d = def_index(file, def);
    return d < file->def_part.size() ? file->def_part[d]
                                     : ECUIO_SIGNAL_PART_NONE;
}

ecuio_status_t ecuio_signal_file_replaced_keys(const ecuio_signal_file_t* file,
                                               const ecuio_signal_def_t* def,
                                               ecuio_signal_key_t* keys,
                                               uint32_t capacity,
                                               uint32_t* count)
{
    if (file == nullptr || def == nullptr || count == nullptr ||
        (keys == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const size_t d = def_index(file, def);
    uint32_t found = 0;
    for (const auto& k : file->second_keys) {
        if (k.second != d) {
            continue;
        }
        if (found < capacity) {
            ecuio_sf::copy_text(keys[found].text, sizeof keys[found].text,
                                k.first);
        }
        ++found;
    }
    *count = found;
    return ECUIO_OK;
}

ecuio_status_t ecuio_signal_file_module(const ecuio_signal_file_t* file,
                                        uint32_t index,
                                        ecuio_signal_module_t* out)
{
    ecuio_detail_clear();
    if (file == nullptr || out == nullptr) {
        ecuio_detail_set("the module needs a file and a struct to fill");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sized(out->struct_size, sizeof(ecuio_signal_module_t), "module")) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (index >= file->channels.size() || index >= file->modules.size()) {
        ecuio_detail_set("the file's set has %u modules, and index %u is none",
                         static_cast<unsigned>(file->channels.size()),
                         static_cast<unsigned>(index));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_signal_file::module& m = file->modules[index];
    ecuio_signal_module_t filled{};
    filled.struct_size = sizeof(ecuio_signal_module_t);
    filled.flags = m.id.empty() ? ECUIO_MODULE_BASE : 0u;
    filled.rx_id = file->channels[index].rx_id;
    filled.tx_id = file->channels[index].tx_id;
    filled.part = m.part;
    filled.session = file->channels[index].session;
    ecuio_sf::copy_text(filled.id, sizeof filled.id, m.id);
    ecuio_sf::copy_text(filled.name, sizeof filled.name, m.name);
    std::memcpy(out, &filled, sizeof filled);
    return ECUIO_OK;
}

ecuio_status_t ecuio_signal_file_needs(const ecuio_signal_file_t* file,
                                       ecuio_signal_key_t* keys,
                                       uint32_t capacity, uint32_t* count)
{
    if (file == nullptr || count == nullptr ||
        (keys == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = static_cast<uint32_t>(file->needs.size());
    for (uint32_t i = 0; i < *count && i < capacity; ++i) {
        ecuio_sf::copy_text(keys[i].text, sizeof keys[i].text, file->needs[i]);
    }
    return ECUIO_OK;
}

uint32_t ecuio_signal_file_note_count(const ecuio_signal_file_t* file)
{
    return file != nullptr ? static_cast<uint32_t>(file->notes.size()) : 0;
}

ecuio_status_t ecuio_signal_file_note(const ecuio_signal_file_t* file,
                                      uint32_t index, ecuio_signal_note_t* out)
{
    ecuio_detail_clear();
    if (file == nullptr || out == nullptr) {
        ecuio_detail_set("the note needs a file and a struct to fill");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sized(out->struct_size, sizeof(ecuio_signal_note_t), "note")) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (index >= file->notes.size()) {
        ecuio_detail_set("the file has %u notes, and index %u is none",
                         static_cast<unsigned>(file->notes.size()),
                         static_cast<unsigned>(index));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_signal_file::note& n = file->notes[index];
    ecuio_signal_note_t filled{};
    filled.struct_size = sizeof(ecuio_signal_note_t);
    filled.kind = n.kind;
    filled.part = n.part;
    filled.other_part = n.other_part;
    filled.def = n.def;
    filled.count = n.count;
    filled.base_def = n.base_def;
    ecuio_sf::copy_text(filled.text, sizeof filled.text, n.text);
    std::memcpy(out, &filled, sizeof filled);
    return ECUIO_OK;
}

} // extern "C"
