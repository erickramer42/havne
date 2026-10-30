// File/layer: fuzz/fuzz_socketcan_wire.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the SocketCAN parsers of outside input
//          (core/src/socketcan_wire.h, ADR-0032), the first byte picking
//          one: ?filter= text (at most 8 filters, each ID within its
//          width, flagged 29-bit exactly when above 0x7FF, the mask
//          within the ID's width and carrying the width flag), an
//          RTM_NEWLINK payload from the kernel (the name and kind
//          terminated, what discovery lists and the verdict on it
//          consistent with the link), or a kernel frame (the length
//          within its kind's room, only the flags it can carry). Every
//          input is a heap copy of its exact size, so ASan sees a read
//          past it; two runs agree.

#include <ecuio/core.h>

#include "socketcan_wire.h" /* core/src: pure, no platform headers */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

bool terminated(const char* s, size_t cap)
{
    return memchr(s, '\0', cap) != nullptr;
}

void filters(const uint8_t* data, size_t size)
{
    /* Exactly the room the transport gives the parser */
    std::unique_ptr<socketcan_filter[]> a(
        new socketcan_filter[SOCKETCAN_MAX_FILTERS]);
    std::unique_ptr<socketcan_filter[]> b(
        new socketcan_filter[SOCKETCAN_MAX_FILTERS]);
    const char* s = reinterpret_cast<const char*>(data);
    const size_t n = socketcan_parse_filters(s, size, a.get());
    if (n > SOCKETCAN_MAX_FILTERS)
        abort();
    for (size_t i = 0; i < n; ++i) {
        const bool extended = (a[i].id & SOCKETCAN_EFF_FLAG) != 0;
        const uint32_t id = a[i].id & ~SOCKETCAN_EFF_FLAG;
        const uint32_t width =
            extended ? SOCKETCAN_EFF_MASK : SOCKETCAN_SFF_MASK;
        if (id > width)
            abort();
        if (extended != (id > SOCKETCAN_SFF_MASK))
            abort();
        if (!(a[i].mask & SOCKETCAN_EFF_FLAG))
            abort();
        if ((a[i].mask & ~SOCKETCAN_EFF_FLAG & ~width) != 0)
            abort();
    }
    if (socketcan_parse_filters(s, size, b.get()) != n)
        abort();
    for (size_t i = 0; i < n; ++i) {
        if (a[i].id != b[i].id || a[i].mask != b[i].mask)
            abort();
    }
}

bool same(const socketcan_link& x, const socketcan_link& y)
{
    return strcmp(x.name, y.name) == 0 && x.index == y.index && x.up == y.up &&
           strcmp(x.kind, y.kind) == 0 && x.mtu == y.mtu &&
           x.has_bitrate == y.has_bitrate && x.bitrate == y.bitrate &&
           x.has_ctrlmode == y.has_ctrlmode && x.ctrlmode == y.ctrlmode;
}

void link(const uint8_t* data, size_t size)
{
    socketcan_link a;
    socketcan_link b;
    const bool ok = socketcan_parse_link(data, size, &a);
    if (socketcan_parse_link(data, size, &b) != ok || !same(a, b))
        abort();
    if (!terminated(a.name, sizeof a.name) ||
        !terminated(a.kind, sizeof a.kind)) {
        abort();
    }
    if (!ok)
        return;

    ecuio_can_interface_t listed;
    const bool can = a.is_can() || a.is_vcan();
    if (socketcan_interface_from(a, &listed) != can)
        abort();
    if (can) {
        if (!terminated(listed.name, sizeof listed.name) ||
            !terminated(listed.kind, sizeof listed.kind)) {
            abort();
        }
        if (((listed.flags & ECUIO_CAN_UP) != 0) != a.up)
            abort();
        if (a.is_vcan() && listed.bitrate != 0)
            abort();
    }
    const socketcan_verdict v = socketcan_judge(a, 0);
    if ((v == socketcan_verdict::not_can) == can)
        abort();
    if (v == socketcan_verdict::ok && !a.up)
        abort();
    if (v == socketcan_verdict::ok && a.is_can() && !a.listen_only())
        abort();
    if (v == socketcan_verdict::wrong_rate)
        abort(); /* no rate asked */
    const socketcan_verdict r = socketcan_judge(a, 500000);
    if (r != v && !(v == socketcan_verdict::ok &&
                    r == socketcan_verdict::wrong_rate && a.is_can())) {
        abort();
    }
}

void frame(const uint8_t* data, size_t size)
{
    if (size < 6)
        return;
    socketcan_raw in;
    memcpy(&in.can_id, data, 4);
    in.len = data[4];
    in.fd = (data[5] & 0x80) != 0;
    in.fd_flags = static_cast<uint8_t>(data[5] & 0x7F);
    /* The kernel's buffer holds the kind's room; the rest is the data */
    const size_t room = in.fd ? 64 : 8;
    std::unique_ptr<uint8_t[]> payload(new uint8_t[room]());
    const size_t have = size - 6 < room ? size - 6 : room;
    if (have > 0)
        memcpy(payload.get(), data + 6, have);
    in.data = payload.get();

    std::unique_ptr<ecuio_frame_t> f(new ecuio_frame_t());
    socketcan_to_frame(in, f.get());
    const uint32_t known = ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_REMOTE |
                           ECUIO_FRAME_FLAG_FD |
                           ECUIO_FRAME_FLAG_BITRATE_SWITCH |
                           ECUIO_FRAME_FLAG_ERROR;
    if (f->flags & ~known)
        abort();
    if (f->data_length > room)
        abort();
    if (f->arbitration_id > SOCKETCAN_EFF_MASK)
        abort();
    const bool error = (f->flags & ECUIO_FRAME_FLAG_ERROR) != 0;
    if (error && (f->flags & ECUIO_FRAME_FLAG_REMOTE))
        abort();
    if ((f->flags & ECUIO_FRAME_FLAG_REMOTE) && f->data_length != 0)
        abort();
    if (!error && !(f->flags & ECUIO_FRAME_FLAG_EXTENDED) &&
        f->arbitration_id > SOCKETCAN_SFF_MASK) {
        abort();
    }
    if (f->reserved != 0 || f->timestamp_us != 0)
        abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* A netlink dump's message is a few hundred bytes; ?filter= text
     * far less */
    if (size < 1 || size > 8192)
        return 0;
    std::unique_ptr<uint8_t[]> rest(new uint8_t[size - 1]);
    if (size > 1)
        memcpy(rest.get(), data + 1, size - 1);
    switch (data[0] % 3) {
    case 0:
        filters(rest.get(), size - 1);
        break;
    case 1:
        link(rest.get(), size - 1);
        break;
    default:
        frame(rest.get(), size - 1);
        break;
    }
    return 0;
}
