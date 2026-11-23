// File/layer: core/src/socketcan_wire.h | internal, not ABI-exported;
//             included by socketcan_transport.cpp and socketcan_interfaces.cpp
// C++17, Qt-free, no platform headers: the kernel's values are mirrored
// here (and checked against <linux/can.h> and <linux/rtnetlink.h> where
// those exist), so every rule below is a unit test on every platform.
// Testing: UNIT: SocketCanTest (test_socketcan); HARNESS:
//          SocketCanRequestTest (test_vcan)
//
// SocketCAN listening (ADR-0032): what a session reads from the kernel,
// with no I/O.

#ifndef ECUIO_SOCKETCAN_WIRE_H
#define ECUIO_SOCKETCAN_WIRE_H

#include "ecuio/core.h"
#include "ecuio/socketcan.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "socketcan_link.h"

/* ---- The simulated ECU's bus (ADR-0046 section 2, its appendix "the
 * desk bus") ---- */

/* Whether the simulated ECU may answer on the link, in the order the
 * refusals are checked */
enum class socketcan_sim_verdict {
    ok,
    not_can,         /* neither a CAN controller nor vcan */
    real_controller, /* a controller, and no desk bus said */
    down,
    listen_only,      /* a controller that can't transmit, or no mode */
    not_error_active, /* the controller already counts errors */
};

/* vcan always, once up. A controller only on a desk bus, said by the
 * caller: up, able to transmit and error-active, at any rate and with or
 * without one-shot mode, since a desk bus has no car to harm */
inline socketcan_sim_verdict socketcan_judge_sim(const socketcan_link& l,
                                                 bool desk_bus)
{
    if (!l.is_can() && !l.is_vcan()) {
        return socketcan_sim_verdict::not_can;
    }
    if (l.is_can() && !desk_bus) {
        return socketcan_sim_verdict::real_controller;
    }
    if (!l.up) {
        return socketcan_sim_verdict::down;
    }
    if (l.is_vcan()) {
        return socketcan_sim_verdict::ok;
    }
    if (!l.has_ctrlmode || l.listen_only()) {
        return socketcan_sim_verdict::listen_only;
    }
    if (!l.has_state || l.state != SOCKETCAN_STATE_ERROR_ACTIVE) {
        return socketcan_sim_verdict::not_error_active;
    }
    return socketcan_sim_verdict::ok;
}

/* Why the simulated ECU won't answer on the link, with the commands that
 * would change that, into out; "" for ok */
inline void socketcan_sim_refusal(const socketcan_link& l,
                                  socketcan_sim_verdict v, char* out,
                                  size_t cap)
{
    out[0] = '\0';
    char rate[32];
    if (l.has_bitrate && l.bitrate != 0) {
        snprintf(rate, sizeof rate, "%u", l.bitrate);
    } else {
        snprintf(rate, sizeof rate, "<the bus's rate>");
    }
    switch (v) {
    case socketcan_sim_verdict::not_can:
        snprintf(out, cap,
                 "%s is a %s link, not vcan: the simulated ECU answers "
                 "only on a virtual bus, never for a car's own ECU",
                 l.name, l.kind[0] != '\0' ? l.kind : "non-CAN");
        break;
    case socketcan_sim_verdict::real_controller:
        snprintf(out, cap,
                 "%s is a CAN controller, not vcan: the simulated ECU "
                 "answers only on a virtual bus, never for a car's own "
                 "ECU, unless the caller says it's a desk bus with no car",
                 l.name);
        break;
    case socketcan_sim_verdict::down:
        if (l.is_vcan()) {
            snprintf(out, cap, "%s is down (sudo ip link set %s up)", l.name,
                     l.name);
        } else {
            snprintf(out, cap,
                     "%s is down: sudo ip link set %s up type can bitrate "
                     "%s listen-only off",
                     l.name, l.name, rate);
        }
        break;
    case socketcan_sim_verdict::listen_only:
        snprintf(out, cap,
                 "%s is listen-only, so the simulated ECU couldn't answer: "
                 "sudo ip link set %s down, then sudo ip link set %s up "
                 "type can bitrate %s listen-only off",
                 l.name, l.name, l.name, rate);
        break;
    case socketcan_sim_verdict::not_error_active: {
        /* the kernel's enum can_state, one below the public one */
        const char* word = l.has_state ? socketcan_state_word(l.state + 1) : "";
        if (word[0] != '\0') {
            snprintf(out, cap,
                     "%s is %s, so the bus already counts errors: check the "
                     "rate and the cable, then sudo ip link set %s down and "
                     "up again",
                     l.name, word, l.name);
        } else {
            snprintf(out, cap,
                     "%s reports no controller state, so it isn't known to "
                     "be error-active: sudo ip link set %s down and up again",
                     l.name, l.name);
        }
        break;
    }
    case socketcan_sim_verdict::ok:
    default:
        break;
    }
}

