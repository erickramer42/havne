// File/layer: cli/can_line.h | havne-cli, a CAN interface as a devices line (ADR-0032 section 7)
// Testing: UNIT (tests/test_cli.cpp, CliCanTest: a controller with its
//          state, its modes and its driver's, an older kernel's, a vcan,
//          a mode bit core doesn't name)
//
// `devices`' can line: what a socketcan:// connect is judged on, so a
// refusal is explained before a run (ADR-0040 section 2): up,
// listen-only, CAN FD, the bit rate, the controller's state, the modes
// it's in and the modes its driver offers (null when the kernel doesn't
// say, before Linux 6.0). The words are core's, ip link's; a mode bit
// core doesn't name is written as its hex.

#ifndef HAVNE_CLI_CAN_LINE_H
#define HAVNE_CLI_CAN_LINE_H

#include "jsonl.h"

#include "ecuio/socketcan.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace cli {

// A mode mask as a list of words, lowest bit first
inline void can_modes(Line& l, uint32_t modes)
{
    l.ch('[');
    bool first = true;
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((modes & bit) == 0) {
            continue;
        }
        if (!first) {
            l.ch(',');
        }
        first = false;
        const char* word = ecuio_can_mode_name(bit);
        char hex[16];
        if (*word == '\0') {
            std::snprintf(hex, sizeof hex, "0x%X", static_cast<unsigned>(bit));
            word = hex;
        }
        l.string(word, SIZE_MAX);
    }
    l.ch(']');
}

// {"type":"can", name, kind, up, listen_only, fd, bitrate, state,
// modes, modes_supported, uri}; state null when none is reported (vcan)
inline void can_line(Line& l, const ecuio_can_interface_t& c)
{
    l.raw("{\"type\":\"can\"");
    l.key("name");
    l.string(c.name, sizeof c.name);
    l.key("kind");
    l.string(c.kind, sizeof c.kind);
    l.key("up");
    l.raw((c.flags & ECUIO_CAN_UP) ? "true" : "false");
    l.key("listen_only");
    l.raw((c.flags & ECUIO_CAN_LISTEN_ONLY) ? "true" : "false");
    l.key("fd");
    l.raw((c.flags & ECUIO_CAN_FD) ? "true" : "false");
    l.key("bitrate");
    l.u64(c.bitrate);
    l.key("state");
    const char* state = ecuio_can_state_name(c.state);
    if (*state == '\0') {
        l.raw("null");
    } else {
        l.string(state, SIZE_MAX);
    }
    l.key("modes");
    can_modes(l, c.modes);
    l.key("modes_supported");
    if ((c.flags & ECUIO_CAN_MODES_KNOWN) != 0) {
        can_modes(l, c.modes_supported);
    } else {
        l.raw("null");
    }
    l.key("uri");
    const std::string uri = std::string("socketcan://") + c.name;
    l.string(uri.c_str(), SIZE_MAX);
    l.ch('}');
}

} // namespace cli

#endif /* HAVNE_CLI_CAN_LINE_H */
