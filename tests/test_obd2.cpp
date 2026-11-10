// File/layer: tests/test_obd2.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include <gtest/gtest.h>

#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "../core/src/request_engine.h"
#include "../core/src/diagnostics.h"
#include "../core/src/obd2_rates.h"
#include "../core/src/obd2_reply.h"
#include "../core/src/pid_support.h"
#include "../core/src/sim_dids.h"
#include "../core/src/sim_ecu.h"
#include "../core/src/uds_generic.h"
#include "../core/src/uri_query.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/sim.h"
#include "fake_diagnostics.h"
#include "fake_ecu.h"
#include "temp_name.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <set>
#include <vector>

namespace {

/* Wire payload as the bridge hands it to the decoder (header stripped):
 * [0x41][PID][data...]: header stripped; the adapter owns ISO-TP
 * framing, so there is no PCI byte */
ecuio_frame_t make_obd2_frame(const uint8_t* payload, uint8_t len)
{
    ecuio_frame_t f{};
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    memcpy(f.data, payload, len);
    f.data_length = len;
    return f;
}

/* Decode a frame's payload, as the bridge does. */
int decode(const ecuio_signal_def_t* d, const ecuio_frame_t& f, double* v)
{
    return ecuio_decode_signal(d, f.data, f.data_length, v);
}

const ecuio_signal_def_t* find_def(const ecuio_signal_set_t* set, uint16_t id)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id) return &set->defs[i];
    }
    return nullptr;
}

} // namespace

/* ---------- set integrity ---------- */

TEST(Obd2SetTest, SetIsValidAndComplete) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(set->count, 130u) << "every linear field of J1979's table";
}

/* The ten keep 7001-7010, first and in their order; every other def's
 * ID is ECUIO_OBD2_PID_SIG_ID of its PID and its place among the PID's
 * fields (ADR-0039 section 4), clear of the mock's band and uds-generic's. */
TEST(Obd2SetTest, IdsFollowTheRule)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const uint16_t ten[] = {
        ECUIO_OBD2_SIG_ENGINE_LOAD_ID, ECUIO_OBD2_SIG_COOLANT_TEMP_ID,
        ECUIO_OBD2_SIG_RPM_ID,         ECUIO_OBD2_SIG_SPEED_ID,
        ECUIO_OBD2_SIG_IAT_ID,         ECUIO_OBD2_SIG_THROTTLE_ID,
        ECUIO_OBD2_SIG_FUEL_LEVEL_ID,  ECUIO_OBD2_SIG_BATTERY_V_ID,
        ECUIO_OBD2_SIG_MAF_ID,         ECUIO_OBD2_SIG_OIL_TEMP_ID};
    ASSERT_GE(obd2->count, 10u);
    for (uint32_t i = 0; i < 10; ++i) {
        EXPECT_EQ(obd2->defs[i].id, ten[i]) << i;
    }
    unsigned last_pid = 0, field = 0;
    for (uint32_t i = 10; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        SCOPED_TRACE(d.name);
        const unsigned pid = d.mux_key & 0xFFu;
        ASSERT_GE(pid, last_pid) << "the rest in PID order";
        field = (pid == last_pid) ? field + 1 : 0;
        last_pid = pid;
        EXPECT_EQ(d.id, ECUIO_OBD2_PID_SIG_ID(pid, field));
        EXPECT_GE(d.id, 11000u);
        EXPECT_LT(d.id, ECUIO_UDS_GENERIC_SIG_ID_BASE);
    }
}

/* ADR-0039 section 8: the signals a UI plots when a session starts,
 * ranked 1 up with no gaps or repeats, the five engine-state ones
 * first; every other def unranked */
TEST(Obd2SetTest, RanksTheSignalsAUiPlotsFirst)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const uint16_t order[] = {
        ECUIO_OBD2_SIG_RPM_ID,          ECUIO_OBD2_SIG_COOLANT_TEMP_ID,
        ECUIO_OBD2_SIG_IAT_ID,          ECUIO_OBD2_SIG_MAF_ID,
        ECUIO_OBD2_SIG_OIL_TEMP_ID,     ECUIO_OBD2_SIG_SPEED_ID,
        ECUIO_OBD2_SIG_ENGINE_LOAD_ID,  ECUIO_OBD2_PID_SIG_ID(0x0E, 0),
        ECUIO_OBD2_PID_SIG_ID(0x06, 0), ECUIO_OBD2_PID_SIG_ID(0x07, 0),
        ECUIO_OBD2_PID_SIG_ID(0x0B, 0), ECUIO_OBD2_SIG_BATTERY_V_ID,
        ECUIO_OBD2_PID_SIG_ID(0x49, 0), ECUIO_OBD2_PID_SIG_ID(0x44, 0)};
    for (size_t r = 0; r < sizeof order / sizeof order[0]; ++r) {
        const ecuio_signal_def_t* d = find_def(obd2, order[r]);
        ASSERT_NE(d, nullptr) << order[r];
        EXPECT_EQ(d->plot_rank, r + 1) << d->name;
    }
    uint32_t ranked = 0;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        ranked += obd2->defs[i].plot_rank != 0 ? 1u : 0u;
    }
    EXPECT_EQ(ranked, sizeof order / sizeof order[0]) << "the rest unranked";

    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    for (uint32_t i = 0; i < mock->count; ++i) {
        EXPECT_EQ(mock->defs[i].plot_rank, 0u) << "the mock ranks none: a UI plots all";
    }
}

/* ADR-0050: every def placed, a PID's fields together but for 4F's
 * intake pressure, a def of each group where it belongs; the mock's
 * defs where obd2's of the same name are */
TEST(Obd2SetTest, GroupsEachSignalByThePartOfTheCarItReads)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        EXPECT_NE(d.group, ECUIO_SIGNAL_GROUP_OTHER) << d.name;
        EXPECT_STRNE(ecuio_signal_group_name(d.group), "") << d.name;
        const ecuio_signal_def_t* first = find_def(
            obd2,
            static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(d.mux_key & 0xFFu, 0)));
        if (first != nullptr && d.id != ECUIO_OBD2_PID_SIG_ID(0x4F, 3)) {
            EXPECT_EQ(d.group, first->group)
                << d.name << " and " << first->name;
        }
    }
    const struct {
        uint16_t id;
        uint8_t group;
    } placed[] = {
        {ECUIO_OBD2_SIG_RPM_ID, ECUIO_SIGNAL_GROUP_ENGINE},
        {ECUIO_OBD2_SIG_OIL_TEMP_ID, ECUIO_SIGNAL_GROUP_ENGINE},
        {ECUIO_OBD2_PID_SIG_ID(0x64, 2), ECUIO_SIGNAL_GROUP_ENGINE},
        {ECUIO_OBD2_SIG_IAT_ID, ECUIO_SIGNAL_GROUP_AIR},
        {ECUIO_OBD2_PID_SIG_ID(0x49, 0), ECUIO_SIGNAL_GROUP_AIR},
        {ECUIO_OBD2_PID_SIG_ID(0x4F, 3), ECUIO_SIGNAL_GROUP_AIR},
        {ECUIO_OBD2_PID_SIG_ID(0x06, 0), ECUIO_SIGNAL_GROUP_FUEL},
        {ECUIO_OBD2_SIG_FUEL_LEVEL_ID, ECUIO_SIGNAL_GROUP_FUEL},
        {ECUIO_OBD2_PID_SIG_ID(0x14, 1), ECUIO_SIGNAL_GROUP_OXYGEN},
        {ECUIO_OBD2_PID_SIG_ID(0x4F, 0), ECUIO_SIGNAL_GROUP_OXYGEN},
        {ECUIO_OBD2_PID_SIG_ID(0x2C, 0), ECUIO_SIGNAL_GROUP_EMISSIONS},
        {ECUIO_OBD2_PID_SIG_ID(0x3C, 0), ECUIO_SIGNAL_GROUP_EMISSIONS},
        {ECUIO_OBD2_SIG_SPEED_ID, ECUIO_SIGNAL_GROUP_VEHICLE},
        {ECUIO_OBD2_PID_SIG_ID(0xA6, 0), ECUIO_SIGNAL_GROUP_VEHICLE},
        {ECUIO_OBD2_SIG_BATTERY_V_ID, ECUIO_SIGNAL_GROUP_ELECTRICAL},
        {ECUIO_OBD2_PID_SIG_ID(0x21, 0), ECUIO_SIGNAL_GROUP_DIAGNOSTICS},
        {ECUIO_OBD2_PID_SIG_ID(0x4E, 0), ECUIO_SIGNAL_GROUP_DIAGNOSTICS},
    };
    for (const auto& p : placed) {
        const ecuio_signal_def_t* d = find_def(obd2, p.id);
        ASSERT_NE(d, nullptr) << p.id;
        EXPECT_EQ(d->group, p.group) << d->name;
    }

    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    for (uint32_t i = 0; i < mock->count; ++i) {
        const ecuio_signal_def_t& m = mock->defs[i];
        /* obd2 has neither: Lambda reads a wideband, boost is computed */
        uint8_t expected = std::strcmp(m.name, "Boost") == 0
                               ? ECUIO_SIGNAL_GROUP_AIR
                               : ECUIO_SIGNAL_GROUP_OXYGEN;
        for (uint32_t k = 0; k < obd2->count; ++k) {
            if (std::strcmp(obd2->defs[k].name, m.name) == 0) {
                expected = obd2->defs[k].group;
            }
        }
        EXPECT_EQ(m.group, expected) << m.name;
    }
}

TEST(Obd2SetTest, DeclaresSingleCanChannel) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->channel_count, 1u);
    ASSERT_NE(set->channels, nullptr);
    EXPECT_EQ(set->channels[0].rx_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(set->channels[0].tx_id, ECUIO_OBD2_TESTER_ARB_ID);
}

TEST(Obd2SetTest, RejectsDuplicateAndIllegalChannelDecls) {
    ecuio_signal_def_t defs[1] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].start_byte = 2;
    defs[0].bit_length = 8;
    defs[0].mux_offset = 0;
    defs[0].mux_length = 2;
    defs[0].mux_key = 0x4104u;
    defs[0].message_id = 0x7E8u;
    auto set_with = [&defs](const ecuio_channel_decl_t* channels, uint32_t n) {
        ecuio_signal_set_t s = {};
        s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        s.count = 1u;
        s.defs = defs;
        s.channel_count = n;
        s.channels = channels;
        return s;
    };

    const ecuio_channel_decl_t dup[] = { {0x7E8u, 0x7E0u}, {0x7E8u, 0x7E2u} };
    const ecuio_signal_set_t s_dup = set_with(dup, 2u);
    EXPECT_EQ(ecuio_validate_signal_set(&s_dup, nullptr), ECUIO_VALIDATE_ERR_DUPLICATE_RX);

    const ecuio_channel_decl_t zero_rx[] = { {0u, 0x7E0u} };
    const ecuio_signal_set_t s_zero = set_with(zero_rx, 1u);
    EXPECT_EQ(ecuio_validate_signal_set(&s_zero, nullptr), ECUIO_VALIDATE_ERR_CHANNEL_ID);

    const ecuio_channel_decl_t legal[] = { {0x7E8u, 0x7E0u} };
    const ecuio_signal_set_t s_ok = set_with(legal, 1u);
    EXPECT_EQ(ecuio_validate_signal_set(&s_ok, nullptr), ECUIO_VALIDATE_OK);
}

/* ---------- decode round-trips (canned wire bytes) ---------- */

TEST(Obd2DecodeTest, RpmBigEndianScaled) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x0C, 0x1F, 0x40};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_TRUE(ecuio_def_applies_to_frame(d, &f));
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 2000.0);   /* 0x1F40 * 0.25 */
}

TEST(Obd2DecodeTest, CoolantOffsetApplied) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_COOLANT_TEMP_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x05, 0x3C};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 20.0);    /* 60 - 40 */
}

TEST(Obd2DecodeTest, BatteryVoltageMillivoltScale) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_BATTERY_V_ID);
    ASSERT_NE(d, nullptr);
    /* 0x3630 = 13872 raw -> 13.872 V at 0.001 scale. Realistic
     * running-system voltage, and the byte pair is chosen so the
     * expected value is DERIVED from it, not asserted from memory. */
    const uint8_t wire[] = {0x41, 0x42, 0x36, 0x30};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_NEAR(v, 13.872, 0.001);   /* 0x3630 * 0.001 */
}

TEST(Obd2DecodeTest, EngineLoadPercentScale) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x04, 25};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_NEAR(v, 25.0 * 100.0 / 255.0, 0.01);
}

TEST(Obd2DecodeTest, TemperatureIsUnsignedMinusFortyOffset)
{
    /* J1979 temps are UNSIGNED A - 40. Decoded as INT8, 0xF0 would read
     * -56 C; correct is 240 - 40 = +200 C, and 0x00 is the -40 C
     * sentinel. Both ends pinned. */
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_IAT_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t lo[] = {0x41, 0x0F, 0x00};
    ecuio_frame_t f = make_obd2_frame(lo, sizeof lo);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, -40.0);
    const uint8_t hi[] = {0x41, 0x0F, 0xF0};
    f = make_obd2_frame(hi, sizeof hi);
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 200.0);
}

TEST(Obd2DecodeTest, MafCentigramScale) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_MAF_ID);
    ASSERT_NE(d, nullptr);
    /* 0x0064 = 100 raw * 0.01 = 1.00 g/s: pins the /100 (centi-grams/s)
     * scale through the generic scale+offset path, with no decoder
     * special case. */
    const uint8_t wire[] = {0x41, 0x10, 0x00, 0x64};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_TRUE(ecuio_def_applies_to_frame(d, &f));
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_NEAR(v, 1.00, 0.0001);
}

TEST(Obd2DecodeTest, OilTempMatchesCoolantShape) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_OIL_TEMP_ID);
    ASSERT_NE(d, nullptr);
    /* 0x78 = 120 raw - 40 = 80 degC, the fake's canned default. */
    const uint8_t wire[] = {0x41, 0x5C, 0x78};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_TRUE(ecuio_def_applies_to_frame(d, &f));
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 80.0);
}

/* ---------- mux gate ---------- */

TEST(Obd2MuxTest, WrongPidRejectedByMatchAndDecode) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* rpm = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    /* A PID 0x0D response whose data bytes would vacuously decode as
     * RPM if the mux gate were absent: the Blocker-1 scenario. */
    const uint8_t wire[] = {0x41, 0x0D, 0x1F, 0x40};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    EXPECT_FALSE(ecuio_def_applies_to_frame(rpm, &f));
    double v = 0.0;
    EXPECT_EQ(decode(rpm, f, &v),
              ECUIO_DECODE_ERR_MUX_MISMATCH);
}

TEST(Obd2MuxTest, WrongArbIdRejected) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* rpm = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    const uint8_t wire[] = {0x41, 0x0C, 0x1F, 0x40};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    f.arbitration_id = 0x0C0;   /* mock telemetry channel */
    EXPECT_FALSE(ecuio_def_applies_to_frame(rpm, &f));
}

TEST(Obd2MuxTest, ShortFrameRejectedWithoutOverread) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* rpm = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    const uint8_t wire[] = {0x41};   /* truncated before PID */
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    EXPECT_FALSE(ecuio_def_applies_to_frame(rpm, &f));
    double v = 0.0;
    EXPECT_EQ(decode(rpm, f, &v),
              ECUIO_DECODE_ERR_PAST_PAYLOAD);
}

/* ---------- request engine ---------- */

namespace {

/* A def read from a reply that opens with key (mux_length bytes) at
 * offset 0, on message_id. Enough for derivation; not a full def. */
ecuio_signal_def_t keyed_def(uint32_t key, uint8_t length,
                             uint32_t message_id = ECUIO_OBD2_ECU_ARB_ID,
                             uint8_t offset = 0)
{
    ecuio_signal_def_t d = {};
    d.message_id = message_id;
    d.mux_offset = offset;
    d.mux_length = length;
    d.mux_key = key;
    return d;
}

ecuio_signal_set_t set_of(const ecuio_signal_def_t* defs, uint32_t n)
{
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = n;
    s.defs = defs;
    return s;
}

/* One channel holding the given Service 01 PIDs, in order. */
void one_channel_of(ecuio_request_engine* e,
                    std::initializer_list<uint8_t> pids)
{
    ecuio_engine_init(e, 1);
    for (uint8_t pid : pids) {
        const uint8_t req[2] = {0x01, pid};
        ASSERT_EQ(ecuio_engine_add(e, 0, req, 2),
                  ecuio_engine_add_result::added);
    }
}

constexpr int64_t kMs = 1000;   /* the engine counts microseconds */

} // namespace

TEST(RequestEngineTest, DefsDeriveTheRequestTheirReplyAnswers) {
    uint8_t req[3] = {};
    uint8_t len = 0;
    ecuio_signal_def_t d = keyed_def(0x410Cu, 2);
    ASSERT_TRUE(ecuio_engine_request_for_def(&d, req, &len));
    EXPECT_EQ(len, 2u);
    EXPECT_EQ(req[0], 0x01u);
    EXPECT_EQ(req[1], 0x0Cu);

    d = keyed_def(0x621E1Cu, 3);
    ASSERT_TRUE(ecuio_engine_request_for_def(&d, req, &len));
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(req[0], 0x22u);
    EXPECT_EQ(req[1], 0x1Eu);
    EXPECT_EQ(req[2], 0x1Cu);

    /* None of these asks for anything, though each still decodes. */
    const ecuio_signal_def_t kNot[] = {
        keyed_def(0x410Cu, 2, ECUIO_OBD2_ECU_ARB_ID, 1),   /* not at offset 0 */
        keyed_def(0x620Cu, 2),      /* a 62 key needs its 2-byte DID */
        keyed_def(0x410C00u, 3),    /* a 41 key is the PID alone */
        keyed_def(0x4902u, 2),      /* Service 09: read once, never rotated */
        keyed_def(0x0Cu, 1),
        keyed_def(0x410Cu, 0),      /* no mux */
    };
    for (const auto& n : kNot) {
        uint8_t untouched[3] = {0xEE, 0xEE, 0xEE};
        uint8_t ulen = 0xEE;
        EXPECT_FALSE(ecuio_engine_request_for_def(&n, untouched, &ulen))
            << std::hex << n.mux_key;
        EXPECT_EQ(untouched[0], 0xEEu);
        EXPECT_EQ(ulen, 0xEEu);
    }
}

TEST(RequestEngineTest, Obd2SetEnrolsItsPidsInDefOrder) {
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1,
                                    nullptr));
    /* The ten first, then each other PID once, in PID order: a PID with
     * several fields costs one request */
    const uint8_t ten[] = {0x04, 0x05, 0x0C, 0x0D, 0x0F,
                           0x11, 0x2F, 0x42, 0x10, 0x5C};
    ASSERT_EQ(e.count, 92u) << "J1979's linear PIDs";
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].len, 2u) << "index " << i;
        EXPECT_EQ(e.requests[i].bytes[0], 0x01u) << "index " << i;
        EXPECT_EQ(e.requests[i].channel, 0u) << "index " << i;
        if (i < sizeof(ten)) {
            EXPECT_EQ(e.requests[i].bytes[1], ten[i]) << "index " << i;
        } else if (i > sizeof(ten)) {
            EXPECT_GT(e.requests[i].bytes[1], e.requests[i - 1].bytes[1])
                << "index " << i;
        }
    }
}

TEST(RequestEngineTest, MockAndNullSetsEnrolNothing) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    EXPECT_TRUE(ecuio_engine_derive(&e, ecuio_mock_signal_set(), nullptr, 0,
                                    nullptr));
    EXPECT_TRUE(ecuio_engine_derive(&e, nullptr, nullptr, 0, nullptr));
    EXPECT_EQ(e.count, 0u);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), -1);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), INT64_MAX);
}

/* One request per reply and channel: two defs read from one reply cost
 * one request, the same PID asked of two ECUs costs two, and a def whose
 * message_id is no channel's goes to channel 0. Service 01 and 0x22 mix
 * in one set. */
TEST(RequestEngineTest, RequestsAreKeptOncePerChannel) {
    const ecuio_signal_def_t defs[] = {
        keyed_def(0x4114u, 2, 0x7E8u),     /* O2 sensor voltage */
        keyed_def(0x4114u, 2, 0x7E8u),     /* its trim, same reply */
        keyed_def(0x4114u, 2, 0x7E9u),     /* the same PID, the TCM */
        keyed_def(0x621E1Cu, 3, 0x7E9u),
        keyed_def(0x410Cu, 2, 0x7EAu),     /* no such channel */
    };
    const ecuio_signal_set_t set = set_of(defs, 5);
    const uint32_t rx[2] = {0x7E8u, 0x7E9u};
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, rx, 2, nullptr));
    ASSERT_EQ(e.count, 4u);
    EXPECT_EQ(e.requests[0].channel, 0u);
    EXPECT_EQ(e.requests[1].channel, 1u);
    EXPECT_EQ(e.requests[1].bytes[1], 0x14u);
    EXPECT_EQ(e.requests[2].channel, 1u);
    EXPECT_EQ(e.requests[2].bytes[0], 0x22u);
    EXPECT_EQ(e.requests[3].channel, 0u);
    EXPECT_EQ(e.requests[3].bytes[1], 0x0Cu);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(e.chans[1].requests, 2u);
}

/* Section 3: whatever reaches the table, only reads go out (ADR-0037 adds the
 * trouble codes' and the freeze frame's). Service 0x04 clears the
 * emission DTCs; 0x10, 0x27, 0x2E, 0x31 and 0x11 change a session,
 * unlock, write, run a routine or reset. */
TEST(RequestEngineTest, OnlyReadServicesEnterTheTable) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t kReads[] = {0x01, 0x02, 0x03, 0x07, 0x09, 0x0A, 0x22};
    for (uint8_t sid : kReads) {
        const uint8_t req[3] = {sid, 0x02, 0x00};
        EXPECT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::added) << std::hex << int{sid};
    }
    const uint8_t kOthers[] = {0x04, 0x10, 0x11, 0x14, 0x19, 0x27, 0x28,
                               0x2E, 0x2F, 0x31, 0x34, 0x3B, 0x3E, 0x85};
    for (uint8_t sid : kOthers) {
        const uint8_t req[3] = {sid, 0x01, 0x00};
        EXPECT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::refused) << std::hex << int{sid};
    }
    const uint8_t pid[4] = {0x01, 0x0C, 0x00, 0x00};
    EXPECT_EQ(ecuio_engine_add(&e, 0, pid, 1), ecuio_engine_add_result::refused);
    EXPECT_EQ(ecuio_engine_add(&e, 0, pid, 4), ecuio_engine_add_result::refused);
    EXPECT_EQ(ecuio_engine_add(&e, 1, pid, 2), ecuio_engine_add_result::refused)
        << "a channel past the table";
    EXPECT_EQ(e.count, sizeof kReads);
}

TEST(RequestEngineTest, TableHoldsTheLimitAndNamesTheFirstDefPastIt) {
    static ecuio_signal_def_t defs[ECUIO_PROFILE_MAX_REQUESTS + 2];
    for (uint32_t i = 0; i < ECUIO_PROFILE_MAX_REQUESTS + 2; ++i) {
        /* Distinct DIDs, with one repeat at index 1. */
        defs[i] = keyed_def(0x620000u | ((i == 1) ? 0u : i), 3);
    }
    ecuio_signal_set_t set = set_of(defs, ECUIO_PROFILE_MAX_REQUESTS + 1);
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    EXPECT_TRUE(ecuio_engine_derive(&e, &set, nullptr, 0, nullptr))
        << "the repeat fits a full table";
    EXPECT_EQ(e.count, ECUIO_PROFILE_MAX_REQUESTS);

    set.count = ECUIO_PROFILE_MAX_REQUESTS + 2;
    ecuio_engine_init(&e, 1);
    uint32_t overflow = UINT32_MAX;
    EXPECT_FALSE(ecuio_engine_derive(&e, &set, nullptr, 0, &overflow));
    EXPECT_EQ(e.count, ECUIO_PROFILE_MAX_REQUESTS);
    EXPECT_EQ(overflow, ECUIO_PROFILE_MAX_REQUESTS + 1);
}

/* A channel's rotation runs in table order and wraps, and each channel
 * walks its own. */
TEST(RequestEngineTest, EachChannelRotatesItsOwnRequests) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t a[2] = {0x01, 0x0A}, b[2] = {0x01, 0x0B},
                  c[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, c, 2), ecuio_engine_add_result::added);

    int64_t now = 0;
    uint8_t sent0[4] = {}, sent1[4] = {};
    for (int i = 0; i < 4; ++i) {
        const int32_t r0 = ecuio_engine_due(&e, 0, now);
        const int32_t r1 = ecuio_engine_due(&e, 1, now);
        ASSERT_GE(r0, 0);
        ASSERT_GE(r1, 0);
        sent0[i] = e.requests[r0].bytes[1];
        sent1[i] = e.requests[r1].bytes[1];
        ecuio_engine_sent(&e, 0, r0, true, now);
        ecuio_engine_sent(&e, 1, r1, true, now);
        now += ECUIO_ENGINE_WAIT_US;   /* both wait out */
        EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        EXPECT_TRUE(ecuio_engine_expire(&e, 1, now));
    }
    const uint8_t want0[4] = {0x0A, 0x0C, 0x0A, 0x0C};
    const uint8_t want1[4] = {0x0B, 0x0B, 0x0B, 0x0B};
    EXPECT_EQ(memcmp(sent0, want0, 4), 0);
    EXPECT_EQ(memcmp(sent1, want1, 4), 0);
}

/* The next request waits for the reply and for the floor: a reply in
 * 4 ms still leaves 16 ms of it. */
TEST(RequestEngineTest, NextRequestWaitsForTheReplyAndTheFloor) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    const int32_t r = ecuio_engine_due(&e, 0, 0);
    ASSERT_EQ(r, 0);
    ecuio_engine_sent(&e, 0, r, true, 0);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 30 * kMs), -1) << "one outstanding";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), ECUIO_ENGINE_WAIT_US);

    const uint8_t reply[4] = {0x41, 0x0C, 0x1F, 0x40};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, reply, 4, 4 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 4 * kMs), -1) << "inside the floor";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 4 * kMs),
              ECUIO_ENGINE_GAP_US - 4 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US), 1);
}

TEST(RequestEngineTest, UnansweredRequestTimesOutAtTheWait) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C}));
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US - 1));
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US))
        << "a request times out once";
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), 0) << "wrapped";

    /* A reply that comes after its request timed out answers nothing. */
    const uint8_t late[3] = {0x41, 0x0C, 0x00};
    ecuio_request_engine idle;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&idle, {0x0C}));
    EXPECT_EQ(ecuio_engine_on_frame(&idle, 0, late, 3, 0, nullptr),
              ecuio_reply::none);
}

/* Positive: the service ID + 0x40 and the whole key. Negative: 7F and
 * the request's service ID. Pending holds the request for P2*. */
TEST(RequestEngineTest, RepliesCorrelateByServiceAndKey) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);

    const uint8_t other_did[3] = {0x62, 0x1E, 0x1D};
    const uint8_t other_service[3] = {0x41, 0x1E, 0x1C};
    const uint8_t neg_other[3] = {0x7F, 0x01, 0x31};
    const uint8_t short_frame[2] = {0x62, 0x1E};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other_did, 3, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other_service, 3, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, neg_other, 3, 0, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, short_frame, 2, 0, nullptr),
              ecuio_reply::none);

    const uint8_t pending[3] = {0x7F, 0x22, 0x78};
    uint8_t nrc = 0;
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, pending, 3, 40 * kMs, &nrc),
              ecuio_reply::pending);
    EXPECT_EQ(nrc, 0x78u);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US))
        << "pending extends the wait";
    EXPECT_FALSE(ecuio_engine_expire(
        &e, 0, 40 * kMs + ECUIO_ENGINE_PENDING_WAIT_US - 1));

    const uint8_t positive[4] = {0x62, 0x1E, 0x1C, 0x2A};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, positive, 4, 300 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].outstanding, -1);

    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 300 * kMs), true,
                      300 * kMs);
    const uint8_t negative[3] = {0x7F, 0x22, 0x31};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, negative, 3, 310 * kMs, &nrc),
              ecuio_reply::negative);
    EXPECT_EQ(nrc, 0x31u);
    EXPECT_EQ(e.chans[0].outstanding, -1);
}

/* A write that didn't go out leaves nothing to wait for; the floor
 * still spaces the next attempt. */
TEST(RequestEngineTest, UnsentRequestLeavesNothingOutstanding) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), false, 0);
    EXPECT_EQ(e.chans[0].outstanding, -1);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US - 1), -1);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US), 1);
}

/* The next event is the nearest of every channel's: a wait ending, or
 * an idle channel's floor; a channel with no requests has none. */
TEST(RequestEngineTest, NextEventIsTheNearestAcrossChannels) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 3);
    const uint8_t a[2] = {0x01, 0x0A}, b[2] = {0x01, 0x0B};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 2), ecuio_engine_add_result::added);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), 0) << "both due now";

    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);
    ecuio_engine_sent(&e, 1, ecuio_engine_due(&e, 1, 0), false, 0);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 5 * kMs),
              ECUIO_ENGINE_GAP_US - 5 * kMs) << "channel 1's floor";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, ECUIO_ENGINE_GAP_US + 5 * kMs), 0)
        << "channel 1 due";
}

/* ---------- rate classes (ADR-0039 section 3) ---------- */

namespace {

/* The PIDs channel 0 sends over n turns, each request waiting out its
 * wait (no reply needed to move the rotation on). */
std::vector<uint8_t> rotation_of(ecuio_request_engine* e, int n)
{
    std::vector<uint8_t> sent;
    int64_t now = 0;
    for (int k = 0; k < n; ++k) {
        const int32_t r = ecuio_engine_due(e, 0, now);
        if (r < 0) {
            break;
        }
        sent.push_back(e->requests[r].bytes[1]);
        ecuio_engine_sent(e, 0, r, true, now);
        now += ECUIO_ENGINE_WAIT_US;
        (void)ecuio_engine_expire(e, 0, now);
    }
    return sent;
}

} // namespace

/* The obd2 set's requests take the classes obd2_rates.h gives their
 * PIDs, and the rotation sends two fast ones for each slow one: the
 * first turns by hand, then over three times the fast class's length,
 * every third request slow, each class in set order and wrapping on
 * its own. */
TEST(RequestEngineTest, TheObd2SetRunsTwoFastRequestsToOneSlow)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ASSERT_TRUE(
        ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1, nullptr));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].slow != 0,
                  !ecuio_obd2_pid_fast(e.requests[i].bytes[1]))
            << "PID " << int(e.requests[i].bytes[1]);
    }
    const std::vector<uint8_t> first = {0x04, 0x0C, 0x05, 0x0D, 0x11, 0x0F,
                                        0x10, 0x06, 0x2F, 0x08, 0x0A, 0x42};
    std::vector<uint8_t> fast, slow;
    for (uint32_t i = 0; i < e.count; ++i) {
        (e.requests[i].slow ? slow : fast).push_back(e.requests[i].bytes[1]);
    }
    ASSERT_FALSE(fast.empty());
    ASSERT_FALSE(slow.empty());
    const std::vector<uint8_t> sent =
        rotation_of(&e, static_cast<int>(3 * fast.size()));
    ASSERT_EQ(sent.size(), 3 * fast.size());
    EXPECT_EQ(std::vector<uint8_t>(sent.begin(), sent.begin() + 12), first);
    size_t f = 0, s = 0;
    for (size_t k = 0; k < sent.size(); ++k) {
        if (k % 3 == 2) {
            EXPECT_EQ(sent[k], slow[s++ % slow.size()]) << "turn " << k;
        } else {
            EXPECT_EQ(sent[k], fast[f++ % fast.size()]) << "turn " << k;
        }
    }
}

/* A class with nothing to send gives its turns to the other: a table of
 * one class, or one whose other class was dropped, rotates in table
 * order. A table never classified is all fast. */
TEST(RequestEngineTest, AClassWithNothingToSendGivesItsTurnsToTheOther)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x05, 0x0F, 0x42}));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    EXPECT_EQ(rotation_of(&e, 7),
              (std::vector<uint8_t>{0x05, 0x0F, 0x42, 0x05, 0x0F, 0x42, 0x05}))
        << "slow alone";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x04, 0x0C, 0x0D}));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    EXPECT_EQ(rotation_of(&e, 7),
              (std::vector<uint8_t>{0x04, 0x0C, 0x0D, 0x04, 0x0C, 0x0D, 0x04}))
        << "fast alone";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x04, 0x05, 0x0C}));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    e.requests[1].dropped = ECUIO_DROP_BITMAP;
    EXPECT_EQ(rotation_of(&e, 5),
              (std::vector<uint8_t>{0x04, 0x0C, 0x04, 0x0C, 0x04}))
        << "the slow class dropped";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x05, 0x04}));
    EXPECT_EQ(rotation_of(&e, 4),
              (std::vector<uint8_t>{0x05, 0x04, 0x05, 0x04}))
        << "never classified";
}

/* Only Service 01 requests take a class: a DID stays fast whatever the
 * predicate says of its high byte. */
TEST(RequestEngineTest, OnlyServiceOneRequestsAreClassified)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did[3] = {0x22, 0x05, 0x01}, pid[2] = {0x01, 0x05};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    EXPECT_EQ(e.requests[0].slow, 0u);
    EXPECT_EQ(e.requests[1].slow, 1u);
}

/* ---------- the probe (ADR-0015 section 4) ---------- */

namespace {

/* Sends what channel c has due at now, which must be the walk's request
 * for the bitmap at pid. */
void send_bitmap_request(ecuio_request_engine* e, uint32_t c, uint8_t pid,
                         int64_t now)
{
    const int32_t r = ecuio_engine_due(e, c, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    ASSERT_EQ(q.len, 2u);
    ASSERT_EQ(q.bytes[0], 0x01u);
    ASSERT_EQ(q.bytes[1], pid);
    ecuio_engine_sent(e, c, r, true, now);
}

/* The ECU's bitmap reply at pid: 41 <pid>, then bits as four bytes,
 * most significant first, as the wire carries them. */
ecuio_reply answer_bitmap(ecuio_request_engine* e, uint32_t c, uint8_t pid,
                          uint32_t bits, int64_t now)
{
    const uint8_t reply[6] = {0x41, pid,
                              static_cast<uint8_t>(bits >> 24),
                              static_cast<uint8_t>(bits >> 16),
                              static_cast<uint8_t>(bits >> 8),
                              static_cast<uint8_t>(bits)};
    return ecuio_engine_on_frame(e, c, reply, sizeof(reply), now, nullptr);
}

/* Sends channel c's next rotation request at now and returns its key's
 * first byte (the PID, or a DID's high byte); 0xEE when nothing is due
 * or the walk is. */
uint8_t send_next(ecuio_request_engine* e, uint32_t c, int64_t now)
{
    const int32_t r = ecuio_engine_due(e, c, now);
    if (r < 0) {
        return 0xEE;
    }
    ecuio_engine_sent(e, c, r, true, now);
    return e->requests[r].bytes[1];
}

ecuio_reply answer_negative(ecuio_request_engine* e, uint32_t c,
                            uint8_t service, uint8_t nrc, int64_t now)
{
    const uint8_t reply[3] = {0x7F, service, nrc};
    return ecuio_engine_on_frame(e, c, reply, sizeof(reply), now, nullptr);
}

} // namespace

/* Before its rotation a channel walks the supported-PID chain, a bitmap
 * at a time, following each one's last bit, and drops the PIDs the
 * answers mark unsupported. The first bitmap is the worked example on
 * Wikipedia's "OBD-II PIDs" page (Service 01 PID 00): BE 1F A8 13 lists
 * PIDs 01, 03 to 07, 0C to 11, 13, 15, 1C, 1F and 20, so 0x0B is clear.
 * The 0x40 bitmap's last bit is clear, so the chain ends there, and 0x61,
 * past it, is unsupported too. */
TEST(RequestEngineTest, ProbeWalksTheBitmapsBeforeTheRotation) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C, 0x42, 0x61}));
    ecuio_engine_probe(&e, 0);

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA813u, now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(ecuio_engine_due(&e, 0, now + 4 * kMs), ECUIO_ENGINE_NONE)
        << "the walk keeps the floor";
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x20, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x20, 0x00000001u, now + 4 * kMs),
              ecuio_reply::positive);   /* only the next bitmap */
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x40, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x40, 0x40000000u, now + 4 * kMs),
              ecuio_reply::positive);   /* 0x42, and the chain ends */

    uint8_t sent[3] = {};
    for (uint8_t& pid : sent) {
        now += ECUIO_ENGINE_WAIT_US;
        pid = send_next(&e, 0, now);
        EXPECT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));
    }
    const uint8_t want[3] = {0x0C, 0x42, 0x0C};
    EXPECT_EQ(memcmp(sent, want, sizeof(want)), 0) << "0x0B and 0x61 left";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.ranges, 0x7u);
    const uint8_t listed[] = {0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C,
                              0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x15,
                              0x1C, 0x1F, 0x20};
    for (unsigned pid = 1; pid <= 0x20; ++pid) {
        bool want_set = false;
        for (uint8_t l : listed) {
            want_set = want_set || l == pid;
        }
        EXPECT_EQ(ecuio_engine_pid_bit(s.pid_bitmap, static_cast<uint8_t>(pid)),
                  want_set) << "PID " << std::hex << pid;
    }
    EXPECT_EQ(s.requests, 4u);
    ASSERT_EQ(s.dropped, 2u);
    EXPECT_EQ(s.dropped_list[0].request[1], 0x0Bu);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_BITMAP);
    EXPECT_EQ(s.dropped_list[0].nrc, 0u);
    EXPECT_EQ(s.dropped_list[1].request[0], 0x01u);
    EXPECT_EQ(s.dropped_list[1].request[1], 0x61u);
    EXPECT_EQ(s.dropped_list[1].length, 2u);
}

