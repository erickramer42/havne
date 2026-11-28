// File/layer: core/src/consent_gate.h | internal, C++17, Qt-free, every
//             platform, header-only
// Testing: UNIT: ConversationSessionTest, ConsentSessionTest (test_core)
//
// The one check before a plugin's code runs in a session (ADR-0066
// section 2.4 and its appendix of 2026-11-23): the conversation's bind
// at start asks it, and so does every later tier that runs a plugin's
// code. True for a consent applied before start
// (ecuio_session_apply_consent), on a bus no car is on (SocketCAN's vcan,
// judged by the link's driver), or while the test door is open
// (ecuio_test_conversation_binding, internal.h), which no public header
// and no havne-cli flag reaches.

#ifndef ECUIO_CONSENT_GATE_H
#define ECUIO_CONSENT_GATE_H

#include "ecuio/core.h"
#include "session.h"

#include <atomic>

/* The test door, process-wide, shut by default (conversation_bind.cpp) */
extern std::atomic<bool> g_ecuio_conversation_door;

/* Whether session may run its plugin's code */
inline bool ecuio_plugin_code_may_run(const ecuio_session_t* session)
{
    return session->consent_applied || session->no_car_bus ||
           g_ecuio_conversation_door.load(std::memory_order_relaxed);
}

/* Whether session may hold channel's declared session */
inline bool ecuio_declared_session_allowed(const ecuio_session_t* session,
                                           uint32_t channel)
{
    return session->no_car_bus ||
           g_ecuio_conversation_door.load(std::memory_order_relaxed) ||
           (channel < 32 && ((session->declared_allowed >> channel) & 1u) != 0);
}

#endif /* ECUIO_CONSENT_GATE_H */
