# ADR-0008: Host 32-bit J2534 DLLs in an x86 surrogate process

- **Status:** Accepted
- **Date:** 2026-10-14 (Phase 3 Part 2, `96acce9`). Cross-arch
  verification in `5681ca7`.

## Context

Many J2534 vendor DLLs are 32-bit only (the OBDX Pro VX, for example),
and an x64 process cannot load them. Modern Qt ships no 32-bit Windows
kits, so the app itself is x64 for good.

## Decision

Load 32-bit vendor DLLs in a separate Win32 executable,
`qttune-j2534-surrogate`, driven by an app-side client
(`core/src/j2534_surrogate_client.cpp`, the `qttune_j2534_sur_*`
functions).

- **Lifecycle.** The client creates every named object (pipe, file
  mapping, event). It then spawns the surrogate suspended, inside a
  `KILL_ON_JOB_CLOSE` job object, and handshakes with `OP_OPEN`.
  Because of the job object, a crashed or wedged child cannot outlive
  the app.
- **Control plane.** An overlapped, byte-mode named pipe. Each
  `transact()` has a bounded timeout and detects desync by request ID.
  The handshake times out after 8 s, and a dead DLL fails within
  milliseconds rather than waiting out that limit.
- **Data plane.** A shared-memory single-producer single-consumer
  (SPSC) ring with 4096 slots, plus an auto-reset event that wakes the
  consumer.
- **Surrogate side.** A control pump, plus a read pump that polls
  `PassThruReadMsgs` into the ring. The read pump runs only while a
  channel is connected.
- **Diagnosability.** Each startup stage that can fail exits with its
  own code (1–7, listed in [docs/j2534.md](../j2534.md#surrogate)),
  and the client traces its failure paths through `SUR_LOG`.
- **Build.** Only the surrogate and the vendor DLLs are x86. They come
  from a second, core-only Win32 build tree.

## Alternatives considered

- **An x86 build of the whole app.** Not possible, because no 32-bit
  Qt kits exist.

## Consequences

- A vendor DLL runs in its own address space, so it never shares one
  with the app.
- Cross-arch testing needs two build trees. Shared headers are ABI
  across them (Known Issue #19).
- The ring drops frames silently when full (Known Issue #17).
- On x86, MSVC decorates `__stdcall` exports (`_PassThruOpen@8`), so
  the fake DLL needs a `.def` file to export undecorated names. Real
  vendors do the same.
- In-proc and surrogate backends must behave identically. That is
  enforced by routing ([ADR-0009](0009-pe-architecture-routing.md)) and
  by parity tests ([ADR-0011](0011-vendor-call-relay-wire-forms.md)).
