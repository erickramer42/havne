// File/layer: tests/test_j2534_health_support.h | GTest, HARNESS tier
// Testing: HARNESS: the helpers of test_j2534_health*.cpp (test_j2534_health); Cross-arch

#pragma once

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/profile.h"

#ifdef _WIN32
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fake_diagnostics.h"
#include "j2534_health_fixture.h"

namespace {

using j2534_health::J2534HealthTest;

/* The fake's writes over the next ms milliseconds. */
inline uint32_t writes_during(fake_control::Page* page, int ms)
{
    const uint32_t before = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return fake_control::writes(page) - before;
}

/* Polls the error state until it reads want or budget_ms passes; le and
 * ec keep the last reading. */
inline bool wait_for_state(ecuio_session_t* s, int32_t want, int budget_ms,
                    int32_t& le, uint32_t& ec)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budget_ms);
    for (;;) {
        (void)ecuio_session_get_error_state(s, &le, &ec);
        if (le == want) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/* A request's send is stamped once its write has come back across the
 * surrogate's pipe, and the fake times its reply from the write itself,
 * so a reply can read up to that round trip under the fake's delay: a
 * floor on the latency is the delay less this. A read that answers at
 * its message has nothing else to add (3.7 ms seen for a 4 ms reply
 * through the x86 surrogate) */
constexpr uint32_t kWriteReturnUs = 1000;

} // namespace

#endif // _WIN32
