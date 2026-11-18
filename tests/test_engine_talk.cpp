// File/layer: tests/test_engine_talk.cpp | test_obd2, Qt-free
// Testing: UNIT: EngineTalkTest (a talk through the request engine's
//          one-shot, ADR-0066 section 2.4, with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"
#include "../core/src/request_talk.h"
#include "../core/src/sim_security.h"
#include "../core/src/tier2_ceiling.h"

#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

constexpr uint16_t kDid = 0x1E1C; /* in the rotation, answered always */
const Bytes kRow = {0x22, 0x1E, 0x1C};
const Bytes kEnter = {0x10, 0x03};
const Bytes kKeep = {0x3E, 0x00};
const Bytes kGated = {0x22, 0xD2, 0x10};

/* An ECU with the simulated ECU's security access (sim_security.h): an
 * extended session, 27 in it, D210 once unlocked, the rotation's DIDs
 * (1Exx) and any DID from D300 on always; silent to whatever `silent`
 * names */
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
        if (q.size() == 2 && q[0] == 0x10 && (q[1] == 0x01 || q[1] == 0x03)) {
            extended = q[1] == 0x03;
            ecuio_sim_security_lock(security);
            return {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == kKeep) {
            return {0x7E, 0x00};
        }
        uint8_t out[32] = {};
        if (!q.empty() && q[0] == 0x27) {
            const uint32_t n = ecuio_sim_security_answer(
                security, extended, ECUIO_SIM_SECURITY_DELAY_US, *now, q.data(),
                static_cast<uint32_t>(q.size()), out);
            return Bytes(out, out + n);
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
Bytes wire(const ecuio_request_engine* e, uint32_t c, int32_t r)
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
Bytes step(ecuio_request_engine* e, uint32_t c, int64_t* now,
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
std::vector<Bytes> until_settled(ecuio_request_engine* e, int64_t* now,
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

/* Channel 0's next one-shot at *now, chosen and not yet sent; the
 * rotation's requests due before it are sent and answered */
int32_t next_once(ecuio_request_engine* e, int64_t* now, Ecu* ecu)
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

ecuio_talk_offer offer(ecuio_request_engine* e, const Bytes& pdu, uint8_t echo,
                       bool first = false, uint32_t c = 0)
{
    return ecuio_engine_talk(e->chans[c].talk, e->pdu_max, pdu.data(),
                             static_cast<uint32_t>(pdu.size()), echo, first);
}

/* The settle, taken: its end, and its code into *nrc when given */
ecuio_talk_end taken(ecuio_request_engine* e, uint8_t* nrc = nullptr)
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

size_t count_of(const std::vector<Bytes>& v, const Bytes& want)
{
    size_t n = 0;
    for (const Bytes& b : v) {
        n += b == want ? 1 : 0;
    }
    return n;
}

/* One channel with kDid in its rotation, on a transport that sends
 * ISO-TP whole; rows 0: nothing in the rotation */
void engine(ecuio_request_engine* e, int rows = 1)
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

/* The engine above, in the extended session and past its first reads */
void entered(ecuio_request_engine* e, Ecu* ecu, int64_t* now, int rows = 1)
{
    engine(e, rows);
    ecu->now = now;
    ASSERT_TRUE(ecuio_engine_session(e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    for (int i = 0; i < 4; ++i) {
        (void)step(e, 0, now, std::ref(*ecu));
    }
    ASSERT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_FALSE(e->chans[0].session_fresh);
    ecu->heard.clear();
}

Bytes key_for(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
    req.resize(2u + l->bytes);
    ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
    return req;
}

} // namespace

TEST(EngineTalkTest, TheDoorIsTheCeilingForEveryServiceAndSubFunction)
{
    /* Whatever the caller checked: an offer is taken exactly when tier
     * 2's ceiling allows its first two bytes */
    for (unsigned sid = 0; sid < 256; ++sid) {
        for (unsigned sub = 0; sub < 256; ++sub) {
            ecuio_engine_talk_state t;
            const uint8_t pdu[3] = {static_cast<uint8_t>(sid),
                                    static_cast<uint8_t>(sub), 0x00};
            const ecuio_talk_offer got =
                ecuio_engine_talk(t, ECUIO_ENGINE_PDU_MAX, pdu, 3, 1, false);
            const bool allowed = ecuio_tier2_allowed(pdu[0], pdu[1]);
            ASSERT_EQ(got, allowed ? ecuio_talk_offer::taken
                                   : ecuio_talk_offer::refused)
                << sid << " " << sub;
            ASSERT_EQ(t.offered, allowed);
        }
    }
    /* By name: a write, a programming session, a routine, the default
     * session, a level with bit 7; each refused however short or long */
    for (const Bytes& pdu :
         {Bytes{0x2E, 0xF1, 0x90, 0x00}, Bytes{0x10, 0x02}, Bytes{0x31, 0x01},
          Bytes{0x10, 0x01}, Bytes{0x27, 0x81}, Bytes(100, 0x2E)}) {
        ecuio_engine_talk_state t;
        EXPECT_EQ(ecuio_engine_talk(t, ECUIO_ENGINE_PDU_MAX, pdu.data(),
                                    static_cast<uint32_t>(pdu.size()), 1,
                                    false),
                  ecuio_talk_offer::refused)
            << int{pdu[0]};
        EXPECT_FALSE(t.offered);
    }
}

TEST(EngineTalkTest, AnOfferNeedsARequestAnEchoAndItsSubFunction)
{
    const uint8_t seed[2] = {0x27, 0x01};
    const uint8_t read[3] = {0x22, 0xD2, 0x10};
    ecuio_engine_talk_state t;
    const auto ask = [&t](const uint8_t* pdu, uint32_t len, uint8_t echo) {
        return ecuio_engine_talk(t, ECUIO_ENGINE_PDU_MAX, pdu, len, echo,
                                 false);
    };
    EXPECT_EQ(ask(nullptr, 2, 1), ecuio_talk_offer::refused);
    EXPECT_EQ(ask(seed, 0, 1), ecuio_talk_offer::refused);
    EXPECT_EQ(ask(seed, 2, 0), ecuio_talk_offer::refused);
    const uint8_t two[5] = {0x22, 0xD2, 0x10, 0xD3, 0x01};
    EXPECT_EQ(ask(two, 5, 4), ecuio_talk_offer::refused) << "echo past 3";
    EXPECT_EQ(ask(seed, 2, 3), ecuio_talk_offer::refused) << "echo past len";
    /* a service that splits by sub-function, with none: 27 alone would
     * be judged as 27 00, which no level has; 3E alone as 3E 00 */
    const uint8_t keep[1] = {0x3E};
    EXPECT_EQ(ask(seed, 1, 1), ecuio_talk_offer::refused);
    EXPECT_EQ(ask(keep, 1, 1), ecuio_talk_offer::refused);
    EXPECT_FALSE(t.offered);
    EXPECT_EQ(ask(read, 3, 3), ecuio_talk_offer::taken);
    EXPECT_TRUE(t.offered);
    EXPECT_EQ(Bytes(t.pdu, t.pdu + t.len), Bytes(read, read + 3))
        << "the offer keeps the request";
}

TEST(EngineTalkTest, ARequestPastTheTransportsBoundIsTooLong)
{
    Bytes pdu(ECUIO_ENGINE_PDU_MAX + 1, 0x00);
    pdu[0] = 0x22;
    ecuio_engine_talk_state t;
    const auto ask = [&](uint32_t len, uint32_t pdu_max) {
        return ecuio_engine_talk(t, pdu_max, pdu.data(), len, 1, false);
    };
    EXPECT_EQ(ask(8, ECUIO_ENGINE_PDU_SINGLE), ecuio_talk_offer::too_long);
    EXPECT_EQ(ask(61, ECUIO_ENGINE_PDU_MAX), ecuio_talk_offer::too_long);
    EXPECT_EQ(ask(61, 4095), ecuio_talk_offer::too_long)
        << "no transport's bound passes the engine's own";
    EXPECT_FALSE(t.offered);
    EXPECT_EQ(ask(7, ECUIO_ENGINE_PDU_SINGLE), ecuio_talk_offer::taken);
    EXPECT_EQ(ask(8, ECUIO_ENGINE_PDU_SINGLE), ecuio_talk_offer::too_long)
        << "whatever is under way: busy would invite the same offer again";
    t = ecuio_engine_talk_state{};
    EXPECT_EQ(ask(60, ECUIO_ENGINE_PDU_MAX), ecuio_talk_offer::taken);
    EXPECT_EQ(t.len, 60);
    /* the engine starts at a single frame's bound until attach raises it */
    static ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    EXPECT_EQ(e.pdu_max, ECUIO_ENGINE_PDU_SINGLE);
    EXPECT_EQ(ECUIO_ENGINE_PDU_SINGLE, 7u);
    EXPECT_EQ(ECUIO_ENGINE_PDU_MAX, 60u);
}

TEST(EngineTalkTest, OneTalkAtATimeAndItsSettleIsTakenOnce)
{
    static ecuio_request_engine e;
    engine(&e);
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    const Bytes a = {0x22, 0xD3, 0x01};
    const Bytes b = {0x22, 0xD3, 0x02};
    EXPECT_EQ(taken(&e), ecuio_talk_end::none) << "there is none";
    ASSERT_EQ(offer(&e, a, 3), ecuio_talk_offer::taken);
    EXPECT_EQ(offer(&e, b, 3), ecuio_talk_offer::busy) << "one is offered";
    /* out: sent and awaiting its reply */
    const int32_t r = next_once(&e, &now, &ecu);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, r), a);
    ecuio_engine_sent(&e, 0, r, true, now);
    EXPECT_EQ(offer(&e, b, 3), ecuio_talk_offer::busy) << "one is out";
    EXPECT_EQ(taken(&e), ecuio_talk_end::none) << "not settled yet";
    const Bytes reply = ecu(a);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()),
                                    now + kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(offer(&e, b, 3), ecuio_talk_offer::busy)
        << "settled and not yet taken";
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(taken(&e), ecuio_talk_end::none) << "read once";
    EXPECT_EQ(offer(&e, b, 3), ecuio_talk_offer::taken);
    EXPECT_EQ(count_of(ecu.heard, a), 1u);
}

TEST(EngineTalkTest, ASeedThenItsEighteenByteKeyUnlockThroughTheEngine)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    const Bytes seed = {0x27, 0x03};
    ASSERT_EQ(offer(&e, seed, 2), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, std::ref(ecu));
    ASSERT_EQ(taken(&e), ecuio_talk_end::answered);
    ASSERT_EQ(ecu.security.seed_out, 0x03);
    /* The key: 18 bytes out, and 67 04 answers it, two bytes */
    const Bytes key = key_for(0x03);
    ASSERT_EQ(key.size(), 18u);
    ASSERT_EQ(offer(&e, key, 2, true), ecuio_talk_offer::taken);
    const std::vector<Bytes> sent = until_settled(&e, &now, std::ref(ecu));
    ASSERT_EQ(sent.size(), 1u) << "nothing between the seed and its key";
    EXPECT_EQ(sent[0], key) << "the whole request, not its echo bytes";
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(ecu.security.unlocked, 0x03);
    /* and the gated DID reads */
    ASSERT_EQ(offer(&e, kGated, 3), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, std::ref(ecu));
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(count_of(ecu.heard, key), 1u);
}

/* Nothing comes between a seed's reply and its key: not a scan entry
 * that waits (the scan's turn is only for a talk that isn't first), not
 * the keep-alive when it is due, not the rotation. Only a session entry
 * that is due stands ahead (FirstStandsBehindASessionEntryThatIsDue): an
 * ECU that has left the session has dropped the seed with it. */
TEST(EngineTalkTest, TheKeyIsTheNextRequestWithAScanWaitingAndTheKeepAliveDue)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    for (uint16_t did = 0xD220; did < 0xD228; ++did) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, did)); /* the ECU has none */
    }
    const Bytes seed = {0x27, 0x03};
    ASSERT_EQ(offer(&e, seed, 2), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, std::ref(ecu));
    ASSERT_EQ(taken(&e), ecuio_talk_end::answered);
    ASSERT_TRUE(e.chans[0].talk.after_scan) << "a scan entry has the next turn";
    ASSERT_GT(e.chans[0].scan_pending, 0u);
    /* the caller takes its time over the key: the keep-alive comes due */
    now += e.keepalive_us;
    ASSERT_LT(now - e.chans[0].last_sent_us, ECUIO_ENGINE_S3_US)
        << "the session is still held";
    ecu.heard.clear();
    const Bytes key = key_for(0x03);
    ASSERT_EQ(offer(&e, key, 2, true), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key) << "the very next request";
    ASSERT_EQ(ecu.heard.size(), 1u);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(ecu.security.unlocked, 0x03);
}

