// File/layer: tests/test_request_turns.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestTurnsTest (which channel asks next on an adapter
//          that asks one channel at a time, request_turns.h, and each
//          channel's walk budget, with the time passed in)

#include "request_turns_support.h"

using namespace request_turns_test;

/* Six rows on channel 0, one on channel 1: channel 0 asks each of its
 * rows, then channel 1 its one, so every row is asked once a lap and the
 * header switches twice a lap, not before and after each of 1's asks */
TEST(RequestTurnsTest, TurnsGoByRequestNotByChannel)
{
    auto a = adapter(2);
    dids(&a->e, 0, 6);
    dids(&a->e, 1, 1, 0x80);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    a->asks(21);
    const std::vector<uint32_t> want = {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
                                        0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_EQ(a->channels(), want);
    for (size_t lap = 0; lap < 3; ++lap) {
        std::set<uint8_t> asked;
        for (size_t i = 0; i < 6; ++i) {
            asked.insert(a->sent[lap * 7 + i].bytes[2]);
        }
        EXPECT_EQ(asked.size(), 6u) << "lap " << lap;
    }
}

/* A pack counts each of its rows: once channel 0's eight PIDs are
 * packed, six at a time (a pack goes on around the rotation), its turn is
 * two exchanges, then channel 1's */
TEST(RequestTurnsTest, APackCountsEachOfItsRows)
{
    auto a = adapter(2);
    const uint8_t pids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11};
    for (const uint8_t pid : pids) {
        add(&a->e, 0, {0x01, pid});
    }
    dids(&a->e, 1, 1, 0x80);
    a->e.pack = true;
    ecuio_engine_probe(&a->e);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q, 0x00); };
    /* the bitmap, eight alone (no length known yet), the DID */
    a->asks(10);
    EXPECT_EQ(runs(a->channels()).size(), 2u);
    a->asks(10 + 3 * 3);
    const std::vector<uint32_t> want = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    EXPECT_EQ(a->channels(10), want);
    EXPECT_EQ(a->sent[10].bytes.size(), 7u) << "a pack of six";
    EXPECT_EQ(a->sent[11].bytes.size(), 7u) << "a pack of six";
}

/* Under packframes=1 the same eight PIDs go in packs of two or three, so
 * channel 0's turn is more exchanges, and each turn still asks every one
 * of its rows before channel 1's */
TEST(RequestTurnsTest, ATurnUnderOneFramePacksAsksEachRow)
{
    auto a = adapter(2);
    const uint8_t pids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11};
    for (const uint8_t pid : pids) {
        add(&a->e, 0, {0x01, pid});
    }
    dids(&a->e, 1, 1, 0x80);
    a->e.pack = true;
    a->e.pack_frames = 1;
    ecuio_engine_probe(&a->e);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q, 0x00); };
    a->asks(10 + 30);
    const auto turns = runs(a->channels(10));
    ASSERT_GE(turns.size(), 6u);
    size_t i = 10;
    for (size_t t = 0; t + 1 < turns.size(); ++t) {
        std::set<uint8_t> asked;
        for (int k = 0; k < turns[t].second; ++k, ++i) {
            const Bytes& q = a->sent[i].bytes;
            if (turns[t].first == 0) {
                EXPECT_LE(q.size(), 4u) << "three PIDs at most";
                asked.insert(q.begin() + 1, q.end());
            }
        }
        if (turns[t].first == 0) {
            EXPECT_GT(turns[t].second, 2) << "more exchanges than packs of six";
            EXPECT_EQ(asked.size(), sizeof pids) << "turn " << t;
        } else {
            EXPECT_EQ(turns[t].second, 1);
        }
    }
}

/* The test car's four-channel pick: 0x7E8's walk comes after a DID, a
 * silent module's two rows and a refusing module's row, each exchange
 * 110 ms. It reaches the last bitmap, since its second starts at its
 * first bitmap and the walk keeps the turn, and its PIDs then pack. */
