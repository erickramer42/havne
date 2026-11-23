// File/layer: core/src/host_awake.h | internal, C++17, Qt-free
// Testing: UNIT: CoreTest.*Awake*, Elm327AwakeTest (test_elm327)
//
// The host held awake while a session reads (ADR-0033's appendix "the host
// kept awake while a session reads"): the platform's take and release, and
// the session's state. Two claims share one session: the caller's
// (ecuio_session_keep_awake) and core's own through a write step, which a
// caller's 0 never drops.

#ifndef ECUIO_HOST_AWAKE_H
#define ECUIO_HOST_AWAKE_H

#include "ecuio/core.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace host_awake {

/* How far a hold reaches: `idle` holds off the idle timer alone, so a
 * sleep the user asks for still sleeps; `sleep` (a write step) holds off
 * that too where the platform can (logind's sleep lock on Linux) */
enum class level : uint32_t { idle = 0, sleep = 1 };

struct hold;

/* The platform's hold, or the test seam's; the detail says why on a
 * refusal. Allocates the handle. Up to 1 s on Linux: never under a lock. */
ecuio_status_t take(level lv, const char* why, hold** out);
/* Never waits and never fails; NULL is a no-op */
void release(hold* h);

/* Whether a session on this backend holds: a live transport's only */
bool live_backend(uint32_t backend);

/* One claim on the hold. `ends` counts its releases and the close, so a
 * take that finishes after one publishes nothing. */
struct claim {
    hold* held = nullptr;
    uint64_t ends = 0;
};

} // namespace host_awake

struct ecuio_awake_state {
    std::mutex mutex; /* the claims and the close's check; never held
                         across a take */
    host_awake::claim caller;           /* ecuio_session_keep_awake's */
    host_awake::claim core;             /* a write step's */
    std::atomic<uint32_t> shown{0};     /* what ecuio_session_get_awake gives */
};

/* Core's own claim, for a write-class step: held whatever the caller
 * asked, until a call with false or the close. ECUIO_OK under mock:// and
 * replay://, holding nothing; the platform's refusal otherwise, and on
 * Linux a refusal when logind's sleep lock isn't held (the desktop's
 * inhibits hold off only its idle timer). Windows' power request and
 * macOS's assertion are the read's own: a sleep the user asks for still
 * sleeps a write there. The take runs outside the mutex: a false or the
 * close meanwhile wins, and the take returns ECUIO_OK (the close
 * ECUIO_ERR_SESSION_CLOSED) holding nothing. */
ecuio_status_t ecuio_awake_core_hold(ecuio_session_t* session, bool on);
/* The close's: every claim released, after the transport's detach */
void ecuio_awake_release_all(ecuio_session_t* session);

#endif /* ECUIO_HOST_AWAKE_H */
