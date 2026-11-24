// File/layer: tests/engine_talk_support.h | test_obd2, Qt-free
// Testing: UNIT: what EngineTalkTest (tests/test_engine_talk.cpp) and
//          KeyWindowTest (tests/test_key_window.cpp) share: an ECU with the
//          simulated ECU's security access, the engine stepped with the
//          time passed in, and each kind of offer

#ifndef ECUIO_TESTS_ENGINE_TALK_SUPPORT_H
#define ECUIO_TESTS_ENGINE_TALK_SUPPORT_H

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"
#include "../core/src/request_talk.h"
#include "../core/src/sim_security.h"
#include "../core/src/tier2_ceiling.h"
#include "ecuio/signal_sets.h"

#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace engine_talk_test {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;
/* A seed's window_us where a case doesn't care: a manifest's default */
constexpr int64_t kWindow = 1000 * kMs;

constexpr uint16_t kDid = 0x1E1C; /* in the rotation, answered always */
inline const Bytes kRow = {0x22, 0x1E, 0x1C};
inline const Bytes kEnter = {0x10, 0x03};
inline const Bytes kKeep = {0x3E, 0x00};
inline const Bytes kGated = {0x22, 0xD2, 0x10};
inline const Bytes kSpeed = {0x01, 0x0C}; /* a clear's first step */

/* An ECU with the simulated ECU's security access (sim_security.h): an
 * extended session, 27 in it, D210 once unlocked, the rotation's DIDs
 * (1Exx) and any DID from D300 on always, a stopped engine's speed and a
 * clear; silent to whatever `silent` names. With security.strict, any
 * request but a 27 between a seed and its key spends the seed. */
struct Ecu {
    ecuio_sim_security security;
    bool extended = false;
    const int64_t* now = nullptr;
    Bytes silent;
    std::vector<Bytes> heard;

