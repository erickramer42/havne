// File/layer: tests/test_conversation.cpp | test_obd2, Qt-free
// Testing: UNIT: ConversationTest (one module's conversation over a
//          scripted table, the engine and the strict simulated ECU
//          stepped with the time passed in): the unlock and its ends

#include "conversation_support.h"

using namespace conversation_run;
using conv_unlock = ecuio_unlock_state;

namespace {

/* The key the simulated ECU wants for a level, as the wire carries it */
Bytes key_request(uint8_t lvl)
{
    return engine_talk_test::key_for(lvl);
}

/* The bytes an unlock's end hands the module: level, result, code */
Bytes unlock_end(uint8_t lvl, uint8_t result, uint8_t nrc = 0)
{
    return {lvl, result, nrc};
}

/* The word for lvl given and its seed on the wire; answered when the
 * case says, the step that hands the reply to the module not yet taken */
void seed_out(Bench& b, uint8_t lvl, bool answer = true)
{
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), lvl));
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    ASSERT_EQ(b.v->report.unlock, conv_unlock::seed_offered);
    for (int i = 0; i < 8; ++i) {
        const int32_t r = ecuio_engine_due(b.e.get(), 0, b.now);
        if (r == ECUIO_ENGINE_NONE) {
            b.now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const Bytes q = engine_talk_test::wire(b.e.get(), 0, r);
        ecuio_engine_sent(b.e.get(), 0, r, true, b.now);
        b.sent.push_back(q);
        const bool seed = q == Bytes{0x27, lvl};
        if (seed && !answer) {
            return;
        }
        b.reply_to(q);
        if (seed) {
            return;
        }
        b.now += ECUIO_ENGINE_GAP_US;
    }
    FAIL() << "no seed went";
}

/* Whether the channel's talk is free for the next offer */
bool talk_free(const Bench& b)
{
    const ecuio_engine_talk_state& t = b.e->chans[0].talk;
    return !t.offered && !t.out && t.result.end == ecuio_talk_end::none;
}

} // namespace

/* The caller's word is the whole unlock: the seed, the module's key
 * straight after it with nothing between, and the gated read the
 * module asks once the level is open */
TEST(ConversationTest, AnUnlockIsTheSeedTheModulesKeyAndTheGatedRead)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x03));
    EXPECT_EQ(b.v->report.unlock, conv_unlock::waiting);
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::unlocked);
    EXPECT_EQ(b.v->report.words_ended, 1u);
    EXPECT_TRUE(b.lvl(0x03).unlocked);
    EXPECT_EQ(b.lvl(0x03).failed, 0u);
    const int seed = engine_talk_test::index_of(b.sent, {0x27, 0x03});
    ASSERT_GE(seed, 0);
    ASSERT_LT(seed + 1, static_cast<int>(b.sent.size()));
    EXPECT_EQ(b.sent[seed + 1], key_request(0x03));
    EXPECT_EQ(b.ecu.security.unlocked, 0x03);

    const std::vector<Bytes> seeds = b.guest.of_kind(ECUIO_GUEST_SEED);
    ASSERT_EQ(seeds.size(), 1u);
    const ecuio_sim_security_level* l = ecuio_sim_security_find(0x03);
    Bytes want = {0x03};
    want.insert(want.end(), l->seed, l->seed + l->bytes);
    EXPECT_EQ(seeds[0], want);
    EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_UNLOCK),
              std::vector<Bytes>{unlock_end(0x03, ECUIO_GUEST_UNLOCKED)});

    b.guest.requests.push_back(kGated);
    b.passes(6);
    EXPECT_EQ(heard(b, kGated), 1u);
    const std::vector<Bytes> replies = b.guest.of_kind(ECUIO_GUEST_REPLY);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0], (Bytes{0x62, 0xD2, 0x10, 0x4B, 0x2D}));
    EXPECT_EQ(b.v->report.requests, 1u);

    /* the level stays open: a second word sends nothing */
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x03));
    const size_t out = b.sent.size();
    b.until_word_ended(4);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::already_unlocked);
    EXPECT_EQ(engine_talk_test::count_of(
                  std::vector<Bytes>(b.sent.begin() + out, b.sent.end()),
                  {0x27, 0x03}),
              0u);
}

/* A wrong key is one failed attempt with its code, and the level's delay
 * refuses the next word with what is left of it, sending nothing */