/* The door judges the bytes it keeps: a caller's buffer that changes
 * after the offer (a guest's memory, a buffer used again) changes neither
 * the request on the wire nor the echo its reply is matched on. */
TEST(EngineTalkTest, TheWireBytesAreTheOnesOfferedWhateverTheCallerDoesAfter)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    Bytes buf = {0x22, 0xD3, 0x01};
    const Bytes offered = buf;
    ASSERT_EQ(offer(&e, buf, 3), ecuio_talk_offer::taken);
    buf[0] = 0x2E; /* a write, in the same bytes, before the talk's turn */
    buf[1] = 0xF1;
    buf[2] = 0x90;
    const int32_t r = next_once(&e, &now, &ecu);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, r), offered) << "what the door let through";
    const ecuio_request& once = ecuio_engine_request_of(&e, 0, r);
    EXPECT_EQ(Bytes(once.bytes, once.bytes + once.len), offered)
        << "and its reply is matched on those";
    buf[0] = 0x31; /* and again while it is the one-shot, before the write */
    EXPECT_EQ(wire(&e, 0, r), offered);
    ecuio_engine_sent(&e, 0, r, true, now);
    const Bytes reply = ecu(wire(&e, 0, r));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()),
                                    now + kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    for (const Bytes& q : ecu.heard) {
        ASSERT_FALSE(q.empty());
        EXPECT_TRUE(q[0] == 0x22 || q[0] == 0x3E)
            << "service " << int{q[0]} << " went out";
    }

    /* the longest request: each of its 60 bytes is kept */
    Bytes longest(ECUIO_ENGINE_PDU_MAX);
    for (size_t k = 0; k < longest.size(); ++k) {
        longest[k] = static_cast<uint8_t>(0x80 + k);
    }
    longest[0] = 0x2C;
    longest[1] = 0x01;
    const Bytes whole = longest;
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(offer(&e, longest, 2, true), ecuio_talk_offer::taken);
    longest.assign(longest.size(), 0x2E);
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, ECUIO_ENGINE_ONCE), whole);
}

