// File/layer: tests/fake_ecus.h | test-only, C++17, Qt-free, no platform
//             headers; header-only
// Testing: UNIT: FakeEcuTest (test_obd2)
//
// The fakes' ECUs by reply ID, over the engine ECU of fake_ecu.h: a
// module of the fake's own that answers Service 0x22 from its rows, an
// ECU that isn't on the car, each ECU's diagnostic session (ADR-0060)
// and security access (ADR-0066 section 2.7, core/src/sim_security.h,
// the simulated ECU's own), its memory (core/src/sim_memory.h, the
// simulated ECU's too), and answer_for(), the one answer the fake
// J2534 DLL, the fake ELM327 and the simulated bus's ECU all give.

#ifndef ECUIO_TESTS_FAKE_ECUS_H
#define ECUIO_TESTS_FAKE_ECUS_H

#include "fake_ecu.h"
#include "sim_kernel.h"   /* core/src: the simulated ECU's, shared */
#include "sim_memory.h"   /* core/src: the simulated ECU's, shared */
#include "sim_security.h" /* core/src: the simulated ECU's, shared */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace fake_ecu {

/* ---- Modules of the fake's own: a second ECU that answers otherwise ----
 * By default every ECU answers as the engine ECU above, so a session that
 * asks two of them gets the same replies from both. A module is an ECU
 * the table below names by its reply ID: it answers Service 0x22 from
 * its own rows alone (7F 22 31 for any other DID) and refuses every
 * other service (7F <service> 11, serviceNotSupported), so a session
 * asking the engine ECU and a module tells their replies apart, as one
 * reading a user's signal on a second module must. One table per
 * process, empty by default: the fake DLL fills it from
 * ECUIO_FAKE_MODULE_DIDS on each write, the in-process fakes' tests with
 * set_module_dids() before a session starts (a reader may run on a
 * session's worker, so it isn't changed while one runs). */
constexpr uint32_t kModuleRowsMax = 64;
constexpr uint32_t kModuleDataMax = 16;

struct ModuleRow {
    uint32_t rx;
    uint16_t did;
    uint8_t len;
    uint8_t data[kModuleDataMax];
};

struct ModuleTable {
    std::atomic<uint32_t> count{0};
    ModuleRow rows[kModuleRowsMax] = {};
};

inline ModuleTable& module_table()
{
    static ModuleTable table;
    return table;
}

/* An ECU that isn't on this car: the reply ID nothing answers on, 0 for
 * none, kEveryEcuAbsent for a car that answers nothing at all (its
 * ignition off). One per process: the fake DLL sets it from
 * ECUIO_FAKE_ABSENT_ECU on each write, the in-process fakes' tests
 * before a session starts. */
constexpr uint32_t kEveryEcuAbsent = UINT32_MAX;
inline std::atomic<uint32_t>& absent_ecu()
{
    static std::atomic<uint32_t> rx{0};
    return rx;
}

inline int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* The table from a spec of rows "<reply ID>:<DID>=<bytes>", all hex, the
 * bytes spaced or not, rows apart by ';': "7E9:D801=00 12;7E9:D802=01".
 * It replaces the table; NULL or "" empties it. False, and the table
 * empty, for a malformed row, more than kModuleRowsMax rows or a row of
 * no bytes or more than kModuleDataMax. */
inline bool set_module_dids(const char* spec)
{
    ModuleTable& t = module_table();
    t.count.store(0, std::memory_order_release);
    if (spec == nullptr) {
        return true;
    }
    uint32_t n = 0;
    const char* s = spec;
    for (;;) {
        while (*s == ';' || *s == ' ') {
            ++s;
        }
        if (*s == '\0') {
            break;
        }
        if (n == kModuleRowsMax) {
            return false;
        }
        ModuleRow r{};
        int digits = 0;
        for (; hex_digit(*s) >= 0; ++s) {
            if (++digits > 8)
                return false;
            r.rx = (r.rx << 4) | static_cast<uint32_t>(hex_digit(*s));
        }
        if (digits == 0 || *s != ':')
            return false;
        ++s;
        uint32_t did = 0;
        digits = 0;
        for (; hex_digit(*s) >= 0; ++s) {
            if (++digits > 4)
                return false;
            did = (did << 4) | static_cast<uint32_t>(hex_digit(*s));
        }
        if (digits == 0 || *s != '=')
            return false;
        ++s;
        r.did = static_cast<uint16_t>(did);
        for (;;) {
            while (*s == ' ') {
                ++s;
            }
            if (*s == '\0' || *s == ';') {
                break;
            }
            const int hi = hex_digit(s[0]);
            const int lo =
                hex_digit(s[1]); /* s[0] isn't NUL, so s[1] is in bounds */
            if (hi < 0 || lo < 0 || r.len == kModuleDataMax)
                return false;
            r.data[r.len++] = static_cast<uint8_t>((hi << 4) | lo);
            s += 2;
        }
        if (r.len == 0)
            return false;
        t.rows[n++] = r;
    }
    t.count.store(n, std::memory_order_release);
    return true;
}

