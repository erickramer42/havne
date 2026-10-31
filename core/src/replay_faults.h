// File/layer: core/src/replay_faults.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: CoreTest.ReplayFaults* (the query's grammar and its
//          refusals, the generator's determinism, each kind's effect and
//          rate); FUZZ: fuzz_replay_file (a ?faults= query from the input)
//
// Fault injection on a replay (ADR-0041): what ?faults= and ?seed= ask
// for, and what each frame gets. Pure: the replay worker hands over each
// frame and applies the verdict, so every rule is testable without a
// file or a clock.

#ifndef ECUIO_REPLAY_FAULTS_H
#define ECUIO_REPLAY_FAULTS_H

#include "ecuio/core.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

/* The kinds, in the order each frame meets them (ADR-0041 section 1) */
enum replay_fault_kind : uint8_t {
    REPLAY_FAULT_STALL = 0,
    REPLAY_FAULT_DROP,
    REPLAY_FAULT_OVERFLOW,
    REPLAY_FAULT_CORRUPT,
    REPLAY_FAULT_TRUNCATE,
    REPLAY_FAULT_KINDS
};

constexpr uint32_t REPLAY_FAULT_RATE_MAX = 100000; /* 100 %, in thousandths */
constexpr uint32_t REPLAY_FAULT_STALL_MS_MAX = 10000;

struct replay_faults {
    bool any = false;
    /* each kind's chance per frame in thousandths of a percent; 0 = off */
    uint32_t rate[REPLAY_FAULT_KINDS] = {};
    uint32_t stall_ms = 0;
    uint32_t seed = 1;
};

/* What a frame gets */
struct replay_fault_verdict {
    uint32_t stall_ms = 0; /* before the frame */
    bool drop = false;     /* not delivered (drop or overflow) */
    bool overflow = false; /* not delivered; the next one flagged */
    bool corrupt = false;
    uint16_t corrupt_bit = 0; /* the bit flipped, within the data */
    bool truncate = false;
    uint16_t truncate_to = 0; /* the length kept, less than the data's */
};

/* xorshift32, never zero from a nonzero seed: the same sequence on every
 * platform */
inline uint32_t replay_fault_next(uint32_t& state)
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

