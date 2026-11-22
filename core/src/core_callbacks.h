// File/layer: core/src/core_callbacks.h | C++17, Qt-free, internal to the core_*.cpp sources
// Testing: UNIT: CoreTest (test_core): frame, values and conditions callbacks
// Internal to the core_*.cpp sources: no include from outside core/src.

#ifndef ECUIO_CORE_CALLBACKS_H
#define ECUIO_CORE_CALLBACKS_H

#include "ecuio/core.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <memory>
#include <utility>

namespace {
    /* The list a dispatch calls (ADR-0002): copied under the callback
     * mutex, called unlocked. On the stack while it fits, as the app's one
     * callback does (no allocation per frame), else on the heap. */
    template <typename Callback>
    class snapshot_of {
    public:
        using entry = std::pair<Callback, void*>;

        /* Copies list; the caller holds the mutex. False when the heap
         * copy failed: the caller drops the delivery (no bad_alloc may
         * leave an extern "C" call). */
        bool take(const std::vector<entry>& list)
        {
            count_ = list.size();
            if (count_ <= kInline) {
                for (size_t i = 0; i < count_; ++i) {
                    inline_[i] = list[i];
                }
                return true;
            }
            try {
                heap_ = list;
            } catch (...) {
                count_ = 0;
                return false;
            }
            data_ = heap_.data();
            return true;
        }

        size_t size() const { return count_; }
        const entry& operator[](size_t i) const { return data_[i]; }

    private:
        static constexpr size_t kInline = 4;
        entry inline_[kInline];
        std::vector<entry> heap_;
        const entry* data_ = inline_;
        size_t count_ = 0;
    };

    /* Adds (callback, user_data) to list once; the caller holds the
     * mutex */
    template <typename Callback>
    ecuio_status_t add_callback(std::vector<std::pair<Callback, void*>>& list,
                                Callback callback, void* user_data)
    {
        for (const auto& entry : list) {
            if (entry.first == callback && entry.second == user_data) {
                return ECUIO_OK;
            }
        }
        try {
            list.emplace_back(callback, user_data);
            return ECUIO_OK;
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
    }

    /* Removes (callback, user_data) from list, if there; the caller
     * holds the mutex */
    template <typename Callback>
    void remove_callback(std::vector<std::pair<Callback, void*>>& list,
                         Callback callback, void* user_data)
    {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->first == callback && it->second == user_data) {
                list.erase(it);
                return;
            }
        }
    }
} // namespace

#endif /* ECUIO_CORE_CALLBACKS_H */
