// File/layer: core/src/pid_scheduler.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its only consumer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — the Mode-01 key test, rotation, cursor wrap, dedupe
//     and capacity (test_obd2); the worker integration and request
//     routing are HARNESS, drain timing on a real adapter is BENCH.

#ifndef ECUIO_PID_SCHEDULER_H
#define ECUIO_PID_SCHEDULER_H

#include "ecuio/signals.h"

#include <cstdint>

#define ECUIO_SCHED_MAX_PIDS 16u

/* Round-robin PID scheduler (ADR-0013). Fixed capacity, no
 * allocation, owned exclusively by the transport worker thread —
 * single-threaded by construction, so no synchronization inside.
 *
 * PID derivation rule (ecuio_mode01_pid): a def drives scheduling iff
 * its mux key is the 2-byte [41][PID] echo at payload offset 0, which
 * opens a Mode-01 response once the transport strips the address
 * header. The PID to request is the key's low byte. Sets without such
 * defs (raw, mock) enroll nothing, and the worker falls back to ?poll=
 * or passive receive. Registration validates each def but not this
 * shape (other mux keys are legal), so from_set() checks it itself. */
typedef struct ecuio_pid_scheduler {
    uint8_t pids[ECUIO_SCHED_MAX_PIDS];
    uint32_t count;
    uint32_t cursor;
} ecuio_pid_scheduler;

/* The one Mode-01 key test. The scheduler enrolls by it and the
 * transport routes each request by it (rx_for_pid), so the two can't
 * disagree. Any other key is not Mode-01, even one whose last byte
 * equals a PID, such as a 3-byte 62 <DID> or another service's 2-byte
 * echo. Writes *out_pid only on true. */
inline bool ecuio_mode01_pid(const ecuio_signal_def_t* d, uint8_t* out_pid)
{
    if (d->mux_length != 2 || d->mux_offset != 0 ||
        (d->mux_key >> 8) != 0x41u) {
        return false;
    }
    *out_pid = (uint8_t)(d->mux_key & 0xFFu);
    return true;
}

inline void ecuio_sched_init(ecuio_pid_scheduler* s)
{
    s->count = 0;
    s->cursor = 0;
}

/* Enroll every requestable def from the set, deduped, first-seen order.
 * Returns true when at least one PID enrolled (worker enters
 * scheduler mode); false when the set yields nothing (raw/mock — the
 * worker then runs ?poll= or passive). Silently stops at capacity:
 * exceeding 16 distinct PIDs in one v1 profile is a design error
 * caught by the set's own unit tests, not a runtime condition. */
inline bool ecuio_sched_from_set(ecuio_pid_scheduler* s,
                                         const ecuio_signal_set_t* set)
{
    ecuio_sched_init(s);
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t pid = 0;
        if (!ecuio_mode01_pid(&set->defs[i], &pid)) {
            continue;
        }
        bool dup = false;
        for (uint32_t j = 0; j < s->count; ++j) {
            if (s->pids[j] == pid) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        if (s->count >= ECUIO_SCHED_MAX_PIDS) {
            break;   /* capacity — documented above */
        }
        s->pids[s->count++] = pid;
    }
    return s->count > 0;
}

/* Advance the cursor round-robin and hand back the next PID to
 * request. Returns false only for an empty scheduler (callers in
 * scheduler mode never see this — they entered on count > 0 and
 * nothing removes PIDs at runtime). */
inline bool ecuio_sched_next(ecuio_pid_scheduler* s,
                                     uint8_t* out_pid)
{
    if (s->count == 0 || out_pid == nullptr) {
        return false;
    }
    *out_pid = s->pids[s->cursor % s->count];
    s->cursor = (s->cursor + 1u) % s->count;
    return true;
}

#endif /* ECUIO_PID_SCHEDULER_H */
