// File/layer: core/src/sim_memory.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimMemoryTest, FakeMemoryTest (test_obd2)
//
// The simulated ECU's memory (ADR-0066 section 4), from the service byte
// on: a region read by address (service 23) or uploaded block by block
// (services 35, 36 and 37) and two identification DIDs, all the caller's
// state. Everything here is made up for the simulated
// car: the region's address, its bytes (a function of the address mixed
// with the letters of "HAVNE") and the DIDs' texts. It is no module's
// memory map. sim_ecu.h's engine ECU and the fakes' one ECU
// (tests/fake_ecus.h) both answer from it. Its own faults (the URI's
// mem=) break a read on request: a short or changed reply, a byte that
// moves, an unlock lost, a wrong counter, a refusal, an identification
// that changes.

#ifndef ECUIO_SIM_MEMORY_H
#define ECUIO_SIM_MEMORY_H

#include "sim_security.h"

#include <cstdint>
#include <cstdio>
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
/* What the software DID answers once the ident fault has struck */
constexpr char ECUIO_SIM_SOFTWARE_CHANGED[] = "HAVNESIM-SW-0002";
static_assert(sizeof ECUIO_SIM_HARDWARE_TEXT == ECUIO_SIM_IDENT_BYTES + 1 &&
                  sizeof ECUIO_SIM_SOFTWARE_TEXT == ECUIO_SIM_IDENT_BYTES + 1 &&
                  sizeof ECUIO_SIM_SOFTWARE_CHANGED ==
                      ECUIO_SIM_IDENT_BYTES + 1,
              "each identification text is 16 bytes");

/* An upload's block length, as a 75 names it: a 76's bytes with its
 * service and counter, so 1,024 bytes of data */
constexpr uint16_t ECUIO_SIM_MEMORY_BLOCK = 0x0402;

/* The codes this header answers with, beside sim_security.h's */
constexpr uint8_t ECUIO_SIM_NRC_TOO_LONG = 0x14;
constexpr uint8_t ECUIO_SIM_NRC_CONDITIONS = 0x22;
constexpr uint8_t ECUIO_SIM_NRC_RANGE = 0x31;
constexpr uint8_t ECUIO_SIM_NRC_UPLOAD = 0x70;
constexpr uint8_t ECUIO_SIM_NRC_COUNTER = 0x73;

/* A fault's longest period, and the longest block length (a 76 fits
 * ECUIO_SIM_MEMORY_REPLY_MAX) */
constexpr uint32_t ECUIO_SIM_MEMORY_FAULT_MAX = 10000;
constexpr uint32_t ECUIO_SIM_MEMORY_BLOCK_MAX = 0xFFF;

/* The memory's own faults (mem=), 0 for off. A data reply is a 63 or a
 * 76 the ECU would give: each read or block that passes every check
 * counts one, whatever a fault then makes of it. */
struct ecuio_sim_memory_faults {
    bool given = false;  /* a mem= key named any of these */
    uint32_t cut = 0;    /* short: every nth data reply is one byte short */
    uint32_t flip = 0;   /* every nth data reply has one bit wrong, the
                            bit another on each pass */
    uint32_t relock = 0; /* the nth data reply is the last before a lock */
    uint32_t seq = 0;    /* every nth 76 carries the wrong counter */
    uint32_t refuse = 0; /* every nth data reply is 7F <service> 22 */
    uint32_t ident = 0;  /* from the nth data reply the software DID
                            answers another text */
    /* volatile: one byte of region A, at this offset, reads as the count
     * of its own reads, so no two reads of it agree */
    bool moving = false;
    uint32_t moving_at = 0;
    uint16_t block = ECUIO_SIM_MEMORY_BLOCK; /* what a 75 names */
};

/* One ECU's memory state */
struct ecuio_sim_memory {
    /* The VIN region A holds (17 characters, the caller's, outliving the
     * state); nullptr for a node with no memory */
    const char* vin = nullptr;
    /* An upload open (a 35 answered, no 37 yet): where its next block
     * starts, the bytes left and the counter its next 36 carries */
    bool uploading = false;
    uint32_t next = 0;
    uint32_t left = 0;
    uint8_t counter = 1;
    ecuio_sim_memory_faults faults;
    uint32_t replies = 0; /* data replies counted */
    uint32_t blocks = 0;  /* 76s counted */
    uint32_t moved = 0;   /* reads of the volatile byte */
    /* The passes over the memory: a 23 at or below the one before starts
     * another, and so does each upload */
    uint32_t pass = 0;
    uint32_t last = 0; /* the last 23's address, once read is set */
    bool read = false;
};

