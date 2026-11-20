// File/layer: fuzz/fuzz_recording_query.h | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (one open and its queries)

#pragma once

#include "fuzz_recording_common.h"
#include "fuzz_recording_conditions.h"

namespace {

/* The closed span over a signal's every sample (ecuio_recording_stats):
 * the open's count, and for a number its range, with the extremes' times
 * among its samples'; a text signal has its count alone */
void whole_stats(const ecuio_recording_t* r, uint32_t k,
                 const ecuio_recording_signal_t& s)
{
    ecuio_recording_stats_t st;
    memset(&st, 0xA5, sizeof st);
    if (ecuio_recording_stats(r, &k, 1, 0, s.last_us + 1, &st) != ECUIO_OK ||
        st.samples != s.samples || st.flags != 0 || st.reserved0 != 0 ||
        st.reserved[0] != 0 || st.reserved[1] != 0 || st.reserved[2] != 0) {
        abort();
    }
    if ((s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
        if (!std::isnan(st.lowest) || !std::isnan(st.highest) ||
            !std::isnan(st.mean) || st.lowest_us != ECUIO_RECORDING_NONE ||
            st.highest_us != ECUIO_RECORDING_NONE) {
            abort();
        }
        return;
    }
    /* With no value but NaNs the open's range reads 0 and the stats' NaN */
    const double lowest = std::isnan(st.lowest) ? 0.0 : st.lowest;
    const double highest = std::isnan(st.highest) ? 0.0 : st.highest;
    if (lowest != s.lowest || highest != s.highest) {
        abort();
    }
    for (uint64_t at : {st.lowest_us, st.highest_us}) {
        if (at != ECUIO_RECORDING_NONE && (at < s.first_us || at > s.last_us)) {
            abort();
        }
    }
}

/* A step at a time (ecuio_recording_step): forward from the start each
 * step lands later, among the signal's times, the first on its first
 * sample and the last on its last, a time per sample at most; backward
 * from past the end is its last sample, and before its first is none */
void steps_through(const ecuio_recording_t* r, uint32_t k,
                   const ecuio_recording_signal_t& s)
{
    constexpr uint32_t kMostSteps = 512;
    uint64_t t = 0;
    uint64_t at = 0;
    uint32_t steps = 0;
    bool ended = false;
    while (steps < kMostSteps) {
        if (ecuio_recording_step(r, k, t, ECUIO_RECORDING_STEP_FORWARD, &at) !=
            ECUIO_OK) {
            abort();
        }
        if (at == ECUIO_RECORDING_NONE) {
            ended = true;
            break;
        }
        /* A sample at 0 is the start itself, which no step lands on */
        if (at <= t || at < s.first_us || at > s.last_us ||
            (steps == 0 && s.first_us > 0 && at != s.first_us)) {
            abort();
        }
        t = at;
        ++steps;
    }
    if (ended && (steps > s.samples || t != s.last_us)) {
        abort();
    }
    if (ecuio_recording_step(r, k, s.last_us + 1, ECUIO_RECORDING_STEP_BACKWARD,
                             &at) != ECUIO_OK ||
        at != s.last_us ||
        ecuio_recording_step(r, k, s.first_us, ECUIO_RECORDING_STEP_BACKWARD,
                             &at) != ECUIO_OK ||
        at != ECUIO_RECORDING_NONE) {
        abort();
    }
}

/* The navigation aids over the whole recording: a role's signal is a
 * number of the recording's or none; the stretches by core's rules lie
 * in the recording, in time order, of a known kind and flags, on a
 * signal of its own, their reserved words zero; the count is the same
 * asked for alone and with room, and nothing past it is written */
void aids(const ecuio_recording_t* r, const ecuio_recording_info_t& info)
{
    for (uint32_t role = ECUIO_RECORDING_ROLE_ENGINE_SPEED - 1;
         role <= ECUIO_RECORDING_ROLE_ENGINE_LOAD + 1; ++role) {
        uint32_t index = 0;
        const ecuio_status_t rc = ecuio_recording_role_signal(r, role, &index);
        if (role < ECUIO_RECORDING_ROLE_ENGINE_SPEED ||
            role > ECUIO_RECORDING_ROLE_ENGINE_LOAD) {
            if (rc != ECUIO_ERR_INVALID_ARGUMENT) {
                abort();
            }
            continue;
        }
        if (rc != ECUIO_OK) {
            abort();
        }
        if (index != UINT32_MAX) {
            ecuio_recording_signal_t s{};
            if (index >= info.signal_count ||
                ecuio_recording_get_signal(r, index, &s) != ECUIO_OK ||
                (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
                abort();
            }
        }
    }
    ecuio_stretch_rules_t rules{};
    rules.kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    rules.throttle = rules.engine_load = UINT32_MAX;
    rules.vehicle_speed = rules.engine_speed = UINT32_MAX;
    uint32_t count = UINT32_MAX;
    if (info.duration_us == 0) {
        /* No span: the range is refused, whatever the folder holds */
        if (ecuio_recording_stretches(r, &rules, 0, 0, nullptr, 0, &count) !=
            ECUIO_ERR_INVALID_ARGUMENT) {
            abort();
        }
        return;
    }
    if (ecuio_recording_stretches(r, &rules, 0, info.duration_us, nullptr, 0,
                                  &count) != ECUIO_OK ||
        count > 1000000) {
        abort();
    }
    constexpr uint32_t kUntouched = 0xA5A5A5A5u;
    std::vector<ecuio_recording_stretch_t> out(count + 1);
    out[count].kind = kUntouched;
    uint32_t again = UINT32_MAX;
    if (ecuio_recording_stretches(r, &rules, 0, info.duration_us, out.data(),
                                  count + 1, &again) != ECUIO_OK ||
        again != count || out[count].kind != kUntouched) {
        abort();
    }
    constexpr uint32_t kFlags = ECUIO_RECORDING_STRETCH_BY_LOAD |
                                ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED |
                                ECUIO_RECORDING_STRETCH_NO_THROTTLE |
                                ECUIO_RECORDING_STRETCH_CUT_START |
                                ECUIO_RECORDING_STRETCH_CUT_END;
    uint64_t before = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_recording_stretch_t& x = out[i];
        if ((x.kind != ECUIO_RECORDING_STRETCH_PULL &&
             x.kind != ECUIO_RECORDING_STRETCH_CRUISE) ||
            x.signal >= info.signal_count || x.from_us > x.to_us ||
            x.to_us > info.duration_us || x.from_us < before ||
            (x.flags & ~kFlags) != 0 || x.reserved0 != 0 ||
            x.reserved[0] != 0 || x.reserved[1] != 0) {
            abort();
        }
        before = x.from_us;
    }
}

/* The folder through signals.csv (set NULL), or through its frames.csv
 * decoded with set (ecuio_recording_open_frames: ISO-TP on the wire
 * frames, the marks a second is read again from) */
opened open_and_query(const ecuio_signal_set_t* set)
{
    opened o;
    ecuio_recording_t* r = nullptr;
    o.rc = set != nullptr
               ? ecuio_recording_open_frames(folder().c_str(), set, nullptr,
                                             nullptr, &r)
               : ecuio_recording_open(folder().c_str(), nullptr, nullptr, &r);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr) {
        abort();
    }
    o.detail = detail;
    if (o.rc != ECUIO_OK) {
        if (r != nullptr || o.rc != ECUIO_ERR_LOAD_FAILED) {
            abort();
        }
        return o;
    }
    if (r == nullptr || ecuio_recording_get_info(r, &o.info) != ECUIO_OK ||
        ((o.info.flags & ECUIO_RECORDING_INFO_FRAMES) != 0) !=
            (set != nullptr)) {
        abort();
    }
    read_conditions(r, o);
    for (uint32_t k = 0; k < o.info.signal_count; ++k) {
        ecuio_recording_signal_t s{};
        if (ecuio_recording_get_signal(r, k, &s) != ECUIO_OK ||
            memchr(s.name, '\0', sizeof s.name) == nullptr ||
            memchr(s.unit, '\0', sizeof s.unit) == nullptr ||
            s.decimal_places > 3) {
            abort();
        }
        /* A key only where the recording keeps one: terminated, zero
         * after it, and the find gives this signal for it */
        const size_t key_len = strnlen(s.key, sizeof s.key);
        const bool keyed = o.info.format == ECUIO_RECORDING_FORMAT_KEYS ||
                           (o.info.flags & ECUIO_RECORDING_INFO_FRAMES) != 0;
        if (key_len == sizeof s.key || (key_len > 0) != keyed) {
            abort();
        }
        for (size_t z = key_len; z < sizeof s.key; ++z) {
            if (s.key[z] != '\0') {
                abort();
            }
        }
        uint32_t found = UINT32_MAX;
        if (keyed &&
            (ecuio_recording_find_signal(r, s.key, &found) != ECUIO_OK ||
             found != k)) {
            abort();
        }
        /* Labels only on an enum, at most a def's, raw ascending, each
         * terminated and found again by its raw value */
        uint32_t nl = UINT32_MAX;
        static ecuio_enum_entry_t labels[ECUIO_ENUM_MAX_ENTRIES + 1];
        if (ecuio_recording_get_labels(r, k, labels, ECUIO_ENUM_MAX_ENTRIES + 1,
                                       &nl) != ECUIO_OK ||
            nl > ECUIO_ENUM_MAX_ENTRIES ||
            (nl > 0 && (s.flags & ECUIO_RECORDING_SIGNAL_ENUM) == 0) ||
            (s.flags & ECUIO_RECORDING_SIGNAL_ENUM &&
             s.flags & ECUIO_RECORDING_SIGNAL_TEXT)) {
            abort();
        }
        for (uint32_t i = 0; i < nl; ++i) {
            const void* end = memchr(labels[i].label, '\0', sizeof labels[i].label);
            if (end == nullptr || end == labels[i].label ||
                (i > 0 && labels[i].raw <= labels[i - 1].raw)) {
                abort();
            }
            const char* found = ecuio_recording_enum_label(r, k, labels[i].raw);
            if (found == nullptr || strcmp(found, labels[i].label) != 0) {
                abort();
            }
        }
        if (s.samples == 0) {
            if (s.first_us != ECUIO_RECORDING_NONE) {
                abort();
            }
            continue;
        }
        if (s.first_us > s.last_us || s.last_us > o.info.duration_us) {
            abort();
        }
        /* One column over everything holds every sample, from the
         * summaries or the raw rows alike */
        ecuio_recording_column_t col{};
        const uint64_t t1 = s.last_us + 1;
        if (ecuio_recording_query(r, &k, 1, 0, t1, &col, 1) != ECUIO_OK ||
            col.samples != (s.samples > UINT32_MAX ? UINT32_MAX : s.samples)) {
            abort();
        }
        const bool text = (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        if (!text && !std::isnan(s.lowest) &&
            (col.lowest != s.lowest || col.highest != s.highest)) {
            abort();
        }
        /* Narrow columns over the first second agree with the wide one
         * on the count there */
        ecuio_recording_column_t cols[16];
        if (ecuio_recording_query(r, &k, 1, s.first_us, s.first_us + 1000, cols,
                                  16) != ECUIO_OK ||
            cols[0].samples == 0) {
            abort();
        }
        /* Half-second columns over a short recording read every second
         * of it again: the same count and range as the open found */
        if (t1 <= 64000000) {
            const auto n = static_cast<uint32_t>((t1 + 499999) / 500000);
            std::vector<ecuio_recording_column_t> half(n);
            if (ecuio_recording_query(r, &k, 1, 0, n * 500000ull, half.data(),
                                      n) != ECUIO_OK) {
                abort();
            }
            uint64_t sum = 0;
            double lo = NAN;
            double hi = NAN;
            for (const ecuio_recording_column_t& c : half) {
                sum += c.samples;
                if (c.samples > 0 && !text) {
                    lo = std::isnan(lo) || c.lowest < lo ? c.lowest : lo;
                    hi = std::isnan(hi) || c.highest > hi ? c.highest : hi;
                }
            }
            if (sum != s.samples ||
                (!text && sum > 0 && (lo != s.lowest || hi != s.highest))) {
                abort();
            }
        }
        char buf[64];
        uint64_t at = 0;
        if (ecuio_recording_text_at(r, k, s.last_us, buf, sizeof buf, &at) !=
                ECUIO_OK ||
            at > s.last_us || at < s.first_us) {
            abort();
        }
        if (!text) {
            ecuio_recording_find_t f{};
            f.kind = ECUIO_RECORDING_FIND_THRESHOLD;
            f.signal = k;
            f.direction = ECUIO_RECORDING_FIND_RISING;
            f.level = s.highest;
            f.hysteresis = 0.0;
            if (std::isinf(f.level)) {
                f.level = 0.0;
            }
            uint64_t found = 0;
            if (ecuio_recording_find(r, &f, 0, &found) != ECUIO_OK ||
                (found != ECUIO_RECORDING_NONE &&
                 (found < s.first_us || found > s.last_us))) {
                abort();
            }
        }
        whole_stats(r, k, s);
        steps_through(r, k, s);
    }
    aids(r, o.info);
    for (uint32_t m = 0; m < o.info.marker_count; ++m) {
        ecuio_recording_marker_t mk{};
        if (ecuio_recording_get_marker(r, m, &mk) != ECUIO_OK ||
            mk.label_utf8 == nullptr ||
            strlen(mk.label_utf8) != mk.label_length) {
            abort();
        }
    }
    ecuio_recording_close(r);
    return o;
}

} // namespace