/* The report's list takes the refused drops before the bitmap's: a car's
 * bitmaps leave out many PIDs, and their drops would otherwise fill the
 * list before a refusal the front ends name. Ten bitmap drops, then a
 * refused DID, then one more refused: both refusals listed first, in set
 * order, then the bitmap drops in theirs; every drop counted. */
TEST(RequestEngineTest, TheReportListsRefusalsBeforeBitmapDrops)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(
        &e, {0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0A, 0x0B, 0x0E, 0x0F}));
    const uint8_t did[3] = {0x22, 0xF1, 0xFE};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    const uint8_t pid[2] = {0x01, 0x11};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(e.count, 12u);
    for (uint32_t i = 0; i < 10; ++i) {
        ecuio_engine_drop(&e, i, ECUIO_DROP_BITMAP, 0);
    }
    ecuio_engine_drop(&e, 10, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    ecuio_engine_drop(&e, 11, ECUIO_DROP_ACCESS, 0x33);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.requests, 12u);
    EXPECT_EQ(s.dropped, 12u);
    EXPECT_EQ(s.dropped_list[0].length, 3u);
    EXPECT_EQ(s.dropped_list[0].request[1], 0xF1u);
    EXPECT_EQ(s.dropped_list[0].request[2], 0xFEu);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_NOT_SUPPORTED);
    EXPECT_EQ(s.dropped_list[0].nrc, 0x31u);
    EXPECT_EQ(s.dropped_list[1].request[1], 0x11u);
    EXPECT_EQ(s.dropped_list[1].reason, ECUIO_DROP_ACCESS);
    const uint8_t bitmap[6] = {0x01, 0x03, 0x04, 0x05, 0x06, 0x07};
    for (uint32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(s.dropped_list[2 + i].request[1], bitmap[i]) << i;
        EXPECT_EQ(s.dropped_list[2 + i].reason, ECUIO_DROP_BITMAP) << i;
    }
}

/* The first vehicle's own bitmaps (docs/bench-sessions/2026-10-22-vehicle.md),
 * read off the bytes by J1979's layout there: of the obd2 set's first
 * ten, every PID is supported but 0x5C, so the walk drops only that one
 * of them, and of the rest exactly those the bitmaps leave out. The 0x40
 * bitmap's last bit says a fourth exists; it's left unanswered here, so
 * PIDs past 0x60 stay. */
TEST(RequestEngineTest, TheFirstVehiclesBitmapsDropOnly0x5C) {
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1,
                                    nullptr));
    ecuio_engine_probe(&e, 0);
    const struct {
        uint8_t pid;
        uint32_t bits;
    } kCar[] = {{0x00, 0xFE7FA813u}, {0x20, 0xA017F011u}, {0x40, 0xFED08C81u}};
    int64_t now = 0;
    for (const auto& b : kCar) {
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, b.pid, now));
        ASSERT_EQ(answer_bitmap(&e, 0, b.pid, b.bits, now + 4 * kMs),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
    }
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x60, now));
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));

    /* Dropped: a PID its answered bitmap marks clear (bit 0x80 >>
     * ((k - 1) % 8) of byte (k - 1) / 8 for PID base + k). The PIDs past
     * 0x60, whose bitmap went unanswered, stay; of the ten, only 0x5C
     * goes, as on the car. */
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const uint8_t pid = e.requests[i].bytes[1];
        bool clear = false;
        for (const auto& b : kCar) {
            const unsigned k = pid - b.pid;
            if (pid > b.pid && k <= 32) {
                clear = ((b.bits >> (32 - k)) & 1u) == 0;
            }
        }
        EXPECT_EQ(e.requests[i].dropped, clear ? ECUIO_DROP_BITMAP : 0u)
            << "PID " << std::hex << int{pid};
        if (i < 10) {
            EXPECT_EQ(clear, pid == 0x5C) << "PID " << std::hex << int{pid};
        }
        dropped += clear ? 1u : 0u;
    }
    EXPECT_GT(dropped, 1u);
    EXPECT_EQ(e.chans[0].requests, e.count - dropped);
}

namespace {

/* Channel 0 of the obd2 set as the setup builds it (the classes and the
 * duplicate pairs), walked against bitmaps answered in turn; the last
 * one's clear last bit ends the walk, unless walk is false. */
void obd2_walked(ecuio_request_engine* e,
                 const std::vector<std::pair<uint8_t, uint32_t>>& car,
                 bool walk = true)
{
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ecuio_engine_init(e, 1);
    ASSERT_TRUE(
        ecuio_engine_derive(e, ecuio_obd2_signal_set(), &rx, 1, nullptr));
    ecuio_engine_classify(e, ecuio_obd2_pid_fast);
    ecuio_engine_set_duplicates(e, &ECUIO_OBD2_DUPLICATE_PIDS[0][0],
                                sizeof(ECUIO_OBD2_DUPLICATE_PIDS) /
                                    sizeof(ECUIO_OBD2_DUPLICATE_PIDS[0]));
    if (!walk) {
        return;
    }
    ecuio_engine_probe(e, 0);
    int64_t now = 0;
    for (const auto& b : car) {
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(e, 0, b.first, now));
        ASSERT_EQ(answer_bitmap(e, 0, b.first, b.second, now + 4 * kMs),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
    }
    ASSERT_FALSE(e->chans[0].walking) << "the last bitmap ends the walk";
}

/* PID pid's request on channel 0: its class (0 fast, 1 slow), or -1
 * when it's dropped or not in the table. */
int class_of(const ecuio_request_engine& e, uint8_t pid)
{
    for (uint32_t i = 0; i < e.count; ++i) {
        const ecuio_request& r = e.requests[i];
        if (r.len == 2 && r.bytes[0] == 0x01 && r.bytes[1] == pid) {
            return r.dropped != 0 ? -1 : r.slow;
        }
    }
    return -1;
}

/* The test car's six bitmaps (docs/bench-sessions/2026-10-25-vehicle.md,
 * the same on 2026-10-31) */
const std::vector<std::pair<uint8_t, uint32_t>> kTestCar = {
    {0x00, 0xFE7FA813u}, {0x20, 0xA017F011u}, {0x40, 0xFED08C81u},
    {0x60, 0x63080001u}, {0x80, 0x0004000Du}, {0xA0, 0x04000000u}};

} // namespace

/* ADR-0039 section 3: the fuel rail's two forms read one value on the test car
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md), so when a car's
 * bitmaps list both, 59 goes slow and 23 stays fast. A car that lists
 * only 59 keeps it fast, and with no walk nothing is judged. */
TEST(RequestEngineTest, ADuplicateFuelRailPressureGoesSlow)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar));
    EXPECT_EQ(class_of(e, 0x23), 0);
    EXPECT_EQ(class_of(e, 0x59), 1) << "both listed: the duplicate is slow";
    EXPECT_EQ(class_of(e, 0x0A), 0) << "the low-pressure side stays fast";

    std::vector<std::pair<uint8_t, uint32_t>> no23 = kTestCar;
    no23[1].second &= ~(0x80000000u >> (0x23 - 0x21));
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, no23));
    EXPECT_EQ(class_of(e, 0x23), -1) << "dropped on its bitmap";
    EXPECT_EQ(class_of(e, 0x59), 0) << "the only rail pressure stays fast";

    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar, false));
    EXPECT_EQ(class_of(e, 0x23), 0);
    EXPECT_EQ(class_of(e, 0x59), 0) << "no walk, nothing judged";
}

/* A bitmap nobody answers ends the walk with nothing judged: every
 * request stays, and the answer coming late changes nothing. */
TEST(RequestEngineTest, SilenceEndsTheWalkAndDropsNothing) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_WAIT_US), 0x0Bu);
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA813u,
                            ECUIO_ENGINE_WAIT_US + 4 * kMs),
              ecuio_reply::none) << "a late bitmap answers nothing";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
    EXPECT_EQ(s.ranges, 0u);
    EXPECT_EQ(s.dropped, 0u);
    EXPECT_GT(s.generation, 0u) << "the walk's end is news";
    EXPECT_EQ(e.chans[0].requests, 2u);
}

/* A bitmap whose wait ends after a reply naming another PID is asked
 * once more: on the car, 01 00 drew 41 1F 00 01, the run time's answer
 * (docs/bench-sessions/2026-10-29-android.md). Asked again, it's
 * answered and the walk goes on; another PID's answer the second time
 * ends the walk as silence does. */
TEST(RequestEngineTest, AnotherPidsAnswerAsksTheBitmapAgainOnce)
{
    const uint8_t run_time[4] = {0x41, 0x1F, 0x00, 0x01};
    {
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
        ecuio_engine_probe(&e, 0);
        int64_t now = 0;
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
        EXPECT_EQ(ecuio_engine_on_frame(&e, 0, run_time, sizeof(run_time),
                                        now + 4 * kMs, nullptr),
                  ecuio_reply::none);
        now += ECUIO_ENGINE_WAIT_US;
        EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now))
            << "the same bitmap again";
        EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA812u, now + 4 * kMs),
                  ecuio_reply::positive);
        EXPECT_EQ(send_next(&e, 0, now + ECUIO_ENGINE_WAIT_US), 0x0Cu)
            << "the walk ended with the chain, and dropped 0x0B";
        ecuio_channel_support_t s;
        ecuio_engine_support(&e, 0, &s);
        EXPECT_EQ(s.ranges, 0x1u);
    }
    {
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
        ecuio_engine_probe(&e, 0);
        int64_t now = 0;
        for (int ask = 0; ask < 2; ++ask) {
            ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
            (void)ecuio_engine_on_frame(&e, 0, run_time, sizeof(run_time),
                                        now + 4 * kMs, nullptr);
            now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        }
        EXPECT_EQ(send_next(&e, 0, now), 0x0Bu) << "the walk ended, once";
        ecuio_channel_support_t s;
        ecuio_engine_support(&e, 0, &s);
        EXPECT_EQ(s.ranges, 0u);
        EXPECT_EQ(s.dropped, 0u);
    }
}

/* With the probe on, a negative reply drops its request when its code
 * can't change within the session: 0x12 and 0x31 (not supported), 0x33
 * and 0x7F (security access, another session: the engine asks for
 * neither). Any other code leaves it in the rotation: 0x22 (conditions
 * not correct), 0x21 (busy), 0x10, 0x13, 0x14. */
TEST(RequestEngineTest, NegativeRepliesDropOnAnswersThatLast) {
    const struct {
        uint8_t nrc;
        uint8_t reason;
    } kCases[] = {{0x12, ECUIO_DROP_NOT_SUPPORTED},
                  {0x31, ECUIO_DROP_NOT_SUPPORTED},
                  {0x33, ECUIO_DROP_ACCESS},
                  {0x7F, ECUIO_DROP_ACCESS},
                  {0x22, 0}, {0x21, 0}, {0x10, 0}, {0x13, 0}, {0x14, 0}};
    for (const auto& k : kCases) {
        ecuio_request_engine e;
        ecuio_engine_init(&e, 1);
        const uint8_t unknown[3] = {0x22, 0x1E, 0x1D};
        const uint8_t known[3] = {0x22, 0x1E, 0x1C};
        ASSERT_EQ(ecuio_engine_add(&e, 0, unknown, 3),
                  ecuio_engine_add_result::added);
        ASSERT_EQ(ecuio_engine_add(&e, 0, known, 3),
                  ecuio_engine_add_result::added);
        ecuio_engine_probe(&e, 0);   /* no Service 01, so no walk */
        ASSERT_EQ(ecuio_engine_due(&e, 0, 0), 0) << std::hex << int{k.nrc};
        ecuio_engine_sent(&e, 0, 0, true, 0);
        EXPECT_EQ(answer_negative(&e, 0, 0x22, k.nrc, 4 * kMs),
                  ecuio_reply::negative);
        EXPECT_EQ(e.requests[0].dropped, k.reason) << std::hex << int{k.nrc};
        EXPECT_EQ(e.requests[0].nrc, (k.reason != 0) ? k.nrc : 0)
            << std::hex << int{k.nrc};
        EXPECT_EQ(e.requests[1].dropped, 0u);
        EXPECT_EQ(e.chans[0].requests, (k.reason != 0) ? 1u : 2u);
    }
}

/* 0x11 says the ECU has no such service: every request of that service
 * on that channel leaves; its other services, and the other channel's
 * requests, stay. */
TEST(RequestEngineTest, ServiceNotSupportedDropsTheServiceOnItsChannel) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t a[3] = {0x22, 0x1E, 0x1C}, b[3] = {0x22, 0x1E, 0x1D},
                  vin[2] = {0x09, 0x02};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, b, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, vin, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, a, 3), ecuio_engine_add_result::added);
    ecuio_engine_probe(&e, 0);
    ASSERT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x11, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(e.requests[1].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(e.requests[1].nrc, 0x11u);
    EXPECT_EQ(e.requests[2].dropped, 0u) << "Service 09 stays";
    EXPECT_EQ(e.requests[3].dropped, 0u) << "the other ECU's stays";
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_EQ(e.chans[1].requests, 1u);
}

/* With the probe off (?probe=0, and ?poll=), no channel walks and every
 * request stays, whatever the ECU answers. */
TEST(RequestEngineTest, WithoutTheProbeNothingIsDropped) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    EXPECT_EQ(send_next(&e, 0, 0), 0x0Bu) << "no walk first";
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x0Cu);
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x11, ECUIO_ENGINE_GAP_US + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[0].dropped, 0u);
    EXPECT_EQ(e.requests[1].dropped, 0u);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(e.chans[0].generation, 0u);
}

/* A negative reply names the service, not the PID, so one that comes
 * after its request timed out would answer the next request: it drops
 * nothing until a positive reply shows the ECU answers the request
 * that's out. Here 0x0B's refusal comes 100 ms late, while 0x0C is
 * out. */
TEST(RequestEngineTest, ALateNegativeReplyDropsNothing) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    /* PIDs 01 to 1F supported, and the chain ends */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0xFFFFFFFEu, 4 * kMs),
              ecuio_reply::positive);

    int64_t now = ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(send_next(&e, 0, now), 0x0Bu);
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_EQ(send_next(&e, 0, now), 0x0Cu);
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[1].dropped, 0u) << "0x0B's answer dropped 0x0C";

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(send_next(&e, 0, now), 0x0Bu);
    const uint8_t reply[3] = {0x41, 0x0B, 0x2A};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply, 3, now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(send_next(&e, 0, now), 0x0Cu);
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[1].dropped, ECUIO_DROP_NOT_SUPPORTED)
        << "back in step, the refusal is 0x0C's";
}

/* A walk that stopped short resumes, at the driver's next probe (on a
 * recovery), at the first bitmap it hasn't had; a channel whose chain
 * was answered to its end isn't walked again. */
TEST(RequestEngineTest, TheWalkResumesWhereItStopped) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x2F}));
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    /* 0x0C (bit 0x10 of the second byte), and the next bitmap */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00100001u, 4 * kMs),
              ecuio_reply::positive);
    int64_t now = ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x20, now));
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.ranges, 0x1u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);

    ASSERT_EQ(send_next(&e, 0, now), 0x0Cu);
    const uint8_t reply[3] = {0x41, 0x0C, 0x1F};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply, 3, now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    ecuio_engine_probe(&e, now + 4 * kMs);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x20, now));
    /* 0x2F (bit 0x02 of the second byte), and the chain ends */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x20, 0x00020000u, now + 4 * kMs),
              ecuio_reply::positive);
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.ranges, 0x3u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.dropped, 0u);

    ecuio_engine_probe(&e, now + 10 * kMs);
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_NE(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE)
        << "an answered chain was walked again";
}

/* No bitmap is asked after the budget: a walk that pending replies hold
 * past it ends there, the answers it had applied, and the PIDs it didn't
 * reach stay. */
TEST(RequestEngineTest, TheBudgetEndsTheWalk) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C, 0x2E}));
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(answer_negative(&e, 0, 0x01, ECUIO_NRC_RESPONSE_PENDING, 10 * kMs),
              ecuio_reply::pending);
    const int64_t late = ECUIO_ENGINE_PROBE_BUDGET_US - 10 * kMs;
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 0xBE1FA813u, late),
              ecuio_reply::positive);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_PROBE_BUDGET_US), 0x0Cu)
        << "the walk asked past its budget";
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_BITMAP) << "0x0B's bit is clear";
    EXPECT_EQ(e.requests[2].dropped, 0u) << "0x2E wasn't reached";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.ranges, 0x1u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
}

/* A refused bitmap ends the walk. 0x12 there says nothing of the PIDs,
 * which stay; 0x11 says the ECU has no Service 01, so every PID leaves.
 * A positive reply too short to be a bitmap ends it unjudged. */
TEST(RequestEngineTest, ARefusedBitmapEndsTheWalk) {
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C}));
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x12, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x0Cu);
    EXPECT_EQ(e.requests[0].dropped, 0u);

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(answer_negative(&e, 0, 0x01, 0x11, 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(e.requests[1].dropped, ECUIO_DROP_SERVICE);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x1Eu) << "the DID stays";

    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B}));
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    const uint8_t short_bitmap[3] = {0x41, 0x00, 0xBE};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, short_bitmap, 3, 4 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(send_next(&e, 0, ECUIO_ENGINE_GAP_US), 0x0Bu);
    EXPECT_EQ(e.chans[0].ranges, 0u);
}

/* Only a channel with Service 01 requests walks; the other's rotation
 * starts at once. A channel whose every request was dropped sends
 * nothing more and has no next event. */
TEST(RequestEngineTest, OnlyChannelsAskingForPidsWalk) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    const uint8_t pid[2] = {0x01, 0x0B}, did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    EXPECT_EQ(send_next(&e, 1, 0), 0x1Eu);

    /* Nothing supported, and the chain ends: 0x0B leaves. */
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00000000u, 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].requests, 0u);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_GAP_US), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, ECUIO_ENGINE_GAP_US),
              ECUIO_ENGINE_WAIT_US - ECUIO_ENGINE_GAP_US)
        << "only channel 1's wait";
}

/* ---------- the DID scan (ADR-0020) ---------- */

namespace {

/* Sends channel c's one-shot, which must be 22 <did>, at now. */
void send_scan_request(ecuio_request_engine* e, uint32_t c, uint16_t did,
                       int64_t now)
{
    const int32_t r = ecuio_engine_due(e, c, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << "DID " << std::hex << did;
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    ASSERT_EQ(q.len, 3u);
    EXPECT_EQ(q.bytes[0], 0x22u);
    EXPECT_EQ(q.bytes[1], static_cast<uint8_t>(did >> 8));
    EXPECT_EQ(q.bytes[2], static_cast<uint8_t>(did));
    ecuio_engine_sent(e, c, r, true, now);
}

/* Sends channel c's next request, which must be table entry i, at now. */
void send_rotation(ecuio_request_engine* e, uint32_t c, int32_t i, int64_t now)
{
    ASSERT_EQ(ecuio_engine_due(e, c, now), i);
    ecuio_engine_sent(e, c, i, true, now);
}

ecuio_reply answer_did(ecuio_request_engine* e, uint32_t c, uint16_t did,
                       int64_t now)
{
    const uint8_t reply[5] = {0x62, static_cast<uint8_t>(did >> 8),
                              static_cast<uint8_t>(did), 0x12, 0x34};
    return ecuio_engine_on_frame(e, c, reply, sizeof(reply), now, nullptr);
}

} // namespace

/* A scan takes turns with the rotation: the rotation's request, then a
 * scanned DID, and so on. A DID the ECU answers joins the rotation at
 * once; one it refuses is counted and never asked again. */
TEST(RequestEngineTest, ScanTakesTurnsWithTheRotationAndKeepsWhatItFinds) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did_c[3] = {0x22, 0x1E, 0x1C};
    const uint8_t did_d[3] = {0x22, 0x1E, 0x1D};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did_c, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, did_d, 3), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));
    EXPECT_EQ(e.scan_count, 2u);
    EXPECT_EQ(e.chans[0].scan_pending, 2u);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), 0) << "something is due";

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));   /* the rotation first */
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    ASSERT_EQ(e.count, 3u) << "the found DID joined the rotation";
    EXPECT_EQ(e.requests[2].bytes[2], 0x20u);
    EXPECT_EQ(e.chans[0].requests, 3u);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 1u);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 1, now));   /* 1E1D: the rotation's turn */
    EXPECT_EQ(answer_did(&e, 0, 0x1E1D, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_REFUSED);
    EXPECT_EQ(e.scan[1].nrc, 0x31u);
    EXPECT_EQ(e.count, 3u) << "a refused DID enters nothing";

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 2, now));   /* 1E20, now rotating */
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "the scan is over";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_total, 2u);
    EXPECT_EQ(s.scan_done, 2u);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 1u);
    EXPECT_EQ(s.scan_polled, 1u);
    EXPECT_EQ(s.requests, 3u);
    EXPECT_EQ(s.dropped, 0u);
}

/* ?scan=first: while DIDs are left to scan, the scan sends every turn and
 * the rotation, the listed DID and the ones found, waits; once the last is
 * asked, the rotation runs. */
TEST(RequestEngineTest, ScanFirstAsksEveryDidBeforeTheRotation) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    e.scan_first = true;
    const uint8_t did_c[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did_c, 3), ecuio_engine_add_result::added);
    for (uint16_t d = 0x1E20; d < 0x1E24; ++d) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, d));
    }

    int64_t now = 0;
    for (uint16_t d = 0x1E20; d < 0x1E24; ++d) {
        ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, d, now))
            << "the scan's turn, back to back";
        if (d == 0x1E21) {
            EXPECT_EQ(answer_did(&e, 0, d, now + 4 * kMs), ecuio_reply::positive);
        } else {
            EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
                      ecuio_reply::negative);
        }
        now += ECUIO_ENGINE_GAP_US;
    }
    EXPECT_EQ(e.chans[0].requests, 2u) << "the found DID joined the rotation";
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "the scan is over";
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 1, now));

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 3u);
}

/* A channel with nothing to rotate scans every turn. A DID nobody answers
 * is unanswered, not refused: the scan ends with it open, and a rescan
 * (the driver's, when the link recovers) asks it again. */
TEST(RequestEngineTest, ScanAloneRunsEveryTurnAndRescanAsksTheUnansweredAgain) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), ECUIO_ENGINE_WAIT_US);
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_UNANSWERED);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), INT64_MAX);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, 0u) << "unanswered isn't complete";
    EXPECT_EQ(s.scan_total, 2u);
    EXPECT_EQ(s.scan_done, 0u);

    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.chans[0].scan_pending, 2u);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    EXPECT_EQ(e.count, 1u);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "the found DID's turn";
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
              ecuio_reply::negative);
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_done, 2u);
}

/* A long reply can come after its wait: a positive reply naming a
 * scanned DID left unanswered answers it, whether another request is
 * out or none, so it's found and polled and the scan completes. The
 * request out when it lands keeps waiting for its own. */
TEST(RequestEngineTest, ALateReplyStillAnswersItsScannedDid) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_UNANSWERED);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::none)
        << "1E21's request still waits";
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 1u);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_ONCE);

    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_UNANSWERED);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_NONE);
    EXPECT_EQ(answer_did(&e, 0, 0x1E21, now + 4 * kMs), ecuio_reply::none);
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_FOUND);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_found, 2u);
    EXPECT_EQ(s.scan_polled, 2u);
    EXPECT_EQ(e.count, 2u);
}

/* A found DID is counted even when the table is full; it just isn't
 * polled, which the report says. */
TEST(RequestEngineTest, ScanFoundWithAFullTableIsCountedButNotPolled) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    for (uint32_t i = 0; i < ECUIO_PROFILE_MAX_REQUESTS; ++i) {
        const uint8_t req[2] = {0x01, static_cast<uint8_t>(i)};
        ASSERT_EQ(ecuio_engine_add(&e, 0, req, 2), ecuio_engine_add_result::added);
    }
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    const uint8_t reply[3] = {0x41, 0x00, 0x00};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, reply, 3, now + kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E1C, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + kMs), ecuio_reply::positive);
    EXPECT_EQ(e.count, ECUIO_PROFILE_MAX_REQUESTS);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 0u);
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_polled, 0u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
}

/* A scan request the driver couldn't send stays due, as a bitmap does. */
TEST(RequestEngineTest, UnsentScanRequestStaysDue) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    const int32_t r = ecuio_engine_due(&e, 0, 0);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, r, false, 0);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_NONE);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_UNASKED);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E1C, ECUIO_ENGINE_GAP_US));
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, ECUIO_ENGINE_GAP_US + kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.count, 1u);
}

TEST(RequestEngineTest, ScanAddKeepsADidOncePerChannelAndStopsAtItsLimit) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    EXPECT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    EXPECT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C)) << "kept once";
    EXPECT_EQ(e.scan_count, 1u);
    EXPECT_TRUE(ecuio_engine_scan_add(&e, 1, 0x1E1C)) << "another ECU";
    EXPECT_EQ(e.scan_count, 2u);
    EXPECT_FALSE(ecuio_engine_scan_add(&e, 2, 0x1E1C)) << "past the channels";
    for (uint32_t d = 0; e.scan_count < ECUIO_ENGINE_SCAN_MAX; ++d) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, static_cast<uint16_t>(0x2000 + d)));
    }
    EXPECT_FALSE(ecuio_engine_scan_add(&e, 0, 0x3000)) << "the scan is full";
    EXPECT_EQ(e.chans[0].scan_pending, ECUIO_ENGINE_SCAN_MAX - 1);
}

/* ---------- identification (ADR-0021) ---------- */

namespace {

/* Sends channel c's one-shot, which must be identification item i of
 * ECUIO_IDENT_REQUESTS, at now. */
void send_ident_request(ecuio_request_engine* e, uint32_t c, uint32_t i,
                        int64_t now)
{
    const ecuio_ident_request& want = ECUIO_IDENT_REQUESTS[i];
    const int32_t r = ecuio_engine_due(e, c, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << "item " << i << " " << want.name;
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    ASSERT_EQ(q.len, want.len) << want.name;
    EXPECT_EQ(memcmp(q.bytes, want.bytes, want.len), 0) << want.name;
    ASSERT_TRUE(e->chans[c].identifying);
    EXPECT_EQ(e->chans[c].ident_index, i);
    ecuio_engine_sent(e, c, r, true, now);
}

/* The ECU's reply to item i: its positive header (49 <item> 01, or
 * 62 <DID>), then data. */
ecuio_reply answer_ident(ecuio_request_engine* e, uint32_t c, uint32_t i,
                         const uint8_t* data, uint16_t n, int64_t now)
{
    const ecuio_ident_request& q = ECUIO_IDENT_REQUESTS[i];
    uint8_t reply[128] = {};
    reply[0] = static_cast<uint8_t>(q.bytes[0] + 0x40);
    reply[1] = q.bytes[1];
    reply[2] = (q.len == 2) ? 0x01 : q.bytes[2];
    memcpy(reply + 3, data, n);
    return ecuio_engine_on_frame(e, c, reply, static_cast<uint16_t>(3 + n),
                                 now, nullptr);
}

} // namespace

/* Identification takes turns with the rotation, ahead of the scan: a
 * rotation request, an item, and so on. A positive reply's data becomes
 * the item's text (ASCII, NUL-padded items joined, or hex), a negative
 * one its code, a timeout leaves it unanswered until a rescan asks it
 * again. Nothing it learns enters the rotation, and the report is
 * complete once every item has an outcome. */
TEST(RequestEngineTest, IdentificationTakesTurnsAndKeepsWhatTheEcuSaid) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t pid_0c[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid_0c, 2), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ecuio_engine_identify(&e);
    EXPECT_EQ(e.chans[0].ident_count, ECUIO_IDENT_COUNT);
    EXPECT_EQ(e.chans[0].ident_pending, ECUIO_IDENT_COUNT);
    ecuio_engine_identify(&e);   /* idempotent */
    EXPECT_EQ(e.chans[0].ident_pending, ECUIO_IDENT_COUNT);
    EXPECT_STREQ(ECUIO_IDENT_REQUESTS[0].name, "VIN");

    int64_t now = 0;
    const uint8_t rpm[4] = {0x41, 0x0C, 0x1F, 0x40};
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));   /* the rotation first */
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));   /* 09 02 */
    const char vin[] = "FAKEJ2534VIN00001";
    EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(vin), 17,
                           now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].ident[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(e.chans[0].ident[0].text, vin);
    EXPECT_EQ(e.chans[0].ident[0].text_length, 17u);
    EXPECT_EQ(e.chans[0].ident_generation, 1u);
    EXPECT_EQ(e.count, 1u) << "nothing joins the rotation";

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));   /* the rotation's turn */
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 1, now));   /* 09 04 */
    const uint8_t two_calids[16] = {'C', 'A', 'L', '-', 'A', 0, 0, 0,
                                    'C', 'A', 'L', '-', 'B', 0, 0, 0};
    EXPECT_EQ(answer_ident(&e, 0, 1, two_calids, sizeof(two_calids), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_STREQ(e.chans[0].ident[1].text, "CAL-A, CAL-B");

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 2, now));   /* 09 06 */
    const uint8_t cvn[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(answer_ident(&e, 0, 2, cvn, sizeof(cvn), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_STREQ(e.chans[0].ident[2].text, "DE AD BE EF");

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 3, now));   /* 09 0A */
    EXPECT_EQ(answer_negative(&e, 0, 0x09, 0x12, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.chans[0].ident[3].outcome, ECUIO_IDENTITY_REFUSED);
    EXPECT_EQ(e.chans[0].ident[3].nrc, 0x12u);
    EXPECT_EQ(e.chans[0].ident[3].text[0], '\0');

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 4, now));   /* 22 F1 87 */
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), ECUIO_ENGINE_WAIT_US);
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.chans[0].ident[4].outcome, ECUIO_IDENTITY_UNANSWERED);

    /* The rest are refused; the probe is off, so nothing is dropped. */
    for (uint32_t i = 5; i < ECUIO_IDENT_COUNT; ++i) {
        now += ECUIO_ENGINE_GAP_US;
        ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
        EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs,
                                        nullptr),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, i, now));
        EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
                  ecuio_reply::negative);
        EXPECT_EQ(e.chans[0].ident[i].outcome, ECUIO_IDENTITY_REFUSED);
    }
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_EQ(e.chans[0].ident_pending, 0u);

    ecuio_channel_identity_t id;
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE);
    EXPECT_EQ(id.items, ECUIO_IDENT_COUNT);
    EXPECT_EQ(id.generation, ECUIO_IDENT_COUNT);
    EXPECT_STREQ(id.list[0].text, vin);
    EXPECT_EQ(id.list[12].request[2], 0x95u);

    /* The scan waited behind identification; now it has the turn. */
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
              ecuio_reply::negative);

    /* A rescan asks the unanswered item again, and only it. */
    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.chans[0].ident_pending, 1u);
    EXPECT_EQ(e.chans[0].ident[4].outcome, ECUIO_IDENTITY_UNASKED);
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, 0u) << "an unasked item isn't complete";
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 4, now));
    const char part[] = "FAKE-PART-0001";
    EXPECT_EQ(answer_ident(&e, 0, 4, reinterpret_cast<const uint8_t*>(part), 14,
                           now + 4 * kMs),
              ecuio_reply::positive);
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE);
    EXPECT_EQ(id.generation, ECUIO_IDENT_COUNT + 1);
    EXPECT_STREQ(id.list[4].text, part);
    EXPECT_EQ(e.count, 1u);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "only the rotation is left";
}

/* A channel with nothing to rotate identifies every turn, in list order,
 * and has nothing to send once the list is asked. A channel never given
 * the list reports no items and never reads complete. */
TEST(RequestEngineTest, IdentificationAloneRunsEveryTurnAndThenRests) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    ecuio_channel_identity_t id;
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.items, 0u);
    EXPECT_EQ(id.flags, 0u);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), INT64_MAX);

    ecuio_engine_identify(&e);
    EXPECT_EQ(e.chans[1].ident_count, ECUIO_IDENT_COUNT) << "every channel";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), 0);
    int64_t now = 0;
    for (uint32_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, i, now));
        if (i % 2 == 0) {
            now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        } else {
            EXPECT_EQ(answer_negative(&e, 0, ECUIO_IDENT_REQUESTS[i].bytes[0], 0x31,
                                      now + 4 * kMs),
                      ecuio_reply::negative);
            now += ECUIO_ENGINE_GAP_US;
        }
    }
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE) << "unanswered counts as settled";
    EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_UNANSWERED);
    EXPECT_EQ(id.list[1].outcome, ECUIO_IDENTITY_REFUSED);
    /* Channel 1 still has its whole list to ask. */
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 0);
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 1, 0, now));
}

namespace {

/* A connect whose walk and first rotation request went unanswered, and
 * whose next turn has the VIN item in the one-shot, not yet sent: the
 * state a recovery's probe can meet. */
void identify_after_a_silent_walk(ecuio_request_engine* e, int64_t* now)
{
    ecuio_engine_init(e, 1);
    const uint8_t pid_0c[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(e, 0, pid_0c, 2), ecuio_engine_add_result::added);
    ecuio_engine_identify(e);
    ecuio_engine_probe(e, *now);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(e, 0, 0x00, *now));
    *now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(e, 0, *now));   /* the walk ends unjudged */
    ASSERT_NO_FATAL_FAILURE(send_rotation(e, 0, 0, *now));
    *now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(e, 0, *now));
}

const char kVin[] = "FAKEJ2534VIN00001";

} // namespace

/* A recovery restarts the walk while an identification item is out: the
 * item keeps the one-shot, its reply settles it, and the walk follows.
 * The recovery's rescan has already run, so an item lost here would stay
 * unanswered until the next one. */
