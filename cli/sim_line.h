// File/layer: cli/sim_line.h | havne-cli, the simulated ECU's lines (ADR-0046)
// Testing: UNIT: CliSimTest (test_cli)
//
// `havne-cli sim`'s own lines: `sim` names what answers and where, the
// URI as given (its values, VIN and faults are in it, the defaults
// ADR-0046's); `sim_stats` and `end` carry the counts core keeps
// (ecuio_sim_get_stats), and whether it failed or yielded to another
// node on its reply ID.

#ifndef HAVNE_CLI_SIM_LINE_H
#define HAVNE_CLI_SIM_LINE_H

#include "jsonl.h"

#include "ecuio/sim.h"

#include <cstdint>

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

// What answers and where: {"type":"sim", uri, on, desk_bus}
inline void sim_line(Line& l, const char* uri, const char* ifname,
                     bool desk_bus)
{
    l.raw("{\"type\":\"sim\"");
    l.key("uri");
    l.string(uri, SIZE_MAX, 256);
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
