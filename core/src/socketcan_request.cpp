// File/layer: core/src/socketcan_request.cpp | C++17, Qt-free; Linux only
//             (ECUIO_HAVE_SOCKETCAN)
// Testing: HARNESS: SocketCanRequestTest, SocketCanFaultTest,
//          SocketCanTalkTest (test_vcan)
//
// socketcan://'s requesting worker (ADR-0040): a session whose profile
// requests sends each request as ISO-TP (isotp.h) through the transport's
// CAN_RAW socket, answers each First Frame with Flow Control and
// dispatches whole messages; the rate's proof decides when its rotation
// starts. A request longer than a frame (a talk, ADR-0066 section 2.4)
// goes out by socketcan_send.h's rules. The transport itself is
// socketcan_transport.cpp's.

#include "socketcan_transport.h"

#if ECUIO_HAVE_SOCKETCAN

#include "link_health.h"
#include "request_setup.h"
#include "request_conversation.h"
#include "socketcan_send.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

using namespace ecuio_socketcan;

namespace {

/* The session's time, as the engine reads it */
int64_t now_us(const ecuio_socketcan_transport* t)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               host_clock::now() - t->epoch)
        .count();
}

/* One classic frame out; false when the kernel didn't take it (a full
 * queue, the interface gone) */
bool write_frame(int fd, const isotp_can& f, bool ext29)
{
    struct can_frame cf;
    std::memset(&cf, 0, sizeof cf);
    cf.can_id = f.id | (ext29 ? CAN_EFF_FLAG : 0u);
    cf.len = f.len;
    std::memcpy(cf.data, f.data, f.len);
    return write(fd, &cf, sizeof cf) == static_cast<ssize_t>(sizeof cf);
}

/* A request written and not yet seen back */
struct sent_request {
    bool active = false;
    uint32_t c = 0;
    int32_t r = ECUIO_ENGINE_NONE;
    int64_t at_us = 0;
    isotp_can f;
};

/* What a frame read is to a requesting session */
struct read_frame {
    socketcan_raw in;
    bool own = false; /* MSG_CONFIRM: this socket's own, back */
    uint64_t timestamp_us = 0;
    bool overflow = false;
};

/* A read at the close, up to left_us: an ECU's frame on a channel's reply
 * ID goes through ISO-TP (a First Frame answered with Flow Control) and a
 * whole message to the engine alone, since the session has closed. False
 * when the link failed, which ends the close at once. */
bool close_read(ecuio_socketcan_transport* t, int64_t left_us)
{
    const int64_t ms = std::min<int64_t>(kWaitMs, (left_us + 999) / 1000);
    struct pollfd p = {t->fd, POLLIN, 0};
    const int ready = poll(&p, 1, static_cast<int>(std::max<int64_t>(1, ms)));
    if (ready <= 0) {
        return ready == 0 || errno == EINTR;
    }
    alignas(struct canfd_frame) uint8_t raw[CANFD_MTU];
    for (int i = 0; i < kReadBurst; ++i) {
        struct iovec iov = {raw, sizeof raw};
        struct msghdr msg;
        std::memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        const ssize_t n = recvmsg(t->fd, &msg, MSG_DONTWAIT);
        if (n < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        if ((msg.msg_flags & MSG_CONFIRM) != 0) {
            continue; /* this socket's own frame, back */
        }
        uint32_t can_id;
        std::memcpy(&can_id, raw, 4);
        if (can_id & CAN_ERR_FLAG) {
            if (socketcan_error_is_hard(can_id)) {
                return false;
            }
            continue;
        }
        if (n != static_cast<ssize_t>(CAN_MTU) || raw[4] > 8) {
            continue;
        }
        isotp_can f;
        f.id = can_id & (can_id & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK);
        f.len = raw[4];
        std::memcpy(f.data, raw + 8, f.len);
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            if (t->rx_ids[c] != f.id ||
                ((can_id & CAN_EFF_FLAG) != 0) != t->ext29[c]) {
                continue;
            }
            isotp_message m;
            const isotp_result res = isotp_take(*t->isotp, f, now_us(t), &m);
            if (res == isotp_result::first) {
                (void)write_frame(t->fd,
                                  isotp_flow_control(isotp_flow_control_id(
                                      t->rx_ids[c], t->tx_ids[c])),
                                  t->ext29[c]);
            } else if (res == isotp_result::complete) {
                (void)ecuio_engine_on_frame(&t->engine, c, m.data, m.len,
                                            now_us(t), nullptr);
            }
            break;
        }
    }
    return true;
}

