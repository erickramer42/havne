# Havne

_From Norwegian *havn*, a harbor or port: Havne reaches a car through its ports. The verb *havne* is to end up somewhere, as in *havne i grøfta*, in the ditch; staying out of it is still your job._

[![CI](https://github.com/erickramer42/havne/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/havne/actions/workflows/ci.yml) [![Coverage](https://codecov.io/gh/erickramer42/havne/graph/badge.svg)](https://codecov.io/gh/erickramer42/havne) [![Release](https://img.shields.io/github/v/release/erickramer42/havne)](https://github.com/erickramer42/havne/releases/latest)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/erickramer42/havne/badge)](https://scorecard.dev/viewer/?uri=github.com/erickramer42/havne) [![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15816/badge)](https://www.bestpractices.dev/projects/15816)

Havne is a cross-platform OBD-II scan tool and data logger built on a
Qt-free C++17 core behind a stable C ABI. It shows live data, reads
trouble codes and readiness monitors, records every frame to replay
and compare, and reads manufacturer-specific signals from data files
you write or import.

It reaches a car through J2534 Pass-Thru adapters on Windows,
ELM327-compatible adapters over USB, Bluetooth or WiFi on every
platform, and SocketCAN on Linux. It runs on Windows, Linux (x86_64
and 64-bit ARM) and Android, builds and passes its tests on macOS, and
runs on the iOS Simulator; a simulated vehicle drives every page with
no hardware. Two front ends share the core: a Qt 6 / QML app, and
`havne-cli`, which streams any session as JSON lines and is written
against the public headers alone.

> [!WARNING]
> **Experimental.** It has read one car through two adapters and listened to it through a third; other vehicles and adapters are untested. It can clear trouble codes, asking and saving them first, but has erased a stored code only on its fakes and simulated vehicle. Read [DISCLAIMER.md](DISCLAIMER.md) before connecting it to a vehicle.

![The Havne Dashboard through a quarter mile on the warmed-up simulated vehicle: launch control holds the engine near 4,000 rpm at the line, the tires spin off it, three upshifts at 6,800 rpm refill the shift lights, and speed climbs to about 107 mph at the line while coolant rises from 194 to 216 deg F, where the fan cuts in, and the oil from 221 to 235 deg F, before the driver lifts](docs/images/dashboard.gif)

*The Dashboard on the simulated vehicle (`mock://demo`), in real time:
a quarter mile off launch control on a warm loop.*

## Engineering highlights

- **Vendor drivers run out of process.** Each J2534 DLL, often 32-bit,
  runs in a single-threaded surrogate of its own architecture behind a
  private named pipe, so a crashing driver takes down its surrogate,
  not the app ([ADR-0025](docs/adr/0025-one-j2534-backend.md),
  [ADR-0014](docs/adr/0014-single-threaded-surrogate.md)).
- **Fast paths, measured.** The worker decodes with no allocation per
  frame, backpressure bounds the frames in flight, and QML gets values
  at 10 Hz whatever the bus rate: a recorded engine start replays
  through decode at over 300,000 frames a second, about 70 times a
  full classic CAN bus, in a load test every CI run repeats. Recording
  costs the worker 31 to 47 ns a frame (a single-producer ring to the
  recorder's own writer thread), and the reader opens an hour of 50
  signals, 1.8 million rows, in under 0.2 s
  ([ADR-0024](docs/adr/0024-decode-on-the-worker.md),
  [ADR-0049](docs/adr/0049-recordings-in-core.md),
  [load test](docs/testing.md#load-test-the-replay-fixture-through-the-bridge)).
- **Measured on a real car, and fixed by what it showed.** A second CAN
  controller listening on the test car's bus timed the Pass-Thru: a
  reply was whole on the wire 14 ms after its request but reached the
  session about 52 ms later, most of it in Havne's own read of the
  adapter. With that read fixed, a request's latency fell from
  65.5-73.7 ms to 16.4 ms; packing up to six PIDs into a request had
  already brought 2.7 times the replies a minute. Each session is
  written up with its scrubbed logs
  ([2026-11-01](docs/bench-sessions/2026-11-01-vehicle.md),
  [2026-11-03](docs/bench-sessions/2026-11-03-socketcan-car.md),
  [2026-11-04](docs/bench-sessions/2026-11-04-two-ecu-car.md),
  [2026-11-05](docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md)).
- **Protocol logic with no I/O, unit-tested and fuzzed.** The request
  engine, ISO-TP and the ELM327 wire layer take no I/O, clock or
  allocation, so every rule is a unit test over canned bytes; they and
  every other parser run under libFuzzer with ASan and UBSan in CI.
  Core's ISO-TP is also held to the Linux kernel's on a virtual CAN bus,
  and to a recording of the test car with its replies broken on purpose
  ([ADR-0015](docs/adr/0015-request-engine.md),
  [ADR-0040](docs/adr/0040-socketcan-requests-isotp.md),
  [ADR-0041](docs/adr/0041-replay-fault-injection.md),
  [fuzz/README.md](fuzz/README.md)).
- **A user's signals are data, checked before the car.** Signal files
  and plugins are JSON that core reads strictly from bytes, never a
  path, so every platform loads them alike, iOS included. Anything wrong
  is refused at load with its line and column, and a plugin's fixtures
  run through the simulated ECU in CI, so its scaling is checked with no
  car ([ADR-0051](docs/adr/0051-signal-files.md),
  [ADR-0052](docs/adr/0052-plugin-framework.md)).
- **A stable C ABI.** Opaque handles, flat functions and POD structs
  with reserved room; CI compiles every public header as strict C,
  compares the ABI with the last release's and builds the core with no
  Qt installed ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)).
- **Tested in tiers.** A harness tier drives the whole J2534 stack
  against an in-tree fake driver, 32-bit included, and ELM327 against a
  fake adapter; what only hardware can confirm is listed with its
  evidence, and each decision is an [ADR](docs/adr/README.md) with the
  alternatives it turned down ([docs/testing.md](docs/testing.md),
  [bench-prep.md](docs/bench-sessions/bench-prep.md)).
- **Downloads anyone can check.** Signed build provenance and an SPDX
  SBOM for every download, Linux core packages that build to the same
  bytes, and debug symbols with each release
  ([ADR-0034](docs/adr/0034-debug-symbols-and-probes.md),
  [ADR-0035](docs/adr/0035-linux-downloads-sbom-reproducible.md)).

## What it does

- **Pages:** a Dashboard of gauges with shift lights and peak hold;
  Live, the values and a strip chart; Diagnostics; Recordings; and a
  Log of every connect, with the raw frames in developer mode.
- **Adapters:** J2534 on Windows; ELM327 over serial, Bluetooth or
  WiFi, on the desktop and on Android, where sessions keep running in
  the background; SocketCAN on Linux, listening through a real
  controller and asking, so far, on a virtual bus
  ([j2534.md](docs/j2534.md), [elm327.md](docs/elm327.md),
  [linux.md](docs/linux.md)).
- **Trouble codes:** each ECU's stored, pending and permanent codes,
  readiness monitors and freeze frame, generic codes named offline, and
  the inspection readiness verdict; a clear asks first and saves the
  report ([ADR-0037](docs/adr/0037-reading-trouble-codes.md),
  [ADR-0038](docs/adr/0038-clearing-trouble-codes.md),
  [ADR-0044](docs/adr/0044-trouble-code-titles.md),
  [ADR-0045](docs/adr/0045-inspection-readiness.md)).
- **Profiles, chosen by the URI:** `obd2` polls every Service 01
  reading the car supports, the fast-changing ones more often;
  `uds-generic` the UDS identifiers you name; `raw` logs frames; and
  your signal files and plugins. The VIN, calibration IDs and each
  request's reply time are read too
  ([ADR-0039](docs/adr/0039-full-service01-coverage.md),
  [ADR-0042](docs/adr/0042-reply-timing.md),
  [ADR-0043](docs/adr/0043-vin-decode.md)).
- **Your own signals:** signal files of Service 01 PIDs and Service 22
  DIDs on up to four modules, as numbers, text, enumerations or values
  computed from others (the simulated car's boost is MAP minus baro).
  Write them per vehicle in Settings, where **Read once** shows a
  reply's bytes beside its value, or convert an OBDb set with
  `havne-cli` ([docs/signal-files.md](docs/signal-files.md)).
- **Plugins:** packages of signal files for one manufacturer's cars;
  the car's VIN proposes the variants that fit, and the pick stays
  with the vehicle ([plugins/README.md](plugins/README.md)).
- **Recording and replay:** every frame, value and event of a session.
  The Recordings page pans, zooms, reads every value at a cursor, marks
  what a check of the frames found and overlays a second run;
  `replay://` plays a recording back through the same decoding, with
  faults injected on request
  ([ADR-0023](docs/adr/0023-replay-transport.md),
  [ADR-0049](docs/adr/0049-recordings-in-core.md)).
- **`havne-cli`:** any session as JSON lines, through a signal file or
  a plugin too; recordings viewed and checked offline; signal files
  checked, OBDb sets converted and plugins checked; on Linux, the
  simulated car or a recording answering requests on a virtual CAN bus;
  and an Android phone relaying its Bluetooth adapter to it over USB
  ([ADR-0031](docs/adr/0031-command-line-client.md),
  [ADR-0046](docs/adr/0046-simulated-ecu.md),
  [ADR-0047](docs/adr/0047-adapter-relay.md)).
- **A simulated vehicle:** a modeled car (gearing, airflow, tire slip,
  coolant and oil heat) through a cold start, pulls onto the limiter, a
  cruise and a quarter mile, its readings consistent with each other.

| Live view | Link health |
|---|---|
| [![Live view: RPM, speed, MAF and lambda plotted through a 3rd-gear pull onto the limiter](docs/images/live.png)](docs/images/live.png) | [![Live view over J2534 with every request failing: an amber "Requests failing" banner and pill, and each signal waiting for data](docs/images/link-health.png)](docs/images/link-health.png) |
| A 3rd-gear pull onto the limiter: rpm, speed and MAF rise together, then rpm saws while speed holds and lambda swings lean on each fuel cut | Every request refused by the fake J2534 driver: the link reads "requests failing", not idle, and the page says what to check |

![The Diagnostics page on the simulated vehicle: the inspection readiness card says it would fail an emissions inspection because an ECU commands the check-engine light on; ECU 0x7E8 lists three stored codes (P0301, P0420, U0100), three pending (P0171, C0035, B1001) and one permanent (P0420), each P and U code with its title, such as "P0301 Cylinder 1 Misfire Detected"; below, the readiness monitors, each done this drive and complete since the last clear](docs/images/diagnostics.png)

*The Diagnostics page on the simulated vehicle: the inspection
verdict, each code with its SAE J2012 title, and the readiness
monitors.*

![The Log page in developer mode on the simulated vehicle: the activity log holds two connects with their Session lines and, between them, the Delivery line of the disconnect (none dropped, the GUI thread's wait in microseconds); the pointer clicks Frames and the raw frame table streams, newest first, each frame's time, length, standard or extended ID and payload; then it clicks back to Activity](docs/images/log.gif)

*The Log page in developer mode: each session as it happened, then the
frames as they arrive.*

![The Recordings page: a recording of the simulated vehicle's third loop, warmed up, open in the viewer with its first loop, a cold start, overlaid dashed and aligned on a marker typed before each 2nd-gear pull; the list is filtered to the four plotted signals, RPM, coolant, vehicle speed and oil; RPM and speed climb to the fuel-cut limiter together in both runs while the warm run's coolant reads 201 deg F and oil 192 deg F at the cursor against the cold run's 99 and 82; under the chart the frames check found nothing in 6,501 frames](docs/images/recordings.png)

*The Recordings page: a warm loop's 2nd-gear pull with the cold
start's overlaid dashed, aligned on a marker typed while recording.*

## Architecture

```
 QML pages: Home · Dashboard · Live · Diagnostics · Log · Recordings · Settings
   ▼  properties and list models, GUI thread only
 SessionBridge (app/)          the app's caller of the core: worker → GUI
   │                           marshaling, 100 ms coalescing, link health
   │   havne-cli (cli/)        a second front end: a session as JSON lines
   ▼  C ABI: core.h · signals.h · profile.h · plugin.h · obdb.h · j2534.h · serial.h
             stream.h · socketcan.h · vin.h · sim.h · recording.h
 ecuio-core (core/)            no Qt, ever: sessions, profiles, signal files, plugins, decoding, recordings
   ├── mock://    a scripted drive on a vehicle model
   ├── replay://  a recording's frames.csv at its own pace
   ├── elm327://  a serial port, a WiFi socket, or a link the app opens
   ├── socketcan:// a Linux CAN interface, listening or through ISO-TP
   │              (havne-cli play drives the simulated car on a virtual one,
   │               havne-cli sim answers as its engine ECU)
   └── j2534://   a surrogate process per driver, over a named pipe
          ├── ecuio-j2534-surrogate.exe      (32-bit drivers)
          └── ecuio-j2534-surrogate-x64.exe  (64-bit drivers)
```

Three rules shape it: **the core never links Qt**, so other front ends
can drive the same core; **the UI receives final display values**, so
decoding, scaling and units have one home; and **the C ABI is the
boundary**. [docs/architecture.md](docs/architecture.md) walks through
it, a frame's path from transport to screen included.

**Where to look in the code:**

| File | What it shows |
|---|---|
| [`core/src/request_engine.h`](core/src/request_engine.h) | The request engine: pacing, the probe, the DID scan and identification, with no I/O and no clock, so every rule is a unit test |
| [`core/src/isotp.h`](core/src/isotp.h) | ISO-TP on classic CAN, shared by ELM327 and SocketCAN: reassembly, Flow Control, the timers, with no I/O and no clock; fuzzed |
| [`core/src/elm327_protocol.h`](core/src/elm327_protocol.h) | A pure protocol layer: line reading and frame parsing; fuzzed |
| [`core/src/j2534_surrogate_client.cpp`](core/src/j2534_surrogate_client.cpp), [`surrogate/surrogate_main.cpp`](surrogate/surrogate_main.cpp) | Both ends of the out-of-process driver host: the pipe's protections, late replies, a child that dies |
| [`core/src/recorder.cpp`](core/src/recorder.cpp), [`core/src/recording_reader.cpp`](core/src/recording_reader.cpp) | Recordings in core: the worker's single-producer ring to a writer thread that never holds the session, and a reader that answers the viewer from one pass's index; the reader fuzzed |
| [`core/src/signal_file.cpp`](core/src/signal_file.cpp), [`core/src/plugin.cpp`](core/src/plugin.cpp) | A user's signals and a manufacturer's plugins read as data: a strict JSON reader over bytes, each refusal with its line and column, one build of a signal set for both; fuzzed |
| [`core/src/link_health.h`](core/src/link_health.h) | The link's state machine, shared by both adapter transports |
| [`app/sessionbridge.cpp`](app/sessionbridge.cpp) | The worker-to-GUI boundary: backpressure, coalescing, recording |
| [`cli/main.cpp`](cli/main.cpp), [`cli/line_ring.h`](cli/line_ring.h) | A second front end over the public headers alone: lines formatted on the worker with no allocation, a ring that drops for a live bus and waits for a replay |
| [`core/src/mock_drive_cycle.h`](core/src/mock_drive_cycle.h) | The simulated vehicle and its physics |
| [`core/src/sim_ecu.h`](core/src/sim_ecu.h) | The simulated ECU behind `havne-cli sim`: what it answers, as the simulated car or a recording of the test car, with faults on request; pure, so every answer is a unit test |

## Try it

Download from the [latest release](https://github.com/erickramer42/havne/releases/latest):

- **Windows 10 or 11 (x64):** unzip the zip and run `havne.exe`; Qt and
  the Visual C++ runtime are inside. It isn't code-signed, so
  SmartScreen may ask first (**More info**, then **Run anyway**).
- **Android 12 or later:** open the APK on the phone. It's signed with
  Havne's release key, so each release installs over the last.
- **Linux (x86_64 or 64-bit ARM such as a Raspberry Pi; glibc 2.39 or
  later):** make the AppImage executable and run it.
  [docs/linux.md](docs/linux.md) covers what it needs, the core
  packages for `havne-cli` and your own programs, serial permissions and
  SocketCAN.

Then in Settings, tick **Developer mode** under Display, press the
`mock://demo` preset and **Connect**; with **Record sessions** on, each
run opens afterward on the Recordings page.

To build from source you need CMake 3.21+, Qt 6.12 and a C++17
compiler:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/<kit>
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`-DHAVNE_BUILD_CORE_ONLY=ON` builds the core, `havne-cli` and their
tests without Qt; `havne-cli stream mock://demo --values` then streams
the simulated car as JSON lines.
[docs/development.md](docs/development.md) covers the 32-bit
surrogate's second tree, Linux, macOS, Android and iOS, and
troubleshooting.

## Status

v0.5.0, your own signals and plugins' data tier with Linux downloads
for 64-bit ARM, is the latest release. Next:

| Version | Plan |
|---|---|
| v0.5.1 | A patch: a replay through its recording's own signal file, refused signals shown as refused, a signal file saved out of the app; fixes from the car run of v0.5.0's signals |
| v0.6.0-v0.9.0 | Exports, imports and Python; triggered logging and alerts; discovery of every module; the scan tool |
| 1.0.0 | The finished reader: frozen and hardened, full featured on Windows, Linux and Android |

[STATUS.md](STATUS.md) holds what works, what's verified, the known
issues and the roadmap; [CHANGELOG.md](CHANGELOG.md) what each release
changed.

## Documentation

| Document | What's in it |
|---|---|
| [docs/architecture.md](docs/architecture.md) | How the parts fit together and why, and a map of the other docs |
| [docs/adr/](docs/adr/README.md) | The architecture decision records |
| [docs/testing.md](docs/testing.md) | Test tiers, what each test pins, and what isn't verified |
| [docs/signal-files.md](docs/signal-files.md) | Writing your own signals: the format, what's refused at load, using a file, importing an OBDb set |
| [plugins/README.md](plugins/README.md) | Plugins: the package, its variants, the example for the simulated car |
| [docs/j2534.md](docs/j2534.md), [docs/elm327.md](docs/elm327.md) | How each adapter transport works |
| [docs/linux.md](docs/linux.md) | Havne on Linux: the AppImage, the core packages, adapters and permissions, SocketCAN, tracing |
| [docs/ios.md](docs/ios.md) | Havne on iPhone: installing CI's unsigned build with your own Apple ID, what works there |
| [docs/development.md](docs/development.md) | Builds, tests, CI, releases, troubleshooting |
| [docs/bench-sessions/](docs/bench-sessions/bench-prep.md) | Real-hardware sessions, their run sheets, and what only hardware can confirm |
| [docs/engineering-log.md](docs/engineering-log.md) | Milestones and lessons learned |
| [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) | Where to start, the rules a change follows, reporting a vulnerability |

## License

MIT; see [LICENSE](LICENSE). Havne uses [Qt](https://www.qt.io) under
LGPL-3.0; Qt isn't bundled in this repository. Havne is an independent
project, not affiliated with or endorsed by The Qt Company.
