// File/layer: tests/fake_diagnostics.h | test-only, C++17, Qt-free,
//             header-only
// Testing: UNIT: DiagnosticsTest (test_obd2); CoreTest (test_core); HARNESS:
//          J2534AdoptionTest (test_j2534_stream); J2534HealthTest
//          (test_j2534_health); Elm327SessionTest (test_elm327); Cross-arch

#ifndef ECUIO_TESTS_FAKE_DIAGNOSTICS_H
#define ECUIO_TESTS_FAKE_DIAGNOSTICS_H

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace fake_diagnostics {

/* The items the fake's report takes: readiness, this drive cycle's
 * (PID 41), the three lists, the freeze frame's bitmap and code, and its
 * four values (04 05 0C 0D) */
constexpr uint32_t kItems = 11;

/* The obd2 set's def reading Service 01 PID pid */
inline uint32_t obd2_def_id(uint8_t pid)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].mux_key == (0x4100u | pid)) {
            return set->defs[i].id;
        }
    }
    ADD_FAILURE() << "no obd2 def for PID " << int{pid};
    return 0;
}

/* The freeze-frame value of def id, or NaN */
inline double freeze_value(const ecuio_channel_diagnostics_t& d, uint32_t id)
{
    for (uint32_t i = 0; i < d.freeze_count; ++i) {
        if (d.freeze_values[i].def_id == id) {
            return d.freeze_values[i].value;
        }
    }
    return std::nan("");
}

/* What a clear read on its way (the engine's and the vehicle's speeds),
 * which a clear's own test checks: not the ECU's report */
constexpr uint32_t kClearReadings = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;

/* The fake's whole report before a clear: the MIL on, three stored
 * codes, three pending, one permanent, every monitor complete, enabled
 * and complete this drive cycle too, and a freeze frame stored by P0301
 * at 3000 rpm */
inline void expect_report(const ecuio_channel_diagnostics_t& d)
{
    EXPECT_EQ(d.flags & ~kClearReadings,
              ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
                  ECUIO_DIAG_CYCLE | ECUIO_DIAG_MIL_ON |
                  ECUIO_DIAG_FREEZE_FRAME);
    EXPECT_EQ(d.readiness, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(d.cycle, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(d.dtc_count, 3u);
    ASSERT_EQ(d.monitor_count, 8u);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].flags, ECUIO_MONITOR_COMPLETE |
                                           ECUIO_MONITOR_CYCLE_ENABLED |
                                           ECUIO_MONITOR_CYCLE_COMPLETE)
            << "monitor " << int{d.monitors[i].id};
    }
    const std::vector<std::vector<uint16_t>> kLists = {
        {0x0301, 0x0420, 0xC100}, {0x0171, 0x4035, 0x9001}, {0x0420}};
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        EXPECT_EQ(d.lists[l].outcome, ECUIO_DIAG_ANSWERED) << "list " << l;
        ASSERT_EQ(d.lists[l].held, kLists[l].size()) << "list " << l;
        EXPECT_EQ(d.lists[l].reported, kLists[l].size()) << "list " << l;
        for (size_t i = 0; i < kLists[l].size(); ++i) {
            EXPECT_EQ(d.lists[l].codes[i], kLists[l][i]) << l << " " << i;
        }
    }
    EXPECT_EQ(d.freeze, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(d.freeze_dtc, 0x0301u);
    ASSERT_EQ(d.freeze_count, 4u);
    EXPECT_NEAR(freeze_value(d, obd2_def_id(0x04)), 0xB3 * 100.0 / 255, 1e-9);
    EXPECT_EQ(freeze_value(d, obd2_def_id(0x05)), 90.0);
    EXPECT_EQ(freeze_value(d, obd2_def_id(0x0C)), 3000.0)
        << "not the live 2000 rpm: the freeze frame's own";
    EXPECT_EQ(freeze_value(d, obd2_def_id(0x0D)), 80.0);
}

/* The fake's report read again after its clear (ADR-0038): the MIL off,
 * no stored or pending codes, P0420 still permanent, no freeze frame,
 * and byte C's monitors incomplete again, byte B's complete, since the
 * clear and this drive cycle alike, every one enabled */
inline void expect_cleared_report(const ecuio_channel_diagnostics_t& d)
{
    EXPECT_EQ(d.flags & ~kClearReadings,
              ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
                  ECUIO_DIAG_CYCLE);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.dtc_count, 0u);
    ASSERT_EQ(d.monitor_count, 8u);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        const uint8_t done =
            d.monitors[i].id < 8
                ? ECUIO_MONITOR_COMPLETE | ECUIO_MONITOR_CYCLE_COMPLETE
                : 0u;
        EXPECT_EQ(d.monitors[i].flags, ECUIO_MONITOR_CYCLE_ENABLED | done)
            << "monitor " << int{d.monitors[i].id};
    }
    for (uint32_t l : {ECUIO_DIAG_STORED, ECUIO_DIAG_PENDING}) {
        EXPECT_EQ(d.lists[l].outcome, ECUIO_DIAG_ANSWERED) << "list " << l;
        EXPECT_EQ(d.lists[l].reported, 0u) << "list " << l;
        EXPECT_EQ(d.lists[l].held, 0u) << "list " << l;
    }
    ASSERT_EQ(d.lists[ECUIO_DIAG_PERMANENT].held, 1u) << "permanent stays";
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].codes[0], 0x0420u);
    EXPECT_EQ(d.freeze_dtc, 0u);
    EXPECT_EQ(d.freeze_count, 0u);
}

} // namespace fake_diagnostics

#endif /* ECUIO_TESTS_FAKE_DIAGNOSTICS_H */
