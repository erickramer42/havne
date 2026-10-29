// File/layer: core/src/socketcan_wire.h | internal, not ABI-exported;
//             included by socketcan_transport.cpp and socketcan_interfaces.cpp
// C++17, Qt-free, no platform headers: the kernel's values are mirrored
// here (and checked against <linux/can.h> and <linux/rtnetlink.h> where
// those exist), so every rule below is a unit test on every platform.
// Testing: UNIT — SocketCanTest.* (test_socketcan): the interface name,
//     the filters, a link's netlink attributes and the judgement on them,
//     a kernel frame to an ecuio_frame_t, the clock
//
// SocketCAN listening (ADR-0032): what a session reads from the kernel,
// with no I/O.

#ifndef ECUIO_SOCKETCAN_WIRE_H
#define ECUIO_SOCKETCAN_WIRE_H

#include "ecuio/core.h"
#include "ecuio/socketcan.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- The kernel's values (linux/can.h, linux/can/error.h,
 *      linux/can/netlink.h, linux/if_link.h, linux/rtnetlink.h) ---- */

constexpr uint32_t SOCKETCAN_EFF_FLAG = 0x80000000u;   /* CAN_EFF_FLAG */
constexpr uint32_t SOCKETCAN_RTR_FLAG = 0x40000000u;   /* CAN_RTR_FLAG */
constexpr uint32_t SOCKETCAN_ERR_FLAG = 0x20000000u;   /* CAN_ERR_FLAG */
constexpr uint32_t SOCKETCAN_SFF_MASK = 0x000007FFu;   /* CAN_SFF_MASK */
constexpr uint32_t SOCKETCAN_EFF_MASK = 0x1FFFFFFFu;   /* CAN_EFF_MASK */
constexpr uint8_t SOCKETCAN_FD_BRS = 0x01;             /* CANFD_BRS */
constexpr size_t SOCKETCAN_MTU = 16;                   /* CAN_MTU */
constexpr size_t SOCKETCAN_FD_MTU = 72;                /* CANFD_MTU */
constexpr uint32_t SOCKETCAN_ERR_CRTL = 0x00000004u;   /* CAN_ERR_CRTL */
constexpr uint32_t SOCKETCAN_ERR_BUSOFF = 0x00000040u; /* CAN_ERR_BUSOFF */
constexpr uint32_t SOCKETCAN_ERR_RESTARTED =
    0x00000100u; /* CAN_ERR_RESTARTED */
constexpr uint32_t SOCKETCAN_CTRLMODE_LISTENONLY =
    0x02u; /* CAN_CTRLMODE_LISTENONLY */

constexpr uint16_t SOCKETCAN_IFLA_IFNAME = 3;
constexpr uint16_t SOCKETCAN_IFLA_MTU = 4;
constexpr uint16_t SOCKETCAN_IFLA_LINKINFO = 18;
constexpr uint16_t SOCKETCAN_IFLA_INFO_KIND = 1;
constexpr uint16_t SOCKETCAN_IFLA_INFO_DATA = 2;
constexpr uint16_t SOCKETCAN_IFLA_CAN_BITTIMING = 1;
constexpr uint16_t SOCKETCAN_IFLA_CAN_CTRLMODE = 5;
constexpr uint16_t SOCKETCAN_NLA_TYPE_MASK =
    0x3FFF; /* without NESTED, BYTEORDER */
constexpr uint32_t SOCKETCAN_IFF_UP = 0x1;
constexpr size_t SOCKETCAN_IFINFOMSG_LEN = 16; /* struct ifinfomsg */
constexpr size_t SOCKETCAN_IFNAME_MAX = 15;    /* IFNAMSIZ - 1 */

/* ?filter= takes at most this many IDs (CAN_RAW_FILTER takes more; a
 * longer list is a whole-bus listen with a filter of its own) */
constexpr size_t SOCKETCAN_MAX_FILTERS = 8;

/* ---- The interface name ---- */