/* ---- Diagnostic sessions (ADR-0060) ----
 * The engine ECU (any reply ID not in the module table) has an extended
 * session: 10 03 enters it, 10 01 leaves it, and kExtDid answers only in
 * it. It has a programming session too, as the simulated ECU's: 10 02
 * from any session, left by 10 01 or by 11 01 (51 01), where its region B
 * reads. A module has none: 10 03 gets 7F 10 session_nrc() (0x12 by
 * default). There is no S3 timer here: the fakes keep no clock of the
 * session's, and the simulated ECU covers the lapse. The state is a
 * small table of the reply IDs in the extended session, allocation-free
 * for the ELM327 fake's worker. */
constexpr uint16_t kExtDid = 0xD200;
constexpr unsigned char kExtDidValue[2] = {0x1A, 0x2C}; /* made up */
constexpr uint32_t kSessionSlots = 8;

/* The code a module's 7F 10 carries: 0 is the default, 0x12; the fake
 * DLL sets it from ECUIO_FAKE_SESSION_NRC on each write */
inline std::atomic<unsigned>& session_nrc()
{
    static std::atomic<unsigned> code{0};
    return code;
}

inline std::atomic<uint32_t> (&extended_ids()) [kSessionSlots] {
    static std::atomic<uint32_t> ids[kSessionSlots] = {};
    return ids;
}

inline std::atomic<uint32_t> (&programming_ids()) [kSessionSlots] {
    static std::atomic<uint32_t> ids[kSessionSlots] = {};
    return ids;
}

inline bool in_table(std::atomic<uint32_t> (&ids)[kSessionSlots], uint32_t rx)
{
    for (auto& s : ids) {
        if (s.load() == rx) {
            return true;
        }
    }
    return false;
}

inline void set_in_table(std::atomic<uint32_t> (&ids)[kSessionSlots],
                         uint32_t rx, bool on)
{
    if (on == in_table(ids, rx)) {
        return;
    }
    for (auto& s : ids) {
        uint32_t want = on ? 0 : rx;
        if (s.compare_exchange_strong(want, on ? rx : 0)) {
            return;
        }
    }
}

inline bool in_extended(uint32_t rx)
{
    return in_table(extended_ids(), rx);
}

inline bool in_programming(uint32_t rx)
{
    return in_table(programming_ids(), rx);
}

/* Whether rx is in a session security access and memory answer in */
inline bool in_session(uint32_t rx)
{
    return in_extended(rx) || in_programming(rx);
}

/* rx into session 01, 02 or 03 */
inline void set_session(uint32_t rx, unsigned char session)
{
    set_in_table(extended_ids(), rx, session == 0x03);
    set_in_table(programming_ids(), rx, session == 0x02);
}

/* Every ECU back in the default session */
inline void reset_sessions()
{
    for (auto& s : extended_ids()) {
        s.store(0);
    }
    for (auto& s : programming_ids()) {
        s.store(0);
    }
}

/* ---- Security access (ADR-0066 section 2.7) ----
 * The engine ECU answers 27 as the simulated ECU does (sim_security.h):
 * in its extended session, a state for each reply ID, locked again by
 * every 10 01 and 10 03 it answers. kGatedDid answers only once a level
 * is unlocked (7F 22 33 before it, in the session). The lockout's delay
 * runs on the steady clock, since the fakes keep none of their own.
 * Its memory is the simulated ECU's as well (sim_memory.h): the region
 * read by 23 or uploaded (35, 36, 37) once unlocked, holding kVin, and
 * the two identification DIDs in the session. */
