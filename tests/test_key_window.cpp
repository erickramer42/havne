// File/layer: tests/test_key_window.cpp | test_obd2, Qt-free
// Testing: UNIT: KeyWindowTest (the key's window, core/src/request_talk.h
//          and the request engine's parts, ADR-0066 section 2.4, with the
//          time passed in: what a channel sends between a seed and its
//          key, and each way the window ends)

#include "engine_talk_support.h"

using namespace engine_talk_test;

TEST(KeyWindowTest, ASeedThenItsEighteenByteKeyUnlockThroughTheEngine)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    seeded(&e, &ecu, &now);
    /* The key: 18 bytes out, and 67 04 answers it, two bytes */
    const Bytes key = key_for(0x03);
    ASSERT_EQ(key.size(), 18u);
    ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
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

/* While a key's window is open the channel sends nothing but the key:
 * not tester present when it is due, not the walk's bitmap, a clear's
 * step, an identification item, a scanned DID or the rotation. The ECU
 * is strict, so one request in between would spend the seed. All of
 * them go once the key has. */
TEST(KeyWindowTest, NothingGoesBetweenASeedAndItsKey)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    const uint8_t pid[2] = {0x01, 0x0C}; /* a Service 01 row: a probe walks */
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    /* a diagnostics report read to its end, so a clear can start */
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    (void)steps(&e, &now, std::ref(ecu), 24);
    ecuio_channel_diagnostics_t report;
    ecuio_engine_diagnostics(&e, 0, &report);
    ASSERT_NE(report.flags & ECUIO_DIAG_COMPLETE, 0u);

    seeded(&e, &ecu, &now, 0x03, ECUIO_KEY_WINDOW_MAX_US);
    const int64_t until = e.chans[0].key.until_us;
    /* everything the channel could send falls due inside the window */
    ecuio_engine_probe(&e);
    ASSERT_TRUE(e.chans[0].walking);
    ecuio_engine_identify(&e);
    ASSERT_GT(e.chans[0].ident_pending, 0u);
    ASSERT_TRUE(ecuio_engine_clear(&e, 0, report.generation));
    for (uint16_t did = 0xD220; did < 0xD224; ++did) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, did));
    }
    now += e.keepalive_us + 500 * kMs;
    ASSERT_GE(now - e.chans[0].last_sent_us, e.keepalive_us)
        << "tester present is due";
    ASSERT_LT(now + 40 * ECUIO_ENGINE_GAP_US, until);
    for (int i = 0; i < 40; ++i) {
        ASSERT_EQ(step(&e, 0, &now, std::ref(ecu)), Bytes{}) << i;
    }
    EXPECT_TRUE(ecu.heard.empty());
    EXPECT_TRUE(ecuio_engine_key_awaited(&e, e.chans[0], now));

    const Bytes key = key_for(0x03);
    ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key) << "the very next request";
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(ecu.security.unlocked, 0x03) << "the strict ECU heard no other";
    EXPECT_FALSE(ecuio_engine_key_awaited(&e, e.chans[0], now));

    /* and then each of them, the key having kept the session */
    const std::vector<Bytes> after = steps(&e, &now, std::ref(ecu), 60);
    EXPECT_NE(after[0], kKeep)
        << "the key was a request: the session needs no other yet";
    EXPECT_GE(index_of(after, Bytes{0x01, 0x00}), 0) << "the walk's bitmap";
    EXPECT_GE(index_of(after, kSpeed), 0) << "the clear's first step";
    EXPECT_GE(index_of(after, Bytes{0x09, 0x02}), 0) << "identification";
    EXPECT_GE(index_of(after, Bytes{0x22, 0xD2, 0x20}), 0) << "the scan";
    EXPECT_GE(index_of(after, kRow), 0) << "the rotation";
}

/* The window's own end: window_us after the seed's reply. Past it no key
 * is taken, and tester present, held back meanwhile, is the next
 * request. */
TEST(KeyWindowTest, TheWindowEndsAtItsTimeAndTesterPresentFollows)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    const int64_t window = 2500 * kMs;
    seeded(&e, &ecu, &now, 0x03, window);
    const int64_t until = e.chans[0].key.until_us;
    EXPECT_EQ(until, now - ECUIO_ENGINE_GAP_US + window)
        << "from the seed's reply";
    now = until - 1;
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    EXPECT_TRUE(ecuio_engine_key_awaited(&e, e.chans[0], now));
    now = until;
    EXPECT_FALSE(ecuio_engine_key_awaited(&e, e.chans[0], now));
    EXPECT_EQ(offer_key(&e, now, key_for(0x03)), ecuio_talk_offer::refused)
        << "a key for a seed that lapsed";
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kKeep)
        << "two seconds and more with nothing sent";
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[0].session_lost, 0u) << "inside S3: the session held";
}

/* until_us is the lesser of the seed's own window from its reply and the
 * session's bound from its send, S3 less a second: a key sent later
 * would reach an ECU whose session had run out. */
