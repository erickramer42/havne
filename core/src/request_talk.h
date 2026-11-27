// File/layer: core/src/request_talk.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: EngineTalkTest (test_obd2)
//
// A talk (ADR-0066 section 2.4): one request of up to ECUIO_ENGINE_PDU_MAX
// bytes that the conversation tier offers a channel, sent through the
// channel's one-shot slot and settled once. This header is the talk by
// itself: its state, the door every offer passes, its place in a turn, its
// settle, and the key's window. It knows nothing of the engine's types;
// request_engine.h holds one state and one window in each channel and
// calls in where a one-shot is chosen, sent, answered or timed out.
//
// The offer copies the request, so the bytes the door judged are the bytes
// a transport sends, whatever becomes of the caller's buffer meanwhile.
// A talk is never sent twice: a failed write settles it unsent, since a
// key must not go out again for one seed. Its reply is matched on its
// first echo bytes (1 to 3: the service, then the request's next bytes),
// because 67 02 answers a key request of any length.
//
// The key's window: ISO 14229-1 lets a server answer a key that doesn't
// directly follow its seed with 7F 27 24, and a wrong step can count
// toward a lockout. So a talk says what it is, and a 27 is taken only as
// one of the two. A seed (27 with an odd sub-function) opens the
// channel's window when it is sent; while the window is open the engine
// sends that channel nothing but that seed's key (27 with the seed's
// sub-function plus one), which is taken only inside the window and goes
// ahead of everything. The
// window ends when the key is sent, when the seed ends any way but
// answered, at the seed's own time (window_us from its reply), and at the
// session's bound from the seed's send, whichever comes first; the engine
// also ends it when the channel's session is no longer entered and when
// the link leaves OK.

#ifndef ECUIO_REQUEST_TALK_H
#define ECUIO_REQUEST_TALK_H

#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_WRITE_MAX; plain C */
#include "tier2_ceiling.h"
#include "tier3_ceiling.h"
#include "operation_checks.h"

#include <cstdint>

/* The longest request a talk carries: what one J2534 write holds after
 * its 4-byte CAN ID, which the surrogate's protocol carries whole */
constexpr uint32_t ECUIO_ENGINE_PDU_MAX = 60;
static_assert(ECUIO_ENGINE_PDU_MAX == ECUIO_PT_WRITE_MAX - 4,
              "a talk's request fits one PassThruWriteMsgs message");
constexpr uint32_t ECUIO_ENGINE_WRITE_MAX = 4095;
static_assert(ECUIO_ENGINE_WRITE_MAX == ECUIO_PT_WRITE_LONG_MAX - 4,
              "a write-class talk is ISO-TP's longest, one J2534 message");
/* The longest on a transport that sends single frames only (ELM327), and
 * to a functional address on any */
constexpr uint32_t ECUIO_ENGINE_PDU_SINGLE = 7;

/* Security access (ISO 14229-1): a seed's request is the service and an
 * odd sub-function, its key's the next, even one */
constexpr uint8_t ECUIO_TALK_SECURITY_ACCESS = 0x27;

/* A seed's window_us, from its reply to its key: a manifest's bounds */
constexpr int64_t ECUIO_KEY_WINDOW_MIN_US = 50000;
constexpr int64_t ECUIO_KEY_WINDOW_MAX_US = 4000000;
/* The session's bound, from the seed's send: S3 less a second
 * (request_engine_state.h asserts it), so a key sent inside it keeps the
 * session itself */
constexpr int64_t ECUIO_KEY_WINDOW_SESSION_US = 4000000;

/* What an offer came to */
enum class ecuio_talk_offer : uint8_t {
    taken,    /* it goes out at its turn */
    busy,     /* one is offered, out, or settled and not yet taken */
    too_long, /* past the transport's bound */
    refused,  /* outside the ceiling, not what its kind says (a 27 that
                 is no seed or key, a key with no window open or for
                 another seed than the window's, a read that echoes more
                 than its service), or no request at all;
                 from the engine's offer also a seed or key to a
                 functional address, and a seed while a functional
                 channel has a session (ecuio_engine_offer_talk) */
};

