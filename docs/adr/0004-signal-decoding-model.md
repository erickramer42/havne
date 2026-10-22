# ADR-0004: Signal decoding: POD definitions, linear scaling, Intel byte order, message-ID gate

- **Status:** Accepted
- **Date:** 2026-10-06 (Phase 1, `bb8eb4b`). The message-ID gate was
  added 2026-10-14 (`d0fb07e`).

## Context

Raw payload bytes have to become physical values with units, inside
core ([ADR-0001](0001-qt-free-core-c-abi.md), rule 2). Signal
definitions will eventually come from several sources (the built-in
mock set, a DBC loader, manufacturer plugins), so the decoder must
not care where a definition came from. A real bus carries many
message IDs, and decoding a signal against the wrong message silently
corrupts it.

## Decision

- **`QttuneSignalDef` is a POD struct** holding the byte and bit
  position, type, scale and offset, unit string, `message_id`,
  display hints and reserved fields. Its size is pinned at 88 bytes by
  an ABI test in `test_signals.cpp`.
- **Decoding is linear:** `display = raw * scale + offset`. INT types
  are sign-extended, and FLOAT32 passes through as IEEE-754.
- **Byte order is Intel (LSB-first) only.** Motorola (MSB-first) is a
  deliberate non-goal until a bit in `flags` selects it, and byte
  order is the intended first use of a reserved flags bit (DBC files
  mix the two).
- **Error codes are a tested contract.** `qttune_decode_signal`
  returns 1 for null, 2 for past the payload length, 3 for bad bit
  params and 4 for an unsupported type. The type is checked before
  the bit params. `qttune_validate_signal` does the structural checks
  and has its own numbering. Its fixed-width strings must terminate
  within the buffer.
- **`QttuneSignalSet`** is a swappable collection, and the decoder
  never sees where it came from. The mock ships
  `qttune_mock_signal_set()`.
- **Message-ID gate:** each definition declares the CAN arbitration ID
  it rides on, and the bridge decodes a signal only from frames whose
  ID matches. The mock set uses `QTTUNE_MOCK_TELEMETRY_ARB_ID = 0x0C0`.
- **The mock encoder is the hand-written inverse of the definitions.**
  `encode_payload()` in `mock_transport.cpp` mirrors `kMockDefs`.
  `CoreTest.MockFramesDecodeAgainstMockSignalSet` decodes real worker
  frames and asserts physical ranges, because byte-order and scale
  mistakes pass structural validation.

## Consequences

- A DBC loader or plugin can build sets through the same struct
  without any ABI change.
- Motorola-ordered signals cannot be expressed until the flags bit
  exists.
- The mock encoder and the mock definitions must always change
  together.
- A transport that reports `arbitration_id = 0` decodes nothing. This
  is the case for J2534 until the transport stamps IDs (Known Issue
  #15, [ADR-0012](0012-j2534-transport-adoption.md)).
