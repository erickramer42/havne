// File/layer: fuzz/fuzz_candump.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_candump
//
// candump.h's line parser (ADR-0054 section 4) over arbitrary bytes, a
// line at a time.

#include "candump.h"

#include <ecuio/core.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

namespace dump = ecuio_candump;

constexpr uint32_t kFlags =
    ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_REMOTE | ECUIO_FRAME_FLAG_FD |
    ECUIO_FRAME_FLAG_BITRATE_SWITCH | ECUIO_FRAME_FLAG_ERROR |
    ECUIO_FRAME_FLAG_TX_ECHO | ECUIO_FRAME_FLAG_FD_ESI;

bool same(const dump::line& a, const dump::line& b)
{
    return a.seconds == b.seconds && a.microseconds == b.microseconds &&
           a.iface_len == b.iface_len &&
           memcmp(a.iface, b.iface, a.iface_len) == 0 && a.id == b.id &&
           a.flags == b.flags && a.fd_flags == b.fd_flags &&
           a.length == b.length && memcmp(a.data, b.data, sizeof a.data) == 0;
}

/* The line as candump -L writes it */
std::string printed(const dump::line& l)
{
    char buf[64];
    snprintf(buf, sizeof buf, "(%llu.%06u) ",
             static_cast<unsigned long long>(l.seconds), l.microseconds);
    std::string out = buf;
    out.append(l.iface, l.iface_len);
    if ((l.flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
        snprintf(buf, sizeof buf, " %08X#", l.id | dump::kErrorFlag);
    } else if ((l.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0) {
        snprintf(buf, sizeof buf, " %08X#", l.id);
    } else {
        snprintf(buf, sizeof buf, " %03X#", l.id);
    }
    out += buf;
    if ((l.flags & ECUIO_FRAME_FLAG_FD) != 0) {
        snprintf(buf, sizeof buf, "#%X", l.fd_flags);
        out += buf;
    }
    if ((l.flags & ECUIO_FRAME_FLAG_REMOTE) != 0) {
        out += 'R';
        if (l.length > 0) {
            out += static_cast<char>('0' + l.length);
        }
    } else {
        for (uint8_t i = 0; i < l.length; ++i) {
            snprintf(buf, sizeof buf, "%02X", l.data[i]);
            out += buf;
        }
    }
    if ((l.flags & ECUIO_FRAME_FLAG_TX_ECHO) != 0) {
        out += " T";
    }
    return out;
}

void one(const char* p, size_t n)
{
    dump::line a;
    dump::line b;
    const char* why_a = nullptr;
    const char* why_b = nullptr;
    const bool ok = dump::parse_line(p, n, a, &why_a);
    if (ok != dump::parse_line(p, n, b, &why_b) || (ok && !same(a, b))) {
        abort();
    }
    if (!ok) {
        if (why_a == nullptr || *why_a == '\0' || why_a != why_b) {
            abort();
        }
        return;
    }
    if (a.iface_len == 0 || a.iface_len > dump::kInterfaceMax || a.iface < p ||
        a.iface + a.iface_len > p + n) {
        abort();
    }
    for (size_t i = 0; i < a.iface_len; ++i) {
        if (!dump::interface_char(a.iface[i])) {
            abort();
        }
    }
    if (a.microseconds > 999999 || (a.flags & ~kFlags) != 0) {
        abort();
    }
    const bool fd = (a.flags & ECUIO_FRAME_FLAG_FD) != 0;
    const bool wide =
        (a.flags & (ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_ERROR)) != 0;
    if (a.id > (wide ? 0x1FFFFFFFu : 0x7FFu) || a.length > (fd ? 64 : 8)) {
        abort();
    }
    if ((a.flags & ECUIO_FRAME_FLAG_ERROR) != 0 &&
        (a.flags & (ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_FD)) != 0) {
        abort(); /* an error frame is classic, its ID its class bits */
    }
    if (!fd && (a.flags & (ECUIO_FRAME_FLAG_BITRATE_SWITCH |
                           ECUIO_FRAME_FLAG_FD_ESI)) != 0) {
        abort(); /* BRS or ESI only on an FD frame */
    }
    if (fd && (((a.fd_flags & dump::kBrs) != 0) !=
                   ((a.flags & ECUIO_FRAME_FLAG_BITRATE_SWITCH) != 0) ||
               ((a.fd_flags & dump::kEsi) != 0) !=
                   ((a.flags & ECUIO_FRAME_FLAG_FD_ESI) != 0))) {
        abort();
    }
    if (!fd && a.fd_flags != 0) {
        abort();
    }
    if ((a.flags & ECUIO_FRAME_FLAG_REMOTE) != 0) {
        if (fd || (a.flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
            abort();
        }
        for (uint8_t d : a.data) {
            if (d != 0) {
                abort();
            }
        }
    }
    for (size_t i = a.length; i < sizeof a.data; ++i) {
        if ((a.flags & ECUIO_FRAME_FLAG_REMOTE) == 0 && a.data[i] != 0) {
            abort();
        }
    }
    /* What it read, written as candump writes it, reads the same */
    const std::string again = printed(a);
    dump::line c;
    const char* why_c = nullptr;
    if (!dump::parse_line(again.data(), again.size(), c, &why_c)) {
        abort();
    }
    c.iface = a.iface; /* the same bytes, in another buffer */
    if (memcmp(again.data() + again.find(' ') + 1, a.iface, a.iface_len) != 0 ||
        !same(a, c)) {
        abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* A few lines are enough; a line past kLineMax is refused unread */
    if (size > 4 * dump::kLineMax) {
        return 0;
    }
    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + size;
    while (p <= end) {
        const char* nl = static_cast<const char*>(
            memchr(p, '\n', static_cast<size_t>(end - p)));
        const char* stop = nl != nullptr ? nl : end;
        size_t n = static_cast<size_t>(stop - p);
        if (n > 0 && p[n - 1] == '\r') {
            --n;
        }
        one(p, n);
        if (nl == nullptr) {
            break;
        }
        p = nl + 1;
    }
    return 0;
}
