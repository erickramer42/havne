// File/layer: core/src/sim.cpp | C++17, Qt-free; the bus side Linux only
//             (SocketCAN), a refusal elsewhere
// Testing: UNIT: SimTest (test_socketcan): the arguments and the URI's
//          refusals on every platform, NO_TRANSPORT off Linux; HARNESS:
//          SimBusTest (test_vcan, Linux): a requesting socketcan://
//          session against it on vcan0 reads the drive cycle's values,
//          the diagnostics and the VIN, clears, and stays in step under
//          every fault
//
// The simulated ECU on a virtual bus (ADR-0046): a CAN_RAW socket on a
// vcan interface hearing 0x7DF and 0x7E0, requests through core's ISO-TP
// receiver, each answered by sim_ecu.h and sent through core's ISO-TP
// sender as 0x7E8, broken as the faults say. One worker thread steps the
// mock's drive cycle every 50 ms, reads, answers and sends; it allocates
// nothing.

#include "ecuio/sim.h"

#include "ecuio/internal.h" /* ECUIO_HAVE_SOCKETCAN */
#include "error_detail.h"
#include "sim_ecu.h"

#include <atomic>
#include <cstring>
#include <new>

#if ECUIO_HAVE_SOCKETCAN

#include "isotp.h"
#include "socketcan_netlink.h"
#include "socketcan_wire.h"

#include <chrono>
#include <thread>

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

