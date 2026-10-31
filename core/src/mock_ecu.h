// File/layer: core/src/mock_ecu.h | internal, C++17, Qt-free, header-only
// Testing: UNIT: CoreTest.MockReportsTheFakeEcusDiagnostics (the report
//          against tests/fake_diagnostics.h, the fakes' own),
//          CoreTest.MockReadAgainReadsTheSameReport,
//          CoreTest.MockDiagnoseOffReportsNone
//
// The simulated car's engine ECU (mock://, ADR-0037) for the diagnostics
// list: the request engine asks it as it asks a real ECU, and it answers
// from the table below, the same codes, monitors and freeze frame as the
// tests' fake ECU (tests/fake_ecu.h) before a clear, so the app and
// havne-cli show what the fakes do with no adapter. Its rx and tx IDs
// are the obd2 engine ECU's.

#ifndef ECUIO_MOCK_ECU_H
#define ECUIO_MOCK_ECU_H

#include "ecuio/signals.h"
#include "request_engine.h"

#include <cstdint>
#include <cstring>

constexpr uint32_t ECUIO_MOCK_ECU_RX_ID = 0x7E8u;
constexpr uint32_t ECUIO_MOCK_ECU_TX_ID = 0x7E0u;

/* A request and its positive reply, service byte on */
struct ecuio_mock_answer {
    uint8_t request[3];
    uint8_t request_len;
    uint8_t reply[8];
    uint8_t reply_len;
};

/* Readiness: the MIL on, 3 codes; misfire, fuel system and components;
 * catalyst, evaporative system, oxygen sensor, its heater and EGR/VVT,
 * all complete. The lists: P0301 P0420 U0100 stored, P0171 C0035 B1001
 * pending, P0420 permanent. The freeze frame P0301 stored: PIDs 02 04
 * 05 0C 0D, at 70 % load, 90 degC, 3000 rpm and 80 km/h. */
constexpr ecuio_mock_answer ECUIO_MOCK_ANSWERS[] = {
    {{0x01, 0x01}, 2, {0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}, 6},
    {{0x03}, 1, {0x43, 0x03, 0x03, 0x01, 0x04, 0x20, 0xC1, 0x00}, 8},
    {{0x07}, 1, {0x47, 0x03, 0x01, 0x71, 0x40, 0x35, 0x90, 0x01}, 8},
    {{0x0A}, 1, {0x4A, 0x01, 0x04, 0x20}, 4},
    {{0x02, 0x00, 0x00}, 3, {0x42, 0x00, 0x00, 0x58, 0x18, 0x00, 0x00}, 7},
    {{0x02, 0x02, 0x00}, 3, {0x42, 0x02, 0x00, 0x03, 0x01}, 5},
    {{0x02, 0x04, 0x00}, 3, {0x42, 0x04, 0x00, 0xB3}, 4},
    {{0x02, 0x05, 0x00}, 3, {0x42, 0x05, 0x00, 0x82}, 4},
    {{0x02, 0x0C, 0x00}, 3, {0x42, 0x0C, 0x00, 0x2E, 0xE0}, 5},
    {{0x02, 0x0D, 0x00}, 3, {0x42, 0x0D, 0x00, 0x50}, 4},
};

/* The table's answer to q, or nullptr: a request it lacks goes
 * unanswered, as a silent ECU's would */
inline const ecuio_mock_answer* ecuio_mock_ecu_answer(const ecuio_request& q)
{
    for (const ecuio_mock_answer& a : ECUIO_MOCK_ANSWERS) {
        if (a.request_len == q.len &&
            std::memcmp(a.request, q.bytes, q.len) == 0) {
            return &a;
        }
    }
    return nullptr;
}

/* Runs channel 0's diagnostics list to its end against the table, on
 * the engine's own clock (*now_us, which only moves forward), with
 * nothing else asked. No sleep: the engine's gaps and waits pass on the
 * clock alone. */
inline void ecuio_mock_ecu_read(ecuio_request_engine* e, int64_t* now_us)
{
    const ecuio_engine_channel& ch = e->chans[0];
    for (int step = 0; step < 256; ++step) {
        if (ch.diag_pending == 0 && !ch.diagnosing &&
            ch.outstanding == ECUIO_ENGINE_NONE) {
            return;
        }
        const int32_t r = ecuio_engine_due(e, 0, *now_us);
        if (r == ECUIO_ENGINE_NONE) {
            if (ecuio_engine_expire(e, 0, *now_us)) {
                continue;
            }
            *now_us += ECUIO_ENGINE_GAP_US;
            continue;
        }
        const ecuio_mock_answer* a =
            ecuio_mock_ecu_answer(ecuio_engine_request_of(e, 0, r));
        ecuio_engine_sent(e, 0, r, true, *now_us);
        if (a != nullptr) {
            (void)ecuio_engine_on_frame(e, 0, a->reply, a->reply_len, *now_us,
                                        nullptr);
        } else {
            *now_us += ECUIO_ENGINE_WAIT_US;
        }
    }
}

#endif /* ECUIO_MOCK_ECU_H */
