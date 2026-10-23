# ADR-0011: Vendor-call relay: flat wire forms, identical ioctl whitelist at both ends

- **Status:** Accepted
- **Date:** 2026-10-15 (Phase 3 Part 4, `6aaf441`)

## Context

Write, ioctl and filter calls have to behave identically through the
in-proc and surrogate backends. Some J2534 argument types contain
pointers (`SCONFIG_LIST` carries `ConfigPtr`), and a pointer means
nothing in another process.

## Decision

**Wire ops.** `OP_WRITE` and `OP_IOCTL` move from reserved to active.
`OP_START_FILTER` (9) and `OP_STOP_FILTER` (10) are appended. Opcodes
are append-only.

**Write.** One message per call, capped at 64 bytes, which matches
the frame ABI cap. Segmenting ISO-TP multiframe messages is
transport-layer work.

**Ioctl.**
- Carries flattened `(Parameter, Value)` u32 pairs. The child builds
  the real `SCONFIG_LIST` in its own address space, calls the vendor,
  and flattens the `GET_CONFIG` results again for the return trip.
- Has an **identical whitelist at the loader seam and in the child:**
  `GET_CONFIG`, `SET_CONFIG`, `READ_VBATT`, `CLEAR_TX_BUFFER`,
  `CLEAR_RX_BUFFER` and `CLEAR_MSG_FILTERS`.
- Refuses ioctls that carry pointers (`FIVE_BAUD_INIT`, `FAST_INIT`)
  outright, rather than relaying them with guessed layouts.

**Filters.**
- Carry flat mask, pattern and (optional) flow-control byte arrays of
  1–12 bytes each, and the lengths must match.
- A flow-control array is allowed only with `FLOW_CONTROL_FILTER`.
- The child expands them into `PASSTHRU_MSG` triplets.

**Exports and statuses.**
- Both backends resolve the same **nine** mandatory exports, filter
  functions included. Before this change the surrogate required only
  seven.
- Vendor statuses come through verbatim, and partial writes report
  the number actually written (`out_num_written`).

**Test observability.** The fake DLL answers every write with a
deterministic echo: byte 0 + 0x40, the UDS/OBD positive-response
convention. That makes the whole request→response loop visible
through the ring, from either architecture.

## Alternatives considered

- **Relay unknown ioctls with NULL payloads.** The first draft did
  this in the child, so the seam refused `FAST_INIT` while the
  surrogate passed it through. When two backends accept different
  inputs, that is a parity bug by definition.
- **A scripted-response fake.** Rejected because it needs an arming
  hook, and an x64 test process cannot reach the fake inside an x86
  child.

## Consequences

- Because pointer-bearing types never cross the process boundary, the
  in-proc and surrogate signatures are identical by construction.
  Filters got the same treatment for free, since they are
  `PASSTHRU_MSG`s rather than a bespoke struct.
- A new ioctl needs its own wire form, not a looser whitelist (Known
  Issue #20).
- The rule that the flow-control length must equal the mask length
  may be stricter than J2534-1 requires. See BENCH prep in
  [STATUS.md](../../STATUS.md#bench-prep).
- The response struct grew a payload region, and the pipe's length
  checks now catch layout skew between the two build trees, loudly.
  Skew in constant values remains silent (Known Issue #19).

## Appendix — output capacity and a version relay (2026-10-22)

The surrogate client's ioctl takes the caller's output capacity, as
the loader's helper does. On both backends the output gets the first
`output_cap` bytes of the result and the reported length is the whole
result's; the loader no longer stages surrogate replies in a buffer of
its own. `OP_READ_VERSION` (12) relays `PassThruReadVersion`, an
optional export like `PassThruGetLastError`
([ADR-0019](0019-session-info.md)).
