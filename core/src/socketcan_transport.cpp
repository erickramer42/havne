// File/layer: core/src/socketcan_transport.cpp | C++17, Qt-free; Linux only
//             (ECUIO_HAVE_SOCKETCAN), its query keys everywhere
// Testing: UNIT: SocketCanTest.* (the URI's refusals off Linux and on it,
//          before any interface is read; the judges and the rate's proof,
//          socketcan_wire.h); HARNESS: SocketCanBusTest.* (Linux, vcan0:
//          frames in order with their flags, the filter, the mock's drive
//          cycle decoding, quiet and back, the interface down and up, an
//          overflow, error frames, no allocation per frame) and
//          SocketCanRequestTest.* (requests through ISO-TP against the
//          fakes' ECU on vcan0). A failed poll other than EINTR by code
//          reading only: nothing makes the kernel fail one on demand;
//          Reply timing on the kernel's clock (ADR-0042):
//          SocketCanRequestTest.TimingReportsEachRequestOnTheKernelsClock;
//          ISO-TP's N_Cr on the kernel's receive times:
//          SocketCanRequestTest.AReplyOnTimeSurvivesAWorkerHeldUpMidMessage
//          (a worker held inside its reads; one held between passes, which
//          the expiry after the reads covers, by code reading only), and
//          the session's own echoes left out of the clock by the same
//          case run under load, where it failed with them in (ADR-0032's
//          appendix "frames out of order");
//          the rate's proof after the first echo (ADR-0040's appendix of
//          2026-11-02), on
//          vcan0 started unproven (ecuio_socketcan_set_vcan_unproven):
//          SocketCanRequestTest.AnUnproven*. A recording's link lines
//          (ADR-0049) come through link_health.h, tested there
//          (RecorderTest.ALinkTransition*) and through the fake ELM327;
//          these workers' argument by code reading.
//          ECUIO_FRAME_FLAG_REASSEMBLED on a requesting session's replies:
//          SocketCanFaultTest (expect_in_step)
//
// socketcan:// (ADR-0032, ADR-0040): a Linux CAN interface. Attach reads
// the interface through rtnetlink and refuses one a session can't use
// safely. A session whose profile asks nothing listens: the worker reads
// a CAN_RAW socket and dispatches each frame with the kernel's receive
// time, and nothing is sent. One whose profile requests sends each
// request as ISO-TP (isotp.h) through the same socket, answers each First
// Frame with Flow Control, and dispatches whole messages, as the J2534
// and ELM327 transports do; until its first request is echoed and then
// answered, or N_As passes after the echo with no error frame, it sends
// one at a time, and a CAN error before then ends its sending.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "error_detail.h"
#include "session.h"
#include "socketcan_wire.h"
#include "uri_query.h"

extern "C" {
/* did is read only so a uds-generic session is refused for what it is,
 * a profile that requests, not for a key the scheme doesn't take */
const char* const ecuio_socketcan_query_keys[] = {
    "profile", "did",      "baud",     "filter", "txid", "poll",
    "probe",   "identify", "diagnose", "pack",   "scan",   nullptr};
}

#if ECUIO_HAVE_SOCKETCAN

#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_PROTOCOL_ISO15765; plain C */
#include "host_clock.h"
#include "isotp.h"
#include "link_health.h"
#include "request_engine.h"
#include "request_setup.h"
#include "socketcan_netlink.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace {

/* Frames read per wake before the stop flag is looked at again */
constexpr int kReadBurst = 256;
/* The worker's wait: one quiet window of link health */
constexpr int kWaitMs = 100;

static_assert(SOCKETCAN_PROOF_QUIET_US == ECUIO_ISOTP_N_AS_US,
              "the rate's proof waits out N_As after the first echo");

/* ecuio_socketcan_set_vcan_unproven's quiet time; 0: vcan starts proven */
std::atomic<int64_t> g_vcan_unproven_us{0};

/* The OS's words for an errno; may throw bad_alloc */
std::string why(int err)
{
    return std::generic_category().message(err);
}

} // namespace

/* Everything the worker uses is made in attach (the frame off its stack),
 * so the worker allocates nothing */
