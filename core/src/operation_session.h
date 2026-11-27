// File/layer: core/src/operation_session.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: SessionOperationTest (test_core); HARNESS: the transports'
//          operation cases
//
// A session's run of one operation (ecuio/operation.h): what the start
// makes on the caller's thread (core_operation.cpp), what the worker
// steps (request_operation.h), and what crosses between them.
//
//   - The start fills the run while its stage is idle or ended, then arms
//     it: the worker reads nothing of it before that and the caller
//     writes nothing of it after, but the cancel flag.
//   - The worker takes an armed run, steps it, and ends it; it writes the
//     view under the session's support mutex, where the getter reads it.
//   - The sink (operation_sink.h) is made by the start and joined by the
//     next start or the close, never by the worker.
//   - The object is made at the first start and lives until the session
//     is destroyed, so the worker's pointer to it never dangles.

#ifndef ECUIO_OPERATION_SESSION_H
#define ECUIO_OPERATION_SESSION_H

#include "ecuio/operation.h"
#include "operation_checks.h"
#include "operation_plan.h"
#include "operation_run.h"
#include "operation_sink.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ecuio_operation {

/* Whose the run is */
constexpr uint8_t kStageIdle = 0;    /* none: the caller's */
constexpr uint8_t kStageArmed = 1;   /* filled: the worker's to take */
constexpr uint8_t kStageRunning = 2; /* the worker's */
constexpr uint8_t kStageEnded = 3;   /* the caller's again */

/* The worker's wait while a run has no request out: for the ring's room,
 * the session's entry, the unlock's end or the seal */
constexpr int64_t kPollUs = 5000;
/* The longest wait for the plan's session to be entered */
constexpr int64_t kEnterMostUs = 10000000;

struct session_run {
    /* The start's, read by the worker once armed */
    const plan* p = nullptr; /* the session's part's, which outlives it */
    std::unique_ptr<dump_sink> sink;
    uint32_t channel = 0;   /* the plan's module among the session's */
    uint32_t operation = 0; /* its index, for the view */
    bool marked = false;    /* an attempt's mark was saved: its word is owed */
    bool write = false;     /* the write class's envelope */
    bool held_awake = false; /* core's keep-awake claim, released at the end */
    uint32_t start_checks = 0; /* the start's warnings (CHECK_*) */

    std::atomic<uint8_t> stage{kStageIdle};
    std::atomic<bool> cancel{false};

    /* The worker's */
    run r;
    checks k;
    volts v;
    uint32_t volts_asked = 0; /* the battery's want this run waits on */
    int64_t volts_at_us = 0;
    uint32_t volts_step = 0;
    bool checking = false;
    uint32_t talk_at = 0;    /* the channel the run's talk is offered on */
    bool out = false;        /* that channel's talk is the run's */
    bool fed = false;        /* its answer reached the machine with its frame */
    bool orphan = false;     /* the machine gave it up: its settle is dropped */
    bool word_given = false; /* the owed word went to the conversation */
    bool closed_sink = false; /* the sink was sealed or abandoned */
    uint8_t sid = 0;          /* the service of the talk that is out */
    uint32_t ends_at = 0;     /* the report's words_ended at the word */
    uint32_t unlocks = 0;     /* attempts that opened the level */
    int64_t began_us = 0;
    int64_t entering_us = 0; /* when the entry was first asked; 0 not yet */
    bool quiet_out = false;  /* the machine's functional request is due */
    int64_t keep_at_us = 0;  /* the next functional 3E 80; 0 none */

    /* What the getter reads, under the session's support mutex */
    ecuio_operation_state_t view{};
};

} // namespace ecuio_operation

struct ecuio_session;

/* core_operation.cpp: the session's deleter for its run */
void ecuio_operation_free(ecuio_operation::session_run* o) noexcept;

/* At close, after the worker has joined: a run not ended ends canceled,
 * the writer is joined and nothing unsealed is kept */
void ecuio_operation_closed(ecuio_session* session) noexcept;

#endif /* ECUIO_OPERATION_SESSION_H */
