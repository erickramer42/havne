// File/layer: tests/bench_car_set.h | bench_car's generator, Qt-free, C++17
// Testing: none (a measurement's input, not in ctest); bench_car checks the
//          loaded set holds the counts asked
//
// A format 2 signal file for a car-sized set with no car: made-up
// modules and DIDs, signals dealt round-robin over the modules, every
// tenth one computed (so the computed path is in the loader's figures).

#ifndef ECUIO_TESTS_BENCH_CAR_SET_H
#define ECUIO_TESTS_BENCH_CAR_SET_H

#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bench_car_set {

/* Numbers a module may ask, kept under ECUIO_MODULE_MAX_REQUESTS */
constexpr unsigned kMostPerModule = 1000;

/* The modules a file of n signals needs when wanted is too few to stay
 * within a module's requests (nine of ten signals are numbers) */
inline unsigned modules_for(unsigned n, unsigned wanted)
{
    const unsigned least = (n + kMostPerModule - 1) / kMostPerModule;
    return std::min<unsigned>(std::max(wanted, least), ECUIO_MAX_CHANNELS);
}

inline bool computed(unsigned k)
{
    return k % 10 == 9;
}

inline std::string key_of(unsigned k, unsigned modules)
{
    char key[32];
    std::snprintf(key, sizeof key, "m%u_s%u", k % modules, k / modules);
    return key;
}

/* Signal k's module is k % modules; its DID is its place in the module.
 * Module 0 is the engine ECU on 7E8 and 7E0, the rest are 0x520 + n
 * and 0x500 + n. */
inline std::string file(unsigned n, unsigned wanted_modules)
{
    const unsigned modules = modules_for(n, wanted_modules);
    std::string s =
        R"({"havne_signals":2,"name":"Bench car","source":{"name":"made up"},"ecus":[)";
    for (unsigned m = 0; m < modules; ++m) {
        char ecu[80];
        const unsigned rx = m == 0 ? 0x7E8u : 0x520u + m;
        const unsigned tx = m == 0 ? 0x7E0u : 0x500u + m;
        std::snprintf(ecu, sizeof ecu, R"(%s{"id":"m%u","rx":"%03X","tx":"%03X"})",
                      m > 0 ? "," : "", m, rx, tx);
        s += ecu;
    }
    s += R"(],"signals":[)";
    s.reserve(s.size() + n * size_t{150});
    for (unsigned k = 0; k < n; ++k) {
        char sig[320];
        const std::string key = key_of(k, modules);
        if (computed(k)) {
            const std::string prev = key_of(k - 1, modules);
            std::snprintf(sig, sizeof sig,
                          R"(%s{"key":"%s","name":"M%u C%u","kind":"computed","expr":"%s * 2"})",
                          k > 0 ? "," : "", key.c_str(), k % modules,
                          k / modules, prev.c_str());
        } else {
            std::snprintf(
                sig, sizeof sig,
                R"(%s{"key":"%s","name":"M%u S%u","ecu":"m%u","read":{"service":"22","number":"%04X"},"field":{"byte":0,"bits":16,"order":"be"},"scale":0.5,"unit":"u"})",
                k > 0 ? "," : "", key.c_str(), k % modules, k / modules,
                k % modules, 0x1000u + k / modules);
        }
        s += sig;
    }
    return s + "]}";
}

} // namespace bench_car_set

#endif // ECUIO_TESTS_BENCH_CAR_SET_H
