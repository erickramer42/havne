// File/layer: core/src/sim_bus.cpp | C++17, Qt-free; Linux only
//             (ECUIO_HAVE_SOCKETCAN)
// Testing: UNIT: SimTest (test_socketcan); HARNESS: SimBusTest (test_vcan)
//
// The simulated ECU on a virtual bus (ADR-0046): a CAN_RAW socket on a
// vcan interface hearing 0x7DF and each node's request ID, requests
// through each node's own ISO-TP receiver, each answered (sim.cpp's
// answer) and sent through that node's ISO-TP sender on its reply ID,
// broken as the faults say, at the pace the timing says (sim_timing.h),
// and with bus= one frame at a time across every node. One worker
// thread ticks the ECU's clock (and the mock's drive cycle) every 50 ms,
// reads, answers and sends; it allocates nothing.

#include "sim_impl.h"

#if ECUIO_HAVE_SOCKETCAN

#include "error_detail.h"
#include "socketcan_netlink.h"
#include "socketcan_wire.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <time.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

/* Frames read per pass before the stop, the clock and the waiting
 * replies are looked at again, as SocketCAN's kReadBurst */
constexpr int kReadBurst = 256;
constexpr int64_t kTickUs = ECUIO_SIM_TICK_US;
static_assert(kTickUs == static_cast<int64_t>(ecuio_mock::kTickS * 1e6f + 0.5f),
              "one drive cycle step per tick");

int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* One frame out; vcan has no flow control, so a full send queue says
 * ENOBUFS and the write is tried again shortly. False when the bus
 * failed. */