TEST(RequestTurnsTest, TheWalkReachesTheLastBitmapWithFourChannels)
{
    auto a = adapter(4);
    dids(&a->e, 0, 1);       /* 0x7E9 */
    dids(&a->e, 1, 8, 0x10); /* 0x769, silent */
    dids(&a->e, 2, 1, 0x20); /* 0x72E, refuses */
    /* 0x7E8 */
    const uint8_t pids[] = {0x0C, 0x33, 0x42, 0x68};
    for (const uint8_t pid : pids) {
        add(&a->e, 3, {0x01, pid});
    }
    a->e.pack = true;
    a->exchange_us = 110 * kMs;
    ecuio_engine_probe(&a->e);
    a->ecus = [](uint32_t c, const Bytes& q) -> Bytes {
        if (c == 1) {
            return {};
        }
        if (c == 2) {
            return {0x7F, q[0], 0x22}; /* conditions not correct: kept */
        }
        return answer(q);
    };
    a->until(4000 * kMs);
    const ecuio_engine_channel& ch = a->e.chans[3];
    EXPECT_TRUE(ch.walked) << "the walk stopped short";
    EXPECT_EQ(ch.ranges, 0xFFu);
    EXPECT_EQ(ch.requests, 4u) << "nothing dropped";
    ecuio_channel_support_t s;
    ecuio_engine_support(&a->e, 3, &s);
    EXPECT_EQ(s.flags & ECUIO_SUPPORT_COMPLETE, ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.flags & ECUIO_SUPPORT_PACKED, ECUIO_SUPPORT_PACKED);
    int bitmaps = 0;
    size_t first = a->sent.size();
    for (size_t i = 0; i < a->sent.size(); ++i) {
        const Sent& x = a->sent[i];
        if (x.c == 3 && x.bytes.size() == 2 && x.bytes[1] % 0x20 == 0) {
            first = (first == a->sent.size()) ? i : first;
            ++bitmaps;
            EXPECT_EQ(i - first, static_cast<size_t>(bitmaps - 1))
                << "another channel asked inside the walk";
        }
    }
    EXPECT_EQ(bitmaps, 8);
    EXPECT_GT(a->sent[first].at_us, ECUIO_ENGINE_PROBE_BUDGET_US)
        << "the case means the walk to start past the probe's second";
}

/* A walk resumed by a recovery in the middle of a long turn keeps the
 * turn past the turn's second until it ends */
TEST(RequestTurnsTest, AWalkKeepsTheTurnPastTheTurnsSecond)
{
    auto a = adapter(2);
    add(&a->e, 0, {0x01, 0x0C});
    add(&a->e, 0, {0x01, 0x42});
    dids(&a->e, 0, 20);
    dids(&a->e, 1, 12, 0x40);
    a->exchange_us = 100 * kMs;
    bool silent = true; /* the first bitmap goes unanswered */
    a->ecus = [&silent](uint32_t c, const Bytes& q) -> Bytes {
        if (c == 1) {
            return {};
        }
        return (silent && q[0] == 0x01) ? Bytes{} : answer(q);
    };
    ecuio_engine_probe(&a->e);
    a->asks(1);
    a->pass(); /* the bitmap's wait ends the walk */
    ASSERT_FALSE(a->e.chans[0].walking);
    silent = false;
    a->until(a->now + 700 * kMs); /* channel 0's rotation, 0.7 s in */
    ASSERT_EQ(a->turns.chan, 0u);
    ecuio_engine_probe(&a->e); /* the driver's recovery */
    a->until(a->now + 3000 * kMs);
    EXPECT_TRUE(a->e.chans[0].walked);
    EXPECT_EQ(a->e.chans[0].ranges, 0xFFu);
}

/* Each channel's budget starts at its own first bitmap: a walk whose
 * first bitmap goes 1.5 s after the probe still walks, and ends a second
 * after that bitmap */
