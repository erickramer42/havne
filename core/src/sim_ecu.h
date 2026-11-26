// File/layer: core/src/sim_ecu.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimRecordingTest (test_obd2)
//
// What the simulated ECU answers (ADR-0046): the engine ECU, from the
// service byte on, its framing (ISO-TP) and its bus left to sim.cpp.
// mock://: Service 01 comes from the mock's drive cycle (values=drive)
// or the fakes' canned readings (values=canned, sim_canned.h); the
// diagnostics are the mock's (mock_ecu.h's tables, the fakes' codes,
// monitors and freeze frame) in both; identification is the VIN the URI
// gives and an ECU name. modules=2 adds a second module's answers, its
// own made-up DIDs (the appendix of 2026-11-04). The engine ECU also has
// an extended session (ADR-0060): 10 03 enters it, 3E 00 keeps it, S3 of
// silence leaves it, and DIDs D200-D203 answer only in it. In that
// session it has security access too (ADR-0066 section 2.7,
// sim_security.h): 27 unlocks a level, and DID D210 answers once one is.
// Unlocked, it has memory to read by address (sim_memory.h): 23 answers a
// made-up region's bytes, 35, 36 and 37 upload them block by block, and
// two identification DIDs answer in the session.
// sessions=1 gives a profile's modules the extended session as well (no
// security access), so a session can be held on many modules at once.
// replay://: a recording's replies at the ECU's time since it started
// (sim_recording.h). Faults break every Nth reply, as ADR-0041 section
// 4's ECU does, and pending puts a reply off behind a 7F <service> 78.
//
// mock://'s gateway= adds a gateway node that routes each request for the
// modules behind it by service (sim_gateway.h, sim_gateway_route.h).
//
// The parts: sim_types.h (what a node is, and refuse), sim_pids.h (the PID
// answers), sim_session.h (a node's session and its answer), sim_engine.h
// (the engine ECU's answer), sim_module.h (the second module's),
// sim_gateway_route.h (the gateway's verdict and answer) and sim_uri.h
// (the URI). This header includes them all and keeps the fate.

#ifndef ECUIO_SIM_ECU_H
#define ECUIO_SIM_ECU_H

#include "ecuio/core.h"
#include "ecuio/profile.h" /* ECUIO_PROFILE_NAME_MAX */
#include "ecuio/vin.h"
#include "mock_drive_cycle.h"
#include "mock_ecu.h"
#include "sim_canned.h"
#include "sim_faults.h"
#include "recording_stretch.h"
#include "sim_memory.h"
#include "sim_recording.h"
#include "sim_security.h"
#include "sim_timing.h"
#include "uri_query.h"

#include "absolute_path.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sim_types.h"
#include "sim_pids.h"
#include "sim_session.h"
#include "sim_engine.h"
#include "sim_enhanced.h"
#include "sim_module.h"
#include "sim_gateway_route.h"
#include "sim_uri.h"

/* The fate of a module's next reply from its periods f, counting it in
 * c: bitmaps are never broken, single-frame and segmented replies are
 * counted apart, and the two frame faults take segmented replies only */
inline ecuio_sim_fate ecuio_sim_next_fate(const ecuio_sim_faults& f,
                                          ecuio_sim_counts& c,
                                          const uint8_t* req, uint32_t req_len,
                                          uint32_t reply_len)
{
    if (req_len == 2 && req[0] == 0x01 &&
        ecuio_sim_detail::bitmap_pid(req[1])) {
        return ecuio_sim_fate::intact;
    }
    const bool segmented = reply_len > 7;
    const uint32_t k = ++(segmented ? c.segmented : c.singles);
    const auto every = [k](uint32_t n) { return n != 0 && k % n == 0; };
    if (every(f.silent)) {
        return ecuio_sim_fate::silent;
    }
    if (every(f.late)) {
        return ecuio_sim_fate::late;
    }
    if (every(f.pending)) {
        return ecuio_sim_fate::pending;
    }
    if (segmented && every(f.bad_sequence)) {
        return ecuio_sim_fate::bad_sequence;
    }
    if (segmented && every(f.stalled)) {
        return ecuio_sim_fate::stalled;
    }
    return ecuio_sim_fate::intact;
}

#endif /* ECUIO_SIM_ECU_H */
