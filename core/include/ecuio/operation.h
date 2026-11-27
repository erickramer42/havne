/* File/layer: core/include/ecuio/operation.h | public C ABI, strict C
 * Testing: UNIT: SessionOperationTest (test_core), test_c_abi; HARNESS:
 *          the transports' operation cases
 *
 * An operation run in a session (ADR-0066 section 4): one plan of the
 * session's plugin (ecuio/plugin.h's ecuio_plugin_operation), run by core
 * on the session's worker. This Havne runs one kind, a dump: a module's
 * memory read twice, compared byte for byte and kept as a copy
 * (ecuio/dump.h) in a folder the caller names.
 *
 * What a run is held to:
 *   - Core makes every request, from the plan the manifest declares and
 *     the consent showed. The plugin's code makes none of them: its own
 *     requests stay the conversation tier's, and it is not asked for one
 *     while a run is under way.
 *   - While a run is under way the session sends the run's requests and
 *     its sessions' keep-alives, nothing else: no reading, no scan, no
 *     identification, no clear.
 *   - Each range is read twice, back to back, and the second reading is
 *     compared with what the disk holds of the first. The first byte that
 *     differs ends the run, and nothing is kept.
 *   - The module's identification DIDs are read before the first byte and
 *     after the last; a value that changed ends the run.
 *   - A copy is kept only whole: every byte read twice and equal, the
 *     file read back from the disk and named by its checksum. Any other
 *     end leaves nothing in the folder.
 *
 * One run for a session at a time. The state is polled: no callback.
 */

#ifndef ECUIO_OPERATION_H
#define ECUIO_OPERATION_H

#include "ecuio/core.h"
#include "ecuio/dump.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* A request's flags */
/* The user confirmed this run with its unlock attempts and their lockout
 * risk (the plan's unlock_sessions): core may ask the module for a seed.
 * Without it a locked level refuses the start. */
#define ECUIO_OPERATION_MAY_UNLOCK 0x1u
/* The person confirmed the vehicle parked: ignition on, engine off by the
 * key, in P or neutral with the parking brake on. Every run needs it. */
#define ECUIO_OPERATION_PARKED 0x2u

/* A run's phase: closed, append-only */
#define ECUIO_OPERATION_PHASE_NONE          0u /* no run was started */
#define ECUIO_OPERATION_PHASE_CHECKING      1u /* handed over, not yet begun */
#define ECUIO_OPERATION_PHASE_PRECONDITIONS 2u /* the checks before a run */
#define ECUIO_OPERATION_PHASE_UNLOCKING     3u /* the session and the level */
#define ECUIO_OPERATION_PHASE_READING       4u /* a range's first reading */
#define ECUIO_OPERATION_PHASE_COMPARING     5u /* its second */
#define ECUIO_OPERATION_PHASE_SAVING        6u /* the copy is sealed */
#define ECUIO_OPERATION_PHASE_LEAVING       7u /* the session is left */
#define ECUIO_OPERATION_PHASE_DONE          8u /* end says how */

/* How a run ended: closed, append-only. Only SAVED keeps a copy.
 *   NONE              it runs, or none was started
 *   SAVED             name is the copy's
 *   CANCELED          the caller's cancel, or the close
 *   REFUSED           the module refused: nrc is its code
 *   NO_ANSWER         a request went unanswered
 *   DIFFER            the two readings differ: step and differ_offset say
 *                     where
 *   IDENTITY_CHANGED  an identification DID changed
 *   UNLOCK_REFUSED    the level stayed locked
 *   SESSION_LOST      the module left its session
 *   LINK_LOST         a request could not be sent
 *   FILE              the copy could not be written
 *   BAD_REPLY         a reply of the wrong shape
 *   ENGINE_RUNNING    rpm says how fast; nothing past the checks was sent
 *   MOVING            kmh says how fast; nothing past the checks was sent */
#define ECUIO_OPERATION_END_NONE             0u
#define ECUIO_OPERATION_END_SAVED            1u
#define ECUIO_OPERATION_END_CANCELED         2u
#define ECUIO_OPERATION_END_REFUSED          3u
#define ECUIO_OPERATION_END_NO_ANSWER        4u
#define ECUIO_OPERATION_END_DIFFER           5u
#define ECUIO_OPERATION_END_IDENTITY_CHANGED 6u
#define ECUIO_OPERATION_END_UNLOCK_REFUSED   7u
#define ECUIO_OPERATION_END_SESSION_LOST     8u
#define ECUIO_OPERATION_END_LINK_LOST        9u
#define ECUIO_OPERATION_END_FILE             10u
#define ECUIO_OPERATION_END_BAD_REPLY        11u
#define ECUIO_OPERATION_END_ENGINE_RUNNING   12u
#define ECUIO_OPERATION_END_MOVING           13u

/* A state's checks: what the run read before it began */
#define ECUIO_OPERATION_CHECK_RPM 0x1u /* rpm holds the engine's answer */
#define ECUIO_OPERATION_CHECK_KMH 0x2u /* kmh holds the vehicle's answer */

typedef struct ecuio_operation_request {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t operation;   /* its index among the session's plugin's
                             (ecuio_plugin_operation) */
    uint32_t flags;       /* ECUIO_OPERATION_* */
    uint32_t reserved[5]; /* zero; ABI growth space */
} ecuio_operation_request_t;