/* ---- The first request proves the rate (ADR-0040 section 3) ---- */

/* Requests that went out unseen (no echo, no error within N_As) before a
 * session stops sending: a frame that lost arbitration goes again */
constexpr uint8_t SOCKETCAN_PROOF_TRIES = 3;

/* How long after the first echo an error frame still unproves the rate:
 * N_As (ISO 15765-2), which socketcan_transport.cpp asserts equal */
constexpr int64_t SOCKETCAN_PROOF_QUIET_US = 25000;

/* What became of a request the worker wrote */
enum class socketcan_sent {
    echoed,   /* the controller sent it: not proof that a node acknowledged
                 it (one adapter echoes first, the error frame after) */
    unechoed, /* no echo and no error within N_As */
    error     /* an error frame, or the controller past error-active */
};

/* What the worker does next */
enum class socketcan_proof_step {
    carry_on,   /* nothing changes: the rotation goes on, or stays halted */
    confirming, /* echoed: nothing more is sent until the proof settles */
    proven,     /* the rate is proven: the rotation starts */
    send_again, /* unproven, unseen: the same request again */
    halt        /* nothing more is sent this session */
};

struct socketcan_proof {
    bool proven = false;
    bool halted = false;
    bool confirming = false; /* echoed; an error frame may still follow */
    uint8_t unechoed = 0;    /* in a row, before the proof */
    int64_t echoed_us = 0;   /* when the confirming echo was read */
    int64_t quiet_us = SOCKETCAN_PROOF_QUIET_US; /* a test may lengthen it */
};

/* vcan has no rate to get wrong: a session on it starts proven */
inline socketcan_proof socketcan_proof_for(const socketcan_link& l)
{
    socketcan_proof p;
    p.proven = l.is_vcan();
    return p;
}

/* Until the rate is proven, the first error or the third request unseen
 * halts the session, and an echo starts the confirmation
 * (socketcan_proof_settle): a controller may echo a frame before any node
 * acknowledged it (the desk bus's adapter reported the missing ACK 1.5 ms
 * after its echo, ADR-0040's appendix of 2026-11-02), so an error frame
 * within the quiet time still halts. Once proven, an unechoed request is a
 * failed write and an error the link health's, so nothing here applies */
inline socketcan_proof_step
socketcan_proof_on(socketcan_proof& p, socketcan_sent what, int64_t now_us)
{
    if (p.halted || p.proven) {
        return socketcan_proof_step::carry_on;
    }
    if (p.confirming) {
        if (what == socketcan_sent::error) {
            p.confirming = false;
            p.halted = true;
            return socketcan_proof_step::halt;
        }
        return socketcan_proof_step::carry_on;
    }
    switch (what) {
    case socketcan_sent::echoed:
        p.confirming = true;
        p.echoed_us = now_us;
        p.unechoed = 0;
        return socketcan_proof_step::confirming;
    case socketcan_sent::unechoed:
        if (++p.unechoed < SOCKETCAN_PROOF_TRIES) {
            return socketcan_proof_step::send_again;
        }
        p.halted = true;
        return socketcan_proof_step::halt;
    case socketcan_sent::error:
    default:
        p.halted = true;
        return socketcan_proof_step::halt;
    }
}

