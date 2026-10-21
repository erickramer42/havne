// File/layer: tests/test_obd2.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + scheduler — no transport)
// Testing: UNIT — set validation, wire-byte decode round-trips matching
//          the fake's canned responses, mux gate accept/reject, and the
//          full pid_scheduler surface. End-to-end cadence is HARNESS
//          (stream tests) and BENCH (VX) — not this file.

#include <gtest/gtest.h>

#include "qttune/signals.h"
#include "qttune/signal_sets.h"
#include "../core/src/pid_scheduler.h"
#include "qttune/core.h"

#include <cstring>

namespace {

/* Wire payload as the bridge hands it to the decoder (header stripped):
 * [0x41][PID][data...] — header stripped; the adapter owns ISO-TP
 * framing, so there is no PCI byte */
qttune_frame_t make_obd2_frame(const uint8_t* payload, uint8_t len)
{
    qttune_frame_t f{};
    f.arbitration_id = OBD2_ECU_ARB_ID;
    memcpy(f.data, payload, len);
    f.data_length = len;
    return f;
}

const QttuneSignalDef* find_def(const QttuneSignalSet* set, uint16_t id)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id) return &set->defs[i];
    }
    return nullptr;
}

} // namespace

/* ---------- set integrity ---------- */

TEST(Obd2SetTest, SetIsValidAndComplete) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(qttune_validate_signal_set(set), 0);
    EXPECT_EQ(set->count, 10u);
}

TEST(Obd2SetTest, PidBandDistinctFromMockIds) {
    /* Guards the 7000-band choice against accidental reuse if the mock
     * set ever grows. Cheap pin, drift-proof. */
    const QttuneSignalSet* obd2 = qttune_obd2_signal_set();
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const uint16_t id = obd2->defs[i].id;
        EXPECT_GE(id, 7001u);
        EXPECT_LE(id, 7010u);
    }
}

TEST(Obd2SetTest, DeclaresSingleCanChannel) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->channel_count, 1u);
    ASSERT_NE(set->channels, nullptr);
    EXPECT_EQ(set->channels[0].rx_id, OBD2_ECU_ARB_ID);
    EXPECT_EQ(set->channels[0].tx_id, OBD2_TESTER_ARB_ID);
}

TEST(Obd2SetTest, RejectsDuplicateAndIllegalChannelDecls) {
    QttuneSignalDef defs[1] = {};
    defs[0].id = 1;
    defs[0].type = QTTUNE_SIGNAL_TYPE_UINT8;
    defs[0].start_byte = 2;
    defs[0].num_bytes = 1;
    defs[0].bit_length = 8;
    defs[0].mux_offset = 0;
    defs[0].mux_length = 2;
    defs[0].mux_key = 0x4104u;
    defs[0].message_id = 0x7E8u;

    const QttuneChannelDecl dup[] = { {0x7E8u, 0x7E0u}, {0x7E8u, 0x7E2u} };
    const QttuneSignalSet s_dup = { 2u, 1u, defs, 2u, dup };
    EXPECT_NE(qttune_validate_signal_set(&s_dup), 0);

    const QttuneChannelDecl zero_rx[] = { {0u, 0x7E0u} };
    const QttuneSignalSet s_zero = { 2u, 1u, defs, 1u, zero_rx };
    EXPECT_NE(qttune_validate_signal_set(&s_zero), 0);

    const QttuneChannelDecl legal[] = { {0x7E8u, 0x7E0u} };
    const QttuneSignalSet s_ok = { 2u, 1u, defs, 1u, legal };
    EXPECT_EQ(qttune_validate_signal_set(&s_ok), 0);
}

/* ---------- decode round-trips (canned wire bytes) ---------- */

TEST(Obd2DecodeTest, RpmBigEndianScaled) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_RPM_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x0C, 0x1F, 0x40};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_EQ(qttune_def_matches_frame(d, &f), 0);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_FLOAT_EQ(v, 2000.0f);   /* 0x1F40 * 0.25 */
}

