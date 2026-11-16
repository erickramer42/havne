// File/layer: core/src/link_health.h | internal, not ABI-exported;
//             included by the transports whose workers judge a link
//             (j2534_transport.cpp; the ELM327 transport, ADR-0026; the
//             SocketCAN transport, ADR-0040)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: CoreTest, RecorderTest, ConditionSessionTest (test_core);
//          HARNESS: J2534HealthTest (test_j2534_health), J2534ConditionTest
//          and J2534AdoptionTest (test_j2534_stream), Elm327SessionTest
//          (test_elm327), SocketCanRequestTest (test_vcan)

#ifndef ECUIO_LINK_HEALTH_H
#define ECUIO_LINK_HEALTH_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "host_clock.h"
#include "recorder.h"

#include <atomic>
#include <stdint.h>

/* Failure backoff, entered on a hard read failure or a failed write:
 * 100 ms base, doubling to a 5 s cap (about 0.2 retries/s against a
 * dead adapter). It ends when the adapter shows it is answering: a
 * matched frame, an unmatched one, a debounced quiet run, or (for
 * ECUIO_ERR_REQUEST_FAILED) a successful write. */
constexpr uint32_t LINK_BACKOFF_BASE_MS = 100;
constexpr uint32_t LINK_BACKOFF_CAP_MS  = 5000;

/* The wait that follows one of `ms`: doubled, then clamped, so none exceeds
 * the cap (which isn't the base times a power of two). */
constexpr uint32_t link_backoff_next(uint32_t ms)
{
    return (ms >= LINK_BACKOFF_CAP_MS / 2) ? LINK_BACKOFF_CAP_MS : ms * 2;
}

/* "Not responding" is this many in a row: requests whose wait ran out,
 * while there is something to ask; reads that brought nothing from an
 * ECU we talk to, in passive mode and once nothing is left to ask. About
 * a second either way. One empty read means nothing: with requests, most
 * reads between two of them are empty. */
constexpr uint32_t LINK_HEALTH_QUIET_RUN = 10;

/* A worker's view of its link (the full table is in docs/j2534.md). It
 * publishes through the session's two atomics, which
 * ecuio_session_get_error_state reads, and only on transitions, each
 * also a link record for the session's recording (ADR-0049 section 1)
 * and then a change for its conditions' link terms, both at the
 * session's time (link_session_us); the rest is the worker's alone. The
 * worker also hands a set of conditions attached meanwhile the state
 * (link_judge_waiting). */
struct link_health {
    link_health(std::atomic<int32_t>& published_state,
                std::atomic<uint32_t>& published_episodes,
                ecuio_record_link* recording = nullptr,
                ecuio_session_t* judged_for = nullptr,
                host_clock::time_point session_epoch = {},
                const uint64_t* last_frame_us = nullptr,
                const uint64_t* last_frame_host_us = nullptr)
        : published(published_state), episodes(published_episodes),
          record(recording), session(judged_for), epoch(session_epoch),
          frame_us(last_frame_us), frame_host_us(last_frame_host_us)
    {}

    std::atomic<int32_t>& published;    /* the session's last_error */
    std::atomic<uint32_t>& episodes;    /* the session's error_count */
    ecuio_record_link* record;          /* the session's, or NULL */
    ecuio_session_t* session;           /* its conditions', or NULL */
    host_clock::time_point epoch;       /* the transport's attach */
    /* A clock that counts frames on the adapter's or the kernel's time
     * (j2534_clock, socketcan_clock): its last frame's session time and
     * host time, both 0 before one, read on this worker; NULL where
     * frames take the host's time */
    const uint64_t* frame_us;
    const uint64_t* frame_host_us;
    int32_t state = ECUIO_OK;
    bool write_failed = false;   /* latched by a failed write, cleared
                                  * only by a sent one */
    uint32_t backoff_ms = 0;     /* 0 = the normal cadence */
    uint32_t quiet_runs = 0;
    bool unanswered = false;     /* a request's wait ran out since the
                                  * last matched reply */
    bool recovered = false;      /* back to OK since the worker last
                                  * looked: the probe resumes */
    /* The last change's session time and host time since epoch, with a
     * session: link_session_us's floor while the link isn't OK */
    uint64_t changed_us = 0;
    uint64_t changed_host_us = 0;
};

