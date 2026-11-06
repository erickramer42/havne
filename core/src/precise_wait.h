// File/layer: core/src/precise_wait.h | C++17, Qt-free, Win32-guarded
// Testing: UNIT: CoreTest (test_core); HARNESS: J2534HealthTest (test_j2534_health), Elm327SessionTest (test_elm327)
//
// A requesting worker's short waits, to the engine's floor between two
// requests, ended on time. Windows rounds a plain sleep up to its timer's
// step, 15.6 ms by default: against the fakes, whose empty reads return
// at once, the floor's 20 ms came to two steps, 31 ms between requests
// on each channel, however many channels. A waitable timer made with
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803 and later) ends
// within about a millisecond of its time and changes nothing for the
// rest of the process, unlike timeBeginPeriod, which Windows 11 stops
// honoring for a window-owning process that can't be seen (the app
// minimized; ADR-0042's appendix of 2026-11-01). One timer per worker,
// made at its start. Where the flag is refused, or off Windows (whose
// sleeps already end on time), the wait is std::this_thread::sleep_for:
// never shorter than asked, at the system's step.

#ifndef ECUIO_PRECISE_WAIT_H
#define ECUIO_PRECISE_WAIT_H

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

class ecuio_precise_wait
{
public:
    /* ask: make the high-resolution timer (false: always the plain
     * sleep, which the fallback's test uses) */
    explicit ecuio_precise_wait(bool ask = true)
    {
#ifdef _WIN32
        if (ask) {
            m_timer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS);
        }
#else
        (void)ask;
#endif
    }
    ~ecuio_precise_wait()
    {
#ifdef _WIN32
        if (m_timer != nullptr) {
            CloseHandle(m_timer);
        }
#endif
    }
    ecuio_precise_wait(const ecuio_precise_wait&) = delete;
    ecuio_precise_wait& operator=(const ecuio_precise_wait&) = delete;

    /* Whether the waits run on the high-resolution timer */
    bool high_resolution() const
    {
#ifdef _WIN32
        return m_timer != nullptr;
#else
        return false;
#endif
    }

    /* Waits d, never less; on the high-resolution timer when there is
     * one, else (or if setting it fails) a plain sleep */
    void sleep_for(std::chrono::microseconds d)
    {
        if (d.count() <= 0) {
            return;
        }
#ifdef _WIN32
        if (m_timer != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -10LL * d.count(); /* relative, in 100 ns */
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0) {
                return;
            }
        }
#endif
        std::this_thread::sleep_for(d);
    }

    /* Waits until `until` in waits of at most `piece`, looking at stop
     * between them, so a stop is seen within a piece however long the
     * wait; true when it ended at a stop */
    bool
    sleep_until(std::chrono::steady_clock::time_point until,
                const std::atomic<bool>& stop,
                std::chrono::microseconds piece = std::chrono::milliseconds(20))
    {
        for (;;) {
            if (stop.load()) {
                return true;
            }
            const auto left = until - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return false;
            }
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(left);
            sleep_for(us < piece ? us : piece);
        }
    }

private:
#ifdef _WIN32
    HANDLE m_timer = nullptr;
#endif
};

#endif /* ECUIO_PRECISE_WAIT_H */