TEST(Obd2DecodeTest, CoolantOffsetApplied) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_COOLANT_TEMP_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x05, 0x3C};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_FLOAT_EQ(v, 20.0f);    /* 60 - 40 */
}

TEST(Obd2DecodeTest, BatteryVoltageMillivoltScale) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_BATTERY_V_ID);
    ASSERT_NE(d, nullptr);
    /* 0x3630 = 13872 raw -> 13.872 V at 0.001 scale. Realistic
     * running-system voltage, and the byte pair is chosen so the
     * expected value is DERIVED from it, not asserted from memory. */
    const uint8_t wire[] = {0x41, 0x42, 0x36, 0x30};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_NEAR(v, 13.872f, 0.001f);   /* 0x3630 * 0.001 */
}

TEST(Obd2DecodeTest, EngineLoadPercentScale) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_ENGINE_LOAD_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x04, 25};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_NEAR(v, 25.0f * 100.0f / 255.0f, 0.01f);
}

TEST(Obd2DecodeTest, TemperatureIsUnsignedMinusFortyOffset)
{
    /* J1979 temps are UNSIGNED A - 40. Decoded as INT8, 0xF0 would read
     * -56 C; correct is 240 - 40 = +200 C, and 0x00 is the -40 C
     * sentinel. Both ends pinned. */
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_IAT_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t lo[] = {0x41, 0x0F, 0x00};
    qttune_frame_t f = make_obd2_frame(lo, sizeof lo);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_FLOAT_EQ(v, -40.0f);
    const uint8_t hi[] = {0x41, 0x0F, 0xF0};
    f = make_obd2_frame(hi, sizeof hi);
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_FLOAT_EQ(v, 200.0f);
}

TEST(Obd2DecodeTest, MafCentigramScale) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_MAF_ID);
    ASSERT_NE(d, nullptr);
    /* 0x0064 = 100 raw * 0.01 = 1.00 g/s — pins the /100 (centi-grams/s)
     * scale through the generic scale+offset path, with no decoder
     * special case. */
    const uint8_t wire[] = {0x41, 0x10, 0x00, 0x64};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_EQ(qttune_def_matches_frame(d, &f), 0);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_NEAR(v, 1.00f, 0.0001f);
}

TEST(Obd2DecodeTest, OilTempMatchesCoolantShape) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* d = find_def(set, OBD2_SIG_OIL_TEMP_ID);
    ASSERT_NE(d, nullptr);
    /* 0x78 = 120 raw - 40 = 80 degC, the fake's canned default. */
    const uint8_t wire[] = {0x41, 0x5C, 0x78};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_EQ(qttune_def_matches_frame(d, &f), 0);
    float v = 0.0f;
    ASSERT_EQ(qttune_decode_signal(d, f.data, f.data_length, &v), 0);
    EXPECT_FLOAT_EQ(v, 80.0f);
}

/* ---------- mux gate ---------- */

TEST(Obd2MuxTest, WrongPidRejectedByMatchAndDecode) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* rpm = find_def(set, OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    /* A PID 0x0D response whose data bytes would vacuously decode as
     * RPM if the mux gate were absent — the Blocker-1 scenario. */
    const uint8_t wire[] = {0x41, 0x0D, 0x1F, 0x40};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    EXPECT_NE(qttune_def_matches_frame(rpm, &f), 0);
    float v = 0.0f;
    EXPECT_NE(qttune_decode_signal(rpm, f.data, f.data_length, &v), 0);
}

TEST(Obd2MuxTest, WrongArbIdRejected) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* rpm = find_def(set, OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    const uint8_t wire[] = {0x41, 0x0C, 0x1F, 0x40};
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    f.arbitration_id = 0x0C0;   /* mock telemetry channel */
    EXPECT_NE(qttune_def_matches_frame(rpm, &f), 0);
}

