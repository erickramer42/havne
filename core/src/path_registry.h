// File/layer: core/src/path_registry.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: SecurityStoreTest (test_core, through store_open)
//
// One object for each path the process has open, for a file core owns and
// rewrites whole: the first caller to name a path makes it, every later
// one shares it, and the last one's release destroys it. Both happen under
// the registry's mutex, so an open of a path waits until the path's last
// object is gone and reads the file it left. A path spelled two ways is
// two objects: the file's own lock and the merge keep those safe (the
// security store's rules, security_store.h).

#ifndef ECUIO_PATH_REGISTRY_H
#define ECUIO_PATH_REGISTRY_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_files {

template <typename T> class path_registry
{
public:
    /* A caller's share of a path's object: released at its end */
    class ref
    {
    public:
        ref() = default;
        ~ref() { reset(); }
        ref(ref&& other) noexcept
            : registry_(other.registry_), object_(other.object_)
        {
            other.registry_ = nullptr;
            other.object_ = nullptr;
        }
        ref& operator=(ref&& other) noexcept
        {
            if (this != &other) {
                reset();
                std::swap(registry_, other.registry_);
                std::swap(object_, other.object_);
            }
            return *this;
        }
        ref(const ref&) = delete;
        ref& operator=(const ref&) = delete;

        void reset() noexcept
        {
            if (object_ != nullptr) {
                registry_->release(object_);
            }
            registry_ = nullptr;
            object_ = nullptr;
        }
        T* get() const { return object_; }
        T* operator->() const { return object_; }
        explicit operator bool() const { return object_ != nullptr; }

    private:
        friend class path_registry;
        ref(path_registry* registry, T* object)
            : registry_(registry), object_(object)
        {}
        path_registry* registry_ = nullptr;
        T* object_ = nullptr;
    };

    path_registry() = default;
    path_registry(const path_registry&) = delete;
    path_registry& operator=(const path_registry&) = delete;

    /* The path's object into *out (released first), made by make(&made),
     * a std::unique_ptr<T>*, when none is open. make answers false with
     * nothing made, and open answers false then with *out empty. Throws
     * std::bad_alloc, from make too, with nothing open. make runs under
     * the registry's mutex: it must not open or release another path. */
    template <typename Make>
    bool open(const std::string& path, ref* out, Make make)
    {
        out->reset();
        const std::lock_guard<std::mutex> hold(mutex_);
        for (slot& s : slots_) {
            if (s.path == path) {
                ++s.refs;
                *out = ref(this, s.object.get());
                return true;
            }
        }
        slot made;
        made.path = path;
        slots_.reserve(slots_.size() + 1);
        if (!make(&made.object) || made.object == nullptr) {
            return false;
        }
        made.refs = 1;
        T* object = made.object.get();
        slots_.push_back(std::move(made)); /* reserved: no allocation */
        *out = ref(this, object);
        return true;
    }

    /* The paths open now */
    size_t open_count()
    {
        const std::lock_guard<std::mutex> hold(mutex_);
        return slots_.size();
    }

private:
    struct slot {
        std::string path;
        std::unique_ptr<T> object;
        size_t refs = 0;
    };

    /* The last share's release destroys the object under the mutex, so an
     * open of its path comes after whatever its end writes */
    void release(T* object) noexcept
    {
        const std::lock_guard<std::mutex> hold(mutex_);
        for (size_t k = 0; k < slots_.size(); ++k) {
            if (slots_[k].object.get() != object) {
                continue;
            }
            if (--slots_[k].refs == 0) {
                slots_[k].object.reset();
                if (k + 1 != slots_.size()) {
                    slots_[k] = std::move(slots_.back());
                }
                slots_.pop_back();
            }
            return;
        }
    }

    std::mutex mutex_;
    std::vector<slot> slots_;
};

} // namespace ecuio_files

#endif /* ECUIO_PATH_REGISTRY_H */