TEST(RequestTurnsTest, TheWalksSecondStartsAtItsFirstBitmap)
{
    auto a = adapter(1);
    add(&a->e, 0, {0x01, 0x0C});
    ecuio_engine_probe(&a->e);
    int64_t now = 1500 * kMs;
    const uint8_t bitmaps[] = {0x00, 0x20};
    for (const uint8_t pid : bitmaps) {
        ASSERT_EQ(ecuio_engine_due(&a->e, 0, now), ECUIO_ENGINE_ONCE)
            << "the walk ended at " << now;
        ASSERT_EQ(a->e.chans[0].once.bytes[1], pid);
        ecuio_engine_sent(&a->e, 0, ECUIO_ENGINE_ONCE, true, now);
        const Bytes r = answer({0x01, pid});
        (void)ecuio_engine_on_frame(&a->e, 0, r.data(),
                                    static_cast<uint16_t>(r.size()),
                                    now + 50 * kMs, nullptr);
        now += 100 * kMs;
    }
    /* a pending reply holds the third past the walk's own second */
    ASSERT_EQ(ecuio_engine_due(&a->e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&a->e, 0, ECUIO_ENGINE_ONCE, true, now);
    const uint8_t pending[3] = {0x7F, 0x01, ECUIO_NRC_RESPONSE_PENDING};
    (void)ecuio_engine_on_frame(&a->e, 0, pending, 3, now + kMs, nullptr);
    const int64_t late = 1500 * kMs + ECUIO_ENGINE_PROBE_BUDGET_US;
    const Bytes r = answer({0x01, 0x40});
    (void)ecuio_engine_on_frame(&a->e, 0, r.data(),
                                static_cast<uint16_t>(r.size()), late, nullptr);
    EXPECT_FALSE(a->e.chans[0].walking) << "the walk asked past its second";
    EXPECT_EQ(a->e.chans[0].ranges, 0x7u);
}

/* Exchanges shorter than the floor: the turn waits its own floor out and
 * keeps going, rather than handing the turn on */
TEST(RequestTurnsTest, ATurnWaitsOutItsOwnFloor)
{
    auto a = adapter(2);
    dids(&a->e, 0, 3);
    dids(&a->e, 1, 1, 0x80);
    a->exchange_us = 5 * kMs;
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    a->asks(2);
    EXPECT_EQ(a->last_wait_us, ECUIO_ENGINE_GAP_US - 5 * kMs);
    a->asks(8);
    const std::vector<uint32_t> want = {0, 0, 0, 1, 0, 0, 0, 1};
    EXPECT_EQ(a->channels(), want);
    EXPECT_GE(a->sent[1].at_us - a->sent[0].at_us, ECUIO_ENGINE_GAP_US);
}

/* A table longer than a second's exchanges: the turn ends at its second,
 * channel 1 asks, and channel 0 goes on from where it stopped, so every
 * row is asked within two laps of 30 rows */
TEST(RequestTurnsTest, ATurnEndsAfterASecondAndTheNextGoesOn)
{
    auto a = adapter(2);
    dids(&a->e, 0, 30);
    dids(&a->e, 1, 1, 0x80);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    a->until(6000 * kMs);
    std::vector<uint8_t> rows;
    for (const Sent& x : a->sent) {
        if (x.c == 0) {
            rows.push_back(x.bytes[2]);
        }
    }
    ASSERT_GE(rows.size(), 60u);
    for (size_t lap = 0; lap < 2; ++lap) {
        const std::set<uint8_t> once(rows.begin() + lap * 30,
                                     rows.begin() + (lap + 1) * 30);
        EXPECT_EQ(once.size(), 30u) << "lap " << lap << " repeated a row";
    }
    for (const auto& run : runs(a->channels())) {
        if (run.first == 0) {
            EXPECT_LE(run.second * a->exchange_us,
                      ECUIO_ENGINE_TURN_US + a->exchange_us);
        }
    }
}

/* With the extended session on both channels, channel 1's keep-alive goes
 * out within an exchange of falling due, not at the end of 0's turn */
TEST(RequestTurnsTest, ADueKeepAliveEndsATurn)
{
    auto a = adapter(2);
    dids(&a->e, 0, 40);
    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MIN_US));
    a->ecus = [](uint32_t, const Bytes& q) { return in_session(q); };
    a->until(4000 * kMs);
    int keeps = 0;
    int64_t last = -1;
    for (const Sent& x : a->sent) {
        if (x.c != 1) {
            continue;
        }
        if (last >= 0) {
            /* an entry's answer, an exchange after it, starts the wait */
            EXPECT_LE(x.at_us - last,
                      ECUIO_ENGINE_KEEPALIVE_MIN_US + 2 * a->exchange_us)
                << "channel 1 waited on 0's turn";
        }
        last = x.at_us;
        keeps += x.bytes[0] == 0x3E ? 1 : 0;
    }
    EXPECT_GE(keeps, 5);
    EXPECT_EQ(a->e.chans[1].session_lost, 0u);
}

/* Four channels with rows ahead of a fifth with none, the keep-alive at
 * its longest: the fifth's keep-alive goes out next when it falls due,
 * not after one exchange on each channel between */
