// File/layer: cli/view_args.h | havne-cli, `view`'s argument words (ADR-0049 section 5)
// Testing: UNIT: CliViewTest (test_cli)
//
// What `havne-cli view`'s options take, as numbers and enums: --align-on,
// --at and --span seconds, and the --stretches words (kinds, pull end,
// levels, roles). No lines are written here (view_lines.h).

#ifndef HAVNE_CLI_VIEW_ARGS_H
#define HAVNE_CLI_VIEW_ARGS_H

#include "ecuio/recording.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cli {

// --align-on: rise:<signal>:<level>, fall:<signal>:<level>,
// marker:<label>, utc, engine-start (the rpm signal reaching 500
// after being below 100, core's ECUIO_RECORDING_FIND_ENGINE_START) or
// pull:<n>[,<n>...] (the start of each recording's nth pull, 1 the
// first: one number for every recording, or one for each in order). The
// level is the last colon's: a signal's name may hold one.
struct AlignOn {
    enum Kind { none, rise, fall, marker, utc, engine_start, pull };
    Kind kind = none;
    std::string signal;
    std::string label;
    double level = 0;
    std::vector<uint32_t> pulls; // pull: one, or one per recording
};

// The most pulls --align-on pull: counts to
constexpr uint32_t kAlignPullMax = 100000;

// pull:'s list: whole numbers from 1 to kAlignPullMax between commas,
// one or more, nothing else
inline bool parse_pull_list(const std::string& text, std::vector<uint32_t>& out)
{
    std::vector<uint32_t> list;
    uint32_t n = 0;
    size_t digits = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c >= '0' && c <= '9') {
            n = n * 10 + static_cast<uint32_t>(c - '0');
            if (++digits > 6) {
                return false;
            }
        } else if (c == ',' && digits != 0 && n >= 1 && n <= kAlignPullMax) {
            list.push_back(n);
            n = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    out = list;
    return true;
}

