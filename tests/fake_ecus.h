// File/layer: tests/fake_ecus.h | test-only, C++17, Qt-free, no platform
//             headers; header-only
// Testing: UNIT: FakeEcuTest (test_obd2)
//
// The fakes' ECUs by reply ID, over the engine ECU of fake_ecu.h: a
// module of the fake's own that answers Service 0x22 from its rows, an
// ECU that isn't on the car, each ECU's diagnostic session (ADR-0060)
// and security access (ADR-0066 section 2.7, core/src/sim_security.h,
// the simulated ECU's own), and answer_for(), the one answer the fake
// J2534 DLL, the fake ELM327 and the simulated bus's ECU all give.

#ifndef ECUIO_TESTS_FAKE_ECUS_H
#define ECUIO_TESTS_FAKE_ECUS_H

#include "fake_ecu.h"
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
 * it. A module has none: 10 03 gets 7F 10 session_nrc() (0x12 by
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

inline bool in_extended(uint32_t rx)
{
    for (auto& s : extended_ids()) {
        if (s.load() == rx) {
            return true;
        }
    }
    return false;
}

inline void set_extended(uint32_t rx, bool on)
{
    if (on == in_extended(rx)) {
        return;
    }
    for (auto& s : extended_ids()) {
        uint32_t want = on ? 0 : rx;
        if (s.compare_exchange_strong(want, on ? rx : 0)) {
            return;
        }
    }
}

/* Every ECU back in the default session */
inline void reset_sessions()
{
    for (auto& s : extended_ids()) {
        s.store(0);
    }
}

/* ---- Security access (ADR-0066 section 2.7) ----
 * The engine ECU answers 27 as the simulated ECU does (sim_security.h):
 * in its extended session, a state for each reply ID, locked again by
 * every 10 01 and 10 03 it answers. kGatedDid answers only once a level
 * is unlocked (7F 22 33 before it, in the session). The lockout's delay
 * runs on the steady clock, since the fakes keep none of their own. */
constexpr uint16_t kGatedDid = ECUIO_SIM_GATED_DID;

/* The lockout's delay in milliseconds, a knob: 0 is the simulated ECU's
 * 10 s. An in-process fake's test sets it and puts it back. */
inline std::atomic<unsigned>& security_delay_ms()
{
    static std::atomic<unsigned> ms{0};
    return ms;
}

struct SecuritySlot {
    uint32_t rx = 0;
    ecuio_sim_security state;
};

struct SecurityTable {
    std::mutex m;
    SecuritySlot slots[kSessionSlots];
};

inline SecurityTable& security_table()
{
    static SecurityTable table;
    return table;
}

/* rx's state, a free slot taken (take) for a reply ID not seen yet;
 * NULL when it has none. The caller holds the table's mutex. */
inline ecuio_sim_security* security_of(SecurityTable& t, uint32_t rx, bool take)
{
    for (SecuritySlot& s : t.slots) {
        if (s.rx == rx) {
            return &s.state;
        }
    }
    for (SecuritySlot& s : t.slots) {
        if (take && s.rx == 0) {
            s.rx = rx;
            return &s.state;
        }
    }
    return nullptr;
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

/* The engine ECU on rx's answer to a 27 request or to 22 <kGatedDid>,
 * into out; the length, 0 when it is neither (or the DID outside the
 * extended session, where answer() refuses it as any DID it lacks).
 * lock: a session request was answered, and the unlock goes with it. */
inline size_t security_answer(uint32_t rx, const unsigned char* req,
                              size_t req_len, bool lock, unsigned char* out)
{
    const bool access = req_len >= 1 && req[0] == 0x27;
    const bool gated =
        req_len == 3 && req[0] == 0x22 &&
        static_cast<uint16_t>((req[1] << 8) | req[2]) == kGatedDid;
    if (!lock && !access && !gated) {
        return 0;
    }
    SecurityTable& t = security_table();
    const std::lock_guard<std::mutex> guard(t.m);
    ecuio_sim_security* s = security_of(t, rx, !lock);
    if (s == nullptr) {
        return 0;
    }
    if (lock) {
        ecuio_sim_security_lock(*s);
        return 0;
    }
    if (gated) {
        return ecuio_sim_security_did(*s, in_extended(rx), out);
    }
    const unsigned ms = security_delay_ms().load();
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return ecuio_sim_security_answer(
        *s, in_extended(rx),
        ms != 0 ? static_cast<int64_t>(ms) * 1000 : ECUIO_SIM_SECURITY_DELAY_US,
        now, req, static_cast<uint32_t>(req_len), out);
}

/* The answer of the ECU replying on rx: a module's from its rows, any
 * other ECU's from answer() above. unsupported_nrc as answer()'s: -1
 * keeps a module's codes (31 for a DID, 11 for a service), 0 answers
 * nothing, any other value is the code. 10 and 3E (two bytes) are the
 * session's: see above; 3E 00 is answered by every ECU, 3E 80 by none.
 * 27 and kGatedDid are the engine ECU's security access; a module
 * refuses 27 as any service it lacks.
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
    if (req_len == 2 && req[0] == 0x3E) {
        if (req[1] != 0x00) {
            return false;
        }
        out[0] = 0x7E;
        out[1] = 0x00;
        *out_len = 2;
        return true;
    }
    if (req_len == 2 && req[0] == 0x10) {
        if (req[1] == 0x01 || (req[1] == 0x03 && !module)) {
            set_extended(rx, req[1] == 0x03);
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