TEST(RequestTurnsTest, ADueKeepAliveIsAskedNextPastEveryChannelBetween)
{
    auto a = adapter(5);
    for (uint32_t c = 0; c < 4; ++c) {
        dids(&a->e, c, 40, static_cast<int>(c) * 0x40);
    }
    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MAX_US));
    a->ecus = [](uint32_t, const Bytes& q) { return in_session(q); };
    a->until(30000 * kMs);
    int keeps = 0;
    int64_t last = -1;
    for (const Sent& x : a->sent) {
        if (x.c != 4) {
            continue;
        }
        if (last >= 0) {
            EXPECT_LE(x.at_us - last,
                      ECUIO_ENGINE_KEEPALIVE_MAX_US + 2 * a->exchange_us)
                << "channel 4 waited on the channels between";
        }
        last = x.at_us;
        keeps += x.bytes[0] == 0x3E ? 1 : 0;
    }
    EXPECT_GE(keeps, 5);
    for (uint32_t c = 0; c < 5; ++c) {
        EXPECT_EQ(a->e.chans[c].session_lost, 0u) << "channel " << c;
    }
}

/* A channel inside its key's window (request_talk.h) has nothing due
 * until the window ends: its keep-alive, due meanwhile, ends no other
 * channel's turn, and goes out within an exchange of the window's end */
TEST(RequestTurnsTest, AChannelInsideItsKeysWindowEndsNoTurnUntilItCloses)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 1, 0x40);
    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MIN_US));
    const Bytes seed = {0x27, 0x01};
    a->ecus = [&seed](uint32_t, const Bytes& q) -> Bytes {
        return q == seed ? Bytes{0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08}
                         : in_session(q);
    };
    a->until(1000 * kMs);
    ecuio_talk_request q;
    q.pdu = seed.data();
    q.len = 2;
    q.echo = 2;
    q.kind = ecuio_talk_kind::seed;
    q.window_us = 3000 * kMs;
    ASSERT_EQ(ecuio_engine_offer_talk(&a->e, 1, q, a->now),
              ecuio_talk_offer::taken);
    ecuio_talk_result settled;
    for (int i = 0;
         i < 100 && !ecuio_engine_talk_take(a->e.chans[1].talk, &settled);
         ++i) {
        a->pass();
    }
    ASSERT_EQ(settled.end, ecuio_talk_end::answered);
    const ecuio_engine_channel& waiting = a->e.chans[1];
    const int64_t until = waiting.key.until_us;
    ASSERT_GT(until, a->now + 2000 * kMs);
    EXPECT_EQ(ecuio_engine_turn_next_us(&a->e, waiting), until)
        << "not its keep-alive's time, " << ECUIO_ENGINE_KEEPALIVE_MIN_US
        << " after the seed";
    /* with its key offered the channel asks at once: the window holds
     * everything but the key */
    ecuio_engine_channel keyed = waiting;
    const Bytes key = {0x27, 0x02, 0x11, 0x22, 0x33, 0x44};
    ecuio_talk_request k;
    k.pdu = key.data();
    k.len = 6;
    k.echo = 2;
    k.kind = ecuio_talk_kind::key;
    ASSERT_EQ(ecuio_engine_talk(keyed.talk, keyed.key, 7, k, a->now),
              ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_turn_next_us(&a->e, keyed), keyed.next_send_us);
    EXPECT_LT(keyed.next_send_us, until);

    const size_t from = a->sent.size();
    std::set<int64_t> turns_begun;
    while (a->now < until - 2 * a->exchange_us) {
        a->pass();
        turns_begun.insert(a->turns.since_us);
    }
    const size_t in_window = a->sent.size() - from;
    ASSERT_GE(in_window, 20u);
    for (size_t i = from; i < a->sent.size(); ++i) {
        ASSERT_EQ(a->sent[i].c, 0u) << "request " << i;
    }
    EXPECT_LE(turns_begun.size(), in_window / 4 + 2)
        << "channel 0 kept each turn for its four rows";

    a->until(until + 4 * a->exchange_us);
    const Sent* next = nullptr;
    for (size_t i = from; i < a->sent.size() && next == nullptr; ++i) {
        next = a->sent[i].c == 1 ? &a->sent[i] : nullptr;
    }
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->bytes, (Bytes{0x3E, 0x00}));
    EXPECT_GE(next->at_us, until);
    EXPECT_LE(next->at_us, until + 2 * a->exchange_us)
        << "channel 1's keep-alive ended channel 0's turn";
    EXPECT_EQ(a->e.chans[1].session_lost, 0u);
}

