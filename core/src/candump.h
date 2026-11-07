// File/layer: core/src/candump.h | internal, C++17, Qt-free, pure; a
//             candump log's lines (ADR-0054 section 4)
// Testing: UNIT: CandumpTest (test_core); FUZZ: fuzz_candump
//
// can-utils' log format, as `candump -L` writes it and `canplayer` reads
// it: one frame a line,
//
//     (<seconds>.<microseconds>) <interface> <frame>[ R| T]
//
// the time six fraction digits, the interface a Linux name (1 to 15 of
// [A-Za-z0-9_.-]), the frame in cansend's notation (can-utils lib.h's
// comment on parse_canframe):
//
//     <can_id>#{data}          classic CAN, 0 to 8 bytes
//     <can_id>#{data}_{dlc}    eight bytes and a DLC of 9 to F
//     <can_id>#R{len}          a remote frame, an optional length 0 to 8
//     <can_id>#R{len}_{dlc}
//     <can_id>##<flags>{data}  CAN FD, 0 to 64 bytes, one hex digit of
//                              flags (BRS 0x1, ESI 0x2)
//
// <can_id> three hex digits (11 bits) or eight (29 bits; with the error
// flag 0x20000000, an error frame whose ID is its class bits, as
// SocketCAN reads one), data hex pairs with an optional '.' between any
// two. candump -x's direction ends a line: R received, T our own
// transmit. CAN XL (<vcid><prio>#<flags>:<sdt>:<af>#<data>) isn't read.
// Anything else is refused, with why; nothing here allocates.

#ifndef ECUIO_CANDUMP_H
#define ECUIO_CANDUMP_H

#include "ecuio/core.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ecuio_candump {

/* The longest line read: an FD frame's 64 bytes with a '.' between each,
 * and room around them */
constexpr size_t kLineMax = 512;
/* The longest interface name, IFNAMSIZ less its NUL */
constexpr size_t kInterfaceMax = 15;
/* SocketCAN's error flag in an ID (linux/can.h's CAN_ERR_FLAG) */
constexpr uint32_t kErrorFlag = 0x20000000u;
/* SocketCAN's FD flags (linux/can.h's CANFD_BRS, CANFD_ESI) */
constexpr uint8_t kBrs = 0x01;
constexpr uint8_t kEsi = 0x02;

struct line {
    uint64_t seconds = 0;
    uint32_t microseconds = 0;
    const char* iface = nullptr; /* into the line, not terminated */
    size_t iface_len = 0;
    uint32_t id = 0;      /* 11 or 29 bits; an error frame's class bits */
    uint32_t flags = 0;   /* ECUIO_FRAME_FLAG_* */
    uint8_t fd_flags = 0; /* an FD frame's flags digit */
    uint8_t length = 0;   /* data bytes, or a remote frame's length */
    uint8_t data[64] = {};
};

inline int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

inline bool interface_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

/* Data bytes from p to end: hex pairs, a '.' allowed between two, at most
 * max of them */
inline bool read_data(const char* p, const char* end, size_t max, line& out,
                      const char** why)
{
    size_t n = 0;
    bool after_dot = false;
    while (p < end) {
        if (*p == '.') {
            if (n == 0 || after_dot) {
                *why = "a '.' stands only between two data bytes";
                return false;
            }
            after_dot = true;
            ++p;
            continue;
        }
        const int hi = hex_value(p[0]);
        const int lo = p + 1 < end ? hex_value(p[1]) : -1;
        if (hi < 0 || lo < 0) {
            *why = "the data isn't hex byte pairs";
            return false;
        }
        if (n == max) {
            *why = max == 8 ? "a classic frame holds at most 8 data bytes"
                            : "an FD frame holds at most 64 data bytes";
            return false;
        }
        out.data[n++] = static_cast<uint8_t>((hi << 4) | lo);
        after_dot = false;
        p += 2;
    }
    if (after_dot) {
        *why = "a '.' stands only between two data bytes";
        return false;
    }
    out.length = static_cast<uint8_t>(n);
    return true;
}

/* One line, its line end removed, into out. False with why for anything
 * the format doesn't hold. */