TEST(ConversationTest, AWrongKeyCountsAndItsDelayRefusesTheNextWord)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.guest.wrong_key = true;
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::failed);
    EXPECT_EQ(b.v->report.nrc, 0x35);
    EXPECT_EQ(b.lvl(0x01).failed, 1u);
    EXPECT_FALSE(b.lvl(0x01).unlocked);
    EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_UNLOCK),
              std::vector<Bytes>{unlock_end(0x01, ECUIO_GUEST_REFUSED, 0x35)});

    b.guest.wrong_key = false;
    const size_t out = b.sent.size();
    b.passes(2);
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended(4);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::delayed);
    EXPECT_GT(b.v->report.left_us, 9000 * kMs);
    EXPECT_LE(b.v->report.left_us, 10000 * kMs);
    EXPECT_EQ(b.v->report.words_ended, 2u);
    for (size_t k = out; k < b.sent.size(); ++k) {
        EXPECT_NE(b.sent[k][0], 0x27);
    }
}

/* Every end of a key that was offered but 67 counts: unanswered, and a
 * write that failed (it may have reached the module) */
TEST(ConversationTest, AKeyUnansweredOrUnsentCountsOneFailure)
{
    for (const bool unsent : {false, true}) {
        SCOPED_TRACE(unsent ? "unsent" : "unanswered");
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        (unsent ? b.fail_on : b.ecu.silent) = key_request(0x01);
        ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
        b.until_word_ended();
        EXPECT_EQ(b.v->report.unlock, conv_unlock::failed);
        EXPECT_EQ(b.lvl(0x01).failed, 1u);
        EXPECT_GT(b.lvl(0x01).wait_until_us, b.now);
    }
}

/* The module's time is bounded by its window: a key computed after the
 * window closed never touches the bus, and nothing is counted */
TEST(ConversationTest, AKeyPastItsWindowNeverGoesAndCountsNothing)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.guest.on_seed = [&b] { b.now += 1500 * kMs; };
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::ended);
    EXPECT_EQ(b.v->report.state, ecuio_conversation_state::talking);
    EXPECT_EQ(heard(b, key_request(0x01)), 0u);
    EXPECT_EQ(b.lvl(0x01).failed, 0u);
    EXPECT_EQ(b.lvl(0x01).wait_until_us, 0);
    EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_UNLOCK),
              std::vector<Bytes>{unlock_end(0x01, ECUIO_GUEST_ENDED)});
}

/* A key's window closed before the step that offers it (the link's
 * windows ended, the session left, the step itself late) is the
 * session's doing, not the module's: the attempt ends with nothing
 * counted and the module talks on */
TEST(ConversationTest, AKeyWhoseWindowClosedBeforeItsStepEndsTheAttempt)
{
    for (int cause = 0; cause < 3; ++cause) {
        SCOPED_TRACE(cause);
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        ASSERT_NO_FATAL_FAILURE(seed_out(b, 0x01));
        ASSERT_GT(b.e->chans[0].key.until_us, b.now);
        if (cause == 0) {
            ecuio_engine_key_windows_end(b.e.get());
        } else if (cause == 1) {
            ecuio_engine_session_step(b.e->chans[0],
                                      ECUIO_SESSION_STATE_ENTERING);
        } else {
            b.now = b.e->chans[0].key.until_us;
        }
        ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
        EXPECT_EQ(b.v->report.state, ecuio_conversation_state::talking);
        EXPECT_EQ(b.v->report.fault, ecuio_conversation_fault::none);
        EXPECT_EQ(b.v->report.unlock, conv_unlock::ended);
        EXPECT_EQ(b.v->report.words_ended, 1u);
        EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_SEED).size(), 1u);
        EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_UNLOCK),
                  std::vector<Bytes>{unlock_end(0x01, ECUIO_GUEST_ENDED)});
        b.passes(6);
        EXPECT_EQ(heard(b, key_request(0x01)), 0u);
        EXPECT_EQ(b.lvl(0x01).failed, 0u);
        EXPECT_EQ(b.lvl(0x01).wait_until_us, 0);
        EXPECT_EQ(b.v->report.state, ecuio_conversation_state::talking);
    }
}

