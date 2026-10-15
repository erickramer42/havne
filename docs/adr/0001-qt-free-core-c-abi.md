# ADR-0001: Qt-free core behind a stable C ABI

- **Status:** Accepted
- **Date:** 2026-09-29 (v0.1.0 skeleton, `35ef664`)

## Context

QtTune targets desktop and mobile, and may later need front ends that
are not Qt at all, such as a native mobile client, Flutter, or
scripting. Vehicle I/O, decoding and unit conversion are the parts
that must be correct, and they need to be testable without a GUI, on
every compiler the project ships with.

## Decision

Split the project into a core library and replaceable UI skins, held
to three rules:

1. **Core never links Qt.** `core/` is plain C++17. The Ubuntu CI job
   builds `core/` and its tests on a runner with no Qt installed, so a
   stray Qt include fails CI.
2. **UI layers receive final display values, never raw frames.**
   Decoding, scaling and units all live in core. Transports encode
   and never decode.
3. **The C ABI is the FFI boundary.** That means opaque handles,
   flat `extern "C"` functions and POD structs. Enum values are
   append-only, and structs carry `flags`/`reserved` fields so they
   can grow. The public headers must compile as strict C, and
   `tests/test_c_abi.c` checks that they do.

## Consequences

- Any front end can sit on the same core. The Qt bridge is one
  consumer of the ABI, not a privileged one.
- Core tests run headless under GCC, MSVC and AppleClang.
- The bridge has to marshal frames from the core's worker thread to
  the GUI thread ([ADR-0002](0002-callback-pair-identity-snapshot-dispatch.md),
  [ADR-0003](0003-backpressure-bounds-in-flight-frames.md)).
- ABI growth has to be planned through reserved fields and
  append-only enums. When a break is unavoidable it is made
  explicitly and called out ([ADR-0006](0006-frame-abi-v0.3.md)).
- The bridge is exposed to QML as a context property, which qmllint
  cannot see, so `[unqualified]` warnings on `QtTune.*` are expected
  noise.
