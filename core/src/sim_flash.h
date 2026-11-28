#ifndef ECUIO_SIM_FLASH_H
#define ECUIO_SIM_FLASH_H

#include "sim_memory.h"
#include "sim_security.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr uint32_t ECUIO_SIM_FLASH_SECTOR_BYTES = 0x00001000u;
constexpr uint32_t ECUIO_SIM_FLASH_SECTORS =
    ECUIO_SIM_MEMORY_CODE_BYTES / ECUIO_SIM_FLASH_SECTOR_BYTES;
constexpr uint16_t ECUIO_SIM_FLASH_ERASE = 0xFF00;
constexpr uint16_t ECUIO_SIM_FLASH_CHECK = 0xFF01;
constexpr uint16_t ECUIO_SIM_FLASH_BLOCK = 0x0402;
constexpr uint8_t ECUIO_SIM_FLASH_PASSED = 0x00;
constexpr uint8_t ECUIO_SIM_FLASH_FAILED = 0x01;
constexpr uint8_t ECUIO_SIM_NRC_GENERAL_PROGRAMMING = 0x72;
constexpr uint8_t ECUIO_SIM_NRC_VOLTAGE_LOW = 0x93;
constexpr uint32_t ECUIO_SIM_FLASH_SILENT_AFTER = 2;
constexpr uint8_t ECUIO_SIM_NRC_NOT_SUPPORTED = 0x11;
constexpr uint32_t ECUIO_SIM_FLASH_PENDING_MOST_MS = 600000;

static_assert(ECUIO_SIM_MEMORY_CODE_BYTES % ECUIO_SIM_FLASH_SECTOR_BYTES == 0,
              "the code region is whole sectors");

struct ecuio_sim_flash_faults {
    bool counter = false; /* the first 36 answers 7F 36 73, once */
    bool general = false; /* the second 36 answers 7F 36 72 */
    bool volts = false;   /* the first 36 answers 7F 36 93, once */
    bool check = false;   /* the check answers its failed status */
    bool erase = false;   /* an erase answers 7F 31 22 */
    bool silent = false;  /* nothing answers after the second 36 */
    bool boot = false;    /* starts part-written, sector boot_erased erased */
    bool locked = false;  /* a seed asks answer 7F 27 36 */
    uint32_t boot_erased = 0; /* ECUIO_SIM_FLASH_SECTORS: none */
    uint32_t pending_ms = 0;  /* each request answers 78 alone this long */
};

enum class ecuio_sim_sector : uint8_t { kept, erased, written };

struct ecuio_sim_flash {
    bool on = false;
    ecuio_sim_flash_faults faults;
    ecuio_sim_sector sectors[ECUIO_SIM_FLASH_SECTORS] = {};
    bool downloading = false;
    uint32_t next = 0;
    uint32_t left = 0;
    uint32_t first_sector = 0;
    uint32_t last_sector = 0;
    uint8_t counter = 1;
    uint32_t blocks = 0;
    bool struck_counter = false;
    bool struck_volts = false;
    bool dead = false;
    bool boot = false; /* the application doesn't run: the bootloader answers */
    bool valid = false; /* a passing check since the last erase */
    bool heard = false;
    int64_t pending_until_us = 0;
    uint32_t erases = 0;
    uint32_t downloads = 0;
    uint32_t checks = 0;
};

/* The code region made writable, filled as it reads; false when out of
 * memory */
inline bool ecuio_sim_flash_arm(ecuio_sim_flash& f, ecuio_sim_memory& m,
                                const ecuio_sim_flash_faults& faults)
{
    f = ecuio_sim_flash{};
    f.on = true;
    f.faults = faults;
    try {
        m.code.resize(ECUIO_SIM_MEMORY_CODE_BYTES);
    } catch (...) {
        return false;
    }
    for (uint32_t i = 0; i < ECUIO_SIM_MEMORY_CODE_BYTES; ++i) {
        m.code[i] = ecuio_sim_memory_fill(ECUIO_SIM_MEMORY_CODE_ADDRESS + i);
    }
    f.boot = faults.boot;
    const uint32_t i = faults.boot_erased;
    if (faults.boot && i < ECUIO_SIM_FLASH_SECTORS) {
        f.sectors[i] = ecuio_sim_sector::erased;
        std::memset(m.code.data() + size_t{i} * ECUIO_SIM_FLASH_SECTOR_BYTES,
                    0xFF, ECUIO_SIM_FLASH_SECTOR_BYTES);
    }
    return true;
}

