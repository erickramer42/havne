// File/layer: core/src/sim_session.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimSecurityTest (test_obd2)
//
// A node's diagnostic session (ADR-0060) and the answer to services 10, 3E
// and 27 (ADR-0066 section 2.7); a node's session state hangs here.

#ifndef ECUIO_SIM_SESSION_H
#define ECUIO_SIM_SESSION_H

#include "sim_types.h"
#include "sim_security.h"
#include <cstdint>
#include <cstring>

/* One node's diagnostic session (ADR-0060): extended since a 10 03 and
 * until S3 of silence, or a 10 01; the ECU's own clock times it. Its
 * security access (ADR-0066 section 2.7) is locked again by each of
 * those and by a 10 03 that enters the session anew. */
struct ecuio_sim_session {
    bool extended = false;
    int64_t heard_us = 0; /* the ECU's time of the last request heard */
    ecuio_sim_security security;
};

/* A physical request at the ECU's time now_us: S3 lapses the session
 * first, then the request counts as heard. Services 10 and 3E are
 * answered here (handled true; the length, 0 for none): 10 03 enters the
 * extended session when has_extended (else 7F 10 12), 10 01 leaves it,
 * any other 10 is 7F 10 12; 3E 00 is 7E 00, 3E 80 is silent, any other
 * 3E 7F 3E 12; a wrong length 7F <service> 13. A node with an extended
 * session answers 27 here too (sim_security.h), every session entered
 * or left locks it, and any other request comes between a strict one's
 * seed and its key. Another service: handled false, the
 * caller's answer. A functional request never reaches this: session
 * control and security access to 0x7DF go unanswered and change
 * nothing. */
inline uint32_t ecuio_sim_session_answer(ecuio_sim_session& s,
                                         bool has_extended, int64_t now_us,
                                         const uint8_t* req, uint32_t len,
                                         uint8_t* out, bool* handled)
{
    using namespace ecuio_sim_detail;
    if (s.extended && now_us - s.heard_us >= ECUIO_SIM_S3_US) {
        s.extended = false;
        ecuio_sim_security_lock(s.security);
    }
    s.heard_us = now_us;
    if (has_extended && len > 0 && req[0] == 0x27) {
        *handled = true;
        return ecuio_sim_security_answer(s.security, s.extended,
                                         ECUIO_SIM_SECURITY_DELAY_US, now_us,
                                         req, len, out);
    }
    ecuio_sim_security_between(s.security);
    *handled = len > 0 && (req[0] == 0x10 || req[0] == 0x3E);
    if (!*handled) {
        return 0;
    }
    if (len != 2) {
        return refuse(req[0], 0x13, out);
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
    if (req[1] == 0x01 || (req[1] == 0x03 && has_extended)) {
        s.extended = req[1] == 0x03;
        ecuio_sim_security_lock(s.security);
        /* the session, then P2 = 50 ms and P2* = 5 s in 10 ms units */
        const uint8_t ok[6] = {0x50, req[1], 0x00, 0x32, 0x01, 0xF4};
        std::memcpy(out, ok, sizeof ok);
        return sizeof ok;
    }
    return refuse(0x10, 0x12, out);
}

#endif /* ECUIO_SIM_SESSION_H */
