// File/layer: tests/test_obd2.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT: set validation, wire-byte decode round-trips matching
//          the fake's canned responses, mux gate accept/reject, the
//          request engine (request_engine.h) with the time passed in,
//          its DID scan and its identification (the turns, the text, the
//          report), and uds-generic's ?did= grammar and the set it
//          builds (uds_generic.h). The Service 01 table
//          (fixtures/obd2/service01.csv) against its own ranges, the
//          obd2 set and the fakes' ECU; that ECU's diagnostics
//          (fake_ecu.h). The trouble codes' and monitors' words and
//          the replies into the report (diagnostics.h), the
//          monitors against fixtures/obd2/monitors.csv. End-to-end pacing is HARNESS (stream and
//          health tests) and BENCH (VX), not this file.

#include <gtest/gtest.h>

#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "../core/src/request_engine.h"
#include "../core/src/diagnostics.h"
#include "../core/src/obd2_rates.h"
#include "../core/src/uds_generic.h"
#include "../core/src/uri_query.h"
#include "ecuio/core.h"
#include "fake_diagnostics.h"
#include "fake_ecu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <string>
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
 * fields (ADR-0039 §4), clear of the mock's band and uds-generic's. */
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

/* §3: whatever reaches the table, only reads go out (ADR-0037 adds the
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

/* ---------- rate classes (ADR-0039 §3) ---------- */

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

/* ---------- the probe (ADR-0015 §4) ---------- */

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

    /* 129 DIDs to poll, one past the engine's table */
    std::string many = "did=";
    for (unsigned i = 0; i <= ECUIO_PROFILE_MAX_REQUESTS; ++i) {
        char one[8];
        snprintf(one, sizeof(one), "%s%04X", (i == 0) ? "" : ",", 0x1000 + i);
        many += one;
    }
    ecuio_uds_session u;
    EXPECT_EQ(build_uds(many, &u), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?did= lists 129 DIDs to poll, more than the 128 a session "
                 "asks for; scan a range instead");

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
 * §1, §2). */
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
    ASSERT_EQ(sent.size(), 6u);
    EXPECT_EQ(sent.back().bytes, (Bytes{0x02, 0x02, 0x00}));

    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags,
              ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS);
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
    {0x01, 0x01},       {0x03}, {0x07}, {0x0A}, {0x02, 0x00, 0x00},
    {0x02, 0x02, 0x00},
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