/* A held channel is passed over, and the turn it held moves on */
TEST(RequestTurnsTest, AHeldChannelIsPassedOver)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 1, 0x80);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    a->asks(2);
    a->held = 1u << 0;
    a->asks(4);
    const std::vector<uint32_t> want = {0, 0, 1, 1};
    EXPECT_EQ(a->channels(), want);
    a->held = 0;
    a->asks(8);
    EXPECT_EQ(a->sent[4].c, 0u);
}

/* A ?gap= longer than an exchange: the turn hands on rather than wait its
 * floor out while another channel can ask, so each channel is asked as
 * often as it was turn by turn, about once a floor */
TEST(RequestTurnsTest, ALongFloorHandsTheTurnOn)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 4, 0x80);
    a->e.gap_us = 250 * kMs;
    a->exchange_us = 120 * kMs;
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    a->until(3000 * kMs);
    int asked[2] = {};
    for (const Sent& x : a->sent) {
        ++asked[x.c];
    }
    EXPECT_GE(asked[0], 11) << "an idle adapter: the turn waited its floor";
    EXPECT_GE(asked[1], 11);
}

/* A walking channel waits out a floor longer than its exchanges, so its
 * second holds its own bitmaps, one a floor (100 ms): handed on at each
 * floor, two busy channels' exchanges (60 ms each) would come between
 * them, and eight wouldn't fit in the second */
TEST(RequestTurnsTest, AWalkWaitsOutALongFloor)
{
    auto a = adapter(3);
    dids(&a->e, 0, 30, 0x40);
    add(&a->e, 1, {0x01, 0x0C});
    dids(&a->e, 2, 30, 0x80);
    a->e.gap_us = 100 * kMs;
    a->exchange_us = 60 * kMs;
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    ecuio_engine_probe(&a->e);
    a->until(4000 * kMs);
    EXPECT_TRUE(a->e.chans[1].walked) << "the walk stopped short";
    EXPECT_EQ(a->e.chans[1].ranges, 0xFFu);
}

/* A recovery while a bitmap is held pending gives the walk a new second:
 * the bitmap's late answer, past the old second, carries the walk on */
TEST(RequestTurnsTest, ARecoveryGivesAPendingBitmapANewSecond)
{
    auto a = adapter(1);
    add(&a->e, 0, {0x01, 0x0C});
    ecuio_engine_probe(&a->e);
    ASSERT_EQ(ecuio_engine_due(&a->e, 0, 0), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&a->e, 0, ECUIO_ENGINE_ONCE, true, 0);
    const uint8_t pending[3] = {0x7F, 0x01, ECUIO_NRC_RESPONSE_PENDING};
    ASSERT_EQ(ecuio_engine_on_frame(&a->e, 0, pending, 3, 10 * kMs, nullptr),
              ecuio_reply::pending);
    ecuio_engine_probe(&a->e); /* the driver's recovery, at 1.5 s */
    const Bytes r = answer({0x01, 0x00});
    const int64_t late = 2000 * kMs;
    ASSERT_EQ(ecuio_engine_on_frame(&a->e, 0, r.data(),
                                    static_cast<uint16_t>(r.size()), late,
                                    nullptr),
              ecuio_reply::positive);
    EXPECT_TRUE(a->e.chans[0].walking) << "the old second ended the walk";
    ASSERT_EQ(ecuio_engine_due(&a->e, 0, late + ECUIO_ENGINE_GAP_US),
              ECUIO_ENGINE_ONCE);
    EXPECT_EQ(a->e.chans[0].once.bytes[1], 0x20u);
}

/* The same for a bitmap whose wait ends after another PID's answer came
 * while it waited: after a recovery it is asked once more */
