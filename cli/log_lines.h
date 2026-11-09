// File/layer: cli/log_lines.h | havne-cli, the analysis log from the
//             command line (ADR-0056 section 7)
// Testing: UNIT: CliLogTest (test_cli); HARNESS: CliLogRunTest
//          (tests/cli_log.py), CliUsageTest.Log*
//
// `log <uri> <folder> [--signal <name>]... [--label <name>] [--seconds S]
// [--signals <file> | --plugin <pkg>]`: its arguments, the picked names
// resolved against the session's set (in the order given, which is the
// focus's and the folder's `focus` line's), the `focus` line, a `log`
// line each period of session time with the recording's size so far, and
// the `end` line. No frame or values line: the folder is the output, as
// a recording the app's Start log makes.

#ifndef HAVNE_CLI_LOG_LINES_H
#define HAVNE_CLI_LOG_LINES_H

#include "jsonl.h"

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cli {

/* The `log` line's period in session time */
constexpr uint64_t kLogEveryUs = 10 * 1000000ULL;

struct LogArgs {
    const char* uri = nullptr;
    const char* folder = nullptr;   /* the recordings' root */
    const char* label = nullptr;    /* --label */
    uint64_t seconds = 0;           /* --seconds; 0 until Ctrl+C */
    const char* signals = nullptr;  /* --signals' file */
    const char* plugin = nullptr;   /* --plugin's package */
    std::vector<const char*> names; /* --signal, in order */
};

// The longest --seconds any command takes: a year and a day, which no run
// reaches and which keeps seconds * 1e6 far inside 64 bits
constexpr uint64_t kMaxRunSeconds = 366ull * 86400ull;

// "--seconds takes ..." for the commands that run until a deadline
inline const char* run_seconds_why()
{
    return "--seconds takes a whole number from 1 to 31622400";
}

// A whole number above 0, decimal digits alone (stream's --count rule)
inline bool parse_whole(const char* text, uint64_t& out)
{
    if (text == nullptr || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v == 0) {
        return false;
    }
    out = v;
    return true;
}

// The arguments after `log`; false with *why, a usage error
inline bool parse_log(int argc, char** argv, LogArgs& a, std::string* why)
{
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        const bool has_value = i + 1 < argc;
        if (s == "--signal" && has_value) {
            const char* name = argv[++i];
            if (*name == '\0') {
                *why = "--signal takes a signal's name";
                return false;
            }
            a.names.push_back(name);
        } else if (s == "--label" && has_value) {
            a.label = argv[++i];
        } else if (s == "--seconds" && has_value) {
            if (!parse_whole(argv[++i], a.seconds) ||
                a.seconds > kMaxRunSeconds) {
                *why = run_seconds_why();
                return false;
            }
        } else if (s == "--signals" && has_value) {
            a.signals = argv[++i];
        } else if (s == "--plugin" && has_value) {
            a.plugin = argv[++i];
        } else if (s.rfind("--", 0) == 0) {
            *why = "unexpected '" + s + "'";
            return false;
        } else if (a.uri == nullptr) {
            a.uri = argv[i];
        } else if (a.folder == nullptr) {
            a.folder = argv[i];
        } else {
            *why = "log takes a URI and a folder";
            return false;
        }
    }
    if (a.uri == nullptr || a.folder == nullptr) {
        *why = "log needs a URI and a folder for the recording";
        return false;
    }
    if (*a.folder == '\0') {
        *why = "log takes a folder";
        return false;
    }
    if (a.signals != nullptr && a.plugin != nullptr) {
        *why = "--signals and --plugin each give the profile: one at a time";
        return false;
    }
    return true;
}

// The picks' ids, each name's first def in the set, in the names' order;
// false with *why for a name the set lacks or one given twice (usage: the
// names are the caller's typing, checked before any line is written)
inline bool resolve_focus(const ecuio_signal_set_t* set,
                          const std::vector<const char*>& names,
                          std::vector<uint16_t>& ids, std::string* why)
{
    ids.clear();
    for (const char* name : names) {
        const ecuio_signal_def_t* found = nullptr;
        for (uint32_t i = 0; set != nullptr && set->defs != nullptr &&
                             i < set->count && found == nullptr;
             ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (std::strncmp(d.name, name, sizeof d.name) == 0) {
                found = &d;
            }
        }
        if (found == nullptr) {
            *why = std::string("no signal named '") + name +
                   "' in the session's profile";
            return false;
        }
        for (uint16_t id : ids) {
            if (id == found->id) {
                *why = std::string("--signal names '") + name + "' twice";
                return false;
            }
        }
        ids.push_back(found->id);
    }
    return true;
}

