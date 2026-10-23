// File/layer: tests/test_obd2.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine — no
//             transport)
// Testing: UNIT — set validation, wire-byte decode round-trips matching
//          the fake's canned responses, mux gate accept/reject, and the
//          request engine (request_engine.h) with the time passed in.
//          End-to-end pacing is HARNESS (stream and health tests) and
//          BENCH (VX) — not this file.

#include <gtest/gtest.h>

#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "../core/src/request_engine.h"
#include "ecuio/core.h"

#include <cstring>
#include <initializer_list>

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
