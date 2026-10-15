# QtTune

> ⚠️ **WARNING:** This software may permanently damage your vehicle.  
> See [DISCLAIMER.md](DISCLAIMER.md) for full terms before use.

[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/erickramer42/qttune/badge)](https://scorecard.dev/viewer/?uri=github.com/erickramer42/qttune)

Cross-platform ECU reader/logger, evolving into a full tuning application.
Qt/QML frontend with a shared codebase targeting desktop (Windows, Linux,
macOS) and mobile (Android, iOS); desktop builds are validated, mobile
kits are pending first on-device build.

## Requirements

### Minimum for Desktop Build

| Component | Version | Notes |
|-----------|---------|-------|
| **CMake** | 3.21+ | Required for `qt_standard_project_setup()` |
| **Qt** | 6.12+ (LGPL) | Core, Gui, Quick modules |
| **C++ Compiler** | C++17 | MSVC 19.x (2019+), GCC 11+, Clang 14+ |

### Runtime

- **Windows:** VC++ 2019 Redistributable (auto-installed by MSVC workload)
- **Linux/macOS:** Qt runtime libraries (provided by package manager or Qt installer)
- **J2534 vendor driver (Windows):** optional — required only for live-vehicle
  communication (BENCH tier, not yet verified); the J2534 transport ships with
  a fake vendor DLL for fully offline HARNESS-tier testing. 32-bit vendor
  DLLs are supported through the bundled x86 surrogate host process.

### Development (Optional)

- **Python 3.13+** (64-bit) — for future trace analysis tooling

Mobile builds additionally require Qt Creator with Android/iOS kits configured.

## Build (Desktop)

    # Windows (adjust path to your Qt installation)
    cmake -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
    cmake --build build --config Release

    # Linux/macOS: use the Qt path from your package manager or Qt installer
    cmake -B build -DCMAKE_PREFIX_PATH=/path/to/qt
    cmake --build build

Run `build/app/Release/qttune.exe` on Windows (VS generator) or
`build/app/qttune` with single-config generators like Ninja.

Note: adding new QML files to the module sometimes requires a clean
reconfigure (`rm -rf build`) — the QML cache linker can hold stale
references to files it hasn't scanned yet.

## Build (Cross-Arch Verification) — dev machines

The surrogate host and fake DLL build in whatever architecture the
tree is configured as; the true cross-arch case (x64 app driving an
x86 surrogate hosting an x86 DLL) uses a second, Win32 core-only
tree (modern Qt ships no 32-bit kits; the surrogate design needs none —
only the surrogate exe and vendor DLLs are ever x86):

    cmake -B build-win32 -A Win32 -DQTTUNE_BUILD_CORE_ONLY=ON
    cmake --build build-win32 --config Release --target qttune-j2534-surrogate fake_j2534

Then from the regular (x64) tree, with Windows-style paths
(MSYS does not translate env var values):

    export QTTUNE_SURROGATE_EXE='E:/.../build-win32/surrogate/Release/qttune-j2534-surrogate.exe'
    export QTTUNE_FAKE_DLL='E:/.../build-win32/tests/Release/fake_j2534.dll'
    ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate" --output-on-failure

## Build (Mobile) — unvalidated

Intended path: open in Qt Creator → select Android or iOS kit → Run. The
QML, bridge, and core are shared; only the kit differs. Not yet verified
on a device/emulator.

## Features

- Cross-platform: Windows, macOS, Linux; desktop validated, mobile unvalidated
- Qt-free C++ core behind a stable C ABI — UI layers are replaceable skins
- Live frame logging: mock transport streams synthetic CAN frames end-to-end
  — core worker thread → GUI-thread bridge → sort proxy → virtualized log table
- J2534 transport (Windows): registry vendor discovery (spec + no-hyphen
  root spellings, both WOW64 views — verified against real installed
  drivers), vendor function-library loading, ISO 15765 connect,
  `ReadMsgs` polling loop delivering frames through the same session
  callback path as the mock; `j2534://` URI routing (`j2534://`,
  `j2534://<vendor>`, `j2534://<path.dll>`) with optional
  `?baud=<decimal>` and `?flags=<0xHEX|decimal>` query parameters
  (strict parsing — malformed URIs rejected at session create, before
  any DLL load)
- **x86 surrogate host:** 32-bit-only vendor DLLs (common among J2534
  drivers) load in a bundled surrogate process — named-pipe control
  plane with bounded-timeout transactions, shared-memory SPSC ring for
  the data plane, kill-on-close job-object lifecycle so a crashed or
  wedged child can never outlive the app. Verified same-arch in CI and
  cross-arch on a dev machine (x64 test process → x86 surrogate → x86
  fake DLL — the exact topology 32-bit-only adapters like the OBDX Pro
  VX require)
- **Architecture-aware routing:** the loader sniffs each DLL's PE
  machine field at open and picks in-proc vs surrogate automatically —
  the transport layer is single-path and backend-blind; sessions behave
  identically either way. Unknown/unreadable DLLs keep the honest
  load-failure error rather than a speculative process spawn
- **Message-ID gating**: signals declare their CAN arbitration ID;
  frames decode only when IDs match (prevents corruption from multi-ID
  bus traffic)
- Transport selection: Settings page with a discovered-interface picker
  (incl. x86/x64 architecture labels), manual URI entry with presets,
  and connect/disconnect with error surfacing
- Signal decoding: POD signal definitions with scale/offset linear decode,
  LSB-first bitfield extraction, signed-type sign extension (core, unit-tested)
- Live signal view: decoded values streamed to a subscription-checked list,
  coalesced to 10 Hz GUI updates (100ms batch window) — decoupled from
  frame rate by design (survives 500 Hz J2534 rates later)
- Strip chart: per-signal rolling history (120 samples, timestamp+value
  ring in the model, oldest evicted first) rendered as a QML Canvas
  trace — one repaint per coalesced batch, subscribing/unsubscribing
  toggles series visibility without losing recorded history
- Physically plausible mock data: idle RPM wobble with rev blips, coolant
  warm-up toward thermostat, trailing intake temps, coupled throttle/load —
  every mock frame is decodable by the built-in signal set (round-trip tested)
- Session lifecycle: create/start/stop/close with clean worker quiesce
- Thread-safe by construction: UI receives display values, never raw frames
- Backpressure: bounded in-flight event-queue depth with honest accounting;
  rolling 10k-row log window via model ring eviction (stream never freezes)
- Sort toggle: newest-first (live edge at viewport top) / oldest-first (chronological)

## Status / Roadmap

| Version | Milestone | Status |
|---------|-----------|--------|
| v0.1.0  | Skeleton: core ABI, Qt shell, responsive layout | ✅ Released |
| v0.2.0  | Callback API, mock transport, bridge marshaling, live LogPage | ✅ Released |
| v0.3.0  | Signals layer, coalesced LiveView, J2534 transport, live vehicle read/logging | 🚧 Phases 1–3 HARNESS-complete incl. URI params, device picker, F1/F2 ABI redesign, **x86 surrogate host + architecture routing (cross-arch verified)**; remaining: WRITE/IOCTL relay, BENCH live verify, live vehicle |
| v0.4.0  | Security access discovery, flash capability | Planned |
| v0.5.0+ | Multi-manufacturer plugins | Future |

Currently 77 automated tests (18 core, 15 signals, 16 bridge, 15 J2534,
6 F1/F2 ABI + lifecycle, 4 surrogate, 3 routing — one of which is
env-gated for the dev-machine cross-arch configuration), all green
locally on Windows/MSVC 2022 and on CI (3-platform matrix).
Per-case ctest granularity via `gtest_discover_tests()`. The J2534 stack
is verified end-to-end against a fake vendor DLL (HARNESS tier) in BOTH
same-arch and cross-arch configurations; discovery is additionally
verified against real installed drivers (OBDX Pro VT/VX). No real vendor
DLL load/connect or live vehicle yet.
See [STATUS.md](STATUS.md) for verified test matrix and engineering notes.

Flash capabilities require discovering each manufacturer's secure gateway authentication. Vehicle communication research is tracked separately and outside this repo for now.

## Plugins

Reserved for manufacturer modules implementing
protocol/security layers.

## Dependencies

This application uses [Qt](https://www.qt.io), licensed under LGPL-3.0.
C ABI headers verified as strict C via `test_c_abi.c` (third-party FFI compatibility guard).
Qt is not bundled in this repository; see qt.io for sources and license text.

## License

MIT — see [LICENSE](LICENSE) file.

⚠️ Early infrastructure. Core callback API, mock transport, signal
decoding, coalesced live view with strip chart, and a HARNESS-verified
J2534 transport (fake vendor DLL verified end-to-end in same-arch AND
cross-arch configurations — x64 app → x86 surrogate → x86 DLL — incl.
`?baud=`/`?flags=` query parameters, PE-architecture routing, and the
Settings-page device picker) implemented and unit-tested.
No real vehicle communication verified yet.
Do not connect to a vehicle expecting functionality.
Frame ABI changed in v0.3.0-dev (arbitration ID, flags, session-relative time). Downstream integrations must migrate from the old `is_extended` flag and implicit 8-byte payload assumption.