/* The diagnostic session changed, or lapsed: an upload ends with it */
inline void ecuio_sim_memory_end(ecuio_sim_memory& m)
{
    m.uploading = false;
    m.left = 0;
}

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

/* size bytes from address into out: the region's, the volatile byte
 * counted where the faults name one inside them */
inline void copy_out(ecuio_sim_memory& m, uint32_t address, uint32_t size,
                     uint8_t* out)
{
    for (uint32_t i = 0; i < size; ++i) {
        out[i] = ecuio_sim_memory_byte(m, address + i);
    }
    const uint32_t at = ECUIO_SIM_MEMORY_ADDRESS + m.faults.moving_at;
    if (m.faults.moving && at >= address && at - address < size) {
        out[at - address] = static_cast<uint8_t>(++m.moved);
    }
}

/* What the faults make of a data reply, count its place among them:
 * refused (nothing is read and nothing moves), else one byte short
 * (cut) or one bit wrong (flip); relock locks once the reply is made */
struct struck {
    uint32_t count = 0;
    bool refused = false;
    bool cut = false;
    bool flip = false;
    bool relock = false;
};

inline struck strike(ecuio_sim_memory& m)
{
    struck s;
    s.count = ++m.replies;
    const ecuio_sim_memory_faults& f = m.faults;
    const auto every = [&s](uint32_t n) { return n != 0 && s.count % n == 0; };
    s.relock = f.relock != 0 && s.count == f.relock;
    s.refused = every(f.refuse);
    s.cut = !s.refused && every(f.cut);
    s.flip = !s.refused && every(f.flip);
    return s;
}

/* The faults applied to a reply of head bytes and size of data in out;
 * its length. A flip's bit is the pass's, so the same bytes read on two
 * passes back to back never come out wrong alike. */
inline uint32_t broken(const struck& s, uint32_t pass,
                       ecuio_sim_security& security, uint32_t head,
                       uint32_t size, uint8_t* out)
{
    if (s.flip) {
        out[head + s.count % size] ^= static_cast<uint8_t>(1u << (pass % 8));
    }
    if (s.relock) {
        ecuio_sim_security_lock(security);
    }
    return head + size - (s.cut ? 1u : 0u);
}

