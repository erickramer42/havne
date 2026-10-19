// File/layer: core/src/pid_scheduler.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its only consumer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — rotation, cursor wrap, dedupe and capacity (test_obd2);
//     the worker integration is HARNESS, drain timing on a real adapter
//     is BENCH.

#ifndef QTTUNE_PID_SCHEDULER_H
#define QTTUNE_PID_SCHEDULER_H

#include "qttune/signals.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QTTUNE_SCHED_MAX_PIDS 16u

/* Round-robin PID scheduler (ADR-0013 slice 2). Fixed capacity, no
 * allocation, owned exclusively by the transport worker thread —
 * single-threaded by construction, so no synchronization inside.
 *
 * PID derivation rule: a def drives scheduling iff it has a 2-byte mux
 * key at payload offset 0 — the [41][PID] echo that opens a Mode-01
 * response once the transport strips the address header. The PID to
 * request is the key's low byte. Sets without such defs (raw, mock)
 * enroll nothing, and the worker falls back to ?poll= or passive
 * receive. Nothing validates this shape at registration, so
 * from_set() checks it itself. */
typedef struct qttune_pid_scheduler {
    uint8_t pids[QTTUNE_SCHED_MAX_PIDS];
    uint32_t count;
    uint32_t cursor;
} qttune_pid_scheduler;

static inline void qttune_sched_init(qttune_pid_scheduler* s)
{
    s->count = 0;
    s->cursor = 0;
}

/* Enroll every requestable def from the set, deduped, first-seen order.
 * Returns true when at least one PID enrolled (worker enters
 * scheduler mode); false when the set yields nothing (raw/mock —
 * caller keeps its legacy behavior). Silently stops at capacity:
 * exceeding 16 distinct PIDs in one v1 profile is a design error
 * caught by the set's own unit tests, not a runtime condition. */
static inline bool qttune_sched_from_set(qttune_pid_scheduler* s,
                                         const QttuneSignalSet* set)
{
    qttune_sched_init(s);
    if (set == NULL || set->defs == NULL || set->count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const QttuneSignalDef* d = &set->defs[i];
        /* OBD-II echo-grammar gate: 2-byte key at offset 0 —
         * [41][PID], the post-header-strip response payload. */
        if (d->mux_length != 2 || d->mux_offset != 0) {
            continue;
        }
        const uint8_t pid = (uint8_t)(d->mux_key & 0xFFu);
        bool dup = false;
        for (uint32_t j = 0; j < s->count; ++j) {
            if (s->pids[j] == pid) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        if (s->count >= QTTUNE_SCHED_MAX_PIDS) {
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
static inline bool qttune_sched_next(qttune_pid_scheduler* s,
                                     uint8_t* out_pid)
{
    if (s->count == 0 || out_pid == NULL) {
        return false;
    }
    *out_pid = s->pids[s->cursor % s->count];
    s->cursor = (s->cursor + 1u) % s->count;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_PID_SCHEDULER_H */