constexpr uint16_t kGatedDid = ECUIO_SIM_GATED_DID;

/* The lockout's delay in milliseconds, a knob: 0 is the simulated ECU's
 * 10 s. An in-process fake's test sets it and puts it back. */
inline std::atomic<unsigned>& security_delay_ms()
{
    static std::atomic<unsigned> ms{0};
    return ms;
}

/* Strict security access, a knob (ecuio_sim_security::strict): any
 * request but a 27 between a seed and its key spends the seed. The fake
 * DLL sets it from ECUIO_FAKE_SECURITY_STRICT on each write, the
 * in-process fakes' tests with this, and put it back. */
inline std::atomic<bool>& security_strict()
{
    static std::atomic<bool> strict{false};
    return strict;
}

struct SecuritySlot {
    uint32_t rx = 0;
    ecuio_sim_security state;
    ecuio_sim_memory memory;
    ecuio_sim_kernel kernel;
};

struct SecurityTable {
    std::mutex m;
    SecuritySlot slots[kSessionSlots];
    ecuio_sim_memory_faults memory; /* set_memory_faults()'s */
    bool kernel = false;            /* set_kernel()'s */
    ecuio_sim_kernel_faults kernel_faults;
};

inline SecurityTable& security_table()
{
    static SecurityTable table;
    return table;
}

/* rx's slot, a free one taken (take) for a reply ID not seen yet; NULL
 * when it has none. The caller holds the table's mutex. */
inline SecuritySlot* slot_of(SecurityTable& t, uint32_t rx, bool take)
{
    for (SecuritySlot& s : t.slots) {
        if (s.rx == rx) {
            return &s;
        }
    }
    for (SecuritySlot& s : t.slots) {
        if (take && s.rx == 0) {
            s.rx = rx;
            s.memory.vin = kVin;
            return &s;
        }
    }
    return nullptr;
}

/* rx's security state, as slot_of */
inline ecuio_sim_security* security_of(SecurityTable& t, uint32_t rx, bool take)
{
    SecuritySlot* s = slot_of(t, rx, take);
    return s != nullptr ? &s->state : nullptr;
}

/* Every ECU locked, no wrong key counted and no delay running */
inline void reset_security()
{
    SecurityTable& t = security_table();
    const std::lock_guard<std::mutex> lock(t.m);
    for (SecuritySlot& s : t.slots) {
        s = SecuritySlot{};
    }
}

/* The memory's faults, a knob in sim_memory.h's mem= words
 * ("flip:3,block:82"); NULL or "" for none. False, the knob unchanged,
 * for words the grammar refuses. Each ECU counts its own replies, from
 * reset_security() on. The fake DLL sets it from ECUIO_FAKE_MEM on each
 * write, the in-process fakes' tests with this, and put it back. */
inline bool set_memory_faults(const char* spec)
{
    ecuio_sim_memory_faults f;
    char why[256];
    if (spec != nullptr && spec[0] != '\0' &&
        !ecuio_sim_parse_memory(spec, strlen(spec), &f, why, sizeof why)) {
        return false;
    }
    SecurityTable& t = security_table();
    const std::lock_guard<std::mutex> lock(t.m);
    t.memory = f;
    return true;
}

/* The helper the engine ECU wants for region B, in the simulated ECU's
 * kernel= words ("1", "once,refuse"); NULL or "" for none. False, the
 * knob unchanged, for words the grammar refuses. The fake DLL sets it
 * from ECUIO_FAKE_KERNEL on each write. silent isn't the fakes'. */
inline bool set_kernel(const char* spec)
{
    bool on = false;
    ecuio_sim_kernel_faults f;
    if (spec != nullptr && spec[0] != '\0' &&
        !ecuio_sim_parse_kernel(spec, strlen(spec), &on, &f)) {
        return false;
    }
    SecurityTable& t = security_table();
    const std::lock_guard<std::mutex> lock(t.m);
    t.kernel = on;
    t.kernel_faults = f;
    return true;
}

