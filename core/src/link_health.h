// File/layer: core/src/link_health.h | internal, not ABI-exported;
//             included by the transports whose workers judge a link
//             (j2534_transport.cpp; the ELM327 transport, ADR-0026)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — every rule below and the backoff step, session-free
//     (CoreTest.Link*, test_core); through a worker HARNESS
//     (J2534HealthTest).

#ifndef ECUIO_LINK_HEALTH_H
#define ECUIO_LINK_HEALTH_H

#include "ecuio/core.h"

#include <atomic>
#include <stdint.h>

/* Failure backoff, entered on a hard read failure or a failed write:
 * 100 ms base, doubling to a 5 s cap (about 0.2 retries/s against a
 * dead adapter). It ends when the adapter shows it is answering: a
 * matched frame, an unmatched one, a debounced quiet run, or (for
 * ECUIO_ERR_REQUEST_FAILED) a successful write. */
constexpr uint32_t LINK_BACKOFF_BASE_MS = 100;
constexpr uint32_t LINK_BACKOFF_CAP_MS  = 5000;

/* The wait that follows one of `ms`: twice as long, clamped to the cap.
 * The doubled value is what gets clamped, so no wait exceeds the cap
 * even though the cap isn't the base times a power of two. */
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
 * ecuio_session_get_error_state reads, and only on transitions; the
 * rest is the worker's alone. */
struct link_health {
    link_health(std::atomic<int32_t>& published_state,
                std::atomic<uint32_t>& published_episodes)
        : published(published_state), episodes(published_episodes) {}

    std::atomic<int32_t>& published;    /* the session's last_error */
    std::atomic<uint32_t>& episodes;    /* the session's error_count */
    int32_t state = ECUIO_OK;
    bool write_failed = false;   /* latched by a failed write, cleared
                                  * only by a sent one */
    uint32_t backoff_ms = 0;     /* 0 = the normal cadence */
    uint32_t quiet_runs = 0;
    bool recovered = false;      /* back to OK since the worker last
                                  * looked: the probe resumes */
};

inline void link_set_state(link_health& h, int32_t state)
{
    if (h.state != state) {
        h.published.store(state);
        h.recovered = h.recovered || state == ECUIO_OK;
        h.state = state;
    }
}

/* A transition into a failure that counts an episode. The count goes
 * first: ecuio_session_get_error_state reads the code, then the count,
 * so a reader that sees the code sees its episode (replay's stop_early
 * does the same). */
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
 * (link_on_request_timeout), and the empty reads between two requests
 * mean nothing. A frame for no channel of ours answers nothing, so it
 * moves no state; it only shows the adapter reads, which ends the
 * backoff. */
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
        link_count_quiet(h);
    }
}

/* Write-side link health, separate from reads, because a bus where no
 * node ACKs (ignition off, ECU asleep) fails writes while the adapter
 * still reads cleanly; calling that a hard failure would blame the
 * adapter. A failed write latches REQUEST_FAILED with ONE episode; a
 * successful write clears it at once, since an accepted write proves
 * requests flow. A hard READ failure outranks it; quiet reads can't
 * downgrade it. */
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