TEST(KeyWindowTest, TheWindowIsTheLesserOfTheSeedsOwnAndTheSessionsBound)
{
    const Bytes seed_reply = {0x67, 0x03, 0x11, 0x92, 0x23, 0xA4};
    struct Row {
        int64_t window_us;
        int64_t reply_after_us; /* with 7F 27 78 first when over the wait */
        int64_t until_after_send_us; /* 0: closed at the reply */
    };
    const Row rows[] = {
        {50 * kMs, 4 * kMs, 54 * kMs},
        {1000 * kMs, 4 * kMs, 1004 * kMs},
        {4000 * kMs, 4 * kMs, 4000 * kMs},
        {4000 * kMs, 0, 4000 * kMs},
        {1000 * kMs, 2999 * kMs, 3999 * kMs},
        {1000 * kMs, 3000 * kMs, 4000 * kMs},
        {1000 * kMs, 3500 * kMs, 4000 * kMs},
        {50 * kMs, 3999999, 4000 * kMs},
        {50 * kMs, 4000 * kMs, 0},
        {1000 * kMs, 4200 * kMs, 0},
    };
    EXPECT_EQ(ECUIO_KEY_WINDOW_SESSION_US, ECUIO_ENGINE_S3_US - 1000 * kMs);
    for (const Row& row : rows) {
        static ecuio_request_engine e;
        engine(&e, 0);
        const int64_t sent = 700 * kMs;
        ASSERT_EQ(offer_seed(&e, sent, 0x03, row.window_us),
                  ecuio_talk_offer::taken);
        ASSERT_EQ(ecuio_engine_due(&e, 0, sent), ECUIO_ENGINE_ONCE);
        EXPECT_EQ(e.chans[0].key.until_us, 0) << "not before it is sent";
        ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, sent);
        EXPECT_EQ(e.chans[0].key.until_us, sent + ECUIO_KEY_WINDOW_SESSION_US);
        if (row.reply_after_us > ECUIO_ENGINE_WAIT_US) {
            const Bytes pending = {0x7F, 0x27, 0x78};
            ASSERT_EQ(ecuio_engine_on_frame(&e, 0, pending.data(), 3,
                                            sent + 50 * kMs, nullptr),
                      ecuio_reply::pending);
        }
        const int64_t at = sent + row.reply_after_us;
        ASSERT_EQ(ecuio_engine_on_frame(
                      &e, 0, seed_reply.data(),
                      static_cast<uint16_t>(seed_reply.size()), at, nullptr),
                  ecuio_reply::positive);
        ASSERT_EQ(taken(&e), ecuio_talk_end::answered);
        const int64_t want =
            row.until_after_send_us == 0 ? 0 : sent + row.until_after_send_us;
        EXPECT_EQ(e.chans[0].key.until_us, want)
            << row.window_us << " " << row.reply_after_us;
        EXPECT_EQ(offer_key(&e, at, key_for(0x03)),
                  want != 0 ? ecuio_talk_offer::taken
                            : ecuio_talk_offer::refused)
            << row.window_us << " " << row.reply_after_us;
    }
}

/* A seed that ends any way but answered leaves no window: no key is
 * taken, and the channel reads on at once. */
TEST(KeyWindowTest, ASeedRefusedUnansweredOrUnsentOpensNoWindow)
{
    const Bytes seed = {0x27, 0x03};
    const Bytes key = key_for(0x03);
    {
        static ecuio_request_engine e; /* refused: a lockout's delay runs */
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        const Responder delayed = [&](const Bytes& q) -> Bytes {
            return q == seed ? Bytes{0x7F, 0x27, 0x37} : ecu(q);
        };
        ASSERT_EQ(offer_seed(&e, now), ecuio_talk_offer::taken);
        (void)until_settled(&e, &now, delayed);
        uint8_t nrc = 0;
        ASSERT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
        EXPECT_EQ(nrc, 0x37);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused);
        EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kRow) << "it reads on";
    }
    {
        static ecuio_request_engine e; /* unanswered */
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        ecu.silent = seed;
        ASSERT_EQ(offer_seed(&e, now), ecuio_talk_offer::taken);
        (void)until_settled(&e, &now, std::ref(ecu));
        ASSERT_EQ(taken(&e), ecuio_talk_end::unanswered);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused);
        EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kRow);
    }
    {
        static ecuio_request_engine e; /* its write failed */
        engine(&e, 0);
        int64_t now = 0;
        ASSERT_EQ(offer_seed(&e, now), ecuio_talk_offer::taken);
        ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
        ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, false, now);
        ASSERT_EQ(taken(&e), ecuio_talk_end::unsent);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused);
    }
    {
        /* sent, then taken back (SocketCAN's unechoed frame): the window
         * its first write opened ends with it */
        static ecuio_request_engine e;
        engine(&e, 0);
        int64_t now = 0;
        ASSERT_EQ(offer_seed(&e, now), ecuio_talk_offer::taken);
        ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
        ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
        ASSERT_NE(e.chans[0].key.until_us, 0);
        ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, false, now + 25 * kMs);
        ASSERT_EQ(taken(&e), ecuio_talk_end::unsent);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
    }
}