struct ecuio_socketcan_transport {
    ecuio_session_t* session = nullptr;
    char name[SOCKETCAN_IFNAME_MAX + 1] = {};
    uint32_t want_baud = 0;
    socketcan_filter filters[SOCKETCAN_MAX_FILTERS];
    size_t filter_count = 0;
    /* A session that requests (ADR-0040): its channels and their
     * addressing, the engine, ISO-TP's receiver (timed) and sender, the
     * message frame, and the rate's proof */
    bool requesting = false;
    uint32_t chan_count = 0;
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    uint32_t tx_ids[ECUIO_MAX_CHANNELS] = {};
    bool ext29[ECUIO_MAX_CHANNELS] = {};
    ecuio_request_engine engine;
    std::unique_ptr<ecuio_timing_table> timing; /* the engine's, ADR-0042 */
    bool probe = false;
    bool identify = false;
    std::unique_ptr<isotp_rx> isotp;
    std::unique_ptr<isotp_tx> sender;
    std::unique_ptr<ecuio_frame_t> reply;
    socketcan_proof proof;
    int fd = -1;
    int wake_fd = -1;
    host_clock::time_point epoch;
    std::unique_ptr<ecuio_frame_t> frame;
    std::thread worker;
    std::atomic<bool> stop{false};

    ~ecuio_socketcan_transport()
    {
        if (fd >= 0) {
            close(fd);
        }
        if (wake_fd >= 0) {
            close(wake_fd);
        }
    }
};

namespace {

/* A CAN_RAW socket on the link, set up as ADR-0032 section 4 says: CAN FD,
 * error frames, the kernel's receive time, its drop count, the filters.
 * -1 with errno set on failure. */
int open_socket(const ecuio_socketcan_transport* t, int ifindex)
{
    const int fd =
        socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) {
        return -1;
    }
    const int on = 1;
    const can_err_mask_t errors = CAN_ERR_MASK;
    bool ok =
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof on) == 0 &&
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errors,
                   sizeof errors) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof on) == 0;
    /* A requesting session hears its channels' replies, and its own
     * requests back (the echo, ADR-0040 section 3) */
    socketcan_filter channel_filters[2 * ECUIO_MAX_CHANNELS];
    const socketcan_filter* filters = t->filters;
    size_t filter_count = t->filter_count;
    if (t->requesting) {
        filter_count = socketcan_channel_filters(
            t->rx_ids, t->tx_ids, t->chan_count, channel_filters);
        filters = channel_filters;
        ok = ok && setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &on,
                              sizeof on) == 0;
    }
    if (ok && filter_count > 0) {
        struct can_filter f[2 * ECUIO_MAX_CHANNELS];
        static_assert(SOCKETCAN_MAX_FILTERS <= 2 * ECUIO_MAX_CHANNELS,
                      "either list fits");
        for (size_t i = 0; i < filter_count; ++i) {
            f[i].can_id = filters[i].id;
            f[i].can_mask = filters[i].mask;
        }
        ok =
            setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, f,
                       static_cast<socklen_t>(filter_count * sizeof f[0])) == 0;
    }
    if (ok) {
        struct sockaddr_can addr;
        std::memset(&addr, 0, sizeof addr);
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifindex;
        ok = bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) ==
             0;
    }
    if (!ok) {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

/* The link again, judged as attach judged it; its socket, or -1 while it
 * isn't back (gone, down, no longer listen-only) */
int reopen(const ecuio_socketcan_transport* t)
{
    socketcan_link link;
    bool found = false;
    if (!socketcan_find_link(t->name, &link, &found) || !found) {
        return -1;
    }
    if (t->requesting
            ? socketcan_judge_sender(link, t->want_baud) !=
                  socketcan_send_verdict::ok
            : socketcan_judge(link, t->want_baud) != socketcan_verdict::ok) {
        return -1;
    }
    return open_socket(t, link.index);
}

/* Waits up to ms for the stop; true when it came */
bool wait_for_stop(ecuio_socketcan_transport* t, uint32_t ms)
{
    struct pollfd p = {t->wake_fd, POLLIN, 0};
    (void)poll(&p, 1, static_cast<int>(ms));
    return t->stop.load();
}

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
    link_health h(s->last_error, s->error_count, s->record_link.get());
    socketcan_clock clock;
    uint32_t drops = 0;
    bool overflow_pending = false;
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_diagnostics[ECUIO_MAX_CHANNELS] = {};
    uint32_t diag_seen = 0;
    uint32_t clear_seen[ECUIO_MAX_CHANNELS] = {};
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
                link_on_request_timeout(h);
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
         * floor, ISO-TP's N_Cr; at most one quiet window */
        int64_t next_us = ecuio_engine_next_event_us(&e, now);
        const bool asking = next_us != INT64_MAX;
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
                link_on_read(h, false, false, true); /* a quiet window */
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
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            ecuio_publish_support(s, e, c, t->rx_ids[c], published[c]);
            ecuio_publish_identity(s, e, c, t->rx_ids[c],
                                   published_identity[c]);
            ecuio_publish_diagnostics(s, e, c, t->rx_ids[c],
                                      published_diagnostics[c]);
        }
        ecuio_publish_timing(s, e, &iso, now_us(t), timing_at, false);
    }
    ecuio_publish_timing(s, e, &iso, now_us(t), timing_at, true);
}

