// File/layer: core/src/access_cut.h | C++17, Qt-free, pure, header-only
// Testing: UNIT: AccessCutTest (test_core)
//
// A security access's seed and key kept out of every capture (ADR-0066
// section 5): a PDU of service 27 or its answer 67 keeps its service and
// sub-function, the bytes after them are zeroed, and the capture's line
// says their length and SHA-256. The recorder's frames, the ELM327 wire
// log and the J2534 surrogate's session log share it; nothing here
// allocates, so a worker and the surrogate's child may call it.

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

/* "N bytes cut, SHA-256 <64 lower-case digits>" of the bytes after the
 * sub-function, into out (cap at least ECUIO_ACCESS_CUT_TEXT) */
inline void ecuio_access_cut_text(const uint8_t* pdu, size_t len, char* out,
                                  size_t cap)
{
    const size_t cut = len > ECUIO_ACCESS_KEPT ? len - ECUIO_ACCESS_KEPT : 0;
    ecuio_sha256::hasher h;
    h.add(pdu + (len > ECUIO_ACCESS_KEPT ? ECUIO_ACCESS_KEPT : len), cut);
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

#endif /* ECUIO_ACCESS_CUT_H */
