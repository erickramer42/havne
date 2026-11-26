// File/layer: core/src/sim_module.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest (test_obd2)
//
// The second module's answer (modules=2): its made-up DIDs under Service 22.

#ifndef ECUIO_SIM_MODULE_H
#define ECUIO_SIM_MODULE_H

#include "sim_types.h"
#include "sim_enhanced.h"
#include <cstdint>
#include <cstring>

/* A functional request's refusals go unsaid, as the engine ECU's */
inline uint32_t ecuio_sim_module_answer(const uint8_t* req, uint32_t len,
                                        bool functional, uint8_t* out,
                                        ecuio_sim_ecu* enhanced = nullptr)
{
    using namespace ecuio_sim_detail;
    if (len == 0) {
        return 0;
    }
    uint32_t n = 0;
    if (req[0] == 0x22 && enhanced != nullptr) {
        n = ecuio_sim_enhanced_answer(enhanced->enhanced, enhanced->car,
                                      ECUIO_SIM_MODULE_REPLY_ID, req, len, out);
    }
    if (n == 0 && req[0] == 0x22) {
        n = refuse(0x22, 0x31, out);
        if (len == 3) {
            const uint16_t did = static_cast<uint16_t>((req[1] << 8) | req[2]);
            for (const ecuio_sim_module_did& d : ECUIO_SIM_MODULE_DIDS) {
                if (d.did == did) {
                    out[0] = 0x62;
                    out[1] = req[1];
                    out[2] = req[2];
                    std::memcpy(out + 3, d.data, d.len);
                    n = 3u + d.len;
                    break;
                }
            }
        }
    } else if (n == 0) {
        n = refuse(req[0], 0x11, out);
    }
    if (functional && out[0] == 0x7F) {
        return 0;
    }
    return n;
}

#endif /* ECUIO_SIM_MODULE_H */
