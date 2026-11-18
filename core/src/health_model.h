// File/layer: core/src/health_model.h | internal, C++17, Qt-free; the
//             health report's model (ADR-0054's appendix "the health
//             report as built")
// Testing: UNIT: HealthTest (test_core)
//
// The facts of one report, built once by build (health_build.cpp) and
// read by the JSON and text writers (health_write.cpp); health.cpp's
// header describes the whole.

#ifndef ECUIO_HEALTH_MODEL_H
#define ECUIO_HEALTH_MODEL_H

#include "health.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecuio_health {

struct shown {
    std::string name;
    std::string value; /* as text: the number to its decimals, or a label */
    bool number = true;
    std::string unit;
    bool has_age = false;
    uint64_t age_ms = 0;
    bool stale = false;
};

struct code {
    std::string code;
    std::string title;
};

struct dtc_list {
    const char* key = "";
    const char* label = "";
    uint8_t service = 0;
    const char* outcome = "unasked";
    std::string reply;
    std::vector<code> codes;
    uint32_t more = 0;
};

struct monitor {
    std::string name;
    bool complete = false;
    bool cycle = false; /* PID 41 answered */
    bool cycle_enabled = false;
    bool cycle_complete = false;
};

struct item {
    std::string name;
    const char* outcome = "unasked";
};

struct ecu {
    std::string id;
    const char* state = "complete"; /* complete, reading, not asked;
                                       recorded */
    const char* readiness = "unasked";
    std::string readiness_reply;
    bool mil = false;
    uint32_t dtc_count = 0;
    bool readiness_known = false;
    std::vector<monitor> monitors;
    dtc_list lists[ECUIO_DIAG_LISTS];
    const char* freeze = "unasked";
    std::string freeze_reply;
    bool freeze_stored = false;
    code freeze_code;
    std::vector<shown> freeze_values;
    std::vector<shown> since_cleared;
    std::vector<item> identification;
};

struct model {
    const char* units = "metric";
    uint64_t identification_left_out = 0;
    std::vector<std::string> incomplete;
    std::vector<ecu> ecus;
    std::vector<shown> key_values;
    /* the last of key_values that are since-cleared values no listed ECU
     * takes */
    size_t key_since_cleared = 0;
    std::string started; /* a recording's start, from_s and to_s */
    std::string from_s;
    std::string to_s;
    std::string uri;
    std::string backend;
    std::string profile;
    std::string utc;
    std::string make;
    uint32_t baud = 0;
    ecuio_inspection_t inspection{};
    uint16_t model_year = 0;
    bool recording = false;
    bool frames = true;
    bool has_started = false;
    bool has_utc = false;
    bool vin = false;
    bool check_matches = false;
    bool ranks = false; /* the set ranks a signal */

    /* the vehicle's baseline recording */
    bool baseline_named = false;
    bool baseline_read = false;
    bool baseline_is_source = false; /* the same start as this recording */
    bool baseline_has_started = false;
    bool baseline_has_length = false;
    std::string baseline_why;
    std::string baseline_started;
    std::string baseline_length_s;
    std::string baseline_profile;
    std::string baseline_backend;
};

/* A def whose value is a number, not text or hex */
bool numeric(const ecuio_signal_def_t& d);

/* The report's model from its inputs; each answered identification
 * item's text goes to *identity for the check */
model build(const inputs& in, ecuio_scrub::values* identity);

} // namespace ecuio_health

#endif /* ECUIO_HEALTH_MODEL_H */