TEST(EngineTalkTest, OnlyAReplyEchoingItsFirstBytesAnswersIt)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    const Bytes key = key_for(0x03);
    ASSERT_EQ(offer(&e, key, 2), ecuio_talk_offer::taken);
    const int32_t r = next_once(&e, &now, &ecu);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, r, true, now);
    const auto frame = [&](std::initializer_list<uint8_t> bytes) {
        const Bytes f(bytes);
        return ecuio_engine_on_frame(&e, 0, f.data(),
                                     static_cast<uint16_t>(f.size()), now + kMs,
                                     nullptr);
    };
    EXPECT_EQ(frame({0x67, 0x03, 0x11, 0x92}), ecuio_reply::none)
        << "another sub-function's reply";
    EXPECT_EQ(frame({0x67}), ecuio_reply::none) << "shorter than its echo";
    EXPECT_EQ(frame({0x62, 0x04}), ecuio_reply::none);
    EXPECT_EQ(taken(&e), ecuio_talk_end::none);
    /* 78: the wait runs on to the pending bound */
    EXPECT_EQ(frame({0x7F, 0x27, 0x78}), ecuio_reply::pending);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US + kMs));
    EXPECT_EQ(frame({0x67, 0x04}), ecuio_reply::positive);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
}

TEST(EngineTalkTest, FirstGoesAheadOfTheKeepAliveTheWalkAndTheRotation)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    /* a Service 01 row, so a probe starts a walk */
    const uint8_t pid[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    now += e.keepalive_us; /* the keep-alive is due */
    ecuio_engine_probe(&e, now);
    ASSERT_TRUE(e.chans[0].walking);
    const Bytes key = key_for(0x01);
    ASSERT_EQ(offer(&e, key, 2, true), ecuio_talk_offer::taken);
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, r), key);
    ecuio_engine_sent(&e, 0, r, true, now);
    /* its service's reply to another request is no stray answer to the
     * walk's bitmap, which waits behind it */
    const Bytes other = {0x67, 0x04};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other.data(), 2, now, nullptr),
              ecuio_reply::none);
    EXPECT_FALSE(e.chans[0].walk_stray);
    const Bytes refusal = ecu(key);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, refusal.data(),
                                    static_cast<uint16_t>(refusal.size()),
                                    now + kMs, nullptr),
              ecuio_reply::negative);
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(taken(&e), ecuio_talk_end::refused) << "24: no seed was out";

    /* Not first: the keep-alive and the walk's bitmap go before it */
    now += e.keepalive_us;
    ecuio_engine_probe(&e, now); /* the walk's budget again */
    ASSERT_TRUE(e.chans[0].walking);
    ASSERT_EQ(offer(&e, key, 2, false), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kKeep);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), (Bytes{0x01, 0x00}));
    const std::vector<Bytes> sent = until_settled(&e, &now, std::ref(ecu));
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.back(), key);
    EXPECT_LE(sent.size(), 2u) << "then at the rotation's next turn";

    /* With no session there is no entry to stand behind, however long
     * the channel has been quiet */
    static ecuio_request_engine plain;
    engine(&plain);
    ASSERT_EQ(ecuio_engine_add(&plain, 0, pid, 2),
              ecuio_engine_add_result::added);
    const int64_t later = 3 * ECUIO_ENGINE_S3_US;
    ecuio_engine_probe(&plain, later);
    ASSERT_TRUE(plain.chans[0].walking);
    const Bytes read = {0x22, 0xD3, 0x01};
    ASSERT_EQ(offer(&plain, read, 3, true), ecuio_talk_offer::taken);
    const int32_t p = ecuio_engine_due(&plain, 0, later);
    ASSERT_EQ(p, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&plain, 0, p), read);
}