TEST(RequestEngineTest, RecoveryWalkWaitsForTheItemOut) {
    ecuio_request_engine e;
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(identify_after_a_silent_walk(&e, &now));
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));   /* 09 02 */

    ecuio_engine_probe(&e, now + kMs);   /* the driver's recovery calls */
    ecuio_engine_rescan(&e);
    EXPECT_TRUE(e.chans[0].walking);
    EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(kVin), 17,
                           now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].ident[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(e.chans[0].ident[0].text, kVin);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 1u << (32 - 0x0C), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_TRUE(e.chans[0].walked);
    EXPECT_EQ(e.chans[0].requests, 1u) << "0x0C is supported";
}

/* A recovery restarts the walk while an identification item waits in
 * the one-shot unsent (its write failed): the item goes out first, so the
 * walk's bitmap reply can never be taken for the item's answer. */
TEST(RequestEngineTest, RecoveryWalkWaitsForAnUnsentItem) {
    ecuio_request_engine e;
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(identify_after_a_silent_walk(&e, &now));
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ASSERT_TRUE(e.chans[0].identifying);
    ecuio_engine_sent(&e, 0, r, false, now);   /* the write failed */

    ecuio_engine_probe(&e, now + kMs);   /* another channel recovered the link */
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));
    EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(kVin), 17,
                           now + 4 * kMs),
              ecuio_reply::positive);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 1u << (32 - 0x0C), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_STREQ(e.chans[0].ident[0].text, kVin) << "the bitmap isn't the VIN";
    EXPECT_TRUE(e.chans[0].walked);
}

/* The report's text: ASCII when the bytes are printable or padding, the
 * padded items joined and trailing blanks dropped; hex pairs otherwise;
 * cut to the field at a whole pair. */
TEST(RequestEngineTest, IdentityTextIsAsciiOrHexAndCutToTheField) {
    char out[ECUIO_IDENTITY_TEXT_LEN];
    const auto text = [&out](std::initializer_list<uint8_t> bytes,
                             uint32_t cap = ECUIO_IDENTITY_TEXT_LEN) {
        uint8_t data[80] = {};
        uint32_t n = 0;
        for (uint8_t b : bytes) {
            data[n++] = b;
        }
        const uint8_t len = ecuio_identity_format(data, n, out, cap);
        EXPECT_EQ(len, strlen(out));
        return std::string(out);
    };
    EXPECT_EQ(text({'A', 'B', 'C'}), "ABC");
    EXPECT_EQ(text({'A', 'B', 'C', 0, 0, 0}), "ABC");
    EXPECT_EQ(text({'A', 'B', ' ', ' ', 0xFF}), "AB");
    EXPECT_EQ(text({'A', 0, 0, 'B', 0xFF, 'C'}), "A, B, C");
    EXPECT_EQ(text({' ', ' '}), "");
    EXPECT_EQ(text({0x00, 0x01}), "00 01");
    EXPECT_EQ(text({0xFF, 0xFF}), "FF FF") << "padding alone isn't text";
    EXPECT_EQ(text({'A', 0x7F}), "41 7F");
    EXPECT_EQ(text({}), "");

    uint8_t many[70];
    memset(many, 'A', sizeof(many));
    EXPECT_EQ(ecuio_identity_format(many, sizeof(many), out, sizeof(out)), 63u);
    EXPECT_EQ(strlen(out), 63u);
    memset(many, 0x10, sizeof(many));
    EXPECT_EQ(ecuio_identity_format(many, 30, out, sizeof(out)), 62u)
        << "21 pairs fit in 63 characters";
    EXPECT_EQ(out[61], '0');
    EXPECT_EQ(text({'A', 'B', 'C', 'D'}, 4), "ABC");
    EXPECT_EQ(text({0x12, 0x34}, 4), "12") << "the second pair needs three more";
    EXPECT_EQ(text({'A', 0, 'B'}, 4), "A") << "the separator needs two";
}

TEST(RequestEngineTest, IdentityRequestsAreNamed) {
    const uint8_t vin[2] = {0x09, 0x02};
    const uint8_t supplier_version[3] = {0x22, 0xF1, 0x95};
    const uint8_t other[3] = {0x22, 0x12, 0x34};
    EXPECT_STREQ(ecuio_identity_request_name(vin, 2), "VIN");
    EXPECT_STREQ(ecuio_identity_request_name(supplier_version, 3),
                 "supplier software version");
    EXPECT_STREQ(ecuio_identity_request_name(other, 3), "");
    EXPECT_STREQ(ecuio_identity_request_name(vin, 3), "") << "the length counts";
    EXPECT_STREQ(ecuio_identity_request_name(nullptr, 2), "");
}

/* ---------- uds-generic: the ?did= grammar and the set it builds ---------- */

namespace {

/* Splits a query (the text after the URI's '?') and builds the
 * uds-generic set from its did= keys, as create does. */
ecuio_status_t build_uds(const std::string& query, ecuio_uds_session* out)
{
    ecuio_uri_query q;
    if (ecuio_uri::split(query.c_str(), &q) != ecuio_uri::split_result::ok) {
        return ECUIO_ERR_BAD_URI;
    }
    return ecuio_uds_build(q, out);
}

/* The requests the set's polled head derives on its own channels. */
uint32_t derived_requests(const ecuio_uds_session& u)
{
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
    for (size_t i = 0; i < u.channels.size(); ++i) {
        rx[i] = u.channels[i].rx_id;
    }
    ecuio_request_engine e;
    ecuio_engine_init(&e, static_cast<uint32_t>(u.channels.size()));
    const ecuio_signal_set_t head = ecuio_uds_polled_set(u);
    EXPECT_TRUE(ecuio_engine_derive(&e, &head, rx,
                                    static_cast<uint32_t>(u.channels.size()),
                                    nullptr));
    return e.count;
}

} // namespace

TEST(UdsGenericTest, SingleDidsArePolledOnTheDefaultEcu) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=1E1C,F190:text", &u), ECUIO_OK);
    ASSERT_EQ(u.set.count, 2u);
    EXPECT_EQ(u.polled, 2u);
    EXPECT_TRUE(u.scan.empty());
    ASSERT_EQ(u.set.channel_count, 1u);
    EXPECT_EQ(u.set.channels[0].rx_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(u.set.channels[0].tx_id, ECUIO_OBD2_TESTER_ARB_ID);
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(u.set.layout_version, ECUIO_SIGNAL_SET_LAYOUT);

    const ecuio_signal_def_t& a = u.set.defs[0];
    EXPECT_EQ(a.id, ECUIO_UDS_GENERIC_SIG_ID_BASE);
    EXPECT_EQ(a.message_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(a.mux_offset, 0u);
    EXPECT_EQ(a.mux_length, 3u);
    EXPECT_EQ(a.mux_key, 0x621E1Cu);
    EXPECT_EQ(a.kind, ECUIO_VALUE_HEX) << "hex by default";
    EXPECT_EQ(a.byte_order, ECUIO_BYTE_ORDER_BE);
    EXPECT_EQ(a.start_byte, 3u);
    EXPECT_EQ(a.bit_length, 0u);
    EXPECT_EQ(a.byte_length, 0u) << "to the end of the reply";
    EXPECT_EQ(a.scale, 1.0);
    EXPECT_EQ(a.offset, 0.0);
    EXPECT_STREQ(a.name, "DID 1E1C");
    EXPECT_STREQ(a.unit, "");
    EXPECT_EQ(a.decimal_places, 0u);
    const ecuio_signal_def_t& b = u.set.defs[1];
    EXPECT_EQ(b.id, ECUIO_UDS_GENERIC_SIG_ID_BASE + 1);
    EXPECT_EQ(b.kind, ECUIO_VALUE_TEXT);
    EXPECT_EQ(b.mux_key, 0x62F190u);
    EXPECT_STREQ(b.name, "DID F190");

    EXPECT_EQ(derived_requests(u), 2u);

    /* The fake's reply to 1E1C, as the bridge decodes it */
    const uint8_t reply[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    char text[16] = "";
    ASSERT_EQ(ecuio_decode_text(&a, reply, sizeof(reply), text, sizeof(text)),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(text, "12 34");
}

TEST(UdsGenericTest, TypesSetTheKindAndWidth) {
    const struct {
        const char* name;
        uint8_t kind;
        uint8_t bits;
    } kCases[] = {
        {"hex", ECUIO_VALUE_HEX, 0},   {"text", ECUIO_VALUE_TEXT, 0},
        {"u8", ECUIO_VALUE_UINT, 8},   {"u16", ECUIO_VALUE_UINT, 16},
        {"u24", ECUIO_VALUE_UINT, 24}, {"u32", ECUIO_VALUE_UINT, 32},
        {"i8", ECUIO_VALUE_INT, 8},    {"i16", ECUIO_VALUE_INT, 16},
        {"i24", ECUIO_VALUE_INT, 24},  {"i32", ECUIO_VALUE_INT, 32},
    };
    for (const auto& c : kCases) {
        ecuio_uds_session u;
        ASSERT_EQ(build_uds(std::string("did=1e1c:") + c.name, &u), ECUIO_OK)
            << c.name;   /* hex digits in either case */
        ASSERT_EQ(u.set.count, 1u) << c.name;
        const ecuio_signal_def_t& d = u.set.defs[0];
        EXPECT_EQ(d.kind, c.kind) << c.name;
        EXPECT_EQ(d.bit_length, c.bits) << c.name;
        EXPECT_EQ(d.byte_order, ECUIO_BYTE_ORDER_BE) << c.name;
        EXPECT_EQ(d.start_byte, 3u) << c.name;
        EXPECT_EQ(d.mux_key, 0x621E1Cu) << c.name;
        EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK)
            << c.name;
    }

    /* Big-endian, raw: 12 34 reads 0x1234 as u16, and FF reads -1 as i8 */
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=1E1C:u16,1E1D:i8", &u), ECUIO_OK);
    const uint8_t word[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    double v = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&u.set.defs[0], word, sizeof(word), &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, 0x1234);
    const uint8_t byte[4] = {0x62, 0x1E, 0x1D, 0xFF};
    ASSERT_EQ(ecuio_decode_signal(&u.set.defs[1], byte, sizeof(byte), &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, -1.0);
}

TEST(UdsGenericTest, EcuPrefixNamesTheRespondingEcu) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=7E9:1E1C,1E1C,18DAF110:F190:u8", &u), ECUIO_OK);
    ASSERT_EQ(u.set.channel_count, 3u) << "in order of first appearance";
    EXPECT_EQ(u.set.channels[0].rx_id, 0x7E9u);
    EXPECT_EQ(u.set.channels[0].tx_id, 0x7E1u);
    EXPECT_EQ(u.set.channels[1].rx_id, 0x7E8u);
    EXPECT_EQ(u.set.channels[1].tx_id, 0x7E0u);
    EXPECT_EQ(u.set.channels[2].rx_id, 0x18DAF110u);
    EXPECT_EQ(u.set.channels[2].tx_id, 0x18DA10F1u);
    ASSERT_EQ(u.set.count, 3u) << "the same DID on two ECUs is two defs";
    EXPECT_EQ(u.set.defs[0].message_id, 0x7E9u);
    EXPECT_STREQ(u.set.defs[0].name, "0x7E9 DID 1E1C");
    EXPECT_STREQ(u.set.defs[1].name, "DID 1E1C");
    EXPECT_STREQ(u.set.defs[2].name, "0x18DAF110 DID F190");
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(derived_requests(u), 3u);

    uint32_t tx = 0;
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x7E8u, &tx));
    EXPECT_EQ(tx, 0x7E0u);
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x9u, &tx));
    EXPECT_EQ(tx, 1u);
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x7FFu, &tx));
    EXPECT_EQ(tx, 0x7F7u);
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x18DAF110u, &tx));
    EXPECT_EQ(tx, 0x18DA10F1u);
    /* 8 would ask on 0, which no channel may carry (the fuzzer's find) */
    for (uint32_t rx : {0x8u, 0x7u, 0x0u, 0x800u, 0x1FFFFFFFu, 0x18DBF110u}) {
        EXPECT_FALSE(ecuio_uds_tx_for_rx(rx, &tx)) << std::hex << rx;
    }
}

/* An ECU prefix takes a range as it takes a DID, with or without a type:
 * the range's last token holds a '-', which no type name does. */
TEST(UdsGenericTest, EcuPrefixTakesARange) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=7E9:1E00-1E01,18DAF110:F190-F190:text", &u),
              ECUIO_OK);
    EXPECT_EQ(u.polled, 0u);
    ASSERT_EQ(u.scan.size(), 3u);
    EXPECT_EQ(u.scan[0].did, 0x1E00u);
    EXPECT_EQ(u.scan[0].rx_id, 0x7E9u);
    EXPECT_EQ(u.scan[1].did, 0x1E01u);
    EXPECT_EQ(u.scan[1].rx_id, 0x7E9u);
    EXPECT_EQ(u.scan[2].did, 0xF190u);
    EXPECT_EQ(u.scan[2].rx_id, 0x18DAF110u);
    ASSERT_EQ(u.set.count, 3u);
    EXPECT_STREQ(u.set.defs[0].name, "0x7E9 DID 1E00");
    EXPECT_EQ(u.set.defs[0].kind, ECUIO_VALUE_HEX) << "a range's default type";
    EXPECT_EQ(u.set.defs[2].kind, ECUIO_VALUE_TEXT);
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
}

/* A range is scanned, a lone DID polled, and a DID listed both ways is
 * polled. Polled defs come first, each group in the order its DIDs first
 * appeared, so the transport can derive the rotation from the head of
 * the set. */
TEST(UdsGenericTest, RangesAreScannedAndRepeatsKeptOnce) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=1E1A-1E1D,1E1C,1E1C&did=1E1B,1E1F-1E1F", &u),
              ECUIO_OK);
    ASSERT_EQ(u.set.count, 5u);
    EXPECT_EQ(u.polled, 2u);
    EXPECT_STREQ(u.set.defs[0].name, "DID 1E1B");
    EXPECT_STREQ(u.set.defs[1].name, "DID 1E1C");
    EXPECT_STREQ(u.set.defs[2].name, "DID 1E1A");
    EXPECT_STREQ(u.set.defs[3].name, "DID 1E1D");
    EXPECT_STREQ(u.set.defs[4].name, "DID 1E1F") << "a range of one scans";
    for (uint32_t i = 0; i < u.set.count; ++i) {
        EXPECT_EQ(u.set.defs[i].id, ECUIO_UDS_GENERIC_SIG_ID_BASE + i) << i;
    }
    ASSERT_EQ(u.scan.size(), 3u);
    EXPECT_EQ(u.scan[0].did, 0x1E1Au);
    EXPECT_EQ(u.scan[0].rx_id, 0x7E8u);
    EXPECT_EQ(u.scan[1].did, 0x1E1Du);
    EXPECT_EQ(u.scan[2].did, 0x1E1Fu);
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(derived_requests(u), 2u) << "the scanned DIDs enrol nothing";
}

TEST(UdsGenericTest, RefusalsNameTheirCause) {
    const char* const shape =
        "?did= needs a DID like 1E1C, an ECU and a type as in 7E9:1E1C:u16, "
        "or a range like 1E00-1EFF, not '";
    const char* const types =
        "?did= needs a type of hex, text, u8, u16, u24, u32, i8, i16, i24 or "
        "i32, not '";
    const char* const ecu =
        "?did= needs an ECU response ID of 9 to 7FF, or 29-bit 18DAxxyy, not '";
    const struct {
        const char* query;
        std::string detail;
    } kCases[] = {
        {"did=", "?did= needs at least one DID"},
        {"did=1E1C,", std::string(shape) + "'"},
        {"did=,1E1C", std::string(shape) + "'"},
        {"did=0x1E1C", std::string(shape) + "0x1E1C'"},
        {"did=12345", std::string(shape) + "12345'"},
        {"did=1E1C-", std::string(shape) + "1E1C-'"},
        {"did=7E8:1E1C:u8:x", std::string(shape) + "7E8:1E1C:u8:x'"},
        {"did=7E8:1E1C:u8:x:y", std::string(shape) + "7E8:1E1C:u8:x:y'"},
        {"did=7E8:1E1C-:u8", std::string(shape) + "7E8:1E1C-:u8'"},
        {"did=1E1C:", std::string(types) + "'"},
        {"did=1E1C:u12", std::string(types) + "u12'"},
        {"did=1E1C:U8", std::string(types) + "U8'"},
        {"did=7E8:1E1C:7E9", std::string(types) + "7E9'"},
        {"did=7E8:7E9:1E1C", std::string(types) + "1E1C'"},
        {"did=7E9:1E1C-", std::string(shape) + "7E9:1E1C-'"},
        {"did=7E9:1E1C-1E1G", std::string(shape) + "7E9:1E1C-1E1G'"},
        {"did=1EFF-1E00",
         "?did= needs a range whose end isn't before its start, not '1EFF-1E00'"},
        {"did=7:1E1C", std::string(ecu) + "7'"},
        {"did=7:1E00-1E01", std::string(ecu) + "7'"},
        {"did=8:1E1C", std::string(ecu) + "8'"},
        {"did=:1E1C", std::string(ecu) + "'"},
        {"did=18DB1234:1E1C", std::string(ecu) + "18DB1234'"},
        {"did=7E8:1E1C,7E9:1E1C,7EA:1E1C,7EB:1E1C,7EC:1E1C",
         "?did= names 5 ECUs, more than the 4 channels a session has"},
        {"did=0000-0400", "?did= scans 1025 DIDs, more than the 1024 a session scans"},
        {"did=0000-FFFF", "?did= scans 65536 DIDs, more than the 1024 a session scans"},
        {"did=0000-03FF,0400-0400",
         "?did= scans 1025 DIDs, more than the 1024 a session scans"},
    };
    for (const auto& c : kCases) {
        ecuio_uds_session u;
        EXPECT_EQ(build_uds(c.query, &u), ECUIO_ERR_BAD_URI) << c.query;
        EXPECT_EQ(std::string(ecuio_last_error_detail()), c.detail) << c.query;
    }

    /* One DID to poll past the engine's table */
    std::string many = "did=";
    for (unsigned i = 0; i <= ECUIO_PROFILE_MAX_REQUESTS; ++i) {
        char one[8];
        snprintf(one, sizeof(one), "%s%04X", (i == 0) ? "" : ",", 0x1000 + i);
        many += one;
    }
    ecuio_uds_session u;
    EXPECT_EQ(build_uds(many, &u), ECUIO_ERR_BAD_URI);
    char detail[96];
    snprintf(detail, sizeof detail,
             "?did= lists %u DIDs to poll, more than the %u a session "
             "asks for; scan a range instead",
             ECUIO_PROFILE_MAX_REQUESTS + 1u, ECUIO_PROFILE_MAX_REQUESTS);
    EXPECT_STREQ(ecuio_last_error_detail(), detail);

    /* At the limits, and other keys beside did=, it builds */
    many.erase(many.rfind(','));
    ASSERT_EQ(build_uds(many + "&profile=uds-generic", &u), ECUIO_OK);
    EXPECT_EQ(u.polled, ECUIO_PROFILE_MAX_REQUESTS);
    ASSERT_EQ(build_uds("did=0000-03FF", &u), ECUIO_OK);
    EXPECT_EQ(u.scan.size(), size_t{ECUIO_ENGINE_SCAN_MAX});
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
}

TEST(UdsGenericTest, TemplateSetIsEmptyAndValid) {
    const ecuio_signal_set_t* t = ecuio_uds_generic_template_set();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->count, 0u);
    EXPECT_EQ(t->channel_count, 0u);
    EXPECT_EQ(ecuio_validate_signal_set(t, nullptr), ECUIO_VALIDATE_OK);
    const ecuio_signal_set_t* found = nullptr;
    ASSERT_EQ(ecuio_profile_lookup(ECUIO_UDS_GENERIC_NAME, &found), ECUIO_OK);
    EXPECT_EQ(found, t) << "the registry binds the name to it";
    EXPECT_EQ(ecuio_profile_register(ECUIO_UDS_GENERIC_NAME, t, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT) << "a built-in";
}

/* ---------- the Service 01 table (fixtures/obd2/service01.csv) ---------- */

namespace {

struct PidRow {
    int line = 0;
    unsigned pid = 0;
    unsigned bytes = 0;
    std::string kind;
    unsigned start = 0;
    unsigned bits = 0;
    bool is_signed = false;
    double scale = 0;
    double offset = 0;
    std::string unit;
    double min = 0;
    double max = 0;
    std::string name;
};

constexpr const char* kTableHeader =
    "pid,bytes,kind,start,bits,signed,scale,offset,unit,min,max,name,note";

/* A number, or a fraction like 100/255 */
double number(const std::string& s)
{
    const size_t slash = s.find('/');
    if (slash == std::string::npos) {
        return std::strtod(s.c_str(), nullptr);
    }
    return std::strtod(s.substr(0, slash).c_str(), nullptr) /
           std::strtod(s.substr(slash + 1).c_str(), nullptr);
}

/* The table's rows; empty with why set when it won't read */
std::vector<PidRow> load_table(std::string* why)
{
    std::vector<PidRow> rows;
    std::ifstream in(ECUIO_OBD2_TABLE, std::ios::binary);
    if (!in) {
        *why = "can't open " ECUIO_OBD2_TABLE;
        return {};
    }
    std::string text;
    int n = 0;
    bool header = false;
    while (std::getline(in, text)) {
        ++n;
        if (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }
        if (text.empty() || text[0] == '#') {
            continue;
        }
        if (!header) {
            if (text != kTableHeader) {
                *why = "line " + std::to_string(n) + " isn't the header";
                return {};
            }
            header = true;
            continue;
        }
        std::vector<std::string> f;
        size_t from = 0;
        for (size_t comma; (comma = text.find(',', from)) != std::string::npos;
             from = comma + 1) {
            f.push_back(text.substr(from, comma - from));
        }
        f.push_back(text.substr(from));
        if (f.size() != 13) {
            *why = "line " + std::to_string(n) + " has " +
                   std::to_string(f.size()) + " fields, not 13";
            return {};
        }
        PidRow r;
        r.line = n;
        r.pid = static_cast<unsigned>(std::strtoul(f[0].c_str(), nullptr, 16));
        r.bytes =
            static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
        r.kind = f[2];
        if (r.kind == "linear") {
            r.start =
                static_cast<unsigned>(std::strtoul(f[3].c_str(), nullptr, 10));
            r.bits =
                static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
            r.is_signed = f[5] == "1";
            r.scale = number(f[6]);
            r.offset = number(f[7]);
            r.unit = f[8];
            r.min = number(f[9]);
            r.max = number(f[10]);
        }
        r.name = f[11];
        rows.push_back(r);
    }
    if (rows.empty()) {
        *why = "no rows";
    }
    return rows;
}

class Obd2TableTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        fake_ecu::reset_diagnostics();
        std::string why;
        rows_ = load_table(&why);
        ASSERT_FALSE(rows_.empty()) << why;
    }

    /* The linear row for a PID's field at a data byte, or nullptr */
    const PidRow* linear(unsigned pid, unsigned start) const
    {
        for (const PidRow& r : rows_) {
            if (r.pid == pid && r.kind == "linear" && r.start == start) {
                return &r;
            }
        }
        return nullptr;
    }

    /* The PID's byte count, or 0 when the table lacks it */
    unsigned bytes_of(unsigned pid) const
    {
        for (const PidRow& r : rows_) {
            if (r.pid == pid) {
                return r.bytes;
            }
        }
        return 0;
    }

    std::vector<PidRow> rows_;
};

} // namespace

TEST_F(Obd2TableTest, RowsAreWellFormed)
{
    const std::string kinds = " linear bitmap bits enum dtc complex ";
    unsigned last = 0;
    for (const PidRow& r : rows_) {
        SCOPED_TRACE("line " + std::to_string(r.line) + ", " + r.name);
        EXPECT_NE(kinds.find(" " + r.kind + " "), std::string::npos) << r.kind;
        EXPECT_LE(r.pid, 0xFFu);
        EXPECT_GE(r.pid, last) << "rows run in PID order";
        last = r.pid;
        EXPECT_EQ(r.bytes, bytes_of(r.pid)) << "one byte count per PID";
        EXPECT_GE(r.bytes, 1u);
        EXPECT_EQ(r.kind == "bitmap", r.pid % 0x20 == 0)
            << "bitmaps at 00, 20, 40 and on";
        if (r.kind == "bitmap") {
            EXPECT_EQ(r.bytes, 4u);
        }
        if (r.kind == "linear") {
            EXPECT_TRUE(r.bits == 8 || r.bits == 16 || r.bits == 32) << r.bits;
            EXPECT_LE(r.start + r.bits / 8, r.bytes)
                << "the field fits the reply";
            EXPECT_NE(r.scale, 0.0);
            EXPECT_EQ(linear(r.pid, r.start), &r) << "one field per start byte";
        }
    }
}

/* The range the scaling gives a field's raw extremes against the
 * source's: a wrong scale, offset, width or sign shows as a range that
 * doesn't match. Within 0.2% of the span, which the source's rounded
 * maxima (99.2 for 99.21875) need. */
TEST_F(Obd2TableTest, EachRangeFollowsFromItsScaling)
{
    for (const PidRow& r : rows_) {
        if (r.kind != "linear") {
            continue;
        }
        SCOPED_TRACE("line " + std::to_string(r.line) + ", " + r.name);
        const double span = std::ldexp(1.0, static_cast<int>(r.bits));
        const double raw_lo = r.is_signed ? -span / 2 : 0.0;
        const double raw_hi = r.is_signed ? span / 2 - 1 : span - 1;
        const double lo = raw_lo * r.scale + r.offset;
        const double hi = raw_hi * r.scale + r.offset;
        const double tolerance = 0.002 * std::fabs(r.max - r.min) + 1e-9;
        EXPECT_NEAR(lo, r.min, tolerance);
        EXPECT_NEAR(hi, r.max, tolerance);
    }
}

/* Each obd2 def against its PID's row: where the field sits, its width,
 * sign, byte order, scaling and unit */
TEST_F(Obd2TableTest, EveryObd2DefMatchesItsRow)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        SCOPED_TRACE(d.name);
        ASSERT_EQ(d.mux_offset, 0u);
        ASSERT_EQ(d.mux_length, 2u);
        ASSERT_EQ(d.mux_key >> 8, 0x41u) << "a Service 01 reply";
        const unsigned pid = d.mux_key & 0xFFu;
        ASSERT_GE(d.start_byte, 2u);
        const PidRow* r = linear(pid, d.start_byte - 2u);
        ASSERT_NE(r, nullptr) << "no linear row for PID " << pid;
        EXPECT_EQ(d.bit_offset, 0u);
        EXPECT_EQ(d.bit_length, r->bits);
        EXPECT_EQ(d.kind,
                  static_cast<uint8_t>(r->is_signed ? ECUIO_VALUE_INT
                                                    : ECUIO_VALUE_UINT));
        if (r->bits > 8) {
            EXPECT_EQ(d.byte_order, static_cast<uint8_t>(ECUIO_BYTE_ORDER_BE))
                << "J1979's multi-byte values";
        }
        EXPECT_NEAR(d.scale, r->scale, 1e-12 * std::fabs(r->scale));
        EXPECT_DOUBLE_EQ(d.offset, r->offset);
        EXPECT_STREQ(d.unit, r->unit.c_str());
    }
}

/* The set covers the table: exactly one def for every linear row (its
 * PID and start byte), and none for a row of another kind (ADR-0039
 * section 1, section 2). */
TEST_F(Obd2TableTest, EveryLinearRowHasExactlyOneDef)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (const PidRow& r : rows_) {
        SCOPED_TRACE("line " + std::to_string(r.line) + ", " + r.name);
        unsigned defs = 0;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if ((d.mux_key & 0xFFu) == r.pid && d.start_byte == 2 + r.start) {
                ++defs;
            }
        }
        EXPECT_EQ(defs, r.kind == "linear" ? 1u : 0u);
    }
}

/* Every fast PID (obd2_rates.h) is one the set asks */
TEST_F(Obd2TableTest, EveryFastPidIsInTheSet)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint8_t pid : ECUIO_OBD2_FAST_PIDS) {
        bool found = false;
        for (uint32_t i = 0; i < set->count; ++i) {
            found = found || (set->defs[i].mux_key & 0xFFu) == pid;
        }
        EXPECT_TRUE(found) << "PID " << int(pid);
        EXPECT_NE(linear(pid, 0), nullptr) << "PID " << int(pid);
    }
}

/* A recording keys each value by its name (signals.csv), and so do
 * Live and the recordings' diff, so every built-in set's names are
 * unique, and each fits the def's buffer with room for its end */
TEST(Obd2SetTest, BuiltInSetNamesAreUniqueAndFit)
{
    for (const ecuio_signal_set_t* set :
         {ecuio_obd2_signal_set(), ecuio_mock_signal_set()}) {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const size_t n = strnlen(d.name, sizeof(d.name));
            ASSERT_LT(n, sizeof(d.name)) << "def " << d.id;
            EXPECT_GT(n, 0u) << "def " << d.id;
            names.emplace_back(d.name, n);
        }
        for (size_t i = 0; i < names.size(); ++i) {
            for (size_t j = i + 1; j < names.size(); ++j) {
                EXPECT_NE(names[i], names[j]) << "defs " << i << " and " << j;
            }
        }
    }
}

/* Every PID the fakes' ECU supports answers with the table's byte
 * count, and every PID the obd2 set asks is one it supports (else the
 * probe drops it in every test) */
TEST_F(Obd2TableTest, TheFakeEcuAnswersEachSupportedPidWithItsLength)
{
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        if (!fake_ecu::supports_pid(pid)) {
            continue;
        }
        SCOPED_TRACE("PID " + std::to_string(pid));
        const unsigned bytes = bytes_of(pid);
        ASSERT_NE(bytes, 0u) << "the table lacks it";
        const unsigned char req[2] = {0x01, static_cast<unsigned char>(pid)};
        unsigned char out[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        ASSERT_TRUE(fake_ecu::answer(req, sizeof(req), -1, out, &n));
        EXPECT_EQ(out[0], 0x41);
        EXPECT_EQ(out[1], pid);
        EXPECT_EQ(n, 2u + bytes);
    }
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        EXPECT_TRUE(fake_ecu::supports_pid(set->defs[i].mux_key & 0xFFu))
            << set->defs[i].name;
    }
}

/* Core's byte count per PID, which splits a packed reply, is the
 * table's for every PID it lists and 0 for every other */
TEST_F(Obd2TableTest, CoresByteCountPerPidIsTheTables)
{
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        EXPECT_EQ(ECUIO_OBD2_PID_BYTES[pid], bytes_of(pid)) << "PID " << pid;
    }
}

/* ---------- a packed reply's records (obd2_reply.h) ---------- */

namespace {

struct Split {
    ecuio_obd2_reply reply;
    std::vector<std::pair<unsigned, std::vector<uint8_t>>> records;
};

/* What ecuio_obd2_split makes of bytes, each record as its PID and data */
Split split_of(std::initializer_list<uint8_t> bytes, uint8_t* learned)
{
    const std::vector<uint8_t> reply(bytes);
    ecuio_obd2_record out[ECUIO_OBD2_MAX_RECORDS];
    uint32_t n = 99;
    Split s;
    s.reply = ecuio_obd2_split(reply.data(), static_cast<uint16_t>(reply.size()),
                               learned, out, &n);
    for (uint32_t k = 0; k < n; ++k) {
        s.records.push_back(
            {out[k].pid, std::vector<uint8_t>(reply.begin() + out[k].start,
                                              reply.begin() + out[k].start +
                                                  out[k].bytes)});
    }
    return s;
}

using Rec = std::pair<unsigned, std::vector<uint8_t>>;

} // namespace

/* ADR-0039 Appendix A: a reply no longer than its first PID's data is
 * that PID's alone and decodes as it came; a longer one splits into its
 * records by J1979's lengths, in the order the ECU sent them */
TEST(Obd2ReplyTest, APackedReplySplitsIntoItsRecords)
{
    uint8_t learned[256] = {};
    EXPECT_EQ(split_of({0x41, 0x0C, 0x1A, 0xF8}, learned).reply,
              ecuio_obd2_reply::whole) << "one PID";
    const Split s =
        split_of({0x41, 0x0D, 0x32, 0x0C, 0x1A, 0xF8, 0x05, 0x5A}, learned);
    EXPECT_EQ(s.reply, ecuio_obd2_reply::split);
    EXPECT_EQ(s.records, (std::vector<Rec>{{0x0D, {0x32}},
                                           {0x0C, {0x1A, 0xF8}},
                                           {0x05, {0x5A}}}));
    EXPECT_EQ(split_of({0x41, 0x00, 1, 2, 3, 4, 0x20, 5, 6, 7, 8}, learned)
                  .records.size(),
              2u) << "the walk's bitmaps too";
    EXPECT_EQ(split_of({0x7F, 0x01, 0x12}, learned).reply,
              ecuio_obd2_reply::whole) << "not a positive reply";
    EXPECT_EQ(split_of({0x41, 0x0C}, learned).reply, ecuio_obd2_reply::whole);
    EXPECT_EQ(split_of({0x41, 0xB0, 1, 2, 3}, learned).reply,
              ecuio_obd2_reply::whole) << "a first PID of no known length";
}

/* A reply that won't split decodes its first record alone: a PID of no
 * known length, one twice, a record past the end or short of it */
TEST(Obd2ReplyTest, ARecordThatWontSplitStopsIt)
{
    uint8_t learned[256] = {};
    for (const auto& bytes : {
             std::vector<uint8_t>{0x41, 0x0C, 0x1A, 0xF8, 0xB0, 0x01},
             std::vector<uint8_t>{0x41, 0x0D, 0x32, 0x0D, 0x33},
             std::vector<uint8_t>{0x41, 0x0D, 0x32, 0x0C, 0x1A},
             std::vector<uint8_t>{0x41, 0x0D, 0x32, 0x05, 0x5A, 0x0C}}) {
        ecuio_obd2_record out[ECUIO_OBD2_MAX_RECORDS];
        uint32_t n = 99;
        EXPECT_EQ(ecuio_obd2_split(bytes.data(), static_cast<uint16_t>(bytes.size()),
                                   learned, out, &n),
                  ecuio_obd2_reply::unsplit) << bytes.size();
        EXPECT_EQ(n, 0u);
    }
    std::vector<uint8_t> many = {0x41};
    for (unsigned pid = 0x04; pid <= 0xFF && many.size() < 3 + 2 * ECUIO_OBD2_MAX_RECORDS;
         ++pid) {
        if (ECUIO_OBD2_PID_BYTES[pid] == 1) {
            many.push_back(static_cast<uint8_t>(pid));
            many.push_back(0x00);
        }
    }
    ASSERT_GT(many.size(), 1 + 2 * ECUIO_OBD2_MAX_RECORDS);
    ecuio_obd2_record out[ECUIO_OBD2_MAX_RECORDS];
    uint32_t n = 0;
    EXPECT_EQ(ecuio_obd2_split(many.data(), static_cast<uint16_t>(many.size()),
                               learned, out, &n),
              ecuio_obd2_reply::unsplit) << "past the most records";
}

/* The test car answers PID 55 with bank 1's byte alone
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md): a single reply
 * shorter than J1979's teaches the length, so a packed reply holding 55
 * splits; a reply as long as J1979's teaches the table's again */
