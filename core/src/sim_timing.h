// File/layer: core/src/sim_timing.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: SimTimingTest, SimAskTest (test_obd2); HARNESS:
//          SimCarBusTest (test_vcan); fuzz_sim
//
// The simulated ECU's reply timing (ADR-0046, ADR-0065 section 10):
// the time from a request to a module's first frame, drawn from a
// log-normal through two points (its median and its 99th percentile)
// on a generator of the module's own; the time from the tester's Flow
// Control to the first Consecutive Frame; and the module's own least gap
// between Consecutive Frames, whatever STmin the tester asks. The URI
// keys delay=, fc=, cf= and seed= read them, each aimable at one module
// by its reply ID as faults= is. No key set is no delay, the tester's
// pace and no draw.
//
// The draw is pinned bit for bit (SimTimingTest's fixed draws), so a
// bench that copies it gives the same numbers: splitmix64 seeds the
// module's std::mt19937_64, each draw takes one 64-bit output as a
// uniform in (0, 1), Wichura's AS241 (PPND16) gives its normal
// quantile, and the delay is llround(exp(mu + sigma z)).

#ifndef ECUIO_SIM_TIMING_H
#define ECUIO_SIM_TIMING_H

#include "sim_faults.h" /* ecuio_sim_fault_detail::hex_digit */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace ecuio_sim_timing {

/* One module's timing: p50_us 0 is no delay and no draw */
struct sim_timing {
    int64_t p50_us = 0;
    int64_t p99_us = 0;
    uint32_t fc_us = 0; /* the tester's Flow Control to the first CF */
    uint32_t cf_us = 0; /* the least gap between CFs; a longer STmin wins */
    uint64_t seed = 1;
};

constexpr int64_t DELAY_MAX_US = 10000000;
/* A draw's ceiling: far past any p99, so only a wide spread's farthest
 * tail meets it, where exp would pass what llround returns */
constexpr double DRAW_MAX_US = 60000000.0;
constexpr uint32_t GAP_MAX_US = 1000000;
/* The 99th percentile of the standard normal: z for p = 0.99 */
constexpr double Z99 = 2.3263478740;

enum class knob : uint8_t { delay, fc, cf };

/* One key's item aimed at one module by its reply ID (delay=900:4000@7EA) */
struct aim {
    uint32_t reply_id = 0;
    knob what = knob::delay;
    int64_t a = 0; /* delay's p50, or fc's or cf's time */
    int64_t b = 0; /* delay's p99 */
};

/* Aims for each key, counted across every time the key comes */
constexpr uint32_t AIMED_MAX = 32;
constexpr uint32_t AIMED_KEYS = 3;

struct timings {
    sim_timing all;
    uint32_t count = 0;
    uint32_t aimed[AIMED_KEYS] = {}; /* each key's aims among items */
    aim items[AIMED_KEYS * AIMED_MAX];
};

/* The timing of the module answering on reply_id: every module's, each
 * item aimed at it replacing that key's, a later aim an earlier one */
inline sim_timing for_node(const timings& t, uint32_t reply_id)
{
    sim_timing out = t.all;
    for (uint32_t i = 0; i < t.count; ++i) {
        const aim& a = t.items[i];
        if (a.reply_id != reply_id) {
            continue;
        }
        if (a.what == knob::delay) {
            out.p50_us = a.a;
            out.p99_us = a.b;
        } else if (a.what == knob::fc) {
            out.fc_us = static_cast<uint32_t>(a.a);
        } else {
            out.cf_us = static_cast<uint32_t>(a.a);
        }
    }
    return out;
}

/* ---- The draw ---------------------------------------------------------- */

inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = x + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* The generator of the module answering on reply_id: its own, so another
 * module added, removed or reordered never moves its draws */
inline std::mt19937_64 sim_timing_rng(uint64_t seed, uint32_t reply_id)
{
    return std::mt19937_64(splitmix64(seed ^ reply_id));
}

namespace detail {

/* PPND16's central region, |q| <= 0.425 for q = p - 0.5 */
inline double ppnd16_central(double q)
{
    const double r = 0.180625 - q * q;
    const double num =
        (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r +
              6.7265770927008700853e+4) *
                 r +
             4.5921953931549871457e+4) *
                r +
            1.3731693765509461125e+4) *
               r +
           1.9715909503065514427e+3) *
              r +
          1.3314166789178437745e+2) *
             r +
         3.3871328727963666080e+0);
    const double den =
        (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r +
              3.9307895800092710610e+4) *
                 r +
             2.1213794301586595867e+4) *
                r +
            5.3941960214247511077e+3) *
               r +
           6.8718700749205790830e+2) *
              r +
          4.2313330701600911252e+1) *
             r +
         1.0);
    return q * num / den;
}

