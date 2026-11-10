// File/layer: core/src/sim.cpp | C++17, Qt-free; the bus side Linux only
//             (SocketCAN), a refusal elsewhere; the unbound sim everywhere
// Testing: UNIT: SimTest (test_socketcan); SimRecordingTest, SimAskTest
//          (test_obd2); HARNESS: SimBusTest (test_vcan)
//
// The simulated ECU on a virtual bus (ADR-0046): a CAN_RAW socket on a
// vcan interface hearing 0x7DF and 0x7E0 (and with modules=2 0x7E1, or
// with profile= each of the profile's channels' request IDs),
// requests through each ECU's own ISO-TP receiver, each answered by
// sim_ecu.h and sent through that ECU's ISO-TP sender as 0x7E8 (or
// 0x7E9), broken as the faults say. One worker thread ticks the
// ECU's clock (and the mock's drive cycle) every 50 ms, reads, answers
// and sends; it allocates nothing. A replay:// recording is read whole at
// create (ecuio_sim_load_recording, every platform; a stretch of it alone
// under from= and to=, recording_stretch.h) and only read after.
//
// An unbound sim (ecuio_sim_create_unbound, ADR-0055) has the same ECUs
// and answers with no bus, worker or socket: ecuio_sim_ask ticks its
// clock to the caller's time and returns each ECU's whole reply, its
// fate as the time it starts and a flag.

#include "ecuio/sim.h"

#include "ecuio/internal.h" /* ECUIO_HAVE_SOCKETCAN */
#include "error_detail.h"
#include "frames_csv.h"
#include "profile_hold.h"
#include "sim_dids.h"
#include "sim_ecu.h"

#include "ecuio/profile.h"

#include "isotp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if ECUIO_HAVE_SOCKETCAN

#include "socketcan_netlink.h"
#include "socketcan_wire.h"

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

/* A reply waiting for its time (late, pending), or under way through the
 * sender */
struct sim_sending {
    uint8_t data[ECUIO_SIM_REPLY_MAX] = {};
    uint32_t len = 0;
    ecuio_sim_fate fate = ecuio_sim_fate::intact;
    bool waiting = false; /* late or pending: not started yet */
    int64_t start_us = 0;
    bool active = false; /* through the sender, Flow Control or frames due */
    bool held = false;   /* stalled: the last frame put off once */
};

/* One ECU on the bus: the IDs it hears and answers on, its own ISO-TP
 * receiver and sender, and its reply, so two modules answering one
 * functional request never share a message's state */
struct sim_node {
    uint32_t request_id = ECUIO_SIM_REQUEST_ID;
    uint32_t reply_id = ECUIO_SIM_REPLY_ID;
    /* 0 the engine ECU, 1 another module (its faults counted with every
     * other module's) */
    int module = 0;
    /* Answers the profile's DIDs (sim_dids.h): Service 22 on the engine
     * ECU, everything on another module */
    bool from_set = false;
    /* The diagnostic session (ADR-0060): only the engine ECU with no
     * profile has an extended session; a profile's nodes and modules=2's
     * module refuse 10 03 */
    ecuio_sim_session session;
    isotp_rx rx;
    isotp_tx tx;
    sim_sending r;
};

static_assert(ECUIO_SIM_MAX_REPLIES >= 2 * (1 + ECUIO_MAX_CHANNELS),
              "every ECU's reply, each with a 7F xx 78 ahead of it");

