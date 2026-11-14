// File/layer: core/src/pid_support.h | C++17, Qt-free; header-only, pure
// Testing: UNIT: PidSupportTest (test_obd2); BRIDGE: BridgeTest
//          (test_bridge); HARNESS: J2534AdoptionTest (test_j2534_stream);
//          Elm327SessionTest (test_elm327)
//
// Which of a set's Service 01 signals their ECU says it doesn't support
// (ADR-0039 section 6): the supported-PID bitmaps a channel's support
// report carries (ADR-0015 section 4), judged against each def's PID. A
// computed def follows its inputs: with one of them gone it is gone too.

#ifndef ECUIO_PID_SUPPORT_H
#define ECUIO_PID_SUPPORT_H

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cstdint>

/* A Service 01 reply's key: 41 <PID>, at the payload's start */
inline bool ecuio_service01_keyed(const ecuio_signal_def_t& def)
{
    return def.mux_offset == 0 && def.mux_length == 2 &&
           (def.mux_key >> 8) == 0x41u;
}

/* The data PIDs a support report's bitmaps list (PID p is bit
 * 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8): the chain's own PIDs
 * (0x20, 0x40, ...) are its links, not data, so they don't count */
inline uint16_t ecuio_supported_pid_count(const uint8_t (&bitmap)[32])
{
    uint16_t n = 0;
    for (uint32_t pid = 1; pid <= 0xFF; ++pid) {
        if (pid % 0x20 != 0 &&
            (bitmap[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) != 0) {
            ++n;
        }
    }
    return n;
}

/* The last PID the answered bitmaps cover: 0x20 * (i + 1) for the
 * highest bit i of ranges (bit i: the bitmap at PID 0x20 * i); 0 with
 * none answered */
inline uint16_t ecuio_pids_through(uint32_t ranges)
{
    uint16_t top = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        if ((ranges & (1u << i)) != 0) {
            top = static_cast<uint16_t>(0x20u * (i + 1));
        }
    }
    return top;
}

/* Whether bit pid of a report's bitmaps is set */
inline bool ecuio_pid_bit_set(const ecuio_channel_support_t& s, unsigned pid)
{
    return (s.pid_bitmap[(pid - 1u) / 8u] & (0x80u >> ((pid - 1u) % 8u))) != 0;
}

/* The bitmap at PID 0x20 * ((pid - 1) / 0x20) was answered and its bit
 * for pid is clear; or the walk is complete and the chain ended before
 * pid's range (the last answered bitmap's link to the next one clear),
 * which J1979 reads as every later PID unsupported. PID 0, and a range a
 * walk that didn't finish never reached, judge nothing. */
inline bool ecuio_pid_marked_unsupported(const ecuio_channel_support_t& s,
                                         uint8_t pid)
{
    if (pid == 0) {
        return false;
    }
    const unsigned range = (pid - 1u) / 0x20u;
    if ((s.ranges & (1u << range)) != 0) {
        return !ecuio_pid_bit_set(s, pid);
    }
    if ((s.flags & ECUIO_SUPPORT_COMPLETE) == 0) {
        return false;
    }
    for (unsigned r = range; r-- > 0;) {
        if ((s.ranges & (1u << r)) != 0) {
            return !ecuio_pid_bit_set(s, 0x20u * (r + 1u));
        }
    }
    return false;
}

/* A Service 01 def whose PID its channel's report marks unsupported: the
 * report whose rx is the def's message ID, once it has learned something
 * (generation 0 judges nothing, nor does a def no report names) */
inline bool ecuio_def_marked_unsupported(const ecuio_signal_def_t& d,
                                         const ecuio_channel_support_t* reports,
                                         uint32_t report_count)
{
    if (!ecuio_service01_keyed(d)) {
        return false;
    }
    for (uint32_t c = 0; c < report_count; ++c) {
        const ecuio_channel_support_t& s = reports[c];
        if (s.generation == 0 || s.rx_id != d.message_id) {
            continue;
        }
        return ecuio_pid_marked_unsupported(
            s, static_cast<uint8_t>(d.mux_key & 0xFFu));
    }
    return false;
}

/* How far a computed def's inputs are followed through computed inputs:
 * past it nothing is judged, so a set whose programs name each other in
 * a ring ends */
constexpr unsigned ECUIO_COMPUTED_FOLLOW_MAX = 8;

/* A computed def never has a value while one of its inputs has none:
 * whether gone(index of a def in the set) holds for an input of d, an
 * input that is computed itself judged by its own inputs. False for a
 * def that isn't computed, a program the set doesn't hold and an input
 * the set lacks. */
template <class Gone>
bool ecuio_computed_input_gone(const ecuio_signal_set_t& set,
                               const ecuio_signal_def_t& d, const Gone& gone,
                               unsigned depth = 0)
{
    if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 || set.computed == nullptr ||
        d.table_first >= set.computed_count ||
        depth >= ECUIO_COMPUTED_FOLLOW_MAX) {
        return false;
    }
    const ecuio_computed_t& program = set.computed[d.table_first];
    for (uint32_t k = 0;
         k < program.input_count && k < ECUIO_COMPUTED_MAX_INPUTS; ++k) {
        for (uint32_t i = 0; i < set.count; ++i) {
            const ecuio_signal_def_t& in = set.defs[i];
            if (in.id != program.input_ids[k]) {
                continue;
            }
            if ((in.flags & ECUIO_DEF_FLAG_COMPUTED) != 0
                    ? ecuio_computed_input_gone(set, in, gone, depth + 1)
                    : gone(i)) {
                return true;
            }
            break;
        }
    }
    return false;
}

/* The set's defs their ECU says it doesn't support: each Service 01 def
 * whose PID its channel's report marks unsupported, and each computed
 * def with such an input, which can never be computed. Fills ids with up
 * to capacity def IDs in set order and returns how many there are. */
inline uint32_t ecuio_unsupported_signals(
    const ecuio_signal_set_t& set, const ecuio_channel_support_t* reports,
    uint32_t report_count, uint16_t* ids, uint32_t capacity)
{
    uint32_t count = 0;
    const auto marked = [&](uint32_t i) {
        return ecuio_def_marked_unsupported(set.defs[i], reports, report_count);
    };
    for (uint32_t i = 0; set.defs != nullptr && i < set.count; ++i) {
        if (marked(i) || ecuio_computed_input_gone(set, set.defs[i], marked)) {
            if (count < capacity) {
                ids[count] = set.defs[i].id;
            }
            ++count;
        }
    }
    return count;
}

#endif