/* A confirming echo proves the rate once a reply came (an ECU answered
 * at this rate) or the quiet time passed with no error frame */
inline socketcan_proof_step
socketcan_proof_settle(socketcan_proof& p, int64_t now_us, bool answered)
{
    if (!p.confirming || (!answered && now_us - p.echoed_us <= p.quiet_us)) {
        return socketcan_proof_step::carry_on;
    }
    p.confirming = false;
    p.proven = true;
    return socketcan_proof_step::proven;
}

/* ---- A requesting session's filters ---- */

/* The reply ID (its ECU's frames) and request ID (the echo of its own)
 * of each channel whose bit hearing sets (the modules open or closing),
 * exact, each of its width; out holds 2 * n. */
inline size_t socketcan_channel_filters(const uint32_t* rx_ids,
                                        const uint32_t* tx_ids, size_t n,
                                        uint32_t hearing, socketcan_filter* out)
{
    size_t k = 0;
    auto add = [&](uint32_t id) {
        /* as ?filter= sets one: the width flag in the ID and the mask */
        const bool extended = id > SOCKETCAN_SFF_MASK;
        const uint32_t flagged = id | (extended ? SOCKETCAN_EFF_FLAG : 0);
        for (size_t i = 0; i < k; ++i) {
            if (out[i].id == flagged) {
                return;
            }
        }
        out[k].id = flagged;
        out[k].mask = (extended ? SOCKETCAN_EFF_MASK : SOCKETCAN_SFF_MASK) |
                      SOCKETCAN_EFF_FLAG;
        ++k;
    };
    for (size_t c = 0; c < n && c < 32; ++c) {
        if (((hearing >> c) & 1u) != 0) {
            add(rx_ids[c]);
            add(tx_ids[c]);
        }
    }
    return k;
}

/* ---- A kernel frame to an ecuio_frame_t ---- */

/* struct can_frame / canfd_frame: can_id, then the length, then (FD)
 * its flags, two reserved bytes, and the data at offset 8 */
struct socketcan_raw {
    uint32_t can_id = 0;
    uint8_t len = 0;
    uint8_t fd_flags = 0;
    bool fd = false; /* read as a CANFD_MTU frame */
    const uint8_t* data = nullptr;
};

/* Fills *out (zeroed first) from a kernel frame; the timestamp is the
 * caller's. An error frame keeps its class bits as the ID and its 8
 * bytes of detail. */
inline void socketcan_to_frame(const socketcan_raw& in, ecuio_frame_t* out)
{
    out->timestamp_us = 0;
    out->channel_id = 0;
    out->flags = 0;
    out->reserved = 0;
    const size_t cap = in.fd ? 64 : 8;
    const size_t n = in.len < cap ? in.len : cap;
    if (in.can_id & SOCKETCAN_ERR_FLAG) {
        out->arbitration_id = in.can_id & SOCKETCAN_EFF_MASK;
        out->flags |= ECUIO_FRAME_FLAG_ERROR;
    } else if (in.can_id & SOCKETCAN_EFF_FLAG) {
        out->arbitration_id = in.can_id & SOCKETCAN_EFF_MASK;
        out->flags |= ECUIO_FRAME_FLAG_EXTENDED;
    } else {
        out->arbitration_id = in.can_id & SOCKETCAN_SFF_MASK;
    }
    if (in.can_id & SOCKETCAN_RTR_FLAG && !(in.can_id & SOCKETCAN_ERR_FLAG)) {
        out->flags |= ECUIO_FRAME_FLAG_REMOTE;
    }
    if (in.fd) {
        out->flags |= ECUIO_FRAME_FLAG_FD;
        if (in.fd_flags & SOCKETCAN_FD_BRS) {
            out->flags |= ECUIO_FRAME_FLAG_BITRATE_SWITCH;
        }
        if (in.fd_flags & SOCKETCAN_FD_ESI) {
            out->flags |= ECUIO_FRAME_FLAG_FD_ESI;
        }
    }
    /* A remote frame's length is the one it asks for: it carries no data */
    const bool remote = (out->flags & ECUIO_FRAME_FLAG_REMOTE) != 0;
    out->data_length = remote ? 0 : static_cast<uint16_t>(n);
    if (!remote && n > 0 && in.data != nullptr) {
        memcpy(out->data, in.data, n);
    }
}

