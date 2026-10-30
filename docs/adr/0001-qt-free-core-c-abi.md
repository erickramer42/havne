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

## Appendix — the product's name (2026-10-28)

The product is renamed **Havne** (Danish "harbors"): QtTune overlaps the
Qt Company's trademark. Trademark searches (2026-10-28) found no other
mark named Havne: none at the USPTO, and at the EUIPO only
HAVNEMØLLERNE, a flour brand registered for foods (Nice classes 30 and
31), not software (classes 9 and 42). The Norwegian sense (*å havne*,
"to end up", as in *havne i grøfta*, "end up in the ditch") was
weighed and accepted. It lands before code signing, since the
certificate carries the name, and before any Android release, since the
package id is permanent.

The appendix of 2026-10-22 gave the app side the product's name. That
narrows: **the product's name goes only where a user sees it**, and
the app's internal names become neutral, so a later rename touches
only those places. The core keeps `ecuio`.

| Scope | Old | New |
|---|---|---|
| Seen by users | `qttune` executable and target, `qttune.app`; the window title, About, the notification | `havne`, Havne |
| | Release zip `qttune-<version>-windows-x64.zip`, the version resource's "QtTune <X.Y.Z>" | `havne-<version>-windows-x64.zip`, "Havne <X.Y.Z>" |
| | QSettings organization and application, `Documents/QtTune/recordings` | Havne, Havne; `Documents/Havne/recordings` |
| | Android `org.qtproject.example.qttune`, version name `1.0` | `io.github.erickramer42.havne` (the reverse of the project's GitHub Pages address, `erickramer42.github.io`); the version name from `project(VERSION)`, the version code `major * 10000 + minor * 100 + patch` |
| | Build options and variables a builder types: `QTTUNE_BUILD_CORE_ONLY`, `QTTUNE_SETTINGS_DIR`, `project(QtTune …)` | `HAVNE_BUILD_CORE_ONLY` (the old one honored for one release, with a warning), `HAVNE_SETTINGS_DIR`, `project(Havne …)` |
| | The repository, `qttune` | `havne` (GitHub redirects the old URL) |
| Internal | QML module `QtTune` (`QtTunePlugin`), `QtTuneBridge` (`qttunebridge.{h,cpp}`), `qttune_ui`, `qttune_uiplugin` | `App` (`AppPlugin`), `SessionBridge` (`sessionbridge.{h,cpp}`), `app_ui`, `app_uiplugin` |
| | CMake functions and variables (`qttune_enable_warnings`, `qttune_add_version_resource`, `qttune_deploy_surrogate`, `_qttune_*`), compile definitions (`QTTUNE_BLUETOOTH`, `QTTUNE_RC_*`, …), `packaging/icons/qttune*.svg` | `app_*`, `APP_*`, `packaging/icons/app*.svg` |
| Core | `ecuio_*`, `ecuio-core`, `ecuio-j2534-surrogate`, `ECUIO_*` | unchanged |

The user's data comes along:

- **Preferences are copied once, on the desktop.** Before the first
  `QSettings` read, if the new store holds no keys and the old one
  does, every key is copied, and the activity log says so. The old store
  stays as it was, so an older build still finds its own; moving it
  would strand that build. The vehicles' salted VIN hashes (ADR-0027)
  travel with it; without the copy each car would need reconnecting.
- **Recordings stay where they are.** New ones go to the new folder; the
  recent list and replay read both, newest first. Moving a user's files
  could fail halfway or meet a folder in use.
- **Android starts fresh.** A new package is a new app, and Android
  keeps each app's files private, so the new one can't read the old
  one's. The run sheet's install step backs up the old app's recordings
  with `adb` and leaves it to be removed by hand. No Android release has
  shipped, so only the test phone has the old app.

The migration lands first, with the old and new names still equal, so
it changes nothing until the rename and its tests stand on their own.
The rename is then one mechanical change (files moved first, then
CMake, types, the QML module and the text), CI and the release workflow
included; afterwards the old name appears only in the records, which
keep it, as in the earlier appendices.

*Amended 2026-10-29:* `QTTUNE_BUILD_CORE_ONLY` was honored through
v0.3.3, as the table says, and v0.3.4 drops it: a tree configured with
only the old name builds the whole app. A tree configured while the
alias stood already holds `HAVNE_BUILD_CORE_ONLY` in its cache.
