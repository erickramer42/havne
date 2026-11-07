// File/layer: cli/health_lines.h | havne-cli, a car's health as one
//             scrubbed report (ADR-0054's appendix "the health report as
//             built")
// Testing: UNIT: CliHealthTest (test_cli)
//
// `health <uri | recording> <out> --format json|text [--units
// metric|imperial] [--from S] [--to S] [--seconds S] [--signals <file> |
// --plugin <pkg>]`: its arguments, the live session's wait, and its lines
// in core's words: `health` with the report's counts (never the VIN or an
// identification text), `health_failed` naming the class a failed check
// found, never its value.

#ifndef HAVNE_CLI_HEALTH_LINES_H
#define HAVNE_CLI_HEALTH_LINES_H

#include "jsonl.h"
#include "transfer_lines.h" /* left_out_word */
#include "view_lines.h"     /* parse_seconds */

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cli {

/* The live wait's bound when --seconds gives none: clear's */
constexpr uint64_t kHealthWaitSeconds = 30;
/* The live wait ends once no shown signal has gained its first value for
 * this long after the reports settled */
constexpr uint64_t kHealthQuietUs = 2000000;

struct HealthArgs {
    const char* source = nullptr; /* a URI, or a recording's folder */
    const char* out = nullptr;
    const char* format_word = nullptr;
    bool live = false; /* source names a session */
    bool from = false;
    bool to = false;
    bool seconds_given = false;
    uint64_t seconds = kHealthWaitSeconds;
    const char* signals = nullptr;
    const char* plugin = nullptr;
    ecuio_health_options_t options{};
};

/* A source that names a session (a scheme's "://"), not a folder */
inline bool names_a_session(const char* source)
{
    return std::strstr(source, "://") != nullptr;
}

/* The arguments after `health`; false with *why, a usage error */
inline bool parse_health(int argc, char** argv, HealthArgs& a, std::string* why)
{
    a.options.units = ECUIO_UNITS_METRIC;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        const bool has_value = i + 1 < argc;
        if (s == "--format" && has_value) {
            a.format_word = argv[++i];
            if (std::strcmp(a.format_word, "json") == 0) {
                a.options.format = ECUIO_HEALTH_JSON;
            } else if (std::strcmp(a.format_word, "text") == 0) {
                a.options.format = ECUIO_HEALTH_TEXT;
            } else {
                *why = "--format takes json or text";
                return false;
            }
        } else if (s == "--units" && has_value) {
            const std::string u = argv[++i];
            if (u == "metric") {
                a.options.units = ECUIO_UNITS_METRIC;
            } else if (u == "imperial") {
                a.options.units = ECUIO_UNITS_IMPERIAL;
            } else {
                *why = "--units takes metric or imperial";
                return false;
            }
        } else if ((s == "--from" || s == "--to") && has_value) {
            int64_t us = 0;
            if (!parse_seconds(argv[++i], us) || us < 0) {
                *why = s + " takes seconds, 0 or more";
                return false;
            }
            if (s == "--from") {
                a.options.from_us = static_cast<uint64_t>(us);
                a.from = true;
            } else if (us == 0) {
                *why = "--to takes seconds after 0";
                return false;
            } else {
                a.options.to_us = static_cast<uint64_t>(us);
                a.to = true;
            }
        } else if (s == "--seconds" && has_value) {
            const char* text = argv[++i];
            char* end = nullptr;
            const unsigned long long n = std::strtoull(text, &end, 10);
            if (*text < '0' || *text > '9' || *end != '\0' || n == 0 ||
                n > 86400) {
                *why = "--seconds takes a whole number from 1 to 86400";
                return false;
            }
            a.seconds = n;
            a.seconds_given = true;
        } else if (s == "--signals" && has_value) {
            a.signals = argv[++i];
        } else if (s == "--plugin" && has_value) {
            a.plugin = argv[++i];
        } else if (s.rfind("--", 0) == 0) {
            *why = "unexpected '" + s + "'";
            return false;
        } else if (a.source == nullptr) {
            a.source = argv[i];
        } else if (a.out == nullptr) {
            a.out = argv[i];
        } else {
            *why = "health takes one session or recording and one file";
            return false;
        }
    }
    if (a.source == nullptr || a.out == nullptr) {
        *why = "health needs a URI or a recording's folder, and the file to "
               "write";
        return false;
    }
    if (a.format_word == nullptr) {
        *why = "health needs --format";
        return false;
    }
    a.live = names_a_session(a.source);
    if (a.live && (a.from || a.to)) {
        *why = "--from and --to take a recording's seconds, not a session's";
        return false;
    }
    if (!a.live &&
        (a.seconds_given || a.signals != nullptr || a.plugin != nullptr)) {
        *why = "--seconds, --signals and --plugin are a live session's: a "
               "recording reads with its own set";
        return false;
    }
    if (a.signals != nullptr && a.plugin != nullptr) {
        *why = "--signals and --plugin each give the profile: one at a time";
        return false;
    }
    if (a.from && a.to && a.options.to_us <= a.options.from_us) {
        *why = "--to comes after --from";
        return false;
    }
    if (!a.live) {
        a.options.folder_utf8 = a.source;
    }
    a.options.out_path_utf8 = a.out;
    return true;
}

