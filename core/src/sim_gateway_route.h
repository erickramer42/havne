// File/layer: core/src/sim_gateway_route.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: SimGatewayTest (test_obd2), fuzz_sim
//
// The simulated gateway at work (ADR-0046's appendix "a gateway node"):
// its verdict on a request for a module behind it, judged before the
// module's busy check, answer, draw and fate, so a request it withholds
// never reaches the module; and its answer on its own request ID.

#ifndef ECUIO_SIM_GATEWAY_ROUTE_H
#define ECUIO_SIM_GATEWAY_ROUTE_H

#include "sim_gateway.h"
#include "sim_session.h"
#include "sim_types.h"

#include <cstdint>

enum class ecuio_sim_gateway_fate : uint8_t {
    forward,  /* the module answers */
    withhold, /* the gateway answers for it: out */
    drop      /* nobody answers */
};

struct ecuio_sim_gateway_verdict {
    ecuio_sim_gateway_fate fate = ecuio_sim_gateway_fate::forward;
    uint8_t out[3] = {}; /* withhold's 7F <service> <code> */
};

/* The codes ISO 14229-1 section 7.5 has a server keep to itself on a
 * functional request */
inline bool ecuio_sim_gateway_suppressed(uint8_t nrc)
{
    return nrc == 0x11 || nrc == 0x12 || nrc == 0x31 || nrc == 0x7E ||
           nrc == 0x7F;
}

/* The gateway's verdict on req (len at least 1) for the module answering
 * on reply_id. A refusal goes on the module's reply ID; on a functional
 * request a withheld one is silent unless aloud, and aloud keeps the
 * suppressed codes silent unless unsuppressed. */
inline ecuio_sim_gateway_verdict
ecuio_sim_gateway_route(const ecuio_sim_gateway_config& g, uint32_t reply_id,
                        const uint8_t* req, bool functional)
{
    ecuio_sim_gateway_verdict v;
    uint8_t nrc = 0;
    const ecuio_sim_route r =
        ecuio_sim_gateway_outcome(g, reply_id, req[0], &nrc);
    switch (r) {
    case ecuio_sim_route::forward:
        return v;
    case ecuio_sim_route::refuse:
        nrc = nrc != 0 ? nrc : ECUIO_SIM_ROUTE_REFUSE_NRC;
        break;
    case ecuio_sim_route::silent:
        v.fate = ecuio_sim_gateway_fate::drop;
        return v;
    }
    const bool quiet =
        functional &&
        (!g.aloud || (ecuio_sim_gateway_suppressed(nrc) && !g.unsuppressed));
    v.fate =
        quiet ? ecuio_sim_gateway_fate::drop : ecuio_sim_gateway_fate::withhold;
    ecuio_sim_detail::refuse(req[0], nrc, v.out);
    return v;
}

/* The gateway's answer to a physical request on its own request ID at
 * the ECU's time now_us, into out, its length: 10 01, 10 03, 3E 00 and
 * 3E 80 (silent) as any node's session answers them, 27 refused 11
 * without security access, every other service 7F <service> 11 */
inline uint32_t ecuio_sim_gateway_answer(ecuio_sim_session& s, int64_t now_us,
                                         const uint8_t* req, uint32_t len,
                                         uint8_t* out)
{
    if (len == 0) {
        return 0;
    }
    bool handled = false;
    const uint32_t n = ecuio_sim_session_answer(s, true, false, now_us, req,
                                                len, out, &handled);
    return handled ? n : ecuio_sim_detail::refuse(req[0], 0x11, out);
}

#endif /* ECUIO_SIM_GATEWAY_ROUTE_H */
