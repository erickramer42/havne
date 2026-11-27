// File/layer: core/src/sim_session.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimSecurityTest, SimMemoryTest (test_obd2)
//
// A node's diagnostic session (ADR-0060) and the answer to services 10, 3E
// and 27 (ADR-0066 section 2.7) and to a memory read (sim_memory.h); a
// node's session state hangs here.

#ifndef ECUIO_SIM_SESSION_H
#define ECUIO_SIM_SESSION_H

#include "sim_types.h"
#include "sim_memory.h"
#include "sim_security.h"
#include "ecuio/plugin.h"
#include <cstdint>
#include <cstring>

/* One node's diagnostic session (ADR-0060): extended since a 10 03 and
 * until S3 of silence, or a 10 01; the ECU's own clock times it. Its
 * security access (ADR-0066 section 2.7) is locked again by each of
 * those and by a 10 03 that enters the session anew. */
struct ecuio_sim_session {
    bool extended = false;
    bool programming = false; /* since a 10 02; never with extended */
    int64_t heard_us = 0; /* the ECU's time of the last request heard */
    ecuio_sim_security security;
    ecuio_sim_memory memory; /* answers where its vin is set */
};

static_assert(ECUIO_SIM_MEMORY_REPLY_MAX == ECUIO_SIM_REPLY_MAX,
              "a memory read's longest reply is the sim's");

/* Whether s is in session (ECUIO_STEP_SESSION_*, or 01 the default) */
inline bool ecuio_sim_session_in(const ecuio_sim_session& s, uint8_t session)
{
    if (session == ECUIO_STEP_SESSION_PROGRAMMING) {
        return s.programming;
    }
    if (session == ECUIO_STEP_SESSION_EXTENDED) {
        return s.extended;
    }
    return session == 0x01 && !s.extended && !s.programming;
}

/* Whether a 23 or a 35 may reach region B, the code: the one judgment
 * both ask, its lock judged after it as region A's is */
inline bool ecuio_sim_memory_code_readable(const ecuio_sim_session& s)
{
    return s.programming;
}

/* Into session (01, 02 or 03): security locked, an upload ended */
inline void ecuio_sim_session_enter(ecuio_sim_session& s, uint8_t session)
{
    s.extended = session == ECUIO_STEP_SESSION_EXTENDED;
    s.programming = session == ECUIO_STEP_SESSION_PROGRAMMING;
    ecuio_sim_security_lock(s.security);
    ecuio_sim_memory_end(s.memory);
}

/* S3 of silence at the ECU's time now_us leaves the extended session,
 * locks its security access and ends an upload */
inline void ecuio_sim_session_lapse(ecuio_sim_session& s, int64_t now_us)
{
    if ((s.extended || s.programming) &&
        now_us - s.heard_us >= ECUIO_SIM_S3_US) {
        ecuio_sim_session_enter(s, 0x01);
    }
}

/* A physical request at the ECU's time now_us: S3 lapses the session
 * first, then the request counts as heard. Services 10 and 3E are
 * answered here (handled true; the length, 0 for none): 10 03 enters the
 * extended session when has_extended (else 7F 10 12), 10 01 leaves it,
 * any other 10 is 7F 10 12; a node with memory too enters the
 * programming session on 10 02 from any session, and answers 11 01 51 01
 * into the default session (any other 11 7F 11 12); 3E 00 is 7E 00, 3E 80 is silent, any other
 * 3E 7F 3E 12; a wrong length 7F <service> 13. A node with security
 * access (has_security, an extended session too) answers 27 here
 * (sim_security.h), every session entered or left locks it, and any
 * other request comes between a strict one's seed and its key. A node
 * with memory (memory.vin set) answers a memory read and an upload here
 * too (sim_memory.h), out ECUIO_SIM_REPLY_MAX bytes; a session entered,
 * left or lapsed ends an upload. Another service:
 * handled false, the caller's answer. A functional request
 * never reaches this: session control and security access to 0x7DF go
 * unanswered and change nothing. */
inline uint32_t ecuio_sim_session_answer(ecuio_sim_session& s,
                                         bool has_extended, bool has_security,
                                         int64_t now_us, const uint8_t* req,
                                         uint32_t len, uint8_t* out,
                                         bool* handled)
{
    using namespace ecuio_sim_detail;
    ecuio_sim_session_lapse(s, now_us);
    s.heard_us = now_us;
    if (has_extended && has_security && len > 0 && req[0] == 0x27) {
        *handled = true;
        return ecuio_sim_security_answer(
            s.security, s.extended || s.programming, s.security.delay_us,
            now_us, req, len, out);
    }
    ecuio_sim_security_between(s.security);
    if (s.memory.vin != nullptr && len > 0 &&
        ecuio_sim_memory_service(req[0])) {
        *handled = true;
        return ecuio_sim_memory_answer(s.memory, s.security,
                                       s.extended || s.programming, req, len,
                                       out, ecuio_sim_memory_code_readable(s));
    }
    const bool programs = has_extended && s.memory.vin != nullptr;
    *handled = len > 0 && (req[0] == 0x10 || req[0] == 0x3E ||
                           (req[0] == 0x11 && programs));
    if (!*handled) {
        return 0;
    }
    if (len != 2) {
        return refuse(req[0], 0x13, out);
    }
    if (req[0] == 0x11) {
        if (req[1] != 0x01) {
            return refuse(0x11, 0x12, out);
        }
        ecuio_sim_session_enter(s, 0x01);
        out[0] = 0x51;
        out[1] = 0x01;
        return 2;
    }
    if (req[0] == 0x3E) {
        if (req[1] == 0x80) {
            return 0;
        }
        if (req[1] != 0x00) {
            return refuse(0x3E, 0x12, out);
        }
        out[0] = 0x7E;
        out[1] = 0x00;
        return 2;
    }
    if (req[1] == 0x01 || (req[1] == 0x03 && has_extended) ||
        (req[1] == 0x02 && programs)) {
        ecuio_sim_session_enter(s, req[1]);
        /* the session, then P2 = 50 ms and P2* = 5 s in 10 ms units */
        const uint8_t ok[6] = {0x50, req[1], 0x00, 0x32, 0x01, 0xF4};
        std::memcpy(out, ok, sizeof ok);
        return sizeof ok;
    }
    return refuse(0x10, 0x12, out);
}

#endif /* ECUIO_SIM_SESSION_H */