inline uint64_t link_host_us(const link_health& h)
{
    const auto since = std::chrono::duration_cast<std::chrono::microseconds>(
                           host_clock::now() - h.epoch)
                           .count();
    return since > 0 ? static_cast<uint64_t>(since) : 0;
}

/* The session's time at host (link_host_us) on its frames' clock: the last
 * frame's plus the host's time since, or the host's. While the link isn't
 * OK, never before the last change plus the host's time since, since frames
 * an adapter held through a fault carry stamps from its start. */
inline uint64_t link_session_us(const link_health& h, uint64_t host)
{
    uint64_t at = host;
    if (h.frame_us != nullptr && h.frame_host_us != nullptr) {
        at = *h.frame_us +
             (host > *h.frame_host_us ? host - *h.frame_host_us : 0);
    }
    if (h.state != ECUIO_OK) {
        const uint64_t floor =
            h.changed_us +
            (host > h.changed_host_us ? host - h.changed_host_us : 0);
        at = at > floor ? at : floor;
    }
    return at;
}

inline uint64_t link_session_us(const link_health& h)
{
    return link_session_us(h, link_host_us(h));
}

inline void link_set_state(link_health& h, int32_t state)
{
    if (h.state != state) {
        h.published.store(state);
        h.recovered = h.recovered || state == ECUIO_OK;
        uint64_t at = 0;
        if (h.session != nullptr) {
            const uint64_t host = link_host_us(h);
            at = link_session_us(h, host); /* the state before: its floor */
            h.changed_us = at;
            h.changed_host_us = host;
        }
        h.state = state;
        ecuio_record_link_state(h.record, state, h.episodes.load(), at);
        if (h.session != nullptr) {
            ecuio_session_dispatch_link(h.session, state, at);
        }
    }
}

/* Once a worker pass: a set of conditions attached since the last frame or
 * change is judged by the state now, so one attached during a fault, with
 * no frame to take it, reports the fault */
inline void link_judge_waiting(const link_health& h)
{
    if (h.session != nullptr &&
        ecuio_session_conditions_waiting(h.session) != 0) {
        ecuio_session_dispatch_link(h.session, h.state, link_session_us(h));
    }
}

/* A transition into a failure that counts an episode. The count goes first:
 * get_error_state reads the code, then the count, so a reader that sees the
 * code sees its episode (replay's stop_early does the same). */
inline void link_begin_episode(link_health& h, int32_t state)
{
    h.episodes.fetch_add(1);
    link_set_state(h, state);
}

/* One more sign of nothing to hear: after LINK_HEALTH_QUIET_RUN in a
 * row, VEHICLE_TIMEOUT, with no episode. */
inline void link_count_quiet(link_health& h)
{
    if (++h.quiet_runs >= LINK_HEALTH_QUIET_RUN) {
        link_set_state(h, ECUIO_ERR_VEHICLE_TIMEOUT);
    }
}

/* An idle pass of a session that asks only its picks, with nothing
 * picked (ecuio_engine_unpicked): nothing is asked, so no read is a
 * quiet one, and a VEHICLE_TIMEOUT that quiet reads alone made (picks
 * all dropped, since taken away) ends. One a request's wait ran out for
 * stays until a reply: the walk, identification and diagnostics ask on
 * such a session too, and OK here would have the worker ask them again
 * and time out again, for as long as the ECU is silent. A hard failure
 * or failing writes are the adapter's, and stay. */