struct ecuio_sim {
    /* The registry's set, held from create (profile_hold.h); first, so
     * it's released after every member that reads it */
    ecuio_profile_hold profile_hold;
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> replies{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> silent{0};
    std::atomic<uint64_t> late{0};
    std::atomic<uint64_t> bad_sequence{0};
    std::atomic<uint64_t> stalled{0};
    std::atomic<uint64_t> unflowed{0};
    std::atomic<uint64_t> clears{0};
    std::atomic<uint64_t> pending{0};
    std::atomic<uint32_t> flags{0};
    /* profile=: the set whose DIDs the ECUs answer (the registry's
     * pointer, valid while the sim is), and each def's physical value,
     * set from any thread (ecuio_sim_set_value) and read by the worker */
    const ecuio_signal_set_t* set = nullptr;
    std::unique_ptr<std::atomic<double>[]> values;
    ecuio_sim_recording recording; /* replay://: the ECU's replies */
    ecuio_sim_ecu ecu;
    /* the engine ECU, then modules=2's module or the profile's */
    sim_node nodes[1 + ECUIO_MAX_CHANNELS];
    int node_count = 1;
    /* ecuio_sim_create_unbound: no bus; ecuio_sim_ask answers, ticking
     * the clock to the time it's given (its ticks never go back) */
    bool unbound = false;
    std::chrono::steady_clock::time_point created =
        std::chrono::steady_clock::now();
#if ECUIO_HAVE_SOCKETCAN
    int fd = -1;
    int stop_fd = -1;
    std::thread worker;
#endif
};

namespace {

/* The reply's send is over: count it */
void sent(ecuio_sim* s, const sim_sending& r)
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

/* n's answer to a whole request, into out: the profile's DIDs, the
 * engine ECU's or modules=2's module's; 0 for none */
uint32_t answer(ecuio_sim* s, sim_node& n, const uint8_t* req, uint32_t len,
                bool functional, uint8_t* out)
{
    const bool has_extended =
        n.module == 0 && !n.from_set && s->ecu.recording == nullptr;
    if (!functional && s->ecu.recording == nullptr) {
        bool handled = false;
        const uint32_t m = ecuio_sim_session_answer(
            n.session, has_extended, s->ecu.time_us, req, len, out, &handled);
        if (handled) {
            return m;
        }
    }
    if (n.from_set && (n.module != 0 || (len > 0 && req[0] == 0x22))) {
        const ecuio_sim* self = s;
        return ecuio_sim_did_answer(
            *s->set, n.reply_id, req, len, functional,
            [self](uint32_t i) {
                return self->values[i].load(std::memory_order_relaxed);
            },
            out, ECUIO_SIM_REPLY_MAX);
    }
    if (n.module == 0) {
        return ecuio_sim_answer(s->ecu, req, len, functional, out,
                                n.session.extended);
    }
    return ecuio_sim_module_answer(req, len, functional, out);
}

/* The fate of n's reply to req, a clear and the fault counted */
ecuio_sim_fate decide(ecuio_sim* s, const sim_node& n, const uint8_t* req,
                      uint32_t len, const uint8_t* reply, uint32_t reply_len)
{
    if (n.module == 0 && reply[0] == 0x44) {
        s->clears.fetch_add(1, std::memory_order_relaxed);
    }
    const ecuio_sim_fate fate =
        ecuio_sim_next_fate(s->ecu, req, len, reply_len, n.module);
    switch (fate) {
    case ecuio_sim_fate::silent:
        s->silent.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::late:
        s->late.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::pending:
        s->pending.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::bad_sequence:
        s->bad_sequence.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::stalled:
        s->stalled.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::intact:
        break;
    }
    return fate;
}

} // namespace

#if ECUIO_HAVE_SOCKETCAN

namespace {

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

/* Starts n's reply at now: its Single Frame (the send is over) or its
 * First Frame. False when the bus failed. */
bool start(ecuio_sim* s, sim_node& n, int64_t now)
{
    sim_sending& r = n.r;
    isotp_can f;
    r.waiting = false;
    if (!isotp_tx_start(n.tx, n.reply_id, r.data, static_cast<uint16_t>(r.len),
                        now, &f)) {
        return true; /* a send under way: dropped, as a busy ECU would */
    }
    if (!put(s->fd, f)) {
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
        isotp_can f;
        const uint8_t seq = n.tx.seq;
        switch (isotp_tx_next(n.tx, now, &f)) {
        case isotp_tx_step::frame:
            if (n.tx.state == isotp_tx_state::idle &&
                r.fate == ecuio_sim_fate::bad_sequence) {
                f.data[0] = static_cast<uint8_t>(0x20 | ((seq + 1) & 0x0F));
            }
            if (!put(s->fd, f)) {
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

/* A whole request to n: answered, its fate decided, its send started or
 * put off. False when the bus failed. */
bool take(ecuio_sim* s, sim_node& n, const isotp_message& m, int64_t now)
{
    sim_sending& r = n.r;
    s->requests.fetch_add(1, std::memory_order_relaxed);
    if (yielded(s) || r.active || r.waiting) {
        /* silent, or busy with a reply: a tester sends one at a time */
        return true;
    }
    const bool functional = m.id == ECUIO_SIM_FUNCTIONAL_ID;
    r.len = answer(s, n, m.data, m.len, functional, r.data);
    if (r.len == 0) {
        return true;
    }
    r.fate = decide(s, n, m.data, m.len, r.data, r.len);
    switch (r.fate) {
    case ecuio_sim_fate::silent:
        return true;
    case ecuio_sim_fate::late:
        r.waiting = true;
        r.start_us = now + ECUIO_SIM_LATE_US;
        return true;
    case ecuio_sim_fate::pending: {
        /* 7F <service> 78 now, a Single Frame; the reply itself later,
         * whole */
        const uint8_t wait[3] = {0x7F, m.data[0], 0x78};
        isotp_can f;
        if (isotp_tx_start(n.tx, n.reply_id, wait, 3, now, &f) &&
            !put(s->fd, f)) {
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
        if (!put(s->fd, isotp_flow_control(n.reply_id))) {
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

/* Reads every frame waiting. False when the bus failed. */
bool drain(ecuio_sim* s)
{
    for (;;) {
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
                    const isotp_flow_result fr = isotp_tx_flow(n.tx, f, now);
                    if (fr == isotp_flow_result::overflow ||
                        fr == isotp_flow_result::abandoned) {
                        n.r.active = false;
                    }
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

#endif /* ECUIO_HAVE_SOCKETCAN */

namespace {

/* What both creates take from the URI: its config and its profile's
 * set, held */
struct sim_plan {
    ecuio_sim_config config;
    const ecuio_signal_set_t* set = nullptr;
    ecuio_profile_hold hold;
    ecuio_sim_module_plan modules;
};

/* The URI and its profile, on every platform */
ecuio_status_t plan_sim(const char* uri, sim_plan* p)
{
    char why[256] = {};
    if (ecuio_sim_parse_uri(uri, &p->config, why, sizeof why) !=
        ecuio_sim_uri_result::ok) {
        ecuio_detail_set("%s", why);
        return ECUIO_ERR_BAD_URI;
    }
    if (p->config.profile_len != 0) {
        const std::string name(p->config.profile, p->config.profile_len);
        const ecuio_status_t found =
            ecuio_profile_acquire(name.c_str(), &p->set);
        p->hold.set = p->set;
        if (found != ECUIO_OK) {
            ecuio_detail_set("profile: no profile '%s' is registered",
                             name.c_str());
            return ECUIO_ERR_UNKNOWN_PROFILE;
        }
        if (p->set != nullptr &&
            !ecuio_sim_plan_modules(*p->set, &p->modules, why, sizeof why)) {
            ecuio_detail_set("profile '%s': %s", name.c_str(), why);
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    p->config.profile = nullptr; /* the URI's, gone after create */
    p->config.profile_len = 0;
    return ECUIO_OK;
}

/* The sim p plans, on no bus yet: its values, its recording and its
 * ECUs */
ecuio_status_t build_sim(sim_plan& p, std::unique_ptr<ecuio_sim>* out)
{
    std::unique_ptr<ecuio_sim> s(new (std::nothrow) ecuio_sim);
    if (s == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    s->profile_hold = std::move(p.hold);
    const ecuio_signal_set_t* set = p.set;
    ecuio_sim_config& config = p.config;
    if (set != nullptr) {
        s->set = set;
        s->values.reset(new (std::nothrow) std::atomic<double>[set->count]);
        if (set->count > 0 && s->values == nullptr) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        /* Until it's given one, each field holds raw 0 */
        for (uint32_t i = 0; i < set->count; ++i) {
            s->values[i].store(set->defs[i].offset, std::memory_order_relaxed);
        }
    }
    if (config.source == ecuio_sim_source::replay) {
        ecuio_status_t loaded = ECUIO_OK;
        try {
            loaded = ecuio_sim_load_recording(
                std::string(config.path, config.path_len), &s->recording,
                config.stretch);
        } catch (const std::bad_alloc&) {
            loaded = ECUIO_ERR_OUT_OF_MEMORY;
        }
        if (loaded != ECUIO_OK) {
            return loaded;
        }
        /* A loop with no span would answer one instant forever */
        if (config.loop && s->recording.span_us == 0) {
            ecuio_detail_set(config.stretch.given()
                                 ? "loop=1 needs a stretch that spans time; "
                                   "its replies all share one time"
                                 : "loop=1 needs a recording that spans "
                                   "time; its replies all share one time");
            return ECUIO_ERR_BAD_URI;
        }
        s->recording.loop = config.loop;
        s->ecu.recording = &s->recording;
    }
    config.path = nullptr; /* the URI's, gone after create */
    config.path_len = 0;
    s->ecu.config = config;
    s->nodes[0].rx.timed = true;
    if (config.second_module) {
        sim_node& m = s->nodes[1];
        m.request_id = ECUIO_SIM_MODULE_REQUEST_ID;
        m.reply_id = ECUIO_SIM_MODULE_REPLY_ID;
        m.module = 1;
        m.rx.timed = true;
        s->node_count = 2;
    }
    if (set != nullptr) {
        s->nodes[0].from_set = p.modules.engine;
        for (uint32_t i = 0; i < p.modules.count; ++i) {
            sim_node& m = s->nodes[1 + i];
            m.request_id = p.modules.tx[i];
            m.reply_id = p.modules.rx[i];
            m.module = 1;
            m.from_set = true;
            m.rx.timed = true;
        }
        s->node_count = 1 + static_cast<int>(p.modules.count);
    }
    *out = std::move(s);
    return ECUIO_OK;
}

/* A reply into the caller's entries, counted whether or not it fits */
void put_reply(ecuio_sim_reply_t* replies, uint32_t capacity, uint32_t* n,
               uint32_t reply_id, uint32_t flags, int64_t delay_us,
               const uint8_t* data, uint32_t len)
{
    if (*n < capacity) {
        ecuio_sim_reply_t& r = replies[*n];
        std::memset(&r, 0, sizeof r);
        r.reply_id = reply_id;
        r.flags = flags;
        r.delay_us = delay_us;
        r.length = len;
        std::memcpy(r.data, data, len);
    }
    ++*n;
}

/* An ask's clock catches up at most this much a call, tick by tick; a
 * longer step moves the ECU's time without stepping the drive cycle
 * through it */
constexpr int64_t kCatchUpUs = 3600LL * 1000000;

} // namespace

ecuio_status_t ecuio_sim_load_recording(const std::string& path,
                                        ecuio_sim_recording* out,
                                        const ecuio_stretch::stretch& stretch)
{
    using namespace ecuio_frames_csv;
    try {
        std::string file = path;
        to_file(file);
        native_path native;
        if (!native.set(file)) {
            ecuio_detail_set("couldn't open the recording's frames.csv");
            return ECUIO_ERR_LOAD_FAILED;
        }
        const std::unique_ptr<FILE, int (*)(FILE*)> f(native.open(),
                                                      &std::fclose);
        if (f == nullptr) {
            ecuio_detail_set("couldn't open the recording's frames.csv");
            return ECUIO_ERR_LOAD_FAILED;
        }
        std::vector<char> line(kLineMax + 2);
        const std::unique_ptr<ecuio_frame_t> frame(new ecuio_frame_t());
        if (!read_header(f.get(), line)) {
            return ECUIO_ERR_LOAD_FAILED;
        }
        ecuio_sim_recording rec;
        row prev, cur;
        uint64_t at_us = 0, rows = 0, played = 0, line_no = 1;
        uint64_t first_us = 0, last_us = 0;
        bool too_long = false;
        bool torn = false;
        ecuio_stretch::cursor in(stretch);
        while (read_line(f.get(), line, &too_long, &torn)) {
            ++line_no;
            if (torn) {
                break; /* a write cut short: left out (ADR-0049 section 7) */
            }
            const char* why = nullptr;
            if (too_long) {
                why = "the line is longer than any frame's row";
            } else if (!parse_row(line.data(), &cur, frame.get(), &why)) {
                /* why is set */
            }
            if (why != nullptr) {
                ecuio_detail_set("frames.csv line %llu: %s",
                                 static_cast<unsigned long long>(line_no), why);
                return ECUIO_ERR_LOAD_FAILED;
            }
            if (rows == 0) {
                first_us = cur.host_us;
            }
            last_us = cur.host_us;
            ++rows;
            if (!in.plays(cur.host_us)) {
                continue;
            }
            if (played > 0) {
                at_us += step_us(prev, cur);
            }
            prev = cur;
            ++played;
            ecuio_sim_recording_take(rec, static_cast<int64_t>(at_us), *frame);
        }
        if (rows > 0 && played == 0) {
            char why[192];
            ecuio_stretch::describe_empty(stretch, first_us, last_us, why,
                                          sizeof why);
            ecuio_detail_set("%s", why);
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (rec.replies.empty()) {
            ecuio_detail_set(stretch.given()
                                 ? "the stretch holds no reply from the "
                                   "engine ECU (0x7E8) to answer with"
                                 : "the recording holds no reply from the "
                                   "engine ECU (0x7E8) to answer with");
            return ECUIO_ERR_LOAD_FAILED;
        }
        *out = std::move(rec);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

extern "C" {

ecuio_status_t ecuio_sim_create(const char* uri, const char* ifname,
                                ecuio_sim_t** out)
{
    return ecuio_sim_create_with(uri, ifname, 0, out);
}

ecuio_status_t ecuio_sim_create_with(const char* uri, const char* ifname,
                                     uint32_t options, ecuio_sim_t** out)
{
    ecuio_detail_clear();
    if (uri == nullptr || ifname == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if ((options & ~ECUIO_SIM_ALLOW_DESK_BUS) != 0) {
        ecuio_detail_set("option bits 0x%x aren't the simulated ECU's",
                         options & ~ECUIO_SIM_ALLOW_DESK_BUS);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    sim_plan p;
    const ecuio_status_t planned = plan_sim(uri, &p);
    if (planned != ECUIO_OK) {
        return planned;
    }
#if !ECUIO_HAVE_SOCKETCAN
    ecuio_detail_set("the simulated ECU needs Linux, whose vcan is a "
                     "virtual CAN bus");
    return ECUIO_ERR_NO_TRANSPORT;
#else
    std::unique_ptr<ecuio_sim> s;
    const ecuio_status_t built = build_sim(p, &s);
    if (built != ECUIO_OK) {
        return built;
    }
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
    s->fd = open_socket(link.index, s->nodes, s->node_count);
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
        s->worker = std::thread(run, s.get());
    } catch (...) {
        close(s->fd);
        close(s->stop_fd);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = s.release();
    return ECUIO_OK;
#endif
}

ecuio_status_t ecuio_sim_create_unbound(const char* uri, uint32_t options,
                                        ecuio_sim_t** out)
{
    ecuio_detail_clear();
    if (uri == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (options != 0) {
        ecuio_detail_set("option bits 0x%x aren't an unbound sim's", options);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    sim_plan p;
    const ecuio_status_t planned = plan_sim(uri, &p);
    if (planned != ECUIO_OK) {
        return planned;
    }
    std::unique_ptr<ecuio_sim> s;
    const ecuio_status_t built = build_sim(p, &s);
    if (built != ECUIO_OK) {
        return built;
    }
    s->unbound = true;
    s->created = std::chrono::steady_clock::now();
    *out = s.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_sim_ask(ecuio_sim_t* sim, uint32_t request_id,
                             const uint8_t* request, uint32_t length,
                             int64_t time_us, ecuio_sim_reply_t* replies,
                             uint32_t capacity, uint32_t* count)
{
    ecuio_detail_clear();
    if (sim == nullptr || request == nullptr || count == nullptr ||
        (replies == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sim->unbound) {
        ecuio_detail_set("this simulated ECU answers on its bus; "
                         "ecuio_sim_create_unbound makes one to ask");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (length == 0 || length > ECUIO_ISOTP_MAX) {
        ecuio_detail_set("a request is 1 to %u bytes, not %u",
                         static_cast<unsigned>(ECUIO_ISOTP_MAX),
                         static_cast<unsigned>(length));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (time_us < ECUIO_SIM_NOW) {
        ecuio_detail_set("time_us counts from create, or is ECUIO_SIM_NOW");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    int64_t t = time_us;
    if (t == ECUIO_SIM_NOW) {
        t = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - sim->created)
                .count());
    }
    ecuio_sim_ecu& e = sim->ecu;
    if (t - e.time_us > kCatchUpUs) {
        e.time_us += (t - e.time_us - kCatchUpUs) / ECUIO_SIM_TICK_US *
                     ECUIO_SIM_TICK_US;
    }
    while (e.time_us + ECUIO_SIM_TICK_US <= t) {
        ecuio_sim_tick(e);
    }

    uint32_t n = 0;
    const bool functional = request_id == ECUIO_SIM_FUNCTIONAL_ID;
    for (int i = 0; i < sim->node_count; ++i) {
        sim_node& node = sim->nodes[i];
        if (!functional && request_id != node.request_id) {
            continue;
        }
        sim->requests.fetch_add(1, std::memory_order_relaxed);
        sim_sending& r = node.r;
        r.len = answer(sim, node, request, length, functional, r.data);
        if (r.len == 0) {
            continue;
        }
        r.fate = decide(sim, node, request, length, r.data, r.len);
        uint32_t flags = 0;
        int64_t delay = 0;
        switch (r.fate) {
        case ecuio_sim_fate::silent:
            continue;
        case ecuio_sim_fate::pending: {
            const uint8_t wait[3] = {0x7F, request[0], 0x78};
            put_reply(replies, capacity, &n, node.reply_id,
                      ECUIO_SIM_REPLY_PENDING, 0, wait, sizeof wait);
            delay = ECUIO_SIM_PENDING_US;
            break;
        }
        case ecuio_sim_fate::late:
            flags = ECUIO_SIM_REPLY_LATE;
            delay = ECUIO_SIM_LATE_US;
            break;
        case ecuio_sim_fate::bad_sequence:
            flags = ECUIO_SIM_REPLY_BAD_SEQUENCE;
            break;
        case ecuio_sim_fate::stalled:
            flags = ECUIO_SIM_REPLY_STALLED;
            break;
        case ecuio_sim_fate::intact:
            break;
        }
        put_reply(replies, capacity, &n, node.reply_id, flags, delay, r.data,
                  r.len);
        sent(sim, r);
    }
    *count = n;
    return ECUIO_OK;
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
    out->pending = sim->pending.load(std::memory_order_relaxed);
    out->flags = sim->flags.load(std::memory_order_acquire);
    return ECUIO_OK;
}

ecuio_status_t ecuio_sim_set_value(ecuio_sim_t* sim, uint16_t signal_id,
                                   double value)
{
    ecuio_detail_clear();
    if (sim == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (sim->set == nullptr) {
        ecuio_detail_set("the simulated ECU answers no profile's DIDs "
                         "(no profile= in its URI)");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < sim->set->count; ++i) {
        const ecuio_signal_def_t& d = sim->set->defs[i];
        if (d.id != signal_id || !ecuio_sim_did_def(d)) {
            continue;
        }
        uint32_t raw = 0;
        if (!ecuio_sim_raw_for(d, value, &raw)) {
            ecuio_detail_set(ecuio_sim_text_def(d)
                                 ? "signal %u is text: it answers its name"
                                 : "signal %u's field can't hold that value",
                             static_cast<unsigned>(signal_id));
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        sim->values[i].store(value, std::memory_order_relaxed);
        return ECUIO_OK;
    }
    ecuio_detail_set("signal %u isn't a Service 22 DID's field of the profile",
                     static_cast<unsigned>(signal_id));
    return ECUIO_ERR_INVALID_ARGUMENT;
}

void ecuio_sim_destroy(ecuio_sim_t* sim)
{
    if (sim == nullptr) {
        return;
    }
#if ECUIO_HAVE_SOCKETCAN
    if (!sim->unbound) {
        const uint64_t one = 1;
        (void)!write(sim->stop_fd, &one, sizeof one);
        if (sim->worker.joinable()) {
            sim->worker.join();
        }
        close(sim->fd);
        close(sim->stop_fd);
    }
#endif
    delete sim;
}

} // extern "C"
