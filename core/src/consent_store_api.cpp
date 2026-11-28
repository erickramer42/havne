// File/layer: core/src/consent_store_api.cpp | C ABI, C++17, Qt-free,
//             every platform
// Testing: UNIT: ConsentStoreTest (test_core); test_c_abi
//
// The consent store's C calls (plugin.h): one owner for a path in the
// process (path_registry.h), each call under its mutex. A record is made
// from the plugin itself, never from fields the caller names, and a give
// or a withdraw is on the disk before the call returns. Each call first
// loads the file again when another program changed it, so a withdraw
// there holds here.

#include "ecuio/plugin.h"

#include "consent_store.h"
#include "error_detail.h"
#include "path_registry.h"
#include "plugin_loader.h"
#include "sha256.h"
#include "signal_file_build.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

using ecuio_consent::record;
using ecuio_consent::store_io;

struct consent_owner {
    std::mutex mutex;
    std::string path;
    ecuio_consent::store store;
};

ecuio_files::path_registry<consent_owner>& owners()
{
    static ecuio_files::path_registry<consent_owner> registry;
    return registry;
}

using ecuio_sf::copy_text;

void fill(const record& r, ecuio_consent_record_t* out)
{
    ecuio_consent_record_t f{};
    f.struct_size = sizeof f;
    f.given_utc = static_cast<uint64_t>(r.given_utc);
    copy_text(f.id, sizeof f.id, r.id);
    copy_text(f.version, sizeof f.version, r.version);
    std::memcpy(f.fingerprint, r.fingerprint, sizeof f.fingerprint);
    std::memcpy(f.terms_sha256, r.terms_sha256, sizeof f.terms_sha256);
    copy_text(f.publisher, sizeof f.publisher, r.publisher);
    copy_text(f.name, sizeof f.name, r.name);
    f.flags = r.file ? ECUIO_CONSENT_RECORD_FILE : 0u;
    *out = f;
}

/* The status a failed save answers, its words set */
ecuio_status_t refused(store_io io, const std::string& why)
{
    if (io == store_io::out_of_memory) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    ecuio_detail_set("%s", why.c_str());
    return ECUIO_ERR_LOAD_FAILED;
}

/* The records newest first, ties by fingerprint: the list's order */
std::vector<const record*> newest_first(const ecuio_consent::store& s)
{
    std::vector<const record*> list;
    list.reserve(s.records.size());
    for (const record& r : s.records) {
        list.push_back(&r);
    }
    std::sort(list.begin(), list.end(), [](const record* a, const record* b) {
        return a->given_utc != b->given_utc
                   ? a->given_utc > b->given_utc
                   : std::memcmp(a->fingerprint, b->fingerprint, 32) < 0;
    });
    return list;
}

bool sized(const ecuio_consent_record_t* out)
{
    return out->struct_size >= sizeof(ecuio_consent_record_t);
}

/* The owner's store as the file holds it, its mutex held: OK, or the
 * status with words, the store as it was */
ecuio_status_t refresh(consent_owner& o)
{
    std::string why;
    const store_io io = ecuio_consent::store_refresh(o.path, &o.store, &why);
    return io == store_io::ok ? ECUIO_OK : refused(io, why);
}

/* The getters' refresh: one that fails answers from the store held, with
 * no words */
void refresh_quietly(consent_owner& o)
{
    std::string why;
    (void)ecuio_consent::store_refresh(o.path, &o.store, &why);
}

} // namespace

struct ecuio_consent_store {
    ecuio_files::path_registry<consent_owner>::ref ref;
};

