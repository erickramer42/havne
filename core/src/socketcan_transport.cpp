// File/layer: core/src/socketcan_transport.cpp | C++17, Qt-free; Linux only
//             (ECUIO_HAVE_SOCKETCAN), its query keys everywhere
// Testing: UNIT — SocketCanTest.* (the URI's refusals off Linux and on it,
//          before any interface is read); HARNESS — VcanTest.SocketCan*
//          (Linux, vcan0: frames in order with their flags, the filter,
//          the mock's drive cycle decoding, quiet and back, the interface
//          down and up, an overflow, no allocation per frame)
//
// socketcan:// (ADR-0032): a Linux CAN interface, listening. Attach reads
// the interface through rtnetlink and refuses one a session can't listen
// on safely; the worker reads a CAN_RAW socket and dispatches each frame
// with the kernel's receive time. Nothing is ever sent.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "error_detail.h"
#include "session.h"
#include "socketcan_wire.h"
#include "uri_query.h"

extern "C" {
/* did is read only so a uds-generic session is refused for what it is,
 * a profile that requests, not for a key the scheme doesn't take */
const char* const ecuio_socketcan_query_keys[] = {"profile", "did", "baud",
                                                  "filter", nullptr};
}

#if ECUIO_HAVE_SOCKETCAN

#include "link_health.h"
#include "socketcan_netlink.h"
#include "host_clock.h"

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

/* A CAN_RAW socket on the link, set up as ADR-0032 §4 says: CAN FD,
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
    if (ok && t->filter_count > 0) {
        struct can_filter f[SOCKETCAN_MAX_FILTERS];
        for (size_t i = 0; i < t->filter_count; ++i) {
            f[i].can_id = t->filters[i].id;
            f[i].can_mask = t->filters[i].mask;
        }
        ok = setsockopt(
                 fd, SOL_CAN_RAW, CAN_RAW_FILTER, f,
                 static_cast<socklen_t>(t->filter_count * sizeof f[0])) == 0;
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
    if (!socketcan_find_link(t->name, &link, &found) || !found ||
        socketcan_judge(link, t->want_baud) != socketcan_verdict::ok) {
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

void socketcan_worker(ecuio_socketcan_transport* t)
{
    ecuio_session_t* s = t->session;
    link_health h(s->last_error, s->error_count);
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
            continue; /* EINTR */
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

    uint32_t want_baud = 0;
    socketcan_filter filters[SOCKETCAN_MAX_FILTERS];
    size_t filter_count = 0;
    for (size_t i = 0; i < query->count; ++i) { /* duplicates: last wins */
        const ecuio_uri::param& p = query->params[i];
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

    /* A profile that requests can't run on a socket that only listens,
     * and its replies arrive as ISO-TP frames its defs don't read */
    const ecuio_signal_set_t* set = session->active_signal_set;
    if (set != nullptr && set->channel_count > 0) {
        ecuio_detail_set("this profile asks an ECU, and socketcan:// only "
                         "listens: requests on SocketCAN need ISO-TP, which "
                         "isn't built yet; use ?profile=raw");
        return ECUIO_ERR_NOT_IMPLEMENTED;
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
        const socketcan_verdict verdict = socketcan_judge(link, want_baud);
        if (verdict != socketcan_verdict::ok) {
            refuse(link, verdict, want_baud);
            return verdict == socketcan_verdict::wrong_rate
                       ? ECUIO_ERR_BAD_URI
                       : ECUIO_ERR_LOAD_FAILED;
        }

        std::unique_ptr<ecuio_socketcan_transport> impl(
            new ecuio_socketcan_transport());
        impl->session = session;
        std::memcpy(impl->name, name, sizeof impl->name);
        impl->want_baud = want_baud;
        std::copy(filters, filters + filter_count, impl->filters);
        impl->filter_count = filter_count;
        impl->frame.reset(new ecuio_frame_t());
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
    return ECUIO_OK;
}

#endif /* ECUIO_HAVE_SOCKETCAN */
