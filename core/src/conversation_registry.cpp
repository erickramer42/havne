// File/layer: core/src/conversation_registry.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: ConversationRegistryTest (test_core)
//
// The table of conversation_registry.h: a short list by set pointer under
// a mutex of its own, never held across another lock.

#include "conversation_registry.h"

#include <mutex>
#include <new>
#include <utility>

namespace {

struct kept_part {
    const ecuio_signal_set_t* set = nullptr;
    ecuio_conversation_part_ref part;
};

std::mutex& table_mutex()
{
    static std::mutex m;
    return m;
}

std::vector<kept_part>& table()
{
    static std::vector<kept_part> t;
    return t;
}

kept_part* find(const ecuio_signal_set_t* set)
{
    for (kept_part& k : table()) {
        if (k.set == set) {
            return &k;
        }
    }
    return nullptr;
}

} // namespace

ecuio_status_t ecuio_conversation_keep(const ecuio_signal_set_t* set,
                                       ecuio_conversation_part_ref part,
                                       bool* added) noexcept
{
    *added = false;
    if (set == nullptr || part == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(table_mutex());
    if (kept_part* k = find(set)) {
        k->part = std::move(part);
        return ECUIO_OK;
    }
    try {
        table().push_back(kept_part{set, std::move(part)});
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *added = true;
    return ECUIO_OK;
}

void ecuio_conversation_forget(const ecuio_signal_set_t* set) noexcept
{
    ecuio_conversation_part_ref last; /* freed after the lock */
    std::lock_guard<std::mutex> lock(table_mutex());
    std::vector<kept_part>& t = table();
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].set == set) {
            last = std::move(t[i].part);
            t[i] = std::move(t.back());
            t.pop_back();
            return;
        }
    }
}

ecuio_conversation_part_ref
ecuio_conversation_of(const ecuio_signal_set_t* set) noexcept
{
    std::lock_guard<std::mutex> lock(table_mutex());
    const kept_part* k = find(set);
    return k == nullptr ? nullptr : k->part;
}

size_t ecuio_conversation_kept() noexcept
{
    std::lock_guard<std::mutex> lock(table_mutex());
    return table().size();
}
