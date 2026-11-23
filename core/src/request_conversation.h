// File/layer: core/src/request_conversation.h | internal, C++17, Qt-free,
//             every platform, header-only
// Testing: UNIT: ConversationSessionTest (test_core)
//
// A requesting worker's half of a bound session (conversation_bind.h):
// the calls each of the three workers makes on its engine, on its own
// thread, beside its clears and talks. Nothing here locks but the
// support mutex for a publish, and nothing waits.
//
//   ecuio_conversations_start  once before the loop: a bound channel's
//                              built-in ?session=03 step is undone, its
//                              module's begin decides
//   ecuio_conversations_frame  after ecuio_engine_on_frame
//   ecuio_step_conversations   once a pass: the caller's word, the step,
//                              the settle, the rejoin, the publish
//   ecuio_conversations_next_us   bounds the worker's wait
//   ecuio_conversations_end    at the close, before the engine's 10 01

#ifndef ECUIO_REQUEST_CONVERSATION_H
#define ECUIO_REQUEST_CONVERSATION_H

#include "conversation_bind.h"
#include "request_engine.h"
#include "session.h"

#include <chrono>
#include <cstdint>
#include <mutex>

/* The requests of channel c that a refusal of code nrc dropped, put back
 * in the rotation once each (ADR-0066 section 2.4: after an unlock, a
 * DID refused for security access may now answer). A row refused again
 * is dropped as any other. */
inline void ecuio_engine_rejoin(ecuio_request_engine* e, uint32_t c,
                                uint8_t nrc)
{
    if (c >= e->chan_count) {
        return;
    }
    ecuio_engine_channel& ch = e->chans[c];
    bool moved = false;
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        if (r.channel != c || r.dropped != ECUIO_DROP_ACCESS || r.nrc != nrc) {
            continue;
        }
        r.dropped = 0;
        r.nrc = 0;
        if (r.parked == 0) {
            ++ch.requests;
        }
        moved = true;
    }
    if (moved) {
        ++ch.generation;
    }
}

/* Once before the worker's loop: no built-in session step on a bound
 * channel (ADR-0060 section 4) */
inline void ecuio_conversations_start(ecuio_session_t* s,
                                      ecuio_request_engine* e)
{
    ecuio_session_talk* t = s->talk.get();
    for (uint32_t k = 0; t != nullptr && k < t->count; ++k) {
        const uint32_t c = t->bound[k].conversation.channel;
        if (c < e->chan_count) {
            t->bound[k].talk_bound.store(ecuio_engine_talk_bound(e, c));
        }
        if (c < e->chan_count && e->chans[c].session_entries == 0 &&
            e->chans[c].session_step == ECUIO_SESSION_STATE_ENTERING) {
            ecuio_engine_session_step(e->chans[c],
                                      ECUIO_SESSION_STATE_NOT_ASKED);
        }
    }
}

inline void ecuio_conversations_frame(ecuio_session_t* s,
                                      const ecuio_request_engine* e, uint32_t c,
                                      const uint8_t* bytes, uint32_t len)
{
    if (ecuio_bound_channel* b = ecuio_bound_channel_of(s, c)) {
        ecuio_conversation_frame(&b->conversation, e, bytes, len);
    }
}

/* A word's end to the store, as the report says it ended */
inline void ecuio_conversation_settle(ecuio_session_talk& t,
                                      ecuio_bound_channel& b)
{
    const ecuio_conversation_report& r = b.conversation.report;
    if (!b.box_attached || r.unlock == ecuio_unlock_state::busy) {
        return; /* a busy word's end is its attempt's */
    }
    ecuio_security::settle_box& x = b.box;
    x.key = {b.vehicle, b.conversation.rx, b.word_taken};
    x.unlocked = r.unlock == ecuio_unlock_state::unlocked;
    x.failed = r.unlock == ecuio_unlock_state::failed;
    x.delay = x.failed || r.nrc == ecuio_security::kNrcExceededAttempts ||
              r.nrc == ecuio_security::kNrcDelayNotExpired;
    x.utc = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
    x.posted.fetch_add(1, std::memory_order_release);
    t.store->wake();
}

/* The store's counts the unlock caller handed over, to the machine */
inline void ecuio_conversation_restore(ecuio_bound_channel& b, int64_t now_us)
{
    const uint32_t seq = b.restore_seq.load(std::memory_order_acquire);
    if (seq == b.restore_seen) {
        return;
    }
    b.restore_seen = seq;
    ecuio_security::module_access& a = b.conversation.access;
    for (uint32_t k = 0; k < a.count; ++k) {
        (void)ecuio_security::restore_level(a, k, b.restore_failed[k],
                                            b.restore_last_utc[k],
                                            b.restore_utc, now_us);
    }
}

/* Whether the levels' counts or delays differ from the view's */
inline bool ecuio_conversation_levels_moved(const ecuio_bound_channel& b)
{
    const ecuio_security::module_access& a = b.conversation.access;
    if (a.count != b.level_count) {
        return true;
    }
    for (uint32_t k = 0; k < a.count; ++k) {
        if (a.levels[k].failed != b.levels[k].failed ||
            (a.levels[k].wait_until_us != 0) !=
                (b.levels[k].delay_left_us != 0)) {
            return true;
        }
    }
    return false;
}

