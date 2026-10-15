# ADR-0002: Callback identity by `(callback, user_data)` pair, snapshot dispatch

- **Status:** Accepted
- **Date:** 2026-10-04 (v0.2.0, `6f2657b`)

## Context

Transports deliver frames on their own worker thread through
registered callbacks. A callback may need to register or unregister
callbacks, including itself, from inside a dispatch. The original
design sketched opaque registration handles, but they were never
implemented.

## Decision

- A registration is identified by its `(callback, user_data)` pair.
  Registering a duplicate pair succeeds and is deduplicated.
  Unregistering is idempotent.
- `qttune_session_dispatch_callbacks` (internal) copies the callback
  list under the mutex, then invokes the copy with the mutex
  released.
- Callback contract:
  - Callbacks run on the worker thread and must not block.
  - They must never call `qttune_session_close` or
    `qttune_session_destroy`, because both join that worker.
- Close discipline: an atomic closed flag is set before teardown
  starts, and detaching the transport joins the worker.

## Alternatives considered

- **Opaque registration handles.** These were dropped. They add
  bookkeeping for every caller and buy nothing that pair identity
  doesn't already give.
- **Dispatch while holding the lock.** This was rejected because a
  callback that re-registers would deadlock.

## Consequences

- Reentrant register and unregister calls cannot deadlock.
- A callback unregistered partway through a dispatch can still be
  invoked for the rest of that dispatch, because the snapshot was
  taken before it was removed.
- A callback that needs to refer to itself must be a named free
  function. A lambda gives it no identity it can reliably name.
- Thread-identity tests must go through `qttune_session_start` (the
  real worker). Calling dispatch by hand proves nothing about which
  thread a callback runs on.