    Bytes operator()(const Bytes& q)
    {
        heard.push_back(q);
        if (q == silent) {
            return {};
        }
        uint8_t out[32] = {};
        if (!q.empty() && q[0] == 0x27) {
            const uint32_t n = ecuio_sim_security_answer(
                security, extended, ECUIO_SIM_SECURITY_DELAY_US, *now, q.data(),
                static_cast<uint32_t>(q.size()), out);
            return Bytes(out, out + n);
        }
        ecuio_sim_security_between(security);
        if (q.size() == 2 && q[0] == 0x10 && (q[1] == 0x01 || q[1] == 0x03)) {
            extended = q[1] == 0x03;
            ecuio_sim_security_lock(security);
            return {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == kKeep) {
            return {0x7E, 0x00};
        }
        if (q == kSpeed) {
            return {0x41, 0x0C, 0x00, 0x00};
        }
        if (q == Bytes{0x04}) {
            return {0x44};
        }
        if (q == Bytes{0x03}) {
            return {0x43, 0x00}; /* no codes: an emissions module */
        }
        if (q == kGated) {
            const uint32_t n = ecuio_sim_security_did(security, extended, out);
            return n != 0 ? Bytes(out, out + n) : Bytes{0x7F, 0x22, 0x31};
        }
        if (q.size() == 3 && q[0] == 0x22 && (q[1] == 0x1E || q[1] >= 0xD3)) {
            return {0x62, q[1], q[2], 0x12, 0x34};
        }
        return {0x7F, q.empty() ? uint8_t{0} : q[0], 0x11};
    }
};

using Responder = std::function<Bytes(const Bytes&)>;

/* What the transport would put on the wire for request r */
inline Bytes wire(const ecuio_request_engine* e, uint32_t c, int32_t r)
{
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    uint32_t len = 0;
    const uint8_t* pdu = ecuio_engine_pdu_of(
        e->chans[c].talk, r == ECUIO_ENGINE_ONCE, q.bytes, q.len, &len);
    return Bytes(pdu, pdu + len);
}

/* Sends channel c's next request at *now and answers it through respond
 * (silence waits the wait out); the bytes sent, or none when nothing was
 * due (the time then moves on a floor's gap) */
inline Bytes step(ecuio_request_engine* e, uint32_t c, int64_t* now,
                  const Responder& respond)
{
    const int32_t r = ecuio_engine_due(e, c, *now);
    if (r == ECUIO_ENGINE_NONE) {
        *now += ECUIO_ENGINE_GAP_US;
        return {};
    }
    const Bytes sent = wire(e, c, r);
    ecuio_engine_sent(e, c, r, true, *now);
    const Bytes reply = respond(sent);
    if (reply.empty()) {
        *now += ECUIO_ENGINE_WAIT_US;
        EXPECT_TRUE(ecuio_engine_expire(e, c, *now));
    } else {
        *now += 4 * kMs;
        (void)ecuio_engine_on_frame(e, c, reply.data(),
                                    static_cast<uint16_t>(reply.size()), *now,
                                    nullptr);
    }
    *now += ECUIO_ENGINE_GAP_US;
    return sent;
}

/* Steps until channel 0's talk has settled, at most n; what went out */
inline std::vector<Bytes> until_settled(ecuio_request_engine* e, int64_t* now,
                                        const Responder& respond, int n = 12)
{
    std::vector<Bytes> out;
    const ecuio_engine_talk_state& t = e->chans[0].talk;
    for (int i = 0; i < n && t.result.end == ecuio_talk_end::none; ++i) {
        Bytes b = step(e, 0, now, respond);
        if (!b.empty()) {
            out.push_back(b);
        }
    }
    return out;
}

/* n steps of channel 0; what went out */
inline std::vector<Bytes> steps(ecuio_request_engine* e, int64_t* now,
                                const Responder& respond, int n)
{
    std::vector<Bytes> out;
    for (int i = 0; i < n; ++i) {
        Bytes b = step(e, 0, now, respond);
        if (!b.empty()) {
            out.push_back(b);
        }
    }
    return out;
}

/* Channel 0's next one-shot at *now, chosen and not yet sent; the
 * rotation's requests due before it are sent and answered */
inline int32_t next_once(ecuio_request_engine* e, int64_t* now, Ecu* ecu)
{
    for (int i = 0; i < 8; ++i) {
        const int32_t r = ecuio_engine_due(e, 0, *now);
        if (r == ECUIO_ENGINE_ONCE) {
            return r;
        }
        if (r != ECUIO_ENGINE_NONE) {
            ecuio_engine_sent(e, 0, r, true, *now);
            const Bytes reply = (*ecu)(wire(e, 0, r));
            (void)ecuio_engine_on_frame(e, 0, reply.data(),
                                        static_cast<uint16_t>(reply.size()),
                                        *now, nullptr);
        }
        *now += ECUIO_ENGINE_GAP_US;
    }
    return ECUIO_ENGINE_NONE;
}

inline ecuio_talk_request request(const Bytes& pdu, uint8_t echo,
                                  ecuio_talk_kind kind, int64_t window_us)
{
    ecuio_talk_request q;
    q.pdu = pdu.data();
    q.len = static_cast<uint32_t>(pdu.size());
    q.echo = echo;
    q.kind = kind;
    q.window_us = window_us;
    return q;
}

/* A plain talk offered to channel c: the door reads no time for one */
inline ecuio_talk_offer offer(ecuio_request_engine* e, const Bytes& pdu,
                              uint8_t echo, uint32_t c = 0)
{
    return ecuio_engine_offer_talk(
        e, c, request(pdu, echo, ecuio_talk_kind::plain, 0), 0);
}

/* A level's seed request offered at now_us */
inline ecuio_talk_offer offer_seed(ecuio_request_engine* e, int64_t now_us,
                                   uint8_t level = 0x03,
                                   int64_t window_us = kWindow, uint32_t c = 0)
{
    const Bytes pdu = {0x27, level};
    return ecuio_engine_offer_talk(
        e, c, request(pdu, 2, ecuio_talk_kind::seed, window_us), now_us);
}

/* A key offered at now_us */
inline ecuio_talk_offer offer_key(ecuio_request_engine* e, int64_t now_us,
                                  const Bytes& pdu, uint32_t c = 0)
{
    return ecuio_engine_offer_talk(
        e, c, request(pdu, 2, ecuio_talk_kind::key, 0), now_us);
}

/* The door itself, with a state and a window of the case's own */
inline ecuio_talk_offer door(ecuio_engine_talk_state& t, ecuio_key_window& w,
                             const Bytes& pdu, uint8_t echo,
                             ecuio_talk_kind kind = ecuio_talk_kind::plain,
                             int64_t window_us = 0, int64_t now_us = 0,
                             uint32_t pdu_max = ECUIO_ENGINE_PDU_MAX)
{
    return ecuio_engine_talk(t, w, pdu_max, request(pdu, echo, kind, window_us),
                             now_us);
}

/* The settle, taken: its end, and its code into *nrc when given */
inline ecuio_talk_end taken(ecuio_request_engine* e, uint8_t* nrc = nullptr)
{
    ecuio_talk_result r;
    if (!ecuio_engine_talk_take(e->chans[0].talk, &r)) {
        return ecuio_talk_end::none;
    }
    if (nrc != nullptr) {
        *nrc = r.nrc;
    }
    return r.end;
}

inline size_t count_of(const std::vector<Bytes>& v, const Bytes& want)
{
    size_t n = 0;
    for (const Bytes& b : v) {
        n += b == want ? 1 : 0;
    }
    return n;
}

/* Where want first stands in v, -1 for nowhere */
inline int index_of(const std::vector<Bytes>& v, const Bytes& want)
{
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == want) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/* One channel with kDid in its rotation, on a transport that sends
 * ISO-TP whole; rows 0: nothing in the rotation */
inline void engine(ecuio_request_engine* e, int rows = 1)
{
    ASSERT_TRUE(ecuio_engine_init(e, 1));
    e->pdu_max = ECUIO_ENGINE_PDU_MAX;
    for (int i = 0; i < rows; ++i) {
        const uint8_t did[3] = {0x22, kDid >> 8,
                                static_cast<uint8_t>((kDid & 0xFF) + i)};
        ASSERT_EQ(ecuio_engine_add(e, 0, did, 3),
                  ecuio_engine_add_result::added);
    }
}

/* The engine above, in the extended session and past its first reads,
 * against a strict ECU */
inline void entered(ecuio_request_engine* e, Ecu* ecu, int64_t* now,
                    int rows = 1)
{
    engine(e, rows);
    ecu->now = now;
    ecu->security.strict = true;
    ASSERT_TRUE(ecuio_engine_session(e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    for (int i = 0; i < 4; ++i) {
        (void)step(e, 0, now, std::ref(*ecu));
    }
    ASSERT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_FALSE(e->chans[0].session_fresh);
    ecu->heard.clear();
}

/* A level's seed offered, sent and answered: channel 0's window is open,
 * and the time is one floor past the seed's reply */
inline void seeded(ecuio_request_engine* e, Ecu* ecu, int64_t* now,
                   uint8_t level = 0x03, int64_t window_us = kWindow)
{
    ASSERT_EQ(offer_seed(e, *now, level, window_us), ecuio_talk_offer::taken);
    (void)until_settled(e, now, std::ref(*ecu));
    ASSERT_EQ(taken(e), ecuio_talk_end::answered);
    ASSERT_EQ(ecu->security.seed_out, level);
    ASSERT_TRUE(ecuio_key_window_open(e->chans[0].key, *now));
    ecu->heard.clear();
}

inline Bytes key_for(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
    req.resize(2u + l->bytes);
    ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
    return req;
}

/* A request of 18 bytes that is no key: longer than its echo */
inline Bytes long_read()
{
    Bytes pdu = {0x2C, 0x01};
    for (uint8_t k = 0; k < 16; ++k) {
        pdu.push_back(static_cast<uint8_t>(0xA0 + k));
    }
    return pdu;
}

} // namespace engine_talk_test

#endif /* ECUIO_TESTS_ENGINE_TALK_SUPPORT_H */
