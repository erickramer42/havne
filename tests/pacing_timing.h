// File/layer: tests/pacing_timing.h | test-only, C++17, Qt-free
// Testing: UNIT: PacingTest (test_pacing)
//
// A simulated module's reply timing, as the simulated ECU's timing knobs
// give it (the sim's delay=, fc=, cf= and seed=): the request to the
// reply's first frame drawn log-normal through a P50 and a P99, the
// tester's Flow Control to the first Consecutive Frame, and the module's
// least time between Consecutive Frames. The draw is the sim's, bit for
// bit: one generator for each module, seeded from the seed and its reply
// ID, so adding or reordering a module moves no other module's draws.
// Until core/src/sim_timing.h holds it (namespace ecuio_sim_timing, the
// same names), this copy stands in.

#ifndef ECUIO_TESTS_PACING_TIMING_H
#define ECUIO_TESTS_PACING_TIMING_H

#include <cmath>
#include <cstdint>
#include <random>

namespace pacing {

// One module's timing: P50 and P99 of the first frame's delay (0 and 0:
// no delay, no draw), Flow Control to the first Consecutive Frame, the
// least time between Consecutive Frames, the generator's seed
struct sim_timing {
    int64_t p50_us = 0;
    int64_t p99_us = 0;
    uint32_t fc_us = 0;
    uint32_t cf_us = 0;
    uint64_t seed = 1;
};

inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = x + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The module's generator: one for each reply ID
inline std::mt19937_64 sim_timing_rng(uint64_t seed, uint32_t reply_id)
{
    return std::mt19937_64(splitmix64(seed ^ reply_id));
}

inline double poly(const double* c, int n, double r)
{
    double v = c[n - 1];
    for (int i = n - 2; i >= 0; --i) {
        v = v * r + c[i];
    }
    return v;
}

// The standard normal's inverse CDF at p in (0, 1): Wichura's AS241
// (PPND16), about 1e-16 relative
inline double inverse_normal(double p)
{
    static const double a[] = {
        3.3871328727963666080e0,  1.3314166789178437745e+2,
        1.9715909503065514427e+3, 1.3731693765509461125e+4,
        4.5921953931549871457e+4, 6.7265770927008700853e+4,
        3.3430575583588128105e+4, 2.5090809287301226727e+3};
    static const double b[] = {1.0,
                               4.2313330701600911252e+1,
                               6.8718700749205790830e+2,
                               5.3941960214247511077e+3,
                               2.1213794301586595867e+4,
                               3.9307895800092710610e+4,
                               2.8729085735721942674e+4,
                               5.2264952788528545610e+3};
    static const double c[] = {
        1.42343711074968357734e0,  4.63033784615654529590e0,
        5.76949722146069140550e0,  3.64784832476320460504e0,
        1.27045825245236838258e0,  2.41780725177450611770e-1,
        2.27238449892691845833e-2, 7.74545014278341407640e-4};
    static const double d[] = {1.0,
                               2.05319162663775882187e0,
                               1.67638483018380384940e0,
                               6.89767334985100004550e-1,
                               1.48103976427480074590e-1,
                               1.51986665636164571966e-2,
                               5.47593808499534494600e-4,
                               1.05075007164441684324e-9};
    static const double e[] = {
        6.65790464350110377720e0,  5.46378491116411436990e0,
        1.78482653991729133580e0,  2.96560571828504891230e-1,
        2.65321895265761230930e-2, 1.24266094738807843860e-3,
        2.71155556874348757815e-5, 2.01033439929228813265e-7};
    static const double f[] = {1.0,
                               5.99832206555887937690e-1,
                               1.36929880922735805310e-1,
                               1.48753612908506148525e-2,
                               7.86869131145613259100e-4,
                               1.84631831751005468180e-5,
                               1.42151175831644588870e-7,
                               2.04426310338993978564e-15};
    const double q = p - 0.5;
    if (std::fabs(q) <= 0.425) {
        const double r = 0.180625 - q * q;
        return q * poly(a, 8, r) / poly(b, 8, r);
    }
    double r = std::sqrt(-std::log(q < 0 ? p : 1.0 - p));
    double v = 0;
    if (r <= 5.0) {
        r -= 1.6;
        v = poly(c, 8, r) / poly(d, 8, r);
    } else {
        r -= 5.0;
        v = poly(e, 8, r) / poly(f, 8, r);
    }
    return q < 0 ? -v : v;
}

// One request's first-frame delay from the module's generator: 0 with no
// P50 set (no draw), the P50 itself when P99 equals it
inline int64_t sim_timing_draw(const sim_timing& t, std::mt19937_64& rng)
{
    if (t.p50_us == 0) {
        return 0;
    }
    const uint64_t x = rng();
    const double u = (static_cast<double>(x >> 11) + 0.5) * 0x1.0p-53;
    const double mu = std::log(static_cast<double>(t.p50_us));
    const double sigma = t.p99_us > t.p50_us
                             ? std::log(static_cast<double>(t.p99_us) /
                                        static_cast<double>(t.p50_us)) /
                                   2.3263478740
                             : 0.0;
    return std::llround(std::exp(mu + sigma * inverse_normal(u)));
}

} // namespace pacing

#endif // ECUIO_TESTS_PACING_TIMING_H