/* The key's send ends the window, whatever becomes of the key: answered,
 * refused, unanswered or its write failed, a second key for the same
 * seed is refused at the door. */
TEST(KeyWindowTest, AKeysSendEndsTheWindowWhateverBecomesOfIt)
{
    const Bytes key = key_for(0x03);
    Bytes wrong = key;
    wrong[5] ^= 0x01;
    {
        static ecuio_request_engine e; /* refused: a wrong key */
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        seeded(&e, &ecu, &now);
        ASSERT_EQ(offer_key(&e, now, wrong), ecuio_talk_offer::taken);
        ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
        ASSERT_TRUE(ecuio_key_window_open(e.chans[0].key, now))
            << "chosen, not yet on the bus";
        ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
        EXPECT_EQ(e.chans[0].key.until_us, 0) << "sent";
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused)
            << "refused, not busy: the door judges the window first";
        const Bytes reply = ecu(wrong);
        (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()),
                                    now + kMs, nullptr);
        uint8_t nrc = 0;
        ASSERT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
        EXPECT_EQ(nrc, ECUIO_SIM_NRC_INVALID_KEY);
        EXPECT_EQ(offer_key(&e, now + kMs, key), ecuio_talk_offer::refused)
            << "the right key needs a new seed";
        EXPECT_EQ(count_of(ecu.heard, wrong), 1u);
    }
    {
        static ecuio_request_engine e; /* unanswered */
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        seeded(&e, &ecu, &now);
        ecu.silent = key;
        ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
        (void)until_settled(&e, &now, std::ref(ecu));
        ASSERT_EQ(taken(&e), ecuio_talk_end::unanswered);
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused);
    }
    {
        static ecuio_request_engine e; /* its write failed */
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        seeded(&e, &ecu, &now);
        ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
        ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
        ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, false, now);
        ASSERT_EQ(taken(&e), ecuio_talk_end::unsent);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
        now += ECUIO_ENGINE_GAP_US;
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused)
            << "part of it may have reached the ECU";
        (void)steps(&e, &now, std::ref(ecu), 6);
        EXPECT_EQ(count_of(ecu.heard, key), 0u) << "and it never goes again";
    }
}

/* A session that is entered again took the seed with it: the window
 * ends, a key offered meanwhile settles unsent ahead of the entry, and
 * none is taken after. */
TEST(KeyWindowTest, ASessionEnteredAgainEndsTheWindow)
{
    const Bytes key = key_for(0x03);
    {
        /* a link's recovery asks 10 03 again (ecuio_engine_rescan) */
        static ecuio_request_engine e;
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        seeded(&e, &ecu, &now);
        ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
        ecuio_engine_rescan(&e);
        EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
        EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter)
            << "the entry, not a key for a seed the ECU dropped";
        EXPECT_EQ(taken(&e), ecuio_talk_end::unsent);
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused);
        (void)steps(&e, &now, std::ref(ecu), 6);
        EXPECT_EQ(count_of(ecu.heard, key), 0u);
    }
    {
        /* the seed itself answered 7F: the ECU had left the session */
        static ecuio_request_engine e;
        Ecu ecu;
        int64_t now = 0;
        entered(&e, &ecu, &now);
        ecu.extended = false;
        ASSERT_EQ(offer_seed(&e, now), ecuio_talk_offer::taken);
        (void)until_settled(&e, &now, std::ref(ecu));
        uint8_t nrc = 0;
        ASSERT_EQ(taken(&e, &nrc), ecuio_talk_end::refused);
        EXPECT_EQ(nrc, 0x7F);
        EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
        EXPECT_EQ(e.chans[0].key.until_us, 0);
        EXPECT_EQ(offer_key(&e, now, key), ecuio_talk_offer::refused);
    }
    {
        /* every step but entered ends it, and entered leaves it */
        ecuio_engine_channel ch;
        const std::initializer_list<uint8_t> others = {
            ECUIO_SESSION_STATE_NOT_ASKED, ECUIO_SESSION_STATE_ENTERING,
            ECUIO_SESSION_STATE_REFUSED, ECUIO_SESSION_STATE_UNANSWERED,
            ECUIO_SESSION_STATE_ABANDONED};
        for (const uint8_t step_to : others) {
            ch.key.until_us = 1000;
            ecuio_engine_session_step(ch, step_to);
            EXPECT_EQ(ch.session_step, step_to);
            EXPECT_EQ(ch.key.until_us, 0) << int{step_to};
        }
        ch.key.until_us = 1000;
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ENTERED);
        EXPECT_EQ(ch.key.until_us, 1000);
    }
}

/* The driver ends every window while its link isn't OK, and the caller
 * its own channel's when no key follows a seed. */