/* ADR-0060's close: 10 01 once a leaving channel's request still out ends
 * (J2534's bound), so its module hears it; its answer unread, as before. */
void leave_sessions(ecuio_socketcan_transport* t)
{
    ecuio_request_engine& e = t->engine;
    if (t->fd < 0) {
        return;
    }
    ecuio_request q[ECUIO_MAX_CHANNELS];
    bool leave[ECUIO_MAX_CHANNELS] = {};
    bool any = false;
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        leave[c] = ecuio_engine_session_leave(&e, c, &q[c]);
        any = any || leave[c];
    }
    if (!any) {
        return;
    }
    const int64_t start = now_us(t);
    for (;;) {
        const int64_t now = now_us(t);
        const int64_t until = ecuio_engine_leave_wait_until(&e, leave, start);
        if (until <= now) {
            break;
        }
        if (!close_read(t, until - now)) {
            return;
        }
    }
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        isotp_can f;
        *t->sender = isotp_tx{};
        if (leave[c] &&
            isotp_tx_start(*t->sender, t->tx_ids[c], q[c].bytes,
                           static_cast<uint16_t>(q[c].len), now_us(t), &f)) {
            (void)write_frame(t->fd, f, t->ext29[c]);
        }
    }
}

/* Writes f for request r of channel c: it awaits its echo, and the
 * engine's wait runs from it, so from a long request's last frame */
bool send_frame(ecuio_socketcan_transport* t, sent_request& pending, uint32_t c,
                int32_t r, const isotp_can& f, int64_t now)
{
    if (!write_frame(t->fd, f, t->ext29[c])) {
        if (errno == ENOBUFS) {
            ecuio_engine_write_refused(&t->engine);
        }
        return false;
    }
    ecuio_engine_sent(&t->engine, c, r, true, now);
    pending.active = true;
    pending.c = c;
    pending.r = r;
    pending.at_us = now;
    pending.f = f;
    return true;
}

/* A request that didn't get out (socketcan_send_failed), with this
 * transport's sender and engine */
void send_failed(ecuio_socketcan_transport* t, link_health& h,
                 socketcan_send& send, uint32_t c, int32_t r, int64_t now,
                 socketcan_send_why why)
{
    socketcan_send_failed(send, *t->sender, &t->engine, c, r, why, h, now);
}

/* The next request due on any channel, from next_chan on: its one frame,
 * or the First Frame of one longer than a frame (socketcan_send.h) */
void send_due(ecuio_socketcan_transport* t, link_health& h,
              sent_request& pending, socketcan_send& send, uint32_t& next_chan,
              int64_t now)
{
    ecuio_request_engine& e = t->engine;
    uint32_t c = 0;
    int32_t r = ECUIO_ENGINE_NONE;
    for (uint32_t k = 0; k < e.chan_count && r == ECUIO_ENGINE_NONE; ++k) {
        c = (next_chan + k) % e.chan_count;
        r = ecuio_engine_due(&e, c, now);
    }
    if (r == ECUIO_ENGINE_NONE) {
        return;
    }
    next_chan = (c + 1) % e.chan_count;
    const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
    uint32_t len = 0;
    const uint8_t* pdu = ecuio_engine_pdu_of(
        e.chans[c].talk, r == ECUIO_ENGINE_ONCE, q.bytes, q.len, &len);
    isotp_can f;
    if (socketcan_send_start(send, *t->sender, c, r, t->tx_ids[c], pdu, len,
                             now, &f) == socketcan_send_step::failed ||
        !send_frame(t, pending, c, r, f, now)) {
        send_failed(t, h, send, c, r, now, socketcan_send_why::write);
    }
}

