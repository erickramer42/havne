// File/layer: tests/test_socketcan_support.h | SocketCAN listening's rules (ADR-0032)
// Testing: UNIT: the helpers of test_socketcan*.cpp (test_socketcan)

#pragma once

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/socketcan.h"
#include "ecuio/sim.h"
#include "socketcan_wire.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* ---- A canned RTM_NEWLINK payload: the ifinfomsg, then attributes ---- */

inline void put16(std::vector<uint8_t>& b, uint16_t v)
{
    uint8_t x[2];
    std::memcpy(x, &v, 2);
    b.insert(b.end(), x, x + 2);
}
inline void put32(std::vector<uint8_t>& b, uint32_t v)
{
    uint8_t x[4];
    std::memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
}
inline void pad4(std::vector<uint8_t>& b)
{
    while (b.size() % 4 != 0) {
        b.push_back(0);
    }
}
inline std::vector<uint8_t> attr(uint16_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> a;
    put16(a, static_cast<uint16_t>(4 + payload.size()));
    put16(a, type);
    a.insert(a.end(), payload.begin(), payload.end());
    pad4(a);
    return a;
}
inline std::vector<uint8_t> str(const char* s)
{
    return std::vector<uint8_t>(s, s + std::strlen(s) + 1);
}
inline std::vector<uint8_t> u32s(std::initializer_list<uint32_t> vs)
{
    std::vector<uint8_t> b;
    for (uint32_t v : vs) {
        put32(b, v);
    }
    return b;
}
inline std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> b;
    for (const auto& p : parts) {
        b.insert(b.end(), p.begin(), p.end());
    }
    return b;
}

constexpr uint16_t kNested = 0x8000; /* NLA_F_NESTED, which the kernel sets */

struct LinkSpec {
    const char* name = "can0";
    int32_t index = 4;
    bool up = true;
    const char* kind = "can"; /* nullptr: no IFLA_LINKINFO (eth0) */
    uint32_t mtu = 16;
    bool bitrate = true;
    uint32_t rate = 500000;
    bool ctrlmode = true;
    uint32_t modes = SOCKETCAN_CTRLMODE_LISTENONLY;
    bool state = true;
    uint32_t can_state = SOCKETCAN_STATE_ERROR_ACTIVE;
    bool restart = true;
    uint32_t restart_ms = 0;
    bool ext = false; /* IFLA_CAN_CTRLMODE_EXT, a kernel from 6.0 */
    uint32_t supported = 0;
};

/* The Innomaker USB2CAN on the desk bus (BENCH prep #26): gs_usb from
 * Linux 6.0 says which modes its firmware offers, and one-shot isn't
 * among them (WSL's 6.18 kernel: loopback, listen-only,
 * cc-len8-dlc) */
inline LinkSpec usb2can()
{
    LinkSpec s;
    s.modes = 0;
    s.ext = true;
    s.supported = 0x01u /* loopback */ | SOCKETCAN_CTRLMODE_LISTENONLY |
                  0x100u /* cc-len8-dlc */;
    return s;
}

/* A controller a session may send through (ADR-0040 section 2) */
inline LinkSpec sender()
{
    LinkSpec s;
    s.modes = SOCKETCAN_CTRLMODE_ONE_SHOT;
    return s;
}

inline std::vector<uint8_t> link_msg(const LinkSpec& s)
{
    std::vector<uint8_t> m;
    m.push_back(0); /* family */
    m.push_back(0); /* pad */
    put16(m, 280);  /* ARPHRD_CAN */
    put32(m, static_cast<uint32_t>(s.index));
    put32(m, s.up ? 0x41u : 0x0u); /* IFF_UP | IFF_RUNNING */
    put32(m, 0);                   /* change */
    auto attrs = cat({attr(SOCKETCAN_IFLA_IFNAME, str(s.name)),
                      attr(SOCKETCAN_IFLA_MTU, u32s({s.mtu}))});
    if (s.kind != nullptr) {
        std::vector<uint8_t> data;
        if (s.bitrate) {
            /* struct can_bittiming: bitrate first, then 7 more fields */
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_BITTIMING,
                                   u32s({s.rate, 875, 125, 6, 7, 2, 1, 4}))});
        }
        if (s.state) {
            data = cat(
                {data, attr(SOCKETCAN_IFLA_CAN_STATE, u32s({s.can_state}))});
        }
        if (s.ctrlmode) {
            /* struct can_ctrlmode: mask, then flags */
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_CTRLMODE,
                                   u32s({0xFFFFFFFFu, s.modes}))});
        }
        if (s.restart) {
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_RESTART_MS,
                                   u32s({s.restart_ms}))});
        }
        if (s.ext) {
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_CTRLMODE_EXT | kNested,
                                   attr(SOCKETCAN_IFLA_CAN_CTRLMODE_SUPPORTED,
                                        u32s({s.supported})))});
        }
        auto info = attr(SOCKETCAN_IFLA_INFO_KIND, str(s.kind));
        if (!data.empty()) {
            info = cat({info, attr(SOCKETCAN_IFLA_INFO_DATA | kNested, data)});
        }
        attrs = cat({attrs, attr(SOCKETCAN_IFLA_LINKINFO | kNested, info)});
    }
    return cat({m, attrs});
}

inline socketcan_link parsed(const LinkSpec& s)
{
    const auto m = link_msg(s);
    socketcan_link l;
    EXPECT_TRUE(socketcan_parse_link(m.data(), m.size(), &l));
    return l;
}

} // namespace
