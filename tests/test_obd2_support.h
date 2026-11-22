// File/layer: tests/test_obd2_support.h | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#pragma once

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
#include "fake_ecus.h"
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
inline ecuio_frame_t make_obd2_frame(const uint8_t* payload, uint8_t len)
{
    ecuio_frame_t f{};
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    memcpy(f.data, payload, len);
    f.data_length = len;
    return f;
}

/* Decode a frame's payload, as the bridge does. */
inline int decode(const ecuio_signal_def_t* d, const ecuio_frame_t& f, double* v)
{
    return ecuio_decode_signal(d, f.data, f.data_length, v);
}

inline const ecuio_signal_def_t* find_def(const ecuio_signal_set_t* set, uint16_t id)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id) return &set->defs[i];
    }
    return nullptr;
}

inline ecuio_signal_set_t set_of(const ecuio_signal_def_t* defs, uint32_t n)
{
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = n;
    s.defs = defs;
    return s;
}

/* One channel holding the given Service 01 PIDs, in order. */
inline void one_channel_of(ecuio_request_engine* e,
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

/* ---------- rate classes (ADR-0039 section 3) ---------- */

namespace {

/* The PIDs channel 0 sends over n turns, each request waiting out its
 * wait (no reply needed to move the rotation on). */
inline std::vector<uint8_t> rotation_of(ecuio_request_engine* e, int n)
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

/* ---------- the probe (ADR-0015 section 4) ---------- */

namespace {

/* Sends what channel c has due at now, which must be the walk's request
 * for the bitmap at pid. */
inline void send_bitmap_request(ecuio_request_engine* e, uint32_t c, uint8_t pid,
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
inline ecuio_reply answer_bitmap(ecuio_request_engine* e, uint32_t c, uint8_t pid,
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
inline uint8_t send_next(ecuio_request_engine* e, uint32_t c, int64_t now)
{
    const int32_t r = ecuio_engine_due(e, c, now);
    if (r < 0) {
        return 0xEE;
    }
    ecuio_engine_sent(e, c, r, true, now);
    return e->requests[r].bytes[1];
}

inline ecuio_reply answer_negative(ecuio_request_engine* e, uint32_t c,
                            uint8_t service, uint8_t nrc, int64_t now)
{
    const uint8_t reply[3] = {0x7F, service, nrc};
    return ecuio_engine_on_frame(e, c, reply, sizeof(reply), now, nullptr);
}

/* Channel 0 of the obd2 set as the setup builds it (the classes and the
 * duplicate pairs), walked against bitmaps answered in turn; the last
 * one's clear last bit ends the walk, unless walk is false. */
inline void obd2_walked(ecuio_request_engine* e,
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
    ecuio_engine_probe(e);
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
inline int class_of(const ecuio_request_engine& e, uint8_t pid)
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
 * the same in docs/bench-sessions/2026-10-31-elm327-vehicle.md) */
const std::vector<std::pair<uint8_t, uint32_t>> kTestCar = {
    {0x00, 0xFE7FA813u}, {0x20, 0xA017F011u}, {0x40, 0xFED08C81u},
    {0x60, 0x63080001u}, {0x80, 0x0004000Du}, {0xA0, 0x04000000u}};

} // namespace

/* ---------- the DID scan (ADR-0020) ---------- */

namespace {

/* Sends channel c's one-shot, which must be 22 <did>, at now. */
inline void send_scan_request(ecuio_request_engine* e, uint32_t c, uint16_t did,
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
inline void send_rotation(ecuio_request_engine* e, uint32_t c, int32_t i, int64_t now)
{
    ASSERT_EQ(ecuio_engine_due(e, c, now), i);
    ecuio_engine_sent(e, c, i, true, now);
}

} // namespace

/* ---------- the fakes' ECU: diagnostics (fake_ecu.h) ---------- */

namespace {

/* The ECU's answer to req, or {} when it stays silent */
inline std::vector<uint8_t> ecu(std::initializer_list<uint8_t> req)
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
inline std::string dtc(uint8_t a, uint8_t b)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string s(1, "PCBU"[a >> 6]);
    s += kHex[(a >> 4) & 0x3];
    s += kHex[a & 0xF];
    s += kHex[b >> 4];
    s += kHex[b & 0xF];
    return s;
}

using Bytes = std::vector<uint8_t>;

class DiagnosticsTest : public ::testing::Test
{
protected:
    void SetUp() override { fake_ecu::reset_diagnostics(); }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

/* One request the engine sent, and whether it went through the
 * one-shot */
struct SentRequest {
    Bytes bytes;
    bool once = false;
};

/* An answer for a request: its reply, or {} for silence */
using Responder = std::function<Bytes(const Bytes&)>;

inline Bytes fake_reply(const Bytes& req)
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
inline std::vector<SentRequest> drive(ecuio_request_engine* e, int64_t* now,
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

} // namespace

/* ---------- several PIDs per request (ADR-0039 Appendix A) ---------- */

namespace {

/* single's answers, a pack answered in J1979's form: each PID's record
 * from its own reply, in the order asked, a PID it doesn't answer left
 * out */
inline Responder packed_from(Responder single)
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

/* n turns of channel 0's rotation after a walk of the test car's
 * bitmaps with packing on, each answered through respond (silence waits
 * out the wait); what went out, in order */
inline std::vector<Bytes> packing_turns(ecuio_request_engine* e, int n,
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
inline void packing_engine(ecuio_request_engine* e)
{
    ASSERT_NO_FATAL_FAILURE(obd2_walked(e, kTestCar));
    e->pack = true;
}

inline std::vector<uint8_t> ask(ecuio_sim_ecu& e, std::initializer_list<uint8_t> req,
                         bool functional = false)
{
    const std::vector<uint8_t> q(req);
    uint8_t out[ECUIO_SIM_REPLY_MAX];
    const uint32_t n = ecuio_sim_answer(
        e, q.data(), static_cast<uint32_t>(q.size()), functional, out);
    return std::vector<uint8_t>(out, out + n);
}

} // namespace

/* ---------- DIDs from a signal set (ADR-0046's appendix) ---------- */

namespace {

/* A def keyed 62 <did> at offset 0 on rx */
inline ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did, uint8_t kind,
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

inline ecuio_signal_set_t
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

} // namespace

/* ---------- a recording as the simulated ECU (ADR-0046 section 7) ---------- */

namespace {

constexpr int64_t kSecond = 1000000;

using Bytes = std::vector<uint8_t>;

} // namespace
