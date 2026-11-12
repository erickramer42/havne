// File/layer: core/src/mock_pids.h | internal, C++17, Qt-free, header-only
// Testing: UNIT: MockPidsTest, CoreTest (test_core)
//
// The demo car's Service 01 values (mock://, under a profile that reads
// PIDs): the simulated ECU of sim_ecu.h answers from the mock's own drive
// cycle, so the supported-PID walk lists what it answers and each tick
// hands on a reply for each of the set's PIDs among them, as a requesting
// transport hands on an ECU's replies. A PID the ECU lacks is left out by
// its bitmaps, so core lists its signals as not sent (ADR-0039 section 6).

#ifndef ECUIO_MOCK_PIDS_H
#define ECUIO_MOCK_PIDS_H

#include "ecuio/core.h"
#include "ecuio/signals.h"
#include "mock_ecu.h"
#include "pid_support.h"
#include "request_engine.h"
#include "sim_ecu.h"

#include <cstdint>
#include <cstring>

/* The set's Service 01 PIDs on the mock's ECU that it answers, each once,
 * in the set's order */
struct ecuio_mock_pids {
    uint8_t pid[256] = {};
    uint32_t count = 0;
};

/* A Service 01 request to the ECU at its present state: its reply into
 * out (ECUIO_SIM_REPLY_MAX bytes), its length; 0 for a refusal or none */
inline uint32_t ecuio_mock_pid_reply(ecuio_sim_ecu& ecu, uint8_t pid,
                                     uint8_t* out)
{
    const uint8_t req[2] = {0x01, pid};
    const uint32_t n = ecuio_sim_answer(ecu, req, 2, false, out);
    return (n >= 3 && out[0] == 0x41) ? n : 0;
}

/* The set's Service 01 defs on the ECU's reply ID whose PID it answers
 * with data (never a bitmap) */
inline void ecuio_mock_pids_of(const ecuio_signal_set_t* set,
                               ecuio_sim_ecu& ecu, ecuio_mock_pids* out)
{
    *out = ecuio_mock_pids{};
    bool seen[256] = {};
    for (uint32_t i = 0;
         set != nullptr && set->defs != nullptr && i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (!ecuio_service01_keyed(d) || d.message_id != ECUIO_MOCK_ECU_RX_ID) {
            continue;
        }
        const uint8_t pid = static_cast<uint8_t>(d.mux_key);
        uint8_t reply[ECUIO_SIM_REPLY_MAX];
        if (seen[pid] || pid % 0x20u == 0 ||
            ecuio_mock_pid_reply(ecu, pid, reply) == 0) {
            continue;
        }
        seen[pid] = true;
        out->pid[out->count++] = pid;
    }
}

/* Each PID's reply at the ECU's present state, into frame (the caller's:
 * 4 KB) and through deliver(frame): 41 <PID> <data> on the ECU's reply ID
 * at timestamp_us, whole (REASSEMBLED), as an adapter hands on an ISO-TP
 * message */
template <typename Deliver>
inline void ecuio_mock_pid_replies(ecuio_sim_ecu& ecu,
                                   const ecuio_mock_pids& pids,
                                   uint64_t timestamp_us, ecuio_frame_t* frame,
                                   Deliver deliver)
{
    for (uint32_t i = 0; i < pids.count; ++i) {
        uint8_t reply[ECUIO_SIM_REPLY_MAX];
        const uint32_t n = ecuio_mock_pid_reply(ecu, pids.pid[i], reply);
        if (n == 0) {
            continue;
        }
        std::memset(frame, 0, sizeof *frame);
        frame->arbitration_id = ECUIO_MOCK_ECU_RX_ID;
        frame->flags = ECUIO_FRAME_FLAG_REASSEMBLED;
        frame->timestamp_us = timestamp_us;
        frame->data_length = static_cast<uint16_t>(n);
        std::memcpy(frame->data, reply, n);
        deliver(frame);
    }
}

/* Runs channel 0's supported-PID walk (ecuio_engine_probe, started by
 * the caller) to its end against the ECU, on the engine's own clock
 * (*now_us, which only moves forward), with nothing else asked. The walk
 * goes before the rotation, so only bitmaps are asked. */
inline void ecuio_mock_pid_walk(ecuio_request_engine* e, ecuio_sim_ecu& ecu,
                                int64_t* now_us)
{
    const ecuio_engine_channel& ch = e->chans[0];
    for (int step = 0; step < 64 && ch.walking; ++step) {
        const int32_t r = ecuio_engine_due(e, 0, *now_us);
        if (r == ECUIO_ENGINE_NONE) {
            if (!ecuio_engine_expire(e, 0, *now_us)) {
                *now_us += ECUIO_ENGINE_GAP_US;
            }
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        uint8_t reply[ECUIO_SIM_REPLY_MAX];
        const uint32_t n = ecuio_sim_answer(ecu, q.bytes, q.len, false, reply);
        ecuio_engine_sent(e, 0, r, true, *now_us);
        if (n > 0) {
            (void)ecuio_engine_on_frame(e, 0, reply, static_cast<uint16_t>(n),
                                        *now_us, nullptr);
        } else {
            *now_us += ECUIO_ENGINE_WAIT_US;
        }
    }
}

#endif /* ECUIO_MOCK_PIDS_H */