TEST(EngineTalkTest, FirstStandsBehindASessionEntryThatIsDue)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    engine(&e);
    ecu.now = &now;
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    const Bytes seed = {0x27, 0x01};
    ASSERT_EQ(offer(&e, seed, 2, true), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter) << "not yet entered";
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), seed);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    /* S3 has passed with nothing sent: the session is gone, so the entry
     * goes first again */
    now += ECUIO_ENGINE_S3_US;
    ASSERT_EQ(offer(&e, seed, 2, true), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), seed);
}

TEST(EngineTalkTest, WithoutFirstItTakesTurnsWithTheScanAndTheRotation)
{
    static ecuio_request_engine e;
    engine(&e);
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    for (uint16_t did = 0xD310; did < 0xD314; ++did) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, did));
    }
    /* A caller that always has a request: a new talk whenever one settles */
    const Bytes talk = {0x22, 0xD3, 0x01};
    std::vector<char> order; /* T a talk, S a scanned DID, R the rotation */
    for (int i = 0; i < 24; ++i) {
        if (taken(&e) != ecuio_talk_end::none || !e.chans[0].talk.offered) {
            ASSERT_EQ(offer(&e, talk, 3), ecuio_talk_offer::taken);
        }
        const int32_t r = ecuio_engine_due(&e, 0, now);
        ASSERT_NE(r, ECUIO_ENGINE_NONE) << i;
        order.push_back(r != ECUIO_ENGINE_ONCE ? 'R'
                        : e.chans[0].talk.out  ? 'T'
                                               : 'S');
        ecuio_engine_sent(&e, 0, r, true, now);
        const Bytes reply = ecu(wire(&e, 0, r));
        (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()), now,
                                    nullptr);
        now += ECUIO_ENGINE_GAP_US;
    }
    std::string once; /* the one-shot's turns alone */
    size_t rows = 0;
    for (const char k : order) {
        if (k == 'R') {
            ++rows;
        } else {
            once.push_back(k);
        }
    }
    ASSERT_GE(once.size(), 8u);
    EXPECT_EQ(once.substr(0, 8), "TSTSTSTS")
        << "the talk and the scan alternate while the scan has entries";
    EXPECT_EQ(e.chans[0].scan_pending, 0u) << "the scan was not held off";
    EXPECT_GE(rows, 8u) << "nor the rotation";
    EXPECT_NE(once.find("TT"), std::string::npos)
        << "with the scan done the talk takes each one-shot turn";

    /* Once a talk has gone, an entry the scan gains goes before the next,
     * and the turn outlives the settle being taken */
    static ecuio_request_engine f;
    engine(&f, 0);
    int64_t at = 0;
    ASSERT_EQ(offer(&f, talk, 3), ecuio_talk_offer::taken);
    (void)until_settled(&f, &at, std::ref(ecu));
    ASSERT_EQ(taken(&f), ecuio_talk_end::answered);
    ASSERT_TRUE(ecuio_engine_scan_add(&f, 0, 0xD220)); /* refused: no row */
    ASSERT_EQ(offer(&f, talk, 3), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&f, 0, &at, std::ref(ecu)), (Bytes{0x22, 0xD2, 0x20}));
    EXPECT_EQ(step(&f, 0, &at, std::ref(ecu)), talk);
}