void socketcan_worker(ecuio_socketcan_transport* t)
{
    if (t->requesting) {
        request_worker(t);
        return;
    }
    ecuio_session_t* s = t->session;
    link_health h(s->last_error, s->error_count, s->record_link.get());
    socketcan_clock clock;
    ecuio_frame_t* frame = t->frame.get();
    uint32_t drops = 0; /* SO_RXQ_OVFL's count on this socket */
    bool overflow_pending = false;
    alignas(struct canfd_frame) uint8_t raw[CANFD_MTU];
    alignas(struct cmsghdr)
        uint8_t control[CMSG_SPACE(sizeof(struct timespec)) +
                        CMSG_SPACE(sizeof(uint32_t))];

    while (!t->stop.load()) {
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
            drops = 0; /* a new socket counts from zero */
        }

        struct pollfd p[2] = {{t->fd, POLLIN, 0}, {t->wake_fd, POLLIN, 0}};
        const int ready = poll(p, 2, kWaitMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* ENOMEM and kin return at once: polling again would spin.
             * Back off and reopen, as for a failed read. */
            link_on_read(h, true, false, false);
            close(t->fd);
            t->fd = -1;
            continue;
        }
        if (p[1].revents != 0) {
            break;
        }
        if (ready == 0) {
            link_on_read(h, false, false, true); /* a quiet window */
            continue;
        }

        bool heard = false;
        for (int i = 0; i < kReadBurst; ++i) {
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
                /* ENETDOWN, ENODEV: the interface went down or away */
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
            socketcan_raw in;
            std::memcpy(&in.can_id, raw, 4);
            in.len = raw[4];
            in.fd = n == static_cast<ssize_t>(CANFD_MTU);
            in.fd_flags = in.fd ? raw[5] : 0;
            in.data = raw + 8;
            socketcan_to_frame(in, frame);
            const uint64_t host_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    host_clock::now() - t->epoch)
                    .count());
            frame->timestamp_us =
                socketcan_clock_step(clock, kernel_ns, host_us);
            if (overflow_pending) {
                frame->flags |= ECUIO_FRAME_FLAG_OVERFLOW;
                overflow_pending = false;
            }
            ecuio_session_dispatch_callbacks(s, frame);
            if (in.can_id & CAN_ERR_FLAG) {
                if (socketcan_error_is_hard(in.can_id)) {
                    link_on_read(h, true, false,
                                 false); /* bus-off, a restart */
                }
            } else {
                heard = true;
            }
        }
        if (heard) {
            link_on_read(h, false, true, false);
        }
    }
}

