// File/layer: core/src/consent_store.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: ConsentStoreTest (test_core); FUZZ: fuzz_consent_store

#include "consent_store.h"

#include "folder_files.h"
#include "json.h"
#include "plugin_loader.h"
#include "sha256.h"
#include "signal_file_build.h"
#include "store_file.h"

#include <cstring>
#include <memory>
#include <new>
#include <string_view>

namespace ecuio_consent {

namespace {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_sf::expect;
using ecuio_sf::only_keys;
using ecuio_sf::refuse;
using ecuio_sf::text_of;
using ecuio_sf::whole_of;
using ecuio_store_file::file_read;
using ecuio_store_file::read_file;

constexpr char kFormatKey[] = "havne_consent_store";
/* The root, its list, a record */
constexpr unsigned kStoreDepth = 3;

/* The longest record as store_text writes it: a publisher of 127 bytes,
 * each escaped to two */
constexpr size_t kRecordTextMax =
    sizeof("    {\n      \"id\": \"\",\n      \"version\": \"\",\n"
           "      \"fingerprint\": \"\",\n      \"terms\": \"\",\n"
           "      \"publisher\": \"\",\n"
           "      \"given_utc\": 253402300799\n    },\n") +
    ECUIO_PLUGIN_ID_MAX + 15 + 64 + 64 + 2 * ECUIO_PLUGIN_PUBLISHER_NAME_MAX;
static_assert(kStoreMaxEntries * kRecordTextMax + 128 <= kStoreMaxBytes,
              "the longest records fit the file's bound");

const Value& needed(const Value& obj, const char* key, std::string_view why)
{
    const Value* v = obj.find(key);
    if (v == nullptr) {
        refuse(obj, std::string(why));
    }
    return *v;
}

/* 64 lower-case hex digits into out */
void digest_of(const Value& v, const char* what, uint8_t out[32])
{
    const std::string& t = text_of(v, 64, 64, what);
    for (size_t i = 0; i < 32; ++i) {
        int b = 0;
        for (size_t k = 0; k < 2; ++k) {
            const char c = t[2 * i + k];
            const int d = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                 : -1;
            if (d < 0) {
                refuse(v, std::string(what) + " takes 64 of 0-9 and a-f");
            }
            b = b * 16 + d;
        }
        out[i] = static_cast<uint8_t>(b);
    }
}

record record_of(const Value& v)
{
    expect(v, Type::object, "a record");
    only_keys(
        v, {"id", "version", "fingerprint", "terms", "publisher", "given_utc"},
        "a record");
    record r;
    const Value& id = needed(v, "id", "a record needs \"id\"");
    r.id = text_of(id, 1, ECUIO_PLUGIN_ID_MAX, "a record's id");
    if (!ecuio_plugin_rules::id_ok(r.id, ECUIO_PLUGIN_ID_MAX)) {
        refuse(id, "a record's id isn't a plugin's id");
    }
    const Value& version = needed(v, "version", "a record needs \"version\"");
    r.version = text_of(version, 1, 15, "a record's version");
    uint32_t parts[3];
    if (!ecuio_plugin_rules::parse_version(r.version, parts)) {
        refuse(version, "a record's version isn't X.Y.Z");
    }
    digest_of(needed(v, "fingerprint", "a record needs \"fingerprint\""),
              "a record's fingerprint", r.fingerprint);
    digest_of(needed(v, "terms", "a record needs \"terms\""),
              "a record's terms", r.terms_sha256);
    r.publisher = ecuio_plugin_loader::plain_of(
        needed(v, "publisher", "a record needs \"publisher\""), 1,
        ECUIO_PLUGIN_PUBLISHER_NAME_MAX, "a record's publisher", false);
    r.given_utc =
        whole_of(needed(v, "given_utc", "a record needs \"given_utc\""), 1,
                 kStoreMaxUtc, "a record's given_utc");
    return r;
}

/* The format is judged before the keys, so a later format's file is
 * refused as that */
void read_root(const Value& root, std::vector<record>& list)
{
    expect(root, Type::object, "a consent store");
    const Value& format = needed(
        root, kFormatKey, "a consent store needs \"havne_consent_store\": 1");
    if (whole_of(format, 0, INT32_MAX, "\"havne_consent_store\"") !=
        kStoreFormat) {
        refuse(format, "this Havne reads a consent store of format 1 only");
    }
    only_keys(root, {kFormatKey, "entries"}, "a consent store");
    const Value& entries =
        needed(root, "entries", "a consent store needs \"entries\"");
    expect(entries, Type::array, "a consent store's entries");
    if (entries.items.size() > kStoreMaxEntries) {
        refuse(entries, "\"entries\" lists " +
                            std::to_string(entries.items.size()) +
                            ", more than " + std::to_string(kStoreMaxEntries));
    }
    for (const Value& v : entries.items) {
        record r = record_of(v);
        for (const record& known : list) {
            if (std::memcmp(known.fingerprint, r.fingerprint, 32) == 0) {
                refuse(v, "a record's fingerprint is given twice");
            }
        }
        list.push_back(std::move(r));
    }
}

Value number(int64_t n)
{
    Value v;
    v.type = Type::number;
    v.integer = true;
    v.in_int64 = true;
    v.whole = n;
    v.number = static_cast<double>(n);
    return v;
}

Value string_of(std::string t)
{
    Value v;
    v.type = Type::string;
    v.text = std::move(t);
    return v;
}

Member member(const char* key, Value v)
{
    Member m;
    m.key = key;
    m.value = std::move(v);
    return m;
}

bool same(const record& a, const record& b)
{
    return std::memcmp(a.fingerprint, b.fingerprint, 32) == 0;
}

bool holds(const std::vector<record>& list, const record& r)
{
    for (const record& x : list) {
        if (same(x, r)) {
            return true;
        }
    }
    return false;
}

/* s's records into *merged, merged with the file's when the file isn't
 * what s last read or wrote: a record s gave or withdrew since is s's,
 * any other the file's. refused when the file there doesn't read. */
store_io merged_records(const std::string& path, const store& s,
                        std::vector<record>* merged, std::string* words)
{
    *merged = s.records;
    std::string there;
    const file_read got = read_file(
        path, kStoreMaxBytes, store_test_open_errno.exchange(0), &there, words);
    if (got == file_read::failed) {
        *words = "the file there " + *words;
        return store_io::refused;
    }
    if (!there.empty() && there == s.on_disk) {
        return store_io::ok;
    }
    /* No file is a "theirs" of none: a removed file's records aren't s's
     * to write again */
    std::vector<record> theirs;
    if (got != file_read::none) {
        const store_io read =
            store_read(there.data(), there.size(), &theirs, words);
        if (read == store_io::refused) {
            *words = "the file there doesn't read (" + *words + ")";
        }
        if (read != store_io::ok) {
            return read;
        }
    }
    merged->clear();
    for (const record& m : s.records) {
        if (!holds(s.base, m) || holds(theirs, m)) {
            merged->push_back(m); /* given here since, or kept by both */
        }
    }
    for (const record& t : theirs) {
        if (!holds(s.base, t) && !holds(*merged, t)) {
            merged->push_back(t); /* given there since */
        }
    }
    return store_io::ok;
}

/* The store's lock into *lock; false with *why */
bool lock_store(const std::string& path, ecuio_files::file_lock* lock,
                std::string* why)
{
    const int seam = store_test_lock_wait_ms.load();
    std::string words;
    const ecuio_files::lock_got got = ecuio_files::lock_file(
        path + ".lock", seam != 0 ? seam : kStoreLockWaitMs, lock, &words);
    if (got == ecuio_files::lock_got::busy) {
        *why = "another program is using the consent store";
    } else if (got == ecuio_files::lock_got::failed) {
        *why = "the consent store can't be written: " + words;
    }
    return got == ecuio_files::lock_got::locked;
}

} // namespace

const record* store_find(const store& s, const uint8_t fingerprint[32])
{
    for (const record& r : s.records) {
        if (std::memcmp(r.fingerprint, fingerprint, 32) == 0) {
            return &r;
        }
    }
    return nullptr;
}

store_io store_read(const char* bytes, size_t size, std::vector<record>* out,
                    std::string* why)
{
    out->clear();
    try {
        Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = kStoreMaxBytes;
        limits.max_depth = kStoreDepth;
        if (!ecuio_json::parse(bytes, size, &root, &error, limits)) {
            if (error.out_of_memory) {
                return store_io::out_of_memory;
            }
            *why = error.to_string();
            return store_io::refused;
        }
        try {
            read_root(root, *out);
        } catch (const ecuio_sf::refused& r) {
            out->clear();
            *why = "line " + std::to_string(r.line) + ", column " +
                   std::to_string(r.column) + ": " + r.words;
            return store_io::refused;
        }
        return store_io::ok;
    } catch (const std::bad_alloc&) {
        out->clear();
        return store_io::out_of_memory;
    }
}

std::string store_text(const std::vector<record>& records)
{
    Value list;
    list.type = Type::array;
    for (const record& r : records) {
        Value entry;
        entry.type = Type::object;
        entry.members.push_back(member("id", string_of(r.id)));
        entry.members.push_back(member("version", string_of(r.version)));
        entry.members.push_back(
            member("fingerprint", string_of(ecuio_sha256::hex(r.fingerprint))));
        entry.members.push_back(
            member("terms", string_of(ecuio_sha256::hex(r.terms_sha256))));
        entry.members.push_back(member("publisher", string_of(r.publisher)));
        entry.members.push_back(member("given_utc", number(r.given_utc)));
        list.items.push_back(std::move(entry));
    }
    Value root;
    root.type = Type::object;
    root.members.push_back(member(kFormatKey, number(kStoreFormat)));
    root.members.push_back(member("entries", std::move(list)));
    return ecuio_json::write(root);
}

store_io store_load(const std::string& path, store* out, std::string* why)
{
    out->records.clear();
    out->on_disk.clear();
    out->base.clear();
    try {
        std::string bytes;
        std::string words;
        const file_read got =
            read_file(path, kStoreMaxBytes, store_test_open_errno.exchange(0),
                      &bytes, &words);
        if (got == file_read::none) {
            return store_io::ok;
        }
        if (got == file_read::failed) {
            *why = "the consent store " + words;
            return store_io::refused;
        }
        if (bytes.size() > kStoreMaxBytes) {
            words = "it is longer than " + std::to_string(kStoreMaxBytes) +
                    " bytes";
        } else {
            const store_io read =
                store_read(bytes.data(), bytes.size(), &out->records, &words);
            if (read == store_io::ok) {
                out->base = out->records;
                out->on_disk = std::move(bytes);
            }
            if (read != store_io::refused) {
                return read;
            }
        }
        *why = "the consent store doesn't read (" + words +
               "). It keeps the plugins you consented to; removing the file "
               "forgets them, and each asks again";
        return store_io::refused;
    } catch (const std::bad_alloc&) {
        out->records.clear();
        out->on_disk.clear();
        out->base.clear();
        return store_io::out_of_memory;
    }
}

store_io store_save(const std::string& path, store* s, std::string* why)
{
    std::vector<record> merged;
    std::vector<record> base; /* merged's copy, made before the rename */
    std::string text;
    bool renamed = false;
    bool kept = false;
    /* Once the new file stands, *s is what it holds */
    const auto keep = [&] {
        if (renamed && !kept) {
            s->records = std::move(merged);
            s->base = std::move(base);
            s->on_disk = std::move(text);
            kept = true;
        }
    };
    try {
        const int fail = store_test_fail_at.exchange(0);
        ecuio_files::file_lock lock;
        if (!lock_store(path, &lock, why)) {
            return store_io::refused;
        }
        std::string words;
        const store_io look = merged_records(path, *s, &merged, &words);
        if (look == store_io::out_of_memory) {
            return look;
        }
        if (look == store_io::refused) {
            *why = "the consent store can't be written: " + words;
            return store_io::refused;
        }
        if (merged.size() > kStoreMaxEntries) {
            *why = "the consent store is full";
            return store_io::refused;
        }
        text = store_text(merged);
        base = merged;
        const bool replaced = ecuio_store_file::replace_whole(
            path, text, fail, &store_test_before_replace, &renamed, &words);
        keep();
        if (!replaced) {
            *why = "the consent store can't be written: " + words;
            return store_io::refused;
        }
        return store_io::ok;
    } catch (const std::bad_alloc&) {
        keep();
        return store_io::out_of_memory;
    }
}

} // namespace ecuio_consent