TEST(EngineTalkTest, AFailedWriteSettlesItUnsentAndItNeverGoesAgain)
{
    static ecuio_request_engine e;
    engine(&e, 0);
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    const Bytes key = key_for(0x01);
    ASSERT_EQ(offer(&e, key, 2), ecuio_talk_offer::taken);
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, r, false, now); /* the write failed */
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE)
        << "a key is not sent twice for one seed";
    EXPECT_EQ(taken(&e), ecuio_talk_end::unsent);
    /* where a scanned DID whose write failed stays due and goes again */
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0xD310));
    const int32_t s = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(s, ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, s, false, now);
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, ECUIO_ENGINE_ONCE), (Bytes{0x22, 0xD3, 0x10}));
}

TEST(EngineTalkTest, SilenceSettlesItUnansweredAndALateReplySettlesNothing)
{
    static ecuio_request_engine e;
    engine(&e);
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    const Bytes talk = {0x22, 0xD3, 0x01};
    ecu.silent = talk;
    ASSERT_EQ(offer(&e, talk, 3), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, std::ref(ecu));
    EXPECT_EQ(taken(&e), ecuio_talk_end::unanswered);
    const Bytes late = {0x62, 0xD3, 0x01, 0x12, 0x34};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, late.data(),
                                    static_cast<uint16_t>(late.size()), now,
                                    nullptr),
              ecuio_reply::none);
    EXPECT_EQ(taken(&e), ecuio_talk_end::none);
    EXPECT_EQ(count_of(ecu.heard, talk), 1u) << "and it is not asked again";
    for (int i = 0; i < 6; ++i) {
        (void)step(&e, 0, &now, std::ref(ecu));
    }
    EXPECT_EQ(count_of(ecu.heard, talk), 1u);
}

