// File/layer: cli/frames_lines.h | havne-cli, a recording's frames read
//             back as recorded (ADR-0049's appendix "frames read back")
// Testing: UNIT: CliFramesTest (test_cli)
//
// `frames <recording> [--from S] [--to S]`: its arguments, a `frame` line
// for each frames.csv row in the stretch (ecuio_recording_read_frames),
// and the `end` line. A row's line is the stream's frame line less `ch`
// (frames.csv keeps no channel) plus `host_us`, the time view's and
// export's --from and --to count on; nothing is decoded.

#ifndef HAVNE_CLI_FRAMES_LINES_H
#define HAVNE_CLI_FRAMES_LINES_H

#include "jsonl.h"
#include "view_lines.h" /* parse_seconds */

#include "ecuio/core.h"
#include "ecuio/recording.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace cli {

struct FramesArgs {
    const char* folder = nullptr;
    uint64_t from_us = 0;
    uint64_t to_us = 0; /* 0: to the end */
};

// The arguments after `frames`; false with *why, a usage error. Both
// edges are included, so --to may equal --from; core's 0 is the end, so
// --to takes a time after 0
inline bool parse_frames(int argc, char** argv, FramesArgs& a, std::string* why)
{
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        if ((s == "--from" || s == "--to") && i + 1 < argc) {
            int64_t us = 0;
            if (!parse_seconds(argv[++i], us) || us < 0) {
                *why = s + " takes seconds, 0 or more";
                return false;
            }
            if (s == "--from") {
                a.from_us = static_cast<uint64_t>(us);
            } else if (us == 0) {
                *why = "--to takes seconds after 0";
                return false;
            } else {
                a.to_us = static_cast<uint64_t>(us);
            }
        } else if (s.rfind("--", 0) == 0) {
            *why = "unexpected '" + s + "'";
            return false;
        } else if (a.folder == nullptr) {
            a.folder = argv[i];
        } else {
            *why = "frames takes one recording";
            return false;
        }
    }
    if (a.folder == nullptr) {
        *why = "frames needs a recording's folder";
        return false;
    }
    if (a.to_us != 0 && a.to_us < a.from_us) {
        *why = "--to comes before --from";
        return false;
    }
    return true;
}

// {"type":"frame", host_us, t_us, id, flags, data}: data in frames.csv's
// form; a line of a whole frame's bytes needs about 3 x
// ECUIO_FRAME_DATA_MAX plus 128
inline void frame_row_line(Line& l, const ecuio_frame_t& f, uint64_t host_us)
{
    l.raw("{\"type\":\"frame\"");
    l.key("host_us");
    l.u64(host_us);
    l.key("t_us");
    l.u64(f.timestamp_us);
    l.key("id");
    l.hex_id(f.arbitration_id);
    l.key("flags");
    l.u64(f.flags);
    l.key("data");
    l.hex_bytes(f.data, f.data_length < ECUIO_FRAME_DATA_MAX
                            ? f.data_length
                            : ECUIO_FRAME_DATA_MAX);
    l.ch('}');
}

// {"type":"end", reason read|interrupted, rows, torn_rows}: rows the
// frame lines written, torn_rows a last row cut short and left out
inline void frames_end_line(Line& l, const char* reason,
                            const ecuio_frames_read_t& s)
{
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string(reason, std::strlen(reason));
    l.key("rows");
    l.u64(s.rows);
    l.key("torn_rows");
    l.u64(s.torn_rows);
    l.ch('}');
}

} // namespace cli

#endif
