// File/layer: cli/scan_line.h | havne-cli, a DID scan's progress as a line
//             (ADR-0020), and each module's session (ADR-0060)
// Testing: UNIT: CliScanTest, CliSessionTest (test_cli); CliUsageTest (ctest)
//
// uds-generic's scan of a did= range, from each channel's support report
// (ecuio_session_get_channel_support): one scan line per ECU whose counts
// changed, at most one a second while it runs, and one at once when it
// completes, so a sweep's reader sees its pace and its end. The found
// DIDs' own replies are the stream's frames (62 <DID> ...). With
// ?session=03, a session line per ECU whose session changed, and the scan
// line names the session in force; without it, neither.

#ifndef HAVNE_CLI_SCAN_LINE_H
#define HAVNE_CLI_SCAN_LINE_H

#include "jsonl.h"

#include "ecuio/core.h"

#include <cstdint>

namespace cli {

// The extended session's number (ISO 14229-1 0x10's sub-function)
constexpr uint8_t kExtendedSession = 0x03;

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
    if (s.session_asked != 0) {
        l.key("session");
        l.raw(s.session == kExtendedSession ? "\"03\"" : "\"01\"");
    }
    l.ch('}');
}

// A session state's word; "unknown" past the list
inline const char* session_state_word(uint8_t state)
{
    switch (state) {
    case ECUIO_SESSION_STATE_NOT_ASKED:
        return "not-asked";
    case ECUIO_SESSION_STATE_ENTERING:
        return "entering";
    case ECUIO_SESSION_STATE_ENTERED:
        return "entered";
    case ECUIO_SESSION_STATE_REFUSED:
        return "refused";
    case ECUIO_SESSION_STATE_UNANSWERED:
        return "unanswered";
    case ECUIO_SESSION_STATE_ABANDONED:
        return "abandoned";
    default:
        return "unknown";
    }
}

inline void session_line(Line& l, const ecuio_channel_support_t& s)
{
    l.raw("{\"type\":\"session\"");
    l.key("ecu");
    l.hex_id(s.rx_id);
    l.key("asked");
    l.raw("\"03\"");
    l.key("session");
    l.raw(s.session == kExtendedSession ? "\"03\"" : "\"01\"");
    l.key("state");
    l.ch('"');
    l.raw(session_state_word(s.session_state));
    l.ch('"');
    l.key("nrc");
    if (s.session_state == ECUIO_SESSION_STATE_REFUSED ||
        s.session_state == ECUIO_SESSION_STATE_ABANDONED) {
        const uint8_t nrc[1] = {s.session_nrc};
        l.hex_bytes(nrc, 1);
    } else {
        l.raw("null");
    }
    l.key("entries");
    l.u64(s.session_entries);
    l.key("lost");
    l.u64(s.session_lost);
    l.ch('}');
}

// What was last written of one channel's session
struct SessionSeen {
    uint8_t state = 0;
    uint8_t nrc = 0;
    uint16_t entries = 0;
    uint16_t lost = 0;
    bool written = false;
};

// Whether s earns a session line after seen: a session asked whose state,
// code or counts changed. Updates seen when it does.
inline bool session_due(const ecuio_channel_support_t& s, SessionSeen& seen)
{
    if (s.session_asked == 0 || s.generation == 0) {
        return false;
    }
    if (seen.written && s.session_state == seen.state &&
        s.session_nrc == seen.nrc && s.session_entries == seen.entries &&
        s.session_lost == seen.lost) {
        return false;
    }
    seen.state = s.session_state;
    seen.nrc = s.session_nrc;
    seen.entries = s.session_entries;
    seen.lost = s.session_lost;
    seen.written = true;
    return true;
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
// found or refused (an unanswered one waits for a rescan, so --seconds
// caps the wait). False while no channel has a scan: a session that
// scans nothing has nothing to wait for, which the caller says apart.
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
