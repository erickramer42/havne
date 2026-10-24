// File/layer: tests/test_obd2.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine — no
//             transport)
// Testing: UNIT — set validation, wire-byte decode round-trips matching
//          the fake's canned responses, mux gate accept/reject, the
//          request engine (request_engine.h) with the time passed in,
//          its DID scan and its identification (the turns, the text, the
//          report), and uds-generic's ?did= grammar and the set it
//          builds (uds_generic.h). End-to-end pacing is HARNESS (stream
//          and health tests) and BENCH (VX) — not this file.

#include <gtest/gtest.h>

#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "../core/src/request_engine.h"
#include "../core/src/uds_generic.h"
#include "../core/src/uri_query.h"
#include "ecuio/core.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace {

/* Wire payload as the bridge hands it to the decoder (header stripped):
 * [0x41][PID][data...] — header stripped; the adapter owns ISO-TP
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
    EXPECT_EQ(set->count, 10u);
}

TEST(Obd2SetTest, PidBandDistinctFromMockIds) {
    /* Guards the 7000-band choice against accidental reuse if the mock
     * set ever grows. Cheap pin, drift-proof. */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const uint16_t id = obd2->defs[i].id;
        EXPECT_GE(id, 7001u);
        EXPECT_LE(id, 7010u);
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
    /* 0x0064 = 100 raw * 0.01 = 1.00 g/s — pins the /100 (centi-grams/s)
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
     * RPM if the mux gate were absent — the Blocker-1 scenario. */
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
    const uint8_t expect[] = {0x04, 0x05, 0x0C, 0x0D,
                              0x0F, 0x11, 0x2F, 0x42, 0x10, 0x5C};
    ASSERT_EQ(e.count, sizeof(expect));
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].len, 2u) << "index " << i;
        EXPECT_EQ(e.requests[i].bytes[0], 0x01u) << "index " << i;
        EXPECT_EQ(e.requests[i].bytes[1], expect[i]) << "index " << i;
        EXPECT_EQ(e.requests[i].channel, 0u) << "index " << i;
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

/* §3: whatever reaches the table, only reads go out. Service 0x04
 * clears the emission DTCs; 0x10, 0x27, 0x2E, 0x31 and 0x11 change a
 * session, unlock, write, run a routine or reset. */
TEST(RequestEngineTest, OnlyReadServicesEnterTheTable) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t kReads[] = {0x01, 0x09, 0x22};
    for (uint8_t sid : kReads) {
        const uint8_t req[3] = {sid, 0x02, 0x00};
        EXPECT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::added) << std::hex << int{sid};
    }
    const uint8_t kOthers[] = {0x02, 0x03, 0x04, 0x07, 0x0A, 0x10, 0x11,
                               0x14, 0x19, 0x27, 0x28, 0x2E, 0x2F, 0x31,
                               0x34, 0x3B, 0x3E, 0x85};
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
    EXPECT_EQ(e.count, 3u);
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
 * read off the bytes by J1979's layout there: of the obd2 set, every PID
 * is supported but 0x5C, so the walk drops exactly that one. The 0x40
 * bitmap's last bit says a fourth exists; it's left unanswered here, so
 * PIDs past 0x60 would stay. */
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

    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].dropped,
                  (e.requests[i].bytes[1] == 0x5C) ? ECUIO_DROP_BITMAP : 0u)
            << "PID " << std::hex << int{e.requests[i].bytes[1]};
    }
    EXPECT_EQ(e.chans[0].requests, e.count - 1);
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
