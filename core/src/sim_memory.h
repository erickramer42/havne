// File/layer: core/src/sim_memory.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimMemoryTest, FakeMemoryTest (test_obd2)
//
// The simulated ECU's memory (ADR-0066 section 4), from the service byte
// on: a region read by address (service 23) and two identification DIDs,
// all the caller's state. Everything here is made up for the simulated
// car: the region's address, its bytes (a function of the address mixed
// with the letters of "HAVNE") and the DIDs' texts. It is no module's
// memory map. sim_ecu.h's engine ECU and the fakes' one ECU
// (tests/fake_ecus.h) both answer from it.

#ifndef ECUIO_SIM_MEMORY_H
#define ECUIO_SIM_MEMORY_H

#include "sim_security.h"

#include <cstdint>
#include <cstring>

/* A region: its first address and its length */
struct ecuio_sim_memory_region {
    uint32_t address;
    uint32_t bytes;
};

/* Region A, "calibration": 64 KiB, read in the extended session once a
 * level is unlocked */
constexpr uint32_t ECUIO_SIM_MEMORY_ADDRESS = 0x00010000u;
constexpr uint32_t ECUIO_SIM_MEMORY_BYTES = 0x00010000u;
constexpr ecuio_sim_memory_region ECUIO_SIM_MEMORY_REGIONS[] = {
    {ECUIO_SIM_MEMORY_ADDRESS, ECUIO_SIM_MEMORY_BYTES},
};

/* Where region A holds the node's VIN, 17 bytes: an image can carry one
 * that no scrub of a recording reaches */
constexpr uint32_t ECUIO_SIM_MEMORY_VIN_ADDRESS = 0x00018000u;
constexpr uint32_t ECUIO_SIM_MEMORY_VIN_BYTES = 17;

/* The longest reply: ISO-TP's longest message, so a read of 4094 bytes */
constexpr uint32_t ECUIO_SIM_MEMORY_REPLY_MAX = 4095;

/* The identification DIDs, readable in the extended session with no
 * unlock: the hardware's and the software's, 16 bytes of made-up text */
constexpr uint16_t ECUIO_SIM_HARDWARE_DID = 0xD220;
constexpr uint16_t ECUIO_SIM_SOFTWARE_DID = 0xD221;
constexpr uint32_t ECUIO_SIM_IDENT_BYTES = 16;
constexpr char ECUIO_SIM_HARDWARE_TEXT[] = "HAVNESIM-HW-0001";
constexpr char ECUIO_SIM_SOFTWARE_TEXT[] = "HAVNESIM-SW-0001";
static_assert(sizeof ECUIO_SIM_HARDWARE_TEXT == ECUIO_SIM_IDENT_BYTES + 1 &&
                  sizeof ECUIO_SIM_SOFTWARE_TEXT == ECUIO_SIM_IDENT_BYTES + 1,
              "each identification text is 16 bytes");

/* The codes this header answers with, beside sim_security.h's */
constexpr uint8_t ECUIO_SIM_NRC_TOO_LONG = 0x14;
constexpr uint8_t ECUIO_SIM_NRC_RANGE = 0x31;

/* One ECU's memory state */
struct ecuio_sim_memory {
    /* The VIN region A holds (17 characters, the caller's, outliving the
     * state); nullptr for a node with no memory */
    const char* vin = nullptr;
};

/* The byte a region holds at address where nothing else is written: a
 * fixed mix of the address with a letter of "HAVNE" */
inline uint8_t ecuio_sim_memory_fill(uint32_t address)
{
    constexpr char word[] = "HAVNE";
    const uint32_t x = address * 0x9E3779B1u;
    return static_cast<uint8_t>((x >> 24) ^ (x >> 11) ^
                                static_cast<uint8_t>(word[address % 5]));
}

/* The byte m's memory holds at address (inside a region): the VIN at its
 * place, the fill elsewhere */
inline uint8_t ecuio_sim_memory_byte(const ecuio_sim_memory& m,
                                     uint32_t address)
{
    if (m.vin != nullptr && address >= ECUIO_SIM_MEMORY_VIN_ADDRESS &&
        address - ECUIO_SIM_MEMORY_VIN_ADDRESS < ECUIO_SIM_MEMORY_VIN_BYTES) {
        return static_cast<uint8_t>(
            m.vin[address - ECUIO_SIM_MEMORY_VIN_ADDRESS]);
    }
    return ecuio_sim_memory_fill(address);
}