TEST(EngineTalkTest, ARefusalSettlesItWithItsCodeAndDropsNoRow)
{
    static ecuio_request_engine e;
    engine(&e, 3);
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    ecuio_engine_probe(&e, now); /* explicit answers drop rows */
    ASSERT_TRUE(e.probe);
    /* 7F 22 11 to an identification item drops the channel's 22 rows; to
     * a talk it drops nothing: a plugin's request takes no row from the
     * data tier */
    const Bytes talk = {0x22, 0xD3, 0x01};
    const Responder no_service = [&](const Bytes& q) -> Bytes {
        return q == talk ? Bytes{0x7F, 0x22, 0x11} : ecu(q);
    };
    ASSERT_EQ(offer(&e, talk, 3), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, no_service);
    uint8_t nrc = 0;
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x11);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].dropped, 0) << i;
    }
    EXPECT_EQ(e.chans[0].requests, 3u);
    /* and a request out of range: its own code */
    ASSERT_EQ(offer(&e, kGated, 3), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, std::ref(ecu));
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x31);
    EXPECT_EQ(e.chans[0].requests, 3u);
}

TEST(EngineTalkTest, A7FInTheSessionEntersItAgainAndTheTalkIsNotAskedAgain)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    /* The ECU has fallen back to the default session behind core's back */
    ecu.extended = false;
    const Bytes seed = {0x27, 0x01};
    ASSERT_EQ(offer(&e, seed, 2), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, std::ref(ecu));
    uint8_t nrc = 0;
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x7F);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
    EXPECT_EQ(e.chans[0].session_lost, 1u);
    EXPECT_EQ(count_of(ecu.heard, seed), 1u)
        << "it went once, not again ahead of the entry";
    ecu.heard.clear();
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter)
        << "the session is entered again";
    for (int i = 0; i < 6; ++i) {
        (void)step(&e, 0, &now, std::ref(ecu));
    }
    EXPECT_EQ(count_of(ecu.heard, seed), 0u)
        << "the caller decides what follows a lost session";
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
}

