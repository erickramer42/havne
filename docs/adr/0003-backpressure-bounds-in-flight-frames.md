# ADR-0003: Backpressure bounds in-flight frames, not cumulative count

- **Status:** Accepted
- **Date:** 2026-10-07 (corrected during v0.3.0 Phase 2)

## Context

`QtTuneBridge::onFrame` runs on the transport worker and posts one
queued lambda per frame to the GUI thread. In v0.2.0 the bridge
stopped delivering once `m_currentFrameCount >= 10000`, and that
count was the **lifetime** total. So once 10k frames had ever been
processed (about 8 minutes at mock rate), delivery stopped for good:
the UI froze while the drop counter kept climbing. The model's ring
eviction never ran, because no frame ever reached it.

## Decision

- The gate bounds the number of **in-flight** lambdas, meaning ones
  that have been posted but not yet drained
  (`MaxInFlightFrames = 1000`). Each lambda decrements the count
  before it does any work.
- Memory is bounded separately. `FrameListModel` keeps a 10k-row ring
  and evicts the oldest rows.
- Drops are counted on the worker (an atomic) and reported to the GUI
  through at most one pending queued notification.
- Queued lambdas use the bridge as their context object, so lambdas
  still pending when the bridge is destroyed are dropped, not run
  against freed memory.

## Consequences

- The stream never freezes. At mock rate, in-flight sits at 0–2, and
  the drop counter rises only under a real backlog (a stalled GUI
  thread).
- The regression test is `BridgeTest.StreamingContinuesPastTenThousandFrames`.
  It relies on the test-only seam `forceFrameCountForTesting`, which
  app code never calls.
- There is still one queued lambda per frame. At J2534 rates that may
  need batching on the worker side (see the Performance backlog in
  [STATUS.md](../../STATUS.md#performance)).
- The broader lesson: pair every drop statistic with a liveness test,
  because confusing a cumulative count with an in-flight count stays
  invisible until someone leaves the app running.
