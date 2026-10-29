// File/layer: core/src/elm327_tcp.h | internal, C++17, Qt-free
// Testing: UNIT — Elm327TcpTest.AddressesAreADottedIpv4AndAPort and
//          .ConnectFailuresAreWorded (test_elm327): the address rules,
//          a failed connect's words; HARNESS — Elm327TcpTest
//          (test_elm327_session, every platform): the socket against the
//          fake ELM327 behind a loopback listener
//
// A WiFi adapter's TCP socket behind ecuio_elm327_port_ops_t (internal.h),
// the second kind of port beside the serial ports of elm327_port.h
// (ADR-0026's appendix "WiFi over TCP"). An adapter runs its own access
// point at a fixed address, so a URI names it by a dotted IPv4 address and
// a port: no name lookup, which can block with no timeout core controls.

#ifndef ECUIO_ELM327_TCP_H
#define ECUIO_ELM327_TCP_H

#include "ecuio/core.h"
#include "ecuio/internal.h"

#include <cstddef>
#include <cstdint>

/* How long a connect may take. On the adapter's own network an answer
 * takes milliseconds; the limit also bounds how long a close waits on a
 * worker that is reconnecting. */
constexpr uint32_t ELM327_TCP_CONNECT_MS = 2000;

/* Whether name (n bytes, not NUL-terminated) is a TCP address: four
 * decimal octets 0 to 255 joined by dots, a colon, and a port 1 to 65535,
 * digits only, no leading zeros (a leading 0 reads as octal elsewhere).
 * The address (host order) and the port go to ip and port when not NULL. */
bool elm327_tcp_address(const char* name, size_t n, uint32_t* ip,
                        uint16_t* port);

/* What an address looks like, for a refusal's detail. */
const char* elm327_tcp_example();

/* A failed connect's reason in the port's words, from the socket's error
 * (a Windows sockets code on Windows, errno elsewhere); a code with no
 * words of its own is named by its number or strerror. */
void elm327_tcp_describe(int error, char* why, uint32_t why_cap);

/* Connects to name, a TCP address, within ELM327_TCP_CONNECT_MS: Nagle
 * off, no SIGPIPE. ECUIO_OK, or LOAD_FAILED with the reason in why
 * ("the connection was refused", "no answer within 2 s …"). The far side
 * closing is a failed read, as a pulled cable's is. */
ecuio_status_t elm327_tcp_open(const char* name, ecuio_elm327_port_t* out,
                               char* why, uint32_t why_cap);

#endif /* ECUIO_ELM327_TCP_H */