typedef struct ecuio_operation_state {
    uint32_t struct_size;   /* the caller's sizeof, at least this one's */
    uint32_t generation;    /* rises at each start: 0 before the first */
    uint32_t operation;     /* the request's */
    uint32_t phase;         /* ECUIO_OPERATION_PHASE_* */
    uint32_t end;           /* ECUIO_OPERATION_END_*; NONE while it runs */
    uint32_t pass;          /* the reading under way: 1 or 2; 0 outside one */
    uint32_t step;          /* the plan's step under way, or where it ended */
    uint8_t nrc;            /* END_REFUSED, END_UNLOCK_REFUSED: the module's
                               code, 0 for none */
    uint8_t reserved0[3];   /* zero */
    uint64_t bytes_done;    /* handed over so far, both readings counted */
    uint64_t bytes_total;   /* twice the image */
    uint64_t differ_offset; /* END_DIFFER: the first differing byte's place
                               in its step's range */
    char name[ECUIO_DUMP_NAME_MAX]; /* END_SAVED: the copy's name in the
                                       request's folder; else "" */
    char words[160];      /* why it ended, for a person; "" while it runs
                             and for END_SAVED. No path. */
    uint32_t volts_mv;
    uint32_t volts_min_mv;
    uint32_t checks; /* ECUIO_OPERATION_CHECK_* */
    uint32_t waiting_ms;
    uint16_t rpm;
    uint16_t kmh;
    uint32_t reserved[1]; /* zero; ABI growth space */
} ecuio_operation_state_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_operation_request_t) == 32,
              "ecuio_operation_request_t size is ABI");
static_assert(sizeof(ecuio_operation_state_t) == 336,
              "ecuio_operation_state_t size is ABI");
static_assert(offsetof(ecuio_operation_state_t, nrc) == 28,
              "operation state nrc offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, bytes_done) == 32,
              "operation state bytes_done offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, differ_offset) == 48,
              "operation state differ_offset offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, name) == 56,
              "operation state name offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, words) == 152,
              "operation state words offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, volts_mv) == 312,
              "operation state volts_mv offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, waiting_ms) == 324,
              "operation state waiting_ms offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, rpm) == 328,
              "operation state rpm offset is ABI");
static_assert(offsetof(ecuio_operation_state_t, reserved) == 332,
              "operation state reserved offset is ABI");
#endif

/*
 * Starts the run of request->operation on a started session, the copy to
 * go to folder_utf8 (an existing folder; the copy's name is core's). The
 * call checks everything that needs no request, makes the copy's
 * unfinished file, and hands the run to the session's worker: OK means it
 * was handed over, and ecuio_session_get_operation says how it goes.
 *
 *   ECUIO_ERR_NULL_ARGUMENT: a NULL session, request or folder.
 *   ECUIO_ERR_SESSION_CLOSED: the session was closed.
 *   ECUIO_ERR_INVALID_ARGUMENT, with words: a struct_size smaller than
 *     the struct, an unknown flag, a reserved word not zero, no
 *     ECUIO_OPERATION_PARKED; a session not
 *     started; no such operation, or a session whose plugin declares
 *     none; a transport that asks nothing (mock://, replay://, a
 *     listening session); the plan's module not one this session talks
 *     to, or its code not talking; a request of the plan longer than the
 *     adapter sends ("... doesn't fit one request on this adapter"); the
 *     plan's level locked and no ECUIO_OPERATION_MAY_UNLOCK ("unlock
 *     level NN first"); an unlock under way.
 *   ECUIO_ERR_CONSENT_REQUIRED: no consent was given to the plugin.
 *   ECUIO_ERR_NOT_IMPLEMENTED, with words: a run needs the write flow's
 *     checks before it (the engine off, the battery's floor), which this
 *     Havne has not got: no call here starts a run on a car yet.
 *   ECUIO_ERR_IN_USE: a run is under way in this session.
 *   ECUIO_ERR_TOO_SOON, with words: the level's delay after a refused key
 *     runs, or the module's identity is still being read.
 *   ECUIO_ERR_LOAD_FAILED, with words (no path): the folder can't take
 *     the copy's file, or the attempt's mark can't be saved.
 *   ECUIO_ERR_OUT_OF_MEMORY.
 *
 * A refusal leaves nothing: no file, no thread, no mark, the session as
 * it was. With ECUIO_OPERATION_MAY_UNLOCK and the level locked, the
 * attempt's mark is saved in the security store before the call returns,
 * as ecuio_session_security_unlock saves it; a refused key ends the run,
 * counts once and is never tried again.
 */
ecuio_status_t
ecuio_session_operation_start(ecuio_session_t* session,
                              const ecuio_operation_request_t* request,
                              const char* folder_utf8);

/*
 * Asks the run under way to stop. It ends CANCELED once the request that
 * is out has ended; a transfer the module holds open is closed first.
 * Nothing is kept. OK with no run under way too: the call is a request,
 * and the state says what became of the run. ECUIO_ERR_NULL_ARGUMENT for
 * a NULL session.
 */
ecuio_status_t ecuio_session_operation_cancel(ecuio_session_t* session);

/*
 * The run's state into out: the one under way, else the last one's end,
 * which stays readable after the session's close (a close ends a run
 * CANCELED and keeps nothing). ECUIO_ERR_NULL_ARGUMENT for a NULL
 * argument; ECUIO_ERR_INVALID_ARGUMENT, nothing written, for a
 * struct_size smaller than the struct; this core's struct_size is written
 * back.
 */
ecuio_status_t ecuio_session_get_operation(const ecuio_session_t* session,
                                           ecuio_operation_state_t* out);

#if defined(__cplusplus)
}
#endif

#endif /* ECUIO_OPERATION_H */
