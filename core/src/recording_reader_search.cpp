// File/layer: core/src/recording_reader_search.cpp | C++17, Qt-free;
//             find, the roles and the stretches (ADR-0049 section 5,
//             ADR-0056's appendixes)
// Testing: UNIT: RecordingTest, RecordingStretchesTest (test_core)

#include "recording_reader.h"
#include "error_detail.h"
#include "recording_aids.h"

#include <algorithm>
#include <new>

using namespace ecuio_recording_reader;

extern "C" {

namespace {

/* A query's stop: asked before each second's rows */
struct query_stop {
    ecuio_cancel_t cancel = nullptr;
    void* user = nullptr;

    bool asked() const { return cancel != nullptr && cancel(user) != 0; }
};

ecuio_status_t canceled()
{
    ecuio_detail_set("the search was canceled");
    return ECUIO_ERR_CANCELED;
}

/* The first sample of a number signal from from_us on that reaches level
 * (rising: at or above it; falling: at or below) after a sample past
 * hysteresis from it (rising: below level - hysteresis) armed it */
ecuio_status_t find_crossing(const ecuio_recording_t* r, uint32_t signal,
                             bool rising, double level, double hysteresis,
                             uint64_t from_us, const query_stop& stop,
                             uint64_t* at_us)
{
    const signal_entry& s = r->signals[signal];
    const double arm_at = rising ? level - hysteresis : level + hysteresis;
    auto arms = [&](double v) { return rising ? v < arm_at : v > arm_at; };
    auto fires = [&](double v) { return rising ? v >= level : v <= level; };
    bool armed = false;
    try {
        const auto first_second = static_cast<uint32_t>(
            (std::min<uint64_t>)(from_us / kSecondUs, UINT32_MAX));
        auto it = std::lower_bound(
            s.numbers.begin(), s.numbers.end(), first_second,
            [](const number_second& e, uint32_t v) { return e.second < v; });
        FILE* f = nullptr;
        file_ptr owner;
        for (; it != s.numbers.end(); ++it) {
            /* A second that can neither arm nor fire is passed by its
             * summary: its range says so for every sample in it */
            if (!std::isnan(it->lowest)) {
                if (!armed && !arms(rising ? it->lowest : it->highest)) {
                    continue;
                }
                if (armed && !fires(rising ? it->highest : it->lowest)) {
                    continue;
                }
            }
            if (stop.asked()) {
                return canceled();
            }
            const index_entry* e = r->index_of(it->second);
            if (e == nullptr) {
                return ECUIO_ERR_RECORDING_CHANGED;
            }
            std::shared_ptr<const std::vector<raw_row>> rows;
            const ecuio_status_t rc = r->rows_of(f, owner, *e, &rows);
            if (rc != ECUIO_OK) {
                return rc;
            }
            for (const raw_row& row : *rows) {
                if (row.signal != signal || row.host_us < from_us) {
                    continue;
                }
                if (armed && fires(row.value)) {
                    *at_us = row.host_us;
                    return ECUIO_OK;
                }
                if (arms(row.value)) {
                    armed = true;
                }
            }
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

/* "rpm" in any ASCII case, the whole unit */
bool is_rpm(const char* unit)
{
    static const char kRpm[] = "rpm";
    for (size_t i = 0; i < sizeof kRpm; ++i) {
        char c = unit[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kRpm[i]) {
            return false;
        }
    }
    return true;
}

char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/* Whether text is word (lower case), ASCII case ignored */
bool is_word(const char* text, const char* word)
{
    for (; *word != '\0'; ++text, ++word) {
        if (ascii_lower(*text) != *word) {
            return false;
        }
    }
    return *text == '\0';
}

/* Whether text holds word (lower case), ASCII case ignored */
bool holds(const char* text, const char* word)
{
    const size_t n = std::strlen(word);
    for (const char* p = text; *p != '\0'; ++p) {
        size_t k = 0;
        while (k < n && p[k] != '\0' && ascii_lower(p[k]) == word[k]) {
            ++k;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

/* Whether a signal can play role, by its unit and name */
bool plays(const signal_entry& s, uint32_t role)
{
    if (s.text()) {
        return false;
    }
    const char* unit = s.pub.unit;
    const char* name = s.pub.name;
    switch (role) {
    case ECUIO_RECORDING_ROLE_ENGINE_SPEED:
        return is_rpm(unit);
    case ECUIO_RECORDING_ROLE_VEHICLE_SPEED:
        return (is_word(unit, "km/h") || is_word(unit, "mph")) &&
               holds(name, "speed");
    case ECUIO_RECORDING_ROLE_THROTTLE:
        return is_word(unit, "%") &&
               (holds(name, "throttle") || holds(name, "pedal")) &&
               !holds(name, "relative") && !holds(name, "commanded");
    case ECUIO_RECORDING_ROLE_ENGINE_LOAD:
        return is_word(unit, "%") && holds(name, "load");
    default:
        return false;
    }
}

/* The signal playing role: the lowest nonzero rank among several, else
 * the first; UINT32_MAX with none */
uint32_t role_signal(const ecuio_recording_t* r, uint32_t role)
{
    uint32_t best = UINT32_MAX;
    for (uint32_t k = 0; k < r->signals.size(); ++k) {
        const signal_entry& s = r->signals[k];
        if (!plays(s, role)) {
            continue;
        }
        const uint8_t rank = s.pub.plot_rank;
        if (best == UINT32_MAX ||
            (rank != 0 && (r->signals[best].pub.plot_rank == 0 ||
                           rank < r->signals[best].pub.plot_rank))) {
            best = k;
        }
    }
    return best;
}

/* The engine-speed signal: a number signal in rpm */
uint32_t engine_speed_signal(const ecuio_recording_t* r)
{
    return role_signal(r, ECUIO_RECORDING_ROLE_ENGINE_SPEED);
}

} // namespace

ecuio_status_t ecuio_recording_find(const ecuio_recording_t* r,
                                    const ecuio_recording_find_t* what,
                                    uint64_t from_us, uint64_t* at_us)
{
    return ecuio_recording_find_with(r, what, from_us, nullptr, nullptr, at_us);
}

ecuio_status_t ecuio_recording_find_with(const ecuio_recording_t* r,
                                         const ecuio_recording_find_t* what,
                                         uint64_t from_us,
                                         ecuio_cancel_t cancel, void* user_data,
                                         uint64_t* at_us)
{
    if (r == nullptr || what == nullptr || at_us == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    for (uint64_t v : what->reserved) {
        if (v != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    if (what->flags != 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *at_us = ECUIO_RECORDING_NONE;
    const query_stop stop{cancel, user_data};
    if (what->kind == ECUIO_RECORDING_FIND_MARKER) {
        if (what->label_utf8 == nullptr) {
            return ECUIO_ERR_NULL_ARGUMENT;
        }
        if (stop.asked()) {
            return canceled();
        }
        for (const marker_entry& m : r->markers) {
            if (m.host_us >= from_us && m.label == what->label_utf8) {
                *at_us = m.host_us;
                break;
            }
        }
        return ECUIO_OK;
    }
    if (what->kind == ECUIO_RECORDING_FIND_ENGINE_START) {
        /* A NaN is refused too: it equals nothing */
        if (what->signal != 0 || what->direction != 0 || what->level != 0.0 ||
            what->hysteresis != 0.0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        if (stop.asked()) {
            return canceled();
        }
        const uint32_t rpm = engine_speed_signal(r);
        if (rpm == UINT32_MAX) {
            return ECUIO_OK;
        }
        return find_crossing(r, rpm, true, ECUIO_ENGINE_START_RPM,
                             ECUIO_ENGINE_START_RPM - ECUIO_ENGINE_STOPPED_RPM,
                             from_us, stop, at_us);
    }
    if (what->kind != ECUIO_RECORDING_FIND_THRESHOLD ||
        what->signal >= r->signals.size() ||
        (what->direction != ECUIO_RECORDING_FIND_RISING &&
         what->direction != ECUIO_RECORDING_FIND_FALLING) ||
        !(what->hysteresis >= 0.0) || std::isnan(what->level)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (r->signals[what->signal].text()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (stop.asked()) {
        return canceled();
    }
    return find_crossing(r, what->signal,
                         what->direction == ECUIO_RECORDING_FIND_RISING,
                         what->level, what->hysteresis, from_us, stop, at_us);
}

ecuio_status_t ecuio_recording_role_signal(const ecuio_recording_t* r,
                                           uint32_t role, uint32_t* index)
{
    if (r == nullptr || index == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (role < ECUIO_RECORDING_ROLE_ENGINE_SPEED ||
        role > ECUIO_RECORDING_ROLE_ENGINE_LOAD) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *index = role_signal(r, role);
    return ECUIO_OK;
}

namespace {

/* How far under its highest a pull's speed may sit and be at it */
constexpr double kPullTolerance = 0.0;

enum : size_t { kThrottle, kLoad, kSpeed, kRpm, kRoles };

/* A rule's level or band: its default for 0; false for a negative or NaN */
bool level_of(double given, double fallback, double* out)
{
    if (std::isnan(given) || given < 0.0) {
        return false;
    }
    *out = given == 0.0 ? fallback : given;
    return true;
}

uint64_t time_of(uint64_t given, uint64_t fallback)
{
    return given == 0 ? fallback : given;
}

/* A role's signal: the rules' index, or core's pick for UINT32_MAX;
 * false for an index past the signals or on a text signal */
bool role_of(const ecuio_recording_t* r, uint32_t given, uint32_t role,
             uint32_t* out)
{
    if (given == UINT32_MAX) {
        *out = role_signal(r, role);
        return true;
    }
    if (given >= r->signals.size() || r->signals[given].text()) {
        return false;
    }
    *out = given;
    return true;
}

/* Each role signal's samples in [from_us, to_us] and the one on each side
 * of it, in one pass over the rows */
ecuio_status_t gather(const ecuio_recording_t* r, const uint32_t* signal,
                      uint64_t from_us, uint64_t to_us, const query_stop& stop,
                      ecuio_aids::samples* out)
{
    uint64_t lo[kRoles];
    uint64_t hi[kRoles];
    uint64_t first = UINT64_MAX;
    uint64_t last = 0;
    for (size_t k = 0; k < kRoles; ++k) {
        lo[k] = from_us;
        hi[k] = to_us;
        if (signal[k] == UINT32_MAX) {
            continue;
        }
        uint64_t at = ECUIO_RECORDING_NONE;
        ecuio_status_t rc = ecuio_recording_step(
            r, signal[k], from_us, ECUIO_RECORDING_STEP_BACKWARD, &at);
        if (rc != ECUIO_OK) {
            return rc;
        }
        if (at != ECUIO_RECORDING_NONE) {
            lo[k] = at;
        }
        rc = ecuio_recording_step(r, signal[k], to_us,
                                  ECUIO_RECORDING_STEP_FORWARD, &at);
        if (rc != ECUIO_OK) {
            return rc;
        }
        if (at != ECUIO_RECORDING_NONE) {
            hi[k] = at;
        }
        first = (std::min)(first, lo[k]);
        last = (std::max)(last, hi[k]);
    }
    if (first == UINT64_MAX) {
        return ECUIO_OK;
    }
    auto it = std::lower_bound(
        r->index.begin(), r->index.end(), first / kSecondUs,
        [](const index_entry& e, uint64_t v) { return e.second < v; });
    FILE* f = nullptr;
    file_ptr owner;
    for (; it != r->index.end() && it->second <= last / kSecondUs; ++it) {
        if (stop.asked()) {
            return canceled();
        }
        std::shared_ptr<const std::vector<raw_row>> rows;
        const ecuio_status_t rc = r->rows_of(f, owner, *it, &rows);
        if (rc != ECUIO_OK) {
            return rc;
        }
        for (const raw_row& row : *rows) {
            for (size_t k = 0; k < kRoles; ++k) {
                if (row.signal == signal[k] && row.host_us >= lo[k] &&
                    row.host_us <= hi[k]) {
                    out[k].push_back(
                        ecuio_aids::sample{row.host_us, row.value});
                }
            }
        }
    }
    return ECUIO_OK;
}

} // namespace

ecuio_status_t ecuio_recording_stretches(const ecuio_recording_t* r,
                                         const ecuio_stretch_rules_t* rules,
                                         uint64_t from_us, uint64_t to_us,
                                         ecuio_recording_stretch_t* out,
                                         uint32_t capacity, uint32_t* count)
{
    return ecuio_recording_stretches_with(r, rules, from_us, to_us, nullptr,
                                          nullptr, out, capacity, count);
}

ecuio_status_t ecuio_recording_stretches_with(
    const ecuio_recording_t* r, const ecuio_stretch_rules_t* rules,
    uint64_t from_us, uint64_t to_us, ecuio_cancel_t cancel, void* user_data,
    ecuio_recording_stretch_t* out, uint32_t capacity, uint32_t* count)
{
    if (r == nullptr || rules == nullptr || count == nullptr ||
        (out == nullptr && capacity != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const uint32_t known = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    if (rules->kinds == 0 || (rules->kinds & ~known) != 0 ||
        (rules->flags & ~ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED) != 0 ||
        from_us >= to_us) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint64_t v : rules->reserved) {
        if (v != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    double pull_level = 0;
    double load_level = 0;
    double speed_band = 0;
    double throttle_band = 0;
    double floor = 0;
    double min_rise = 0;
    double min_gain = 0;
    uint32_t signal[kRoles] = {};
    if (!level_of(rules->pull_level, ECUIO_STRETCH_DEFAULT_PULL_LEVEL,
                  &pull_level) ||
        !level_of(rules->pull_min_rise, ECUIO_STRETCH_DEFAULT_PULL_RISE,
                  &min_rise) ||
        !level_of(rules->pull_min_gain, ECUIO_STRETCH_DEFAULT_PULL_GAIN,
                  &min_gain) ||
        !level_of(rules->pull_load_level, ECUIO_STRETCH_DEFAULT_PULL_LOAD_LEVEL,
                  &load_level) ||
        !level_of(rules->cruise_speed_band,
                  ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND, &speed_band) ||
        !level_of(rules->cruise_throttle_band,
                  ECUIO_STRETCH_DEFAULT_CRUISE_THROTTLE_BAND, &throttle_band) ||
        !level_of(rules->cruise_floor, ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR,
                  &floor) ||
        !role_of(r, rules->throttle, ECUIO_RECORDING_ROLE_THROTTLE,
                 &signal[kThrottle]) ||
        !role_of(r, rules->engine_load, ECUIO_RECORDING_ROLE_ENGINE_LOAD,
                 &signal[kLoad]) ||
        !role_of(r, rules->vehicle_speed, ECUIO_RECORDING_ROLE_VEHICLE_SPEED,
                 &signal[kSpeed]) ||
        !role_of(r, rules->engine_speed, ECUIO_RECORDING_ROLE_ENGINE_SPEED,
                 &signal[kRpm])) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *count = 0;
    const uint64_t pull_hold =
        time_of(rules->pull_hold_us, ECUIO_STRETCH_DEFAULT_PULL_HOLD_US);
    const uint64_t cruise_hold =
        time_of(rules->cruise_hold_us, ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US);
    const query_stop stop{cancel, user_data};
    if (stop.asked()) {
        return canceled();
    }
    try {
        ecuio_aids::samples s[kRoles];
        const ecuio_status_t rc = gather(r, signal, from_us, to_us, stop, s);
        if (rc != ECUIO_OK) {
            return rc;
        }
        const bool has_throttle = signal[kThrottle] != UINT32_MAX;
        const bool has_speed = signal[kSpeed] != UINT32_MAX;
        const size_t gate = has_throttle ? kThrottle : kLoad;
        const size_t measure = has_speed ? kSpeed : kRpm;
        const ecuio_aids::samples none;
        const ecuio_aids::samples* engine_speed =
            signal[kRpm] != UINT32_MAX ? &s[kRpm] : nullptr;
        /* The rules' km/h in an mph signal's own unit, by the display's
         * table */
        double per_kmh = 1.0;
        ecuio_display_value_t mph{};
        if (has_speed && is_word(r->signals[signal[kSpeed]].pub.unit, "mph") &&
            ecuio_display_convert("km/h", 0, 1.0, ECUIO_UNITS_IMPERIAL, &mph) ==
                ECUIO_DISPLAY_OK) {
            per_kmh = mph.value;
        }
        const ecuio_aids::pull_rules pull{
            has_throttle ? pull_level : load_level,
            pull_hold,
            time_of(rules->pull_gap_us, ECUIO_STRETCH_DEFAULT_PULL_GAP_US),
            kPullTolerance,
            min_rise,
            min_gain * per_kmh};
        std::vector<ecuio_aids::stretch> found;
        if ((rules->kinds & ECUIO_STRETCH_PULLS) != 0 &&
            signal[gate] != UINT32_MAX && signal[measure] != UINT32_MAX) {
            const uint32_t flags =
                (has_throttle ? 0u : ECUIO_RECORDING_STRETCH_BY_LOAD) |
                (has_speed ? 0u : ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
            ecuio_aids::find_pulls(
                s[gate], s[measure], has_speed,
                (rules->flags & ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED) != 0,
                engine_speed, pull, flags, found);
        }
        if ((rules->kinds & ECUIO_STRETCH_CRUISES) != 0 && has_speed) {
            const ecuio_aids::cruise_rules cruise{
                cruise_hold, speed_band * per_kmh, throttle_band,
                floor * per_kmh};
            ecuio_aids::find_cruises(
                s[kSpeed], has_throttle ? &s[kThrottle] : nullptr, cruise,
                has_throttle ? 0u : ECUIO_RECORDING_STRETCH_NO_THROTTLE, found);
        }
        ecuio_aids::cut_to(
            found, from_us, to_us, pull, has_speed, engine_speed, cruise_hold,
            signal[measure] != UINT32_MAX ? s[measure] : none, s[kSpeed]);
        *count =
            static_cast<uint32_t>((std::min<size_t>)(found.size(), UINT32_MAX));
        for (uint32_t i = 0; i < capacity && i < *count; ++i) {
            const ecuio_aids::stretch& f = found[i];
            ecuio_recording_stretch_t& o = out[i];
            o = ecuio_recording_stretch_t{};
            o.kind = f.kind;
            o.signal = f.kind == ECUIO_RECORDING_STRETCH_PULL ? signal[measure]
                                                              : signal[kSpeed];
            o.from_us = f.from_us;
            o.to_us = f.to_us;
            o.start = f.start;
            o.end = f.end;
            o.flags = f.flags;
        }
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} // extern "C"