/* A request longer than a frame under way: its next Consecutive Frame
 * once the frame before is echoed and the ECU's Flow Control allows it;
 * N_Bs passing or a refused write ends it unsent. Until its last frame
 * its wait hasn't begun, so a deadline that passes meanwhile moves on.
 * A reply that settles the request first ends the send with it. */
void send_on(ecuio_socketcan_transport* t, link_health& h,
             sent_request& pending, socketcan_send& send, int64_t now)
{
    /* Settled mid-send (a refusal after the First Frame, a late one that
     * names its service): a frame more would arm a one-shot nothing owns */
    if (send.active && t->engine.chans[send.c].outstanding != send.r) {
        socketcan_send_end(send, *t->sender);
        return;
    }
    isotp_can f;
    const socketcan_send_step step =
        socketcan_send_next(send, *t->sender, pending.active, now, &f);
    if (step == socketcan_send_step::failed) {
        send_failed(t, h, send, send.c, send.r, now, send.why);
    } else if (step != socketcan_send_step::none &&
               !send_frame(t, pending, send.c, send.r, f, now)) {
        send_failed(t, h, send, send.c, send.r, now, socketcan_send_why::write);
    } else if (send.active && now >= t->engine.chans[send.c].deadline_us) {
        ecuio_engine_sent(&t->engine, send.c, send.r, true, now);
    }
}

/* A frame from channel c's ECU while a long request is under way: true
 * when it is that send's Flow Control, which also shows an ECU answering
 * at this rate. One that ends the send takes the request back unsent. */
bool send_flow(ecuio_socketcan_transport* t, link_health& h,
               socketcan_send& send, uint32_t c, const isotp_can& f,
               int64_t now)
{
    socketcan_send_step step = socketcan_send_step::none;
    if (send.c != c || !socketcan_send_flow(send, *t->sender, f, now, &step)) {
        return false;
    }
    (void)socketcan_proof_settle(t->proof, now, true);
    if (step == socketcan_send_step::failed) {
        send_failed(t, h, send, c, send.r, now, send.why);
    }
    return true;
}

/* A requesting worker's sends in one place: the request awaiting its
 * echo, a request longer than a frame under way, and what each part of
 * a pass does with them */
struct request_sends {
    request_sends(ecuio_socketcan_transport* transport, link_health& health)
        : t(transport), e(transport->engine), h(health)
    {}

    ecuio_socketcan_transport* t;
    ecuio_request_engine& e;
    link_health& h;
    uint32_t next_chan = 0;
    sent_request pending;
    socketcan_send send; /* a request longer than a frame, under way */

    /* What became of the request awaiting its echo */
    void settle(socketcan_sent what)
    {
        const socketcan_proof_step step =
            socketcan_proof_on(t->proof, what, now_us(t));
        if (what == socketcan_sent::echoed) {
            link_on_write(h, true);
        } else if (pending.active) {
            /* it never reached the bus: not sent */
            socketcan_send_end(send, *t->sender);
            ecuio_engine_sent(&e, pending.c, pending.r, false, now_us(t));
            if (step == socketcan_proof_step::carry_on) {
                link_on_write(h, false);
            }
        }
        if (step == socketcan_proof_step::halt) {
            link_on_read(h, true, false, false); /* one episode; no more sent */
            if (send.active) {
                send_failed(t, h, send, send.c, send.r, now_us(t),
                            socketcan_send_why::write);
            }
        }
        pending.active = false;
    }

    /* The socket is new: nothing awaits an echo, and a send under way
     * ended unsent */
    void reopened()
    {
        pending.active = false;
        if (send.active) {
            send_failed(t, h, send, send.c, send.r, now_us(t),
                        socketcan_send_why::write);
        }
    }