/* What `ip link` shows: 1 to 15 of letters, digits, '_', '-' and '.' */
inline bool socketcan_name_ok(const char* s, size_t len)
{
    if (len == 0 || len > SOCKETCAN_IFNAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* ---- ?filter= ---- */

/* One receive filter as CAN_RAW_FILTER takes it: a frame passes when
 * (its can_id & mask) == (id & mask). The ID and mask carry
 * SOCKETCAN_EFF_FLAG, so an 11-bit filter never passes a 29-bit frame
 * with the same low bits, nor the other way round. */
struct socketcan_filter {
    uint32_t id = 0;
    uint32_t mask = 0;
};

namespace socketcan_detail {
/* A decimal or 0x hexadecimal number from [s, s + len) */
inline bool number(const char* s, size_t len, uint32_t* out)
{
    size_t i = 0;
    uint32_t base = 10;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i == len) {
        return false;
    }
    uint64_t v = 0;
    for (; i < len; ++i) {
        const char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a') + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A') + 10;
        } else {
            return false;
        }
        v = v * base + d;
        if (v > 0xFFFFFFFFull) {
            return false;
        }
    }
    *out = static_cast<uint32_t>(v);
    return true;
}
} // namespace socketcan_detail

/* "<id>[/<mask>],…": up to SOCKETCAN_MAX_FILTERS entries. An ID above
 * 0x7FF is 29-bit; a mask defaults to every bit of the ID's width and
 * may not reach past it. Returns the count, or 0 on a malformed list. */
inline size_t socketcan_parse_filters(const char* s, size_t len,
                                      socketcan_filter* out)
{
    size_t count = 0;
    size_t start = 0;
    while (start <= len) {
        size_t end = start;
        while (end < len && s[end] != ',') {
            ++end;
        }
        if (end == start || count == SOCKETCAN_MAX_FILTERS) {
            return 0; /* an empty entry, or one too many */
        }
        size_t slash = start;
        while (slash < end && s[slash] != '/') {
            ++slash;
        }
        uint32_t id = 0;
        if (!socketcan_detail::number(s + start, slash - start, &id) ||
            id > SOCKETCAN_EFF_MASK) {
            return 0;
        }
        const bool extended = id > SOCKETCAN_SFF_MASK;
        const uint32_t width =
            extended ? SOCKETCAN_EFF_MASK : SOCKETCAN_SFF_MASK;
        uint32_t mask = width;
        if (slash < end &&
            (!socketcan_detail::number(s + slash + 1, end - slash - 1, &mask) ||
             (mask & ~width) != 0)) {
            return 0;
        }
        out[count].id = id | (extended ? SOCKETCAN_EFF_FLAG : 0);
        out[count].mask = mask | SOCKETCAN_EFF_FLAG; /* the width must match */
        ++count;
        if (end == len) {
            break;
        }
        start = end + 1;
        if (start == len) {
            return 0; /* a trailing comma */
        }
    }
    return count;
}

/* ---- A link, from the kernel's RTM_NEWLINK message ---- */

struct socketcan_link {
    char name[SOCKETCAN_IFNAME_MAX + 1] = {};
    int32_t index = 0;
    bool up = false;
    char kind[16] = {}; /* IFLA_INFO_KIND: "can", "vcan", ... */
    uint32_t mtu = 0;   /* 16 classic, 72 with CAN FD */
    bool has_bitrate = false;
    uint32_t bitrate = 0; /* IFLA_CAN_BITTIMING's first field */
    bool has_ctrlmode = false;
    uint32_t ctrlmode = 0; /* IFLA_CAN_CTRLMODE's flags */

    bool is_can() const { return strcmp(kind, "can") == 0; }
    bool is_vcan() const { return strcmp(kind, "vcan") == 0; }
    bool listen_only() const
    {
        return has_ctrlmode && (ctrlmode & SOCKETCAN_CTRLMODE_LISTENONLY) != 0;
    }
};

namespace socketcan_detail {
inline uint16_t u16(const uint8_t* p)
{
    uint16_t v;
    memcpy(&v, p, 2); /* the kernel's byte order is the host's */
    return v;
}
inline uint32_t u32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
inline size_t align4(size_t n)
{
    return (n + 3) & ~static_cast<size_t>(3);
}

/* Calls visit(type, payload, payload_len) for each attribute in
 * [p, p + len); false on one whose length runs past the end */
template <typename Visit>
bool each_attr(const uint8_t* p, size_t len, Visit visit)
{
    size_t off = 0;
    while (off + 4 <= len) {
        const uint16_t alen = u16(p + off);
        const uint16_t type = u16(p + off + 2) & SOCKETCAN_NLA_TYPE_MASK;
        if (alen < 4 || off + alen > len) {
            return false;
        }
        visit(type, p + off + 4, static_cast<size_t>(alen - 4));
        off += align4(alen);
    }
    return true;
}

inline void copy_str(char* dst, size_t cap, const uint8_t* src, size_t n)
{
    size_t k = 0;
    while (k + 1 < cap && k < n && src[k] != 0) {
        dst[k] = static_cast<char>(src[k]);
        ++k;
    }
    dst[k] = '\0';
}
} // namespace socketcan_detail