struct ecuio_sim {
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> replies{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> silent{0};
    std::atomic<uint64_t> late{0};
    std::atomic<uint64_t> bad_sequence{0};
    std::atomic<uint64_t> stalled{0};
    std::atomic<uint64_t> unflowed{0};
    std::atomic<uint64_t> clears{0};
    std::atomic<uint32_t> flags{0};
#if ECUIO_HAVE_SOCKETCAN
    ecuio_sim_ecu ecu;
    int fd = -1;
    int stop_fd = -1;
    isotp_rx rx;
    isotp_tx tx;
    std::thread worker;
#endif
};

#if ECUIO_HAVE_SOCKETCAN

namespace {

constexpr int64_t kTickUs = 50000; /* the drive cycle's kTickS */
static_assert(kTickUs == static_cast<int64_t>(ecuio_mock::kTickS * 1e6f + 0.5f),
              "one step per tick");

int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* A reply waiting for its time (late), or under way through the sender */
struct sending {
    uint8_t data[ECUIO_SIM_REPLY_MAX] = {};
    uint32_t len = 0;
    ecuio_sim_fate fate = ecuio_sim_fate::intact;
    bool waiting = false; /* late: not started yet */
    int64_t start_us = 0;
    bool active = false; /* through the sender, Flow Control or frames due */
    bool held = false;   /* stalled: the last frame put off once */
};

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

/* The reply's send is over: count it */
void sent(ecuio_sim* s, const sending& r)
{
    switch (r.fate) {
    case ecuio_sim_fate::bad_sequence:
    case ecuio_sim_fate::stalled:
        break; /* counted when decided: never whole */
    default:
        s->replies.fetch_add(1, std::memory_order_relaxed);
        if (r.len == 3 && r.data[0] == 0x7F) {
            s->refused.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
}

/* Starts r's send at now: its Single Frame (the send is over) or its
 * First Frame. False when the bus failed. */
bool start(ecuio_sim* s, sending& r, int64_t now)
{
    isotp_can f;
    r.waiting = false;
    if (!isotp_tx_start(s->tx, ECUIO_SIM_REPLY_ID, r.data,
                        static_cast<uint16_t>(r.len), now, &f)) {
        return true; /* a send under way: dropped, as a busy ECU would */
    }
    if (!put(s->fd, f)) {
        return false;
    }
    if (s->tx.state == isotp_tx_state::idle) {
        sent(s, r);
    } else {
        r.active = true;
        r.held = false;
    }
    return true;
}

/* Sends what the sender has due at now. False when the bus failed. */
bool pump(ecuio_sim* s, sending& r, int64_t now)
{
    while (r.active) {
        const bool last = s->tx.state == isotp_tx_state::sending &&
                          s->tx.len - s->tx.sent <= 7;
        if (last && r.fate == ecuio_sim_fate::stalled && !r.held) {
            r.held = true;
            s->tx.due_us = now + ECUIO_SIM_STALLED_US;
        }
        isotp_can f;
        const uint8_t seq = s->tx.seq;
        switch (isotp_tx_next(s->tx, now, &f)) {
        case isotp_tx_step::frame:
            if (s->tx.state == isotp_tx_state::idle &&
                r.fate == ecuio_sim_fate::bad_sequence) {
                f.data[0] = static_cast<uint8_t>(0x20 | ((seq + 1) & 0x0F));
            }
            if (!put(s->fd, f)) {
                return false;
            }
            if (s->tx.state == isotp_tx_state::idle) {
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

/* A whole request: answered, its fate decided, its send started or
 * put off. False when the bus failed. */
bool take(ecuio_sim* s, sending& r, const isotp_message& m, int64_t now)
{
    s->requests.fetch_add(1, std::memory_order_relaxed);
    if (r.active || r.waiting) {
        return true; /* busy with a reply: a tester sends one at a time */
    }
    r.len = ecuio_sim_answer(s->ecu, m.data, m.len,
                             m.id == ECUIO_SIM_FUNCTIONAL_ID, r.data);
    if (r.len == 0) {
        return true;
    }
    if (r.data[0] == 0x44) {
        s->clears.fetch_add(1, std::memory_order_relaxed);
    }
    r.fate = ecuio_sim_next_fate(s->ecu, m.data, m.len, r.len);
    switch (r.fate) {
    case ecuio_sim_fate::silent:
        s->silent.fetch_add(1, std::memory_order_relaxed);
        return true;
    case ecuio_sim_fate::late:
        s->late.fetch_add(1, std::memory_order_relaxed);
        r.waiting = true;
        r.start_us = now + ECUIO_SIM_LATE_US;
        return true;
    case ecuio_sim_fate::bad_sequence:
        s->bad_sequence.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::stalled:
        s->stalled.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::intact:
        break;
    }
    return start(s, r, now);
}

/* Reads every frame waiting. False when the bus failed. */
bool drain(ecuio_sim* s, sending& r)
{
    for (;;) {
        struct can_frame c;
        const ssize_t n = read(s->fd, &c, sizeof c);
        if (n < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        if (n != static_cast<ssize_t>(sizeof c)) {
            continue;
        }
        const int64_t now = now_us();
        isotp_can f;
        f.id = c.can_id & CAN_SFF_MASK;
        f.len = c.can_dlc <= 8 ? c.can_dlc : 8;
        std::memcpy(f.data, c.data, f.len);
        if (isotp_kind(f) == isotp_pci::flow) {
            if (f.id == ECUIO_SIM_REQUEST_ID) {
                const isotp_flow_result fr = isotp_tx_flow(s->tx, f, now);
                if (fr == isotp_flow_result::overflow ||
                    fr == isotp_flow_result::abandoned) {
                    r.active = false;
                }
            }
            continue;
        }
        isotp_message m;
        switch (isotp_take(s->rx, f, now, &m)) {
        case isotp_result::first:
            if (!put(s->fd, isotp_flow_control(ECUIO_SIM_REPLY_ID))) {
                return false;
            }
            break;
        case isotp_result::complete:
            if (!take(s, r, m, now)) {
                return false;
            }
            break;
        default:
            break;
        }
        if (!pump(s, r, now_us())) {
            return false;
        }
    }
}

void run(ecuio_sim* s)
{
    sending r;
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
        if (r.waiting && now >= r.start_us && !start(s, r, now)) {
            break;
        }
        if (!pump(s, r, now)) {
            break;
        }
        (void)isotp_expire(s->rx, now);
        int64_t due = next_tick;
        if (r.waiting && r.start_us < due) {
            due = r.start_us;
        }
        if (r.active && s->tx.due_us < due) {
            due = s->tx.due_us;
        }
        const int64_t wait_us = due > now ? due - now : 0;
        struct pollfd p[2] = {{s->fd, POLLIN, 0}, {s->stop_fd, POLLIN, 0}};
        const int got = poll(p, 2, static_cast<int>((wait_us + 999) / 1000));
        if (got < 0 && errno != EINTR) {
            break;
        }
        if (p[1].revents != 0) {
            return; /* destroy */
        }
        if ((p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if ((p[0].revents & POLLIN) != 0 && !drain(s, r)) {
            break;
        }
    }
    s->flags.fetch_or(ECUIO_SIM_FAILED, std::memory_order_release);
}

/* A CAN_RAW socket on ifindex hearing the two request IDs, 11-bit only,
 * non-blocking; -1 with errno */
int open_socket(int32_t ifindex)
{
    const int fd =
        socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) {
        return -1;
    }
    struct can_filter f[2];
    f[0].can_id = ECUIO_SIM_FUNCTIONAL_ID;
    f[1].can_id = ECUIO_SIM_REQUEST_ID;
    f[0].can_mask = f[1].can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifindex;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, f, sizeof f) != 0 ||
        bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

} // namespace

#endif /* ECUIO_HAVE_SOCKETCAN */

extern "C" {

ecuio_status_t ecuio_sim_create(const char* uri, const char* ifname,
                                ecuio_sim_t** out)
{
    ecuio_detail_clear();
    if (uri == nullptr || ifname == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    ecuio_sim_config config;
    char why[256] = {};
    switch (ecuio_sim_parse_uri(uri, &config, why, sizeof why)) {
    case ecuio_sim_uri_result::ok:
        break;
    case ecuio_sim_uri_result::replay:
        ecuio_detail_set("%s", why);
        return ECUIO_ERR_NOT_IMPLEMENTED;
    case ecuio_sim_uri_result::bad:
        ecuio_detail_set("%s", why);
        return ECUIO_ERR_BAD_URI;
    }
#if !ECUIO_HAVE_SOCKETCAN
    ecuio_detail_set("the simulated ECU needs Linux, whose vcan is a "
                     "virtual CAN bus");
    return ECUIO_ERR_NO_TRANSPORT;
#else
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
    if (!link.is_vcan()) {
        ecuio_detail_set("%s is a %s link, not vcan: the simulated ECU "
                         "answers only on a virtual bus, never for a car's "
                         "own ECU",
                         ifname, link.kind[0] != '\0' ? link.kind : "non-CAN");
        return ECUIO_ERR_LOAD_FAILED;
    }
    if (!link.up) {
        ecuio_detail_set("%s is down (sudo ip link set %s up)", ifname, ifname);
        return ECUIO_ERR_LOAD_FAILED;
    }
    ecuio_sim* s = new (std::nothrow) ecuio_sim;
    if (s == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    s->ecu.config = config;
    s->rx.timed = true;
    s->fd = open_socket(link.index);
    s->stop_fd = s->fd >= 0 ? eventfd(0, EFD_CLOEXEC) : -1;
    if (s->fd < 0 || s->stop_fd < 0) {
        ecuio_detail_set("%s: the socket wouldn't open (%s)", ifname,
                         std::strerror(errno));
        if (s->fd >= 0) {
            close(s->fd);
        }
        delete s;
        return ECUIO_ERR_LOAD_FAILED;
    }
    try {
        s->worker = std::thread(run, s);
    } catch (...) {
        close(s->fd);
        close(s->stop_fd);
        delete s;
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = s;
    return ECUIO_OK;
#endif
}

ecuio_status_t ecuio_sim_get_stats(const ecuio_sim_t* sim,
                                   ecuio_sim_stats_t* out)
{
    if (sim == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::memset(out, 0, sizeof *out);
    out->requests = sim->requests.load(std::memory_order_relaxed);
    out->replies = sim->replies.load(std::memory_order_relaxed);
    out->refused = sim->refused.load(std::memory_order_relaxed);
    out->silent = sim->silent.load(std::memory_order_relaxed);
    out->late = sim->late.load(std::memory_order_relaxed);
    out->bad_sequence = sim->bad_sequence.load(std::memory_order_relaxed);
    out->stalled = sim->stalled.load(std::memory_order_relaxed);
    out->unflowed = sim->unflowed.load(std::memory_order_relaxed);
    out->clears = sim->clears.load(std::memory_order_relaxed);
    out->flags = sim->flags.load(std::memory_order_acquire);
    return ECUIO_OK;
}

void ecuio_sim_destroy(ecuio_sim_t* sim)
{
    if (sim == nullptr) {
        return;
    }
#if ECUIO_HAVE_SOCKETCAN
    const uint64_t one = 1;
    (void)!write(sim->stop_fd, &one, sizeof one);
    if (sim->worker.joinable()) {
        sim->worker.join();
    }
    close(sim->fd);
    close(sim->stop_fd);
#endif
    delete sim;
}

} // extern "C"
