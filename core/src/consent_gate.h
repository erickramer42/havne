// File/layer: core/src/consent_gate.h | internal, C++17, Qt-free, every
//             platform, header-only
// Testing: UNIT: ConversationSessionTest (test_core)
//
// The one check before a plugin's code runs in a session (ADR-0066
// section 2.4): the conversation's bind at start asks it, and so does
// every later tier that runs a plugin's code. True only while the test
// door is open (ecuio_test_conversation_binding, internal.h), which no
// public header and no havne-cli flag reaches. A consent the session was
// given joins this one predicate, never a second place.

#ifndef ECUIO_CONSENT_GATE_H
#define ECUIO_CONSENT_GATE_H

#include "ecuio/core.h"

#include <atomic>

/* The test door, process-wide, shut by default (conversation_bind.cpp) */
extern std::atomic<bool> g_ecuio_conversation_door;

/* Whether session may run its plugin's code */
inline bool ecuio_plugin_code_may_run(const ecuio_session_t* session)
{
    (void)session;
    return g_ecuio_conversation_door.load(std::memory_order_relaxed);
}

#endif /* ECUIO_CONSENT_GATE_H */