inline void link_on_unpicked(link_health& h)
{
    h.quiet_runs = 0;
    if (h.state == ECUIO_ERR_VEHICLE_TIMEOUT && !h.unanswered) {
        link_set_state(h, ECUIO_OK);
    }
}

/* Read-side link health. A read is a hard failure (the adapter or the
 * connection to it failed), a matched reply (a frame from an ECU we talk
 * to), or anything else (empty, or a frame for no channel of ours).
 * First match wins:
 *   hard failure       -> TRANSPORT_HARD_FAILURE, one episode, backoff
 *                         (restarted if a clean read had ended it)
 *   write latch set    -> a hard failure steps down to REQUEST_FAILED,
 *                         with no new episode: the adapter reads again,
 *                         so what fails is the writes. Otherwise no
 *                         change; the backoff stays the writes'
 *   matched reply      -> OK, backoff and quiet count cleared
 *   anything else      -> backoff cleared; when reads count (passive
 *                         mode, or nothing left to ask), one more quiet
 *                         read
 * While there is something to ask the quiet count is the requests' own
 * (link_on_request_timeout), and the empty reads between two requests mean
 * nothing. A frame for no channel of ours moves no state;
 * it only shows the adapter reads, which ends the backoff. */
inline void link_on_read(link_health& h, bool hard_failure, bool matched,
                         bool reads_count)
{
    if (hard_failure) {
        if (h.state != ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
            link_begin_episode(h, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
            h.backoff_ms = LINK_BACKOFF_BASE_MS;
        } else if (h.backoff_ms == 0) {
            /* A clean read ended the backoff, and the adapter is
             * failing again: back off from the base, still one episode. */
            h.backoff_ms = LINK_BACKOFF_BASE_MS;
        }
        h.quiet_runs = 0;
        return;
    }

    if (h.write_failed) {
        if (h.state == ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
            link_set_state(h, ECUIO_ERR_REQUEST_FAILED);
        }
        return;
    }

    if (matched) {
        link_set_state(h, ECUIO_OK);
        h.backoff_ms = 0;
        h.quiet_runs = 0;
        h.unanswered = false;
        return;
    }

    /* The adapter answered with nothing for us. Ending the backoff here
     * keeps a hard failure that ends in silence from counting its quiet
     * run one backoff interval at a time, about 26 s. */
    h.backoff_ms = 0;
    if (reads_count) {
        link_count_quiet(h);
    }
}

/* A request's wait ran out with no answer, the request modes' quiet
 * unit. While the write latch is set the state is the writes', so it
 * doesn't count. */
inline void link_on_request_timeout(link_health& h)
{
    if (!h.write_failed) {
        h.unanswered = true;
        link_count_quiet(h);
    }
}

/* Write-side link health, apart from reads: a bus where no node ACKs
 * (ignition off, ECU asleep) fails writes while the adapter reads cleanly,
 * and a hard failure would blame the adapter. A failed write latches
 * REQUEST_FAILED with one episode; a successful one clears it at once,
 * since an accepted write proves requests flow. A
 * hard READ failure outranks it; quiet reads can't downgrade it. */
inline void link_on_write(link_health& h, bool sent)
{
    if (sent) {
        if (h.write_failed) {
            h.write_failed = false;
            if (h.state == ECUIO_ERR_REQUEST_FAILED) {
                link_set_state(h, ECUIO_OK);
                h.backoff_ms = 0;
            }
        }
        return;
    }

    h.write_failed = true;
    if (h.state == ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
        return;  /* hard read failure outranks, don't change state */
    }
    if (h.state != ECUIO_ERR_REQUEST_FAILED) {
        /* Transition INTO requests-failing: ONE episode, backoff at base */
        link_begin_episode(h, ECUIO_ERR_REQUEST_FAILED);
        h.backoff_ms = LINK_BACKOFF_BASE_MS;
    }
    /* Already REQUEST_FAILED: leave backoff alone, it doubles at sleep site */
}

#endif /* ECUIO_LINK_HEALTH_H */
