// File/layer: core/src/talk_seam.h | internal, C++17, Qt-free, header-only
// Testing: HARNESS: each requesting transport's talk cases
//          (J2534TalkTest, Elm327TalkTest, SocketCanTalkTest)
//
// A talk (request_talk.h) from a session's caller to its requesting
// worker, for the transports' own cases: ecuio_session_test_talk
// (internal.h, talk_seam.cpp) leaves one request in the session's slot,
// and each requesting worker calls ecuio_take_test_talk where it takes a
// clear (SocketCAN's once before its loop too, so a request handed over
// before start is the session's first). The worker offers the request to
// its engine at its own time (ecuio_engine_offer_talk: a seed with its
// window, a key inside one) and, once the engine has settled it, leaves
// the result for
// ecuio_session_test_talk_result. Nothing in the app or the CLI reaches
// this: it lets a test send a request longer than a frame through a real
// session.
//
// The slot goes from hand to hand on its state alone: the caller fills
// it and stores asked; the worker reads it then and stores settled once
// it has written the result; the caller reads the result and stores
// idle. Neither side touches the fields while the other holds them, so
// no lock is taken and the worker allocates nothing.

#ifndef ECUIO_TALK_SEAM_H
#define ECUIO_TALK_SEAM_H

#include "request_engine.h"
#include "session.h"

/* The worker's pass over the session's slot at now_us: offers a request
 * newly asked, and settles the one its engine has finished */
inline void ecuio_take_test_talk(ecuio_session_t* session,
                                 ecuio_request_engine* e, int64_t now_us)
{
    ecuio_test_talk_slot& slot = session->test_talk;
    const uint8_t state = slot.state.load();
    if (state == ECUIO_TEST_TALK_ASKED) {
        ecuio_talk_request q;
        q.pdu = slot.pdu;
        q.len = slot.len;
        q.echo = slot.echo;
        q.kind = slot.kind;
        q.window_us = int64_t{slot.window_ms} * 1000;
        const ecuio_talk_offer offer =
            ecuio_engine_offer_talk(e, slot.channel, q, now_us);
        slot.offer = offer;
        slot.result = ecuio_talk_result{};
        slot.state.store(offer == ecuio_talk_offer::taken
                             ? ECUIO_TEST_TALK_OUT
                             : ECUIO_TEST_TALK_SETTLED);
    } else if (state == ECUIO_TEST_TALK_OUT &&
               ecuio_engine_talk_take(e->chans[slot.channel].talk,
                                      &slot.result)) {
        slot.state.store(ECUIO_TEST_TALK_SETTLED);
    }
}

#endif /* ECUIO_TALK_SEAM_H */
