// File/layer: core/src/operation_plan.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: PluginOperationTest (test_core)
//
// An operation's plan (ADR-0066 section 4), as a plugin's manifest
// declares it: an ordered list of steps on one module, and the rules a
// plan is judged by that need no manifest to state: what each step may
// ask, what core derives from the steps (the class, the unlock attempts,
// the image's size), and which ranges share an address. The manifest's
// reader (plugin_operations.cpp) and the run (operation_run.h) both read
// these.

#ifndef ECUIO_OPERATION_PLAN_H
#define ECUIO_OPERATION_PLAN_H

#include "ecuio/dump.h"
#include "ecuio/plugin.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecuio_operation {

/* A 23's data fits one ISO-TP message with its 63: 4095 less one */
constexpr uint32_t kChunkMax = 4094;

/* A plan's steps are a copy's ranges, one for one, with the same words */
static_assert(ECUIO_PLUGIN_MAX_STEPS == ECUIO_DUMP_MAX_RANGES,
              "a copy holds a range for each step a plan may have");
static_assert(ECUIO_PLUGIN_MAX_IDENTS == ECUIO_DUMP_MAX_IDENTS,
              "a copy holds each identifier a plan may name");
static_assert(ECUIO_STEP_READ == ECUIO_DUMP_STEP_READ,
              "a read step is a copy's read range");
static_assert(ECUIO_STEP_UPLOAD == ECUIO_DUMP_STEP_UPLOAD,
              "an upload step is a copy's upload range");
static_assert(ECUIO_STEP_SESSION_EXTENDED == ECUIO_DUMP_SESSION_EXTENDED,
              "a step's extended session is a range's");
static_assert(ECUIO_STEP_SESSION_PROGRAMMING == ECUIO_DUMP_SESSION_PROGRAMMING,
              "a step's programming session is a range's");

/* One step: a range of the module's memory and how it is asked */
struct step {
    uint32_t kind = ECUIO_STEP_READ;
    uint32_t session = ECUIO_STEP_SESSION_EXTENDED;
    uint64_t address = 0;
    uint64_t bytes = 0;
    uint32_t chunk = 0;        /* a read's bytes for each request; 0 for an
                                  upload, whose block the module names */
    uint8_t address_bytes = 4; /* the request's address, 1 to 4 bytes */
    uint8_t size_bytes = 2;    /* its size, 1 to 4 bytes */
    uint8_t dfi = 0;           /* an upload's data format: 00 alone */
    std::string name;
};

/* An identification DID read before the first byte and after the last */
struct ident {
    uint32_t did = 0;
    uint32_t role = ECUIO_DUMP_ROLE_HARDWARE;
};

struct plan {
    std::string id;
    std::string title;
    uint32_t kind = ECUIO_OPERATION_KIND_DUMP;
    uint32_t ecu = 0; /* its place in the conversation's ecus */
    uint32_t rx_id = 0;
    uint32_t tx_id = 0;
    uint8_t level = 0; /* the security level its reads need */
    std::vector<ident> idents;
    std::vector<step> steps;
};

/* The most an unsigned value of n bytes (1 to 4) holds */
constexpr uint64_t most_of(unsigned n)
{
    return (uint64_t{1} << (8 * n)) - 1;
}

/* Why a step can't be asked as it is declared, or nullptr. The rules are
 * the request's own: each number fits the bytes that carry it. */
inline const char* step_fault(const step& s)
{
    if (s.kind != ECUIO_STEP_READ && s.kind != ECUIO_STEP_UPLOAD) {
        return "its kind isn't one this Havne runs";
    }
    if (s.session != ECUIO_STEP_SESSION_EXTENDED) {
        return "its session isn't one this Havne reads in";
    }
    if (s.address_bytes < 1 || s.address_bytes > 4) {
        return "its address_bytes is 1 to 4";
    }
    if (s.size_bytes < 1 || s.size_bytes > 4) {
        return "its size_bytes is 1 to 4";
    }
    if (s.bytes < 1 || s.bytes > ECUIO_DUMP_IMAGE_MAX) {
        return "its bytes is 1 to the image's bound";
    }
    if (s.address + s.bytes - 1 > most_of(s.address_bytes)) {
        return "its last address doesn't fit its address_bytes";
    }
    if (s.kind == ECUIO_STEP_READ) {
        if (s.chunk < 1 || s.chunk > kChunkMax) {
            return "a read's chunk is 1 to 4094";
        }
        if (s.chunk > most_of(s.size_bytes)) {
            return "its chunk doesn't fit its size_bytes";
        }
        return s.dfi == 0 ? nullptr : "a read takes no dfi";
    }
    if (s.chunk != 0) {
        return "an upload takes no chunk: the module names its block";
    }
    if (s.bytes > most_of(s.size_bytes)) {
        return "its bytes doesn't fit its size_bytes";
    }
    return s.dfi == 0 ? nullptr : "an upload's dfi is 00";
}

/* Whether two steps' ranges share an address */
inline bool overlap(const step& a, const step& b)
{
    return a.address < b.address + b.bytes && b.address < a.address + a.bytes;
}

/* The class core derives from the steps, never a manifest's word: a plan
 * of reads and uploads alone is read-class */
inline uint32_t class_of(const plan& p)
{
    for (const step& s : p.steps) {
        if (s.kind != ECUIO_STEP_READ && s.kind != ECUIO_STEP_UPLOAD) {
            return ECUIO_OPERATION_CLASS_WRITE;
        }
    }
    return ECUIO_OPERATION_CLASS_READ;
}

/* The unlock attempts a run makes: one for each session it enters, a
 * session change locking the module again. Each range is read twice
 * inside its session, so a second read costs none. */
inline uint32_t unlock_sessions(const plan& p)
{
    uint32_t n = 0;
    for (size_t i = 0; i < p.steps.size(); ++i) {
        n += i == 0 || p.steps[i].session != p.steps[i - 1].session ? 1u : 0u;
    }
    return n;
}

/* The image's bytes: the steps' added, each range once */
inline uint64_t image_bytes(const plan& p)
{
    uint64_t n = 0;
    for (const step& s : p.steps) {
        n += s.bytes;
    }
    return n;
}

/* Whether every request of the step fits one CAN frame's 7 bytes, which
 * is all an ELM327 adapter sends: 23 <ALFID> <address> <size>, and an
 * upload's 35 <dfi> <ALFID> <address> <size> */
inline bool fits_one_frame(const step& s)
{
    const unsigned head = s.kind == ECUIO_STEP_UPLOAD ? 3u : 2u;
    return head + s.address_bytes + s.size_bytes <= 7u;
}

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_PLAN_H */
