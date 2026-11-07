// File/layer: core/src/health.h | internal, C++17, Qt-free; a car's health
//             as one scrubbed report (ADR-0054's appendix "the health
//             report as built")
// Testing: UNIT: HealthTest (test_core)
//
// The report's facts from what a session publishes (its info, URI and
// set, each channel's diagnostics and identity) and the values a caller
// holds, or from what a recording holds (its header, set, replies and
// values), written as JSON or text from the one model, so the two
// formats can't hold different facts. Pure: no session, file or clock,
// so a test builds reports by hand. ecuio_health_export (health.cpp)
// gathers the inputs from a session or a recording's folder, writes the
// file and checks it.

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

/* The vehicle's baseline recording as its events.log names it
 * (ecuio_health_export reads it): never its folder, whose name can hold
 * the vehicle's label */
struct baseline_facts {
    bool named = false;      /* a folder was given */
    bool read = false;       /* its events.log is a recording's */
    std::string why;         /* why it wasn't read, when named */
    uint64_t started_us = 0; /* its start, microseconds since 1970 UTC */
    uint64_t length_us = 0;  /* its last timed line's time */
    bool has_length = false; /* a timed line was there */
    std::string profile;
    std::string uri; /* its backend is the URI's scheme */
};

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

    /* A recording's report (info NULL): each ECU's reports rebuilt from
     * its replies (health_replies.h), a kind with none "not recorded" */
    bool recording = false;
    bool frames = true;      /* the recording holds frames.csv */
    std::string profile;     /* its header's */
    uint64_t started_us = 0; /* its start, microseconds since 1970 UTC */
    uint64_t from_us = 0;    /* the range read, host_us */
    uint64_t to_us = 0;

    baseline_facts baseline;
};

/* The report in one format, its counts in *report, and each identity
 * value it must not hold (every answered identification item's text)
 * added to *identity for the check */
std::string write(const inputs& in, uint32_t format,
                  ecuio_health_report_t* report, ecuio_scrub::values* identity);

/* The report's bytes checked for the identity values and the VIN's
 * shape (a match fails it, its class in r.failed_class), then written as
 * a name of its own beside <out> and renamed into place; false with the
 * detail set */
bool put(const std::string& out, const std::string& bytes,
         const ecuio_scrub::values& identity, ecuio_health_report_t& r);

/* microseconds since 1970 UTC as "YYYY-MM-DDTHH:MM:SSZ" */
std::string utc_text(uint64_t utc_us);

/* v to dp decimals with a '.', whatever the locale */
std::string fixed(double v, uint8_t dp);

} // namespace ecuio_health

#endif /* ECUIO_HEALTH_H */