/* A request that is no 27 reached the engine ECU on rx: it comes
 * between a strict ECU's seed and its key */
inline void security_between(uint32_t rx)
{
    if (!security_strict().load()) {
        return;
    }
    SecurityTable& t = security_table();
    const std::lock_guard<std::mutex> guard(t.m);
    ecuio_sim_security* s = security_of(t, rx, false);
    if (s != nullptr) {
        s->strict = true;
        ecuio_sim_security_between(*s);
    }
}

/* The engine ECU on rx's answer to a 27 request, to 22 <kGatedDid>, to
 * a memory read or upload or to an identification DID, into out (kReplyMax
 * bytes); the length, 0 when it is none of them (or a DID outside the
 * extended session, where answer() refuses it as any DID it lacks).
 * lock: a session request was answered, and the unlock goes with it. */
inline size_t security_answer(uint32_t rx, const unsigned char* req,
                              size_t req_len, bool lock, unsigned char* out)
{
    static_assert(ECUIO_SIM_MEMORY_REPLY_MAX <= kReplyMax,
                  "a memory read's longest reply fits the fakes' answer");
    const bool access = req_len >= 1 && req[0] == 0x27;
    const bool memory = req_len >= 1 && ecuio_sim_memory_service(req[0]);
    const uint16_t did = req_len == 3 && req[0] == 0x22
                             ? static_cast<uint16_t>((req[1] << 8) | req[2])
                             : 0;
    const bool gated = did == kGatedDid;
    const bool ident =
        did == ECUIO_SIM_HARDWARE_DID || did == ECUIO_SIM_SOFTWARE_DID;
    const bool helper = req_len >= 1 && (req[0] == 0x34 || req[0] == 0x31);
    if (!lock && !access && !gated && !memory && !ident && !helper) {
        return 0;
    }
    SecurityTable& t = security_table();
    const std::lock_guard<std::mutex> guard(t.m);
    SecuritySlot* slot = slot_of(t, rx, !lock);
    if (slot == nullptr) {
        return 0;
    }
    ecuio_sim_security* s = &slot->state;
    if (lock) {
        ecuio_sim_security_lock(*s);
        ecuio_sim_memory_end(slot->memory);
        ecuio_sim_kernel_end(slot->kernel);
        return 0;
    }
    slot->kernel.on = t.kernel;
    slot->kernel.faults = t.kernel_faults;
    if (req_len >= 1 && ecuio_sim_kernel_service(slot->kernel, req)) {
        return ecuio_sim_kernel_answer(slot->kernel, *s, in_programming(rx),
                                       req, static_cast<uint32_t>(req_len),
                                       out);
    }
    if (helper) {
        return 0;
    }
    if (gated) {
        return ecuio_sim_security_did(*s, in_session(rx), out);
    }
    slot->memory.faults = t.memory;
    if (ident) {
        return ecuio_sim_memory_did(slot->memory, in_session(rx), did, out);
    }
    if (memory && slot->kernel.on && !slot->kernel.running &&
        in_programming(rx) && s->unlocked != 0 &&
        ecuio_sim_memory_asks_code(req, static_cast<uint32_t>(req_len))) {
        out[0] = 0x7F;
        out[1] = req[0];
        out[2] = 0x7F;
        return 3;
    }
    if (memory) {
        const uint32_t n = ecuio_sim_memory_answer(
            slot->memory, *s, in_session(rx), req,
            static_cast<uint32_t>(req_len), out, in_programming(rx));
        if (req[0] == 0x37 && n == 1 && out[0] == 0x77) {
            ecuio_sim_kernel_upload_done(slot->kernel);
        }
        return n;
    }
    const unsigned ms = security_delay_ms().load();
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return ecuio_sim_security_answer(
        *s, in_session(rx),
        ms != 0 ? static_cast<int64_t>(ms) * 1000 : ECUIO_SIM_SECURITY_DELAY_US,
        now, req, static_cast<uint32_t>(req_len), out);
}

