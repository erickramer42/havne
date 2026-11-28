#ifndef ECUIO_REQUEST_ENGINE_DECLARED_H
#define ECUIO_REQUEST_ENGINE_DECLARED_H

#include "request_engine_session.h"

#include <cstdint>

inline bool ecuio_engine_declared_wanted(const ecuio_engine_channel& ch)
{
    return ch.session_declared && ecuio_engine_module_open(ch) &&
           (ch.reasons & ECUIO_ENGINE_REASON_READ) != 0;
}

/* Channel c's declared session follows what is read on it: asked when a
 * signal on it is, given up when none is (an entered one leaves by
 * ecuio_engine_declared_leave_due). A refusal stands; the URI's session
 * and a conversation's are their own. */
inline void ecuio_engine_session_follow(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.handed || (ch.session_source != ECUIO_SESSION_SOURCE_NONE &&
                      ch.session_source != ECUIO_SESSION_SOURCE_SET)) {
        return;
    }
    const bool wanted = ecuio_engine_declared_wanted(ch);
    if (wanted && ch.session_source == ECUIO_SESSION_SOURCE_NONE) {
        ch.session_asked = ECUIO_SESSION_EXTENDED;
        ch.session_source = ECUIO_SESSION_SOURCE_SET;
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_ENTERING);
        ++ch.generation;
    } else if (!wanted && ch.session_source == ECUIO_SESSION_SOURCE_SET &&
               !ch.sessioning &&
               (ch.session_step == ECUIO_SESSION_STATE_ENTERING ||
                ch.session_step == ECUIO_SESSION_STATE_UNANSWERED)) {
        ch.session_asked = 0;
        ch.session_source = ECUIO_SESSION_SOURCE_NONE;
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_NOT_ASKED);
        ++ch.generation;
    }
}

/* Channel c's declaration in force when allowed, else let go before
 * anything is sent */
inline void ecuio_engine_session_allow(ecuio_request_engine* e, uint32_t c,
                                       bool allowed)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (!ch.session_may_declare) {
        return;
    }
    const uint8_t idle = allowed ? uint8_t{ECUIO_SESSION_IDLE_NONE}
                                 : uint8_t{ECUIO_SESSION_IDLE_NOT_ALLOWED};
    if (allowed == ch.session_declared && idle == ch.session_idle) {
        return;
    }
    ch.session_declared = allowed;
    ch.session_idle = idle;
    if (!allowed && ch.session_source == ECUIO_SESSION_SOURCE_SET &&
        ch.session_entries == 0 && !ch.sessioning) {
        ch.session_asked = 0;
        ch.session_source = ECUIO_SESSION_SOURCE_NONE;
        ecuio_engine_session_step(ch, ECUIO_SESSION_STATE_NOT_ASKED);
    }
    ++ch.generation;
}

inline void ecuio_engine_declared_let_go(ecuio_engine_channel& ch)
{
    if (ch.session_source == ECUIO_SESSION_SOURCE_SET) {
        ch.session_asked = 0;
        ch.session_source = ECUIO_SESSION_SOURCE_NONE;
    }
}

inline void ecuio_engine_sessions_follow(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_session_follow(e, c);
    }
}

/* Open module c's 10 01 once the last signal on it stops being read,
 * into the one-shot; true when it went there */
inline bool ecuio_engine_declared_leave_due(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.session_source != ECUIO_SESSION_SOURCE_SET || ch.handed ||
        ch.session_step != ECUIO_SESSION_STATE_ENTERED ||
        ecuio_engine_declared_wanted(ch) ||
        !ecuio_engine_session_leave(e, c, &ch.once)) {
        return false;
    }
    ecuio_engine_declared_let_go(ch);
    ch.sessioning = true;
    ++ch.generation;
    return true;
}

#endif