TEST(Obd2ReplyTest, AShortSingleReplyTeachesItsLength)
{
    uint8_t learned[256] = {};
    const auto packed = [&learned]() {
        return split_of({0x41, 0x0C, 0x1A, 0xF8, 0x55, 0x80, 0x0D, 0x32},
                        learned);
    };
    EXPECT_EQ(packed().reply, ecuio_obd2_reply::unsplit)
        << "by J1979's length, 55 takes 0D's PID";

    EXPECT_EQ(split_of({0x41, 0x55, 0x80}, learned).reply,
              ecuio_obd2_reply::whole);
    EXPECT_EQ(learned[0x55], 1u);
    const Split s = packed();
    EXPECT_EQ(s.reply, ecuio_obd2_reply::split);
    EXPECT_EQ(s.records, (std::vector<Rec>{{0x0C, {0x1A, 0xF8}},
                                           {0x55, {0x80}},
                                           {0x0D, {0x32}}}));

    EXPECT_EQ(split_of({0x41, 0x55, 0x80, 0x7F}, learned).reply,
              ecuio_obd2_reply::whole) << "both banks after all";
    EXPECT_EQ(learned[0x55], 0u) << "the table's length again";
    EXPECT_EQ(split_of({0x41, 0x0C, 0x1A}, learned).reply,
              ecuio_obd2_reply::whole);
    EXPECT_EQ(learned[0x0C], 1u) << "any PID's";
}

/* ---------- the fakes' ECU: diagnostics (fake_ecu.h) ---------- */

namespace {

/* The ECU's answer to req, or {} when it stays silent */
std::vector<uint8_t> ecu(std::initializer_list<uint8_t> req)
{
    const std::vector<uint8_t> r(req);
    unsigned char out[fake_ecu::kReplyMax] = {};
    size_t n = 0;
    if (!fake_ecu::answer(r.data(), r.size(), -1, out, &n)) {
        return {};
    }
    return std::vector<uint8_t>(out, out + n);
}

/* A DTC's five characters, from SAE J1979's layout: A7-A6 the category
 * (P C B U), A5-A4 the first digit, then a hex digit per nibble */
std::string dtc(uint8_t a, uint8_t b)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string s(1, "PCBU"[a >> 6]);
    s += kHex[(a >> 4) & 0x3];
    s += kHex[a & 0xF];
    s += kHex[b >> 4];
    s += kHex[b & 0xF];
    return s;
}

/* A list reply's DTCs, after checking its count */
std::vector<std::string> dtcs(const std::vector<uint8_t>& reply)
{
    std::vector<std::string> out;
    if (reply.size() < 2 || reply.size() != 2u + 2u * reply[1]) {
        ADD_FAILURE() << "a list reply of " << reply.size() << " bytes";
        return out;
    }
    for (size_t i = 2; i + 1 < reply.size(); i += 2) {
        out.push_back(dtc(reply[i], reply[i + 1]));
    }
    return out;
}

using Codes = std::vector<std::string>;
using Bytes = std::vector<uint8_t>;

class FakeEcuTest : public ::testing::Test
{
protected:
    void SetUp() override { fake_ecu::reset_diagnostics(); }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

} // namespace

/* A module answers Service 0x22 from its own rows and refuses the rest;
 * an ECU the table doesn't name answers as the engine ECU; a malformed
 * spec leaves the table empty */
TEST_F(FakeEcuTest, AModuleAnswersItsOwnDidsAndRefusesTheRest)
{
    struct Empty {
        ~Empty() { fake_ecu::set_module_dids(nullptr); }
    } empty;
    const auto ask = [](uint32_t rx, Bytes req, long nrc = -1) {
        uint8_t out[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        if (!fake_ecu::answer_for(rx, req.data(), req.size(), nrc, out, &n)) {
            return Bytes{};
        }
        return Bytes(out, out + n);
    };
    ASSERT_TRUE(fake_ecu::set_module_dids(
        "7E9:D801=00 12; 7e9:d802=ABCDEF;18DAF1E9:0102=01"));
    EXPECT_EQ(fake_ecu::module_table().count.load(), 3u);
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x01}),
              (Bytes{0x62, 0xD8, 0x01, 0x00, 0x12}));
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x02}),
              (Bytes{0x62, 0xD8, 0x02, 0xAB, 0xCD, 0xEF}))
        << "lower case reads the same";
    EXPECT_EQ(ask(0x18DAF1E9, {0x22, 0x01, 0x02}),
              (Bytes{0x62, 0x01, 0x02, 0x01}))
        << "a 29-bit reply ID";
    EXPECT_EQ(ask(0x7E9, {0x22, 0x1E, 0x1C}), (Bytes{0x7F, 0x22, 0x31}))
        << "the engine ECU's DID isn't the module's";
    EXPECT_EQ(ask(0x7E9, {0x01, 0x00}), (Bytes{0x7F, 0x01, 0x11}))
        << "a module serves only 0x22";
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x03}, 0), Bytes{})
        << "silence asked for";
    EXPECT_EQ(ask(0x7E9, {0x22, 0xD8, 0x03}, 0x33), (Bytes{0x7F, 0x22, 0x33}));
    EXPECT_EQ(ask(0x7E8, {0x22, 0x1E, 0x1C}),
              (Bytes{0x62, 0x1E, 0x1C, 0x12, 0x34}))
        << "an ECU the table doesn't name answers as the engine ECU";
    EXPECT_EQ(ask(0x7E8, {0x22, 0xD8, 0x01}), (Bytes{0x7F, 0x22, 0x31}));

    for (const char* bad :
         {"7E9", "7E9:D801", "7E9:D801=", "7E9:D801=1", "7E9:D801=0G",
          ":D801=00", "7E9:=00", "7E9:1D8010=00", "123456789:D801=00",
          "7E9:D801=00112233445566778899AABBCCDDEEFF00"}) {
        SCOPED_TRACE(bad);
        EXPECT_FALSE(fake_ecu::set_module_dids(bad));
        EXPECT_EQ(fake_ecu::module_table().count.load(), 0u)
            << "a bad spec empties it";
    }
    std::string many;
    for (int i = 0; i < 17; ++i) {
        many += "7E9:D8" + std::string(i < 16 ? "0" : "1") +
                "0123456789ABCDEF"[i % 16] + "=01;";
    }
    EXPECT_FALSE(fake_ecu::set_module_dids(many.c_str())) << "17 rows";
    ASSERT_TRUE(fake_ecu::set_module_dids(""));
    EXPECT_EQ(ask(0x7E9, {0x22, 0x1E, 0x1C}),
              (Bytes{0x62, 0x1E, 0x1C, 0x12, 0x34}))
        << "with the table empty every ECU is the engine ECU";
}

/* The lists cover all four categories; three stored DTCs make an
 * 8-byte reply, past a Single Frame */
TEST_F(FakeEcuTest, TheDtcListsAndTheMilBeforeAClear)
{
    const Bytes stored = ecu({0x03});
    ASSERT_EQ(stored.size(), 8u);
    EXPECT_EQ(stored[0], 0x43);
    EXPECT_EQ(dtcs(stored), (Codes{"P0301", "P0420", "U0100"}));
    const Bytes pending = ecu({0x07});
    ASSERT_FALSE(pending.empty());
    EXPECT_EQ(pending[0], 0x47);
    EXPECT_EQ(dtcs(pending), (Codes{"P0171", "C0035", "B1001"}));
    const Bytes permanent = ecu({0x0A});
    ASSERT_FALSE(permanent.empty());
    EXPECT_EQ(permanent[0], 0x4A);
    EXPECT_EQ(dtcs(permanent), (Codes{"P0420"}));

    EXPECT_EQ(ecu({0x01, 0x01}), (Bytes{0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}))
        << "MIL on, 3 DTCs; every monitor complete";
    EXPECT_EQ(ecu({0x01, 0x41}), (Bytes{0x41, 0x41, 0x00, 0x07, 0xE5, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x21}), (Bytes{0x41, 0x21, 0x00, 0x2A}));
    EXPECT_EQ(ecu({0x01, 0x30}), (Bytes{0x41, 0x30, 20}));
    EXPECT_EQ(ecu({0x01, 0x31}), (Bytes{0x41, 0x31, 0x04, 0xD2}));
    EXPECT_EQ(ecu({0x01, 0x4D}), (Bytes{0x41, 0x4D, 0x00, 0x3C}));
    EXPECT_EQ(ecu({0x01, 0x4E}), (Bytes{0x41, 0x4E, 0x0B, 0xB8}));
}

TEST_F(FakeEcuTest, TheFreezeFrameHoldsItsOwnValues)
{
    EXPECT_EQ(ecu({0x02, 0x00, 0x00}),
              (Bytes{0x42, 0x00, 0x00, 0x58, 0x18, 0x00, 0x00}))
        << "PIDs 02 04 05 0C 0D";
    const Bytes cause = ecu({0x02, 0x02, 0x00});
    ASSERT_EQ(cause.size(), 5u);
    EXPECT_EQ(dtc(cause[3], cause[4]), "P0301");
    EXPECT_EQ(ecu({0x02, 0x0C, 0x00}), (Bytes{0x42, 0x0C, 0x00, 0x2E, 0xE0}))
        << "3000 RPM";
    EXPECT_EQ(ecu({0x01, 0x0C}), (Bytes{0x41, 0x0C, 0x1F, 0x40}))
        << "the live value differs";
    EXPECT_EQ(ecu({0x02, 0x0C, 0x01}), (Bytes{0x7F, 0x02, 0x12}))
        << "only frame 0";
    EXPECT_EQ(ecu({0x02, 0x0F, 0x00}), (Bytes{0x7F, 0x02, 0x12}))
        << "not in the frame";
}

/* What J1979 lists a clear as resetting, reset; the permanent DTC
 * stays */
TEST_F(FakeEcuTest, AClearEmptiesTheListsAndResetsTheMonitors)
{
    EXPECT_EQ(ecu({0x04}), (Bytes{0x44}));
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
    EXPECT_EQ(ecu({0x03}), (Bytes{0x43, 0x00}));
    EXPECT_EQ(ecu({0x07}), (Bytes{0x47, 0x00}));
    EXPECT_EQ(dtcs(ecu({0x0A})), (Codes{"P0420"}));
    EXPECT_EQ(ecu({0x01, 0x01}), (Bytes{0x41, 0x01, 0x00, 0x07, 0xE5, 0xE5}))
        << "MIL off; C's monitors incomplete";
    EXPECT_EQ(ecu({0x01, 0x41}), (Bytes{0x41, 0x41, 0x00, 0x07, 0xE5, 0xE5}));
    EXPECT_EQ(ecu({0x01, 0x21}), (Bytes{0x41, 0x21, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x30}), (Bytes{0x41, 0x30, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x31}), (Bytes{0x41, 0x31, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x4D}), (Bytes{0x41, 0x4D, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x01, 0x4E}), (Bytes{0x41, 0x4E, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x02, 0x00, 0x00}),
              (Bytes{0x42, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}));
    EXPECT_EQ(ecu({0x02, 0x02, 0x00}), (Bytes{0x42, 0x02, 0x00, 0x00, 0x00}))
        << "J1979: 00 00 when no freeze frame is stored";
    EXPECT_EQ(ecu({0x02, 0x0C, 0x00}), (Bytes{0x7F, 0x02, 0x12}));
    EXPECT_EQ(ecu({0x01, 0x0C}), (Bytes{0x41, 0x0C, 0x1F, 0x40}))
        << "live data stays";
}

TEST_F(FakeEcuTest, ARefusedClearChangesNothing)
{
    fake_ecu::diagnostics().clear_nrc = 0x22; /* conditionsNotCorrect */
    EXPECT_EQ(ecu({0x04}), (Bytes{0x7F, 0x04, 0x22}));
    EXPECT_FALSE(fake_ecu::diagnostics().cleared.load());
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
    EXPECT_EQ(dtcs(ecu({0x03})).size(), 3u);
}

TEST_F(FakeEcuTest, OtherRequestShapesGetNoAnswer)
{
    EXPECT_TRUE(ecu({0x05}).empty()) << "Service 05 isn't on CAN";
    EXPECT_TRUE(ecu({0x03, 0x00}).empty());
    EXPECT_TRUE(ecu({0x04, 0x00}).empty());
    EXPECT_TRUE(ecu({0x02, 0x0C}).empty()) << "no frame byte";
    EXPECT_FALSE(fake_ecu::diagnostics().cleared.load());
}

namespace {

/* A row of tests/fixtures/obd2/monitors.csv */
struct MonitorRow {
    int line = 0;
    std::string engine;    /* any, spark, compression */
    std::string available; /* B0..B2, C0..C7 */
    std::string complete;  /* B4..B6, D0..D7: set while not complete */
    unsigned id = 0;
    std::string name;
    std::string short_name;
};

constexpr const char* kMonitorHeader =
    "engine,available,complete,id,name,short,note";

std::vector<MonitorRow> load_monitors(std::string* why)
{
    std::vector<MonitorRow> rows;
    std::ifstream in(ECUIO_OBD2_MONITORS, std::ios::binary);
    if (!in) {
        *why = "can't open " ECUIO_OBD2_MONITORS;
        return {};
    }
    std::string text;
    int n = 0;
    bool header = false;
    while (std::getline(in, text)) {
        ++n;
        if (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }
        if (text.empty() || text[0] == '#') {
            continue;
        }
        if (!header) {
            if (text != kMonitorHeader) {
                *why = "line " + std::to_string(n) + " isn't the header";
                return {};
            }
            header = true;
            continue;
        }
        std::vector<std::string> f;
        size_t from = 0;
        for (size_t comma; (comma = text.find(',', from)) != std::string::npos;
             from = comma + 1) {
            f.push_back(text.substr(from, comma - from));
        }
        f.push_back(text.substr(from));
        if (f.size() != 7) {
            *why = "line " + std::to_string(n) + " has " +
                   std::to_string(f.size()) + " fields, not 7";
            return {};
        }
        MonitorRow r;
        r.line = n;
        r.engine = f[0];
        r.available = f[1];
        r.complete = f[2];
        r.id = static_cast<unsigned>(std::strtoul(f[3].c_str(), nullptr, 10));
        r.name = f[4];
        r.short_name = f[5];
        rows.push_back(r);
    }
    return rows;
}

/* PID 01's four bytes with one monitor's availability bit set, and its
 * completeness bit too when incomplete; B3 set for a compression row */
std::vector<uint8_t> readiness_bytes(const MonitorRow& r, bool incomplete)
{
    std::vector<uint8_t> abcd(4, 0);
    const auto bit = [](const std::string& s) {
        return static_cast<unsigned>(s[1] - '0');
    };
    abcd[r.available[0] == 'B' ? 1 : 2] |=
        static_cast<uint8_t>(1u << bit(r.available));
    if (incomplete) {
        abcd[r.complete[0] == 'B' ? 1 : 3] |=
            static_cast<uint8_t>(1u << bit(r.complete));
    }
    if (r.engine == "compression") {
        abcd[1] |= 0x08;
    }
    return abcd;
}

uint32_t flags_for(const MonitorRow& r)
{
    return r.engine == "compression" ? ECUIO_DIAG_COMPRESSION : 0u;
}

class DiagnosticsTest : public ::testing::Test
{
protected:
    void SetUp() override { fake_ecu::reset_diagnostics(); }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

} // namespace

/* Every code, against the test's own reading of J1979's layout */
TEST_F(DiagnosticsTest, ACodeIsItsLetterAndFourDigits)
{
    char out[6];
    const std::pair<uint16_t, const char*> kKnown[] = {
        {0x0301, "P0301"}, {0x4035, "C0035"}, {0x9001, "B1001"},
        {0xC100, "U0100"}, {0x0000, "P0000"}, {0xFFFF, "U3FFF"},
        {0x3ABC, "P3ABC"},
    };
    for (const auto& k : kKnown) {
        ASSERT_EQ(ecuio_dtc_format(k.first, out, sizeof out), ECUIO_OK);
        EXPECT_STREQ(out, k.second);
    }
    for (uint32_t code = 0; code <= 0xFFFF; ++code) {
        ASSERT_EQ(
            ecuio_dtc_format(static_cast<uint16_t>(code), out, sizeof out),
            ECUIO_OK);
        ASSERT_EQ(std::string(out), dtc(static_cast<uint8_t>(code >> 8),
                                        static_cast<uint8_t>(code)))
            << std::hex << code;
    }
    EXPECT_EQ(ecuio_dtc_format(0x0301, nullptr, 6), ECUIO_ERR_NULL_ARGUMENT);
    std::memset(out, 'x', sizeof out);
    EXPECT_EQ(ecuio_dtc_format(0x0301, out, 5), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out[0], 'x') << "out untouched";
}

/* core's table is the fixture's, row for row, and nothing else has a
 * name: ids 3-7 (byte B's other bits) and compression's C2 and C4 */
TEST_F(DiagnosticsTest, EveryMonitorIsNamedAsTheTableSays)
{
    std::string why;
    const std::vector<MonitorRow> rows = load_monitors(&why);
    ASSERT_FALSE(rows.empty()) << why;
    EXPECT_EQ(rows.size(), sizeof(ECUIO_MONITORS) / sizeof(ECUIO_MONITORS[0]));
    for (const MonitorRow& r : rows) {
        SCOPED_TRACE("monitors.csv line " + std::to_string(r.line));
        ASSERT_LT(r.id, 16u);
        const bool in_b = r.id < 8;
        const unsigned bit = in_b ? r.id : r.id - 8;
        EXPECT_EQ(r.available, (in_b ? "B" : "C") + std::to_string(bit))
            << "id is the availability bit's place";
        EXPECT_EQ(r.complete, in_b ? "B" + std::to_string(bit + 4)
                                   : "D" + std::to_string(bit));
        const std::vector<uint32_t> engines =
            (r.engine == "any")
                ? std::vector<uint32_t>{0u, ECUIO_DIAG_COMPRESSION}
                : std::vector<uint32_t>{flags_for(r)};
        for (uint32_t f : engines) {
            EXPECT_STREQ(ecuio_monitor_name(static_cast<uint8_t>(r.id), f),
                         r.name.c_str());
            EXPECT_STREQ(
                ecuio_monitor_short_name(static_cast<uint8_t>(r.id), f),
                r.short_name.c_str());
        }
    }
    for (uint32_t f : {0u, ECUIO_DIAG_COMPRESSION}) {
        for (unsigned id = 0; id < 256; ++id) {
            bool listed = false;
            for (const MonitorRow& r : rows) {
                listed = listed || (r.id == id &&
                                    (r.engine == "any" || flags_for(r) == f));
            }
            if (!listed) {
                EXPECT_STREQ(ecuio_monitor_name(static_cast<uint8_t>(id), f),
                             "")
                    << id << " flags " << f;
                EXPECT_STREQ(
                    ecuio_monitor_short_name(static_cast<uint8_t>(id), f), "");
            }
        }
    }
}

/* Each monitor's two bits, alone, through the parser: the fixture's
 * columns and PID 01's layout agree */
TEST_F(DiagnosticsTest, EachMonitorReadsFromItsOwnBits)
{
    std::string why;
    const std::vector<MonitorRow> rows = load_monitors(&why);
    ASSERT_FALSE(rows.empty()) << why;
    for (const MonitorRow& r : rows) {
        SCOPED_TRACE("monitors.csv line " + std::to_string(r.line));
        for (bool incomplete : {false, true}) {
            ecuio_channel_diagnostics_t d{};
            ecuio_diag_take_readiness(readiness_bytes(r, incomplete).data(),
                                      &d);
            ASSERT_EQ(d.monitor_count, 1u);
            EXPECT_EQ(d.monitors[0].id, r.id);
            EXPECT_EQ(d.monitors[0].flags,
                      incomplete ? 0u : ECUIO_MONITOR_COMPLETE);
            EXPECT_EQ(d.flags & ECUIO_DIAG_COMPRESSION, flags_for(r));
        }
    }
}

/* Every availability bit set: the reserved ones and the engine-type bit
 * name no monitor */
TEST_F(DiagnosticsTest, ReservedBitsNameNoMonitor)
{
    const uint8_t spark[4] = {0x00, 0x07 | 0x80, 0xFF, 0x00};
    ecuio_channel_diagnostics_t d{};
    ecuio_diag_take_readiness(spark, &d);
    EXPECT_EQ(d.monitor_count, 11u) << "3 common and 8 spark";

    const uint8_t compression[4] = {0x00, 0x0F, 0xFF, 0xFF};
    d = ecuio_channel_diagnostics_t{};
    ecuio_diag_take_readiness(compression, &d);
    ASSERT_EQ(d.monitor_count, 9u) << "3 common and 6: C2 and C4 are reserved";
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_NE(d.monitors[i].id, 10u);
        EXPECT_NE(d.monitors[i].id, 12u);
        EXPECT_EQ(d.monitors[i].flags, i < 3 ? ECUIO_MONITOR_COMPLETE : 0u)
            << "D set: C's monitors incomplete";
    }
}

/* The fake ECU's PID 01 before and after a clear (J1979: the clear
 * leaves each monitor incomplete) */
TEST_F(DiagnosticsTest, ReadinessFromTheFakeEcu)
{
    const Bytes before = ecu({0x01, 0x01});
    ASSERT_EQ(before, (Bytes{0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}));
    ecuio_channel_diagnostics_t d{};
    ecuio_diag_take_readiness(before.data() + 2, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_READINESS | ECUIO_DIAG_MIL_ON);
    EXPECT_EQ(d.dtc_count, 3u);
    const uint8_t kIds[] = {0, 1, 2, 8, 10, 13, 14, 15};
    ASSERT_EQ(d.monitor_count, sizeof kIds);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].id, kIds[i]);
        EXPECT_EQ(d.monitors[i].flags, ECUIO_MONITOR_COMPLETE) << int{kIds[i]};
    }

    ASSERT_EQ(ecu({0x04}), (Bytes{0x44}));
    const Bytes after = ecu({0x01, 0x01});
    ecuio_diag_take_readiness(after.data() + 2, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_READINESS) << "the MIL off";
    EXPECT_EQ(d.dtc_count, 0u);
    ASSERT_EQ(d.monitor_count, sizeof kIds);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].flags,
                  kIds[i] < 8 ? ECUIO_MONITOR_COMPLETE : 0u)
            << int{kIds[i]};
    }
}

/* PID 41 onto each monitor PID 01 listed, from that monitor's own two
 * bits: enabled this drive cycle or not, complete this cycle or not;
 * PID 01's completeness stays as it was */
TEST_F(DiagnosticsTest, EachMonitorReadsThisDriveCycleFromItsOwnBits)
{
    std::string why;
    const std::vector<MonitorRow> rows = load_monitors(&why);
    ASSERT_FALSE(rows.empty()) << why;
    for (const MonitorRow& r : rows) {
        SCOPED_TRACE("monitors.csv line " + std::to_string(r.line));
        for (bool enabled : {false, true}) {
            for (bool incomplete : {false, true}) {
                ecuio_channel_diagnostics_t d{};
                ecuio_diag_take_readiness(readiness_bytes(r, true).data(), &d);
                ASSERT_EQ(d.monitor_count, 1u);
                std::vector<uint8_t> cycle = readiness_bytes(r, incomplete);
                if (!enabled) {
                    /* the availability bit off; B3, the engine type, stays */
                    const std::vector<uint8_t> lone = readiness_bytes(r, false);
                    cycle[1] =
                        static_cast<uint8_t>(cycle[1] & ~(lone[1] & ~0x08u));
                    cycle[2] = static_cast<uint8_t>(cycle[2] & ~lone[2]);
                }
                ecuio_diag_take_cycle(cycle.data(), &d);
                EXPECT_TRUE(d.flags & ECUIO_DIAG_CYCLE);
                EXPECT_EQ(d.monitors[0].flags,
                          (enabled ? ECUIO_MONITOR_CYCLE_ENABLED : 0u) |
                              (incomplete ? 0u : ECUIO_MONITOR_CYCLE_COMPLETE))
                    << "enabled " << enabled << " incomplete " << incomplete;
            }
        }
    }
}

/* A monitor PID 41 enables but PID 01 doesn't list stays out: the
 * report's monitors are the ones the ECU has */
TEST_F(DiagnosticsTest, ThisDriveCycleAddsNoMonitor)
{
    const uint8_t misfire[4] = {0x00, 0x01, 0x00, 0x00};
    ecuio_channel_diagnostics_t d{};
    ecuio_diag_take_readiness(misfire, &d);
    const uint8_t every[4] = {0x00, 0x07, 0xFF, 0x00};
    ecuio_diag_take_cycle(every, &d);
    ASSERT_EQ(d.monitor_count, 1u);
    EXPECT_EQ(d.monitors[0].flags, ECUIO_MONITOR_COMPLETE |
                                       ECUIO_MONITOR_CYCLE_ENABLED |
                                       ECUIO_MONITOR_CYCLE_COMPLETE);
}

/* The count as the ECU said it, and the codes the list and the reply
 * both hold */
TEST_F(DiagnosticsTest, ACodeListHoldsWhatTheReplyCarries)
{
    ecuio_dtc_list_t l{};
    const Bytes stored = ecu({0x03});
    ecuio_diag_take_codes(stored.data(), static_cast<uint32_t>(stored.size()),
                          &l);
    EXPECT_EQ(l.reported, 3u);
    ASSERT_EQ(l.held, 3u);
    EXPECT_EQ(l.codes[0], 0x0301u);
    EXPECT_EQ(l.codes[1], 0x0420u);
    EXPECT_EQ(l.codes[2], 0xC100u);

    const uint8_t none[] = {0x43, 0x00};
    ecuio_diag_take_codes(none, sizeof none, &l);
    EXPECT_EQ(l.reported, 0u);
    EXPECT_EQ(l.held, 0u);

    const uint8_t cut[] = {0x47, 0x03, 0x01, 0x71, 0x40, 0x35, 0x90};
    ecuio_diag_take_codes(cut, sizeof cut, &l);
    EXPECT_EQ(l.reported, 3u);
    EXPECT_EQ(l.held, 2u) << "the reply ends inside the third";

    Bytes many{0x4A, 40};
    for (uint8_t i = 0; i < 40; ++i) {
        many.push_back(0x01);
        many.push_back(i);
    }
    ecuio_diag_take_codes(many.data(), static_cast<uint32_t>(many.size()), &l);
    EXPECT_EQ(l.reported, 40u);
    ASSERT_EQ(l.held, ECUIO_DIAG_MAX_DTCS);
    EXPECT_EQ(l.codes[ECUIO_DIAG_MAX_DTCS - 1], 0x011Fu);

    const uint8_t bare[] = {0x43};
    ecuio_diag_take_codes(bare, sizeof bare, &l);
    EXPECT_EQ(l.reported, 0u);
    EXPECT_EQ(l.held, 0u);
}

namespace {

/* One request the engine sent, and whether it went through the
 * one-shot */
struct SentRequest {
    Bytes bytes;
    bool once = false;
};

/* An answer for a request: its reply, or {} for silence */
using Responder = std::function<Bytes(const Bytes&)>;

Bytes fake_reply(const Bytes& req)
{
    unsigned char out[fake_ecu::kReplyMax] = {};
    size_t n = 0;
    if (!fake_ecu::answer(req.data(), req.size(), -1, out, &n)) {
        return {};
    }
    return Bytes(out, out + n);
}

/* Drives channel 0 until its one-shots (walk, identification,
 * diagnostics, a clear, scan) have all settled, answering each request through
 * respond; a silent one waits out its wait. Returns what went out, in
 * order. */
std::vector<SentRequest> drive(ecuio_request_engine* e, int64_t* now,
                               const Responder& respond)
{
    std::vector<SentRequest> sent;
    const ecuio_engine_channel& ch = e->chans[0];
    for (int step = 0; step < 1000; ++step) {
        if (!ch.walking && ch.ident_pending == 0 && ch.diag_pending == 0 &&
            ch.scan_pending == 0 && ch.clear_step == ECUIO_CLEAR_STEP_NONE &&
            ch.outstanding == ECUIO_ENGINE_NONE) {
            return sent;
        }
        const int32_t r = ecuio_engine_due(e, 0, *now);
        if (r == ECUIO_ENGINE_NONE) {
            *now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        SentRequest s;
        s.bytes.assign(q.bytes, q.bytes + q.len);
        s.once = r == ECUIO_ENGINE_ONCE;
        sent.push_back(s);
        ecuio_engine_sent(e, 0, r, true, *now);
        const Bytes reply = respond(s.bytes);
        if (reply.empty()) {
            *now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(e, 0, *now));
        } else {
            EXPECT_NE(ecuio_engine_on_frame(e, 0, reply.data(),
                                            static_cast<uint16_t>(reply.size()),
                                            *now + 4 * kMs, nullptr),
                      ecuio_reply::none);
        }
        *now += ECUIO_ENGINE_GAP_US;
    }
    ADD_FAILURE() << "the one-shots never settled";
    return sent;
}

/* An engine with one channel, one rotation request (01 0C) and one
 * scanned DID, asked to probe, identify and diagnose */
void full_engine(ecuio_request_engine* e)
{
    ecuio_engine_init(e, 1);
    const uint8_t rpm[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(e, 0, rpm, 2), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(e, 0, 0x1E20));
    ecuio_engine_probe(e, 0);
    ecuio_engine_identify(e);
    ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
}

} // namespace

/* Against the fakes' ECU: the walk first, then identification, then
 * diagnostics, then the scan, each one-shot taking turns with the
 * rotation; the freeze frame's code and values follow its bitmap; the
 * report holds what the ECU said */
TEST_F(DiagnosticsTest, TheListTakesItsTurnAfterIdentificationAndFillsTheReport)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(full_engine(&e));
    int64_t now = 0;
    const std::vector<SentRequest> sent = drive(&e, &now, fake_reply);

    std::vector<Bytes> once;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (!sent[i].once) {
            continue;
        }
        once.push_back(sent[i].bytes);
        const bool walking =
            sent[i].bytes[0] == 0x01 && sent[i].bytes[1] % 0x20 == 0;
        if (!walking && i + 1 < sent.size()) {
            EXPECT_FALSE(sent[i + 1].once)
                << "the rotation's turn follows each one-shot";
        }
    }
    const std::vector<Bytes> kDiag = {
        {0x01, 0x01},
        {0x01, 0x41},
        {0x03},
        {0x07},
        {0x0A},
        {0x02, 0x00, 0x00},
        {0x02, 0x02, 0x00},
        {0x02, 0x04, 0x00},
        {0x02, 0x05, 0x00},
        {0x02, 0x0C, 0x00},
        {0x02, 0x0D, 0x00},
    };
    const size_t walk = 6; /* 01 00 to 01 A0 */
    ASSERT_EQ(once.size(), walk + ECUIO_IDENT_COUNT + kDiag.size() + 1);
    for (size_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
        EXPECT_EQ(once[walk + i][0], ECUIO_IDENT_REQUESTS[i].bytes[0]);
    }
    for (size_t i = 0; i < kDiag.size(); ++i) {
        EXPECT_EQ(once[walk + ECUIO_IDENT_COUNT + i], kDiag[i]) << i;
    }
    EXPECT_EQ(once.back(), (Bytes{0x22, 0x1E, 0x20})) << "the scan last";
    EXPECT_EQ(e.count, 1u) << "nothing joins the rotation";

    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(kDiag.size(), fake_diagnostics::kItems);
    EXPECT_EQ(d.generation, kDiag.size()) << "one per item settled";
    fake_diagnostics::expect_report(d);
}

/* An ECU whose bitmaps leave PID 41 out isn't asked it: the item
 * settles unsupported, unsent, and the rest of the report is the
 * fake's */
TEST_F(DiagnosticsTest, APid41TheBitmapsLeaveOutIsNotAsked)
{
    struct Withhold41 {
        Withhold41()
        {
            fake_ecu::withheld_pids()[0x41 / 32] |= 1u << (0x41 % 32);
        }
        ~Withhold41() { fake_ecu::withheld_pids()[0x41 / 32] = 0; }
    } withhold;
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(full_engine(&e));
    int64_t now = 0;
    for (const SentRequest& r : drive(&e, &now, fake_reply)) {
        EXPECT_NE(r.bytes, (Bytes{0x01, 0x41})) << "asked though unsupported";
    }
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.cycle, ECUIO_DIAG_UNSUPPORTED);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CYCLE);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.generation, fake_diagnostics::kItems) << "settled once";
    ASSERT_EQ(d.monitor_count, 8u);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].flags, ECUIO_MONITOR_COMPLETE);
    }
}

/* After a clear: no codes stored, the freeze frame's code 00 00, so no
 * values are asked; the permanent code stays */
TEST_F(DiagnosticsTest, NoFreezeFrameStoredAsksNoValues)
{
    ASSERT_EQ(ecu({0x04}), (Bytes{0x44}));
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    const std::vector<SentRequest> sent = drive(&e, &now, fake_reply);
    ASSERT_EQ(sent.size(), 7u);
    EXPECT_EQ(sent.back().bytes, (Bytes{0x02, 0x02, 0x00}));

    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE |
                           ECUIO_DIAG_READINESS | ECUIO_DIAG_CYCLE);
    EXPECT_EQ(d.dtc_count, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].held, 0u);
    ASSERT_EQ(d.lists[ECUIO_DIAG_PERMANENT].held, 1u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].codes[0], 0x0420u);
    EXPECT_EQ(d.freeze_dtc, 0u);
    EXPECT_EQ(d.freeze_count, 0u);
}

/* Every outcome: refused items keep their code, a silent one is
 * unanswered until a rescan asks it again, and the report is complete
 * only once nothing is unasked */
TEST_F(DiagnosticsTest, RefusalsAndSilenceSettleEachItem)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set()); /* idempotent */
    EXPECT_EQ(e.chans[0].diag_count, ECUIO_DIAG_FIRST_COUNT);

    const Responder refuse = [](const Bytes& req) -> Bytes {
        switch (req[0]) {
        case 0x01:
            return {0x7F, 0x01, 0x12};
        case 0x03:
            return {}; /* silent */
        case 0x07:
        case 0x0A:
            return {0x7F, req[0], 0x11};
        default:
            return {0x7F, 0x02, 0x12};
        }
    };
    int64_t now = 0;
    drive(&e, &now, refuse);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.readiness, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(d.readiness_nrc, 0x12u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].outcome, ECUIO_DIAG_UNANSWERED);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].outcome, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].nrc, 0x11u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].outcome, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(d.freeze, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(e.chans[0].diag_count, ECUIO_DIAG_FIRST_COUNT)
        << "a refused freeze frame asks nothing more";

    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.chans[0].diag_pending, 1u) << "only the silent one";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags & ECUIO_DIAG_COMPLETE, 0u);
    const std::vector<SentRequest> again = drive(&e, &now, fake_reply);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].bytes, (Bytes{0x03}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_NE(d.flags & ECUIO_DIAG_COMPLETE, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 3u);
}

/* 43, 47 and 4A carry no key: a reply of the request's service answers
 * it; another service's, or a negative one naming another, doesn't */
TEST_F(DiagnosticsTest, AOneByteRequestIsAnsweredByItsServiceAlone)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const Bytes readiness = fake_reply({0x01, 0x01});
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, readiness.data(), 6, now, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const Bytes cycle = fake_reply({0x01, 0x41});
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, cycle.data(), 6, now, nullptr),
              ecuio_reply::positive);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ASSERT_EQ(e.chans[0].once.len, 1u);
    ASSERT_EQ(e.chans[0].once.bytes[0], 0x03u);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const uint8_t other[] = {0x47, 0x00};
    const uint8_t other_negative[] = {0x7F, 0x07, 0x11};
    const uint8_t pending[] = {0x7F, 0x03, 0x78};
    const uint8_t mine[] = {0x43, 0x01, 0x03, 0x01}; /* one code: P0301 */
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other, sizeof other, now, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other_negative,
                                    sizeof other_negative, now, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, mine, 0, now, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(
        ecuio_engine_on_frame(&e, 0, pending, sizeof pending, now, nullptr),
        ecuio_reply::pending);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, mine, sizeof mine, now, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].diag.lists[ECUIO_DIAG_STORED].outcome,
              ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(e.chans[0].diag.lists[ECUIO_DIAG_STORED].reported, 1u);
    EXPECT_EQ(e.chans[0].diag.lists[ECUIO_DIAG_STORED].codes[0], 0x0301u);
}