/* How a talk ended */
enum class ecuio_talk_end : uint8_t {
    none,       /* not settled: offered or out, or there is none */
    answered,   /* its positive reply came */
    refused,    /* a negative reply: nrc is its code */
    unanswered, /* its wait ran out */
    unsent,     /* its write failed: not confirmed on the bus, whole or
                   part, so it may have reached the module */
    lapsed,     /* a key's window ended before its turn: it never
                   touched the bus */
};

/* What a talk is to the key's window */
enum class ecuio_talk_kind : uint8_t {
    plain,   /* on the guest's own list: its turn with the scan */
    seed,    /* its turn as plain; sent, it opens the channel's window */
    key,     /* only inside a window, ahead of everything; sent, it ends it */
    read,    /* core's own read of a module's memory, inside tier 3's read
              ceiling (tier3_ceiling.h): its turn as plain. Only an
              operation's run offers one (request_operation.h); a plugin's
              request is always plain */
    check,   /* an operation's reading before its run (ecuio_check_allowed) */
    session, /* a run's own session (ecuio_operation_session_allowed) */
    write,   /* an operation's write-class request (ecuio_tier3_write_allowed) */
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
    ecuio_talk_kind kind = ecuio_talk_kind::plain;
    bool offered = false;    /* waiting for its turn */
    bool out = false;        /* the channel's one-shot is this talk */
    bool after_scan = false; /* the scan's next entry goes before it */
    uint16_t wide_len = 0;   /* a write's length; its bytes are the engine's */
    ecuio_talk_result result;
};

/* One channel's window for a key. until_us 0: none. It closes by the
 * time alone too, so "open" is always asked with the time. */
struct ecuio_key_window {
    int64_t until_us = 0;
    int32_t window_us = 0; /* the seed's own, taken at its offer */
    uint8_t seed = 0;      /* the seed's sub-function, taken with it */
};
static_assert(ECUIO_KEY_WINDOW_MAX_US <= INT32_MAX,
              "a seed's window_us fits the window's 32 bits");

inline bool ecuio_key_window_open(const ecuio_key_window& w, int64_t now_us)
{
    return w.until_us != 0 && now_us < w.until_us;
}

/* An offer: the request (read only during the call), how many of its
 * first bytes its reply echoes, what it is, and a seed's window_us */
struct ecuio_talk_request {
    const uint8_t* pdu = nullptr;
    uint32_t len = 0;
    uint8_t echo = 0;
    ecuio_talk_kind kind = ecuio_talk_kind::plain;
    int64_t window_us = 0;
};

/* The fewest bytes a reply to service sid must echo to be that request's
 * own: a 22's DID with it (any 62 would answer a read otherwise), the
 * service alone for 2A, whose reply carries no sub-function, and the
 * sub-function for the rest */
constexpr uint8_t ecuio_talk_echo_min(uint8_t sid)
{
    return sid == 0x22 ? uint8_t{3} : sid == 0x2A ? uint8_t{1} : uint8_t{2};
}

/* Whether q is what its kind says, at now_us with the channel's window w:
 * a seed is 27 and an odd sub-function, nothing more, its window_us in
 * bounds; a key is 27 with the sub-function after the seed's whose window
 * w is, while it is open; a plain talk is any other service. So a 27 goes
 * out only as a seed or as the key of the seed just answered, however its
 * caller named it. */
inline bool ecuio_talk_kind_fits(const ecuio_talk_request& q,
                                 const ecuio_key_window& w, int64_t now_us)
{
    const bool access = q.pdu[0] == ECUIO_TALK_SECURITY_ACCESS;
    if (q.kind == ecuio_talk_kind::plain) {
        return !access;
    }
    if (q.kind == ecuio_talk_kind::read) {
        /* matched on its service alone: the run judges a 76's counter and
         * a 63's length itself, so a wrong one is an answer, not a silence.
         * Its ceiling holds no 27 (tier3_ceiling.h asserts it). */
        return q.echo == 1;
    }
    if (q.kind == ecuio_talk_kind::check ||
        q.kind == ecuio_talk_kind::session) {
        return q.len == 2;
    }
    if (q.kind == ecuio_talk_kind::write) {
        return !access;
    }
    if (!access || q.len < 2) {
        return false;
    }
    if (q.kind == ecuio_talk_kind::seed) {
        return (q.pdu[1] & 0x01) != 0 && q.len == 2 &&
               q.window_us >= ECUIO_KEY_WINDOW_MIN_US &&
               q.window_us <= ECUIO_KEY_WINDOW_MAX_US;
    }
    return ecuio_key_window_open(w, now_us) &&
           q.pdu[1] == static_cast<uint8_t>(w.seed + 1);
}