TEST(KeyWindowTest, TheDriverEndsEveryWindowAndTheCallerItsOwn)
{
    static ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 3));
    e.pdu_max = ECUIO_ENGINE_PDU_MAX;
    const auto open_all = [&](int64_t now) {
        for (uint32_t c = 0; c < 2; ++c) {
            ASSERT_EQ(offer_seed(&e, now, 0x01, kWindow, c),
                      ecuio_talk_offer::taken);
            ASSERT_EQ(ecuio_engine_due(&e, c, now), ECUIO_ENGINE_ONCE);
            ecuio_engine_sent(&e, c, ECUIO_ENGINE_ONCE, true, now);
            const Bytes reply = {0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08};
            ASSERT_EQ(ecuio_engine_on_frame(&e, c, reply.data(), 6, now + kMs,
                                            nullptr),
                      ecuio_reply::positive);
            ecuio_talk_result r;
            ASSERT_TRUE(ecuio_engine_talk_take(e.chans[c].talk, &r));
            ASSERT_TRUE(ecuio_key_window_open(e.chans[c].key, now + kMs));
        }
    };
    const Bytes key = key_for(0x01);
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(open_all(now));
    now += 100 * kMs;
    ecuio_engine_key_window_end(&e, 1);
    EXPECT_EQ(offer_key(&e, now, key, 1), ecuio_talk_offer::refused);
    EXPECT_TRUE(ecuio_key_window_open(e.chans[0].key, now))
        << "the other channel's stands";
    EXPECT_EQ(e.chans[2].key.until_us, 0);

    now += 2000 * kMs;
    ASSERT_NO_FATAL_FAILURE(open_all(now));
    now += 100 * kMs;
    ASSERT_EQ(offer_key(&e, now, key, 0), ecuio_talk_offer::taken);
    ecuio_engine_key_windows_end(&e);
    EXPECT_EQ(e.chans[0].key.until_us, 0);
    EXPECT_EQ(e.chans[1].key.until_us, 0);
    EXPECT_EQ(offer_key(&e, now, key, 1), ecuio_talk_offer::refused);
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE)
        << "the key offered before the link failed doesn't go";
    EXPECT_EQ(taken(&e), ecuio_talk_end::unsent);
}

/* A key offered in time that reaches its turn late (a long floor, a
 * worker held up) settles unsent: the engine sends no key for a seed
 * that has lapsed, whatever the caller did. */
TEST(KeyWindowTest, AKeyOfferedInTimeAndLateToItsTurnIsUnsent)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    seeded(&e, &ecu, &now, 0x03, 200 * kMs);
    const int64_t until = e.chans[0].key.until_us;
    const Bytes key = key_for(0x03);
    ASSERT_EQ(offer_key(&e, until - 1, key), ecuio_talk_offer::taken);
    EXPECT_TRUE(ecuio_engine_once_waiting(e.chans[0]));
    now = until;
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_NE(r, ECUIO_ENGINE_NONE) << "the channel reads on";
    EXPECT_EQ(wire(&e, 0, r), kRow);
    EXPECT_EQ(taken(&e), ecuio_talk_end::unsent);
    EXPECT_EQ(e.chans[0].key.until_us, 0);
    /* one microsecond earlier it goes */
    static ecuio_request_engine f;
    Ecu other;
    int64_t at = 0;
    entered(&f, &other, &at);
    seeded(&f, &other, &at, 0x03, 200 * kMs);
    const int64_t end = f.chans[0].key.until_us;
    ASSERT_EQ(offer_key(&f, end - 1, key), ecuio_talk_offer::taken);
    at = end - 1;
    ASSERT_EQ(ecuio_engine_due(&f, 0, at), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&f, 0, ECUIO_ENGINE_ONCE), key);
}

/* A key is the key of the seed whose window is open: the seed's
 * sub-function plus one. Any other is refused at the door and leaves
 * the window as it was. */
TEST(KeyWindowTest, AKeyIsItsOwnSeedsOrRefused)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    seeded(&e, &ecu, &now, 0x01);
    EXPECT_EQ(e.chans[0].key.seed, 0x01);
    EXPECT_EQ(offer_key(&e, now, key_for(0x03)), ecuio_talk_offer::refused)
        << "27 04 after the seed 27 01";
    EXPECT_EQ(offer_key(&e, now, Bytes{0x27, 0x06, 0x11, 0x22, 0x33, 0x44}),
              ecuio_talk_offer::refused);
    EXPECT_EQ(offer_key(&e, now, Bytes{0x27, 0x01, 0x11, 0x22, 0x33, 0x44}),
              ecuio_talk_offer::refused)
        << "the seed's own sub-function is no key's";
    EXPECT_TRUE(ecuio_key_window_open(e.chans[0].key, now));
    EXPECT_TRUE(ecu.heard.empty());
    const Bytes key = key_for(0x01);
    ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key);
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(ecu.security.unlocked, 0x01);

    /* and a level 03 seed takes 27 04 alone */
    seeded(&e, &ecu, &now, 0x03);
    EXPECT_EQ(e.chans[0].key.seed, 0x03);
    EXPECT_EQ(offer_key(&e, now, key_for(0x01)), ecuio_talk_offer::refused);
    ASSERT_EQ(offer_key(&e, now, key_for(0x03)), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key_for(0x03));
    EXPECT_EQ(ecu.security.unlocked, 0x03);
}

