# QtTune

> ⚠️ **WARNING:** This software may permanently damage your vehicle.  
> See [DISCLAIMER.md](DISCLAIMER.md) for full terms before use.

[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/erickramer42/qttune/badge)](https://scorecard.dev/viewer/?uri=github.com/erickramer42/qttune)

QtTune is a cross-platform ECU reader and logger. A Qt-free C++ core,
behind a stable C ABI, handles vehicle I/O and decoding. The Qt/QML
front end is a replaceable skin on top of it. One codebase targets
desktop (Windows, Linux, macOS) and mobile (Android, iOS). Desktop
builds are validated; mobile builds are not yet.

**Early development.** The full pipeline runs end to end against a
simulated transport and against a fake J2534 vendor driver, and has
connected to a real adapter on the bench (no vehicle attached). It
has **not yet communicated with a real vehicle** — do not connect
it to one expecting it to work. See [STATUS.md](STATUS.md) for
details.

## Features

- **Live frame log.** A streaming log with newest-first or
  oldest-first ordering.
- **Live signal view.** Decoded values with units, refreshed at 10 Hz,
  plus a rolling strip chart.
- **J2534 Pass-Thru support (Windows).**
  - Finds installed vendor drivers.
  - Pick a device on the Settings page or enter a `j2534://` URI.
  - 32-bit-only vendor drivers run in a separate helper process
    (the surrogate), selected automatically by driver architecture.
    On developer setups the surrogate is deployed next to the app at
    build time; otherwise it is resolved via the
    `QTTUNE_SURROGATE_EXE` environment variable
    (see [docs/development.md](docs/development.md)).
  - Optional `?filter=`, `?txid=` and `?poll=` URI parameters select the
    CAN IDs and run a Mode-01 request loop; received frames are
    header-stripped and carry their arbitration ID.
- **Protocol profiles.** Sessions select their signal-decoding
  profile from the URI: `mock://` defaults to the mock set,
  `j2534://` defaults to `raw` (frames stream to the log, nothing
  is decoded), and `?profile=<name>` overrides either
  ([ADR-0013](docs/adr/0013-protocol-profiles.md)).
- **Mock transport.** Simulated engine data, so the app runs with no
  hardware.
- **Stable C ABI.** Other front ends (native mobile, Flutter,
  scripting) can drive the core unchanged.

## Roadmap

| Version | Milestone | Status |
|---|---|---|
| v0.1.0 | Skeleton: core ABI, Qt shell, responsive layout | ✅ Released |
| v0.2.0 | Callback API, mock transport, live frame log | ✅ Released |
| v0.3.0 | Signal decoding, live view, J2534 transport, live vehicle read and logging | 🚧 In progress |
| v0.4.0 | Security access discovery, flash capability | Planned |
| v0.5.0+ | Manufacturer plugin modules | Planned |

The detailed phase checklist and known issues are in
[STATUS.md](STATUS.md).

## Requirements

| Component | Version | Notes |
|---|---|---|
| CMake | 3.21+ | |
| Qt | 6.12+ (LGPL) | Core, Gui and Quick modules |
| C++ compiler | C++17 | MSVC 2019+, GCC 11+ or Clang 14+ |

Runtime requirements:

- **Windows:** the VC++ 2019 Redistributable (installed with the MSVC
  workload).
- **Linux and macOS:** the Qt runtime libraries, from your package
  manager or the Qt installer.
- **J2534 vendor driver (Windows):** optional. It is only needed for
  real adapters. Tests use a bundled fake driver.

## Build

```sh
# Windows (adjust the path to your Qt installation)
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/msvc2022_64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/app/Release/qttune.exe

# Linux / macOS
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
ctest --test-dir build --output-on-failure
build/app/qttune
```

Add `-DQTTUNE_BUILD_CORE_ONLY=ON` to build only the core library and
its tests, without Qt.

**32-bit vendor drivers (optional):** to run against x86-only J2534
drivers, build the Win32 surrogate in a second build tree and either
deploy it next to the app at configure time
(`-DQTTUNE_SURROGATE_SOURCE=...`) or export
`QTTUNE_SURROGATE_EXE` at runtime:

```sh
cmake -B build-win32 -A Win32 -DQTTUNE_BUILD_CORE_ONLY=ON
cmake --build build-win32 --config Release --target qttune-j2534-surrogate fake_j2534 fake_j2534_legacy
```

**Mobile (not yet validated):** open the project in Qt Creator, select
an Android or iOS kit, and Run.

The cross-architecture test setup, test fixtures and troubleshooting
notes are in [docs/development.md](docs/development.md).

## Documentation

| Document | What's in it |
|---|---|
| [STATUS.md](STATUS.md) | Current state, phase checklist, known issues, detailed roadmap |
| [docs/development.md](docs/development.md) | Build configurations, running tests, CI, troubleshooting |
| [docs/testing.md](docs/testing.md) | Test tiers, suites, what is and isn't verified |
| [docs/j2534.md](docs/j2534.md) | How the J2534 transport works |
| [docs/adr/](docs/adr/README.md) | Architecture decision records |
| [docs/engineering-log.md](docs/engineering-log.md) | Milestone history and lessons learned |

## Plugins

Reserved for manufacturer modules that implement protocol and
security layers.

## License

MIT. See the [LICENSE](LICENSE) file.

QtTune uses [Qt](https://www.qt.io), licensed under LGPL-3.0. Qt is
not bundled in this repository; see qt.io for sources and the license
text.