/* An error frame that ends reception: bus-off, or the controller
 * restarting after one (ADR-0032 section 6) */
inline bool socketcan_error_is_hard(uint32_t can_id)
{
    return (can_id & SOCKETCAN_ERR_FLAG) &&
           (can_id & (SOCKETCAN_ERR_BUSOFF | SOCKETCAN_ERR_RESTARTED)) != 0;
}

/* ---- The clock (ADR-0032 section 5) ---- */

/* A frame is received before it's read, so a kernel delta that puts it
 * later than the read by more than this, or a backward one, means the
 * wall clock stepped: the host's delta stands in */
constexpr uint64_t SOCKETCAN_CLOCK_SLACK_US = 1000000;

/* Frames reach a socket nearly in the order the kernel stamped them, not
 * exactly: each CPU queues the frames it sends, so a frame can be read
 * after one stamped later (ADR-0032's appendix "frames out of order").
 * A backward delta up to this is such a frame, not a step */
constexpr uint64_t SOCKETCAN_CLOCK_REORDER_US = 100000;

struct socketcan_clock {
    bool started = false;
    uint64_t last_kernel_ns = 0;
    uint64_t last_host_us = 0;
    uint64_t last_out_us = 0;
    /* Frames given the host's delta (no kernel time, or a step): an
     * interval across one is part host time (ADR-0042) */
    uint32_t host_steps = 0;
};

/* A frame's session time from the kernel's receive time (ns, wall clock;
 * 0 when the frame came without one) and the session's host time at the
 * read (us since attach). The first frame takes the host's time; each
 * later one adds the kernel's delta since the newest frame before,
 * unless it stepped. Frames read late after a stall keep their kernel
 * spacing; one stamped up to SOCKETCAN_CLOCK_REORDER_US before the
 * newest takes the newest's time, so a stall never enters the spacing
 * of frames read out of order across it. Never decreases. */
inline uint64_t socketcan_clock_step(socketcan_clock& c, uint64_t kernel_ns,
                                     uint64_t host_us)
{
    if (!c.started) {
        c.started = true;
        c.last_kernel_ns = kernel_ns;
        c.last_host_us = host_us;
        c.last_out_us = host_us;
        return host_us;
    }
    if (kernel_ns != 0 && kernel_ns < c.last_kernel_ns &&
        (c.last_kernel_ns - kernel_ns) / 1000 <= SOCKETCAN_CLOCK_REORDER_US) {
        c.last_host_us = host_us; /* the kernel's newest stays */
        return c.last_out_us;
    }
    const uint64_t host_delta =
        host_us >= c.last_host_us ? host_us - c.last_host_us : 0;
    uint64_t delta = host_delta;
    bool kernel = false;
    if (kernel_ns != 0 && c.last_kernel_ns != 0 &&
        kernel_ns >= c.last_kernel_ns) {
        const uint64_t kernel_delta = (kernel_ns - c.last_kernel_ns) / 1000;
        if (c.last_out_us + kernel_delta <=
            host_us + SOCKETCAN_CLOCK_SLACK_US) {
            delta = kernel_delta;
            kernel = true;
        }
    }
    if (!kernel) {
        ++c.host_steps;
    }
    c.last_kernel_ns = kernel_ns;
    c.last_host_us = host_us;
    c.last_out_us += delta;
    return c.last_out_us;
}

#endif /* ECUIO_SOCKETCAN_WIRE_H */