    /* The waits that ran out: each channel's but the one a send still
     * going out keeps, then a request whose echo is late */
    void expire(int64_t now)
    {
        ecuio_key_windows_judge(&e, h);
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (send.active && send.c == c) {
                continue; /* still going out: send_on keeps its wait */
            }
            if (ecuio_engine_expire(&e, c, now)) {
                ecuio_request_timed_out(e, h);
            }
        }
        if (pending.active && now - pending.at_us > ECUIO_ISOTP_N_AS_US) {
            settle(socketcan_sent::unechoed);
        }
        (void)socketcan_proof_settle(t->proof, now, false);
    }

    /* The next request due, one awaiting its echo at a time, and a send
     * under way's next frame */
    void send_next(int64_t now)
    {
        if (!t->proof.halted && !t->proof.confirming) {
            if (!pending.active && !send.active) {
                send_due(t, h, pending, send, next_chan, now);
            }
            send_on(t, h, pending, send, now);
        }
    }

    /* How long the poll waits, in ms; *asks is whether the engine has
     * anything to wait for */
    int poll_wait_ms(int64_t now, bool* asks) const
    {
        /* Until the next event: the echo's N_As, the engine's next wait or
         * floor, ISO-TP's N_Cr; at most one quiet window. A halted session
         * sends nothing more, so what the engine calls due isn't one: it
         * waits the window out, still asking, so no quiet read counts */
        const int64_t in_us =
            (std::min)(ecuio_engine_next_event_us(&e, now),
                       ecuio_conversations_next_us(t->session, &e, now));
        const bool asking = in_us != INT64_MAX;
        int64_t next_us = asking && !t->proof.halted ? now + in_us : INT64_MAX;
        if (pending.active) {
            next_us =
                std::min(next_us, pending.at_us + ECUIO_ISOTP_N_AS_US + 1);
        } else if (!t->proof.halted) {
            next_us =
                std::min(next_us, socketcan_send_due_us(send, *t->sender));
        }
        if (t->proof.confirming) {
            next_us =
                std::min(next_us, t->proof.echoed_us + t->proof.quiet_us + 1);
        }
        int wait_ms = kWaitMs;
        if (next_us != INT64_MAX) {
            const int64_t ms = (next_us - now + 999) / 1000;
            wait_ms = static_cast<int>(
                std::max<int64_t>(0, std::min<int64_t>(ms, kWaitMs)));
        }
        *asks = asking;
        return wait_ms;
    }
};

/* The worker's locals, in the order the loop declared them */
struct worker_state {
    ecuio_session_t* s;
    ecuio_request_engine& e;
    isotp_rx& iso;
    socketcan_clock clock;
    link_health h;
    ecuio_unpicked_check unpicked_check;
    uint32_t drops = 0;
    bool overflow_pending = false;
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_diagnostics[ECUIO_MAX_CHANNELS] = {};
    uint32_t diag_seen = 0;
    uint32_t clear_seen[ECUIO_MAX_CHANNELS] = {};
    ecuio_focus_seen focus_seen; /* what ecuio_take_focus took */
    int64_t timing_at = 0;
    request_sends sends;

    explicit worker_state(ecuio_socketcan_transport* t)
        : s(t->session), e(t->engine), iso(*t->isotp),
          h(s->last_error, s->error_count, s->record_link.get(), s, t->epoch,
            &clock.last_out_us, &clock.last_host_us),
          sends(t, h)
    {
    }
};

enum class socket_step { go, again, stopped };

/* One frame's values, for the steps that take it */
struct frame_in {
    const uint8_t* raw;
    uint32_t can_id;
    uint8_t len;
    uint64_t stamp;
    int64_t newest;
    int64_t now;
};

void begin_worker(ecuio_socketcan_transport* t, worker_state& w)
{
    ecuio_session_t* s = w.s;
    ecuio_request_engine& e = w.e;
    if (t->probe) {
        ecuio_engine_probe(&e);
    }
    /* A focus set before start applies from the first request, and the
     * seam's talk handed over before start is that request */
    ecuio_take_focus(s, &e, w.focus_seen);
    ecuio_take_test_talk(s, &e, now_us(t));
    ecuio_conversations_start(s, &e);
}

socket_step socket_pass(ecuio_socketcan_transport* t, worker_state& w)
{
    link_health& h = w.h;
    if (t->fd < 0) {
        if (wait_for_stop(t, h.backoff_ms ? h.backoff_ms
                                          : LINK_BACKOFF_BASE_MS)) {
            return socket_step::stopped;
        }
        t->fd = reopen(t);
        if (t->fd < 0) {
            h.backoff_ms = link_backoff_next(
                h.backoff_ms ? h.backoff_ms : LINK_BACKOFF_BASE_MS);
            return socket_step::again;
        }
        w.drops = 0;
        (void)isotp_end_all(w.iso);
        w.sends.reopened();
    }
    return socket_step::go;
}

