// File/layer: core/src/sim_types.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest (test_obd2)
//
// The simulated ECU's types (ADR-0046): the IDs and constants, the URI's
// config, the ECU's state and clock, and the NRC reply every node's answer
// shares.

#ifndef ECUIO_SIM_TYPES_H
#define ECUIO_SIM_TYPES_H

#include "ecuio/core.h"
#include "ecuio/profile.h" /* ECUIO_PROFILE_NAME_MAX */
#include "ecuio/vin.h"
#include "mock_drive_cycle.h"
#include "recording_stretch.h"
#include "sim_faults.h"
#include "sim_gateway.h"
#include "sim_kernel.h"
#include "sim_memory.h"
#include "sim_recording.h"
#include "sim_timing.h"
#include <cstdint>
#include <cstring>

enum class ecuio_sim_source : uint8_t { mock, replay };
/* parked: canned, the engine off by the key and the vehicle standing */
enum class ecuio_sim_values : uint8_t { drive, canned, parked };

/* A VIN of the VIN alphabet that decodes (model year 2021, its check
 * digit right) under a manufacturer code NHTSA doesn't list, so the
 * simulated car is no maker's */
constexpr char ECUIO_SIM_DEFAULT_VIN[] = "ZZZFAKE11M0000001";

/* Service 09 item 0A, 20 bytes, NUL-padded */
constexpr char ECUIO_SIM_ECU_NAME[] = "ECM-HavneSim";

/* The ECU's IDs: the obd2 engine ECU's, and the functional request */
constexpr uint32_t ECUIO_SIM_FUNCTIONAL_ID = 0x7DFu;
constexpr uint32_t ECUIO_SIM_REQUEST_ID = 0x7E0u;
constexpr uint32_t ECUIO_SIM_REPLY_ID = 0x7E8u;
/* modules=2: a second module, a transmission's, on the next pair */
constexpr uint32_t ECUIO_SIM_MODULE_REQUEST_ID = 0x7E1u;
constexpr uint32_t ECUIO_SIM_MODULE_REPLY_ID = 0x7E9u;

/* The second module's DIDs (Service 22) and their data: made up for the
 * simulated car, no maker's. D100 fluid temperature (A - 40, 90 deg C),
 * D101 the gear (3), D102 the module's name as text, a segmented reply */
struct ecuio_sim_module_did {
    uint16_t did;
    uint8_t len;
    uint8_t data[16];
};
constexpr ecuio_sim_module_did ECUIO_SIM_MODULE_DIDS[] = {
    {0xD100, 1, {0x82}},
    {0xD101, 1, {0x03}},
    {0xD102, 12, {'T', 'C', 'M', '-', 'H', 'a', 'v', 'n', 'e', 'S', 'i', 'm'}},
};

/* The engine ECU's DIDs that answer only in its extended session
 * (ADR-0060): D200 to D203, two made-up bytes each (no manufacturer's) */
struct ecuio_sim_extended_did {
    uint16_t did;
    uint8_t data[2];
};
constexpr ecuio_sim_extended_did ECUIO_SIM_EXTENDED_DIDS[] = {
    {0xD200, {0x1A, 0x2C}},
    {0xD201, {0x00, 0x64}},
    {0xD202, {0x03, 0xE8}},
    {0xD203, {0xFF, 0x01}},
};

/* ISO 14229-2's S3: an ECU in the extended session that hears no request
 * this long returns to the default session */
constexpr int64_t ECUIO_SIM_S3_US = 5000000;

/* The longest reply: a recording's, ISO-TP's longest message (the mock's
 * is a pack of six PIDs, the longest PID 13 bytes) */
constexpr uint32_t ECUIO_SIM_REPLY_MAX = ECUIO_SIM_RECORDING_REPLY_MAX;
static_assert(1 + 6 * (1 + 13) <= ECUIO_SIM_REPLY_MAX, "the mock's pack fits");

/* One tick of the ECU's clock: the drive cycle's step */
constexpr int64_t ECUIO_SIM_TICK_US = 50000;

struct ecuio_sim_config {
    ecuio_sim_source source = ecuio_sim_source::mock;
    ecuio_sim_values values = ecuio_sim_values::drive;
    char vin[ECUIO_VIN_LEN + 1] = {};
    bool vin_given = false; /* replay: 09 02 answers it, not the car's */
    ecuio_sim_faults faults;    /* every module's, each on its own count */
    ecuio_sim_aims aimed;       /* faults aimed at one module each */
    int64_t pending_us = ECUIO_SIM_PENDING_US; /* pending_ms= */
    uint8_t pending_service = 0;
    ecuio_sim_timing::timings timing; /* delay=, fc=, cf=, seed= */
    bool second_module = false; /* mock: modules=2 */
    bool sessions = false;      /* mock: sessions=1, the profile's modules */
    ecuio_sim_gateway_config gateway; /* mock: gateway= and its keys */
    ecuio_sim_memory_faults memory;   /* mock: mem=, the engine ECU's */
    bool kernel = false;              /* mock: kernel=, the engine ECU's */
    ecuio_sim_kernel_faults kernel_faults;
    /* mock: profile=, a registered profile whose set's DIDs the ECUs
     * answer (sim_dids.h), a view into the URI parsed (valid while it
     * is); empty for none */
    const char* profile = nullptr;
    size_t profile_len = 0;
    /* replay: the recording's path, a view into the URI parsed (valid
     * while it is), and whether its time goes round its span */
    const char* path = nullptr;
    size_t path_len = 0;
    bool loop = false;
    ecuio_stretch::stretch stretch; /* replay: from=, to= */
};

struct ecuio_sim_ecu {
    ecuio_sim_config config;
    /* replay: the recording loaded (the caller's, outliving the ECU) */
    const ecuio_sim_recording* recording = nullptr;
    int64_t time_us = 0;        /* since the ECU started, by its ticks */
    ecuio_mock::DriveCycle car; /* values=drive: stepped by the bus side */
    float engine_s = 0.0f;      /* time since the engine last started */
    bool cleared = false;       /* a 04 was obeyed */
};

/* One tick (kTickS of the ECU's time): its clock runs; the mock's drive
 * cycle steps, and the run time since the engine started counts while it
 * runs */
inline void ecuio_sim_tick(ecuio_sim_ecu& e)
{
    e.time_us += ECUIO_SIM_TICK_US;
    if (e.recording != nullptr) {
        return;
    }
    const bool was = e.car.running;
    e.car.step();
    if (!e.car.running) {
        e.engine_s = 0.0f;
    } else {
        e.engine_s = was ? e.engine_s + ecuio_mock::kTickS : 0.0f;
    }
}

namespace ecuio_sim_detail {

inline uint32_t refuse(uint8_t service, uint8_t nrc, uint8_t* out)
{
    out[0] = 0x7F;
    out[1] = service;
    out[2] = nrc;
    return 3;
}

} // namespace ecuio_sim_detail

#endif /* ECUIO_SIM_TYPES_H */
