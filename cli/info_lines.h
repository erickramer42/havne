// File/layer: cli/info_lines.h | havne-cli, a recording's check as lines (ADR-0049 section 6)
// Testing: UNIT: CliInfoTest (test_cli)
//
// `havne-cli info`'s own lines, around view's opened, signal and marker
// lines: what the frames check found (ecuio_recording_check), each finding
// in the order core settles it, then the summary. Times are the
// recording's host_us.

#ifndef HAVNE_CLI_INFO_LINES_H
#define HAVNE_CLI_INFO_LINES_H

#include "jsonl.h"
#include "view_lines.h"

#include "ecuio/core.h"
#include "ecuio/recording.h"

#include <cmath>
#include <cstdint>

namespace cli {

inline const char* check_kind_word(uint32_t kind)
{
    switch (kind) {
    case ECUIO_CHECK_GAP:
        return "gap";
    case ECUIO_CHECK_RATE_DROP:
        return "rate-drop";
    case ECUIO_CHECK_LATE_ID:
        return "late-id";
    case ECUIO_CHECK_OVERFLOW:
        return "overflow";
    case ECUIO_CHECK_ISOTP:
        return "isotp";
    case ECUIO_CHECK_LINK:
        return "link";
    case ECUIO_CHECK_ASLEEP:
        return "asleep";
    default:
        return "unknown";
    }
}

// Whether the check read ISO-TP sequences, or why not
inline const char* isotp_word(uint32_t summary_flags)
{
    if ((summary_flags & ECUIO_CHECK_SKIPPED_ISOTP) == 0) {
        return "checked";
    }
    if ((summary_flags & ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN) != 0) {
        return "flags-unknown";
    }
    if ((summary_flags & ECUIO_CHECK_ISOTP_NO_WIRE) != 0) {
        return "no-wire";
    }
    return "skipped";
}

// --rate-drop: a multiple of an ID's median interval, more than 1, as the
// check's hundredths
inline bool parse_rate_drop(const char* text, uint32_t& x100)
{
    double v = 0;
    if (!parse_decimal(text, v) || v > 4e7) {
        return false;
    }
    const long long n = std::llround(v * 100);
    if (n <= 100) {
        return false;
    }
    x100 = static_cast<uint32_t>(n);
    return true;
}

// The reader couldn't open the folder; the check still runs:
// {"type":"open_failed", recording, code, message, detail}
inline void open_failed_line(Line& l, uint32_t recording, ecuio_status_t status,
                             const char* detail)
{
    l.raw("{\"type\":\"open_failed\"");
    l.key("recording");
    l.u64(recording);
    l.key("code");
    l.u64(static_cast<uint32_t>(status));
    l.key("message");
    l.string(ecuio_status_string(status), SIZE_MAX);
    l.key("detail");
    l.string(detail, SIZE_MAX, 1024);
    l.ch('}');
}

// {"type":"finding", kind, id (null for the whole bus's), extended,
// at_us, length_us, count, and a link's code and message}
inline void finding_line(Line& l, const ecuio_check_finding_t& f)
{
    l.raw("{\"type\":\"finding\"");
    l.key("kind");
    l.string(check_kind_word(f.kind), SIZE_MAX);
    l.key("id");
    if ((f.flags & ECUIO_CHECK_FINDING_WHOLE_BUS) != 0) {
        l.raw("null");
    } else {
        l.hex_id(f.arbitration_id);
    }
    l.key("extended");
    l.raw((f.flags & ECUIO_CHECK_FINDING_EXTENDED) != 0 ? "true" : "false");
    l.key("at_us");
    l.u64(f.at_us);
    l.key("length_us");
    l.u64(f.length_us);
    l.key("count");
    l.u64(f.count);
    if (f.kind == ECUIO_CHECK_LINK) {
        l.key("code");
        l.number(f.code);
        l.key("message");
        l.string(ecuio_status_string(static_cast<ecuio_status_t>(f.code)),
                 SIZE_MAX);
    }
    l.ch('}');
}

// The check's summary, last: {"type":"summary", duration_us, frames, ids,
// torn_rows, isotp (checked, flags-unknown or no-wire), findings {kind:
// count}, gap_us, rate_drop, settle_us}
inline void summary_line(Line& l, const ecuio_check_summary_t& s,
                         const ecuio_check_options_t& o)
{
    l.raw("{\"type\":\"summary\"");
    l.key("duration_us");
    l.u64(s.duration_us);
    l.key("frames");
    l.u64(s.frames);
    l.key("ids");
    l.u64(s.ids);
    l.key("torn_rows");
    l.u64(s.torn_rows);
    l.key("isotp");
    l.string(isotp_word(s.flags), SIZE_MAX);
    l.key("findings");
    l.ch('{');
    for (uint32_t k = ECUIO_CHECK_GAP; k <= ECUIO_CHECK_ASLEEP; ++k) {
        l.key(check_kind_word(k));
        l.u64(s.counts[k]);
    }
    l.ch('}');
    l.key("gap_us");
    l.u64(o.gap_us);
    l.key("rate_drop");
    l.number(o.rate_drop_x100 / 100.0);
    l.key("settle_us");
    l.u64(o.settle_us);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_INFO_LINES_H