TEST(Obd2MuxTest, ShortFrameRejectedWithoutOverread) {
    const QttuneSignalSet* set = qttune_obd2_signal_set();
    const QttuneSignalDef* rpm = find_def(set, OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    const uint8_t wire[] = {0x41};   /* truncated before PID */
    qttune_frame_t f = make_obd2_frame(wire, sizeof wire);
    EXPECT_NE(qttune_def_matches_frame(rpm, &f), 0);
    float v = 0.0f;
    EXPECT_NE(qttune_decode_signal(rpm, f.data, f.data_length, &v), 0);
}

/* ---------- scheduler ---------- */

TEST(PidSchedulerTest, EnrollsFromObd2SetInDefOrder) {
    qttune_pid_scheduler s;
    ASSERT_TRUE(qttune_sched_from_set(&s, qttune_obd2_signal_set()));
    EXPECT_EQ(s.count, 10u);
    const uint8_t expect[] = {0x04, 0x05, 0x0C, 0x0D,
                               0x0F, 0x11, 0x2F, 0x42, 0x10, 0x5C};
    for (uint32_t i = 0; i < s.count; ++i) {
        EXPECT_EQ(s.pids[i], expect[i]) << "index " << i;
    }
}

TEST(PidSchedulerTest, MockAndNullSetsDoNotEnroll) {
    qttune_pid_scheduler s;
    EXPECT_FALSE(qttune_sched_from_set(&s, qttune_mock_signal_set()));
    EXPECT_EQ(s.count, 0u);
    EXPECT_FALSE(qttune_sched_from_set(&s, nullptr));
}

TEST(PidSchedulerTest, RotationWrapsDeterministically) {
    qttune_pid_scheduler s;
    ASSERT_TRUE(qttune_sched_from_set(&s, qttune_obd2_signal_set()));
    uint8_t last_of_cycle = 0, first_of_next = 0;
    /* Consume one FULL cycle. The loop variable holds the LAST PID
     * handed out (index count-1), not the first — the off-by-one
     * that made the first draft of this test fail against a correct
     * scheduler. */
    for (uint32_t i = 0; i < s.count; ++i) {
        (void)qttune_sched_next(&s, &last_of_cycle);
    }
    /* The next call must wrap to the set's first PID. */
    (void)qttune_sched_next(&s, &first_of_next);
    EXPECT_EQ(last_of_cycle, 0x5Cu);    /* 10th of 10 */
    EXPECT_EQ(first_of_next, 0x04u);    /* wrapped to 1st */
    EXPECT_EQ(s.cursor, 1u);            /* advanced past the wrap */
}

TEST(PidSchedulerTest, EmptySchedulerNextFails) {
    qttune_pid_scheduler s;
    qttune_sched_init(&s);
    uint8_t pid = 0xFF;
    EXPECT_FALSE(qttune_sched_next(&s, &pid));
}

TEST(PidSchedulerTest, DedupesSharedPidAndCapsCapacity) {
    /* Synthetic set: two defs sharing PID 0x0C, plus 16 distinct
     * others — dedupe collapses to 1, capacity stops at 16 total. */
    QttuneSignalDef defs[18] = {};
    for (int i = 0; i < 18; ++i) {
        defs[i].id = static_cast<uint16_t>(100 + i);
        defs[i].type = QTTUNE_SIGNAL_TYPE_UINT8;
        defs[i].start_byte = 3;
        defs[i].num_bytes = 1;
        defs[i].bit_length = 8;
        defs[i].mux_offset = 0;
        defs[i].mux_length = 2;
        defs[i].mux_key = 0x4100u |
            ((i == 1) ? 0x0Cu : static_cast<uint32_t>(i + 1));
        defs[i].message_id = OBD2_ECU_ARB_ID;
    }
    const QttuneSignalSet set = {1u, 18u, defs};
    qttune_pid_scheduler s;
    ASSERT_TRUE(qttune_sched_from_set(&s, &set));
    EXPECT_EQ(s.count, QTTUNE_SCHED_MAX_PIDS);   /* 18 defs -> 16 PIDs */
    /* The dup (index 1, PID 0x0C) did not double-enroll: */
    uint8_t hits = 0;
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.pids[i] == 0x0C) ++hits;
    }
    EXPECT_EQ(hits, 1);
}