/* A live session's channel has settled once each of its reports that was
 * asked has: its diagnostics COMPLETE or never asked (only obd2 and the
 * mock ask them), its identification COMPLETE or with no items */
inline bool reports_settled(const ecuio_channel_diagnostics_t& d,
                            const ecuio_channel_identity_t& id)
{
    const bool diag = (d.flags & ECUIO_DIAG_COMPLETE) != 0 ||
                      (d.flags & ECUIO_DIAG_ASKED) == 0;
    const bool ident =
        (id.flags & ECUIO_IDENTITY_COMPLETE) != 0 || id.items == 0;
    return diag && ident;
}

/* A def the report shows a value of: a ranked number or enumeration, or a
 * since-the-codes-were-cleared PID (Service 01's 21, 30, 31, 4D, 4E keyed
 * 41 <PID>), whichever ECU it reads */
inline bool shown_def(const ecuio_signal_def_t& d)
{
    if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
        return false;
    }
    if (d.plot_rank > 0) {
        return true;
    }
    if (d.mux_offset != 0 || d.mux_length != 2 || (d.mux_key >> 8) != 0x41u) {
        return false;
    }
    const uint32_t pid = d.mux_key & 0xFFu;
    return pid == 0x21 || pid == 0x30 || pid == 0x31 || pid == 0x4D ||
           pid == 0x4E;
}

/* The values' part of the wait, once the reports settled: done when the
 * count of shown signals with a value hasn't grown for quiet_us (a PID the
 * ECU lacks never comes, so the wait ends on quiet, not on all of them) */
class ValuesQuiet
{
public:
    bool quiet(uint32_t count, uint64_t now_us, uint64_t quiet_us)
    {
        if (!started_ || count != seen_) {
            started_ = true;
            seen_ = count;
            since_us_ = now_us;
            return false;
        }
        return now_us - since_us_ >= quiet_us;
    }

private:
    bool started_ = false;
    uint32_t seen_ = 0;
    uint64_t since_us_ = 0;
};

/* The bytes health_line (or health_failed_line) needs: its counts and
 * keys, and the file's path at its longest escape */
inline size_t health_line_bytes(const HealthArgs& a)
{
    return 1024 + Line::string_bytes(a.out) + Line::string_bytes(a.format_word);
}

/* {"type":"health","format":...,"file":...,"source":"session"|"recording",
 *  "ecus":N,"codes":N,"values":N,"since_cleared":N,
 *  "left_out":{"identification":N},"vin_decoded":bool}; a session's adds
 *  "settled":bool, whether every asked report had settled */
inline void health_line(Line& l, const HealthArgs& a,
                        const ecuio_health_report_t& r, bool settled)
{
    l.raw("{\"type\":\"health\"");
    l.key("format");
    l.string(a.format_word, SIZE_MAX);
    l.key("file");
    l.string(a.out, SIZE_MAX);
    l.key("source");
    l.string(a.live ? "session" : "recording", SIZE_MAX);
    l.key("ecus");
    l.u64(r.ecus);
    l.key("codes");
    l.u64(r.codes);
    l.key("values");
    l.u64(r.values);
    l.key("since_cleared");
    l.u64(r.since_cleared);
    l.key("left_out");
    l.raw("{\"identification\":");
    l.u64(r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION]);
    l.ch('}');
    l.key("vin_decoded");
    l.raw((r.flags & ECUIO_HEALTH_VIN_DECODED) != 0 ? "true" : "false");
    if (a.live) {
        l.key("settled");
        l.raw(settled ? "true" : "false");
    }
    l.ch('}');
}

/* {"type":"health_failed","class":...}: a failed check, before the error */
inline void health_failed_line(Line& l, const ecuio_health_report_t& r)
{
    l.raw("{\"type\":\"health_failed\"");
    l.key("class");
    l.string(left_out_word(r.failed_class - 1), SIZE_MAX);
    l.ch('}');
}

} // namespace cli

#endif /* HAVNE_CLI_HEALTH_LINES_H */