/* The answer of the ECU replying on rx: a module's from its rows, any
 * other ECU's from answer() above. unsupported_nrc as answer()'s: -1
 * keeps a module's codes (31 for a DID, 11 for a service), 0 answers
 * nothing, any other value is the code. 10 and 3E (two bytes) are the
 * session's: see above; 3E 00 is answered by every ECU, 3E 80 by none,
 * and 11 01 by the engine ECU.
 * 27 and kGatedDid are the engine ECU's security access, strict under
 * security_strict(); a memory read, an upload and the identification
 * DIDs are its memory's; a module refuses them as any service it lacks.
 * False when nothing answers, as for every request to absent_ecu(). */
inline bool answer_for(uint32_t rx, const unsigned char* req, size_t req_len,
                       long unsupported_nrc, unsigned char* out,
                       size_t* out_len)
{
    const uint32_t absent = absent_ecu().load(std::memory_order_acquire);
    if (absent == kEveryEcuAbsent || (rx != 0 && rx == absent)) {
        return false;
    }
    const ModuleTable& t = module_table();
    const uint32_t n = t.count.load(std::memory_order_acquire);
    bool module = false;
    for (uint32_t i = 0; i < n; ++i) {
        const ModuleRow& r = t.rows[i];
        if (r.rx != rx) {
            continue;
        }
        module = true;
        if (req_len == 3 && req[0] == 0x22 &&
            r.did == static_cast<uint16_t>((req[1] << 8) | req[2])) {
            out[0] = 0x62;
            out[1] = req[1];
            out[2] = req[2];
            memcpy(out + 3, r.data, r.len);
            *out_len = 3u + r.len;
            return true;
        }
    }
    if (!module && !(req_len >= 1 && req[0] == 0x27)) {
        security_between(rx);
    }
    if (req_len == 2 && req[0] == 0x3E) {
        if (req[1] != 0x00) {
            return false;
        }
        out[0] = 0x7E;
        out[1] = 0x00;
        *out_len = 2;
        return true;
    }
    if (req_len == 2 && req[0] == 0x11 && req[1] == 0x01 && !module) {
        set_session(rx, 0x01);
        (void)security_answer(rx, req, req_len, true, out);
        out[0] = 0x51;
        out[1] = 0x01;
        *out_len = 2;
        return true;
    }
    if (req_len == 2 && req[0] == 0x10 && req[1] == 0x82 && !module) {
        set_session(rx, 0x02);
        (void)security_answer(rx, req, req_len, true, out);
        return false; /* suppressed: ISO 14229 sends no 12 for it either */
    }
    if (req_len == 2 && req[0] == 0x10) {
        if (req[1] == 0x01 || ((req[1] == 0x03 || req[1] == 0x02) && !module)) {
            set_session(rx, req[1]);
            (void)security_answer(rx, req, req_len, true, out);
            const unsigned char ok[6] = {0x50, req[1], 0x00, 0x32, 0x01, 0xF4};
            memcpy(out, ok, sizeof ok);
            *out_len = sizeof ok;
            return true;
        }
        const unsigned code = session_nrc().load();
        out[0] = 0x7F;
        out[1] = 0x10;
        out[2] = code != 0 ? static_cast<unsigned char>(code) : 0x12;
        *out_len = 3;
        return true;
    }
    if (!module) {
        const size_t secured = security_answer(rx, req, req_len, false, out);
        if (secured != 0) {
            *out_len = secured;
            return true;
        }
        if (req_len == 3 && req[0] == 0x22 &&
            static_cast<uint16_t>((req[1] << 8) | req[2]) == kExtDid &&
            in_extended(rx)) {
            out[0] = 0x62;
            out[1] = req[1];
            out[2] = req[2];
            memcpy(out + 3, kExtDidValue, sizeof kExtDidValue);
            *out_len = 3 + sizeof kExtDidValue;
            return true;
        }
        return answer(req, req_len, unsupported_nrc, out, out_len);
    }
    if (req_len == 0 || unsupported_nrc == 0) {
        return false;
    }
    out[0] = 0x7F;
    out[1] = req[0];
    out[2] = unsupported_nrc > 0 ? static_cast<unsigned char>(unsupported_nrc)
             : req[0] == 0x22    ? 0x31
                                 : 0x11;
    *out_len = 3;
    return true;
}

} // namespace fake_ecu

#endif /* ECUIO_TESTS_FAKE_ECUS_H */