/* Reads a link from an RTM_NEWLINK message's payload (the ifinfomsg,
 * then its attributes). False when it's too short or an attribute runs
 * past its end. */
inline bool socketcan_parse_link(const uint8_t* msg, size_t len,
                                 socketcan_link* out)
{
    using namespace socketcan_detail;
    *out = socketcan_link{};
    if (len < SOCKETCAN_IFINFOMSG_LEN) {
        return false;
    }
    int32_t index;
    memcpy(&index, msg + 4, 4);
    out->index = index;
    out->up = (u32(msg + 8) & SOCKETCAN_IFF_UP) != 0;
    bool ok = true;
    const bool attrs = each_attr(
        msg + SOCKETCAN_IFINFOMSG_LEN, len - SOCKETCAN_IFINFOMSG_LEN,
        [&](uint16_t type, const uint8_t* p, size_t n) {
            if (type == SOCKETCAN_IFLA_IFNAME) {
                copy_str(out->name, sizeof out->name, p, n);
            } else if (type == SOCKETCAN_IFLA_MTU && n >= 4) {
                out->mtu = u32(p);
            } else if (type == SOCKETCAN_IFLA_LINKINFO) {
                ok =
                    ok &&
                    each_attr(p, n, [&](uint16_t t, const uint8_t* q, size_t m) {
                        if (t == SOCKETCAN_IFLA_INFO_KIND) {
                            copy_str(out->kind, sizeof out->kind, q, m);
                        } else if (t == SOCKETCAN_IFLA_INFO_DATA) {
                            ok =
                                ok &&
                                each_attr(
                                    q, m,
                                    [&](uint16_t u, const uint8_t* r,
                                        size_t k) {
                                        if (u == SOCKETCAN_IFLA_CAN_BITTIMING &&
                                            k >= 4) {
                                            out->has_bitrate = true;
                                            out->bitrate = u32(r);
                                        } else if (
                                            u == SOCKETCAN_IFLA_CAN_CTRLMODE &&
                                            k >= 8) {
                                            out->has_ctrlmode = true;
                                            out->ctrlmode = u32(
                                                r + 4); /* mask, then flags */
                                        }
                                    });
                        }
                    });
            }
        });
    return attrs && ok;
}

/* What discovery lists for a link (ecuio/socketcan.h): false for a link
 * that's neither a controller nor vcan, which isn't listed */
inline bool socketcan_interface_from(const socketcan_link& l,
                                     ecuio_can_interface_t* out)
{
    if (!l.is_can() && !l.is_vcan()) {
        return false;
    }
    *out = ecuio_can_interface_t{};
    memcpy(out->name, l.name, sizeof l.name);
    memcpy(out->kind, l.kind, sizeof out->kind - 1);
    out->flags = (l.up ? ECUIO_CAN_UP : 0u) |
                 (l.listen_only() ? ECUIO_CAN_LISTEN_ONLY : 0u) |
                 (l.mtu >= SOCKETCAN_FD_MTU ? ECUIO_CAN_FD : 0u);
    out->bitrate = l.is_can() && l.has_bitrate ? l.bitrate : 0;
    return true;
}

/* Whether a session may listen on the link (ADR-0032 §2), in the order
 * the refusals are checked */
enum class socketcan_verdict {
    ok,
    not_can, /* neither a CAN controller nor vcan */
    down,
    not_listen_only, /* a controller that could transmit */
    wrong_rate,      /* up at another rate than ?baud= asks */
};

/* want_baud 0: no check */
inline socketcan_verdict socketcan_judge(const socketcan_link& l,
                                         uint32_t want_baud)
{
    if (!l.is_can() && !l.is_vcan()) {
        return socketcan_verdict::not_can;
    }
    if (!l.up) {
        return socketcan_verdict::down;
    }
    if (l.is_can() && !l.listen_only()) {
        return socketcan_verdict::not_listen_only;
    }
    if (want_baud != 0 && l.is_can() && l.bitrate != want_baud) {
        return socketcan_verdict::wrong_rate;
    }
    return socketcan_verdict::ok;
}

/* ---- A kernel frame to an ecuio_frame_t ---- */

/* struct can_frame / canfd_frame: can_id, then the length, then (FD)
 * its flags, two reserved bytes, and the data at offset 8 */