/* PPND16's near tail, r <= 5 for r = sqrt(-log(min(p, 1 - p))) */
inline double ppnd16_near(double r)
{
    r -= 1.6;
    return (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) *
                     r +
                 2.41780725177450611770e-1) *
                    r +
                1.27045825245236838258e+0) *
                   r +
               3.64784832476320460504e+0) *
                  r +
              5.76949722146069140550e+0) *
                 r +
             4.63033784615654529590e+0) *
                r +
            1.42343711074968357734e+0) /
           (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) *
                     r +
                 1.51986665636164571966e-2) *
                    r +
                1.48103976427480074590e-1) *
                   r +
               6.89767334985100004550e-1) *
                  r +
              1.67638483018380384940e+0) *
                 r +
             2.05319162663775882187e+0) *
                r +
            1.0);
}

/* PPND16's far tail, r > 5 */
inline double ppnd16_far(double r)
{
    r -= 5.0;
    return (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) *
                     r +
                 1.24266094738807843860e-3) *
                    r +
                2.65321895265761230930e-2) *
                   r +
               2.96560571828504891230e-1) *
                  r +
              1.78482653991729133580e+0) *
                 r +
             5.46378491116411436990e+0) *
                r +
            6.65790464350110377720e+0) /
           (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) *
                     r +
                 1.84631831751005468180e-5) *
                    r +
                7.86869131145613259100e-4) *
                   r +
               1.48753612908506148525e-2) *
                  r +
              1.36929880922735805310e-1) *
                 r +
             5.99832206555887937690e-1) *
                r +
            1.0);
}

} // namespace detail

/* The standard normal's quantile at p in (0, 1): AS241's PPND16 */
inline double inverse_normal(double p)
{
    const double q = p - 0.5;
    if (std::fabs(q) <= 0.425) {
        return detail::ppnd16_central(q);
    }
    const double r = std::sqrt(-std::log(q < 0 ? p : 1.0 - p));
    const double v = r <= 5.0 ? detail::ppnd16_near(r) : detail::ppnd16_far(r);
    return q < 0 ? -v : v;
}

/* A generator's output as a uniform in (0, 1): its top 52 bits and a
 * half, each sum exact in a double, so neither end is reached (53 bits
 * would round the largest to 1) */
inline double sim_timing_uniform(uint64_t x)
{
    return (static_cast<double>(x >> 12) + 0.5) * 0x1.0p-52;
}

/* The next delay from request to first frame, in microseconds: 0, and
 * nothing drawn, with no delay set; at most DRAW_MAX_US */
inline int64_t sim_timing_draw(const sim_timing& t, std::mt19937_64& rng)
{
    if (t.p50_us <= 0) {
        return 0;
    }
    const double z = inverse_normal(sim_timing_uniform(rng()));
    const double mu = std::log(static_cast<double>(t.p50_us));
    const double sigma = t.p99_us == t.p50_us
                             ? 0.0
                             : std::log(static_cast<double>(t.p99_us) /
                                        static_cast<double>(t.p50_us)) /
                                   Z99;
    const double v = std::exp(mu + sigma * z);
    return static_cast<int64_t>(
        std::llround(v < DRAW_MAX_US ? v : DRAW_MAX_US));
}

/* ---- The grammar ------------------------------------------------------- */

