# ADR-0012: J2534 transport adoption: caller-chosen IDs, transport-driven request loop

- **Status:** Accepted. Implemented in Phase 3 Part 5 (commit `161af4a`, 2026-10-15).
- **Date:** 2026-10-15

## Context

After Part 4 the vendor-call relay works through both backends, but
`j2534_transport.cpp` is still read-only. Two gaps follow from that:

- **ISO 15765 receive.** An ISO 15765 channel receives nothing until a
  flow-control filter is registered
  ([ADR-0010](0010-j2534-v0404-shared-types.md)).
- **The message-ID gate.** The bridge's gate
  ([ADR-0004](0004-signal-decoding-model.md)) stays inert while J2534
  frames carry `arbitration_id = 0` (Known Issue #15).

## Decision

1. **The caller picks the IDs through URI parameters.**

   | Parameter | Range | Default | What it does |
   |---|---|---|---|
   | `?filter=` | 1–`0x1FFFFFFF` | none | Receive ID. Registers a `FLOW_CONTROL` filter at start. |
   | `?txid=` | 1–`0x1FFFFFFF` | `0x7E0` (OBD-II physical) | Transmit ID. |
   | `?poll=` | PID 1–`0xFF` | none | Runs a Mode-01 request loop every 100 ms. |

   If either ID is above `0x7FF`, `CAN_29BIT_ID` is set on both the
   filter and the writes.
2. **Coupling rule.** `poll` without `filter` is rejected at parse
   time, because such a session would be guaranteed silent.
3. **Filter before traffic.** The filter is registered after connect
   and before the worker starts, so no response can arrive unfiltered.
   If registration fails, start fails and the connect is undone.
4. **Receive framing.**
   - *With a filter,* the worker checks the frame's 4-byte in-band
     address header against the filter ID, strips the header, and
     stamps `arbitration_id`. Frames that don't match, or are too
     short, are dropped.
   - *Without a filter,* frames are dispatched verbatim with ID 0, as
     they were before adoption. That is why the existing stream tests
     pass unmodified.
5. **Teardown order:** join the worker, stop the filter (best-effort),
   disconnect, close. The spec says disconnect invalidates filters
   anyway. The explicit stop is there for vendor compatibility, since
   some drivers leak filter state when it is skipped.
6. **Fake semantics.**
   - Once a filter is registered, the fake's echo is filter-aware,
     unconditionally, which keeps the two architectures in parity.
   - A separate `SetFilterGating` hook decides only whether
     `ReadMsgs` drops frames that don't match.

## Consequences

- `?filter=` alone gives a read-only live session, and adding
  `?poll=` adds a request loop. Together that is the minimum for a
  first live vehicle read.
- Known Issue #15 closes for sessions that use `?filter=`.
- One PID per session for now.
- LiveView still shows nothing from J2534. The bridge only knows the
  mock signal set (Known Issue #21), so an OBD-II PID signal set is
  the next step.
- Several framing choices are assumptions until a real device answers.
  They are listed in [BENCH prep](../bench-sessions/bench-prep.md).
