// File/layer: core/src/recording_stretch.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: CoreTest.ReplayStretch* (replay_transport.cpp: the rows a
//          stretch plays, their stamps, a loop, the refusals);
//          SimEcuTest.TheUrisKeysAndRefusals and
//          SimRecordingTest.AStretchAnswersFromItsOwnRows (the simulated
//          ECU's recording)
//
// A stretch of a recording (ADR-0023's appendix "a stretch replayed"):
// ?from= and ?to= in seconds of the recording's host_us, the time the
// viewer, havne-cli view and havne-cli info show, so a stretch read off
// any of them replays as written. The stretch is the rows from the first
// at or after from up to the first past to. Shared by replay://
// (replay_transport.cpp) and the simulated ECU's recording (sim.cpp,
// sim_ecu.h), so both cut a recording the same way.

#ifndef ECUIO_RECORDING_STRETCH_H
#define ECUIO_RECORDING_STRETCH_H

#include "uri_query.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ecuio_stretch {

constexpr uint64_t kOpen = UINT64_MAX; /* no ?to=: to the end */
/* The most seconds a key takes (about 31 years), so the microseconds
 * can't overflow */
constexpr uint64_t kSecondsMax = 1000000000;

constexpr const char* kValueWhy =
    "?from= and ?to= take the recording's seconds, as the viewer and "
    "havne-cli view show them, with at most three decimals";
constexpr const char* kOrderWhy = "?to= must come after ?from=";

struct stretch {
    uint64_t from_us = 0;
    uint64_t to_us = kOpen;
    bool given() const { return from_us != 0 || to_us != kOpen; }
};

/* "12", "12.5", "0.250": seconds, no sign, no locale */
inline bool parse_seconds(const char* v, size_t len, uint64_t* us)
{
    uint64_t whole = 0, frac = 0;
    size_t i = 0, int_digits = 0, frac_digits = 0;
    for (; i < len && v[i] >= '0' && v[i] <= '9'; ++i, ++int_digits) {
        whole = whole * 10 + static_cast<uint64_t>(v[i] - '0');
        if (whole > kSecondsMax) {
            return false;
        }
    }
    if (i < len && v[i] == '.') {
        for (++i; i < len && v[i] >= '0' && v[i] <= '9'; ++i) {
            if (++frac_digits > 3) {
                return false;
            }
            frac = frac * 10 + static_cast<uint64_t>(v[i] - '0');
        }
        if (frac_digits == 0) {
            return false;
        }
    }
    if (i != len || int_digits == 0) {
        return false;
    }
    for (size_t k = frac_digits; k < 3; ++k) {
        frac *= 10;
    }
    *us = whole * 1000000 + frac * 1000;
    return true;
}

/* Takes p into *s when it's from or to (duplicates: the last wins);
 * false when its value isn't seconds */
inline bool take(const ecuio_uri::param& p, stretch* s)
{
    if (p.is("from")) {
        return parse_seconds(p.val, p.val_len, &s->from_us);
    }
    if (p.is("to")) {
        return parse_seconds(p.val, p.val_len, &s->to_us);
    }
    return true;
}

/* Once every key is taken: to after from */
inline bool ordered(const stretch& s)
{
    return s.to_us > s.from_us;
}

/* Follows a recording's rows in file order and says which the stretch
 * plays: none before the first at or after from, none from the first
 * past to on */
struct cursor {
    explicit cursor(const stretch& s) : s_(s) {}
    bool plays(uint64_t host_us)
    {
        if (ended_) {
            return false;
        }
        if (!started_) {
            if (host_us < s_.from_us) {
                return false;
            }
            started_ = true;
        }
        if (host_us > s_.to_us) {
            ended_ = true;
            return false;
        }
        return true;
    }

private:
    stretch s_;
    bool started_ = false;
    bool ended_ = false;
};

/* Seconds to the ms with no trailing zeros: "12", "12.5", "0.481" */
inline void seconds_text(uint64_t us, char* out, size_t cap)
{
    const uint64_t ms = us / 1000;
    int n = std::snprintf(out, cap, "%llu.%03llu",
                          static_cast<unsigned long long>(ms / 1000),
                          static_cast<unsigned long long>(ms % 1000));
    if (n <= 0 || static_cast<size_t>(n) >= cap) {
        return;
    }
    while (out[n - 1] == '0') {
        out[--n] = '\0';
    }
    if (out[n - 1] == '.') {
        out[n - 1] = '\0';
    }
}

/* What a stretch that plays no row says: its bounds, and the times the
 * recording's rows run between (first_us to last_us) */
inline void describe_empty(const stretch& s, uint64_t first_us,
                           uint64_t last_us, char* out, size_t cap)
{
    char from[24], to[24], first[24], last[24];
    seconds_text(s.from_us, from, sizeof from);
    seconds_text(s.to_us == kOpen ? 0 : s.to_us, to, sizeof to);
    seconds_text(first_us, first, sizeof first);
    seconds_text(last_us, last, sizeof last);
    if (s.to_us == kOpen) {
        std::snprintf(out, cap,
                      "the recording holds no frames from %s s on: its frames "
                      "run from %s s to %s s",
                      from, first, last);
    } else {
        std::snprintf(out, cap,
                      "the recording holds no frames from %s s to %s s: its "
                      "frames run from %s s to %s s",
                      from, to, first, last);
    }
}

} // namespace ecuio_stretch

#endif /* ECUIO_RECORDING_STRETCH_H */