/* A functional channel asked into a session would have its keep-alive
 * held by another channel's window, past S3 by a long one: an engine
 * with one takes no seed. With no session on it, or a session and no
 * functional channel, a seed is taken. */
TEST(KeyWindowTest, NoSeedIsTakenWhileAFunctionalChannelHasASession)
{
    const auto make = [](ecuio_request_engine* e, bool functional,
                         bool session) {
        ASSERT_TRUE(ecuio_engine_init(e, 2));
        e->pdu_max = ECUIO_ENGINE_PDU_MAX;
        e->chans[1].functional = functional;
        if (session) {
            ASSERT_TRUE(
                ecuio_engine_session(e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
        }
    };
    static ecuio_request_engine both;
    make(&both, true, true);
    EXPECT_TRUE(ecuio_engine_functional_session(&both));
    EXPECT_EQ(offer_seed(&both, 0, 0x01, kWindow, 0),
              ecuio_talk_offer::refused);
    EXPECT_EQ(offer(&both, kRow, 3, 0), ecuio_talk_offer::taken)
        << "a plain talk is no seed";
    /* entered, refused or given up: asked into one all the same */
    for (const auto state :
         {ECUIO_SESSION_STATE_ENTERED, ECUIO_SESSION_STATE_REFUSED,
          ECUIO_SESSION_STATE_UNANSWERED, ECUIO_SESSION_STATE_ABANDONED}) {
        ecuio_engine_session_step(both.chans[1], static_cast<uint8_t>(state));
        EXPECT_TRUE(ecuio_engine_functional_session(&both)) << state;
    }

    static ecuio_request_engine unasked;
    make(&unasked, true, false);
    EXPECT_FALSE(ecuio_engine_functional_session(&unasked));
    EXPECT_EQ(offer_seed(&unasked, 0, 0x01, kWindow, 0),
              ecuio_talk_offer::taken);

    static ecuio_request_engine physical;
    make(&physical, false, true);
    EXPECT_FALSE(ecuio_engine_functional_session(&physical));
    EXPECT_EQ(offer_seed(&physical, 0, 0x01, kWindow, 0),
              ecuio_talk_offer::taken);
}

/* A new seed gives up the window of the one before, and a seed's window
 * runs from its first write only. */
TEST(KeyWindowTest, ANewSeedEndsTheWindowBeforeAndOpensItsOwnOnce)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    seeded(&e, &ecu, &now, 0x03, ECUIO_KEY_WINDOW_MAX_US);
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    ASSERT_EQ(offer_seed(&e, now, 0x01, 300 * kMs), ecuio_talk_offer::taken);
    EXPECT_EQ(e.chans[0].key.until_us, 0) << "no key follows the first seed";
    EXPECT_EQ(offer_key(&e, now, key_for(0x03)), ecuio_talk_offer::refused);
    /* the seed at its own turn, with the rotation */
    const std::vector<Bytes> sent = until_settled(&e, &now, std::ref(ecu));
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.back(), (Bytes{0x27, 0x01}));
    ASSERT_EQ(taken(&e), ecuio_talk_end::answered);
    EXPECT_EQ(e.chans[0].key.window_us, 300 * kMs) << "its own window";
    ASSERT_EQ(offer_key(&e, now, key_for(0x01)), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key_for(0x01));
    EXPECT_EQ(ecu.security.unlocked, 0x01);

    /* a seed's write counted twice opens the window once */
    static ecuio_request_engine f;
    engine(&f, 0);
    ASSERT_EQ(offer_seed(&f, 0), ecuio_talk_offer::taken);
    ASSERT_EQ(ecuio_engine_due(&f, 0, 0), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&f, 0, ECUIO_ENGINE_ONCE, true, 10 * kMs);
    ecuio_engine_sent(&f, 0, ECUIO_ENGINE_ONCE, true, 90 * kMs);
    EXPECT_EQ(f.chans[0].key.until_us, 10 * kMs + ECUIO_KEY_WINDOW_SESSION_US);
}

/* A functional address reaches the module that waits for its key too:
 * while any channel's window is open a functional channel sends nothing,
 * then reads on. It takes no seed, no key and no talk longer than a
 * frame. */
