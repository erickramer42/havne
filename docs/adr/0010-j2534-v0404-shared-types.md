# ADR-0010: SAE J2534-1 v04.04 is the contract; one shared types header

- **Status:** Accepted
- **Date:** 2026-10-14 (`046b320`)

## Context

The OBDX Pro driver ships no vendor header. We checked both the
installation and the web, and that is normal for this market. The
loader, the surrogate and the fake vendor DLL all compile from one
in-tree header. That keeps them consistent with each other, but it
also means **the test suite cannot detect divergence from the spec**,
because the fake diverges in exactly the same way.

## Decision

- `core/include/ecuio/j2534_ptypes.h` is the single definition of
  `PASSTHRU_MSG` and the J2534 constants. The loader, the surrogate
  and the fake all use it.
- That header is verified against the canonical v04.04 definitions,
  cross-checked across three independent public references:
  - FreeSSM `src/J2534.h`
  - the diamondman J2534 PassThru Logger v04.04 header
  - PEAK System's PCAN-PassThru API 04.04 manual
- The first verification found and fixed three divergences:
  1. **`DataSize` / `ExtraDataIndex` were in the wrong order.** This
     happened to work for unpadded single frames, where the two values
     are equal. It corrupted padded and multiframe messages and gave
     wrong transmit lengths.
  2. **`CAN_29BIT_ID` is `0x0100` (bit 8), not `0x01000000`.** The old
     mask would never fire on a conforming vendor.
  3. **`ISO15765` is `0x06`.** The old value, `0x05`, is raw CAN.
     Connecting with it opens an unfiltered raw-CAN channel that half
     works, which is the most dangerous kind of failure.
- The same review confirmed that timestamps are in microseconds (vendor
  conformance is checked at BENCH). It also confirmed the filter,
  ioctl and `SCONFIG` constants.
- Reading the spec text showed that an ISO 15765 channel receives
  **nothing** without a matching flow-control filter. That turned
  filter support from polish into a prerequisite
  ([ADR-0011](0011-vendor-call-relay-wire-forms.md),
  [ADR-0012](0012-j2534-transport-adoption.md)).

## Consequences

- In-tree consumers cannot drift apart in layout.
- Spec conformance is checked by diffing against reference headers,
  not by the test suite. A real device is the only thing that catches
  a vendor that does not follow the spec (the BENCH tier).
- Constants in this header are ABI between the x64 build tree and the
  x86 build tree. Change a value, rebuild only one tree, and you get
  silently wrong IPC data (Known Issue #19).
