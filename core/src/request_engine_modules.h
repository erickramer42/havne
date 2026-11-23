// File/layer: core/src/request_engine_modules.h | internal, not
//             ABI-exported; a part of the request engine, included by
//             request_engine.h, whose description covers it
// The modules a session has out at once: the cap and the transport's
// ceiling (ADR-0065 sections 5 and 11). The free slot's picker,
// ecuio_engine_next_slot, is beside the turns in request_turns.h.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestSlotsTest (test_obd2); HARNESS: J2534HealthTest
//          (test_j2534_health)

#ifndef ECUIO_REQUEST_ENGINE_MODULES_H
#define ECUIO_REQUEST_ENGINE_MODULES_H

#include "request_engine_reply.h"

#include <cstdint>

/* The cap on requests out at once and the transport's ceiling, each at
 * least 1: attach calls it after ecuio_engine_init */
inline void ecuio_engine_slots(ecuio_request_engine* e, uint32_t cap,
                               uint32_t ceiling)
{
    e->cap = cap > 0 ? cap : 1u;
    e->ceiling = ceiling > 0 ? ceiling : 1u;
}

#endif
