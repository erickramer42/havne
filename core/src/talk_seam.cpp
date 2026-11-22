// File/layer: core/src/talk_seam.cpp | C++17, Qt-free; the caller's half of
//             talk_seam.h, declared in internal.h (test-only)
// Testing: HARNESS: each requesting transport's talk cases
//          (J2534TalkTest, Elm327TalkTest, SocketCanTalkTest)

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"

#include <cstring>

ecuio_status_t ecuio_session_test_talk(ecuio_session_t* session,
                                       uint32_t channel, const uint8_t* pdu,
                                       uint32_t len, uint8_t echo, int kind,
                                       uint32_t window_ms)
{
    if (session == nullptr || pdu == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_test_talk_slot& slot = session->test_talk;
    if ((session->info.flags & ECUIO_SESSION_INFO_REQUESTS) == 0 ||
        channel >= session->info.module_count || len == 0 ||
        len > sizeof slot.pdu || kind < 0 || kind > 2 ||
        slot.state.load() != ECUIO_TEST_TALK_IDLE) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    slot.channel = channel;
    slot.len = len;
    slot.echo = echo;
    slot.kind = static_cast<ecuio_talk_kind>(kind);
    slot.window_ms = window_ms;
    std::memcpy(slot.pdu, pdu, len);
    slot.state.store(ECUIO_TEST_TALK_ASKED);
    return ECUIO_OK;
}

/* internal.h names an offer and an end by these numbers */
static_assert(static_cast<int>(ecuio_talk_offer::refused) == 3 &&
                  static_cast<int>(ecuio_talk_end::unsent) == 4 &&
                  static_cast<int>(ecuio_talk_end::lapsed) == 5,
              "the seam's numbers are the enums' own");

int ecuio_session_test_talk_result(ecuio_session_t* session, int* offer,
                                   int* end, uint8_t* nrc)
{
    if (session == nullptr || offer == nullptr || end == nullptr ||
        nrc == nullptr) {
        return 0;
    }
    ecuio_test_talk_slot& slot = session->test_talk;
    if (slot.state.load() != ECUIO_TEST_TALK_SETTLED) {
        return 0;
    }
    *offer = static_cast<int>(slot.offer);
    *end = static_cast<int>(slot.result.end);
    *nrc = slot.result.nrc;
    slot.state.store(ECUIO_TEST_TALK_IDLE);
    return 1;
}
