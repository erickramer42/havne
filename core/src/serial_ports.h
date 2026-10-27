// File/layer: core/src/serial_ports.h | internal, C++17, Qt-free, header-only
// Testing: UNIT — SerialPortsTest (test_elm327): the order, the dedupe,
//          the capacity and the /dev names, over made-up ports on every
//          platform
//
// The rules behind ecuio_serial_ports (serial.h) that need no OS: which
// /dev names are listed, the order, and the copy out. serial_ports.cpp
// only gathers the candidates.

#ifndef ECUIO_SERIAL_PORTS_H
#define ECUIO_SERIAL_PORTS_H

#include "ecuio/serial.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

inline bool serial_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Whether a sorts before b: byte by byte, but a run of digits by its
 * value, so COM2 comes before COM10 and ttyUSB9 before ttyUSB10. */
inline bool serial_name_less(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        if (serial_is_digit(*a) && serial_is_digit(*b)) {
            while (*a == '0')
                ++a;
            while (*b == '0')
                ++b;
            size_t na = 0;
            size_t nb = 0;
            while (serial_is_digit(a[na]))
                ++na;
            while (serial_is_digit(b[nb]))
                ++nb;
            if (na != nb) {
                return na < nb;
            }
            const int c = memcmp(a, b, na);
            if (c != 0) {
                return c < 0;
            }
            a += na;
            b += nb;
            continue;
        }
        if (*a != *b) {
            return static_cast<unsigned char>(*a) <
                   static_cast<unsigned char>(*b);
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b != '\0';
}

/* Whether a /dev entry is listed (serial.h): on macOS every cu.* (the
 * call-out side, which opens without waiting for carrier); elsewhere
 * ttyUSB, ttyACM or rfcomm followed by a number. */
inline bool serial_dev_listed(const char* entry, bool apple)
{
    if (apple) {
        return strncmp(entry, "cu.", 3) == 0 && entry[3] != '\0';
    }
    for (const char* prefix : {"ttyUSB", "ttyACM", "rfcomm"}) {
        const size_t n = strlen(prefix);
        if (strncmp(entry, prefix, n) != 0 || entry[n] == '\0') {
            continue;
        }
        const char* p = entry + n;
        while (serial_is_digit(*p))
            ++p;
        return *p == '\0';
    }
    return false;
}

/* A candidate from a name and the driver's device: false when the name
 * doesn't fit; a device too long is left "". */
inline bool serial_port_make(const char* name, const char* device,
                             ecuio_serial_port_t* out)
{
    *out = ecuio_serial_port_t{};
    const size_t n = strlen(name);
    if (n == 0 || n >= sizeof(out->name)) {
        return false;
    }
    memcpy(out->name, name, n);
    const size_t d = strlen(device);
    if (d < sizeof(out->device)) {
        memcpy(out->device, device, d);
    }
    return true;
}

/* Sorts the candidates, drops a repeated name (the first kept), and
 * copies out serial.h's way: *count all of them, ports the first
 * capacity. Names equal by value ("COM5", "COM05") fall back to byte
 * order, so a name's repeats always end up side by side. */
inline void serial_ports_finish(std::vector<ecuio_serial_port_t>& found,
                                ecuio_serial_port_t* ports, uint32_t capacity,
                                uint32_t* count)
{
    std::stable_sort(
        found.begin(), found.end(),
        [](const ecuio_serial_port_t& a, const ecuio_serial_port_t& b) {
            if (serial_name_less(a.name, b.name))
                return true;
            if (serial_name_less(b.name, a.name))
                return false;
            return strcmp(a.name, b.name) < 0;
        });
    found.erase(std::unique(found.begin(), found.end(),
                            [](const ecuio_serial_port_t& a,
                               const ecuio_serial_port_t& b) {
                                return strcmp(a.name, b.name) == 0;
                            }),
                found.end());
    *count = static_cast<uint32_t>(found.size());
    const size_t n = std::min<size_t>(found.size(), capacity);
    std::copy(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(n),
              ports);
}

#endif /* ECUIO_SERIAL_PORTS_H */