/* Read again starts the list over once a read has settled, with a new
 * generation; mid-read, or on a channel never asked, it does nothing */
TEST_F(DiagnosticsTest, ReadAgainStartsOverOnlyOnceSettled)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    ecuio_engine_diagnose_again(&e);
    EXPECT_EQ(e.chans[0].diag_count, 0u) << "never asked: nothing to repeat";

    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    ecuio_engine_diagnose_again(&e);
    EXPECT_EQ(e.chans[0].diag_pending, ECUIO_DIAG_FIRST_COUNT)
        << "mid-read: left as it is";
    EXPECT_EQ(e.chans[0].diag.generation, 0u);

    int64_t now = 0;
    drive(&e, &now, fake_reply);
    ecuio_channel_diagnostics_t first;
    ecuio_engine_diagnostics(&e, 0, &first);
    ASSERT_NE(first.flags & ECUIO_DIAG_COMPLETE, 0u);

    ecuio_engine_diagnose_again(&e);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED) << "emptied, and asked again";
    EXPECT_EQ(d.generation, first.generation + 1);
    EXPECT_EQ(d.freeze_count, 0u);
    EXPECT_EQ(e.chans[0].diag_count, ECUIO_DIAG_FIRST_COUNT);
    EXPECT_EQ(e.chans[0].diag_pending, ECUIO_DIAG_FIRST_COUNT);

    drive(&e, &now, fake_reply);
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, first.flags);
    EXPECT_EQ(d.freeze_count, first.freeze_count);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held,
              first.lists[ECUIO_DIAG_STORED].held);
    EXPECT_GT(d.generation, first.generation);
}

namespace {

/* ADR-0038: the clear's gate and steps, against the fake ECU */
class ClearTest : public DiagnosticsTest
{};

/* The fake ECU with its engine stopped: 01 0C reads 0 rpm */
Bytes stopped_engine(const Bytes& req)
{
    if (req == Bytes{0x01, 0x0C}) {
        return {0x41, 0x0C, 0x00, 0x00};
    }
    return fake_reply(req);
}

/* An engine reading only diagnostics, read to the end through the fake;
 * the report it holds */
ecuio_channel_diagnostics_t read_report(ecuio_request_engine* e, int64_t* now)
{
    ecuio_engine_init(e, 1);
    ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
    drive(e, now, fake_reply);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(e, 0, &d);
    return d;
}

std::vector<Bytes> requests_of(const std::vector<SentRequest>& sent)
{
    std::vector<Bytes> out;
    for (const SentRequest& s : sent) {
        out.push_back(s.bytes);
    }
    return out;
}

const std::vector<Bytes> kReadAfterAClear = {
    {0x01, 0x01},       {0x01, 0x41},       {0x03}, {0x07}, {0x0A},
    {0x02, 0x00, 0x00}, {0x02, 0x02, 0x00},
};

} // namespace

/* At 0 rpm the clear sends 04 and reads the report again: the fake's
 * stored and pending codes and its freeze frame gone, the MIL off, its
 * catalyst-side monitors incomplete, the permanent code kept */
TEST_F(ClearTest, AStoppedEngineIsClearedAndReadAgain)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    ASSERT_EQ(d.clear, ECUIO_CLEAR_NONE);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_CLEARING);

    const std::vector<Bytes> sent =
        requests_of(drive(&e, &now, stopped_engine));
    std::vector<Bytes> expected = {{0x01, 0x0C}, {0x04}};
    expected.insert(expected.end(), kReadAfterAClear.begin(),
                    kReadAfterAClear.end());
    EXPECT_EQ(sent, expected) << "the speed, 04, then the list again";
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);

    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.clear_nrc, 0u);
    EXPECT_EQ(d.clear_rpm, 0u);
    EXPECT_EQ(d.flags & (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH),
              ECUIO_DIAG_CLEAR_RPM)
        << "0 rpm read; no vehicle speed asked without the caller's word";
    EXPECT_EQ(d.flags & (ECUIO_DIAG_COMPLETE | ECUIO_DIAG_CLEARING |
                         ECUIO_DIAG_MIL_ON | ECUIO_DIAG_FREEZE_FRAME),
              ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.dtc_count, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].held, 0u);
    ASSERT_EQ(d.lists[ECUIO_DIAG_PERMANENT].held, 1u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].codes[0], 0x0420u);
    EXPECT_EQ(d.freeze_count, 0u);
    uint32_t incomplete = 0;
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        incomplete += (d.monitors[i].flags & ECUIO_MONITOR_COMPLETE) ? 0 : 1;
    }
    EXPECT_EQ(d.monitor_count, 8u);
    EXPECT_EQ(incomplete, 5u) << "byte C's monitors start over";
}

/* A turning engine ends the clear before 04, with the speed it read
 * (the fake's 2000 rpm); the codes stay as they were */
TEST_F(ClearTest, ARunningEngineIsNotCleared)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    const uint32_t before = d.generation;
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    EXPECT_EQ(requests_of(drive(&e, &now, fake_reply)),
              (std::vector<Bytes>{{0x01, 0x0C}}));
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);

    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 2000u);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    EXPECT_GT(d.generation, before);
    fake_diagnostics::expect_report(d);
}

/* A speed that turns at all is running: 1 count (0.25 rpm) reads 1 rpm,
 * never 0 */
TEST_F(ClearTest, TheSlowestTurningEngineIsRunning)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    const Responder crawling = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0C} ? Bytes{0x41, 0x0C, 0x00, 0x01}
                                        : fake_reply(req);
    };
    EXPECT_EQ(requests_of(drive(&e, &now, crawling)).size(), 1u);
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 1u);
}

/* No speed, refused or unanswered, sends nothing more */
TEST_F(ClearTest, NoEngineSpeedSendsNothing)
{
    const std::pair<Bytes, uint8_t> kAnswers[] = {
        {Bytes{}, 0x00},                 /* silence */
        {Bytes{0x7F, 0x01, 0x12}, 0x12}, /* refused */
        {Bytes{0x41, 0x0C, 0x00}, 0x00}, /* cut short */
    };
    for (const auto& a : kAnswers) {
        SCOPED_TRACE(a.first.size());
        static ecuio_request_engine e;
        int64_t now = 0;
        ecuio_channel_diagnostics_t d = read_report(&e, &now);
        ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
        const Bytes answer = a.first;
        const Responder speed = [answer](const Bytes& req) {
            return req == Bytes{0x01, 0x0C} ? answer : fake_reply(req);
        };
        EXPECT_EQ(requests_of(drive(&e, &now, speed)),
                  (std::vector<Bytes>{{0x01, 0x0C}}));
        ecuio_engine_diagnostics(&e, 0, &d);
        EXPECT_EQ(d.clear, ECUIO_CLEAR_NO_ENGINE_SPEED);
        EXPECT_EQ(d.clear_nrc, a.second);
        EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    }
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
}

/* The caller's word on a running engine (ADR-0038's appendix of
 * 2026-11-01): the fake's 2000 rpm is recorded, not a gate; its 0 km/h
 * sends 04, and the report reads again as at a stopped engine */
TEST_F(ClearTest, ARunningEngineClearsWithTheCallersWord)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(
        ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, fake_reply));
    std::vector<Bytes> expected = {{0x01, 0x0C}, {0x01, 0x0D}, {0x04}};
    expected.insert(expected.end(), kReadAfterAClear.begin(),
                    kReadAfterAClear.end());
    EXPECT_EQ(sent, expected) << "both speeds, 04, then the list again";
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);

    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.clear_rpm, 2000u);
    EXPECT_EQ(d.clear_kmh, 0u);
    EXPECT_EQ(d.flags & (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH),
              ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH)
        << "both read, kept across the read after";
    fake_diagnostics::expect_cleared_report(d);
}

/* Moving, the caller's word on the engine isn't enough: the clear ends
 * at 01 0D with the speed it read, nothing sent; the word on a moving
 * vehicle, at the report's new generation, then clears */
TEST_F(ClearTest, AMovingVehicleNeedsTheCallersWordOnItToo)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    const Responder moving = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0D} ? Bytes{0x41, 0x0D, 0x30}
                                        : fake_reply(req);
    };
    ASSERT_TRUE(
        ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
    EXPECT_EQ(requests_of(drive(&e, &now, moving)),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_MOVING);
    EXPECT_EQ(d.clear_kmh, 48u);
    EXPECT_EQ(d.clear_rpm, 2000u);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEARING);
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);
    fake_diagnostics::expect_report(d);

    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation,
                                   ECUIO_CLEAR_ALLOW_RUNNING |
                                       ECUIO_CLEAR_ALLOW_MOVING));
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, moving));
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(std::vector<Bytes>(sent.begin(), sent.begin() + 3),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}, {0x04}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_EQ(d.clear_kmh, 48u) << "the speed it cleared at, recorded";
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
}

/* No vehicle speed, refused, unanswered or cut short, is no word that the
 * vehicle stands: nothing is sent, unless the caller allowed it moving */
TEST_F(ClearTest, NoVehicleSpeedSendsNothingWithoutTheCallersWord)
{
    const std::pair<Bytes, uint8_t> kAnswers[] = {
        {Bytes{}, 0x00},                 /* silence */
        {Bytes{0x7F, 0x01, 0x12}, 0x12}, /* refused */
        {Bytes{0x41, 0x0D}, 0x00},       /* cut short */
    };
    for (const auto& a : kAnswers) {
        SCOPED_TRACE(a.first.size());
        static ecuio_request_engine e;
        int64_t now = 0;
        ecuio_channel_diagnostics_t d = read_report(&e, &now);
        const Bytes answer = a.first;
        const Responder speed = [answer](const Bytes& req) {
            return req == Bytes{0x01, 0x0D} ? answer : fake_reply(req);
        };
        ASSERT_TRUE(
            ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
        EXPECT_EQ(requests_of(drive(&e, &now, speed)),
                  (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}}));
        ecuio_engine_diagnostics(&e, 0, &d);
        EXPECT_EQ(d.clear, ECUIO_CLEAR_NO_VEHICLE_SPEED);
        EXPECT_EQ(d.clear_nrc, a.second);
        EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEAR_KMH);
        EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);

        ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation,
                                       ECUIO_CLEAR_ALLOW_RUNNING |
                                           ECUIO_CLEAR_ALLOW_MOVING));
        drive(&e, &now, speed);
        ecuio_engine_diagnostics(&e, 0, &d);
        EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
        fake_ecu::reset_diagnostics();
    }
}

/* Under the caller's word on the engine, an ECU that gives no engine
 * speed (a transmission's, say) is asked the vehicle speed and cleared;
 * the report says no engine speed was read */
TEST_F(ClearTest, NoEngineSpeedClearsWithTheCallersWord)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    const Responder no_rpm = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0C} ? Bytes{} : fake_reply(req);
    };
    ASSERT_TRUE(
        ecuio_engine_clear(&e, 0, d.generation, ECUIO_CLEAR_ALLOW_RUNNING));
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, no_rpm));
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(std::vector<Bytes>(sent.begin(), sent.begin() + 3),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x01, 0x0D}, {0x04}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_CLEARED);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CLEAR_RPM);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_CLEAR_KMH);
}

/* A refusal (7F 04 22: the engine's conditions) changed nothing, so
 * nothing is read again */
TEST_F(ClearTest, ARefusedClearReadsNothingAgain)
{
    fake_ecu::diagnostics().clear_nrc = 0x22;
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    EXPECT_EQ(requests_of(drive(&e, &now, stopped_engine)),
              (std::vector<Bytes>{{0x01, 0x0C}, {0x04}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_REFUSED);
    EXPECT_EQ(d.clear_nrc, 0x22u);
    fake_diagnostics::expect_report(d);
}

/* 04 unanswered may still have cleared: the report is read again, which
 * here shows the codes kept */
TEST_F(ClearTest, AnUnansweredClearReadsTheReportAgain)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    const Responder deaf = [](const Bytes& req) {
        return req == Bytes{0x04} ? Bytes{} : stopped_engine(req);
    };
    const std::vector<Bytes> sent = requests_of(drive(&e, &now, deaf));
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(sent[1], (Bytes{0x04}));
    EXPECT_EQ(sent[2], (Bytes{0x01, 0x01})) << "the read again";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_UNANSWERED);
    fake_diagnostics::expect_report(d);
}

/* Only the report the caller saw is cleared: one never read, one mid-
 * read, or another generation records REPORT_CHANGED and sends nothing;
 * a channel past the count, or one already clearing, is left as it is */
TEST_F(ClearTest, OnlyTheReportTheCallerSawIsCleared)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_engine_init(&e, 1);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, 0)) << "no diagnostics read";
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_NONE);
    EXPECT_EQ(d.generation, 0u);

    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, 0)) << "mid-read";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_REPORT_CHANGED);
    for (const Bytes& b : requests_of(drive(&e, &now, stopped_engine))) {
        EXPECT_NE(b, (Bytes{0x04}));
        EXPECT_NE(b, (Bytes{0x01, 0x0C}));
    }

    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, d.generation - 1)) << "older";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_REPORT_CHANGED);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, d.generation + 1)) << "newer";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_FALSE(ecuio_engine_clear(&e, 1, d.generation)) << "no channel 1";
    ecuio_channel_diagnostics_t same;
    ecuio_engine_diagnostics(&e, 0, &same);
    EXPECT_EQ(same.generation, d.generation) << "channel 0 untouched";
    EXPECT_TRUE(requests_of(drive(&e, &now, stopped_engine)).empty());
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 0u);

    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_FALSE(ecuio_engine_clear(&e, 0, d.generation)) << "one at a time";
    ecuio_channel_diagnostics_t after;
    ecuio_engine_diagnostics(&e, 0, &after);
    EXPECT_EQ(after.generation, d.generation) << "the running one untouched";
    EXPECT_EQ(after.clear, ECUIO_CLEAR_NONE);
    drive(&e, &now, stopped_engine);
    EXPECT_EQ(fake_ecu::diagnostics().clears.load(), 1u);
}

/* A Read again waits for a clear under way, and a later one keeps the
 * last clear's outcome while the report starts over */
TEST_F(ClearTest, ReadAgainKeepsTheLastClear)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_channel_diagnostics_t d = read_report(&e, &now);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_diagnose_again(&e);
    ecuio_engine_rescan(&e);
    EXPECT_EQ(requests_of(drive(&e, &now, fake_reply)),
              (std::vector<Bytes>{{0x01, 0x0C}}))
        << "the clear alone, ended by the running engine";

    ecuio_engine_diagnose_again(&e);
    std::vector<Bytes> list = requests_of(drive(&e, &now, fake_reply));
    ASSERT_FALSE(list.empty());
    EXPECT_EQ(list.front(), (Bytes{0x01, 0x01}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.clear, ECUIO_CLEAR_ENGINE_RUNNING);
    EXPECT_EQ(d.clear_rpm, 2000u);
    fake_diagnostics::expect_report(d);
}

/* A rescan (the link back to ok) leaves a clearing channel's report as
 * the caller saved it: an item left unanswered isn't asked again until
 * the clear ends */
TEST_F(ClearTest, ARescanWaitsForTheClear)
{
    static ecuio_request_engine e;
    int64_t now = 0;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    const Responder no_pending = [](const Bytes& req) {
        return req == Bytes{0x07} ? Bytes{} : fake_reply(req);
    };
    drive(&e, &now, no_pending);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    ASSERT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    ASSERT_EQ(d.lists[ECUIO_DIAG_PENDING].outcome, ECUIO_DIAG_UNANSWERED);

    ASSERT_TRUE(ecuio_engine_clear(&e, 0, d.generation));
    ecuio_engine_rescan(&e);
    EXPECT_EQ(requests_of(drive(&e, &now, fake_reply)),
              (std::vector<Bytes>{{0x01, 0x0C}}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].outcome, ECUIO_DIAG_UNANSWERED);
}

/* ---------- several PIDs per request (ADR-0039 Appendix A) ---------- */

namespace {

/* single's answers, a pack answered in J1979's form: each PID's record
 * from its own reply, in the order asked, a PID it doesn't answer left
 * out */
Responder packed_from(Responder single)
{
    return [single](const Bytes& req) {
        if (req.size() <= 2 || req[0] != 0x01) {
            return single(req);
        }
        Bytes out = {0x41};
        for (size_t k = 1; k < req.size(); ++k) {
            const Bytes one = single({0x01, req[k]});
            if (one.size() >= 2 && one[0] == 0x41) {
                out.insert(out.end(), one.begin() + 1, one.end());
            }
        }
        return out.size() > 1 ? out : Bytes{};
    };
}

Bytes packed_fake_reply(const Bytes& req)
{
    return packed_from(fake_reply)(req);
}

/* n turns of channel 0's rotation after a walk of the test car's
 * bitmaps with packing on, each answered through respond (silence waits
 * out the wait); what went out, in order */
std::vector<Bytes> packing_turns(ecuio_request_engine* e, int n,
                                 const Responder& respond)
{
    std::vector<Bytes> sent;
    int64_t now = 0;
    for (int k = 0; k < n; ++k) {
        const int32_t r = ecuio_engine_due(e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        const Bytes req(q.bytes, q.bytes + q.len);
        sent.push_back(req);
        EXPECT_EQ(r == ECUIO_ENGINE_PACK, req.size() > 2);
        ecuio_engine_sent(e, 0, r, true, now);
        const Bytes reply = respond(req);
        if (reply.empty()) {
            now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(e, 0, now));
        } else {
            (void)ecuio_engine_on_frame(e, 0, reply.data(),
                                        static_cast<uint16_t>(reply.size()),
                                        now + 4 * kMs, nullptr);
        }
        now += ECUIO_ENGINE_GAP_US;
    }
    return sent;
}

/* An engine on the test car's bitmaps, walked, packing on */
void packing_engine(ecuio_request_engine* e)
{
    ASSERT_NO_FATAL_FAILURE(obd2_walked(e, kTestCar));
    e->pack = true;
}

bool slow_pid(const ecuio_request_engine& e, uint8_t pid)
{
    return class_of(e, pid) == 1;
}

} // namespace

/* A PID goes in a pack only once its own reply has shown its length,
 * so the first lap is unpacked; then every request carries up to six
 * kept PIDs of one class, never a bitmap, and every kept PID is still
 * asked */
TEST(RequestEngineTest, PacksCarryUpToSixKeptPidsOfOneClass)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    const std::vector<Bytes> sent = packing_turns(&e, 400, packed_fake_reply);
    std::vector<bool> answered_alone(256, false);
    size_t packs = 0;
    for (const Bytes& req : sent) {
        if (req.size() == 2) {
            answered_alone[req[1]] = true;
            continue;
        }
        ++packs;
        ASSERT_LE(req.size(), 1 + ECUIO_ENGINE_PACK_MAX);
        for (size_t k = 1; k < req.size(); ++k) {
            SCOPED_TRACE("PID " + std::to_string(req[k]));
            EXPECT_TRUE(answered_alone[req[k]]) << "packed before it went alone";
            EXPECT_NE(req[k] % 0x20, 0) << "a bitmap";
            EXPECT_EQ(slow_pid(e, req[k]), slow_pid(e, req[1])) << "one class";
        }
    }
    EXPECT_GT(packs, 0u);

    /* After the first laps: packs alone, every kept PID among them */
    std::vector<bool> asked(256, false);
    size_t singles = 0;
    for (size_t k = sent.size() - 60; k < sent.size(); ++k) {
        singles += sent[k].size() == 2 ? 1 : 0;
        for (size_t b = 1; b < sent[k].size(); ++b) {
            asked[sent[k][b]] = true;
        }
    }
    EXPECT_EQ(singles, 0u);
    for (uint32_t i = 0; i < e.count; ++i) {
        const ecuio_request& r = e.requests[i];
        if (r.dropped == 0) {
            EXPECT_TRUE(asked[r.bytes[1]]) << "PID " << int(r.bytes[1]);
        }
    }
    ecuio_channel_support_t s{};
    ecuio_engine_support(&e, 0, &s);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
    EXPECT_FALSE(s.flags & ECUIO_SUPPORT_UNPACKED);
}

/* A pack's reply length is its records' at the lengths their own
 * replies showed, a short one (the test car's PID 55) included; a PID
 * whose own reply was longer than J1979's is never packed, since the
 * split can't tell it from a packed reply */
TEST(RequestEngineTest, APackIsSizedByItsPidsOwnReplies)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    const auto car = [](const Bytes& req) {
        if (req == Bytes{0x01, 0x55}) {
            return Bytes{0x41, 0x55, 0x80};
        }
        if (req == Bytes{0x01, 0x0C}) {
            return Bytes{0x41, 0x0C, 0x1A, 0xF8, 0x00};
        }
        return req.size() > 2 ? Bytes{} : fake_reply(req);
    };
    (void)packing_turns(&e, 200, car);
    const ecuio_engine_channel& ch = e.chans[0];
    EXPECT_EQ(ch.pid_len[0x55], 1u);
    EXPECT_EQ(ch.pid_len[0x0C], ECUIO_ENGINE_PID_LONG);

    ecuio_request_engine f;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&f));
    const std::vector<Bytes> sent = packing_turns(&f, 400, packed_from(car));
    bool packed55 = false;
    for (const Bytes& req : sent) {
        for (size_t k = 1; req.size() > 2 && k < req.size(); ++k) {
            EXPECT_NE(req[k], 0x0C) << "a reply longer than J1979's: alone";
            packed55 = packed55 || req[k] == 0x55;
        }
    }
    EXPECT_TRUE(packed55);
    const ecuio_engine_channel& fc = f.chans[0];
    ASSERT_GT(fc.pack_count, 1u);
    uint32_t reply = 1;
    for (uint32_t k = 0; k < fc.pack_count; ++k) {
        reply += 1u + fc.pid_len[fc.pack.bytes[1 + k]];
    }
    EXPECT_EQ(fc.pack.reply_len, reply);
}

/* A reply whose first record is any PID the pack asked answers it, in
 * any order, and one that leaves a PID out drops nothing */
TEST(RequestEngineTest, APackedReplyMatchesByAnyOfItsPids)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    const auto reversed_less_one = [](const Bytes& req) {
        if (req.size() <= 3) {
            return packed_fake_reply(req);
        }
        Bytes back = {0x01};
        for (size_t k = req.size() - 1; k >= 2; --k) {
            back.push_back(req[k]); /* the first PID left out */
        }
        return packed_fake_reply(back);
    };
    (void)packing_turns(&e, 400, reversed_less_one);
    ecuio_channel_support_t s{};
    ecuio_engine_support(&e, 0, &s);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
    EXPECT_FALSE(s.flags & ECUIO_SUPPORT_UNPACKED);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_NE(e.requests[i].dropped, ECUIO_DROP_NOT_SUPPORTED)
            << "PID " << int(e.requests[i].bytes[1]);
    }
    EXPECT_EQ(e.chans[0].pack_failures, 0u);
}

/* Silence, a negative reply or one record, three packs in a row, puts
 * the channel back to one PID per request; one answered in between
 * starts the count again; a pending reply only extends the wait */
/* An ECU that goes quiet (the ignition off) leaves its packs unanswered
 * along with everything else, so the channel unpacks; once the link
 * recovers (the driver's rescan) packing is judged again and resumes */
TEST(RequestEngineTest, PackingResumesOnceTheLinkRecovers)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    int turn = 0;
    int packs_before = 0;
    int packs_after = 0;
    uint32_t quiet_flags = 0;
    (void)packing_turns(&e, 600, [&](const Bytes& req) {
        ++turn;
        if (turn <= 200) {
            packs_before += req.size() > 2 ? 1 : 0;
            return packed_fake_reply(req);
        }
        if (turn <= 300) {
            return Bytes{};
        }
        if (turn == 301) {
            ecuio_channel_support_t s{};
            ecuio_engine_support(&e, 0, &s);
            quiet_flags = s.flags;
            ecuio_engine_rescan(&e);
        }
        packs_after += req.size() > 2 ? 1 : 0;
        return packed_fake_reply(req);
    });
    EXPECT_GT(packs_before, 0);
    EXPECT_TRUE(quiet_flags & ECUIO_SUPPORT_UNPACKED)
        << "the quiet ECU unpacked it";
    EXPECT_GT(packs_after, 50);
    ecuio_channel_support_t s{};
    ecuio_engine_support(&e, 0, &s);
    EXPECT_FALSE(s.flags & ECUIO_SUPPORT_UNPACKED);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
}

TEST(RequestEngineTest, ThreeFailedPacksEndPackingOnTheChannel)
{
    for (const char* how : {"silence", "negative", "first PID only"}) {
        SCOPED_TRACE(how);
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
        const std::string kind = how;
        int packs = 0;
        const std::vector<Bytes> sent =
            packing_turns(&e, 400, [&](const Bytes& req) {
                if (req.size() <= 2) {
                    return fake_reply(req);
                }
                ++packs;
                if (packs == 2) {
                    return packed_fake_reply(req); /* one in between */
                }
                if (kind == "silence") {
                    return Bytes{};
                }
                if (kind == "negative") {
                    return Bytes{0x7F, 0x01, 0x12};
                }
                return fake_reply({0x01, req[1]});
            });
        EXPECT_EQ(packs, 1 + 1 + int(ECUIO_ENGINE_PACK_FAILURES));
        ecuio_channel_support_t s{};
        ecuio_engine_support(&e, 0, &s);
        EXPECT_TRUE(s.flags & ECUIO_SUPPORT_UNPACKED);
        size_t k = sent.size();
        while (k > 0 && sent[k - 1].size() == 2) {
            --k;
        }
        EXPECT_LT(k + 100, sent.size()) << "one PID per request from then on";
        for (uint32_t i = 0; i < e.count; ++i) {
            EXPECT_NE(e.requests[i].dropped, ECUIO_DROP_NOT_SUPPORTED);
        }
    }

    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_fake_reply);
    int32_t r = ECUIO_ENGINE_NONE;
    int64_t now = 10 * 1000 * kMs;
    for (int k = 0; k < 50 && r != ECUIO_ENGINE_PACK; ++k) {
        r = ecuio_engine_due(&e, 0, now);
        if (r != ECUIO_ENGINE_PACK && r != ECUIO_ENGINE_NONE) {
            ecuio_engine_sent(&e, 0, r, true, now);
            const Bytes reply =
                fake_reply({0x01, ecuio_engine_request_of(&e, 0, r).bytes[1]});
            (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                        static_cast<uint16_t>(reply.size()), now,
                                        nullptr);
        }
        now += ECUIO_ENGINE_WAIT_US;
    }
    ASSERT_EQ(r, ECUIO_ENGINE_PACK);
    ecuio_engine_sent(&e, 0, r, true, now);
    const uint8_t pending[3] = {0x7F, 0x01, 0x78};
    uint8_t nrc = 0;
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, pending, 3, now + kMs, &nrc),
              ecuio_reply::pending);
    EXPECT_EQ(nrc, 0x78);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US + kMs))
        << "the pending wait";
    EXPECT_EQ(e.chans[0].pack_failures, 0u);
}

/* Out of step, after a pack's wait ended with nothing, the request
 * before's reply come late answers nothing: a refusal naming Service 01,
 * or a record of one of the next pack's PIDs. That pack's own reply then
 * answers it, so late replies never add up to three failures. In step,
 * the same refusal fails the pack (ThreeFailedPacksEndPackingOnTheChannel) */
TEST(RequestEngineTest, OutOfStepALateReplyFailsNoPack)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_fake_reply);
    int64_t now = 10 * 1000 * kMs;
    const auto next_pack = [&] {
        for (int k = 0; k < 50; ++k, now += ECUIO_ENGINE_GAP_US) {
            const int32_t r = ecuio_engine_due(&e, 0, now);
            if (r == ECUIO_ENGINE_PACK) {
                ecuio_engine_sent(&e, 0, r, true, now);
                const ecuio_request& q = ecuio_engine_request_of(&e, 0, r);
                return Bytes(q.bytes, q.bytes + q.len);
            }
            if (r != ECUIO_ENGINE_NONE) {
                ecuio_engine_sent(&e, 0, r, true, now);
                const Bytes reply = fake_reply(
                    {0x01, ecuio_engine_request_of(&e, 0, r).bytes[1]});
                (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                            static_cast<uint16_t>(reply.size()),
                                            now + kMs, nullptr);
            }
        }
        return Bytes{};
    };
    const Bytes first = next_pack();
    ASSERT_GT(first.size(), 2u);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.chans[0].pack_failures, 1u);
    EXPECT_FALSE(e.chans[0].in_step);

    now += ECUIO_ENGINE_GAP_US;
    const Bytes second = next_pack();
    ASSERT_GT(second.size(), 2u);
    const Bytes refusal = {0x7F, 0x01, 0x12};
    const Bytes record = fake_reply({0x01, second[1]});
    uint8_t nrc = 0;
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, refusal.data(), 3, now + kMs, &nrc),
              ecuio_reply::none);
    EXPECT_EQ(nrc, 0u);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, record.data(),
                                    static_cast<uint16_t>(record.size()),
                                    now + kMs, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_PACK) << "still waiting";
    const Bytes own = packed_fake_reply(second);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, own.data(),
                                    static_cast<uint16_t>(own.size()),
                                    now + 2 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].pack_failures, 0u);
    EXPECT_TRUE(e.chans[0].in_step);
}

/* Packing waits for the driver's word, and a set walked with the probe
 * off has no bitmaps to pack by */
TEST(RequestEngineTest, PackingIsTheDriversToTurnOn)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar));
    for (const Bytes& req : packing_turns(&e, 300, packed_fake_reply)) {
        EXPECT_EQ(req.size(), 2u);
    }
    ecuio_request_engine f;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&f, kTestCar, false));
    f.pack = true;
    for (const Bytes& req : packing_turns(&f, 300, packed_fake_reply)) {
        EXPECT_EQ(req.size(), 2u) << "no bitmaps listed";
    }
}

/* ---- Reply timing (ADR-0042, request_timing.h) ---------------------- */

/* One sample reads back at most an eighth above itself and never below,
 * exactly below 16 us, and a sample past the top band reads its exact
 * value */
TEST(RequestTimingTest, AQuantileReadsAtMostAnEighthHigh)
{
    for (uint32_t v = 0; v < (1u << 24); v = v < 64 ? v + 1 : v + v / 7) {
        timing_hist h;
        h.add(v);
        const uint32_t q = h.quantile(0.5);
        ASSERT_GE(q, v);
        ASSERT_LE(static_cast<double>(q), static_cast<double>(v) * 1.125) << v;
        if (v < 16) {
            ASSERT_EQ(q, v);
        }
    }
    timing_hist past;
    past.add(20 * 1000 * 1000);
    past.add(1000);
    EXPECT_EQ(past.quantile(1.0), 20u * 1000 * 1000);
    EXPECT_EQ(past.max_us, 20u * 1000 * 1000);
}

/* A sample with its top bit set (36 minutes and more), or past 32 bits
 * (a wide CSV survey's gap between two samples hours apart), goes in the
 * last bucket at once: the log2 search stops at bit 31 */
TEST(RequestTimingTest, ASamplePastThirtyOneBitsLandsInTheLastBucket)
{
    timing_hist h;
    h.add(INT64_C(0x80000000));
    h.add(INT64_C(0xFFFFFFFF));
    h.add(INT64_C(1000000000000000));
    EXPECT_EQ(h.count, 3u);
    EXPECT_EQ(h.buckets[timing_hist::kBuckets - 1], 3u);
    EXPECT_EQ(h.max_us, 0xFFFFFFFFu);
    EXPECT_EQ(h.quantile(0.5), 0xFFFFFFFFu);
}

/* Over 1 to 1000 us, each quantile is within its bucket above the true
 * one, capped at the largest sample */
TEST(RequestTimingTest, QuantilesOfADistribution)
{
    timing_hist h;
    for (int v = 1; v <= 1000; ++v) {
        h.add(v);
    }
    EXPECT_EQ(h.count, 1000u);
    EXPECT_GE(h.quantile(0.5), 500u);
    EXPECT_LE(h.quantile(0.5), 563u);
    EXPECT_GE(h.quantile(0.99), 990u);
    EXPECT_LE(h.quantile(0.99), 1000u);
    EXPECT_EQ(h.quantile(1.0), 1000u);
    EXPECT_EQ(timing_hist().quantile(0.5), 0u) << "no samples";
}

/* A P99 needs 100 samples and a P99.9 1,000; below, 0 and no flag */
TEST(RequestTimingTest, TooFewSamplesLeaveAQuantileEmpty)
{
    for (uint32_t n : {99u, 100u, 999u, 1000u}) {
        SCOPED_TRACE(n);
        timing_row r;
        for (uint32_t k = 0; k <= n; ++k) {
            timing_answered(r, 4000, static_cast<int64_t>(k) * 100000, 0,
                            false);
        }
        /* n + 1 answers: n + 1 latencies, n intervals */
        const ecuio_request_timing_t row = timing_public(r);
        EXPECT_EQ(row.answered, n + 1);
        EXPECT_EQ(row.intervals, n);
        EXPECT_EQ(row.latency_p50_us, 4000u);
        EXPECT_EQ((row.flags & ECUIO_TIMING_INTERVAL_P99) != 0, n >= 100);
        EXPECT_EQ(row.interval_p99_us, n >= 100 ? 100000u : 0u);
        EXPECT_EQ((row.flags & ECUIO_TIMING_INTERVAL_P999) != 0, n >= 1000);
        EXPECT_EQ(row.interval_p999_us, n >= 1000 ? 100000u : 0u);
        EXPECT_NE(row.flags & ECUIO_TIMING_LATENCY_P99, 0u);
        EXPECT_EQ((row.flags & ECUIO_TIMING_LATENCY_P999) != 0, n + 1 >= 1000);
    }
}

/* An interval with a fallback of the frames' clock between its answers,
 * or one running backwards, is counted apart and kept out */
TEST(RequestTimingTest, AnIntervalAcrossAClockFallbackIsLeftOut)
{
    timing_row r;
    timing_answered(r, 4000, 0, 0, false);
    timing_answered(r, 4000, 100000, 0, false);
    timing_answered(r, 4000, 200000, 1, false); /* a fallback between */
    timing_answered(r, 4000, 300000, 1, false);
    timing_answered(r, 4000, 250000, 1, false); /* backwards */
    EXPECT_EQ(r.interval.count, 2u);
    EXPECT_EQ(r.intervals_mixed, 2u);
    EXPECT_EQ(r.interval.max_us, 100000u);
    timing_refused(r, 6000, true);
    EXPECT_EQ(r.refused, 1u);
    EXPECT_EQ(r.packed, 1u);
    EXPECT_EQ(r.latency.count, 6u) << "a refusal has a latency";
    EXPECT_EQ(r.interval.count, 2u) << "but refreshes nothing";
}

namespace {

/* The figures of an engine walked on the test car's bitmaps, timing on,
 * after n turns answered through respond, each reply 4 ms after its
 * request */
std::unique_ptr<ecuio_timing_table>
timed_turns(ecuio_request_engine* e, bool pack, int n, const Responder& respond)
{
    auto timing = std::make_unique<ecuio_timing_table>();
    if (pack) {
        packing_engine(e);
    } else {
        obd2_walked(e, kTestCar);
    }
    e->timing = timing.get();
    (void)packing_turns(e, n, respond);
    return timing;
}

} // namespace

/* Each rotation request's answers and timeouts, its latency from its
 * write to its reply, and its intervals between answers on the host's
 * clock when the driver gives no stamps */