TEST(EngineTalkTest, A7FStraightAfterTheEntryIsTheTalksOwnAnswer)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    engine(&e, 0);
    ecu.now = &now;
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter);
    ASSERT_TRUE(e.chans[0].session_fresh);
    /* An ECU that keeps 27 to another session answers 7F at once: no
     * session was lost, and nothing is entered again */
    const Bytes seed = {0x27, 0x01};
    const Responder kept = [&](const Bytes& q) -> Bytes {
        return q == seed ? Bytes{0x7F, 0x27, 0x7F} : ecu(q);
    };
    ASSERT_EQ(offer(&e, seed, 2), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, kept);
    uint8_t nrc = 0;
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x7F);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[0].session_lost, 0u);
}

TEST(EngineTalkTest, AnOfferedTalkIsSomethingToSend)
{
    static ecuio_request_engine e;
    engine(&e, 0);
    int64_t now = 5 * ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), INT64_MAX)
        << "a channel with nothing to ask";
    EXPECT_FALSE(ecuio_engine_once_waiting(e.chans[0]));
    const Bytes talk = {0x22, 0xD3, 0x01};
    ASSERT_EQ(offer(&e, talk, 3), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 0)
        << "a worker waiting on the next event wakes for it";
    EXPECT_TRUE(ecuio_engine_once_waiting(e.chans[0]));
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_TRUE(ecuio_engine_once_waiting(e.chans[0])) << "taken, not yet sent";
    ecuio_engine_sent(&e, 0, r, true, now);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), ECUIO_ENGINE_WAIT_US)
        << "then its wait";
}

TEST(EngineTalkTest, TheWireBytesAreTheTalksOnlyWhileItIsTheOneShot)
{
    static ecuio_request_engine e;
    engine(&e);
    int64_t now = 0;
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0xD310));
    const Bytes key = key_for(0x03);
    /* a row and a scanned DID: their own bytes, a talk offered or not */
    EXPECT_EQ(wire(&e, 0, 0), kRow);
    ASSERT_EQ(offer(&e, key, 2, true), ecuio_talk_offer::taken);
    EXPECT_EQ(wire(&e, 0, 0), kRow);
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE)
        << "taken: it stays the one-shot until it is sent";
    const ecuio_request& once = ecuio_engine_request_of(&e, 0, r);
    EXPECT_EQ(Bytes(once.bytes, once.bytes + once.len), (Bytes{0x27, 0x04}))
        << "the one-shot holds the echo bytes its reply is matched on";
    uint32_t len = 0;
    const uint8_t* out =
        ecuio_engine_pdu_of(e.chans[0].talk, true, once.bytes, once.len, &len);
    EXPECT_NE(out, key.data()) << "the talk's own copy, not the caller's bytes";
    ASSERT_EQ(len, 18u);
    EXPECT_EQ(Bytes(out, out + len), key);
    EXPECT_EQ(wire(&e, 0, 0), kRow)
        << "a row's are its own while a talk is out";
    ecuio_engine_sent(&e, 0, r, false, now);
    ASSERT_EQ(taken(&e), ecuio_talk_end::unsent);
    now += ECUIO_ENGINE_GAP_US;
    Ecu ecu;
    ecu.now = &now;
    const int32_t s = next_once(&e, &now, &ecu);
    ASSERT_EQ(s, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, s), (Bytes{0x22, 0xD3, 0x10}))
        << "the next one-shot's are its own";

    /* a talk's one-shot is its own channel's */
    static ecuio_request_engine two;
    ASSERT_TRUE(ecuio_engine_init(&two, 2));
    two.pdu_max = ECUIO_ENGINE_PDU_MAX;
    ASSERT_EQ(offer(&two, key, 2, true, 1), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_due(&two, 0, now), ECUIO_ENGINE_NONE);
    ASSERT_EQ(ecuio_engine_due(&two, 1, now), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(ecuio_engine_request_of(&two, 1, ECUIO_ENGINE_ONCE).channel, 1);
    EXPECT_EQ(wire(&two, 1, ECUIO_ENGINE_ONCE), key);
}
