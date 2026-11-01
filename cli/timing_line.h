// File/layer: cli/timing_line.h | havne-cli, a request's reply timing as a line (ADR-0042)
// Testing: UNIT (tests/test_cli.cpp, CliTimingTest: the keys are
//          ECUIO_TIMING_COLUMNS in order, a quantile with too few samples
//          is null, a snapshot carries the rows that changed)
//
// Reply timing's snapshots: every 10 s of session time and once at the
// end, one timing line for each request whose counts changed since the
// last, its figures cumulative from the connect, so the last line for a
// request is its total. A recording's latency.csv holds the same rows
// under the same names (ECUIO_TIMING_COLUMNS, the one list).

#ifndef HAVNE_CLI_TIMING_LINE_H
#define HAVNE_CLI_TIMING_LINE_H

#include "jsonl.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"

#include <cstdint>
#include <cstdio>

namespace cli {

// The interval's clock as the line and latency.csv name it
inline const char* timing_clock_name(uint32_t clock)
{
    switch (clock) {
    case ECUIO_TIMING_CLOCK_HOST:
        return "host";
    case ECUIO_TIMING_CLOCK_ADAPTER:
        return "adapter";
    case ECUIO_TIMING_CLOCK_KERNEL:
        return "kernel";
    default:
        return "none";
    }
}

// A quantile, or null when it has too few samples (its flag clear)
inline void timing_quantile(Line& l, const char* key, uint32_t us, bool has)
{
    l.key(key);
    if (has) {
        l.u64(us);
    } else {
        l.raw("null");
    }
}

// One row of a snapshot taken at snapshot_us (session time), the
// channel's ECU answering on rx_id
inline void timing_line(Line& l, uint64_t snapshot_us, uint32_t rx_id,
                        const ecuio_request_timing_t& r, uint32_t clock)
{
    char t[32];
    std::snprintf(t, sizeof t, "%.3f", static_cast<double>(snapshot_us) / 1e6);
    const uint8_t length = r.length <= 3 ? r.length : 3;
    l.raw("{\"type\":\"timing\"");
    l.key("t_s");
    l.raw(t);
    l.key("ecu");
    l.hex_id(rx_id);
    l.key("request");
    l.hex_bytes(r.request, length);
    l.key("answered");
    l.u64(r.answered);
    l.key("refused");
    l.u64(r.refused);
    l.key("timeouts");
    l.u64(r.timeouts);
    l.key("packed");
    l.u64(r.packed);
    const bool latency = r.answered + r.refused > 0;
    timing_quantile(l, "latency_p50_us", r.latency_p50_us, latency);
    timing_quantile(l, "latency_p99_us", r.latency_p99_us,
                    (r.flags & ECUIO_TIMING_LATENCY_P99) != 0);
    timing_quantile(l, "latency_p999_us", r.latency_p999_us,
                    (r.flags & ECUIO_TIMING_LATENCY_P999) != 0);
    timing_quantile(l, "latency_max_us", r.latency_max_us, latency);
    l.key("intervals");
    l.u64(r.intervals);
    l.key("intervals_mixed");
    l.u64(r.intervals_mixed);
    const bool interval = r.intervals > 0;
    timing_quantile(l, "interval_p50_us", r.interval_p50_us, interval);
    timing_quantile(l, "interval_p99_us", r.interval_p99_us,
                    (r.flags & ECUIO_TIMING_INTERVAL_P99) != 0);
    timing_quantile(l, "interval_p999_us", r.interval_p999_us,
                    (r.flags & ECUIO_TIMING_INTERVAL_P999) != 0);
    timing_quantile(l, "interval_max_us", r.interval_max_us, interval);
    l.key("interval_clock");
    l.string(timing_clock_name(clock), SIZE_MAX);
    l.ch('}');
}

// When a snapshot is due and which rows it carries (ADR-0042 section 3)
class TimingSnapshots
{
public:
    static constexpr uint64_t kEveryUs = 10u * 1000 * 1000;

    // A snapshot every 10 s of session time
    bool due(uint64_t snapshot_us) const { return snapshot_us >= m_next_us; }
    void taken(uint64_t snapshot_us) { m_next_us = snapshot_us + kEveryUs; }

    // Whether row i's counts changed since the last snapshot that carried
    // it; true marks it carried
    bool changed(uint32_t i, const ecuio_request_timing_t& r)
    {
        if (i >= ECUIO_PROFILE_MAX_REQUESTS) {
            return false;
        }
        const uint64_t total = uint64_t{r.answered} + r.refused + r.timeouts;
        if (total == m_totals[i]) {
            return false;
        }
        m_totals[i] = total;
        return true;
    }

private:
    uint64_t m_next_us = kEveryUs;
    uint64_t m_totals[ECUIO_PROFILE_MAX_REQUESTS] = {};
};

} // namespace cli

#endif /* HAVNE_CLI_TIMING_LINE_H */