/* A key the door refuses while its window is open is fault window */
TEST(ConversationTest, AKeyTheDoorRefusesInsideItsWindowIsAFault)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_NO_FATAL_FAILURE(seed_out(b, 0x01));
    b.e->chans[0].key.seed = 0x03; /* the window is another seed's */
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    EXPECT_EQ(b.v->report.state, ecuio_conversation_state::faulted);
    EXPECT_EQ(b.v->report.fault, ecuio_conversation_fault::window);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::ended);
    EXPECT_EQ(b.v->report.words_ended, 1u);
    EXPECT_EQ(b.e->chans[0].key.until_us, 0);
    b.passes(4);
    EXPECT_EQ(heard(b, key_request(0x01)), 0u);
    EXPECT_EQ(b.lvl(0x01).failed, 0u);
}

/* A seed of zeroes is the level open already: no key goes, the window
 * ends, nothing is counted */
TEST(ConversationTest, ASeedOfZeroesUnlocksWithNoKey)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.ecu.security.unlocked = 0x01;
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::unlocked);
    EXPECT_TRUE(b.lvl(0x01).unlocked);
    EXPECT_EQ(heard(b, {0x27, 0x01}), 1u);
    EXPECT_EQ(heard(b, key_request(0x01)), 0u);
    EXPECT_EQ(b.e->chans[0].key.until_us, 0);
    EXPECT_TRUE(b.guest.of_kind(ECUIO_GUEST_SEED).empty());
}

/* A seed refused ends the attempt with nothing counted; 7F 27 37 (the
 * module's own delay) starts the level's delay */
TEST(ConversationTest, ASeedRefusedCountsNothing)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.ecu.security.delayed = true;
    b.ecu.security.delay_until_us = INT64_MAX;
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::ended);
    EXPECT_EQ(b.v->report.nrc, 0x37);
    EXPECT_EQ(b.lvl(0x01).failed, 0u);
    EXPECT_GT(b.lvl(0x01).wait_until_us, b.now);
    EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_UNLOCK),
              std::vector<Bytes>{unlock_end(0x01, ECUIO_GUEST_ENDED, 0x37)});
}

/* The session entered again locks the level: after an unlock, the gated
 * read is refused once more; mid-attempt, a key lost with the session is
 * one failure */
TEST(ConversationTest, ASessionEnteredAgainLocksTheLevel)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    ASSERT_TRUE(b.lvl(0x01).unlocked);
    b.lose_on = kRow;
    for (int i = 0; i < 10 && !b.lose_on.empty(); ++i) {
        b.pass();
    }
    b.passes(4);
    EXPECT_FALSE(b.lvl(0x01).unlocked);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::lost);
    EXPECT_EQ(b.e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    b.guest.requests.push_back(kGated);
    b.passes(6);
    const std::vector<Bytes> replies = b.guest.of_kind(ECUIO_GUEST_REPLY);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0], (Bytes{0x7F, 0x22, 0x33}));

    Bench k;
    ASSERT_NO_FATAL_FAILURE(k.start());
    k.lose_on = key_request(0x01);
    ASSERT_TRUE(ecuio_conversation_unlock(k.v.get(), 0x01));
    k.until_word_ended();
    EXPECT_EQ(k.v->report.unlock, conv_unlock::failed);
    EXPECT_EQ(k.v->report.nrc, 0x7F);
    EXPECT_EQ(k.lvl(0x01).failed, 1u);
    EXPECT_EQ(k.e->chans[0].session_lost, 1u);
}

/* A session lost and entered again between two of the conversation's
 * steps is still a loss: the count moved, though no step saw it left */
