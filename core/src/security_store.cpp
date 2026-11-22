// File/layer: core/src/security_store.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: SecurityStoreTest (test_core); FUZZ: fuzz_security_store

#include "security_store.h"

#include "folder_files.h"
#include "json.h"
#include "sha256.h"
#include "signal_file_build.h"
#include "tier2_ceiling.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>

namespace ecuio_security {

namespace {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_sf::expect;
using ecuio_sf::only_keys;
using ecuio_sf::q;
using ecuio_sf::refuse;
using ecuio_sf::text_of;
using ecuio_sf::whole_of;

constexpr char kFormatKey[] = "havne_security_store";
constexpr uint8_t kSecurityAccess = 0x27;
constexpr uint32_t kRxMax = 0x1FFFFFFF;
constexpr size_t kVehicleDigits = 16;
/* The arrays and objects a store nests: the root, its list, an entry */
constexpr unsigned kStoreDepth = 3;

/* The longest entry as store_text writes it, its lines and their indents:
 * kStoreMaxEntries of them and the root's own lines fit the file */
constexpr size_t kEntryTextMax =
    sizeof("    {\n      \"vehicle\": \"FFFFFFFFFFFFFFFF\",\n"
           "      \"rx\": \"1FFFFFFF\",\n      \"level\": 125,\n"
           "      \"failed\": 2147483647,\n      \"last_utc\": 253402300799,\n"
           "      \"pending_utc\": 253402300799\n    },\n");
static_assert(kStoreMaxEntries * kEntryTextMax + 128 <= kStoreMaxBytes,
              "a full store's text fits the file's bound");

bool nothing_kept(const store_entry& e)
{
    return e.failed == 0 && e.last_utc == 0 && e.pending_utc == 0;
}

size_t kept_in(const std::vector<store_entry>& list)
{
    size_t kept = 0;
    for (const store_entry& e : list) {
        kept += nothing_kept(e) ? 0 : 1;
    }
    return kept;
}

store_entry* find_in(std::vector<store_entry>& list, uint64_t vehicle,
                     uint32_t rx, uint8_t level)
{
    for (store_entry& e : list) {
        if (e.vehicle == vehicle && e.rx == rx && e.level == level) {
            return &e;
        }
    }
    return nullptr;
}

/* why a view, as text_of's what */
const Value& needed(const Value& obj, const char* key, std::string_view why)
{
    const Value* v = obj.find(key);
    if (v == nullptr) {
        refuse(obj, std::string(why));
    }
    return *v;
}

/* Hex as the store writes it, 0-9 and A-F, of 16 digits at most: its
 * value into *out, false for any other character */
bool hex_value(const std::string& t, uint64_t* out)
{
    uint64_t n = 0;
    for (const char c : t) {
        const bool digit = c >= '0' && c <= '9';
        if (!digit && (c < 'A' || c > 'F')) {
            return false;
        }
        n = n * 16 + static_cast<uint64_t>(digit ? c - '0' : c - 'A' + 10);
    }
    *out = n;
    return true;
}

/* An ID as the store writes it: 1 to 8 hex digits, no leading zero, 1 to
 * 1FFFFFFF. One spelling each, so "each entry once" is the text's own
 * rule. */
uint32_t rx_of(const Value& v)
{
    const std::string& t = text_of(v, 1, 8, "an entry's rx");
    uint64_t rx = 0;
    if (!hex_value(t, &rx)) {
        refuse(v, "an entry's rx " + q(t) + " isn't an ID in hex: 0-9 and A-F");
    }
    if (t[0] == '0' || rx > kRxMax) {
        refuse(v, "an entry's rx " + q(t) +
                      " isn't an ID from 1 to 1FFFFFFF without leading zeros");
    }
    return static_cast<uint32_t>(rx);
}

/* A vehicle's number: its 16 hex digits, each one written */
uint64_t vehicle_of(const Value& v)
{
    const std::string& t = text_of(v, 0, 64, "an entry's vehicle");
    uint64_t vehicle = 0;
    if (t.size() != kVehicleDigits || !hex_value(t, &vehicle)) {
        refuse(v, "an entry's vehicle " + q(t) +
                      " isn't 16 hex digits: 0-9 and A-F");
    }
    return vehicle;
}

store_entry entry_of(const Value& v)
{
    expect(v, Type::object, "an entry");
    only_keys(v,
              {"vehicle", "rx", "level", "failed", "last_utc", "pending_utc"},
              "an entry");
    store_entry e;
    e.vehicle = vehicle_of(needed(v, "vehicle", "an entry needs \"vehicle\""));
    e.rx = rx_of(needed(v, "rx", "an entry needs \"rx\""));
    const Value& level = needed(v, "level", "an entry needs \"level\"");
    e.level = static_cast<uint8_t>(whole_of(level, 0, 255, "an entry's level"));
    /* The ceiling takes a seed's sub-function and its key's together */
    if ((e.level & 1) == 0 || !ecuio_tier2_allowed(kSecurityAccess, e.level)) {
        refuse(level, "an entry's level isn't a seed request's sub-function: "
                      "odd, 1 to 125, never 95");
    }
    e.failed = static_cast<uint32_t>(
        whole_of(needed(v, "failed", "an entry needs \"failed\""), 0,
                 kStoreMaxFailed, "an entry's failed"));
    e.last_utc = whole_of(needed(v, "last_utc", "an entry needs \"last_utc\""),
                          0, kStoreMaxUtc, "an entry's last_utc");
    e.pending_utc =
        whole_of(needed(v, "pending_utc", "an entry needs \"pending_utc\""), 0,
                 kStoreMaxUtc, "an entry's pending_utc");
    return e;
}

/* An attempt marked and never ended is a failed one, dated by its mark */
void count_pending(store_entry& e)
{
    if (e.pending_utc == 0) {
        return;
    }
    if (e.failed < kStoreMaxFailed) {
        ++e.failed;
    }
    e.last_utc = e.pending_utc;
    e.pending_utc = 0;
}

/* The format is judged before the keys, so a later format's file is
 * refused as that and not for a key this one doesn't know */
void read_root(const Value& root, std::vector<store_entry>& list)
{
    expect(root, Type::object, "a security store");
    const Value& format = needed(
        root, kFormatKey, "a security store needs \"havne_security_store\": 1");
    if (whole_of(format, 0, INT32_MAX, "\"havne_security_store\"") !=
        kStoreFormat) {
        refuse(format, "this Havne reads a security store of format 1 only");
    }
    only_keys(root, {kFormatKey, "entries"}, "a security store");
    const Value& entries =
        needed(root, "entries", "a security store needs \"entries\"");
    expect(entries, Type::array, "a security store's entries");
    if (entries.items.size() > kStoreMaxEntries) {
        refuse(entries, "\"entries\" lists " +
                            std::to_string(entries.items.size()) +
                            ", more than " + std::to_string(kStoreMaxEntries));
    }
    for (const Value& v : entries.items) {
        const store_entry e = entry_of(v);
        if (find_in(list, e.vehicle, e.rx, e.level) != nullptr) {
            refuse(v, "an entry's vehicle, rx and level are given twice");
        }
        list.push_back(e);
    }
}

/* A store's text into *list as the file holds it, each mark still in
 * place; store_read's answers */
store_io read_entries(const char* bytes, size_t size,
                      std::vector<store_entry>* list, std::string* why)
{
    list->clear();
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
            read_root(root, *list);
        } catch (const ecuio_sf::refused& r) {
            list->clear();
            *why = "line " + std::to_string(r.line) + ", column " +
                   std::to_string(r.column) + ": " + r.words;
            return store_io::refused;
        }
        return store_io::ok;
    } catch (const std::bad_alloc&) {
        list->clear();
        return store_io::out_of_memory;
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

Value string_of(const char* t)
{
    Value v;
    v.type = Type::string;
    v.text = t;
    return v;
}

Member member(const char* key, Value v)
{
    Member m;
    m.key = key;
    m.value = std::move(v);
    return m;
}

std::string text_of_entries(const std::vector<store_entry>& entries)
{
    Value list;
    list.type = Type::array;
    for (const store_entry& e : entries) {
        if (nothing_kept(e)) {
            continue;
        }
        char vehicle[17];
        std::snprintf(vehicle, sizeof vehicle, "%016llX",
                      static_cast<unsigned long long>(e.vehicle));
        char rx[9];
        std::snprintf(rx, sizeof rx, "%X", e.rx);
        Value entry;
        entry.type = Type::object;
        entry.members.push_back(member("vehicle", string_of(vehicle)));
        entry.members.push_back(member("rx", string_of(rx)));
        entry.members.push_back(member("level", number(e.level)));
        entry.members.push_back(member("failed", number(e.failed)));
        entry.members.push_back(member("last_utc", number(e.last_utc)));
        entry.members.push_back(member("pending_utc", number(e.pending_utc)));
        list.items.push_back(std::move(entry));
    }
    Value root;
    root.type = Type::object;
    root.members.push_back(member(kFormatKey, number(kStoreFormat)));
    root.members.push_back(member("entries", std::move(list)));
    return ecuio_json::write(root);
}

struct file_closer {
    void operator()(FILE* f) const { std::fclose(f); }
};

enum class file_read : uint8_t {
    none,   /* no such file */
    read,   /* its bytes are in *bytes */
    failed, /* it is there, or may be, and can't be opened or read */
};

/* The file's bytes into *bytes, one past the store's bound at most;
 * failed with *why, words that follow "the security store" */
file_read read_file(const std::string& path, std::string* bytes,
                    std::string* why)
{
    bytes->clear();
    bool none = false;
    std::string opened;
    const int seam = store_test_open_errno.exchange(0);
    const std::unique_ptr<FILE, file_closer> f(
        seam != 0 ? nullptr : ecuio_files::open_existing(path, &none, &opened));
    if (f == nullptr) {
        if (seam != 0) {
            none = seam == ENOENT;
            opened = ecuio_files::errno_words(seam);
        }
        if (none) {
            return file_read::none;
        }
        *why = "can't be opened: " + opened;
        return file_read::failed;
    }
    bytes->resize(kStoreMaxBytes + 1);
    const size_t got = std::fread(&(*bytes)[0], 1, bytes->size(), f.get());
    if (std::ferror(f.get()) != 0) {
        bytes->clear();
        *why = "can't be read";
        return file_read::failed;
    }
    bytes->resize(got);
    return file_read::read;
}

/* s's entries into *merged, and the file's own with them when the file
 * isn't what s last read or wrote: each of the file's kept, and where
 * both hold one the larger count, the later second and the later mark.
 * refused with *words when the file there can't be read or doesn't read:
 * it is never written over unread. */
store_io merged_entries(const std::string& path, const store& s,
                        std::vector<store_entry>* merged, std::string* words)
{
    *merged = s.entries;
    std::string there;
    const file_read got = read_file(path, &there, words);
    if (got == file_read::failed) {
        *words = "the file there " + *words;
        return store_io::refused;
    }
    if (got == file_read::none || (!there.empty() && there == s.on_disk)) {
        return store_io::ok;
    }
    std::vector<store_entry> theirs;
    const store_io read =
        read_entries(there.data(), there.size(), &theirs, words);
    if (read == store_io::refused) {
        *words = "the file there doesn't read (" + *words + ")";
    }
    if (read != store_io::ok) {
        return read;
    }
    for (const store_entry& t : theirs) {
        store_entry* mine = find_in(*merged, t.vehicle, t.rx, t.level);
        if (mine == nullptr) {
            merged->push_back(t);
            continue;
        }
        mine->failed = (std::max)(mine->failed, t.failed);
        mine->last_utc = (std::max)(mine->last_utc, t.last_utc);
        mine->pending_utc = (std::max)(mine->pending_utc, t.pending_utc);
    }
    return store_io::ok;
}

/* f's bytes down to the disk itself */
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

/* The store's lock into *lock; false with *why, the save's refusal */
bool lock_store(const std::string& path, ecuio_files::file_lock* lock,
                std::string* why)
{
    const int seam = store_test_lock_wait_ms.load();
    std::string words;
    const ecuio_files::lock_got got = ecuio_files::lock_file(
        path + ".lock", seam != 0 ? seam : kStoreLockWaitMs, lock, &words);
    if (got == ecuio_files::lock_got::busy) {
        *why = "another program is using the security store";
    } else if (got == ecuio_files::lock_got::failed) {
        *why = "the security store can't be written: " + words;
    }
    return got == ecuio_files::lock_got::locked;
}

} // namespace

uint64_t store_vehicle_of(const std::string& identity)
{
    ecuio_sha256::hasher h;
    h.add(identity.data(), identity.size());
    uint8_t digest[32];
    h.finish(digest);
    uint64_t vehicle = 0;
    for (int i = 0; i < 8; ++i) {
        vehicle = (vehicle << 8) | digest[i];
    }
    return vehicle;
}

store_entry* store_find(store& s, uint64_t vehicle, uint32_t rx, uint8_t level)
{
    return find_in(s.entries, vehicle, rx, level);
}

store_entry* store_entry_for(store& s, uint64_t vehicle, uint32_t rx,
                             uint8_t level)
{
    if (store_entry* e = store_find(s, vehicle, rx, level)) {
        return e;
    }
    store_entry made;
    made.vehicle = vehicle;
    made.rx = rx;
    made.level = level;
    /* One with nothing to keep gives its place up before the store is full */
    for (store_entry& e : s.entries) {
        if (nothing_kept(e)) {
            e = made;
            return &e;
        }
    }
    if (s.entries.size() >= kStoreMaxEntries) {
        return nullptr;
    }
    s.entries.push_back(made);
    return &s.entries.back();
}

void store_attempt_began(store_entry& e, int64_t utc)
{
    /* 0 is "no mark": a clock before 1970 still marks */
    e.pending_utc = utc < 1 ? 1 : utc > kStoreMaxUtc ? kStoreMaxUtc : utc;
}

void store_attempt_ended(store_entry& e, bool failed, bool delay, int64_t utc)
{
    e.pending_utc = 0;
    if (failed && e.failed < kStoreMaxFailed) {
        ++e.failed;
    }
    if (failed || delay) {
        e.last_utc = utc < 1 ? 1 : utc > kStoreMaxUtc ? kStoreMaxUtc : utc;
    }
}

void store_unlocked(store_entry& e)
{
    e.failed = 0;
    e.last_utc = 0;
    e.pending_utc = 0;
}

store_io store_read(const char* bytes, size_t size, store* out,
                    std::string* why)
{
    out->entries.clear();
    std::vector<store_entry> list;
    const store_io io = read_entries(bytes, size, &list, why);
    if (io != store_io::ok) {
        return io;
    }
    size_t kept = 0;
    for (store_entry& e : list) {
        count_pending(e);
        if (!nothing_kept(e)) {
            list[kept++] = e;
        }
    }
    list.resize(kept);
    out->entries = std::move(list);
    return store_io::ok;
}

std::string store_text(const store& s)
{
    return text_of_entries(s.entries);
}

store_io store_load(const std::string& path, store* out, std::string* why)
{
    out->entries.clear();
    out->on_disk.clear();
    try {
        std::string bytes;
        std::string words;
        const file_read got = read_file(path, &bytes, &words);
        if (got == file_read::none) {
            return store_io::ok;
        }
        if (got == file_read::failed) {
            *why = "the security store " + words;
            return store_io::refused;
        }
        if (bytes.size() > kStoreMaxBytes) {
            words = "it is longer than " + std::to_string(kStoreMaxBytes) +
                    " bytes";
        } else {
            const store_io read =
                store_read(bytes.data(), bytes.size(), out, &words);
            if (read == store_io::ok) {
                out->on_disk = std::move(bytes);
            }
            if (read != store_io::refused) {
                return read;
            }
        }
        *why = "the security store doesn't read (" + words +
               "). It keeps the count of failed security-access attempts; "
               "removing the file forgets them";
        return store_io::refused;
    } catch (const std::bad_alloc&) {
        out->entries.clear();
        out->on_disk.clear();
        return store_io::out_of_memory;
    }
}

store_io store_save(const std::string& path, store* s, std::string* why)
{
    std::string name; /* this write's own file, while there is one */
    std::vector<store_entry> merged;
    std::string text;
    bool renamed = false;
    bool kept = false;
    /* Once the new file stands, *s is what it holds, even when the save
     * then fails: its next save would merge its own file back otherwise */
    const auto keep = [&] {
        if (renamed && !kept) {
            s->entries = std::move(merged);
            s->on_disk = std::move(text);
            kept = true;
        }
    };
    try {
        const int fail = store_test_fail_at.exchange(0);
        /* Held from the read to the replace, so no other save comes between */
        ecuio_files::file_lock lock;
        if (!lock_store(path, &lock, why)) {
            return store_io::refused;
        }
        std::string words;
        const store_io look = merged_entries(path, *s, &merged, &words);
        if (look == store_io::out_of_memory) {
            return look;
        }
        if (look == store_io::refused) {
            *why = "the security store can't be written: " + words;
            return store_io::refused;
        }
        if (kept_in(merged) > kStoreMaxEntries) {
            /* Only a store filled past store_entry_for, or by another
             * writer's entries: what is written must read back, and that
             * many entries fit the bytes */
            *why = "the security store is full";
            return store_io::refused;
        }
        text = text_of_entries(merged);
        if (!write_beside(path, text, fail, &name, &words)) {
            *why = "the security store can't be written: " + words;
            return store_io::refused;
        }
        if (void (*hook)() = store_test_before_replace.exchange(nullptr)) {
            hook();
        }
        if (fail == 3) {
            words = "the disk is full";
        }
        bool replaced = fail != 3 &&
                        ecuio_files::replace_with(name, path, &words, &renamed);
        if (fail == 4 && replaced) {
            replaced = false;
            words = "its folder can't be flushed";
        }
        if (!renamed) {
            ecuio_files::remove_file(name);
        }
        name.clear();
        keep();
        if (!replaced) {
            *why = "the security store can't be written: " + words;
            return store_io::refused;
        }
        return store_io::ok;
    } catch (const std::bad_alloc&) {
        if (!renamed && !name.empty()) {
            ecuio_files::remove_file(name);
        }
        keep();
        return store_io::out_of_memory;
    }
}

} // namespace ecuio_security
