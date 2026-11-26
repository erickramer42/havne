// File/layer: core/src/sim_engine.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimSecurityTest, SimMemoryTest (test_obd2)
//
// The engine ECU's answer to a request: Service 01, 03, 04, 07, 09 and 0A,
// Service 22's DIDs, and a node in a session's extended DIDs.

#ifndef ECUIO_SIM_ENGINE_H
#define ECUIO_SIM_ENGINE_H

#include "sim_types.h"
#include "sim_pids.h"
#include "sim_session.h"
#include "sim_security.h"
#include "sim_recording.h"
#include <cstdint>
#include <cstring>

/* The ECU's reply to req (len bytes, the service byte first) into out
 * (ECUIO_SIM_REPLY_MAX bytes), its length; 0 for none. functional: the
 * request came on 0x7DF, where ISO 14229-1 has an ECU keep its negative
 * replies 11, 12 and 31 to itself. extended: the node is in the extended
 * session, where D200-D203 answer (else 7F 22 31), and D210 with them
 * when security is the node's: 7F 22 33 until a level is unlocked; a
 * node with memory answers its two identification DIDs there too. The
 * mock's ECU clears on a 04; a recording answers from its replies at the
 * ECU's time. */
inline uint32_t ecuio_sim_answer(ecuio_sim_ecu& e, const uint8_t* req,
                                 uint32_t len, bool functional, uint8_t* out,
                                 bool extended = false,
                                 const ecuio_sim_security* security = nullptr,
                                 const ecuio_sim_memory* memory = nullptr)
{
    using namespace ecuio_sim_detail;
    if (len == 0) {
        return 0;
    }
    const uint8_t service = req[0];
    uint32_t n = 0;
    if (e.recording != nullptr) {
        n = ecuio_sim_recording_answer(
            *e.recording, e.time_us, req, len,
            e.config.vin_given ? e.config.vin : nullptr, out);
    } else if (service == 0x01 && len == 2) {
        out[0] = 0x41;
        out[1] = req[1];
        const uint32_t m = pid_data(e, req[1], out + 2);
        n = m > 0 ? 2 + m : refuse(0x01, 0x12, out);
    } else if (service == 0x01 && len >= 3 && len <= 7) {
        /* J1979's pack: each PID it has, in the order asked */
        n = 1;
        out[0] = 0x41;
        for (uint32_t k = 1; k < len; ++k) {
            uint8_t data[13];
            const uint32_t m = pid_data(e, req[k], data);
            if (m > 0) {
                out[n] = req[k];
                std::memcpy(out + n + 1, data, m);
                n += 1 + m;
            }
        }
        if (n == 1) {
            return 0; /* none it has: no answer */
        }
    } else if (service == 0x04 && len == 1) {
        e.cleared = true;
        out[0] = 0x44;
        n = 1;
    } else if ((service == 0x03 || service == 0x07 || service == 0x0A ||
                service == 0x02) &&
               len <= 3) {
        n = from_table(e, req, len, out);
        if (n == 0) {
            n = refuse(service, 0x12, out);
        }
    } else if (service == 0x09 && len == 2) {
        out[0] = 0x49;
        out[1] = req[1];
        switch (req[1]) {
        case 0x00: /* items 02 and 0A */
            out[2] = 0x40;
            out[3] = 0x40;
            out[4] = 0x00;
            out[5] = 0x00;
            n = 6;
            break;
        case 0x02:
            out[2] = 0x01;
            std::memcpy(out + 3, e.config.vin, ECUIO_VIN_LEN);
            n = 3 + ECUIO_VIN_LEN;
            break;
        case 0x0A:
            out[2] = 0x01;
            std::memset(out + 3, 0, 20);
            std::memcpy(out + 3, ECUIO_SIM_ECU_NAME,
                        sizeof ECUIO_SIM_ECU_NAME - 1);
            n = 3 + 20;
            break;
        default:
            n = refuse(0x09, 0x12, out);
            break;
        }
    } else if (service == 0x22) {
        n = refuse(0x22, 0x31, out);
        if (extended && len == 3) {
            const uint16_t did = static_cast<uint16_t>((req[1] << 8) | req[2]);
            if (did == ECUIO_SIM_GATED_DID && security != nullptr) {
                n = ecuio_sim_security_did(*security, extended, out);
            }
            if (memory != nullptr) {
                const uint32_t m =
                    ecuio_sim_memory_did(*memory, extended, did, out);
                n = m != 0 ? m : n;
            }
            for (const ecuio_sim_extended_did& d : ECUIO_SIM_EXTENDED_DIDS) {
                if (d.did == did) {
                    out[0] = 0x62;
                    out[1] = req[1];
                    out[2] = req[2];
                    std::memcpy(out + 3, d.data, sizeof d.data);
                    n = 3 + sizeof d.data;
                    break;
                }
            }
        }
    } else {
        n = refuse(service, 0x11, out);
    }
    if (functional && n == 3 && out[0] == 0x7F &&
        (out[2] == 0x11 || out[2] == 0x12 || out[2] == 0x31)) {
        return 0;
    }
    return n;
}

/* The engine ECU's reply as a node in session s gives it: its extended
 * DIDs in the session, the gated one by its security access, the
 * identification DIDs where it has memory */
inline uint32_t ecuio_sim_answer(ecuio_sim_ecu& e, const ecuio_sim_session& s,
                                 const uint8_t* req, uint32_t len,
                                 bool functional, uint8_t* out)
{
    return ecuio_sim_answer(e, req, len, functional, out, s.extended,
                            &s.security, &s.memory);
}

#endif /* ECUIO_SIM_ENGINE_H */