namespace detail {

/* Decimal digits [s, s + n) into *out, at most max; false otherwise */
inline bool decimal(const char* s, size_t n, uint64_t max, uint64_t* out)
{
    uint64_t v = 0;
    if (n == 0 || n > 20) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        const uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (v > (max - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* An item's "@<reply ID>" (11-bit hex) at [s, s + n) into *id */
inline bool reply_id(const char* s, size_t n, uint32_t* id)
{
    if (n < 2 || n > 4 || s[0] != '@') {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 1; i < n; ++i) {
        const int d = ecuio_sim_fault_detail::hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = v * 16 + static_cast<uint32_t>(d);
    }
    *id = v;
    return v <= 0x7FFu;
}

/* One item of what's key: <p50>:<p99> for delay, <us> for fc and cf,
 * then [@<reply ID>]; false for any other shape or a value out of range */
inline bool parse_item(knob what, const char* s, size_t n, aim* out,
                       bool* aimed)
{
    size_t at_sign = 0;
    while (at_sign < n && s[at_sign] != '@') {
        ++at_sign;
    }
    *aimed = at_sign < n;
    if (*aimed && !reply_id(s + at_sign, n - at_sign, &out->reply_id)) {
        return false;
    }
    out->what = what;
    uint64_t a = 0;
    uint64_t b = 0;
    if (what != knob::delay) {
        if (!decimal(s, at_sign, GAP_MAX_US, &a)) {
            return false;
        }
        out->a = static_cast<int64_t>(a);
        return true;
    }
    size_t colon = 0;
    while (colon < at_sign && s[colon] != ':') {
        ++colon;
    }
    if (colon == at_sign ||
        !decimal(s, colon, static_cast<uint64_t>(DELAY_MAX_US), &a) ||
        !decimal(s + colon + 1, at_sign - colon - 1,
                 static_cast<uint64_t>(DELAY_MAX_US), &b) ||
        a == 0 || a > b) {
        return false;
    }
    out->a = static_cast<int64_t>(a);
    out->b = static_cast<int64_t>(b);
    return true;
}

inline const char* name_of(knob what)
{
    return what == knob::delay ? "delay" : what == knob::fc ? "fc" : "cf";
}

inline void refuse_item(knob what, const char* item, size_t len, char* why,
                        size_t why_cap)
{
    const int shown = static_cast<int>(len < 40 ? len : 40);
    if (what == knob::delay) {
        std::snprintf(why, why_cap,
                      "delay: '%.*s' isn't <p50 us>:<p99 us>[@<reply ID>], "
                      "p50 from 1 to p99 and p99 at most %lld, the reply ID "
                      "11-bit hex",
                      shown, item, static_cast<long long>(DELAY_MAX_US));
    } else {
        std::snprintf(why, why_cap,
                      "%s: '%.*s' isn't <us>[@<reply ID>], from 0 to %u, the "
                      "reply ID 11-bit hex",
                      name_of(what), shown, item, GAP_MAX_US);
    }
}

} // namespace detail

/* One of delay=, fc= or cf= (what) into *t: its untargeted item sets
 * every module's (a later one in place of an earlier), each aimed one
 * joins t's aims, at most AIMED_MAX for the key however often it comes;
 * false with why */
inline bool parse_key(knob what, const char* text, size_t len, timings* t,
                      char* why, size_t why_cap)
{
    uint32_t& aimed_here = t->aimed[static_cast<uint8_t>(what)];
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        aim a;
        bool aimed = false;
        if (!detail::parse_item(what, text + at, end - at, &a, &aimed)) {
            detail::refuse_item(what, text + at, end - at, why, why_cap);
            return false;
        }
        if (!aimed) {
            if (what == knob::delay) {
                t->all.p50_us = a.a;
                t->all.p99_us = a.b;
            } else if (what == knob::fc) {
                t->all.fc_us = static_cast<uint32_t>(a.a);
            } else {
                t->all.cf_us = static_cast<uint32_t>(a.a);
            }
        } else if (aimed_here == AIMED_MAX) {
            std::snprintf(why, why_cap,
                          "%s: more than %u items aimed at a module",
                          detail::name_of(what), AIMED_MAX);
            return false;
        } else {
            t->items[t->count++] = a;
            ++aimed_here;
        }
        at = end + 1;
    }
    if (len == 0 || text[len - 1] == ',') {
        std::snprintf(why, why_cap, "%s: names none", detail::name_of(what));
        return false;
    }
    return true;
}

/* seed=<n>: the modules' generators' seed, a decimal uint64 */
inline bool parse_seed(const char* text, size_t len, timings* t, char* why,
                       size_t why_cap)
{
    uint64_t v = 0;
    if (!detail::decimal(text, len, UINT64_MAX, &v)) {
        std::snprintf(why, why_cap,
                      "seed: a decimal number from 0 to 18446744073709551615");
        return false;
    }
    t->all.seed = v;
    return true;
}

/* Whether every aim names a reply ID one of a sim's ECUs answers on
 * (ids, count of them); false with why naming the first that doesn't
 * and the first 8 of ids */
inline bool aims_land(const timings& t, const uint32_t* ids, uint32_t count,
                      char* why, size_t why_cap)
{
    for (uint32_t i = 0; i < t.count; ++i) {
        const aim& a = t.items[i];
        bool lands = false;
        for (uint32_t k = 0; k < count && !lands; ++k) {
            lands = ids[k] == a.reply_id;
        }
        if (lands) {
            continue;
        }
        size_t n = 0;
        const auto add = [&](int wrote) {
            if (wrote > 0) {
                n += static_cast<size_t>(wrote);
            }
            if (n >= why_cap) {
                n = why_cap - 1;
            }
        };
        add(std::snprintf(why, why_cap,
                          "%s: an item @%X aims at a module the simulated car "
                          "hasn't; it answers on",
                          detail::name_of(a.what), a.reply_id));
        for (uint32_t k = 0; k < count && k < 8; ++k) {
            add(std::snprintf(why + n, why_cap - n, "%s %X", k == 0 ? "" : ",",
                              ids[k]));
        }
        if (count > 8) {
            add(std::snprintf(why + n, why_cap - n, " and %u more", count - 8));
        }
        return false;
    }
    return true;
}

} // namespace ecuio_sim_timing

#endif /* ECUIO_SIM_TIMING_H */
