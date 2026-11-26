// File/layer: core/src/sim_impl.h | internal, C++17, Qt-free; the bus's
//             members Linux only (ECUIO_HAVE_SOCKETCAN)
// Testing: UNIT: SimAskTest, SimGatewayTest (test_obd2); HARNESS:
//          SimBusTest (test_vcan)
//
// struct ecuio_sim and what its two files share: a node, the reply under
// way, and the answer, fate and count every reply goes through. sim.cpp
// plans, builds and asks on every platform; sim_bus.cpp is the bus's
// socket and worker. Only those files include it.

#ifndef ECUIO_SIM_IMPL_H
#define ECUIO_SIM_IMPL_H

#include "ecuio/sim.h"

#include "ecuio/internal.h" /* ECUIO_HAVE_SOCKETCAN */
#include "isotp.h"
#include "profile_hold.h"
#include "sim_ecu.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>

#if ECUIO_HAVE_SOCKETCAN
#include <thread>
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
    /* The ID it goes on: the node's reply ID, or a module's when the
     * gateway answers for it */
    uint32_t on_id = 0;
};

/* What a node answers as: the engine ECU, a module, or the gateway
 * (sim_gateway_route.h) */
enum class sim_role : uint8_t { engine, module, gateway };

/* One ECU on the bus: the IDs it hears and answers on, its own ISO-TP
 * receiver and sender, its reply, and its faults' periods and counts, so
 * two modules answering one functional request never share a message's
 * state or a fault's count */
struct sim_node {
    uint32_t request_id = ECUIO_SIM_REQUEST_ID;
    uint32_t reply_id = ECUIO_SIM_REPLY_ID;
    sim_role role = sim_role::engine;
    bool behind = false;     /* the gateway judges its requests first */
    ecuio_sim_faults faults; /* every module's, with those aimed at it */
    ecuio_sim_counts counts;
    ecuio_sim_timing::sim_timing timing; /* the same, sim_timing.h */
    /* Its delays' own generator, seeded again at build from the URI */
    std::mt19937_64 rng =
        ecuio_sim_timing::sim_timing_rng(1, ECUIO_SIM_REPLY_ID);
    /* An unbound ask's: when its last reply's last frame goes (busy=) */
    int64_t busy_until_us = 0;
    /* Answers the profile's DIDs (sim_dids.h): Service 22 on the engine
     * ECU, everything on another module */
    bool from_set = false;
    /* The diagnostic session (ADR-0060): the engine ECU with no profile
     * has an extended session, and a profile's nodes under sessions=1;
     * the rest and modules=2's module refuse 10 03 */
    ecuio_sim_session session;
    /* A node hears its own request ID and the functional one */
    isotp_rx_of<2> rx;
    isotp_tx tx;
    sim_sending r;
};

static_assert(ECUIO_SIM_MAX_REPLIES >= 2 * (1 + ECUIO_MAX_CHANNELS),
              "every ECU's reply, each with a 7F xx 78 ahead of it");
/* The gateway answers no functional request itself and refuses aloud
 * only in place of a reply from a module behind it */
static_assert(ECUIO_SIM_BEHIND_MAX <= ECUIO_MAX_CHANNELS,
              "a gateway's refusals aloud stay inside the replies' bound");

/* The engine ECU, a module for each channel, and the gateway */
constexpr int ECUIO_SIM_NODES_MAX = 2 + ECUIO_MAX_CHANNELS;

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
    std::atomic<bool> strict{false}; /* ecuio_sim_test_set_strict */
    /* enhanced=1's bits set_value holds: the mask high, the values low */
    std::atomic<uint64_t> enhanced_held{0};
    /* profile=: the set whose DIDs the ECUs answer (the registry's
     * pointer, valid while the sim is), and each def's physical value,
     * set from any thread (ecuio_sim_set_value) and read by the worker */
    const ecuio_signal_set_t* set = nullptr;
    std::unique_ptr<std::atomic<double>[]> values;
    ecuio_sim_recording recording; /* replay://: the ECU's replies */
    ecuio_sim_ecu ecu;
    /* the engine ECU, then modules=2's module or the profile's, then the
     * gateway: as many as answer, sized by build_sim and never after */
    std::unique_ptr<sim_node[]> nodes;
    int node_count = 1;
    int gateway = -1; /* the gateway's node, -1 for none */
    /* The gateway's counts (the worker's or the ask's alone), and its
     * view, copied under the mutex after each request it routes or
     * answers, for ecuio_sim_get_gateway from any thread */
    ecuio_sim_gateway_tally gateway_tally;
    ecuio_sim_gateway_t gateway_view{};
    mutable std::mutex gateway_mutex;
    /* ecuio_sim_create_unbound: no bus; ecuio_sim_ask answers, ticking
     * the clock to the time it's given (its ticks never go back) */
    bool unbound = false;
    int64_t ask_us = 0; /* the last ask's time: an earlier one is taken as it */
    std::chrono::steady_clock::time_point created =
        std::chrono::steady_clock::now();
    /* bus=: each frame's time on the bus, and when the bus is free again,
     * across every node (the worker's alone) */
    int64_t bus_frame_us = 0;
    int64_t bus_free_us = 0;
#if ECUIO_HAVE_SOCKETCAN
    int fd = -1;
    int stop_fd = -1;
    std::thread worker;
#endif
};

namespace ecuio_sim_impl {

void sent(ecuio_sim* s, const sim_sending& r);
uint32_t answer(ecuio_sim* s, sim_node& n, const uint8_t* req, uint32_t len,
                bool functional, uint8_t* out);
ecuio_sim_fate decide(ecuio_sim* s, sim_node& n, const uint8_t* req,
                      uint32_t len, const uint8_t* reply, uint32_t reply_len);
/* The gateway's verdict on req (at least a byte) for n, a module behind
 * it, counted and its view copied: in the ask and on the bus, before the
 * module's busy check, answer, draw and fate */
ecuio_sim_gateway_verdict route(ecuio_sim* s, const sim_node& n,
                                const uint8_t* req, bool functional);
/* The gateway's view copied under its mutex */
void publish_gateway(ecuio_sim* s);

#if ECUIO_HAVE_SOCKETCAN
/* ecuio_sim_create_with's bus: ifname judged, its socket opened and the
 * worker started; else the detail set and nothing left open */
ecuio_status_t open_bus(ecuio_sim* s, const char* ifname, uint32_t options);
/* The worker stopped and the socket closed */
void close_bus(ecuio_sim* s);
#endif

} // namespace ecuio_sim_impl

using namespace ecuio_sim_impl;

#endif /* ECUIO_SIM_IMPL_H */