/* The poll until the next event; *ready_out is its result */
socket_step wait_pass(ecuio_socketcan_transport* t, worker_state& w,
                      int64_t now, int* ready_out)
{
    ecuio_request_engine& e = w.e;
    link_health& h = w.h;
    request_sends& sends = w.sends;
    bool asking = false;
    const int wait_ms = sends.poll_wait_ms(now, &asking);
    struct pollfd p[2] = {{t->fd, POLLIN, 0}, {t->wake_fd, POLLIN, 0}};
    const int ready = poll(p, 2, wait_ms);
    *ready_out = ready;
    if (ready < 0) {
        if (errno == EINTR) {
            return socket_step::again;
        }
        link_on_read(h, true, false, false);
        close(t->fd);
        t->fd = -1;
        return socket_step::again;
    }
    if (p[1].revents != 0) {
        return socket_step::stopped;
    }
    if (ready == 0) {
        if (!asking && !sends.pending.active) {
            if (ecuio_engine_unpicked(&e)) {
                if (!t->proof.halted) { /* a halt's failure stays */
                    ecuio_unpicked_pass(&e, h, w.unpicked_check, now_us(t));
                }
            } else {
                link_on_read(h, false, false, true); /* a quiet window */
            }
        }
    }
    return socket_step::go;
}

/* The message's kernel stamp, and the receive queue's drop count */
uint64_t take_control(struct msghdr& msg, worker_state& w)
{
    uint32_t& drops = w.drops;
    bool& overflow_pending = w.overflow_pending;
    uint64_t kernel_ns = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr;
         c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET) {
            continue;
        }
        if (c->cmsg_type == SCM_TIMESTAMPNS) {
            struct timespec ts;
            std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
            kernel_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000u +
                        static_cast<uint64_t>(ts.tv_nsec);
        } else if (c->cmsg_type == SO_RXQ_OVFL) {
            uint32_t count;
            std::memcpy(&count, CMSG_DATA(c), sizeof count);
            if (count != drops) {
                overflow_pending = true;
                drops = count;
            }
        }
    }
    return kernel_ns;
}

void error_frame(ecuio_socketcan_transport* t, worker_state& w,
                 const frame_in& f)
{
    ecuio_session_t* s = w.s;
    link_health& h = w.h;
    const uint8_t* raw = f.raw;
    const uint32_t can_id = f.can_id;
    const uint8_t len = f.len;
    const uint64_t stamp = f.stamp;
    socketcan_raw in;
    in.can_id = can_id;
    in.len = len;
    in.data = raw + 8;
    socketcan_to_frame(in, t->frame.get());
    t->frame->timestamp_us = stamp;
    ecuio_session_dispatch_callbacks(s, t->frame.get());
    if (!t->proof.proven) {
        w.sends.settle(socketcan_sent::error);
    } else if (socketcan_error_is_hard(can_id)) {
        link_on_read(h, true, false, false); /* bus-off */
    }
}

void own_frame(worker_state& w, const isotp_can& f)
{
    request_sends& sends = w.sends;
    if (sends.pending.active && f.id == sends.pending.f.id &&
        f.len == sends.pending.f.len &&
        std::memcmp(f.data, sends.pending.f.data, f.len) == 0) {
        sends.settle(socketcan_sent::echoed);
    }
}

