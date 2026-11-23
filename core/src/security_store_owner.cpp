// File/layer: core/src/security_store_owner.cpp | internal, C++17, Qt-free,
//             every platform
// Testing: UNIT: SecurityStoreTest, ConversationSessionTest (test_core)

#include "security_store_owner.h"

#include <chrono>
#include <memory>
#include <new>
#include <system_error>
#include <utility>

namespace ecuio_security {

namespace {

ecuio_files::path_registry<store_owner>& owners()
{
    /* Never destroyed: a reference released during exit still finds it */
    static auto* const registry = new ecuio_files::path_registry<store_owner>;
    return *registry;
}

/* The key's entry, or one of zero counts; allocates nothing */
store_entry copy_of(store& s, const store_key& key)
{
    if (const store_entry* e = store_find(s, key.vehicle, key.rx, key.level)) {
        return *e;
    }
    store_entry none;
    none.vehicle = key.vehicle;
    none.rx = key.rx;
    none.level = key.level;
    return none;
}

void no_room(std::string* why)
{
    *why = "the security store holds " + std::to_string(kStoreMaxEntries) +
           " entries and has no room";
}

} // namespace

store_entry store_owner::counts(const store_key& key)
{
    const std::lock_guard<std::mutex> hold(mutex_);
    return copy_of(store_, key);
}

/* Under the mutex: a save moves the entries, so the key's is found again */
store_io store_owner::save_and_find(const store_key& key, store_entry* after,
                                    std::string* why)
{
    const store_io io = store_save(path_, &store_, why);
    if (after != nullptr) {
        *after = copy_of(store_, key);
    }
    return io;
}

store_io store_owner::began(const store_key& key, int64_t utc,
                            store_entry* after, std::string* why)
{
    const std::lock_guard<std::mutex> hold(mutex_);
    settle_all_held();
    store_io io = store_io::out_of_memory;
    try {
        store_entry* e =
            store_entry_for(store_, key.vehicle, key.rx, key.level);
        if (e == nullptr) {
            no_room(why);
            io = store_io::refused;
        } else {
            const int64_t before = e->pending_utc;
            store_attempt_began(*e, utc);
            io = store_save(path_, &store_, why);
            /* An attempt that can't be marked doesn't start */
            store_entry* again =
                io == store_io::ok
                    ? nullptr
                    : store_find(store_, key.vehicle, key.rx, key.level);
            if (again != nullptr) {
                again->pending_utc = before;
            }
        }
    } catch (const std::bad_alloc&) {
        io = store_io::out_of_memory; /* before the mark: nothing marked */
    }
    if (after != nullptr) {
        *after = copy_of(store_, key);
    }
    return io;
}

store_io store_owner::ended(const store_key& key, bool failed, bool delay,
                            int64_t utc, store_entry* after, std::string* why)
{
    const std::lock_guard<std::mutex> hold(mutex_);
    return ended_held(key, failed, delay, utc, after, why);
}

store_io store_owner::ended_held(const store_key& key, bool failed, bool delay,
                                 int64_t utc, store_entry* after,
                                 std::string* why)
{
    try {
        store_entry* e =
            store_entry_for(store_, key.vehicle, key.rx, key.level);
        if (e == nullptr) {
            no_room(why);
            if (after != nullptr) {
                *after = copy_of(store_, key);
            }
            return store_io::refused;
        }
        store_attempt_ended(*e, failed, delay, utc);
        return save_and_find(key, after, why);
    } catch (const std::bad_alloc&) {
        if (after != nullptr) {
            *after = copy_of(store_, key);
        }
        return store_io::out_of_memory;
    }
}

store_io store_owner::unlocked(const store_key& key, store_entry* after,
                               std::string* why)
{
    const std::lock_guard<std::mutex> hold(mutex_);
    return unlocked_held(key, after, why);
}

store_io store_owner::unlocked_held(const store_key& key, store_entry* after,
                                    std::string* why)
{
    store_entry* e = store_find(store_, key.vehicle, key.rx, key.level);
    if (e == nullptr) {
        /* Nothing to keep, nothing to save */
        if (after != nullptr) {
            *after = copy_of(store_, key);
        }
        return store_io::ok;
    }
    store_unlocked(*e);
    return save_and_find(key, after, why);
}

/* Under the mutex: b's posted end saved once. A refused save keeps the
 * end in memory for the next (ended's rule); the box is free either way. */
void store_owner::settle_held(settle_box& b) noexcept
{
    const uint32_t posted = b.posted.load(std::memory_order_acquire);
    if (posted == b.taken) {
        return;
    }
    b.taken = posted;
    try {
        std::string why;
        if (b.unlocked) {
            (void)unlocked_held(b.key, nullptr, &why);
        } else {
            (void)ended_held(b.key, b.failed, b.delay, b.utc, nullptr, &why);
        }
    } catch (const std::bad_alloc&) {
        return; /* the mark stays on the disk: the next load counts it once */
    }
}

void store_owner::settle_all_held() noexcept
{
    for (settle_box* b : boxes_) {
        settle_held(*b);
    }
}

/* How long the writer sleeps between looks when no wake comes: a wake
 * sent while it saves is seen at the next look */
constexpr auto kWriterLook = std::chrono::milliseconds(50);

void store_owner::write_loop() noexcept
{
    std::unique_lock<std::mutex> hold(mutex_);
    while (!stop_) {
        settle_all_held();
        wake_.wait_for(hold, kWriterLook);
    }
    settle_all_held();
}

store_io store_owner::attach(settle_box* b, std::string* why)
{
    const std::lock_guard<std::mutex> hold(mutex_);
    try {
        boxes_.push_back(b);
    } catch (const std::bad_alloc&) {
        return store_io::out_of_memory;
    }
    b->taken = b->posted.load(std::memory_order_acquire);
    if (!writer_.joinable()) {
        try {
            writer_ = std::thread([this] { write_loop(); });
        } catch (const std::system_error&) {
            boxes_.pop_back();
            *why = "the security store's writer couldn't start";
            return store_io::refused;
        } catch (const std::bad_alloc&) {
            boxes_.pop_back();
            return store_io::out_of_memory;
        }
    }
    return store_io::ok;
}

void store_owner::detach(settle_box* b) noexcept
{
    const std::lock_guard<std::mutex> hold(mutex_);
    for (size_t i = 0; i < boxes_.size(); ++i) {
        if (boxes_[i] == b) {
            settle_held(*b);
            boxes_.erase(boxes_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

store_owner::~store_owner()
{
    {
        const std::lock_guard<std::mutex> hold(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    if (writer_.joinable()) {
        writer_.join();
    }
}

store_io store_open(const std::string& path, store_ref* out, std::string* why)
{
    store_io io = store_io::ok;
    try {
        const bool open =
            owners().open(path, out, [&](std::unique_ptr<store_owner>* made) {
                store loaded;
                io = store_load(path, &loaded, why);
                if (io != store_io::ok) {
                    return false;
                }
                *made = std::make_unique<store_owner>(path, std::move(loaded));
                return true;
            });
        return open ? store_io::ok : io;
    } catch (const std::bad_alloc&) {
        out->reset();
        return store_io::out_of_memory;
    }
}

size_t store_owners_open()
{
    return owners().open_count();
}

} // namespace ecuio_security
