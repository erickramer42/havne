// File/layer: core/src/host_clock.cpp | C++17, Qt-free; one clock per platform
// Testing: UNIT: CoreTest.Clock* (never back, advances by a sleep_for,
//          reads the platform's clock); BENCH: a real sleep
//          (docs/bench-sessions/bench-prep.md, BENCH prep #27)
//
// The host clock session time runs on (ADR-0033 section 1). Each platform's
// clock is named outright rather than left to steady_clock's choice, so
// counting sleep rests on documented OS behavior: CLOCK_BOOTTIME on
// Linux and Android, QueryPerformanceCounter on Windows ("including the
// time when the machine was in a sleep state"), CLOCK_MONOTONIC_RAW on
// Apple. The slept count is the difference between that clock and the
// platform's clock that stops in sleep, read stop-first so it never comes
// out below the true count.

#include "ecuio/core.h"

#include <atomic>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <time.h>
#elif defined(__linux__)
#include <time.h>
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#endif

namespace {

/* Two reads of two clocks aren't one instant, so the difference wobbles
 * by the time between them; the highest seen keeps it from going back */
[[maybe_unused]] uint64_t never_back(uint64_t slept)
{
    static std::atomic<uint64_t> high{0};
    uint64_t seen = high.load(std::memory_order_relaxed);
    while (slept > seen && !high.compare_exchange_weak(
                               seen, slept, std::memory_order_relaxed)) {
    }
    return slept > seen ? slept : seen;
}

/* The counting clock minus the stopped one, the stopped one read before and
 * after (microseconds each): a thread held between the reads would raise
 * the difference for good, so a bracket over 1 ms is read again, and the
 * later stopped read keeps a hold from ever overstating it */
template <typename Awake, typename All>
uint64_t slept_between(Awake awake, All all)
{
    uint64_t before = 0;
    uint64_t counting = 0;
    uint64_t after = 0;
    for (int tries = 0; tries < 4; ++tries) {
        before = awake();
        counting = all();
        after = awake();
        if (after - before < 1000) {
            break;
        }
    }
    return counting > after ? counting - after : 0;
}

#if defined(_WIN32)

uint64_t qpc_us()
{
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t ticks = static_cast<uint64_t>(c.QuadPart);
    /* Whole seconds first: ticks * 1e6 would overflow after ~30 days */
    return ticks / freq * 1000000u + ticks % freq * 1000000u / freq;
}

/* Interrupt time, biased (counts sleep) and unbiased (doesn't), in
 * 100 ns: both from one source, so their difference is the sleep alone.
 * Exported by kernelbase.dll from Windows 10; looked up so a build
 * needs no umbrella library */
using interrupt_time_fn = VOID(WINAPI*)(PULONGLONG);
struct interrupt_clocks {
    interrupt_time_fn biased = nullptr;
    interrupt_time_fn unbiased = nullptr;
};

const interrupt_clocks& interrupt_time()
{
    static const interrupt_clocks clocks = [] {
        interrupt_clocks c;
        HMODULE mod = GetModuleHandleW(L"kernelbase.dll");
        if (mod != nullptr) {
            c.biased = reinterpret_cast<interrupt_time_fn>(
                GetProcAddress(mod, "QueryInterruptTimePrecise"));
            c.unbiased = reinterpret_cast<interrupt_time_fn>(
                GetProcAddress(mod, "QueryUnbiasedInterruptTimePrecise"));
        }
        return c;
    }();
    return clocks;
}

#elif defined(__linux__) && !defined(__APPLE__)

uint64_t read_us(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

#endif

} // namespace

extern "C" uint64_t ecuio_clock_us(void)
{
#if defined(_WIN32)
    return qpc_us();
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000u;
#elif defined(__linux__)
    return read_us(CLOCK_BOOTTIME);
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

extern "C" uint64_t ecuio_clock_slept_us(void)
{
#if defined(_WIN32)
    const interrupt_clocks& c = interrupt_time();
    if (c.biased == nullptr || c.unbiased == nullptr) {
        return 0;
    }
    return never_back(slept_between(
        [&] {
            ULONGLONG t = 0;
            c.unbiased(&t);
            return static_cast<uint64_t>(t / 10u);
        },
        [&] {
            ULONGLONG t = 0;
            c.biased(&t);
            return static_cast<uint64_t>(t / 10u);
        }));
#elif defined(__APPLE__)
    return never_back(slept_between(
        [] { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u; },
        [] { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000u; }));
#elif defined(__linux__)
    return never_back(slept_between([] { return read_us(CLOCK_MONOTONIC); },
                                    [] { return read_us(CLOCK_BOOTTIME); }));
#else
    return 0;
#endif
}
