// File/layer: core/src/access_cut.h | C++17, Qt-free, pure, header-only
// Testing: UNIT: AccessCutTest (test_core)
//
// A security access's seed and key kept out of every capture (ADR-0066
// section 5): a PDU of service 27 or its answer 67 keeps its service and
// sub-function, the bytes after them are zeroed, and the capture's line
// says their length and SHA-256. A module's memory is kept out the same
// way (ADR-0066 section 4), or a capture running beside a dump would be a
// second copy of it: a read's answer 63 keeps its service, a transfer's
// answer 76 its service and block counter. The recorder's frames, the
// ELM327 wire log and the J2534 surrogate's session log share it; nothing
// here allocates, so a worker and the surrogate's child may call it.

#ifndef ECUIO_ACCESS_CUT_H
#define ECUIO_ACCESS_CUT_H

#include "sha256.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

/* The bytes a cut PDU keeps: its service and sub-function */
constexpr size_t ECUIO_ACCESS_KEPT = 2;
/* Room for the words of ecuio_access_cut_text, terminated */
constexpr size_t ECUIO_ACCESS_CUT_TEXT = 96;

/* Whether pdu (len bytes) is a security access's with bytes to cut */
inline bool ecuio_access_cut(const uint8_t* pdu, size_t len)
{
    return len > ECUIO_ACCESS_KEPT && (pdu[0] == 0x27 || pdu[0] == 0x67);
}

/* Whether sid asks for a module's memory: read memory by address, and a
 * transfer's block (an upload's here: tier3_ceiling.h has no download) */
constexpr bool ecuio_memory_request(uint8_t sid)
{
    return sid == 0x23 || sid == 0x36;
}

/* How many of pdu's first bytes a capture keeps, 0 for a PDU it keeps
 * whole: a security access's two, a 63's one, a 76's two, each only with
 * bytes after them */
inline size_t ecuio_capture_kept(const uint8_t* pdu, size_t len)
{
    if (len == 0) {
        return 0;
    }
    size_t kept = 0;
    if (pdu[0] == 0x63) {
        kept = 1;
    } else if (pdu[0] == 0x76 || pdu[0] == 0x27 || pdu[0] == 0x67) {
        kept = ECUIO_ACCESS_KEPT;
    }
    return len > kept ? kept : 0;
}

/* "N bytes cut, SHA-256 <64 lower-case digits>" of the bytes after pdu's
 * first kept, into out (cap at least ECUIO_ACCESS_CUT_TEXT) */
inline void ecuio_capture_cut_text(const uint8_t* pdu, size_t len, size_t kept,
                                   char* out, size_t cap)
{
    const size_t cut = len > kept ? len - kept : 0;
    ecuio_sha256::hasher h;
    h.add(pdu + (len > kept ? kept : len), cut);
    uint8_t digest[32];
    h.finish(digest);
    int n = std::snprintf(out, cap, "%lu bytes cut, SHA-256 ",
                          static_cast<unsigned long>(cut));
    for (size_t i = 0; i < sizeof digest && n > 0 &&
                       static_cast<size_t>(n) + 2 < cap;
         ++i) {
        n += std::snprintf(out + n, cap - static_cast<size_t>(n), "%02x",
                           digest[i]);
    }
}

/* The same of a security access's bytes after its sub-function */
inline void ecuio_access_cut_text(const uint8_t* pdu, size_t len, char* out,
                                  size_t cap)
{
    ecuio_capture_cut_text(pdu, len, ECUIO_ACCESS_KEPT, out, cap);
}

#endif /* ECUIO_ACCESS_CUT_H */
