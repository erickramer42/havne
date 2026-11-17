// File/layer: core/src/socketcan_request.cpp | C++17, Qt-free; Linux only
//             (ECUIO_HAVE_SOCKETCAN)
// Testing: HARNESS: SocketCanRequestTest, SocketCanFaultTest (test_vcan)
//
// socketcan://'s requesting worker (ADR-0040): a session whose profile
// requests sends each request as ISO-TP (isotp.h) through the transport's
// CAN_RAW socket, answers each First Frame with Flow Control and
// dispatches whole messages; the rate's proof decides when its rotation
// starts. The transport itself is socketcan_transport.cpp's.

#include "socketcan_transport.h"

#if ECUIO_HAVE_SOCKETCAN

#include "link_health.h"
#include "request_setup.h"

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
 * comes or N_As passes with no error frame. */
void request_worker(ecuio_socketcan_transport* t)
{
    ecuio_session_t* s = t->session;
    ecuio_request_engine& e = t->engine;
    isotp_rx& iso = *t->isotp;
    socketcan_clock clock;
    link_health h(s->last_error, s->error_count, s->record_link.get(), s,
                  t->epoch, &clock.last_out_us, &clock.last_host_us);
    ecuio_unpicked_check unpicked_check;
    uint32_t drops = 0;
    bool overflow_pending = false;
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_diagnostics[ECUIO_MAX_CHANNELS] = {};
    uint32_t diag_seen = 0;
    uint32_t clear_seen[ECUIO_MAX_CHANNELS] = {};
    ecuio_focus_seen focus_seen; /* what ecuio_take_focus took */
    uint32_t next_chan = 0;
    int64_t timing_at = 0;
    sent_request pending;
    alignas(struct canfd_frame) uint8_t raw[CANFD_MTU];
    alignas(struct cmsghdr)
        uint8_t control[CMSG_SPACE(sizeof(struct timespec)) +
                        CMSG_SPACE(sizeof(uint32_t))];
    if (t->probe) {
        ecuio_engine_probe(&e, now_us(t));
    }
    /* A focus set before start applies from the first request */
    ecuio_take_focus(s, &e, focus_seen);

    /* What became of the request awaiting its echo */
    const auto settle = [&](socketcan_sent what) {
        const socketcan_proof_step step =
            socketcan_proof_on(t->proof, what, now_us(t));
        if (what == socketcan_sent::echoed) {
            link_on_write(h, true);
        } else if (pending.active) {
            /* it never reached the bus: not sent */
            ecuio_engine_sent(&e, pending.c, pending.r, false, now_us(t));
            if (step == socketcan_proof_step::carry_on) {
                link_on_write(h, false);
            }
        }
        if (step == socketcan_proof_step::halt) {
            link_on_read(h, true, false, false); /* one episode; no more sent */
        }
        pending.active = false;
    };

    while (!t->stop.load()) {
        link_judge_waiting(h);
        if (t->fd < 0) {
            if (wait_for_stop(t, h.backoff_ms ? h.backoff_ms
                                              : LINK_BACKOFF_BASE_MS)) {
                break;
            }
            t->fd = reopen(t);
            if (t->fd < 0) {
                h.backoff_ms = link_backoff_next(
                    h.backoff_ms ? h.backoff_ms : LINK_BACKOFF_BASE_MS);
                continue;
            }
            drops = 0;
            (void)isotp_end_all(iso);
            pending.active = false;
        }

        int64_t now = now_us(t);
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (ecuio_engine_expire(&e, c, now)) {
                ecuio_request_timed_out(e, h);
            }
        }
        if (pending.active && now - pending.at_us > ECUIO_ISOTP_N_AS_US) {
            settle(socketcan_sent::unechoed);
        }
        (void)socketcan_proof_settle(t->proof, now, false);

        if (!pending.active && !t->proof.halted && !t->proof.confirming) {
            uint32_t c = 0;
            int32_t r = ECUIO_ENGINE_NONE;
            for (uint32_t k = 0; k < e.chan_count && r == ECUIO_ENGINE_NONE;
                 ++k) {
                c = (next_chan + k) % e.chan_count;
                r = ecuio_engine_due(&e, c, now);
            }
            if (r != ECUIO_ENGINE_NONE) {
                next_chan = (c + 1) % e.chan_count;
                const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
                isotp_can f;
                if (isotp_tx_start(*t->sender, t->tx_ids[c], q.bytes,
                                   static_cast<uint16_t>(q.len), now, &f) &&
                    t->sender->state == isotp_tx_state::idle &&
                    write_frame(t->fd, f, t->ext29[c])) {
                    ecuio_engine_sent(&e, c, r, true, now);
                    pending.active = true;
                    pending.c = c;
                    pending.r = r;
                    pending.at_us = now;
                    pending.f = f;
                } else {
                    *t->sender = isotp_tx{};
                    ecuio_engine_sent(&e, c, r, false, now);
                    link_on_write(h, false);
                }
            }
        }

        /* Until the next event: the echo's N_As, the engine's next wait or
         * floor, ISO-TP's N_Cr; at most one quiet window. A halted session
         * sends nothing more, so what the engine calls due isn't one: it
         * waits the window out, still asking, so no quiet read counts */
        const int64_t in_us = ecuio_engine_next_event_us(&e, now);
        const bool asking = in_us != INT64_MAX;
        int64_t next_us = asking && !t->proof.halted ? now + in_us : INT64_MAX;
        if (pending.active) {
            next_us =
                std::min(next_us, pending.at_us + ECUIO_ISOTP_N_AS_US + 1);
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
        struct pollfd p[2] = {{t->fd, POLLIN, 0}, {t->wake_fd, POLLIN, 0}};
        const int ready = poll(p, 2, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            link_on_read(h, true, false, false);
            close(t->fd);
            t->fd = -1;
            continue;
        }
        if (p[1].revents != 0) {
            break;
        }
        if (ready == 0) {
            if (!asking && !pending.active) {
                if (ecuio_engine_unpicked(&e)) {
                    if (!t->proof.halted) { /* a halt's failure stays */
                        ecuio_unpicked_pass(&e, h, unpicked_check, now_us(t));
                    }
                } else {
                    link_on_read(h, false, false, true); /* a quiet window */
                }
            }
        }

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
            now = now_us(t);
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

            if (can_id & CAN_ERR_FLAG) { /* never this socket's own */
                socketcan_raw in;
                in.can_id = can_id;
                in.len = len;
                in.data = raw + 8;
                socketcan_to_frame(in, t->frame.get());
                t->frame->timestamp_us = stamp;
                ecuio_session_dispatch_callbacks(s, t->frame.get());
                if (!t->proof.proven) {
                    settle(socketcan_sent::error);
                } else if (socketcan_error_is_hard(can_id)) {
                    link_on_read(h, true, false, false); /* bus-off */
                }
                continue;
            }
            if (n != static_cast<ssize_t>(CAN_MTU) || len > 8) {
                continue; /* ISO-TP on classic CAN only */
            }
            isotp_can f;
            f.id =
                can_id & (can_id & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK);
            f.len = len;
            std::memcpy(f.data, raw + 8, len);
            if (own) {
                if (pending.active && f.id == pending.f.id &&
                    f.len == pending.f.len &&
                    std::memcmp(f.data, pending.f.data, f.len) == 0) {
                    settle(socketcan_sent::echoed);
                }
                continue; /* a Flow Control frame's echo, or a late one */
            }
            for (uint32_t c = 0; c < t->chan_count; ++c) {
                if (t->rx_ids[c] != f.id ||
                    ((can_id & CAN_EFF_FLAG) != 0) != t->ext29[c]) {
                    continue;
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
                    ecuio_frame_t& reply = *t->reply;
                    reply.timestamp_us = stamp;
                    reply.arbitration_id = m.id;
                    reply.flags =
                        (t->ext29[c] ? ECUIO_FRAME_FLAG_EXTENDED : 0u) |
                        ECUIO_FRAME_FLAG_REASSEMBLED;
                    if (overflow_pending) {
                        reply.flags |= ECUIO_FRAME_FLAG_OVERFLOW;
                        overflow_pending = false;
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
                    (void)ecuio_engine_on_frame(
                        &e, c, reply.data, reply.data_length, now, nullptr);
                    /* an ECU answered at this rate */
                    (void)socketcan_proof_settle(t->proof, now, true);
                    matched = true;
                }
                break;
            }
        }
        if (matched) {
            link_on_read(h, false, true, false);
        }
        (void)isotp_expire(iso, newest >= 0 ? newest : now_us(t));

        if (h.recovered) {
            h.recovered = false;
            if (t->probe) {
                ecuio_engine_probe(&e, now_us(t));
            }
            ecuio_engine_rescan(&e);
        }
        ecuio_take_diag_request(s, &e, diag_seen);
        ecuio_take_clear_requests(s, &e, clear_seen);
        ecuio_take_focus(s, &e, focus_seen);
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            ecuio_publish_support(s, e, c, t->rx_ids[c], published[c]);
            ecuio_publish_identity(s, e, c, t->rx_ids[c],
                                   published_identity[c]);
            ecuio_publish_diagnostics(s, e, c, t->rx_ids[c],
                                      published_diagnostics[c]);
        }
        ecuio_publish_timing(s, e, &iso, now_us(t), timing_at, false);
    }
    leave_sessions(t);
    ecuio_publish_timing(s, e, &iso, now_us(t), timing_at, true);
}

} // namespace ecuio_socketcan

#endif /* ECUIO_HAVE_SOCKETCAN */