TEST(ConversationTest, ASessionLostBetweenTwoStepsLocksTheLevel)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    ASSERT_TRUE(b.lvl(0x01).unlocked);
    ecuio_request_engine* e = b.e.get();
    for (int i = 0; i < 8; ++i) { /* the read, refused for a lost session */
        const int32_t r = ecuio_engine_due(e, 0, b.now);
        if (r == ECUIO_ENGINE_NONE) {
            b.now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const Bytes q = engine_talk_test::wire(e, 0, r);
        ecuio_engine_sent(e, 0, r, true, b.now);
        b.now += 4 * kMs;
        if (r == 0) {
            b.frame({0x7F, 0x22, 0x7F});
            break;
        }
        b.frame(b.answer(q));
        b.now += ECUIO_ENGINE_GAP_US;
    }
    ASSERT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERING);
    b.now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(ecuio_engine_due(e, 0, b.now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(e, 0, ECUIO_ENGINE_ONCE, true, b.now);
    b.now += 4 * kMs;
    b.frame({0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
    ASSERT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ecuio_conversation_step(b.v.get(), e, b.now);
    EXPECT_FALSE(b.lvl(0x01).unlocked);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::lost);
}

/* A session lost while the word's seed waits its turn ends the attempt and
 * takes the seed back: none goes for it, so no answer to it (a 36 or 37)
 * goes untold, and the next word's seed is the only one */
TEST(ConversationTest, ASeedWaitingWhenTheSessionIsLostNeverGoes)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    ASSERT_EQ(b.v->report.unlock, conv_unlock::seed_offered);
    b.now += ECUIO_ENGINE_S3_LATE_US; /* nothing sent: the session lapsed */
    b.passes(10);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::ended);
    EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_UNLOCK),
              std::vector<Bytes>{unlock_end(0x01, ECUIO_GUEST_ENDED)});
    EXPECT_EQ(heard(b, {0x27, 0x01}), 0u);
    EXPECT_TRUE(talk_free(b));
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::unlocked);
    EXPECT_EQ(heard(b, {0x27, 0x01}), 1u);
}

/* The module's own request waiting as the session lapses still goes, in
 * the session entered again, and its reply reaches the module */
TEST(ConversationTest, AModulesRequestWaitingWhenTheSessionIsLostStillGoes)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const Bytes read = {0x22, 0xD3, 0x00};
    b.guest.requests.push_back(read);
    ASSERT_NO_FATAL_FAILURE(b.until_offered());
    b.now += ECUIO_ENGINE_S3_LATE_US; /* nothing sent: the session lapsed */
    b.passes(10);
    EXPECT_EQ(b.e->chans[0].session_lost, 1u);
    EXPECT_EQ(heard(b, read), 1u);
    EXPECT_EQ(b.guest.of_kind(ECUIO_GUEST_REPLY).size(), 1u);
}

/* The engine giving a loss back (a refusal that was the request's own)
 * is no loss: a level unlocked since stays open */
TEST(ConversationTest, ALossGivenBackLeavesTheLevelOpen)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    b.lose_on = kRow;
    for (int i = 0; i < 10 && !b.lose_on.empty(); ++i) {
        b.pass();
    }
    b.passes(4);
    ASSERT_EQ(b.e->chans[0].session_lost, 1u);
    ASSERT_EQ(b.e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    ASSERT_TRUE(b.lvl(0x01).unlocked);
    --b.e->chans[0].session_lost; /* as request_engine_session.h gives back */
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    EXPECT_TRUE(b.lvl(0x01).unlocked);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::unlocked);
    ++b.e->chans[0].session_lost; /* and a loss after it still counts */
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    EXPECT_FALSE(b.lvl(0x01).unlocked);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::lost);
}

/* A fault while the seed's talk is offered or out lets the talk go: an
 * offered seed never reaches the bus, one out is taken at its settle,
 * and the channel's talk is free for the next offer either way */
TEST(ConversationTest, AFaultLetsTheChannelsTalkGo)
{
    Bench offered;
    ASSERT_NO_FATAL_FAILURE(offered.start());
    offered.guest.fault_kind = ECUIO_GUEST_UNLOCK;
    offered.guest.accept_fault = ecuio_guest_fault::trap;
    ASSERT_TRUE(ecuio_conversation_unlock(offered.v.get(), 0x01));
    ecuio_conversation_step(offered.v.get(), offered.e.get(), offered.now);
    ASSERT_TRUE(offered.e->chans[0].talk.offered);
    ecuio_engine_session_step(offered.e->chans[0],
                              ECUIO_SESSION_STATE_ABANDONED);
    ecuio_conversation_step(offered.v.get(), offered.e.get(), offered.now);
    EXPECT_EQ(offered.v->report.fault, ecuio_conversation_fault::trap);
    EXPECT_TRUE(talk_free(offered));
    offered.passes(6);
    EXPECT_EQ(heard(offered, {0x27, 0x01}), 0u);

    Bench out;
    ASSERT_NO_FATAL_FAILURE(out.start());
    out.guest.fault_kind = ECUIO_GUEST_UNLOCK;
    out.guest.accept_fault = ecuio_guest_fault::trap;
    ASSERT_NO_FATAL_FAILURE(seed_out(out, 0x01, false));
    ecuio_engine_session_step(out.e->chans[0], ECUIO_SESSION_STATE_ABANDONED);
    ecuio_conversation_step(out.v.get(), out.e.get(), out.now);
    ASSERT_EQ(out.v->report.fault, ecuio_conversation_fault::trap);
    ASSERT_TRUE(out.e->chans[0].talk.out);
    out.reply_to({0x27, 0x01});
    ASSERT_NE(out.e->chans[0].talk.result.end, ecuio_talk_end::none);
    EXPECT_EQ(ecuio_conversation_next_us(out.v.get(), out.e.get(), out.now),
              out.now);
    ecuio_conversation_step(out.v.get(), out.e.get(), out.now);
    EXPECT_TRUE(talk_free(out));
    EXPECT_EQ(ecuio_conversation_next_us(out.v.get(), out.e.get(), out.now),
              INT64_MAX);
    EXPECT_EQ(out.guest.of_kind(ECUIO_GUEST_SEED).size(), 0u);
}