/* Under the support mutex: each level's view at now_us */
inline void ecuio_conversation_view(ecuio_bound_channel& b, int64_t now_us)
{
    const ecuio_security::module_access& a = b.conversation.access;
    b.level_count = a.count;
    for (uint32_t k = 0; k < a.count; ++k) {
        const int64_t until = a.levels[k].wait_until_us;
        b.levels[k].level = a.levels[k].level;
        b.levels[k].failed = a.levels[k].failed;
        b.levels[k].delay_left_us = until > now_us ? until - now_us : 0;
    }
    b.published_at = std::chrono::steady_clock::now();
}

inline bool ecuio_conversation_same(const ecuio_conversation_report& a,
                                    const ecuio_conversation_report& b)
{
    return a.state == b.state && a.session == b.session && a.fault == b.fault &&
           a.level == b.level && a.unlock == b.unlock && a.nrc == b.nrc &&
           a.left_us == b.left_us && a.words_ended == b.words_ended &&
           a.requests == b.requests;
}

/* One bound channel's pass: begun once, the word taken, the step */
inline void ecuio_step_one(ecuio_session_t* s, ecuio_session_talk& t,
                           ecuio_bound_channel& b, ecuio_request_engine* e,
                           int64_t now_us)
{
    ecuio_conversation& v = b.conversation;
    if (v.report.state == ecuio_conversation_state::unbegun) {
        ecuio_conversation_begin(&v, e, now_us);
    }
    b.functional.store(ecuio_engine_functional_session(e),
                       std::memory_order_relaxed);
    ecuio_conversation_restore(b, now_us);
    const uint32_t seq = b.word_seq.load(std::memory_order_acquire);
    if (seq != b.word_seen) {
        b.word_seen = seq;
        const uint8_t level = b.word_level.load(std::memory_order_relaxed);
        b.word_taken = level;
        if (!ecuio_conversation_unlock(&v, level)) {
            v.report.level = level; /* not talking: the word ends here */
            v.report.unlock = ecuio_unlock_state::refused;
            ++v.report.words_ended;
        }
    }
    ecuio_conversation_step(&v, e, now_us);
    if (v.report.words_ended != b.ends_seen) {
        b.ends_seen = v.report.words_ended;
        ecuio_conversation_settle(t, b);
    }
    if (!b.rejoined && v.report.unlock == ecuio_unlock_state::unlocked) {
        b.rejoined = true;
        ecuio_engine_rejoin(e, v.channel, 0x33);
    }
    const bool moved = !ecuio_conversation_same(v.report, b.published);
    if (moved || ecuio_conversation_levels_moved(b)) {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        if (moved) {
            b.published = v.report;
            ++b.generation;
        }
        ecuio_conversation_view(b, now_us);
    }
}

inline void ecuio_step_conversations(ecuio_session_t* s,
                                     ecuio_request_engine* e, int64_t now_us)
{
    ecuio_session_talk* t = s->talk.get();
    for (uint32_t k = 0; t != nullptr && k < t->count; ++k) {
        ecuio_step_one(s, *t, t->bound[k], e, now_us);
    }
}

/* The wait until the next conversation's step, from now_us (INT64_MAX
 * for none): a word waiting is now */
inline int64_t ecuio_conversations_next_us(ecuio_session_t* s,
                                           const ecuio_request_engine* e,
                                           int64_t now_us)
{
    int64_t next = INT64_MAX;
    ecuio_session_talk* t = s->talk.get();
    for (uint32_t k = 0; t != nullptr && k < t->count; ++k) {
        const ecuio_bound_channel& b = t->bound[k];
        if (b.word_seq.load(std::memory_order_acquire) != b.word_seen) {
            return 0;
        }
        if (b.conversation.report.state ==
            ecuio_conversation_state::unbegun) {
            continue; /* begun at the next pass, whatever it waits for */
        }
        const int64_t at =
            ecuio_conversation_next_us(&b.conversation, e, now_us);
        if (at != INT64_MAX && at - now_us < next) {
            next = at > now_us ? at - now_us : 0;
        }
    }
    return next;
}

/* Each module's end, while its channel's session still stands (the
 * engine's 10 01 comes after): the report says ended */
inline void ecuio_conversations_end(ecuio_session_t* s,
                                    const ecuio_request_engine* e)
{
    ecuio_session_talk* t = s->talk.get();
    for (uint32_t k = 0; t != nullptr && k < t->count; ++k) {
        ecuio_bound_channel& b = t->bound[k];
        const uint32_t c = b.conversation.channel;
        b.ended_in_session =
            c < e->chan_count &&
            e->chans[c].session_step == ECUIO_SESSION_STATE_ENTERED;
        ecuio_conversation_end(&b.conversation);
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        b.published = b.conversation.report;
        ++b.generation;
    }
}

#endif /* ECUIO_REQUEST_CONVERSATION_H */