// A finite decimal, the whole text
inline bool parse_decimal(const char* text, double& out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text, &end);
    if (errno != 0 || *end != '\0' || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

inline bool parse_align_on(const char* text, AlignOn& out)
{
    if (text == nullptr) {
        return false;
    }
    const std::string s = text;
    AlignOn a;
    if (s == "utc") {
        a.kind = AlignOn::utc;
    } else if (s == "engine-start") {
        a.kind = AlignOn::engine_start;
    } else if (s.rfind("marker:", 0) == 0 && s.size() > 7) {
        a.kind = AlignOn::marker;
        a.label = s.substr(7);
    } else if (s.rfind("pull:", 0) == 0) {
        a.kind = AlignOn::pull;
        if (!parse_pull_list(s.substr(5), a.pulls)) {
            return false;
        }
    } else if (s.rfind("rise:", 0) == 0 || s.rfind("fall:", 0) == 0) {
        a.kind = s[0] == 'r' ? AlignOn::rise : AlignOn::fall;
        const std::string rest = s.substr(5);
        const size_t colon = rest.rfind(':');
        if (colon == std::string::npos || colon == 0 ||
            !parse_decimal(rest.c_str() + colon + 1, a.level)) {
            return false;
        }
        a.signal = rest.substr(0, colon);
    } else {
        return false;
    }
    out = a;
    return true;
}

// Seconds, signed (an aligned axis runs before its zero), to the
// microsecond; at most a billion either way
inline bool parse_seconds(const char* text, int64_t& us)
{
    double s = 0;
    if (!parse_decimal(text, s) || std::fabs(s) > 1e9) {
        return false;
    }
    us = static_cast<int64_t>(std::llround(s * 1e6));
    return true;
}

// --span A:B, seconds on the axis as --at takes them, A before B
struct Span {
    int64_t from_us = 0;
    int64_t to_us = 0;
};

inline bool parse_span(const char* text, Span& out)
{
    const char* colon = std::strchr(text, ':');
    if (colon == nullptr || std::strchr(colon + 1, ':') != nullptr) {
        return false;
    }
    const std::string from(text, static_cast<size_t>(colon - text));
    return parse_seconds(from.c_str(), out.from_us) &&
           parse_seconds(colon + 1, out.to_us) && out.from_us < out.to_us;
}

// --- --stretches (ADR-0056's appendix "the navigation aids") -----------

// km/h in a mile per hour, by definition
constexpr double kKmhPerMph = 1.609344;

// --stretches' word: pulls, cruises or all
inline bool parse_stretch_kinds(const char* word, uint32_t& kinds)
{
    if (std::strcmp(word, "pulls") == 0) {
        kinds = ECUIO_STRETCH_PULLS;
    } else if (std::strcmp(word, "cruises") == 0) {
        kinds = ECUIO_STRETCH_CRUISES;
    } else if (std::strcmp(word, "all") == 0) {
        kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    } else {
        return false;
    }
    return true;
}

// The rules core's defaults fill, kinds as given, every role core's pick
inline ecuio_stretch_rules_t stretch_rules(uint32_t kinds)
{
    ecuio_stretch_rules_t r{};
    r.kinds = kinds;
    r.throttle = r.engine_load = r.vehicle_speed = r.engine_speed = UINT32_MAX;
    return r;
}

// --pull-end's word: lift (the default, the flag clear) or top-speed
inline bool parse_pull_end(const char* word, uint32_t& flags)
{
    if (std::strcmp(word, "lift") == 0) {
        flags &= ~ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;
    } else if (std::strcmp(word, "top-speed") == 0) {
        flags |= ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;
    } else {
        return false;
    }
    return true;
}

// --stretch-level <key>=<value>: pull-throttle and pull-load in %,
// pull-hold, pull-gap and cruise-hold in seconds, pull-rise in rpm,
// pull-gain, cruise-speed-band and cruise-floor in km/h (mph with
// imperial, converted), and cruise-throttle-band in %; 0 or more, 0 the
// default
inline bool parse_stretch_level(const char* text, bool imperial,
                                ecuio_stretch_rules_t& r)
{
    const char* eq = std::strchr(text, '=');
    double v = 0;
    if (eq == nullptr || !parse_decimal(eq + 1, v) || v < 0 || v > 1e9) {
        return false;
    }
    const std::string key(text, static_cast<size_t>(eq - text));
    const double speed = imperial ? v * kKmhPerMph : v;
    const auto us = static_cast<uint64_t>(std::llround(v * 1e6));
    if (key == "pull-throttle") {
        r.pull_level = v;
    } else if (key == "pull-load") {
        r.pull_load_level = v;
    } else if (key == "pull-hold") {
        r.pull_hold_us = us;
    } else if (key == "pull-gap") {
        r.pull_gap_us = us;
    } else if (key == "pull-rise") {
        r.pull_min_rise = v;
    } else if (key == "pull-gain") {
        r.pull_min_gain = speed;
    } else if (key == "cruise-hold") {
        r.cruise_hold_us = us;
    } else if (key == "cruise-speed-band") {
        r.cruise_speed_band = speed;
    } else if (key == "cruise-throttle-band") {
        r.cruise_throttle_band = v;
    } else if (key == "cruise-floor") {
        r.cruise_floor = speed;
    } else {
        return false;
    }
    return true;
}

// --role <role>=<name>: throttle, load, speed or rpm, and a signal's name
inline bool parse_role(const char* text, uint32_t& role, std::string& name)
{
    const char* eq = std::strchr(text, '=');
    if (eq == nullptr || eq[1] == '\0') {
        return false;
    }
    const std::string key(text, static_cast<size_t>(eq - text));
    if (key == "throttle") {
        role = ECUIO_RECORDING_ROLE_THROTTLE;
    } else if (key == "load") {
        role = ECUIO_RECORDING_ROLE_ENGINE_LOAD;
    } else if (key == "speed") {
        role = ECUIO_RECORDING_ROLE_VEHICLE_SPEED;
    } else if (key == "rpm") {
        role = ECUIO_RECORDING_ROLE_ENGINE_SPEED;
    } else {
        return false;
    }
    name = eq + 1;
    return true;
}

// The rules' field for a role
inline uint32_t& role_field(ecuio_stretch_rules_t& r, uint32_t role)
{
    switch (role) {
    case ECUIO_RECORDING_ROLE_THROTTLE:
        return r.throttle;
    case ECUIO_RECORDING_ROLE_ENGINE_LOAD:
        return r.engine_load;
    case ECUIO_RECORDING_ROLE_VEHICLE_SPEED:
        return r.vehicle_speed;
    default:
        return r.engine_speed;
    }
}

} // namespace cli

#endif // HAVNE_CLI_VIEW_ARGS_H
