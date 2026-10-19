# ADR-0014: Single-threaded surrogate child; reads travel over the pipe

- **Status:** Accepted. Supersedes the data-plane and surrogate-side
  threading parts of [ADR-0008](0008-x86-surrogate-host.md).
- **Date:** 2026-10-19 (design item D1 from the 2026-10-18 review).

## Context

ADR-0008 gave the surrogate child two threads: a control pump serving
the pipe, and a read pump polling `PassThruReadMsgs` into a 4096-slot
shared-memory ring. That shape cost more than it bought:

- J2534-1 promises no thread safety, so every vendor call needed one
  lock, held across teardown as well (Known Issue #24, fixed
  2026-10-17).
- A full ring dropped frames with no counter (Known Issue #17).
- The ring's ordering relied on MSVC's `/volatile:ms` semantics rather
  than explicit atomics.
- The read pump started only when the channel ID was nonzero, so a
  vendor returning channel 0 would look exactly like a quiet bus.
- The read pump discarded every vendor status, so the transport could
  not tell a quiet bus from a dead adapter on the surrogate backend
  (Known Issue #11).

The concurrency was never needed. The app drives a session from one
thread at a time: attach and start run before the worker exists, the
worker issues every read and write itself, and detach runs only after
the worker has been joined.

## Decision

- **One thread in the child.** The child reads one request from the
  pipe, makes at most one vendor call, writes one response, and loops.
  There is no vendor lock and no second thread, so vendor calls are
  serialized by construction.
- **`OP_READ` (opcode 5) carries the data plane.** It was reserved
  under ADR-0008. The request carries the caller's read timeout. The
  child calls `PassThruReadMsgs` for up to
  `QTTUNE_SUR_MAX_FRAMES_PER_READ` (16) messages, drops TX indications
  and loopback echoes (`TX_INDICATION`, `TX_MSG_TYPE`), clamps each
  payload to 64 bytes, and returns the frames together with the
  vendor status, verbatim.
- **The client caches the batch.** `qttune_j2534_sur_read_frame`
  dispenses one cached frame per call and sends a new `OP_READ` only
  when the cache is empty. The transaction timeout is the read
  timeout plus 500 ms, so a timeout means the child is hung, not busy.
- **Vendor status is data, never control flow.** A refused ioctl, a
  failed filter stop or an empty read is answered and the loop
  continues. Only `OP_SHUTDOWN` or a broken pipe ends it.
- **Command line:** `qttune-j2534-surrogate.exe <dll_path> <pipe_name>`.
  The file mapping and event are gone. Exit codes 2 and 3, which
  covered attaching to them, are retired and are not reused.
- **The child re-checks lengths** on `WRITE`, `IOCTL` and
  `START_FILTER` requests before copying, and the client rejects a
  response whose `num_frames` exceeds the batch capacity. The pipe is
  a trust boundary in both directions.

## Alternatives considered

- **Keep the ring; add a drop counter and explicit atomics.** This
  fixes #17 and the ordering concern but keeps the second thread, the
  vendor lock and the status-blind read pump. It adds code rather than
  removing it.
- **Always use the surrogate, including for x64 DLLs** (design item D2
  from the same review). This would delete the in-proc vendor path and
  its parity burden. It was deferred: it is independent of this change
  and has a larger test cost.

## Consequences

- No vendor lock, no ring, no read thread. Known Issue #17 is
  resolved by removal, and the `volatile` ordering concern no longer
  applies.
- Teardown order is automatic: nothing can be inside a vendor call
  when the child reaches its disconnect/close/`FreeLibrary` sequence.
- The vendor's read status reaches the transport on the surrogate
  backend too. Both backends now map `ERR_TIMEOUT` to a vehicle
  timeout, `ERR_BUFFER_EMPTY` to "no frame", and anything else to a
  hard failure.
- Every read is a pipe round trip. At the transport's read cap
  (≤100 reads/s) that cost is negligible, and receive latency is now
  bounded by the vendor's own `ReadMsgs` timeout instead of the old
  pump's sleep granularity.
- `qttune_sur_response_t` grows to 1368 bytes to carry the batch.
  Both build trees must be rebuilt together (Known Issue #19). A
  skewed tree fails loudly as a short read.
- Open at the time of writing:
  - A conforming vendor answers a 16-message read with `ERR_TIMEOUT`
    when fewer messages arrive within the timeout, and the client
    currently discards frames that arrive with any non-success status
    (Known Issue #26). The fake ignores `NumMsgs` and the timeout, so
    the suite cannot show this.
  - The child exits on `OP_SHUTDOWN` without replying, so every
    surrogate close waits out the client's 1.5 s polite-shutdown
    timeout (Known Issue #27).
  - The child still treats channel and device ID 0 as "none".