TEST(RequestTurnsTest, ARecoveryLetsAStrayBitmapBeAskedAgain)
{
    auto a = adapter(1);
    add(&a->e, 0, {0x01, 0x0C});
    ecuio_engine_probe(&a->e);
    ASSERT_EQ(ecuio_engine_due(&a->e, 0, 0), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&a->e, 0, ECUIO_ENGINE_ONCE, true, 0);
    const uint8_t pending[3] = {0x7F, 0x01, ECUIO_NRC_RESPONSE_PENDING};
    ASSERT_EQ(ecuio_engine_on_frame(&a->e, 0, pending, 3, 10 * kMs, nullptr),
              ecuio_reply::pending);
    const uint8_t stray[4] = {0x41, 0x0C, 0x1F, 0x40};
    (void)ecuio_engine_on_frame(&a->e, 0, stray, 4, 20 * kMs, nullptr);
    ASSERT_TRUE(a->e.chans[0].walk_stray);
    ecuio_engine_probe(&a->e); /* the driver's recovery */
    ASSERT_TRUE(ecuio_engine_expire(&a->e, 0, 6000 * kMs));
    EXPECT_TRUE(a->e.chans[0].walking) << "the old second ended the walk";
    EXPECT_TRUE(a->e.chans[0].walk_reasked);
}

/* A scanned DID whose write keeps failing holds the slot ahead of the
 * walk: that isn't the walk being asked, so the turn still ends at its
 * second and the other channel asks */
TEST(RequestTurnsTest, AOneShotAheadOfTheWalkEndsItsTurnAtASecond)
{
    auto a = adapter(2);
    add(&a->e, 0, {0x01, 0x0C});
    ASSERT_TRUE(ecuio_engine_scan_add(&a->e, 0, 0xD100));
    dids(&a->e, 1, 1, 0x80);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    a->unsent = [](uint32_t c, const Bytes& q) {
        return c == 0 && q.size() == 3 && q[0] == 0x22 && q[1] == 0xD1;
    };
    for (int i = 0; i < 50 && !a->e.chans[0].scanning; ++i) {
        a->pass();
    }
    ASSERT_TRUE(a->e.chans[0].scanning);
    ecuio_engine_probe(&a->e);
    ASSERT_TRUE(a->e.chans[0].walking);
    const size_t before = a->sent.size();
    a->until(a->now + 3000 * kMs);
    int other = 0;
    for (size_t i = before; i < a->sent.size(); ++i) {
        other += a->sent[i].c == 1 ? 1 : 0;
    }
    EXPECT_GE(other, 2) << "the failing write kept the turn";
}

/* Rule 4 passes over a held channel: its keep-alive falling due (and its
 * session lapsing) ends no other channel's turn, so those still ask each
 * of their rows in a turn */
TEST(RequestTurnsTest, AHeldChannelsKeepAliveEndsNoTurn)
{
    auto a = adapter(3);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 1, 0x40);
    dids(&a->e, 2, 4, 0x80);
    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MIN_US));
    a->ecus = [](uint32_t, const Bytes& q) { return in_session(q); };
    a->until(1000 * kMs);
    ASSERT_EQ(a->e.chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    a->held = 1u << 1;
    const size_t from = a->sent.size();
    a->until(a->now + 3000 * kMs);
    const auto r = runs(a->channels(from));
    ASSERT_GE(r.size(), 4u);
    for (size_t i = 1; i + 1 < r.size(); ++i) {
        EXPECT_NE(r[i].first, 1u);
        EXPECT_GE(r[i].second, 4)
            << "run " << i << " of channel " << r[i].first;
    }
}

/* Rule 4 passes over a channel whose request is out: a keep-alive that
 * falls due while it waits on a pending reply ends no other turn */
TEST(RequestTurnsTest, AKeepAliveDueWithARequestOutEndsNoTurn)
{
    auto a = adapter(3);
    dids(&a->e, 0, 4);
    dids(&a->e, 1, 1, 0x40);
    dids(&a->e, 2, 4, 0x80);
    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MAX_US));
    bool held_once = false;
    a->ecus = [&held_once](uint32_t c, const Bytes& q) -> Bytes {
        if (c == 1 && q[0] == 0x22 && !held_once) {
            held_once = true;
            return {0x7F, 0x22, ECUIO_NRC_RESPONSE_PENDING};
        }
        return in_session(q);
    };
    a->until(2000 * kMs);
    ASSERT_TRUE(held_once);
    int64_t asked = -1;
    for (const Sent& x : a->sent) {
        if (x.c == 1 && x.bytes[0] == 0x22) {
            asked = x.at_us;
        }
    }
    a->until(asked + 6000 * kMs);
    /* the keep-alive is due 4.5 s on, the pending wait ends 5.05 s on */
    std::vector<uint32_t> window;
    for (const Sent& x : a->sent) {
        if (x.at_us > asked + ECUIO_ENGINE_KEEPALIVE_MAX_US &&
            x.at_us < asked + 5000 * kMs) {
            window.push_back(x.c);
        }
    }
    ASSERT_GE(window.size(), 4u);
    EXPECT_LE(runs(window).size(), 3u) << "the turns ended at each exchange";
}