/* A reassembled reply: the frame, the engine's stamp and frame, the proof */
void deliver_reply(ecuio_socketcan_transport* t, worker_state& w, uint32_t c,
                   const isotp_message& m, uint64_t stamp, int64_t now)
{
    ecuio_session_t* s = w.s;
    ecuio_request_engine& e = w.e;
    socketcan_clock& clock = w.clock;
    bool& overflow_pending = w.overflow_pending;
    ecuio_frame_t& reply = *t->reply;
    reply.timestamp_us = stamp;
    reply.arbitration_id = m.id;
    reply.flags = (t->ext29[c] ? ECUIO_FRAME_FLAG_EXTENDED : 0u) |
                  ECUIO_FRAME_FLAG_REASSEMBLED;
    if (overflow_pending) {
        reply.flags |= ECUIO_FRAME_FLAG_OVERFLOW;
        overflow_pending = false;
        ecuio_engine_overflowed(&e);
    }
    reply.reserved = 0;
    std::memcpy(reply.data, m.data, m.len);
    reply.data_length = m.len;
    ecuio_session_dispatch_callbacks(s, &reply);
    /* the refresh interval on the kernel's receive
     * times (ADR-0042) */
    e.stamp_set = true;
    e.stamp_us = static_cast<int64_t>(stamp);
    e.stamp_breaks = clock.host_steps;
    (void)ecuio_engine_on_frame(&e, c, reply.data, reply.data_length, now,
                                nullptr);
    ecuio_conversations_frame(s, &e, c, reply.data, reply.data_length);
    /* an ECU answered at this rate */
    (void)socketcan_proof_settle(t->proof, now, true);
}

/* True when a reply matched */
bool isotp_frame(ecuio_socketcan_transport* t, worker_state& w,
                 const isotp_can& f, const frame_in& fr)
{
    link_health& h = w.h;
    isotp_rx& iso = w.iso;
    const uint32_t can_id = fr.can_id;
    const uint64_t stamp = fr.stamp;
    const int64_t newest = fr.newest;
    const int64_t now = fr.now;
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        if (t->rx_ids[c] != f.id ||
            ((can_id & CAN_EFF_FLAG) != 0) != t->ext29[c]) {
            continue;
        }
        if (send_flow(t, h, w.sends.send, c, f, now)) {
            return false;
        }
        isotp_message m;
        const isotp_result res = isotp_take(iso, f, newest, &m);
        if (res == isotp_result::first) {
            /* ISO 15765-4: within 25 ms of the First Frame */
            (void)write_frame(t->fd,
                              isotp_flow_control(isotp_flow_control_id(
                                  t->rx_ids[c], t->tx_ids[c])),
                              t->ext29[c]);
        } else if (res == isotp_result::complete) {
            deliver_reply(t, w, c, m, stamp, now);
            return true;
        }
        return false;
    }
    return false;
}

/* Every frame waiting, up to the burst; then the ISO-TP waits' expiry */
void read_pass(ecuio_socketcan_transport* t, worker_state& w, int ready)
{
    link_health& h = w.h;
    socketcan_clock& clock = w.clock;
    alignas(struct canfd_frame) uint8_t raw[CANFD_MTU];
    alignas(struct cmsghdr)
        uint8_t control[CMSG_SPACE(sizeof(struct timespec)) +
                        CMSG_SPACE(sizeof(uint32_t))];
    bool matched = false;
    int64_t newest = -1; /* the last frame's receive time */
    for (int i = 0; ready > 0 && i < kReadBurst; ++i) {
        struct iovec iov = {raw, sizeof raw};
        struct msghdr msg;
        std::memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        const ssize_t n = recvmsg(t->fd, &msg, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            link_on_read(h, true, false, false);
            close(t->fd);
            t->fd = -1;
            break;
        }
        if (n != static_cast<ssize_t>(CAN_MTU) &&
            n != static_cast<ssize_t>(CANFD_MTU)) {
            continue;
        }
        const uint64_t kernel_ns = take_control(msg, w);
        const int64_t now = now_us(t);
        uint32_t can_id;
        std::memcpy(&can_id, raw, 4);
        const uint8_t len = raw[4];
        /* This socket's own frame, back: its stamp can trail the
         * frames it brought on by a stall (ADR-0032's appendix
         * "frames out of order"), and nothing reads it, so it leaves
         * the clock alone */
        const bool own = (msg.msg_flags & MSG_CONFIRM) != 0;
        uint64_t stamp = 0;
        if (!own) {
            stamp = socketcan_clock_step(clock, kernel_ns,
                                         static_cast<uint64_t>(now));
            newest = static_cast<int64_t>(stamp);
        }

        const frame_in fr{raw, can_id, len, stamp, newest, now};
        if (can_id & CAN_ERR_FLAG) { /* never this socket's own */
            error_frame(t, w, fr);
            continue;
        }
        if (n != static_cast<ssize_t>(CAN_MTU) || len > 8) {
            continue; /* ISO-TP on classic CAN only */
        }
        isotp_can f;
        f.id = can_id & (can_id & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK);
        f.len = len;
        std::memcpy(f.data, raw + 8, len);
        if (own) {
            own_frame(w, f);
            continue; /* a Flow Control frame's echo, or a late one */
        }
        if (isotp_frame(t, w, f, fr)) {
            matched = true;
        }
    }
    if (matched) {
        link_on_read(h, false, true, false);
    }
    (void)isotp_expire(w.iso, newest >= 0 ? newest : now_us(t));
}

