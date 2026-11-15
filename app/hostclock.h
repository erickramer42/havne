// File/layer: app/hostclock.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#pragma once

#include <QtGlobal>

#include <chrono>

#include "ecuio/core.h"

// ecuio_clock_us as a chrono clock: the bridge stamps a frame's and a
// value's arrival with it, for the delivery figures
struct HostClock {
    using duration = std::chrono::microseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<HostClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept
    {
        return time_point(duration(static_cast<rep>(ecuio_clock_us())));
    }
};

// QElapsedTimer's restart() and elapsed() on core's clock (ecuio_clock_us),
// which counts the time the machine sleeps (ADR-0033): a value read before
// a sleep is as old as it is at the wake, and dims then
class HostElapsedTimer
{
public:
    void restart() { m_startUs = ecuio_clock_us(); }
    qint64 elapsed() const
    {
        return static_cast<qint64>((ecuio_clock_us() - m_startUs) / 1000);
    }

private:
    uint64_t m_startUs = ecuio_clock_us();
};