/* Whether q is inside the ceiling for its kind: a plain talk on the
 * guest's own list (the reads: the session and its keep-alive are core's
 * own step), a seed or a key anywhere in tier 2's (ecuio_talk_kind_fits
 * then holds it to 27), a read inside tier 3's read ceiling and nowhere
 * in tier 2's */
inline bool ecuio_talk_inside_ceiling(const ecuio_talk_request& q)
{
    const uint8_t sub = q.len < 2 ? uint8_t{0} : q.pdu[1];
    if (q.kind == ecuio_talk_kind::read) {
        return ecuio_tier3_read_allowed(q.pdu[0], sub);
    }
    if (q.kind == ecuio_talk_kind::check) {
        return ecuio_check_allowed(q.pdu[0], sub);
    }
    if (q.kind == ecuio_talk_kind::session) {
        return ecuio_operation_session_allowed(q.pdu[0], sub);
    }
    if (q.kind == ecuio_talk_kind::write) {
        return ecuio_tier3_write_allowed(q.pdu[0], sub);
    }
    return q.kind == ecuio_talk_kind::plain
               ? ecuio_tier2_guest_may_send(q.pdu[0], sub)
               : ecuio_tier2_allowed(q.pdu[0], sub);
}

/* Whether q's echo is at least its service's own. A read's is one byte
 * (ecuio_talk_kind_fits): a 63, a 75 and a 77 echo nothing more */
inline bool ecuio_talk_echo_fits(const ecuio_talk_request& q)
{
    return q.kind == ecuio_talk_kind::read ||
           q.kind == ecuio_talk_kind::write ||
           q.echo >= ecuio_talk_echo_min(q.pdu[0]);
}

/* Offers q as the channel's talk at now_us. The door: a request outside
 * tier 2's ceiling for its kind (ecuio_talk_inside_ceiling) is refused
 * here whatever the caller checked, so nothing a plugin's code asks for
 * reaches a transport by another road. A service that splits by
 * sub-function needs its second byte (the guest's list and the kind's
 * rule refuse one alone too: the check stands should either widen); the
 * echo is 1 to 3 bytes of the request and at least its service's own
 * (ecuio_talk_echo_min); a seed or a key is what its kind says
 * (ecuio_talk_kind_fits). pdu_max is the bound for this channel on this
 * transport. A taken request is copied: q.pdu is not read after the call
 * returns. A taken seed ends the window of the seed before it and keeps
 * its own window_us and sub-function in w. */
inline ecuio_talk_offer
ecuio_engine_talk(ecuio_engine_talk_state& t, ecuio_key_window& w,
                  uint32_t pdu_max, const ecuio_talk_request& q, int64_t now_us,
                  uint8_t* wide = nullptr, uint32_t wide_max = 0)
{
    if (q.pdu == nullptr || q.len == 0 || q.echo < 1 || q.echo > 3 ||
        q.echo > q.len || (q.len < 2 && ecuio_tier2_splits(q.pdu[0])) ||
        !ecuio_talk_inside_ceiling(q) || !ecuio_talk_echo_fits(q) ||
        !ecuio_talk_kind_fits(q, w, now_us)) {
        return ecuio_talk_offer::refused;
    }
    const bool write = q.kind == ecuio_talk_kind::write;
    if (write ? wide == nullptr || q.len > wide_max ||
                    q.len > ECUIO_ENGINE_WRITE_MAX
              : q.len > pdu_max || q.len > ECUIO_ENGINE_PDU_MAX) {
        return ecuio_talk_offer::too_long;
    }
    if (t.offered || t.out || t.result.end != ecuio_talk_end::none) {
        return ecuio_talk_offer::busy;
    }
    const uint32_t kept =
        q.len < ECUIO_ENGINE_PDU_MAX ? q.len : ECUIO_ENGINE_PDU_MAX;
    for (uint32_t k = 0; k < kept; ++k) {
        t.pdu[k] = q.pdu[k];
    }
    if (write) {
        for (uint32_t k = 0; k < q.len; ++k) {
            wide[k] = q.pdu[k];
        }
        t.wide_len = static_cast<uint16_t>(q.len);
    }
    t.len = static_cast<uint8_t>(kept);
    t.echo = q.echo;
    t.kind = q.kind;
    t.offered = true;
    if (q.kind == ecuio_talk_kind::seed) {
        w.until_us = 0;
        w.window_us = static_cast<int32_t>(q.window_us);
        w.seed = q.pdu[1];
    }
    return ecuio_talk_offer::taken;
}