ecuio_status_t socketcan_start(ecuio_session_t* session, void* impl)
{
    (void)session;
    ecuio_socketcan_transport* t =
        static_cast<ecuio_socketcan_transport*>(impl);
    if (t == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return ECUIO_OK; /* already running: idempotent */
    }
    try {
        t->worker = std::thread(socketcan_worker, t);
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void socketcan_detach(ecuio_session_t* session, void* impl)
{
    (void)session;
    ecuio_socketcan_transport* t =
        static_cast<ecuio_socketcan_transport*>(impl);
    if (t == nullptr) {
        return;
    }
    t->stop.store(true);
    const uint64_t one = 1;
    (void)!write(t->wake_fd, &one, sizeof one);
    if (t->worker.joinable()) {
        t->worker.join();
    }
    delete t;
}

const ecuio_transport_ops_t kSocketCanTransportOps = {socketcan_start,
                                                      socketcan_detach};

/* The detail for an interface attach won't listen on */
void refuse(const socketcan_link& l, socketcan_verdict v, uint32_t want_baud)
{
    switch (v) {
    case socketcan_verdict::not_can:
        if (l.kind[0] != '\0') {
            ecuio_detail_set("%s isn't a CAN interface (its kind is %s)",
                             l.name, l.kind);
        } else {
            ecuio_detail_set("%s isn't a CAN interface", l.name);
        }
        break;
    case socketcan_verdict::down:
        if (l.is_vcan()) {
            ecuio_detail_set("%s is down: sudo ip link set %s up", l.name,
                             l.name);
        } else {
            ecuio_detail_set("%s is down: sudo ip link set %s up type can "
                             "bitrate <the bus's rate> listen-only on",
                             l.name, l.name);
        }
        break;
    case socketcan_verdict::not_listen_only:
        ecuio_detail_set(
            "%s isn't listen-only, so it could transmit on the bus: sudo "
            "ip link set %s down, then sudo ip link set %s up type can "
            "bitrate %u listen-only on",
            l.name, l.name, l.name, l.has_bitrate ? l.bitrate : 500000u);
        break;
    case socketcan_verdict::wrong_rate:
        ecuio_detail_set("%s runs at %u bit/s, not the %u ?baud= asks for",
                         l.name, l.bitrate, want_baud);
        break;
    case socketcan_verdict::ok:
    default:
        break;
    }
}

/* The detail for an interface a requesting session won't send through
 * (ADR-0040 section 2), with the commands that make it one it would */
void refuse_sender(const socketcan_link& l, socketcan_send_verdict v,
                   uint32_t want_baud)
{
    const unsigned rate =
        want_baud != 0 ? want_baud : (l.has_bitrate ? l.bitrate : 500000u);
    const char* one_shot = SOCKETCAN_SEND_NEEDS_ONE_SHOT ? " one-shot on" : "";
    switch (v) {
    case socketcan_send_verdict::not_can:
        refuse(l, socketcan_verdict::not_can, want_baud);
        break;
    case socketcan_send_verdict::down:
        if (l.is_vcan()) {
            ecuio_detail_set("%s is down: sudo ip link set %s up", l.name,
                             l.name);
        } else {
            ecuio_detail_set("%s is down: sudo ip link set %s up type can "
                             "bitrate <the bus's rate> restart-ms 0%s",
                             l.name, l.name, one_shot);
        }
        break;
    case socketcan_send_verdict::listen_only:
        ecuio_detail_set("%s is listen-only, and this profile sends requests: "
                         "sudo ip link set %s down, then sudo ip link set %s "
                         "up type can bitrate %u restart-ms 0%s",
                         l.name, l.name, l.name, rate, one_shot);
        break;
    case socketcan_send_verdict::no_rate:
        ecuio_detail_set("a profile that sends needs ?baud= with the rate the "
                         "car's bus runs at (%s is up at %u bit/s): stated, "
                         "never assumed",
                         l.name, l.has_bitrate ? l.bitrate : 0u);
        break;
    case socketcan_send_verdict::wrong_rate:
        refuse(l, socketcan_verdict::wrong_rate, want_baud);
        break;
    case socketcan_send_verdict::restarts_itself:
        ecuio_detail_set("%s restarts itself after a bus-off, so a wrong rate "
                         "would go on disturbing the bus: sudo ip link set %s "
                         "down, then sudo ip link set %s up type can bitrate "
                         "%u restart-ms 0%s",
                         l.name, l.name, l.name, rate, one_shot);
        break;
    case socketcan_send_verdict::no_one_shot:
        ecuio_detail_set("%s isn't in one-shot mode, so a frame that fails "
                         "would be sent again by the controller: sudo ip link "
                         "set %s down, then sudo ip link set %s up type can "
                         "bitrate %u restart-ms 0 one-shot on (where its "
                         "driver offers it)",
                         l.name, l.name, l.name, rate);
        break;
    case socketcan_send_verdict::not_error_active:
        ecuio_detail_set("%s already reports bus errors (it isn't "
                         "error-active), so nothing was sent: check the rate "
                         "and the cable, then take it down and up again",
                         l.name);
        break;
    case socketcan_send_verdict::ok:
    default:
        break;
    }
}

} // namespace

extern "C" ecuio_status_t
ecuio_socketcan_transport_attach(ecuio_session_t* session, const char* uri,
                                 const ecuio_uri_query* query)
{
    if (session->transport != nullptr) {
        return ECUIO_OK; /* idempotent */
    }
    const char* body = uri + std::strlen("socketcan://");
    const char* q = std::strchr(body, '?');
    const size_t body_len =
        q != nullptr ? static_cast<size_t>(q - body) : std::strlen(body);
    if (!socketcan_name_ok(body, body_len)) {
        ecuio_detail_set("socketcan:// needs an interface's name, as ip link "
                         "shows it (can0): 1 to 15 letters, digits, '_', '-' "
                         "or '.'");
        return ECUIO_ERR_BAD_URI;
    }

    /* A profile that declares channels asks its ECUs (ADR-0040); any other
     * listens (ADR-0032) */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    const bool requesting = set != nullptr && set->channel_count > 0;

    uint32_t want_baud = 0;
    socketcan_filter filters[SOCKETCAN_MAX_FILTERS];
    size_t filter_count = 0;
    request_params req;
    for (size_t i = 0; i < query->count; ++i) { /* duplicates: last wins */
        const ecuio_uri::param& p = query->params[i];
        const char* wanted = nullptr;
        if (requesting && !p.is("baud")) {
            if (ecuio_request_param(p, &req, &wanted) && wanted != nullptr) {
                ecuio_detail_bad_value(p, wanted);
                return ECUIO_ERR_BAD_URI;
            }
            continue;
        }
        if (!requesting &&
            (p.is("txid") || p.is("poll") || p.is("probe") ||
             p.is("identify") || p.is("diagnose") || p.is("pack") ||
             p.is("scan"))) {
            ecuio_detail_set("?%.*s= belongs to a profile that asks an ECU, "
                             "and this one only listens",
                             static_cast<int>(p.key_len), p.key);
            return ECUIO_ERR_BAD_URI;
        }
        if (p.is("baud")) {
            uint32_t v = 0;
            if (!socketcan_detail::number(p.val, p.val_len, &v) ||
                (v != 500000 && v != 250000)) {
                ecuio_detail_set("?baud= takes 500000 or 250000");
                return ECUIO_ERR_BAD_URI;
            }
            want_baud = v;
        } else if (p.is("filter")) {
            filter_count = socketcan_parse_filters(p.val, p.val_len, filters);
            if (filter_count == 0) {
                ecuio_detail_set("?filter= takes 1 to %d CAN IDs joined by "
                                 "',', each with an optional /mask "
                                 "(0x7E8,0x18DA00F1/0x1FFF00FF)",
                                 static_cast<int>(SOCKETCAN_MAX_FILTERS));
                return ECUIO_ERR_BAD_URI;
            }
        }
    }

    char name[SOCKETCAN_IFNAME_MAX + 1] = {};
    std::memcpy(name, body, body_len);
    socketcan_link link;
    /* Every allocation from here is inside the try: a bad_alloc becomes
     * OUT_OF_MEMORY, never an exception out of ecuio_session_create */
    try {
        bool found = false;
        if (!socketcan_find_link(name, &link, &found)) {
            ecuio_detail_set("couldn't read the network interfaces: %s",
                             why(errno).c_str());
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (!found) {
            ecuio_detail_set("no network interface is named %s", name);
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (requesting) {
            const socketcan_send_verdict verdict =
                socketcan_judge_sender(link, want_baud);
            if (verdict != socketcan_send_verdict::ok) {
                refuse_sender(link, verdict, want_baud);
                return verdict == socketcan_send_verdict::no_rate ||
                               verdict == socketcan_send_verdict::wrong_rate
                           ? ECUIO_ERR_BAD_URI
                           : ECUIO_ERR_LOAD_FAILED;
            }
        } else {
            const socketcan_verdict verdict = socketcan_judge(link, want_baud);
            if (verdict != socketcan_verdict::ok) {
                refuse(link, verdict, want_baud);
                return verdict == socketcan_verdict::wrong_rate
                           ? ECUIO_ERR_BAD_URI
                           : ECUIO_ERR_LOAD_FAILED;
            }
        }

        std::unique_ptr<ecuio_socketcan_transport> impl(
            new ecuio_socketcan_transport());
        impl->session = session;
        std::memcpy(impl->name, name, sizeof impl->name);
        impl->want_baud = want_baud;
        std::copy(filters, filters + filter_count, impl->filters);
        impl->filter_count = filter_count;
        impl->frame.reset(new ecuio_frame_t());
        if (requesting) {
            ecuio_socketcan_transport& r = *impl;
            r.requesting = true;
            r.chan_count = ecuio_resolve_channels(set, req, r.rx_ids, r.tx_ids);
            for (uint32_t c = 0; c < r.chan_count; ++c) {
                r.ext29[c] = ecuio_channel_is_29bit(r.rx_ids[c], r.tx_ids[c]);
            }
            const ecuio_status_t st =
                ecuio_build_requests(session, set, req, r.rx_ids, r.chan_count,
                                     &r.engine, &r.probe, &r.identify);
            if (st != ECUIO_OK) {
                return st;
            }
            ecuio_attach_timing(r.engine, r.timing, ECUIO_TIMING_CLOCK_KERNEL);
            r.isotp.reset(new isotp_rx());
            r.isotp->timed = true;
            r.sender.reset(new isotp_tx());
            r.reply.reset(new ecuio_frame_t());
            r.proof = socketcan_proof_for(link);
            const int64_t unproven_us = g_vcan_unproven_us.load();
            if (unproven_us > 0 && link.is_vcan()) {
                r.proof.proven = false; /* TEST-ONLY: the proof on vcan */
                r.proof.quiet_us = unproven_us;
            }
        }
        impl->fd = open_socket(impl.get(), link.index);
        if (impl->fd < 0) {
            ecuio_detail_set("%s: %s", name, why(errno).c_str());
            return ECUIO_ERR_LOAD_FAILED;
        }
        impl->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (impl->wake_fd < 0) {
            ecuio_detail_set("%s: %s", name, why(errno).c_str());
            return ECUIO_ERR_LOAD_FAILED;
        }
        impl->epoch = host_clock::now();
        session->transport =
            new ecuio_transport_t{&kSocketCanTransportOps, nullptr};
        session->transport->impl = impl.release();
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    session->info.backend = ECUIO_BACKEND_SOCKETCAN;
    session->info.baud = link.is_can() && link.has_bitrate ? link.bitrate : 0;
    if (link.listen_only()) {
        session->info.flags |= ECUIO_SESSION_INFO_LISTEN_ONLY;
    }
    if (requesting) {
        const auto* t = static_cast<const ecuio_socketcan_transport*>(
            session->transport->impl);
        session->info.protocol_id =
            static_cast<uint32_t>(ECUIO_PT_PROTOCOL_ISO15765);
        session->info.channel_count = t->chan_count;
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            session->info.rx_ids[c] = t->rx_ids[c];
            session->info.tx_ids[c] = t->tx_ids[c];
        }
        ecuio_seed_reports(session, t->engine, t->rx_ids, t->chan_count);
    }
    return ECUIO_OK;
}

extern "C" void ecuio_socketcan_set_vcan_unproven(int64_t quiet_us)
{
    g_vcan_unproven_us.store(quiet_us > 0 ? quiet_us : 0);
}

#endif /* ECUIO_HAVE_SOCKETCAN */