/* A session abandoned counts no loss: the level is locked all the same */
TEST(ConversationTest, ASessionLeftWithoutALossLocksTheLevel)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    ASSERT_TRUE(b.lvl(0x01).unlocked);
    const uint16_t lost = b.e->chans[0].session_lost;
    ecuio_engine_session_step(b.e->chans[0], ECUIO_SESSION_STATE_ABANDONED);
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    EXPECT_EQ(b.e->chans[0].session_lost, lost);
    EXPECT_FALSE(b.lvl(0x01).unlocked);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::lost);
}

/* One word is one attempt: a second while the first runs is busy, and the
 * first goes on to its end */
TEST(ConversationTest, AWordWhileAnAttemptRunsIsBusy)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x03));
    EXPECT_FALSE(ecuio_conversation_unlock(b.v.get(), 0x01)); /* one waits */
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    ASSERT_EQ(b.v->report.unlock, conv_unlock::seed_offered);
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    EXPECT_EQ(b.v->report.level, 0x03); /* the report stays the attempt's */
    EXPECT_EQ(b.v->report.unlock, conv_unlock::seed_offered);
    ecuio_conversation_step(b.v.get(), b.e.get(), b.now);
    EXPECT_EQ(b.v->report.level, 0x01);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::busy);
    EXPECT_EQ(b.v->report.words_ended, 1u);
    b.until_word_ended();
    EXPECT_EQ(b.v->report.level, 0x03);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::unlocked);
    EXPECT_TRUE(b.lvl(0x03).unlocked);
    EXPECT_FALSE(b.lvl(0x01).unlocked);
    EXPECT_EQ(heard(b, {0x27, 0x01}), 0u);
}

/* The word waits for the module's own request to settle; the seed and its
 * key then go together */
TEST(ConversationTest, AWordWaitsForTheModulesOwnRequest)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    const Bytes read = {0x22, 0xD3, 0x00};
    b.guest.requests.push_back(read);
    ASSERT_NO_FATAL_FAILURE(b.until_offered());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x01));
    b.until_word_ended();
    EXPECT_EQ(b.v->report.unlock, conv_unlock::unlocked);
    const int r = engine_talk_test::index_of(b.sent, read);
    const int s = engine_talk_test::index_of(b.sent, {0x27, 0x01});
    ASSERT_GE(r, 0);
    ASSERT_GT(s, r);
    EXPECT_EQ(b.sent[s + 1], key_request(0x01));
    ASSERT_GE(b.guest.accepted.size(), 2u);
    EXPECT_EQ(b.guest.accepted[0].kind, ECUIO_GUEST_REPLY);
    EXPECT_EQ(b.guest.accepted[1].kind, ECUIO_GUEST_SEED);
}

/* The word's refusals send nothing: a level the module doesn't declare,
 * and a seed the engine won't take while a functional channel has a
 * session */