TEST(RequestEngineTest, TimingRecordsEachRotationRequestsReplies)
{
    ecuio_request_engine e;
    const auto silent0D = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0D} ? Bytes{} : fake_reply(req);
    };
    const auto t = timed_turns(&e, false, 400, silent0D);
    uint32_t rows = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const ecuio_request& q = e.requests[i];
        const timing_row& r = t->rows[i];
        SCOPED_TRACE("PID " + std::to_string(q.bytes[1]));
        if (q.dropped != 0) {
            EXPECT_EQ(r.answered, 0u);
            continue;
        }
        ++rows;
        if (q.bytes[1] == 0x0D) {
            EXPECT_EQ(r.answered, 0u);
            EXPECT_GT(r.timeouts, 0u);
            EXPECT_EQ(r.latency.count, 0u) << "a timeout has no latency";
            continue;
        }
        EXPECT_GT(r.answered, 1u);
        EXPECT_EQ(r.timeouts, 0u);
        EXPECT_EQ(r.packed, 0u);
        EXPECT_EQ(r.latency.quantile(0.5), 4000u);
        EXPECT_EQ(r.latency.max_us, 4000u);
        EXPECT_EQ(r.interval.count, r.answered - 1);
        EXPECT_GT(r.interval.quantile(0.5), 0u);
    }
    EXPECT_GT(rows, 10u);
}

/* A pack's reply answers each PID whose record it carries, with the
 * pack's latency, counted as packed */
TEST(RequestEngineTest, TimingCountsAPacksAnswersForEachOfItsPids)
{
    ecuio_request_engine e;
    const auto t = timed_turns(&e, true, 400, packed_fake_reply);
    uint32_t packed = 0;
    uint32_t kept = 0;
    uint32_t packed_rows = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const timing_row& r = t->rows[i];
        if (e.requests[i].dropped != 0) {
            continue;
        }
        SCOPED_TRACE("PID " + std::to_string(e.requests[i].bytes[1]));
        ++kept;
        packed_rows += r.packed > 0 ? 1 : 0;
        EXPECT_GT(r.answered, 0u);
        EXPECT_LE(r.packed, r.answered);
        EXPECT_EQ(r.latency.max_us, 4000u);
        packed += r.packed;
    }
    EXPECT_GT(packed, 100u);
    EXPECT_GT(packed_rows * 4, kept * 3)
        << "every record of a pack's reply answers its PID, not the first "
           "alone";
}

/* The driver's stamps time the intervals: on the frames' clock, an
 * interval across one of its fallbacks left out and counted */
TEST(RequestEngineTest, TimingTakesTheIntervalFromTheDriversStamps)
{
    ecuio_request_engine e;
    obd2_walked(&e, kTestCar);
    auto timing = std::make_unique<ecuio_timing_table>();
    e.timing = timing.get();
    e.stamp_set = true;
    int64_t now = 10 * 1000 * kMs;
    int answers = 0;
    for (int k = 0; k < 300; ++k, now += ECUIO_ENGINE_GAP_US) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            continue;
        }
        ecuio_engine_sent(&e, 0, r, true, now);
        const ecuio_request& q = ecuio_engine_request_of(&e, 0, r);
        const Bytes reply = fake_reply(Bytes(q.bytes, q.bytes + q.len));
        e.stamp_us = static_cast<int64_t>(answers) * 7000; /* 7 ms apart */
        e.stamp_breaks = answers >= 150 ? 1u : 0u;
        (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()),
                                    now + 4 * kMs, nullptr);
        ++answers;
    }
    uint32_t mixed = 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const timing_row& r = timing->rows[i];
        kept += e.requests[i].dropped == 0 ? 1 : 0;
        if (r.interval.count > 0) {
            EXPECT_EQ(r.interval.max_us % 7000, 0u) << "the stamps', not now's";
        }
        mixed += r.intervals_mixed;
    }
    EXPECT_GT(mixed, 0u);
    EXPECT_LE(mixed, kept) << "one fallback, at most one per request";
}

namespace {

/* The first vehicle's support report: the bitmaps at 00, 20 and 40
 * (docs/bench-sessions/2026-10-22-vehicle.md), from the engine ECU */
ecuio_channel_support_t first_vehicle()
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    return car;
}

/* The obd2 set's unsupported ids against reports */
std::vector<uint16_t> unsupported(const ecuio_channel_support_t* reports,
                                  uint32_t n)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> ids(set->count);
    const uint32_t count = ecuio_unsupported_signals(
        *set, reports, n, ids.data(), static_cast<uint32_t>(ids.size()));
    EXPECT_LE(count, ids.size());
    ids.resize(count);
    return ids;
}

} // namespace

/* ADR-0039 section 6: a Service 01 signal is unsupported when its ECU's
 * answered bitmap clears its PID's bit; the rule from the bitmaps'
 * bytes: PID p is supported when bit 0x80 >> ((p - 1) % 8) of byte
 * (p - 1) / 8 is set. Ranges never answered (past 0x60 here) and PID 0
 * judge nothing */
TEST(PidSupportTest, TheBitmapsMarkTheirClearPidsUnsupported)
{
    const ecuio_channel_support_t car = first_vehicle();
    const uint8_t* bitmaps = car.pid_bitmap;
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> expected;
    int pastTheRanges = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const unsigned pid = set->defs[i].mux_key & 0xFFu;
        if (pid <= 0x60 &&
            (bitmaps[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) == 0) {
            expected.push_back(set->defs[i].id);
        }
        pastTheRanges += pid > 0x60 ? 1 : 0;
    }
    ASSERT_GT(expected.size(), 1u);
    ASSERT_GT(pastTheRanges, 0);
    EXPECT_EQ(unsupported(&car, 1), expected);

    EXPECT_TRUE(ecuio_pid_marked_unsupported(car, 0x5C));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x0C));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x61))
        << "a range never answered";
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x00));

    /* A rescan that finds 0x5C takes its signal off */
    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    const std::vector<uint16_t> after = unsupported(&rescan, 1);
    EXPECT_EQ(after.size(), expected.size() - 1);
    for (uint16_t id : after) {
        EXPECT_NE(id, ECUIO_OBD2_PID_SIG_ID(0x5C, 0));
    }
}

/* The first vehicle's sheet counts 44 data PIDs to 0x60 (0x20 and 0x40,
 * the chain's links, and 0x60 left out); the bitmaps cover what their
 * ranges answered */
TEST(PidSupportTest, TheCountLeavesOutTheChainsOwnPids)
{
    const ecuio_channel_support_t car = first_vehicle();
    EXPECT_EQ(ecuio_supported_pid_count(car.pid_bitmap), 44u);
    uint8_t chain[32] = {};
    chain[(0x20 - 1) / 8] = 0x01; /* 0x20 */
    chain[(0x40 - 1) / 8] = 0x01; /* 0x40 */
    EXPECT_EQ(ecuio_supported_pid_count(chain), 0u);
    chain[0] = 0x80;  /* 0x01 */
    chain[31] = 0x02; /* 0xFF */
    EXPECT_EQ(ecuio_supported_pid_count(chain), 2u);
    EXPECT_EQ(ecuio_pids_through(car.ranges), 0x60u);
    EXPECT_EQ(ecuio_pids_through(0x3F), 0xC0u);
    EXPECT_EQ(ecuio_pids_through(0x1), 0x20u);
    EXPECT_EQ(ecuio_pids_through(0), 0u);
}

/* Only the report of the channel a def's message ID names judges it,
 * and one with nothing learned judges nothing; a set with no Service 01
 * keys has none; the count is all of them, the first capacity copied */
TEST(PidSupportTest, OnlyTheDefsOwnEcusReportJudgesIt)
{
    const ecuio_channel_support_t car = first_vehicle();
    ecuio_channel_support_t other = car;
    other.rx_id = 0x7E9;
    ecuio_channel_support_t nothing = car;
    nothing.generation = 0;
    const ecuio_channel_support_t neither[2] = {other, nothing};
    EXPECT_TRUE(unsupported(neither, 2).empty());
    EXPECT_TRUE(unsupported(nullptr, 0).empty());
    const ecuio_channel_support_t both[2] = {other, car};
    EXPECT_EQ(unsupported(both, 2), unsupported(&car, 1));

    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    EXPECT_EQ(ecuio_unsupported_signals(*mock, &car, 1, nullptr, 0), 0u);

    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const uint32_t all = ecuio_unsupported_signals(*set, &car, 1, nullptr, 0);
    ASSERT_GT(all, 2u);
    uint16_t two[3] = {0, 0, 0xFFFF};
    EXPECT_EQ(ecuio_unsupported_signals(*set, &car, 1, two, 2), all);
    EXPECT_EQ(two[0], unsupported(&car, 1)[0]);
    EXPECT_EQ(two[1], unsupported(&car, 1)[1]);
    EXPECT_EQ(two[2], 0xFFFF) << "nothing past the capacity";
}

// ---- Trouble-code titles (ADR-0044): Wal33D's generic list, SAE J2012's
// powertrain and network ranges only

namespace {

// J2012's ranges SAE controls, of the letters the table keeps
bool sae_titled_range(uint16_t code)
{
    const unsigned letter = code >> 14;
    const unsigned d1 = (code >> 12) & 3;
    const unsigned d2 = (code >> 8) & 0xF;
    if (letter == 0) { // P
        return d1 == 0 || d1 == 2 || (d1 == 3 && d2 >= 4 && d2 <= 9);
    }
    if (letter == 3) { // U
        return d1 == 0 || d1 == 3;
    }
    return false; // C and B are left out
}

} // namespace

TEST(DtcTitleTest, CommonCodesHaveTheirTitles)
{
    EXPECT_STREQ(ecuio_dtc_title(0x0301), "Cylinder 1 Misfire Detected");
    EXPECT_STREQ(ecuio_dtc_title(0x0420),
                 "Catalyst System Efficiency Below Threshold Bank 1");
    EXPECT_STREQ(ecuio_dtc_title(0x0171), "System Too Lean Bank 1");
    EXPECT_STREQ(ecuio_dtc_title(0xC100), "Lost Communication With ECM/PCM A");
    // An en dash in the source is a hyphen here
    EXPECT_STREQ(ecuio_dtc_title(0x2110),
                 "Throttle Actuator A Control System - Forced Limited RPM");
    EXPECT_EQ(
        std::strncmp(ecuio_dtc_title_source(), "Wal33D/dtc-database ", 20), 0);
}

TEST(DtcTitleTest, OnlySaesPowertrainAndNetworkRangesHaveTitles)
{
    // Every code: a title only in the ranges kept, each plain ASCII text;
    // most of the ranges' codes are listed (J2012 leaves some reserved)
    int titled = 0;
    for (uint32_t c = 0; c <= 0xFFFF; ++c) {
        const char* t = ecuio_dtc_title(static_cast<uint16_t>(c));
        ASSERT_NE(t, nullptr);
        if (*t == '\0') {
            continue;
        }
        ++titled;
        EXPECT_TRUE(sae_titled_range(static_cast<uint16_t>(c)))
            << std::hex << c;
        for (const char* p = t; *p != '\0'; ++p) {
            EXPECT_TRUE(*p >= 32 && *p <= 126) << std::hex << c;
        }
    }
    EXPECT_GT(titled, 8000);
    // The fake ECU's: a manufacturer's code and a chassis code have none
    EXPECT_STREQ(ecuio_dtc_title(0x1234), ""); // P1234
    EXPECT_STREQ(ecuio_dtc_title(0x4035), ""); // C0035
    EXPECT_STREQ(ecuio_dtc_title(0x9001), ""); // B1001
    EXPECT_STREQ(ecuio_dtc_title(0xD000), ""); // U1000
}

TEST(DtcTitleTest, ParseIsFormatsInverse)
{
    for (uint32_t c = 0; c <= 0xFFFF; ++c) {
        char text[8] = {};
        ASSERT_EQ(ecuio_dtc_format(static_cast<uint16_t>(c), text, sizeof text),
                  ECUIO_OK);
        uint16_t back = 0;
        ASSERT_EQ(ecuio_dtc_parse(text, &back), ECUIO_OK) << text;
        EXPECT_EQ(back, c) << text;
    }
}

TEST(DtcTitleTest, ParseRefusesWhatIsntACode)
{
    for (const char* bad : {"p0301", "P4301", "P030", "P03011", "X0301",
                            "P03G1", "P03a1", "", " P0301", "P0301 "}) {
        uint16_t out = 0xABCD;
        EXPECT_EQ(ecuio_dtc_parse(bad, &out), ECUIO_ERR_INVALID_ARGUMENT)
            << bad;
        EXPECT_EQ(out, 0xABCD) << bad;
    }
    uint16_t out = 0;
    EXPECT_EQ(ecuio_dtc_parse(nullptr, &out), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dtc_parse("P0301", nullptr), ECUIO_ERR_NULL_ARGUMENT);
}

namespace {

// A read report whose PID 01 answered: these monitors, the first
// `incomplete` of them incomplete
ecuio_channel_diagnostics_t inspected_report(std::initializer_list<uint8_t> ids,
                                             uint32_t incomplete,
                                             bool mil = false)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
              (mil ? ECUIO_DIAG_MIL_ON : 0u);
    d.readiness = ECUIO_DIAG_ANSWERED;
    for (uint8_t id : ids) {
        ecuio_monitor_t& m = d.monitors[d.monitor_count];
        m.id = id;
        m.flags = d.monitor_count < incomplete ? 0 : ECUIO_MONITOR_COMPLETE;
        ++d.monitor_count;
    }
    return d;
}

ecuio_inspection_t inspect(const std::vector<ecuio_channel_diagnostics_t>& r,
                           uint32_t model_year)
{
    ecuio_inspection_t out{};
    EXPECT_EQ(ecuio_diag_inspect(r.data(), static_cast<uint32_t>(r.size()),
                                 model_year, &out),
              ECUIO_OK);
    return out;
}

} // namespace

// 40 CFR 85.2222(c)(2): two incomplete for 1996-2000, one from 2001
TEST(InspectionTest, TheModelYearSetsHowManyMayBeIncomplete)
{
    const std::vector<ecuio_channel_diagnostics_t> two = {
        inspected_report({8, 10, 13, 0}, 2)};
    const std::vector<ecuio_channel_diagnostics_t> one = {
        inspected_report({8, 10, 13, 0}, 1)};
    EXPECT_EQ(inspect(two, 1996).verdict, ECUIO_INSPECTION_READY);
    EXPECT_EQ(inspect(two, 2000).verdict, ECUIO_INSPECTION_READY);
    EXPECT_EQ(inspect(two, 2000).allowed, 2);
    EXPECT_EQ(inspect(two, 2001).verdict, ECUIO_INSPECTION_NOT_READY);
    EXPECT_EQ(inspect(two, 2001).allowed, 1);
    EXPECT_EQ(inspect(one, 2001).verdict, ECUIO_INSPECTION_READY);
    EXPECT_EQ(inspect(one, 2026).verdict, ECUIO_INSPECTION_READY);
    const std::vector<ecuio_channel_diagnostics_t> three = {
        inspected_report({8, 10, 13, 0}, 3)};
    EXPECT_EQ(inspect(three, 1998).verdict, ECUIO_INSPECTION_NOT_READY);
    const ecuio_inspection_t i = inspect(two, 2001);
    EXPECT_EQ(i.incomplete, 2);
    EXPECT_EQ(i.incomplete_ids, (1u << 8) | (1u << 10));
    EXPECT_EQ(i.reports, 1);
    EXPECT_EQ(i.diag_flags, 0u);
}

// With no model year only two incomplete needs one: one passes both
// rules, three fail both
TEST(InspectionTest, WithoutAModelYearOnlyTwoIncompleteIsUndecided)
{
    for (uint32_t n = 0; n <= 4; ++n) {
        const ecuio_inspection_t i =
            inspect({inspected_report({8, 10, 13, 15}, n)}, 0);
        EXPECT_EQ(i.verdict, n <= 1   ? ECUIO_INSPECTION_READY
                             : n == 2 ? ECUIO_INSPECTION_YEAR_NEEDED
                                      : ECUIO_INSPECTION_NOT_READY)
            << n;
        EXPECT_EQ(i.incomplete, n);
        EXPECT_EQ(i.allowed, 0);
    }
}

// (d)(1): a commanded MIL fails, whatever the monitors say
TEST(InspectionTest, ACommandedMilFails)
{
    EXPECT_EQ(inspect({inspected_report({8, 10}, 0, true)}, 2015).verdict,
              ECUIO_INSPECTION_MIL_ON);
    EXPECT_EQ(inspect({inspected_report({8, 10}, 0, true)}, 0).verdict,
              ECUIO_INSPECTION_MIL_ON);
    // on a second ECU only, it still fails the car
    EXPECT_EQ(
        inspect({inspected_report({8, 10}, 0), inspected_report({2}, 0, true)},
                2015)
            .verdict,
        ECUIO_INSPECTION_MIL_ON);
}

// The car's monitors are every ECU's: one incomplete anywhere counts,
// once however many ECUs have it
TEST(InspectionTest, ReportsCountTogetherAsOneCar)
{
    const ecuio_inspection_t i = inspect(
        {inspected_report({8, 13}, 1), inspected_report({2, 8}, 2)}, 2015);
    EXPECT_EQ(i.incomplete, 2); // catalyst on both, components on the second
    EXPECT_EQ(i.incomplete_ids, (1u << 8) | (1u << 2));
    EXPECT_EQ(i.reports, 2);
    EXPECT_EQ(i.verdict, ECUIO_INSPECTION_NOT_READY);
    // a compression-ignition report names its monitors so
    ecuio_channel_diagnostics_t diesel = inspected_report({9}, 1);
    diesel.flags |= ECUIO_DIAG_COMPRESSION;
    EXPECT_EQ(inspect({diesel}, 2015).diag_flags, ECUIO_DIAG_COMPRESSION);
}

TEST(InspectionTest, ReadingNoAnswerAndOldCarsHaveNoCount)
{
    // a report still being read holds the verdict back
    ecuio_channel_diagnostics_t reading = inspected_report({8}, 0);
    reading.flags &= ~ECUIO_DIAG_COMPLETE;
    EXPECT_EQ(inspect({inspected_report({8}, 0), reading}, 2015).verdict,
              ECUIO_INSPECTION_READING);
    // one never asked (?diagnose=0) doesn't
    ecuio_channel_diagnostics_t unasked{};
    EXPECT_EQ(inspect({inspected_report({8}, 0), unasked}, 2015).verdict,
              ECUIO_INSPECTION_READY);
    // no PID 01 answer from any ECU
    ecuio_channel_diagnostics_t refused{};
    refused.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    refused.readiness = ECUIO_DIAG_REFUSED;
    EXPECT_EQ(inspect({refused}, 2015).verdict, ECUIO_INSPECTION_UNKNOWN);
    EXPECT_EQ(inspect({}, 2015).verdict, ECUIO_INSPECTION_UNKNOWN);
    // before 1996 the rule says nothing, even with the MIL on
    EXPECT_EQ(inspect({inspected_report({8}, 0, true)}, 1995).verdict,
              ECUIO_INSPECTION_NOT_COVERED);
    ecuio_inspection_t out{};
    EXPECT_EQ(ecuio_diag_inspect(nullptr, 1, 2015, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_diag_inspect(nullptr, 0, 2015, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_diag_inspect(nullptr, 0, 2015, &out), ECUIO_OK);
    EXPECT_EQ(out.verdict, ECUIO_INSPECTION_UNKNOWN);
}

/* ---------- the simulated ECU's answers (ADR-0046) ---------- */

namespace {

ecuio_sim_ecu sim_of(ecuio_sim_values values)
{
    ecuio_sim_ecu e;
    e.config.values = values;
    std::memcpy(e.config.vin, ECUIO_SIM_DEFAULT_VIN,
                sizeof ECUIO_SIM_DEFAULT_VIN);
    return e;
}

std::vector<uint8_t> ask(ecuio_sim_ecu& e, std::initializer_list<uint8_t> req,
                         bool functional = false)
{
    const std::vector<uint8_t> q(req);
    uint8_t out[ECUIO_SIM_REPLY_MAX];
    const uint32_t n = ecuio_sim_answer(
        e, q.data(), static_cast<uint32_t>(q.size()), functional, out);
    return std::vector<uint8_t>(out, out + n);
}

/* A Service 01 reply decoded by the obd2 def id */
double drive_value(const std::vector<uint8_t>& reply, uint16_t id)
{
    const ecuio_signal_def_t* d = find_def(ecuio_obd2_signal_set(), id);
    EXPECT_NE(d, nullptr);
    const ecuio_frame_t f =
        make_obd2_frame(reply.data(), static_cast<uint8_t>(reply.size()));
    double v = -1.0;
    EXPECT_TRUE(ecuio_def_applies_to_frame(d, &f)) << id;
    EXPECT_EQ(decode(d, f, &v), ECUIO_DECODE_OK) << id;
    return v;
}

} // namespace

// values=drive: each PID through the obd2 set's own def reads the drive
// cycle's value, to its resolution, while the car moves
TEST(SimEcuTest, DriveValuesAreTheDriveCyclesThroughTheObd2Defs)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    // 60 s in: off the cold start, pulling through the gears
    for (int i = 0; i < 1200; ++i) {
        ecuio_sim_tick(e);
    }
    const ecuio_mock::DriveCycle& car = e.car;
    ASSERT_TRUE(car.running);
    ASSERT_GT(car.speed_kmh, 1.0f);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x0C}), ECUIO_OBD2_SIG_RPM_ID),
                car.rpm, 0.25);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x0D}), ECUIO_OBD2_SIG_SPEED_ID),
                car.speed_kmh, 0.5);
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x04}), ECUIO_OBD2_SIG_ENGINE_LOAD_ID),
        car.load_pct, 0.2);
    EXPECT_NEAR(
        drive_value(ask(e, {0x01, 0x05}), ECUIO_OBD2_SIG_COOLANT_TEMP_ID),
        car.coolant_c, 0.5);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x0F}), ECUIO_OBD2_SIG_IAT_ID),
                car.iat_c, 0.5);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x10}), ECUIO_OBD2_SIG_MAF_ID),
                car.maf_gs, 0.005);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x11}), ECUIO_OBD2_SIG_THROTTLE_ID),
                car.throttle_pct, 0.2);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x5C}), ECUIO_OBD2_SIG_OIL_TEMP_ID),
                car.oil_c, 0.5);
    EXPECT_NEAR(drive_value(ask(e, {0x01, 0x42}), ECUIO_OBD2_SIG_BATTERY_V_ID),
                14.1, 0.001);
    // run time since start, lambda and ambient by J1979's scaling
    const std::vector<uint8_t> run = ask(e, {0x01, 0x1F});
    ASSERT_EQ(run.size(), 4u);
    EXPECT_NEAR((run[2] << 8) | run[3], e.engine_s, 1.0);
    EXPECT_GT(e.engine_s, 30.0f);
    const std::vector<uint8_t> lambda = ask(e, {0x01, 0x24});
    ASSERT_EQ(lambda.size(), 6u);
    EXPECT_NEAR(((lambda[2] << 8) | lambda[3]) * 2.0 / 65536.0, car.lambda,
                0.0001);
    EXPECT_EQ(ask(e, {0x01, 0x46}), (std::vector<uint8_t>{0x41, 0x46, 60}));
    // the values move: rpm over the next 5 s
    const double before =
        drive_value(ask(e, {0x01, 0x0C}), ECUIO_OBD2_SIG_RPM_ID);
    for (int i = 0; i < 100; ++i) {
        ecuio_sim_tick(e);
    }
    EXPECT_NE(drive_value(ask(e, {0x01, 0x0C}), ECUIO_OBD2_SIG_RPM_ID), before);
}

// The key on, the engine stopped: 0 rpm, the battery's voltage, no run
// time
TEST(SimEcuTest, TheKeyOnWithTheEngineStopped)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    ecuio_sim_tick(e);
    EXPECT_EQ(ask(e, {0x01, 0x0C}), (std::vector<uint8_t>{0x41, 0x0C, 0, 0}));
    EXPECT_EQ(ask(e, {0x01, 0x42}),
              (std::vector<uint8_t>{0x41, 0x42, 0x31, 0x38}));
    EXPECT_EQ(ask(e, {0x01, 0x1F}), (std::vector<uint8_t>{0x41, 0x1F, 0, 0}));
}

// Each mode's bitmaps list exactly the PIDs it answers, and chain
TEST(SimEcuTest, TheBitmapsListWhatEachModeAnswers)
{
    for (ecuio_sim_values values :
         {ecuio_sim_values::drive, ecuio_sim_values::canned}) {
        ecuio_sim_ecu e = sim_of(values);
        int listed = 0;
        for (unsigned base = 0; base <= 0xE0; base += 0x20) {
            const std::vector<uint8_t> b =
                ask(e, {0x01, static_cast<uint8_t>(base)});
            if (b.size() != 6) {
                // a bitmap the chain doesn't reach is refused
                EXPECT_EQ(b, (std::vector<uint8_t>{0x7F, 0x01, 0x12})) << base;
                continue;
            }
            for (unsigned k = 1; k <= 32 && base + k <= 0xFF; ++k) {
                const uint8_t pid = static_cast<uint8_t>(base + k);
                const bool bit =
                    (b[2 + (k - 1) / 8] & (0x80u >> ((k - 1) % 8))) != 0;
                const std::vector<uint8_t> r = ask(e, {0x01, pid});
                EXPECT_EQ(bit, r.size() >= 3 && r[0] == 0x41)
                    << int(values) << " PID " << std::hex << int(pid);
                listed += (bit && pid % 0x20 != 0) ? 1 : 0;
            }
        }
        EXPECT_EQ(listed, values == ecuio_sim_values::drive ? 19 : 95);
    }
}

// values=canned: every Service 01 PID answered as the fakes' ECU answers
// it, refusals included (sim_canned.h mirrors tests/fake_ecu.h)
TEST(SimEcuTest, CannedValuesAreTheFakesOwn)
{
    fake_ecu::reset_diagnostics();
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::canned);
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        const unsigned char req[2] = {0x01, static_cast<unsigned char>(pid)};
        unsigned char fake[fake_ecu::kReplyMax];
        size_t n = 0;
        ASSERT_TRUE(fake_ecu::answer(req, 2, -1, fake, &n));
        EXPECT_EQ(ask(e, {0x01, static_cast<uint8_t>(pid)}),
                  std::vector<uint8_t>(fake, fake + n))
            << std::hex << pid;
    }
    // and after a clear
    const unsigned char clear[1] = {0x04};
    unsigned char out[fake_ecu::kReplyMax];
    size_t n = 0;
    ASSERT_TRUE(fake_ecu::answer(clear, 1, -1, out, &n));
    EXPECT_EQ(ask(e, {0x04}), (std::vector<uint8_t>{0x44}));
    for (unsigned p : {0x01, 0x21, 0x30, 0x31, 0x41, 0x4D, 0x4E}) {
        const uint8_t pid = static_cast<uint8_t>(p);
        const unsigned char req[2] = {0x01, pid};
        ASSERT_TRUE(fake_ecu::answer(req, 2, -1, out, &n));
        EXPECT_EQ(ask(e, {0x01, pid}), std::vector<uint8_t>(out, out + n))
            << std::hex << int(pid);
    }
    fake_ecu::reset_diagnostics();
}

// The diagnostics are the mock's (the fakes' codes), before and after a
// clear
TEST(SimEcuTest, TheDiagnosticsAreTheMocksAndAClearClearsThem)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    EXPECT_EQ(ask(e, {0x03}), (std::vector<uint8_t>{0x43, 0x03, 0x03, 0x01,
                                                    0x04, 0x20, 0xC1, 0x00}));
    EXPECT_EQ(ask(e, {0x07}).size(), 8u);
    EXPECT_EQ(ask(e, {0x0A}), (std::vector<uint8_t>{0x4A, 0x01, 0x04, 0x20}));
    EXPECT_EQ(ask(e, {0x01, 0x01}),
              (std::vector<uint8_t>{0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}));
    EXPECT_EQ(ask(e, {0x02, 0x02, 0x00}),
              (std::vector<uint8_t>{0x42, 0x02, 0x00, 0x03, 0x01}));
    EXPECT_EQ(ask(e, {0x02, 0x0C, 0x01}),
              (std::vector<uint8_t>{0x7F, 0x02, 0x12}));
    EXPECT_EQ(ask(e, {0x01, 0x31}),
              (std::vector<uint8_t>{0x41, 0x31, 0x04, 0xD2}));
    EXPECT_EQ(ask(e, {0x04}), (std::vector<uint8_t>{0x44}));
    EXPECT_TRUE(e.cleared);
    EXPECT_EQ(ask(e, {0x03}), (std::vector<uint8_t>{0x43, 0x00}));
    EXPECT_EQ(ask(e, {0x0A}), (std::vector<uint8_t>{0x4A, 0x01, 0x04, 0x20}))
        << "permanent codes stay";
    EXPECT_EQ(ask(e, {0x01, 0x01}),
              (std::vector<uint8_t>{0x41, 0x01, 0x00, 0x07, 0xE5, 0xE5}));
    EXPECT_EQ(ask(e, {0x01, 0x31}), (std::vector<uint8_t>{0x41, 0x31, 0, 0}));
    EXPECT_EQ(ask(e, {0x02, 0x0C, 0x00}),
              (std::vector<uint8_t>{0x7F, 0x02, 0x12}));
}

// Service 09: the VIN the URI gave and the ECU name; the rest refused,
// and a functional request's refusals unsaid (ISO 14229-1)
TEST(SimEcuTest, IdentificationAndRefusals)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    std::vector<uint8_t> vin = ask(e, {0x09, 0x02});
    ASSERT_EQ(vin.size(), 20u);
    EXPECT_EQ(std::string(vin.begin() + 3, vin.end()), ECUIO_SIM_DEFAULT_VIN);
    EXPECT_EQ(vin[2], 0x01);
    const std::vector<uint8_t> name = ask(e, {0x09, 0x0A});
    ASSERT_EQ(name.size(), 23u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(name.data()) + 3),
              "ECM-HavneSim");
    EXPECT_EQ(ask(e, {0x09, 0x00}),
              (std::vector<uint8_t>{0x49, 0x00, 0x40, 0x40, 0x00, 0x00}));
    EXPECT_EQ(ask(e, {0x09, 0x04}), (std::vector<uint8_t>{0x7F, 0x09, 0x12}));
    EXPECT_EQ(ask(e, {0x22, 0xF1, 0x90}),
              (std::vector<uint8_t>{0x7F, 0x22, 0x31}));
    EXPECT_EQ(ask(e, {0x10, 0x03}), (std::vector<uint8_t>{0x7F, 0x10, 0x11}));
    EXPECT_EQ(ask(e, {0x01, 0x12}), (std::vector<uint8_t>{0x7F, 0x01, 0x12}));
    EXPECT_TRUE(ask(e, {0x01, 0x12}, true).empty());
    EXPECT_TRUE(ask(e, {0x10, 0x03}, true).empty());
    EXPECT_EQ(ask(e, {0x01, 0x0D}, true).size(), 3u) << "a positive one is";
    ecuio_sim_config c;
    char why[256];
    ASSERT_EQ(ecuio_sim_parse_uri("mock://x?vin=1FAFAKE10M0000001", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok);
    e.config = c;
    vin = ask(e, {0x09, 0x02});
    EXPECT_EQ(std::string(vin.begin() + 3, vin.end()), "1FAFAKE10M0000001");
}

// J1979's pack: each PID it has, in the order asked; none, no answer
TEST(SimEcuTest, APackAnswersEachPidItHas)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::canned);
    EXPECT_EQ(
        ask(e, {0x01, 0x0D, 0x12, 0x05, 0x0C}),
        (std::vector<uint8_t>{0x41, 0x0D, 0x00, 0x05, 60, 0x0C, 0x1F, 0x40}));
    EXPECT_TRUE(ask(e, {0x01, 0x12, 0x13}).empty());
    // a pack holding run time's 13 bytes is past a Single Frame
    EXPECT_EQ(ask(e, {0x01, 0x7F, 0x0D}).size(), 1u + 14 + 2);
}