TEST(KeyWindowTest, AFunctionalChannelWaitsOutEveryWindow)
{
    static ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 3));
    e.pdu_max = ECUIO_ENGINE_PDU_MAX;
    e.chans[1].functional = true;
    const uint8_t row[3] = {0x22, 0x1E, 0x1C};
    for (uint32_t c = 0; c < 3; ++c) {
        ASSERT_EQ(ecuio_engine_add(&e, c, row, 3),
                  ecuio_engine_add_result::added);
    }
    int64_t now = 0;
    const auto seed_on = [&](uint32_t c, int64_t window_us) {
        ASSERT_EQ(offer_seed(&e, now, 0x01, window_us, c),
                  ecuio_talk_offer::taken);
        int32_t r = ECUIO_ENGINE_NONE;
        for (int i = 0; i < 4 && r != ECUIO_ENGINE_ONCE; ++i) {
            r = ecuio_engine_due(&e, c, now);
            ASSERT_NE(r, ECUIO_ENGINE_NONE);
            ecuio_engine_sent(&e, c, r, true, now);
            const Bytes reply = r == ECUIO_ENGINE_ONCE
                                    ? Bytes{0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08}
                                    : Bytes{0x62, 0x1E, 0x1C, 0x00};
            ASSERT_EQ(ecuio_engine_on_frame(&e, c, reply.data(),
                                            static_cast<uint16_t>(reply.size()),
                                            now, nullptr),
                      ecuio_reply::positive);
            now += ECUIO_ENGINE_GAP_US;
        }
        ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
        ecuio_talk_result settled;
        ASSERT_TRUE(ecuio_engine_talk_take(e.chans[c].talk, &settled));
    };
    ASSERT_EQ(ecuio_engine_due(&e, 1, now), 1) << "its row, with no window";
    ecuio_engine_sent(&e, 1, 1, false, now);
    now += ECUIO_ENGINE_GAP_US;

    ASSERT_NO_FATAL_FAILURE(seed_on(0, 300 * kMs));
    ASSERT_NO_FATAL_FAILURE(seed_on(2, 900 * kMs));
    const int64_t first = e.chans[0].key.until_us;
    const int64_t last = e.chans[2].key.until_us;
    ASSERT_LT(first, last);
    EXPECT_EQ(ecuio_engine_key_held_until(&e, e.chans[1]), last)
        << "the latest of every channel's";
    EXPECT_EQ(ecuio_engine_key_held_until(&e, e.chans[0]), first) << "its own";
    EXPECT_EQ(ecuio_engine_due(&e, 1, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_due(&e, 1, first), ECUIO_ENGINE_NONE)
        << "one module's window ended, another's stands";
    EXPECT_EQ(ecuio_engine_due(&e, 1, last - 1), ECUIO_ENGINE_NONE);
    EXPECT_GE(ecuio_engine_due(&e, 0, first), 0) << "the module reads on";
    EXPECT_EQ(ecuio_engine_due(&e, 1, last), 1) << "and it reads on";

    /* what a functional channel is offered */
    const Bytes seven = {0x22, 0xD3, 0x01, 0xD3, 0x02, 0xD3, 0x03};
    Bytes eight = seven;
    eight.push_back(0xD3);
    EXPECT_EQ(offer(&e, eight, 3, 1), ecuio_talk_offer::too_long);
    EXPECT_EQ(offer(&e, eight, 3, 0), ecuio_talk_offer::taken)
        << "a module's own address takes it";
    EXPECT_EQ(offer_seed(&e, last, 0x01, kWindow, 1),
              ecuio_talk_offer::refused);
    e.chans[1].key.until_us = last + kWindow; /* however one came to be */
    EXPECT_EQ(offer_key(&e, last, key_for(0x01), 1), ecuio_talk_offer::refused);
    e.chans[1].key.until_us = 0;
    EXPECT_EQ(offer(&e, seven, 3, 1), ecuio_talk_offer::taken);
    /* on a transport of single frames the bound is the same 7 */
    e.pdu_max = ECUIO_ENGINE_PDU_SINGLE;
    e.chans[1].talk = ecuio_engine_talk_state{};
    EXPECT_EQ(offer(&e, eight, 3, 1), ecuio_talk_offer::too_long);
    EXPECT_EQ(offer(&e, seven, 3, 1), ecuio_talk_offer::taken);
}

/* A functional channel's one-shot whose write failed stays the one-shot,
 * and waits out a window opened meanwhile as its other requests do. */
TEST(KeyWindowTest, AFunctionalChannelsUnsentOneShotWaitsOutAWindow)
{
    static ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 2));
    e.pdu_max = ECUIO_ENGINE_PDU_MAX;
    e.chans[1].functional = true;
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 1, 0xD310));
    const int64_t now = 0;
    ASSERT_EQ(ecuio_engine_due(&e, 1, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 1, ECUIO_ENGINE_ONCE, false, now);
    ASSERT_EQ(offer_seed(&e, now, 0x01, kWindow, 0), ecuio_talk_offer::taken);
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const int64_t until = e.chans[0].key.until_us;
    ASSERT_GT(until, now + ECUIO_ENGINE_GAP_US);
    EXPECT_EQ(ecuio_engine_due(&e, 1, now + ECUIO_ENGINE_GAP_US),
              ECUIO_ENGINE_NONE)
        << "its scan entry, taken before the window opened";
    EXPECT_EQ(ecuio_engine_due(&e, 1, until - 1), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_due(&e, 1, until), ECUIO_ENGINE_ONCE);
}

/* A clear asked for while a window is open starts after the key; a clear
 * under way when a seed is offered finishes first, the seed taking its
 * ordinary turn. */
