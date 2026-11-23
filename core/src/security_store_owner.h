// File/layer: core/src/security_store_owner.h | internal, C++17, Qt-free,
//             every platform
// Testing: UNIT: SecurityStoreTest, ConversationSessionTest (test_core)
//
// The one owner of a security store's file in the process (the rules in
// security_store.h): every caller naming a path reaches its store through
// one owner (path_registry.h), one step at a time under the owner's mutex,
// and each step that changes an entry saves at once. An entry is named by
// its key, never by a pointer, and each step answers the entry as found
// again after its save, a merge's larger counts with it.
//
// A save waits up to kStoreLockWaitMs for another program's lock: a step
// is called off any thread with a deadline. A session's worker never
// calls one: it posts each attempt's end to a settle box, and the owner's
// writer thread saves it (the next mark saves every box first, under the
// mutex, so a mark never overtakes a settle). The post takes no lock and
// waits for nothing; a box holds one end at a time, since the next mark
// on its channel takes it first.

#ifndef ECUIO_SECURITY_STORE_OWNER_H
#define ECUIO_SECURITY_STORE_OWNER_H

#include "path_registry.h"
#include "security_store.h"

#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ecuio_security {

/* An entry's key: store_vehicle_of's, the module's response ID, the seed
 * request's sub-function */
struct store_key {
    uint64_t vehicle = 0;
    uint32_t rx = 0;
    uint8_t level = 0;
};

/* One channel's attempt end on its way to the store: the worker writes
 * the fields, then moves posted on (release); the owner reads them once
 * posted differs from taken */
struct settle_box {
    std::atomic<uint32_t> posted{0};
    uint32_t taken = 0; /* the owner's, under its mutex */
    store_key key;
    bool unlocked = false; /* the right key; else ended's failed and delay */
    bool failed = false;
    bool delay = false;
    int64_t utc = 0;
};

class store_owner
{
public:
    store_owner(std::string path, store loaded)
        : path_(std::move(path)), store_(std::move(loaded))
    {}
    store_owner(const store_owner&) = delete;
    store_owner& operator=(const store_owner&) = delete;
    /* Saves what the boxes hold, then joins the writer */
    ~store_owner();

    /* b's ends are saved by this owner's writer from now (started at the
     * first box): refused in words when the thread can't start,
     * out_of_memory; nothing kept either way */
    store_io attach(settle_box* b, std::string* why);

    /* b's end still posted is saved, and b is let go; called once its
     * worker has stopped */
    void detach(settle_box* b) noexcept;

    /* The worker's call after a post: the writer looks at once, no lock */
    void wake() noexcept { wake_.notify_one(); }

    /* The key's entry as the store holds it: zero counts when it has none.
     * Allocates nothing. */
    store_entry counts(const store_key& key);

    /* An attempt begins at utc: the mark, saved. refused with nothing
     * marked when the store is full ("the security store holds 256
     * entries and has no room") or the save is refused (its words), so
     * the attempt must not start; out_of_memory with nothing marked.
     * *after (may be NULL) the entry once saved. */
    store_io began(const store_key& key, int64_t utc, store_entry* after,
                   std::string* why);

    /* The attempt ended (store_attempt_ended's failed and delay), saved.
     * A refused save keeps the end in memory for the next save, and the
     * file's mark counts it once at a load in between. */
    store_io ended(const store_key& key, bool failed, bool delay, int64_t utc,
                   store_entry* after, std::string* why);

    /* The right key: the entry's counts gone, saved as ended's are */
    store_io unlocked(const store_key& key, store_entry* after,
                      std::string* why);

private:
    store_io save_and_find(const store_key& key, store_entry* after,
                           std::string* why);
    store_io ended_held(const store_key& key, bool failed, bool delay,
                        int64_t utc, store_entry* after, std::string* why);
    store_io unlocked_held(const store_key& key, store_entry* after,
                           std::string* why);
    void settle_held(settle_box& b) noexcept;
    void settle_all_held() noexcept;
    void write_loop() noexcept;

    std::mutex mutex_;
    const std::string path_;
    store store_;
    std::vector<settle_box*> boxes_; /* under mutex_ */
    std::condition_variable wake_;
    bool stop_ = false; /* under mutex_ */
    std::thread writer_;
};

using store_ref = ecuio_files::path_registry<store_owner>::ref;

/* The owner of the store at path into *out (released first), the file
 * loaded by the path's first open: refused with store_load's words when
 * it doesn't load, out_of_memory, each with *out empty and nothing open */
store_io store_open(const std::string& path, store_ref* out, std::string* why);

/* TEST-ONLY: how many paths have an owner */
size_t store_owners_open();

} // namespace ecuio_security

#endif /* ECUIO_SECURITY_STORE_OWNER_H */
