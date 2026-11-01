// File/layer: cli/scan_line.h | havne-cli, a DID scan's progress as a line (ADR-0020)
// Testing: UNIT (tests/test_cli.cpp, CliScanTest: the line's counts, a
//          change written at most once a second but a scan's end at once,
//          nothing for a channel with no scan, and when --until-scanned
//          ends); CliUsageTest for the option's bad forms; end to end by
//          hand against the simulated ECU on vcan0 (WSL)
//
// uds-generic's scan of a did= range, from each channel's support report
// (ecuio_session_get_channel_support): one scan line per ECU whose counts
// changed, at most one a second while it runs, and one at once when it
// completes, so a sweep's reader sees its pace and its end. The found
// DIDs' own replies are the stream's frames (62 <DID> ...).

#ifndef HAVNE_CLI_SCAN_LINE_H
#define HAVNE_CLI_SCAN_LINE_H

#include "jsonl.h"

#include "ecuio/core.h"

#include <cstdint>

namespace cli {

// A scan line's least spacing while the scan runs, in microseconds
constexpr uint64_t kScanLineEveryUs = 1000000;

inline void scan_line(Line& l, const ecuio_channel_support_t& s)
{
    l.raw("{\"type\":\"scan\"");
    l.key("ecu");
    l.hex_id(s.rx_id);
    l.key("total");
    l.u64(s.scan_total);
    l.key("done");
    l.u64(s.scan_done);
    l.key("found");
    l.u64(s.scan_found);
    l.key("refused");
    l.u64(s.scan_refused);
    l.key("left");
    l.u64(s.scan_total > s.scan_done ? s.scan_total - s.scan_done : 0);
    l.key("polled");
    l.u64(s.scan_polled);
    l.key("complete");
    l.raw((s.flags & ECUIO_SUPPORT_SCAN_COMPLETE) ? "true" : "false");
    l.ch('}');
}

// What was last written for one channel
struct ScanSeen {
    uint32_t done = 0;
    uint32_t found = 0;
    bool complete = false;
    bool written = false;
    uint64_t at_us = 0;
};

// Whether s, read at now_us, earns a line after seen: a channel with a
// scan whose counts or end changed, at once for its end or when final
// (the stream's last read), else no sooner than kScanLineEveryUs after
// the last. Updates seen when it does.
inline bool scan_due(const ecuio_channel_support_t& s, ScanSeen& seen,
                     uint64_t now_us, bool final = false)
{
    if (s.scan_total == 0) {
        return false;
    }
    const bool complete = (s.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
    const bool changed = !seen.written || s.scan_done != seen.done ||
                         s.scan_found != seen.found || complete != seen.complete;
    if (!changed) {
        return false;
    }
    const bool ended = complete && !seen.complete;
    if (seen.written && !ended && !final &&
        now_us - seen.at_us < kScanLineEveryUs) {
        return false;
    }
    seen.done = s.scan_done;
    seen.found = s.scan_found;
    seen.complete = complete;
    seen.written = true;
    seen.at_us = now_us;
    return true;
}

// Whether any channel has a scan
inline bool scans_any(const ecuio_channel_support_t* s, uint32_t count)
{
    for (uint32_t c = 0; c < count; ++c) {
        if (s[c].scan_total > 0) {
            return true;
        }
    }
    return false;
}

// --until-scanned: every channel with a scan has completed it, each DID
// found or refused (one that went unanswered waits for a rescan, so a
// sweep caps the wait with --seconds). False while none has one: a
// session that scans nothing has nothing to wait for, which the caller
// says apart.
inline bool scans_complete(const ecuio_channel_support_t* s, uint32_t count)
{
    bool any = false;
    for (uint32_t c = 0; c < count; ++c) {
        if (s[c].scan_total == 0) {
            continue;
        }
        any = true;
        if ((s[c].flags & ECUIO_SUPPORT_SCAN_COMPLETE) == 0) {
            return false;
        }
    }
    return any;
}

} // namespace cli

#endif // HAVNE_CLI_SCAN_LINE_H