TEST(SimEcuTest, TheUrisKeysAndRefusals)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_EQ(c.values, ecuio_sim_values::drive);
    EXPECT_STREQ(c.vin, ECUIO_SIM_DEFAULT_VIN);
    EXPECT_EQ(c.faults.silent + c.faults.late + c.faults.bad_sequence +
                  c.faults.stalled,
              0u);
    ASSERT_EQ(
        ecuio_sim_parse_uri("mock://sim?values=canned&faults=silent:5,late:7,"
                            "bad_sequence:3,stalled:11",
                            &c, why, sizeof why),
        ecuio_sim_uri_result::ok);
    EXPECT_EQ(c.values, ecuio_sim_values::canned);
    EXPECT_EQ(c.faults.silent, 5u);
    EXPECT_EQ(c.faults.late, 7u);
    EXPECT_EQ(c.faults.bad_sequence, 3u);
    EXPECT_EQ(c.faults.stalled, 11u);
    for (const char* bad :
         {"socketcan://vcan0", "mock://sim?did=1E1C", "mock://sim?values=x",
          "mock://sim?vin=1FAFAKE10M000000", "mock://sim?vin=1FAFAKEI0M0000001",
          "mock://sim?vin=1fafake10m0000001",
          "mock://sim?faults=", "mock://sim?faults=silent",
          "mock://sim?faults=silent:0", "mock://sim?faults=silent:10001",
          "mock://sim?faults=drop:5", "mock://sim?faults=silent:5,",
          "mock://sim?faults=silent:x", "mock://sim?values"}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad, &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
    // replay://: an absolute path, its own keys
#ifdef _WIN32
    const std::string abs = "C:/recordings/run 1";
#else
    const std::string abs = "/recordings/run 1";
#endif
    const std::string replay = "replay://" + abs;
    ASSERT_EQ(ecuio_sim_parse_uri(replay.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.source, ecuio_sim_source::replay);
    EXPECT_EQ(std::string(c.path, c.path_len), abs);
    EXPECT_FALSE(c.loop);
    EXPECT_FALSE(c.vin_given);
    const std::string looped =
        replay + "?loop=1&vin=1FAFAKE10M0000001&faults=late:7";
    ASSERT_EQ(ecuio_sim_parse_uri(looped.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(std::string(c.path, c.path_len), abs) << "the path ends at ?";
    EXPECT_TRUE(c.loop);
    EXPECT_TRUE(c.vin_given);
    EXPECT_STREQ(c.vin, "1FAFAKE10M0000001");
    EXPECT_EQ(c.faults.late, 7u);
    EXPECT_FALSE(c.stretch.given());
    const std::string stretched = replay + "?from=12.5&to=40";
    ASSERT_EQ(ecuio_sim_parse_uri(stretched.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.stretch.from_us, 12500000u);
    EXPECT_EQ(c.stretch.to_us, 40000000u);
    for (const std::string& bad :
         {std::string("replay://"), std::string("replay://?loop=1"),
          std::string("replay://recordings/run"), replay + "?values=canned",
          replay + "?loop=2", replay + "?speed=2", replay + "?from=x",
          replay + "?to=1.2345", replay + "?from=5&to=5",
          std::string("mock://sim?from=1")}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad.c_str(), &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
    // and mock:// never takes replay's key
    EXPECT_EQ(ecuio_sim_parse_uri("mock://sim?loop=1", &c, why, sizeof why),
              ecuio_sim_uri_result::bad);
}

// Every Nth reply of its kind, single-frame and segmented counted apart;
// the bitmaps never; the frame faults on segmented replies only
TEST(SimEcuTest, FaultsBreakEveryNthReplyOfItsKind)
{
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    e.config.faults.silent = 3;
    e.config.faults.bad_sequence = 2;
    const uint8_t pid[2] = {0x01, 0x0C};
    const uint8_t bitmap[2] = {0x01, 0x00};
    std::vector<ecuio_sim_fate> singles;
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(ecuio_sim_next_fate(e, bitmap, 2, 6), ecuio_sim_fate::intact);
        singles.push_back(ecuio_sim_next_fate(e, pid, 2, 4));
    }
    EXPECT_EQ(singles, (std::vector<ecuio_sim_fate>{
                           ecuio_sim_fate::intact, ecuio_sim_fate::intact,
                           ecuio_sim_fate::silent, ecuio_sim_fate::intact,
                           ecuio_sim_fate::intact, ecuio_sim_fate::silent}));
    const uint8_t pack[4] = {0x01, 0x0C, 0x0D, 0x05};
    EXPECT_EQ(ecuio_sim_next_fate(e, pack, 4, 9), ecuio_sim_fate::intact);
    EXPECT_EQ(ecuio_sim_next_fate(e, pack, 4, 9), ecuio_sim_fate::bad_sequence);
    EXPECT_EQ(ecuio_sim_next_fate(e, pack, 4, 9), ecuio_sim_fate::silent);
    EXPECT_EQ(e.singles, 6u);
    EXPECT_EQ(e.segmented, 3u);
}

// pending: a fault like the others, every Nth reply of its kind, never a
// bitmap; the second module's replies counted apart from the engine
// ECU's, so its period is its own
TEST(SimEcuTest, PendingIsEveryNthReplyAndEachModuleCountsItsOwn)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?faults=pending:3,late:5", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.faults.pending, 3u);
    EXPECT_EQ(c.faults.late, 5u);
    EXPECT_EQ(
        ecuio_sim_parse_uri("mock://sim?faults=pending:0", &c, why, sizeof why),
        ecuio_sim_uri_result::bad);
    ecuio_sim_ecu e = sim_of(ecuio_sim_values::drive);
    e.config.faults.pending = 3;
    const uint8_t pid[2] = {0x01, 0x0C};
    const uint8_t bitmap[2] = {0x01, 0x20};
    const uint8_t did[3] = {0x22, 0xD1, 0x00};
    std::vector<ecuio_sim_fate> engine, module;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(ecuio_sim_next_fate(e, bitmap, 2, 6), ecuio_sim_fate::intact);
        engine.push_back(ecuio_sim_next_fate(e, pid, 2, 4));
    }
    module.push_back(ecuio_sim_next_fate(e, did, 3, 4, 1));
    module.push_back(ecuio_sim_next_fate(e, did, 3, 4, 1));
    module.push_back(ecuio_sim_next_fate(e, did, 3, 4, 1));
    const std::vector<ecuio_sim_fate> third = {ecuio_sim_fate::intact,
                                               ecuio_sim_fate::intact,
                                               ecuio_sim_fate::pending};
    EXPECT_EQ(engine, third);
    EXPECT_EQ(module, third) << "the module's own third reply, not the "
                                "engine ECU's sixth";
    EXPECT_EQ(e.singles, 3u);
    EXPECT_EQ(e.module_singles, 3u);
}

namespace {

std::vector<uint8_t> module_ask(std::initializer_list<uint8_t> req,
                                bool functional = false)
{
    const std::vector<uint8_t> q(req);
    uint8_t out[ECUIO_SIM_REPLY_MAX];
    const uint32_t n = ecuio_sim_module_answer(
        q.data(), static_cast<uint32_t>(q.size()), functional, out);
    return std::vector<uint8_t>(out, out + n);
}

} // namespace

// modules=2's module: Service 22 from its own made-up DIDs, 7F 22 31 for
// one it lacks, 7F <service> 11 for any other service, and a functional
// request's refusals unsaid; only mock:// takes modules, 1 or 2
TEST(SimEcuTest, TheSecondModuleAnswersItsOwnDidsAndRefusesTheRest)
{
    EXPECT_EQ(module_ask({0x22, 0xD1, 0x00}),
              (std::vector<uint8_t>{0x62, 0xD1, 0x00, 0x82}));
    EXPECT_EQ(module_ask({0x22, 0xD1, 0x01}),
              (std::vector<uint8_t>{0x62, 0xD1, 0x01, 0x03}));
    const std::vector<uint8_t> name = module_ask({0x22, 0xD1, 0x02});
    ASSERT_EQ(name.size(), 15u) << "past a Single Frame";
    EXPECT_EQ(std::string(name.begin() + 3, name.end()), "TCM-HavneSim");
    EXPECT_EQ(module_ask({0x22, 0xF1, 0x90}),
              (std::vector<uint8_t>{0x7F, 0x22, 0x31}));
    EXPECT_EQ(module_ask({0x22, 0xD1}),
              (std::vector<uint8_t>{0x7F, 0x22, 0x31}));
    EXPECT_EQ(module_ask({0x01, 0x0C}),
              (std::vector<uint8_t>{0x7F, 0x01, 0x11}));
    EXPECT_EQ(module_ask({0x09, 0x02}),
              (std::vector<uint8_t>{0x7F, 0x09, 0x11}));
    EXPECT_EQ(module_ask({0x22, 0xD1, 0x00}, true),
              (std::vector<uint8_t>{0x62, 0xD1, 0x00, 0x82}))
        << "a positive reply to a functional request is said";
    EXPECT_TRUE(module_ask({0x01, 0x0C}, true).empty());
    EXPECT_TRUE(module_ask({0x22, 0xF1, 0x90}, true).empty());
    EXPECT_TRUE(module_ask({}).empty());

    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_FALSE(c.second_module) << "one ECU by default";
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?modules=2&values=canned", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_TRUE(c.second_module);
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?modules=1", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_FALSE(c.second_module);
#ifdef _WIN32
    const std::string replay = "replay://C:/recordings/run?modules=2";
#else
    const std::string replay = "replay:///recordings/run?modules=2";
#endif
    for (const std::string& bad :
         {std::string("mock://sim?modules=3"),
          std::string("mock://sim?modules=0"),
          std::string("mock://sim?modules="), replay}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad.c_str(), &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
}

/* ---------- DIDs from a signal set (ADR-0046's appendix) ---------- */

namespace {

/* A def keyed 62 <did> at offset 0 on rx */
ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did, uint8_t kind,
                           uint8_t order, uint16_t start, uint8_t bit,
                           uint8_t bits, double scale, double offset,
                           const char* name)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_offset = 0;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = kind;
    d.byte_order = order;
    d.start_byte = start;
    d.bit_offset = bit;
    d.bit_length = bits;
    d.scale = scale;
    d.offset = offset;
    std::snprintf(d.name, sizeof d.name, "%s", name);
    return d;
}

ecuio_signal_set_t
set_of(const std::vector<ecuio_signal_def_t>& defs,
       const std::vector<ecuio_channel_decl_t>& channels = {})
{
    ecuio_signal_set_t s{};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(defs.size());
    s.defs = defs.data();
    s.channel_count = static_cast<uint32_t>(channels.size());
    s.channels = channels.empty() ? nullptr : channels.data();
    return s;
}

std::vector<uint8_t> did_ask(const ecuio_signal_set_t& set, uint32_t rx,
                             std::initializer_list<uint8_t> req,
                             const std::vector<double>& values,
                             bool functional = false)
{
    const std::vector<uint8_t> q(req);
    uint8_t out[ECUIO_SIM_REPLY_MAX];
    const uint32_t n = ecuio_sim_did_answer(
        set, rx, q.data(), static_cast<uint32_t>(q.size()), functional,
        [&values](uint32_t i) { return values[i]; }, out, sizeof out);
    return std::vector<uint8_t>(out, out + n);
}

} // namespace

// Every field of one DID's reply, each kind and byte order, decodes
// through its own def to the value it was given: a big-endian word with
// an offset, a signed little-endian field at a bit offset, a binary32,
// a one-bit flag in a byte another field shares, and a text field its
// name
TEST(SimDidsTest, EachFieldDecodesToTheValueItWasGiven)
{
    const std::vector<ecuio_signal_def_t> defs = {
        did_def(1, 0x7E9, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                16, 1.0, -40.0, "Fluid"),
        did_def(2, 0x7E9, 0x1234, ECUIO_VALUE_INT, ECUIO_BYTE_ORDER_LE, 5, 4,
                12, 0.5, 0.0, "Torque"),
        did_def(3, 0x7E9, 0x1234, ECUIO_VALUE_FLOAT, ECUIO_BYTE_ORDER_BE, 7, 0,
                32, 1.0, 0.0, "Ratio"),
        did_def(4, 0x7E9, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_LE, 5, 0,
                1, 1.0, 0.0, "Lockup"),
        did_def(5, 0x7E9, 0x1235, ECUIO_VALUE_TEXT, ECUIO_BYTE_ORDER_LE, 3, 0,
                0, 1.0, 0.0, "TCM-Sim"),
    };
    const std::vector<ecuio_channel_decl_t> channels = {{0x7E9, 0x7E1}};
    const ecuio_signal_set_t set = set_of(defs, channels);
    ASSERT_EQ(ecuio_validate_signal_set(&set, nullptr), 0)
        << "defs a session would take";
    const std::vector<double> values = {90.0, -100.5, 1.25, 1.0, 0.0};
    const std::vector<uint8_t> r =
        did_ask(set, 0x7E9, {0x22, 0x12, 0x34}, values);
    ASSERT_EQ(r.size(), 11u)
        << "62 12 34, then the fields to the binary32's end";
    EXPECT_EQ(r[0], 0x62);
    EXPECT_EQ(r[3], 0x00);
    EXPECT_EQ(r[4], 0x82) << "90 + 40 = 130, big-endian";
    for (size_t i = 0; i < 4; ++i) {
        double v = -1.0;
        ASSERT_EQ(ecuio_decode_signal(&defs[i], r.data(),
                                      static_cast<uint16_t>(r.size()), &v),
                  ECUIO_DECODE_OK)
            << defs[i].name;
        EXPECT_EQ(v, values[i]) << defs[i].name;
    }
    const std::vector<uint8_t> t =
        did_ask(set, 0x7E9, {0x22, 0x12, 0x35}, values);
    char text[ECUIO_DECODE_TEXT_MAX] = {};
    ASSERT_EQ(ecuio_decode_text(&defs[4], t.data(),
                                static_cast<uint16_t>(t.size()), text,
                                sizeof text),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(text, "TCM-Sim");
}

// The values a field holds and the ones it can't: a byte with an offset
// runs offset to offset + 255, a signed byte -128 to 127, a value rounds
// to the field's step, and NaN, a text def, or a zero scale off its
// offset hold none
TEST(SimDidsTest, AFieldsRangeIsWhatItAnswers)
{
    uint32_t raw = 0;
    const ecuio_signal_def_t u8 =
        did_def(1, 0x7E8, 1, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 8,
                1.0, -40.0, "u8");
    EXPECT_TRUE(ecuio_sim_raw_for(u8, -40.0, &raw));
    EXPECT_EQ(raw, 0u);
    EXPECT_TRUE(ecuio_sim_raw_for(u8, 215.0, &raw));
    EXPECT_EQ(raw, 255u);
    EXPECT_FALSE(ecuio_sim_raw_for(u8, 216.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(u8, -41.0, &raw));
    const ecuio_signal_def_t s8 =
        did_def(2, 0x7E8, 1, ECUIO_VALUE_INT, ECUIO_BYTE_ORDER_BE, 3, 0, 8, 1.0,
                0.0, "s8");
    EXPECT_TRUE(ecuio_sim_raw_for(s8, -128.0, &raw));
    EXPECT_EQ(raw, 0x80u);
    EXPECT_TRUE(ecuio_sim_raw_for(s8, 127.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(s8, 128.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(s8, -129.0, &raw));
    const ecuio_signal_def_t tenth =
        did_def(3, 0x7E8, 1, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 16,
                0.1, 0.0, "tenth");
    EXPECT_TRUE(ecuio_sim_raw_for(tenth, 1.04, &raw));
    EXPECT_EQ(raw, 10u) << "the nearest step";
    EXPECT_FALSE(ecuio_sim_raw_for(tenth, std::nan(""), &raw));
    const ecuio_signal_def_t text =
        did_def(4, 0x7E8, 1, ECUIO_VALUE_TEXT, ECUIO_BYTE_ORDER_LE, 3, 0, 0,
                1.0, 0.0, "text");
    EXPECT_FALSE(ecuio_sim_raw_for(text, 0.0, &raw));
    const ecuio_signal_def_t flat =
        did_def(5, 0x7E8, 1, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 8,
                0.0, 7.0, "flat");
    EXPECT_TRUE(ecuio_sim_raw_for(flat, 7.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(flat, 8.0, &raw));
}

// What isn't the set's: a DID none of its defs reads, one on another
// module, a list of DIDs, another service; a functional request's
// refusals unsaid, its answer said
TEST(SimDidsTest, WhatTheSetDoesntReadIsRefused)
{
    const std::vector<ecuio_signal_def_t> defs = {
        did_def(1, 0x7E9, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                8, 1.0, 0.0, "a"),
        did_def(2, 0x7EA, 0x5678, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                8, 1.0, 0.0, "b"),
    };
    const ecuio_signal_set_t set = set_of(defs);
    const std::vector<double> values = {7.0, 9.0};
    const std::vector<uint8_t> no = {0x7F, 0x22, 0x31};
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x12, 0x34}, values),
              (std::vector<uint8_t>{0x62, 0x12, 0x34, 0x07}));
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0xF1, 0x90}, values), no);
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x56, 0x78}, values), no)
        << "0x7EA's DID";
    EXPECT_EQ(did_ask(set, 0x7EA, {0x22, 0x56, 0x78}, values),
              (std::vector<uint8_t>{0x62, 0x56, 0x78, 0x09}));
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x12, 0x34, 0x56, 0x78}, values), no);
    EXPECT_EQ(did_ask(set, 0x7E9, {0x01, 0x0C}, values),
              (std::vector<uint8_t>{0x7F, 0x01, 0x11}));
    EXPECT_TRUE(did_ask(set, 0x7E9, {0x22, 0xF1, 0x90}, values, true).empty());
    EXPECT_TRUE(did_ask(set, 0x7E9, {0x01, 0x0C}, values, true).empty());
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x12, 0x34}, values, true).size(), 4u);
    EXPECT_TRUE(did_ask(set, 0x7E9, {}, values).empty());
}

// A set's channels are the simulated car's modules: the engine ECU's
// pair and none at all both put the DIDs on the engine ECU, every other
// channel is a module; a 29-bit ID, the functional ID, or half of the
// engine ECU's pair is refused, why naming the channel
TEST(SimDidsTest, TheModulesAreTheSetsChannels)
{
    ecuio_sim_module_plan plan;
    char why[256] = {};
    const ecuio_signal_set_t none = set_of({});
    ASSERT_TRUE(ecuio_sim_plan_modules(none, &plan, why, sizeof why));
    EXPECT_TRUE(plan.engine);
    EXPECT_EQ(plan.count, 0u);
    const std::vector<ecuio_channel_decl_t> two = {{0x7E8, 0x7E0},
                                                   {0x7EA, 0x7E2}};
    ASSERT_TRUE(
        ecuio_sim_plan_modules(set_of({}, two), &plan, why, sizeof why));
    EXPECT_TRUE(plan.engine);
    ASSERT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.tx[0], 0x7E2u);
    EXPECT_EQ(plan.rx[0], 0x7EAu);
    const std::vector<ecuio_channel_decl_t> module = {{0x7E9, 0x7E1}};
    ASSERT_TRUE(
        ecuio_sim_plan_modules(set_of({}, module), &plan, why, sizeof why));
    EXPECT_FALSE(plan.engine) << "the engine ECU answers no DID of the set";
    EXPECT_EQ(plan.count, 1u);
    for (const std::vector<ecuio_channel_decl_t>& bad :
         {std::vector<ecuio_channel_decl_t>{{0x18DAF110, 0x18DA10F1}},
          std::vector<ecuio_channel_decl_t>{{0x7E9, 0x7DF}},
          std::vector<ecuio_channel_decl_t>{{0x7EA, 0x7E0}},
          std::vector<ecuio_channel_decl_t>{{0x7E8, 0x7E2}}}) {
        why[0] = '\0';
        EXPECT_FALSE(
            ecuio_sim_plan_modules(set_of({}, bad), &plan, why, sizeof why))
            << std::hex << bad[0].rx_id;
        EXPECT_NE(std::strstr(why, "channel 0"), nullptr) << why;
    }
}

// profile= names a registered profile by the registry's grammar, on
// mock:// alone, and never beside modules=2
TEST(SimDidsTest, TheUrisProfile)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?profile=user.daily-extras_2", &c,
                                  why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(std::string(c.profile, c.profile_len), "user.daily-extras_2");
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_EQ(c.profile_len, 0u);
#ifdef _WIN32
    const std::string replay = "replay://C:/recordings/run?profile=obd2";
#else
    const std::string replay = "replay:///recordings/run?profile=obd2";
#endif
    for (const std::string& bad :
         {std::string("mock://sim?profile="),
          std::string("mock://sim?profile=Obd2"),
          std::string("mock://sim?profile=a/b"),
          std::string("mock://sim?profile=") + std::string(64, 'a'),
          std::string("mock://sim?profile=obd2&modules=2"), replay}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad.c_str(), &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
}

/* ---------- a recording as the simulated ECU (ADR-0046 section 7) ---------- */

namespace {

constexpr int64_t kSecond = 1000000;

/* One recorded frame: the engine ECU's unless id says otherwise */
void take(ecuio_sim_recording& r, int64_t at_us,
          std::initializer_list<uint8_t> data, uint32_t id = 0x7E8,
          uint32_t flags = 0)
{
    std::unique_ptr<ecuio_frame_t> f(new ecuio_frame_t());
    f->arbitration_id = id;
    f->flags = flags;
    f->data_length = static_cast<uint16_t>(data.size());
    std::copy(data.begin(), data.end(), f->data);
    ecuio_sim_recording_take(r, at_us, *f);
}

std::vector<uint8_t> rask(const ecuio_sim_recording& r, int64_t now_us,
                          std::initializer_list<uint8_t> req,
                          const char* vin = nullptr)
{
    const std::vector<uint8_t> q(req);
    std::vector<uint8_t> out(ECUIO_SIM_RECORDING_REPLY_MAX);
    const uint32_t n = ecuio_sim_recording_answer(
        r, now_us, q.data(), static_cast<uint32_t>(q.size()), vin, out.data());
    out.resize(n);
    return out;
}

using Bytes = std::vector<uint8_t>;

/* A scratch frames.csv in TEMP/TMPDIR, removed when it goes */
struct ScratchCsv {
    std::string path;
    explicit ScratchCsv(const char* name, const std::string& text)
    {
        const char* dir = std::getenv("TEMP");
        if (dir == nullptr) {
            dir = std::getenv("TMPDIR");
        }
        path = std::string(dir != nullptr ? dir : "/tmp") + "/" +
               temp_name(std::string("ecuio-") + name) + ".csv";
        FILE* f = std::fopen(path.c_str(), "wb");
        EXPECT_NE(f, nullptr) << path;
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    ~ScratchCsv() { std::remove(path.c_str()); }
};

} // namespace

// Each kept reply answers the request it came for; a refusal, a clear's
// 44, another ECU's reply, an error, a 29-bit or a cut-short frame isn't
// kept
TEST(SimRecordingTest, KeepsEachReplyUnderTheRequestItAnswers)
{
    ecuio_sim_recording r;
    take(r, 0, {0x41, 0x0C, 0x1A, 0xF8});
    take(r, 0, {0x42, 0x0C, 0x00, 0x2E, 0xE0});
    take(r, 0, {0x43, 0x01, 0x03, 0x01});
    take(r, 0, {0x47, 0x00});
    take(r, 0, {0x4A, 0x00});
    take(r, 0, {0x49, 0x0A, 0x01, 'E', 'C', 'M'});
    take(r, 0, {0x62, 0x1E, 0x05, 0x55});
    take(r, 0, {0x7F, 0x22, 0x31});
    take(r, 0, {0x44});
    take(r, 0, {0x41, 0x0D, 0x32}, 0x7E9);
    take(r, 0, {0x41, 0x05, 0x7B}, 0x7E8, ECUIO_FRAME_FLAG_ERROR);
    take(r, 0, {0x41, 0x0F, 0x50}, 0x7E8, ECUIO_FRAME_FLAG_EXTENDED);
    take(r, 0, {0x41, 0x10, 0x01}, 0x7E8, ECUIO_FRAME_FLAG_TRUNCATED);
    EXPECT_EQ(r.replies.size(), 7u);
    EXPECT_EQ(rask(r, 0, {0x01, 0x0C}), (Bytes{0x41, 0x0C, 0x1A, 0xF8}));
    EXPECT_EQ(rask(r, 0, {0x02, 0x0C, 0x00}),
              (Bytes{0x42, 0x0C, 0x00, 0x2E, 0xE0}));
    EXPECT_EQ(rask(r, 0, {0x03}), (Bytes{0x43, 0x01, 0x03, 0x01}));
    EXPECT_EQ(rask(r, 0, {0x07}), (Bytes{0x47, 0x00}));
    EXPECT_EQ(rask(r, 0, {0x0A}), (Bytes{0x4A, 0x00}));
    EXPECT_EQ(rask(r, 0, {0x09, 0x0A}),
              (Bytes{0x49, 0x0A, 0x01, 'E', 'C', 'M'}));
    EXPECT_EQ(rask(r, 0, {0x22, 0x1E, 0x05}), (Bytes{0x62, 0x1E, 0x05, 0x55}));
    for (uint8_t pid : std::initializer_list<uint8_t>{0x0D, 0x05, 0x0F, 0x10}) {
        EXPECT_EQ(rask(r, 0, {0x01, pid}), (Bytes{0x7F, 0x01, 0x12}))
            << "not kept: " << int(pid);
    }
}

// The reply at the ECU's time: the first before it, the last at or
// before it between, the last after the end (held), or round the span
TEST(SimRecordingTest, AnswersAtTheEcusTime)
{
    ecuio_sim_recording r;
    take(r, 1 * kSecond, {0x41, 0x0D, 10});
    take(r, 3 * kSecond, {0x41, 0x0D, 30});
    take(r, 5 * kSecond, {0x41, 0x0D, 50});
    EXPECT_EQ(r.span_us, 5 * kSecond);
    const auto speed = [&](int64_t at) { return rask(r, at, {0x01, 0x0D})[2]; };
    EXPECT_EQ(speed(0), 10);
    EXPECT_EQ(speed(2 * kSecond), 10);
    EXPECT_EQ(speed(3 * kSecond), 30);
    EXPECT_EQ(speed(5 * kSecond - 1), 30);
    EXPECT_EQ(speed(5 * kSecond), 50);
    EXPECT_EQ(speed(60 * kSecond), 50) << "held after the end";
    r.loop = true;
    EXPECT_EQ(speed(6 * kSecond), 10) << "6 s is 1 s into the second lap";
    EXPECT_EQ(speed(8 * kSecond), 30);
    EXPECT_EQ(speed(14 * kSecond), 30);
}

// A packed reply (ADR-0039 Appendix A) is split into its PIDs, and a pack
// asked is put together from each PID's reply now
TEST(SimRecordingTest, PacksAreSplitAndPutTogether)
{
    ecuio_sim_recording r;
    take(r, 0, {0x41, 0x0C, 0x1A, 0xF8, 0x0D, 0x32, 0x05, 0x7B});
    take(r, 2 * kSecond, {0x41, 0x0D, 0x40});
    EXPECT_EQ(rask(r, 0, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x32}));
    EXPECT_EQ(rask(r, 0, {0x01, 0x05}), (Bytes{0x41, 0x05, 0x7B}));
    EXPECT_EQ(rask(r, 3 * kSecond, {0x01, 0x05, 0x0D, 0x0C}),
              (Bytes{0x41, 0x05, 0x7B, 0x0D, 0x40, 0x0C, 0x1A, 0xF8}))
        << "in the order asked, each PID's newest";
    EXPECT_EQ(rask(r, 0, {0x01, 0x0C, 0x11}), (Bytes{0x41, 0x0C, 0x1A, 0xF8}))
        << "a PID it doesn't hold left out";
    EXPECT_TRUE(rask(r, 0, {0x01, 0x11, 0x0F}).empty()) << "none: silence";
}

// The car's own bitmaps when the recording holds any (the rest refused,
// as that car's chain is what it is); worked out from the PIDs it holds
// when it holds none
TEST(SimRecordingTest, TheBitmapsAreTheCarsOrWorkedOut)
{
    ecuio_sim_recording car;
    take(car, 0, {0x41, 0x00, 0xBE, 0x3E, 0xB8, 0x11});
    take(car, 0, {0x41, 0x0C, 0x1A, 0xF8});
    take(car, 0, {0x41, 0x2F, 0x80}); /* a PID past 0x20, its bitmap unsent */
    EXPECT_TRUE(car.bitmaps);
    EXPECT_EQ(rask(car, 0, {0x01, 0x00}),
              (Bytes{0x41, 0x00, 0xBE, 0x3E, 0xB8, 0x11}));
    EXPECT_EQ(rask(car, 0, {0x01, 0x20}), (Bytes{0x7F, 0x01, 0x12}))
        << "the car's chain, not one worked out";

    ecuio_sim_recording none;
    for (uint8_t pid :
         std::initializer_list<uint8_t>{0x05, 0x0C, 0x0D, 0x2F, 0x42}) {
        take(none, 0, {0x41, pid, 0x00, 0x00});
    }
    EXPECT_FALSE(none.bitmaps);
    // 05 is bit 4, 0C and 0D bits 11 and 12, 20 bit 31 (data past it)
    EXPECT_EQ(rask(none, 0, {0x01, 0x00}),
              (Bytes{0x41, 0x00, 0x08, 0x18, 0x00, 0x01}));
    EXPECT_EQ(rask(none, 0, {0x01, 0x20}),
              (Bytes{0x41, 0x20, 0x00, 0x02, 0x00, 0x01}));
    EXPECT_EQ(rask(none, 0, {0x01, 0x40}),
              (Bytes{0x41, 0x40, 0x40, 0x00, 0x00, 0x00}));
    EXPECT_EQ(rask(none, 0, {0x01, 0x60}), (Bytes{0x7F, 0x01, 0x12}))
        << "nothing past 0x40";
}

// A VIN given answers 09 02 instead of the recording's; what it doesn't
// hold is refused, a clear always
TEST(SimRecordingTest, TheVinGivenAndTheRefusals)
{
    ecuio_sim_recording r;
    take(r, 0, {0x49, 0x02, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
                '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'});
    take(r, 0, {0x41, 0x0C, 0x1A, 0xF8});
    EXPECT_EQ(rask(r, 0, {0x09, 0x02})[3], 'Z') << "the recording's";
    const Bytes given = rask(r, 0, {0x09, 0x02}, "1FAFAKE10M0000001");
    ASSERT_EQ(given.size(), 20u);
    EXPECT_EQ(std::string(given.begin() + 3, given.end()), "1FAFAKE10M0000001");
    EXPECT_EQ(rask(r, 0, {0x09, 0x04}), (Bytes{0x7F, 0x09, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x01, 0x05}), (Bytes{0x7F, 0x01, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x03}), (Bytes{0x7F, 0x03, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x02, 0x0C, 0x00}), (Bytes{0x7F, 0x02, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x22, 0xF1, 0x90}), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(rask(r, 0, {0x22, 0xF1, 0x90, 0xF1, 0x8C}),
              (Bytes{0x7F, 0x22, 0x13}));
    EXPECT_EQ(rask(r, 0, {0x04}), (Bytes{0x7F, 0x04, 0x22}));
    EXPECT_EQ(rask(r, 0, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x11}));
}

// Through the simulated ECU: its ticks are the recording's time, its
// functional refusals kept to itself, its VIN given or not
TEST(SimRecordingTest, TheEcuAnswersFromItsRecording)
{
    ecuio_sim_recording r;
    take(r, 0, {0x41, 0x0D, 10});
    take(r, 3 * kSecond, {0x41, 0x0D, 30});
    ecuio_sim_ecu e;
    e.recording = &r;
    EXPECT_EQ(ask(e, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 10}));
    for (int i = 0; i < 60; ++i) { /* 3 s of 50 ms ticks */
        ecuio_sim_tick(e);
    }
    EXPECT_EQ(e.time_us, 3 * kSecond);
    EXPECT_EQ(ask(e, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 30}));
    EXPECT_EQ(ask(e, {0x01, 0x05}), (Bytes{0x7F, 0x01, 0x12}));
    EXPECT_TRUE(ask(e, {0x01, 0x05}, true).empty()) << "functional: silence";
    EXPECT_EQ(ask(e, {0x09, 0x02}), (Bytes{0x7F, 0x09, 0x12}));
    std::memcpy(e.config.vin, "1FAFAKE10M0000001", 18);
    e.config.vin_given = true;
    EXPECT_EQ(ask(e, {0x09, 0x02}).size(), 20u);
    EXPECT_EQ(ask(e, {0x04}), (Bytes{0x7F, 0x04, 0x22})) << "never clears";
}

// The committed fixture (the test car's engine start): the car's
// bitmaps, its PIDs over the recording, its refusals not kept; the
// folder or the file
TEST(SimRecordingTest, TheLoaderReadsTheFixture)
{
    const std::string dir =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    for (const std::string& path : {dir, dir + "/frames.csv"}) {
        ecuio_sim_recording r;
        ASSERT_EQ(ecuio_sim_load_recording(path, &r), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_TRUE(r.bitmaps);
        EXPECT_EQ(rask(r, 0, {0x01, 0x00}),
                  (Bytes{0x41, 0x00, 0xFE, 0x7F, 0xA8, 0x13}))
            << "the fixture's first row";
        EXPECT_GT(r.span_us, 10 * kSecond);
        const Bytes first = rask(r, 0, {0x01, 0x0C});
        const Bytes last = rask(r, r.span_us, {0x01, 0x0C});
        ASSERT_EQ(first.size(), 4u);
        ASSERT_EQ(last.size(), 4u);
        EXPECT_NE(first, last) << "the engine speed moves over the start";
        EXPECT_EQ(rask(r, 0, {0x22, 0xF1, 0x90}), (Bytes{0x7F, 0x22, 0x31}))
            << "its refusals aren't replies";
        EXPECT_EQ(r.replies.size(), 15u) << "9 data PIDs, 6 bitmaps";
    }
}

// What the loader refuses, by the line, never naming the path
// A last row with no newline is a write cut short (ADR-0049 section 7):
// left out, whether or not it would parse
TEST(SimRecordingTest, ATornLastRowIsLeftOut)
{
    ScratchCsv csv("sim-torn", "host_us,timestamp_us,id,flags,data\n"
                               "1,1,0x7E8,0x0,41 0C 1A F8\n"
                               "2,2,0x7E8,0x0,41 0D 20");
    ecuio_sim_recording r;
    ASSERT_EQ(ecuio_sim_load_recording(csv.path, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(r.replies.size(), 1u) << "only 41 0C's reply";
}

// A stretch (from and to, by host_us) answers from its own rows, its
// first at the ECU's start: before it, after it, or a stretch of a reply
// the engine ECU never sent, nothing of it is kept
TEST(SimRecordingTest, AStretchAnswersFromItsOwnRows)
{
    ScratchCsv csv("sim-stretch", "host_us,timestamp_us,id,flags,data\n"
                                  "0,500,0x7E8,0x0,41 0D 0A\n"
                                  "1000000,1000500,0x7E8,0x0,41 0D 14\n"
                                  "2000000,1500500,0x7E8,0x0,41 0D 1E\n"
                                  "3000000,2000500,0x7E9,0x0,41 0D 28\n"
                                  "4000000,2500500,0x7E8,0x0,41 0D 32\n");
    ecuio_stretch::stretch s;
    s.from_us = 900000;
    s.to_us = 2000000;
    ecuio_sim_recording r;
    ASSERT_EQ(ecuio_sim_load_recording(csv.path, &r, s), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(rask(r, 0, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x14}))
        << "the stretch's first row at 0";
    EXPECT_EQ(rask(r, 499999, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x14}));
    EXPECT_EQ(rask(r, 500000, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x1E}))
        << "its next by the session time between them";
    EXPECT_EQ(r.span_us, 500000);
    EXPECT_EQ(rask(r, 10 * kSecond, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x1E}))
        << "held past its end: row 5 isn't in it";

    s.from_us = 2500000;
    s.to_us = 3500000;
    EXPECT_EQ(ecuio_sim_load_recording(csv.path, &r, s), ECUIO_ERR_LOAD_FAILED)
        << "0x7E9's row alone";
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the stretch holds no reply from the engine ECU (0x7E8) to "
                 "answer with");
    s.from_us = 5000000;
    s.to_us = ecuio_stretch::kOpen;
    EXPECT_EQ(ecuio_sim_load_recording(csv.path, &r, s), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the recording holds no frames from 5 s on: its frames run "
                 "from 0 s to 4 s");
}

TEST(SimRecordingTest, TheLoadersRefusals)
{
    const std::string header = "host_us,timestamp_us,id,flags,data\n";
    struct Case {
        const char* name;
        std::string text;
        const char* says;
    };
    const Case cases[] = {
        {"sim-not-frames", "time,value\n1,2\n", "header row"},
        {"sim-bad-row", header + "1,1,0x7E8,0x0,41 0C 1A F8\n2,2,7E8,0x0,41\n",
         "frames.csv line 3"},
        {"sim-no-ecu", header + "1,1,0x7E9,0x0,41 0C 1A F8\n",
         "no reply from the engine ECU"},
        {"sim-empty", "", "empty"},
    };
    for (const Case& c : cases) {
        ScratchCsv csv(c.name, c.text);
        ecuio_sim_recording r;
        EXPECT_EQ(ecuio_sim_load_recording(csv.path, &r), ECUIO_ERR_LOAD_FAILED)
            << c.name;
        const std::string detail = ecuio_last_error_detail();
        EXPECT_NE(detail.find(c.says), std::string::npos) << detail;
        EXPECT_EQ(detail.find("ecuio-"), std::string::npos) << detail;
    }
    ecuio_sim_recording r;
    EXPECT_EQ(ecuio_sim_load_recording(std::string(ECUIO_REPLAY_FIXTURES) +
                                           "/no-such-recording",
                                       &r),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("couldn't open"),
              std::string::npos);
}

/* ---------- The unbound sim (ADR-0055) ---------- */