void serve_session(ecuio_socketcan_transport* t, worker_state& w)
{
    ecuio_session_t* s = w.s;
    ecuio_request_engine& e = w.e;
    link_health& h = w.h;
    if (h.recovered) {
        h.recovered = false;
        if (t->probe) {
            ecuio_engine_probe(&e);
        }
        ecuio_engine_rescan(&e);
    }
    ecuio_take_diag_request(s, &e, w.diag_seen);
    ecuio_take_clear_requests(s, &e, w.clear_seen);
    ecuio_take_test_talk(s, &e, now_us(t));
    ecuio_take_focus(s, &e, w.focus_seen);
    ecuio_step_conversations(s, &e, now_us(t));
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        ecuio_publish_support(s, e, c, t->rx_ids[c], w.published[c]);
        ecuio_publish_identity(s, e, c, t->rx_ids[c], w.published_identity[c]);
        ecuio_publish_diagnostics(s, e, c, t->rx_ids[c],
                                  w.published_diagnostics[c]);
    }
    ecuio_publish_timing(s, e, &w.iso, now_us(t), w.timing_at, false);
}

void end_worker(ecuio_socketcan_transport* t, worker_state& w)
{
    ecuio_conversations_end(w.s, &w.e);
    leave_sessions(t);
    ecuio_publish_timing(w.s, w.e, &w.iso, now_us(t), w.timing_at, true);
}

} // namespace

namespace ecuio_socketcan {

/* A requesting session's worker (ADR-0040 sections 3 and 5): each pass
 * expires the engine's waits, judges a request whose echo is late, sends
 * the next request due (one awaiting its echo at a time), waits on the
 * socket until the next event, then takes every frame waiting: an error
 * frame, an echo, or an ECU's frame through ISO-TP, a First Frame
 * answered with Flow Control in the same pass. ISO-TP judges N_Cr on the
 * kernel's receive times, and expires after the reads, so a worker held
 * up (the OS, a slow callback) never drops a reply the bus carried on
 * time. Link health
 * as J2534's: a reply OK, a request unanswered a timeout, an unechoed
 * request a failed write, a bus-off or the interface gone a hard
 * failure; the rate's proof (socketcan_proof_on, socketcan_proof_settle)
 * decides when the rotation starts and when sending stops for the
 * session: after the first echo, nothing more is sent until a reply
 * comes or N_As passes with no error frame. While a request longer than
 * a frame is going out (send), no other request is sent on any channel. */
void request_worker(ecuio_socketcan_transport* t)
{
    worker_state w(t);
    begin_worker(t, w);
    while (!t->stop.load()) {
        link_judge_waiting(w.h);
        const socket_step s = socket_pass(t, w);
        if (s == socket_step::stopped) {
            break;
        }
        if (s == socket_step::again) {
            continue;
        }

        const int64_t now = now_us(t);
        w.sends.expire(now);
        w.sends.send_next(now);

        int ready = 0;
        const socket_step p = wait_pass(t, w, now, &ready);
        if (p == socket_step::stopped) {
            break;
        }
        if (p == socket_step::again) {
            continue;
        }
        read_pass(t, w, ready);
        serve_session(t, w);
    }
    end_worker(t, w);
}

} // namespace ecuio_socketcan

#endif /* ECUIO_HAVE_SOCKETCAN */
