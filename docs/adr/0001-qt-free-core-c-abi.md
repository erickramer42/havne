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
- The bridge is exposed to QML as the `Bridge` singleton of the
  `QtTune` module, registered from C++ (`app/bridgesingleton.h`), so
  qmllint and the QML compiler see its properties.
- Every public identifier carries the core's prefix: `ecuio_`
  functions and `_t` types, and `ECUIO_` macros and enum values. The
  prefix names the domain (ECU I/O), not the product, so renaming the
  product leaves the ABI alone. A bare `QT_` would sit in Qt's own
  macro namespace, and unprefixed names (`MOCK_`, `OBD2_`) collide in
  any consumer that has its own.

## Appendix — naming (2026-10-21)

Status codes were renamed from `QT_OK` / `QT_ERR_*` to `QTTUNE_OK` /
`QTTUNE_ERR_*`, and the mock and OBD-II ID macros gained the
`QTTUNE_` prefix (`QTTUNE_MOCK_*`, `QTTUNE_OBD2_*`). The values are
unchanged, so the binary ABI is too; it is a source break. The bridge's
QML name changed from the context property `QtTune` to the `Bridge`
singleton. The engineering log, filled-in bench sheets and earlier ADR
appendices keep the old names as written.

## Appendix — the core's own name (2026-10-22)

The core's public names moved from the product's prefix to a
brand-neutral one, `ecuio` ("ECU I/O"), so a product rename can't
break the ABI again. Each scope has its own name:

| Scope | Name | Covers |
|---|---|---|
| Core | `ecuio` | Every public C identifier (functions, types, macros, enum values); the `ecuio/` include directory; the `ecuio-core` library; `ecuio-j2534-surrogate.exe` with its pipe and session-log names; the `ECUIO_SURROGATE_EXE`, `ECUIO_SUR_*` and `ECUIO_FAKE_*` environment variables |
| App | The product's name | `qttune.exe`, the `QtTune` QML module, `QtTuneBridge`, the window title, the repository and its build options (`QTTUNE_BUILD_CORE_ONLY`), and the docs |

- **C style throughout:** `ecuio_session_create`, `ecuio_frame_t`,
  `ECUIO_OK`. The four CamelCase signal types became
  `ecuio_signal_def_t`, `ecuio_signal_set_t`, `ecuio_channel_decl_t`
  and `ecuio_signal_type_t`; their struct tags drop the `_t`.
- **Why not `ecu_`:** Embedded C Utilities, a C99 library, prefixes
  every public symbol with `ecu_` and keeps its headers under `ecu/`.
  A common domain word is also the likeliest clash with an embedder's
  own automotive code. `ecuio` had no hits in the repository or in a
  web search.
- No value, size or layout changed. Everything that names the core
  did: sources, link symbols, the library and executable names, the
  pipe and log names, and the environment variables.

The engineering log, filled-in bench sheets and earlier ADR
appendices keep the old names as written.
