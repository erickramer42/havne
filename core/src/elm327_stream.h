// File/layer: core/src/elm327_stream.h | internal, C++17, Qt-free
// Testing: HARNESS: Elm327StreamTest (test_elm327): the fake ELM327
//          behind a registered stream, a pump thread playing the app: a
//          session through it, a failure and its reopen, an open that
//          never answers or fails, a stop during the wait, an unknown
//          name, the registry's refusals, the buffer's overflow, pushes
//          after close
//
// App-fed streams (ADR-0028, ecuio/stream.h) behind
// ecuio_elm327_port_ops_t (internal.h): the third kind of port beside the
// serial ports of elm327_port.h and the sockets of elm327_tcp.h. The app's
// pushes fill a fixed buffer that the worker's reads drain under ADR-0026's
// timeouts, so the protocol layer, the worker, link health and the wire
// log run as they do on a serial port.

#ifndef ECUIO_ELM327_STREAM_H
#define ECUIO_ELM327_STREAM_H

#include "ecuio/core.h"
#include "ecuio/internal.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

/* How long an open waits for the app's opened or failed: a BLE connect
 * with its service discovery takes seconds (ADR-0028 §1). */
constexpr uint32_t ELM327_STREAM_OPEN_MS = 10000;

/* The bytes received and not yet read that a stream holds, allocated at
 * register: far more than an adapter sends between two of the worker's
 * reads. */
constexpr uint32_t ELM327_STREAM_BUFFER = 64 * 1024;

/* Whether name (n bytes, not NUL-terminated) is a stream's name:
 * "<kind>:<id>", the kind one or more lower-case letters, the id one or
 * more printable ASCII characters but '?' and space, 127 bytes at most.
 * The serial and TCP names never take this shape. */
bool elm327_stream_name(const char* name, size_t n);

/* Opens the stream registered as name: calls its open, then waits up to
 * ELM327_STREAM_OPEN_MS for opened or failed, ending early once *stop is
 * set (stop may be NULL). ECUIO_OK, or LOAD_FAILED with the reason in why
 * ("no such port", failed's reason, "the link didn't open within 10 s"),
 * after the stream's close. */
ecuio_status_t elm327_stream_open(const char* name,
                                  const std::atomic<bool>* stop,
                                  ecuio_elm327_port_t* out, char* why,
                                  uint32_t why_cap);

#endif /* ECUIO_ELM327_STREAM_H */