inline bool parse_line(const char* p, size_t n, line& out, const char** why)
{
    out = line{};
    const char* end = p + n;
    if (n > kLineMax) {
        *why = "the line is longer than a candump line can be";
        return false;
    }
    /* (<seconds>.<microseconds>) */
    if (p == end || *p != '(') {
        *why = "the line doesn't start with its time in parentheses";
        return false;
    }
    ++p;
    const char* digits = p;
    uint64_t seconds = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (p - digits >= 12) {
            *why = "the time's seconds are past what a log holds";
            return false;
        }
        seconds = seconds * 10 + static_cast<uint64_t>(*p - '0');
        ++p;
    }
    if (p == digits || p == end || *p != '.') {
        *why = "the time isn't <seconds>.<microseconds>";
        return false;
    }
    ++p;
    uint32_t micro = 0;
    for (int i = 0; i < 6; ++i, ++p) {
        if (p == end || *p < '0' || *p > '9') {
            *why = "the time's fraction isn't six digits";
            return false;
        }
        micro = micro * 10 + static_cast<uint32_t>(*p - '0');
    }
    if (p == end || *p != ')') {
        *why = "the time's fraction isn't six digits";
        return false;
    }
    ++p;
    out.seconds = seconds;
    out.microseconds = micro;
    /* <interface> */
    if (p == end || *p != ' ') {
        *why = "a space doesn't follow the time";
        return false;
    }
    ++p;
    const char* iface = p;
    while (p < end && interface_char(*p)) {
        ++p;
    }
    const size_t iface_len = static_cast<size_t>(p - iface);
    if (iface_len == 0 || iface_len > kInterfaceMax || p == end || *p != ' ') {
        *why = "the interface isn't 1 to 15 of A-Z, a-z, 0-9, '_', '.' or "
               "'-', then a space";
        return false;
    }
    out.iface = iface;
    out.iface_len = iface_len;
    ++p;
    /* candump -x's direction, at the end */
    if (end - p >= 2 && end[-2] == ' ' && (end[-1] == 'R' || end[-1] == 'T')) {
        if (end[-1] == 'T') {
            out.flags |= ECUIO_FRAME_FLAG_TX_ECHO;
        }
        end -= 2;
    }
    /* <can_id># */
    const char* id_start = p;
    uint32_t id = 0;
    while (p < end && hex_value(*p) >= 0) {
        if (p - id_start == 8) {
            break;
        }
        id = (id << 4) | static_cast<uint32_t>(hex_value(*p));
        ++p;
    }
    const size_t id_len = static_cast<size_t>(p - id_start);
    if (p == end || *p != '#') {
        *why = "the frame isn't <id>#<data>, its ID 3 or 8 hex digits";
        return false;
    }
    /* CAN XL's <vcid><prio>, two hex digits and three */
    if (id_len == 5) {
        *why = "a CAN XL frame isn't read";
        return false;
    }
    if (id_len == 3) {
        if (id > 0x7FF) {
            *why = "an 11-bit ID is at most 7FF";
            return false;
        }
    } else if (id_len == 8) {
        if ((id & kErrorFlag) != 0) {
            if ((id & ~(kErrorFlag | 0x1FFFFFFFu)) != 0) {
                *why = "an error frame's ID holds no other flag";
                return false;
            }
            out.flags |= ECUIO_FRAME_FLAG_ERROR;
            id &= 0x1FFFFFFFu;
        } else if (id > 0x1FFFFFFFu) {
            *why = "a 29-bit ID is at most 1FFFFFFF";
            return false;
        } else {
            out.flags |= ECUIO_FRAME_FLAG_EXTENDED;
        }
    } else {
        *why = "the frame isn't <id>#<data>, its ID 3 or 8 hex digits";
        return false;
    }
    out.id = id;
    ++p;
    /* ##<flags>{data}: CAN FD */
    if (p < end && *p == '#') {
        if ((out.flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
            *why = "an error frame isn't an FD frame";
            return false;
        }
        ++p;
        const int f = p < end ? hex_value(*p) : -1;
        if (f < 0) {
            *why = "an FD frame's flags aren't one hex digit";
            return false;
        }
        out.fd_flags = static_cast<uint8_t>(f);
        out.flags |= ECUIO_FRAME_FLAG_FD;
        if ((f & kBrs) != 0) {
            out.flags |= ECUIO_FRAME_FLAG_BITRATE_SWITCH;
        }
        if ((f & kEsi) != 0) {
            out.flags |= ECUIO_FRAME_FLAG_FD_ESI;
        }
        ++p;
        return read_data(p, end, 64, out, why);
    }
    /* _{dlc} after eight bytes, or a remote frame's length */
    const char* data_end = end;
    if (end - p >= 2 && end[-2] == '_') {
        const int dlc = hex_value(end[-1]);
        if (dlc < 9) {
            *why = "a DLC after '_' is 9 to F";
            return false;
        }
        data_end = end - 2;
    }
    if (p < data_end && (*p == 'R' || *p == 'r')) {
        if ((out.flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
            *why = "an error frame isn't a remote frame";
            return false;
        }
        out.flags |= ECUIO_FRAME_FLAG_REMOTE;
        ++p;
        if (p == data_end) {
            return true;
        }
        if (data_end - p != 1 || *p < '0' || *p > '8') {
            *why = "a remote frame's length is one digit, 0 to 8";
            return false;
        }
        out.length = static_cast<uint8_t>(*p - '0');
        if (data_end != end && out.length != 8) {
            *why = "a DLC after '_' comes only with a length of 8";
            return false;
        }
        return true;
    }
    if (!read_data(p, data_end, 8, out, why)) {
        return false;
    }
    if (data_end != end && out.length != 8) {
        *why = "a DLC after '_' comes only with eight data bytes";
        return false;
    }
    return true;
}

} // namespace ecuio_candump

#endif /* ECUIO_CANDUMP_H */