/* 23 <ALFID> <address> <size> */
inline uint32_t read_bytes(ecuio_sim_memory& m, ecuio_sim_security& security,
                           const uint8_t* req, uint32_t len, uint8_t* out)
{
    if (len < 3) {
        return refuse(0x23, ECUIO_SIM_NRC_LENGTH, out);
    }
    span s = fields(req[1]);
    if (len != 2u + s.address_bytes + s.size_bytes) {
        return refuse(0x23, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (s.address_bytes <= 4 && s.size_bytes <= 4) {
        read_span(req + 2, &s);
    }
    if (!in_a_region(s)) {
        return refuse(0x23, ECUIO_SIM_NRC_RANGE, out);
    }
    if (security.unlocked == 0) {
        return refuse(0x23, ECUIO_SIM_NRC_DENIED, out);
    }
    if (s.size > ECUIO_SIM_MEMORY_REPLY_MAX - 1) {
        return refuse(0x23, ECUIO_SIM_NRC_TOO_LONG, out);
    }
    const struck hit = strike(m);
    if (hit.refused) {
        if (hit.relock) {
            ecuio_sim_security_lock(security);
        }
        return refuse(0x23, ECUIO_SIM_NRC_CONDITIONS, out);
    }
    const uint32_t size = static_cast<uint32_t>(s.size);
    const uint32_t address = static_cast<uint32_t>(s.address);
    if (m.read && address <= m.last) {
        ++m.pass;
    }
    m.read = true;
    m.last = address;
    out[0] = 0x63;
    copy_out(m, address, size, out + 1);
    return broken(hit, m.pass, security, 1, size, out);
}

/* 35 <dataFormatIdentifier> <ALFID> <address> <size> */
inline uint32_t open_upload(ecuio_sim_memory& m,
                            const ecuio_sim_security& security,
                            const uint8_t* req, uint32_t len, uint8_t* out)
{
    if (len < 4) {
        return refuse(0x35, ECUIO_SIM_NRC_LENGTH, out);
    }
    span s = fields(req[2]);
    if (len != 3u + s.address_bytes + s.size_bytes) {
        return refuse(0x35, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (s.address_bytes <= 4 && s.size_bytes <= 4) {
        read_span(req + 3, &s);
    }
    if (req[1] != 0x00 || !in_a_region(s)) {
        return refuse(0x35, ECUIO_SIM_NRC_RANGE, out);
    }
    if (security.unlocked == 0) {
        return refuse(0x35, ECUIO_SIM_NRC_DENIED, out);
    }
    if (m.uploading) {
        return refuse(0x35, ECUIO_SIM_NRC_UPLOAD, out);
    }
    m.uploading = true;
    m.next = static_cast<uint32_t>(s.address);
    m.left = static_cast<uint32_t>(s.size);
    m.counter = 1;
    ++m.pass;
    out[0] = 0x75;
    out[1] = 0x20; /* the block length that follows takes two bytes */
    out[2] = static_cast<uint8_t>(m.faults.block >> 8);
    out[3] = static_cast<uint8_t>(m.faults.block & 0xFF);
    return 4;
}

/* 36 <counter>: the upload's next block */
inline uint32_t next_block(ecuio_sim_memory& m, ecuio_sim_security& security,
                           const uint8_t* req, uint32_t len, uint8_t* out)
{
    if (len != 2) {
        return refuse(0x36, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (!m.uploading || m.left == 0 || m.faults.block < 3) {
        return refuse(0x36, ECUIO_SIM_NRC_SEQUENCE, out);
    }
    if (security.unlocked == 0) {
        return refuse(0x36, ECUIO_SIM_NRC_DENIED, out);
    }
    if (req[1] != m.counter) {
        return refuse(0x36, ECUIO_SIM_NRC_COUNTER, out);
    }
    const struck hit = strike(m);
    if (hit.refused) {
        if (hit.relock) {
            ecuio_sim_security_lock(security);
        }
        return refuse(0x36, ECUIO_SIM_NRC_CONDITIONS, out);
    }
    const uint32_t room = m.faults.block - 2u;
    const uint32_t n = m.left < room ? m.left : room;
    const uint32_t k = ++m.blocks;
    const bool wrong = m.faults.seq != 0 && k % m.faults.seq == 0;
    out[0] = 0x76;
    out[1] = static_cast<uint8_t>(m.counter + (wrong ? 1 : 0));
    copy_out(m, m.next, n, out + 2);
    m.next += n;
    m.left -= n;
    ++m.counter; /* FF wraps to 00 */
    return broken(hit, m.pass, security, 2, n, out);
}

/* 37: the upload's end */
inline uint32_t end_upload(ecuio_sim_memory& m, uint32_t len, uint8_t* out)
{
    if (len != 1) {
        return refuse(0x37, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (!m.uploading || m.left != 0) {
        return refuse(0x37, ECUIO_SIM_NRC_SEQUENCE, out);
    }
    m.uploading = false;
    out[0] = 0x77;
    return 1;
}

} // namespace ecuio_sim_memory_detail

/* Whether req's service is the memory's to answer */
inline bool ecuio_sim_memory_service(uint8_t service)
{
    return service == 0x23 || service == 0x35 || service == 0x36 ||
           service == 0x37;
}

/* The ECU's answer to a memory request (req[0] a service of
 * ecuio_sim_memory_service, len at least 1) into out
 * (ECUIO_SIM_MEMORY_REPLY_MAX bytes); the length. extended: the node is
 * in the extended session, the only one the region answers in: outside
 * it every service here is 7F (judged in the active session first, as
 * 27 is).
 *
 * 23 <ALFID> <address> <size> answers 63 <bytes>. Its refusals in the
 * order they are judged: under 3 bytes, or a length that isn't the
 * identifier's, 13; an identifier's length outside 1 to 4, a zero size,
 * or any byte outside one region, 31; locked 33; a reply past
 * ECUIO_SIM_MEMORY_REPLY_MAX 14.
 *
 * 35 00 <ALFID> <address> <size> opens an upload and answers 75 20
 * <block length>: under 4 bytes or not the identifier's length 13; a
 * data format but 00, or a range as 23 refuses, 31; locked 33; one open
 * already 70. 36 <counter> answers 76 <counter> and the next bytes, the
 * block length less 2 at most, the counter from 01 and FF wrapping to
 * 00: not two bytes 13; none open, or all handed over, 24; locked 33;
 * another counter 73. 37 ends it with 77: more than one byte 13; none
 * open, or bytes still left, 24.
 *
 * m.faults break a 63 or a 76 that passed every check above; relock
 * locks security once its reply is made. */
inline uint32_t ecuio_sim_memory_answer(ecuio_sim_memory& m,
                                        ecuio_sim_security& security,
                                        bool extended, const uint8_t* req,
                                        uint32_t len, uint8_t* out)
{
    namespace detail = ecuio_sim_memory_detail;
    if (!extended) {
        return detail::refuse(req[0], ECUIO_SIM_NRC_SESSION, out);
    }
    switch (req[0]) {
    case 0x23:
        return detail::read_bytes(m, security, req, len, out);
    case 0x35:
        return detail::open_upload(m, security, req, len, out);
    case 0x36:
        return detail::next_block(m, security, req, len, out);
    default:
        return detail::end_upload(m, len, out);
    }
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
    const bool changed = m.faults.ident != 0 && m.replies >= m.faults.ident;
    std::memcpy(out + 3,
                did == ECUIO_SIM_HARDWARE_DID ? ECUIO_SIM_HARDWARE_TEXT
                : changed                     ? ECUIO_SIM_SOFTWARE_CHANGED
                                              : ECUIO_SIM_SOFTWARE_TEXT,
                ECUIO_SIM_IDENT_BYTES);
    return 3 + ECUIO_SIM_IDENT_BYTES;
}

namespace ecuio_sim_memory_detail {

/* A number of base 10 or 16 filling [s, s + n), at most max; false for
 * none, another character or a larger one */
inline bool number(const char* s, size_t n, uint32_t base, uint32_t max,
                   uint32_t* out)
{
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        const uint32_t d = c >= '0' && c <= '9'                 ? c - '0'
                           : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10u
                           : base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10u
                                                                : 99u;
        if (d >= base || v > max) {
            return false;
        }
        v = v * base + d;
    }
    *out = v;
    return n > 0 && v <= max;
}

/* One item, <fault>:<n>, into *f; false for any other shape */
inline bool parse_item(const char* s, size_t n, ecuio_sim_memory_faults* f)
{
    size_t colon = 0;
    while (colon < n && s[colon] != ':') {
        ++colon;
    }
    if (colon == n) {
        return false;
    }
    const auto is = [s, colon](const char* name) {
        return std::strlen(name) == colon && std::strncmp(s, name, colon) == 0;
    };
    const char* v = s + colon + 1;
    const size_t vn = n - colon - 1;
    uint32_t value = 0;
    if (is("volatile")) {
        if (!number(v, vn, 16, ECUIO_SIM_MEMORY_BYTES - 1, &value)) {
            return false;
        }
        f->moving = true;
        f->moving_at = value;
        return true;
    }
    if (is("block")) {
        if (!number(v, vn, 16, ECUIO_SIM_MEMORY_BLOCK_MAX, &value) ||
            value == 0) {
            return false;
        }
        f->block = static_cast<uint16_t>(value);
        return true;
    }
    uint32_t* slot = is("short")    ? &f->cut
                     : is("flip")   ? &f->flip
                     : is("relock") ? &f->relock
                     : is("seq")    ? &f->seq
                     : is("refuse") ? &f->refuse
                     : is("ident")  ? &f->ident
                                    : nullptr;
    if (slot == nullptr ||
        !number(v, vn, 10, ECUIO_SIM_MEMORY_FAULT_MAX, &value) || value == 0) {
        return false;
    }
    *slot = value;
    return true;
}

} // namespace ecuio_sim_memory_detail

/* mem=<fault>:<n>[,...] into *out, adding to what it holds, so a second
 * mem= key adds to the first (a later item in place of an earlier);
 * false with why (a line) */
inline bool ecuio_sim_parse_memory(const char* text, size_t len,
                                   ecuio_sim_memory_faults* out, char* why,
                                   size_t why_cap)
{
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        if (!ecuio_sim_memory_detail::parse_item(text + at, end - at, out)) {
            std::snprintf(why, why_cap,
                          "mem: '%.*s' isn't <fault>:<n>: short, flip, relock, "
                          "seq, refuse or ident with n from 1 to %u, volatile "
                          "with a hex offset into the region (0 to %X) or "
                          "block with a hex length from 1 to %X",
                          static_cast<int>(end - at < 40 ? end - at : 40),
                          text + at, ECUIO_SIM_MEMORY_FAULT_MAX,
                          ECUIO_SIM_MEMORY_BYTES - 1,
                          ECUIO_SIM_MEMORY_BLOCK_MAX);
            return false;
        }
        at = end + 1;
    }
    if (len == 0 || text[len - 1] == ',') {
        std::snprintf(why, why_cap, "mem: names none");
        return false;
    }
    out->given = true;
    return true;
}

#endif /* ECUIO_SIM_MEMORY_H */