bool put(int fd, const isotp_can& f)
{
    struct can_frame c;
    std::memset(&c, 0, sizeof c);
    c.can_id = f.id;
    c.can_dlc = f.len;
    std::memcpy(c.data, f.data, f.len);
    for (int tries = 0; tries < 1000; ++tries) {
        if (write(fd, &c, sizeof c) == static_cast<ssize_t>(sizeof c)) {
            return true;
        }
        if (errno != ENOBUFS && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

/* One frame out, holding the bus for its time from when it went (bus=):
 * the worker's own work since its wake doesn't shorten a frame */
bool send(ecuio_sim* s, const isotp_can& f)
{
    if (!put(s->fd, f)) {
        return false;
    }
    if (s->bus_frame_us != 0) {
        const int64_t went = now_us();
        s->bus_free_us =
            (s->bus_free_us > went ? s->bus_free_us : went) + s->bus_frame_us;
    }
    return true;
}

/* Whether another frame holds the bus at now (bus=) */
bool bus_held(const ecuio_sim* s, int64_t now)
{
    return s->bus_frame_us != 0 && now < s->bus_free_us;
}

/* Starts n's reply at now: its Single Frame (the send is over) or its
 * First Frame; put off while the bus is held. False when the bus
 * failed. */
bool start(ecuio_sim* s, sim_node& n, int64_t now)
{
    sim_sending& r = n.r;
    isotp_can f;
    if (bus_held(s, now)) {
        r.waiting = true;
        r.start_us = s->bus_free_us;
        return true;
    }
    r.waiting = false;
    if (!isotp_tx_start(n.tx, n.reply_id, r.data, static_cast<uint16_t>(r.len),
                        now, &f)) {
        return true; /* a send under way: dropped, as a busy ECU would */
    }
    if (!send(s, f)) {
        return false;
    }
    if (n.tx.state == isotp_tx_state::idle) {
        sent(s, r);
    } else {
        r.active = true;
        r.held = false;
    }
    return true;
}

/* Sends what n's sender has due at now. False when the bus failed. */
bool pump(ecuio_sim* s, sim_node& n, int64_t now)
{
    sim_sending& r = n.r;
    while (r.active) {
        const bool last =
            n.tx.state == isotp_tx_state::sending && n.tx.len - n.tx.sent <= 7;
        if (last && r.fate == ecuio_sim_fate::stalled && !r.held) {
            r.held = true;
            n.tx.due_us = now + ECUIO_SIM_STALLED_US;
        }
        if (n.tx.state == isotp_tx_state::sending && now >= n.tx.due_us &&
            bus_held(s, now)) {
            n.tx.due_us = s->bus_free_us;
            return true;
        }
        isotp_can f;
        const uint8_t seq = n.tx.seq;
        switch (isotp_tx_next(n.tx, now, &f)) {
        case isotp_tx_step::frame:
            if (n.tx.state == isotp_tx_state::idle &&
                r.fate == ecuio_sim_fate::bad_sequence) {
                f.data[0] = static_cast<uint8_t>(0x20 | ((seq + 1) & 0x0F));
            }
            if (!send(s, f)) {
                return false;
            }
            if (n.tx.state == isotp_tx_state::idle) {
                r.active = false;
                sent(s, r);
            }
            break;
        case isotp_tx_step::timed_out:
            s->unflowed.fetch_add(1, std::memory_order_relaxed);
            r.active = false;
            break;
        case isotp_tx_step::later:
        case isotp_tx_step::idle:
            return true;
        }
    }
    return true;
}

bool yielded(const ecuio_sim* s)
{
    return (s->flags.load(std::memory_order_relaxed) & ECUIO_SIM_YIELDED) != 0;
}

/* Another node sent on a reply ID: an ECU answers there already, so
 * the sim goes silent for good, every reply under way or waiting
 * dropped */
void yield(ecuio_sim* s)
{
    s->flags.fetch_or(ECUIO_SIM_YIELDED, std::memory_order_release);
    for (int i = 0; i < s->node_count; ++i) {
        sim_node& n = s->nodes[i];
        n.r.waiting = false;
        n.r.active = false;
        n.tx.state = isotp_tx_state::idle;
    }
}

/* A request to n while its reply waits (busy=21): 7F <service> 21 as a
 * Single Frame; while its frames are under way, nothing, since a Single
 * Frame on its reply ID would cut its own message. False when the bus
 * failed. */
bool refuse_busy(ecuio_sim* s, sim_node& n, const isotp_message& m, int64_t now)
{
    if (n.timing.busy != ecuio_sim_timing::busy_mode::nrc21 || n.r.active ||
        m.len == 0) {
        return true;
    }
    const uint8_t refusal[3] = {0x7F, m.data[0], 0x21};
    isotp_can f;
    if (!isotp_tx_start(n.tx, n.reply_id, refusal, 3, now, &f)) {
        return true;
    }
    if (!send(s, f)) {
        return false;
    }
    s->replies.fetch_add(1, std::memory_order_relaxed);
    s->refused.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/* A whole request to n: answered, its fate decided, its send started or
 * put off. False when the bus failed. */
bool take(ecuio_sim* s, sim_node& n, const isotp_message& m, int64_t now)
{
    sim_sending& r = n.r;
    s->requests.fetch_add(1, std::memory_order_relaxed);
    if (yielded(s)) {
        return true;
    }
    if (r.active || r.waiting) {
        /* busy with a reply: a tester sends one at a time */
        return refuse_busy(s, n, m, now);
    }
    const bool functional = m.id == ECUIO_SIM_FUNCTIONAL_ID;
    r.len = answer(s, n, m.data, m.len, functional, r.data);
    if (r.len == 0) {
        return true;
    }
    /* Drawn before the fate, so a fault never moves the next draws */
    const int64_t drawn = ecuio_sim_timing::sim_timing_draw(n.timing, n.rng);
    r.fate = decide(s, n, m.data, m.len, r.data, r.len);
    switch (r.fate) {
    case ecuio_sim_fate::silent:
        return true;
    case ecuio_sim_fate::late:
        r.waiting = true;
        r.start_us = now + drawn + ECUIO_SIM_LATE_US;
        return true;
    case ecuio_sim_fate::pending: {
        /* 7F <service> 78 now, a Single Frame; the reply itself later,
         * whole */
        const uint8_t wait[3] = {0x7F, m.data[0], 0x78};
        isotp_can f;
        if (isotp_tx_start(n.tx, n.reply_id, wait, 3, now, &f) && !send(s, f)) {
            return false;
        }
        r.fate = ecuio_sim_fate::intact;
        r.waiting = true;
        r.start_us = now + ECUIO_SIM_PENDING_US;
        return true;
    }
    case ecuio_sim_fate::bad_sequence:
    case ecuio_sim_fate::stalled:
    case ecuio_sim_fate::intact:
        break;
    }
    if (drawn > 0) {
        r.waiting = true;
        r.start_us = now + drawn;
        return true;
    }
    return start(s, n, now);
}

/* One frame for n (its own request ID, or the functional one): through
 * its receiver, a Flow Control sent for a First Frame, a whole request
 * answered. False when the bus failed. */
bool feed(ecuio_sim* s, sim_node& n, const isotp_can& f, int64_t now)
{
    isotp_message m;
    switch (isotp_take(n.rx, f, now, &m)) {
    case isotp_result::first:
        if (yielded(s)) {
            break; /* silent: no Flow Control either */
        }
        if (!send(s, isotp_flow_control(n.reply_id))) {
            return false;
        }
        break;
    case isotp_result::complete:
        if (!take(s, n, m, now)) {
            return false;
        }
        break;
    default:
        break;
    }
    return true;
}

/* The tester's Flow Control frame for n's send: the first Consecutive
 * Frame fc after it, the rest at cf or the tester's STmin, the longer */
void flow(sim_node& n, const isotp_can& f, int64_t now)
{
    const isotp_flow_result fr = isotp_tx_flow(n.tx, f, now);
    if (fr == isotp_flow_result::overflow ||
        fr == isotp_flow_result::abandoned) {
        n.r.active = false;
    } else if (fr == isotp_flow_result::go) {
        const int64_t cf = n.timing.cf_us;
        n.tx.stmin_us = n.tx.stmin_us > cf ? n.tx.stmin_us : cf;
        n.tx.due_us = now + n.timing.fc_us;
    }
}

/* Reads the frames waiting, kReadBurst at most. False when the bus
 * failed. */
bool drain(ecuio_sim* s)
{
    for (int read_n = 0; read_n < kReadBurst; ++read_n) {
        struct can_frame c;
        const ssize_t got = read(s->fd, &c, sizeof c);
        if (got < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        if (got != static_cast<ssize_t>(sizeof c)) {
            continue;
        }
        const int64_t now = now_us();
        isotp_can f;
        f.id = c.can_id & CAN_SFF_MASK;
        f.len = c.can_dlc <= 8 ? c.can_dlc : 8;
        std::memcpy(f.data, c.data, f.len);
        bool replied_on = false;
        for (int i = 0; i < s->node_count; ++i) {
            replied_on = replied_on || f.id == s->nodes[i].reply_id;
        }
        if (replied_on) {
            /* never its own: a CAN_RAW socket isn't sent what it wrote */
            yield(s);
            continue;
        }
        for (int i = 0; i < s->node_count; ++i) {
            sim_node& n = s->nodes[i];
            if (isotp_kind(f) == isotp_pci::flow) {
                if (f.id == n.request_id) {
                    flow(n, f, now);
                }
            } else if ((f.id == n.request_id ||
                        f.id == ECUIO_SIM_FUNCTIONAL_ID) &&
                       !feed(s, n, f, now)) {
                return false;
            }
            if (!pump(s, n, now_us())) {
                return false;
            }
        }
    }
    return true;
}

void run(ecuio_sim* s)
{
    int64_t next_tick = now_us();
    for (;;) {
        int64_t now = now_us();
        while (now >= next_tick) {
            ecuio_sim_tick(s->ecu);
            next_tick += kTickUs;
            if (now - next_tick > 1000000) {
                next_tick = now + kTickUs; /* held up: no catching up */
            }
        }
        int64_t due = next_tick;
        bool failed = false;
        for (int i = 0; i < s->node_count && !failed; ++i) {
            sim_node& n = s->nodes[i];
            if (n.r.waiting && now >= n.r.start_us && !start(s, n, now)) {
                failed = true;
            } else if (!pump(s, n, now)) {
                failed = true;
            }
            (void)isotp_expire(n.rx, now);
            if (n.r.waiting && n.r.start_us < due) {
                due = n.r.start_us;
            }
            if (n.r.active && n.tx.due_us < due) {
                due = n.tx.due_us;
            }
        }
        if (failed) {
            break;
        }
        const int64_t wait_us = due > now ? due - now : 0;
        struct pollfd p[2] = {{s->fd, POLLIN, 0}, {s->stop_fd, POLLIN, 0}};
        /* To the microsecond: a frame's time on the bus is a fraction of
         * a millisecond */
        const timespec wait = {static_cast<time_t>(wait_us / 1000000),
                               static_cast<long>(wait_us % 1000000) * 1000};
        const int got = ppoll(p, 2, &wait, nullptr);
        if (got < 0 && errno != EINTR) {
            break;
        }
        if (p[1].revents != 0) {
            return; /* destroy */
        }
        if ((p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if ((p[0].revents & POLLIN) != 0 && !drain(s)) {
            break;
        }
    }
    s->flags.fetch_or(ECUIO_SIM_FAILED, std::memory_order_release);
}

/* A CAN_RAW socket on ifindex hearing the functional request ID and each
 * node's request and reply IDs (another node answering), 11-bit data
 * frames only, non-blocking; -1 with errno */
int open_socket(int32_t ifindex, const sim_node* nodes, int count)
{
    const int fd =
        socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) {
        return -1;
    }
    struct can_filter f[1 + 2 * (1 + ECUIO_MAX_CHANNELS)];
    int k = 0;
    f[k++].can_id = ECUIO_SIM_FUNCTIONAL_ID;
    for (int i = 0; i < count; ++i) {
        f[k++].can_id = nodes[i].request_id;
        f[k++].can_id = nodes[i].reply_id;
    }
    for (int i = 0; i < k; ++i) {
        f[i].can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    }
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifindex;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, f,
                   static_cast<socklen_t>(k * sizeof f[0])) != 0 ||
        bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

} // namespace

namespace ecuio_sim_impl {

ecuio_status_t open_bus(ecuio_sim* s, const char* ifname, uint32_t options)
{
    if (!socketcan_name_ok(ifname, std::strlen(ifname))) {
        ecuio_detail_set("'%.20s' isn't an interface name", ifname);
        return ECUIO_ERR_LOAD_FAILED;
    }
    socketcan_link link;
    bool found = false;
    if (!socketcan_find_link(ifname, &link, &found)) {
        ecuio_detail_set("couldn't list the network interfaces");
        return ECUIO_ERR_LOAD_FAILED;
    }
    if (!found) {
        ecuio_detail_set("no interface %s (sudo ip link add dev %s type vcan)",
                         ifname, ifname);
        return ECUIO_ERR_LOAD_FAILED;
    }
    const socketcan_sim_verdict v =
        socketcan_judge_sim(link, (options & ECUIO_SIM_ALLOW_DESK_BUS) != 0);
    if (v != socketcan_sim_verdict::ok) {
        char refusal[512];
        socketcan_sim_refusal(link, v, refusal, sizeof refusal);
        ecuio_detail_set("%s", refusal);
        return ECUIO_ERR_LOAD_FAILED;
    }
    s->fd = open_socket(link.index, s->nodes.get(), s->node_count);
    s->stop_fd = s->fd >= 0 ? eventfd(0, EFD_CLOEXEC) : -1;
    if (s->fd < 0 || s->stop_fd < 0) {
        ecuio_detail_set("%s: the socket wouldn't open (%s)", ifname,
                         std::strerror(errno));
        if (s->fd >= 0) {
            close(s->fd);
        }
        return ECUIO_ERR_LOAD_FAILED;
    }
    try {
        s->worker = std::thread(run, s);
    } catch (...) {
        close(s->fd);
        close(s->stop_fd);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

void close_bus(ecuio_sim* sim)
{
    const uint64_t one = 1;
    (void)!write(sim->stop_fd, &one, sizeof one);
    if (sim->worker.joinable()) {
        sim->worker.join();
    }
    close(sim->fd);
    close(sim->stop_fd);
}

} // namespace ecuio_sim_impl

#endif /* ECUIO_HAVE_SOCKETCAN */
