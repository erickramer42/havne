// File/layer: tests/awake_seam.h | test-only, C++17, Qt-free; header-only
// Testing: UNIT: the seam of CoreTest.*Awake* (test_core) and
//          Elm327AwakeTest (test_elm327)
//
// Stands in for the platform's keep-awake hold (ecuio_test_set_awake_platform,
// internal.h) for a scope: counts takes and releases, keeps each take's
// level and reason, and refuses with words when told to. A shut gate holds
// each take until it opens; on_release runs inside each release.

#ifndef ECUIO_TESTS_AWAKE_SEAM_H
#define ECUIO_TESTS_AWAKE_SEAM_H

#include "ecuio/core.h"
#include "ecuio/internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

struct AwakeSeam {
    static inline std::atomic<int> takes{0};
    static inline std::atomic<int> releases{0};
    static inline std::atomic<uint32_t> last_level{99};
    static inline std::mutex m;
    static inline std::string last_why;
    static inline std::string refuse; /* non-empty: the take refuses so */
    static inline std::atomic<void (*)()> on_release{nullptr};

    static inline std::mutex gate_m;
    static inline std::condition_variable gate_cv;
    static inline bool gate_shut = false;
    static inline int held = 0; /* takes waiting at the shut gate */

    AwakeSeam()
    {
        takes = 0;
        releases = 0;
        last_level = 99;
        on_release = nullptr;
        {
            std::lock_guard<std::mutex> g(m);
            last_why.clear();
            refuse.clear();
        }
        {
            std::lock_guard<std::mutex> g(gate_m);
            gate_shut = false;
            held = 0;
        }
        ecuio_test_set_awake_platform(&take, &release);
    }
    ~AwakeSeam()
    {
        open();
        off();
    }
    AwakeSeam(const AwakeSeam&) = delete;
    AwakeSeam& operator=(const AwakeSeam&) = delete;

    /* The platform's own hold again, for a case that checks it */
    static void off() { ecuio_test_set_awake_platform(nullptr, nullptr); }

    static void refuse_with(const char* words)
    {
        std::lock_guard<std::mutex> g(m);
        refuse = words;
    }

    static std::string why()
    {
        std::lock_guard<std::mutex> g(m);
        return last_why;
    }

    static void shut()
    {
        std::lock_guard<std::mutex> g(gate_m);
        gate_shut = true;
    }

    static void open()
    {
        {
            std::lock_guard<std::mutex> g(gate_m);
            gate_shut = false;
        }
        gate_cv.notify_all();
    }

    /* Whether a take is held at the shut gate within 5 s */
    static bool a_take_is_held()
    {
        std::unique_lock<std::mutex> g(gate_m);
        return gate_cv.wait_for(g, std::chrono::seconds(5),
                                [] { return held > 0; });
    }

    static ecuio_status_t take(uint32_t level, const char* why,
                               void** out_handle, const char** out_detail)
    {
        {
            std::unique_lock<std::mutex> g(gate_m);
            ++held;
            gate_cv.notify_all();
            gate_cv.wait(g, [] { return !gate_shut; });
            --held;
        }
        std::lock_guard<std::mutex> g(m);
        if (!refuse.empty()) {
            *out_detail = refuse.c_str();
            return ECUIO_ERR_NOT_IMPLEMENTED;
        }
        last_level = level;
        last_why = why != nullptr ? why : "";
        *out_handle = reinterpret_cast<void*>(
            static_cast<uintptr_t>(++takes));
        return ECUIO_OK;
    }

    static void release(void*)
    {
        if (void (*const hook)() = on_release.load()) {
            hook();
        }
        ++releases;
    }
};

#endif /* ECUIO_TESTS_AWAKE_SEAM_H */
