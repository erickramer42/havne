// File/layer: cli/sim_line.h | havne-cli, the simulated ECU's lines (ADR-0046)
// Testing: UNIT: CliSimTest (test_cli)
//
// `havne-cli sim`'s own lines: `sim` names what answers and where, the
// URI as given (its values, VIN and faults are in it, the defaults
// ADR-0046's) but for the gateway's sa= seeds, keys and masks, shown as
// "*"; `sim_stats` and `end` carry the counts core keeps
// (ecuio_sim_get_stats), and whether it failed or yielded to another
// node on its reply ID.

#ifndef HAVNE_CLI_SIM_LINE_H
#define HAVNE_CLI_SIM_LINE_H

#include "jsonl.h"

#include "ecuio/sim.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace cli {

// The counts after an open object, whether the bus failed and whether
// another node answered
inline void sim_stats_fields(Line& l, const ecuio_sim_stats_t& s)
{
    l.key("requests");
    l.u64(s.requests);
    l.key("replies");
    l.u64(s.replies);
    l.key("refused");
    l.u64(s.refused);
    l.key("silent");
    l.u64(s.silent);
    l.key("late");
    l.u64(s.late);
    l.key("pending");
    l.u64(s.pending);
    l.key("bad_sequence");
    l.u64(s.bad_sequence);
    l.key("stalled");
    l.u64(s.stalled);
    l.key("unflowed");
    l.u64(s.unflowed);
    l.key("clears");
    l.u64(s.clears);
    l.key("failed");
    l.raw((s.flags & ECUIO_SIM_FAILED) ? "true" : "false");
    l.key("yielded");
    l.raw((s.flags & ECUIO_SIM_YIELDED) ? "true" : "false");
}

// An sa= value as shown: key, never and never:<ms> as given; any other
// form its word and "*" (key:*, xor:*, add:*), or "*" alone
inline std::string sim_shown_sa(const std::string& v)
{
    const bool digits =
        v.size() > 6 && v.compare(0, 6, "never:") == 0 &&
        v.find_first_not_of("0123456789", 6) == std::string::npos;
    if (v == "key" || v == "never" || digits) {
        return v;
    }
    const size_t colon = v.find(':');
    return colon == std::string::npos ? "*" : v.substr(0, colon + 1) + "*";
}

// The URI as given, each sa= value's seeds, keys and masks hidden: the
// sim line is a run's record, which a tester shares
inline std::string sim_shown_uri(const char* uri)
{
    std::string s(uri);
    size_t at = s.find('?');
    while (at != std::string::npos) {
        const size_t start = at + 1;
        const size_t end = std::min(s.find('&', start), s.size());
        if (s.compare(start, 3, "sa=") == 0 && end > start + 3) {
            const std::string shown =
                sim_shown_sa(s.substr(start + 3, end - start - 3));
            s.replace(start + 3, end - start - 3, shown);
            at = s.find('&', start + 3 + shown.size());
        } else {
            at = end < s.size() ? end : std::string::npos;
        }
    }
    return s;
}

// What answers and where: {"type":"sim", uri, on, desk_bus}, the URI
// as sim_shown_uri gives it
inline void sim_line(Line& l, const char* uri, const char* ifname,
                     bool desk_bus)
{
    l.raw("{\"type\":\"sim\"");
    l.key("uri");
    const std::string shown = sim_shown_uri(uri);
    l.string(shown.c_str(), SIZE_MAX, 256);
    l.key("on");
    l.string(ifname, SIZE_MAX, 64);
    l.key("desk_bus");
    l.raw(desk_bus ? "true" : "false");
    l.ch('}');
}

// The counts so far
inline void sim_stats_line(Line& l, const ecuio_sim_stats_t& s)
{
    l.raw("{\"type\":\"sim_stats\"");
    sim_stats_fields(l, s);
    l.ch('}');
}

// The end: why (interrupted, seconds, bus-failed, yielded) and the
// counts
inline void sim_end_line(Line& l, const char* reason,
                         const ecuio_sim_stats_t& s)
{
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string(reason, SIZE_MAX);
    sim_stats_fields(l, s);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_SIM_LINE_H