TEST(KeyWindowTest, AClearNeverComesBetweenASeedAndItsKey)
{
    const Bytes key = key_for(0x03);
    const auto diagnosed = [](ecuio_request_engine* e, Ecu* ecu, int64_t* now,
                              uint32_t* generation) {
        entered(e, ecu, now);
        ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
        (void)steps(e, now, std::ref(*ecu), 24);
        ecuio_channel_diagnostics_t d;
        ecuio_engine_diagnostics(e, 0, &d);
        ASSERT_NE(d.flags & ECUIO_DIAG_COMPLETE, 0u);
        *generation = d.generation;
        ecu->heard.clear();
    };
    {
        static ecuio_request_engine e;
        Ecu ecu;
        int64_t now = 0;
        uint32_t generation = 0;
        ASSERT_NO_FATAL_FAILURE(diagnosed(&e, &ecu, &now, &generation));
        seeded(&e, &ecu, &now);
        ASSERT_TRUE(ecuio_engine_clear(&e, 0, generation));
        for (int i = 0; i < 6; ++i) {
            ASSERT_EQ(step(&e, 0, &now, std::ref(ecu)), Bytes{}) << i;
        }
        ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
        const std::vector<Bytes> sent = steps(&e, &now, std::ref(ecu), 8);
        ASSERT_FALSE(sent.empty());
        EXPECT_EQ(sent[0], key);
        EXPECT_GT(index_of(sent, kSpeed), 0) << "the clear, after the key";
        EXPECT_EQ(ecu.security.unlocked, 0x03);
    }
    {
        static ecuio_request_engine e;
        Ecu ecu;
        int64_t now = 0;
        uint32_t generation = 0;
        ASSERT_NO_FATAL_FAILURE(diagnosed(&e, &ecu, &now, &generation));
        ASSERT_TRUE(ecuio_engine_clear(&e, 0, generation));
        ASSERT_EQ(offer_seed(&e, now), ecuio_talk_offer::taken);
        const std::vector<Bytes> sent =
            until_settled(&e, &now, std::ref(ecu), 40);
        ASSERT_EQ(taken(&e), ecuio_talk_end::answered);
        const int speed = index_of(sent, kSpeed);
        const int clear = index_of(sent, Bytes{0x04});
        const int seed = index_of(sent, Bytes{0x27, 0x03});
        ASSERT_GE(speed, 0);
        EXPECT_GT(clear, speed);
        EXPECT_GT(seed, clear) << "the clear's steps, then the seed";
        ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
        EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key);
        EXPECT_EQ(ecu.security.unlocked, 0x03);
    }
}

/* A key goes ahead of the walk, and its service's reply to another
 * request is no stray answer to the walk's bitmap, which waits behind
 * it. A plain talk takes no such place: tester present and the bitmap
 * go before it. */
TEST(KeyWindowTest, AKeyGoesAheadOfTheWalkAndAPlainTalkBehindIt)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    entered(&e, &ecu, &now);
    const uint8_t pid[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    seeded(&e, &ecu, &now, 0x01);
    ecuio_engine_probe(&e);
    ASSERT_TRUE(e.chans[0].walking);
    const Bytes key = key_for(0x01);
    ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, r), key);
    ecuio_engine_sent(&e, 0, r, true, now);
    const Bytes other = {0x67, 0x04};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other.data(), 2, now, nullptr),
              ecuio_reply::none);
    EXPECT_FALSE(e.chans[0].walk_stray);
    const Bytes unlocked = ecu(key);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, unlocked.data(),
                                    static_cast<uint16_t>(unlocked.size()),
                                    now + kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    EXPECT_EQ(wire(&e, 0, ECUIO_ENGINE_ONCE), (Bytes{0x01, 0x00}))
        << "then the walk";

    /* A plain talk: the keep-alive and the walk's bitmap go before it */
    now += e.keepalive_us;
    ecuio_engine_probe(&e); /* the walk's budget again */
    ASSERT_TRUE(e.chans[0].walking);
    const Bytes read = {0x22, 0xD3, 0x01};
    ASSERT_EQ(offer(&e, read, 3), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kKeep);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), (Bytes{0x01, 0x00}));
    const std::vector<Bytes> sent = until_settled(&e, &now, std::ref(ecu));
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.back(), read);
    EXPECT_LE(sent.size(), 2u) << "then at the rotation's next turn";
}

/* A window needs no session of core's: an engine that enters none holds
 * the channel for the key just the same. */
TEST(KeyWindowTest, AWindowHoldsTheChannelWithNoSessionToo)
{
    static ecuio_request_engine e;
    engine(&e);
    Ecu ecu;
    int64_t now = 0;
    ecu.now = &now;
    ecu.extended = true; /* entered by another's hand */
    ecu.security.strict = true;
    seeded(&e, &ecu, &now, 0x01, 500 * kMs);
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(step(&e, 0, &now, std::ref(ecu)), Bytes{}) << i;
    }
    const Bytes key = key_for(0x01);
    ASSERT_EQ(offer_key(&e, now, key), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), key);
    EXPECT_EQ(ecu.security.unlocked, 0x01);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kRow);
}

