// File/layer: core/src/elm327_port.h | internal, C++17, Qt-free
// Testing: HARNESS: Elm327PortTest (test_elm327, POSIX): the termios
//          port over a pseudo-terminal, its read timeouts and its end
//          when the far side closes, and a whole session through it;
//          the Win32 port is verified at BENCH (ADR-0026 §8)
//
// The OS's serial ports behind ecuio_elm327_port_ops_t (internal.h):
// CreateFileW on \\.\COMn on Windows, open and termios elsewhere. A
// paired Bluetooth SPP adapter is a serial port on all three desktop
// systems, so this covers USB and classic Bluetooth alike (ADR-0026 §1).
// A WiFi adapter's socket is elm327_tcp.h's, reached through the same
// opener.

#ifndef ECUIO_ELM327_PORT_H
#define ECUIO_ELM327_PORT_H

#include "ecuio/core.h"
#include "ecuio/internal.h"

#include <cstddef>
#include <cstdint>

/* The longest port name a URI may give. */
constexpr size_t ELM327_PORT_NAME_MAX = 127;

/* Whether name (n bytes, not NUL-terminated) names a port this OS's
 * opener takes: COM1 to COM255, in any case, on Windows; an absolute
 * /dev/ path with no control characters elsewhere. */
bool elm327_port_name_ok(const char* name, size_t n);

/* What a valid name looks like here, for a refusal's detail. */
const char* elm327_port_name_example();

/* The OS's opener, ecuio_elm327_port_open_t's contract: 8 data bits, no
 * parity, one stop bit, no flow control, at serial_baud. A Bluetooth
 * port takes the setting and ignores it. A TCP address
 * (elm327_tcp_address) goes to elm327_tcp_open, which has no rate. */
ecuio_status_t elm327_port_open_os(const char* name, uint32_t serial_baud,
                                   ecuio_elm327_port_t* out, char* why,
                                   uint32_t why_cap);

#endif /* ECUIO_ELM327_PORT_H */