struct socketcan_raw {
    uint32_t can_id = 0;
    uint8_t len = 0;
    uint8_t fd_flags = 0;
    bool fd = false; /* read as a CANFD_MTU frame */
    const uint8_t* data = nullptr;
};

/* Fills *out (zeroed first) from a kernel frame; the timestamp is the
 * caller's. An error frame keeps its class bits as the ID and its 8
 * bytes of detail. */
inline void socketcan_to_frame(const socketcan_raw& in, ecuio_frame_t* out)
{
    out->timestamp_us = 0;
    out->channel_id = 0;
    out->flags = 0;
    out->reserved = 0;
    const size_t cap = in.fd ? 64 : 8;
    const size_t n = in.len < cap ? in.len : cap;
    if (in.can_id & SOCKETCAN_ERR_FLAG) {
        out->arbitration_id = in.can_id & SOCKETCAN_EFF_MASK;
        out->flags |= ECUIO_FRAME_FLAG_ERROR;
    } else if (in.can_id & SOCKETCAN_EFF_FLAG) {
        out->arbitration_id = in.can_id & SOCKETCAN_EFF_MASK;
        out->flags |= ECUIO_FRAME_FLAG_EXTENDED;
    } else {
        out->arbitration_id = in.can_id & SOCKETCAN_SFF_MASK;
    }
    if (in.can_id & SOCKETCAN_RTR_FLAG && !(in.can_id & SOCKETCAN_ERR_FLAG)) {
        out->flags |= ECUIO_FRAME_FLAG_REMOTE;
    }
    if (in.fd) {
        out->flags |= ECUIO_FRAME_FLAG_FD;
        if (in.fd_flags & SOCKETCAN_FD_BRS) {
            out->flags |= ECUIO_FRAME_FLAG_BITRATE_SWITCH;
        }
    }
    /* A remote frame's length is the one it asks for: it carries no data */
    const bool remote = (out->flags & ECUIO_FRAME_FLAG_REMOTE) != 0;
    out->data_length = remote ? 0 : static_cast<uint16_t>(n);
    if (!remote && n > 0 && in.data != nullptr) {
        memcpy(out->data, in.data, n);
    }
}

/* An error frame that ends reception: bus-off, or the controller
 * restarting after one (ADR-0032 §6) */
inline bool socketcan_error_is_hard(uint32_t can_id)
{
    return (can_id & SOCKETCAN_ERR_FLAG) &&
           (can_id & (SOCKETCAN_ERR_BUSOFF | SOCKETCAN_ERR_RESTARTED)) != 0;
}

/* ---- The clock (ADR-0032 §5) ---- */

/* A frame is received before it's read, so a kernel delta that puts it
 * later than the read by more than this, or a backward one, means the
 * wall clock stepped: the host's delta stands in */
constexpr uint64_t SOCKETCAN_CLOCK_SLACK_US = 1000000;

struct socketcan_clock {
    bool started = false;
    uint64_t last_kernel_ns = 0;
    uint64_t last_host_us = 0;
    uint64_t last_out_us = 0;
};

/* A frame's session time from the kernel's receive time (ns, wall clock;
 * 0 when the frame came without one) and the session's host time at the
 * read (µs since attach). The first frame takes the host's time; each
 * later one adds the kernel's delta since the frame before, unless it
 * stepped. Frames read late after a stall keep their kernel spacing.
 * Never decreases. */
inline uint64_t socketcan_clock_step(socketcan_clock& c, uint64_t kernel_ns,
                                     uint64_t host_us)
{
    if (!c.started) {
        c.started = true;
        c.last_kernel_ns = kernel_ns;
        c.last_host_us = host_us;
        c.last_out_us = host_us;
        return host_us;
    }
    const uint64_t host_delta =
        host_us >= c.last_host_us ? host_us - c.last_host_us : 0;
    uint64_t delta = host_delta;
    if (kernel_ns != 0 && c.last_kernel_ns != 0 &&
        kernel_ns >= c.last_kernel_ns) {
        const uint64_t kernel_delta = (kernel_ns - c.last_kernel_ns) / 1000;
        if (c.last_out_us + kernel_delta <=
            host_us + SOCKETCAN_CLOCK_SLACK_US) {
            delta = kernel_delta;
        }
    }
    c.last_kernel_ns = kernel_ns;
    c.last_host_us = host_us;
    c.last_out_us += delta;
    return c.last_out_us;
}

#endif /* ECUIO_SOCKETCAN_WIRE_H */