/* A seed takes its ordinary turn, so a session entry that is due goes
 * before it. */
TEST(KeyWindowTest, ASeedStandsBehindASessionEntryThatIsDue)
{
    static ecuio_request_engine e;
    Ecu ecu;
    int64_t now = 0;
    engine(&e, 0);
    ecu.now = &now;
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    ASSERT_EQ(offer_seed(&e, now, 0x01), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter) << "not yet entered";
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), (Bytes{0x27, 0x01}));
    EXPECT_EQ(taken(&e), ecuio_talk_end::answered);
    /* S3 has passed with nothing sent: the session is gone, and the
     * window with it long before */
    now += ECUIO_ENGINE_S3_US;
    EXPECT_EQ(offer_key(&e, now, key_for(0x01)), ecuio_talk_offer::refused);
    ASSERT_EQ(offer_seed(&e, now, 0x01), ecuio_talk_offer::taken);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), kEnter);
    EXPECT_EQ(step(&e, 0, &now, std::ref(ecu)), (Bytes{0x27, 0x01}));
}

/* What a worker waiting on the next event wakes for inside a window:
 * its end, where tester present or the rotation is next, and never a
 * spin through it; a key offered, at once. */
TEST(KeyWindowTest, TheNextEventInsideAWindowIsItsEndOrAKeyOffered)
{
    /* in the session, with a row to ask */
    static ecuio_request_engine f;
    Ecu ecu;
    int64_t at = 0;
    entered(&f, &ecu, &at);
    seeded(&f, &ecu, &at, 0x03, 3000 * kMs);
    const int64_t until = f.chans[0].key.until_us;
    ASSERT_GT(until, f.chans[0].last_sent_us + f.keepalive_us)
        << "tester present falls due inside the window";
    EXPECT_EQ(ecuio_engine_next_event_us(&f, at), until - at);
    EXPECT_EQ(ecuio_engine_session_next_us(&f, f.chans[0]), until);
    at = until - 7;
    EXPECT_EQ(ecuio_engine_next_event_us(&f, at), 7);
    ASSERT_EQ(offer_key(&f, at, key_for(0x03)), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_next_event_us(&f, at), 0) << "the key goes now";
    ecuio_engine_key_window_end(&f, 0);
    EXPECT_EQ(ecuio_engine_next_event_us(&f, at), 0)
        << "and a key whose window ended is settled at once";
    EXPECT_EQ(step(&f, 0, &at, std::ref(ecu)), kKeep);

    /* no session and one row: the row waits for the window's end */
    static ecuio_request_engine g;
    engine(&g);
    Ecu other;
    int64_t t = 0;
    other.now = &t;
    other.extended = true;
    seeded(&g, &other, &t, 0x01, 400 * kMs);
    EXPECT_EQ(ecuio_engine_next_event_us(&g, t), g.chans[0].key.until_us - t);
    /* and with nothing at all to ask after it, the window's end still:
     * a driver counts quiet reads only when nothing is next, and a link
     * judged silent would end the window. Past the end, nothing. */
    static ecuio_request_engine h;
    engine(&h, 0);
    Ecu third;
    int64_t u = 0;
    third.now = &u;
    third.extended = true;
    seeded(&h, &third, &u, 0x01, 400 * kMs);
    const int64_t end = h.chans[0].key.until_us;
    ASSERT_GT(end, u);
    EXPECT_EQ(ecuio_engine_next_event_us(&h, u), end - u);
    EXPECT_EQ(ecuio_engine_next_event_us(&h, end - 1), 1);
    EXPECT_EQ(ecuio_engine_next_event_us(&h, end), INT64_MAX)
        << "no spin on a window that ended by its time";
    ASSERT_EQ(offer_key(&h, end - 1, key_for(0x01)), ecuio_talk_offer::taken);
    EXPECT_EQ(ecuio_engine_next_event_us(&h, end - 1), 0);
}

/* A functional channel's next event is the end of the latest window */
TEST(KeyWindowTest, AFunctionalChannelsNextEventIsTheLastWindowsEnd)
{
    static ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 2));
    e.pdu_max = ECUIO_ENGINE_PDU_MAX;
    e.chans[1].functional = true;
    const uint8_t row[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 1, row, 3), ecuio_engine_add_result::added);
    int64_t now = 100 * kMs;
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 0);
    ASSERT_EQ(offer_seed(&e, now, 0x01, 600 * kMs, 0), ecuio_talk_offer::taken);
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const Bytes reply = {0x67, 0x01, 0x5A, 0x17, 0xC3, 0x08};
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, reply.data(), 6, now, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 600 * kMs)
        << "the functional channel's row waits the window out";
    EXPECT_EQ(ecuio_engine_due(&e, 1, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now + 600 * kMs), 0);
    EXPECT_EQ(ecuio_engine_due(&e, 1, now + 600 * kMs), 0);
}
