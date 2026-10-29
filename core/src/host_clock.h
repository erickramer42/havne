// File/layer: core/src/host_clock.h | C++17, Qt-free, header-only
// Testing: UNIT — CoreTest.Clock* (the clock it reads)
//
// ecuio_clock_us (ADR-0033) as a chrono clock, so a transport's epoch and
// the stamps it takes from it keep their chrono types. Waits stay on
// std::chrono::steady_clock: the standard's waits take no other clock.

#ifndef ECUIO_HOST_CLOCK_H
#define ECUIO_HOST_CLOCK_H

#include "ecuio/core.h"

#include <chrono>
#include <cstdint>

struct host_clock {
    using duration = std::chrono::microseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<host_clock>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept
    {
        return time_point(duration(static_cast<rep>(ecuio_clock_us())));
    }
};

#endif
