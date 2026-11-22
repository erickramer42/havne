// File/layer: core/src/socketcan_send.h | internal, C++17, Qt-free, header-only
// Testing: UNIT: SocketCanSendTest (test_socketcan); HARNESS:
//          SocketCanTalkTest (test_vcan)
//
// A request on its way out of a requesting socketcan:// session (ADR-0040,
// ADR-0066 section 2.4): isotp.h's sender paced by the bus. A request of
// up to 7 bytes is one frame. A longer one is a First Frame, the ECU's
// Flow Control, then Consecutive Frames at the pace it set, and while it
// is under way the socket sends nothing else. Each frame waits for its
// own echo before the next, so nothing queues behind a frame the bus
// never took; the worker holds that frame and its N_As bound.
//
// A send ends failed when no Flow Control comes within N_Bs, the ECU
// answers overflow or a Flow Control that can't be followed, a frame
// isn't echoed, or the kernel refuses a write. A failed request counts as
// never sent: the engine takes it back, and a talk settles unsent. The
// send says why it failed (socketcan_send_why), since only the last two
// are the link's doing: the first is one ECU's silence and the second
// its answer (socketcan_send_failed).
//
// The sender is pure over isotp.h. socketcan_send_failed alone knows the
// engine and link health, which is why request_setup.h is included.

#ifndef ECUIO_SOCKETCAN_SEND_H
#define ECUIO_SOCKETCAN_SEND_H

#include "isotp.h"
#include "request_setup.h"

#include <cstdint>

/* Why a send ended failed, as the sender knows it */
enum class socketcan_send_why : uint8_t {
    write, /* not the sender's doing: a refused write, an unechoed frame */
    n_bs,  /* no Flow Control within N_Bs: the ECU's silence */
    flow,  /* an overflow, or a Flow Control that can't be followed */
};

/* A request longer than a frame, between its First Frame and its last */
struct socketcan_send {
    bool active = false; /* a Flow Control or a Consecutive Frame is owed */
    uint32_t c = 0;      /* its channel */
    int32_t r = 0;       /* the engine's name for the request */
    /* why the last step answered failed */
    socketcan_send_why why = socketcan_send_why::write;
};

enum class socketcan_send_step : uint8_t {
    none,   /* nothing goes out now */
    frame,  /* *out goes on the bus and awaits its echo; more follow */
    last,   /* *out goes on the bus and awaits its echo; the request is out */
    failed, /* the send has ended: the request was not sent */
};

/* Starts request r of channel c, len bytes to id, at now_us: last with
 * its one frame, frame with its First Frame, or failed (a send is under
 * way, or isotp.h's sender refuses the length). */
inline socketcan_send_step socketcan_send_start(socketcan_send& s, isotp_tx& tx,
                                                uint32_t c, int32_t r,
                                                uint32_t id, const uint8_t* pdu,
                                                uint32_t len, int64_t now_us,
                                                isotp_can* out)
{
    if (s.active || len > ECUIO_ISOTP_MAX ||
        !isotp_tx_start(tx, id, pdu, static_cast<uint16_t>(len), now_us, out)) {
        return socketcan_send_step::failed;
    }
    s.c = c;
    s.r = r;
    s.why = socketcan_send_why::write;
    s.active = tx.state != isotp_tx_state::idle;
    return s.active ? socketcan_send_step::frame : socketcan_send_step::last;
}

/* A frame from the send's ECU at now_us. True when it is Flow Control
 * while a send is under way: the sender's, never part of a reply. *step
 * is failed when it ended the send (overflow, an abandoned or malformed
 * Flow Control), else none. */
inline bool socketcan_send_flow(socketcan_send& s, isotp_tx& tx,
                                const isotp_can& f, int64_t now_us,
                                socketcan_send_step* step)
{
    *step = socketcan_send_step::none;
    if (!s.active || isotp_kind(f) != isotp_pci::flow) {
        return false;
    }
    const isotp_flow_result got = isotp_tx_flow(tx, f, now_us);
    if (got == isotp_flow_result::overflow ||
        got == isotp_flow_result::abandoned) {
        s.active = false;
        s.why = socketcan_send_why::flow;
        *step = socketcan_send_step::failed;
    }
    return true;
}

/* The send's next frame at now_us, when one is due. echo_pending: the
 * frame before hasn't come back yet, so nothing goes. failed once N_Bs
 * has passed with no Flow Control. */
inline socketcan_send_step socketcan_send_next(socketcan_send& s, isotp_tx& tx,
                                               bool echo_pending,
                                               int64_t now_us, isotp_can* out)
{
    if (!s.active || echo_pending) {
        return socketcan_send_step::none;
    }
    switch (isotp_tx_next(tx, now_us, out)) {
    case isotp_tx_step::frame:
        s.active = tx.state != isotp_tx_state::idle;
        return s.active ? socketcan_send_step::frame
                        : socketcan_send_step::last;
    case isotp_tx_step::later:
        return socketcan_send_step::none;
    case isotp_tx_step::timed_out:
        s.active = false;
        s.why = socketcan_send_why::n_bs;
        return socketcan_send_step::failed;
    case isotp_tx_step::idle:
    default:
        s.active = false;
        s.why = socketcan_send_why::write;
        return socketcan_send_step::failed;
    }
}

/* The send can't go on (an unechoed frame, a refused write, the socket
 * gone): the sender is free for the next request */
inline void socketcan_send_end(socketcan_send& s, isotp_tx& tx)
{
    s.active = false;
    tx.state = isotp_tx_state::idle;
}

/* Request r of channel c didn't get out, at now_us: the sender is free
 * for the next, the engine takes the request back (a talk settles
 * unsent), and link health counts it by its reason. A refused write or
 * an unechoed frame is a failed write; no Flow Control within N_Bs is a
 * request that timed out, one ECU's silence; an overflow or a Flow
 * Control that can't be followed is that ECU's answer, and counts
 * nothing. So one module's long request can't latch REQUEST_FAILED,
 * whose recovery asks 10 03 of every module again. */
inline void socketcan_send_failed(socketcan_send& s, isotp_tx& tx,
                                  ecuio_request_engine* e, uint32_t c,
                                  int32_t r, socketcan_send_why why,
                                  link_health& h, int64_t now_us)
{
    socketcan_send_end(s, tx);
    ecuio_engine_sent(e, c, r, false, now_us);
    if (why == socketcan_send_why::write) {
        link_on_write(h, false);
    } else if (why == socketcan_send_why::n_bs) {
        ecuio_request_timed_out(*e, h);
    }
}

/* When the send next needs the worker with no frame awaiting its echo:
 * N_Bs's end while Flow Control is owed, the next frame's earliest time
 * otherwise; INT64_MAX with no send under way. */
inline int64_t socketcan_send_due_us(const socketcan_send& s,
                                     const isotp_tx& tx)
{
    if (!s.active) {
        return INT64_MAX;
    }
    return tx.state == isotp_tx_state::awaiting_flow ? tx.due_us + 1
                                                     : tx.due_us;
}

#endif /* ECUIO_SOCKETCAN_SEND_H */