namespace replay_faults_detail {

inline bool digits(const char* s, size_t n, uint32_t max, uint32_t* out)
{
    if (n == 0 || n > 10) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    }
    if (v > max) {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

/* "2", "0.5", "100", "12.125": more than 0, at most 100, up to three
 * decimals; in thousandths of a percent */
inline bool percent(const char* s, size_t n, uint32_t* out)
{
    const char* dot = static_cast<const char*>(std::memchr(s, '.', n));
    const size_t whole_n = dot != nullptr ? static_cast<size_t>(dot - s) : n;
    uint32_t whole = 0;
    if (!digits(s, whole_n, 100, &whole)) {
        return false;
    }
    uint32_t frac = 0;
    if (dot != nullptr) {
        const size_t frac_n = n - whole_n - 1;
        if (frac_n == 0 || frac_n > 3 || !digits(dot + 1, frac_n, 999, &frac)) {
            return false;
        }
        for (size_t i = frac_n; i < 3; ++i) {
            frac *= 10;
        }
    }
    const uint32_t v = whole * 1000 + frac;
    if (v == 0 || v > REPLAY_FAULT_RATE_MAX) {
        return false;
    }
    *out = v;
    return true;
}

inline bool kind_of(const char* s, size_t n, replay_fault_kind* out)
{
    static const char* const names[REPLAY_FAULT_KINDS] = {
        "stall", "drop", "overflow", "corrupt", "truncate"};
    for (uint8_t k = 0; k < REPLAY_FAULT_KINDS; ++k) {
        if (std::strlen(names[k]) == n && std::memcmp(names[k], s, n) == 0) {
            *out = static_cast<replay_fault_kind>(k);
            return true;
        }
    }
    return false;
}

} // namespace replay_faults_detail

/* ?faults=: "<kind>:<p>[,...]", stall as "stall:<p>:<ms>"; each kind
 * once. False, *out unchanged, for anything else. */
inline bool replay_parse_faults(const char* s, size_t n, replay_faults* out)
{
    using namespace replay_faults_detail;
    replay_faults f = *out;
    bool seen[REPLAY_FAULT_KINDS] = {};
    size_t at = 0;
    if (n == 0) {
        return false;
    }
    while (at <= n) {
        const char* comma =
            static_cast<const char*>(std::memchr(s + at, ',', n - at));
        const size_t end =
            comma != nullptr ? static_cast<size_t>(comma - s) : n;
        const char* item = s + at;
        const size_t len = end - at;
        const char* c1 = static_cast<const char*>(std::memchr(item, ':', len));
        if (c1 == nullptr) {
            return false;
        }
        replay_fault_kind k;
        if (!kind_of(item, static_cast<size_t>(c1 - item), &k) || seen[k]) {
            return false;
        }
        seen[k] = true;
        const char* rest = c1 + 1;
        size_t rest_n = len - static_cast<size_t>(rest - item);
        const char* c2 =
            static_cast<const char*>(std::memchr(rest, ':', rest_n));
        if ((k == REPLAY_FAULT_STALL) != (c2 != nullptr)) {
            return false; /* a stall has its length; nothing else does */
        }
        if (c2 != nullptr) {
            uint32_t ms = 0;
            const size_t ms_n = rest_n - static_cast<size_t>(c2 - rest) - 1;
            if (!digits(c2 + 1, ms_n, REPLAY_FAULT_STALL_MS_MAX, &ms) ||
                ms == 0) {
                return false;
            }
            f.stall_ms = ms;
            rest_n = static_cast<size_t>(c2 - rest);
        }
        if (!percent(rest, rest_n, &f.rate[k])) {
            return false;
        }
        if (comma == nullptr) {
            break;
        }
        at = end + 1;
        if (at == n) {
            return false; /* a trailing comma */
        }
    }
    f.any = true;
    *out = f;
    return true;
}

/* ?seed=: 1 to 4294967295 */
inline bool replay_parse_seed(const char* s, size_t n, uint32_t* out)
{
    uint32_t v = 0;
    if (!replay_faults_detail::digits(s, n, UINT32_MAX, &v) || v == 0) {
        return false;
    }
    *out = v;
    return true;
}

/* The draws for a frame of data_len bytes: one per listed kind in the
 * table's order, then the corruption's bit or the truncation's length.
 * A frame dropped or lost to an overflow draws nothing more after it. */
inline replay_fault_verdict
replay_fault_draw(const replay_faults& f, uint32_t& state, uint16_t data_len)
{
    replay_fault_verdict v;
    const auto hit = [&](replay_fault_kind k) {
        return f.rate[k] != 0 &&
               replay_fault_next(state) % REPLAY_FAULT_RATE_MAX < f.rate[k];
    };
    if (hit(REPLAY_FAULT_STALL)) {
        v.stall_ms = f.stall_ms;
    }
    if (hit(REPLAY_FAULT_DROP)) {
        v.drop = true;
        return v;
    }
    if (hit(REPLAY_FAULT_OVERFLOW)) {
        v.drop = true;
        v.overflow = true;
        return v;
    }
    if (hit(REPLAY_FAULT_CORRUPT) && data_len > 0) {
        v.corrupt = true;
        v.corrupt_bit =
            static_cast<uint16_t>(replay_fault_next(state) % (data_len * 8u));
    }
    if (hit(REPLAY_FAULT_TRUNCATE) && data_len > 0) {
        v.truncate = true;
        v.truncate_to =
            static_cast<uint16_t>(replay_fault_next(state) % data_len);
    }
    return v;
}

#endif /* ECUIO_REPLAY_FAULTS_H */
