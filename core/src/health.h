// File/layer: core/src/health.h | internal, C++17, Qt-free; a car's health
//             as one scrubbed report (ADR-0054's appendix "the health
//             report as built")
// Testing: UNIT: HealthTest (test_core)
//
// The report's facts from what a session publishes (its info, URI and
// set, each channel's diagnostics and identity) and the values a caller
// holds, written as JSON or text from the one model, so the two formats
// can't hold different facts. Pure: no session, file or clock, so a test
// builds reports by hand. ecuio_health_export (health.cpp) gathers the
// inputs from a session, writes the file and checks it.

#ifndef ECUIO_HEALTH_H
#define ECUIO_HEALTH_H

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <string>

namespace ecuio_scrub {
class values;
}

namespace ecuio_health {

/* What a report is made from, every pointer the caller's */
struct inputs {
    const ecuio_session_info_t* info = nullptr;
    std::string uri;                         /* as given; scrubbed here */
    const ecuio_signal_set_t* set = nullptr; /* the session's; may be NULL */
    const ecuio_channel_diagnostics_t* diagnostics = nullptr;
    const ecuio_channel_identity_t* identity = nullptr;
    uint32_t channels = 0; /* entries of both */
    const ecuio_health_value_t* values = nullptr;
    uint32_t value_count = 0;
    uint32_t units = ECUIO_UNITS_METRIC;
    uint32_t latest_model_year = 0;
    uint64_t utc_us = 0; /* 0: no time */
};

/* The report in one format, its counts in *report, and each identity
 * value it must not hold (every answered identification item's text)
 * added to *identity for the check */
std::string write(const inputs& in, uint32_t format,
                  ecuio_health_report_t* report, ecuio_scrub::values* identity);

/* The report's bytes checked for the identity values and the VIN's
 * shape (a match fails it, its class in r.failed_class), then written as
 * <out>.partial and renamed into place; false with the detail set */
bool put(const std::string& out, const std::string& bytes,
         const ecuio_scrub::values& identity, ecuio_health_report_t& r);

/* microseconds since 1970 UTC as "YYYY-MM-DDTHH:MM:SSZ" */
std::string utc_text(uint64_t utc_us);

/* v to dp decimals with a '.', whatever the locale */
std::string fixed(double v, uint8_t dp);

} // namespace ecuio_health

#endif /* ECUIO_HEALTH_H */
