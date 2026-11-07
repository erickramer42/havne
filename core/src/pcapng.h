// File/layer: core/src/pcapng.h | internal, C++17, Qt-free, pure; pcapng's
//             blocks for a CAN export (ADR-0054 section 3)
// Testing: UNIT: ExportTest (test_core)
//
// The pcapng file format (IETF draft-ietf-opsawg-pcapng): a Section Header
// Block, one Interface Description Block per bus, then an Enhanced Packet
// Block per frame, each block's length a multiple of 4 and given at both
// ends, written little-endian (the Section Header's byte-order magic says
// which). Packets are LINKTYPE_CAN_SOCKETCAN (227): SocketCAN's struct
// can_frame or canfd_frame, the ID and its flags big-endian, as
// libpcap captures them on Linux: 16 bytes for a classic frame, 72 for
// CAN FD.

#ifndef ECUIO_PCAPNG_H
#define ECUIO_PCAPNG_H

#include "ecuio/core.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ecuio_pcapng {

constexpr uint32_t kSectionHeader = 0x0A0D0D0Au;
constexpr uint32_t kInterface = 0x00000001u;
constexpr uint32_t kEnhancedPacket = 0x00000006u;
constexpr uint32_t kByteOrderMagic = 0x1A2B3C4Du;
constexpr uint16_t kLinkCanSocketCan = 227;

/* Options */
constexpr uint16_t kOptEnd = 0;
constexpr uint16_t kOptComment = 1;
constexpr uint16_t kShbUserAppl = 4;
constexpr uint16_t kIfTsresol = 9;
constexpr uint16_t kEpbFlags = 2;
/* epb_flags' direction, bits 0-1 */
constexpr uint32_t kOutbound = 0x2;

/* SocketCAN's can_id flags and canfd_frame's flags (linux/can.h) */
constexpr uint32_t kEffFlag = 0x80000000u;
constexpr uint32_t kRtrFlag = 0x40000000u;
constexpr uint32_t kErrFlag = 0x20000000u;
constexpr uint8_t kFdBrs = 0x01;
constexpr uint8_t kFdEsi = 0x02;
constexpr uint8_t kFdFdf = 0x04; /* CANFD_FDF: an FD frame, in a capture */
constexpr uint32_t kClassicBytes = 16;
constexpr uint32_t kFdBytes = 72;

inline void u16(std::string& out, uint16_t v)
{
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>(v >> 8);
}

inline void u32(std::string& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out += static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}

inline void pad4(std::string& out, size_t from)
{
    while ((out.size() - from) % 4 != 0) {
        out += '\0';
    }
}

inline void option(std::string& out, uint16_t code, const void* value,
                   uint16_t n)
{
    u16(out, code);
    u16(out, n);
    const size_t at = out.size();
    if (n > 0) {
        out.append(static_cast<const char*>(value), n);
    }
    pad4(out, at);
}

inline void option_text(std::string& out, uint16_t code, const std::string& v)
{
    option(out, code, v.data(),
           static_cast<uint16_t>(v.size() < 0xFFFC ? v.size() : 0xFFFC));
}

/* A block's start: its type and room for its length, filled by end */
inline size_t begin(std::string& out, uint32_t type)
{
    const size_t at = out.size();
    u32(out, type);
    u32(out, 0);
    return at;
}

inline void end(std::string& out, size_t at)
{
    const auto len = static_cast<uint32_t>(out.size() - at + 4);
    u32(out, len);
    for (int i = 0; i < 4; ++i) {
        out[at + 4 + i] = static_cast<char>((len >> (8 * i)) & 0xFF);
    }
}

/* The section, of unknown length, with a comment and the application */
inline void section_header(std::string& out, const std::string& comment,
                           const std::string& application)
{
    const size_t at = begin(out, kSectionHeader);
    u32(out, kByteOrderMagic);
    u16(out, 1); /* major */
    u16(out, 0); /* minor */
    u32(out, 0xFFFFFFFFu);
    u32(out, 0xFFFFFFFFu); /* section length: not given */
    option_text(out, kOptComment, comment);
    option_text(out, kShbUserAppl, application);
    option(out, kOptEnd, nullptr, 0);
    end(out, at);
}

/* A CAN bus, its times in microseconds (if_tsresol 6) */
inline void interface_block(std::string& out)
{
    const size_t at = begin(out, kInterface);
    u16(out, kLinkCanSocketCan);
    u16(out, 0);
    u32(out, 0); /* snaplen: no limit */
    const uint8_t tsresol = 6;
    option(out, kIfTsresol, &tsresol, 1);
    option(out, kOptEnd, nullptr, 0);
    end(out, at);
}

/* A frame as SocketCAN holds it: false for one no CAN frame can be (an FD
 * frame past 64 bytes, a classic one past 8) */
inline bool socketcan(const ecuio_frame_t& f, uint8_t out[kFdBytes],
                      uint32_t* n)
{
    const bool fd = (f.flags & ECUIO_FRAME_FLAG_FD) != 0;
    if (f.data_length > (fd ? 64u : 8u)) {
        return false;
    }
    std::memset(out, 0, kFdBytes);
    uint32_t id = f.arbitration_id;
    if ((f.flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
        id = (id & 0x1FFFFFFFu) | kErrFlag;
    } else {
        if ((f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0) {
            id = (id & 0x1FFFFFFFu) | kEffFlag;
        } else {
            id &= 0x7FFu;
        }
        if ((f.flags & ECUIO_FRAME_FLAG_REMOTE) != 0) {
            id |= kRtrFlag;
        }
    }
    out[0] = static_cast<uint8_t>(id >> 24);
    out[1] = static_cast<uint8_t>(id >> 16);
    out[2] = static_cast<uint8_t>(id >> 8);
    out[3] = static_cast<uint8_t>(id);
    out[4] = static_cast<uint8_t>(f.data_length);
    if (fd) {
        out[5] = kFdFdf;
        if ((f.flags & ECUIO_FRAME_FLAG_BITRATE_SWITCH) != 0) {
            out[5] |= kFdBrs;
        }
        if ((f.flags & ECUIO_FRAME_FLAG_FD_ESI) != 0) {
            out[5] |= kFdEsi;
        }
    }
    if ((f.flags & ECUIO_FRAME_FLAG_REMOTE) == 0) {
        std::memcpy(out + 8, f.data, f.data_length);
    }
    *n = fd ? kFdBytes : kClassicBytes;
    return true;
}

/* A packet on interface 0 at ts_us (since 1970 UTC), with a comment when
 * it's given and epb_flags when flags isn't 0 */
inline void enhanced_packet(std::string& out, uint64_t ts_us,
                            const uint8_t* data, uint32_t n,
                            const std::string* comment, uint32_t flags)
{
    const size_t at = begin(out, kEnhancedPacket);
    u32(out, 0); /* interface */
    u32(out, static_cast<uint32_t>(ts_us >> 32));
    u32(out, static_cast<uint32_t>(ts_us & 0xFFFFFFFFu));
    u32(out, n); /* captured */
    u32(out, n); /* original */
    const size_t data_at = out.size();
    out.append(reinterpret_cast<const char*>(data), n);
    pad4(out, data_at);
    if (comment != nullptr) {
        option_text(out, kOptComment, *comment);
    }
    if (flags != 0) {
        std::string v;
        u32(v, flags);
        option(out, kEpbFlags, v.data(), 4);
    }
    if (comment != nullptr || flags != 0) {
        option(out, kOptEnd, nullptr, 0);
    }
    end(out, at);
}

} // namespace ecuio_pcapng

#endif /* ECUIO_PCAPNG_H */
