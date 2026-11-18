// File/layer: core/src/request_talk.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: EngineTalkTest (test_obd2)
//
// A talk (ADR-0066 section 2.4): one request of up to ECUIO_ENGINE_PDU_MAX
// bytes that the conversation tier offers a channel, sent through the
// channel's one-shot slot and settled once. This header is the talk by
// itself: its state, the door every offer passes, its place in a turn and
// its settle. It knows nothing of the engine's types; request_engine.h
// holds one state in each channel and calls in where a one-shot is
// chosen, sent, answered or timed out.
//
// The offer copies the request, so the bytes the door judged are the bytes
// a transport sends, whatever becomes of the caller's buffer meanwhile.
// A talk is never sent twice: a failed write settles it unsent, since a
// key must not go out again for one seed. Its reply is matched on its
// first echo bytes (1 to 3: the service, then the request's next bytes),
// because 67 02 answers a key request of any length.

#ifndef ECUIO_REQUEST_TALK_H
#define ECUIO_REQUEST_TALK_H

#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_WRITE_MAX; plain C */
#include "tier2_ceiling.h"

#include <cstdint>

/* The longest request a talk carries: what one J2534 write holds after
 * its 4-byte CAN ID, which the surrogate's protocol carries whole */
constexpr uint32_t ECUIO_ENGINE_PDU_MAX = 60;
static_assert(ECUIO_ENGINE_PDU_MAX == ECUIO_PT_WRITE_MAX - 4,
              "a talk's request fits one PassThruWriteMsgs message");
/* The longest on a transport that sends single frames only (ELM327) */
constexpr uint32_t ECUIO_ENGINE_PDU_SINGLE = 7;

/* What an offer came to */
enum class ecuio_talk_offer : uint8_t {
    taken,    /* it goes out at its turn */
    busy,     /* one is offered, out, or settled and not yet taken */
    too_long, /* past the transport's bound */
    refused,  /* outside the ceiling, or no request at all */
};

/* How a talk ended */
enum class ecuio_talk_end : uint8_t {
    none,       /* not settled: offered or out, or there is none */
    answered,   /* its positive reply came */
    refused,    /* a negative reply: nrc is its code */
    unanswered, /* its wait ran out */
    unsent,     /* its write failed: not confirmed on the bus, whole or part */
};

struct ecuio_talk_result {
    ecuio_talk_end end = ecuio_talk_end::none;
    uint8_t nrc = 0;
};

/* One channel's talk */
struct ecuio_engine_talk_state {
    uint8_t pdu[ECUIO_ENGINE_PDU_MAX] = {}; /* the request, a copy */
    uint8_t len = 0;
    uint8_t echo = 0;        /* how many of its first bytes its reply echoes */
    bool first = false;      /* ahead of the rotation and the keep-alive */
    bool offered = false;    /* waiting for its turn */
    bool out = false;        /* the channel's one-shot is this talk */
    bool after_scan = false; /* the scan's next entry goes before it */
    ecuio_talk_result result;
};

/* Offers pdu as the channel's talk. The door: a request outside tier 2's
 * ceiling is refused here whatever the caller checked, so nothing a
 * plugin's code asks for reaches a transport by another road. A service
 * that splits by sub-function needs its second byte. pdu_max is the
 * transport's bound (the engine's pdu_max). A taken request is copied:
 * pdu is not read after the call returns. */
inline ecuio_talk_offer ecuio_engine_talk(ecuio_engine_talk_state& t,
                                          uint32_t pdu_max, const uint8_t* pdu,
                                          uint32_t len, uint8_t echo,
                                          bool first)
{
    if (pdu == nullptr || len == 0 || echo < 1 || echo > 3 || echo > len ||
        (len < 2 && ecuio_tier2_splits(pdu[0])) ||
        !ecuio_tier2_allowed(pdu[0], len < 2 ? uint8_t{0} : pdu[1])) {
        return ecuio_talk_offer::refused;
    }
    if (len > pdu_max || len > ECUIO_ENGINE_PDU_MAX) {
        return ecuio_talk_offer::too_long;
    }
    if (t.offered || t.out || t.result.end != ecuio_talk_end::none) {
        return ecuio_talk_offer::busy;
    }
    for (uint32_t k = 0; k < len; ++k) {
        t.pdu[k] = pdu[k];
    }
    t.len = static_cast<uint8_t>(len);
    t.echo = echo;
    t.first = first;
    t.offered = true;
    return ecuio_talk_offer::taken;
}

/* Whether the offered talk goes at this point of the channel's turn.
 * first_turn: ahead of the keep-alive, the walk and every list, where
 * only a talk offered first goes. Elsewhere it takes turns with the
 * scan: once a talk has gone, a scan entry that waits (scan_waits) goes
 * before the next, so a caller that always has a request can't hold the
 * scan off. */
inline bool ecuio_talk_turn(ecuio_engine_talk_state& t, bool first_turn,
                            bool scan_waits)
{
    if (!t.offered) {
        return false;
    }
    if (first_turn) {
        return t.first;
    }
    if (scan_waits && t.after_scan) {
        t.after_scan = false;
        return false;
    }
    t.after_scan = true;
    return true;
}

/* The talk takes the one-shot: its echo bytes into bytes (the one-shot's
 * request, which the reply is matched against), their count returned.
 * The transport reads the whole request through ecuio_engine_pdu_of. */
inline uint8_t ecuio_talk_take_slot(ecuio_engine_talk_state& t, uint8_t* bytes)
{
    for (uint8_t k = 0; k < t.echo; ++k) {
        bytes[k] = t.pdu[k];
    }
    t.offered = false;
    t.out = true;
    return t.echo;
}

/* What a channel's request puts on the wire, its length into *len: the
 * talk's whole request when the one-shot (once) is the talk, else the
 * request's own bytes, count of them */
inline const uint8_t* ecuio_engine_pdu_of(const ecuio_engine_talk_state& t,
                                          bool once, const uint8_t* bytes,
                                          uint8_t count, uint32_t* len)
{
    const bool talk = once && t.out;
    *len = talk ? t.len : count;
    return talk ? t.pdu : bytes;
}

/* The talk out has ended: the one-shot is free, the result waits */
inline void ecuio_talk_settle(ecuio_engine_talk_state& t, ecuio_talk_end end,
                              uint8_t nrc)
{
    t.out = false;
    t.result.end = end;
    t.result.nrc = nrc;
}

/* Reads the settle once: true and *out filled when the talk has ended,
 * after which the next may be offered; false while it is offered or out,
 * or there is none. */
inline bool ecuio_engine_talk_take(ecuio_engine_talk_state& t,
                                   ecuio_talk_result* out)
{
    if (t.result.end == ecuio_talk_end::none) {
        return false;
    }
    *out = t.result;
    const bool after_scan = t.after_scan; /* whose turn it is outlives a talk */
    t = ecuio_engine_talk_state{};
    t.after_scan = after_scan;
    return true;
}

#endif /* ECUIO_REQUEST_TALK_H */