namespace ecuio_sim_memory_detail {

inline uint32_t refuse(uint8_t service, uint8_t nrc, uint8_t* out)
{
    out[0] = 0x7F;
    out[1] = service;
    out[2] = nrc;
    return 3;
}

/* An addressAndLengthFormatIdentifier's fields read from p: the size's
 * length is the high nibble, the address's the low */
struct span {
    uint8_t address_bytes = 0;
    uint8_t size_bytes = 0;
    uint64_t address = 0;
    uint64_t size = 0;
};

inline span fields(uint8_t alfid)
{
    span s;
    s.size_bytes = static_cast<uint8_t>(alfid >> 4);
    s.address_bytes = static_cast<uint8_t>(alfid & 0x0F);
    return s;
}

/* The big-endian address and size after the identifier at p */
inline void read_span(const uint8_t* p, span* s)
{
    for (uint8_t i = 0; i < s->address_bytes; ++i) {
        s->address = (s->address << 8) | p[i];
    }
    for (uint8_t i = 0; i < s->size_bytes; ++i) {
        s->size = (s->size << 8) | p[s->address_bytes + i];
    }
}

/* Whether the identifier's lengths are ones the ECU takes (1 to 4 bytes
 * each) and every byte asked for lies in one region */
inline bool in_a_region(const span& s)
{
    if (s.address_bytes < 1 || s.address_bytes > 4 || s.size_bytes < 1 ||
        s.size_bytes > 4 || s.size == 0) {
        return false;
    }
    for (const ecuio_sim_memory_region& r : ECUIO_SIM_MEMORY_REGIONS) {
        if (s.address >= r.address && s.size <= r.bytes &&
            s.address - r.address <= r.bytes - s.size) {
            return true;
        }
    }
    return false;
}

} // namespace ecuio_sim_memory_detail

/* Whether req's service is the memory's to answer */
inline bool ecuio_sim_memory_service(uint8_t service)
{
    return service == 0x23;
}

/* The ECU's answer to a memory request (req[0] a service of
 * ecuio_sim_memory_service, len at least 1) into out
 * (ECUIO_SIM_MEMORY_REPLY_MAX bytes); the length. extended: the node is
 * in the extended session, the only one the region answers in. A 23 is
 * 23 <ALFID> <address> <size>, answered 63 <bytes>. The refusals in the
 * order they are judged: outside the session 7F (the service is judged
 * in the active session first, as 27 is); under 3 bytes, or a length
 * that isn't the identifier's, 13; an identifier's length outside 1 to
 * 4, a zero size, or any byte outside one region, 31; locked 33; a
 * reply past ECUIO_SIM_MEMORY_REPLY_MAX 14. */
inline uint32_t ecuio_sim_memory_answer(const ecuio_sim_memory& m,
                                        const ecuio_sim_security& security,
                                        bool extended, const uint8_t* req,
                                        uint32_t len, uint8_t* out)
{
    using namespace ecuio_sim_memory_detail;
    const uint8_t service = req[0];
    if (!extended) {
        return refuse(service, ECUIO_SIM_NRC_SESSION, out);
    }
    if (len < 3) {
        return refuse(service, ECUIO_SIM_NRC_LENGTH, out);
    }
    span s = fields(req[1]);
    if (len != 2u + s.address_bytes + s.size_bytes) {
        return refuse(service, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (s.address_bytes <= 4 && s.size_bytes <= 4) {
        read_span(req + 2, &s);
    }
    if (!in_a_region(s)) {
        return refuse(service, ECUIO_SIM_NRC_RANGE, out);
    }
    if (security.unlocked == 0) {
        return refuse(service, ECUIO_SIM_NRC_DENIED, out);
    }
    if (s.size > ECUIO_SIM_MEMORY_REPLY_MAX - 1) {
        return refuse(service, ECUIO_SIM_NRC_TOO_LONG, out);
    }
    const uint32_t address = static_cast<uint32_t>(s.address);
    const uint32_t size = static_cast<uint32_t>(s.size);
    out[0] = 0x63;
    for (uint32_t i = 0; i < size; ++i) {
        out[1 + i] = ecuio_sim_memory_byte(m, address + i);
    }
    return 1 + size;
}

/* An identification DID's answer to 22 <did> into out, its length; 0
 * when did is neither or the node is outside the extended session,
 * where the caller answers as for any DID it lacks */
inline uint32_t ecuio_sim_memory_did(const ecuio_sim_memory& m, bool extended,
                                     uint16_t did, uint8_t* out)
{
    if (m.vin == nullptr || !extended ||
        (did != ECUIO_SIM_HARDWARE_DID && did != ECUIO_SIM_SOFTWARE_DID)) {
        return 0;
    }
    out[0] = 0x62;
    out[1] = static_cast<uint8_t>(did >> 8);
    out[2] = static_cast<uint8_t>(did & 0xFF);
    std::memcpy(out + 3,
                did == ECUIO_SIM_HARDWARE_DID ? ECUIO_SIM_HARDWARE_TEXT
                                              : ECUIO_SIM_SOFTWARE_TEXT,
                ECUIO_SIM_IDENT_BYTES);
    return 3 + ECUIO_SIM_IDENT_BYTES;
}

#endif /* ECUIO_SIM_MEMORY_H */
