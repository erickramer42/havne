// File/layer: tests/worker_hold.h | test helper, C++17, Qt-free
// Testing: HARNESS: ConditionSessionTest (test_core), Elm327SessionTest
//          (test_elm327)
//
// A running session's worker parked inside a frame callback, before it
// decodes the frame, so the test thread can dispatch frames of its own
// (ecuio_session_dispatch_callbacks) with nothing of the worker's in the
// decoder or the conditions meanwhile: two threads in dispatch at once
// would race on both. Register it before the hold; it must outlive the
// session's close (the callback's user data), and the hold must end
// before the close, which joins the worker: declare a WorkerHold::Release
// after the session. The parked worker has taken the set: attach before
// the hold. An attach while it holds leaves the parked frame to the set
// before, and a second frees that set under the worker.

#ifndef ECUIO_TESTS_WORKER_HOLD_H
#define ECUIO_TESTS_WORKER_HOLD_H

#include "ecuio/core.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct WorkerHold {
    std::mutex m;
    std::condition_variable cv;
    bool armed = false;
    bool parked = false;
    std::thread::id caller = std::this_thread::get_id();

    static void on_frame(const ecuio_frame_t*, void* user)
    {
        auto* h = static_cast<WorkerHold*>(user);
        if (std::this_thread::get_id() == h->caller) {
            return; /* the test's own dispatch */
        }
        std::unique_lock<std::mutex> lock(h->m);
        if (!h->armed) {
            return;
        }
        h->parked = true;
        h->cv.notify_all();
        h->cv.wait(lock, [h] { return !h->armed; });
        h->parked = false;
    }

    ecuio_status_t attach(ecuio_session_t* s)
    {
        return ecuio_register_frame_callback(s, &WorkerHold::on_frame, this);
    }

    /* True once the worker is parked, within ms; false disarmed */
    bool hold(int ms)
    {
        std::unique_lock<std::mutex> lock(m);
        armed = true;
        if (!cv.wait_for(lock, std::chrono::milliseconds(ms),
                         [this] { return parked; })) {
            armed = false;
            return false;
        }
        return true;
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m);
        armed = false;
        cv.notify_all();
    }

    struct Release {
        WorkerHold& h;
        ~Release() { h.release(); }
    };
};

#endif /* ECUIO_TESTS_WORKER_HOLD_H */