/* The last module's bits are its own: held, channel 31 is passed over,
 * the turn it holds too, and a channel that shares its low bits (15)
 * still asks; its walk away from the turn is bit 31 alone */
TEST(RequestTurnsTest, TheLastModulesBitsAreItsOwn)
{
    constexpr uint32_t last = ECUIO_MAX_CHANNELS - 1;
    auto a = adapter(ECUIO_MAX_CHANNELS);
    dids(&a->e, 15, 4);
    dids(&a->e, last, 4, 0x40);
    a->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    for (int i = 0; i < 1000 && (a->sent.empty() || a->sent.back().c != last);
         ++i) {
        a->pass();
    }
    ASSERT_EQ(a->sent.back().c, last) << "channel 31 holds the turn";
    a->held = 1u << last;
    const size_t from = a->sent.size();
    a->asks(from + 12);
    for (const uint32_t c : a->channels(from)) {
        EXPECT_EQ(c, 15u) << "a held channel 31 asks nothing";
    }

    auto w = adapter(ECUIO_MAX_CHANNELS);
    add(&w->e, last, {0x01, 0x0C});
    ecuio_engine_probe(&w->e);
    ecuio_engine_turns t;
    ecuio_engine_channel& ch = w->e.chans[last];
    int64_t now = 100 * kMs;
    ASSERT_EQ(ecuio_engine_turn_due(&w->e, t, last, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&w->e, last, ECUIO_ENGINE_ONCE, true, now);
    const Bytes r = answer({0x01, 0x00});
    now += 90 * kMs;
    ASSERT_EQ(ecuio_engine_on_frame(&w->e, last, r.data(),
                                    static_cast<uint16_t>(r.size()), now,
                                    nullptr),
              ecuio_reply::positive);
    ASSERT_TRUE(ch.walking);
    ecuio_engine_turn_left(&w->e, t, last, now);
    EXPECT_EQ(t.walk_away, 1u << last);
    const int64_t until = ch.walk_until_us;
    ch.next_send_us = now + 400 * kMs;
    EXPECT_EQ(ecuio_engine_turn_due(&w->e, t, last, now + 100 * kMs),
              ECUIO_ENGINE_NONE);
    EXPECT_EQ(ch.walk_until_us, until + 100 * kMs) << "its second stands still";
    ASSERT_EQ(ecuio_engine_turn_due(&w->e, t, last, now + 400 * kMs),
              ECUIO_ENGINE_ONCE);
    EXPECT_EQ(t.walk_away, 0u) << "it asks: away no more";
}

/* A held channel past bit 15 is its own: channel 16 held neither cuts
 * channel 0's floor short nor has its session request end a turn, where
 * channel 0's bit would say it isn't held */
TEST(RequestTurnsTest, AHeldChannelPastBitFifteenIsItsOwn)
{
    constexpr uint32_t n = 17;
    auto a = adapter(n);
    dids(&a->e, 0, 4);
    dids(&a->e, 16, 4, 0x80);
    const int64_t now = 100 * kMs;
    a->e.chans[0].next_send_us = now + 3 * ECUIO_ENGINE_GAP_US;
    a->e.chans[16].next_send_us = now + ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_turn_floor_us(&a->e, 0, 0, now), 0)
        << "channel 16 asks first";
    EXPECT_EQ(ecuio_engine_turn_floor_us(&a->e, 0, 1u << 16, now),
              3 * ECUIO_ENGINE_GAP_US)
        << "held, channel 16 asks nothing";

    ASSERT_TRUE(
        ecuio_engine_session(&a->e, 0x03, ECUIO_ENGINE_KEEPALIVE_MIN_US));
    for (uint32_t c = 0; c < 16; ++c) {
        a->e.chans[c].session_step = ECUIO_SESSION_STATE_REFUSED;
    }
    const int64_t later = now + ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(ecuio_engine_turn_session(&a->e, 0, n, later), 16u);
    EXPECT_EQ(ecuio_engine_turn_session(&a->e, 1u << 16, n, later), n)
        << "held, its session request is no one's turn";
}