extern "C" {

ecuio_status_t ecuio_consent_store_open(const char* path,
                                        ecuio_consent_store_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (path == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    store_io io = store_io::ok;
    std::string why;
    try {
        std::unique_ptr<ecuio_consent_store> handle(new ecuio_consent_store());
        const bool open = owners().open(
            path, &handle->ref, [&](std::unique_ptr<consent_owner>* made) {
                auto owner = std::make_unique<consent_owner>();
                owner->path = path;
                io = ecuio_consent::store_load(path, &owner->store, &why);
                if (io != store_io::ok) {
                    return false;
                }
                *made = std::move(owner);
                return true;
            });
        if (!open) {
            return refused(io, why);
        }
        *out = handle.release();
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_consent_store_close(ecuio_consent_store_t* store)
{
    if (store == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    delete store;
    return ECUIO_OK;
}

ecuio_status_t ecuio_consent_store_find(const ecuio_consent_store_t* store,
                                        const uint8_t* fingerprint,
                                        ecuio_consent_record_t* out)
{
    ecuio_detail_clear();
    if (store == nullptr || fingerprint == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sized(out)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    consent_owner& o = *store->ref.get();
    const std::lock_guard<std::mutex> hold(o.mutex);
    const ecuio_status_t fresh = refresh(o);
    if (fresh != ECUIO_OK) {
        return fresh;
    }
    const record* r = ecuio_consent::store_find(o.store, fingerprint);
    if (r == nullptr) {
        return ECUIO_ERR_CONSENT_REQUIRED;
    }
    fill(*r, out);
    return ECUIO_OK;
}

ecuio_status_t ecuio_consent_store_latest(const ecuio_consent_store_t* store,
                                          const char* id,
                                          ecuio_consent_record_t* out)
{
    if (store == nullptr || id == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sized(out)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    consent_owner& o = *store->ref.get();
    const std::lock_guard<std::mutex> hold(o.mutex);
    refresh_quietly(o);
    try {
        for (const record* r : newest_first(o.store)) {
            if (!r->file && r->id == id) {
                fill(*r, out);
                return ECUIO_OK;
            }
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_ERR_CONSENT_REQUIRED;
}

ecuio_status_t ecuio_consent_store_count(const ecuio_consent_store_t* store,
                                         uint32_t* out)
{
    if (store == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    consent_owner& o = *store->ref.get();
    const std::lock_guard<std::mutex> hold(o.mutex);
    refresh_quietly(o);
    *out = static_cast<uint32_t>(o.store.records.size());
    return ECUIO_OK;
}

ecuio_status_t ecuio_consent_store_get(const ecuio_consent_store_t* store,
                                       uint32_t index,
                                       ecuio_consent_record_t* out)
{
    if (store == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sized(out)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    consent_owner& o = *store->ref.get();
    const std::lock_guard<std::mutex> hold(o.mutex);
    refresh_quietly(o);
    if (index >= o.store.records.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        fill(*newest_first(o.store)[index], out);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

} // extern "C"

namespace {

/* Under the owner's mutex, refreshed; a fingerprint given keeps its first */
ecuio_status_t add_record(consent_owner& o, record r)
{
    if (ecuio_consent::store_find(o.store, r.fingerprint) != nullptr) {
        return ECUIO_OK;
    }
    if (o.store.records.size() >= ecuio_consent::kStoreMaxEntries) {
        ecuio_detail_set("the consent store holds %u consents and has no "
                         "room: withdraw one first",
                         static_cast<unsigned>(o.store.records.size()));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_consent::store next = o.store;
    next.records.push_back(std::move(r));
    std::string why;
    const store_io io = ecuio_consent::store_save(o.path, &next, &why);
    if (io != store_io::ok && next.on_disk == o.store.on_disk) {
        return refused(io, why);
    }
    o.store = std::move(next);
    return io == store_io::ok ? ECUIO_OK : refused(io, why);
}

bool utc_ok(uint64_t utc)
{
    if (utc < 1 || utc > static_cast<uint64_t>(ecuio_consent::kStoreMaxUtc)) {
        ecuio_detail_set("the second is outside 1970 to 9999");
        return false;
    }
    return true;
}

ecuio_status_t own_declaring(const char* bytes, size_t length)
{
    ecuio_signal_file_t* f = nullptr;
    const ecuio_status_t loaded = ecuio_signal_file_load(bytes, length, &f);
    if (loaded != ECUIO_OK) {
        if (loaded == ECUIO_ERR_INVALID_ARGUMENT) {
            const std::string words = ecuio_last_error_detail();
            ecuio_detail_set("the file doesn't read as a signal file (%s)",
                             words.c_str());
        }
        return loaded;
    }
    const bool merged = !f->parts.empty();
    bool declares = false;
    for (const ecuio_channel_decl_t& c : f->channels) {
        declares = declares || c.session == ECUIO_CHANNEL_SESSION_EXTENDED;
    }
    ecuio_signal_file_free(f);
    if (merged) {
        ecuio_detail_set("the file is a merged set: a consent goes to the "
                         "parts that declare its sessions");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (!declares) {
        ecuio_detail_set("the file declares no module's session, so there "
                         "is nothing to consent to");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_consent_store_give(ecuio_consent_store_t* store,
                                        const ecuio_plugin_t* plugin,
                                        uint64_t utc)
{
    ecuio_detail_clear();
    if (store == nullptr || plugin == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (plugin->publisher == nullptr) {
        ecuio_detail_set("this plugin names no publisher, so no consent can "
                         "be given to it: it reads signals only");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (!utc_ok(utc)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    consent_owner& o = *store->ref.get();
    const std::lock_guard<std::mutex> hold(o.mutex);
    const ecuio_status_t fresh = refresh(o);
    if (fresh != ECUIO_OK) {
        return fresh;
    }
    try {
        record r;
        r.id = plugin->id;
        r.version = plugin->version;
        std::memcpy(r.fingerprint, plugin->fingerprint, 32);
        std::memcpy(r.terms_sha256, plugin->publisher->terms_sha256, 32);
        r.publisher = plugin->publisher->name;
        r.given_utc = static_cast<int64_t>(utc);
        return add_record(o, std::move(r));
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_consent_store_give_file(ecuio_consent_store_t* store,
                                             const char* bytes, size_t length,
                                             const char* name, uint64_t utc)
{
    ecuio_detail_clear();
    if (store == nullptr || name == nullptr ||
        (bytes == nullptr && length > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!utc_ok(utc)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        record r;
        r.file = true;
        r.name = name;
        ecuio_sha256::hasher h;
        h.add(bytes, length);
        h.finish(r.fingerprint);
        r.given_utc = static_cast<int64_t>(utc);
        std::string why;
        const store_io judged = ecuio_consent::record_judged(r, &why);
        if (judged == store_io::out_of_memory) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        if (judged != store_io::ok) {
            ecuio_detail_set("%s", why.c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        const ecuio_status_t own = own_declaring(bytes, length);
        if (own != ECUIO_OK) {
            return own;
        }
        consent_owner& o = *store->ref.get();
        const std::lock_guard<std::mutex> hold(o.mutex);
        const ecuio_status_t fresh = refresh(o);
        if (fresh != ECUIO_OK) {
            return fresh;
        }
        return add_record(o, std::move(r));
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_consent_store_withdraw(ecuio_consent_store_t* store,
                                            const char* id,
                                            const uint8_t* fingerprint)
{
    ecuio_detail_clear();
    if (store == nullptr || (id == nullptr && fingerprint == nullptr)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    consent_owner& o = *store->ref.get();
    const std::lock_guard<std::mutex> hold(o.mutex);
    const ecuio_status_t fresh = refresh(o);
    if (fresh != ECUIO_OK) {
        return fresh;
    }
    try {
        ecuio_consent::store next = o.store;
        const auto gone = [&](const record& r) {
            return (id == nullptr || (!r.file && r.id == id)) &&
                   (fingerprint == nullptr ||
                    std::memcmp(r.fingerprint, fingerprint, 32) == 0);
        };
        next.records.erase(
            std::remove_if(next.records.begin(), next.records.end(), gone),
            next.records.end());
        if (next.records.size() == o.store.records.size()) {
            ecuio_detail_set("the consent store holds no such consent");
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::string why;
        const store_io io = ecuio_consent::store_save(o.path, &next, &why);
        if (io != store_io::ok && next.on_disk == o.store.on_disk) {
            return refused(io, why);
        }
        o.store = std::move(next);
        return io == store_io::ok ? ECUIO_OK : refused(io, why);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
