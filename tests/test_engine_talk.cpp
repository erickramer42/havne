// File/layer: tests/test_engine_talk.cpp | test_obd2, Qt-free
// Testing: UNIT: EngineTalkTest (a talk through the request engine's
//          one-shot, ADR-0066 section 2.4, with the time passed in: the
//          door, its turn and its settle)

#include "engine_talk_support.h"

using namespace engine_talk_test;

TEST(EngineTalkTest, TheDoorIsTheCeilingForEveryServiceAndSubFunction)
{
    /* Whatever the caller checked: an offer is taken exactly when tier
     * 2's ceiling allows its first two bytes for its kind, a plain talk
     * by the guest's own list. Each is offered as what it can be (a 27 as
     * a seed or, inside a window, a key) with its service's echo, by a
     * list of the case's own. */
    for (unsigned sid = 0; sid < 256; ++sid) {
        for (unsigned sub = 0; sub < 256; ++sub) {
            ecuio_engine_talk_state t;
            ecuio_key_window w;
            w.until_us = 1000;
            w.seed = static_cast<uint8_t>(sub - 1); /* a key is its seed's */
            const Bytes pdu = {static_cast<uint8_t>(sid),
                               static_cast<uint8_t>(sub), 0x00};
            const uint8_t echo = sid == 0x22 ? 3 : sid == 0x2A ? 1 : 2;
            ecuio_talk_offer got = ecuio_talk_offer::busy;
            if (sid != 0x27) {
                got = door(t, w, pdu, echo);
            } else if (sub % 2 == 1) {
                got = door(t, w, {pdu[0], pdu[1]}, 2, ecuio_talk_kind::seed,
                           kWindow);
            } else {
                got = door(t, w, pdu, 2, ecuio_talk_kind::key);
            }
            const bool allowed =
                sid == 0x27 ? ecuio_tier2_allowed(pdu[0], pdu[1])
                            : ecuio_tier2_guest_may_send(pdu[0], pdu[1]);
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
        ecuio_key_window w;
        EXPECT_EQ(door(t, w, pdu, 2), ecuio_talk_offer::refused) << int{pdu[0]};
        EXPECT_EQ(door(t, w, pdu, 2, ecuio_talk_kind::seed, kWindow),
                  ecuio_talk_offer::refused)
            << int{pdu[0]};
        EXPECT_FALSE(t.offered);
    }
    /* Inside the ceiling and still refused plain: the extended session and
     * tester present are core's own step, and nothing takes a 2A's
     * periodic reply */
    for (const Bytes& pdu :
         {Bytes{0x10, 0x03}, Bytes{0x3E, 0x00}, Bytes{0x2A, 0x01, 0x10}}) {
        ecuio_engine_talk_state t;
        ecuio_key_window w;
        ASSERT_TRUE(ecuio_tier2_allowed(pdu[0], pdu[1])) << int{pdu[0]};
        EXPECT_EQ(door(t, w, pdu, ecuio_talk_echo_min(pdu[0])),
                  ecuio_talk_offer::refused)
            << int{pdu[0]};
        EXPECT_FALSE(t.offered);
    }
}

/* A 27 leaves core only as a seed or as a key inside a window, however
 * its caller named it: the sandbox's bound doesn't rest on the label. */
TEST(EngineTalkTest, A27IsTakenOnlyAsASeedOrAKey)
{
    const Bytes seed = {0x27, 0x01};
    const Bytes key = key_for(0x01);
    const Bytes read = {0x22, 0xD2, 0x10};
    const auto refused = [](const Bytes& pdu, uint8_t echo,
                            ecuio_talk_kind kind, int64_t window_us,
                            int64_t until_us, int64_t now_us) {
        ecuio_engine_talk_state t;
        ecuio_key_window w;
        w.until_us = until_us;
        w.seed = 0x01;
        const ecuio_talk_offer got =
            door(t, w, pdu, echo, kind, window_us, now_us);
        EXPECT_EQ(t.offered, got == ecuio_talk_offer::taken);
        EXPECT_NE(got, ecuio_talk_offer::busy);
        EXPECT_NE(got, ecuio_talk_offer::too_long);
        return got == ecuio_talk_offer::refused;
    };
    const ecuio_talk_kind plain = ecuio_talk_kind::plain;
    const ecuio_talk_kind as_seed = ecuio_talk_kind::seed;
    const ecuio_talk_kind as_key = ecuio_talk_kind::key;
    /* plain: never a 27, with a window open or none */
    EXPECT_TRUE(refused(seed, 2, plain, 0, 0, 0));
    EXPECT_TRUE(refused(key, 2, plain, 0, 0, 0));
    EXPECT_TRUE(refused(key, 2, plain, 0, 1000, 0)) << "a window is open";
    EXPECT_FALSE(refused(read, 3, plain, 0, 0, 0));
    /* a seed: 27 and an odd sub-function, nothing more, its window in
     * bounds */
    EXPECT_FALSE(refused(seed, 2, as_seed, kWindow, 0, 0));
    EXPECT_TRUE(refused(read, 3, as_seed, kWindow, 0, 0)) << "a read";
    EXPECT_TRUE(refused({0x10, 0x03}, 2, as_seed, kWindow, 0, 0))
        << "odd and two bytes, and no 27";
    EXPECT_TRUE(refused({0x27, 0x02}, 2, as_seed, kWindow, 0, 0)) << "even";
    EXPECT_TRUE(refused({0x27, 0x01, 0x00}, 2, as_seed, kWindow, 0, 0))
        << "a seed request carries nothing";
    EXPECT_TRUE(refused(seed, 2, as_seed, 0, 0, 0)) << "no window";
    EXPECT_TRUE(refused(seed, 2, as_seed, ECUIO_KEY_WINDOW_MIN_US - 1, 0, 0));
    EXPECT_FALSE(refused(seed, 2, as_seed, ECUIO_KEY_WINDOW_MIN_US, 0, 0));
    EXPECT_FALSE(refused(seed, 2, as_seed, ECUIO_KEY_WINDOW_MAX_US, 0, 0));
    EXPECT_TRUE(refused(seed, 2, as_seed, ECUIO_KEY_WINDOW_MAX_US + 1, 0, 0));
    EXPECT_EQ(ECUIO_KEY_WINDOW_MIN_US, 50 * kMs);
    EXPECT_EQ(ECUIO_KEY_WINDOW_MAX_US, 4000 * kMs);
    /* a key: 27 and the sub-function after its seed's (01 here), while
     * the window is open */
    EXPECT_FALSE(refused(key, 2, as_key, 0, 1000, 999));
    EXPECT_TRUE(refused(key, 2, as_key, 0, 1000, 1000)) << "at its end";
    EXPECT_TRUE(refused(key, 2, as_key, 0, 0, 0)) << "no window";
    EXPECT_TRUE(refused(read, 3, as_key, 0, 1000, 0)) << "a read";
    EXPECT_TRUE(refused({0x19, 0x02, 0xFF}, 2, as_key, 0, 1000, 0))
        << "the key's sub-function, and no 27";
    EXPECT_TRUE(refused(seed, 2, as_key, 0, 1000, 0)) << "odd";
    EXPECT_TRUE(refused(key_for(0x03), 2, as_key, 0, 1000, 0))
        << "even, and another seed's";
    EXPECT_FALSE(refused({0x27, 0x02}, 2, as_key, 0, 1000, 0))
        << "its length is the ECU's to judge";
}

TEST(EngineTalkTest, AnOfferNeedsARequestAnEchoAndItsSubFunction)
{
    /* Each probe is a guest's own service, so the rule named is the one
     * that refuses it; its twin beside it is taken */
    const Bytes read = {0x22, 0xD2, 0x10};
    const Bytes dtc = {0x19, 0x02};
    const auto ask = [](const uint8_t* pdu, uint32_t len, uint8_t echo) {
        ecuio_engine_talk_state t;
        ecuio_key_window w;
        ecuio_talk_request q;
        q.pdu = pdu;
        q.len = len;
        q.echo = echo;
        const ecuio_talk_offer got =
            ecuio_engine_talk(t, w, ECUIO_ENGINE_PDU_MAX, q, 0);
        EXPECT_EQ(t.offered, got == ecuio_talk_offer::taken);
        return got;
    };
    const ecuio_talk_offer taken = ecuio_talk_offer::taken;
    const ecuio_talk_offer refused = ecuio_talk_offer::refused;
    EXPECT_EQ(ask(dtc.data(), 2, 2), taken);
    EXPECT_EQ(ask(nullptr, 2, 2), refused);
    EXPECT_EQ(ask(dtc.data(), 0, 2), refused) << "no request";
    EXPECT_EQ(ask(dtc.data(), 2, 0), refused) << "echo 0, below 19's own too";
    const uint8_t two[5] = {0x22, 0xD2, 0x10, 0xD3, 0x01};
    EXPECT_EQ(ask(two, 5, 3), taken);
    EXPECT_EQ(ask(two, 5, 4), refused) << "echo past 3";
    EXPECT_EQ(ask(dtc.data(), 2, 3), refused)
        << "echo past len: the third byte would be another talk's";
    EXPECT_EQ(ask(read.data(), 2, 3), refused) << "echo past len, a read";
    /* a service that splits by sub-function, with none: 2C alone would
     * be judged as 2C 00 (the guest's list refuses that too) */
    const uint8_t define[1] = {0x2C};
    EXPECT_EQ(ask(define, 1, 1), refused);
    ecuio_engine_talk_state t;
    ecuio_key_window w;
    ecuio_talk_request q;
    q.pdu = read.data();
    q.len = 3;
    q.echo = 3;
    EXPECT_EQ(ecuio_engine_talk(t, w, ECUIO_ENGINE_PDU_MAX, q, 0), taken);
    EXPECT_TRUE(t.offered);
    EXPECT_EQ(Bytes(t.pdu, t.pdu + t.len), read)
        << "the offer keeps the request";
}

/* A reply is matched on the echo bytes, so too few would let another
 * request's reply settle the talk: any 62 a read, any 67 a key */
TEST(EngineTalkTest, AnEchoIsNeverShorterThanItsServicesOwn)
{
    struct Row {
        Bytes pdu;
        uint8_t least;
        ecuio_talk_kind kind;
    };
    const Row rows[] = {
        {{0x22, 0xD2, 0x10}, 3, ecuio_talk_kind::plain},
        {{0x22, 0xD2, 0x10, 0xD3, 0x01}, 3, ecuio_talk_kind::plain},
        {{0x19, 0x02, 0xFF}, 2, ecuio_talk_kind::plain},
        {{0x2C, 0x03, 0xF2, 0x00}, 2, ecuio_talk_kind::plain},
        {{0x27, 0x01}, 2, ecuio_talk_kind::seed},
        {key_for(0x01), 2, ecuio_talk_kind::key},
    };
    for (const Row& row : rows) {
        for (uint8_t echo = 1; echo <= 3 && echo <= row.pdu.size(); ++echo) {
            ecuio_engine_talk_state t;
            ecuio_key_window w;
            w.until_us = 1000;
            w.seed = 0x01;
            EXPECT_EQ(door(t, w, row.pdu, echo, row.kind, kWindow),
                      echo >= row.least ? ecuio_talk_offer::taken
                                        : ecuio_talk_offer::refused)
                << int{row.pdu[0]} << " echo " << int{echo};
        }
        EXPECT_EQ(ecuio_talk_echo_min(row.pdu[0]), row.least);
    }
    /* and the services no plain talk carries keep the rule */
    EXPECT_EQ(ecuio_talk_echo_min(0x10), 2);
    EXPECT_EQ(ecuio_talk_echo_min(0x3E), 2);
    EXPECT_EQ(ecuio_talk_echo_min(0x2A), 1);
    /* a 22 with no whole DID has no echo of three */
    ecuio_engine_talk_state t;
    ecuio_key_window w;
    EXPECT_EQ(door(t, w, {0x22, 0xD2}, 2), ecuio_talk_offer::refused);
    EXPECT_EQ(door(t, w, {0x22}, 1), ecuio_talk_offer::refused);
}

TEST(EngineTalkTest, ARequestPastTheTransportsBoundIsTooLong)
{
    Bytes pdu(ECUIO_ENGINE_PDU_MAX + 1, 0x00);
    pdu[0] = 0x22;
    ecuio_engine_talk_state t;
    ecuio_key_window w;
    const auto ask = [&](uint32_t len, uint32_t pdu_max) {
        return door(t, w, Bytes(pdu.begin(), pdu.begin() + len), 3,
                    ecuio_talk_kind::plain, 0, 0, pdu_max);
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
    EXPECT_EQ(offer(&e, {0x22, 0xD3, 0x01}, 3, 1), ecuio_talk_offer::refused)
        << "a channel the engine lacks";
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
    ASSERT_EQ(offer(&e, longest, 2), ecuio_talk_offer::taken);
    longest.assign(longest.size(), 0x2E);
    ASSERT_EQ(next_once(&e, &now, &ecu), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, ECUIO_ENGINE_ONCE), whole);
}

TEST(EngineTalkTest, OnlyAReplyEchoingItsFirstBytesAnswersIt)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    seeded(&e, &ecu, &now);
    const Bytes key = key_for(0x03);
    ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
    const int32_t r = ecuio_engine_due(&e, 0, now);
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

TEST(EngineTalkTest, APlainTalkTakesTurnsWithTheScanAndTheRotation)
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
    const Bytes talk = long_read();
    ASSERT_EQ(offer(&e, talk, 2), ecuio_talk_offer::taken);
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, r, false, now); /* the write failed */
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE)
        << "part of it may be on the bus: never a second time";
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
    ecuio_engine_probe(&e); /* explicit answers drop rows */
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
    /* The ECU has fallen back to the default session behind core's back,
     * and keeps this DID to the extended one */
    const Bytes talk = {0x22, 0xD3, 0x01};
    const Responder left = [&](const Bytes& q) -> Bytes {
        if (q == talk && !ecu.extended) {
            ecu.heard.push_back(q);
            return {0x7F, 0x22, 0x7F};
        }
        return ecu(q);
    };
    ecu.extended = false;
    ASSERT_EQ(offer(&e, talk, 3), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, left);
    uint8_t nrc = 0;
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x7F);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
    EXPECT_EQ(e.chans[0].session_lost, 1u);
    EXPECT_EQ(count_of(ecu.heard, talk), 1u)
        << "it went once, not again ahead of the entry";
    ecu.heard.clear();
    EXPECT_EQ(step(&e, 0, &now, left), kEnter)
        << "the session is entered again";
    for (int i = 0; i < 6; ++i) {
        (void)step(&e, 0, &now, left);
    }
    EXPECT_EQ(count_of(ecu.heard, talk), 0u)
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
    ASSERT_EQ(offer_seed(&e, now, 0x01), ecuio_talk_offer::taken);
    (void)until_settled(&e, &now, kept);
    uint8_t nrc = 0;
    EXPECT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
    EXPECT_EQ(nrc, 0x7F);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[0].session_lost, 0u);
    EXPECT_EQ(e.chans[0].key.until_us, 0) << "a refused seed opens no window";
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
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0xD310));
    const Bytes talk = long_read();
    /* a row and a scanned DID: their own bytes, a talk offered or not */
    EXPECT_EQ(wire(&e, 0, 0), kRow);
    ASSERT_EQ(offer(&e, talk, 2), ecuio_talk_offer::taken);
    EXPECT_EQ(wire(&e, 0, 0), kRow);
    const int32_t r = next_once(&e, &now, &ecu);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE)
        << "taken: it stays the one-shot until it is sent";
    const ecuio_request& once = ecuio_engine_request_of(&e, 0, r);
    EXPECT_EQ(Bytes(once.bytes, once.bytes + once.len), (Bytes{0x2C, 0x01}))
        << "the one-shot holds the echo bytes its reply is matched on";
    uint32_t len = 0;
    const uint8_t* out =
        ecuio_engine_pdu_of(e.chans[0].talk, true, once.bytes, once.len, &len);
    EXPECT_NE(out, talk.data())
        << "the talk's own copy, not the caller's bytes";
    ASSERT_EQ(len, 18u);
    EXPECT_EQ(Bytes(out, out + len), talk);
    EXPECT_EQ(wire(&e, 0, 0), kRow)
        << "a row's are its own while a talk is out";
    ecuio_engine_sent(&e, 0, r, false, now);
    ASSERT_EQ(taken(&e), ecuio_talk_end::unsent);
    now += ECUIO_ENGINE_GAP_US;
    const int32_t s = next_once(&e, &now, &ecu);
    ASSERT_EQ(s, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, s), (Bytes{0x22, 0xD3, 0x10}))
        << "the next one-shot's are its own";

    /* a talk's one-shot is its own channel's */
    static ecuio_request_engine two;
    ASSERT_TRUE(ecuio_engine_init(&two, 2));
    two.pdu_max = ECUIO_ENGINE_PDU_MAX;
    ASSERT_EQ(offer(&two, talk, 2, 1), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_due(&two, 0, now), ECUIO_ENGINE_NONE);
    ASSERT_EQ(ecuio_engine_due(&two, 1, now), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(ecuio_engine_request_of(&two, 1, ECUIO_ENGINE_ONCE).channel, 1);
    EXPECT_EQ(wire(&two, 1, ECUIO_ENGINE_ONCE), talk);
}