// Room for the focus line: a def's name is at most 31 bytes, 6 a byte
// quoted, plus its id and the punctuation
inline size_t focus_line_bytes(size_t picks)
{
    return 64 + picks * 256;
}

// {"type":"focus","signals":[{id, name}...]}: the picks, in order; empty
// for none (the whole set is read)
inline void focus_line(Line& l, const ecuio_signal_set_t* set,
                       const std::vector<uint16_t>& ids)
{
    l.raw("{\"type\":\"focus\"");
    l.key("signals");
    l.ch('[');
    bool first = true;
    for (uint16_t id : ids) {
        for (uint32_t i = 0;
             set != nullptr && set->defs != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (d.id != id) {
                continue;
            }
            if (!first) {
                l.ch(',');
            }
            first = false;
            l.ch('{');
            l.key("id");
            l.u64(d.id);
            l.key("name");
            l.string(d.name, sizeof d.name);
            l.ch('}');
            break;
        }
    }
    l.raw("]}");
}

// What a log line and the end line say of the recording so far
struct LogReadout {
    uint64_t seconds = 0; /* session time since start, whole */
    uint64_t bytes = 0;   /* the folder's files */
    uint64_t values = 0;  /* rows written to signals.csv */
};

// {"type":"log", seconds, bytes, values}
inline void log_line(Line& l, const LogReadout& r)
{
    l.raw("{\"type\":\"log\"");
    l.key("seconds");
    l.u64(r.seconds);
    l.key("bytes");
    l.u64(r.bytes);
    l.key("values");
    l.u64(r.values);
    l.ch('}');
}

inline size_t log_end_line_bytes(const char* folder)
{
    return 192 + Line::string_bytes(folder);
}

// {"type":"end", reason, folder, seconds, bytes, values}: reason is
// interrupted, seconds, recording-failed, replay-end or output-failed; the
// recording's folder and its last readout. A null folder leaves the key
// out: the line the recording keeps of itself names no path
inline void log_end_line(Line& l, const char* reason, const char* folder,
                         const LogReadout& r)
{
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string(reason, std::strlen(reason));
    if (folder != nullptr) {
        l.key("folder");
        l.string(folder, std::strlen(folder));
    }
    l.key("seconds");
    l.u64(r.seconds);
    l.key("bytes");
    l.u64(r.bytes);
    l.key("values");
    l.u64(r.values);
    l.ch('}');
}

// The loop's end, in the order that matters: an interrupt first, then
// the recorder failing (the log is what the command is for, so a disk
// that fills ends it), a replay's last frame, the seconds asked; nullptr
// while none holds. A session whose link is lost or dead keeps the log
// going: its health lines say so and the recorder holds the gap, so an
// unattended log survives a dropout.
inline const char* log_end_reason(bool interrupted, bool recording_failed,
                                  bool replay_ended, bool seconds_over)
{
    if (interrupted) {
        return "interrupted";
    }
    if (recording_failed) {
        return "recording-failed";
    }
    if (replay_ended) {
        return "replay-end";
    }
    if (seconds_over) {
        return "seconds";
    }
    return nullptr;
}

// When a log line is due: once each period of session time from the
// start. A stall longer than a period (a laptop asleep) owes one line,
// not one per period missed: the next due moves past now.
class LogTicker
{
public:
    LogTicker(uint64_t started_us, uint64_t every_us)
        : m_started(started_us), m_every(every_us > 0 ? every_us : 1),
          m_next(m_every)
    {}

    bool due(uint64_t now_us)
    {
        const uint64_t elapsed = now_us >= m_started ? now_us - m_started : 0;
        if (elapsed < m_next) {
            return false;
        }
        m_next = (elapsed / m_every + 1) * m_every;
        return true;
    }

private:
    uint64_t m_started;
    uint64_t m_every;
    uint64_t m_next;
};

} // namespace cli

#endif