TEST(ConversationTest, AWordForNoLevelOrWithAFunctionalSessionSendsNothing)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ASSERT_TRUE(ecuio_conversation_unlock(b.v.get(), 0x05));
    b.until_word_ended(4);
    EXPECT_EQ(b.v->report.unlock, conv_unlock::no_level);

    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_TRUE(ecuio_engine_init(e.get(), 2));
    e->pdu_max = ECUIO_ENGINE_PDU_MAX;
    e->chans[1].functional = true;
    ASSERT_TRUE(ecuio_engine_session(e.get(), 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Guest g;
    auto v = std::make_unique<ecuio_conversation>();
    const auto services = default_services();
    const auto levels = default_levels();
    ASSERT_TRUE(ecuio_conversation_init(v.get(), ops_of(&g), 0, kRx, kTx,
                                        services.data(), 4, levels.data(), 2));
    ecuio_conversation_begin(v.get(), e.get(), 0);
    ASSERT_TRUE(ecuio_conversation_unlock(v.get(), 0x01));
    ecuio_conversation_step(v.get(), e.get(), 0);
    EXPECT_EQ(v->report.unlock, conv_unlock::refused);
    EXPECT_EQ(v->report.words_ended, 1u);
    EXPECT_FALSE(e->chans[0].talk.offered);
    EXPECT_EQ(v->access.at, ecuio_security::phase::idle);
}

/* Two modules' conversations share nothing: one's unlock and failures
 * never reach the other's ECU, machine or code */
TEST(ConversationTest, TwoModulesConversationsShareNothing)
{
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_TRUE(ecuio_engine_init(e.get(), 2));
    e->pdu_max = ECUIO_ENGINE_PDU_MAX;
    for (uint32_t c = 0; c < 2; ++c) {
        const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(0x1C + c)};
        ASSERT_EQ(ecuio_engine_add(e.get(), c, did, 3),
                  ecuio_engine_add_result::added);
    }
    int64_t now = 0;
    Ecu ecus[2];
    Guest guests[2];
    std::unique_ptr<ecuio_conversation> v[2];
    const auto services = default_services();
    const std::vector<ecuio_plugin_security_level_t> levels = {
        level(0x01, 4, kRx), level(0x01, 4, 0x7E9)};
    guests[1].rx = 0x7E9;
    guests[1].tx = 0x7E1;
    for (uint32_t c = 0; c < 2; ++c) {
        ecus[c].now = &now;
        ecus[c].security.strict = true;
        v[c] = std::make_unique<ecuio_conversation>();
        ASSERT_TRUE(ecuio_conversation_init(
            v[c].get(), ops_of(&guests[c]), c, guests[c].rx, guests[c].tx,
            services.data(), 4, levels.data(), 2));
        EXPECT_EQ(v[c]->access.count, 1u);
        ecuio_conversation_begin(v[c].get(), e.get(), now);
    }
    guests[0].wrong_key = true;
    const auto pass = [&](uint32_t c) {
        ecuio_conversation_step(v[c].get(), e.get(), now);
        const int32_t r = ecuio_engine_due(e.get(), c, now);
        if (r != ECUIO_ENGINE_NONE) {
            const Bytes q = engine_talk_test::wire(e.get(), c, r);
            ecuio_engine_sent(e.get(), c, r, true, now);
            const Bytes reply = ecus[c](q);
            now += 4 * kMs;
            (void)ecuio_engine_on_frame(e.get(), c, reply.data(),
                                        static_cast<uint16_t>(reply.size()),
                                        now, nullptr);
            ecuio_conversation_frame(v[c].get(), e.get(), reply.data(),
                                     static_cast<uint32_t>(reply.size()));
        }
        now += ECUIO_ENGINE_GAP_US;
        ecuio_conversation_step(v[c].get(), e.get(), now);
    };
    for (int i = 0; i < 8; ++i) {
        pass(0);
        pass(1);
    }
    ASSERT_EQ(e->chans[1].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_TRUE(ecuio_conversation_unlock(v[0].get(), 0x01));
    for (int i = 0; i < 40; ++i) {
        pass(0);
        pass(1);
    }
    EXPECT_EQ(v[0]->report.unlock, conv_unlock::failed);
    EXPECT_EQ(v[0]->access.levels[0].failed, 1u);
    EXPECT_EQ(v[1]->access.levels[0].failed, 0u);
    EXPECT_EQ(v[1]->report.words_ended, 0u);
    EXPECT_TRUE(guests[1].of_kind(ECUIO_GUEST_SEED).empty());
    EXPECT_TRUE(guests[1].of_kind(ECUIO_GUEST_UNLOCK).empty());
    EXPECT_EQ(ecus[1].security.wrong, 0u);
    for (const Bytes& q : ecus[1].heard) {
        EXPECT_NE(q[0], 0x27);
    }
}