/* Whether the offered talk is a key: what an open window lets through */
inline bool ecuio_talk_key_waits(const ecuio_engine_talk_state& t)
{
    return t.offered && t.kind == ecuio_talk_kind::key;
}

/* An offered key whose window ended before its turn (the time, the
 * session, the link): true, and it is settled lapsed */
inline bool ecuio_talk_key_lapsed(ecuio_engine_talk_state& t,
                                  ecuio_key_window& w, int64_t now_us)
{
    if (!ecuio_talk_key_waits(t) || ecuio_key_window_open(w, now_us)) {
        return false;
    }
    w.until_us = 0;
    t.offered = false;
    t.result.end = ecuio_talk_end::lapsed;
    t.result.nrc = 0;
    return true;
}

/* Whether the offered talk goes at this point of the channel's turn.
 * first_turn: ahead of the keep-alive, the walk and every list, where
 * only a key goes. Elsewhere it takes turns with the scan: once a talk
 * has gone, a scan entry that waits (scan_waits) goes before the next,
 * so a caller that always has a request can't hold the scan off. */
inline bool ecuio_talk_turn(ecuio_engine_talk_state& t, bool first_turn,
                            bool scan_waits)
{
    if (!t.offered) {
        return false;
    }
    if (first_turn) {
        return t.kind == ecuio_talk_kind::key;
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

/* ecuio_engine_pdu_of, with a write's bytes in wide */
inline const uint8_t* ecuio_engine_wire_of(const ecuio_engine_talk_state& t,
                                           const uint8_t* wide, bool once,
                                           const uint8_t* bytes, uint8_t count,
                                           uint32_t* len)
{
    if (once && t.out && t.wide_len != 0) {
        *len = t.wide_len;
        return wide;
    }
    return ecuio_engine_pdu_of(t, once, bytes, count, len);
}

/* The talk out has ended at now_us: the one-shot is free, the result
 * waits. A seed answered inside the window brings its end forward to its
 * own window_us from now; a seed that ended any other way, or answered
 * after the session's bound, ends it, and so does a key's every end. */
inline void ecuio_talk_settle(ecuio_engine_talk_state& t, ecuio_key_window& w,
                              ecuio_talk_end end, uint8_t nrc, int64_t now_us)
{
    if (t.kind == ecuio_talk_kind::seed && end == ecuio_talk_end::answered &&
        ecuio_key_window_open(w, now_us)) {
        if (now_us + w.window_us < w.until_us) {
            w.until_us = now_us + w.window_us;
        }
    } else if (t.kind == ecuio_talk_kind::seed ||
               t.kind == ecuio_talk_kind::key) {
        w.until_us = 0;
    }
    t.out = false;
    t.result.end = end;
    t.result.nrc = nrc;
}

/* The driver's write of the talk out, at now_us: one that failed settles
 * it unsent; a seed's first frame on the bus opens the window to the
 * session's bound; a key's ends it, so no end of the key lets another go
 * for the same seed */
inline void ecuio_talk_sent(ecuio_engine_talk_state& t, ecuio_key_window& w,
                            bool sent, int64_t now_us)
{
    if (!sent) {
        ecuio_talk_settle(t, w, ecuio_talk_end::unsent, 0, now_us);
    } else if (t.kind == ecuio_talk_kind::seed && w.until_us == 0) {
        w.until_us = now_us + ECUIO_KEY_WINDOW_SESSION_US;
    } else if (t.kind == ecuio_talk_kind::key) {
        w.until_us = 0;
    }
}

/* Withdraws a talk offered and not yet taken by the one-shot: true, and
 * nothing of it reaches the bus. False while it is out, settled or none
 * is offered. A seed's window stays as its offer left it (none). */
inline bool ecuio_talk_withdraw(ecuio_engine_talk_state& t)
{
    if (!t.offered) {
        return false;
    }
    const bool after_scan = t.after_scan;
    t = ecuio_engine_talk_state{};
    t.after_scan = after_scan;
    return true;
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