namespace {

/* An unbound sim, destroyed at the end */
struct Unbound {
    ecuio_sim_t* sim = nullptr;
    explicit Unbound(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    ecuio_sim_stats_t stats() const
    {
        ecuio_sim_stats_t s{};
        EXPECT_EQ(ecuio_sim_get_stats(sim, &s), ECUIO_OK);
        return s;
    }
};

/* Each reply to req on id at t (the caller's microseconds) */
std::vector<ecuio_sim_reply_t> sim_ask(ecuio_sim_t* sim, uint32_t id,
                                       std::initializer_list<uint8_t> req,
                                       int64_t t = 0)
{
    const std::vector<uint8_t> q(req);
    std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
    uint32_t n = 0;
    EXPECT_EQ(ecuio_sim_ask(sim, id, q.data(), static_cast<uint32_t>(q.size()),
                            t, out.data(), static_cast<uint32_t>(out.size()),
                            &n),
              ECUIO_OK)
        << ecuio_last_error_detail();
    out.resize(n);
    return out;
}

Bytes reply_bytes(const ecuio_sim_reply_t& r)
{
    return Bytes(r.data, r.data + r.length);
}

/* The engine speed in rpm from a 41 0C reply */
double rpm_of(const ecuio_sim_reply_t& r)
{
    EXPECT_EQ(r.length, 4u);
    return (r.data[2] * 256 + r.data[3]) / 4.0;
}

} // namespace

// The unbound sim is the same car as the bus's: its engine ECU answers
// its own request ID and the functional one as sim_ecu.h does, and an ID
// no ECU hears gets nothing and isn't counted
TEST(SimAskTest, AnUnboundSimAnswersAsTheEngineEcu)
{
    Unbound u("mock://ask?values=canned");
    ASSERT_NE(u.sim, nullptr);
    ecuio_sim_ecu canned;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://ask?values=canned", &canned.config,
                                  why, sizeof why),
              ecuio_sim_uri_result::ok);
    for (const Bytes& q :
         {Bytes{0x01, 0x00}, Bytes{0x01, 0x0C}, Bytes{0x09, 0x02}, Bytes{0x03},
          Bytes{0x22, 0xF1, 0x90}}) {
        uint8_t want[ECUIO_SIM_REPLY_MAX];
        const uint32_t want_len = ecuio_sim_answer(
            canned, q.data(), static_cast<uint32_t>(q.size()), false, want);
        std::vector<ecuio_sim_reply_t> got(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        ASSERT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q.data(),
                                static_cast<uint32_t>(q.size()), 0, got.data(),
                                ECUIO_SIM_MAX_REPLIES, &n),
                  ECUIO_OK);
        ASSERT_EQ(n, 1u) << std::hex << int{q[0]};
        EXPECT_EQ(got[0].reply_id, 0x7E8u);
        EXPECT_EQ(got[0].flags, 0u);
        EXPECT_EQ(got[0].delay_us, 0);
        EXPECT_EQ(reply_bytes(got[0]), Bytes(want, want + want_len));
        for (uint32_t i = 0; i < 5; ++i) {
            EXPECT_EQ(got[0].reserved[i], 0u);
        }
    }
    const std::vector<ecuio_sim_reply_t> functional =
        sim_ask(u.sim, 0x7DF, {0x01, 0x00});
    ASSERT_EQ(functional.size(), 1u);
    EXPECT_EQ(functional[0].reply_id, 0x7E8u);
    EXPECT_TRUE(sim_ask(u.sim, 0x7DF, {0x22, 0xF1, 0x90}).empty())
        << "a functional request's refusal is suppressed";

    const uint64_t before = u.stats().requests;
    EXPECT_TRUE(sim_ask(u.sim, 0x7E5, {0x01, 0x00}).empty());
    EXPECT_EQ(u.stats().requests, before) << "no ECU heard it";

    ASSERT_EQ(reply_bytes(sim_ask(u.sim, 0x7E0, {0x04})[0]), (Bytes{0x44}));
    const ecuio_sim_stats_t s = u.stats();
    EXPECT_EQ(s.clears, 1u);
    EXPECT_EQ(s.requests, 8u);
    EXPECT_EQ(s.replies, 7u);
    EXPECT_EQ(s.refused, 1u) << "22 F190 on the engine ECU";
    EXPECT_EQ(s.flags, 0u);
}

TEST(SimAskTest, TheContracts)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound(nullptr, 0, &sim),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_unbound("mock://a", 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_unbound("mock://a", 1, &sim),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sim, nullptr);
    EXPECT_EQ(ecuio_sim_create_unbound("can://a", 0, &sim), ECUIO_ERR_BAD_URI);
    EXPECT_EQ(
        ecuio_sim_create_unbound("mock://a?profile=sim-ask-none", 0, &sim),
        ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(ecuio_sim_create_unbound("replay://relative/frames.csv", 0, &sim),
              ECUIO_ERR_BAD_URI);
    const std::string missing =
        std::string(ECUIO_REPLAY_FIXTURES) + "/no-such-recording/frames.csv";
    EXPECT_EQ(
        ecuio_sim_create_unbound(("replay://" + missing).c_str(), 0, &sim),
        ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()).find(missing),
              std::string::npos)
        << "never the path";

    Unbound u("mock://ask");
    ASSERT_NE(u.sim, nullptr);
    const uint8_t q[2] = {0x01, 0x00};
    ecuio_sim_reply_t r[2];
    uint32_t n = 99;
    EXPECT_EQ(ecuio_sim_ask(nullptr, 0x7E0, q, 2, 0, r, 2, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, nullptr, 2, 0, r, 2, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, r, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, nullptr, 1, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 0, 0, r, 2, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 4096, 0, r, 2, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, -2, r, 2, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(n, 99u) << "a refusal leaves the count";
    EXPECT_EQ(u.stats().requests, 0u);

    /* capacity 0 counts the replies */
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7DF, q, 2, 0, nullptr, 0, &n), ECUIO_OK);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7DF, q, 2, ECUIO_SIM_NOW, r, 2, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 1u);
}

// The ECU's time is the caller's: the drive cycle's key-on holds the
// engine stopped for its first 15 s, it runs by 20 s, and a time before
// the last is taken as the last
TEST(SimAskTest, TheClockIsTheCallersAndNeverGoesBack)
{
    Unbound u("mock://ask");
    ASSERT_NE(u.sim, nullptr);
    std::vector<ecuio_sim_reply_t> r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 0);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(rpm_of(r[0]), 0.0) << "key on, the engine stopped";
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 10 * kSecond);
    EXPECT_EQ(rpm_of(r[0]), 0.0);
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 20 * kSecond);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_GT(rpm_of(r[0]), 500.0) << "the engine started at 15 s";
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 5 * kSecond);
    EXPECT_GT(rpm_of(r[0]), 500.0) << "the clock never goes back";

    /* A step past an hour moves the time and steps an hour of the
     * cycle at most: it answers at once, as the drive cycle reads an
     * hour of ticks on from 20 s */
    const auto before = std::chrono::steady_clock::now();
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 1000LL * 3600 * kSecond);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - before)
            .count();
    EXPECT_LT(took, 2.0) << "the step ran the whole thousand hours";
    ASSERT_EQ(r.size(), 1u);
    ecuio_mock::DriveCycle car;
    for (int i = 0; i < 20 * 20 + 3600 * 20; ++i) {
        car.step();
    }
    EXPECT_NEAR(rpm_of(r[0]), car.rpm, 0.25);
}

// A request is answered once, whatever the capacity: a capacity of 0
// counts the replies and is still a request, so the next one meets the
// second request's fault
TEST(SimAskTest, ACountOnlyCallIsARequestOfItsOwn)
{
    Unbound u("mock://ask?faults=silent:2");
    ASSERT_NE(u.sim, nullptr);
    const uint8_t q[] = {0x01, 0x0C};
    uint32_t n = 99;
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, nullptr, 0, &n), ECUIO_OK);
    EXPECT_EQ(n, 1u);
    std::vector<ecuio_sim_reply_t> r(10);
    n = 99;
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, r.data(), 10, &n), ECUIO_OK);
    EXPECT_EQ(n, 0u) << "the second request is the silent one";
    const ecuio_sim_stats_t s = u.stats();
    EXPECT_EQ(s.requests, 2u);
    EXPECT_EQ(s.silent, 1u);
    EXPECT_EQ(s.replies, 1u);
}

// Each fault as the reply's start and a flag: pending's 7F <service> 78
// at once and the reply 300 ms on, late's 130 ms, the two segmented
// breaks flagged, silent none; each counted as on a bus
TEST(SimAskTest, FaultsAreTheRepliesTimesAndFlags)
{
    {
        Unbound u("mock://ask?faults=pending:1");
        const std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E0, {0x01, 0x0C});
        ASSERT_EQ(r.size(), 2u);
        EXPECT_EQ(r[0].flags, ECUIO_SIM_REPLY_PENDING);
        EXPECT_EQ(r[0].delay_us, 0);
        EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x7F, 0x01, 0x78}));
        EXPECT_EQ(r[1].flags, 0u);
        EXPECT_EQ(r[1].delay_us, 300000);
        EXPECT_EQ(r[1].data[0], 0x41);
        EXPECT_EQ(u.stats().pending, 1u);
        EXPECT_EQ(u.stats().replies, 1u) << "the 78 isn't the reply";
        EXPECT_EQ(sim_ask(u.sim, 0x7E0, {0x01, 0x00}).size(), 1u)
            << "the bitmaps are never broken";
    }
    {
        Unbound u("mock://ask?faults=late:1");
        const std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E0, {0x01, 0x0C});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].flags, ECUIO_SIM_REPLY_LATE);
        EXPECT_EQ(r[0].delay_us, 130000);
        EXPECT_EQ(u.stats().late, 1u);
    }
    {
        Unbound u("mock://ask?faults=silent:1");
        EXPECT_TRUE(sim_ask(u.sim, 0x7E0, {0x01, 0x0C}).empty());
        EXPECT_EQ(u.stats().silent, 1u);
        EXPECT_EQ(u.stats().replies, 0u);
    }
    for (const char* uri :
         {"mock://ask?faults=bad_sequence:1", "mock://ask?faults=stalled:1"}) {
        Unbound u(uri);
        const bool bad = std::strstr(uri, "bad_sequence") != nullptr;
        std::vector<ecuio_sim_reply_t> r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].flags, 0u) << uri << ": a Single Frame can't break";
        r = sim_ask(u.sim, 0x7E0, {0x09, 0x02});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_GT(r[0].length, 7u);
        EXPECT_EQ(r[0].flags,
                  bad ? ECUIO_SIM_REPLY_BAD_SEQUENCE : ECUIO_SIM_REPLY_STALLED)
            << uri;
        const ecuio_sim_stats_t s = u.stats();
        EXPECT_EQ(bad ? s.bad_sequence : s.stalled, 1u) << uri;
        EXPECT_EQ(s.replies, 1u) << uri << ": a broken reply isn't whole";
    }
}

// modules=2's module answers its own IDs and, functionally, only what it
// has; a profile's DIDs answer the values they were given
TEST(SimAskTest, ModulesAndAProfilesDids)
{
    {
        Unbound u("mock://ask?modules=2");
        std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E1, {0x22, 0xD1, 0x01});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].reply_id, 0x7E9u);
        EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x62, 0xD1, 0x01, 0x03}));
        r = sim_ask(u.sim, 0x7DF, {0x22, 0xD1, 0x01});
        ASSERT_EQ(r.size(), 1u) << "the engine ECU's refusal is suppressed";
        EXPECT_EQ(r[0].reply_id, 0x7E9u);
        r = sim_ask(u.sim, 0x7DF, {0x01, 0x00});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].reply_id, 0x7E8u);
    }
    /* The registry keeps the pointers: static storage */
    static const ecuio_signal_def_t def =
        did_def(7, 0x7E8, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                16, 0.5, -10.0, "Ask field");
    static ecuio_signal_set_t set = set_of({});
    set.count = 1;
    set.defs = &def;
    ASSERT_EQ(ecuio_profile_register("sim-ask-dids", &set, nullptr), ECUIO_OK);
    {
        Unbound u("mock://ask?profile=sim-ask-dids");
        ASSERT_NE(u.sim, nullptr);
        ASSERT_EQ(ecuio_sim_set_value(u.sim, 7, 90.5), ECUIO_OK);
        const std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E0, {0x22, 0x12, 0x34});
        ASSERT_EQ(r.size(), 1u);
        /* (90.5 + 10) / 0.5 = 201 */
        EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x62, 0x12, 0x34, 0x00, 0xC9}));
    }
    EXPECT_EQ(ecuio_profile_unregister("sim-ask-dids"), ECUIO_OK);
}

// A recording answers at the caller's time: the fixture's engine start
// moves the engine speed from its first row to its last
TEST(SimAskTest, ARecordingAnswersAtTheCallersTime)
{
    const std::string uri = std::string("replay://") + ECUIO_REPLAY_FIXTURES +
                            "/2026-10-25-engine-start";
    Unbound u(uri.c_str());
    ASSERT_NE(u.sim, nullptr);
    std::vector<ecuio_sim_reply_t> r = sim_ask(u.sim, 0x7E0, {0x01, 0x00}, 0);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x41, 0x00, 0xFE, 0x7F, 0xA8, 0x13}))
        << "the car's own bitmap";
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 0);
    ASSERT_EQ(r.size(), 1u);
    const Bytes first = reply_bytes(r[0]);
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 600 * kSecond);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NE(reply_bytes(r[0]), first) << "its last replies hold";
}

// Every value the fakes answer for a Service 01 PID decodes inside its
// def's range (ecuio_def_range): what a gauge's default scale starts from
TEST(Obd2SetTest, EveryValueTheFakesAnswerIsInsideItsDefsRange)
{
    fake_ecu::reset_diagnostics();
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    auto frame = std::make_unique<ecuio_frame_t>();
    uint32_t checked = 0;
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        const unsigned char req[2] = {0x01, static_cast<unsigned char>(pid)};
        unsigned char out[fake_ecu::kReplyMax];
        size_t n = 0;
        if (!fake_ecu::answer(req, 2, -1, out, &n) || out[0] != 0x41) {
            continue;
        }
        *frame = {};
        frame->arbitration_id = 0x7E8u;
        frame->data_length = static_cast<uint16_t>(n);
        std::memcpy(frame->data, out, n);
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (!ecuio_def_applies_to_frame(&d, frame.get())) {
                continue;
            }
            double lo = 0.0;
            double hi = 0.0;
            if (ecuio_def_range(&d, &lo, &hi) != ECUIO_DECODE_OK) {
                continue;
            }
            double v = 0.0;
            ASSERT_EQ(
                ecuio_decode_signal(&d, frame->data, frame->data_length, &v),
                ECUIO_DECODE_OK)
                << d.name;
            EXPECT_GE(v, lo) << d.name;
            EXPECT_LE(v, hi) << d.name;
            ++checked;
        }
    }
    EXPECT_GT(checked, 100u) << "most of the set's numbers were answered";
    fake_ecu::reset_diagnostics();
}

namespace {

/* Starts channel 0's clock over, for a helper that counts time from 0
 * (rotation_of, packing_turns) run again on an engine that has run:
 * nothing is out, so only the floor stands in the way */
void clock_over(ecuio_request_engine* e)
{
    ASSERT_EQ(e->chans[0].outstanding, ECUIO_ENGINE_NONE);
    e->chans[0].next_send_us = 0;
    e->chans[0].deadline_us = 0;
}

} // namespace

/* ADR-0056 section 2: a focus parks every request outside it and runs
 * the picks in the fast class, so a slow PID picked is asked each turn,
 * the two picks taking turns in table order; a count of 0 unparks them
 * all and gives each its class back */
TEST(RequestEngineTest, AFocusParksTheRestAndRunsThePicksFast)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ASSERT_TRUE(
        ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1, nullptr));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    uint32_t slow_row = UINT32_MAX;
    uint32_t fast_row = UINT32_MAX;
    for (uint32_t i = 0; i < e.count; ++i) {
        const uint8_t pid = e.requests[i].bytes[1];
        if (pid == 0x0C || pid == 0x05) {
            keep[i] = 1;
        }
        if (pid == 0x05) {
            slow_row = i;
        }
        if (pid == 0x0C) {
            fast_row = i;
        }
    }
    ASSERT_NE(slow_row, UINT32_MAX);
    ASSERT_NE(fast_row, UINT32_MAX);
    ASSERT_EQ(e.requests[slow_row].slow, 1u) << "05 is slow by class";
    const uint32_t before = e.chans[0].requests;
    const uint32_t generation = e.chans[0].generation;

    ecuio_engine_focus(&e, keep);
    EXPECT_TRUE(e.focused);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_GT(e.chans[0].generation, generation);
    EXPECT_EQ(e.requests[slow_row].slow, 0u) << "a pick runs fast";
    EXPECT_EQ(e.requests[slow_row].slow_default, 1u);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].parked != 0, keep[i] == 0) << "row " << i;
    }
    const uint8_t first = slow_row < fast_row ? 0x05 : 0x0C;
    const uint8_t second = first == 0x05 ? 0x0C : 0x05;
    EXPECT_EQ(
        rotation_of(&e, 6),
        (std::vector<uint8_t>{first, second, first, second, first, second}));

    ecuio_engine_unfocus(&e);
    ASSERT_NO_FATAL_FAILURE(clock_over(&e));
    EXPECT_FALSE(e.focused);
    EXPECT_EQ(e.chans[0].requests, before);
    EXPECT_EQ(e.requests[slow_row].slow, 1u) << "its class is back";
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].parked, 0u) << "row " << i;
        EXPECT_EQ(e.requests[i].slow, e.requests[i].slow_default);
    }
    /* Two fast to one slow again: three slow turns in nine, whatever the
     * phase the focused turns left the counter in */
    const std::vector<uint8_t> sent = rotation_of(&e, 9);
    ASSERT_EQ(sent.size(), 9u);
    size_t slow_turns = 0;
    for (uint8_t pid : sent) {
        slow_turns += ecuio_obd2_pid_fast(pid) ? 0 : 1;
    }
    EXPECT_EQ(slow_turns, 3u);
}

/* A focus that leaves a channel nothing to ask: the rotation sends
 * nothing and the engine has no next event, and the focus's end brings
 * the rotation back */
TEST(RequestEngineTest, AFocusWithNoPickOnAChannelLeavesItNothingToAsk)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    const uint8_t none[ECUIO_PROFILE_MAX_REQUESTS] = {};
    ecuio_engine_focus(&e, none);
    EXPECT_EQ(e.chans[0].requests, 0u);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), INT64_MAX);
    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(send_next(&e, 0, 0), 0x0Bu);
    EXPECT_LT(ecuio_engine_next_event_us(&e, 0), INT64_MAX);
}

/* The probe's walk runs on under a focus and what it drops stays
 * dropped after the focus ends; a request both parked and dropped is
 * counted out of the rotation once, and a rescan unparks nothing.
 * Bitmap 00's bits: PID p is bit 32 - p, so 0x00180000 lists 0x0C and
 * 0x0D, leaves 0x0B clear, and ends the chain. */
TEST(RequestEngineTest, TheWalkDropsUnderAFocusAndADropOutlivesIt)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C, 0x0D}));
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[1] = 1; /* 0x0C */
    ecuio_engine_focus(&e, keep);
    ecuio_engine_probe(&e, 0);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00180000u, 4 * kMs),
              ecuio_reply::positive);
    EXPECT_FALSE(e.chans[0].walking);
    EXPECT_NE(e.requests[0].dropped, 0u) << "0x0B is unsupported";
    EXPECT_EQ(e.requests[0].parked, 1u);
    EXPECT_EQ(e.chans[0].requests, 1u) << "0x0C alone; 0x0B counted out once";
    EXPECT_EQ(send_next(&e, 0, 4 * kMs + ECUIO_ENGINE_GAP_US), 0x0Cu);
    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.requests[0].parked, 1u) << "a rescan unparks nothing";
    EXPECT_EQ(e.requests[2].parked, 1u);
    ecuio_engine_unfocus(&e);
    EXPECT_NE(e.requests[0].dropped, 0u) << "the drop outlives the focus";
    EXPECT_EQ(e.requests[0].parked, 0u);
    EXPECT_EQ(e.chans[0].requests, 2u) << "0x0C and 0x0D";
}

/* A pack gathers kept requests alone: under a focus on three fast PIDs
 * every request out names picks alone, and the three pack together,
 * the parked rows between them passed over (0x11, a fast row between
 * 0x0D and 0x10 in the table, is parked) */
TEST(RequestEngineTest, APackGathersKeptRequestsAlone)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_from(fake_reply));
    const std::vector<uint8_t> picks = {0x0C, 0x0D, 0x10};
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    for (uint32_t i = 0; i < e.count; ++i) {
        if (std::find(picks.begin(), picks.end(), e.requests[i].bytes[1]) !=
            picks.end()) {
            keep[i] = 1;
        }
    }
    ecuio_engine_focus(&e, keep);
    ASSERT_NO_FATAL_FAILURE(clock_over(&e));
    const std::vector<Bytes> sent =
        packing_turns(&e, 40, packed_from(fake_reply));
    ASSERT_FALSE(sent.empty());
    bool all_three = false;
    for (const Bytes& req : sent) {
        ASSERT_GE(req.size(), 2u);
        EXPECT_EQ(req[0], 0x01u);
        for (size_t k = 1; k < req.size(); ++k) {
            EXPECT_NE(std::find(picks.begin(), picks.end(), req[k]),
                      picks.end())
                << "PID " << int(req[k]) << " isn't a pick";
        }
        all_three = all_three || req.size() == 1 + picks.size();
    }
    EXPECT_TRUE(all_three) << "the three picks pack as one request";
}

namespace {

/* One request out on channel 0: when, and what it asked */
struct Asked {
    int64_t at;
    Bytes request;
};

/* Channel 0's rotation up to until_us on the engine's own clock, each
 * request answered 4 ms later through respond, the clock stepped by
 * ecuio_engine_next_event_us alone: a wait it doesn't report fails here,
 * where a worker would spin */
std::vector<Asked> paced_run(ecuio_request_engine* e, int64_t until_us,
                             const Responder& respond)
{
    std::vector<Asked> out;
    int64_t now = 0;
    while (now < until_us) {
        (void)ecuio_engine_expire(e, 0, now);
        const int32_t r = ecuio_engine_due(e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            const int64_t next = ecuio_engine_next_event_us(e, now);
            if (next <= 0 || next == INT64_MAX) {
                ADD_FAILURE() << "nothing due, and next event " << next
                              << " us, at " << now;
                break;
            }
            now += next;
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        const Bytes req(q.bytes, q.bytes + q.len);
        out.push_back({now, req});
        ecuio_engine_sent(e, 0, r, true, now);
        now += 4 * kMs;
        const Bytes reply = respond(req);
        if (!reply.empty()) {
            (void)ecuio_engine_on_frame(e, 0, reply.data(),
                                        static_cast<uint16_t>(reply.size()),
                                        now, nullptr);
        }
    }
    return out;
}

Bytes single_pid_reply(const Bytes& req)
{
    return Bytes{0x41, req[1], 0x12, 0x34};
}

/* Keeps every row of a table one_channel_of made */
void focus_all(ecuio_request_engine* e, int64_t min_sweep_us)
{
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    for (uint32_t i = 0; i < e->count; ++i) {
        keep[i] = 1;
    }
    ecuio_engine_focus(e, keep, min_sweep_us);
}

} // namespace

/* ADR-0056 section 4: a sweep starts no sooner than the floor after the
 * last one started, the requests inside it at the engine's own 20 ms
 * floor; the picks keep their order, and the wait is reported to the
 * driver (paced_run steps by it) */
TEST(RequestEngineTest, AMinimumTimeSpacesTheSweepsFromStartToStart)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    focus_all(&e, 200 * kMs);
    const std::vector<Asked> sent = paced_run(&e, 1000 * kMs, single_pid_reply);
    ASSERT_EQ(sent.size(), 15u) << "five sweeps of three in 1 s";
    const uint8_t order[3] = {0x0C, 0x0D, 0x0F};
    for (size_t k = 0; k < sent.size(); ++k) {
        EXPECT_EQ(sent[k].request[1], order[k % 3]) << "request " << k;
        EXPECT_EQ(sent[k].at, int64_t(k / 3) * 200 * kMs +
                                  int64_t(k % 3) * ECUIO_ENGINE_GAP_US)
            << "request " << k;
    }
}

/* A sweep that takes longer than the floor waits for nothing: one
 * after another at the engine's own pace, as with no floor at all */
TEST(RequestEngineTest, ASweepLongerThanItsMinimumIsNotHeldBack)
{
    ecuio_request_engine floored;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&floored, {0x0C, 0x0D, 0x0F, 0x10}));
    focus_all(&floored, 50 * kMs); /* a sweep takes 80 ms */
    ecuio_request_engine plain;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&plain, {0x0C, 0x0D, 0x0F, 0x10}));
    focus_all(&plain, 0);
    const std::vector<Asked> a =
        paced_run(&floored, 500 * kMs, single_pid_reply);
    const std::vector<Asked> b = paced_run(&plain, 500 * kMs, single_pid_reply);
    ASSERT_EQ(a.size(), b.size());
    for (size_t k = 0; k < a.size(); ++k) {
        EXPECT_EQ(a[k].at, b[k].at) << "request " << k;
        EXPECT_EQ(a[k].request, b[k].request) << "request " << k;
    }
}

/* Eight picks at six PIDs a pack: a sweep is two requests, whose rows
 * run past the eight, and the count starts over, so every sweep asks
 * every pick, sweeps start a floor apart and the wait falls only
 * between requests */
TEST(RequestEngineTest, AMinimumTimeCountsAPacksRowsAndEveryPickIsAsked)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_from(fake_reply));
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    std::set<uint8_t> picks;
    for (uint32_t i = 0; i < e.count && picks.size() < 8; ++i) {
        if (ecuio_engine_packable(&e, 0, i)) {
            keep[i] = 1;
            picks.insert(e.requests[i].bytes[1]);
        }
    }
    ASSERT_EQ(picks.size(), 8u);
    ecuio_engine_focus(&e, keep, 300 * kMs);
    ASSERT_NO_FATAL_FAILURE(clock_over(&e));
    const std::vector<Asked> sent =
        paced_run(&e, 3000 * kMs, packed_from(fake_reply));
    ASSERT_EQ(sent.size(), 20u) << "ten sweeps of two requests";
    for (size_t k = 0; k < sent.size(); k += 2) {
        EXPECT_EQ(sent[k].at, int64_t(k / 2) * 300 * kMs) << "sweep " << k / 2;
        EXPECT_EQ(sent[k + 1].at - sent[k].at, ECUIO_ENGINE_GAP_US);
        EXPECT_EQ(sent[k].request.size(), 7u) << "six PIDs in the pack";
        std::set<uint8_t> asked;
        for (size_t r = 0; r < 2; ++r) {
            asked.insert(sent[k + r].request.begin() + 1,
                         sent[k + r].request.end());
        }
        EXPECT_EQ(asked, picks) << "sweep " << k / 2;
    }
}

/* Only the rotation waits: a scanned DID goes out in the gap, the next
 * event says so, and the sweep after it starts on its own time */
TEST(RequestEngineTest, AOneShotGoesOutInAMinimumTimesGap)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    focus_all(&e, 1000 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    const uint8_t a[4] = {0x41, 0x0C, 0x12, 0x34};
    (void)ecuio_engine_on_frame(&e, 0, a, sizeof a, 4 * kMs, nullptr);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 20 * kMs), 1);
    ecuio_engine_sent(&e, 0, 1, true, 20 * kMs);
    const uint8_t b[3] = {0x41, 0x0D, 0x20};
    (void)ecuio_engine_on_frame(&e, 0, b, sizeof b, 24 * kMs, nullptr);

    /* The gap, with only the rotation left to send */
    EXPECT_EQ(ecuio_engine_due(&e, 0, 40 * kMs), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 40 * kMs), 960 * kMs);

    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 40 * kMs), 0)
        << "the scanned DID is due now";
    EXPECT_EQ(ecuio_engine_due(&e, 0, 40 * kMs), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, 40 * kMs);
    const uint8_t refused[3] = {0x7F, 0x22, 0x31};
    (void)ecuio_engine_on_frame(&e, 0, refused, sizeof refused, 44 * kMs,
                                nullptr);

    EXPECT_EQ(ecuio_engine_due(&e, 0, 60 * kMs), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 60 * kMs), 940 * kMs)
        << "the rotation waits on";
    EXPECT_EQ(ecuio_engine_due(&e, 0, 999 * kMs), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 1000 * kMs), 0);
}

/* The wait lasts as long as its focus: the same time again (a scanned
 * DID joined) keeps a wait under way, another ends it, and the focus's
 * end frees the rotation and sets the minimum back to none */
TEST(RequestEngineTest, AMinimumTimeLastsAsLongAsItsFocus)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C}));
    focus_all(&e, 5000 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    (void)ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), ECUIO_ENGINE_NONE);

    focus_all(&e, 5000 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), ECUIO_ENGINE_NONE)
        << "the same time keeps the wait";

    focus_all(&e, 100 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), 0)
        << "a new time ends it";
    ecuio_engine_sent(&e, 0, 0, true, ECUIO_ENGINE_WAIT_US);
    (void)ecuio_engine_expire(&e, 0, 2 * ECUIO_ENGINE_WAIT_US);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 2 * ECUIO_ENGINE_WAIT_US - 20 * kMs),
              ECUIO_ENGINE_NONE)
        << "the new time, from that sweep's start";

    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.min_sweep_us, 0);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 2 * ECUIO_ENGINE_WAIT_US), 0)
        << "no focus, no minimum";
}

/* A focus taken in the middle of a sweep starts a new one: every row
 * of it is asked before the next wait */
TEST(RequestEngineTest, AFocusInTheMiddleOfASweepStartsANewOne)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    focus_all(&e, 500 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    (void)ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US);
    focus_all(&e, 500 * kMs);
    int64_t now = 100 * kMs;
    for (int k = 0; k < 3; ++k) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        ASSERT_NE(r, ECUIO_ENGINE_NONE) << "row " << k << " of the new sweep";
        ecuio_engine_sent(&e, 0, r, true, now);
        now += ECUIO_ENGINE_WAIT_US;
        (void)ecuio_engine_expire(&e, 0, now);
    }
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE)
        << "its three rows asked, the wait begins";
}

/* A pick refused and dropped after it was asked still leaves the sweep
 * to ask every pick left, and the next sweep counts from the first
 * request's time */
TEST(RequestEngineTest, APickDroppedInASweepStillLeavesTheRestAsked)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    e.probe = true; /* an explicit refusal drops its request */
    focus_all(&e, 500 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    const uint8_t refused[3] = {0x7F, 0x01, 0x12};
    (void)ecuio_engine_on_frame(&e, 0, refused, sizeof refused, 4 * kMs,
                                nullptr);
    ASSERT_NE(e.requests[0].dropped, 0) << "0C dropped";

    int64_t now = 20 * kMs;
    for (const int32_t row : {1, 2}) {
        ASSERT_EQ(ecuio_engine_due(&e, 0, now), row)
            << "row " << row << " in the sweep the drop happened in";
        ecuio_engine_sent(&e, 0, row, true, now);
        const uint8_t ok[4] = {0x41, e.requests[row].bytes[1], 0x12, 0x34};
        (void)ecuio_engine_on_frame(&e, 0, ok, sizeof ok, now + 4 * kMs,
                                    nullptr);
        now += ECUIO_ENGINE_GAP_US;
    }
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 500 * kMs - now)
        << "the next sweep is a minimum after the first request";
    EXPECT_EQ(ecuio_engine_due(&e, 0, 500 * kMs), 1);
}

/* A row dropped before it was asked in this sweep isn't waited for:
 * the sweep ends once the rows left have been asked */
TEST(RequestEngineTest, ARowDroppedBeforeItsTurnShortensTheSweep)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    focus_all(&e, 500 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    (void)ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US);
    ecuio_engine_drop(&e, 2, ECUIO_DROP_BITMAP, 0);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), 1);
    ecuio_engine_sent(&e, 0, 1, true, ECUIO_ENGINE_WAIT_US);
    (void)ecuio_engine_expire(&e, 0, 2 * ECUIO_ENGINE_WAIT_US);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 2 * ECUIO_ENGINE_WAIT_US),
              ECUIO_ENGINE_NONE)
        << "0C and 0D asked, 0F gone: the wait begins";
}

/* No focus, no minimum: the rotation runs at the engine's own pace */
TEST(RequestEngineTest, ASessionWithNoFocusHasNoMinimumTime)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x05}));
    EXPECT_EQ(e.min_sweep_us, 0);
    const std::vector<Asked> sent = paced_run(&e, 400 * kMs, single_pid_reply);
    ASSERT_EQ(sent.size(), 20u) << "a request every 20 ms";
    for (size_t k = 0; k < sent.size(); ++k) {
        EXPECT_EQ(sent[k].at, int64_t(k) * ECUIO_ENGINE_GAP_US);
    }
}

/* A request out when a focus parks it is still answered: its reply
 * correlates by the table, and the next request is a pick */
TEST(RequestEngineTest, ARequestOutWhenParkedIsStillAnswered)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    EXPECT_EQ(send_next(&e, 0, 0), 0x0Bu);
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[1] = 1; /* 0x0C */
    ecuio_engine_focus(&e, keep);
    EXPECT_EQ(e.chans[0].outstanding, 0) << "0x0B stays out";
    const uint8_t reply[3] = {0x41, 0x0B, 0x64};
    EXPECT_EQ(
        ecuio_engine_on_frame(&e, 0, reply, sizeof(reply), 4 * kMs, nullptr),
        ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_NONE);
    EXPECT_EQ(send_next(&e, 0, 4 * kMs + ECUIO_ENGINE_GAP_US), 0x0Cu);
}

/* A duplicate pair judged while a focus is on (a re-walk after a
 * recovery): the demoted PID's class is remembered, not applied, so a
 * pick keeps running fast until the focus ends. PID p's bit in the
 * answered bitmaps is 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8. */
TEST(RequestEngineTest, ADuplicateJudgedUnderAFocusStaysFastUntilItEnds)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar, false));
    uint32_t demoted = UINT32_MAX;
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    for (uint32_t i = 0; i < e.count; ++i) {
        const uint8_t pid = e.requests[i].bytes[1];
        if (pid == 0x23 || pid == 0x59) {
            keep[i] = 1;
        }
        if (pid == 0x59) {
            demoted = i;
        }
    }
    ASSERT_NE(demoted, UINT32_MAX);
    ecuio_engine_focus(&e, keep);
    for (const uint8_t pid : {uint8_t{0x23}, uint8_t{0x59}}) {
        e.chans[0].supported[(pid - 1u) / 8u] |= 0x80u >> ((pid - 1u) % 8u);
    }
    ecuio_engine_judge_duplicates(&e, 0);
    EXPECT_EQ(e.requests[demoted].slow_default, 1u) << "judged slow";
    EXPECT_EQ(e.requests[demoted].slow, 0u) << "but a pick runs fast";
    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.requests[demoted].slow, 1u) << "its class once the focus ends";
}

/* A request added under a focus (a scanned DID found) starts parked and
 * marks the focus for resolving again (focus_joined), which the next
 * focus call clears; it joins the rotation when the focus ends */
TEST(RequestEngineTest, ARequestAddedUnderAFocusStartsParked)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B}));
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[0] = 1;
    ecuio_engine_focus(&e, keep);
    EXPECT_FALSE(e.focus_joined);
    const uint8_t req[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, req, 3), ecuio_engine_add_result::added);
    EXPECT_EQ(e.requests[1].parked, 1u);
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_TRUE(e.focus_joined);
    ecuio_engine_focus(&e, keep);
    EXPECT_FALSE(e.focus_joined) << "the focus resolved again";
    EXPECT_EQ(e.requests[1].parked, 1u) << "no pick names it";
    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.requests[1].parked, 0u);
    EXPECT_EQ(e.chans[0].requests, 2u);
}

/* A focus moves only the generations of the channels whose count it
 * changed: a channel that keeps every request keeps its generation, so
 * its support report isn't published again, and a swap of picks inside
 * a channel that keeps its count moves nothing */
TEST(RequestEngineTest, AFocusMovesOnlyTheChangedChannelsGenerations)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    for (const uint8_t pid : {uint8_t{0x0B}, uint8_t{0x0C}}) {
        const uint8_t req[2] = {0x01, pid};
        ASSERT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::added);
    }
    const uint8_t other[2] = {0x01, 0x0D};
    ASSERT_EQ(ecuio_engine_add(&e, 1, other, 2),
              ecuio_engine_add_result::added);
    const uint32_t g0 = e.chans[0].generation;
    const uint32_t g1 = e.chans[1].generation;

    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[0] = 1; /* channel 0's 0x0B */
    keep[2] = 1; /* channel 1's 0x0D */
    ecuio_engine_focus(&e, keep);
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_GT(e.chans[0].generation, g0) << "channel 0 lost a request";
    EXPECT_EQ(e.chans[1].generation, g1) << "channel 1 kept its one";

    const uint32_t g0_focused = e.chans[0].generation;
    keep[0] = 0;
    keep[1] = 1; /* 0x0C in 0x0B's place */
    ecuio_engine_focus(&e, keep);
    EXPECT_EQ(e.requests[0].parked, 1u);
    EXPECT_EQ(e.requests[1].parked, 0u);
    EXPECT_EQ(e.chans[0].generation, g0_focused) << "the count stood";
    EXPECT_EQ(e.chans[1].generation, g1);

    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_GT(e.chans[0].generation, g0_focused);
    EXPECT_EQ(e.chans[1].generation, g1);
}
