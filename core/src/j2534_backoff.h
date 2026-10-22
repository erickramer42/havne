// File/layer: core/src/j2534_backoff.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its only consumer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT — the step and its cap (test_j2534_stream); the worker's
//     use of it is HARNESS (BackoffGrowsUnderSustainedHardFailure).

#ifndef ECUIO_J2534_BACKOFF_H
#define ECUIO_J2534_BACKOFF_H

#include <stdint.h>

/* Failure backoff, entered on a hard read failure or a failed write:
 * 100 ms base, doubling to a 5 s cap (about 0.2 retries/s against a
 * dead adapter). It ends when the adapter shows it is answering: a
 * matched frame, an unmatched one, a debounced quiet run, or (for
 * ECUIO_ERR_REQUEST_FAILED) a successful write. */
constexpr uint32_t J2534_BACKOFF_BASE_MS = 100;
constexpr uint32_t J2534_BACKOFF_CAP_MS  = 5000;

/* The wait that follows one of `ms`: twice as long, clamped to the cap.
 * The doubled value is what gets clamped, so no wait exceeds the cap
 * even though the cap isn't the base times a power of two. */
constexpr uint32_t j2534_backoff_next(uint32_t ms)
{
    return (ms >= J2534_BACKOFF_CAP_MS / 2) ? J2534_BACKOFF_CAP_MS : ms * 2;
}

#endif /* ECUIO_J2534_BACKOFF_H */
