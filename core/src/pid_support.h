// File/layer: core/src/pid_support.h | C++17, Qt-free; header-only, pure
// Testing: UNIT: PidSupportTest (test_obd2); BRIDGE: BridgeTest
//          (test_bridge); HARNESS: J2534AdoptionTest (test_j2534_stream);
//          Elm327SessionTest (test_elm327)
//
// Which of a set's Service 01 signals their ECU says it doesn't support
// (ADR-0039 section 6): the supported-PID bitmaps a channel's support
// report carries (ADR-0015 section 4), judged against each def's PID.

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

/* The bitmap at PID 0x20 * ((pid - 1) / 0x20) was answered and its bit
 * for pid is clear. PID 0, and a range never answered, judge nothing. */
inline bool ecuio_pid_marked_unsupported(const ecuio_channel_support_t& s,
                                         uint8_t pid)
{
    if (pid == 0) {
        return false;
    }
    const unsigned range = (pid - 1u) / 0x20u;
    if ((s.ranges & (1u << range)) == 0) {
        return false;
    }
    return (s.pid_bitmap[(pid - 1u) / 8u] & (0x80u >> ((pid - 1u) % 8u))) == 0;
}

/* The set's Service 01 defs whose PID the report of the channel their
 * message ID names marks unsupported; a report with nothing learned
 * (generation 0) judges nothing, nor does a def no report names. Fills
 * ids with up to capacity def IDs in the set's order and returns how
 * many there are. */
inline uint32_t ecuio_unsupported_signals(
    const ecuio_signal_set_t& set, const ecuio_channel_support_t* reports,
    uint32_t report_count, uint16_t* ids, uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; set.defs != nullptr && i < set.count; ++i) {
        const ecuio_signal_def_t& d = set.defs[i];
        if (!ecuio_service01_keyed(d)) {
            continue;
        }
        for (uint32_t c = 0; c < report_count; ++c) {
            const ecuio_channel_support_t& s = reports[c];
            if (s.generation == 0 || s.rx_id != d.message_id) {
                continue;
            }
            if (ecuio_pid_marked_unsupported(
                    s, static_cast<uint8_t>(d.mux_key & 0xFFu))) {
                if (count < capacity) {
                    ids[count] = d.id;
                }
                ++count;
            }
            break;
        }
    }
    return count;
}

#endif