inline void ecuio_sim_flash_end(ecuio_sim_flash& f)
{
    f.downloading = false;
    f.left = 0;
}

/* The application runs again only after a passing check */
inline void ecuio_sim_flash_restart(ecuio_sim_flash& f)
{
    f.boot = f.boot && !f.valid;
}

inline bool ecuio_sim_flash_withholds(const ecuio_sim_flash& f,
                                      const uint8_t* req, uint32_t len)
{
    if (!f.on || !f.boot || len == 0) {
        return false;
    }
    switch (req[0]) {
    case 0x10:
    case 0x11:
    case 0x27:
    case 0x31:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
    case 0x3E:
        return false;
    default:
        return true;
    }
}

/* pending:<ms>, from the first request heard: true while it answers 78 */
inline bool ecuio_sim_flash_pending(ecuio_sim_flash& f, int64_t now_us)
{
    if (!f.on || f.faults.pending_ms == 0) {
        return false;
    }
    if (!f.heard) {
        f.heard = true;
        f.pending_until_us = now_us + int64_t{f.faults.pending_ms} * 1000;
    }
    return now_us < f.pending_until_us;
}

namespace ecuio_sim_flash_detail {

using ecuio_sim_memory_detail::refuse;
using ecuio_sim_memory_detail::span;

/* The span after an identifier at p, its whole sectors in the code
 * region; false for any other */
inline bool sectors_of(const uint8_t* p, uint32_t len, span* s, uint32_t* first,
                       uint32_t* last)
{
    *s = ecuio_sim_memory_detail::fields(p[0]);
    if (s->address_bytes < 1 || s->address_bytes > 4 || s->size_bytes < 1 ||
        s->size_bytes > 4 || len != 1u + s->address_bytes + s->size_bytes) {
        return false;
    }
    ecuio_sim_memory_detail::read_span(p + 1, s);
    const bool inside = s->size != 0 && ecuio_sim_memory_detail::in_region(
                                            *s, {ECUIO_SIM_MEMORY_CODE_ADDRESS,
                                                 ECUIO_SIM_MEMORY_CODE_BYTES});
    const uint64_t at = s->address - ECUIO_SIM_MEMORY_CODE_ADDRESS;
    if (!inside || at % ECUIO_SIM_FLASH_SECTOR_BYTES != 0 ||
        s->size % ECUIO_SIM_FLASH_SECTOR_BYTES != 0) {
        return false;
    }
    *first = static_cast<uint32_t>(at / ECUIO_SIM_FLASH_SECTOR_BYTES);
    *last = *first +
            static_cast<uint32_t>(s->size / ECUIO_SIM_FLASH_SECTOR_BYTES) - 1;
    return true;
}

inline uint32_t erase(ecuio_sim_flash& f, ecuio_sim_memory& m,
                      const uint8_t* req, uint32_t len, uint8_t* out)
{
    span s;
    uint32_t first = 0;
    uint32_t last = 0;
    if (len < 5 || !sectors_of(req + 4, len - 4, &s, &first, &last)) {
        return refuse(0x31, ECUIO_SIM_NRC_RANGE, out);
    }
    if (f.faults.erase || f.downloading) {
        return refuse(0x31, ECUIO_SIM_NRC_CONDITIONS, out);
    }
    ++f.erases;
    f.boot = true;
    f.valid = false;
    for (uint32_t i = first; i <= last; ++i) {
        f.sectors[i] = ecuio_sim_sector::erased;
        std::memset(m.code.data() + size_t{i} * ECUIO_SIM_FLASH_SECTOR_BYTES,
                    0xFF, ECUIO_SIM_FLASH_SECTOR_BYTES);
    }
    std::memcpy(out, req, 4);
    out[0] = 0x71;
    return 4;
}

inline uint32_t check(ecuio_sim_flash& f, const uint8_t* req, uint32_t len,
                      uint8_t* out)
{
    if (len != 4) {
        return refuse(0x31, ECUIO_SIM_NRC_LENGTH, out);
    }
    ++f.checks;
    bool whole = !f.downloading;
    for (const ecuio_sim_sector s : f.sectors) {
        whole = whole && s != ecuio_sim_sector::erased;
    }
    std::memcpy(out, req, 4);
    out[0] = 0x71;
    f.valid = whole && !f.faults.check;
    out[4] = f.valid ? ECUIO_SIM_FLASH_PASSED : ECUIO_SIM_FLASH_FAILED;
    return 5;
}

inline uint32_t open(ecuio_sim_flash& f, const uint8_t* req, uint32_t len,
                     uint8_t* out)
{
    span s;
    uint32_t first = 0;
    uint32_t last = 0;
    if (len < 4 || req[1] != 0x00 ||
        !sectors_of(req + 2, len - 2, &s, &first, &last)) {
        return refuse(0x34, ECUIO_SIM_NRC_RANGE, out);
    }
    if (f.downloading) {
        return refuse(0x34, ECUIO_SIM_NRC_CONDITIONS, out);
    }
    for (uint32_t i = first; i <= last; ++i) {
        if (f.sectors[i] != ecuio_sim_sector::erased) {
            return refuse(0x34, ECUIO_SIM_NRC_UPLOAD, out);
        }
    }
    f.downloading = true;
    f.next = static_cast<uint32_t>(s.address);
    f.left = static_cast<uint32_t>(s.size);
    f.first_sector = first;
    f.last_sector = last;
    f.counter = 1;
    ++f.downloads;
    out[0] = 0x74;
    out[1] = 0x20;
    out[2] = static_cast<uint8_t>(ECUIO_SIM_FLASH_BLOCK >> 8);
    out[3] = static_cast<uint8_t>(ECUIO_SIM_FLASH_BLOCK & 0xFF);
    return 4;
}

inline uint32_t block(ecuio_sim_flash& f, ecuio_sim_memory& m,
                      const uint8_t* req, uint32_t len, uint8_t* out)
{
    const uint32_t n = len > 2 ? len - 2 : 0;
    if (len < 3 || len > ECUIO_SIM_FLASH_BLOCK || n > f.left) {
        return refuse(0x36, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (f.faults.volts && !f.struck_volts) {
        f.struck_volts = true;
        return refuse(0x36, ECUIO_SIM_NRC_VOLTAGE_LOW, out);
    }
    if (req[1] != f.counter ||
        (f.faults.counter && !f.struck_counter && f.blocks == 0)) {
        f.struck_counter = f.struck_counter || f.faults.counter;
        return refuse(0x36, ECUIO_SIM_NRC_COUNTER, out);
    }
    if (f.faults.general && f.blocks == 1) {
        return refuse(0x36, ECUIO_SIM_NRC_GENERAL_PROGRAMMING, out);
    }
    std::memcpy(m.code.data() + (f.next - ECUIO_SIM_MEMORY_CODE_ADDRESS),
                req + 2, n);
    f.next += n;
    f.left -= n;
    ++f.blocks;
    f.dead = f.faults.silent && f.blocks >= ECUIO_SIM_FLASH_SILENT_AFTER;
    out[0] = 0x76;
    out[1] = f.counter++;
    return 2;
}

inline uint32_t finish(ecuio_sim_flash& f, uint32_t len, uint8_t* out)
{
    if (len != 1) {
        return refuse(0x37, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (f.left != 0) {
        return refuse(0x37, ECUIO_SIM_NRC_SEQUENCE, out);
    }
    f.downloading = false;
    for (uint32_t i = f.first_sector; i <= f.last_sector; ++i) {
        f.sectors[i] = ecuio_sim_sector::written;
    }
    out[0] = 0x77;
    return 1;
}

inline uint16_t routine_of(const uint8_t* req, uint32_t len)
{
    return len >= 4 && req[1] == 0x01
               ? static_cast<uint16_t>((uint32_t{req[2]} << 8) | req[3])
               : 0;
}

} // namespace ecuio_sim_flash_detail

/* Whether req is the flash's: its two routines, a 34 into the code region,
 * or a 36 or 37 while it downloads */
inline bool ecuio_sim_flash_service(const ecuio_sim_flash& f,
                                    const uint8_t* req, uint32_t len)
{
    namespace detail = ecuio_sim_flash_detail;
    if (!f.on || len == 0) {
        return false;
    }
    if (f.dead) {
        return true;
    }
    if (req[0] == 0x31) {
        const uint16_t r = detail::routine_of(req, len);
        return r == ECUIO_SIM_FLASH_ERASE || r == ECUIO_SIM_FLASH_CHECK;
    }
    if (req[0] == 0x34) {
        ecuio_sim_memory_detail::span s;
        uint32_t first = 0;
        uint32_t last = 0;
        return len >= 4 &&
               detail::sectors_of(req + 2, len - 2, &s, &first, &last);
    }
    return f.downloading && (req[0] == 0x36 || req[0] == 0x37);
}

/* 0: no answer */
inline uint32_t ecuio_sim_flash_answer(ecuio_sim_flash& f, ecuio_sim_memory& m,
                                       const ecuio_sim_security& security,
                                       bool programming, const uint8_t* req,
                                       uint32_t len, uint8_t* out)
{
    namespace detail = ecuio_sim_flash_detail;
    if (f.dead) {
        return 0;
    }
    if (!programming) {
        return detail::refuse(req[0], ECUIO_SIM_NRC_SESSION, out);
    }
    if (security.unlocked == 0) {
        return detail::refuse(req[0], ECUIO_SIM_NRC_DENIED, out);
    }
    switch (req[0]) {
    case 0x31:
        return detail::routine_of(req, len) == ECUIO_SIM_FLASH_ERASE
                   ? detail::erase(f, m, req, len, out)
                   : detail::check(f, req, len, out);
    case 0x34:
        return detail::open(f, req, len, out);
    case 0x36:
        return detail::block(f, m, req, len, out);
    default:
        return detail::finish(f, len, out);
    }
}

namespace ecuio_sim_flash_detail {

inline bool number_after(const char* text, size_t n, const char* name,
                         uint32_t least, uint32_t most, uint32_t* out)
{
    const size_t k = std::strlen(name);
    if (n <= k + 1 || std::memcmp(text, name, k) != 0 || text[k] != ':') {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = k + 1; i < n; ++i) {
        if (text[i] < '0' || text[i] > '9' || v > most) {
            return false;
        }
        v = v * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    if (v < least || v > most) {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

inline bool word(const char* text, size_t n, ecuio_sim_flash_faults* out)
{
    const auto is = [&](const char* w) {
        return std::strlen(w) == n && std::memcmp(text, w, n) == 0;
    };
    bool* const words[] = {&out->counter, &out->general, &out->volts,
                           &out->check,   &out->erase,   &out->silent,
                           &out->boot,    &out->locked};
    const char* const names[] = {"counter", "general", "volts", "check",
                                 "erase",   "silent",  "boot",  "locked"};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i) {
        if (is(names[i])) {
            *words[i] = true;
            return true;
        }
    }
    if (number_after(text, n, "boot", 0, ECUIO_SIM_FLASH_SECTORS,
                     &out->boot_erased)) {
        out->boot = true;
        return true;
    }
    return is("1") ||
           number_after(text, n, "pending", 1, ECUIO_SIM_FLASH_PENDING_MOST_MS,
                        &out->pending_ms);
}

} // namespace ecuio_sim_flash_detail

/* flash=: "1", or counter, general, volts, check, erase, silent,
 * boot[:<n>], locked and pending:<ms>, comma-separated */
inline bool ecuio_sim_parse_flash(const char* text, size_t len, bool* on,
                                  ecuio_sim_flash_faults* out)
{
    size_t at = 0;
    while (at <= len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        if (!ecuio_sim_flash_detail::word(text + at, end - at, out)) {
            return false;
        }
        at = end + 1;
    }
    *on = true;
    return true;
}

#endif /* ECUIO_SIM_FLASH_H */
