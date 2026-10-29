// File/layer: cli/sleep_watch.h | havne-cli, a sleep between two polls (ADR-0033 §6)
// Testing: UNIT (tests/test_cli.cpp, CliSleepTest); a real sleep BENCH
//          (BENCH prep #27)
//
// The stream polls ecuio_clock_slept_us every 100 ms. A rise of a second
// or more within one poll is a sleep, reported once; smaller rises (the
// count's own grain, a short doze) are taken in without a line.

#ifndef HAVNE_CLI_SLEEP_WATCH_H
#define HAVNE_CLI_SLEEP_WATCH_H

#include <cstdint>

namespace cli {

constexpr uint64_t kSleepReportedUs = 1000000;

class SleepWatch
{
public:
    explicit SleepWatch(uint64_t slept_us) : last_(slept_us) {}

    /* The sleep since the last poll, in microseconds, or 0 if it's under
     * a second. A count that went back (it shouldn't) is taken as is. */
    uint64_t poll(uint64_t slept_us)
    {
        const uint64_t rise = slept_us > last_ ? slept_us - last_ : 0;
        last_ = slept_us;
        return rise >= kSleepReportedUs ? rise : 0;
    }

private:
    uint64_t last_;
};

} // namespace cli

#endif
